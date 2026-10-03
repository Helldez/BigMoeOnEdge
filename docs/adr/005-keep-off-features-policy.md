# ADR-005: Keep-off features stay in the CLI; the verdict is documentation, not deletion

## Status

Accepted (2026-09-20)

## Context

The feature scoreboard (host-benchmarks.md) now carries explicit verdicts, and three
knobs measured as refuted or harmful in matched pairs:

- `--prefetch K` — ~2× slowdown on device (gpt-oss); the popularity signal is too weak
  at top-2 (~908 MiB/token speculated vs ~587 demanded).
- `--predict-prefetch` — −21% matched re-run (−38% raw, thermally contaminated); the
  predictor's accuracy is proven (88.6%/80.7%) but *acting* on it loses.
- `--drop-in-prefill` — −11.6% vs `--drop-cold-experts` alone on Cyber-Tiel; dropping
  during prefill churns a cold cache (decode faults 26.6 → 158.5/tok).
- `--dense-odirect` — neutral at best and superseded by `--dense-weights` (kept as a
  deprecated alias).

Removal was proposed: dead code is maintenance surface, and a knob that should never
be used is arguably a bug left in.

Forces on the other side:

1. **The refutations were measured *with* these flags.** `bench-report.sh` cells,
   findings docs (e.g. `bench-data/2026-07-20-sidecar/`, `2026-08-*` prefetch runs),
   and the scoreboard's negative rows all reference the flags by name. Deleting the
   flag orphans the evidence: the docs describe experiments that the shipped CLI can
   no longer reproduce.
2. **The knobs are the instruments, not just the product.** `--predict-prefetch` is
   the only way to exercise the proven-accurate predictor; future work (a better
   action policy, a stronger popularity signal) re-measures by flipping one flag, not
   by re-implementing.
3. **Refutations are regime-bound, not eternal.** `--prefetch` lost because top-2
   routing speculated too much; a model with different expert granularity or an
   on-device cache with different bandwidth could flip the verdict. The mechanism
   stays cheap to re-test.
4. **The point of use already warns.** `--help` marks the harmful ones ("off: the
   cold cache makes it expensive", "debug/tests only") and the app only exposes
   default-off experimental rungs; no shipping path turns these on.
5. **Removal is not free.** Each flag has config plumbing, gates, and tests that
   assert its behavior; ripping it out is a diff through `RunConfig`, the engine, and
   the test suite — real review cost for zero user-facing gain.

## Decision

1. **Keep-off features remain in the CLI, default-off, in mainline.** No flag is
   removed because its verdict is negative.
2. **The verdict lives in three places, all documentation:**
   - the scoreboard's `Verdict` column (`docs/host-benchmarks.md`);
   - `--help` (kept honest — a knob whose measured verdict contradicts its help text
     gets the help text fixed in the same PR as the measurement);
   - the per-feature findings doc / `bench-data/` evidence the verdict cites.
3. **A refuted knob's code may be simplified but not deleted**: dead-config cleanups
   are allowed only when they keep the flag parseable (accept + ignore is *not*
   acceptable — it silently lies to scripts).
4. **Re-implementation is the failure mode this prevents.** If someone proposes
   re-adding a removed mechanism, the scoreboard row is the prior art they must
   answer.
5. **A keep-off verdict can be overturned** by a new matched pair in a regime where
   the mechanism should win (e.g. speculative reads that are cheaper than demand
   reads). Overturning means: new rows in the scoreboard, a verdict flip, and a note
   in the old findings doc pointing at the new one — never a silent rewrite.

## Consequences

- Easier: the evidence chain (docs → flags → reproduction) stays intact; new
  speculation/latency-hiding work reuses the instruments; no test-suite churn.
- Harder: the CLI surface keeps knobs we recommend against (~10 extra lines of help
  text and plumbing); contributors may propose "cleaning them up" and must be
  pointed here.
- Accepted as-is: a user who passes `--prefetch 4` gets the slow thing the docs
  promised, with the warning at every layer that documents it.
- The Android app never gains a keep-off rung beyond a default-off experimental
  entry; the app's defaults are governed by the scoreboard's `On`/`Use-when`
  verdicts, never its `Keep-off` ones.
