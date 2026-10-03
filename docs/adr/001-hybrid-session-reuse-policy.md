# ADR-001: Hybrid session reuse policy

**Status:** Accepted (2026-09-17, arc `feat/session-residency`, commits `e930b4d`..`d83d153`)

## Context

Hybrid/recurrent models (LFM2 family, Qwen3.5 family, bailingmoe2) cannot rewind their
recurrent state: `llama_memory_seq_rm` on the recurrent side returns failure for any
mid-sequence cut (err 2), so the generic KV diff path is unsafe. The engine therefore
full-cleared the session on every turn of a hybrid — correct, but each turn re-prefilled
the whole rendered history.

Three mechanisms were evaluated to recover reuse:

1. **Append-only reuse** (`e930b4d`): if the next turn's rendered prompt strictly extends
   the resident token mirror, skip the clear and prefill only the delta. No rewind ever
   happens, so recurrent state stays valid. Safe by construction.
2. **Snapshot rollback** (`63768e7`): upstream's per-token recurrent-state snapshots
   (`llama_context_params.n_rs_seq`, `[EXPERIMENTAL]`) make `seq_rm` restorable within
   budget; the generic diff path already falls back to full-clear when `seq_rm` returns
   false. Measured on Qwen3.5-9B: reuse engages (`n_reused` 21/33/58), but restore is
   **not bit-exact** — under greedy decoding the identical prompt returned `40` fresh and
   `420` restored. Contamination, not noise. *(Superseded 2026-09-18, ADR-004 Addendum 2:
   that harness cleared its own pending rollback before the comparison — the `40` vs `420`
   contamination was an artifact of it, not of upstream restore. The off-by-default decision
   in §2 below stands, re-derived on corrected evidence: snapshot planes d ≥ 1 go stale
   across single-token decode steps, which is the shape every real turn has.)*
3. **Reasoning echo** (`d83d153`, ADR-002): keep the resident prefix growing so the
   append path fires without any rewind.

Two further blockers were discovered and documented in `docs/serve.md`:

- Templates diverge structurally: Qwen3.5 bakes an empty `<think></think>` span into the
  generation prompt that its re-rendered history omits, so renders never nest even with
  thinking disabled. LFM2.5 puts reasoning tokens in the cache that clients never echo back.
- LFM2/LFM2MOE crash at graph reserve with snapshots enabled (`needed 836640, available
  836272` — a fixed ~368-byte node-pool overflow, invariant to ctx/ubatch/budget).
  Upstream's reserve estimate under-counts the snapshot ops; tracked as the third gate.

## Decision

1. Hybrid sessions run the append-only path when (and only when) the next render strictly
   extends the token mirror; every failure/cancel/overflow path poisons the mirror so the
   next turn falls back to a full clear. Never rewind recurrent state without snapshots.
2. Snapshot rollback ships **off by default** (`--rs-seq 0`). It stays available for
   experimentation and flips on the day upstream restore is bit-exact. Never enable it by
   default: silent wrong answers are worse than slow turns.
3. The hybrid clear-block remains the fallback for every path the append check does not
   cover. Correctness outranks prefill cost.

## Consequences

- Easier: hybrids are safe on every turn regardless of client behavior; pure-transformer
  behavior is bit-identical (the new code only gates the hybrid branch).
- Harder: default hybrid turns still pay full prefill — the speedup needs ADR-002's echo
  cooperation or an upstream fix.
- Accepted as-is: `--rs-seq` is a loaded footgun by design; its docs say so. The three
  upstream gates (llama.cpp #25913 open, snapshot-plane staleness after single-token steps
  — re-derived from the falsified "non-bit-exact restore", see ADR-004 Addendum 2 — and
  the LFM2 graph-reserve crash) are tracked in `docs/serve.md` and must be re-verified
  after any submodule bump.
