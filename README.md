# BigMoeOnEdge

**Run Mixture-of-Experts models far bigger than your edge device's RAM.**

> **The result: a ~60 GB model on a 12 GB phone: 1.3 tok/s lossless, byte-identical to running
> from RAM, 2.2 tok/s with one speed knob. CPU-only, through llama.cpp's public API.**

A Mixture-of-Experts (MoE) model is made of many small "experts", and each generated token only
uses a few of them. BigMoeOnEdge takes that literally: it keeps the small always-needed parts of
the model at hand and reads just the experts each token asks for, directly from flash storage, at
the moment they're needed. The rest of the model stays on disk. That's what makes large open MoE
models (Qwen3, Qwen3.6, Gemma, gpt-oss and most of their relatives) runnable on edge devices whose RAM they
exceed several times over. The focus today is phones, where memory is tightest.

The flagship case: **gpt-oss-120b**, a ~60 GB model (Q4_K_M), on a phone with 12 GB of RAM. That's
about five times more model than memory, so holding it resident is simply impossible. It runs
anyway, at **1.3 tok/s** with the model's own settings (**2.2 tok/s** with one speed knob), against
**0.09 tok/s** for the same file loaded the ordinary way (mmap).

<p align="center"><img src="docs/assets/hero.gif" width="380" alt="gpt-oss-120b (~60 GB) generating on a 12 GB phone, with live tok/s and telemetry"></p>
<p align="center"><em>gpt-oss-120b (~60 GB) on a 12 GB phone — real time, not sped up. Full three-model demo below.</em></p>

https://github.com/user-attachments/assets/f899b93f-c7c4-4ce9-9fb0-5ed1bae13761

<p align="center"><em>Left to right: gpt-oss-120b (~60 GB), Qwen3-30B-A3B (18.5 GB), Gemma-4-26B-A4B (17 GB) —
recorded in the demo app on the OnePlus 15R, real time, not sped up.</em></p>

**And it's all plain CPU inference.** No GPU, no NPU, no special hardware: four CPU cores, the
phone's flash storage, and nothing else. The entire budget is a phone's UFS storage, a fraction of
the bandwidth a desktop NVMe drive offers, which is exactly why the expert cache, the dense-weight
policy and the read/compute overlap all have to earn their keep.

The streaming path works entirely through llama.cpp's **public API**, so following new llama.cpp
releases is a routine submodule bump, not a merge. The one exception is the optional `--overlap`
flag, which needs a per-expert wait point inside the CPU MoE kernel that no public API exposes: it
carries a single ~25-line hook, as one commit on the fork branch `bmoe/expert-ready-hook`. The hook
is zero-cost when unregistered, everything else builds against stock upstream, and it is dropped the
moment upstream ships an equivalent. Details in [docs/seam.md](docs/seam.md).

Highlights:

- **gpt-oss-120b (Q4_K_M), ~5× device RAM**: **1.3 tok/s** at the model's own routing width against
  0.09 tok/s for the same file loaded the ordinary way (mmap), a **14×** difference at matched settings.
  **2.2 tok/s** with the measured lossy knob on (fewer experts).
- **Lossless on models past RAM**: Qwen3-30B-A3B (Q4_K_M, 18.5 GB) up to **5.2 tok/s**,
  Qwen3.6-35B-A3B (Q4_K_M, 22.3 GB) up to **5.0 tok/s** and Gemma-4-26B-A4B (Q4_K_M, 17.0 GB) up to
  **4.1 tok/s** on the same phone, output identical to the resident model.
- **The problem only phones have**: Android's reclaim keeps taking back the
  always-used weights mid-generation. `--dense-weights anon` feature puts them where reclaim can't cheaply
  take them, and it's worth **3.2×** on gpt-oss-120b on its own.
- **Easy to extend**: a new MoE architecture is one row in a registry, and nothing about a specific
  model is hardcoded in the streaming path. Because the engine sits *on* stock llama.cpp instead of
  replacing it, quantization formats, tokenizers and chat templates come for free: MXFP4 and Q4_K_M
  stream through the same code, since the per-expert stride is read from the model file at runtime.

> **About the numbers.** Measured on one device (OnePlus 15R: 12 GB RAM, 11.3 GB usable, UFS 4.x
> storage - **...imagine with UFS 5.x...**) over `adb shell`, 256-token greedy decode. Each number is the best observed for that
> configuration, and rows in a table can come from different benchmark sessions. Phone throughput
> moves a lot with device state (heat, free memory), so the same command can read lower. Full
> method and distributions: [docs/benchmarks.md](docs/benchmarks.md).

> Prior art, credited: this is an engineering package of ideas from AirLLM, Apple's *LLM in a
> flash*, FlexGen, PowerInfer and EdgeMoE, not a novel technique. The closest recent work is
> [flash-moe](https://github.com/danveloper/flash-moe): a purpose-built Metal engine that streams a
> 397B MoE from SSD on Apple Silicon, and on an iPhone through a community fork. BigMoeOnEdge takes
> the other side of that problem: CPU-only, on Android, on llama.cpp's public API (bar one optional
> ~25-line hook, above), across architectures.
> See [docs/limitations.md](docs/limitations.md).

## Try it on your phone

No build needed. Install the APK from the
[latest release](https://github.com/Helldez/BigMoeOnEdge/releases/latest), open the **Get a
model** card, and tap one of the catalog entries — Qwen3-30B-A3B (~18.6 GB), Qwen3.6-35B-A3B
(~22.3 GB) or Gemma-4-26B-A4B (~17 GB), each past most phones' RAM. When the download finishes,
pick the model and chat; the telemetry panel shows tok/s and the compute-vs-flash split live,
and every streaming knob below is in Settings.

The flagship gpt-oss-120b ships from Hugging Face as two shards, so it needs a one-time merge
and a manual copy to the device: steps in the
[Android example README](examples/android/README.md#gpt-oss-120b).

## Features

- **Expert streaming** (`--moe-stream`): reads only the experts each token routes to, straight
  from flash.
- **Expert cache** (`--cache-mb N|auto`): keeps the most-used experts in RAM, sized by hand or to
  the device; the biggest lever when the model's working set fits.
- **Direct flash reads** (`--io-threads N`): parallel read lanes that bypass the OS page cache.
- **Dense-weight policy** (`--dense-weights mmap|warm|anon|ahwb`): how the always-used (non-expert)
  weights are held in memory. The decisive setting for models far past RAM: on gpt-oss-120b it's
  worth **3.2×** on its own. `ahwb` goes further on Android — dma-buf memory the kernel cannot
  reclaim *even to zram*, measured at **+17.9%** over `anon` on a long generation
  ([data](docs/bench-data/2026-07-21-pinned-dense-ab/findings.md)); off by default, measured on one
  device.
- **I/O–compute overlap** (`--overlap`): hides flash latency behind compute. Byte-identical;
  needs a small optional add-on to llama.cpp (see [docs/seam.md](docs/seam.md)).
- **Turbo top-k** (`--n-expert-used N`): the measured lossy knob. Fewer experts per token, ~+22–24%
  speed, output quality is yours to judge.
- **Cache-aware expert dropping** (`--drop-cold-experts F`): skips a routed expert only when it would
  cost a flash read *and* the router barely weighted it, so quality is spent only where it buys I/O.
  Replayed against recorded traces it avoids ~3× the reads of turbo top-k at a comparable weight
  cost. It is the one setting whose output is **not reproducible** — what gets skipped depends on
  what the cache held — so it has no rows in the tables below, which are a deterministic protocol.
  Off by default everywhere, pending an on-device A/B. See
  [docs/expert-dropping.md](docs/expert-dropping.md).
- **Multi-turn sessions and live telemetry**: the model stays loaded across chat turns, and every
  run can emit a per-token breakdown of where the time went.
- **Android demo app** ([`examples/android`](examples/android)): a chat app with a live telemetry
  panel and every knob above exposed in Settings.

## Supported models

| Architecture | Reference models | Notes |
|---|---|---|
| `qwen3moe` | Qwen3-30B-A3B and siblings | Shipped default, validated below |
| `qwen35moe` | Qwen3.6-35B-A3B and siblings | Hybrid attention/SSM stack; routed experts stream unchanged |
| `qwen2moe` | Qwen2 MoE family | Same layout as qwen3moe |
| `gemma4` | Gemma 4 MoE (e.g. 26B-A4B) | Fused expert layout, handled by its registry row |
| `gpt-oss` | OpenAI gpt-oss-20b / 120b | Purely routed; MXFP4 weights stream unchanged |
| `lfm2moe` | Liquid AI LFM2 / LFM2.5 MoE (e.g. 8B-A1B) | Hybrid conv/attention stack with leading dense blocks; those stay resident |

Adding an architecture is one row in the registry; expert counts and layouts are discovered from
the model file at runtime. Procedure: [docs/adding-a-model.md](docs/adding-a-model.md).
`bmoe-cli --list-archs` prints the compiled-in set.

## Benchmarks

All figures are measured, not modelled. Device and protocol are in the note above; all models are
Q4_K_M. **How to read the tables**: each row is one configuration of the same model on the same
phone.

- **Configuration**: the streaming settings used. *mmap baseline* is the same file loaded the
  ordinary way (no streaming), which is what the streamed rows are compared against. *k* is how many
  experts each token routes to — for us the number of experts, i.e. `n_expert_used` (set with
  `--n-expert-used`). Each table shows the model's default width and, where measured, a reduced *k*,
  the measured lossy setting — see [Turbo top-k — the measured lossy option](#turbo-top-k--the-measured-lossy-option)
  below.
- **tok/s**: generation speed; higher is better.
- **Flash/token**: data read from storage per generated token; lower means the cache is working.
- **Cache hit**: share of expert reads served from RAM instead of flash.

Bold marks the best configuration for that model.

### gpt-oss-120b (Q4_K_M) — ~60 GB on a 12 GB phone

| Configuration | tok/s | Flash/token | Cache hit |
|---|---:|---:|---:|
| **streamed, k=2, cache 2000 MiB, 8 lanes** | **2.2** | 590 MiB | 32% |
| streamed, k=2, no cache, 4 lanes | 1.8 | 909 MiB | — |
| streamed, default k=4, cache 2000 MiB, 8 lanes | 1.3 | 1292 MiB | 27% |
| streamed, default k=4, no cache, 4 lanes | 0.7 | 1817 MiB | — |
| mmap baseline (no streaming) | 0.09 | — | — |

All streamed rows use `--overlap --dense-weights anon --no-think`. The setting that unlocked this
model is `--dense-weights anon`: this far past RAM the phone keeps reclaiming the always-used
weights mid-generation, and moving them out of the page cache was worth **3.2×** by itself. Note
that `--no-think` disables the model's reasoning and costs quality at k=4; the full matrix and
quality notes are in [docs/benchmarks-gpt-oss.md](docs/benchmarks-gpt-oss.md).

### Qwen3.6-35B-A3B (Q4_K_M) — 22.3 GB

A hybrid attention/SSM MoE (256 experts, top-8, 41 blocks): most layers are linear attention
(Gated Delta Net) rather than full attention, and the routed experts stream exactly like a plain
`qwen3moe`. At ~2× device RAM the mmap baseline collapses into a fault storm; streaming with the
dense weights kept out of the page cache (`--dense-weights anon`) runs it stably.

| Configuration | tok/s | Flash/token | Cache hit |
|---|---:|---:|---:|
| mmap baseline (no streaming) | 0.1 (unstable) | — | — |
| streamed, default k=8, cache 2000 MiB, 4 lanes, overlap | 4.3 | 206 MiB | 56% |
| streamed, default k=8, cache 3000 MiB, 4 lanes, overlap | 5.0 | 144 MiB | 65% |
| streamed, k=6, cache 2000 MiB, 4 lanes, overlap | 5.4 | 137 MiB | 60% |
| **streamed, k=6, cache 3000 MiB, 4 lanes, overlap** | **5.8** | 91 MiB | 68% |

All streamed rows use `--overlap --dense-weights anon`. A larger cache is the main lossless lever
(cache 3000 is worth +16% over 2000); the k=6 rows are the measured lossy option (turbo top-k, below),
worth a further ~16% by routing to six experts instead of eight. The lossless best here is cache
3000 at the model's own width, **5.0 tok/s** — output byte-identical to the resident model.

> Unlike the tables above, these Qwen3.6 figures are a single 96-token run rather than the
> 256-token best-of protocol — treat them as indicative, and not strictly comparable to the other
> models until re-measured under the full protocol.

### Qwen3-30B-A3B (Q4_K_M) — 18.5 GB

| Configuration | tok/s | Flash/token | Cache hit |
|---|---:|---:|---:|
| mmap baseline (no streaming) | 2.0 (unstable) | — | — |
| streamed, default k=8, no cache, 4 lanes | 1.7 | 1051 MiB | — |
| streamed, default k=8, cache 2000 MiB, 4 lanes | 2.4 | 480 MiB | 53% |
| streamed, default k=8, cache 4000 MiB, 4 lanes | 4.0 | 225 MiB | 76% |
| **streamed, default k=8, auto cache (capped 4000 MiB), 4 lanes, overlap** | **5.2** | 225 MiB | 76% |
| streamed, k=6, cache 4000 MiB, 4 lanes | 5.0 | 165 MiB | 77% |

Cache size is the dominant lever here, and the auto-sized cache with a ceiling
([docs/cache-sizing.md](docs/cache-sizing.md)) is the winning recipe. The mmap baseline
averages ~2 tok/s but swings wildly token to token and evicts other apps. Turbo top-k (k=6) trades
a little quality for a further +24% over the model's own width.

### Gemma-4-26B-A4B (Q4_K_M) — 17.0 GB

| Configuration | tok/s | Flash/token | Cache hit |
|---|---:|---:|---:|
| mmap baseline (no streaming) | 0.4 | — | — |
| streamed, default k=8, no cache, 4 lanes | 1.6 | 904 MiB | — |
| streamed, default k=8, cache 2000 MiB, 4 lanes | 2.2 | 366 MiB | 58% |
| streamed, default k=8, cache 2000 MiB, 4 lanes, overlap | 2.8 | 365 MiB | 58% |
| streamed, default k=8, cache 4000 MiB, 4 lanes | 4.1 | 144 MiB | 82% |
| **streamed, k=6, cache 4000 MiB, 4 lanes** | **5.0** | 98 MiB | 83% |

Gemma keeps more of itself permanently resident, so the 4000 MiB cache fits only when enough RAM is
free at launch; cache 2000 + overlap is the dependable everyday setting on this device. Turbo top-k
(k=6) is the fastest here (+22%) but changes the output.

### Turbo top-k — the measured lossy option

Every model here ships a routing width — the number of experts each token uses (8 for the Qwen and
Gemma models, 4 for gpt-oss). Forcing it lower with `--n-expert-used` cuts both compute and flash
reads; the `k=6` rows folded into the tables above are that knob, measured A/B against each model's
own width. It is worth **+22–24%** on the Qwen and Gemma models, and takes gpt-oss from 1.3 to
**2.2 tok/s** (k=2).

Every benchmarked setting other than this one changes *how* weights are fetched, never the math.
This knob changes *what* the model computes: output differs from the full model and quality can
degrade. Judge it on your own task before relying on it.

It also spends quality indiscriminately: the tail of the routing goes whether or not those experts
were already in RAM, and a resident expert costs no flash read at all.
[Cache-aware dropping](docs/expert-dropping.md) is the experimental answer to that — same kind of
trade, but only where it buys I/O. It has no measured rows here yet, which is why the tables above
are still turbo top-k's.

### What to expect in the app

The tables above are a benchmark protocol over `adb`, not a chat session. The demo app lands close:
the best gpt-oss config reads **1.9 tok/s** in the app against 2.2 over adb, about 13% below. The
gap is the protocol (short chat replies never fully warm the cache), not the app. The app's
telemetry panel reports the same fields as the CLI, so you can see it directly. Analysis:
[docs/warmup-analysis.md](docs/warmup-analysis.md).

### Desktop

The same trick works on desktop where a model exceeds RAM (quick check: Qwen3-30B on a 14.8 GiB
Windows PC streamed at 2.6 tok/s), but mobile is the target and desktop isn't tuned. If the model
fits in RAM, just run it resident.

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

For a model several × device RAM, the shape that produced the gpt-oss numbers is:

```bash
build/cli/bmoe-cli -m gpt-oss-120b-Q4_K_M.gguf --moe-stream --overlap \
  --dense-weights anon --cache-mb 2000 --io-threads 8 -t 4 -n 256 \
  --chatml --no-think -p "Explain MoE routing."
```

The model must live on a real filesystem (on Android `/data/local/tmp/...`, not `/sdcard`). Omit
`--moe-stream` for the plain mmap baseline. The byte-identity gates (streamed == resident) run with
`cd build && ctest --output-on-failure` (needs `python3` with the `gguf` package).

## Quickstart (Android)

The demo app is in [`examples/android`](examples/android): build the CLI for arm64 with
`scripts/build-android.ps1`, build the APK, push a model. Settings expose every streaming knob with
a one-line note on what it does; defaults are the measured winning recipe for a model near RAM. For
a model several × RAM, switch the dense-weights policy to `anon` and give the cache a real budget.
A prebuilt debug APK is attached to each
[release](https://github.com/Helldez/BigMoeOnEdge/releases).

## How it works

The model loads file-backed, and the engine hooks llama.cpp's public evaluation callback. When a
layer picks its experts for the current token, the engine fetches exactly those slices from flash
just in time for the matmul, optionally caching the hottest ones and overlapping reads with
compute. No llama.cpp sources are modified. Design and the exact API contract:
[docs/architecture.md](docs/architecture.md), [docs/seam.md](docs/seam.md).

## Telemetry

A model streamed past RAM fails in ways that all look identical from outside: it's just slow. So
every run accounts for its own time. `--progress` breaks each token down into flash I/O, cache
management and compute, next to the cache hit rate and the bytes read; `--csv` adds the memory
picture those numbers have to be read against. The Android app renders the same feed live while you
chat, which is how you watch a phone quietly take its memory back mid-answer.

What makes it worth reading is that the engine is candid about its own numbers. Compute, in
particular, isn't measured: it's whatever time is left once the measured parts come out, so it
silently absorbs page faults and throttled cores. Rather than let that pass, the engine ships the
counters that pull the residual apart, and marks anything it couldn't measure as unmeasured instead
of quietly reporting zero.

When the per-token split isn't enough, two diagnostics go further: one records what the router
asked for on every token, the other times the compute graph node by node. Both perturb the run they
measure, and both say so.

Formats and schemas: [docs/telemetry.md](docs/telemetry.md).

## Documentation

[docs/](docs/README.md) is indexed by what you're trying to do: understand the design, extend it,
or reproduce the measurements. Most-wanted entry points:

- [docs/architecture.md](docs/architecture.md): the layer map and the llama.cpp relationship.
- [docs/adding-a-model.md](docs/adding-a-model.md): supporting a new MoE architecture.
- [docs/benchmarks.md](docs/benchmarks.md): measured results and
  [how they were produced](docs/benchmark-method.md).
- [docs/telemetry.md](docs/telemetry.md): the per-token line protocol, the CSV schema and the traces.
- [docs/android-memory.md](docs/android-memory.md): what reclaims the engine's memory on a phone.

## License

Apache-2.0. See [LICENSE](LICENSE).
