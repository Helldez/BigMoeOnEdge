# Where MoE model architecture is going, and what it means for streaming

This engine exists because a mixture-of-experts model reads only a few percent of its weights per
token, so the rest can live on flash. This page looks at how model architecture changed in 2025 and
2026, which of those changes are aimed at weights that do not fit in memory, and what they mean for
this engine: what it already exploits, what it should support next, and what it can ignore.

It is a survey, not a measurement. Figures from other work are quoted as their authors report them
and linked; most of the 2026 sources are preprints, and the page says so where it matters. Figures
from this project link to the page that measured them. Anything that is our projection rather than
a reported result is labelled as such. Last checked on 2026-11-07.

## In one paragraph

Open frontier models are now sparse MoEs with many small experts, attention that keeps a small or
fixed state, and, new in 2026, large **lookup tables addressed by token ids** (DeepSeek's Engram,
the n-gram table of Qwen3.8-Flash-Next). Together these split a model into three tiers with three
access patterns: a small hot part read every token, routed experts read selectively, and tables
read a few rows at a time at addresses known before the forward pass. Almost all of the growth goes
into the two tiers that can live on flash. A few models are now designed for flash outright:
SmallThinker routes before attention so expert reads can start early, and Apple's on-device model
picks its experts once per prompt. For this engine the two most valuable next steps follow from
that: serve token-addressed tables as a storage tier of their own, and make route-ahead exact on
models whose router runs early. The large speedups reported elsewhere mostly come from changing
the routing, which is lossy, or from hardware with idle bandwidth.

## How the models changed

### Experts got smaller and more numerous

The open models this engine streams moved from a few large experts to hundreds of small ones, with a
top-k of 6 to 10 and usually one always-on shared expert:

| model | experts per layer | routed per token | shared | expert FFN width |
|---|---:|---:|---:|---:|
| Qwen3.6-35B-A3B | 256 | 8 | 1 | — |
| DeepSeek-V4.1-Flash | 384 | 6 | 1 | — |
| Qwen3.8-Flash-Next (`qwen4exp`) | 512 | 10 | 1 (gated) | 640 |
| SmallThinker-21B-A3B | 64 | 6 | 0 | 768 |

Sources: [Qwen3.8-Flash-Next model card](https://huggingface.co/Qwen/Qwen3.8-Flash-Next),
[vLLM recipe for DeepSeek-V4.1-Flash](https://recipes.vllm.ai/deepseek-ai/DeepSeek-V4.1-Flash),
[SmallThinker, arXiv 2507.20984](https://arxiv.org/html/2507.20984v2).

Small experts are not a problem for a flash reader as long as one projection slice stays above the
size where read bandwidth flattens, which on the 12 GB test phone is about 256 KiB per read
([roadmap.md](roadmap.md)). A Flash-Next expert has 3 × 2560 × 640 weights, about 0.9 MB per
projection at Q4_K (our arithmetic), comfortably above it. What small experts do change is the
number of reads per token: Flash-Next needs 10 × 48 = 480 expert slices per token against 384 for
Qwen3-30B-A3B, about 25% more expert I/O at the same bits per weight (our arithmetic from the card).

### Attention stopped growing with the context

Hybrid models interleave linear-attention layers with a minority of full or sparse attention layers:
Gated DeltaNet at 3:1 in the Qwen line, Kimi Linear with up to 75% less KV cache
([arXiv 2510.26692](https://alphaxiv.org/abs/2510.26692)), and sparse attention that reads only
selected blocks, such as DeepSeek Sparse Attention ([LMSYS](https://lmsys.org/blog/2025-09-29-deepseek-V32))
and Qwen Sparse Attention in Flash-Next. For this engine all of these are dense-side code inside
llama.cpp, invisible to the streamer ([seam.md](seam.md)); they need no engine change. What they
change is the memory budget: a smaller, fixed state leaves more RAM for the expert cache. Note that
DeepSeek Sparse Attention reduces KV *reads*, not KV *storage*, since its indexer keeps its own key
cache.

### A third kind of weight: tables read by token id

The 2026 change that matters most for streaming is a class of parameters that are never multiplied,
only looked up, at rows chosen by the token ids:

- **Per-layer embeddings** in Gemma 3n and the Gemma 4 E2B/E4B models keep a large share of the
  parameters out of accelerator memory; Gemma 4 E2B has 2.3B effective parameters out of 5.1B with
  embeddings ([Gemma 4 model card](https://ai.google.dev/gemma/docs/core/model_card_4?hl=en),
  [Gemma 3n guide](https://developers.googleblog.com/en/introducing-gemma-3n-developer-guide/)).
- **Engram** (DeepSeek) turns n-gram embeddings into an O(1) lookup whose address depends only on
  the token ids, so it can be fetched before the layer that consumes it
  ([deepseek-ai/Engram](https://github.com/deepseek-ai/Engram)). DeepSeek-V4.1-Flash ships it:
  196.6B Engram parameters (about 183 GiB in FP8) beside a 552B backbone, at layers 1 and 14, served
  from pinned host memory with unchanged output in the vLLM recipe
  ([vLLM recipe](https://recipes.vllm.ai/deepseek-ai/DeepSeek-V4.1-Flash)). About a quarter of that
  model is a read-only table.
- **Qwen3.8-Flash-Next** carries the same idea as a 51.2B-parameter n-gram table at layer 2, placed
  there so that a host-side prefetch can overlap layer 1 ([technical report, arXiv 2608.30320](https://arxiv.org/html/2608.30320v1)).
  In GGUF it is `per_layer_token_embd`, 16 hash heads read at 16 rows per token; the Q4 shard is
  28.8 GB ([Unsloth](https://unsloth.ai/docs/models/qwen3.8-flash-next)).
- **Research variants** push further: Mixture of Lookup Experts reparameterises experts into tables
  after training (tested up to 1B, [arXiv 2503.15798](https://arxiv.org/html/2503.15798v2)); memory
  layers reach 128B memory parameters ([arXiv 2412.09764](https://arxiv.org/abs/2412.09764)); and
  UltraMemV2 matches an 8-expert MoE ([arXiv 2508.18756](https://arxiv.org/html/2508.18756v1)).
  Memory-layer keys depend on the hidden state, so unlike Engram they cannot be fetched ahead.

For a flash reader these tables are almost free in bandwidth and expensive in latency. A Flash-Next
row is about 90 bytes at IQ4_NL, about 1.4 KB per token, but that is 16 random page reads per token
(our arithmetic); an Engram row in FP8 is 256 bytes against a 4 KiB minimum read. What matters is
issuing all of a token's row reads together and as early as possible.

### Models designed for flash

Two models are designed, at the architecture level, for weights that live on flash:

- **SmallThinker** (SJTU IPADS and Zenergize, July 2025, preprint) moves the **router before the
  attention block**, so a layer's experts are known before its attention runs and their reads can
  overlap it ([arXiv 2507.20984](https://arxiv.org/abs/2507.20984v2)). It ships open weights
  (Apache-2.0) in 4B-A0.6B and 21B-A3B sizes, and llama.cpp runs it as the `smallthinker`
  architecture. Its quality is close to Qwen3-30B-A3B (MMLU 84.4 vs 85.1). Its headline speedup
  (15.50 against 0.18 tok/s under an 8 GiB limit on a phone) is measured against a model thrashing
  through mmap; this project measured the same pathology at 0.1 tok/s mapped against 5.0 streamed
  on the 12 GB test phone ([CHANGELOG.md](../CHANGELOG.md)), so against a streaming engine the gap
  is far smaller.
- **Apple's on-device model** (WWDC, June 2026) is a 20B sparse model held in flash whose experts
  are chosen **once per prompt** by a small predictor, because NAND-to-DRAM bandwidth is too low to
  swap weights token by token; shared experts stay in DRAM, and 1B to 4B parameters are active
  ([VentureBeat](https://venturebeat.com/technology/on-device-ai-agents-hit-a-hard-memory-limit-apples-new-architecture-routes-around-it),
  secondary source). It is closed, so it matters here as evidence of direction, not as a model to
  support.

## Routing: what the literature found, against what this engine measured

| question | reported elsewhere | measured here |
|---|---|---|
| How far is LRU from the optimum? | 65.9% vs 79.1% (Belady) on Qwen3-30B ([arXiv 2608.18261](https://arxiv.org/abs/2608.18261)); 38.6-47.6% vs 56.9-68.2% on an iPhone ([arXiv 2609.29032](https://arxiv.org/html/2609.29032v1)) | Belady 11-23 points above LRU ([cache replay](bench-data/2026-07-20-cache-replay/findings.md)) |
| Does LRU collapse below a token's working set? | Zero hits below 313 slots on Qwen3.6-35B on an iPhone ([arXiv 2609.29032](https://arxiv.org/html/2609.29032v1)) | Exactly 0.0% below one token cycle; the planner sizes around it ([cache-sizing.md](cache-sizing.md)) |
| How well can routing be predicted a layer early? | 76.9% (previous-layer gate, [arXiv 2502.12224](https://arxiv.org/html/2502.12224v1)) to 93-97.6% (same-layer pre-attention predictor, [arXiv 2511.10676](https://arxiv.org/html/2511.10676v1)) | 88.6% on Qwen3-30B and 80.7% on Qwen3.6-35B with the stale gate ([expert-prediction.md](expert-prediction.md)) |
| Does speculative prefetch pay? | Up to 2.34x with a quantised draft ([arXiv 2511.14102](https://arxiv.org/html/2511.14102v1)) | −21% on the phone (3.14 vs 3.96 tok/s); committing to the prediction instead, route-ahead, is +21% ([expert-prediction.md](expert-prediction.md), [route-ahead.md](route-ahead.md)) |
| Does a contiguous per-expert layout pay? | 2.6x and 4.1x on a GPU with PCIe 5 NVMe ([arXiv 2609.18110](https://arxiv.org/html/2609.18110v1)) | A contiguous sidecar loses 20-23% on the phone with overlap and cache on ([roadmap.md](roadmap.md)) |
| Does MTP speculation pay? | 4.83x over on-demand offload on a phone NPU ([arXiv 2609.14643](https://arxiv.org/abs/2609.14643)); gains shrink at batch 1-2 ([arXiv 2608.15018](https://arxiv.org/pdf/2608.15018)) | +15.1% on a desktop host; a loss on the phone, where the verify batch reads 35-54% more flash ([mtp.md](mtp.md)) |

Where the question is about the *model* (the LRU gap, the cliff below a token cycle, prediction
accuracy) the literature and this engine agree. Where it is about a *system technique*, three
results disagree. In each case the paper's setting has bandwidth to spare (a GPU over PCIe 5) or a
baseline without overlap, while the phone's flash is already saturated at two read lanes
([roadmap.md](roadmap.md)). Edge0 supports the same reading: replacing the router with a predictor
and committing to it reaches 20.4 tok/s on a 35B MoE on a 24 GB desktop, at a cost of 3.9 points
across five benchmarks and 6.1 on AIME (preprint, figures from secondary summaries,
[alphaXiv](https://www.alphaxiv.org/abs/2609.18063)). Committing beats speculating; committing also
changes the output.

Three more results shape what is worth building:

- **Training a router for locality costs quality that does not shrink with scale.** In a
  pre-registered study, locality and cache losses cut misses by up to 60%, but no configuration
  stayed within 1% perplexity, and the cost did not fall from 137M to 340M parameters
  ([arXiv 2608.18261](https://arxiv.org/abs/2608.18261)).
- **Cache-aware rerouting is the lossy lever with the best measured trade.** It comes from
  Qualcomm's work ([arXiv 2412.00099](https://arxiv.org/html/2412.00099v2)) and ships here as
  `--expert-substitute`: at `L = 0.15` Qwen3.6-35B goes from 2.37 to 3.84 tok/s with perplexity up
  1-4% ([cache-aware-substitution.md](cache-aware-substitution.md)).
- **Learned cache retention helps a little.** A learned eviction policy gains 7% decode over LRU on
  Qwen3-30B-A3B ([FlashMoE, arXiv 2601.17063](https://arxiv.org/html/2601.17063v1)); an auxiliary
  "temporal router" that steers retention without changing selection raises simulated hit rates by
  10 to 35 points over LRFU, with no measured tokens per second ([arXiv 2609.04895](https://arxiv.org/html/2609.04895v1)).
  The same paper finds LRFU the strongest classical policy on Qwen3.

Variable compute per token is arriving too. Zero-computation experts (MoE++, ICLR 2025,
[arXiv 2410.07348](https://arxiv.org/html/2410.07348v1)) ship in LongCat-Flash, which activates
18.6B to 31.3B of its 560B parameters per token ([arXiv 2509.01322](https://arxiv.org/html/2509.01322v1)).
A slot routed to an empty expert is a read that never happens, so average bytes per token fall;
peak bytes do not, and the token cycle the cache is sized against becomes a range.

## Storage hardware

UFS 5.0 roughly doubles phone flash to about 10.8 GB/s, with devices expected in 2027
([TrendForce](https://www.trendforce.com/news/2026/06/24/news-samsung-unveils-ufs-5-0-storage-claims-industrys-fastest-with-10-8gbs-speeds/),
[heise](https://heise.de/-10744571)). LPDDR6 reaches 38.4 GB/s per chip, in mass production from
the second half of 2026 ([TweakTown](https://tweaktown.com/news/112910/sk-hynix-will-start-lpddr6-mass-production-in-2026-for-xiaomi-smartphones/index.html)).
High-bandwidth flash (SanDisk and SK hynix) promises 0.4-3 TB/s per stack from 2027 for data-centre
accelerators ([TrendForce](https://www.trendforce.com/news/2026/08/04/sk-hynix-sandisk-debut-hbf-standard-to-challenge-ai-memory-bottlenecks-with-google-tenstorrent-support/)).

On the phone, faster flash moves the cold regime and models with large experts. The warm regime is
already compute-bound: an infinite cache would only reach about 6.2 tok/s on Qwen on the test
phone ([benchmarks.md](benchmarks.md)), and Qwen3.8-Flash-Next spends most of each token computing
its 4.3 GB dense side ([CHANGELOG.md](../CHANGELOG.md)). Memory bandwidth and kernels will matter as
much as UFS 5.0. The planner measures bandwidth instead of assuming it
([hardware-planning.md](hardware-planning.md)), so faster storage needs no engine change.

## Qwen 4 and Qwen3.8-Flash-Next

As of 2026-11-07 Qwen 4 is not released. Alibaba presented it as in training at Apsara on
2026-09-22, with no date, sizes or licence ([NYU Shanghai RITS](https://rits.shanghai.nyu.edu/ai/alibaba-at-apsara-2026-qwen-4-in-training-10t-models-next/)).
Its public architecture preview is **Qwen3.8-Flash-Next**, open weights since 2026-08-26 under
`qwen-community-1.0` ([model card](https://huggingface.co/Qwen/Qwen3.8-Flash-Next)):

- 125B total and 6B active parameters, plus the 51.2B n-gram table and a 4B MTP module;
- 48 layers, 512 routed experts at top-10 plus one gated shared expert;
- Gated DeltaNet and Qwen Sparse Attention at 3:1, with a four-branch gated residual;
- 262K native context.

llama.cpp runs it as `qwen4exp` since [PR #27742](https://github.com/ggml-org/llama.cpp/pull/27742);
its MTP head is not in mainline. This engine streams it unchanged. It is the test phone's
compute-bound case, and its n-gram table stays memory-mapped with `MADV_RANDOM`: `--row-stream`
correctly refuses it, because the table's indices are hashes computed inside the graph rather than
indices known before the node runs ([row-gathered-tables.md](row-gathered-tables.md)). If Qwen 4
keeps the table and the fine-grained experts, as the preview suggests, token-addressed tables become
a class this engine has to serve well rather than a one-off.

## Where this is heading

This section is our projection from the evidence above, not a reported result.

**The three-tier split will deepen.** A model will increasingly be:

| tier | holds | size trend | access |
|---|---|---|---|
| hot (RAM or VRAM) | attention, shared experts, recurrent state | small, growing slowly | every token |
| warm (cache over flash) | routed experts | tens to hundreds of GB | per token, partly predictable |
| cold (flash) | token-addressed tables | tens to hundreds of GB | a few rows, addresses known early |

The economics push the same way: DRAM prices rose sharply through 2026, flash costs an order of
magnitude less per gigabyte, and the parameters that are cheapest to add are the ones that
are only looked up. At data-centre scale the same pressure produced high-bandwidth flash; models of
5-10T parameters, which Alibaba has projected for its next generations, will not fit in fast memory
anywhere. Weights larger than fast memory stop being an edge niche.

What we expect in the next two to three years:

1. **Routing computed early becomes common**, because every offloading runtime needs it. Pre-gated
   routers, as in SmallThinker, make an early read exact instead of a bet.
2. **More capacity moves into lookup tables**, prefetchable as soon as a token is sampled. Engram's
   quarter of DeepSeek-V4.1-Flash is a starting point, not a ceiling.
3. **Per-prompt or per-block expert selection on devices**, the route Apple took: it turns random
   per-token reads into one load per request, at a quality cost that suits narrow tasks.
4. **Attention mostly linear or sparse**, freeing RAM for the expert cache.
5. **Storage-aware formats shipped with the model**: lossless compression of cold experts (rANS cut
   SSD traffic by 33.2% with byte-identical output in [arXiv 2609.18110](https://arxiv.org/html/2609.18110v1))
   and mixed precision between hot and cold experts.

What we do not expect to win: routers trained for cache locality (the quality cost does not shrink
with scale), batching as a way to amortise flash reads at n=1 on a device (the union of distinct
experts saturates; DeepSeek-R1 touches 163 of 256 experts at batch 32, [vLLM RFC #38256](https://github.com/vllm-project/vllm/issues/38256)),
and compute inside the flash device, which remains research.

## What it means for this engine

What it already exploits: top-k sparsity at n=1, reuse across tokens through a large cache sized to
a token cycle, the similarity of adjacent layers' residuals (the stale gate behind `--prefetch` and
`--route-ahead`), the router's tolerance to perturbation (`--drop-cold-experts`,
`--expert-substitute`), trained MTP heads (`--mtp`), sparse access to embedding tables
(`--row-stream`), and a device cache tier on discrete GPUs. New attention variants need nothing from
it.

What to do next, in order of expected value for cost, within the project's rules (no llama.cpp patch,
nothing named by model, every lossless path behind a byte-identity gate):

| # | work | output | expected gain |
|---|---|---|---|
| 1 | **Token-addressed tables as their own tier.** Capture the table's row indices from the graph through the eval callback, as the streamer already does with `ffn_moe_topk`, and fill those rows in one batched, page-aligned read before the gather runs. First, measure what the mapped `qwen4exp` table costs in page faults on the phone today. | identical | Removes 28.8 GB (`qwen4exp`) or about 183 GiB (DeepSeek-V4.1-Flash) from memory at under 1 MiB of traffic per token; speed effect unknown until the faults are measured |
| 2 | **Exact route-ahead on pre-gated models.** Where the router runs before attention, take the selection from the router's own node instead of predicting it. | identical | A free read window of one attention block; the lossy route-ahead it replaces is worth +21% on the phone ([route-ahead.md](route-ahead.md)) |
| 3 | **Measure how well expert slices compress** (rANS over Q4_K, Q4_0, Q2_K, MXFP4) offline, before building anything. Decompression would land in the native layout, so repack stays off. | identical | Up to the 33.2% reported for GPU offload; on the phone it would help mainly cold starts and large experts |
| 4 | **Close the LRFU branch** with a phone run. | identical | Small: published learned and classical policies recover a fraction of the Belady gap, about 7% decode at best on Qwen3 |
| 5 | **MTP draft length as a measured knob**, chosen by the planner's first-turn trial as threads and overlap are. | exact, not bit-identical | Keeps the desktop gain and removes the phone loss ([mtp.md](mtp.md)) |
| 6 | **Evaluate `--route-ahead` on real benchmarks** (the same protocol as `--drop-cold-experts`) | — | No speed: it makes sure a cost of Edge0's size is not shipped unseen |

Watch, do not build yet: zero-computation experts (check that the expert-layout rule, third
dimension equal to the declared `expert_count`, holds when some experts have no weights; that is a
case for a gate, [which-models-stream.md](which-models-stream.md)), and offline expert pruning such
as REAP, which streams without change and raises the cache's coverage without lowering bytes per
token ([arXiv 2510.13999](https://arxiv.org/html/2510.13999v2)).

Not worth pursuing here: memory layers and UltraMem (no production checkpoints, keys depend on the
hidden state), routers trained for locality, closed per-prompt designs, high-bandwidth flash and
near-storage compute, and KV offload to flash on a phone whose flash already serves the experts.

## Related pages

- [moe-streaming.md](moe-streaming.md): why streaming experts makes a larger-than-RAM model run at all.
- [which-models-stream.md](which-models-stream.md): the rule that decides which tensors stream.
- [row-gathered-tables.md](row-gathered-tables.md): the tables served row by row today, and why the n-gram table is not one of them.
- [expert-prediction.md](expert-prediction.md), [route-ahead.md](route-ahead.md), [prefetch.md](prefetch.md): prediction, and what committing to it costs.
- [comparison.md](comparison.md): other engines that run MoE models larger than memory.
