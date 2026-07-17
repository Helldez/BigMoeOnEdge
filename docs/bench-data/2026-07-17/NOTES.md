# 2026-07-17 — all-O_DIRECT dense weights (`--dense-weights anon`)

Captured against `main` @ `1419af3` + a fresh arm64 build of the CLI (NDK r29,
`armv8.2-a+dotprod+fp16`). Everything here runs the dense (non-expert) weights through
**O_DIRECT into anonymous buffers** instead of leaving them mmap'd in the page cache — the axis
this session set out to measure. Device: OnePlus 15R, Android 16, 11.3 GB RAM. All runs are
256-token greedy decode over `adb shell`, `--overlap` on, `-t 4`.

## What the runs are

| File prefix | Model | Config |
|---|---|---|
| `qwen_kdef` / `qwen_k6` | Qwen3-30B-A3B-Q4_K_M | `--cache-mb 4000 --io-threads 4`, top-k 8 (default) / 6 |
| `gemma_kdef` / `gemma_k6` | Gemma-4-26B-A4B-it-Q4_K_M | `--cache-mb 4000 --io-threads 4`, default / 6 |
| `gpt_kdef` / `gpt_k2` | gpt-oss-120b-Q4_K_M | `--cache-mb 0 --io-threads 4 --no-think`, top-k 4 (default) / 2 |
| `gpt_c2000_l4` / `gpt_c2000_l8` | gpt-oss-120b-Q4_K_M | `--cache-mb 2000 --no-think`, top-k 4, lanes 4 / 8 |
| `gpt_c2000_l8_k2` | gpt-oss-120b-Q4_K_M | `--cache-mb 2000 --io-threads 8 --no-think`, top-k 2 |

Drivers as run: `driver-matrix.sh` (the 6-cell matrix), `driver-lanes.sh` (the lane pair),
`bench-run-doc.sh` (single run + device-pressure sampler; a copy of `scripts/bench-run.sh` that
keeps stderr so the effective `n_expert_used` is on the record). `driver-lanes.log` /
`driver-k2.log` carry each cell's logged entry state.

## Results

| Run | tok/s | flash read/token | cache hit | compute s/tok | majflt/token |
|---|---:|---:|---:|---:|---:|
| `qwen_kdef` | 4.667 | 217.6 MiB | 77.1 % | 0.126 | 149 |
| `qwen_k6` † | 4.168 | 137.5 MiB | 80.0 % | 0.175 | 1189 |
| `gemma_kdef` † | 2.734 | 132.8 MiB | 82.9 % | 0.284 | 1894 |
| `gemma_k6` † | 3.792 | 98.2 MiB | 82.8 % | 0.200 | 1004 |
| `gpt_kdef` | 0.711 | 1817.0 MiB | — (cache 0) | 0.948 | 7 |
| `gpt_k2` | 1.790 | 908.5 MiB | — (cache 0) | 0.242 | 6 |
| `gpt_c2000_l4` ‡ | 0.998 | 1292.4 MiB | 27.1 % | 0.436 | 314 |
| `gpt_c2000_l8` ‡ | 1.300 | 1292.4 MiB | 27.1 % | 0.288 | — |
| `gpt_c2000_l8_k2` ‡ | **2.191** | 590.3 MiB | 32.0 % | 0.156 | 10 |

## Read these with the confounds — several cells are contaminated

**The device does not return to baseline between cells.** The 45 s cooldown that
`benchmark-method.md` prescribed when this session started is not enough: across the 6-cell
matrix the free-RAM floor and major-fault rate degraded monotonically with execution order and
throughput followed. Run 1 (`qwen_kdef`) entered at a 1.61 GB free-RAM floor and 149
majflt/token; run 3 (`gemma_kdef`) was down to a 0.63 GB floor, 1894 majflt/token, 88.8 °C.

† **Do not use these rows for a top-k claim.** The tell is arithmetic: `qwen_k6` reads 36.8 %
*less* flash than `qwen_kdef` (correct — fewer experts) yet its **compute rises**, 0.126 →
0.175 s/tok. Fewer active experts cannot make the same kernels slower; that is fault-service
time landing in the compute bucket, and majflt/token confirms it (149 → 1189). It inverts the
documented Turbo top-k effect (+24 % published, −10.7 % here) purely because the k=6 cell ran
second. `gemma_*` ran third and fourth, deeper into the same degradation.

‡ **The lane pair is confounded and no lane claim is drawn from it.** `gpt_c2000_l4` entered
with a **CPU at 52.9 °C** and peaked at **93.8 °C**; `gpt_c2000_l8` entered at 44.4 °C and
peaked at 70.2 °C. Battery temperature said the opposite (38.0 vs 40.7 °C) — it lags, and it is
the CPU sensor that governs compute throttling, so read the CPU one. `gpt_c2000_l8_k2` entered
at `scaling_max_freq` = **1 555 200** (1.55 GHz) against 2 265 600 (2.27 GHz) for the pass-1
cells, i.e. it is a throttled floor, not a peak.

## What survives the confounds

Claims below hold because the contamination pushes *against* them:

- **`--dense-weights anon` collapses the fault storm.** majflt/token on gpt-oss drops to **6–10**
  against 314–1894 for the cells whose dense set is page-cached. This is a mechanism, not a
  correlation.
- **A 2000 MiB cache beats cache-off on gpt-oss** once the dense weights are out of the page
  cache: `gpt_c2000_l4` (0.998) vs `gpt_kdef` (0.711) at the same lane count — and the cache cell
  ran at 1.9 GHz vs 2.27 GHz and entered with a hotter CPU. It wins carrying both handicaps.
  2000 MiB is the smallest budget that is both above `cache_min_mb` (1500) and above the measured
  1815 MiB/token working set at k=4; 1000 MiB was not run because it sits below both and the
  engine rejects that band.
- **2.191 tok/s is the highest throughput measured on gpt-oss-120b on this device**, and it was
  measured while throttled to 1.55 GHz — a floor.

`qwen_kdef` (4.667) is clean: it ran first, on the coolest device of the session. It does not beat
the all-time Qwen best (5.23 tok/s, capped-auto cache, `2026-07-13/`) — expected, since Qwen at
1.64× RAM has far less dense-fault pressure for this policy to remove than gpt-oss at 5.2×.

## Owed

A cold-device re-measure of the Qwen and Gemma top-k pairs, and of the gpt-oss lane pair, each
gated on a **measured condition** (CPU temperature and free RAM back under a threshold) rather
than a fixed sleep, with the entry state logged per cell.
