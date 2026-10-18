# Roadmap

Two parts. The first is where the project is going as a product, in the order the work has to
happen. The second is the research themes the engine still owes, with what was measured and what
was refuted; it is the older half of this page and it stays, because a measured negative is worth
more than an unmeasured idea.

## From an engine with demos to a product

What exists is one engine and three front-ends, each with an application layer of its own. The
desktop server holds a session, a model library with its catalog and downloads, settings in layers
rendered from the parameter table, conversations, telemetry and the planner, and serves a web UI
and an OpenAI-compatible endpoint over them. The Android app rebuilds most of that in Kotlin over
the CLI as a child process, with a settings screen and a catalog of its own, and has no planner.
The iOS harness is the CLI compiled into an app, a measuring instrument and not a chat app. The
planner reaches the CLI and the desktop; the phone, where the project started, has not got it.

The goal is one engine, one application framework, and a native UI per operating system that
looks the same and does the same everywhere: a person loads a model larger than the device's
memory, chats and works with it, or hands the device's hardware to any OpenAI-compatible client.
A phone serving a laptop's coding agent is the same product as a laptop serving its own.

The order is the order of dependence, and each step ships on its own.

**Release what is on `main`.** Shipped: the planner, the parameter table, the macOS port, the
desktop server and the iOS harness in 0.29.0; the server as the framework, the Android app on it
and one visual language in 0.30.0, after a run of the app on a phone and of the desktop server on
Windows. Owed from that run: the NPU prefill on the release APK, the one build that carries the
Hexagon backend.

**One application framework.** The server is it ([application-framework.md](application-framework.md)).
Its policy - the engine host with its one session and its turn line, the model library, the
settings layers, the event bus, the planner adapter, the chat protocol, the access rules - is the
`bmoe_server` library, with `bmoe-server` the process around it, and every UI speaks the one API
`docs/server-api.md` describes, over HTTP on loopback: as a child process where the OS lets the
engine run in a process of its own, which is what keeps a driver crash from taking the app with
it, and in-process, `run()` on a thread, where it does not. The library, the API key that lets the
server be reached from the network, and the Android builds that carry the server have shipped;
what the UIs owe is the move onto it. What falls out: the OpenAI-compatible endpoint exists on
every platform at no extra cost, a knob added to the engine appears in every UI with no UI change,
and the model catalog is one data file instead of a JSON here and a Kotlin table there.

**The Android app, complete, on that framework.** In code: the app runs `bmoe-server` from the APK
as a child process and is a client of its API, with the planner in the load path and its reasons
readable, settings rendered from the parameter table (sampling included), the shared catalog, chat
history that survives a restart, a copyable answer, the prompt where a thumb reaches it, and the
switch to serve the network with a key under the same security statement the desktop carries.
Owed: a run on a phone before it is released, and the decide endpoint (#245) that brings Choose
from options back to every UI.

**Desktop and Windows at parity.** An installer per OS, signed, that updates itself, in place of an
archive. In the engine, what Windows lacks: the device probes in a process of their own, the cache
lock, the byte-identity gates in its CI job, and one documented planner run.

**iOS, with the community.** The harness proves the engine builds and runs there; the app on the
framework needs a device, an Apple team and the memory entitlements, none of which this project
has. An issue states what exists, what is needed and what a contributor needs to bring.

**One experience, three native UIs.** One written specification for the five screens - chat,
models, plan, settings, metrics - that each UI follows in its own toolkit: [screens.md](screens.md).
Alike in what they show and in what a setting means, native in how they feel, and one visual
language for both, from the logo ([design.md](design.md)). Owed against it: the per-value
provenance (what a plan set, what differs from the default) on the phone.

**The planner on hardware it has not seen.** Every rule carries the machine it was measured on, and
the list of machines it is owed on is in [hardware-planning.md](hardware-planning.md): the idle
headroom arms on every kind of machine, a discrete GPU, a thread sweep so the withdrawn thread rule
can return with a measurement behind it, a Windows run. The plan already prints its prediction next
to what happened; collecting that pair from every run and from the community benchmarks is how the
coefficients get corrected on machines nobody here owns.

**The GPUs most machines have: Vulkan and OpenCL.** Of the eighteen backends the pinned llama.cpp
carries, three have a measurement behind them here ([backends.md](backends.md)), and the GPU in
most phones and PCs is reached through neither of the two GPU ones measured (Metal, Hexagon).
Vulkan reaches any GPU with a driver - discrete and integrated on Windows and Linux, and Android -
and OpenCL is llama.cpp's Adreno path. Both name the expert op, and both implement wrapping host
memory without advertising it, which is the capability streamed experts computed on a device would
need. The work, in order: carry each backend in a build (a desktop archive with Vulkan, an APK
with OpenCL beside Hexagon), so the planner sees the device with no code here; one measured run of
the prefill device on each, against the CPU and, on the phone, against the NPU; then the
`buffer_from_host_ptr` route for the streamed experts where the measurement says the device wins.
The OpenCL entry in [backends.md](backends.md) carries the one negative so far: the dense set on a
phone's integrated GPU lost 27%. That measured the dense weights, not the prefill.

**Connect a client from the UI.** Serving a coding agent or a chat client is what the endpoint is
for, and today it takes launch options and hand-typed values. One Connect view in every UI, fed by
the server: share on the network with a key, the base URL, model id, context and maximum output a
client must be told, a preset that sizes the context for an agent, and a filled-in snippet per
client. Researched and specified in [#257](https://github.com/Helldez/BigMoeOnEdge/issues/257).

**The planner, drawn.** One diagram of the whole solution, as logic rather than code: the facts
the probes measure, the rules that read them, the ledger that keeps memory honest, and the plan
that comes out with its reasons, for prefill and decode apart. It goes in
[hardware-planning.md](hardware-planning.md) and, in a short form, in the README, so a reader
sees in one picture why the same rules give a different plan on a phone, a laptop and a desktop.

**Measuring and loading, made readable.** A load in auto mode already measures the chosen model
on a quiet machine: the model in use is unloaded first, then the machine is measured, then the
model loads. What the person sees is one fixed line, "measuring the machine, then loading", for
tens of seconds on a phone, then "loading the model", with no progress for either: the probes
report nothing while they run and the load does not use llama.cpp's progress callback. Owed, in
every UI and from server events rather than from each UI, in this order:

- **Each step of the measurement, with its result and its duration.** The probes report through a
  callback the core takes, the CLI prints it, the server publishes it, and a UI lists whatever
  arrives (the drive's read rate, the memory the run may hold, each compute device with its figure
  or the reason it was left out), naming no probe, device or platform. The durations are also the
  first record of what measuring costs, which nothing keeps today.
- **The load's own progress, and a way to cancel it.** llama.cpp's progress callback for its part,
  and the engine's own for the dense loader and the prefill device's slots, where the time goes
  when the file is mapped.
- **A notice before measuring.** What is measured and why, how long it took last time on this
  machine, that the device is busy meanwhile and that other heavy apps skew the result.
- **Measurements kept, if the durations say it pays.** The machine's facts and each device's
  figure for a given quantization, kept by the server and used again until the build, the
  backends, the devices or the drive change, with the date shown and a button to measure again.
  The memory headroom and the placement are read at every load. Most of the measuring time may be
  the per-device matmuls, which depend on the model, so this waits for the first step's numbers.

Not now: a cache that resizes itself at run time. It is the agnostic answer to the available-memory
problem and it stays behind the static readings until those have been measured.

## Research themes

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
2026-07-20 with `bmoe-iobench` (`tools/iobench.cpp`) and **does not hold on the reference device**
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
- **Expert-contiguous layout — re-justified, built, and measured NEGATIVE (2026-07-20).** The
  re-justification came back genuinely positive at the drive level (`bmoe-iobench --scatter`:
  sub-MiB scattered pieces plateau ~10% under the ceiling; "flat above 256 KiB" held only at
  saturating lane counts), and a full sidecar implementation passed every byte-identity gate —
  but on device it wins only in the serial cache-off regime (+16%), and **loses 20-23% in every
  shipping configuration** (overlap + cache, LFM2.5 and Qwen3-30B k=8): the overlap kernel
  consumes projection-major and a whole-entry read triples the latency to an expert's first
  slice, so the pipeline stalls. Bandwidth is not the binding constraint; latency-to-ready is.
  Closed unmerged (PR #90, tag `expert-sidecar-refuted`); full data in
  [bench-data/2026-07-20-sidecar/findings.md](bench-data/2026-07-20-sidecar/findings.md).
- **Reading a slice straight into the cache buffer — blocked by the gguf's own offsets
  (2026-07-28).** Every streamed byte is pulled into a per-lane bounce buffer and then memcpy'd to
  its cache slot, so the CPU touches it twice; at 100-200 MB/token that copy is not free, and it
  lands on the I/O lanes, which under overlap run alongside a decode that is already compute-bound.
  Skipping it needs the read to be block-aligned at all three of offset, length and destination.
  Measured on the three shipped models: **bytes-per-expert is 4096-aligned, the tensor's file
  offset is not** — 512 (Qwen3-30B), 1152 (Qwen3.6-35B), 2272 (gpt-oss-120b) — because gguf aligns
  tensor data to `general.alignment` (32), not to a device block. So an O_DIRECT window must round
  outward and always spills one block into the *neighbouring experts'* bytes.

  Writing that spill is content-identical (the cache buffer mirrors the file layout), but it is not
  safe here: those pages belong to other cache entries, which may have been evicted, and touching
  them would fault a page back with nothing accounting for it — putting the residency budget out of
  step with what is actually resident. The trick that would make it work (offsetting each buffer's
  base by `file_off % align` so the destination inherits the file's misalignment) does not remove
  the spill, only its size. Not built: the win is a memcpy, the risk is the memory accounting the
  whole engine's budget rests on, and it cannot be judged without a device.
- **The gap between the engine's effective rate and the drive's was the model file's own mapping,
  on Windows (2026-08-29).** This entry used to call it duty cycle and leave it unsized. It is
  not duty cycle: while llama.cpp's section of the gguf is alive, NTFS serialises the streamer's
  concurrent unbuffered reads, so four lanes deliver one lane's throughput — which is also the
  real reason lanes and threads measured dead on this host, and why the serial path once read at
  exactly the one-lane rate with four lanes open. `bmoe-iobench --mmap` reproduces it in one
  cell and `--mmap --reopen-lanes` recovers it; `--release-mmap` is the engine's version and is
  worth +46% decode on the desktop host, lossless. The same cells are flat on the phone, so this
  is a platform defect, not a read-path one, and the phone's remaining gap is still unsized.

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

Keeping the dense weights resident once they are in has no lever in
[android-memory.md](android-memory.md) — except one, found on 2026-07-21: a **dma-buf allocated
through `AHardwareBuffer`** is exempt from reclaim by construction and needs no privilege. Its
bandwidth gate passes cleanly (1.00× against anonymous memory; usable in 2047 MiB units, an
`AHardwareBuffer_lock` boundary at 2^31 bytes —
[bench-data/2026-07-21-pinned-memory/](bench-data/2026-07-21-pinned-memory/findings.md)). What is
`--dense-weights ahwb` implements it, and the in-app A/B came back **positive: +17.9% on a long
generation, confidence intervals disjoint**, with `dense_resident_frac` pinned at exactly 1.000
([bench-data/2026-07-21-pinned-dense-ab/](bench-data/2026-07-21-pinned-dense-ab/findings.md)).

The mechanism is not the predicted one, and that correction is the more valuable half. Major faults
are *equal* between the modes: `anon` already keeps the dense weights off the flash. What it does
not prevent is the kernel taking ~15% of them into **zram**, where a later touch is a minor fault
plus a decompression — a cost that appears in no I/O or fault counter and lands in `compute_ms`.
So **`compute_ms` has been absorbing swap-in all along**, and any earlier "this regime is
compute-bound" conclusion deserves re-examination. Making that cost visible (minor faults, swap-in
time) is worth more than the next few percent of throughput.

Still open: the reversed-order pair (`ahwb` ran first in the decisive pair), and anything beyond one
device / model / config — hence default off. **Not** worth extending to the expert cache without
sizing the prize first: under `ahwb` only ~294 MiB of a 3000 MiB budget sits in zram, so the prize
is a fraction of the dense one while the cost — 3 GiB of rigid, LMK-accounted memory, and the loss
of the reserve/commit/evict elasticity the cache is built on — is far higher.

## Expert cache policy — closed, negatively

Whether a smarter eviction policy could raise throughput is **answered, and the answer is no**
([bench-data/2026-07-20-cache-replay/](bench-data/2026-07-20-cache-replay/)).

Offline replay of the route traces through Bélády, LRU, LFU, random and a per-layer partition
(`scripts/route-replay.py`, validated against the recorded hit rates to the decimal) shows the
offline optimum is 11-23 points above LRU, but **no online policy recovers more than ~5**. The
best candidate, a per-layer budget partition with frequency eviction, was implemented under the
tag `experiment/layer-lfu` and measured: it delivers the predicted hit-rate gain
(+2.0 points, −7 % flash reads) and is **~30 % slower**, because a hard per-layer cap removes the
cache's ability to self-balance and the resulting `MADV_DONTNEED` churn gets paid for by the
kernel reclaiming the dense weights (majflt/token 6 → 2370). It is preserved under that tag rather
than merged — a measured regression does not belong in the engine.

The transferable lesson: a hit-rate curve is not a throughput argument. Any future policy has to
be measured on device, however good its simulation.

What was still worth doing here was **not** a policy but a guard, and it has shipped. Global LRU's
recency order is anti-correlated with the deterministic layer cycle, so below one token cycle it
evicts precisely what it is about to read and the hit rate goes to **exactly 0 %** — reproduced on
device at a budget the CLI accepts today. Since the worst-case cycle is computable at init from
model shape alone, the engine now prices it there, records it as `cache_cycle_mb`, and warns when
the resolved budget falls under it. It warns rather than refuses: the budget is legal and the run is
byte-correct, it just cannot hit. See [cache-sizing.md](cache-sizing.md).

## More architectures

`qwen3moe`, `qwen2moe`, `qwen35moe` (the hybrid attention/SSM family, e.g. Qwen3.6-35B-A3B),
`gemma4` (merged `ffn_gate_up_exps` plus shared experts), OpenAI `gpt-oss` (MXFP4, purely
routed) and `nemotron_h_moe` (gate-less `ffn_up_exps`/`ffn_down_exps`) are supported; other
`build_moe_ffn` models are one recipe row each. The remaining frontier is architectures whose
routing node is not the shared `ffn_moe_topk` — custom gating, which the capture/stream hook
would need to learn. See [adding-a-model.md](adding-a-model.md).

## Steering the routing toward what is resident — built, measured on the desktop

`--expert-substitute` ([cache-aware-substitution.md](cache-aware-substitution.md)) re-ranks each
decode routing toward the experts already in the cache, by a margin that is a fraction of the
token's own score range. It is the mechanism of Skliar et al. (arXiv:2412.00099) with the cache in
front of flash instead of DRAM, and on the desktop it is the strongest lever measured at its
quality cost: half the flash bytes per token and +62% decode for a 1 to 4% perplexity increase at
`0.15`. What it owes is the same device A/B as dropping, and a task-level quality check, since the
desktop's flash share of a token is larger than the phone's and the throughput column will
compress.

## Skipping reads the router barely wants — built, unmeasured

`--drop-cold-experts` ([expert-dropping.md](expert-dropping.md)) is the first lever that treats
quality and I/O as a *joint* budget rather than two separate knobs: an expert already in the cache
runs however small its weight, and only a routing that would cost a flash read can be dropped. On
the recorded traces that is worth ~3× the reads of turbo top-k for a comparable weight cost, which
is the strongest offline case any remaining lever has shown.

What it does **not** have is a device measurement, and the previous entry on this page is the reason
that matters: `layer-lfu` simulated well and was ~30% slower in reality. The open questions are the
device A/B against turbo top-k at matched throughput, the quality comparison at that speed, and how
far the static replay overstated the win once dropping starts changing what the cache holds.

## Expert quantization on the fly

Storing streamed experts at a lower precision than the resident parts to cut read volume, if it
can stay within an acceptable quality boundary. Most valuable exactly where read bandwidth still
binds, above.

## Bigger, smarter cache

The cache is capacity-bound, not policy-bound (reuse is broad, not skewed), so the simplest win is
more budget — which `--cache-mb auto` now takes automatically, capped by `--cache-ceil-mb`
([cache-sizing.md](cache-sizing.md)). Admission policies and a persistent cross-run cache
remain unexplored.

## Hardware planning — what is measured, and what is still owed

`--auto` derives the streaming knobs from the machine and the model instead of from flags, and
prints the fact behind each choice ([hardware-planning.md](hardware-planning.md)). The rules carry
no platform names, so the same rule reaches a different answer on a desktop, a phone and a
kill-on-resident platform from the facts alone.

In: the read-rate probe (request size against lane count, about a second at load, the only honest
source of `--io-threads`); the mapping-interference probe; llama.cpp's own capacity fitter as a
first stage; the headroom probe, which answers what this process can hold rather than what the
machine reports available; and a thread rule that reads core classes rather than a core count,
because a barrier waits for its slowest participant.

What is still owed is measurement rather than design, and every knob the plan cannot decide prints
`[unprobed]`.

- **Streamed experts computed on a device, rather than on the CPU cores.** Half built, and the
  missing half is a design question rather than plumbing. A device is asked whether it offers a host buffer type and whether it runs this model's
  own expert matmul on the file's native layout; the bandwidth probe schedules one GEMV on every
  backend so the host's figure and the device's are the same measurement; where the device wins, the
  session binds the overridden experts to that device's host buffer type at load. What is not done is
  the streamer's own reservations coming from that same allocator: it rebinds `data` onto memory it
  reserved itself, so a device pointed at those tensors would read ordinary host memory it was never
  given access to. Doing it trades against the lazy commit that lets a model far past RAM have valid
  addresses everywhere while holding a fraction of it. On a machine that registers only a CPU the
  measured half runs and finds nothing to compare, which is what the plan says.
- **A prefill-aware cache floor.** Each expert is already read at most once per ubatch (the `seen_`
  guard in the streamer), so the read-once property that wave-partitioned prefill exists to provide
  is not missing. What is missing is a guarantee that the cache can hold one layer's ubatch working
  set: the derived floor is the *decode* token cycle, and a wide prefill batch touches many more
  experts per layer than a token does.
- **The prefill device, anywhere but where it was chosen.** `--auto` now picks it: it measures the
  model's matmul at prefill width on the host and on every device, and arms the fastest one that
  reproduces the host's result, wins by a margin and whose slots fit the memory ledger
  ([hardware-planning.md](hardware-planning.md)). That choice has been made on one laptop with a
  GPU that shares the host's memory. A phone's NPU, which a caller still names by hand in the
  measurements here ([npu-prefill.md](npu-prefill.md)), and a discrete GPU each owe a run.
- **The budget, from what other processes hold idle: measured on three machines, and parked.**
  The reported available figure moves (3.0 to 12.4 GB within an hour on one laptop) and is a
  floor wherever the kernel reclaims other processes' idle memory to make room. Counting that idle
  memory into the budget was tried as one formula over measured facts (the idle set times what a
  byte shrinks to when taken, capped by the swap left free) and measured on a Windows laptop whose
  reclaim goes to a pagefile, a phone and a Mac whose reclaim compresses. It was right where bytes
  go out whole (the laptop: 3.22 tok/s from the reported figure, 4.37 plain mmap, 4.92 from the
  idle set) and wrong wherever they are compressed: on the phone a cache sized that way cost 15 s
  of stalls in 64 tokens, and on the Mac with a 7.8 GB neighbour every cache size lost to no cache
  at all, the reported reading included (7.28 tok/s with the cache off, 5.47 at 1.5 GB, 3.06 at
  2.5 GB), because the compressor takes the cache's cold pages and each hit then pays a
  decompression. `--auto` keeps today's reading, which was the best of the three everywhere; the
  code and the full tables are reachable from the closed PR #230. What the measurements say is
  that on a machine whose reclaim compresses, no static reading survives pressure: the size at
  which the system starts taking the cache back is a property of the run, which is what
  the proposal in [#238](https://github.com/Helldez/BigMoeOnEdge/pull/238), on hold, would read at
  run time.
- **The graph width, anywhere but where it was measured.** `--auto` now chooses `--ubatch` from
  the compute buffers a width reserves, projected at the width the session opens with. The rule and
  its one policy figure were sized on a single laptop; a phone with an NPU, a machine with no
  accelerator, a discrete GPU and a system that commits what it reserves each owe a run before the
  rule is believed there ([hardware-planning.md](hardware-planning.md#the-widest-graph)).

## Not on this list

Routing prediction and speculative expert gating were built and **removed**: the recall/latency
trade never paid on-device, and the predictor coupled the streamer to model internals, which cost
more in modularity than it returned in throughput. The archived measurements are in
[bench-data/2026-07-12-pr23/](bench-data/2026-07-12-pr23/).

**Reading a routing early is now measured, and it does not buy throughput either.** The question
was reopened with a predictor that costs no training and no model coupling — run the *next* layer's
router matrix on the *current* layer's gate input, which `--predict-log` scores against the
previous-token bet and against a zero-staleness control ([expert-prediction.md](expert-prediction.md)).
The accuracy is real: **88.6%** of routed slots on Qwen3-30B and **80.7%** on Qwen3.6-35B, against
43.3% / 35.4% for the incumbent, with the control at exactly 100% on every layer. Acting on it is
what fails. In thermally matched pairs, reading ahead on that prediction **lost 21%** in the
shipping drop + pinned-dense configuration with its hit rate *up* 4.3 points and 79% of speculations
useful — the same mechanism that killed more lanes, coalescing and the sidecar: on a saturated
flash, a better guess still spends bandwidth that was the binding constraint. The half that spends
nothing — retaining predicted residents against eviction — moved the hit rate by 0.4 points at a
3000 MiB cache, inside the ~5-point ceiling the offline replay already put on the whole class.
`--predict-prefetch` ships off by default, and the app defaults its budget to retention-only. What
would change the answer is a device where flash is *not* the constraint, or a predictor that reaches
layer 0 — which needs a trained per-layer artifact, a different project from this one.
