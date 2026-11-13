# Measured planning: pricing the plan on the model itself

> **Partly built.** This page describes where `--auto` goes next and in what order. Steps 1 to 5 are
> built and await their on-device judgement (their status is below each). Each step names what it
> removes, what it must not break, and how it is judged. The planner as it stands is
> [hardware-planning.md](hardware-planning.md).

The goal is a planner that is smaller than today's, agnostic by construction, and right more often:
the plan that decodes and prefills fastest on this machine, with this model, holding no more memory
than it needs. The way there is to stop predicting what can be measured, and to measure it on the
model's own tensors rather than on stand-ins.

## Where the plan's numbers come from today

| Decision | Source | On the real model? |
|---|---|---|
| What fits where (layers per device, context and compute reservations) | llama.cpp's capacity fitter, a `no_alloc` load of the real file | yes, sizes only |
| `--io-threads`, `--no-odirect`, `--release-mmap` | the storage probe, reading the model file itself at its own expert slice size | yes |
| Prefill device | one real layer of the model at 128 tokens (Step 3), then a short real prefill compared against the host at load | yes |
| `--threads`, host and device bandwidth | a GEMV sweep on synthetic bytes encoded in the model's quant format; for `--threads`, once a front-end kept a verdict, the model's own decode on the first answer (Step 2) | format only; with a verdict, yes |
| Host and device rate on the experts and on the dense matrices | one real layer of the model at one token (Step 3) | yes, its matmuls |
| Cache size against resident dense groups | seconds saved per byte, with the hit rate read off the model's own routing record (Step 1), or on a first load credited **linearly** in the fraction of the expert set held | with a record, yes; on a first load, no: a coefficient |
| Layers on a device for decode, beyond what the fitter placed for capacity | the fitter's split layers priced against the same layers on the host, on the measured crossing and dense rates, in the ranking (Step 3) | yes, its matmuls and the crossing |
| `--overlap` | measured on the first answer against the other setting, and kept (Step 2); without a place to keep it, never armed (#294) | with a verdict, yes |

At the end of a run the plan prints its prediction against the measurement
(`predicted 0.048 s/token, measured 0.055`) and the credited hit rate against the measured one. The
load also keeps what its decode cost, and the next plan of the same configuration prices decode with
it (Step 5).

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

**Status: built, judgement owed.** What landed, and how it was checked off the device:

- The replay is `replay_lru` in `core/src/plan/routing.cpp` (`bmoe/routing.h`). On the three archived
  phone traces it matches `scripts/route-replay.py`'s LRU row to the third decimal at five budgets each,
  and at each run's own budget reproduces the hit rate the run recorded (74.9, 76.8, 32.4%); a test
  (`routing`) holds both. In the gates (`G2r`), the record a real streamed session kept replays to the
  hit rate that session's cache measured, prompt included and decode alone.
- The record is kept by the session itself (`--routing-record`, default 256 decoded tokens and the
  prompt's last 256), from the ids the hook gathers for the cache anyway: no extra graph nodes, unlike
  `--route-trace`. It is one line per cache request, so a chunked prompt replays as the engine served
  it rather than as one batch.
- The front-ends keep it: `bmoe-cli --routing-dir DIR`, `bmoe-server` under `routing/` in its data
  folder, one file per model named by a content fingerprint of the file (`model_fingerprint`). The
  next plan of the file hands it back (`AutoPlanOptions::routing`) and `attach_routing` turns it into
  `ModelProfile::hit_curve`: `hit_share(bytes)`, monotone, over the whole expert set, absent unless
  the record is this model at this top-k with enough decoded tokens.
- The cache then enters the ranking as pieces of the curve's concave hull, and is credited the curve
  at the size it gets, tagged `[measured on this model]`. The predicted-against-measured line sets
  that credit against the decode's own measured hit rate, the figure it prices.
- **What the ranking leaves unspent is the cache's too** (`cache-remainder`). The record is a few
  hundred tokens, so its curve is flat past the experts those tokens touched and prices a byte past
  them at nothing; a longer answer routes to experts the record never saw, and RAM left idle saves
  nothing. The first version stopped the cache at the curve's knee: on a 16 GB card with 30 GiB of
  RAM and Qwen3.8-Flash-Next, the plan's 15.7 GB cache held 83% of the routed experts over 1024
  tokens and decoded 11.6 tok/s, where 22 GB held 96% and decoded 20.7 (27.5 over the last 256
  tokens).
- **Not removed:** `cache_hit_optimism` and the linear credit stay as what a first load, with no
  record yet, is priced with, and a plan without a record is byte for byte the plan before this step
  (asserted in `planner`). They go when every front-end ships with a record for every model it
  plans, which is not yet true on a first download.
- The same record ranks the hottest (block, expert) pairs by use (`hottest_experts`), for Step 4's
  device tier to warm from at start. Nothing reads it yet.

Owed: the on-device runs this step is judged by (the reference machines and #321's desktop), each a
first load to record and a second to plan from the record, with the predicted-against-measured line
from both.

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

**Status: built, judgement owed.** What landed, and how it was checked off the device:

- **The policy** is `KnobTrial` (`core/src/engine/knob_trial.h`), pure and fed one decode step at a
  time. It waits for the cache's hit rate to settle (two windows within `settle_hit_delta`, two
  points), then runs `incumbent, c1, incumbent, c2, incumbent, ...` for `rounds` (3) rounds, a window
  of `--trial-window` tokens (8) each, the first token after a switch untimed. A candidate's ratio in
  a round is its seconds per token over the mean of the two incumbent windows around it, so a drift
  that is linear over three windows cancels exactly; the verdict is the median of the ratios, and a
  candidate wins only past `min_backend_win` (the same field the device rule reads). A candidate
  stops being measured once the rounds left cannot make it a winner: the median of three ratios is
  past the margin as soon as two are, so two losing rounds settle it and the third leaves the
  schedule, whatever the hardware (eight threads on a phone with four fast cores ran every window
  at ten times the seconds per token, and three such rounds tripled the first answer); one stalled
  round settles nothing, as before. One window settles a candidate that ran apart from the
  incumbent: its fastest token slower than the incumbent's slowest token in the trial by more than
  the incumbent's own range (slowest over fastest) and the margin. The bound is the noise the trial
  measured, not a constant: a stall elsewhere slows some tokens of a window, not every one, and a
  gap wider than any swing the incumbent has shown is the arm's own (on a 12 GB phone, eight threads
  against four ran about 2 s a token against 0.1 s, and the median's second losing window was 18 s
  of stutter). The cache brake does not read a token decoded under a candidate
  ([pressure.md](pressure.md)). The `knob_trial`
  test holds it on synthetic timings: equal arms under 10% noise never win over 50 seeds, a 25% win
  always does, 5% is a tie, 12% wins, a run that slows to twice the time per token measures a 20%
  win as 20%, one stalled window does not overturn the median, a window cut by the end of a request
  is dropped, a candidate twenty times slower stops after one window and leaves the schedule, a 5%
  loss does not, and a loss inside the incumbent's own spread is left to the median.
- **Which knobs switch live.** `--threads` through `llama_set_n_threads` (decode only; prefill keeps
  its count). `--overlap` too, by a change to the streamer rather than a reload: serial and overlapped
  reads differ only in whether the first read lane is the eval thread or a worker of its own, so the
  switch is starting or stopping that one worker and registering the expert-ready hook, between two
  decodes (`ExpertStreamSource::set_overlap`). It needs every layer on the host: with one on a device
  overlap also turns off the context's offload of host-weight matmuls, which is fixed when the
  context is created. **`--io-threads` is left out**: the storage probe already measures it on the
  model's own file at its own slice size, and changing it with the session open means reopening every
  reader with another lane count for a figure the plan already has from the model.
- **Lossless.** Gate `G24` runs the trial over a 120-token answer (threads 2 against 1 and 4, then
  overlap) and over four turns of 40 that each end inside a window; every answer equals the resident
  one, and the gate fails if the trial reached no verdict.
- **Kept.** The verdict is `MeasuredKnobs` (`bmoe/measured_knobs.h`), returned by
  `Session::measured_knobs()` as each knob's verdict comes in. The front-ends keep it beside the
  model's routing record, one `.knobs` file per model fingerprint with a line per machine
  (`hardware_fingerprint`: cores, core classes, total memory, each device's name and own memory) and
  engine version. `bmoe-cli --routing-dir DIR` arms the trial on its `--auto` plans and writes the
  verdict; `bmoe-server` always does, under `routing/` in its data folder. `core/` gained no file
  access and no environment read.
- **Read back.** `probe_and_plan` matches the record to this machine and engine
  (`AutoPlanOptions::knobs` into `ModelProfile::knobs`); the plan applies it, says
  `measured on this model` with the gain and the margin, and arms no trial. A plan with the feature
  off and no record is byte for byte the plan before this step: the previous `planner` test's whole
  output (its checks and three printed rationales) is identical against this build, and the
  `planner` test asserts the overlap and thread decisions word for word.
- **Not removed yet:** the synthetic thread sweep stays as a first load's count, the one the trial
  starts from; it goes when every front-end ships with a verdict for every model it plans, as with
  Step 1's linear credit. A device-side expert cache refuses overlap, so when one is armed neither
  the trial nor a verdict may turn overlap on.

Owed: the on-device runs this step is judged by, each a first load whose first answer measures and a
second that starts from the verdict, on the phone, the Mac and the Windows laptop.

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

**Status: built, judgement owed.** What landed:

- **The probe** (`core/src/plan/layer_probe.cpp`). The MoE block with the most expert bytes in the
  file the experts live in; its expert matrices (a block's 3-D weights indexed by the expert count,
  whatever the file fuses) as `mul_mat_id`, k experts per token, and the block's other 2-D weights
  as `mul_mat`, each matrix with an input of its own width - so no dataflow, no norms and no
  attention arithmetic, which is the stated limit. The bytes are the file's, read once. At one token
  a pass reads enough disjoint sets of k experts, and enough copies of the dense matrices, to outrun
  the last-level cache (64 MiB of each, at most 256 MiB of experts held, 16 copies); at 128 tokens a
  subset of the experts gets the per-expert batch a 128-token prompt gives the whole set
  (`128 * k / n_expert`), so the kernels run at the prompt's own intensity. Samples are 0.15 s each,
  every pass ends with a read of its result (a backend that queues work is timed on doing it, not on
  accepting it), and everything is freed on every path. It runs in the isolated child with the other
  device probes; the facts line is `v=2` and carries `expert=` and `dense=`.
- **Correctness first.** Every backend computes the same bytes on the same inputs, and a device too
  far from the right result at one token is refused for good (`identity_ok = No`); at 128 tokens it
  is not armed for the prefill. The right result is computed: the compared nodes (the first decode
  group, and the first four tokens at width) in double precision from the dequantized weights. A
  device passes within twice llama.cpp's per-op NMSE bound of it (`k_max_nmse_device`, 1e-3).
  Upstream's 5e-4 compares a backend with the CPU on random data, and upstream relaxes it where a
  backend quantizes the activations its own way. RTX 3090 cards on GLM-4.5-Air's Q5_0/Q5_1 experts
  measured 5.3e-4 from the exact result (the host 1.1e-5, every other layout 6e-6 or less), where a
  wrong kernel is off by 1e-1 to 1. Judging a device by the host instead refused those cards for
  rounding differently from the host. The host is the reference only where the exact result cannot be
  computed (a type with no conversion to float), or where the host is not within `k_max_nmse` of it.
  The plan's log line `real layer: <device> on the <half>: <e> from the exact result, the host <e>`
  gives both distances. A node a device has no kernel for is left out of its graph and priced at the
  host's rate for that half, where the scheduler would run it.
- **The facts.** `expert_gibs` and `dense_gibs` per device and the host's `host_expert_gibs` and
  `host_dense_gibs`; `wide_matmul_gibs` is now the whole layer at 128 tokens. A rule comparing a host
  with a device reads both figures from the same experiment (`RatePair`): the real layer's where both
  were measured on it, the GEMV's otherwise.
- **The crossing** (`device_probe.cpp`): one token's hidden state copied host to device, one op run
  there, copied back, each leg waited for; half the round trip. The chain differenced through the
  scheduler it replaces read 0 on a CUDA card (its mixed arm was no slower than the host arm), which
  is why every discrete card's plan said the crossing was unpriced.
- **What reads them.** `choose_device_tier` (Step 4) prices the split's host layers at the host's
  real expert rate. The allocation prices the host's expert and dense compute each at its own rate,
  and a dense group's offer to a device on the dense pair. And the new `price_layers_home`: where the
  fitter put the dense part of some layers on a device with their experts on the host, those layers'
  dense matmuls on the device plus two crossings each are priced against the same matmuls on the host.
  Where the host wins by `min_backend_win`, bringing them home - whole, onto the host, the device
  keeping as many whole layers at the top - is one more candidate in the ranking (`device-crossing`),
  its RAM their dense part and, bounded, the context, against the cache; funded, the plan is made
  again from that placement (`device-layers`). The device's bound per whole layer is the file's
  largest, so a device that could not be shown to hold them keeps the fitter's placement.
- **No regression where there is no device of its own.** The plan of a CPU-only machine, and of one
  whose devices share its memory, is byte for byte what it was on the planner's fixtures, the real
  layer's rates included, and a measured crossing moves no layer there (asserted in `planner`). The
  full plan text of twelve fixture plans was diffed before and after: only a discrete card with a
  split placement gained a line, its priced `device-layers`.
- **Measured off the device, on this repository's tiny models** (gate G26, every architecture): a
  16 GB laptop's host, its integrated GPU and the loopback RPC device all agree with the host at both
  widths, every fact is filled, the crossing reads 65 us on the integrated GPU and 28 us on loopback
  RPC, and the probe with the device costs takes about 7 s for the three backends in one process.

**Not done.** The synthetic GEMV is not removed: the thread sweep runs on it, and `memory_bandwidth_gibs`
and `host_bandwidth_gibs` keep its meaning for the rules that read a bandwidth (the shared-memory
notes, the expert-compute note); Step 2 retires the sweep. "More" layers on a device than the fitter
placed is not chosen: the bytes the split layers free on the device would hold more whole layers, but
sizing them needs the fitter's projection rather than a bound. Attention is not probed, as stated. The
plan's predicted decode still prices the host's share only; the device side is in the `device-layers`
line. Owed: the runs this step is judged by, on #321's desktop first.

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

**Status: built, judgement owed.** What landed (`choose_device_tier` in `core/src/plan/planner.cpp`):

- **Correctness.** Gates G25a-c: the device cache over resident experts, over streamed experts, and
  over streamed experts under n-gram speculation, each byte-identical to the same placement with the
  experts resident, on the loopback RPC device (buffers that are not host memory, as a discrete card's
  are). The cache was live in every run: 75-90% device hits, misses uploaded.
- **The rule: the experts used most live next to the fastest processor.** It is the rule flash and
  RAM already follow, one level up. Where every layer's dense part is on devices with memory of their
  own (the fitter gives the dense part, read by every token, the first claim on them) and a device
  computes the model's experts faster than the host by `min_backend_win` (both rates from the same
  experiment: the real layer where both were measured on it, the GEMV otherwise), the card's memory
  past its dense part holds a cache of the experts used most, from every layer, and every layer's
  experts go back to the host, resident or streamed. A miss is uploaded from there as a miss in RAM
  is read from flash. The cache fills from use, so it needs no record of the routing; a record only
  says, in the plan's line, how much of a token it serves. Where the device is not faster, its rate is
  unmeasured, or the dense parts did not all fit, the fitter's placement stands.
- **Sized as the RAM cache is (#428).** The first version took exactly the bytes the fitter gave whole
  layers' experts, so it inherited the fitter's granularity: the memory under one more whole layer was
  never used, and a card whose dense part fit with no whole layer beside it got no cache at all. The
  cache now takes those bytes plus what the fitter left free past its margin, per card, read in the
  placement probe off the same free figure the fitter planned against; on several cards, the largest
  budget whose share (llama.cpp splits it by layers) fits every card. Its floor is one token cycle,
  the RAM cache's floor for the same reason; under it, the fitter's placement stands. llama.cpp's
  cache is an SLRU and the replayed hit curve an LRU's, so the plan's share for it is an estimate.
- **Why not a price.** The first version priced the two per token (the split's host layers at the
  host's rate and its crossings, against the cache's misses at the measured upload rate) and armed
  the cache only where it won. On the second rented card of the runs below it called them a tie
  (4.23 against 4.73 ms a token) and kept the layers, which decoded at 30-47 tok/s against the
  cache's 64-79 there set by hand: the price missed what a host layer costs beyond its bytes, and a
  price that can be wrong by half on the one comparison it exists for is a coefficient, not a fact.
  A layer split by capacity waits for the host on every token wherever the device is the faster
  processor; that part needs no price.
- **Kept by measurement across loads.** The rule is necessary and not sufficient: every miss of the
  cache crosses the link before the device computes it. On two 24 GB cards over PCIe 3 with GLM-4.7
  (355B, 32B active), the cache held a tenth of the experts, misses went up at 4.5 GB/s, and decode was
  1.6 tok/s where the fitter's placement did 3.9. A price would have to know the hit rate the cache
  reaches and what the host really decodes at, and the one comparison on record is wrong by half on
  exactly those. A device cache is fixed when a context is created, so the two sides cannot
  alternate inside one answer as the knob trial's do. They alternate across loads instead
  (`MeasuredKnobs::device_cache_tps`, `no_device_cache_tps`). A load with nothing measured arms the
  cache and times its decode (`KnobTrialConfig::device_tier`, a `RateProbe` after the knobs' trials:
  the median of `2 * rounds + 1` windows, after the longest settle the trial makes, since a device
  cache fills out of the session's counters). The next load runs the fitter's placement and times it.
  The plan after keeps the faster, and the cache holds where the two are within `min_backend_win`.
- **The slower side is not timed where it cannot win.** Timing the fitter's placement costs a whole
  load at its rate. On a 12 GB card with Qwen3.6-35B the cache decoded at 73.5 tok/s, and the second
  load then decoded a whole session at 37.8 to time a placement that could never have won. That
  placement needs no price of a miss, because it pays none: it computes the experts the fitter left on
  the host at the host's rate and everything else on the devices. Its ceiling (`fitter_decode_ceiling`)
  adds what a token reads at each processor's probed rate and leaves out crossings and overheads (55
  tok/s there). A probe can read below the decode: on two 16 GB cards with GLM-4.5-Air the ceiling
  was 8.5 and the placement decoded at 9.34. So the ceiling is raised by `rate_ceiling_slack`, a
  quarter, the plan's own accuracy bound. The fitter's placement is timed only where that reach beats
  the cache's measured rate by `min_backend_win`; elsewhere the second load keeps the cache, and the
  plan line gives the two figures. On GLM-4.7 over PCIe 3 (cache 1.6 tok/s) and GLM-4.5-Air (cache
  4.6-5.9), it is still timed, and won.
  The front-end merges each load's side into the record it keeps for the model and machine. Gate G25e
  times both sides on the loopback device, byte-identical, with a rate in the record. Where the plan
  is calibrated at load, both sides are timed before it is served instead, each a short load of its
  own ([below](#decisions-across-loads-timed-at-load)); the across-loads timing is what a load with
  calibration off, or one it left incomplete, does.
- **Not done.** The double reads stay, measured rather than removed: on a model that fits the host,
  the experts are resident and nothing is read. Pipeline parallelism on several devices is not
  weighed. A warm start of the device cache from the record's hottest experts is not built: the first
  hundred tokens fill it (51 tok/s in the first 128 tokens, 84-88 from the 256th, on the card below).
- **Measured on a 24 GB discrete card,** Qwen3.8-Flash-Next 125B Q2_0, every layer on the card: experts
  on the cores 18.9 tok/s, the best whole-layer split 31.9, resident behind a 16 GB device cache 61.8
  and 62.8 over 256 tokens (84-88 once settled), against the planner's prediction of a cache faster
  than the split on a curve like that model's. On a second card the plan's own choice, from a
  recorded first run, armed the cache: 63.3 tok/s over 256 tokens and 80.8 over 1024. The cache costs
  prefill there: 650 tok/s on a 1516-token prompt against 1134 for the whole layers, which the plan
  does not price yet. The rule above, which arms the cache from the first load, is not yet run on a
  card.

## Step 5: the decode's own cost, the coefficient a run's counters correct

- **What it removes:** a decode price that never learns. The ledger prices a token's compute from a
  probed matmul rate (`seconds_at_gibs(bytes_per_token, host rate)` per group) and the cache's hits
  from the routing replay or the linear credit, and a one-token decode pays what neither sees:
  per-op and thread-synchronisation costs, and a cache that serves less than the replay of a few
  hundred tokens did. Measured on a 12 GB phone with Ling-3.0-tiny Q8_0, `--auto` through
  `bmoe-server`: predicted 0.037 s/token at a 95% credited hit rate, measured 0.107 at 77-85%, the
  per-token CSV's `compute_ms` alone ~64 ms; after the run the prediction was still 0.037. A 16 GB
  Mac predicted 0.073 against 0.054 measured, a Windows PC 0.18 against 0.21: the error has no sign a
  rule could fix.
- **What is recorded.** Every load times its decode with a `RateProbe`, the trial's own windows
  (`--trial-window` tokens, `2 * rounds + 1` of them, their median), once the cache has settled and
  after any knob trial, so no switch is in its windows; it switches nothing, so it runs on loads that
  arm no trial too. It keeps, in `MeasuredKnobs`: `decode_compute_s`, the median window's compute
  per token (the wall less the flash waits and the cache's management, the CSV's `compute_ms`);
  `decode_threads`, the count it ran at; `decode_hit`, the cache's hits over the same windows, at
  `decode_cache_mib`, the host cache's budget then; and the placement, `decode_gpu_layers` and
  `decode_device_cache_mb`. They are keys of the same `bmoe-measured-knobs 1` line, which an older
  engine reads past and which, missing, read as not measured. A later load's figure replaces the
  whole measurement, never half of it, and it counts in `verdicts()`, so the front-end keeps it.
- **How the plan uses it** (`planner.cpp`, `calibrate_decode_cost`). Only for the configuration it
  was measured under: the plan's decode thread count (the one `choose_threads` will settle on) and the
  same placement on the devices; otherwise a `decode-cost` decision says the decode is priced from
  the probes and why. Compute: the ratio between the measured compute and what the cost model prices
  the same placement at (`host_compute_seconds`) scales every group's compute term alike
  (`AllocationInputs::compute_scale`), so the predicted token moves to the measurement, the groups
  keep their prices relative to each other, and the ranking, whose savings are reads, decides what it
  decided. Hits: the ratio between the hits measured at that budget and the credit there scales the
  credit at every size (`AllocationInputs::hit_scale`): the curve, lowered toward the measurement and
  never raised past what the replay justified; the linear credit, which is conservative on purpose,
  either way. A cache whose hits are worth less may then be sized differently, which is the ranking's
  legitimate answer to a changed price. A `decode-cost` decision, `[measured]`, states both figures
  against the predicted ones. With no record, or one that measured no decode, the plan is byte for
  byte the one before this existed.
- **Checked:** planner checks (a compute three times the model's adds twice the model's compute to
  the token and is reported as measured; another thread count or placement is not used; hits scale
  the linear credit and the curve, never above the replay; an empty record leaves the plan as it is);
  record round-trip checks in the `knob_trial` test; gate G24d, a streamed decode with no trial whose
  answer is the resident one and whose record carries the decode's cost.
- **Not done.** The read rate is not calibrated: the expert lane's price stays the storage probe's,
  so where the flash waits are what the plan got wrong this does not correct it. A load that runs the
  plan `bmoe-server` holds keeps that plan on `GET /api/plan` until the next measurement or load.
- **Status:** built, host-checked; the phone run that judges it is owed.

**The request mix.** The same record also keeps how the machine is used, on every load and whatever
the plan armed: per request, over the requests of the last load that served any, `mix_gen_tokens`
decoded and `mix_wide_prompt_tokens` prefilled (after reuse) in prompts at least as wide as the
narrowest graph a device is handed (`--prefill-min-tokens`), the part a prefill device would take;
`mix_requests` counts them, and `host_prefill_tps` is the host's rate over those prompts where the
host ran them. A later load that served requests replaces the mix whole (a front-end hands on a
load's record each time it grows, so its figures are already that load's means), and keeps the host
rate an earlier load measured where its own long prompts all ran on a device. They are later keys of
the same line: a record written before reads as nothing recorded. The plan prices one trade by them,
a prefill device whose room takes the expert cache away ([hardware-planning.md](hardware-planning.md),
the prefill plan); with nothing recorded it decides as it did before. Checked: record round-trip and
merge checks in the `knob_trial` test, planner checks for both outcomes and for the unpriced one,
and gate G24e (a load's record carries its one request).

## Decisions across loads, timed at load

Two decisions in Step 4's neighbourhood are fixed when the model or its context is created, so one
load cannot try both sides: the device tier (a cache of experts on the card, or the capacity fitter's
whole layers: `MeasuredKnobs::device_cache_tps`, `no_device_cache_tps`) and the graph width where
even a graph held to the device bound (`ubatch_max_device`) takes more of the card's memory than its
share, memory the experts would use (`width_narrow` and its rates,
[hardware-planning.md](hardware-planning.md#the-widest-graph)). Timed across loads, each side cost a
whole session of a person's use at whatever rate it decoded. On rented cards a model's second load
decoded at 37 tok/s instead of 68 on a 12 GB card, and at 35 instead of 84 on a 32 GB one.

**What it does.** Where a plan leaves such a side pending, each side is opened as a short candidate
load of its own and timed before the plan is served, and the plan on the grown record is the one
served (`bmoe/calibrate.h`):

- **The planner stays the oracle.** A side is pending exactly when the plan sets one
  (`KnobTrialConfig::device_tier` or `width_side`). `calibrate()` runs that candidate, puts its rate
  in the record with the mapping a session's own timing uses (`record_side_rate`, one decision per
  load: the tier where the plan names one, else the width), and asks the planner again
  (`replan`: `probe_and_plan`'s planning half on the same facts, the fitter's re-fits at a narrower
  width included, no probe). Every rule above holds as it is: the fitter's placement is still timed
  only where its ceiling could win, and a side wins only past `min_backend_win`. The tier is decided
  first, the width then on the winner, as the planner already orders them; a configuration already
  timed is not opened again (the cache side at the plan's width answers the width's wide side too).
  At most four candidates, every side of both decisions once; a plan that asks past that, or asks
  for something a short decode cannot measure (the prefill of long prompts a machine has been
  sending), is served as it stands and its load times the rest as before.
- **A candidate** is a `Session` of its own (`run_calibration_candidate`), opened, given one fixed
  generic instruction through the model's own chat template, and closed. Its decode is the ordinary
  one, every token through the same hook, streamer and caches. The first candidate samples with a
  fixed seed (greedy prose falls into loops that replay a few experts) and returns its token ids; a
  later one replays them teacher-forced, so both sides route the same tokens and differ only in where
  the experts run. Speculation is off: a draft's acceptance would time the drafter. Before each
  candidate opens, the system's cache of the model's files is evened out, so the later side does not
  load and decode from memory what the first read from the drive: the cache of every shard, the
  projector and an MTP head is handed back where the system takes the advice
  (`pio::drop_file_cache`), and the dense part, which every token reads whatever the placement, is
  then read in on every system alike. Each candidate so opens with exactly the dense part cached,
  where a drop is a no-op (Darwin, Windows) as where it is not. What stays uneven there is the experts
  a placement loads through the mapping, which the later side may find partly cached; they are not
  read in, since a model larger than the memory is what this engine exists for. A stop is honoured
  before each open and as soon as a load returns, as well as at every decoded token.
- **Bounded by measurement.** The windows are the knob trial's (`KnobTrialPolicy`), and the turn ends
  once the rate is known. A cache whose hits the session sees settles as the trial does; a device
  expert cache, filled inside llama.cpp, settles once two windows in a row agree within the margin
  (`RateProbe::settle_on_rate`), where the session's own timing waits the longest settle. The second
  side of a decision is cut once a timed window's fastest step is past the first side's range by the
  margin (`apart_bar`, the knob trial's own test for a candidate apart from its incumbent).
- **A side that cannot be opened loses, and is tried again only by a calibration.** A candidate, or a
  served load, that fails to load or to decode a side (out of device memory, most likely) adds one to
  that side's failed-open count in the record (`unviable=side:n`, scoped like every other measurement
  to this machine's fingerprint and the engine version). A side with any is the loser in every plan
  made on the record, so no served load opens what is known not to open. A failure may be the
  machine's state at that moment (another program holding the card's memory, a driver's hiccup), so
  a calibration offers the side again while its count is under `k_unviable_retries` (two: a second
  failure, on another calibration, is taken as the side not fitting this machine with this engine,
  and each try costs a load that fails), and only when the plan, with that count set aside, would ask
  for the side at all: a decision a person pinned, or a narrow width that has moved, asks for nothing,
  and the server does not unload a model for a calibration that would open nothing
  (`calibration_due`, the rules alone, no probe). A rate measured since clears the count; a record
  handed on beside the side's old rate does not; a calibration stopped before it reached the side
  leaves it. A request the session refuses (a context too short for the instruction) counts nothing.
- **A served load never opens a pending side not proven to open.** With calibration on, a plan whose
  pending side's other side has opened and decoded here before is served on that other side, and the
  pending one is opened only as a calibration's candidate. Where neither side is proven (the first
  load), or calibration is off, the load serves the plan's side and times it as before, and if that
  open fails the server opens the other side instead of ending with no model
  (`EngineHost::OpenFallback`, never for a load an unload cancelled). Only an open of the other side
  says the first failure was the side's: then, with calibration on, it counts against the side; where
  both fail, the first open's reason is the error and nothing is counted. With calibration off nothing
  would try the side again, so nothing is counted and those loads stay as they were. An "other side"
  that leaves a side of the other decision pending is no side known to open, and is not offered. A
  calibration stopped with a side still pending and no proven side to serve applies nothing: the load
  opens the settings it was serving, which opened here.
- **What is kept, and what is not.** The rates go in the model's `.knobs` record as a served load's
  do, so the next plan of the file on this machine starts decided. The candidates' output is thrown
  away, and they write no `decode_compute_s` or `decode_hit`: those are the served configuration's.
  The winner is then opened fresh, as a plain load, so what is served is byte for byte what the same
  plan would load without a calibration. With no request served yet, the width weighs the two decode
  rates alone (`usage_requests` 0). That verdict is provisional: a width decided so names the narrow
  width on its loads with no side to time (`KnobTrialConfig::width_narrow` alone), and they record the
  requests they serve; the plan after weighs those, asking for each side's prefill of long prompts if
  they show up, timed on a served load as before. Only that case records: a width decided on requests
  already seen (all a load timed across loads ever leaves) carries no width value, records nothing,
  and stays where it was decided, as it always has.

**Where it runs.** `bmoe-cli --auto` calibrates at load, before the first answer, and says so on one
line; with nowhere to keep the verdict (no `--routing-dir`) it does not, and says that instead, since
every run would pay the candidates again. `bmoe-server` never holds the first answer for it: the
model loads on the plan's own side and serves at once, the served load timing that side as before.
Once the server has been idle for `kCalibrationIdleDelay` (a minute, `http_server.cpp`) after an answer,
nothing running and nothing waiting, it unloads the model, times each side, keeps the record and
loads the decided plan, as one load (`EngineHost::load_when_idle`). The delay is the gap a
conversation has: a person reads the answer before typing, an agent runs its tools between turns,
and each new answer starts the wait again. Meanwhile `/api/info` reads `ready` with `calibrating`
and the model's name, and `plan_progress` events name each candidate (`step` `calibrate`): the
server is busy and still takes requests. A request that arrives stops the candidate in flight and
waits for that load, as it waits for any load the server runs, never refused; what was measured
until then is kept. A load a person asks for meanwhile supersedes it, and an unload or a shutdown
ends it: what was measured is kept, and nothing is opened after it. The decided plan is applied only
if no setting has moved since the calibration began and loads still plan (`apply_plan_if`, the check
and the write under the settings' one lock), and the load opens the settings the plan was made for;
another model chosen, auto planning turned off or a setting changed meanwhile leaves the settings as
the person left them. It does not start at all with auto planning off. One attempt per load of a
plan, tried again at the next idle point when a turn slipped in as it started. `--no-calibrate` (the `calibrate` parameter) turns it off, and the
decisions are timed across loads exactly as before.

**Where it does not.** Both decisions exist only beside a device with memory of its own: the tier
needs one (`device_tier_device`), and the width's trade is declined on a pool shared with the host
(`devices_share_host_memory`). A phone, a unified-memory Mac and a machine without a graphics card
never have a side pending, so they never open a candidate; with nothing pending `calibrate()` opens
nothing and the plan is the one it always was. The Android and iOS apps run their server on the phone, so they never see
`calibrating` and show no label for it; the web UI does.

**Judged by.** The `calibrate` test runs the loop on fake ports: it ends, runs the sides in the
planner's order, opens nothing the planner does not ask for, treats a candidate that cannot open as
the losing side, replays the first run's tokens against the other side's range, and stops for an
aborted run. `planner_test` runs it on the real rules for both decisions and the ceiling's skip.
Gates G24f (the width, on the host) and G25f (the tier, on the loopback device) run the engine's own
candidates: no side left pending, both rates in the record, and the winner, opened fresh, equal to
the resident answer; G24g that a load with the width's trade and no side timed records its requests.
`engine_host_test` drives the server's idle load with a scripted session, and `server_app_test` the
whole server over HTTP with the engine and the planner scripted: the calibration starts once idle;
a request during it, with no `--autoload`, stops it and is answered; a cancel, an unload, another
model chosen, a load asked for and a final open that fails each end it as above; a served load whose
side will not open opens the other side and counts the failure; and a side measured before is served
instead of a pending one not proven.

Owed: a run on a card of each class, the second load's decode against the first's, with the
candidates' wall time, and the server's idle path against a client that sends turns back to back.

### A prefill device that did not hold up for a model

The plan arms a prefill device on a probe graph and a matmul of the model's own format; the session
then checks it on the model itself (`PrefillDeviceConfig::verify`, a short prefill on the host and
on the device, compared row by row) and hands it back where the two disagree. Measured on a 12 GB
phone: an NPU that passed the probe picked the host's next token in only 25 of 31 positions on
Gemma 4 26B-A4B, and, with that backend's host buffer on, never returned from the model's first
layer, so the load never finished; on Qwen3.6 the same NPU agreed in every position and prefilled
1906 tokens at 139 tok/s against the cores' 12. The cause is the backend's: its expert matmul is exact
until a value passes the fp16 range and returns infinities past it, and Gemma's activations are known
to reach that far in a few channels (ggml-org/llama.cpp#32198). The check's line gives the KL divergence
of the device's next-token distributions from the host's beside the agreement: 0.004 mean for Qwen3.6,
rounding, against 0.60 for Gemma with one row at 13.5. So what came of the device is kept in the model's
record, scoped like every other measurement to this machine and engine (`prefill=<device>:<word>`):

- `kept`: the session opened with it. The next plan arms it as before.
- `refused`: it would not open, or its check on this model refused it. No plan arms it again for the
  model; another device that measures faster than the host still can.
- `unconfirmed` (`prefill_unconfirmed=<n>`): a front-end writes it before opening a session that arms
  the device, and the session's word replaces it once open, so only a load that never got that far
  leaves it. One may be a person closing the app mid-load, so the device is tried again; after
  `MeasuredKnobs::prefill_unconfirmed_max` (two) in a row it is kept out like a refused one.

The server and the CLI write the word as soon as the session has opened, not after the first
answer. A new engine version starts a new record, and tries the device again. Tested by
`knob_trial_test` (the record) and `planner_test` (a device not kept is not armed; one kept is; a word
about another device leaves this one alone); on a 16 GB Apple-silicon laptop, two loads killed while
opening left `MTL0:unconfirmed` 1 then 2, the plan armed Metal after the first and not after the
second, and a load that finished wrote `MTL0:kept`.

## What the planner looks like afterwards

- **Memory:** the ledger, unchanged, still the only thing that decides what is held.
- **Speed:** facts measured on the model, the routing trace for the cache, the first request for the
  lossless knobs, one real layer for devices. Each enters the existing ranking in its own unit.
- **Tiers:** flash, RAM and device memory, each a cache for the one above it, the experts used most
  next to the fastest processor.
- **Removed:** the linear hit credit and its coefficient, the synthetic thread sweep, the synthetic
  bandwidth GEMV and the separate prefill matmul probe (gone, Step 3, except for a file whose layer
  cannot be read), and the unpriced crossing term that kept devices out of decode (gone, Step 3).
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
