# Route-ahead: committing the routing to its own prediction

`--route-ahead N` — experimental, lossy, off by default.

## The idea

Every prefetch this engine has tried lives with the same ceiling: the routing of MoE layer L is a
function of the hidden state *entering* layer L, which does not exist until layer L−1 has run. A
predictor working any earlier is structurally approximate — measured here, evaluating a layer's
gate on the hidden state one layer back agrees with the real routing on roughly 85% of slots, and
each further layer of anticipation costs ~3.5–4pp more ([expert-prediction.md](expert-prediction.md)).
So a speculative prefetch is always paying for some misses, and the anticipation window it buys is
never wider than the accuracy it can afford.

Route-ahead inverts the bet: if the prediction cannot reach the routing, lower the routing to the
prediction. With `--route-ahead N`, the expert selection of decode layer L is *replaced* by the
ranking layer L's own gate matrix produced on the hidden state N layers earlier in the same
forward pass. The committed selection is by definition known N full layers of compute before layer
L needs its experts — a prefetch of them can start that early and can never be wrong, the
100%-hit regime no honest predictor reaches. The literature's trained variant of this is Pre-gated
MoE (ISCA'24), which fine-tunes the gate to choose the next layer's experts; route-ahead is the
training-free version, and the flag exists to measure whether the model survives it.

## What still runs, and what changes

The router itself is not skipped. Layer L's gate matmul still computes, for two reasons:

- its logits feed the graph's own weight chain, so after the ids are rewritten at the topk node
  the substituted experts receive their **true, renormalized routing weights** — the graph's
  `get_rows` + norm/softmax runs on the committed ids exactly as it would on the router's own;
- its gate input is what produces the prediction for layer L+N — read barrier-less at the topk
  callback and ranked on the prediction worker thread, the same machinery `--predict-prefetch`
  built (and the same sampled fresh-gate watchdog validating the read). Nothing is isolated and
  no graph barrier is added, so the eval thread pays only a row copy per layer. Because a
  committed prediction's failure mode is the routing itself rather than a wasted read, two
  guards close the gap: a watchdog trip disarms the policy out loud and permanently for the run,
  and a layer whose ranking is not ready when its topk fires keeps the router's own choice
  (counted as passed through) — never a stale or suspect one. At N=1 the worker's deadline is a
  single layer of FFN compute, so the ranking usually lands exactly at the committing topk and
  the early-read window is thin; at N=2 and above the ranking is collected a layer or more
  before its topk and the reads start there — depth buys window, at the staleness cost the
  agreement number reports.

What changes is only *which* experts the selection names. The rewrite happens at the topk node,
before anything consumes the ids, so the trace, the drop policy, `load_layer` and the expert
matmul all agree on the committed routing — the pipeline has no second opinion to leak through.

Layers `0..N-1` (nothing precedes them by N), prefill, the first decode token (the gate matrices
ahead are not stashed yet) and any layer whose gate or input the hook cannot read route normally
and are counted as passed through, never silently substituted.

## What it costs

This changes the output. The committed selection disagrees with the router on a measured fraction
of slots (the `moe-route-ahead:` line reports it per run; expect ~15% at N=1 on a 128-expert
model, worse as N grows), and unlike [expert dropping](expert-dropping.md) — which only removes
low-weight experts from the tail of a routing — a disagreement here can replace a *top-weighted*
expert, and the perturbed hidden state feeds the next layer's routing, so errors can compound
through the depth of the model. Whether they compound into visible damage is exactly the open
question; it cannot be answered from routing logs, only by comparing generations.

It is therefore mutually exclusive with `--predict-log` (the probe would grade a predictor against
a routing rewritten from that same predictor), and with both prefetchers (`--prefetch`,
`--predict-prefetch`: betting speculative lanes on a future the policy has already fixed is the
same bytes twice). It composes with the cache and with `--drop-cold-experts`, which operate
downstream of whatever the selection says.

## Reading a run

```
moe-route-ahead: 1 layers early — 6096 routings committed, 48 passed through;
committed selection agreed with the router on 84.7% of slots
```

The agreement percentage is the size of the perturbation the generation ran under, not an accuracy
claim — with the routing committed there is no miss to speak of. Judge the flag on the generated
text against `--route-ahead 0` on the same prompt with greedy sampling, and on tok/s only after
quality survives.

## The early reads

With the LRU cache on, the committed selection is also *acted on*: the moment layer L's own load
is handed to the source, the committed ids of layer L+N — fixed at L's gate matmul a few
callbacks earlier — are split by residency and issued on the same speculative path `--prefetch`
uses: residents are retained (LRU-protected, zero bytes), misses are read on the idle lanes. No
`predict-spec-max`-style cap applies, deliberately: these are not guesses, they are the routing
the topk of L+N will commit verbatim, so a miss not issued early is the same read paid on demand
later, minus the N-layer window. The `moe-prefetch:` line reports them with a `[route-ahead]`
tag; expect the useful fraction to sit at ~100%, which for every other predictor would be
suspicious and here is the construction. Without the cache the selection still commits but
nothing is read early — the flag then only measures quality.

The issue list is drop-aware, and on a device this is not a nicety but the difference between a
win and a rout: with `--drop-cold-experts` armed, the baseline never reads the routing's cold
low-weight tail at all, and an early read that makes those experts resident un-drops them — a
perfect prefetch of bytes the run was never going to pay for. Measured on a phone at depth 4,
drop 0.75: 78 → 244 MB/token and half the tok/s. So committed experts whose predicted routing
weight sits below the drop threshold are left cold on purpose (the same softmax filter
`--predict-prefetch` applies), and the commit-time drop discards them unread exactly as it does
the router's own tail. With dropping off the filter is inert and the full committed top-k is
issued.

Committed reads are never cancelled. The speculation machinery was built for guesses, and its
housekeeping treated every queued read as disposable: each demand load (and each settle before an
issue) quiesced the lanes, discarding in-flight and queued speculation wholesale — measured, that
destroyed every early read at depth 2 (0% useful, the same bytes re-bought on demand). Under
route-ahead the queue holds no guesses, so the same housekeeping adopts instead: a loading layer
moves its own committed jobs to the front and finishes them (they are its demand reads, already
staged), completed entries integrate into the cache, and other layers' committed reads stay
queued for the idle lanes. On the serial path the realized window is still one layer — lane time
only exists during a layer's FFN — so depth beyond 1 buys nothing there; under `--overlap` depth
is the lever: on the host A/B (fixed cache, same prompt) N=1 cut the overlap stall from 0.060 to
0.047 s/token and N=2 cut it to 0.010 (−83%) with 97% of early reads useful, worth +20% tok/s on
a machine whose decode is otherwise compute-bound. N=2 is the recommended overlap setting;
deeper adds routing perturbation faster than it adds window.

## What it costs, measured

The feature reports its own overhead, because every attempt to reason about it from wall time
alone was wrong. `moe-route-ahead:` prints the prediction's worker CPU (`ra_gemv_ms/tok` in the
CSV trailer) and, separately, the eval-thread cost of issuing the early reads — different threads,
and only the second is on the critical path. Alongside them the cache reports `evictions` and
`re-reads` (a read of an entry it had held before), which is where any prefetch that raises the
byte count while calling its reads useful must be hiding.

Two of those numbers were bugs, found by instrumenting rather than by argument:

- **The issue path took the I/O lanes' mutex once per expert.** `prefetch` filtered residency
  under a fresh lock per candidate, roughly 120 acquisitions a token on the mutex four lanes pull
  every read index from. A blocked eval thread is a descheduled compute thread: the same run with
  one lane, hence no contention, did identical work at a quarter of the compute time (0.189 vs
  0.793 s/token). Filtering under one lock, plus an atomic pre-check in the settle, put the whole
  issue path at **0.9 ms/token**.
- **The gate GEMV was bandwidth-bound, not compute-bound.** Ranking 256 experts re-reads the
  target layer's whole gate matrix — ~4 MB per layer, ~160 MB/token of DRAM traffic on a device
  whose decode already saturates the bus, which is why vectorising the loop changed little. An
  int8 mirror of each gate (one scale per expert row) with the activation quantized per call
  makes the inner loop int8×int8 in int32 — the shape ARM's dotprod instructions exist for — and
  cuts it to **6 ms/token** on device from 19, with the committed selection agreeing with the
  router on 74.5% of slots against 74.7% for the exact weights. It is aarch64-only and measured
  both ways: on x86 the float path is already vectorized and bandwidth is ample, so the int8
  detour costs more than it saves.

## Status

**Host: a measured win.** Quality holds — the committed generations are indistinguishable from the
baseline's on a four-prompt objective battery (arithmetic, translation, code, facts: 4/4 correct
in both, one answer byte-identical), on a 512-token essay, and on a second model of a different
generation and quantization. Output is **deterministic**, byte-identical across repeated runs,
unlike [expert dropping](expert-dropping.md), because the prediction depends only on hidden states
and never on what the cache happened to hold. Throughput over three interleaved pairs under
`--overlap` at a fixed cache: **+17%** median with the overlap stall down 0.060 → 0.010 s/token;
**+22%** on the second model; **+40%** with enough cache headroom that nothing is evicted.

**Device: +21%, measured.** Byte accounting is settled first — an I/O trace of every physical
read, decode-only and equal length, shows the committed routing reads what the baseline reads
(133.6 vs 134.4 MiB/token) with the same 4% read redundancy and zero speculative-then-demand
pairs, so nothing is fetched twice. On throughput, absolutes on this phone are worthless (after a
long session its own baseline swung between 0.79 and 4.54 tok/s in the same cell), so the figure
comes from **interleaved cells** — baseline and N=2 alternating, two passes, same prompt and
length, so thermal drift hits both equally:

| cell | tok/s | compute | stall | mgmt (of which adopt) | flash |
| --- | --- | --- | --- | --- | --- |
| baseline, 4 lanes | 4.36 | 0.135 | 0.067 | 0.027 | 630 MiB/s |
| N=2, 4 lanes | **5.29** | 0.146 | 0.010 | 0.035 (0.008) | 637 MiB/s |
| N=2, 2 lanes | 5.16 | 0.127 | 0.014 | 0.053 (0.027) | 1045 MiB/s |
| N=2, 1 lane | 2.47 | 0.139 | 0.026 | 0.149 (0.124) | 1090 MiB/s |

The win is the stall and nothing else: the compute residual barely moves, so this is not a trade
of one cost for another. Lane count is the interesting column — flash throughput saturates well
before four lanes (630 vs 1045 MiB/s of effective bandwidth for the same bytes), so the extra two
lanes buy no bandwidth while costing DRAM traffic that the compute now competes with; at two lanes
the compute residual is the lowest of any cell, but the adoption wait grows to pay it back, and at
one lane the adoption wait dominates outright. Four and two lanes come out level.

**Inside the demo app the same build measures only +4%, and the cause is not the engine.** With
the app in foreground the device caps its six main cores at 1.9 GHz (2188800 → 1900800 Hz,
measured with the app stopped and running). That slows exactly the compute this flag works to
keep busy, so the share of a token that was stall — the part route-ahead removes — shrinks. It
penalises the whole engine, not this feature.

The flag stays **off by default**: it is lossy by construction, and a lossy default is a decision
for a release note rather than a benchmark.

## Next: the second router matmul, and the fork it argues for

One structural cost remains, and it is not in the algorithm — it is in what this engine may touch.
The baseline computes **one** gate matmul per layer, `gate_L × h_L`. The algorithm as designed
also wants one, substituted: `gate_{L+N} × h_L`. This implementation runs **two**, because the
graph's own router node computes regardless and cannot be removed through the public eval callback.
That second matmul *is* the 6 ms.

Removing it means changing the graph, i.e. the [fork](seam.md) that already exists for the
expert-ready hook `--overlap` depends on. The shape:

1. at layer L the graph computes its logits against layer L+N's gate matrix rather than its own;
2. the MoE node takes its expert ids **and weights** from an override hook — the same shape as the
   readiness hook — instead of its own argsort and softmax.

That is roughly 50-100 lines in `build_moe_ffn` plus the engine side, and it takes the prediction's
cost to zero: route-ahead would then compute exactly what the baseline computes. The trade to
measure is quality, not speed: today the substituted experts carry the true router's renormalized
weights (the graph computes them anyway, so they are free); under the substitution they would carry
the prediction's own, which the quality battery above is already set up to judge.

Now that the device number exists, the fork can be sized honestly: 6 ms against a token that takes
roughly 190 ms there is **about 3%**. It is real and it is the only structural cost left in the
algorithm, but it is not where this feature's remaining time is. The measured costs that are
larger sit in the streaming path both configurations share — the read lanes spend most of their
CPU inside the kernel's O_DIRECT path, and the cache's page commit/evict churn shows up as
`mgmt_ms` — and neither is specific to route-ahead. Those come first.
