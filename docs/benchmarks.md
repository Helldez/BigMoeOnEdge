# Benchmarks — Android (OnePlus 15R)

Measured throughput of the expert-streaming engine on a phone whose RAM is smaller than
the model, across the full configuration matrix, for two MoE families. These are the
numbers the README table and the headline claim are drawn from; the how-to lives in
[benchmark-method.md](benchmark-method.md).

For the far end of the ratio — a 58 GB model at 5.2× device RAM — see
[benchmarks-gpt-oss.md](benchmarks-gpt-oss.md).

## TL;DR

- Streaming a >RAM MoE model is **stable and usable**: ~1.6–1.7 tok/s with no cache, and up
  to **3.98 tok/s (Qwen) / 2.78 tok/s (Gemma)** with the best viable cache plus intra-layer
  I/O–compute overlap — on an 11 GB phone holding an 18.5 GB (Qwen) or 17.0 GB (Gemma) model,
  in the 2026-07-12 matrix below. Later sessions on a cooler device read higher on the same
  levers (**5.23 tok/s** Qwen with a capped-auto cache, **5.01 / 4.99 tok/s** at k=6); the spread
  between those sessions is device state, not code, which is why each table is compared only
  against its own baseline.
- **Expert-cache size is the dominant lever.** Larger cache raises the hit rate and collapses
  flash read per token (Qwen 480 MiB at cache 2000 → 225 MiB at cache 4000, hit 53 % → 76 %;
  Gemma 366 MiB at cache 2000, hit 58 %). Qwen nearly doubles from cache 2000 → 4000; Gemma
  cannot reach cache 4000 on this device (OOM — see the Gemma table note), so it tops out at
  cache 2000.
- **Intra-layer overlap is the second lever, but only over a warm cache.** Pipelining each
  layer's expert reads with its compute (`--overlap`) lifts the best config on both models —
  Qwen 3.47 → **3.98**, Gemma 2.24 → **2.78** — by hiding the residual flash wait behind FFN
  compute (the stall drops to 0.06 s/tok on Qwen). Over a cold cache-0 stream it is a **loss**
  (Qwen 1.71 → 1.27) because there is far more I/O than compute can mask.
- **Parallel read lanes are a secondary lever.** Lane 2 → 4 adds ~15–20 % at a 2000 MiB
  cache and little at 4000 MiB (once the cache absorbs most reads, decode is no longer
  I/O-bound).
- **`mmap`-only is a trap.** Its *median* token rate looks fine (Qwen 2.76 tok/s) but its
  *aggregate* throughput collapses (Gemma **0.36** tok/s) because a handful of
  page-cache-eviction stalls (single tokens as slow as 8 s) dominate the total — and during
  those stalls the phone is effectively unusable for anything else (see
  [Device pressure](#device-pressure--not-just-tokens)).
- **With a warm cache, decode is compute-bound, not I/O-bound.** The engine reports the
  decode split as `compute + flash I/O`. At Qwen's cache 4000 the serial flash I/O share is
  ~0.13 s/token against ~0.16 s of compute; with overlap the flash wait (stall) falls to
  ~0.06 s. Zeroing I/O entirely — an infinite cache — would only reach 1/compute ≈ **6.2 tok/s
  (Qwen)**, this SoC's in-RAM decode speed. The streaming path is no longer the bottleneck —
  the compute kernels are.

## Environment

| | |
|---|---|
| Device | OnePlus 15R (`CPH2769`), Android 16 |
| SoC / cores | Snapdragon-class, 8 cores, `arm64-v8a` |
| RAM | 11.3 GB (`MemTotal` 11 366 276 kB) |
| Storage | UFS 4.x, models read from `/sdcard/Download` (O_DIRECT verified working) |
| Engine | `bmoe-cli` built from `main` @ `f3371aa` (arm64, NDK r29, `armv8.2-a+dotprod+i8mm+fp16`) |
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

# full matrix over adb (8 configs × 2 models), one CSV per config
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
| streaming O_DIRECT + overlap, cache 0, lane 4 | `--moe-stream --cache-mb 0 --io-threads 4 --overlap` |
| streaming + cache 4000 MiB, lane 4, overlap | `--moe-stream --cache-mb 4000 --io-threads 4 --overlap` |

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
| solo mmap (no streaming) | **2.00** | 0.15 | 8.15 | 2.76 | 0.93 | 5.90 | — | 0 MiB | — |
| streaming O_DIRECT, cache 0, lane 4 | **1.71** | 0.50 | 1.81 | 1.78 | 1.62 | 1.80 | — | 1051 MiB | 0.123 + 0.462 |
| streaming + cache 2000 MiB, lane 2 | **2.01** | 1.17 | 4.66 | 2.04 | 1.42 | 3.17 | 53% | 480 MiB | 0.187 + 0.312 |
| streaming + cache 2000 MiB, lane 4 | **2.37** | 1.45 | 4.88 | 2.40 | 1.73 | 3.57 | 53% | 480 MiB | 0.181 + 0.241 |
| streaming + cache 4000 MiB, lane 2 | **3.12** | 1.24 | 6.66 | 3.30 | 2.01 | 5.46 | 76% | 225 MiB | 0.159 + 0.161 |
| streaming + cache 4000 MiB, lane 4 | **3.47** | 1.09 | 7.62 | 3.64 | 2.15 | 6.00 | 76% | 225 MiB | 0.161 + 0.127 |
| streaming O_DIRECT + overlap, cache 0, lane 4 † | **1.27** | 0.58 | 2.02 | 1.44 | 0.83 | 2.00 | — | 1051 MiB | 0.412 + 1.627 |
| **streaming + cache 4000 MiB, lane 4, overlap †** | **3.98** | 1.26 | 8.43 | 4.89 | 2.00 | 7.37 | 76% | 225 MiB | 0.193 + 0.300 |

† Overlap rows: I/O runs concurrently with compute, so the `compute + I/O` split no longer sums
to wall time — the I/O figure is the **sum of per-lane busy time** (it can exceed wall because
lanes read in parallel). The wall time compute actually lost to flash is the **stall**: 0.058 s/tok
for cache 4000 + overlap, 0.373 s/tok for cache 0 + overlap. So overlap on the 4000 MiB cache lifts
the best config from 3.47 → **3.98 tok/s** (median 3.64 → 4.89) by hiding almost all of the residual
flash wait behind FFN compute; overlap on a cold cache-0 stream is a net loss (1.71 → 1.27) because
there is too much I/O — 1.6 s/tok of lane-busy reads — to hide behind ~0.4 s of compute.

### Gemma-4-26B-A4B-it-Q4_K_M

- **File:** `Gemma-4-26B-A4B-it-Q4_K_M.gguf` — 17.0 GB on disk, Q4_K_M quantization.
- **Shape:** fused gate+up expert layout. The "A4B" label is ~4 **billion active parameters**, not
  4 active experts — the measured I/O ratio puts the default routing width at 8 (see the
  active-expert override section below). ≈1.51× device RAM (11.3 GB).

| Config | mean | min | max | median | p5 | p95 | cache hit | flash read/token | decode: compute + I/O (s/tok) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| solo mmap (no streaming) | **0.36** | 0.12 | 4.47 | 0.44 | 0.15 | 3.41 | — | 0 MiB | — |
| streaming O_DIRECT, cache 0, lane 4 | **1.61** | 1.08 | 1.68 | 1.62 | 1.57 | 1.66 | — | 904 MiB | 0.172 + 0.449 |
| streaming + cache 2000 MiB, lane 2 | **2.08** | 1.21 | 4.02 | 2.14 | 1.49 | 2.98 | 58% | 366 MiB | 0.225 + 0.256 |
| streaming + cache 2000 MiB, lane 4 | **2.24** | 1.19 | 3.86 | 2.29 | 1.67 | 3.09 | 58% | 366 MiB | 0.226 + 0.221 |
| streaming O_DIRECT + overlap, cache 0, lane 4 † | **1.81** | 1.50 | 1.90 | 1.83 | 1.73 | 1.86 | — | 904 MiB | 0.185 + 1.503 |
| **streaming + cache 2000 MiB, lane 4, overlap †** | **2.78** | 1.78 | 4.04 | 2.82 | 2.24 | 3.58 | 58% | 365 MiB | 0.237 + 0.540 |

**Cache 4000 MiB is borderline for Gemma on this device — it depends on the free RAM at launch.**
Gemma's 17.0 GB file, held resident through mmap, already fills most of the page cache; reserving a
further 4 GiB pinned expert cache on top can push `MemAvailable` to zero and get the run OOM-killed
by the Android low-memory-killer before it generates a token. In this session it did: the
successful cache-2000 runs held a ~3.8 GiB free-RAM floor, while a cache-4000 attempt started from
only ~2.3 GiB free and collapsed. So the matrix above tops Gemma out at **cache-2000 + overlap**,
and the two `cache 4000` rows are omitted rather than reported as failures. On a cooler device with
more headroom cache 4000 *does* fit — the Turbo top-k A/B below happens to have run Gemma at cache
4000 (81.7 % hit) — but it is not dependable here, so it is not the recommended steady setting.
Qwen (18.5 GB but only 3 B active, lighter page-cache footprint) sustains cache 4000 with a
~1.8 GiB floor, so its best row uses it.

† Overlap rows: I/O runs concurrently with compute, so the `compute + I/O` split no longer sums to
wall time — the I/O figure is the **sum of per-lane busy time** and exceeds wall. The wall time
compute actually lost to flash is the **stall**: 0.123 s/tok for cache 2000 + overlap, 0.367 s/tok
for cache 0 + overlap. Overlap lifts Gemma's best viable config from 2.24 → **2.78 tok/s**
(median 2.29 → 2.82); on the cold cache-0 stream it is a smaller gain (1.61 → 1.81) because most of
the 1.5 s/tok of lane reads cannot be hidden behind ~0.19 s of compute.

## Reading the numbers

- **Cache dominates, lanes assist.** Throughput tracks the hit rate almost linearly. As the
  cache absorbs more reads the routed working set goes mostly resident and extra read lanes
  have less left to hide — the lane 2 → 4 gain shrinks from ~18 % at cache 2000 (Qwen 2.01 →
  2.37) to ~11 % at cache 4000 (Qwen 3.12 → 3.47).
- **Overlap is the top of the stack, but only over a warm cache.** Pipelining reads with
  compute converts residual flash wait into hidden time: on Qwen's cache-4000 config it drops
  the per-token stall to 0.058 s and lifts mean 3.47 → **3.98** (median 3.64 → 4.89); on
  Gemma's cache-2000 config, 2.24 → **2.78**. On a cold cache-0 stream it *regresses* (Qwen
  1.71 → 1.27) — with 1.6 s/tok of lane reads and only ~0.4 s of compute, there is nothing to
  hide the I/O behind, and the overlap machinery's own stalls dominate.
- **Streaming without a cache is the most *predictable* setting.** cache-0 (serial) has the
  tightest spread (Qwen p5–p95 = 1.62–1.80) because every token re-reads the same ~1 GiB with
  O_DIRECT: no cache warm-up, no eviction cliffs. It is slower on average but jitter-free.
- **`mmap`-only trades average speed and system health for nothing.** Qwen's mmap mean
  (2.00) edges out streaming-only (1.71), but that number is page-cache-dependent and its
  spread is enormous (min 0.15, max 8.15). On Gemma the same mode collapses to 0.36 tok/s
  aggregate despite a 0.44 median — proof that a few multi-second eviction stalls, not the
  typical token, set the user-visible speed. Streaming replaces those unbounded stalls with
  a bounded, O_DIRECT read the engine controls.
- **The remaining bottleneck is compute, not the seam.** Follow the `compute + I/O` column
  down the Qwen table: at cache 0 flash I/O is ~79 % of decode (0.462 of 0.585); at cache
  4000 it inverts to compute-bound (compute 0.161 vs serial I/O 0.127, and with overlap the
  flash wait falls to a 0.058 s stall). Zeroing I/O entirely — an infinite cache — would only
  reach 1/compute ≈ **6.2 tok/s (Qwen)**, i.e. this SoC's in-RAM decode speed. So a well-sized
  cache plus overlap has already recovered most of what streaming can recover; further gains
  have to come from the compute kernels, not from the streaming path. (Note the compute share
  *rises* when the cache is enabled — cache-0's ~0.12 s grows to ~0.18 s at cache 2000 —
  because cache lookup/copy is counted inside compute; it settles back down at cache 4000 as
  the hit rate makes those copies rarer.)
- **The mean is a steady-state number; a fresh run warms up to it.** Each `--n 256` figure is
  the steady rate after the expert cache fills (hit 4.5% → ~77–83% over the first tokens), so a
  short generation reads slower than the table. The per-token trajectory — flat `compute_ms`
  with the warm-up carried entirely by the cache-hit climb, and hidden by overlap — is dissected
  in [warmup-analysis.md](warmup-analysis.md).

## Active-expert override (`--n-expert-used`) — Turbo top-k

`--n-expert-used N` lowers the model's top-k routing (e.g. 8 → 6) at load, via a llama.cpp
`kv_override` on the arch-prefixed `expert_used_count` metadata — no fork, no patch. Fewer
active experts cut both per-token compute *and* the streamed flash reads, at a quality cost
(it changes the output). This is a **matched A/B**: default vs `k=6`, same session, same
`--cache-mb 4000 --io-threads 4` config, back-to-back, and — the part that makes it usable —
**thermally comparable**, with the CPU peak within ~1–2 °C between the two rows, so the delta is
the override's alone and not a warm-vs-cold artefact. (A later attempt to re-measure this pair on a
device that had *not* returned to baseline inverted the result outright; see
[Dense-weight policy](#dense-weight-policy---dense-weights). Verify the thermal match before
trusting any top-k pair.) (These rows are measured fresh on a cool device, so their *default* mean is higher
than the 2026-07-11 matrix above, which is why the pair is compared to its own baseline, not
to that table.)

### Qwen3-30B-A3B-Q4_K_M — default (top-8) vs k=6

| Config | mean | min | max | median | p5 | p95 | cache hit | flash read/token | decode: compute + I/O (s/tok) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| default (k=8) | **4.03** | 1.55 | 9.34 | 4.19 | 2.65 | 7.08 | 76.5% | 224.65 MiB | 0.127 + 0.121 |
| **k=6** | **5.01** | 1.90 | 9.58 | 5.23 | 3.36 | 7.99 | 76.7% | 164.52 MiB | 0.106 + 0.093 |
| Δ | **+24.3%** | | | | | | +0.2 pt | **−26.8%** | −0.048 |

### Gemma-4-26B-A4B-it-Q4_K_M — default vs k=6

| Config | mean | min | max | median | p5 | p95 | cache hit | flash read/token | decode: compute + I/O (s/tok) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| default | **4.09** | 0.58 | 7.05 | 4.42 | 2.82 | 6.08 | 81.7% | 143.50 MiB | 0.150 + 0.095 |
| **k=6** | **4.99** | 1.18 | 7.77 | 5.19 | 3.61 | 7.05 | 82.8% | 97.81 MiB | 0.134 + 0.066 |
| Δ | **+22.1%** | | | | | | +1.1 pt | **−31.8%** | −0.045 |

> **On Gemma's default routing width:** the "A4B" label is ~4 **billion active parameters**,
> not 4 active experts — `k=6` reads 31.8 % *less* than default, which is only possible if the
> default routes well above 6. The exact `expert_used_count` should be confirmed from the gguf
> metadata; the I/O ratio (97.81 / 143.50 = 0.68) is consistent with a default of 8.

Device pressure captured on the same runs (peak process RSS / free-RAM floor / CPU start→max /
battery max): Qwen default 5.89 GB / 2.23 GB / 38.6→62.9 °C / 35.5 °C, Qwen k=6 5.08 GB /
2.09 GB / 38.6→62.1 °C / 34.2 °C; Gemma default 6.04 GB / 2.15 GB / 40.1→71.0 °C / 36.5 °C,
Gemma k=6 6.22 GB / 2.34 GB / 40.1→69.1 °C / 34.9 °C. Reducing k trims peak RSS and the thermal
rise slightly, as expected from the smaller working set and lower compute.

**On theory.** Qwen's −26.8 % flash read matches 6/8 = 0.75 (the small extra comes from the hit
rate holding), confirming its default is top-8. The throughput gain (+22–24 %) tracks the s/token
drop (−18 to −19 %): cutting ~25 % of the routed work removes ~25 % of the decode time.

**Quality.** Greedy decoding is deterministic, so a narrower top-k changes the argmax from the
first token whose routing differs — the text **diverges** from the default. On the fixed essay
prompt no degradation was observed at k=6: both models stayed coherent and factually accurate
(Qwen at k=6 even surfaced more specific history — the Antikythera mechanism, Al-Khwarizmi → the
word "algorithm"; Gemma produced the same chronological outline as its default). This is a single
prompt, not a quality benchmark — treat k=6 as a speed/quality knob to evaluate per use case,
not a free win.

Reproduce (feature branch `feat/expert-count-override`):

```bash
scripts/bench-run.sh 256 <model.gguf> <out.csv> <out.metrics> \
  --moe-stream --cache-mb 4000 --io-threads 4 --n-expert-used 6
```

Raw per-token CSVs, `.metrics`, and generated text for both the default and k=6 runs are in
`.bench-k6/` (git-ignored), measured 2026-07-13.

## Dense-weight policy (`--dense-weights`)

The tables above stream the *experts* from flash but leave the **non-expert** weights (embeddings,
attention, norms, lm_head) mmap-resident in the page cache. `--dense-weights anon` instead reads
them via O_DIRECT into the engine's own anonymous buffers and rebinds the tensors onto them, so a
reclaim sends them to zram rather than dropping them to be re-read from flash.

**How much it is worth depends entirely on the model-to-RAM ratio**, and the two families here sit
on the easy side of it:

| Model | × device RAM | majflt/token (`anon`) | tok/s (`anon`, cache 4000, lane 4, overlap, default k) |
|---|---:|---:|---:|
| Qwen3-30B-A3B | 1.64× | 149 | 4.667 |
| Gemma-4-26B-A4B | 1.51× | 1894 | 2.734 |
| gpt-oss-120b | **5.2×** | **6–10** | see [benchmarks-gpt-oss.md](benchmarks-gpt-oss.md) |

Qwen's 4.667 tok/s does **not** beat its all-time best (5.23 tok/s, capped-auto cache, no
dense-anon), and that is the expected result rather than a disappointment: at 1.64× RAM there is
enough headroom to hold the dense set, so there is little refault pressure for the policy to
remove. The lever is decisive only well past RAM — on gpt-oss at 5.2× RAM it is worth **3.2×** and
drops majflt/token by two orders of magnitude. Use `warm` (the default) near RAM; reach for `anon`
when `majflt/token` is in the hundreds.

> **The Qwen and Gemma cells from that session are order-contaminated and no top-k claim is drawn
> from them.** They ran with a 45 s cooldown that does not return this device to baseline, and
> throughput tracked execution order: `qwen_k6` read 36.8 % less flash than `qwen_kdef` (correct)
> yet its **compute rose** 0.126 → 0.175 s/tok, which is impossible for the same kernels and is in
> fact fault time (majflt/token 149 → 1189). It inverted the Turbo top-k result above to −10.7 %.
> The Turbo table stands; these rows do not replace it. Details and the per-cell entry states:
> [`bench-data/2026-07-17/NOTES.md`](bench-data/2026-07-17/NOTES.md).

## Device pressure — not just tokens

Tokens/s is only half the story. Under `mmap`-only the model is faulted in through the
**page cache**, so the kernel evicts everything else — other apps, the launcher, the
keyboard — to make room for a 17–18 GB mapping on 11 GB of RAM. The phone becomes
sluggish or unusable for anything besides inference, and the eviction stalls are exactly
the slow tokens above. Expert streaming with a bounded cache avoids this: it holds a fixed,
declared amount (2–4 GiB) and reads the rest with O_DIRECT, which **bypasses the page
cache**, so the rest of the system keeps its working set.

`scripts/bench-run.sh` now samples this axis alongside tok/s: peak process RSS, the
`MemAvailable` **floor** (its lowest point during the run), and CPU/battery temperature
before and at peak. All are read over adb **without root** (`dumpsys battery`,
`/sys/class/thermal/thermal_zone*/temp`, `/proc/meminfo`). Kernel **PSI**
(`/proc/pressure/*`) — the cleanest stall metric — returns *Permission denied* without root,
so it is not recorded. Runs are brought to a common baseline between configs so sustained-decode
throttling does not confound the comparison — a fixed sleep is **not** sufficient for that; see
[benchmark-method.md](benchmark-method.md#cool-on-a-condition-not-a-timer--and-log-the-entry-state).

**Qwen — device pressure**

| Config | peak RSS | free-RAM floor | CPU start → max | battery max |
|---|---:|---:|---:|---:|
| solo mmap | 5.87 GB | 5.76 GB | 39.0 → 64.8 °C | 35.1 °C |
| stream (cache 0, lane 4) | 6.13 GB | 4.62 GB | 42.8 → 66.8 °C | 36.6 °C |
| cache 2000, lane 4 | 5.62 GB | 3.58 GB | 44.8 → 62.9 °C | 38.9 °C |
| cache 4000, lane 2 | 5.97 GB | 2.01 GB | 45.5 → 63.3 °C | 39.4 °C |
| cache 4000, lane 4 | 5.56 GB | 1.79 GB | 48.2 → 73.7 °C | 39.4 °C |
| cache 4000, lane 4, overlap | 5.80 GB | 1.98 GB | 46.7 → 70.2 °C | 39.4 °C |

**Gemma — device pressure**

| Config | peak RSS | free-RAM floor | CPU start → max | battery max |
|---|---:|---:|---:|---:|
| solo mmap | 5.85 GB | 5.68 GB | 47.1 → 67.2 °C | 41.1 °C |
| stream (cache 0, lane 4) | 6.16 GB | 4.96 GB | 45.9 → 62.1 °C | 40.9 °C |
| cache 2000, lane 4 | 6.08 GB | 3.85 GB | 47.1 → 63.3 °C | 41.3 °C |
| cache 2000, lane 4, overlap | 6.06 GB | 4.11 GB | 53.2 → 73.7 °C | 45.5 °C |

The **free-RAM floor is the number that governs which cache size is usable.** Qwen's
cache-4000 configs run the device down to a ~1.8–2.0 GiB floor and still complete; Gemma at
cache 2000 keeps a comfortable ~3.9 GiB floor, but a cache-4000 attempt (not shown) started
from ~2.3 GiB free and was OOM-killed before its first token — the extra 2 GiB of pinned cache
is exactly what Gemma's heavier resident footprint cannot spare. Overlap costs a few extra °C
(more concurrent flash + compute) but does not change the memory floor. `mmap`-only keeps the
highest apparent floor because it *is* the page cache — but that is the mode that evicts every
other app, which the floor number does not capture.

## Provenance

Measured 2026-07-12 on `main` @ `f3371aa`. Raw per-token CSVs, `.metrics` sidecars and full
`.log` stderr dumps are committed under [`bench-data/2026-07-12/`](bench-data/2026-07-12/)
(the live `.bench/` working dir is git-ignored); drivers in `scripts/bench-run.sh`,
`scripts/bench-matrix.ps1`, `scripts/bench-analyze.py`. Both models read from `/sdcard/Download`;
O_DIRECT streaming from that path was verified working before the matrix ran. The two Gemma
`cache 4000` configs are absent by design — they OOM on this device (see the Gemma throughput
table note).

Model files (both Q4_K_M GGUF, staged on the device at `/sdcard/Download/`):

| Table section | Device path | Size |
|---|---|---|
| Qwen | `/sdcard/Download/Qwen3-30B-A3B-Q4_K_M.gguf` | 18.5 GB |
| Gemma | `/sdcard/Download/google_gemma-4-26B-A4B-it-Q4_K_M.gguf` | 17.0 GB |

Sizes are the on-disk GGUF byte counts; both are stock Q4_K_M conversions, unmodified by the
engine (it loads `use_mmap=true`, rebinds expert tensors to the native gguf layout, no repack).
