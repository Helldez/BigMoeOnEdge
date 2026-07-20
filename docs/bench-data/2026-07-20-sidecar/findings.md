# Contiguous per-expert sidecar — on-device A/B (2026-07-20)

**Question.** Does re-ordering the expert bytes into one contiguous entry per (layer, expert)
— one read per routed expert instead of one per projection — raise decode throughput on the
reference device, at identical weights and quantization?

**Method.** `bmoe-cli` on LFM2.5-8B-A1B-Q5_K_M (arch `lfm2moe`, 24 MoE layers, 32 experts,
top-4), model and sidecar both on `/sdcard/Download` (O_DIRECT verified working there).
Serial streaming, `--cache-mb 0 --io-threads 4 -t 4 -n 96`, dense weights anon. Cells
interleaved A/B/A/B; each cell starts only once `scaling_max_freq` is back within 90% of max
and thermal status is 0 (condition, not a timer — the first cooldown legitimately waited out
a 2.0 GHz vendor cap). Raw per-token CSVs and the run log are in this directory.

**Result.**

| cell | layout  | decode tok/s | mean wall ms | mean io ms |
|------|---------|--------------|--------------|------------|
| a1   | gguf    | 2.528        | 396.6        | 327.6      |
| b1   | sidecar | 2.901        | 345.2        | 282.0      |
| a2   | gguf    | 2.530        | 393.7        | 327.3      |
| b2   | sidecar | 2.952        | 338.8        | 282.7      |

Sidecar **+15.7% decode tok/s** (2.529 → 2.927 mean), reproducible to ~2% across the
interleaved repeats, with the sidecar cells running *later* (thermally disadvantaged).
The attribution is exactly the designed one: io_ms fell 14% while compute was unchanged —
the win is read bandwidth, not a side effect.

**Why it works (microbench).** `bmoe-iobench --scatter` on the same device: at the Qwen-class
slice size (3 × ~300 KiB per expert), scattered reads plateau at ~1815 MiB/s even at 16
lanes, while the same bytes as one contiguous window reach ~1980; at the engine's real ~2
effective lanes the gap is 1355 vs 1826 MiB/s (+35%). Sub-MiB scattered reads are NOT free —
the earlier "flat above 256 KiB" result was measured at saturating lane counts and does not
transfer to the engine's operating point.

**Correctness.** Gates G8a–e: byte-identity streamed == resident with the sidecar across
cache off / LRU / overlap / prefetch on both expert layouts (split and fused), plus refusal
of a tampered sidecar. The sidecar carries the source gguf's size and an expert-map hash;
a mismatch is a hard init error, never a silent fallback.

**Owed.** The same A/B on Qwen3-30B-A3B k=8 (pure I/O-bound: io/wall ≈ 2.0), where the
microbench predicts a larger gain; and the app-side wiring (build post-download + toggle).
