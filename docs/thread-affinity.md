# Thread affinity

The engine chooses how many compute threads to run and nothing else: where those threads land is
the kernel's decision. This note is about whether that decision is worth taking away from it.

Nothing here is implemented. It is a design note for an axis that is **open and unmeasured**, and
it exists so the experiment is not re-derived from scratch a third time. It records what is already
closed, which distinct hypotheses hide behind the single word "affinity", why a CPU mask cannot be a
shipped constant, and the order the cells should run in.

## Why the axis is worth a note

Affinity can only ever move **compute**. That is the whole of its reach, so the first question is
how large compute is.

On the host reference run (a 35B-A3B MoE at Q4_K_M, streaming, with overlap, an auto-sized cache and
cold experts dropped) the decode budget decomposes as:

    0.192 s/token  =  compute 0.100  +  management 0.012  +  stall 0.080

Compute is about half the wall there, so the slice affinity is allowed to touch is the biggest one
in the budget. That is the opposite of the situation on the flash axis, where
[roadmap.md](roadmap.md) has spent most of its measured verdicts.

The same decomposition **on the device is owed, not measured**. The shipping configuration is
expected to be more compute-bound still, since the phone's storage serves the same bytes faster
relative to its cores, but that is an expectation, and the cells below should produce the device
breakdown rather than assume it.

All of which is an argument for measuring the axis, not a prediction that it pays.

## What is already closed: affinity on the I/O side

Pinning the **read** workers is dead, and it was measured rather than argued. Swept with
`tools/bmoe-iobench` on the reference phone (2026-08-14, O_DIRECT), moving the issuing core across
all four core classes changed the 8 KiB rate only between 74.2 and 76.6 MiB/s, and the 64 KiB rate
between 392.0 and 398.6, so under 2%. Four lanes pinned to four different cores measured
250.1 MiB/s against 255.3 unpinned. Both are noise, and both contradict the published storage
characterisation that motivated running the cell in the first place.

This note is about a **different subsystem**: the compute threadpool inside ggml. The two share a
word and nothing else. One is about which core submits a storage request, the other about which core
executes a matmul chunk before a barrier. The null result above says nothing about this one, in
either direction. It is recorded here only so the two are never quoted for each other.

## Three hypotheses, not one

"Affinity" is doing too much work as a name. There are three mechanisms underneath, they predict
different signs for the same intervention, and they are distinguishable by measurement.

**H1, barrier skew.** Every ggml node ends in a barrier, so a node costs what its slowest thread
costs. If threads are placed freely across an asymmetric CPU, the thread that lands on a slow core
sets the pace for all of them, on every node, all decode long. Pinning to a homogeneous set removes
the skew. This is the classical form of the argument and the one the earlier scouting assumed.

**H2, migration deflates the frequency.** Utilisation signals are per-core. A thread that migrates
leaves behind utilisation that decays, and arrives on a core whose utilisation is low, so the
governor asks for a lower operating point than the work deserves. Pinning would raise the clock
**without touching skew at all**: the same intervention, an entirely different mechanism, showing up
as a frequency difference rather than as a distribution across threads.

**H3, the co-tenant.** This engine is not a pure compute workload. Alongside the compute threads
there is a pool of workers doing positioned O_DIRECT reads, plus the streamer, and under `--overlap`
they must run *while* compute runs. If the compute threads occupy and spin on every core, the cost
does not appear in `compute`. It appears in `stall`, because the reads that were supposed to be
hidden are the ones that got descheduled. Under H3 the correct intervention is to **reserve** a
core, and a mask that claims all the fast cores makes things worse.

H1 and H2 both push toward taking the fast cores. H3 pushes the other way. Any cell that reports a
single tok/s number, without saying which of `compute` and `stall` moved, cannot tell them apart.

## Why a CPU mask cannot be a shipped constant

The mechanism (`sched_setaffinity`) is portable across every Android device. The *contents* of a
mask are portable across none, and this is the part that decides what can ship.

- **cgroup cpusets.** The app runs inside a scheduling group whose cpuset the vendor controls, and
  which changes as the app moves between foreground and background. Our mask is intersected with
  that set, so asking for cores the group does not grant either fails or silently under-delivers.
- **Vendor userspace re-affinitises.** The same platform layers that move `scaling_max_freq` under a
  running process also move threads. A mask is a request, not a guarantee.
- **Priority is mostly unavailable.** The threadpool parameters include a priority field, but an
  ordinary app does not get real-time scheduling on Android. Treat that field as inert, and evaluate
  it separately from the mask rather than letting it ride along in the same cell.
- **Topology varies.** 4+4, 1+3+4, 2+6, three clusters, recent parts with no low-power cluster at
  all. A hex constant is meaningless one device over, and hardcoding one would violate the project's
  rule that device and model specifics are discovered at runtime, never written into the path.
- **The ranking is not even stable within a run.** Per-cluster frequency caps move under sustained
  load on at least one platform we test on, so a mask computed at startup from "which cores are
  fastest" can be describing a machine that no longer exists ninety seconds into a decode.

What *is* discoverable at runtime, portably, from standard sysfs and syscalls:

| source | what it gives |
| --- | --- |
| `sched_getaffinity` at startup | the set the cgroup actually grants, which is the correct universe to choose from rather than the full core count |
| `/sys/devices/system/cpu/cpu*/cpu_capacity` | normalised per-core capacity on EAS kernels: how fast this core is, without knowing the part |
| `/sys/devices/system/cpu/cpufreq/policy*/related_cpus` | cluster membership, without inferring it from core numbering |

So the shippable unit is a **policy**, resolved on the device, not a mask. And of the plausible
policies, the frequency-invariant one (reserve a core) is structurally safer than the
frequency-derived one (take the N fastest), precisely because of the last bullet above.

## Prior art

This is a well-trodden axis everywhere except in the one detail that is ours.

**llama.cpp already exposes it.** `--cpu-mask`, `--cpu-range`, `--cpu-strict`, `--poll` and `--prio`
are wired to the public `ggml_threadpool_params` and `llama_attach_threadpool` API, so nothing needs
to be added to the submodule to try it. That matters, because this project does not fork llama.cpp
([seam.md](seam.md)). Upstream's own Snapdragon benchmark script pins deliberately:

    --poll 1000 -t 6 --cpu-mask 0xfc --cpu-strict 1

That mask excludes the two lowest cores. It is not evidence of a gain on our workload, but it is a
default chosen by people who benchmark this class of SoC continuously.

**On desktop hybrid CPUs the effect is large and documented.** Restricting generation to performance
cores has been reported at roughly 3x against free placement
([llama.cpp discussion #572](https://github.com/ggerganov/llama.cpp/discussions/572)). Different
scale, same mechanism: the slow participant sets the pace.

**Mobile inference engines have shipped this for years, and they answer the portability question the
same way this note does.** ncnn exposes `set_cpu_powersave(0|1|2)`: all cores, low-power cluster
only, performance cluster only. An enum of three policies rather than a bitmask, deployed across the
whole Android fleet. It also warns that switching is expensive and not thread-safe, so the choice is
made once. MNN binds cores in its CPU backend under a power mode.

**The runtime-adaptive version is published and measured.** MNN-AECS
([arXiv 2506.19884](https://arxiv.org/abs/2506.19884)) selects cores adaptively at runtime for the
decode phase, without root, and reports 23% lower energy than MNN's own baseline with no slowdown,
across five Android devices, two iOS devices and five models. Two cautions before importing anything
from it. Its objective is **energy**, so its policy deliberately idles clusters to drop frequency, a
direction that can be the opposite of ours. And its headline 12-363% speedup is a comparison against
*other engines*, not a measurement of what core selection alone buys. Related profiling work
([arXiv 2607.05475](https://arxiv.org/abs/2607.05475)) puts the cost of getting it wrong at up to
35% performance variance, and notes that raising thread count past the available cores collapses
throughput outright.

**What the prior art does not cover is H3.** Every engine above assumes the weights are resident.
None of them has, sharing the same cores, a worker pool doing O_DIRECT reads for the majority of the
model's bytes on every token. Whether the right move is to claim the fast cores or to leave one free
for the reader is a question this field has not had to ask.

## The order of cells

Cheapest first, and the first two need no code at all. The project's rule is to measure the ceiling
before building the mechanism, and here the ceiling is measurable directly.

1. **Does the skew exist?** During a real decode, sample `/proc/<pid>/task/*/stat` for each worker:
   accumulated user time, and the CPU it is currently running on. If the workers' user times come
   out near-identical, **H1 is dead before anything is built**, and only H2 and H3 remain. The
   migration count from the same samples says whether H2 has any raw material.
2. **Does this SoC have an exploitable skew at all?** The upstream `llama-bench` in the pinned
   submodule already accepts `--cpu-mask`, `--cpu-strict` and `--poll`. Run it on a resident model.
   A flat result there closes the axis for this device class without touching engine code.
3. **Mask against thread count, crossed.** Only if the first two survive, and it has to be a
   two-dimensional sweep: the optimal thread count *depends on* the mask, since four threads on two
   fast cores is contention rather than parallelism. `taskset` is enough to run this against the
   current binary.
4. **Implement**, and re-verify in the app rather than over adb.

### The confounds that decide whether the numbers mean anything

- **The verdict metric is `compute` in the decode breakdown, not tok/s.** A tok/s gain with
  `compute` unmoved is a confound, not a result. A gain that lands in `stall` instead is evidence
  for H3, and the conclusion about which mask to use inverts.
- **Frequency regime.** Per-app frequency caps mean an adb-only run and an in-app run are not the
  same machine, and the caps also decay under sustained load. Cells must be long enough to reach
  steady state, and the cap must be sampled **during** the cell, never only before it.
- **Thermals cut the other way here.** Concentrating threads on the fastest cluster creates a
  hotspot, so a short cell can show a gain that a realistic one gives back. Cooldown is gated on
  battery temperature as a condition, not as a timer ([benchmark-method.md](benchmark-method.md)).
- One engine at a time, and the cache budget held fixed across cells.

## The shape it would take, if it survives

Sketched here so the cells above are run against a design rather than toward one.

- `--cpu-policy off | reserve-io | fastest-n`, resolved on device from the table above. `off` is the
  default, as for every lossy or platform-dependent knob in this engine.
- **Two pools, not one.** `llama_attach_threadpool` takes a batch pool and a decode pool, and
  prefill and decode are opposite regimes here: prefill is compute-saturated, decode is
  latency-bound and shares the machine with the reader. A single global mask cannot express that,
  and the API already can.
- Failure is silent and total. If the affinity call is refused, or the granted set is smaller than
  assumed, fall back to today's behaviour. An unknown device must be no worse off than before.
- Configuration reaches the engine through `RunConfig`; the library reads no environment
  ([architecture.md](architecture.md)).

## What would close it

- Flat per-thread user times in cell 1, meaning H1 has no material.
- A flat upstream `llama-bench` sweep in cell 2, meaning the SoC has no exploitable placement effect
  and the axis closes for this device class regardless of what the engine does.
- A gain that survives a short cell and disappears once the cell is long enough to throttle.

A fourth outcome is worth naming because it is the most portable of all: the sweep may show that the
mask is worth little while the **thread count** is worth a lot. The engine's thread count has never
been swept in the current frequency regime. That result would be an integer chosen from
`cpu_capacity` at runtime, with none of the cgroup and vendor hazards above, and thread affinity
would be closed, documented, and cheap to have asked.
