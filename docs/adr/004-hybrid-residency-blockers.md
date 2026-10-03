# ADR-004: Hybrid residency blockers — upstream PR, not a fork branch (for now)

**Status:** Accepted (2026-09-18). *Addendum (2026-09-18, later): the reserve-fix PR is
open as [ggml-org/llama.cpp#29085](https://github.com/ggml-org/llama.cpp/pull/29085),
contribution-routed via the user's fork — see Addendum; the Helldez fork-branch option
stays reserved for anything that must touch the submodule pin before upstream merges.*

## Context

Aider's confirmed telemetry shows the residency ladder working on a thinking hybrid:
non-edit turns prefill 16–18 tokens with `n_reused` growing 587 → 1243, while edit turns
full-clear (~815 prefilled, ~36 s). The edit-turn rewind-to-common-prefix would roughly
halve that (~390 reused, ~425 re-prefilled, ~18 s) — and it needs recurrent-state
snapshot rollback (`--rs-seq`), which is blocked by two upstream defects:

1. **lfm2moe crashes at graph reserve** with snapshots enabled: a fixed ~368-byte
   node-pool shortfall (`needed 836640, available 836272`), invariant to context,
   ubatch and budget. Upstream's reserve estimate under-counts the snapshot ops the
   LFM2 graph emits — a small, well-understood bug in an arch that is already on
   upstream's rollback allowlist.
2. **Snapshot restore is not bit-exact**: proven on Qwen3.5-9B under greedy decoding
   (identical prompt: `40` fresh vs `420` restored). The gated delta net accumulates
   state in chunks; floating-point recurrence is order-sensitive, so a mid-chunk
   snapshot restores state a fresh prefill would never produce. Fixing it means
   redesigning snapshot semantics (chunk-boundary snapshots + partial-chunk replay) —
   research-grade work in novel attention code, which is why upstream marks the
   mechanism `[EXPERIMENTAL]`.

Both fixes live in llama.cpp, which this project deliberately does not fork (the
submodule points at `Helldez/llama.cpp`, a mirror carrying exactly one sanctioned
1-commit expert hook). The payoff is also bounded: the file content an edit changes can
never be reused, so the rewind halves edit turns — while non-edit turns already cost
~0.8 s thanks to append reuse.

## Decision

1. **The reserve-crash fix is pursued as an upstream PR against
   `ggml-org/llama.cpp`** (mainline) — not against `Helldez/llama.cpp`, which is only
   the submodule's fork remote. Upstreaming removes the blocker for everyone and costs
   no fork maintenance; the arch is on their allowlist, so the bug blocks their own
   feature.
2. **Bit-exact restore is not attempted on a fork now.** If a fork experiment is ever
   warranted, it goes as a separate 1-commit branch on `Helldez/llama.cpp` (the
   submodule remote) with maintainer agreement per AGENTS.md rule 1 — never a new
   personal fork and never an in-tree diff. The trigger to revisit: upstream landing
   chunk-boundary snapshots or an equivalent exactness fix; then `--rs-seq` flips on
   with no residency-code changes (ADR-001 §2).
3. Until both land, hybrid edit turns full-clear — accepted, because the alternative
   (non-bit-exact restore) silently corrupts outputs.

## Consequences

- Easier: no fork maintenance, no bump cost; the reserve PR is cheap and unblocks
  experimentation on lfm2moe even before exactness is solved.
- Harder: hybrid edit turns stay at ~36 s on this host until upstream fixes land —
  timeline outside our control.
- Accepted as-is: pure-transformer models (Ling-mini) already get the edit-turn rewind
  for free via the generic diff path; the `40` vs `420` measurement stands as the
  standing proof of why default-off is correct.

## Addendum (2026-09-18, later): the reserve PR — root cause, routing, and a wider exactness proof

Decision 1 was executed the same day. Root cause first: the crash reproduces on pristine
upstream master (same one-node shortfall), so it is not a pin artifact. It is a budget-list
omission — `llama_context::graph_max_nodes()` gives the linear-attention family
(`max(n_tokens * 40, 32 * n_tensors)`) the node headroom their GDN graphs need, and LFM2 /
LFM2MOE, though on the rollback allowlist, were missing from that list and fell into the
default `max(1024, 8 * n_tensors)` bucket, exactly one `ggml_tensor` object short.

The fix ([ggml-org/llama.cpp#29085](https://github.com/ggml-org/llama.cpp/pull/29085)) adds
the two archs to that bucket and registers the `lfm2` / `lfm2moe` fixture models for
`test-recurrent-state-rollback` — the generator already produced them, but no rollback test
had ever exercised either arch; with the fix the suite passes 7/7. Routing note superseding
the §2 fork-branch framing for *this* PR: it went out from the user's own fork
(`cjl4hd/llama.cpp`) per the normal mainline contribution flow — an outside user cannot
push a branch to `Helldez/llama.cpp`, and the Helldez 1-commit fork-branch option remains
reserved for anything that must touch the submodule pin before upstream merges.

`tools/bmoe-rsbench` (this repo) now reproduces both blockers against any llama.cpp, using
the engine's context shape. Re-verified on current upstream master: reserve aborts without
the fix and reserves cleanly with it. ~~Snapshot restore is **still not bit-exact on both
GDN families** — qwen35 (as before) and now lfm2 too, at rollback depths 3 (matching the
upstream fixture test's depth) and 8 alike.~~ **Superseded by Addendum 2 below: those
exactness baselines were a harness artifact.** Decision 3 (full-clear fallback,
`--rs-seq` default-off) stands, but on the corrected evidence of Addendum 2.

## Addendum 2 (2026-09-18, later): the exactness "blocker" was a harness bug — the real law is snapshot-plane staleness after single-token steps

**Falsification.** The `diverge` harness compared a rolled-back context against a fresh
reference — but its continuation call went through a helper whose first line is
`llama_memory_seq_rm(mem, 0, -1, -1)`, a **full clear that wipes the pending rollback**
(`rm_all` → `rs_idx = 0`, `llama-memory-recurrent.cpp:179`). The "restored" side therefore
re-prefilled its tail on a zeroed recurrent state plus partial attention KV, and every prior
exactness result — `40` vs `420` on Qwen3.5-9B (2026-09-17), the depth-3/8 DIFFERs on both
families (2026-09-18) — measured that artifact, not upstream restore. Falsified per the
discovery rule; nothing upstream contradicted.

**Corrected method.** `bmoe-rsbench` was rewritten: both sides of every cell are fed
**identical token sequences** (prompt + m single-token steps + the rm-phase tokens), and the
rolled-back side's pending rollback is never cleared — its replay then decodes the same tail
the reference side merely continues from. A sensitivity probe (full-clear + re-prefill vs
plain continuation) proves per prompt whether the greedy argmax can even see a state
difference.

**Measured law (engine-shaped context, qwen35 + lfm2moe, pin and patched clone agree):**

- **m = 0** (rollback directly after the last multi-token ubatch): **EXACT at every swept
depth** (1, 3, 8, 24; single-token and single-ubatch rm alike) on qwen35, and on lfm2moe at
d = 1, 3, 24. Upstream's snapshot-restore roundtrip **is exact** in the regime its fixture
test exercises — the fixture test passes because it never generates between prefill and the
rollback, and with the harness fixed, so do we.
- **m ≥ 1** (single-token decode steps between the last multi-token ubatch and the rollback):
**DIFFER on both families at every depth and rm shape.** Mechanism: a ubatch writes only
`min(n_seq_tokens, K)` snapshot planes (`delta-net-base.cpp:587`, lfm2.cpp:211), so
single-token steps refresh **only plane 0** and planes d ≥ 1 keep their values from the last
multi-token ubatch; the rollback then reads a plane that is m tokens stale. The m ≥ 1 shape
is exactly what the server produces on a hybrid edit turn (engine decode steps always
intervene between the prefill and the mid-sequence `seq_rm`).
- **Residual anomaly (open):** lfm2moe at m = 0, d = 8 differs **identically in both rm
shapes** (same first diverging token) — not explained by the plane-staleness law above and
not reproduced at d = 1, 3 or 24, nor on qwen35 at any depth.

**Decision 3 stands, re-derived.** `--rs-seq` stays default-off — but the reason changes:
restore is exact at m = 0 and the doc claims of a general "non-bit-exact restore" upstream
are withdrawn; the blocker is **staleness of snapshot planes d ≥ 1 across single-token
decode steps**, which no upstream code guards against today. A candidate upstream fix falls
out of the mechanism: before applying a pending rollback with depth d, replay the m trailing
tokens through one multi-token ubatch (which rewrites planes 0..min(m, K−1) from the true
states), or maintain the planes per token at decode time. Only after such a fix lands may
ADR-001 §2's flip-on trigger fire — and the m = 0 exactness result is what makes that fix
look cheap rather than research-grade.

Cost of the correction: two prior "measurements" (ADR-001 §Context 2, the 0.24.2 CHANGELOG
bullet) are marked superseded here and in place; no code change results (the default was
already off and remains off).

## Addendum 3 (2026-09-18, latest): the plane law resolved, the shape-noise confound, and the index-shift fix

Addendum 2's two open items are resolved and its candidate fix is superseded by a better
one, implemented and verified.

**The d=8 anomaly: resolved.** The sweep's rm-phase ubatch had exactly d tokens, so plane d
was never written by it — the "anomaly" was a read of a plane holding whatever an earlier
epoch (or the allocator fill) left there. Arch-independent, no LFM quirk. The kernel-level
law: a ubatch of n tokens writes planes 0..min(n,K)−1 (GDN `ops.cpp` writes slot
`n_tokens−1−t` per token, slots ≥ n untouched; LFM2 conv `lfm2.cpp` writes min(n,K) slots),
single-token steps rewrite only plane 0, and `seq_rm` reads plane d.

**The shape-noise confound (invalidates bitwise cross-shape comparisons).** With NO rollback
anywhere, splitting a 10-token prefill 6+4 moves logits by up to ~3.6 on this backend — MoE
routing flips amplify accumulation-order noise. Consequences: upstream's multi-seq fixture
(`test-recurrent-state-rollback`) **fails on vanilla master** because it compares 12-token-
ubatch vs 10-token-ubatch histories at eps=1e-7; and every "restore is not bitwise"
measurement taken this session against a differently-shaped reference is void. Admissible
evidence is: identical-shape controls, or argmax-level verdicts with a shape-control row.
The fixture now probes shape noise in-test and downgrades bitwise assertions to
reported-not-asserted when the backend is shape-dependent.

**Decision: implement the index-shift restore, not ubatch-replay-before-restore.** Track per
sequence the last multi-token ubatch (end position, planes written) and a floor of planes
still on the current timeline; on `seq_rm`, restore plane `end − (p0−1)` when that state
survives the timeline, and refuse when it does not. Reasons: ubatch-replay costs a full
ubatch at every hybrid edit turn and still cannot resurrect destroyed states (m > d), while
index-shift is O(1), exact where restoration is possible, and honest (returns false) where
it is not — the caller's existing re-prefill fallback takes over. Accepted costs: `seq_rm`
now returns false where it used to return true silently (callers ignoring the return value
hit the position check on the next decode and fall back); checkpoint-loaded state accepts no
rollback (a state blob carries a single plane) until the next multi-token ubatch; the
restore remains subject to backend shape noise like any decode.

**Implementation** (upstream branch `fix/rs-rollback-index-shift` from `cjl4hd/llama.cpp`,
separate from PR #29085): `rs_epoch_end` / `rs_epoch_planes` / `rs_epoch_lo` per seq set in
`find_slot` (single-token ubatches don't open an epoch; `prepare`'s dry run is undone;
`rm_all`, tail invalidation and fresh starts reset; `seq_cp` inherits, `seq_add` follows
affine shifts, `seq_div` invalidates); `delta-net-base.cpp` conv writes aligned to
min(n,K) (was: clamped all K slots every ubatch, desyncing conv from GDN planes after
single-token steps).

**Evidence** (`bmoe-rsbench cutsweep`, the rescuable shape — c tokens cut into the prefill ×
m single-token steps, per-cell no-rollback shape control): fix build 9/9 EXACT on lfm2moe
and qwen35; vanilla 3/9 EXACT (m=0) + 4 DIFFER (m≥1) + 2 REFUSED (c+m > n_rs_seq, where
upstream already failed). Fixture test passes on the fix branch (single-seq bitwise incl.
checkpoint round-trips; multi-seq shape-gated) after being reshaped to the sound
decode-then-rollback shape and to assert the new refusal semantics.

Decision 3 stands until this branch is upstream and released; after that, ADR-001 §2's
flip-on trigger can fire with a submodule bump.
