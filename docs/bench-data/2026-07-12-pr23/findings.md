# On-device A/B for temporal prefetch (PR2) and speculative gating (PR3)

> **Archived, 2026-07-12.** Speculative gating has since been removed from the engine to restore
> the modular seam; `--spec-gate` is no longer a flag. The `spec-gate` rows and the advice about
> tuning it below are kept as a record of why that decision was made — they are not actionable.
> See [../README.md](../README.md).

OnePlus 15R (11.3 GB), 256-token decode, prompt-processing + steady-state. Each config on top of
its model's best measured baseline (Qwen: cache 4000, lane 4, overlap; Gemma: cache 2000, lane 4,
overlap). See `summary.md` for the full tables. Device was cooler than the 2026-07-12 baseline run,
so absolute tok/s is higher; the A/B is within-session (same 45 s cooldowns, back-to-back).

## Verdict

**Both features are byte-correct and work end-to-end on device** — the recall and useful-hit
metrics prove the prediction→prefetch→integrate→hit path runs. But **neither improves steady-state
throughput here**, for two clear reasons:

1. **Temporal prefetch is neutral (±1%).** It fires with high useful rate (85–100%) but does not
   move tok/s, because intra-layer overlap already hides the read latency and there is no idle
   flash bandwidth to exploit — the prefetch reads just add to the same saturated flash. Best case
   was shallow `--prefetch 1` at cache 5000 (**5.14 vs 5.10**, +0.9%, essentially free at 36 MiB).
   Its real payoff is the cold-start ramp / TTFT, which a 256-token steady-state run does not
   isolate.

2. **Speculative gating is net-harmful at steady state.** It predicts accurately (Qwen 88% recall,
   Gemma 66%) and raises the cache hit rate (76%→88% on Qwen) while cutting per-token stall
   (0.063→0.012 s) — the mechanism works. But it reads **24–44 GB speculatively** over the run,
   which saturates the flash and roughly **halves throughput** (Qwen 5.09→2.65, Gemma 3.15→1.60).
   The cache (2–4 GB) cannot hold the speculation alongside the working set, so it thrashes
   (re-reading experts). It needs a **speculative-bytes cap** (and/or a confidence threshold)
   before it is usable for throughput on flash-bound hardware.

3. **The real throughput lever is cache size, and Qwen is already compute-bound.** Growing the
   cache 4000→5000 lifted the hit rate 76%→84% and cut stall 0.063→0.041 s — yet tok/s stayed at
   ~5.1. At these cache sizes the token time is dominated by compute, not I/O, so neither a bigger
   cache nor prefetch moves the ceiling.

## Takeaways

- For **maximum steady-state throughput** on Qwen here: `cache 5000, lane 4, overlap` (optionally
  `+ prefetch 1`, near-free) — but you are at the compute ceiling (~5.1 tok/s); speculation cannot
  raise it.
- **Do not enable `--spec-gate` for steady-state on this device** until it caps speculative reads.
- The features are TTFT/warm-up tools; a follow-up measuring the **first N tokens from a cold
  cache** (or session-mode prompt-2 warm start) is where they should show value.
