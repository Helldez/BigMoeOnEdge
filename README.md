# BigMoeOnEdge

**Run Mixture-of-Experts language models that are larger than a device's RAM, at usable
speed, by streaming only the experts each token actually routes to** — losslessly, and
without forking llama.cpp.

The idea is simple. A MoE layer holds many experts, but each token only uses a few of them —
for Qwen3-30B-A3B, 8 out of 128 per layer, about 6% of the expert weights. So instead of holding
the whole model in RAM, BigMoeOnEdge keeps the small, always-used parts in memory and reads just
the experts a token needs from flash storage, right when it needs them. That lets an **18.5 GB
model run on an 11 GB phone**, with output identical to running the full model in RAM.

The target is mobile: phones are where memory is tight and this trade — trading some speed for a
much smaller memory footprint — is worth making.

- **Measured** (Qwen3-30B-A3B-Q4_K_M, OnePlus 15R, 11.3 GB RAM): **1.71 tok/s** streaming with
  no cache → **3.47 tok/s** with a 4 GiB expert cache and 4 read lanes → **3.98 tok/s** adding
  intra-layer I/O–compute overlap. Trading routing width down to `--n-expert-used 6` adds a
  further **+24%** (5.01 tok/s). See [Benchmarks](#benchmarks).
- **Built on llama.cpp, not a fork.** All the streaming runs through llama.cpp's public API,
  against the stock upstream code — so keeping up with llama.cpp is just a submodule bump. The one
  exception is the optional overlap mode, which needs a tiny (~25-line) addition to llama.cpp, kept
  on a one-commit branch and meant to be dropped the moment upstream ships an equivalent hook. See
  [docs/seam.md](docs/seam.md).
- **Modular by design.** The engine is built from interchangeable parts (the streaming strategy,
  the metrics sink, the run target are all interfaces), so adding a new MoE model is one line in a
  registry — no change to the streaming code. See [Supported models](#supported-models-and-architectures).

> Prior art, credited: this is an engineering package of ideas from AirLLM, Apple's *LLM in a
> flash*, FlexGen, PowerInfer and EdgeMoE — not a novel technique. See
> [docs/limitations.md](docs/limitations.md).

## Features

- **Expert-selective streaming** (`--moe-stream`) — reads only the routed experts per token from
  flash. Loads `use_mmap=true`, repack off, and rebinds each expert tensor onto a streaming
  buffer in the native gguf layout. Fails fast if the model is not MoE.
- **LRU expert cache with an auto budget and ceiling** (`--cache-mb N|auto`, `--cache-ceil-mb`) —
  a fixed MiB budget or one sized to the device (free RAM minus a floor), clamped to
  `[1.5 GiB, full expert-set size]` and re-checked during generation so it shrinks and grows with
  available memory. Cache size is the single biggest throughput lever.
- **Multi-lane O_DIRECT I/O** (`--io-threads 1..8`) — parallel expert-read lanes that bypass the
  page cache, so streaming stays fast without evicting other apps. 4 lanes is the measured UFS 4.x
  sweet spot.
- **Intra-layer I/O–compute overlap** (`--overlap`) — pipelines each layer's async expert reads
  with its FFN compute, hiding flash latency behind the matmul; byte-identical to the serial path.
  Top throughput lever over a warm cache. Requires the fork submodule.
- **Turbo top-k** (`--n-expert-used N`) — overrides the model's active-experts-per-token via a
  llama.cpp `kv_override` (no fork, no patch). Cuts per-token compute and flash I/O roughly in
  proportion; **+22–24% tok/s at k=6**. A speed/quality knob — fewer experts changes the output.
- **Reusable multi-turn Session** (`--session`) — keeps the model loaded and the expert cache warm
  across prompts, reuses the KV prefix between chat turns and prefills only the new suffix. Powers
  the persistent Android chat session.
- **Honest, per-token telemetry** — `--progress`/`--csv` emit a per-token breakdown: compute vs
  cache-management vs flash-I/O vs stall seconds, cache hit rate, flash bytes read, cache
  residency and resizes. The Android panel renders it live.
- **Experimental, default-off**: temporal prefetch (`--prefetch K`, a cold-start/TTFT tool) and
  speculative gating (`--spec-gate`, predicts the next layer's experts with a recall self-governor).
  Both are honest toggles kept for provability; neither helps steady-state throughput on current
  hardware — see [Benchmarks](#benchmarks).
- **Android demo APK** ([`examples/android`](examples/android)) — a multi-turn chat app with a live
  telemetry panel and every streaming knob exposed with a one-line note on what it does.

## Supported models and architectures

Adding a MoE architecture is **one row** in `core/src/moe/arch_registry.cpp` (arch string + expert
tensor suffixes); expert count and per-expert stride are discovered at runtime, so there are no
model-specific constants in the streaming path. Most llama.cpp MoE models share the same
`ffn_{gate,up,down}_exps` naming and are a single row. Full procedure:
[docs/adding-a-model.md](docs/adding-a-model.md).

| Architecture | Reference models | Expert layout | Notes |
|---|---|---|---|
| `qwen3moe` | Qwen3-30B-A3B and siblings | `ffn_{gate,up,down}_exps` (separate) | Shipped default; validated in the benchmarks below |
| `qwen2moe` | Qwen2 MoE family | `ffn_{gate,up,down}_exps` (separate) | Same seam as qwen3moe |
| `gemma4` | Gemma 4 MoE (e.g. 26B-A4B) | `ffn_gate_up_exps` (**fused** gate+up) + `ffn_down_exps` | Fused gate+up = 2× per-expert stride; an always-on dense/shared expert stays mmap-resident, lowering the streamed fraction |

Run `bmoe-cli --list-archs` to print the compiled-in set.

## Benchmarks

All figures are **measured**, not modelled. Device: **OnePlus 15R** (Android 16, arm64-v8a,
**11.3 GB RAM**, UFS 4.x), 4 compute threads, 256-token steady-state greedy decode. Models
(both Q4_K_M): **Qwen3-30B-A3B** (18.5 GB, 128 experts, top-8, 48 layers, ≈1.64× RAM) and
**Gemma-4-26B-A4B-it** (17.0 GB, fused gate+up, ≈1.51× RAM). Method, per-token distributions
(min/max/median/p5/p95) and device-pressure numbers: [docs/benchmarks.md](docs/benchmarks.md),
[docs/benchmark-method.md](docs/benchmark-method.md).

### Qwen3-30B-A3B-Q4_K_M — cache and lanes

| Expert cache | I/O lanes | tok/s (mean) | flash read/token | cache hit |
|---:|---:|---:|---:|---:|
| mmap only (no stream) | — | 2.00 (unstable) | 0 | — |
| off (stream) | 4 | 1.71 | 1051 MiB | — |
| 2000 MiB | 4 | 2.37 | 480 MiB | 53% |
| 4000 MiB | 2 | 3.12 | 225 MiB | 76% |
| 4000 MiB | 4 | 3.47 | 225 MiB | 76% |
| **4000 MiB + overlap** | **4** | **3.98** | 225 MiB | 76% |

Cache size is the dominant lever (2000 → 4000 MiB nearly doubles throughput as the hit rate climbs
53% → 76%); lanes help most when the cache is small. `mmap`-only averages ~2 tok/s but is
**unstable** (single tokens from 0.15 to 8.15 tok/s) and evicts other apps. The cache rule is
**0 or ≥ ~2 GB**: a smaller budget thrashes and is slower than no cache — the engine rejects the
1–1499 MiB band.

### Gemma-4-26B-A4B-it-Q4_K_M — cache and lanes

| Expert cache | I/O lanes | tok/s (mean) | flash read/token | cache hit |
|---:|---:|---:|---:|---:|
| mmap only (no stream) | — | 0.36 | 0 | — |
| off (stream) | 4 | 1.61 | 904 MiB | — |
| 2000 MiB | 4 | 2.24 | 366 MiB | 58% |
| **2000 MiB + overlap** | **4** | **2.78** | 365 MiB | 58% |

Gemma's heavier resident footprint OOMs a 4 GiB cache on this device, so it tops out at
cache-2000 + overlap. Its fused gate+up layout and resident shared expert are handled by the
`gemma4` registry row with no streaming-path changes.

### Turbo top-k (`--n-expert-used 6`)

Every model ships a routing width — the number of experts each token uses. Qwen3-30B-A3B uses 8.
`--n-expert-used 6` forces it down to 6: fewer experts means less compute **and** less flash to
read, so it runs faster.

The rows below are an A/B test: same model, same session, same settings (`--cache-mb 4000
--io-threads 4`), only the routing width changes. **"default" is the model's own width** — this is
the baseline each `k=6` row is compared against. (These runs were done back-to-back on a cool
device, so the default numbers here are a bit higher than the cache/lane table above, which came
from a separate, warmer session — so compare `k=6` only to the `default` row in *this* table.)

| Model | Routing | tok/s (mean) | flash read/token | cache hit | Δ tok/s | Δ flash |
|---|---|---:|---:|---:|---:|---:|
| Qwen3-30B-A3B | default (8 experts) | 4.03 | 224.65 MiB | 76.5% | — | — |
| Qwen3-30B-A3B | **6 experts** | **5.01** | 164.52 MiB | 76.7% | **+24.3%** | −26.8% |
| Gemma-4-26B-A4B | default | 4.09 | 143.50 MiB | 81.7% | — | — |
| Gemma-4-26B-A4B | **6 experts** | **4.99** | 97.81 MiB | 82.8% | **+22.1%** | −31.8% |

**This is the one lossy option.** Everything else here is byte-identical to the full model — the
streaming, cache and overlap change *how* the weights are fetched, never the math. Dropping experts
changes *what* the model computes, so the output differs from the full model and quality can degrade.
It is a deliberate speed-for-quality trade you opt into, and you should judge the quality on your own
task before shipping it. The speed gain tracks the cut: 6 of 8 experts reads ≈¼ less from flash
(−26.8%), which is where most of the +24% comes from.

### Adaptive cache budget (`--cache-mb auto`)

Sizing the budget to the device and capping it (`--cache-ceil-mb`) matches or beats a hand-tuned
fixed budget: on Qwen, adaptive-**capped** at 4000 MiB + 4 lanes + overlap is the current winning
recipe (**5.23 tok/s**, 76% hit); uncapped auto over-allocates (4675 MiB) and regresses. Details:
[docs/adaptive-cache.md](docs/adaptive-cache.md).

### Desktop is not the target (for now)

This project is built for **mobile** — phones are where RAM is scarce and flash streaming earns its
keep. The engine does also run on desktop, and the same trick makes a model larger than the
machine's RAM runnable there (a quick check: Qwen3-30B-A3B-Q4_K_M, 17.3 GiB, on a Windows PC with
14.8 GiB of RAM streamed at **2.58 tok/s**, coherent output). But desktop isn't tuned or a priority
right now — it may get a proper look later. On a machine where the model fits in RAM, just run it
resident; it will be faster.

## Quickstart (host)

```bash
git clone --recursive https://github.com/Helldez/BigMoeOnEdge.git
cd BigMoeOnEdge
scripts/build-host.sh

# stream a MoE model with a device-sized expert cache and 4 read lanes
build/cli/bmoe-cli -m Qwen3-30B-A3B-Q4_K_M.gguf --moe-stream \
  --cache-mb auto --cache-ceil-mb 4000 --io-threads 4 -t 4 -n 48 \
  --chatml -p "Explain MoE routing."
```

- `--cache-mb auto` sizes the expert cache to free RAM (minus `--cache-floor-mb`); `--cache-ceil-mb`
  caps it. Use a fixed `--cache-mb 4000` to pin an exact budget.
- `--overlap` pipelines expert reads with compute (needs the fork submodule).
- `--n-expert-used 6` trades routing width for speed.
- `--no-think` renders the chat template with reasoning off. Omit `--moe-stream` for the plain
  mmap baseline the streaming modes are compared against.

Run the byte-identity gates (proves streamed == resident; needs `python3` with the `gguf` package):

```bash
cd build && ctest --output-on-failure
```

## Quickstart (Android)

A multi-turn chat app with a live telemetry panel is in [`examples/android`](examples/android).
Build the CLI for arm64 with `scripts/build-android.ps1`, then build the APK and push a model.
Settings expose every streaming knob — expert cache with an auto-ceiling, I/O lanes, O_DIRECT,
overlap, the active-experts/top-k knob, a reasoning toggle, and an mmap-baseline switch that turns
streaming off so you can compare modes on the same device — each with a one-line note on whether it
actually helps tok/s. Defaults are the measured winning recipe (auto cache capped, 4 lanes, overlap
on). The conversation keeps the KV between turns and prefills only each new turn; **New chat**
starts over.

A prebuilt debug APK is attached to each [release](https://github.com/Helldez/BigMoeOnEdge/releases).

## How it works, briefly

1. Load the model file-backed (mmap on, weight repack off).
2. A one-token warm-up capture reads the expert tensor pointers from the compute graph via the
   eval-callback, then rebinds them onto streaming buffers.
3. Each token, the callback sees the routing node, reads the selected expert ids, and the expert
   source reads exactly those slices from flash (O_DIRECT) — with an optional LRU cache and a
   parallel read pool — just before that layer's expert matmul runs.

Details: [docs/moe-streaming.md](docs/moe-streaming.md),
[docs/architecture.md](docs/architecture.md).

## License

Apache-2.0. See [LICENSE](LICENSE).
