# BigMoeOnEdge

**A 120-billion-parameter model. On a phone. Generating tokens.**

`gpt-oss-120b` is 58.46 GB. The phone it runs on here — a OnePlus 15R — has **11.3 GB of RAM**. That
is **5.2× more model than memory**: holding it resident is not slow, it is *impossible*. It streams
anyway, at **1.300 tok/s** at the model's own routing width — **2.191 tok/s** if you narrow it — and
to our knowledge this is the first 120B model to generate tokens on a phone at all.

The trick is that a Mixture-of-Experts model only *uses* a sliver of itself per token — 4 of 128
experts per layer for gpt-oss. So BigMoeOnEdge stops pretending the model has to be in RAM: it keeps
the small always-used parts to hand and reads exactly the experts each token routes to, straight
from flash, in the moment it needs them. The weights the model isn't using are simply not in memory.

**And it is not an approximation.** Streaming is byte-for-byte identical to running the full model
resident — same weights, same math, just fetched later. An 18.5 GB Qwen3-30B-A3B on that same 11 GB
phone runs at **5.23 tok/s** producing exactly the bytes the resident model would. There is one
optional lossy knob (narrowing the routing width), and it is always labelled as such.

All of it built on **stock llama.cpp** — no fork — by one person.

- **The flagship: gpt-oss-120b, 5.2× device RAM.** 58.46 GB, 128 experts at top-4, on 11.3 GB of
  phone. **1.300 tok/s** at the default top-4, **2.191 tok/s** at k=2 — against **11.24 s/token** for
  a plain mmap load of the same file, which thrashes the page cache raw on every token. The lever
  that unlocked it was getting the *dense* (non-expert) weights out of the page cache
  (`--dense-weights anon`): major faults per token fell from the hundreds to **6–10**, and throughput
  went **3.2×**. Details, the quality caveat, and the confounds:
  [Benchmarks](#gpt-oss-120b--a-58-gb-model-on-the-phone-52-ram).
- **Lossless on models that exceed RAM.** Qwen3-30B-A3B-Q4_K_M (18.5 GB, 1.64× RAM) at up to
  **5.23 tok/s**, Gemma-4-26B-A4B (17.0 GB) at up to **4.09 tok/s** — both byte-identical to the
  resident model, against **2.00** and **0.36 tok/s** for a plain mmap load of the same files. Trade
  the one lossy knob (top-k) and they reach **5.01 / 4.99 tok/s**. See [Benchmarks](#benchmarks).
- **Built on llama.cpp, not a fork.** All the streaming runs through llama.cpp's public API,
  against the stock upstream code — so keeping up with llama.cpp is just a submodule bump. The one
  exception is the optional overlap mode, which needs a tiny (~25-line) addition to llama.cpp, kept
  on a one-commit branch and meant to be dropped the moment upstream ships an equivalent hook. See
  [docs/seam.md](docs/seam.md).
- **Modular by design.** The engine is built from interchangeable parts (the streaming strategy,
  the metrics sink, the run target are all interfaces), so adding a new MoE model is one line in a
  registry — no change to the streaming code. See [Supported models](#supported-models-and-architectures).

The target is mobile: phones are where memory is tight, and this trade — a little speed for a much
smaller memory footprint — is where it pays off.

> **About the numbers.** Measured on one device (OnePlus 15R, 11.3 GB, UFS 4.x) over `adb shell`,
> 256-token steady-state greedy decode, quoted as the **best observed for that config**. Phone
> throughput depends heavily on device state — thermal headroom and how much RAM the kernel will
> concede right then — so the same command on a hot or memory-squeezed device reads materially lower.
> Distributions, device-pressure numbers and the sessions that disagree with each other are all in
> [docs/benchmarks.md](docs/benchmarks.md). In the demo app the same config reads ~13% below the adb
> figure — see [What to expect in the app](#what-to-expect-in-the-app).

> Prior art, credited: this is an engineering package of ideas from AirLLM, Apple's *LLM in a
> flash*, FlexGen, PowerInfer and EdgeMoE — not a novel technique. See
> [docs/limitations.md](docs/limitations.md).

## Features

- **Expert-selective streaming** (`--moe-stream`) — reads only the routed experts per token from
  flash. Loads `use_mmap=true`, repack off, and rebinds each expert tensor onto a streaming
  buffer in the native gguf layout. Fails fast if the model is not MoE.
- **LRU expert cache with a fixed or auto budget** (`--cache-mb N|auto`, `--cache-ceil-mb`) — a
  fixed MiB budget, or one sized to the device once at load. It keeps the hottest experts resident
  and is the biggest lever on a model whose working set fits (53–82% hit on RAM-fitting Qwen/Gemma).
  The budget must clear **one token's working set** (`token_demand_MiB` in the telemetry) or it
  evicts what it just read: the 1–1499 MiB band is rejected outright. Past RAM the cache only pays
  once the dense weights are out of the page cache too — otherwise the two fight for the same RAM and
  the cache loses. See [docs/pressure.md](docs/pressure.md). Default off (shared-slot streaming).
- **Direct-from-flash reads, O_DIRECT** (`--io-threads 1..8`, `--no-odirect`) — each expert slice
  is read straight into the engine's own buffer, bypassing the page cache that would otherwise hold
  a second copy of weights the engine already caches. Several read lanes run in parallel (4 is a good
  default on UFS 4.x; more lanes pay only where there is enough I/O left unhidden to parallelize),
  with a buffered fallback where O_DIRECT misbehaves.
- **Dense-weight policy** (`--dense-weights mmap|warm|anon`) — how the non-expert weights (embeddings,
  attention, norms, lm_head) are handled: left mmap'd (`mmap`), page-cached once at load (`warm`, the
  default, which kills the >RAM first-token fault storm), or read via O_DIRECT into anon buffers and
  rebound (`anon`, so a reclaim hits zram instead of dropping them to be re-read from flash).
  **Well past RAM this is the single biggest lever the engine has** — on gpt-oss-120b at 5.2× RAM,
  `anon` cuts major faults per token from the hundreds to 6–10 and is worth **3.2×**; near RAM it is
  close to neutral, because there is little refault pressure to remove. The dense loader has its own
  reader, so its O_DIRECT choice is independent of the expert stream's.
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

All figures are **measured**, not modelled, and come from `bmoe-cli` over `adb shell` — see
[What to expect in the app](#what-to-expect-in-the-app) for how that relates to the demo APK.
Device: **OnePlus 15R** (Android 16, arm64-v8a, **11.3 GB RAM**, UFS 4.x), 4 compute threads,
256-token steady-state greedy decode. Models (all Q4_K_M): **gpt-oss-120b** (58.46 GB, 128 experts,
top-4, ≈5.2× RAM), **Qwen3-30B-A3B** (18.5 GB, 128 experts, top-8, 48 layers, ≈1.64× RAM) and
**Gemma-4-26B-A4B-it** (17.0 GB, fused gate+up, ≈1.51× RAM). Method, per-token distributions
(min/max/median/p5/p95) and device-pressure numbers: [docs/benchmarks.md](docs/benchmarks.md),
[docs/benchmark-method.md](docs/benchmark-method.md).

Each number is the **best observed for that config**, and device state moves them a lot — a hot or
memory-squeezed phone reads materially lower on the identical command. Tables are only ever compared
against their own session's baseline, never across sessions.

### gpt-oss-120b — a 58 GB model on the phone (5.2× RAM)

A resident load is physically impossible: there is 5.2× more model than memory. It streams anyway.

| top-k | expert cache | lanes | tok/s | flash read/token | cache hit |
|---:|---:|---:|---:|---:|---:|
| **2** | **2000 MiB** | **8** | **2.191** | 590 MiB | 32.0% |
| 2 | off | 4 | 1.790 | 909 MiB | — |
| 4 (default) | 2000 MiB | 8 | 1.300 | 1292 MiB | 27.1% |
| 4 (default) | 2000 MiB | 4 | 0.998 | 1292 MiB | 27.1% |
| 4 (default) | off | 4 | 0.711 | 1817 MiB | — |
| mmap only (no stream) † | — | — | 0.089 (11.24 s/token) | 0 (page cache) | — |

Common config: `--moe-stream --overlap --dense-weights anon -t 4 --no-think`, 256 tokens.
† A 24-token probe from an older build — indicative, not a matched A/B. The two lane rows are **not**
a clean lane comparison either (they ran at different thermal states), so no lane advice is drawn
from them.

**Getting the dense weights out of the page cache is what unlocked this model.** Streaming already
kept the 58 GB expert bank out of RAM, but the *non-expert* weights stayed mmap-resident — and at
5.2× RAM the kernel reclaims them mid-decode and refaults them from flash a page at a time. The
kernel bills that to `compute`, which is why gpt-oss looked hopelessly compute-bound.
`--dense-weights anon` cut major faults per token from **hundreds to 6–10** and compute from
**0.948 → 0.156 s/tok**: **3.2×**, same kernels.

It also **overturned the old cache advice** here: with the dense set anonymous, a 2000 MiB cache now
beats cache-off (0.998 vs 0.711 at matched lanes, and the cache cell was clocked *lower*). The budget
must clear one token's working set — 1815 MiB at k=4 — which is why 2000 works and 1000 is rejected.

Note `--no-think` is a speed mode, not a free lunch: it drops gpt-oss's reasoning channel, and the
default k=4 then answers `17×23` **wrong** while k=2/3 get it right. Full matrix, confounds and the
quality note: [docs/benchmarks-gpt-oss.md](docs/benchmarks-gpt-oss.md).

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

### Dense-weight policy (`--dense-weights anon`)

How much the all-O_DIRECT dense policy is worth is decided by one number — how far past RAM the model
is:

| Model | × device RAM | majflt/token with `anon` | worth |
|---|---:|---:|---|
| Qwen3-30B-A3B | 1.64× | 149 | ~neutral (4.667 tok/s — below its 5.23 best) |
| Gemma-4-26B-A4B | 1.51× | 1894 | ~neutral |
| **gpt-oss-120b** | **5.2×** | **6–10** | **3.2×** |

Near RAM there is enough headroom to hold the dense set, so there is little refault pressure to
remove and `warm` (the default) is fine. Well past RAM the dense set is *the* bottleneck and `anon`
is transformative. `majflt/token` on the engine's `compute:` line tells you which regime you are in:
hundreds means the dense weights are thrashing. Measured 2026-07-17, data in
[docs/bench-data/2026-07-17/](docs/bench-data/2026-07-17/).

### What to expect in the app

**The tables above are `bmoe-cli` over `adb shell`, not the demo APK** — a benchmark protocol, not a
chat session. The APK lands close: the same gpt-oss config (cache 2000, 8 lanes, O_DIRECT for both
dense and experts, k=2) reports **1.91 tok/s** in the app against **2.191** over adb — ~13% below.

The gap is the protocol, not the app. Measured at true parity (identical argv, same model path, same
moment) the app was actually *slightly faster* — 3.68 vs 3.42 tok/s on Qwen. What separates a
benchmark from a chat turn is reply length (256-token runs vs ~48-token replies, which never leave
the cache warm-up window — worth ~15–20% on their own), device state, and the UI process competing
for the RAM the expert cache wants. The app's telemetry panel reports the same fields as `--csv`, so
you can see which one you are hitting. Analysis:
[docs/warmup-analysis.md](docs/warmup-analysis.md).

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
- `--dense-weights anon` reads the non-expert weights via O_DIRECT instead of leaving them in the
  page cache. Reach for it when the model is **far** past RAM — check `majflt/token` in the run
  summary; hundreds means you need it.
- `--no-think` renders the chat template with reasoning off. Omit `--moe-stream` for the plain
  mmap baseline the streaming modes are compared against.

For a model several × device RAM, the shape that produced the gpt-oss numbers above is:

```bash
build/cli/bmoe-cli -m gpt-oss-120b-Q4_K_M.gguf --moe-stream --overlap \
  --dense-weights anon --cache-mb 2000 --io-threads 8 -t 4 -n 256 \
  --chatml --no-think -p "Explain MoE routing."
```

The model must live on a real filesystem (on Android, `/data/local/tmp/...`, **not** `/sdcard` —
that is FUSE and O_DIRECT silently degrades to buffered there).

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
each with a one-line note on whether it actually helps tok/s. Defaults are the measured winning recipe
for a model near RAM (auto cache capped, 4 lanes, overlap on); for a model several × RAM, switch the
dense-weights policy to `anon` and give the cache a budget above one token's working set. The
conversation keeps the KV between turns and prefills only each new turn; **New chat** starts over.
What the app reports will not match the benchmark tables one-for-one — see
[What to expect in the app](#what-to-expect-in-the-app).

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
