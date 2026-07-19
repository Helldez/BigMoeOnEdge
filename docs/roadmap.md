# Roadmap

Themes, not deadlines. Ordered roughly by expected impact.

The starting point has moved: with a well-sized expert cache and `--overlap`, decode on the
reference device is **compute-bound, not I/O-bound**. Flash I/O is ~79 % of decode only with the
cache off; at Qwen's cache 4000 it inverts, and an infinite cache would still cap at 1/compute
≈ 6.2 tok/s — this SoC's in-RAM decode speed ([benchmarks.md](benchmarks.md#reading-the-numbers)).
So the streaming path has already recovered most of what streaming can recover, and the themes
below are ordered by that fact: read bandwidth still matters for the models that do not fit a
useful cache, but throughput on the ones that do now depends on compute.

## Read bandwidth, where it still binds

Effective O_DIRECT bandwidth is well below the drive's sequential ceiling because routed expert
slices are scattered. This still dominates the models too large for a useful cache (gpt-oss-120b
at 5.2× RAM), and the cold-cache warm-up on every model.

- **Expert-contiguous gguf layout.** An offline repack storing each layer's experts contiguously
  (and/or grouping the projections per expert) so a routed set becomes one coalesced read instead
  of many scattered ones.
- **Read coalescing** of adjacent routed slices at runtime.

## Warm-up

Dense weights default to `--dense-weights anon` — read once through O_DIRECT into anonymous
memory, which is what actually removes the >RAM fault storm; the page-cache `warm` policy holds
only near RAM, since past it the kernel reclaims the warmed pages back out from under the run
([warmup-analysis.md](warmup-analysis.md)). The expert cache still fills from cold, so the first
tokens pay for it and no warm-up flag can change that. Worth exploring: preloading experts by
routing frequency rather than by arrival, and a cross-run persistent cache so a second session
starts warm.

## More architectures

`qwen3moe`, `qwen2moe`, `qwen35moe` (the hybrid attention/SSM family, e.g. Qwen3.6-35B-A3B),
`gemma4` (merged `ffn_gate_up_exps` plus shared experts) and OpenAI `gpt-oss` (MXFP4, purely
routed) are supported; other `build_moe_ffn` models are one recipe row each. The remaining
frontier is architectures whose routing node is not the shared `ffn_moe_topk` — custom gating,
which the capture/stream hook would need to learn. See [adding-a-model.md](adding-a-model.md).

## Expert quantization on the fly

Storing streamed experts at a lower precision than the resident parts to cut read volume, if it
can stay within an acceptable quality boundary. Most valuable exactly where read bandwidth still
binds, above.

## Bigger, smarter cache

The cache is capacity-bound, not policy-bound (reuse is broad, not skewed), so the simplest win is
more budget — which `--cache-mb auto` now takes automatically, capped by `--cache-ceil-mb`
([cache-sizing.md](cache-sizing.md)). Admission policies and a persistent cross-run cache
remain unexplored.

## Not on this list

Routing prediction and speculative expert gating were built and **removed**: the recall/latency
trade never paid on-device, and the predictor coupled the streamer to model internals, which cost
more in modularity than it returned in throughput. The archived measurements are in
[bench-data/2026-07-12-pr23/](bench-data/2026-07-12-pr23/).
