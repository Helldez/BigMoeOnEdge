# Cache-aware expert dropping — measured POSITIVE on device (2026-07-22)

**Verdict: `--drop-cold-experts` is a real throughput win, and the offline replay understated it at
full strength. 0.75 and 1.00 are each separable from the baseline and from each other; 0.50 does
nothing measurable. Quality was not measured and remains the open question.**

## Setup

In-app, one device, `Qwen3.6-35B-A3B-Q4_K_M` (`qwen35moe`, 40 layers, 256 experts, top-k 8), cache
3000 MiB, `--dense-weights ahwb`, overlap on, prefetch off, 4 compute + 4 read lanes, ~1350-1470
token generations. Every cell differs in `drop_cold_frac` **only**; the four `# moe_stream=…`
preambles in this directory are identical apart from that field.

## Whole-run numbers

| `drop_cold_frac` | tok/s | flash read | I/O s/token | cache hit | routings dropped |
|---|---|---|---|---|---|
| 0 (off) | 2.549 | 248.2 GiB | 0.299 | 67.8% | 0 |
| 0.50 | 2.564 | 231.4 GiB | 0.279 | 69.1% | 2.7% |
| 0.75 | **3.938** | 162.8 GiB | 0.190 | 77.2% | 14.2% |
| 1.00 | **4.702** | 48.1 GiB | 0.070 | 90.7% | 28.4% |

Per-token, first 50 tokens trimmed (cache warm-up, plus the one token the hook spends learning the
graph), 2000-sample bootstrap on the mean:

| `drop_cold_frac` | median ms/token | mean ms/token | 95% CI on the mean |
|---|---|---|---|
| 0 | 249.5 | 391.1 | [375.3, 407.1] |
| 0.50 | 263.2 | 391.6 | [375.3, 408.5] |
| 0.75 | 202.8 | 253.5 | [246.9, 260.4] |
| 1.00 | 153.4 | 214.9 | [206.2, 225.0] |

**Every pair is disjoint except 0 vs 0.50, which overlaps.** At half the uniform share the policy
finds almost nothing to drop (2.7% of routings) and buys nothing — exactly what the replay
predicted, and a useful negative control: the machinery costs nothing measurable when it is not
firing.

## Why this is not the device drifting

Run order was **1.00 (13:42), off (13:51), 0.50 (14:04), 0.75 (14:23)** — deliberately not in
threshold order. The two fastest cells are the **first and the last**. Thermal drift or accumulated
memory pressure would make the last cell the worst; it is the second best. The baseline also ran
early, on a relatively cool device, and still lost.

The mechanism is monotone in the threshold even though the run order is not: flash read
248 → 231 → 163 → 48 GiB and I/O 0.299 → 0.279 → 0.190 → 0.070 s/token order themselves perfectly by
`drop_cold_frac`. That dose-response is what run order cannot fake.

## The replay was conservative, not optimistic

`docs/expert-dropping.md` calls the offline replay an **upper bound**, because it cannot model the
cache changing as a result of dropping. At `F = 0.75` it was accurate (predicted ~37% of reads
avoided, measured 34%). At `F = 1.0` it **understated**: predicted 66%, measured **81%**.

The extra comes from a compounding effect the static replay is blind to: reads avoided free cache
capacity, which raises the hit rate, which leaves fewer misses to drop in the first place. The
caveat in the doc should be read as "the bound holds where dropping is light, and is pessimistic
where it is heavy".

## Read `cache_hit_pct` with the documented caveat

The rise from 67.8% to 90.7% is **not** the cache serving more. A dropped routing is a miss that is
never looked up, so it leaves both sides of the ratio (see `docs/telemetry.md`). `read_MiB` is the
honest number here, and it is unaffected by that accounting: 248 GiB → 48 GiB is real.

`majflt/token` (224 / 178 / 14 / 611) does **not** order by threshold and is dominated by what the
device's memory looked like when each run started. Nothing should be concluded from it here.

## What this does not show

- **Quality is unmeasured.** At `F = 0.75` the policy discards 14.2% of routings and at `F = 1.0`
  28.4%. There is no perplexity number and no side-by-side comparison in this experiment. Throughput
  is settled; whether the output holds up is not.
- **One model, one device, one run per cell.** The confidence intervals are within-run (over ~1300
  tokens), not across repeats. The reversed-order pair is owed, as it was for `ahwb`.
- **Top-k 8 only.** Qwen3.6 routes 8 of 256. A model routing 2 or 4 experts has a much larger uniform
  share, so the same `F` bites differently there. gpt-oss in particular is unmeasured.
- **Prefetch was off.** The interaction between speculation and dropping (a correct guess un-drops an
  expert) is untested.
