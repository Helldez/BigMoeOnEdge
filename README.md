# BigMoeOnEdge

**Run Mixture-of-Experts language models far larger than a device's RAM, at usable speed, by
streaming only the experts each token routes to** — output byte-for-byte identical to the full
model in RAM, built on stock llama.cpp without a fork.

The idea is simple. A MoE layer holds many experts, but each token uses only a few — for
Qwen3-30B-A3B, 8 of 128 per layer, about 6% of the expert weights. So instead of holding the whole
model in RAM, BigMoeOnEdge keeps the small, always-used parts resident and reads just the experts a
token needs from flash, right when it needs them. That lets an **18.5 GB model run on an 11 GB
phone**, identical to running it fully resident.

And it does not stop there. The same trick puts **OpenAI's gpt-oss-120b — a 120-billion-parameter,
58 GB model — on that same 11 GB phone**: **5.2× the device's RAM**, physically impossible to hold
resident, generating coherent tokens **7.7× faster than a plain mmap load**. To our knowledge, the
first 120B model ever to run on a phone — from a project built by one person, on a stock submodule.

The target is mobile: phones are where memory is tight, and this trade — a little speed for a much
smaller memory footprint — is where it pays off.

- **Measured** on a OnePlus 15R (11.3 GB RAM, UFS 4.x): Qwen3-30B-A3B-Q4_K_M runs at up to
  **5.01 tok/s** streamed, against **2.00 tok/s** for a plain mmap load of the same model;
  Gemma-4-26B-A4B up to **4.99 tok/s** vs **0.36 tok/s** for mmap. See [Benchmarks](#benchmarks)
  for the full method, the config behind each number, and the lossless streaming-only figures.
- **The flagship: gpt-oss-120b on a phone.** 58.46 GB, 128 experts at top-4, 5.2× the OnePlus 15R's
  RAM — a resident load is physically impossible, yet it streams and generates. Top-k is the dominant
  lever here (each expert is large, so it is compute-bound): **0.687 tok/s at k=2**, 7.7× a plain
  mmap. The full matrix, the quality note, and the method are in
  [Benchmarks](#gpt-oss-120b--a-58-gb-model-on-the-phone-52-ram).
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
- **LRU expert cache with a fixed or auto budget** (`--cache-mb N|auto`, `--cache-ceil-mb`) — a
  fixed MiB budget, or one sized to the device once at load. It keeps the hottest experts resident
  and is the biggest lever on a model whose working set fits (53–82% hit on RAM-fitting Qwen/Gemma);
  on a >RAM model that routes more than the device concedes, **cache-off is the ceiling** — see
  [docs/pressure.md](docs/pressure.md). Default off (shared-slot streaming).
- **Direct-from-flash reads, O_DIRECT** (`--io-threads 1..8`, `--no-odirect`) — each expert slice
  is read straight into the engine's own buffer, bypassing the page cache that would otherwise hold
  a second copy of weights the engine already caches. Several read lanes run in parallel (4 is the
  UFS 4.x sweet spot), with a buffered fallback where O_DIRECT misbehaves.
- **Dense-weight policy** (`--dense-weights mmap|warm|anon`) — how the non-expert weights (embeddings,
  attention, norms, lm_head) are kept resident: left mmap'd, page-cached once at load (`warm`, the
  default, which kills the >RAM first-token fault storm), or read via O_DIRECT into anon buffers and
  rebound (`anon`, so a reclaim hits zram not flash). The dense loader has its own reader, so its
  O_DIRECT choice is independent of the expert stream's.
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
- **Routing traces** (`--route-trace PATH`) — records which experts every token actually routed to,
  per layer, for offline analysis (`scripts/route-analyze.py`, `scripts/route-viewer.py`). A
  diagnostic: it perturbs the run, so its tok/s are not comparable with the benchmark tables.
- **Experimental, default-off**: temporal prefetch (`--prefetch K`, a cold-start/TTFT tool) reads
  the next layers' likely experts on idle I/O lanes. An honest toggle kept for provability; it does
  not help steady-state throughput on current hardware — see [Benchmarks](#benchmarks).
- **Android demo APK** ([`examples/android`](examples/android)) — a multi-turn chat app with
  Markdown-rendered answers, a live telemetry panel, and every streaming knob exposed with a
  one-line note on what it does.

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
| `gpt-oss` | OpenAI gpt-oss-20b / 120b | `ffn_{gate,up,down}_exps` (separate) | Purely routed (no resident shared expert → high streamed fraction); MXFP4 weights stream unchanged since the stride is read from `nb[2]`, quant-agnostic |

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

Gemma's heavier resident footprint makes a 4 GiB cache unreliable on this device — it can be
OOM-killed depending on how much RAM is free at launch — so its dependable setting tops out at
cache-2000 + overlap. (The Turbo top-k table below did manage cache-4000 on a cooler run; it just
isn't something to count on here.) Its fused gate+up layout and always-resident shared expert are
handled by the `gemma4` registry row with no streaming-path changes.

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

### Auto-sized cache budget (`--cache-mb auto`)

Sizing the budget to the device once at load and capping it (`--cache-ceil-mb`) matches or beats a
hand-tuned fixed budget where the cache fits: on Qwen, auto-**capped** at 4000 MiB + 4 lanes + overlap
is the winning recipe (**5.23 tok/s**, 76% hit); uncapped auto over-allocates (4675 MiB) and regresses.
The budget is fixed for the run — an earlier runtime governor that resized it under memory pressure was
measured to be a net loss on >RAM models and retired ([docs/pressure.md](docs/pressure.md)). Details:
[docs/adaptive-cache.md](docs/adaptive-cache.md).

### gpt-oss-120b — a 58 GB model on the phone (5.2× RAM)

The same engine streams **OpenAI gpt-oss-120b** (58.46 GB, 128 experts, top-4) on the same 11.3 GB
phone — **5.2× device RAM**, so a resident load is physically impossible. To our knowledge this is
the first time a 120B / 58 GB model has generated tokens on a phone.

| top-k | tok/s | s/token | vs mmap |
|---:|---:|---:|---:|
| **2** | **0.687** | 1.455 | **7.7× faster** (mmap 11.24 s/tok) |
| 3 | 0.391 | 2.556 | — |
| 4 (default) | 0.223 | 4.489 | 3.0× faster (mmap 13.34 s/tok) |

Config: `--cache-mb auto --cache-ceil-mb 3000`, O_DIRECT, `--overlap`, 4 lanes, `-t 4`, `--no-think`.
gpt-oss is heavily **compute-bound** (each expert is large), so **top-k is the dominant lever** — k=2
is ~3× faster than the default k=4 — and prefetch only hurts. These are exploratory **24-token** probes
(cache still warming, 13–21% hit), not the 256-token steady state above; treat them as a floor. Full
matrix, the k=4 interruption caveat, and a **quality** note (`--no-think` drops gpt-oss's reasoning, so
default k=4 answers `17×23` *wrong* while k=2/3 get it right): [docs/benchmarks-gpt-oss.md](docs/benchmarks-gpt-oss.md).

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
Settings expose every streaming knob — expert cache (fixed or auto-with-ceiling), I/O lanes, O_DIRECT,
the dense-weights policy (mmap/warm/anon), overlap, the active-experts/top-k knob, a reasoning toggle,
and an mmap-baseline switch that turns streaming off so you can compare modes on the same device —
each with a one-line note on whether it actually helps tok/s. Defaults are the measured winning recipe (auto cache capped, 4 lanes, overlap
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

## Documentation

[docs/](docs/README.md) is indexed by what you are trying to do — understand the design, extend
it, or reproduce the measurements. The entry points most people want:

- [docs/architecture.md](docs/architecture.md) — the layer map, and why llama.cpp is not forked.
- [docs/seam.md](docs/seam.md) — the exact contract with llama.cpp's public API.
- [docs/adding-a-model.md](docs/adding-a-model.md) — supporting a new MoE architecture.
- [docs/telemetry.md](docs/telemetry.md) — the `BMOE_*` line protocol and CSV schema.
- [docs/android-memory.md](docs/android-memory.md) — what reclaims the engine's memory on a phone,
  and which levers actually exist.
- [docs/benchmarks.md](docs/benchmarks.md) — measured results, and [how they were
  produced](docs/benchmark-method.md).

## License

Apache-2.0. See [LICENSE](LICENSE).
