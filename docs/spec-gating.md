# Speculative gating

[Temporal prefetch](prefetch.md) predicts the next layers' experts from what the *previous token*
routed there. Speculative gating (`--spec-gate`) predicts more sharply: it runs the **next layer's
router itself** on the **current** layer's hidden state.

## The idea

Across a transformer's layers the residual stream changes slowly, so the hidden state entering
layer *l* is a good approximation of the one entering layer *l+1*. The router is tiny and
mmap-resident (`blk.<il>.ffn_gate_inp.weight` — it is never streamed). So while layer *l* computes,
we take its hidden state, run the **next** MoE layer's gate on it, take the top-k, and prefetch
those experts. When layer *l+1* actually routes, they are already resident.

Only the predicted **ids** matter — they feed the same speculative read queue as temporal
prefetch, which is byte-safe by construction (a prefetched expert is the identical read a real miss
would issue). A wrong prediction wastes a read; it can never change the output. That is what makes
it safe to run an approximate router ahead of time. Gates **G6a/b/c** assert byte-identity, and
**G6d** asserts it still holds when the recall self-governor (below) turns the feature off mid-run.

## Recipe-driven, per architecture

Each architecture describes its router in one recipe row (`arch_registry.cpp`) — no constants in
the engine:

- **qwen3moe / qwen2moe** route on the post-norm hidden state (graph node `ffn_norm-<il>`), so the
  observed node is the router input directly (`RouterPre::kNone`). The prediction applies the next
  layer's gate to the current layer's post-norm hidden — an approximation (the norm weights differ
  per layer), which trades some recall for zero extra transform work.
- **gemma4** routes on the raw `attn_out-<il>` with an explicit
  `rms_norm · 1/√n_embd · per-channel-scale` (`ffn_gate_inp.scale`) before the gate
  (`RouterPre::kRmsScaled`), and interleaves dense layers — handled by always targeting the *next
  bound MoE layer*.

`n_expert_used` (the top-k) and the rms epsilon come from the model's own gguf metadata
(`<arch>.expert_used_count`, `<arch>.attention.layer_norm_rms_epsilon`). F32 and F16 gate weights
are supported; a quantized router disables the feature for the run (logged once) rather than
guessing on dequantised noise — prediction quality would suffer and it is never worth corrupting
the read budget with it.

## Paying for prediction without stalling decode

Prediction is not free — a top-k over every expert per MoE layer, plus one sync barrier per layer to
read the router-input node. Three things keep that cost off the decode critical path:

- **Off the eval thread.** The eval callback does only the cheap part inside the barrier: it copies
  the router-input hidden state (a few KiB) and hands it to a dedicated **prediction worker**. That
  worker runs the pre-transform, the matvec and the top-k; the eval thread picks up the finished
  ids on a later callback and enqueues their prefetch. Everything the worker reads (gate weights,
  recipe, layer map) is immutable once capture ends, so no lock guards it. Requests are latest-wins:
  a prediction that lands after its layer already ran is simply a wasted read, never a stall.
- **Vectorised matvec.** The per-expert dot products use NEON kernels on arm64 (FMA accumulators,
  baseline `vcvt` for f16 gate weights) with a scalar fallback on other hosts (`spec_dot.{h,cpp}`).
- **Cold-end cache inserts.** A predicted expert enters the LRU at the *cold* end, so a mispredict is
  the first thing evicted and can never displace a demanded expert; a real hit promotes it to the
  front. This stops speculation from amplifying demand reads.

## Self-governing on recall

Speculation only pays off when the router is actually predictable one layer ahead. The engine
measures this continuously as **recall** — the fraction of predicted experts a later routing really
selected — and governs itself: after a warm-up of `--spec-gate` predictions (default 512, past the
prompt-transition noise), if recall is below a floor (`--spec-recall-min`, default 75%) it latches
the feature off for the rest of the run, which also removes the per-layer barrier. Set
`--spec-recall-min 0` to never auto-disable. This is why `--spec-gate` is safe to leave on across
models: an architecture whose routing does not correlate across layers (low recall) turns it off by
itself instead of paying for prediction that does not land.

## Composition and telemetry

`--spec-gate` predicts one layer ahead; it composes with `--prefetch K`, which can cover deeper
layers from the previous token's routing.

The summary reports `moe-spec-gate: <pct>% router prediction recall` — with a
`(auto-disabled below <P>%)` suffix when the self-governor fired — alongside the shared
`moe-prefetch:` line (speculative bytes and useful-hit rate). Recall is the quality knob: high
recall with low useful% means the cache is too small to hold the speculation; low recall means the
look-ahead approximation is too coarse for the model, and the governor will disable it. Because the
prediction now runs on the worker thread, its CPU time no longer inflates the decode `compute` term
in the `moe-stream:` split — see [telemetry.md](telemetry.md).
