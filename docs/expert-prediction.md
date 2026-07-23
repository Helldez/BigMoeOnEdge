# Expert prediction: measuring it before building it

`--predict-log` answers one question and refuses to answer any other: **how much of a layer's
routing could be known before that layer runs?** It predicts, scores itself against what the router
actually chose, and prints the result. Nothing it computes reaches the loading path — a probed run
reads exactly the bytes an unprobed one does, only slower.

It exists because [temporal prefetch](prefetch.md) was built on a predictor nobody had priced. That
predictor — "the previous token routed these, the next one probably will too" — turned out to be
right about 38% of the time on Qwen and 18% on gpt-oss, which is not enough to pay for the reads it
speculates. The lesson was not that prefetch is impossible; it was that a predictor should be
measured before it is wired into anything.

## The three predictors

All three are scored on the same routings, on the same run, so they are directly comparable.

**stale-gate** is the one under test. Each layer's router is a matrix multiplied by that layer's
gate input; the input comes from the residual stream, which every layer only nudges. So while layer
*l* is computing, we take the input *it* is about to consume and multiply it by **layer *l+1*'s**
router matrix. The ranking that comes out is a guess at what layer *l+1* will select, available a
full layer early. It needs no training and no change to the model — the matrix is already resident
(it is a dense weight, not a streamed expert) and the multiply is one GEMV.

This is the mechanism [FATE](https://arxiv.org/html/2502.12224v1) uses, where it scored 78.8% on
DeepSeek-V2-Lite.

**prev-token** is the incumbent: what this layer routed for the *previous* token — exactly the bet
`--prefetch` places. Reported here so the new predictor is judged against the one already shipped,
not against zero.

**ctrl** is not a predictor. It is the stale-gate with the staleness removed: the same row read,
the same GEMV, the same ranking, but using layer *l*'s **own** matrix on layer *l*'s own input. It
therefore has to reproduce the selection llama.cpp is about to compute from those very tensors, and
**it should read 100%**. Anything less is this probe being wrong, not routing being hard:

- a GEMV that disagrees with ggml's (a transposed matrix, a mis-strided row, the wrong token of the
  batch) collapses it toward chance;
- an architecture that does not select by raw-logit ranking — one that adds a per-expert selection
  bias, or masks whole expert groups before the argsort — puts it somewhere below 100%, and by
  roughly that much the stale-gate number understates the method.

Read the other two against `ctrl`, never against 100%. The CLI says so out loud when it drops.

## Reading the report

```
moe-predict: stale-gate <pct>% of routed slots (<pct>% whole routings) | prev-token <pct>% (<pct>%) | fresh-gate control <pct>% (<pct>%)
moe-predict: scored — stale-gate <rows> routings/<slots> slots, prev-token <rows>/<slots>, control <rows>/<slots>; <n> routings the stale-gate could not rank
  layer   stale    prev   ctrl   routings
```

The headline figure is **slot overlap**: of the *k* experts a routing selected, how many the
predictor had named. That is the number a prefetch cares about — each hit is one read turned into a
hit. The parenthesised figure is **whole routings** predicted exactly, i.e. the fraction of layers
that would have needed no on-demand read at all. They are far apart, and papers differ on which one
they quote, so both are printed. FATE's 78.8% is the first kind; the
[ETH pre-attention work](https://arxiv.org/abs/2511.10676)'s 93–97% is the second.

**The per-layer table is the substantive half.** An aggregate flatters a prefetch, because what a
prefetch costs is set by the layers it gets *wrong*, and those are not evenly spread: a MoE model's
first layers route far less predictably than its last, their routing scores sitting close enough
together that a slightly stale input reorders them. Both published results above report the same
shape.

A predictor with no routings at a layer prints `-`, never `0.0`.

## What it structurally cannot do

- **Layer 0 is out of reach.** There is no previous layer to borrow an input from, so the stale gate
  never predicts it — the table shows `-`. This is the method's limit, not a bad score. It is also
  precisely the gap the ETH work exists to close, by training a small per-layer predictor that reads
  the layer's own pre-attention state instead. That needs training data and a shipped artifact per
  model, which is a different project from this one.
- **The first token of a run is unscored**, because a layer only learns the next layer's matrix once
  the graph has reached it. Both cases are counted and reported, never folded into the denominator:
  a routing that was not ranked is not a wrong guess.
- **Decode only.** During prefill every token's routing is known at once and there is nothing to
  predict; it is also not the phase a prefetch would be built for.
- **Routings wider than 32 experts are declined** rather than scored against a truncated ranking.

## Cost, and why it is not a benchmark run

The probe isolates one extra graph node per MoE layer (a barrier, as `--route-trace` costs) and runs
two GEMVs per layer on the eval thread — the prediction and its control. On a 48-layer model with
128 experts that is a few tens of millions of scalar multiply-accumulates per token, single
threaded. Correctness is unaffected and the byte-identity gates cover it (`G9a`), but **do not read
tok/s off a probed run.**

Requires streaming (`--moe-stream`): the probe attaches to the routing nodes the streamer already
isolates, and routing does not depend on how the weights reached memory, so a dense run would only
reproduce the same numbers more slowly.

## Acting on it: `--predict-prefetch`

`--predict-log` measures; `--predict-prefetch` bets. It hands the stale-gate prediction for layer
*l+1* to the same speculative read path [`--prefetch`](prefetch.md) uses — same cache buffers, same
accounting, same `moe-prefetch:` summary line (tagged `[stale-gate]`) — replacing the previous-token
predictor with one measured roughly twice as accurate on the same run. Mutually exclusive with
`--prefetch`, and it needs the LRU cache for the same reason.

Two design points matter more than the swap itself:

- **The speculation is issued after the current layer's load, not when the prediction is made.**
  Every load path begins by quiescing speculation, and the layer's own load sits a few graph nodes
  after its gate — reads queued at prediction time would be cancelled before a lane picked them up.
  Issuing after the load gives a queued read the layer's whole expert-matmul window, the same window
  the temporal prefetch gets.
- **It is drop-aware.** With [`--drop-cold-experts`](expert-dropping.md) armed, a predicted expert
  whose predicted routing weight (softmax over the predicted top-k) falls below the drop threshold
  is not speculated: if it misses, the policy would drop it unread — prefetching it would spend the
  exact I/O the policy exists to save. The top prediction is always kept, mirroring the policy's own
  pin of the top-weighted expert.

Byte-identity is the same contract as the temporal prefetch (gate `G10`): speculation only warms the
cache, so output is unchanged — except under `--drop-cold-experts`, where residency is an input to
the routing policy and a correct guess un-drops an expert. That interplay is deliberate: prediction
spent there buys back quality at the same threshold, not speed.

Cost: one gate GEMV per MoE layer per decoded token on the eval thread (the control GEMV is
probe-only and skipped here), plus one isolated node per layer.

## The honest caveat about what a good number would mean

A high stale-gate accuracy would say the routing is *knowable* earlier. It would not say that
knowing it earlier makes decode faster. Those are different claims, and on this engine the second
one is the doubtful one: when the flash is already saturated — which
[the decode attribution](benchmarks.md) shows it is on Qwen at top-k 8 — starting a read sooner adds
no bandwidth, and speculating extra reads spends the bandwidth that was the binding constraint. That
is why [temporal prefetch](prefetch.md), [layer-LFU caching](benchmarks.md) and the expert sidecar
all lost despite improving the metric each was designed around.

The use that does *not* spend bandwidth is eviction: knowing what the next layer will ask for is a
reason not to discard it. That is a cache-policy change, not a prefetch, and offline replay bounds
the whole class of online cache policies at roughly 5 percentage points over LRU. So even a
predictor that scores well should be expected to buy little — which is exactly why it gets measured
here first, at the cost of one flag, rather than built.
