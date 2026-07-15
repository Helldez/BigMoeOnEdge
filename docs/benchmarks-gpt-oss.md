# gpt-oss-120b — a 58 GB model at 5.2× device RAM

The same engine streams **OpenAI gpt-oss-120b** — a 58.46 GB MoE — on the same 11.3 GB phone.
That is **5.2× device RAM**: the model cannot be held resident by any means, and to our knowledge
this is the first time a 120B / 58 GB model has generated tokens on a phone at all. The run below
is an exploratory sweep (top-k × read-lanes × prefetch), not the polished 256-token matrix used for
Qwen/Gemma in [benchmarks.md](benchmarks.md) — read the two caveats before the numbers.

## Environment

| | |
|---|---|
| Device | OnePlus 15R (`CPH2769`), Android 16, 11.3 GB RAM — same as [benchmarks.md](benchmarks.md) |
| Model | `gpt-oss-120b-Q4_K_M.gguf` — 58.46 GB, 36 layers, 128 experts, **top-4** default, MXFP4 expert weights |
| Device path | `/data/local/tmp/shardllm/` — the real `/data` partition, **required** for working O_DIRECT (`/sdcard` is FUSE and silently falls back to buffered) |
| Fraction of RAM | ≈**5.2×** (58.46 GB / 11.3 GB) — resident load is impossible, so there is no in-RAM baseline, only `mmap` page-cache thrash |
| Engine | `bmoe-cli` built from `feat/harmony-nothink-final-channel` @ `4d12b75` (arm64, NDK r26, `armv8.2-a+dotprod+fp16`) |
| Fixed config | `--cache-mb auto --cache-ceil-mb 3000` (auto-sized, capped 3000 MiB), O_DIRECT on, `--overlap` on, `-t 4`, `--no-think` |
| Swept | top-k ∈ {2, 3, 4}, read-lanes ∈ {4, 8}, prefetch ∈ {off, 4} — 12 cells, plus a 2-cell `mmap` baseline |
| Probe | `-n 24`, prompt *"What is 17 times 23? Then name the capital of Australia."* (a short, checkable probe — see Quality) |

`--no-think` matters here. gpt-oss uses the harmony format, whose template **always** opens an
`analysis` (chain-of-thought) channel — so a normal run spends its whole budget reasoning before it
answers. `--no-think` now primes the `final` channel directly (see the engine fix on this branch), so
the model answers immediately with no analysis tokens. That is what makes a 24-token probe meaningful
— but it also removes the model's scratch space, which the Quality section below shows has a cost.

## Two caveats (both narrow the numbers, honestly)

1. **Short probe, not steady state.** These are **24-token** runs, not the 256-token runs used for
   Qwen/Gemma. The expert cache is still warming — hit rate sits at **13–21 %** (vs 76 % for Qwen at
   256 tokens), so flash-read/token is high and the absolute tok/s is a **floor**: a warm, longer run
   would read less and decode faster.
2. **The k=4 rows were interrupted.** The phone was physically unplugged several times during the
   k=4 cells; model-load and TTFT balloon there (k4 · io8 · pf0: load 90 s, TTFT 120 s). That row's
   decode is **not trustworthy** — its compute drops to 2.042 s/tok against 3.869 s/tok for the *same*
   k=4 at 4 lanes, but compute is lane-independent, so the gap is device state (cooler / less contended
   after the interruption), not a lane effect. It is marked † and excluded from every conclusion. Read
   k=4 from the **io4 · pf0** row (4.489 s/tok).

## Results

s/token and tok/s are the engine's `generation:` line; `compute` and `flash read/token` are from its
`moe-stream:` line; `cache hit` from `moe-cache:`. `--overlap` is on, so `flash I/O` runs concurrently
with compute and the **stall** (residual flash wait not hidden behind compute) is the honest I/O cost —
it stays ~0.2–0.3 s/tok throughout, i.e. overlap hides almost all of the flash read.

| top-k | lanes | prefetch | tok/s | s/token | compute (s/tok) | flash read/token | cache hit |
|---:|---:|---:|---:|---:|---:|---:|---:|
| **2** | **4** | **off** | **0.687** | **1.455** | 1.159 | 535.75 MiB | 20.4 % |
| 2 | 4 | 4 | 0.532 | 1.878 | 1.529 | 640.91 MiB | 21.2 % |
| 2 | 8 | off | 0.620 | 1.613 | 1.310 | 535.75 MiB | 20.4 % |
| 2 | 8 | 4 | 0.516 | 1.937 | 1.647 | 640.91 MiB | 21.2 % |
| **3** | **4** | **off** | **0.391** | **2.556** | 2.118 | 925.74 MiB | 16.9 % |
| 3 | 4 | 4 | 0.304 | 3.293 | 2.854 | 1100.68 MiB | 18.1 % |
| 3 | 8 | off | 0.279 | 3.581 | 3.074 | 925.74 MiB | 16.9 % |
| 3 | 8 | 4 | 0.295 | 3.394 | 2.924 | 1100.68 MiB | 18.1 % |
| **4** | **4** | **off** | **0.223** | **4.489** | 3.869 | 1402.75 MiB | 13.4 % |
| 4 | 4 | 4 | 0.188 | 5.327 | 4.701 | 1619.72 MiB | 14.7 % |
| 4 | 8 | off † | *0.383* | *2.613* | *2.042* | 1402.75 MiB | 13.4 % |
| 4 | 8 | 4 | 0.213 | 4.704 | 4.045 | 1623.57 MiB | 14.7 % |
| mmap | — | — | 0.089 | 11.240 | — | 0 (page cache) | — |
| mmap (k=4) | — | — | 0.075 | 13.337 | — | 0 (page cache) | — |

† Interrupted run — see caveat 2. Excluded from conclusions.

## Reading the numbers

- **Streaming vs `mmap`: 3–8×.** k=2 streams at 1.455 s/tok against **11.240 s/tok** for a plain
  `mmap` load of the same file — **7.7× faster**; k=4 is 4.489 vs 13.337 — **3.0×**. `mmap`-ing 58 GB
  onto 11 GB of RAM thrashes the page cache on *every* token (10–13 s each); the bounded 3 GB O_DIRECT
  cache replaces that with reads the engine controls, and leaves the rest of RAM for the system.
- **top-k is the dominant lever — it cuts compute *and* I/O.** Both scale almost linearly with k:
  compute 1.16 → 2.12 → 3.87 s/tok and flash-read 536 → 926 → 1403 MiB/tok across k = 2 → 3 → 4 (4
  lanes). k=2 is ~**3× faster** than k=4. This is the same knob as Qwen/Gemma's Turbo top-k, but it
  matters far more here because gpt-oss is heavily compute-bound.
- **Compute-bound, hard.** Even at these low hit rates the *compute* share dominates at k ≥ 3 (k=4:
  3.87 s of the 4.49 s decode), because each gpt-oss expert is large (d_ff 2880 — several × a Qwen
  expert), so top-4 is a lot of MAC per token. Overlap already hides almost all flash wait (stall
  ~0.2–0.3 s/tok), so the remaining cost is kernels, not the seam — exactly as on Qwen at a warm cache.
- **prefetch=4 always regresses.** Every `pf 4` row is slower than its `pf off` sibling. Prefetch
  reads 20–25 % *more* per token speculatively (k=2: 640.91 vs 535.75 MiB/tok) but only **12–15 %** of
  those experts are ever used — on a compute-bound model that wasted flash bandwidth buys nothing and
  costs cache churn. Leave prefetch off for gpt-oss.
- **Lanes 4 vs 8: 4 wins where it's trustworthy.** At k=2 io4 beats io8 (1.455 vs 1.613) — with the
  flash wait already overlapped, extra lanes only add contention. The k=4 lane comparison is confounded
  (caveat 2), so no lane claim is made there.
- **A 24-token probe is mostly warm-up.** Unlike Qwen/Gemma, gpt-oss at 5.2× RAM warms up *inside*
  compute: the first tokens fault the mmap-resident, non-expert working set in from flash (`compute_ms`
  ~18 s), settling to sub-second once hot. The mean over 24 tokens is therefore a floor dominated by that
  cold head, and the steady tail is several × faster. This memory-residency warm-up — distinct from the
  gentle, I/O-bound cache warm-up on Qwen/Gemma — is analysed token-by-token in
  [warmup-analysis.md](warmup-analysis.md).

## Quality — the cost of dropping reasoning

Because these runs use `--no-think` (forced `final` channel, **no** chain-of-thought), the model
answers with no scratch work — and decode is greedy/deterministic, so the answer depends only on k
(identical under streaming and `mmap`):

| top-k | `17 × 23 =` | capital |
|---:|---|---|
| 2 | **391** ✅ | Canberra ✅ |
| 3 | **391** ✅ | Canberra ✅ |
| 4 (default) | **387** ❌ | Canberra ✅ |

The model's *default* top-4 gets the arithmetic **wrong** (387) while the narrower k=2/k=3 get it
**right** (391). This is not "smaller k is smarter" — it is that **without the analysis channel there
is no scratch space to compute 17 × 23**, so the answer is a one-shot guess whose correctness is
prompt- and k-specific. Takeaway: `--no-think` (forced-final) is a **latency/throughput mode** — use
it for direct-answer UX and for benchmarking decode speed; for arithmetic or any multi-step task, drop
`--no-think` and let gpt-oss spend analysis tokens. The capital is correct at every k.

## Provenance

Measured 2026-07-14 with `bmoe-cli` @ `4d12b75` (branch `feat/harmony-nothink-final-channel`). The
summary log for all 14 cells and the two `mmap` per-token CSVs are committed under
[`bench-data/2026-07-14/`](bench-data/2026-07-14/); the streaming cells were run without `--csv` (this
sweep captured only the `moe-stream:` / `moe-cache:` summary lines), so there are no per-token CSVs for
them — a follow-up warm 256-token run with `--csv` and a compute-thread sweep (`-t 4/6/8`) is the next
step. Drivers: [`scripts/gptoss-matrix.sh`](../scripts/gptoss-matrix.sh) (the 12-cell streaming sweep)
and [`scripts/gptoss-mmap.sh`](../scripts/gptoss-mmap.sh) (the 2-cell mmap baseline), prompt and flags
baked in. The model was merged from its two HF shards with `llama-gguf-split --merge` before use.

| Model | Device path | Size |
|---|---|---|
| gpt-oss-120b | `/data/local/tmp/shardllm/gpt-oss-120b-Q4_K_M.gguf` | 58.46 GB |
