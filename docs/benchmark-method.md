# Benchmark method

How to measure this engine on any machine, and how to read what comes out.

- Want to submit a comparable row? [community-benchmarks.md](community-benchmarks.md) has the
  fixed protocol and one command.
- Want to tune, sweep, or understand a number? You are in the right place.
- Want the results? [benchmarks.md](benchmarks.md).

Nothing here is specific to one device. Numbers are quoted to illustrate a mechanism; the hardware
behind the published figures is named [at the end](#where-the-published-numbers-come-from).

## The run

```bash
bmoe-cli -m MODEL.gguf --moe-stream --cache-mb auto --io-threads 4 -t 4 -n 256 \
    --overlap --dense-weights anon --csv run.csv -p "..."
```

- **Fixed prompt, `-n` at least 256.** Below that the expert cache is still warming and you are
  timing the cold head.
- **Check `o_direct=1` before trusting anything.** On Android the app's external files dir under
  `/storage/emulated` is FUSE-backed: the `O_DIRECT` open succeeds but reads can return wrong data,
  so the engine falls back to buffered and you are no longer measuring the path this page assumes.
  `/data/local/tmp` and `/sdcard/Download` are real filesystems. On macOS the flag is a no-op today
  (see [limitations.md](limitations.md)).
- **Report** `s/token`, the `compute + flash I/O` split from the `moe-stream:` line, and the
  `moe-cache:` hit rate.
- **Drivers:** `scripts/bench-report.sh` (host, prints a paste-ready block), `scripts/bench-run.sh`
  (device-side over adb, prompt baked in, also samples peak RSS, the `MemAvailable` floor and CPU
  and battery temperature).

## Choosing the parameters

Move one at a time: a number produced by moving two is not attributable to either. The knobs fall
into three groups, and mixing the groups in one table is how a lossy result ends up quoted as a
speedup.

### Lossless: same output, different speed

Everything here is safe to tune. The generated text does not change.

| Knob | CLI default | Protocol uses | Move it when | Read this to check |
|---|---|---|---|---|
| `--cache-mb` | `auto` | `auto` | auto guessed badly | `moe-cache:` hit rate |
| `--io-threads` | 4 | 4 | deep queue (NVMe) or shallow one (SD, USB) | stall s/tok |
| `-t` | 4 | `min(8, cores)` | many cores, or many slow ones | compute s/tok |
| `--overlap` | off | on | never off, except to A/B it | stall s/tok falls, compute does not |
| `--dense-weights` | `anon` | `anon` | the model fits in RAM: use `warm` | `majflt/tok` |
| `--ubatch` | 0 (follow `n_ctx`) | 512 | memory is tight: it trades prefill width for RAM | reserved compute buffer, then hit rate |

### Lossy: the output changes

Never quote one of these as a speedup without the text and the quality check next to it.

| Knob | Default | What it trades | Report alongside |
|---|---|---|---|
| `--n-expert-used` | model default | routes fewer experts: compute and I/O fall close to linearly (8 to 6 is about -25 %) | the generated text, and an A/B against the model's own default in the same session |
| `--drop-cold-experts` | off | skips experts that are not cached, decided from live cache state | `experts_dropped` / `experts_routed`. **Non-deterministic**: the same command drops a different number run to run |

### Experimental: off by default, pending an on-device A/B

Fine to measure, not fine to publish as the engine's number.

| Knob | Default | Status |
|---|---|---|
| `--io-two-wave` | off | publishes a layer's first-projection reads early so the lanes start sooner |
| `--prefetch K` | off | temporally prefetches the next K layers' experts; needs the cache |
| `--mtp`, `--ngram` | off | speculative decode. Not byte-identical to a plain run, so it is its own comparison |

The rest of this section takes them in turn.

### The lossless knobs, in detail

**Cache.** `auto` sizes from free memory, leaving `--cache-floor-mb` (default 1536 MiB) for the
system; `--cache-ceil-mb` caps it. Two ways to get it wrong:

- **Too small is a cliff, not a slope.** Below one token's worth of experts the hit rate is 0 % and
  the run is slower than with no cache at all. Use `0` (off) or a real budget, never a gesture.
- **Too large costs more than it looks** on a phone or small board: the memory comes out of the
  dense weights and the kernel reclaims those mid-decode. The tell is `majflt/tok` climbing while
  the hit rate improves.

Size it from the *model*: the run prints MiB read per token, and a few times that is where the hit
rate stops moving. See [cache-sizing.md](cache-sizing.md).

**Read lanes.** 4 is the plateau on flash for the request sizes the streamer issues. Fewer leave
bandwidth unused; more do nothing once the drive is saturated, and can hurt on storage that
penalises small requests. If stall s/tok will not fall when you raise them, the drive is the limit,
not the queue: compare `Flash/token` over the measured read rate against the stall.

**Threads.** The curve is a U, not a ramp. Past the big-core count the extra threads contend and
each layer's tail gets longer. Start at `min(8, cores)`; on big.LITTLE try the big cores alone.

**Dense weights.** The biggest lever once the model is well past RAM, and picking it wrong changes
*what* you measure, not just how fast it is:

| Mode | What it does | Use when |
|---|---|---|
| `mmap` | leaves them in the page cache; the kernel reclaims mid-decode and the refault is billed to **compute**, so a fault-bound run looks compute-bound | diagnosis only |
| `anon` | `O_DIRECT` into our own buffers, so a reclaim goes to zram, not flash | default, and the answer past RAM |
| `warm` | page-cached at load | the model fits in RAM |
| `ahwb` | dma-buf the kernel may not reclaim at all | Android, models where even zram hurts |

`majflt/tok` on the `compute:` line says which regime you are in: hundreds means the dense set is
thrashing, single digits means it is not.

**Prefill width.** `--ubatch` sets the widest graph computed at once. Decode is one token wide
whatever it says, so this does not cost decode throughput: what it costs is prefill speed, and what
it buys is **resident memory**. The scheduler reserves compute buffers for the worst-case graph, and
on this engine every reserved MiB is a MiB the expert cache and the dense weights do not get.
Measured at a 2048 context: 320 MiB reserved at full width, 80 MiB at 512. The CLI default is `0`,
meaning follow the context; the Android app and the community protocol both pin 512. On a
memory-tight machine, set it before you conclude the cache is too small.

### The lossy two, in detail

**Top-k.** `--n-expert-used` cuts compute and I/O close to linearly (8 to 6 is about -25 %) and
changes the output. A speed/quality trade, never a free speedup: inspect the text, report speed and
correctness separately. A/B it against the model's own default *in the same session*, both cells
entering from the same measured state — never against an older table. A cool-versus-warm machine
shifts the baseline enough to swamp the effect, and has been measured inverting it outright.

**Dropping.** [`--drop-cold-experts`](expert-dropping.md) is the only non-deterministic knob: it
reads live cache state, so the same command legitimately drops a different number of experts run to
run. Always record `experts_dropped` / `experts_routed` (or the `moe-drop:` line) next to the
tok/s — the flag fixes a threshold, not a rate, and a dropping cell without its drop rate is
uninterpretable. It also pays the same per-MoE-layer barriers a route-traced run does, so an A/B
against `--n-expert-used` is not overhead-matched.

## gpt-oss and other harmony models

The harmony template **always** opens an `analysis` channel before the answer, so a plain run
spends its whole `-n` budget reasoning. Pass **`--no-think`**: the engine primes the `final`
channel directly. Without it, a throughput run is timing chain-of-thought.

- **`--no-think` is a speed mode, not a free lunch.** It removes the model's scratch space, so
  reasoning-dependent tasks degrade: on `17 x 23` the default top-4 answers *wrong* while k=2/3
  answer right (greedy, so it depends only on k). Report speed and correctness separately.
- **Use the 256-token protocol, not a short probe.** A 24-token run leaves the cache warming
  (10-20 % hit against 27-32 % at 256) and is dominated by the cold head. The 2026-07-14 sweep in
  [benchmarks-gpt-oss.md](benchmarks-gpt-oss.md) was 24-token probes and reads about 3x low.
- **Set `--dense-weights anon`.** At this distance past RAM it changes the regime, not just the
  number.
- **Read from a real filesystem**, `/data/local/tmp/...` on Android, never `/sdcard`.

## Cool on a condition, not a timer

The rule that most often decides whether a matrix means anything. A fixed sleep does not return a
machine to baseline, so throughput tracks execution *order* and the matrix measures the order
instead of the config. On a phone this has inverted a reproducible +24 % into a measured -10.7 %
purely by cell position. The cause is reclaim hysteresis: the kernel takes memory away in seconds
and gives it back over minutes ([android-memory.md](android-memory.md)).

- **Gate each cell on a measured condition** — CPU temperature and free memory back under a
  threshold — with a bounded give-up that is *recorded* when it fires.
  `bench-data/2026-07-17/driver-lanes.sh` is a working example.
- **Read the CPU sensor, not the battery.** Battery temperature lags the SoC by minutes and will
  rank two cells backwards.
- **Log the entry state next to every number** (`scaling_max_freq`, CPU temp, `MemAvailable`), so a
  contaminated cell is visible in the data instead of being found later. Or published.
- **Start cold and idle.** A phone on USB power idles warm and may never reach a low gate at all.
- **Far past RAM, budget minutes rather than seconds.** A run that large leaves the machine hot and
  its free memory depressed long after the process exits.

Two tells that a cell is contaminated rather than informative:

1. **compute s/token rises as top-k falls.** Physically impossible — fewer active experts cannot
   make the same kernels slower. It is fault-service time landing in the compute bucket.
2. **majflt/token jumps an order of magnitude** between cells that should do comparable work.

Re-run such a cell, do not publish it, and sanity-check any matrix by **reversing the run order**:
cells that move were measuring machine state. The reversal check does not work under
`--drop-cold-experts`, where a cell can move because the drop rate moved, and the two tells above
cannot tell that apart from contamination.

## Caveats

- **Thermal.** Sustained decode throttles. Warm up, measure a steady window, discard the first few
  tokens. On Android ignore `cpu-hw-trip-*` sensors: static 95 °C trip points, not live readings.
- **Report the distribution, not just the mean.** `min` and `max` tok/s are single-token extremes
  (one eviction stall crushes `min`). Pair them with median and p5/p95 so an unstable config is
  distinguishable from a slow-but-steady one.
- **Streaming only pays off above the RAM ceiling.** Where the model fits, run it resident. A
  streamed run there measures the overhead: a valid thing to measure, not a recommendation.

## Device pressure (throughput is half the story)

tok/s says nothing about what a config does to the rest of the machine. `mmap` faults the whole
model through the page cache and evicts everything else, so a phone goes sluggish; a bounded cache
with O_DIRECT keeps the system responsive. Record a pressure indicator next to tok/s. On Android
all of these are readable over adb without root:

| Signal | Where | Note |
|---|---|---|
| Temperature | `/sys/class/thermal/thermal_zone*/temp` + `.../type` | CPU `cpu-*`, GPU `gpuss-*`, skin zones. `dumpsys battery` lags the SoC by minutes: record it, decide with the CPU zone |
| Free-RAM floor | `/proc/meminfo` `MemAvailable` | sample before, mid-run, after. Its collapse under mmap *is* the pressure signal |
| Major faults | `majflt/token`, engine's `compute:` line | the only thing that separates "kernels are slow" from "dense weights are refaulting" |
| Throttling | `dumpsys thermalservice`, `scaling_max_freq` | |

Kernel **PSI** (`/proc/pressure/*`) is the cleanest stall metric but needs root on Android.
Protocol: bring every config to a common baseline *by measurement*
(see [above](#cool-on-a-condition-not-a-timer)), then publish a tok/s versus thermal-rise and
free-RAM-floor table alongside the throughput one.

## Host correctness

- **Gates** (mandatory before release): `cd build && ctest --output-on-failure`. They prove
  streamed == resident on the tiny synthetic model.
- **Real small MoE** (release checklist): Qwen1.5-MoE-A2.7B-Q4_K_M streamed against resident on the
  dev host, identical output. Too large for CI.

The streamer works on Linux, macOS and Windows, but the throughput targets are stated for Android
and Linux on flash: Windows `VirtualAlloc` commit-per-slice is heavier, and macOS does not bypass
the page cache today ([limitations.md](limitations.md)).

## Where the published numbers come from

A 12 GB, UFS 4.x Snapdragon-class phone and a 16 GB x86 laptop with an NVMe drive, driven by
`scripts/bench-run.sh` (one device-side run over adb), `scripts/bench-matrix.ps1` (8 configs by 2
models, two `--overlap` rows among them) and `scripts/bench-analyze.py` (mean/min/max, median,
p5/p95, plus the pressure table). Community rows are in
[community-benchmarks.md](community-benchmarks.md).
