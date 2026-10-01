# PROGRESS — session entry point + evidence log

**Structure (per the project-lifecycle skill):** the resume section below is rewritten
every session — never append to it. Under the divider, the append-only history log holds
the long-form evidence narrative; entries are never rewritten, only falsified explicitly
by newer entries. Trust hierarchy: resume section > history > older sections of either.
Log opened 2026-09-18; earlier project history lives in `CHANGELOG.md` and `git log`.

*Resume last rewritten: 2026-09-30 (session 22, continuing in the same sitting).
Phase: **merge landed and proven; KV-cache quantization shipped on top — 19 gates
green, version 0.27.1**. Merge commit `f3a9517` on `feat/session-residency`;
parent-2 is upstream `374f562` (0.28.0). Submodule pin `dce969851` (+530 commits).
The old "pos0 working-tree port" is RETIRED — upstream's rename is in the pin; do
not stash/restore anything for pin builds anymore.*
*One-line status: after the merge and the 0.27.1 test buildout, the session
benchmarked the dense pair (tinyMMLU + HumanEval-50, tooling gained `--dense`),
then implemented and measured KV-cache quantization (`--cache-type-k/v` +
`flash_attn` override): q8_0/q8_0 halves the KV allocation (64 → 34 MiB @ n_ctx
2048, linear in context) with mean NLL unchanged (2.17794 → 2.17237); decode at
short context is a wash (weights-bandwidth floor) — the win is memory/long-context,
not short-prompt speed.*

## State delta (this session)

- **Merge `f3a9517`: upstream `origin/main` (0.24.0 → 0.28.0, 56 commits, 4 releases)
  into `feat/session-residency` (8 commits ahead).** Conflict census matched the
  pre-merge trial exactly: `CHANGELOG.md`, `CMakeLists.txt`, `README.md`,
  `core/src/engine/session.cpp`, `docs/README.md`, `docs/telemetry.md`,
  `examples/android/app/build.gradle`. Engine-critical overlap (`cli/main.cpp`,
  `config.h`, `session.h`, `runtime.cpp`, `arch_registry.cpp`) auto-merged.
- **The one semantic resolution:** upstream refactored the inline prompt-building
  block into `detail::build_turn_inputs()` (`core/src/engine/thinking_control.cpp`);
  upstream's helper carries everything ours did **except** the ADR-002
  preserve-reasoning kwarg. Resolution: upstream's call + the
  `preserve_thinking` kwarg re-added at the call site (session.cpp, after the
  helper call). `preserve_reasoning` end-to-end (session.h field, CLI flag, bridge
  JSON key, telemetry doc) survived the merge — `session.h`/`cli/main.cpp`
  auto-merged.
- **Post-merge compile fix (amended into `f3a9517`):** upstream added
  `GenerateRequest::messages` (vector) before `n_predict`; the positional
  `GenerateRequest{prompt, n_predict}` in `tests/moe_gates.cpp:1183` bound into the
  wrong member. Now explicit field assignment. Only occurrence in the repo.
- **Uncommitted `dp.pos0` edit DISCARDED, superseded:** the edit did not compile
  against the old pin (`pos0` absent there); upstream's line 1637 already carries
  the rename for the new pin. The PROGRESS doc's "stash the pos0 port for pin
  builds" doctrine is obsolete as of this merge.
- **Version fields took upstream** (`project VERSION 0.27.0`; app versionCode 43 /
  versionName 0.28.0) — newer and monotonic; the branch's 0.24.1–0.24.3 CHANGELOG
  sections were spliced under upstream's 0.25.0–0.28.0, reverse-chronological order
  restored. README model table: upstream's `nemotron_h_moe` row + our `laguna` row
  both kept. docs/README and telemetry.md: union of both sides' additions; the
  `preserve_reasoning` key stays in the documented generate request.
- **#29085 (`74e1ee6de`) is NOT in the new pin** — verified by ancestry check in
  `third_party/llama.cpp`. The Next-actions playbook for it is unchanged.
- **Smoke test of the merged engine (Next action 2, done in-session): PASS.**
  One-shot greedy olmoe-1b-7b coherent (14.5 tok/s, 0 majflt). Three-turn session
  on olmoe (pure attention): `n_reused` 0 → 60 → 114 with suffix-only prefills
  (22/25 tokens) — the generic diff-reuse path works on the new pin, and a
  `preserve_reasoning:true` request executes without error. LFM2.5 session:
  hybrid policy behaves per design (non-append turn full-clears, `n_reused` 0;
  turn 3 took the preserve_reasoning append path). `--version` → 0.27.0;
  `--list-archs` carries both upstream rows (nemotron_h_moe, qwen4exp) and the
  branch's laguna row. Note: the new pin logs per-tensor graph diagnostics on
  stderr — cosmetic, not a failure signal.
- **Deeper test infrastructure added same session (0.27.1): 19 ctest entries,
  both trees green.** G19/G20 (messages + preserve_reasoning vs full-clear
  reference, reuse asserted via `Session::hybrid_kv()` — new public probe); the
  laguna arch row now runs the full harness (`make-tiny-moe.py --arch laguna`,
  zero router bias like nemotron's — the G9b control is bias-blind, recorded in
  docs/limitations.md territory); `BMOE_SANITIZE` ASan+UBSan tree (19/19; found
  upstream ggml-rpc misaligned reference, device gates excluded there);
  `tests/session_fuzz.py` interactive driver pins the stdin protocol contract —
  notably `cancel` fires immediately on the reader thread, so piped bursts
  cancel in-flight turns (by design, now documented).
- **KV-cache quantization (same sitting, 0.27.1): `--cache-type-k/-v` shipped
  and measured.** `RunConfig::cache_type_k/v` (+ `flash_attn` ""/on/off) →
  `SessionConfig` → `cparams.type_k/type_v`/`flash_attn_type` at creation;
  validate() spell-checks names; CLI forces FA on for quantized V (upstream
  throws otherwise; AUTO resolves ON for CPU so default gates are unaffected).
  Measured (MiMo-V2.6-9B dense, tinyMMLU-100): NLL 2.17794 f16 vs 2.17237 q8/q8
  (noise), flips 8-for/4-against; KV alloc 64→34 MiB @2048 (llama's own buffer
  line, linear in ctx); interleaved decode A/B at ~100 tok ctx: 0.39–0.41 s/tok
  both arms — no short-context speed win (weights-bandwidth floor), the payoff
  is RAM and long-context decode bytes. Long-context quality unmeasured
  (compounding-error regime — measure before long-YaRN trust).
- **Dense A/B benchmark record (history addenda): tinyMMLU 68 vs 74, HumanEval-50
  82.0% vs 88.0%** (MiMo-V2.6 vs Qwen3.5-9B, no-think completion regime — the
  HumanEval gap is inside binomial noise; Qwen3.5 keeps the knowledge lead).
  `humaneval-bench.py --dense` + stdin-close shutdown fix landed `5b955c1`.
- **Host upgraded 12 GiB → 16 GiB DDR3** (session-side fact): the 35B-A3B quads
  now cache-resident instead of eviction-storm; KV math for long context is in
  the history log (qwen35moe 80–82 KiB/tok f16 → 32k fits easily, 64k fits;
  full-attention archs — attention compute becomes the wall at the top end;
  Laguna-XS SWA-512 is the long-context bargain).

## Artifacts touched (this session)

| File | What |
|---|---|
| `core/src/engine/session.cpp` | conflict resolution: `build_turn_inputs` + `preserve_thinking` re-port |
| `tests/moe_gates.cpp` | G18f positional-init fix (amended into the merge commit) |
| `CHANGELOG.md`, `README.md`, `docs/README.md`, `docs/telemetry.md`, `CMakeLists.txt`, `examples/android/app/build.gradle` | conflict resolutions (see State delta) |
| `PROGRESS.md` | this rewrite + the session-22 history entry |
| `/tmp/progress-resume.md` | scratch splice file, deletable |
| `third_party/llama.cpp` | working tree at new pin `dce969851` (gguf-v0.19.0-2151) |
| `build/` | full rebuild against the new pin, green |

Regeneration: none needed — everything is in the merge commit. Gates re-run:
`(cd build && ctest --output-on-failure)`.

## Environment state

- **The old pin-build ritual is DEAD:** no stash/pop dance anymore — the new pin
  has upstream's `pos0` rename, the tree builds as committed. Resume gate 1 below
  is the simple build+ctest now.
- **Quality-gate datasets** `~/llm/data/`: unchanged (tinyMMLU parquet + HumanEval;
  reader `/tmp/evalvenv`; system python is PEP-668 blocked).
- **LFM2.5 daily driver: still DOWN** (was down for the session-21 quality gate;
  this session did not touch the runtime). Standing recipe: `setsid nohup python3
  -u scripts/bmoe-serve.py -m ~/llm/models/LFM2.5-8B-A1B-UD-Q4_K_M.gguf
  --engine-args "--ctx-size 8192 --chatml" --auto-echo --port 8017 >
  /tmp/bmoe-serve.log 2>&1 &` — then assert UP (`pgrep -f "[b]moe-serve.py"`).
- **Swap** 8G permanent (fstab) — unchanged; majflt/tok stays the pressure sensor
  before any perf claim.
- **Models** `~/llm/models/`: Ling-mini-2.0, LFM2.5 UD-Q4_K_M, Qwen3.5-9B,
  olmoe-1b-7b, Laguna-XS-2.1, Ornith-1.5, Qwen3-30B, Qwen3.6-35B, Cyber-Tiel-35B,
  **NEW 2026-09-23: MiMo-V2.6-Distill-Qwen-9B-Q4_K_M** (5.4 GB, dense `qwen35`
  arch — verified on the merged engine 2026-09-30: correct one-shot output,
  session KV reuse n_reused 38 on turn 2, non-MoE dense path; benchmarked
  against Qwen3.5-9B same day, see the session-22 benchmark addendum; no
  registry row needed, dense `qwen35` is pure attention).
  Also unlogged but pre-2026-07: DeepSeek-R1-Distill-Llama-8B (dense llama,
  outside the arc's MoE scope).
- **Remotes**: `origin` = Helldez/BigMoeOnEdge, `fork` = cjl4hd/BigMoeOnEdge (push
  target); `gh` authed as `cjl4hd`. Submodule: `Helldez/llama.cpp` @ `dce969851`
  (this merge). The `~/git/llama.cpp` work area still rests on `bench/host-rs` and
  `build-bench/` still links it — but its pos0-port dependency is gone; a bench
  rebuild against `bench/host-rs` is unaffected by this merge.
- **`build-bench/`**: still the OLD bench build (llama `bench/host-rs`); stale
  relative to the new pin until `bench/host-rs` itself is rebased — not needed for
  the arc's gates, which run against `build/`.
- Untracked, NOT ours: `.opencode/`, `bmoe-arm64*`, `opencode.json`, `.aider*`, logs.
- Ephemeral: `/tmp/bmoe-serve.log`, `/tmp/bmoe-reqs.jsonl`, `/tmp/bf-*`,
  `/tmp/sweep-*.txt` (regenerable per history entries).

## Open questions / blocked items

1. **Stacked PR held until #197 merges** (user decision) — unchanged, but the
   rebase step is now "rebase onto a main that contains the merge" (the branch is
   9 commits ahead of upstream main, one of them the merge). Plan otherwise as
   recorded in history (session 14).
2. **PR #29085 — READY FOR REVIEW; user monitors CI.** Verified NOT in the new
   pin. When merged: reopen #29117, submodule bump + full gates + `bmoe-rsbench
   reserve` re-run (ADR-001 bump rule).
3. **CPU multi-seq split-replay mismatch** (session-10): unchanged, upstream-side,
   out of arc scope.
4. **Shape-dependent-backend caveat** for bitwise claims: unchanged doctrine.
5. **NEW — Android app version skew:** the branch's next release must rebase its
   versionCode/versionName plan on the merged `build.gradle` (43 / 0.28.0);
   a branch release before upstream's next tag would need 44 / its own version.

## Next actions (ordered)

1. **Restore the LFM2.5 daily driver** (still DOWN; carried from session 21):
   launch command in Environment state — then assert UP per the MISTAKES rule
   (`pgrep -f "[b]moe-serve.py"`) and record the PID here.
2. **Opencode re-test with `--auto-echo`** on a serve instance (carried from
   session 21; the daily driver is deliberately down — model choice undecided).
3. **Verify the `multiple-choice` skill live** (carried from session 21):
   logprobs round-trip through the bridge, then the ~50-item demo.
4. **#29085 playbook** (Open question 2) unchanged; when it merges: reopen
   #29117 → bump + gates + rsbench re-run. When #197 merges: stacked-PR plan
   (Open question 1).
5. **Kill-process rule** (docs/MISTAKES.md): never chain `pkill -f <pat>` —
   bracket the pattern or run standalone and assert afterwards.

## Resume gates (all must assert positives)

1. Pin build + tests (simple now — no stash dance):
   `cmake --build build -j4 2>&1 | grep -E 'error|warning' | grep -v 'ccache not
   found'` → empty (ccache advisory is environmental) → `cd build && ctest
   --output-on-failure` → **19/19 passed** (18 original + session_fuzz).
   Sanitizer check when needed: `cmake -S . -B build-san -DBMOE_SANITIZE=ON
   -DGGML_RPC=OFF && cmake --build build-san -j4 && (cd build-san && ctest)` →
   also 19/19.
2. `git status -sb` → `feat/session-residency` == `fork/feat/session-residency`
   (pushed; the workflow-scope blocker is resolved); working tree clean except
   the known-untracked non-ours list (`.opencode/`, `bmoe-arm64*`, `opencode.json`).
   The old "` M core/src/engine/session.cpp` expected" clause is RETIRED — the tree
   must be CLEAN now; anything dirty: triage before working.
3. `bash -n scripts/cellc.sh && bash -n scripts/publish-host-bench.sh && python3
   -m py_compile scripts/bmoe-serve.py` → silent.
4. `grep -c '<<<<<<<' CHANGELOG.md README.md docs/telemetry.md
   core/src/engine/session.cpp` → all 0 (no conflict markers survived).

---

# History — append-only evidence log

Newer entries at the bottom; never rewrite an entry — falsify explicitly.

## 2026-09-16 → 09-17 — Hybrid residency arc, phase 1: warmup + append reuse

Branch `feat/session-residency` (stacked on `feat/serve-bridge-arm64`, confirmed
ancestor). Goal: kill the full-clear-every-turn cost on hybrid models without breaking
recurrent-state validity.

**Shipped:**
- `2cb9cd3` — warmup replay of the conversation prefix at server startup (front-loads
  prefill while the user types). Tag `progress/2026-09-17-residency-warmup`.
- `e930b4d` — append-only prefix reuse for hybrids: when the next render strictly
  extends the resident token mirror, skip the hybrid clear and prefill only the delta.
  Every fail/cancel/overflow path poisons the mirror so the next turn full-clears.
  Pure-transformer behavior bit-identical (gates green).

**Key discovery (debug instrumentation, later removed):** on LFM2.5 the mirror holds
`[prompt][reasoning][answer]` but the client echoes only `[answer]` — a thinking
hybrid's re-render *can never* extend the mirror. Structural, not a bug; documented in
`docs/serve.md`. The engine's divergence fallback (full clear) handled it correctly.

## 2026-09-17 — Phase 2: snapshot rollback wired, proven non-bit-exact, shipped off

- `63768e7` — `--rs-seq N`: allocate upstream's `[EXPERIMENTAL]` per-token
  recurrent-state snapshots (`n_rs_seq`) so `seq_rm` mid-sequence becomes restorable
  within budget; the generic diff path already falls back to full-clear on `seq_rm`
  failure. Tag `progress/2026-09-17-snapshot-rollback`.
- Verified on Qwen3.5-9B (dense hybrid, arch `qwen35`, `think_ctl: template`):
  reuse engages (`n_reused` 21/33/58) **but restore is not bit-exact** — greedy decoding,
  identical prompt: `40` fresh vs `420` restored (and a self-correcting "wait, that's not
  right…" on a cache-warm repeat). Attention side exact; gated-delta-net snapshots are
  the divergence. **Default flipped to 0** (off) after proving the off-behavior
  byte-identical to the shipped baseline. See `docs/adr/001-hybrid-session-reuse-policy.md`.
- `5e664bf` — LFM2/LFM2MOE crash at graph reserve with snapshots enabled:
  `not enough space in the context's memory pool (needed 836640, available 836272)` —
  fixed ~368-byte shortfall, invariant to ctx (4096/2048), ubatch (256/full), budget.
  Upstream reserve under-counts snapshot ops; no public knob. Third upstream gate.
- Bridge: per-request `think` flag threaded (HTTP → engine). Template ground truth via
  jinja: Qwen3.5 silences thinking by baking an empty `<think></think>` into the
  generation prompt that re-rendered history omits — renders never nest, append can
  never fire on it even with thinking off.

## 2026-09-18 — Phase 3: reasoning echo — the rewind question answered

User question: "why can't we rewind the KV cache to before the last thinking stage?"
Answer built up in-session: (1) on transformers the diff path already reuses exactly to
that boundary; (2) KV surgery between reasoning and answer is impossible (positions
baked in, causal entanglement — only suffix chops are legal, and they drop the answer
too); (3) the better move is not rewinding at all.

**Ground truth from the LFM2.5 template** (gguf + jinja2, `{% generation %}` tags
stripped for stock jinja): a `preserve_thinking` variable (default false) renders
history reasoning verbatim — the echo mechanism exists natively. With it true,
`render1 + generated` is a strict prefix of `render2` (case A verified). Default kwargs
stop reuse exactly at the rewind-to-before-thinking boundary (123/177 tokens) — theory
confirmed empirically.

**Shipped `d83d153`** — `preserve_reasoning` request flag end-to-end:
`GenerateRequest` (`core/include/bmoe/session.h`) → `SessionCmd` (`cli/main.cpp`) →
`chat_template_kwargs["preserve_thinking"]` (`core/src/engine/session.cpp`) → bridge
(`scripts/bmoe-serve.py`, both handlers). Measured on LFM2.5-8B-A1B, 3-turn echo chain,
answers `4`/`5`/`6` all correct:

| Turn | prefilled | reused |
|---|---|---|
| T1 | 28 | 0 |
| T2 | 17 | 101 |
| T3 | 17 | 235 |

No-echo control: full clear every turn (n_reused=0), as designed. Decode measured
~9.2 tok/s in the resident regime — fastest LFM2.5 result of the project. See
`docs/adr/002-reasoning-echo.md`. Docs updated: `docs/serve.md` (gates + echo section),
`docs/telemetry.md` (protocol field), `CHANGELOG.md`.

**Upstream check** (same day): #25913 still open (touched 09-12); rollback allowlist now
includes LFM2/LFM2MOE (intent confirmed, reserve crash blocks it); no bit-exactness fix
landed. `--rs-seq` stays off.

## 2026-09-18 — Session wrap-up: lifecycle docs + ADRs

Gates re-run and green: clean build, 13/13 ctest, server healthy on Ling-mini :8017,
branch in sync with fork. Decision sweep produced `docs/adr/001` (hybrid reuse policy)
and `docs/adr/002` (reasoning echo), indexed in `docs/README.md`.

Resume doc (`SESSION_SUMMARY.md`) written with next actions: stacked PR (fork-internal base),
bridge auto-echo prototype, aider benchmark on LFM2.5.

*2026-09-18 note: renamed per the updated lifecycle skill — this file was RE_PROGRESS.md,
the resume doc became PROGRESS.md (formerly SESSION_SUMMARY.md); consolidated back into a
single PROGRESS.md later the same day per user choice.*

## 2026-09-18 — Feature-flag audit, bench-features.sh, lifecycle migration

User question: "are all of our new features behind command line flags?" — answered by
audit: no, and deliberately. Warmup is a bridge default (`--no-warmup`), echo/think are
per-request fields; `--rs-seq` is the only new CLI flag (off by default, ADR-001).

**Shipped `scripts/bench-features.sh`** — before/after proof of all three residency
features over the bridge, isolated port (default 8019), 3-turn chain with VERIFIED
answers (42/52/62; fast-but-wrong = FAIL). Debugging it exercised real failure modes:
(1) a static "template mentions think" probe over-triggered on Ling-mini — replaced with
a behavioral probe on `reasoning_content`; (2) forgetting `--chatml` in the bridge args
reproduced the known empty-prompt 502 from earlier sessions — engine args now default to
`--chatml` (overridable via `EXTRA_ENGINE_ARGS`); (3) `gguf-py`'s `get_string` silently
fails on this version — the fields API is the working one; (4) chain construction must
be incremental (each turn needs the prior reply file); (5) answer verification must
match the exact content field (`"content": "52"`), not a substring (52 ⊂ 525); (6) the
script backs up/restores the global `warmup.json` so bench runs never contaminate the
user's warm cache.

Measured (Ling-mini-2.0, this host): warmup off→on cuts T1 prefill 2.19 s → 0.06 s
(~35×), tok/s 9.4 → 16.1; echo skipped behaviorally; `--rs-seq` a verified no-op on pure
transformers. Table in `docs/serve.md`; CHANGELOG updated.

**Lifecycle migration** (user choice): single-`PROGRESS.md` convention (resume section
rewritten per session on top, append-only history below) replaces the two-file scheme.
`SESSION_SUMMARY.md` → `PROGRESS.md`, `RE_PROGRESS.md` → `PROGRESS.history.md` (git mv,
history preserved). The project-lifecycle skill was updated in both home copies —
discovery: `~/.agents/skills/` was STALE (missing the SOFTWARE_REQUIREMENTS module);
`~/skills/skills/` was newer and is now the synced source of truth.

**Stacked-PR plan recorded and HELD** (user decision: wait for serve-bridge PR #197 to
merge): after merge, sync fork main → rebase `feat/session-residency` onto it (the
serve-bridge commits collapse into main) → `gh pr create --repo Helldez/BigMoeOnEdge
--base main`. Fallback documented in PROGRESS.md Open questions if #197 stalls.

## 2026-09-18 (evening) — LFM2.5 matrix, --auto-echo shipped and proven

Followups executed: PR #197 still OPEN (stacked PR remains held). The LFM2.5 bench run
surfaced three script bugs, each fixed in the script: (1) static "template mentions
think" probe over-triggered — echo skip is behavioral on reasoning_content; (2) the
INT/TERM trap returned instead of exiting, so a killed script RESUMED into the next
scenario as a zombie (cost: it later overwrote the restored warmup.json — user's cached
LFM2.5 math chain lost; self-regenerates, lesson recorded); (3) the 6*7→+10→+10 chain
relied on coreference ("add 10 again") which the 1B model parsed as "repeat 52" —
questions now restate their inputs (6*7=42, 42+10=52, 52+10=62), testing the cache, not
the parser. Also: thinking models need N_PREDICT≈192 (reasoning alone eats smaller
budgets — nothing emitted, verification fails regardless of cache behavior).

**LFM2.5 matrix, all verified:** echo-off T2/T3 prefill 2.46/3.59 s (n_reused 0) vs
echo-on 1.38/1.27 s (n_reused 124/205); warmup rows equal (stale warmup file, mechanism
proven on Ling-mini); rs-seq-on reproduces the lfm2moe graph-reserve crash at load.

**--auto-echo shipped (ADR-003):** bridge records each reply's exact
(reasoning, answer) span (cap 16, newest-wins) and rewrites matching assistant history
turns before the engine sees them. First live test FAILED (n_reused=0) — the rewritten
spans were stripped by the template unless preserve_thinking is set; auto-echo now
implies preserve_reasoning. Retest: plain client echoing ONLY answer text got
n_reused 124 → 205, prefill ~24 tokens/turn, answers 42/52/\boxed{62} all correct.
Unmodified OpenAI clients now get delta-only prefill on thinking hybrids.

Process notes for future sessions: never `pkill -f` a pattern that matches the invoking
shell's own command line (killed our own launches twice); after killing a bridge, WAIT
for the port to actually free (ss -tln) before rebinding — the bind races the dying
process's TIME_WAIT and the new bridge dies after loading the engine.

## 2026-09-18 (night) — aider debugging: two client behaviors found, one fixed bridge-side

The aider live test showed n_reused=0 on every turn despite --auto-echo. BMOE_DEBUG_ECHO
payload capture (/tmp/bmoe-reqs.jsonl, dump of pre-canonical + canonical arrays) exposed
TWO aider behaviors, not one:

1. **Shrinking newest turn** (FIXED bridge-side): aider appends its edit-format
   boilerplate ("To suggest changes to a file you MUST return…") to the NEWEST user turn
   only, so the same turn's payload shrinks in the next request. First fix attempt
   (strip boilerplate from all-but-newest) was WRONG — the same turn renders differently
   across requests. Correct fix: strip from ALL user turns and relocate the boilerplate
   into the system prompt once (request-independent canonical form). Verified: canonical
   msgs byte-identical across captured requests, no duplicate scaffold on later requests.
2. **Edit-turn file re-add** (inherent, not fixable): after an accepted edit, aider
   re-adds the file with NEW content at a NEW position ("I updated the files." + re-add
   block). Semantically required (model must see the updated file) and structurally
   unavoidable for a hybrid (cannot rewind over the changed span). Measured directly by
   replaying captured payloads: edit turn prefilled 955 / reused 0 (42.6 s); a
   pure-question follow-up prefilled 26 / reused 1683 (1.6 s).

Practical guidance: with aider + LFM2.5 + --auto-echo, batch edits; non-edit follow-up
questions are now ~25x cheaper prefill. ADR-003 addendum, docs/serve.md, CHANGELOG
updated. The debug capture stays behind BMOE_DEBUG_ECHO (off by default).

## 2026-09-18 (night) — aider telemetry confirmed; blockers routed (ADR-004); lifecycle consolidated

Mixed aider session confirmed the ladder on LFM2.5: edit turns 426/815 prefilled with
0 reused (full clear, ~18/36 s); non-edit turns **18/16 prefilled with 587/1243 reused,
~0.8 s prefill**. User asked "why can't we rewind before the edit?" — answered: that is
the generic diff path (free on transformers, unmeasured on Ling-mini), blocked on
hybrids by the two upstream defects; bounded payoff (~half of edit-turn prefill; the
changed file content can never be reused).

User asked "why wait on upstream — can't we make these changes?" — answer recorded in
**ADR-004**: the submodule remote is `Helldez/llama.cpp` carrying one sanctioned
1-commit expert hook, so fork patches are possible but governed. Decisions: (1) the
lfm2moe graph-reserve fix goes as an **upstream PR to ggml-org/llama.cpp** (not
Helldez/llama.cpp — that is only the submodule mirror); (2) bit-exact restore is NOT
attempted now; any future fork experiment would be a 1-commit branch on
Helldez/llama.cpp with maintainer agreement, never a new personal fork. Trigger to
revisit: upstream chunk-boundary snapshots or equivalent.

**Consolidation (user choice):** `PROGRESS.history.md` merged back into `PROGRESS.md`
(resume section + history in one file); `PROGRESS.history.md` removed. ADR-004 indexed
in docs/README.md.

## 2026-09-18 (late) — Blockers phase: reserve root-cause + upstream PR; exactness baselines

User decision: attempt ALL blockers including bit-exact restore; route the reserve fix as
a mainline contribution; the pin stays untouched and bumpable throughout (work happens in a
separate clone, never in `third_party/`).

**Setup.** `ggml-org/llama.cpp` forked to `cjl4hd/llama.cpp`; clone at `~/git/llama.cpp`
(origin=cjl4hd, upstream=ggml-org, `helldez` remote added for pin archaeology). Release
build with tests; rollback-suite fixture models generated via
`cmake --build build -j4 --target test-llama-archs && ./build/bin/test-llama-archs -o build/tests/test-models/`.

**Repro A — the reserve crash.** First probe (upstream-default shapes: ubatch 512,
n_rs_seq 8) did **not** crash — the shortfall is graph-shape dependent. Mirroring the
engine's exact context shape (n_ctx = n_batch = n_ubatch = 2048, `use_extra_bufts=false`,
n_rs_seq 64) reproduced it byte-for-byte on pristine master `4fea119de`:
`needed 836640, available 836272`, `GGML_ASSERT(obj_new)` — the same numbers as on our pin,
proving it is not a pin artifact.

**Root cause.** `llama_context::graph_max_nodes()` buckets node budgets by arch; the
linear-attention family gets `max(n_tokens*40, 32*n_tensors)` because GDN graphs with
snapshot planes need the headroom. LFM2/LFM2MOE are on the rollback allowlist
(`llm_arch_supports_rs_rollback`) but missing from that bucket → default
`max(1024, 8*n_tensors)`, which their snapshot graph exceeds by exactly one `ggml_tensor`
object (368 B = GGML_OBJECT_SIZE + GGML_TENSOR_SIZE). Invariant to ctx/ubatch/budget
because the budget itself scales with those.

**Fix and PR.** Two lines (add the archs to the bucket) plus registration of the `lfm2` /
`lfm2moe` fixture models for `test-recurrent-state-rollback` — the generator already
produced them, but no rollback test had ever exercised either arch. Suite passes 7/7
(qwen35, nemotron-h, dsv4, kimi-k3 + new lfm2, lfm2moe). Committed `74e1ee6de`, pushed to
the fork, **PR opened: ggml-org/llama.cpp#29085**. Note: an outside contributor cannot
push a branch to `Helldez/llama.cpp`; mainline PRs therefore go via the user's own fork.
The Helldez 1-commit fork-branch option stays reserved for anything that must bridge the
submodule pin before upstream merges (needs maintainer agreement, AGENTS.md #1).

**Repro B — exactness baselines (phase 2 open).** New `diverge` mode: fresh greedy decode
vs greedy decode after a snapshot rollback, against an untouched reference context.
- Qwen3.5-9B: DIFFER at rollback depth 8 (restored stream began correct — `hello world` —
  then degenerated into `<|im_start|>` repetition).
- LFM2.5-8B: DIFFER at depths 8 and 3 (3 = the upstream fixture test's parity depth).
  Restored streams degenerate into prompt-tail echo loops.
- Position-dependence (sometimes-correct start) is consistent with the chunk-boundary
  hypothesis; the depth × ubatch matrix is the next artifact.
- (A fixture-model probe is invalid: synthetic test vocabs do not support
  `llama_tokenize` — the real models above are the evidence.)

**Tooling.** `tools/rsbench.cpp` → `bmoe-rsbench` (engine-shaped context, public API
only), the deliberate exception to the tools' no-llama rule, opt-in via
`-DBMOE_BUILD_TOOLS=ON`. A/B proof in one command: against our unfixed pin it aborts
(exit 134, backtrace through `ggml_view_3d` in the pin's older LFM2 graph); against the
patched clone it reserves cleanly. Pin/master backtrace call sites differ — re-verify the
repro after every submodule bump.

## 2026-09-18 (late, session 2) — exactness falsified as a harness bug; the m-law

Goal (next actions 2–3 of the previous resume): the depth × ubatch × snapshot matrix and
the chunk-boundary hypothesis test. Both executed — and the outcome falsified the premise.

**Source trace first** (pin @ `0e8c83e51`): GDN CPU kernel `ggml-cpu/ops.cpp:10752` is
strictly sequential per token and writes snapshot slot `n_tokens−1−t` per step (only
`min(n_seq_tokens, K)` planes per ubatch); conv writes in `delta-net-base.cpp:479` map
slot t → state `(n_seq_tokens−(ch−1)+t)…` clamped at 0; LFM2 shortconv (lfm2.cpp:211)
writes true per-token planes (guard `causal_attn` holds — LFM2 not in the non-causal list,
no `attention.causal` key in the gguf); `seq_rm` rollback needs depth ≤ n_rs_seq, is
single-use, and `rm_all` resets `rs_idx`; restore reads plane `rs_idx` via the
`s_copy` gather. During the trace: **`bmoe-rsbench diverge` was self-defeating** — its
continuation went through `greedy_generate`, which opened with a full `seq_rm(-1,-1)`,
wiping the pending rollback. Every prior exactness datum was that artifact.

**Rewritten tool** (`tools/rsbench.cpp`): `diverge` = one matched-feed cell (identical
token feeds both sides, pending rollback never cleared, reference continues instead of
re-clearing); `sweep` = mode {single, batched} × d_rm {1,3,8,24} × m {0,4} matrix, per-cell
predicted-vs-observed, plus a sensitivity probe (full-clear+re-prefill vs continuation)
that qualifies each prompt. Process lessons: stdout must be line-buffered (two 600 s
timeouts lost their buffered output before the fix); `pgrep -f` self-match false-positived
(the known trap from the evening entry); the counting-chain prompt is state-INSENSITIVE
(14/14 EXACT cells on Qwen3.5 including full-clear probes — EXACT there is uninformative);
the fox prompt is sensitive.

**The m-law (matched-feed, sensitive prompts, both families, pin and clone agree):**

- m=0: EXACT everywhere tested on qwen35 (d=1,3,8,24; single AND batched rm — the
  predicted single-mode conv collapse is falsified; the clamp yields oldest-window, which
  for slots 0/1 is correct) and on lfm2moe at d=1,3,24.
- m≥1: DIFFER everywhere, both families, all depths, both rm shapes. Mechanism:
  single-token steps refresh only plane 0; planes d≥1 keep last-multi-token-ubatch values
  (stale by m). This is the real edit-turn shape of the server.
- lfm2moe m=0 d=8: DIFFER identically in both rm shapes (`100` vs `99`) — residual anomaly,
  OPEN (next action 2).
- Upstream's fixture test passes because it never generates before the rollback (m=0 by
  construction) and (checkpoint path) both sides read the same planes; PR #29085's lfm2
  rows never exercised m≥1 either.

Evidence files (regenerable, ephemeral): `/tmp/sweep-q35.txt` (std matrix, insensitive
prompt), `/tmp/sweep-q35-fox.txt`-equivalent console output, `/tmp/sweep-lfm-std.txt`,
`/tmp/sweep-lfm-fox.txt`, `/tmp/reserve-pin.txt`; run via
`/tmp/bmoe-rsbench-clone sweep ~/llm/models/<model>.gguf [fox]` (clone-linked) or
`./build/tools/bmoe-rsbench` (pin-linked). Clone runner compile line in Artifacts above.

**Docs:** ADR-004 Addendum 2 written (falsification + law + candidate upstream fix:
ubatch-replay-before-restore or per-token plane maintenance); Addendum 1's exactness claim
struck; ADR-001 §Context 2 superseded; docs/serve.md and CHANGELOG 0.24.2 corrected.
Gates all green after the rewrite: clean build, 13/13 ctest, `reserve` still exit 134 on
the pin. No code-behavior change anywhere in the engine — the falsification changes
documentation and the upstream plan, not this repo's defaults (`--rs-seq` stays off for
the staleness reason now, not the non-exactness reason before).

## 2026-09-18 (late, session 3) — the mechanism mapped end to end; the index-shift fix

Goal (next action 1 of the previous resume): the staleness fix. Executed — but only after
the instrument itself was fixed one more time, which is where the session's real findings
live.

**Kernel-level plane law, final form** (GDN: `ggml-cpu/ops.cpp` `target_slot = n_tokens−1−t`,
slots ≥ n never written; conv: `lfm2.cpp` `n_written = min(n, K)`, `delta-net-base.cpp`
pre-fix clamped ALL K slots every ubatch; `s_copy` maps `rs_idx` 1:1 to plane rows): a
ubatch of n tokens writes planes 0..min(n,K)−1, plane p = state p tokens before its end;
single-token steps rewrite only plane 0. `seq_rm` reads plane d. Consequence: exact iff
the wanted state still occupies plane d — m=0 cuts into the last multi-token ubatch only.
The **d=8 anomaly is resolved**: the sweep's rm ubatch had exactly d tokens → plane d was
never written by it → garbage read, arch-independent (not an LFM quirk).

**The shape-noise confound (the session's decisive discovery).** A no-rollback control
(same tokens, 10-token prefill vs 6+4 split) moves logits by up to **3.6** on this backend
(MoE routing flips amplify accumulation-order noise). This invalidated: (a) upstream's own
multi-seq fixture, which FAILS on vanilla master (diff 11.6) comparing 12-token-ubatch vs
10-token-ubatch histories at eps=1e-7 — a bar nothing can meet; (b) every
`statecmp`/`dsteps` "restore is not bitwise" result from earlier this session (only the
d=0 identical-shape control was sound, and it passed). Lesson recorded as an open-questions
rule: bitwise comparisons need identical ubatch shapes; everything else is argmax + shape
controls.

**The fix** (`~/git/llama.cpp`, branch `fix/rs-rollback-index-shift` off origin/master
4fea119de): per-seq epoch bookkeeping (`rs_epoch_end` / `rs_epoch_planes` / `rs_epoch_lo`)
set in `find_slot` per multi-token ubatch; `seq_rm` restores plane `d−m` when the wanted
state survives, refuses honestly otherwise (destroyed planes; checkpoint-loaded state — a
state blob carries a single plane, so nothing exists to roll back into; cleared seqs;
pending rollback); `delta-net-base.cpp` conv writes aligned to min(n,K) (was: clamped all
K — desynced conv from GDN planes after single-token steps); `prepare` dry-run saves/
restores the bookkeeping; `rm_all`, tail invalidation and fresh starts reset it;
`seq_cp` inherits, `seq_add` follows affine shifts, `seq_div` invalidates. Code review
caught and fixed: discarded-timeline planes after rollback+single-token replay (the
`rs_epoch_lo` floor), stale epochs surviving sequence teardown, fixture's dst-side
impossible rollback (now asserts the refusal).

**Verification** (`cutsweep`, new rsbench mode: c tokens cut into the prefill × m
single-token steps, each cell with a no-rollback SHAPE-control row — the rescuable shape,
unlike `sweep`'s unrescuable ones):

- fix build: **9/9 EXACT** on lfm2moe AND qwen35 (controls SAME everywhere)
- vanilla: 3/9 EXACT (m=0), 4 DIFFER (m≥1), 2 REFUSED (c+m > n_rs_seq)
- fixture test passes on both cache fills: single-seq bitwise incl. checkpoint round-trips
  and dirty-context load; multi-seq shape-gated (probe in-test, asserts mechanics + refusal
  semantics, reports diffs on shape-dependent backends)

Tool notes: `cutsweep` uses `n_rs_seq=8` because LFM2 archs abort at graph reserve with
larger values until #29085 lands (runner must not depend on that PR). `predict()` and the
sweep's top-of-file comment now document vanilla's behavior; `sweep` cells against the fix
build report REFUSED/INFRA, which is the honest verdict for those shapes.

Evidence: `/tmp/cutsweep-{fix,vanilla}-{lfm,q35}.txt` (vanilla baseline via stash push/
pop around a rebuild — regenerable; see Artifacts). Upstream branch pushed to
`cjl4hd/llama.cpp` and opened as ggml-org PR #29117 (draft); PR description draft at
`/tmp/pr-index-shift-description-draft.md`.

Session 3 addendum (2026-09-19): the root-README feature tables (`In-flight features` +
`Forks`) and the AGENTS.md rule-6 wrap-up mandate landed on **`cjl4hd:main`** (`b1f34f7`),
not on this branch — the arc stays free of them and the tables document unreleased work
from the canonical repo's point of view. An intermediate commit carrying them
(`cc8a999`) was force-pushed off this branch at the user's request. Rule 6 lives on
`fork/main` for now; this branch picks it up at the next main→arc sync (or the eventual
stacked PR does).

## 2026-09-19 - Session 4: host feature-bench campaign; Ornith 1.5 done

User: benchmark ALL on-disk models, three cells each - (a) mmap baseline, (b) bmoe streaming
stack, (c) bmoe + llama-side features - results on `cjl4hd:main` linked from the in-flight
table. Scope per user: distinct archs (dense R1-Distill/Qwen3.5-9B and quant variants
skipped), Ornith first then approve.

Harness facts (must not be re-derived): background processes die between tool calls here
(`process_type=BACKGROUND` unimplemented; setsid/nohup dies too) - every cell lives inside
ONE 575 s window; tmux sessions DO survive (the 8017 server now runs in tmux session
`bmoe-serve`). `bench/host-rs` on `cjl4hd/llama.cpp` = upstream `4fea119de` + index-shift
`7b2ec36d1` + reserve `8f3e6776b` (cherry-pick) + expert-ready hook `2a8d47ac9`
(cherry-pick; ONE conflict - upstream's IQP fast path in `mul_mat_id` - resolved
hook-first so both consumers gate on it); pushed. `build-bench/` = the arc built against
the clone (submodule detached at `2a8d47ac9`, pin restored after). `session.cpp` carries
a WORKING-TREE-ONLY port: upstream `4fea119de` renamed
`common_speculative_draft_params.n_past` to `pos0` (MTP-off path unaffected; NOT
committable - the pin still has `n_past` and would fail to build).

`scripts/cellc.sh` committed (arc): one scenario per invocation; verified 42/52/62 chain
over the bridge with word-boundary answer checks; scenarios c1 warmup-off (seeds the
warmup file), c2 warmup-on (replays), c3 `--auto-echo` + `preserve_reasoning` + echoed
chain; `MAXTOK` env (160 for the 35Bs). Warmup cache backed up on first call, restored
when c3 exits.

Ornith-1.5-35B-Q4_K_M (`qwen35moe`, 20.2 GB, 4-core host, 8.5 GiB avail RAM):
(a) baseline **1.39 tok/s**, load 145 s, **470 majflt/tok** (thrash); (b) streaming stack
**2.06 tok/s (+48%)**, load 86 s, cache hit 88.6%, 129 majflt/tok; (c) warmup T1 prefill
40.7 s -> **8.6 s (4.7x)**; auto-echo T3 **n_reused 98 / n_prompt 28**, prefill -50%,
tok/s **+69%**, answers verified. T2 stays a full clear: the FIRST echoed turn's render
must reconcile with what was generated (same qwen35 template mechanism as Qwen3.5-9B),
then later turns ride the cache. `--rs-seq` eligible (bench build) but off: Ornith's
echo-style reuse is template-blocked. Evidence: `/tmp/bench-ornith/*.{csv,log}`.

Results published: `docs/host-benchmarks.md` on `cjl4hd:main` (`8be5bc5`) with the README
residency row pointing at it; tooling caveat documented (bridge/cellc live on the arc
until #197 merges).

## 2026-09-19 (session 5) — c4/c5 divergence cells; the c5 OOM (three kernel kills)

Continued session 4's follow-ups: cellc.sh gained c4/c5 (divergence-turn cells: the
second request resends T1 + the engine's own reply and asks a NEW question — a hybrid
must rewind to just after A1, so rs-seq OFF full-clears (c4) and rs-seq ON is the
bounded rewind (c5); verify 42 then 62). Uncommitted alongside it: the session.cpp
pos0 port (working-tree-only, NOT committable — session 4's protocol note).

**The OOM (why sessions kept dying):** the kernel OOM-killer killed bmoe-cli three
times today (10:57, 12:32, 12:48; all ~10.4–10.5 GB RSS, all in bench-driven scopes).
Root cause is arithmetic: c5 launched with `--rs-seq 160` (a mid-session edit meant to
cover MAXTOK) → `llama_memory_recurrent: size = 10112.81 MiB ... S (f32): 9660.00 MiB`
on Ornith (~60.4 MiB per snapshot plane × 160) on an 11 GiB host. Load completes, the
first token work thrashes swap, OOM. Nothing in the engine is at fault: seq_rm
failures fall back to full clear everywhere (verified), and c4/c5 content generation
ran clean at normal RSS before each kill.

**c4 result (rs-seq OFF, completed pre-kill):** verdict ok (42/62 verified); T1
n_prompt 24 / n_reused 0; T2 divergence n_prompt 56 / n_reused 0 — the designed
full-clear baseline. Evidence: `/tmp/bench-ornith/server-c4.log` + `c4/`.

**c5 result (rs-seq 64, rerun after resume with user approval):** verdict ok (42/62
verified). T1 n_prompt 24 / n_reused 0; T2 divergence n_prompt 33 / **n_reused 23** —
the snapshot rewind fired (restored to just after T1, re-prefilled only the
plain-rendered reply + question, ~10 fresh tokens) and the restored stream decoded 62
correctly with no degeneration: the index-shift fix's first live end-to-end proof
through the real engine on a 35B hybrid (cutsweep's 9/9, now live). Honest wall-clock
read: prefill_s 15.45 ≈ c4's 14.50 despite 23 fewer prompt tokens — on this IO-bound
streaming host cached-token compute is not the bottleneck, so c5's win is
mechanism-proving, not latency. No OOM: snapshot cache ≈3.9 GiB at 64 planes, peak
fit within the 9.3 GiB available. Evidence: `/tmp/bench-ornith/{server-c5.log,c5/}`.

**Cleanup done:** the user's global warmup.json restored from the cellc backup (a c5
attempt had clobbered it with a 196-byte file; leftover copy kept at
/tmp/warmup-c5-leftover.json); no stray listeners; cellc.sh + bmoe-serve.py syntax
gates pass. The session-5 commit (cellc.sh c4/c5 + this record) is local-only — the
push to `fork` is left for the user to call.

**Publish + wrap-up (same session, user request):** the Ornith c4/c5 rows are published
on `cjl4hd:main` `5988e17` — via the new `scripts/publish-host-bench.sh` (throwaway
worktree of fork/main → apply → drift guard vs origin/main → rule-7 identifying-data
scan → single commit → push HEAD:main), doc diff archived on the arc as
`scripts/host-bench-ornith-c4c5.patch`; landed tree verified with
`git diff origin/main fork/main --stat` (only intended docs). The published doc
corrects the Ornith `--rs-seq` bullet: echo-style reuse stays structurally blocked on
qwen35 templates, but the c5 rewind proves the snapshot-rollback path engages. The arc
carries cellc.sh c4/c5 + the publisher + this record (`20841ec` + the wrap-up commit);
the session.cpp pos0 port remains working-tree-only (stashed for pin builds + gates).

## 2026-09-19 (session 6) — Cyber-Tiel-Coder-35B full batch; the warmup mechanism resolved

Next action 2 of the session-5 resume executed for the queue head. Gate 1 re-run first
(stash pos0 port → pin build clean → 13/13 ctest → pop). Cell (b) ran first by accident —
`bench-report.sh` hardcodes `--moe-stream --cache-mb auto --io-threads 4 --overlap
--dense-weights anon`, which IS cell (b); cell (a) is the same protocol as a direct
pin-CLI run minus the streaming flags (recorded in the published protocol rows).

**Results (all verdicts ok, answers verified):** (a) 1.29 tok/s, load 47 s, 616
majflt/tok — thrash profile; (b) 2.19 tok/s (+70%), load 178 s, 72 majflt/tok, hit
81.9%, prefill 68.3 s — SLOWER than (a)'s 61.8 s, honestly so: prefill routes nearly
all experts so streaming has nothing to skip and pays the streamer's overhead; (c1–c3)
auto-echo T2 28 prompt / 222 reused, T3 28/265, prefill 42 → 10–12 s; (c4) divergence
full-clear T2 236/0; (c5) T2 REWOUND 33 prompt / 203 reused, 62 correct, no
degeneration — T2 prefill 42.6 → 19.3 s. Unlike IO-bound Ornith (c5 = mechanism proof,
no latency win), Cyber-Tiel's rewind pays WALL-CLOCK: 203 skipped tokens outweigh the
restore. And c5's T1 came back **1 prompt / 203 reused** (2.6 s vs ~45 s everywhere
else): warmup and rs-seq COMPOSE — the replay seeds what the rewind restores.

**Mechanism find (code-verified in session.cpp generate()):** the hybrid clear-block
runs BEFORE the residency diff — with `n_rs_seq==0` a hybrid may only APPEND to the
resident mirror; any non-append turn (warmup T1's short render, a divergence)
full-clears unconditionally, cells cannot be rewound without snapshots. This resolves
the c2 anomaly (replay completed — log "4/4 messages resident in 59s" — yet T1
n_reused 0): the warmup cells' zeros are the designed worst case, and what warmup buys
without snapshots is cold-start only (c2 T1 42.4 s vs c4's identical T1 47.6 s). With
snapshots the clear-block is skipped and the diff path is legal — hence c5. Corollary
recorded: a clobbered warmup.json costs nothing structurally (pre-snapshot, any
non-append turn cleared anyway); session 5's restore-after-clobber was precautionary.

**Publish:** `cjl4hd:main` `4418988` (rows + reading bullets + queue line), then a
correction `d19ead7` — the first patch carried cell (b) prefill 28.1 s, a pattern slip
from Ornith's sibling row; the run's own CSV says 68.255 s. Caught in the post-publish
audit against evidence. Rule going into Next action 1: never transcribe a published
number from memory or a sibling row — re-read the summary line of the run's own CSV.

**Tooling:** cellc.sh readiness wait 150 → 420 s (Cyber-Tiel's streamed load alone is
178 s; every c-cell on a 35B would have false-FAILed with SERVER-FAILED). Watched PRs
checked: #29085 OPEN/REVIEW_REQUIRED, #197 OPEN — held items stay held. Session commit
left local for the user to push (session-5 convention); `fork/main` carries the
published doc rows regardless.

## 2026-09-19 (session 7) — README perf column; summary/conclusions/recommendations; LFM2.5-8B batch

User request: refresh the README in-flight perf column with the Cyber-Tiel point; give
host-benchmarks.md a summary, conclusions, and recommendations; then repeat for LFM2.5.

**Doc work (`3999832`):** three README rows refreshed — append-reuse gained the
Cyber-Tiel host point; the `--rs-seq` row's stale "restore not bit-exact" claim
(falsified by the session-3 cutsweep and two live c5 proofs by then) replaced with the
true state; the warmup row qualified with the composition finding.
host-benchmarks.md gained a Summary / Conclusions / Recommendations section grounded
strictly in published rows, plus a protocol-line fix (192-token budget for thinking
models, ahead of the LFM2.5 cells that use it).

**LFM2.5-8B-A1B batch (`6d0bc1e`, `f9b9f4f`) — the fits-RAM contrast case** (5.0 GB on
11 GB RAM; lfm2moe): (a) 9.69 tok/s, load 10.5 s, prefill 1.65 s, 0 majflt/tok;
(b) 8.64 tok/s, prefill 7.9 s — streaming is a NET LOSS with nothing to fix: 0 faults
in both cells, so the (b) stack only pays overhead. This is the doc's third profile and
it sharpened the recommendations: streaming is for at-or-past-RAM models, not a default.
(c) warmup T1 7.59 → 1.12 s (6.8×) — on a resident model the replay is pure prefill
speed, no fault storm; reuse still 0 (prefix-cut, same designed mechanism). (c) auto-echo
T2 24 prompt / 124 reused, T3 24/205, prefill ~1.2 s — LFM2.5's template echoes reasoning
natively so reuse engages from the FIRST follow-up (no qwen35-style reconcile turn).
(c4) 48/0 full clear. (c5) rewind 26 prompt / 22 reused, prefill 2.82 → 1.25 s, answer
62 verified; T1 1 fresh / 22 reused at 0.12 s — warmup+rs-seq composition replicated on
a second arch family, and the rewind's wall-clock win confirmed on a model where the
skipped tokens are few but cheap.

**Second self-caught slip:** the fits-RAM recommendation first said "7× faster prefill";
7.9/1.65 is ~5×. Caught in the post-publish audit, fixed `f9b9f4f` same session. The
session-6 audit rule extended: derived ratios are audited against the CSVs too.

**State:** gates re-run green (pin build + 13/13 ctest, stash/pop around); no stray
listeners or engine processes; warmup cache holds the LFM2.5 chain; the arc is two
commits ahead of `fork/feat/session-residency` (session-6 and -7 wrap-ups, push left to
the user per convention); `fork/main` carries all doc commits (5988e17 → 4418988 →
d19ead7 → 3999832 → 6d0bc1e → f9b9f4f). Queue head: Laguna-XS-2.1.

## 2026-09-19 (session 8) — arc pushed; Laguna-XS batch: the first non-hybrid

User request: push the arc, then the Laguna-XS-2.1 batch and publish. Push done first
(`2070368..8958e88`), clearing the two-commit backlog.

**Classification find (the session's real result).** Laguna measured unlike every prior
model and the c-suite only made sense under one hypothesis: upstream classifies `laguna`
as a plain-transformer MoE. Verified three ways — `llama-arch.cpp` lists it in neither
`llm_arch_is_hybrid` nor `llm_arch_supports_rs_rollback`; the gguf carries only standard
attention keys (sliding window, rope dims) with no recurrent/conv state tensors; and the
c5 cell is behaviourally identical to c4 (snapshot pool unused). Consequences, all
published: the divergence turn reuses 55 tokens with rs-seq OFF (hybrids full-clear the
same shape — the divergence tax is a hybrid phenomenon); warmup composes freely (T1
1 fresh / 54 reused); `--auto-echo` is REDUNDANT on this template and its echo rewrite
actually costs a 252-fresh reconcile turn (c3 T2 44 s) before a decode-side T3 win —
recommendation: leave it off on laguna-class templates. Also the first model whose
PREFILL speeds up under streaming (47.5 → 34.1 s): a ~2×-RAM baseline thrashes prefill
too. An earlier header edit claimed Laguna was the MTP-capable arch — the post-publish
audit proved it backwards (Cyber-Tiel has `qwen35moe.nextn_predict_layers=1` + nextn
tensors; Laguna has zero) and `8c77846` restored the original carrier claim plus exact
fault ratios (3.6×/8.6×/9.9×, not "~6–9×").

**Process:** MAXTOK ladder measured — 192 and 256 truncate Laguna's think spans (coherent
verbose reasoning cut at the budget; verdict FAIL, empty content), 384 passes all cells;
the CLI has no reasoning-budget wiring (upstream's common/reasoning-budget.h unwired in
bmoe-cli), so max_tokens is the honest lever. All five cells ran in ONE detached tmux
loop (~25 min wall) since background processes die between tool calls; the loop self-
terminated and every result was re-derived from the on-disk logs (per-turn JSONs +
TELEMETRY lines), answers 42/52/62 verified in every cell.

**State:** no stray listeners/processes; user's tmux session untouched; warmup cache
holds the Laguna chain; arc synced at 8958e88 (wrap-up commit local on top);
`fork/main` doc chain now 5988e17 → 4418988 → d19ead7 → 3999832 → 6d0bc1e → f9b9f4f →
d336133 → 8c77846. Queue head: Ling-mini-2.0.

## 2026-09-19 (session 9) — Ling-mini-2.0 batch: second non-hybrid, the barely-fits edge

User request: push the session-8 wrap-up, then the Ling-mini-2.0 batch and publish.
Push done first (`8958e88..b7f3cd0`).

**Classification (verified before the c-suite ran, the session-8 lesson applied):**
`bailingmoe2` is in neither the hybrid nor the recurrent lists; the one rollback-list
hit in the grep was BAILINGMOE3 — a different arch. Second plain-transformer MoE in the
matrix, and a NON-THINKING model (no reasoning span in replies — the session-2
behavioral-probe finding). All c-suite differences followed: reuse native from the
first follow-up (c1 T2 27 prompt / 34 reused, no echo, no reconcile turn), `--auto-echo`
a verified no-op (c3 ≈ c2 with warm-cache decode gains), divergence turns free partial
chops with rs-seq OFF (c4 27/34), c5 ≡ c4 (snapshot pool unused), warmup composing
freely (T1 1 fresh / 31 reused; ~10× on prefill seconds, 0.08–0.8 s across cells).

**The barely-fits edge case (the batch's headline).** At 9.9 GB on 11 GB RAM the mmap
baseline is essentially resident: 12.59 tok/s, 0.86 majflt/tok, load 21.3 s. The (b)
streaming stack LOSES 31% decode (8.63 tok/s) with prefill 5.6× slower (12.75 s) —
because its 7.5 GiB expert cache plus the anon dense copy push the model into swap
(0.86 → 19.27 majflt/tok): the cache itself became the memory pressure. Folded into
the recommendations as the second and sharpest fits-RAM data point (LFM2.5: −11%;
Ling-mini: −31%). One audit catch: 7686.8 MiB is 7.5 GiB, not 7.7 — fixed `8605806`
same session (unit conversions joined the audit rule).

**Process:** c4's T1 prefill row (34.4 s at 1.0 tok/s) is the cold-load cell racing the
prior cells' page cache — noted as the outlier in-doc with its comparable T2 figure.
All five cells ran in ONE tmux loop in ~5 min (fast model); loop self-terminated;
results re-derived from on-disk logs, answers 42/52/62 verified everywhere.

**State:** no stray listeners/processes/tmux; user's tmux session untouched; warmup
cache holds the Ling-mini chain; arc synced at b7f3cd0 (wrap-up commit local on top);
`fork/main` doc chain through 8605806. Queue head: Qwen3-30B-A3B.

**Session 10 (2026-09-19, late) — upstream: #29085 finalization pass.** Trigger:
"lets go back to pr 29085, and review whats needed for a clean submission."

- **Bot flags vs reality:** the 09-18 bot flagged (1) template-not-respected and (2)
  AI-generated content. The template text had been posted as a COMMENT (20:23) with
  the AI disclosure in the user's own words — comments don't satisfy the checker
  (body-only). Commit `74e1ee6de` clean (author cjl4hd, no AI trailers); PR still a
  DRAFT so full CI never ran.
- **Drift audit (user: summarize the 17, check overlap, merge?):** the 17 are
  upstream's own work — nothing to move. Only `efa28e950` (test-llama-archs dummy
  vocab) touches what the PR depends on (its new rollback rows' fixture generator).
  Zero commits touch the reserve path (ggml/src + llama-context byte-identical from
  `4fea119de` to tip). Branch stays 1 commit, MERGEABLE — no merge/rebase needed.
- **Tip re-verification** (`/tmp/lp-verify`, lean CPU build):
  ```bash
  git worktree add --detach /tmp/lp-verify upstream/master
  cmake -S /tmp/lp-verify -B /tmp/lp-verify/build-tmp -DGGML_CUDA=OFF \
    -DGGML_VULKAN=OFF -DLLAMA_CURL=OFF -DLLAMA_BUILD_TOOLS=OFF -DGGML_OPENMP=OFF \
    -DCMAKE_BUILD_TYPE=Release
  cmake --build /tmp/lp-verify/build-tmp -j4 --target test-llama-archs \
    test-recurrent-state-rollback
  /tmp/lp-verify/build-tmp/bin/test-llama-archs -o /tmp/lp-verify/build-tmp/test-models/
  # BASE (no fix): dummy lfm2 rollback PASSES; real LFM2.5 Q4_K_M: creation +
  # single-seq rollback OK — the assert does NOT fire at tip in this config.
  git show 74e1ee6de -- src/llama-context.cpp tests/CMakeLists.txt | git apply
  cmake --build /tmp/lp-verify/build-tmp -j4 --target test-recurrent-state-rollback
  # FIXED: dummy lfm2 + lfm2moe PASS; real model identical to base.
  ```
  Verdict: the fix is behaviorally inert in this config (base ≡ fixed) — the 368-byte
  razor-edge shortfall manifests config-dependently (assert on the original repro
  build, clean pass here); the classification argument stands.
- **NEW upstream finding — CPU multi-seq split-replay mismatch (real LFM2.5,
  11.587):** base and fixed identically FAIL `test_multi_seq_split_replay` on the real
  model (max diff 11.587, first at seq 0 pos 16, deterministic); dummies match
  (1.2e-10 / 0); single-seq `test_rollback` restores successfully. Test source:
  mismatch ⇒ `return false` (eps 1e-7) — earlier exit=0 readings were pipeline
  artifacts (`$?` trap). Pre-existing (no CPU-path commits since `4fea119de`); first
  seen today because the real model never got past context creation on CPU before.
  Our engine unaffected (single-seq rollback usage; c5 verified). User decision:
  track here, narrow the repro before any upstream report; NOT in #29085.
- **PR body rewritten onto the template:** Overview / Additional information /
  Requirements (user's Overview sentence + AI disclosure kept; re-verification
  results + the out-of-scope mismatch noted; "Ran CI locally" unchecked — user's
  call). `gh pr edit` broken on ggml-org (Projects-classic GraphQL deprecation) —
  applied via `gh api -X PATCH repos/ggml-org/llama.cpp/pulls/29085 -F
  body=@/tmp/pr-29085-body.md`, verified landed. PR left DRAFT (user decision).

**State:** no bmoe-repo code changes; `session.cpp` pos0 port untouched; arc `b7f3cd0`
(+ wrap-up local); #29085 template-compliant + draft; verify worktree kept at
`/tmp/lp-verify` (patch applied, regenerable — commands above); queue head
Qwen3-30B-A3B (bench track unpauses next session).

**Addendum (same session — local CI attempt + arch sweep):**

- **Local CI attempt 1 (07:19):** `~/git/lp-ci` worktree (tip `f072b1037` + FULL patch
  incl. test rows, verified 2+4 greps) + `bash ci/run.sh ./ci-results ./ci-mnt` in
  tmux → died at the git-lfs prerequisite check (`command -v git-lfs` fails; tokenizer
  tests read LFS-tracked vocab ggufs). Nothing built; rerun after installing git-lfs
  (Next actions 1 has the exact command).
- **Arch sweep (user todo: "check other new or less popular architectures that support
  recurrent rollback"):** set-diff of `llm_arch_supports_rs_rollback` (10: BAILINGMOE3,
  DEEPSEEK4, KIMI_K3, LFM2, LFM2MOE, NEMOTRON_H, NEMOTRON_H_MOE, QWEN35, QWEN35MOE,
  QWEN4EXP) against the `graph_max_nodes` budget list (16): the only gaps are
  **NEMOTRON_H + NEMOTRON_H_MOE**. Process note: the first sweep regex grabbed the
  wrong function region and reported all 10 as gaps — re-anchored on the exact
  signature before believing it (trust the artifact over the first answer).
- **Nemotron probe:** dummy `nemotron_h-dense.gguf` rollback suite PASSES on the lean
  build (exit 0, split replay max diff 0, `/tmp/nemotron-probe.log`) — a latent-only
  gap, severity model-dependent (LFM2 precedent: its dummy passed too, its real
  Q4_K_M asserted). Upstream already registers nemotron rollback test rows, so a
  scope extension of #29085 is literally +2 lines in the same list; a separate PR is
  also defensible. Decision pending.
- **Mismatch narrowed:** nemotron multi-seq split replay = diff 0 on CPU ⇒ the
  real-LFM2.5 11.587 mismatch is lfm2-family-specific, not a generic CPU-hybrid issue.
- **Nemotron scope decision (user):** cannot resolve on this hardware (no real model)
  → raise the upstream issue and leave it to someone with the hardware; draft issued
  at `/tmp/nemotron-budget-issue-draft.md` for the user to own and post. #29085 stays
  LFM2-minimal.

## 2026-09-20 — Session 11: #29085 ready for review; Qwen3-30B batch; MISTAKES.md born

Continued directly from session 10's followups (user: "continue with followups").

- **#29085 → READY FOR REVIEW.** REST `-F draft=false` is silently ignored (response
  still `draft=true`) — the GraphQL `markPullRequestReadyForReview` mutation behind
  `gh pr ready` is the only way; that worked first try. Full ggml CI now runs on
  GitHub; local CI was already green; body carries both checked boxes.
- **Arc pushed** `b7f3cd0..9624b13`; gate 1 green (stash → clean pin build → ctest →
  pop; port restored, verified `1 file changed`).
- **Qwen3-30B-A3B batch (arch `qwen3moe`, 18.6 GB, ~2× host RAM):**
  - Cells (a)+(b) via a two-cell tmux runner: (a) direct pin CLI (bench-report's
    exact params minus the streaming stack — `-n 256 -t 8 --ubatch 512`, same essay
    prompt): 2.44 tok/s, 340.38 majflt/tok, load 117.7 s, prefill 44.53 s (n_prompt
    34). (b) `bench-report.sh`: 3.59 tok/s, 1.91 majflt/tok, 93.4% hit, read 10327.6
    MiB = 40.3/tok, prefill 22.11 s (halved — second arch with streaming prefill
    wins, after Laguna), 0 dropped experts.
  - c-suite: **MAXTOK ladder needed 768** — 192: r1 content empty (FAIL); 384: still
    truncated (1313 chars, cut at "I think"); 768: all five cells `verdict: ok`,
    answers 42/52/62 (c4/c5: 42/62) verified from `c*/r*.json`.
  - Classification predictions all reproduced (`qwen3moe` on neither the hybrid nor
    rollback lists): native reuse from the first follow-up (c1 T2 28/22, T3 28/50),
    warmup T1 24.3 → 1.23 s (~20×), auto-echo a verified no-op (c3 ≡ c2), divergence
    free partial chop (c4 T2 28/22 with rs-seq OFF), c5 ≡ c4 (pool unused) — third
    plain-transformer confirmation after Laguna and Ling-mini.
  - Process: two failed c-suite launches taught the pkill self-match lesson (below);
    the working launch is `tmux new-session -d -s c-q30 'cd <root>; for C in c1..c5;
    do MAXTOK=768 scripts/cellc.sh "$M" /tmp/bench-q30 $C > run-$C.log; done'` —
    **3 positional args, `mkdir -p` the OUTDIR first** (a missing OUTDIR fails every
    redirect and burns the loop in milliseconds).
  - Published `091ea05` on fork/main via the publisher (section + queue line +
    summary/conclusions/recommendations + README: reuse 22–310 across five models,
    divergence 22-point, warmup ~20×). Post-publish audit: all ratios recomputed from
    CSVs/logs (47.4%→"+47%", 2.01×→"halved", 19.7×→"~20×", 40.3 MiB exact, gain range
    42–70% exact) — no slips this time.
- **`docs/MISTAKES.md` created** (first entry; trigger = 3× same failure class):
  `pkill -f <pattern>` matches the calling shell's own cmdline and kills the rest of
  the compound command — three swallowed-tail cleanups cost ~30 min. Rule: bracketed
  patterns (`pkill -f "[c]ellc.sh"`) or standalone kill + post-assert; never chain
  `pkill -f X` with follow-ups.
- **Nemotron issue NOT posted** (user posts it themselves — AI-content rule); draft
  stands at `/tmp/nemotron-budget-issue-draft.md`.

**State:** gates green at wrap-up; no stray listeners/engine processes; tmux bench
sessions self-terminated; user's tmux session ("0") untouched; arc `9624b13` (+ this
wrap-up local); fork/main through `091ea05` carrying all six models' doc rows; queue
head Qwen3.6-35B-A3B, then OLMoE-1B-7B.
- **Local CI GREEN (attempt 2):** after user-level git-lfs install (`~/.local/bin`,
  v3.8.0 tarball, no sudo) + `git lfs install` + `git -C ~/git/lp-ci lfs pull`, the
  full `ci/run.sh` CPU run finished **CI_EXIT=0**: 54/54 debug, 55/55 release (both
  new rollback rows Passed: lfm2 0.16 s, lfm2moe 0.18 s), 5/5 + 5/5 model-labeled
  suites, qwen3-0.6B download→convert→12 quantizations→ppl + save/load suites all
  passed; backend-ops, archs models, tensor-split, scripts — zero FAILED lines in
  any log. PR body updated with both checked boxes (template boilerplate, backed by
  the run). Evidence: `~/git/lp-ci/ci-results/*.log` (~9 GB worktree incl. models-mnt
  — removable after the PR lands, keep the logs).

## 2026-09-20 — Session 12: Qwen3.6 batch; echo-reconcile is template-sensitive; first live rewind refusal

User: "Lets push the wrap up. Then continue next model batch. No need to check PR CI,
as I'm monitoring that."

- **Arc pushed** `9624b13..dd43788` (session-11 wrap-up).
- **Qwen3.6-35B-A3B batch (arch `qwen35moe`, 22.1 GB UD-Q4_K_M — largest in the
  matrix, ~2× host RAM):**
  - Cells (a)+(b) via the two-cell tmux runner: (a) 1.43 tok/s, **533.0 majflt/tok**
    (deepest thrash measured), load 93.2 s, prefill 31.94 s; (b) **2.05 tok/s
    (+43%)**, 3.68 majflt/tok (~145× collapse), 87.4% hit, 13798.3 MiB = 53.9/tok,
    prefill 18.96 s (1.7×) — **third arch where streaming speeds up prefill too**
    (after Laguna, Qwen3-30B; the ~2×-RAM baseline thrashes its prefill as well).
  - c-suite at MAXTOK=192 first try (r1 think span 419 chars — lighter thinker than
    Qwen3-30B's 1313): all five cells `verdict: ok`, answers 42/52/62 (c4/c5: 42/62)
    verified from `c*/r*.json`.
  - **FINDING 1 — echo reconcile is template-sensitive, not arch-sensitive:** c3's
    auto-echo ENGAGED (prompts balloon 52 → 185 → 350 — the rewrite is in the
    payloads) yet `n_reused` stays 0 everywhere, while Cyber-Tiel (SAME `qwen35moe`
    arch) reuses from T3. Different chat template ⇒ different render ⇒ no reconcile.
    First matrix model where echo costs prefill and buys nothing. Lesson: check the
    first follow-up's `n_reused` on any new template before trusting echo.
  - **FINDING 2 — first live rewind refusal:** c5 T2
    `seq_rm: rollback refused: seq=0 depth=135 pending=0 want_idx=2 epoch_end=23
    epoch_planes=2 epoch_lo=0` — the divergence must rewind past Qwen3.6's whole T1
    turn (~105-token think span + reply) and 64 planes cannot reach; engine refuses
    honestly → full clear (52/0 ≡ c4; refused attempt costs ~6.4 s: T2 prefill 21.9
    vs 15.5 s). c5 T1 still composed warmup+snapshots (2/22 at 2.6 s vs c2's 13.2 s).
    Corollary: snapshot budgets bound TURN DEPTH, not context; a heavy thinker's
    single turn can exceed any affordable budget — read the `depth=` line before
    assuming coverage.
  - Published `d24f55e` on fork/main via the publisher (section + queue line →
    OLMoE only + summary/conclusions/recommendations + README: three rows updated).
    Post-publish audit: 6/6 ratios recomputed from raw evidence, all match; refusal
    line verbatim in `server-c5.log` — no slips.
- **Protocol updates for the queue's last model (OLMoE-1B-7B):** read `depth=` on
  refusals; check first-follow-up `n_reused` before trusting echo; c-suite launch =
  3 positional args + `mkdir -p` OUTDIR first (session-11 lessons, now in Next
  actions 3).

**State:** gates green at wrap-up; no listeners/engine procs; only the user's tmux
session ("0") alive; arc `dd43788` (+ this wrap-up local, push is yours); fork/main
through `d24f55e` carrying all SEVEN models' rows; queue head OLMoE-1B-7B (last).

## 2026-09-20 — Session 13: OLMoE completes the eight-model queue; fits-RAM law refined

User: "TODO unrelated to this project: lets increase swap to 8GB permanently… Then lets
continue with suggested followups."

- **Arc pushed** `dd43788..1094910` (session-12 wrap-up).
- **Swap task (host-level, not project):** current swap is `/swap.img` 4G at 80% used;
  fstab also carries a stale line for a nonexistent `/swapfile_extra`. Handed the user
  the sudo commands (swapoff → rm → fallocate 8G → mkswap → swapon; fstab line 12
  already makes it permanent; stale line removed conditionally). NOT executed at
  wrap-up — swap still 4G/3.0G used. Relevant context for the OOM law: the host has
  been leaning on swap during bench bursts; 8G widens the margin for c5-style snapshot
  budgets.
- **OLMoE-1B-7B batch (arch `olmoe`, 4.0 GB, non-thinking):** classification verified
  in the pin (absent from both hybrid and rollback lists). (a) 10.75 tok/s, 0 majflt,
  load 8.2 s, prefill 1.67 s; (b) 10.13 tok/s (−6%), 0 majflt both cells, 97.0% hit,
  2.3 MiB/tok, prefill 7.81 s; (c1) T2 26/34, T3 26/69; (c2) warmup T1 6.6 → 0.12 s
  (**~56×**, largest in the matrix); (c3) ≡ c2 (no-op); (c4) 26/34 free chop; (c5) ≡
  c4. Suite ran start-to-finish inside one 4-minute poll window (small model).
  Answers 42/52/62 verified everywhere.
- **Fits-RAM law refined:** OLMoE's 3.7 GiB cache coexists with the 4 GB model at
  zero faults (−6% decode), vs Ling-mini where the 7.5 GiB cache pushed a 9.9 GB
  model into swap (−31%) — the verdict is the model-plus-cache sum.
- **Published `632fdf7`** via the publisher (section + queue-closure line +
  summary/recommendations + README rows: reuse across seven models, OLMoE divergence
  point, ~56× warmup, OLMoE added to the echo no-op set). Audit clean; one near-miss
  caught pre-publish (README model count six → seven).
- **Queue CLOSED.** Eight models span the full profile space: 3 qwen35moe hybrids
  (~2× RAM, thrash→streaming-wins, echo-reconcile shapes, hybrid full-clears,
  warmup+rs-seq composition with a live depth-135 refusal), 1 LFM hybrid (fits RAM),
  4 plain transformers (free chops, c5 ≡ c4, warmup composes for free). Next: the
  post-queue list (Next actions 3).

**State:** gates green at wrap-up; environment clean; arc `1094910` (+ this wrap-up
local, push is yours); fork/main through `632fdf7` carrying all EIGHT models' rows;
swap resize pending on the user's sudo.

## Session 14 — 2026-09-20: evidence tables refreshed (`eb8ec6a`); wrap-up push verified

- **Push verified:** `c6db52b` confirmed on `fork/feat/session-residency` — the
  previous turn's exit-1 was pipe noise, ancestry was clean.
- **Target audit before editing:** the queued next-action named
  `docs/benchmarks.md`/`docs/serve.md` — both stale. `docs/benchmarks.md` is the
  ANDROID device matrix (its Provenance section says so; host rows already live in
  the dedicated `docs/host-benchmarks.md`), and `docs/serve.md` does not exist.
  Real refresh targets: `docs/benchmark-method.md` (the method doc) and
  `docs/session.md` (bridge behavior).
- **Published `eb8ec6a`** via the publisher (2 files, +25/−1):
  - `docs/benchmark-method.md`: new subsection **"The host campaign"** under
    "Where the published numbers come from" — the eight-model scope (3 qwen35moe
    hybrids ~2× RAM, 1 LFM hybrid, 4 fits-RAM transformers) and the three
    protocol rules generalized for future suites: MAXTOK laddered per model (a
    FAIL may be a truncated think span — Qwen3-30B needed 768), echo
    template-sensitivity (check the first follow-up's `n_reused`; same-family
    archs diverged), and `depth=` reading on rewind refusals (the plane budget
    bounds turn depth, not context).
  - `docs/session.md`: the thinking-turn fallback paragraph now states that how
    much reuse claws back is **template-sensitive, not arch-sensitive** (two
    same-family archs diverged in the host matrix), with the host-benchmarks
    cross-link.
- **Not touched:** `docs/benchmarks.md` (Android-scoped, nothing falsified),
  README (rows current through OLMoE). The session.cpp pos0 port flag in
  `git status` is the known working-tree-only port, never committed.
- **Environment:** clean (no serve listeners, no bench processes). Swap still 4G
  (~3G used) — user's sudo pending.

**State:** arc `c6db52b` pushed; the session-14 record commit goes on top and is
pushed; fork/main through `eb8ec6a`; swap resize pending; Nemotron issue draft
still with the user; #29085 with the user for CI monitoring.

## Session 15 — 2026-09-20: swap 8G live; aider edit-turn reuse PROVEN (504/576)

- **Swap verified** (user ran the sudo): 8.0Gi total / 658Mi used, fstab line 12
  permanent, stale `/swapfile_extra` line gone. Impact doctrine recorded in
  Environment: (1) thrash profiles UNCHANGED — the ~2× RAM models' majflt/tok
  counts file-backed page eviction/re-read, which never touches swap, so the
  bench matrix and its sensor stay valid; (2) c5-class anon over-commits (the
  rs-seq snapshot pools, 1.7–1.8× ctx-size GB) now COMPLETE instead of hitting
  the OOM killer — the widened margin is exactly what makes big-budget c5 cells
  and agent sessions safe; (3) guard unchanged: majflt/tok before any perf claim
  (a config error that used to fail fast now degrades slowly).
- **Aider edit-turn reuse experiment (post-queue item b) — PROVEN:** Ling-mini
  served on :8017 (`arch bailingmoe2` per the load line — noted), aider 0.86.2
  pointed at the bridge (`OPENAI_API_KEY=dummy` — aider refuses to start without
  a key string even for a local bridge; the only setup gotcha). Real session in
  `~/aider-test`: turn A "add subtract()", turn B "add multiply_check()". Server
  TELEMETRY (the evidence): turn A call 1 (cold) **857 prompt / 0 reused / 51.5 s
  prefill**; turn A call 2 (aider's edit-confirm round) 231/**625**/11.9 s;
  **turn B: 576 / 504 / 20.7 s** — cross-edit-turn reuse 87.5%, divergence
  anchored at the re-sent changed-file snapshot, exactly the prefix-diff design's
  prediction. Both edits verified correct in calculator.py; 6.7–9.2 tok/s decode.
  (Note: BMOE_DEBUG_ECHO=/tmp/bmoe-reqs.jsonl capture produced no file — the
  echo gate at bmoe-serve.py:359 covers a different path; TELEMETRY rows are the
  evidence of record.)
- **Recipe (regenerable):** reset fixture → serve Ling-mini on :8017 → tmux
  aider interactive → two edit requests → read TELEMETRY in /tmp/bmoe-serve.log.
- **Environment restored:** daily driver LFM2.5 UP on :8017 with --auto-echo
  (health ok); aider + Ling-mini sessions killed; fixture left with both edits
  applied (uncommitted, reset documented).

**State:** arc `ab79bad` pushed; fork/main through `eb8ec6a`; daily driver UP;
next: opencode re-test on the live daily driver; #29085 + Nemotron draft still
with the user.

## Session 17 — 2026-09-20: feature A/B campaign COMPLETE (`e4345ad`)

- **Cyber-Tiel (bench binary, one load per cell):** base 1.884 tok/s / 21.9
  majflt/tok / stall 0.180. substitute-0.15 **2.773 (+47.2%)** — 18.1% slots
  reranked resident, stall 0.060, biggest single win on this host (lossy; gate on
  record is Qwen3.6's). drop-0.75 **2.493 (+32.3%)** — published recipe
  quantified. drop+prefill-drop 2.203 (**−11.6% vs drop**, decode majflt
  26.6→158.5 — prefill dropping churns a cold cache; keep OFF). mtp 1.982
  (**+5.2%**, 51.3% accept, 2.51 tok/verify — I/O-bound ceiling; speculation pays
  on compute-bound profiles, confirming the mtp.md thesis on the carrier).
- **LFM2.5 × ngram: +7.6%** (7.566→8.143, 48.5% accept) — **verify composes with
  the rs rollback planes**; the majflt rise is cheap page-cache re-read.
- **Qwen3.6 × io-two-wave: +25.2%** (1.763→2.208), majflt/tok 175.3→34.6 (5.1×)
  — the last untabled knob pays on the worst cell.
- **serve.md triage closed:** PR-scoped by design (born on the serve-bridge
  commits; ships with the arc's PR), same for docs/adr/ + MISTAKES.md.
- **Ops note:** bench cells are metrics-only (no generated text in logs) — the
  lossy rows cite the substitution doc's quality-gate protocol instead of char
  diffs. Raw evidence in /tmp/feat-ab/ (regenerable).

**State:** published chain `790d364` → `a81106b` → `e4345ad` → `ab16ff5` on fork/main; arc
record this commit; remaining backlog: `--release-mmap` load metric (lowest),
Cyber-Tiel substitution quality gate. #29085 + Nemotron draft still with user.

### 2026-09-20 — Session 18: scoreboard verdict columns + ADR-005 (keep-off policy)

The user asked the right challenge question about the scoreboard: why do slowdowns
appear in a "best result" table — and should refuted features be removed from
mainline? The answer shaped two artifacts.

**1. Verdict columns published (`3aecace` on fork/main, 1 file +22/−23).** Both
tables got a `Verdict` column with a legend line: `On` = measured default,
`Use-when` = pays only in its named regime, `Keep-off` = refuted or superseded.
Details: the streaming-stack row is **On for past-RAM models, off for fits-RAM**
(−6% to −31%); `--mtp`/`--ngram`/substitute/drop are Use-when with their regime
named; `--prefetch`, `--predict-prefetch`, `--drop-in-prefill`, `--dense-odirect`
are Keep-off (the last because it is superseded by `--dense-weights`, kept as a
deprecated alias). One self-caught slip during editing: I first annotated the
io-two-wave row as the help text being "stale" — wrong, the help says "pending the
**on-device** A/B" and our A/B was host; the help is accurate, the on-device A/B
is still open. Fixed before publish.

**2. ADR-005 (`docs/adr/005-keep-off-features-policy.md`, arc-scoped, indexed in
docs/README.md): keep-off features stay in the CLI default-off.** The decision is
the instruments argument: the refutations were measured *with* these flags
(bench cells + findings docs reference them by name), the knobs are how future
work re-tests (the predictor's accuracy is proven even though acting on it
refuted), refutations are regime-bound not eternal, and removal would orphan the
evidence and invite re-implementation. The verdict lives in documentation —
scoreboard, `--help` (kept honest: a knob whose help contradicts its measurement
gets fixed in the same PR), findings docs. Rule 3 is the load-bearing detail:
*accept-and-ignore is NOT acceptable* for any future cleanup — a silently ignored
flag lies to scripts. Overturning a Keep-off verdict requires a new matched pair
in a regime where the mechanism should win, with an explicit verdict flip, never
a silent rewrite. App exposure verified first: only default-off experimental
rungs — no shipping path turns a keep-off knob on.

**State:** published chain now `…ab16ff5` → `3aecace` on fork/main; arc record
this commit. Backlog unchanged: `--release-mmap` load metric, Cyber-Tiel
substitution quality gate. #29085 READY FOR REVIEW (user monitors CI).

### 2026-09-21 — Session 19 (cont.): Cyber-Tiel substitution gate COMPLETE — neutral; verdict published (`8ec6e72`)

- **All four cells finished** (two driver chains; the first died between HE
  cells, λ=0.15 relaunched alone — valid per protocol, each cell is its own
  engine load): tinyMMLU λ=0 **66.0%** / λ=0.15 **67.0%**; HumanEval λ=0
  **43/50** / λ=0.15 **43/50**. Per-problem A/B: 40 pass in both arms, 3 swap
  each way (L0-only: 32/38/41; L0.15-only: 6/20/39) — net zero, the
  definition of quality-neutral.
- **Speed in the gate's warm-session regime** (the regime the knob targets):
  λ=0 0.955 tok/s / 49.5% cache hit / 288 MiB read per generated token →
  λ=0.15 2.492 tok/s (+161%) / 85.7% / 55.6 MiB. The bench cell's +47.2%
  (cold sessions) and the gate's +161% (warm edit-turns) bracket the knob's
  real-world payoff.
- **Published `8ec6e72` → fork/main** via the patch flow (identifying-data
  scan clean): the scoreboard substitution row now carries "+161% warm" +
  "gated on two models, both neutral"; the Cyber-Tiel cell bullet's "ungated
  until the same gate runs on Cyber-Tiel" caveat replaced by the verdict;
  `cache-aware-substitution.md` quality-evidence paragraph: one architecture
  → two. Patch on record: `scripts/host-bench-qgate-cyber.patch`.
- **Incident + lesson:** the driver exited rc=1 on both chains (post-run
  cleanup); the first rc=1 was read as "run died" and drove a failure
  investigation; artifacts proved every cell complete. MISTAKES.md: artifacts,
  never exit codes, declare a bench run dead — and the driver log captured to
  a file is the durable error surface.
- **Evidence banked** at `bench-data/qgate-cyber-2026-09-21/` (all four cell
  files + stage records; scan clean per rule 7): commits `45d7000`, `083afba`,
  + the wrap-up.

**State:** fork/main through `8ec6e72`; arc push at wrap-up. Substitution is
now the best-evidenced lossy knob in the engine: two-model quality gate, both
neutral, +47% (cold) / +161% (warm) measured. Backlog: LFM2.5 driver restore,
opencode re-test, skill live check. #29085 with user.

### 2026-09-22 — Session 20: recipes.md completed to the eight-model matrix; tables collapsed

- User request: "update the recipe markdown with all of the results we have; make
  the tables collapsible, default collapsed." Clarified up front (3 questions):
  collapse the per-flag tables only inside recipes.md, also wrap host-benchmarks.md's
  per-model cell tables, publish directly per the established flow.
- **recipes.md rebuilt (`52b3e64` on fork/main):** six new sections — Ornith 1.5,
  Qwen3.6-35B, Qwen3-30B, Laguna-XS-2.1, Ling-mini-2.0, OLMoE-1B-7B — in the
  Cyber-Tiel/LFM2.5 shape (recommended recipe, per-flag provenance table,
  two-metric totals). TOC regrouped past-RAM vs fits-RAM, the cross-model pattern
  stated at the top (past-RAM: bmoe-main decode +42–70% streaming / +47%
  substitute; fits-RAM: stack off −6% to −31%, fork session layer warmup ~10–56×).
  Content rules held: every number copied from the published scoreboard rows
  (nothing re-measured); the two quality gates quoted with their protocol named
  (bench-cold vs device-protocol gate — Qwen3.6's substitution row cites the
  device gate, the only non-host numbers in the doc, marked as such); per-model
  "not measured here" rows point at the sibling model that carries the number.
- **Collapsible tables (GitHub-flavored `<details>`, default collapsed):** all 8
  per-flag tables in recipes.md; all 8 raw cell tables in host-benchmarks.md, with
  a one-paragraph intro note (open for commits/fault columns; bullets carry the
  interpretation; recipes.md link). Feature scoreboard tables stay expanded —
  they ARE the summary.
- **Publish flow:** the publisher's `git apply --check` cannot create a new file
  (second occurrence of the session-19 untracked-new-file gap — recipes.md was
  rewritten wholesale). Standard fix applied: rule-7 scan on the staged diff
  (clean), ancestry assert origin/main ⊂ fork/main (OK), commit inside the
  throwaway worktree, push HEAD:main. Patches archived on the arc
  (`scripts/host-bench-recipes-full.patch`, `scripts/host-bench-collapse.patch`),
  verified two ways: scratch-repo apply + byte-diff vs worktree before push, and
  landed-tree diff vs worktree after (`git show fork/main:<file>`). Worktree
  `pub-recipes` removed after the push.
- First wrapping attempt put every `<details>` block AFTER its table (0- vs
  1-indexed line numbers); caught by re-reading the rendered section before any
  publish, worktree file reset, wrapper rewritten to locate table ends properly.

**State:** fork/main through `52b3e64`; arc record this commit. Environment
unchanged from session 19 (daily driver still DOWN for the gate — restore is Next
action 1; gates not re-run this session: docs-only change, no engine or script
touched).

### 2026-09-21 — Session 19 (cont. 2): recipes doc — provenance corrected, LFM2.5 added

- User challenged the provenance framing: anything the fork inherited from
  Helldez's BigMoeOnEdge is not the fork's to claim. Git-verified against
  `origin/main`, commit authors, and upstream PRs: the streaming engine, its
  compute/io knobs, and the expert-ready seam commit are ALL bmoe-main's;
  upstream ngram-mod is ggerganov's (#19164 — the first draft's "ours merged
  upstream" was a misattribution); the fork owns the session-residency layer
  only (`--rs-seq`, bridge, warmup, `--auto-echo`, seam PR #29085).
  Published as `b4f96cf` with the two-metric totals: (1) bmoe vs llama
  +115% decode (all bmoe-main mechanisms); (2) fork vs bmoe-main =
  session/turn latency (2.2×/4× on Cyber-Tiel), decode 0% by design.
- LFM2.5 recipe published (`38746cf`): fits-RAM contrast — bmoe-main stack
  off (−10.8%), mainline ngram +7.6%, fork session layer primary (6.8× warmup,
  ~6× follow-ups, 2.3× divergence). Two recipes → one pattern: model size
  decides which layer pays.
- Publish-flow lesson reinforced: new files committed in the worktree before
  patch capture; landed tree asserted after every push
  (`git diff --stat <pre> fork/main` + `git show fork/main:<file>`).

**State:** fork/main through `38746cf`; arc push at wrap-up. Session ends
here; next session opens at Next actions 1 (LFM2.5 driver restore).

## 2026-09-22 — Session 21: collapse bug fixed, per-model serve commands — published `2bcdab6`

User follow-ups to session 20's publish, both landed on fork/main in one commit
(`2bcdab6`):

1. **host-benchmarks.md: the "entire lower part collapses" report.** Not a missing
   `</details>` and not an intentional page-wide wrapper — the session-20 intro
   sentence contained a **bare `<details>` token in prose** ("…its own <details>
   below…"). GitHub pairs a stray opener with the next closer, so scoreboard +
   summary + the Ornith section all rendered inside one accidental block (the first
   per-model `</details>` at line 187 closed it). Fix: code-quote the token. Verified
   after: 8 open / 8 close, no raw HTML tokens left in prose in either doc. Lesson
   generalizeable: **in these two docs, never write an HTML tag token unquoted in
   prose** — they are the only HTML on the page, so the parser takes them literally.
2. **recipes.md: served/multi-turn command per model.** The user asked for a second
   bash block per model and a shorter paragraph. Each "Add for served / multi-turn
   sessions" now leads with `python3 scripts/bmoe-serve.py -m <model> [--auto-echo]
   --engine-args "…"` and keeps only the why-notes as prose. `--auto-echo` included
   only where the per-flag verdict says On (Ornith, Cyber-Tiel, LFM2.5); omitted
   where it is refuted/no-op (Qwen3.6, Laguna, Qwen3-30B, Ling-mini, OLMoE). Engine
   args mirror the single-shot recipe; `--ctx-size 16384 --ubatch 512` are the
   protocol defaults (scoreboard footnote), `--rs-seq 64` only on the hybrid cells
   (the plain transformers have nothing for it to fix). Fits-RAM models carry just
   `--ubatch 512` in engine args; `--ngram` stays a prose note (workload add-on).
   Flags cross-checked against `scripts/bmoe-serve.py`'s argparse before publishing.

**Publish mechanics:** both targets were existing files, so the standard
`publish-host-bench.sh` patch path worked first try (the session-19/20 new-file
workaround was NOT needed). Patches archived on the arc:
`scripts/host-bench-details-fix.patch` + `scripts/recipes-serve-blocks.patch`;
landed tree diffed byte-identical against the worktree (`git show fork/main:<file>`)
after push.

**State:** fork/main through `2bcdab6`; arc push DONE this session (user asked:
"push the arc record when most convenient") — arc at wrap-up commit. Session ends
here; next session opens at Next actions 1 (LFM2.5 driver restore).
## 2026-09-30 — Session 22: upstream 0.25.0–0.28.0 merged; gates green on the new pin

**Why:** the branch sat 8/56 against upstream main (fork point 0.24.0; upstream had
shipped 0.25.0–0.28.0). A read-only `git merge-tree` trial before touching anything
predicted exactly 7 conflicted files / ~8 hunks and clean auto-merges for every
engine-critical overlap (`cli/main.cpp`, `config.h`, `session.h`, `runtime.cpp`,
`arch_registry.cpp`); the real merge matched the census one-for-one. The trial merge
also caught the only two semantic items before any file was edited: upstream had
folded our inline prompt-building block into `detail::build_turn_inputs()` **without**
the ADR-002 `preserve_thinking` kwarg, and (later, at build) upstream's new
`GenerateRequest::messages` field broke a positional aggregate init in `moe_gates.cpp`.

**Sequence:** uncommitted `dp.pos0` edit discarded first (verified it could not
compile against the OLD pin — `pos0` absent there — and upstream's line 1637 carries
the same rename for the new pin; committing dead code past a red flag is exactly what
the git rule forbids) → `git merge origin/main` → 7 files resolved
(session.cpp = upstream helper + kwarg re-added at the call site; CHANGELOG =
upstream's 4 release sections with our 0.24.1–0.24.3 spliced under, order restored;
README = both architecture rows; docs = unions; version fields = upstream, newer and
monotonic) → `git commit --no-edit` → `submodule update` to `dce969851` (+530
commits) → host rebuild (one gate fix, amended into the merge per "never commit past
a failed check") → `ctest` **16/16 passed**, byte-identity moe gates included
(qwen3moe, gemma4, nemotron_h_moe, split).

**Resolution notes worth keeping:** the `preserve_reasoning` field survived the merge
end-to-end (`session.h`, CLI flag, bridge JSON key, telemetry doc) — only the call
site needed the kwarg re-added, and upstream's helper (thinking_control.cpp) is
otherwise byte-for-byte our old logic, so the no-think-prefill path and the AUTO
reasoning-format invariant are upstream's now, unchanged. #29085 (`74e1ee6de`)
verified NOT an ancestor of the new pin — its playbook is unchanged. The old
"stash the pos0 port before pin builds" doctrine is RETIRED: the pin has upstream's
rename, the tree builds as committed, and resume gate 2's "expected dirty file"
clause is gone.

**Evidence:** merge commit `f3a9517` (parents: `f7f44e8` × `374f562`, amended);
ctest `16/16 passed`, 36.6 s; build log tail clean (`built: build/cli/bmoe-cli`).
The PROGRESS resume section now documents the clean-tree gate; the Android app
carries upstream's 43 / 0.28.0 (version skew note in Open questions 5).

**State:** `feat/session-residency` @ merge `f3a9517` + wrap-up on top.
Merge landed, gates green, PROGRESS committed, **pushed to
`fork/feat/session-residency`**. The first push was rejected — the merge
carries upstream's `.github/workflows/release-apk.yml` and the stored OAuth
token lacked the `workflow` scope; `gh auth refresh -h github.com -s workflow`
unblocked it. Lesson for future submodule bumps across upstreams that touch
workflow files: refresh the scope first.
Session ends here; next session opens at Next actions 1 (LFM2.5 driver restore),
with Next action 2 the new one-shot sanity run of the merged engine.

### 2026-09-30 addendum — MiMo-V2.6 vs Qwen3.5-9B, dense baseline on the host

Same arch (`qwen35`), same quant (Q4_K_M), 5.4 GB vs 5.3 GB; host i7-5500U 2C/4T,
16 GB, all figures from `build/cli/bmoe-cli` 0.27.1, measured this session.

**Speed** (house protocol from scripts/bench-report.sh: the fixed essay prompt,
`--chatml -n 256 -t 4 --ubatch 512`, mmap dense mode — bench-report.sh itself is
MoE-only, so the protocol was run manually, two runs each):

| model | decode | prefill (33/36 tok) | TTFT warm |
|---|---|---|---|
| MiMo-V2.6-Distill-Qwen-9B | 2.61 / 2.58 tok/s | 5.1 / 5.0 tok/s | ~7.4 s |
| Qwen3.5-9B | 2.55 / 2.58 tok/s | 5.6 / 5.4 tok/s | ~7.6 s |

Dead heat on decode — same arch and quant, the quantization dominates, the
weights differ. Qwen3.5 prefills slightly faster (same-token budget).

**Quality** (tinyMMLU-100, the arc's gate set; zero-shot, house prompt format
via `scripts/tinymmlu-bench.py build_prompts`; engine `--ppl-list` +
`--ppl-choices " A, B, C, D"`, one wide batch per question, `-c 2048 --ubatch
2048 -t 4`; scorer: argmax of choice log-probs vs key.json —
`/tmp/tinymmlu-score.py`, logs `/tmp/tinymmlu-{mimo,q35}.log`, questions
`/tmp/tinymmlu/` regenerable from `~/llm/data/tinyMMLU-test.parquet` under
`/tmp/evalvenv`):

| model | tinyMMLU |
|---|---|
| MiMo-V2.6-Distill-Qwen-9B | **68/100 = 68.0%** |
| Qwen3.5-9B | **74/100 = 74.0%** |

Reading: MiMo-V2.6 gives up 6 points of MMLU for nothing in exchange — decode
speed is identical and prefill is marginally slower. For the arc's purposes
(quality ceiling of the shared dense path) Qwen3.5-9B remains the reference
dense baseline; MiMo-V2.6 is a valid drop-in for throughput experiments but not
a quality upgrade. Caveats: zero-shot, no chat template, quantized — absolute
numbers sit below both models' published MMLU, per the tinyMMLU script's own
docstring; the comparison, not the absolute, is the measurement. The answers
were read from a full 100/100 scoring on both models.

### 2026-09-30 addendum — HumanEval pass@1, dense A/B (supersedes the "6 points for nothing" reading above)

The tinyMMLU leg measured general knowledge under teacher-forced, no-template, no-thinking
conditions; the user's pushback (MiMo is newer and coding-focused) prompted the axis that
can actually see code skill. Same 50-problem HumanEval prefix, greedy, raw completion
prompts, canonical tests executed per completion (scripts/humaneval-bench.py):

| model | HumanEval pass@1 (first 50) | mean tok/s |
|---|---|---|
| MiMo-V2.6-Distill-Qwen-9B | **41/50 = 82.0%** | 2.37 |
| Qwen3.5-9B | **44/50 = 88.0%** | 2.31 |

Corrected reading: at n=50 the 3-problem gap is inside binomial noise (~±10 pts) — on the
coding axis the two models are **statistically indistinguishable in the no-think completion
regime**, while Qwen3.5-9B's general-knowledge lead (74 vs 68, same regime) is the one clear
measured difference. Neither model's thinking channel was exercised (raw completion mode,
fair but not either model's best mode); a template-rendered, thinking-enabled HumanEval
variant is future work. Regenerate:
`python3 scripts/humaneval-bench.py --data ~/llm/data/HumanEval.jsonl.gz --cli build/cli/bmoe-cli --model <gguf> --out <dir> --lambda 0 --limit 50 --threads 4 --dense`
Raw cells: /tmp/he-mimo/cell_L0.jsonl, /tmp/he-qwen35/cell_L0.jsonl (temp — re-run to restore).

Tooling note: the harness gained `--dense` (the engine correctly refuses `--moe-stream` on
a recipe-less arch, so dense cells omit the streaming flags) and now closes the CLI's stdin
after `close` — with stdin left open the CLI reader thread blocks in getline and the
process never exits.
