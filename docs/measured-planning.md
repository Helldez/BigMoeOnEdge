# Measured planning: pricing the plan on the model itself

> **Design, not yet built.** This page describes where `--auto` goes next and in what order. Nothing
> here has shipped; each step names what it removes, what it must not break, and how it is judged.
> The planner as it stands is [hardware-planning.md](hardware-planning.md).

The goal is a planner that is smaller than today's, agnostic by construction, and right more often:
the plan that decodes and prefills fastest on this machine, with this model, holding no more memory
than it needs. The way there is to stop predicting what can be measured, and to measure it on the
model's own tensors rather than on stand-ins.

## Where the plan's numbers come from today

| Decision | Source | On the real model? |
|---|---|---|
| What fits where (layers per device, context and compute reservations) | llama.cpp's capacity fitter, a `no_alloc` load of the real file | yes, sizes only |
| `--io-threads`, `--no-odirect`, `--release-mmap` | the storage probe, reading the model file itself at its own expert slice size | yes |
| Prefill device | a wide matmul probe, then a short real prefill compared against the host at load | yes, for correctness |
| `--threads`, host and device bandwidth | a GEMV sweep on synthetic bytes encoded in the model's quant format | format only |
| Cache size against resident dense groups | seconds saved per byte, with the hit rate credited **linearly** in the fraction of the expert set held | no: a coefficient |
| Layers on a device for decode, beyond what the fitter placed for capacity | the crossing price, which is unmeasured, so never | no |
| `--overlap` | lossless and measured faster on two machines, never armed by the plan (#294) | no |

At the end of a run the plan prints its prediction against the measurement
(`predicted 0.048 s/token, measured 0.055`) and the credited hit rate against the measured one. Then
the comparison is thrown away: the next load plans from zero.

The gaps are where the model was replaced by something simpler. A synthetic GEMV has the right quant
format and none of the rest of the graph: no attention, no crossings between host and device, and
none of the kernels the dense weights actually fall to (on one machine the fast path was gated on
expert tensor names, and dense weights at 2-4 bit ran generic kernels). The linear hit credit is
conservative by about 5x on purpose: the one measurement on record is a cache holding 13% of the
expert set serving 61.9% of the reads.

## The principle

The plan splits into two halves that never mix.

- **Safety is accounted, never tried.** The memory ledger, the token-cycle floor and the correctness
  checks stay rules over facts. An out-of-memory kill is not a measurement you can take and keep
  running, so what is held is decided before anything is held.
- **Speed is measured, on the model itself.** Among the plans the ledger allows, which one is
  fastest is a measurement on the model's own tensors and routing. A coefficient that stands in
  for a measurement goes as soon as the measurement exists.

The rules stay pure functions over `hardware_profile.h` and `model_profile.h`. What changes is that
more facts are measured, and measured on the real thing. A measurement names no platform: "this
layer took 3.1 ms on device 1" holds as well on a phone as on a desktop with two GPUs, which is why
this direction makes the planner more agnostic rather than less.

## What earlier measurements rule out

Each step below is shaped by something this repository already measured and refuted. These are
constraints, not background:

- **No loop that resizes memory against the kernel.** The runtime cache governor was retired because
  a budget the device cannot concede starts a reclaim war the loop itself feeds: gpt-oss went from
  0.517 tok/s with the cache off to 0.156 governed ([pressure.md](pressure.md)). Memory sizes are
  decided at load, by the ledger, and measurement may only choose among sizes the ledger allows.
- **A hit-rate curve is not a throughput argument.** A per-layer cache policy delivered the hit rate
  its simulation promised and was 30% slower ([roadmap.md](roadmap.md#expert-cache-policy--closed-negatively)).
  A simulated curve may size the cache; whether the run got faster is decided by the run.
- **Comparisons must be matched.** A day of A/B on a phone was invalidated by throttling: the
  baseline itself fell from 6.54 to 3.93 tok/s with byte-identical I/O as the device heated
  ([expert-prediction.md](expert-prediction.md)). Candidates are interleaved, never run one after
  the other.
- **One machine is not a rule.** The thread rule from core classes was shipped and withdrawn in a
  day ([hardware-planning.md](hardware-planning.md#a-rule-that-a-machine-took-back)). A measurement on
  this machine replaces the rule rather than tuning it.
- **The budget moves.** The same machine planned minutes apart read 4.3 GB and then 8.2 GB
  available. A result kept between runs keeps what was measured about the model and the hardware,
  never the memory that happened to be free.

## Step 1: the cache sized from the model's own routing

**What it measures.** The engine already writes a per-step, per-layer routing trace
(`--route-trace`), and `scripts/route-replay.py` replays one through LRU and reproduces the recorded
hit rate to the decimal. Bring that replay into `core/src/plan/`: an LRU simulation over a recorded
trace, at every cache size the ledger could allow, gives this model's hit-rate curve. It costs
milliseconds and needs no extra load.

**Where the trace comes from.** From the model's own runs. The first load of a model plans as today;
its session records a bounded trace of its first few hundred tokens; the next load reads the curve
from it. A trace is a property of the model and the kind of prompt, not of the machine, so it stays
valid across machines and memory states.

**What it removes.** `cache_hit_optimism` and the linear credit in `allocate.cpp`. The ranking of the
cache against resident dense groups then prices a hit the model actually delivers at that size, which
is where most of the remaining misallocation sits: on a 16 GB laptop with a 12 GB model the plan
credited 65% and the run measured 79.4%.

**Judged by.** The predicted-against-measured line: the credited hit rate should land within a few
points of the measured one on the reference machines (phone, Mac, Windows laptop) and on #321's
desktop. Decode tok/s must not drop on any of them. Output is unchanged by construction: only the
cache size moves, and the cache is lossless.

## Step 2: lossless knobs settled on the first real request

**What it measures.** The knobs that do not change output and that can change with the session open:
`--threads` first (`llama_set_n_threads` already sets it at context creation), then `--overlap` and
`--io-threads` once it is confirmed they can switch without a reload. On the first real request, after
the cache's hit rate has settled, decode windows alternate between two or three candidates, A B A B,
and the winner must beat the incumbent by `min_backend_win`, the margin the device rule already uses.
The user is not kept waiting: every window is decoding their own answer.

**What it never touches.** Memory: cache size, dense policy, context, ubatch. Those are the ledger's
(see the governor above).

**Kept between runs.** The winner is stored against a fingerprint of the hardware facts, the model
file and the engine version, so the second load starts from it and measures nothing. The core returns
it as a value and takes it back as a value; the front-end decides where it lives, as it already does
for the plan. `core/` gains no file and no environment read.

**What it removes.** The synthetic thread sweep and the `[unprobed]` on `--overlap`. The plan states
the same thing either way: the fact, its source (`[measured on this model]`), and the number.

**Judged by.** Interleaved windows against the incumbent on every reference machine; byte-identical
output with the gates (`bmoe_moe_gates`), since every knob here is lossless.

## Step 3: the cost of a device measured on a real layer

**What it measures.** The expert block and the dense matmuls of one real layer, built from the
model's own tensors with ggml's public API (shapes from the file, as the bandwidth probe already
does for its synthetic GEMV), run on the host and on every device at decode width and at prefill
width, with the results compared for correctness first. Plus the crossing itself: a hidden-state
copy host to device and back, the way the graph would cross at a layer boundary.

**What it unlocks.** The crossing price (`device-crossing` in `allocate.cpp`) becomes a measurement,
and with it the one decision the planner has never been able to make: more layers on a device for
decode than capacity alone dictates, or fewer, where crossings cost more than the device saves. On
#321's machine (two 16 GB cards, a model of 85 GB) this is the decision that matters most.

**What it removes.** The synthetic GEMV of the bandwidth probe and the separate wide-matmul probe of
the prefill device: one probe on the real layer answers both, at two widths.

**The limit.** A whole layer, attention included, is assembled by llama.cpp's graph builder, which has
no public entry point for one layer, and per-architecture attention code here would break the rule
that the streaming path knows no model. So the attention's cost comes from the fitter-placed run, not
from the probe, and the probe is the matmuls and the crossing. If that turns out not to be enough, the
next step is a discussion about the llama.cpp boundary, not an in-tree patch.

**Judged by.** On a machine with a discrete device, the plan's predicted decode within a few percent
of the measured run, and a placement that matches or beats the fitter's. On machines with none, an
unchanged plan.

## Step 4: device memory as a tier of the expert cache

The hierarchy has three levels, and the plan manages only the first step of it:

```
flash ──(our streamer, adaptive cache)──▶ RAM ──(?)──▶ device memory
```

**What already happens between RAM and device memory.** More than the plan says. When a MoE model
does not fit, llama.cpp's capacity fitter does what `--n-cpu-moe` does by hand: it first places
the dense part of as many layers as fit on the devices (attention, KV, norms, router, shared
experts) with the routed experts overridden to host memory, and, if every dense part fits, turns
layers back into full device layers, front to back, while memory remains (`common/fit.cpp`, steps 3 and 4). The plan applies that
placement, and the streamer serves the experts the fitter left on the host. The first real run of
that combination was #321's DeepSeek on two 16 GB cards, and it crashed: the router hook read the
routing result out of device memory (#335). On a box whose device is set aside, the same run
streams everything and the device only computes the prompt: #330's 8 GB card stays nearly empty
while decoding.

**What is missing.** The step from RAM to device memory is static: a layer's experts live on the
device for the whole run or not at all, decided by capacity alone. The tier the hierarchy lacks is a
cache of the hot experts in device memory, computed there.

**The route exists, through the public API.** llama.cpp has an experimental device-side expert
cache, `moe_cache_size` in `llama_context_params` (`--moe-cache-mib` in its tools,
`src/llama-moe-cache.cpp`). For every layer assigned to a device whose experts all stay in host
memory (exactly the fitter's dense-only layers) it keeps an LRU of the most recently used experts
in a device buffer, uploads a miss from the host copy when the scheduler copies the layer's slot
map, and runs the expert matmul on the device. Its budget is split across devices like the layers.
It needs no fork, and it is agnostic by construction: it applies wherever a device has memory of
its own, and on a shared pool there is no second tier to fill.

**What must be settled before the plan arms it.**

- **Correctness with streamed experts.** A miss is uploaded from the expert's host bytes inside the
  scheduler's copy callback. A streamed expert must be resident by then, and nothing guarantees it:
  the expert-ready hook guards the CPU's own read (`mul_mat_id`), not this copy. With `--overlap` the
  read can still be in flight. The gates (`bmoe_moe_gates`) must run with the cache on, on a discrete
  device or on Metal private buffers, which emulate one (as #335 was verified).
- **Double reads.** On a device hit, the streamer still loads the expert from flash into RAM, because
  the hook cannot see the slot map. Correct, and wasted I/O. Either the hit becomes visible through
  public state, or the I/O is measured and accepted.
- **Which use of device memory wins.** The same bytes can hold full layers (the fitter's step 4) or
  expert slots. Step 1's trace replay gives the hit curve at every slot count. Step 3 gives the cost
  of an expert on the device against the host and the price of a crossing. The plan picks the split
  with both, measured on the model.
- **Pipeline parallelism.** llama.cpp turns pipeline parallelism off when the cache is on. On several
  devices that is a cost to weigh, not a detail.
- **One failed device must not take the others with it.** Today, if any device fails its isolated
  probe, the fitter is not run at all (`placement_probe.cpp`), and every device loses its placement.
  The failed device should be left out of the fitter's device list (`mparams.devices`) and the
  others placed as usual. #330's machine, an unstable integrated GPU beside a working external card,
  is the case this decides.

**Judged by.** Byte-identical output with the cache on (gates). Decode and prefill against the same
plan without it, interleaved, on #321's desktop and on a single small card like #330's. On machines
without a device of their own, an unchanged plan.

## What the planner looks like afterwards

- **Memory:** the ledger, unchanged, still the only thing that decides what is held.
- **Speed:** facts measured on the model, the routing trace for the cache, the first request for the
  lossless knobs, one real layer for devices. Each enters the existing ranking in its own unit.
- **Tiers:** flash, RAM and device memory, each a cache for the one above it. The split of device
  memory between full layers and expert slots is chosen with the same measurements.
- **Removed:** the linear hit credit and its coefficient, the synthetic thread sweep, the synthetic
  bandwidth GEMV and the separate prefill matmul probe, and the unpriced crossing term that kept
  devices out of decode.
- **Remembered:** what was measured about this hardware and this model, never the free memory.

Fewer coefficients, fewer probes, and every number in the plan traceable to a measurement on the
thing it describes.

## Order

Step 1 first: it is cheap, self-contained, cannot change output, and its error is already printed at
the end of every run. Step 2 second, because it needs the stored result Step 1 introduces. Step 3
next, the largest piece, and first validated on #321's desktop, the one machine in reach with two
discrete devices. Step 4 last: it needs Step 1's curve and Step 3's costs to choose its split, and its
correctness check can start earlier, since it decides whether the tier is usable at all. The
failed-device fix in Step 4 does not wait for any of it. Each step lands with its own measurements in this page, and is withdrawn here if a
machine takes it back.
