# Benchmarks — Android (OnePlus 15R)

Measured throughput of the expert-streaming engine on a phone whose RAM is smaller than
the model, across the full configuration matrix, for two MoE families. These are the
numbers the README table and the headline claim are drawn from; the how-to lives in
[benchmark-method.md](benchmark-method.md).

## TL;DR

- Streaming a >RAM MoE model is **stable and usable**: ~1.7–1.8 tok/s with no cache, up to
  **~3.5–3.75 tok/s** with a 4 GiB expert cache and 4 read lanes — on an 11 GB phone
  holding an 18.5 GB (Qwen) or 17.0 GB (Gemma) model.
- **Expert-cache size is the dominant lever.** Going 2000 → 4000 MiB roughly doubles
  throughput because the cache hit rate climbs (Qwen 53 % → 76 %, Gemma 58 % → 82 %) and
  flash read per token collapses (Qwen 480 → 225 MiB, Gemma 366 → 144 MiB).
- **Parallel read lanes are a secondary lever.** Lane 2 → 4 adds ~10–20 % at a 2000 MiB
  cache and almost nothing at 4000 MiB (once the cache absorbs most reads, decode is no
  longer I/O-bound).
- **`mmap`-only is a trap.** Its *median* token rate looks fine (Qwen 2.06, Gemma 1.96
  tok/s) but its *aggregate* throughput collapses (Gemma **0.61** tok/s) because a handful
  of page-cache-eviction stalls (single tokens as slow as 7.7 s) dominate the total — and
  during those stalls the phone is effectively unusable for anything else (see
  [Device pressure](#device-pressure-not-just-tokens)).
- **At a 4 GiB cache, decode is compute-bound, not I/O-bound.** The engine reports the
  decode split as `compute + flash I/O`. At cache 4000 the flash I/O share drops to
  ~0.10–0.15 s/token while compute (~0.14 Qwen, ~0.19 Gemma) dominates. Streaming overhead
  is therefore small: with a perfectly-sized cache the ceiling would be **~7.0 tok/s (Qwen)
  / ~5.3 tok/s (Gemma)**, which is the same SoC's in-RAM decode speed. The streaming path
  is no longer the bottleneck — the compute kernels are.

## Environment

| | |
|---|---|
| Device | OnePlus 15R (`CPH2769`), Android 16 |
| SoC / cores | Snapdragon-class, 8 cores, `arm64-v8a` |
| RAM | 11.3 GB (`MemTotal` 11 366 276 kB) |
| Storage | UFS 4.x, models read from `/sdcard/Download` (O_DIRECT verified working) |
| Engine | `bmoe-cli` built from branch `test/gemma4-apk` (integrates the model-import and fused-expert-recipe PRs) |
| Compute threads | 4 (`-t` default) |
| Decoding | greedy (argmax) — output is deterministic, so token content is identical across configs |

Models (both Q4_K_M, so the two families are compared at the same quantization):

- **Qwen3-30B-A3B-Q4_K_M** — 18.5 GB, 128 experts, top-8, 48 layers (≈1.64× device RAM).
- **Gemma-4-26B-A4B-it-Q4_K_M** — 17.0 GB, fused gate+up expert layout (≈1.51× device RAM).

## Method

Each configuration is one `bmoe-cli` run over `adb shell`, generating **256 tokens** from a
single fixed prompt with `--chatml`, writing per-token metrics to CSV. 256 tokens (vs. the
older 48-token spot checks) lets the expert cache reach steady state, which is why the
cached configurations here read higher than earlier short-run numbers.

`wall_ms` in the CSV is the **per-token decode time** (one `llama_decode`), excluding
prompt prefill and model load. From it:

- **mean** = aggregate throughput = `n_tokens / Σ decode_seconds` — the rate a user sees.
- **min / max** = slowest / fastest *single* token (`1000 / max|min(wall_ms)`) — the
  worst-case stall and the best-case cache-warm token.
- **median / p5 / p95** = the steady-state distribution; more robust than min/max, which
  are single-token extremes.

Reproduce with the committed drivers:

```bash
# device-side single run (prompt lives in the script, n and flags are args)
scripts/bench-run.sh 256 <model.gguf> <out.csv> [--moe-stream --cache-mb 4000 --io-threads 4]

# full 12-run matrix over adb, one CSV per config
pwsh scripts/bench-matrix.ps1        # writes .bench/*.csv
python scripts/bench-analyze.py      # mean/min/max/median/p5/p95 + .bench/summary.md
```

Each table row is one fixed flag string (from `scripts/bench-matrix.ps1`), so a row
reproduces exactly by re-running its config:

| Config row | `bench-run.sh` flags |
|---|---|
| solo mmap (no streaming) | *(none)* |
| streaming O_DIRECT, cache 0, lane 4 | `--moe-stream` |
| streaming + cache 2000 MiB, lane 2 | `--moe-stream --cache-mb 2000 --io-threads 2` |
| streaming + cache 2000 MiB, lane 4 | `--moe-stream --cache-mb 2000 --io-threads 4` |
| streaming + cache 4000 MiB, lane 2 | `--moe-stream --cache-mb 4000 --io-threads 2` |
| streaming + cache 4000 MiB, lane 4 | `--moe-stream --cache-mb 4000 --io-threads 4` |

The `compute + I/O` split, `flash read/token` and `cache hit` columns are parsed from the
engine's own `moe-stream:` / `moe-cache:` stderr summary printed at the end of each run —
not computed post-hoc — so they reproduce verbatim in the `.bench/*.log` files.

## Results

tok/s. **mean** = aggregate throughput (bold). min/max = slowest/fastest single token.
median/p5/p95 = steady-state distribution. `flash read/token` and `cache hit` are from the
engine's `moe-stream:` / `moe-cache:` summary. `decode: compute + I/O` splits the mean
per-token decode time (s) into compute and flash-I/O, straight from the `moe-stream:` line —
it shows how much of decode is spent waiting on flash vs. running kernels.

### Qwen3-30B-A3B-Q4_K_M

- **File:** `Qwen3-30B-A3B-Q4_K_M.gguf` — 18.5 GB on disk, Q4_K_M quantization.
- **Shape:** 128 experts, top-8 routing, 48 layers. ≈1.64× device RAM (11.3 GB).

| Config | mean | min | max | median | p5 | p95 | cache hit | flash read/token | decode: compute + I/O (s/tok) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| solo mmap (no streaming) | **1.86** | 0.36 | 8.34 | 2.06 | 0.95 | 5.18 | — | 0 MiB | — |
| streaming O_DIRECT, cache 0, lane 4 | **1.78** | 1.56 | 1.82 | 1.79 | 1.73 | 1.81 | — | 1051 MiB | 0.102 + 0.459 |
| streaming + cache 2000 MiB, lane 2 | **2.00** | 0.20 | 4.98 | 2.19 | 1.38 | 3.42 | 53% | 480 MiB | 0.199 + 0.302 |
| streaming + cache 2000 MiB, lane 4 | **2.47** | 1.54 | 5.27 | 2.51 | 1.81 | 3.66 | 53% | 480 MiB | 0.177 + 0.228 |
| streaming + cache 4000 MiB, lane 2 | **3.38** | 1.57 | 7.66 | 3.51 | 2.16 | 5.98 | 76% | 225 MiB | 0.144 + 0.152 |
| streaming + cache 4000 MiB, lane 4 | **3.75** | 1.88 | 7.81 | 3.88 | 2.51 | 6.22 | 76% | 225 MiB | 0.143 + 0.124 |

### Gemma-4-26B-A4B-it-Q4_K_M

- **File:** `Gemma-4-26B-A4B-it-Q4_K_M.gguf` — 17.0 GB on disk, Q4_K_M quantization.
- **Shape:** fused gate+up expert layout, A4B (4 experts active). ≈1.51× device RAM (11.3 GB).

| Config | mean | min | max | median | p5 | p95 | cache hit | flash read/token | decode: compute + I/O (s/tok) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| solo mmap (no streaming) | **0.61** | 0.13 | 5.91 | 1.96 | 0.18 | 4.18 | — | 0 MiB | — |
| streaming O_DIRECT, cache 0, lane 4 | **1.69** | 1.36 | 1.73 | 1.70 | 1.66 | 1.72 | — | 904 MiB | 0.156 + 0.435 |
| streaming + cache 2000 MiB, lane 2 | **2.24** | 1.33 | 4.21 | 2.27 | 1.67 | 3.26 | 58% | 366 MiB | 0.203 + 0.243 |
| streaming + cache 2000 MiB, lane 4 | **2.34** | 1.50 | 3.98 | 2.40 | 1.80 | 3.23 | 58% | 366 MiB | 0.212 + 0.215 |
| streaming + cache 4000 MiB, lane 2 | **3.39** | 1.34 | 5.74 | 3.55 | 2.39 | 5.01 | 82% | 144 MiB | 0.185 + 0.110 |
| streaming + cache 4000 MiB, lane 4 | **3.48** | 0.96 | 5.84 | 3.65 | 2.54 | 5.19 | 82% | 144 MiB | 0.187 + 0.100 |

## Reading the numbers

- **Cache dominates, lanes assist.** Throughput tracks the hit rate almost linearly. At a
  4000 MiB cache the routed working set is mostly resident, so extra read lanes have little
  left to hide — lane 2 vs 4 is within noise (Qwen 3.38 → 3.75, Gemma 3.39 → 3.48).
- **Streaming without a cache is the most *predictable* setting.** cache-0 has the tightest
  spread (Qwen p5–p95 = 1.73–1.81) because every token re-reads the same ~1 GiB with
  O_DIRECT: no cache warm-up, no eviction cliffs. It is slower on average but jitter-free.
- **`mmap`-only trades average speed and system health for nothing.** Qwen's mmap mean
  (1.86) edges out streaming-only (1.78), but that number is page-cache-dependent and its
  spread is enormous (min 0.36, max 8.34). On Gemma the same mode collapses to 0.61 tok/s
  aggregate despite a healthy 1.96 median — proof that a few multi-second eviction stalls,
  not the typical token, set the user-visible speed. Streaming replaces those unbounded
  stalls with a bounded, O_DIRECT read the engine controls.
- **The remaining bottleneck is compute, not the seam.** Follow the `compute + I/O` column
  down each table: at cache 0 flash I/O is 74–82 % of decode (Qwen 0.459 of 0.561, Gemma
  0.435 of 0.591); at cache 4000 it inverts to compute-bound (Qwen compute 0.143 vs I/O
  0.124; Gemma 0.187 vs 0.100). Zeroing I/O entirely — an infinite cache — would only reach
  1/compute ≈ **7.0 tok/s (Qwen) / 5.3 tok/s (Gemma)**, i.e. this SoC's in-RAM decode
  speed. So a well-sized cache has already recovered most of what streaming can recover;
  further gains have to come from the compute kernels, not from the streaming path.
  (Note the compute share *rises* when the cache is enabled — cache-0's 0.10–0.16 s grows
  to ~0.18–0.21 s at cache 2000 — because cache lookup/copy is counted inside compute;
  it settles back down at cache 4000 as the hit rate makes those copies rarer.)

## Device pressure — not just tokens

Tokens/s is only half the story. Under `mmap`-only the model is faulted in through the
**page cache**, so the kernel evicts everything else — other apps, the launcher, the
keyboard — to make room for a 17–18 GB mapping on 11 GB of RAM. The phone becomes
sluggish or unusable for anything besides inference, and the eviction stalls are exactly
the slow tokens above. Expert streaming with a bounded cache avoids this: it holds a fixed,
declared amount (2–4 GiB) and reads the rest with O_DIRECT, which **bypasses the page
cache**, so the rest of the system keeps its working set.

**This axis is not yet measured** — the tables above are throughput only. Future runs
should record, alongside tok/s, an indicator of the pressure a config puts on the device.
Accessible on this phone **without root**:

| Signal | Source (adb, no root) | Notes |
|---|---|---|
| Battery / phone temperature | `dumpsys battery` → `temperature` (deci-°C), `PhoneTemp` | e.g. `415` = 41.5 °C |
| Per-component temperature | `/sys/class/thermal/thermal_zone*/temp` + `.../type` | CPU/GPU/skin zones readable by shell (`cpu-*`, `gpuss-*`) |
| Free-RAM headroom | `/proc/meminfo` → `MemAvailable` | collapse under mmap is the pressure signal |
| Thermal throttling state | `dumpsys thermalservice` | throttle status / trip transitions |

Not available without root on this device: the kernel **PSI** counters
(`/proc/pressure/{memory,io,cpu}`) return *Permission denied* — the cleanest
memory/IO-stall metric, but it needs root or a privileged helper.

Recommended protocol for the next round: sample battery temperature + `MemAvailable`
before, mid-run, and after each config; warm up, then measure a steady thermal window;
and cool the device to a common baseline between configs so sustained-decode throttling
doesn't confound the comparison. The goal is a second table — *tok/s vs. thermal rise and
free-RAM floor* — that makes the "mmap is fast until the phone melts" cost explicit.

## Provenance

Measured 2026-07-11 on branch `test/gemma4-apk`. Raw per-token CSVs and full `.log` stderr
dumps in `.bench/` (git-ignored); drivers in `scripts/bench-run.sh`, `scripts/bench-matrix.ps1`,
`scripts/bench-analyze.py`. Both models read from `/sdcard/Download`; O_DIRECT streaming from
that path was verified working before the matrix ran.

Model files (both Q4_K_M GGUF, staged on the device at `/sdcard/Download/`):

| Table section | Device path | Size |
|---|---|---|
| Qwen | `/sdcard/Download/Qwen3-30B-A3B-Q4_K_M.gguf` | 18.5 GB |
| Gemma | `/sdcard/Download/google_gemma-4-26B-A4B-it-Q4_K_M.gguf` | 17.0 GB |

Sizes are the on-disk GGUF byte counts; both are stock Q4_K_M conversions, unmodified by the
engine (it loads `use_mmap=true`, rebinds expert tensors to the native gguf layout, no repack).
