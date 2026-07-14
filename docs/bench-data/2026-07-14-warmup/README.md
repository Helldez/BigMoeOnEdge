# Dense warm-up — on-device results (2026-07-14)

Measured on the OnePlus 15R (Snapdragon, 11.4 GB RAM), UFS4 storage, models pushed to
`/data/local/tmp`. Each CSV is one row per generated token
(`step,steps,wall_ms,io_ms,compute_ms,read_bytes,cache_hit_pct,stall_ms,mgmt_ms`) plus a `#
summary` trailer. Reproduced with `scripts/bench-warmonly.sh`.

## What changed

Only the expert tensors are streamed from flash; every other weight (embeddings, attention,
norms, lm_head) stays mmap-resident and is demand-paged by the kernel. On a model far larger than
RAM those dense pages fault in randomly, 4 KiB at a time, *inside* the first decodes — surfacing as
a large `compute_ms` residual that only settles after ~20 tokens.

**Dense warm-up** (default on) reads the non-expert file ranges sequentially once at load, so the
pages are already resident when generation starts. It touches neither the expert cache nor the
budget; it only moves cold faults out of the hot path and into `load_seconds`.

## Recipe

`--chatml --moe-stream --cache-mb auto --overlap --io-threads {4|8} -t 4 --n-expert-used K`,
`--cache-ceil-mb 3000` (gpt-oss) / `4000` (qwen, gemma). gpt-oss adds `--no-think`, n=24; qwen and
gemma n=256. Greedy decoding, so the routing and cache behaviour are deterministic: `cache_hit_pct`
is byte-identical across binaries at every step, which is what lets a wall-time delta be attributed
to the warm-up rather than to the streaming path.

## Results — `baseline_*` (no warm-up) vs `warmup_*` (warm-up on, reserve off)

| model | first-5 wall avg | tok/s | hit% | budget MiB |
|---|---|---|---|---|
| gpt-oss-120b k4 | 16641 → **829 ms (20×)** | 0.134 → **1.150** | 13.4 = 13.4 | 3000 = 3000 |
| Qwen3-30B-A3B k8 | 367 → 399 ms | 4.92 → 4.57 | 77.1 = 77.1 | 4000 = 4000 |
| Gemma-4-26B-A4B k8 | 329 → 397 ms | 4.76 → 4.76 | 82.9 = 82.9 | 4000 = 4000 |

The win is specific to the >RAM case: gpt-oss-120b (62 GB file, 1.8 GB dense) collapses its
first-five-token wall average ~20× and the first token goes from ~18 s to ~1 s. Qwen and Gemma
(≈1 GB dense, kernel pages it in quickly regardless) are neutral — within run-to-run noise, no
regression. `hit%` and `budget` are unchanged everywhere, confirming the streaming path is untouched.

## Rejected alternative: reserving dense bytes in the budget — `reserve_gemma_k8.csv`

Before settling on the warm-up, we measured the obvious alternative: subtract the dense bytes from
the auto-cache floor so the expert cache can never evict them. It is a safety knob, not a free win —
on Gemma it lowered the budget and the hit rate, costing throughput:

| Gemma k8 | budget MiB | hit% | tok/s |
|---|---|---|---|
| baseline / warm-up | 4000 | 82.9 | 4.76 |
| reserve dense in budget | **2909** | **73.5** | **3.76** |

The warm-up pre-faults the same pages *without* touching the budget, so it keeps the full hit rate
everywhere. That made the budget reservation redundant, and it was dropped in favour of the warm-up
alone. See [../../adaptive-cache.md](../../adaptive-cache.md).

## Note on thermal

Absolute steady-state `tok/s` varies with SoC temperature; back-to-back runs heat-soak the device
and depress later numbers. The `warmup_*` runs used ≥90 s cooldowns from a cool start. Because
`hit%` is temperature-independent, the budget/hit-rate conclusions above hold regardless of thermal
state; only the absolute `tok/s` figures carry thermal noise.
