# Pinned dense weights vs anon — measured POSITIVE, and the cost was hiding in `compute_ms` (2026-07-21)

**Verdict: `--dense-weights ahwb` is +17.9% over `anon` on a long generation, confidence intervals
disjoint. The mechanism is not what was predicted: it is not avoided flash refaults, it is avoided
*zram decompression*, which the telemetry was charging to compute.**

All runs in-app (not adb), Qwen3.6-35B-A3B-Q4_K_M, `arch=qwen35moe`, k=8, cache 3000 MiB, io4,
O_DIRECT, overlap on, prefetch off. Identical config in every cell; only `dense_weights` varies.

## The decisive pair — same session, same binary, 14 minutes apart

| | `anon` (10:37) | `ahwb` (10:23) | |
|---|---|---|---|
| tokens | 1354 | 1354 | |
| **decode tok/s** | **2.588** [2.497–2.687] | **3.053** [2.928–3.176] | **+17.9%, CIs disjoint** |
| `dense_resident_frac` | 0.848 | **1.000** | |
| majflt / token | 265.0 | 256.8 | equal |
| `compute_ms` | 298 | 241 | **−19%** |
| `io_ms` | 299 | 297 | equal |
| `stall_ms` | 58 | 58 | equal |
| cache hit % | 67.7 | 67.7 | equal |
| swap | 562 MiB | 294 MiB | |

Files: [`bmoe-20260721-103724.csv`](bmoe-20260721-103724.csv) (anon),
[`bmoe-20260721-101543.csv`](bmoe-20260721-101543.csv) (ahwb).

## The mechanism, corrected

The prediction was that pinning would remove major faults. **It does not** — majflt/token is equal
(265 vs 257). What actually happens is subtler, and is exactly what `anon` was designed to do:

`anon` does keep the dense weights off the flash. But the kernel still takes ~15% of them
(`dense_resident_frac` 0.848) and puts them in **zram**. A page reclaimed to zram is not a major
fault when touched again — it is a minor fault plus a **decompression**. That cost appears in no I/O
counter and no fault counter. It lands in `compute_ms`, which is a residual, not a measurement of
arithmetic.

So the whole delta shows up in the one column that can hide it: `compute_ms` 298 → 241, while
`io_ms`, `stall_ms`, `mgmt_ms` and the cache hit rate are all unchanged to within 1%. The swap
figure confirms the account: 562 → 294 MiB, and the difference is the dense set that can no longer
be swapped at all.

**`anon` protects the dense weights from flash. `ahwb` also protects them from zram.** The premium
`anon` was paying was invisible because it was disguised as compute — and it is worth 18%.

The expert cache is untouched: hit rate identical to the decimal, `io_ms` identical. The feared
trade — pinned dense memory starving the cache — does not occur at this size, because the dense set
(~1.6 GiB here) is small next to the 3000 MiB cache budget.

## Why a longer generation was required

The three short pairs run first (67–74 tokens each, files `bmoe-20260721-09*.csv`) are **all
inconclusive**: per-token coefficient of variation 33–71%, every bootstrap interval overlapping,
deltas scattered −0.9% / +17.8% / +2.5%. Reclaim is a standing condition that accumulates; at 70
tokens it has not yet bitten. The effect is only resolvable over a real conversation-length turn.

## A discarded comparison, kept as a warning

A cross-day pair (`anon` 20 Jul 21:35, [`bmoe-20260720-212009.csv`](bmoe-20260720-212009.csv),
against the same `ahwb` run) reads **+63.6%**. It is not usable: `anon` alone moved from 1.865 to
2.588 tok/s (**+38.8%**) between the two days with nothing changed but device state and the build.
That older run was also in a different regime entirely — 1468 majflt/token and 9 GB of refault
traffic against 1.4 GB today. Two thirds of that headline was the device, not the mode. It is
committed here so the correction is checkable, not to be quoted.

## What is still open

- **Run order.** In the decisive pair `ahwb` ran first. Across the three short pairs the
  first-executed cell won every time, so an order effect cannot be excluded from the +17.9%. The
  confirmation is one reversed pair — `anon` first, then `ahwb`, same long generation. **Owed.**
- **One device, one model, one config.** `ahwb` therefore ships default off.
- The 2047 MiB per-buffer lock ceiling is a driver property and may differ elsewhere
  ([2026-07-21 bandwidth probe](../2026-07-21-pinned-memory/findings.md)).

## The transferable lesson

`compute_ms` is a residual and it has been absorbing zram decompression this whole time. Any past
conclusion of the form "this regime is compute-bound" is now suspect: part of that compute may have
been swap-in that a memory policy could have removed. Making the zram cost visible — minor faults
and swap-in time are readable from `/proc/self/stat` and the counters already sampled — is worth
more than the next few percent of throughput, because it is an attribution error that has been
steering decisions.
