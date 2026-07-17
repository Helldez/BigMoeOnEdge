# gpt-oss-120b — a 58 GB model at 5.2× device RAM

The same engine streams **OpenAI gpt-oss-120b** — a 58.46 GB MoE — on the same 11.3 GB phone.
That is **5.2× device RAM**: the model cannot be held resident by any means, and to our knowledge
this is the first time a 120B / 58 GB model has generated tokens on a phone at all.

This is also the model where the engine's newest lever pays the most. Sending the **dense
(non-expert) weights through O_DIRECT** instead of leaving them in the page cache
(`--dense-weights anon`) takes gpt-oss from **0.687 tok/s to 2.191 tok/s** — a **3.2×** — and the
mechanism is visible in the telemetry rather than inferred. See [Why the dense policy is the
whole story here](#why-the-dense-policy-is-the-whole-story-here).

## Environment

| | |
|---|---|
| Device | OnePlus 15R (`CPH2769`), Android 16, 11.3 GB RAM — same as [benchmarks.md](benchmarks.md) |
| Model | `gpt-oss-120b-Q4_K_M.gguf` — 58.46 GB, 36 layers, 128 experts, **top-4** default, MXFP4 expert weights |
| Device path | `/data/local/tmp/shardllm/` — the real `/data` partition, **required** for working O_DIRECT (`/sdcard` is FUSE and silently falls back to buffered) |
| Fraction of RAM | ≈**5.2×** (58.46 GB / 11.3 GB) — resident load is impossible, so there is no in-RAM baseline, only `mmap` page-cache thrash |
| Decoding | greedy — deterministic, so two cells at the same top-k do identical work and read identical bytes |

## Current numbers (256-token steady state)

Measured 2026-07-17 on `main` @ `1419af3`. Every row: `--moe-stream --overlap --dense-weights anon
-t 4 --no-think`, 256 tokens, one fixed prompt, over `adb shell`. `compute` and `flash read/token`
come from the engine's `moe-stream:` line, `cache hit` from `moe-cache:`, `majflt/token` from its
`compute:` line.

| top-k | expert cache | lanes | tok/s | s/token | compute (s/tok) | stall (s/tok) | flash read/token | cache hit | majflt/token |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **2** | **2000 MiB** | **8** | **2.191** | 0.456 | 0.156 | 0.217 | 590.3 MiB | 32.0 % | 10 |
| 2 | off | 4 | 1.790 | 0.559 | 0.242 | 0.316 | 908.5 MiB | — | 6 |
| 4 (default) | 2000 MiB | 8 | 1.300 | 0.769 | 0.288 | 0.298 | 1292.4 MiB | 27.1 % | — |
| 4 (default) | 2000 MiB | 4 | 0.998 | 1.002 | 0.436 | 0.400 | 1292.4 MiB | 27.1 % | 314 |
| 4 (default) | off | 4 | 0.711 | 1.407 | 0.948 | 0.457 | 1817.0 MiB | — | 7 |

**These cells were not all measured from the same device state, and the differences matter.** The
phone does not return to baseline between 58 GB runs (see [Method — cooldown](#method)); each
cell's entry state was logged, and the honest reading is:

- The **k=2 / cache 2000 / lane 8** row — the headline — entered at `scaling_max_freq` **1.55 GHz**
  against the 2.27 GHz the cache-off rows enjoyed. It is a **throttled floor**: at equal clock it
  would read higher, not lower.
- **The lane 4 vs lane 8 pair is confounded and no lane recommendation is drawn from it here.** The
  lane-4 cell entered with its CPU at 52.9 °C and peaked at 93.8 °C; the lane-8 cell entered at
  44.4 °C and peaked at 70.2 °C. (Battery temperature suggested the opposite ordering — it lags
  behind the SoC and is the wrong sensor for this.) The two read *identical* bytes (1292.4
  MiB/token) at an identical 27.1 % hit, as greedy decode requires, so the gap is entirely
  execution-side — but thermal state, not lane count, is the uncontrolled variable. A cold re-run
  is owed.

Raw CSVs, `.metrics`, generated text and the drivers: [`bench-data/2026-07-17/`](bench-data/2026-07-17/).

**In the demo app**, the headline config (cache 2000, 8 lanes, O_DIRECT for both dense and experts,
k=2) reports **1.91 tok/s** — ~13 % below the 2.191 above. That is the expected protocol gap (short
turns, co-resident UI, live device state), not an app defect; see the README's
[What to expect in the app](../README.md#what-to-expect-in-the-app).

## Why the dense policy is the whole story here

Expert streaming was never the problem on gpt-oss. **The dense weights were.**

The engine streams experts through O_DIRECT into a bounded cache, so the 58 GB expert bank never
lands in RAM. But the *non-expert* weights — embeddings, attention, norms, lm_head — are
mmap-resident by default, held in the page cache. At 1.6× RAM there is room for them. At **5.2×
RAM there is not**: the kernel reclaims them mid-decode, and every token faults them back in from
flash, one 4 KiB page at a time. That fault storm is not visible as I/O in the engine's own
accounting — the kernel services it inside the compute call — so it shows up as *compute*, and it
is why gpt-oss looked hopelessly compute-bound.

`--dense-weights anon` reads those weights via O_DIRECT into the engine's own anonymous buffers and
rebinds the tensors onto them. They are no longer file-backed, so a reclaim sends them to zram
rather than dropping them to be re-read from flash. The measurement:

| dense policy | majflt/token (gpt-oss) |
|---|---:|
| page-cached (`warm`, previous default) | 314 – 1894 |
| **O_DIRECT anon** | **6 – 10** |

Two orders of magnitude. And the consequence lands where the theory says it should — in the
compute column: **0.948 → 0.156 s/tok** across the old and new best configs. The kernels never
changed; the fault service inside them went away.

This also **overturns the previous cache-off recommendation for this model.** With the dense set in
the page cache, an expert cache competed with it for the same scarce RAM, so a bounded cache lost
to no cache at all and `cache-off` was the ceiling (see [pressure.md](pressure.md)). With the dense
set out of the page cache that competition is gone, and a **2000 MiB cache beats cache-off**: 0.998
vs 0.711 tok/s at the same lane count — while running at 1.9 GHz against the cache-off cell's
2.27 GHz and entering with a hotter CPU. It wins carrying both handicaps.

On budget choice: 2000 MiB is the smallest value that clears both constraints — above
`cache_min_mb` (1500, below which `validate()` rejects the budget) and above the **measured 1815
MiB/token** working set at k=4. A budget under one token's working set evicts what it just read and
returns a 0 % hit; that is the pathological band, and it is rejected rather than offered.

## top-k is still the dominant lever

Each gpt-oss expert is large (d_ff 2880 — several × a Qwen expert), so halving the routing width
halves both the MAC count and the streamed bytes:

| top-k | tok/s (cache 2000, lane 8) | flash read/token | token working set |
|---:|---:|---:|---:|
| 4 (default) | 1.300 | 1292.4 MiB | 1815 MiB |
| **2** | **2.191** | 590.3 MiB (−54.3 %) | 908 MiB |

**+68.5 %** for k=2 — and the k=2 cell was the throttled one (1.55 GHz vs 1.9 GHz), so the real
margin is wider. The flash cut (−54.3 %) tracks the routing cut, as it must.

## vs `mmap`

A plain `mmap` load of the same file, measured 2026-07-14, decodes at **11.24 s/token** (k=2) and
**13.34 s/token** (k=4): faulting 58 GB through an 11 GB page cache thrashes on every token. Against
the current best (0.456 s/token) that is **~25×**. Treat the ratio as indicative rather than a
matched A/B — the mmap rows are 24-token probes from an older build, kept because re-measuring a
13 s/token baseline for 256 tokens costs an hour of device time to reconfirm a number nobody
should use.

## Quality — the cost of dropping reasoning

All rows use `--no-think`. gpt-oss uses the harmony format, whose template **always** opens an
`analysis` (chain-of-thought) channel; a plain run spends its whole budget reasoning before it
answers, so a throughput probe would be timing analysis tokens. `--no-think` primes the `final`
channel directly and the model answers immediately.

That is a **latency mode, not a free lunch** — it removes the model's scratch space. Measured on
`17 × 23` (greedy, so the answer depends only on k; identical under streaming and `mmap`):

| top-k | `17 × 23 =` | capital of Australia |
|---:|---|---|
| 2 | **391** ✅ | Canberra ✅ |
| 3 | **391** ✅ | Canberra ✅ |
| 4 (default) | **387** ❌ | Canberra ✅ |

The *default* top-4 gets the arithmetic **wrong** while the narrower k=2/3 get it right. This is not
"smaller k is smarter" — without the analysis channel there is no scratch space to compute 17 × 23,
so the answer is a one-shot guess whose correctness is prompt- and k-specific. Use `--no-think` for
direct-answer UX and for benchmarking decode speed; for arithmetic or any multi-step task, drop it
and let gpt-oss spend analysis tokens. On the long-essay prompt used for the throughput rows above,
both k=2 and k=4 stayed coherent and well-structured — one prompt, not a quality benchmark.

## Method

Same protocol as [benchmarks.md](benchmarks.md): 256 tokens, one fixed prompt, greedy, `--csv` for
per-token metrics, `-t 4`. Two gpt-oss-specific requirements:

- **Read from the real `/data` partition.** `/sdcard` is FUSE; O_DIRECT silently falls back to
  buffered there, which quietly deletes the thing being measured.
- **Cool on a condition, not a timer.** A 58 GB run leaves the device hot and its free RAM
  depressed for *minutes*; a fixed sleep between cells benchmarks the run order. Gate each cell on
  CPU temperature and free RAM coming back under a threshold, log the entry state
  (`scaling_max_freq`, CPU temp, `MemAvailable`) with the result, and treat any cell that missed
  the gate as a floor. See [benchmark-method.md](benchmark-method.md).

## Provenance

Current numbers measured 2026-07-17 on `main` @ `1419af3`, arm64 (NDK r29,
`armv8.2-a+dotprod+fp16`); data and drivers in [`bench-data/2026-07-17/`](bench-data/2026-07-17/),
with per-cell confounds recorded in its `NOTES.md`.

The **superseded** exploratory sweep (2026-07-14, `bmoe-cli` @ `4d12b75`, 24-token probes at
`--cache-mb auto --cache-ceil-mb 3000` with the dense weights page-cached — best **0.687 tok/s**)
and the two `mmap` baselines live in [`bench-data/2026-07-14/`](bench-data/2026-07-14/). That sweep
also concluded `prefetch=4` always regresses on this model (it reads 20–25 % more per token
speculatively and only 12–15 % of it is used); nothing since has challenged that, and prefetch stays
off here. Its lane-4-beats-lane-8 finding is **not** carried forward — it was measured in a regime
(24-token probes, dense weights page-cached) that no longer describes how this model runs.

| Model | Device path | Size |
|---|---|---|
| gpt-oss-120b | `/data/local/tmp/shardllm/gpt-oss-120b-Q4_K_M.gguf` | 58.46 GB |
