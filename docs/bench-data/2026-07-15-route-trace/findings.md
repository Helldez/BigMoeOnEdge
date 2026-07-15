# Route-trace session — 2026-07-15

First capture with `--route-trace` (PR #21): one run per model, each in the best config the
maintained docs record for it. The subject is **routing** — which experts each layer picks, how the
router weights them, and whether they were resident — not throughput.

## These are not benchmark numbers

Every run here had the trace **on**, which asks the graph for extra nodes (a barrier per MoE layer)
and writes a row per routed expert. The `tok/s` below are context for reading the traces, nothing
more. **Do not feed them into the README or `benchmarks.md` tables**, and do not compare them with
the numbers there: those were measured trace-off, on other builds, with other prompts and lengths.
Routing itself is deterministic and indifferent to clocks, so the trace data does not carry this
caveat.

## Setup

| | |
|---|---|
| Device | OnePlus 15R (CPH2769, SM8845), 11.1 GB RAM, Android 15 |
| Engine | `feat/route-trace` rebased on `main` @ `e7d2e8c` — includes prompt-tail retention (PR #22) and the compute decomposition (PR #20) |
| Build | arm64, NDK r26, `armv8.2-a+dotprod+fp16`, `-t 4`, O_DIRECT, `--overlap` |
| Models | `/data/local/tmp/shardllm/` (real `/data`; O_DIRECT silently degrades on FUSE `/sdcard`) |
| Prompt | *"Explain how a mixture-of-experts model decides which experts handle each token."*, `--chatml --no-think` |
| Script | [`route-trace-bench.sh`](route-trace-bench.sh) — 60 s cooldown per run, strays killed, device state captured before/after each run into `<tag>.state` |

Device state was **not ideal and is recorded per run**: `policy0` was capped at 2.19 GHz of 3.32 GHz
hardware max and fell to 1.90 GHz mid-run; `policy6` sat at 1.65 GHz of 3.80 GHz throughout. A
Bitdefender APK scan (161 % CPU) was waited out before starting. See the `.state` files.

## Runs

| tag | model | config | tok/s | cache hit | compute/tok | files |
|---|---|---|---|---|---|---|
| `qwen` | Qwen3-30B-A3B-Q4_K_M | `--cache-mb 4000 --overlap --io-threads 4 --n-expert-used 6`, `-n 128` | 5.657 | 74.9 % | 0.114 s | [route](qwen.route.csv) · [metrics](qwen.metrics) · [state](qwen.state) · [log](qwen.log) |
| `gemma` | google_gemma-4-26B-A4B-it-Q4_K_M | same, `-n 128` | 4.971 | 76.8 % | 0.133 s | [route](gemma.route.csv) · [metrics](gemma.metrics) · [state](gemma.state) · [log](gemma.log) |
| `gptoss` | gpt-oss-120b-Q4_K_M | `--cache-mb auto --cache-ceil-mb 3000 --overlap --io-threads 4 --n-expert-used 2`, `-n 64` | 0.503 | 32.4 % | 1.705 s | [route](gptoss.route.csv) · [metrics](gptoss.metrics) · [state](gptoss.state) · [log](gptoss.log) |

`cache hit` is the cumulative figure from the `# summary` line (it includes prefill, so it is lower
than the decode-only hit rate the trace shows).

Two ways to read the traces, both stdlib-only:

```
python scripts/route-analyze.py docs/bench-data/2026-07-15-route-trace/qwen.route.csv
python scripts/route-viewer.py  docs/bench-data/2026-07-15-route-trace/ viewer.html
```

`route-analyze.py` answers questions in the terminal; `route-viewer.py` packs the whole session into
one self-contained HTML page — the step × layer matrix with the expert ids in its cells, hot experts
per layer, the per-token metrics and the raw rows — for when there is no spreadsheet around. The
page rounds weights to 4 decimals; these CSVs carry the full precision.

## What the routing looks like

| | arch | MoE layers | experts | top-k | one expert | whole bank | touched this run |
|---|---|---|---|---|---|---|---|
| qwen | qwen3moe | 48 | 128 | 6 | 2.72 MiB | 16 740 MiB | 8 386 MiB (50 %) |
| gemma | gemma4 | 30 | 128 | 6 | 3.76 MiB | 14 429 MiB | 7 576 MiB (53 %) |
| gptoss | gpt-oss | 36 | 128 | 2 | **12.61 MiB** | 58 092 MiB | 16 288 MiB (28 %) |

Mean distinct experts touched per layer (of 128): qwen 64 (min 33, max 107), gemma 67 (52–97),
gptoss 36 (19–64).

### 1. Temporal prefetch has no signal beyond popularity

For each layer, how much of a step's top-k did a predictor get right, at the same budget of `k`
experts per layer:

| predictor | qwen | gemma | gptoss |
|---|---|---|---|
| random guess (`k/n_expert`) | 4.7 % | 4.7 % | 1.6 % |
| **temporal** — the previous token's `k` | 38.0 % | 35.7 % | 17.9 % |
| **static** — the run's `k` hottest | 37.6 % | **39.6 %** | **26.7 %** |
| static, learned from the **prompt only** | 17.9 % | 26.8 % | 8.2 % |

Predicting from the previous token does **not** beat a fixed hot list, and loses to it on two of
three models. The corroborating detail is in the lag profile: for qwen the overlap is 38.0 % at
lag 1 and still 26.7 % at lag 8. If routing had real temporal adjacency, lag 1 would tower over
lag 8; it does not. The previous token is simply a *noisy sample* of the hot distribution, while a
frequency list is a clean one — which is why `--prefetch` does not lift steady-state throughput
(as `benchmarks.md` already reported) and why it actively costs gpt-oss.

The last row is the honest one for warm-up purposes: a hot list learned from the prompt alone is a
much weaker predictor than one that has seen the decode. It improves with budget (qwen reaches
52.0 % at 24 experts/layer ≈ 3.1 GiB), but the LRU cache at 4000 MiB already delivers ~80 % hit, so
a static preload is only a **cold-start** tool, not a steady-state one.

### 2. The dense warm-up decays, and on gpt-oss it collapses

Major faults per decoded token, averaged per quarter of the run (`majflt` column, PR #20):

| | Q1 | Q2 | Q3 | Q4 | corr(majflt, wall_ms) |
|---|---|---|---|---|---|
| qwen | 17.4 | 14.1 | 20.2 | 21.3 | +0.05 |
| gemma | 42.4 | 34.8 | 54.1 | 65.5 | — |
| **gptoss** | **1 875** | 5 924 | 7 922 | **8 431** | **+0.49** |

`warm_dense` pages the non-expert bytes in at load (gpt-oss: 790 MiB of dense against 58 GB of
experts). It is a **one-shot with no defence against later eviction**: on gpt-oss the 32 GB of
expert traffic pushes the kernel to reclaim those pages, and by the last quarter the model is
demand-faulting ~8 400 pages per token. Wall time follows (1 640 → 2 039 ms per token, +24 %).

On qwen the effect is absent — its dense is 541 MiB and the device has room. So this is not "warm-up
is broken"; it is **warm-up has no retention**, and the gap only opens when dense + expert traffic
exceeds what the page cache can hold.

### 3. gpt-oss is compute-bound; its cache cannot be fixed by policy

One gpt-oss expert is 12.61 MiB — 4.6× a qwen expert. The 3000 MiB budget therefore holds 238
experts across 36 layers, **6.6 per layer of 128**, or 18 % of what the run touched. Hence 45 %
decode hit against qwen's 80 %. But compute is 1.705 s of the 1.988 s per token (**86 %**), so the
cache is not the binding constraint: `top-k` remains the dominant lever, as `benchmarks.md` found.

### 4. Early layers route almost uniformly

Qwen's layer 0 touched 107 of 128 experts in 128 tokens, with its 8 hottest carrying only 21.5 % of
activations. Concentration rises with depth (layer 5: 43.2 %; mean across layers 44.5 %). Any
frequency-based preload should target the deeper layers and skip the near-uniform early ones —
spreading the budget evenly wastes it where prediction is impossible.

## Hypothesis this session did not test

On gpt-oss the 3000 MiB expert cache buys 45 % hit **at the cost of dense residency** (8 400
faults/token). Since the model is 86 % compute-bound, a *smaller* expert cache might leave the dense
weights resident and win overall. One A/B run would settle it.
