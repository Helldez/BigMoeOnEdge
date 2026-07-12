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
it safe to run an approximate router ahead of time. Gates **G6a/b/c** assert byte-identity.

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

## Composition and telemetry

`--spec-gate` predicts one layer ahead with high recall; it composes with `--prefetch K`, which can
cover deeper layers from the previous token's routing. Isolating the router-input node adds one
sync barrier per layer, so the feature is opt-in and measured on-device.

The summary reports `moe-spec-gate: <pct>% router prediction recall` — how often a predicted expert
was actually routed — alongside the shared `moe-prefetch:` line (speculative bytes and useful-hit
rate). Recall is the quality knob: high recall with low useful% means the cache is too small to
hold the speculation; low recall means the look-ahead approximation is too coarse for the model.
