# Roadmap

Themes, not deadlines. Ordered roughly by expected impact.

The starting point has moved: with a well-sized expert cache and `--overlap`, decode on the
reference device is **compute-bound, not I/O-bound**. Flash I/O is ~79 % of decode only with the
cache off; at Qwen's cache 4000 it inverts, and an infinite cache would still cap at 1/compute
≈ 6.2 tok/s — this SoC's in-RAM decode speed ([benchmarks.md](benchmarks.md#reading-the-numbers)).
So the streaming path has already recovered most of what streaming can recover, and the themes
below are ordered by that fact: read bandwidth still matters for the models that do not fit a
useful cache, but throughput on the ones that do now depends on compute.

## Read bandwidth — measured, and mostly not the lever it looked like

This section used to open by asserting that effective O_DIRECT bandwidth sits well below the
drive's ceiling *because the routed slices are scattered*. That premise was measured on
2026-07-20 with `tools/bmoe-iobench` and **does not hold on the reference device**
([bench-data/2026-07-20-cache-replay/iobench-ceiling.md](bench-data/2026-07-20-cache-replay/iobench-ceiling.md)):

- Random O_DIRECT reads **saturate at 2 lanes** (~2460 MiB/s cold); 8, 16 and 32 lanes are
  flat-to-worse while latency grows linearly. `io_threads_max = 8` is already past the knee.
- Bandwidth is **flat above ~256 KiB** per read (2100-2550 MiB/s). Expert slices are MiB-class,
  so scatter costs little: only sub-256 KiB reads lose throughput.

What follows from that:

- **Read coalescing of adjacent routed slices at runtime — not worth building.** For it to pay,
  a layer's routed experts would have to land on consecutive ids. Measured over the committed
  route traces they do so **0.6 % of the time on gpt-oss and ~4 % on Qwen/Gemma**; an ideal
  coalescer removes 4 % of the read *count* and zero bytes.
- **Expert-contiguous gguf layout** (offline repack storing a layer's experts contiguously, and/or
  grouping projections per expert) is the item that survives — but its value is no longer
  "coalescing", since scatter is cheap here. Re-justify it before building: on storage where
  large sequential reads beat scattered ones it still pays, and it is the prerequisite that would
  make coalescing meaningful.
- The remaining gap between the engine's effective rate and the drive's is **duty cycle, not
  bandwidth**, and is not yet honestly sized: the ceiling itself falls by a third once the device
  is hot, so engine and microbench must be measured interleaved at matched entry state. Owed.

## Warm-up

Dense weights default to `--dense-weights anon` — read once through O_DIRECT into anonymous
memory, which is what actually removes the >RAM fault storm; the page-cache `warm` policy holds
only near RAM, since past it the kernel reclaims the warmed pages back out from under the run
([warmup-analysis.md](warmup-analysis.md)). The expert cache still fills from cold, so the first
tokens pay for it and no warm-up flag can change that. Worth exploring: preloading experts by
routing frequency rather than by arrival, and a cross-run persistent cache so a second session
starts warm.

Frequency **is** the signal to preload on, and that is now measured rather than assumed: over the
committed route traces, predicting a token's experts from the run's hottest list is right 26.7 %
of the time on gpt-oss against 17.9 % for the previous token's routing. But note what the same
session found about *spending* memory work to exploit it — see the expert-cache theme below.

## Expert cache policy — closed, negatively

Whether a smarter eviction policy could raise throughput is **answered, and the answer is no**
([bench-data/2026-07-20-cache-replay/](bench-data/2026-07-20-cache-replay/)).

Offline replay of the route traces through Bélády, LRU, LFU, random and a per-layer partition
(`scripts/route-replay.py`, validated against the recorded hit rates to the decimal) shows the
offline optimum is 11-23 points above LRU, but **no online policy recovers more than ~5**. The
best candidate, a per-layer budget partition with frequency eviction, was implemented
(`--cache-policy layer-lfu`), shipped off, and measured: it delivers the predicted hit-rate gain
(+2.0 points, −7 % flash reads) and is **~30 % slower**, because a hard per-layer cap removes the
cache's ability to self-balance and the resulting `MADV_DONTNEED` churn gets paid for by the
kernel reclaiming the dense weights (majflt/token 6 → 2370).

The transferable lesson: a hit-rate curve is not a throughput argument. Any future policy has to
be measured on device, however good its simulation.

What is still worth doing here is **not** a policy but a guard. Global LRU's recency order is
anti-correlated with the deterministic layer cycle, so below one token cycle it evicts precisely
what it is about to read and the hit rate goes to **exactly 0 %** — reproduced on device at a
budget the CLI accepts today. The worst-case cycle is computable at init from model shape alone,
so refusing or warning on a budget under it costs almost nothing.

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
