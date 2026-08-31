# Hardware planning (`--auto`)

The engine exposes a lot of knobs, and almost every one of them has a right answer that depends on
the machine rather than on taste. `--auto` derives them instead of asking, and prints the fact
behind each choice so a run can still be explained afterwards.

```
bmoe-cli -m model.gguf --auto                   # measures, plans, runs. Nothing else to do
bmoe-cli -m model.gguf --auto --plan-explain    # ...and print why it chose what it chose
bmoe-cli -m model.gguf --plan-only              # print the plan and exit, without loading the model
bmoe-cli -m model.gguf --auto --no-probe-io     # plan from free facts only, touching no storage
bmoe-cli -m model.gguf --auto --probe-mem      # ...and measure what this machine will let us KEEP
```

It is opt-in. Without it nothing changes.

## What runs where, so the rest of this document is not misread

A machine with an accelerator uses it. The first stage is llama.cpp's own capacity fitter, and
whatever it places on a device is placed, computed there, and none of the rules below touch it: the
plan carries its `n_gpu_layers` and its override patterns straight into the load. On a box with
24 GB of VRAM and a 150 GB model, the attention, the dense set and the experts of every layer that
fits are on the GPU.

What this planner owns is the residue - the expert tensors the fitter left on the host because
there was no room - and those are the ones it streams from flash. **Those** cannot be computed on a
discrete GPU, and the `extra-offload` line in every plan is about them and only them, never about
the fitter's placement. The reason is in "Why the obvious route is closed" below, and it is not a
choice: a streamed expert exists by having its `data` pointer rebound onto memory this engine
reserved, and a discrete device cannot read that memory through anything the public API offers
today. Upstream is stuck on the same step - its own expert-streaming PR has been open since July,
and the neighbouring issue says the fix needs a change inside `ggml_backend_sched_compute_splits`,
which is a fork.

## Two stages, one composer

Placing tensors across compute devices is a capacity problem, exactly solvable from tensor sizes,
and llama.cpp already solves it: `common_fit_params` loads the model with `no_alloc`, measures the
projected memory per device, and if it does not fit first shrinks the context, then moves weights
from device memory into system memory — per class inside each layer, attention first and the
sparse expert tensors last. It sees every backend through ggml. The comment above it reads
*"assumes system memory is unlimited"*, and that line is where this planner begins.

So `--auto` runs in two stages. The **first** is the fitter, called behind one adapter
(`core/src/plan/placement_probe.cpp`, the only file that touches llama.cpp's `common/`), which
answers: which layers keep their experts on the host, and what the placed model reserves there for
context and compute. The **second** is this planner, sizing the flash tier on what is left. The
session then loads with the fitter's `n_gpu_layers` and override patterns rather than a hard-coded
zero.

Three of the fitter's answers are read rather than trusted. Left at 0 it picks the model's full
training context, so our context is always the pin. On a machine with no GPU it leaves
`n_gpu_layers` at its default, which means "all", so devices are counted instead. And its host
`model` term is neither the file nor the dense set, so dense bytes come from the gguf profile and
only the context and compute reservations come from the fitter — the one thing it knows that the
profile does not, and 605 MiB the earlier budget had overcommitted on the desktop.

Two outcomes of the first stage end the plan early, and both are stated: if the host residual
fits in RAM the experts stay resident and nothing streams (the classic `-ot exps=CPU` offload);
if every layer's experts land on a device, nothing is left for the streamer.

## The two invariants

**Decline instead of guessing.** Every probed fact is a tri-state, and `Unknown` is an input rather
than a defect: a knob whose deciding fact is missing keeps its default and is reported as
`[unprobed]`. A plan full of `unprobed` lines is a list of the measurements the project still owes
itself, which is worth more than a plan that invented values for them.

**Nothing lossy arms itself.** Expert dropping, substitution and route-ahead change the output and
make a run irreproducible. They are never turned on by a plan, at any quality budget, on any
machine. A caller that arms one keeps it, attributed to `[operator]`.

A third rule falls out of the first two: **a knob you passed by hand is never overwritten.** Flags
and `BMOE_*` env overrides both count as the caller speaking.

## The flow

1. **Probe** the model from its gguf metadata (nothing loaded, no tensor data read) and the machine
   from what it reports for free.
2. **Classify the regime** — a ratio of the two, never a property of either. The whole model inside
   the residency budget is `fits`; only the dense set is `experts-stream`; not even that is
   `dense-oversized`.
3. `fits` **declines streaming and says so**: on that machine reading experts from flash only adds
   reads that resident weights do not need.
4. **Dense policy**, from what a reclaim costs here.
5. **Cache budget** = usable memory, less what the dense policy is about to make non-reclaimable,
   capped at the experts' own size — and floored at the model's token cycle.
6. **I/O policy** from the storage facts.
7. **Emit the plan with its rationale.** A decline is a plan too: the config it carries still runs.

## The facts, and which knob each one decides

| fact | decides |
|---|---|
| residency budget: how much this process may hold | the regime, and the cache budget |
| headroom: how much of it can be KEPT | the same two, when it has been measured rather than reported |
| core classes, fastest first | `--threads`: a barrier waits for its slowest participant |
| whether a device executes over a host buffer | whether streamed experts could be computed on it |
| host memory bandwidth, and a device's | whether moving that compute is worth anything |
| what happens to anonymous memory under pressure | the dense policy, and the margin |
| whether a reclaim-exempt allocation exists | whether the dense set can be pinned |
| whether mapped file pages count against the fatal limit | whether leaving the dense set mapped is free |
| whether uncached reads work on this path | `--no-odirect` |
| whether a live mapping serialises concurrent reads | `--release-mmap` |
| read rate by request size and lane count | `--io-threads` |
| model: expert slice size | which point of that rate curve applies |
| model: token cycle | the cache floor, below which streaming is declined |

There are no platform names in the rules — Windows, Android, iOS, CUDA and Metal appear only in
`core/src/plan/hardware_probe.cpp`, the adapter that fills the profile. That is what lets the rules
be unit-tested against machines nobody here owns (`tests/planner_test.cpp`), and it is why the same
rule that picks anonymous buffers on a desktop picks a pinned allocation on a phone and leaves the
weights mapped on a platform that kills a process for holding too much: one rule, three facts,
three answers, no branch.

## The token cycle

The cache floor is the model's own arithmetic: for every MoE layer, one expert's bytes across that
layer's expert tensors, times the routing width. Under it the cache evicts what the same token
still needs, so it costs its memory and returns no hits at all — the cliff `docs/cache-sizing.md`
documents.

It is summed over the tensors the file carries rather than multiplied out, because the factors are
not uniform: a fused `gate_up` projection is twice the stride of a split one, an architecture with
leading dense blocks has layers that demand nothing, and a trailing multi-token-prediction block
names expert tensors that llama.cpp never loads. On Qwen3.6-35B-A3B-Q4_K_M this derivation returns
581 MiB against the 582 MiB the engine computes at load from the tensors it actually bound — an
independent check that the metadata-only path sees the same model.

Because the floor is per-model, it also overrides the generic `cache_min_mb` guard: a budget above
this model's cycle is not pathological however small it looks, and the plan says so when it forces
past it.

## The headroom probe

`MemAvailable` and its equivalents answer "how much could be allocated right now". Every sizing
rule here is asking something else: how much can be held and *kept*. On a machine whose reclaim
compresses, the two differ in both directions at once. The reported figure is a floor, because the
kernel will compress other processes' idle pages to make room — the test phone held 3.8 GB of
pinned dense set plus cache while reporting 3.6 GB available, and reported 5.6 GB afterwards. And
it is an over-promise, because that same cheap reclaim takes our pages back just as readily, which
is why a model that merely fits is not a model that survives.

Two answers, and the caller chooses what to spend. The **estimate** is free: what the machine's own
compressor is currently achieving, applied to the set the kernel would compress first. Both numbers
are read from this machine's accounting; neither is a constant, and where the accounting is not
readable — an unprivileged process may not read the compressor's statistics on some systems — the
fact stays unknown and every rule falls back to the reported budget and says which one it used. The
**measurement** is `--probe-mem`: hold memory in steps and watch for the moment the kernel takes the
first of it back. It is the real answer and it is intrusive by nature, so it never runs unasked; it
grows in steps and stops at the first sign of loss, so it usually never reaches its ceiling, and it
releases everything on every path out.

## The storage probe

**Nothing to run beforehand.** The probe is part of `--auto`, happens once inside the load, and
asks the caller nothing. It costs about a second of a load already measured in seconds - 6.3 to 6.9
on the desktop it was validated on - against a lane count worth 12% of every token afterwards.
`--no-probe-io` opts out for a caller that must not touch the drive at all.

It measures the rate curve around the model's own expert slice, at one, two and four lanes, taking
the median of three samples at the size that decides anything, and picks the **largest** lane count
that reaches within 5% of the best rate.

Largest, not smallest, and that is a correction the machine forced. The rule used to take the
cheapest count inside the tolerance, on the reasoning that where two lane counts deliver the same
throughput the cheaper one is strictly better: fewer threads, less queueing, less contention with
the compute the reads are meant to overlap. Sound, and wrong. On the desktop SSD here the probe
cannot separate two lanes from four - repeated runs pick either, medians and all - while the engine
is not ambiguous at all: three interleaved 64-token runs give **4.648 tok/s at two lanes against
5.261 at four**, +13%, with no overlap between the groups.

So a lane buys something this probe does not measure. What it reads is aggregate throughput; what a
streamed decode also spends is **latency**, waiting for the slice the next expert needs, and a queue
that drains sooner ends the stall sooner even when the bytes per second come out the same. Until
that is measured directly, the tie-break follows the evidence rather than the principle: inside the
probe's own resolution, more lanes. With that, the pick is stable across runs.

### What it will not tell you

The same probe tries to answer whether a live mapping of the model serialises concurrent uncached
reads, by reading with a mapping alive and again after releasing it. **It reports `Yes` or nothing,
never `No`**, and that is a deliberate limitation rather than an oversight.

On the desktop where the engine gains 24% of decode from `--release-mmap` — its own read rate goes
from 871 to 1680 MiB/s — this probe sees the two arms within 2% of each other. Whatever it is
failing to reproduce (most likely the access pattern: it reads uniformly at random where the engine
walks expert slices layer by layer against a warm cache), its fidelity is established in the
positive direction only. A negative from an instrument that missed a known positive is not evidence
of absence, and printing one as `measured` would be exactly the confident wrong answer this design
exists to avoid. So the plan prints the two rates, says the probe saw nothing, and tells you to
measure `--release-mmap` yourself.

## A rule that a machine took back

**Threads from core classes: proposed, shipped, refuted, withdrawn — in a day.** The rule set the
count to the fast class, because every thread meets the same barrier and one on a slower core sets
the pace rather than adding to it. The first heterogeneous machine it met disagreed: a phone
reporting two prime cores and six others got two threads instead of four, decode compute went
0.127 → 0.195 s/token and throughput 4.24 → 2.68 tok/s. Half the threads cost more than the
imbalance saved, and nothing in this planner knows where that trade turns over.

What survives is the fact. The classes are measured, they are printed, and the knob keeps its
default — because a count derived from them would need a thread sweep on the machine itself, which
is a probe this does not have. The same reasoning retired the prefill count at the same time: it is
*plausibly* helped by every core there is, and plausibly is precisely what does not ship here.

It is worth writing down that this is the second principle in two days to be overturned by a
measurement rather than by an argument — the lane tie-break was the first. Both were sound. Neither
was true.

## Where a rule is still cruder than the fact it stands in for

**The "fits" exit asks for air, and the ratio is still policy.** The case is the phone, and it is
the common one rather than the exotic one: an 8B-class MoE that fits in 12 GB *just barely*. It
fits on paper. In practice the system, the app and the kernel's own page cache sit on top of it,
and what the last few hundred MiB go to is decided by whoever touched memory last — so a resident
model is reclaimed from underneath every few tokens and refaults its dense set from flash a page at
a time. Streamed, the same model holds a pinned dense set and a cache that *chooses* what to keep,
and decodes faster than the "fully resident" version that keeps losing itself. The measured case is
stark: the 35B fully resident through mmap decoded at 0.1 tok/s where streaming it decoded at 5.0.
Fitting is not the same as being left alone.

So the exit now asks a second question. Where a reclaim is **cheap for the kernel** — it compresses
the page, or drops a clean one — residency must leave room beyond the model itself, `fits_air_ratio`
of what it would hold, or the experts stream and the plan says why. Where a reclaim has to write to
a disk the kernel is far more reluctant and the classic host offload stands; where the allocation is
exempt nothing can take it; where the limit is a hard per-process cap nothing is taken at all, and
the margin for that case is already the largest of the three. An unprofiled machine keeps the old
behaviour, like every other missing fact: the evidence is specific to cheap reclaim and does not
entitle a rule to generalise past it.

That ratio is policy, not measurement, and it is deliberately crude because the error it guards is
asymmetric by two orders of magnitude — streaming a model that would have fitted costs some reads,
residency on one that does not costs fifty times the throughput. What narrows it is the headroom
probe above, which the air test already reads through the budget: where the headroom is measured,
the air being counted is real spare capacity rather than an accounting figure, and only the
multiplier is still policy. The same measurement is what stops the budget being read as a floor
on a compressing machine, which is why the two were one piece of work rather than two.

## What is not probed yet

- **A machine with a device to try it on.** The bandwidth probe runs on every backend that
  registers, and the routing that acts on its answer is in the session. Neither has met an
  accelerator: this machine registers only the CPU, so the probe measures the host's figure, the
  rule finds nothing to compare it against, and the plan says so. The mechanism is complete and
  unverified, which is a different thing from missing, and the plan distinguishes them.
- **`--ubatch`, and this one has been tried.** The compute-buffer reservation trades against the
  expert cache: a narrower ubatch reserves less and leaves more cache, at the cost of chunking
  prefill. Six interleaved decode runs on the desktop, 512 / 256 / 128, gave 4.016 and 4.893 tok/s
  at 512, 4.907 and 3.370 at 256, 4.638 and 3.604 at 128. **The spread within one setting is larger
  than the difference between settings** — 256 alone ranges from 3.37 to 4.91 — so the first
  repetition and the second disagree about the winner. Short runs that each reload the model leave
  the page cache in a different state every time, and 48 tokens is not long enough to average that
  out. The knob keeps its default and prints `[unprobed]`, now because the measurement was made and
  did not resolve, which is a different thing from never having looked.

The residency budget is `MemAvailable` on every platform that reports it, and on a machine whose
reclaim *compresses* that is a floor rather than a cap: the phone held 3.8 GB of pinned dense set
plus cache with 3.6 GB "available", because the kernel compressed other processes' idle memory to
make room, and reported 5.6 GB available afterwards. The honest budget there is the compressible
headroom, which is a measurement this planner does not yet take; until it does, an unpinned plan
on a phone under pressure declines and says why, and a pinned `--cache-mb` wins over the floor.

One caveat on the budget itself: it is a one-shot reading of a quantity that moves. The same
machine and model planned twice minutes apart produced 4.3 GB and 8.2 GB of available memory, and
therefore two different cache budgets. The plan quotes the number it used, which is what makes the
difference visible rather than mysterious.

Compute devices are enumerated through `ggml_backend_dev_*` and recorded, but no rule reads them
yet; a device whose memory is host memory frees nothing when a tensor moves off it, which is why
the capacity tier is nearly inert on an integrated GPU and only the bandwidth tier is left.

## Architecture

The logical flow, as implemented. Diamonds are decisions; the two bracketed boxes in the session
are designed and not yet active.

```
 INPUT
   model.gguf              machine                  caller
   (never loaded)          (as it is right now)     (flags typed by hand = PINS)
        |                      |                         |
        v                      v                         |
 +--------------+   +--------------------+               |
 | MODEL PROBE  |   | MACHINE PROBE      |               |
 | experts/layer|   | available memory   |               |
 | slice, token |   | overflow: compress |               |
 | cycle, dense,|   |  / swap / kill     |               |
 | MTP, tied    |   | reclaim-exempt     |               |
 | head         |   | core classes       |               |
 |              |   | host bandwidth     |               |
 |              |   | devices: host buf? |               |
 |              |   |  runs our layout?  |               |
 +------+-------+   +---------+----------+               |
        |                     |                          |
        |           +---------v----------+               |
        |           | HEADROOM PROBE     |               |
        |           | free: compressor   |               |
        |           |  ratio x reclaimable                |
        |           | --probe-mem: hold  |               |
        |           |  until pages go    |               |
        |           +---------+----------+               |
        |                     |                          |
        |           +---------v----------+               |
        |           | BANDWIDTH PROBE    |               |
        |           | one GEMV, every     |               |
        |           |  backend: host vs   |               |
        |           |  device, same units |               |
        |           +---------+----------+               |
        |                     |                          |
        |           +---------v----------+               |
        |           | STORAGE PROBE ~0.6s|               |
        |           | rate(size, lanes)  |               |
        |           | O_DIRECT ok?       |               |
        |           | mapping serialises?|  <- Yes or Unknown, never No
        |           +---------+----------+               |
        v                     v                          v
 ==================================================================
 | STAGE 1 . llama.cpp CAPACITY FITTER        (VRAM -> RAM)       |
 | per layer: attention -> up -> gate -> sparse experts last      |
 | on every backend ggml sees                                     |
 | in:  context pinned by us (never 0 -> "model max")             |
 | out: n_gpu_layers . split . overrides . KV+compute reservation |
 ================================+=================================
                                | Placement:
                                |  which layers keep experts on the HOST
                                |  what stays resident on the host
                                v
        <> everything on devices?  -- yes --> done: nothing to stream
        | no
        v
        <> host residual FITS in RAM? -- yes --> experts resident,
        | no                                      no streaming
        v                                         (= -ot exps=CPU)
 ==================================================================
 | STAGE 2 . PURE PLANNER                     (RAM -> flash)      |
 | 0 llama.cpp symbols . 0 platform names                         |
 |                                                                |
 |  DENSE policy  <-- overflow + reclaim-exempt store + who bills |
 |      compress & dma-buf         -> pinned (ahwb)               |
 |      kill & file pages uncounted -> mmap                       |
 |      otherwise                  -> anon                        |
 |                                                                |
 |  CACHE budget = usable - host dense - fitter reservations      |
 |      cap: only the expert share left on the host               |
 |      <> >= token cycle?  no -> DECLINE and say why             |
 |                            (caller pin -> proceed + warning)    |
 |                                                                |
 |  I/O   <-- curve: smallest lane count within 5% of peak        |
 |        <-- O_DIRECT verified on the path                       |
 |        <-- mapping release: only if measured Yes AND the shape |
 |            is safe (head not tied, MTP not in use)             |
 |                                                                |
 |  LOSSY = off. Always. Unless the caller armed it.              |
 ================================+=================================
                                | Plan = RunConfig + rationale
                                | every line: [measured|derived|policy|operator|unprobed]
                                v
 +----------------------------------------------------------------+
 | SESSION                                                        |
 | loads with the fitter's n_gpu_layers + overrides               |
 | dense -> anon | dma-buf | mmap                                 |
 | host experts -> streamer: LRU cache, lanes, mul_mat_id overlap |
 | experts -> planned device's HOST buffer type at LOAD          |
 | (but the streamer then rebinds onto its own reservation)      |
 | [NOT ACTIVE] prefill on the device over streamed experts       |
 +--------------------------------+-------------------------------+
                                  v
 +----------------------------------------------------------------+
 | llama.cpp + ggml (stock submodule)                             |
 | CPU . CUDA . Metal . Vulkan . ROCm . SYCL . OpenCL             |
 +----------------------------------------------------------------+

 OUTPUT: tok/s + the printed plan (--plan-explain) + the plan in the CSV header
```

Three things to read off it. One knowledge boundary: above the probes there is no platform name;
below them, public API plus `common/fit.h` in one file. Two stages in sequence, not merged: the
fitter answers *how much fits where* (exact, computed), the planner answers *how to read what does
not fit* (measured, or declared unknown). Four early exits, and every one of them is a plan: a
dense model, everything on devices, a host residual that fits, a budget under the token cycle.
None is an error; each says why.

## Levers llama.cpp already has that this planner does not use yet

Each row is a gap here paired with the public facility that closes it. None needs a fork.

| gap | facility | what it would allow |
|---|---|---|
| streamed experts computed on a device rather than on the CPU cores | **not** a buffer-type override — that route is closed, see below. The open one is `ggml_backend_dev_buffer_from_host_ptr` + `caps.buffer_from_host_ptr` | on a backend that can wrap memory the caller already owns, the streamer's reservations could be handed over and the device could execute out of them. Metal, CPU and BLAS implement it; CUDA, SYCL, Vulkan and OpenCL do not |
| prefill on the device over streamed experts (designed, not built) | `cparams.op_offload`, `offload_kqv`, the scheduler's batch threshold (`ggml_backend_dev_offload_op`) | per-op copy of host weights to the device above a batch size: the "batch amortises bytes over link bandwidth" rule |
| ~~no thread rule~~ — **in**, from core classes rather than a core count | `n_threads_batch` distinct from `n_threads`, `llama_numa_init` | still open: different counts for decode and prefill, and NUMA on workstations |
| KV and flash attention undecided | `flash_attn_type` (auto), `type_k` / `type_v`, `kv_unified`, `swa_full` | KV memory as a budget line instead of an ignored one |
| device selection | `mparams.devices[]`, `split_mode`, `main_gpu` | tell the fitter which devices to use, e.g. exclude one that demands a repack |
| ~~a unified-memory device looks discrete~~ — **no longer decides anything** | the bandwidth probe, not the `integrated` flag | what the flag would have told us cheaply is now measured directly, and measured beats reported: the rule compares the device's figure to the host's on the same graph |
| ~~the budget under a compressing reclaim~~ — **in** (`--probe-mem`, and a free estimate always) | nothing in llama.cpp: a kernel fact | the headroom probe stays ours, and is the one measurement no other engine takes |

### DGX Spark, as a worked case

128 GB of unified LPDDR5x at roughly 273 GB/s, twenty ARM cores, a Blackwell GPU, models
streamed from NVMe: exactly the `experts-stream` regime, and the machine upstream's own expert
streaming PR benchmarked on. This planner used to misread it twice over: ggml reports the GB10 as a GPU
with memory of its own, because the CUDA backend's `integrated` flag is disabled, and the profile
called a weight rebindable only where the memory was the host's — so every expert matmul went to the
ARM cores with the GPU idle. The reading is fixed: rebindability is asked of the device, and the
`integrated` flag is not needed for anything any more, because what it would have hinted at is now
measured directly — the same GEMV on every backend, and the rule compares the rates.

**But the route everyone reaches for first is closed, and the evidence is in the pinned submodule
rather than inferred.** Binding the expert overrides to a device's *host buffer type*:

- `src/llama-model-loader.cpp` carries the comment *"avoid using a host buffer when using mmap"* and
  substitutes the CPU's buffer type for any device host buffer whenever the model is mapped. This
  engine always maps it — load-bearing, not a setting — so such an override is undone at load and
  nothing downstream ever sees it.
- `ggml/src/ggml-cuda/ggml-cuda.cu`'s `supports_buft` accepts the CUDA host buffer **only on an
  `integrated` device**. On a discrete one no device claims the buffer and the op lands on the CPU.
  With `integrated` disabled upstream, a unified-memory GB10 answers as discrete: the machine where
  this would pay is the machine that refuses it.

And a third reason that is ours alone: the streamer rebinds `data` onto memory it reserved itself,
so the bytes a device would read were never allocated by it or registered with it.

**The route that is actually open** is `ggml_backend_dev_buffer_from_host_ptr`, which wraps memory
the caller already owns in a buffer of the device's own; `caps.buffer_from_host_ptr` says who
implements it — CPU, BLAS and **Metal** do, CUDA, SYCL, Vulkan and OpenCL do not — and llama.cpp
uses it itself to hand Metal the mapped model region. For us it would mean the streamer's per-layer
reservations being wrapped that way, which trades against the lazy commit they exist for: a
wrapped or page-locked range needs its pages present, and a full-size reservation with pages
committed on a miss and released on eviction is exactly what lets a 150 GB model have valid
addresses everywhere while holding two. CUDA's nearest equivalent, `cudaHostRegister` via
`ggml_backend_cuda_register_host_buffer`, is behind an environment variable upstream and accelerates
transfers rather than moving where an op runs.

So the axis, stated honestly: **on Apple-style unified memory a path exists and is unbuilt; on
discrete CUDA there is no path through today's public API; on a unified CUDA device the path exists
in principle and upstream reports the device in a way that refuses it.** The planner says as much,
names the device that would qualify, and arms nothing.

The measurement to beat is public: on the same GB10, llama.cpp's own expert-streaming PR reports
0.87 tok/s of decode with the experts on the CPU against 2.20 with them on the GPU, and 1.06
against 5.69 in prefill. The counter-argument — that shared memory means both are bound by the same
DRAM, so the move buys nothing — has no measurement behind it anywhere, and the Apple evidence runs
the other way: token generation there peaks at about six CPU threads and gets slower with more,
which is what saturating below the fabric's limit looks like.
