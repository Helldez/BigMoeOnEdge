# Benchmark method

This is *how* to measure. The measured results — the full config matrix for Qwen and
Gemma, with mean/min/max/median/p95 tok/s — live in [benchmarks.md](benchmarks.md).

## On-device (the headline number)

Hardware for the reference numbers: OnePlus 15R, Snapdragon-class SoC, 11.3 GB RAM,
UFS 4.x storage. Model: Qwen3-30B-A3B-Q4_K_M (18.5 GB), ~1.7× device RAM.

Reproducible drivers (used for [benchmarks.md](benchmarks.md)): `scripts/bench-run.sh`
(one device-side run, prompt baked in so no quoting has to survive adb), `scripts/
bench-matrix.ps1` (the full 12-run matrix over adb), `scripts/bench-analyze.py`
(mean/min/max + median/p5/p95 from the CSVs). Use a fixed prompt and a fixed `-n`
(≥256 tokens, so the expert cache reaches steady state) across every config.

```bash
# push the model
adb push Qwen3-30B-A3B-Q4_K_M.gguf /sdcard/Android/data/io.bigmoeonedge.example/files/

# or, running the CLI directly over adb shell (binary staged by build-android.ps1):
bmoe-cli -m Qwen3-30B-A3B-Q4_K_M.gguf --moe-stream \
  --cache-mb 4000 --io-threads 4 -t 4 -n 48 --progress \
  --csv /sdcard/.../sweep-4000.csv -p "Explain mixture-of-experts routing."
```

Report `s/token` and the `compute + flash I/O` split from the `moe-stream:` line, and the
`moe-cache:` hit rate.

### Sweep

Vary one axis at a time:

| Axis | Values | Expectation |
|------|--------|-------------|
| cache-mb | 0, 2000, 4000, 6000 | monotone improvement; 0-or-≥2000 only |
| io-threads | 1, 4 | 4 ≈ 3× the serial read bandwidth |
| threads (-t) | 2, 4, 8 | U-shape, 4 optimal, 8 regresses |

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

- **Temperature** — `dumpsys battery` (`temperature` in deci-°C, `PhoneTemp`) and
  `/sys/class/thermal/thermal_zone*/temp` with matching `.../type` (CPU `cpu-*`, GPU
  `gpuss-*`, skin zones are readable by the shell user).
- **Free-RAM floor** — `/proc/meminfo` `MemAvailable`, sampled before / mid-run / after;
  its collapse under mmap *is* the pressure signal.
- **Throttling state** — `dumpsys thermalservice`.

Kernel **PSI** (`/proc/pressure/{memory,io,cpu}`) is the cleanest stall metric but returns
*Permission denied* without root on this device. Protocol: cool to a common baseline
between configs so sustained-decode throttling doesn't confound the comparison, then
produce a *tok/s vs. thermal rise and free-RAM floor* table alongside the throughput one.

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
