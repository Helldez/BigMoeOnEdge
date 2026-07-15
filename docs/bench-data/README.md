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
| `2026-07-14-warmup/` | dense warm-up A/B | Feeds `adaptive-cache.md` and `warmup-analysis.md`. |
| `2026-07-15-route-trace/` | first `--route-trace` capture (Qwen / Gemma / gpt-oss) | Routing data, not throughput: every run had the trace **on**, so its `tok/s` are not comparable with `benchmarks.md` and must not feed those tables. |
