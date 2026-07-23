# Temporal prefetch

The expert LRU cache is filled reactively: a layer's experts are read only once its router has
selected them. That leaves the read on the critical path — the first tokens of a generation miss
often and stall on flash (see [benchmarks.md](benchmarks.md)). Temporal prefetch reads ahead.

> **Measured, 2026-07-20 — the bet below does not pay on gpt-oss, and the premise is weaker than
> stated.** Routing's temporal locality is not "strong": over the committed route traces the
> previous token's top-k predicts the next token's **38.0 % on Qwen, 35.7 % on Gemma, 17.9 % on
> gpt-oss** — and a *static* list of the run's hottest experts does as well or better (37.6 /
> 39.6 / 26.7 %). The signal is popularity, not recency. On gpt-oss `--prefetch 1` was measured a
> **2× slowdown** (828-895 ms/token against a 414 ms baseline) with no hit-rate gain: at top-2 a
> one-layer read-ahead speculates ~908 MiB/token against 587 MiB/token of real demand, which a
> 1-in-6 hit rate cannot pay for. Prefetch stays off by default. It has not been re-measured on
> the top-6 models, where the predictor is twice as accurate and the wasted-read ratio smaller —
> that is the open question, not the mechanism.
> See [bench-data/2026-07-20-cache-replay/iobench-ceiling.md](bench-data/2026-07-20-cache-replay/iobench-ceiling.md)
> and [bench-data/2026-07-15-route-trace/findings.md](bench-data/2026-07-15-route-trace/findings.md).

> The predictor below can now be measured online, against a stronger one, on any model:
> [`--predict-log`](expert-prediction.md) scores the previous-token bet alongside running the next
> layer's router a layer early, and reports both per layer. The stronger predictor can also be
> acted on: [`--predict-prefetch`](expert-prediction.md) feeds it to this same speculative read
> path (mutually exclusive with `--prefetch`).

## The bet

MoE routing has strong temporal locality: the experts a token selects at layer *l* overlap
heavily with the experts the **previous** token selected at the same layer. So while a token
computes layer *l*, we speculatively read — on the otherwise-idle I/O lanes — the experts the
previous token routed at layers *l+1 … l+K* (`--prefetch K`). A correct guess turns the next
layer's read into a cache hit; a wrong guess only wastes a read. For the very first generated
token, the predictor is the last prompt token's routing, recorded during prefill.

`K` is a depth, not a certainty: recall falls as you look further ahead, so small `K` (1–2)
captures most of the benefit. Prefetch requires the LRU cache to be on — either a fixed
`--cache-mb N` or `--cache-mb auto`; the speculative slices land in the per-layer cache buffers.

## How it stays correct and out of the way

The speculative path never delays real work and never changes output — with the lossy knobs off.
(Under [`--drop-cold-experts`](expert-dropping.md) residency is an *input* to the routing policy,
so a correct guess un-drops an expert that would otherwise have been discarded. Prefetch depth
becomes output-affecting there; everything below still holds for the bytes themselves.)


- **Same bytes.** A speculative read is the *identical* read a real miss would issue — same file
  offset, same destination buffer (`lbuf_[p][il] + e*slice`). A prefetched expert is therefore
  bit-for-bit what a real read would produce; a later routing that hits it gets identical bytes.
  Integration cannot change output by construction. Gates **G5a/b/c** assert this.
- **One writer of cache state.** All LRU mutation stays on the eval-callback thread.
  `prefetch()` (eval thread) commits pages and enqueues per-projection reads; **workers only read
  bytes**; `quiesce_spec()` (eval thread, at the next real `load_layer`) integrates the entries
  whose every projection finished and releases the rest. No lock on the hot path.
- **Real work wins.** Workers drain the real batch first and only spend spare capacity on the
  speculative queue, yielding the instant a real batch appears. At each real load, in-flight
  speculation is cancelled (its reads disowned) before staging touches the cache, so speculation
  can never race the reads a token actually depends on, nor hold a page an eviction wants.

Because speculation only pays off when there is real flash latency to hide, it does nothing on a
fast host with the model in page cache (the reads are cancelled before they start) — which is
exactly correct. It is measured on-device.

## Telemetry

With `--prefetch K` the summary gains a `moe-prefetch:` line: speculative MiB read this
generation, and how many prefetched experts a later routing actually used (`useful / prefetched`).
A low useful rate means the look-ahead is too deep for this model, or the cache is too small to
hold the speculation alongside the working set.

`--prefetch-sync` (debug) completes speculative reads synchronously on the eval thread — it
defeats the latency hiding but makes integration deterministic, which the gates use to exercise
the integrate-then-hit path on a host where the timing race otherwise never fires.
