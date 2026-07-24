# Benchmark data archive

Raw per-run CSVs and the session notes written alongside them, one directory per measurement
session. Each directory is a **historical record of the build it was captured on**, kept for
provenance so the tables in [../benchmarks.md](../benchmarks.md) can be traced back to data.

**These files are not maintained.** They are not corrected when the code moves on, and a
recommendation inside one is only valid for the build it was measured against. Read the
maintained docs for current guidance; read these to check where a number came from.

| Session | Captured against | Note |
|---|---|---|
| `2026-07-12/` | baseline cache/lane sweep | Feeds the cache-and-lanes tables in `benchmarks.md`. |
| `2026-07-12-pr23/` | temporal prefetch + speculative gating | **Speculative gating no longer exists** — it was removed to restore the modular seam. The `--spec-gate` rows and the advice about it describe a flag the CLI no longer accepts. |
| `2026-07-13/` | adaptive cache budget | Source of the capped-auto recipe numbers. |
| `2026-07-14/` | per-token warm-up | Feeds `warmup-analysis.md`. |
| `2026-07-14-warmup/` | dense warm-up A/B | Feeds `cache-sizing.md` and `warmup-analysis.md`. |
| `2026-07-15-route-trace/` | first `--route-trace` capture (Qwen / Gemma / gpt-oss) | Routing data, not throughput: every run had the trace **on**, so its `tok/s` are not comparable with `benchmarks.md` and must not feed those tables. |
| `2026-07-17/` | all-O_DIRECT dense weights (`--dense-weights anon`) | Source of the gpt-oss steady-state numbers. **Several cells are contaminated by run order** (a fixed cooldown does not reach baseline) — `NOTES.md` marks exactly which, and which claims survive. Do not read a top-k or lane claim out of it. |
| `2026-07-24-desktop-qwen36/` | desktop (x86 laptop) streaming, Qwen3.6-35B at ~1.5× RAM | Source of the README Desktop table. Decode is DRAM-bandwidth-bound there, the opposite of the phone; round 1 carries a documented `--cache-mb auto` confound that round 2 (fixed cache) resolves. |
