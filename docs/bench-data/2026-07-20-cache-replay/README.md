# 2026-07-20 — expert cache and I/O: six levers measured, six closed

A session that set out to raise tok/s by improving the expert cache, and ended by ruling out
every candidate it examined, including the one it implemented. Recorded here because a measured
"no" is worth as much as a "yes" and is easier to forget.

Device: the reference phone, entry state logged per cell. Models from `/data/local/tmp/bmoe/`.
Nothing here is a headline benchmark number — several cells are single runs, and where the device
state confounds a comparison it is said so rather than smoothed over.

## The documents

| file | what it settles |
|---|---|
| [findings.md](findings.md) | offline policy replay: the LRU cliff, the oracle bound, why #78 would cost hit rate |
| [iobench-ceiling.md](iobench-ceiling.md) | flash ceiling vs lanes and read size; temporal prefetch measured |
| [layer-lfu-verdict.md](layer-lfu-verdict.md) | the implemented policy: wins the hit rate, loses the run |

## The verdicts

| lever | status | why |
|---|---|---|
| More I/O lanes (issue #75) | **dead** | flash saturates at 2 lanes (~2460 MiB/s); 8-32 lanes flat-to-worse, latency linear. `io_threads_max = 8` is already past the knee. |
| Bigger / coalesced reads (#75) | **dead** | bandwidth flat above 256 KiB; only 0.6-4 % of a layer's routed experts sit at consecutive ids, so an ideal coalescer removes 4 % of read *count* and zero bytes. |
| Temporal prefetch | **dead on gpt-oss** | `--prefetch 1` = 2× slowdown, no hit gain. Predictor is 17.9 % accurate there, worse than a static hot list (26.7 %). Untested on top-6 models. |
| Subtract dense bytes from `auto` (#78) | **dead** | costs 5-11 points of hit rate across the three models; reproduces offline the reason the earlier attempt was rejected on device. |
| Smarter eviction policy (#33) | **dead** | offline optimum is 11-23 points above LRU but no *online* policy recovers more than ~5. |
| `layer-lfu` (the best candidate, built) | **dead, and instructive** | delivers the predicted +2.0 points of hit and −7 % reads, and is **~30 % slower**: majflt/token 6 → 2370. |

## What actually came out of it

**One real defect, worth fixing.** Global LRU returns **exactly 0.0 %** — not "degraded" — once
the budget cannot hold one token cycle, because its recency order is anti-correlated with the
deterministic layer cycle. Reproduced on device at `--cache-mb 1500`, a value the CLI accepts
today, where it reads exactly as much as having no cache while still costing 24 % of the token in
management and 1500 MiB of RAM. The guard is cheap: the worst-case cycle is computable at init
from model shape alone. Documented in [cache-sizing.md](../../cache-sizing.md).

**One transferable lesson.** `scripts/route-replay.py` predicted `layer-lfu`'s hit rate correctly
— it models *which* entries a policy keeps. It cannot model *what keeping them costs*. The
partition's hard per-layer cap removes the cache's ability to self-balance, and the resulting
`MADV_DONTNEED` churn is paid for by the kernel reclaiming the dense weights. **A hit-rate curve
is not a throughput argument.**

**Two reusable instruments**, both of which paid for themselves in one session:
`tools/bmoe-iobench` (standalone, links no engine — killed two roadmap items in minutes) and
`scripts/route-replay.py` (zero device cost, validated to the decimal against recorded runs).

## Owed

- An **interleaved** engine/microbench measurement at matched entry state. The engine's
  1419 MiB/s looks like 58 % of the cold ceiling (2460), but the ceiling itself falls to 1645 once
  the device is hot, so the duty-cycle gap is not honestly sized yet. Until then "the engine
  leaves bandwidth on the table" is unproven.
- Prefetch re-measured on the top-6 models, where the predictor is twice as accurate.
- The cliff guard itself.

The device thermally shut down at the end of this session after ~1.5 h of sustained load
(battery was at 86 %, so it was heat, not charge). The `bmoe-iobench` sweeps are a harsher I/O
load than the engine ever produces — pace them.
