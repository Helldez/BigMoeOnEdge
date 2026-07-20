# Contiguous per-expert sidecar — measured NEGATIVE at engine level (2026-07-20)

**Verdict: refuted for the shipping configurations. PR #90 closed unmerged; the implementation
survives at tag `expert-sidecar-refuted` if a future storage stack changes the trade.**

The sidecar re-orders the gguf's expert bytes into one contiguous entry per (layer, expert), so
a routed expert costs one O_DIRECT read instead of one per projection. The microbench premise is
TRUE; the engine-level conclusion drawn from it was false. Three interleaved A/B/A/B rounds
(condition-gated cooldown, raw CSVs + logs in this directory):

| round | model / config | gguf | sidecar | delta |
|---|---|---|---|---|
| 1 | LFM2.5-8B, serial, cache 0, io4 | 2.529 tok/s | 2.927 | **+15.7%** |
| 2 | LFM2.5-8B, overlap, cache 3000, io4 | 13.25 | 10.63 | **−20%** |
| 3 | Qwen3-30B k8, overlap, cache auto/4000, io4 (README config) | 4.55 (clean pair) | 3.52 | **−23%** |

Round 3 carries thermal drift (a1 5.85 → a2 4.55 across the run; b1 started under a 2.27 GHz
vendor cap) but the ordering is unambiguous: every sidecar cell lost to every adjacent gguf cell.

## Why the microbench won and the engine lost

- `bmoe-iobench --scatter` (this directory's justification, still valid as a *drive* fact):
  at 3×~300 KiB scattered pieces the flash plateaus ~1815 MiB/s even at 16 lanes; the same
  bytes contiguous reach ~1980 at 4 lanes, 1826 at 2. Contiguity genuinely buys bandwidth.
- But the engine's overlap path consumes **projection-major** (mul_mat_id: all gates, then ups,
  then downs) and emits its reads in that order so the kernel unblocks early. The sidecar can
  only deliver **expert-major whole entries**: an expert's readiness flips when its full ~0.9 MB
  entry lands (~2.6 ms) instead of when its first ~0.3 MB slice does (~1 ms). Latency-to-first-
  projection triples, the compute pipeline stalls, and 3× fewer jobs under-fill the lanes when
  misses are few. Bandwidth won; pipeline lost; pipeline is what tok/s is made of.
- Round 1 (serial + cache off) is the one regime with no pipeline to starve — a barrier waits
  for the whole batch anyway — which is why it showed +16% and why it was the wrong regime to
  extrapolate from. Consistently, in rounds 2-3 the sidecar cells' *prefill* (batched, union
  demand, bandwidth-bound) was equal or better; decode is where it loses.

## What stays true

- Gates G8a–e (byte-identity in every mode on both layouts, tamper refusal) all pass — the
  implementation is correct, just not faster where it matters.
- The scatter numbers refine `iobench-ceiling.md`: "flat above 256 KiB" holds only at
  saturating lane counts. Sub-MiB scattered reads DO cost bandwidth at low effective lanes.
- A hybrid (per-layer choice: entry reads when misses are many, per-projection when few) was
  considered and NOT built: round 3 is the miss-heavy regime and still lost, so the hybrid's
  best case is parity with today plus complexity. Do not rebuild without new evidence — e.g.
  a projection-major sidecar variant would fix the consumption-order mismatch but reintroduces
  scattered sub-entry reads, i.e. the thing the sidecar exists to remove.
