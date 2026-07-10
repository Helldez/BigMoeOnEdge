# Roadmap

Themes, not deadlines. Ordered roughly by expected impact on the flash-I/O-bound decode.

## Read bandwidth

Decode is ~79% flash I/O, and effective O_DIRECT bandwidth is well below the drive's
sequential ceiling because routed expert slices are scattered. The largest untapped lever:

- **Expert-contiguous gguf layout.** An offline repack that stores each layer's experts
  contiguously (and/or groups the three projections per expert) so a routed set becomes
  one coalesced read instead of many scattered ones.
- **Read coalescing** of adjacent routed slices at runtime.

## Overlap I/O with compute

Today the read phase and the expert matmul are serial. Prefetching the next layer's likely
experts (or overlapping the read of layer N+1 with the compute of layer N) could hide a
large share of the I/O. Requires a routing predictor or speculative prefetch.

## More architectures

Beyond `qwen3moe`: other `build_moe_ffn` models (one recipe row each), then the merged
`ffn_gate_up_exps` layout and shared-expert models. See
[adding-a-model.md](adding-a-model.md).

## Expert quantization on the fly

Storing streamed experts at a lower precision than the resident parts to cut read volume,
if it can stay within an acceptable quality/lossless boundary.

## Bigger, smarter cache

The cache is capacity-bound, not policy-bound (reuse is broad, not skewed), so the simplest
win is more budget. A cross-run persistent cache and admission policies are worth exploring.
