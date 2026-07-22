# Benchmark method

This is *how* to measure. The measured results — the full config matrix for Qwen and
Gemma, with mean/min/max/median/p95 tok/s — live in [benchmarks.md](benchmarks.md).

## On-device (the headline number)

Hardware for the reference numbers: OnePlus 15R, Snapdragon-class SoC, 11.3 GB RAM,
UFS 4.x storage. Model: Qwen3-30B-A3B-Q4_K_M (18.5 GB), ~1.7× device RAM.

Reproducible drivers (used for [benchmarks.md](benchmarks.md)): `scripts/bench-run.sh`
(one device-side run, prompt baked in so no quoting has to survive adb, and it also samples
the device-pressure axis — peak RSS, `MemAvailable` floor, CPU/battery temperature),
`scripts/bench-matrix.ps1` (the full matrix over adb — 8 configs × 2 models, including two
`--overlap` rows), `scripts/bench-analyze.py` (mean/min/max + median/p5/p95 and the pressure
table from the CSVs + `.metrics`). Use a fixed prompt and a fixed `-n` (≥256 tokens, so the
expert cache reaches steady state) across every config.

Push the model to a real filesystem. The app's external files dir under
`/storage/emulated` is FUSE-backed, where an `O_DIRECT` open succeeds but reads can return
wrong data — the engine detects this and falls back to buffered I/O, so a model staged there
does not measure the O_DIRECT path the rest of this method assumes. `/sdcard/Download` and
`/data/local/tmp` are real filesystems; check the `o_direct=1` field in the run's telemetry
before trusting a number.

```bash
# push the model
adb push Qwen3-30B-A3B-Q4_K_M.gguf /sdcard/Download/

# or, running the CLI directly over adb shell (binary staged by build-android.ps1):
bmoe-cli -m Qwen3-30B-A3B-Q4_K_M.gguf --moe-stream \
  --cache-mb 4000 --io-threads 4 -t 4 -n 256 --progress \
  --csv /sdcard/.../sweep-4000.csv -p "Explain mixture-of-experts routing."
```

Report `s/token` and the `compute + flash I/O` split from the `moe-stream:` line, and the
`moe-cache:` hit rate.

### Sweep

Vary one axis at a time:

| Axis | Values | Expectation |
|------|--------|-------------|
| cache-mb | 0, 2000, 4000, 6000 | monotone improvement; 0-or-≥2000 only. On a device whose resident model already fills RAM (Gemma), the top of this range OOMs — watch the `MemAvailable` floor |
| io-threads | 1, 4 | 4 ≈ 3× the serial read bandwidth |
| threads (-t) | 2, 4, 8 | U-shape, 4 optimal, 8 regresses |
| overlap | off, on | net gain **only over a warm cache** (hides residual flash wait behind compute); a net loss on a cold cache-0 stream, where I/O dwarfs compute |
| n-expert-used | default, 6 | fewer active experts cut compute + I/O ~linearly (8→6 ≈ −25%), changes the output |
| drop-cold-experts | off, 0.75, 1.0 | the second lossy axis, and the only **non-deterministic** one: what is skipped depends on cache state, so cells are noisier and the drop rate must be reported with the tok/s. Needs the cache on |
| dense-weights | warm, anon | decisive well past RAM, near-neutral near it: on gpt-oss (5.2× RAM) `anon` drops majflt/token from the hundreds to **6–10** and compute with it; on Qwen (1.64× RAM) there is little dense-fault pressure to remove. Watch `majflt/token`, not just tok/s |

When sweeping `--n-expert-used`, run it as a **matched A/B against the model's own default**
in the same session (same config, both cells entering from the same *measured* device state —
see [Cool on a condition](#cool-on-a-condition-not-a-timer--and-log-the-entry-state)) rather than
against an older table — a cool-vs-warm device shifts the baseline enough to swamp the effect, and
has been measured inverting it outright. Greedy decode makes the
output diverge once routing narrows, so inspect the generated text for quality alongside tok/s;
it is a speed/quality trade-off, not a free speedup. See the Turbo top-k section in
[benchmarks.md](benchmarks.md) for a measured pair.

### gpt-oss / harmony models

gpt-oss uses the harmony chat format, whose template **always** opens an `analysis`
(chain-of-thought) channel before the answer — a plain run spends its whole `-n` budget
reasoning, so a short probe never reaches the answer and per-token timing is measuring
analysis tokens. Pass **`--no-think`**: on harmony models the engine primes the `final`
channel directly, so the model answers immediately with no analysis tokens. Without it a
throughput run is timing chain-of-thought.

Three things to keep honest when reporting gpt-oss numbers:

- **`--no-think` is a speed mode, not a free lunch.** Forcing the `final` channel removes
  the model's scratch space, so reasoning-dependent tasks degrade — on `17 × 23` the default
  top-4 answers *wrong* while k=2/3 answer right (greedy, so it depends only on k). Always
  inspect the generated text; report speed and correctness separately.
- **Use the 256-token protocol, not a short probe.** A 24-token run leaves the expert cache
  warming (hit 10–20 % vs 27–32 % at 256 tokens here) *and* is dominated by the cold head, so it
  is a floor rather than a result. The 2026-07-14 sweep in
  [benchmarks-gpt-oss.md](benchmarks-gpt-oss.md) was 24-token probes and reads ~3× low against
  the current steady-state rows for that reason (among others).
- **Set `--dense-weights anon`.** This is the single biggest lever on a model this far past RAM,
  and *not* setting it silently changes what you are measuring: with the dense weights left in the
  page cache the kernel reclaims them mid-decode and the refault cost is billed to `compute`, so
  the model looks compute-bound when it is actually fault-bound. `majflt/token` (on the engine's
  `compute:` line) tells you which regime you are in — hundreds means the dense set is thrashing,
  single digits means it is not.

gpt-oss must be read from the real `/data` partition (e.g. `/data/local/tmp/...`), not
`/sdcard` — the latter is FUSE and O_DIRECT silently falls back to buffered there.

### Cool on a condition, not a timer — and log the entry state

**This is the rule that most often decides whether a matrix means anything.** A fixed sleep
between cells does *not* return the device to baseline, so throughput tracks execution order and
the matrix silently measures the order instead of the config. Measured 2026-07-17 over a 6-cell
matrix with a 45 s cooldown (the value this page used to prescribe): the free-RAM floor and the
major-fault rate degraded monotonically with position — run 1 entered at a 1.61 GB floor and 149
majflt/token, run 3 at a 0.63 GB floor, 1894 majflt/token and 88.8 °C. It inverted a
well-established result (Turbo top-k measured −10.7 % where it reproducibly gives +24 %) purely
because the k=6 cell ran second. The cause is reclaim hysteresis: the kernel takes memory away in
seconds and returns it over minutes (see [android-memory.md](android-memory.md)).

So:

- **Gate each cell on a measured condition** — CPU temperature and `MemAvailable` back under a
  threshold — with a bounded give-up that is *recorded* when it fires. `bench-data/2026-07-17/driver-lanes.sh`
  is a working example.
- **Read the CPU sensor, not the battery.** Battery temperature lags behind the SoC by minutes and
  will rank two cells backwards: in the 2026-07-17 lane pair, battery said cell A entered 2.7 °C
  *cooler* while the CPU said it entered 8.5 °C *hotter* and peaked at 93.8 °C. The CPU sensor is
  the one that governs compute throttling.
- **Log the entry state next to every number** (`scaling_max_freq`, CPU temp, `MemAvailable`), so a
  contaminated cell is visible in the data instead of being discovered later — or worse, published.
- **Prefer a cold, idle, unplugged-then-reattached device.** On USB power this phone idles around
  38 °C and will not reach a 36 °C gate at all.
- On a >>RAM model (gpt-oss at 5.2× RAM) budget **minutes**, not seconds: a 58 GB run leaves the
  device hot and its free RAM depressed long after the process exits.

**Two tells that a cell is contaminated rather than informative:**

1. **`compute` s/token rises as top-k falls.** Physically impossible — fewer active experts cannot
   make the same kernels slower. It is fault-service time landing inside the compute bucket.
2. **majflt/token jumps an order of magnitude** between cells that should do comparable work.

Re-run such a cell; do not publish it. And sanity-check any matrix by **reversing the run order** —
cells that move were measuring device state.

**The reversal check does not work under `--drop-cold-experts`.** There a cell can move because the
*drop rate* moved — the policy reads live cache state, so the same command legitimately discards a
different number of experts on a different run. That is the feature working, not the device
contaminating the cell, and the two tells above cannot tell them apart. Always record
`experts_dropped`/`experts_routed` (or the `moe-drop:` line) next to the tok/s: a dropping cell
without its drop rate is uninterpretable, because the flag fixes a threshold and not a rate. Note
also that a dropping run pays the same extra per-MoE-layer barriers a route-traced run does, so an
A/B against `--n-expert-used` is not overhead-matched — see
[expert-dropping.md](expert-dropping.md).

### Caveats

- **Thermal.** Sustained decode throttles. Warm up, then measure a steady window; discard
  the first few tokens. Ignore `cpu-hw-trip-*` sensors — those are static 95 °C trip
  points, not live temperatures.
- **Report the distribution, not just the mean.** `min`/`max` tok/s are single-token
  extremes (a lone eviction stall crushes `min`); pair them with median and p5/p95 so an
  unstable config (wide spread) is distinguishable from a slow-but-steady one.
- Expect **0.27–0.6 s/token** across the good part of the sweep (4000 MiB cache, 4 lanes,
  256-token steady state); shorter runs read slower because the cache is still warming.

### Device pressure (throughput is only half the story)

tok/s does not capture what a config does to the *rest* of the phone. `mmap`-only faults
the whole model through the page cache and evicts other apps, so the device goes sluggish;
streaming with a bounded cache + O_DIRECT bypasses the page cache and keeps the system
responsive. Record a pressure indicator next to tok/s. Accessible over adb **without root**:

- **Temperature** — `/sys/class/thermal/thermal_zone*/temp` with matching `.../type` (CPU
  `cpu-*`, GPU `gpuss-*`, skin zones are readable by the shell user). `dumpsys battery`
  (`temperature`, deci-°C) is easy to read but **lags the SoC by minutes** — record it if you
  like, decide with the CPU zone.
- **Free-RAM floor** — `/proc/meminfo` `MemAvailable`, sampled before / mid-run / after;
  its collapse under mmap *is* the pressure signal.
- **Major faults** — `majflt/token` on the engine's own `compute:` line. This is the one that
  distinguishes "the kernels are slow" from "the dense weights are refaulting"; nothing else
  separates those two, because the kernel bills the refault to the compute call.
- **Throttling state** — `dumpsys thermalservice`, plus `scaling_max_freq` (see
  [Caveats](#caveats)).

Kernel **PSI** (`/proc/pressure/{memory,io,cpu}`) is the cleanest stall metric but returns
*Permission denied* without root on this device. Protocol: bring every config to a common
baseline *by measurement* (see [Cool on a condition](#cool-on-a-condition-not-a-timer--and-log-the-entry-state))
so sustained-decode throttling doesn't confound the comparison, then produce a *tok/s vs. thermal
rise and free-RAM floor* table alongside the throughput one.

## Host (correctness + a sanity number)

- **Gates** (mandatory before release): `cd build && ctest --output-on-failure`. These
  prove streamed == resident on the tiny synthetic model.
- **Real small MoE** (release checklist): run Qwen1.5-MoE-A2.7B-Q4_K_M streamed vs
  resident on the dev host and confirm identical output. It is too large for CI.

### Desktop over-RAM run (measured)

Streaming also delivers on desktop when the model exceeds the machine's RAM.
Qwen3-30B-A3B-Q4_K_M (17.3 GiB, 128 experts, 48 layers) on a Windows PC with 14.8 GiB RAM
— 1.17× RAM, so it cannot be held resident — cache 4000 MiB, 4 I/O lanes, 4 threads:

```
generation: 16 tokens, 0.388 s/token (2.58 tok/s)
moe-stream: read 13774 MiB (861 MiB/token), 1.28 GiB/s O_DIRECT
moe-cache: 44.8% hit, resident 3997 MiB
```

Output is coherent; running the same model resident is impossible on this machine (OOM).
On a desktop where the model *does* fit in RAM, run it resident — streaming only pays off
above the RAM ceiling.

The host streamer works on Linux/macOS/Windows, but throughput targets apply to
Android/Linux on UFS storage; Windows `VirtualAlloc` commit-per-slice is heavier (see
[limitations.md](limitations.md)).
