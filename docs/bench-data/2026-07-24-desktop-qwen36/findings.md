# Desktop (x86) streaming — Qwen3.6-35B on a 16 GB laptop (2026-07-24)

**Verdict: the engine runs unmodified on a Windows x86 laptop, and the bottleneck flips.
On the phone streamed decode is I/O-bound; here it is DRAM-bandwidth-bound in compute
(~0.11 s/token floor in every cell → a ~9 tok/s ceiling at zero I/O). io lanes and compute
threads are both dead levers — the live ones are cache budget, `--drop-cold-experts`, and
`--overlap`. Best recipe: `--overlap --cache-mb auto --drop-cold-experts 0.75` = 7.33 tok/s,
+55% over the 4.78 baseline and 80% of the DRAM ceiling.**

## Setup

Host build (`bmoe-cli.exe`, Release, engine at 0.15.1), Windows 11, an 8-core x86-64 laptop
(2021 class): 14.8 GiB usable RAM, dual-channel DDR4, NVMe SSD (~3 GB/s class).
`Qwen3.6-35B-A3B-Q4_K_M` (22.3 GB ≈ 1.5× RAM, `qwen35moe`, 40 layers, 256 experts, top-k 8).
Same prompt everywhere, greedy, 256-token generations, 60 s between cells, nothing else
running. Defaults unless stated: `--moe-stream --dense-weights anon --io-threads 4 -t 4`,
overlap and prefetch off. Single runs, not best-of — treat absolute numbers as indicative.

## Round 1 — exploratory (cache auto): a confound, caught

| Cell | tok/s | compute s/tok | I/O s/tok | hit | cache budget |
|---|---:|---:|---:|---:|---:|
| A baseline io4·t4 | 4.78 | 0.116 | 0.083 | 84.5% | 5281 MiB |
| B io8 | 5.61 | 0.116 | 0.057 | 89.0% | 7128 MiB |
| C drop-cold 0.75 | 6.82 | 0.112 | 0.032 | 92.4% | 7420 MiB |
| D t8 | 6.14 | 0.107 | 0.050 | 89.3% | 7305 MiB |

`--cache-mb auto` sized itself differently per run (5.3–7.4 GiB, tracking free RAM), so B and
D are **confounded**: their gains could be the varied knob or the extra cache. Round 2 pins it.

## Round 2 — fixed cache (`--cache-mb 5000`): lanes and threads are dead

| Cell | tok/s | compute s/tok | I/O s/tok | hit |
|---|---:|---:|---:|---:|
| E io4·t4 | 4.72 | 0.115 | 0.088 | 83.5% |
| F io8·t4 | 4.63 | 0.117 | 0.089 | 83.5% |
| G io4·t8 | 4.68 | 0.113 | 0.089 | 83.3% |
| H io8·t8·drop 0.75 | 6.33 | 0.107 | 0.046 | 89.1% |

- **Lanes: dead.** io8 = io4 to the millisecond; aggregate read bandwidth is pinned at
  ~900 MiB/s on a ~3 GB/s disk with either lane count. B's +17% was entirely the cache budget.
  The phone-side "flash saturates at 2 lanes" finding holds on NVMe too, for this read pattern.
- **Threads: dead.** Doubling compute threads moves compute 0.115 → 0.113. The GEMV work is
  limited by DRAM bandwidth, not cores — which is also why the ~0.11 s/token compute floor is
  identical in every cell of both rounds.

## Round 3 — overlap

| Cell | tok/s | compute s/tok | I/O stall s/tok | hit |
|---|---:|---:|---:|---:|
| I overlap, cache 5000 | 5.14 | 0.115 | 0.068 | 83.5% |
| **J overlap + cache auto + drop 0.75** | **7.33** | 0.107 | 0.026 | 92.3% |

Overlap hides part of the I/O behind FFN compute (+9% on the plain baseline, not all of it —
at 83% hit there is more I/O than compute to hide it behind). On the full recipe the residual
stall is 0.026 s/token: the token costs 0.136 s of which 0.107 is the compute floor. With
overlap on, the reported flash bandwidth drops to ~330 MiB/s — the async reads compete with
compute for the same DRAM bandwidth, which is the wall itself showing up in a second gauge.

## What this changes

- Desktop >RAM streaming works out of the box and beats the phone on the same model
  (7.3 vs 5.0–5.8 tok/s), but for the opposite reason the phone numbers happen: I/O is
  nearly free to fix, compute is not.
- To break the ~9 tok/s ceiling on this class of machine the levers are outside today's
  scope: dense weights on a GPU, or a lower expert quantization.
- `--drop-cold-experts 0.75` transfers cleanly from the phone (+43% alone, 6% of routings
  dropped at auto cache). Quality was not re-measured here; the phone-side GSM8K result is
  the only quality evidence.

## Files

`pc-A…D` are round 1 (cache auto), `pc-E…H` round 2 (cache 5000), `pc-I/J` round 3 (overlap).
The `# moe_stream=…` preamble in each CSV records the exact configuration.
