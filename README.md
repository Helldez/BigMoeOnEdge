# BigMoeOnEdge

**Run Mixture-of-Experts models far bigger than your edge device's RAM.**

A Mixture-of-Experts (MoE) model is made of many small "experts", and each generated token only
uses a few of them. BigMoeOnEdge takes that literally: it keeps the small always-needed parts of
the model at hand and reads just the experts each token asks for, directly from flash storage, at
the moment they're needed. The rest of the model stays on disk. That's what makes large open MoE
models (Qwen3, Gemma, gpt-oss and most of their relatives) runnable on edge devices whose RAM they
exceed several times over. The focus today is phones, where memory is tightest.

The flagship case: **gpt-oss-120b**, a ~60 GB model (Q4_K_M), on a mid-range phone with 12 GB of
RAM. That's about five times more model than memory, so holding it resident is simply impossible.
It runs anyway, at **1.3 tok/s** with the model's own settings (**2.2 tok/s** with one speed knob),
and as far as we know it's the first time a 120B model has generated tokens on a phone at all.

**And it's all plain CPU inference.** No GPU, no NPU, no special hardware: four CPU cores of a
mid-range phone, its flash storage, and nothing else.

Streaming does not change what the model computes: the output is **byte-for-byte identical** to
running the model fully in RAM. Same weights, same math, just fetched later. There is exactly one
optional lossy setting, and it's always labelled as such.

Everything runs on **stock llama.cpp**. Upstream stays untouched, tracked as a plain submodule, and
the streaming works through its public API, so following new llama.cpp releases is a routine
submodule bump, not a merge.

Highlights:

- **gpt-oss-120b (Q4_K_M), ~5× device RAM**: 1.3 tok/s as shipped, 2.2 tok/s with the speed knob.
  The same file loaded the ordinary way manages 0.09 tok/s.
- **Lossless on models past RAM**: Qwen3-30B-A3B (Q4_K_M, 18.5 GB) up to **5.2 tok/s** and
  Gemma-4-26B-A4B (Q4_K_M, 17.0 GB) up to **4.1 tok/s** on the same phone, output identical to the
  resident model.
- **Easy to extend**: supporting a new MoE architecture is one line in a registry; nothing about a
  specific model is hardcoded in the streaming path.

> **About the numbers.** Measured on one device (OnePlus 15R: 12 GB RAM, 11.3 GB usable, UFS 4.x
> storage) over `adb shell`, 256-token greedy decode. Each number is the best observed for that
> configuration, and rows in a table can come from different benchmark sessions. Phone throughput
> moves a lot with device state (heat, free memory), so the same command can read lower. Full
> method and distributions: [docs/benchmarks.md](docs/benchmarks.md).

> Prior art, credited: this is an engineering package of ideas from AirLLM, Apple's *LLM in a
> flash*, FlexGen, PowerInfer and EdgeMoE, not a novel technique. See
> [docs/limitations.md](docs/limitations.md).

## Features

- **Expert streaming** (`--moe-stream`): reads only the experts each token routes to, straight
  from flash.
- **Expert cache** (`--cache-mb N|auto`): keeps the most-used experts in RAM, sized by hand or to
  the device; the biggest lever when the model's working set fits.
- **Direct flash reads** (`--io-threads N`): parallel read lanes that bypass the OS page cache.
- **Dense-weight policy** (`--dense-weights mmap|warm|anon`): how the always-used (non-expert)
  weights are held in memory. The decisive setting for models far past RAM: on gpt-oss-120b it's
  worth **3.2×** on its own.
- **I/O–compute overlap** (`--overlap`): hides flash latency behind compute. Byte-identical;
  needs a small optional add-on to llama.cpp (see [docs/seam.md](docs/seam.md)).
- **Turbo top-k** (`--n-expert-used N`): the one lossy knob. Fewer experts per token, ~+22–24%
  speed, output quality is yours to judge.
- **Multi-turn sessions and live telemetry**: the model stays loaded across chat turns, and every
  run can emit a per-token breakdown of where the time went.
- **Android demo app** ([`examples/android`](examples/android)): a chat app with a live telemetry
  panel and every knob above exposed in Settings.

## Supported models

| Architecture | Reference models | Notes |
|---|---|---|
| `qwen3moe` | Qwen3-30B-A3B and siblings | Shipped default, validated below |
| `qwen2moe` | Qwen2 MoE family | Same layout as qwen3moe |
| `gemma4` | Gemma 4 MoE (e.g. 26B-A4B) | Fused expert layout, handled by its registry row |
| `gpt-oss` | OpenAI gpt-oss-20b / 120b | Purely routed; MXFP4 weights stream unchanged |

Adding an architecture is one row in the registry; expert counts and layouts are discovered from
the model file at runtime. Procedure: [docs/adding-a-model.md](docs/adding-a-model.md).
`bmoe-cli --list-archs` prints the compiled-in set.

## Benchmarks

All figures are measured, not modelled. Device and protocol are in the note above; all models are
Q4_K_M. **How to read the tables**: each row is one configuration of the same model on the same
phone.

- **Configuration**: the streaming settings used. *mmap baseline* is the same file loaded the
  ordinary way (no streaming), which is what the streamed rows are compared against. *k* is the
  number of experts used per token where it differs from the model's default.
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

### Qwen3-30B-A3B (Q4_K_M) — 18.5 GB

| Configuration | tok/s | Flash/token | Cache hit |
|---|---:|---:|---:|
| mmap baseline (no streaming) | 2.0 (unstable) | — | — |
| streamed, no cache, 4 lanes | 1.7 | 1051 MiB | — |
| streamed, cache 2000 MiB, 4 lanes | 2.4 | 480 MiB | 53% |
| streamed, cache 4000 MiB, 4 lanes | 4.0 | 225 MiB | 76% |
| **streamed, auto cache (capped 4000 MiB), 4 lanes, overlap** | **5.2** | 225 MiB | 76% |

Cache size is the dominant lever here, and the auto-sized cache with a ceiling
([docs/adaptive-cache.md](docs/adaptive-cache.md)) is the winning recipe. The mmap baseline
averages ~2 tok/s but swings wildly token to token and evicts other apps.

### Gemma-4-26B-A4B (Q4_K_M) — 17.0 GB

| Configuration | tok/s | Flash/token | Cache hit |
|---|---:|---:|---:|
| mmap baseline (no streaming) | 0.4 | — | — |
| streamed, no cache, 4 lanes | 1.6 | 904 MiB | — |
| streamed, cache 2000 MiB, 4 lanes | 2.2 | 366 MiB | 58% |
| streamed, cache 2000 MiB, 4 lanes, overlap | 2.8 | 365 MiB | 58% |
| **streamed, cache 4000 MiB, 4 lanes** | **4.1** | 144 MiB | 82% |

Gemma keeps more of itself permanently resident, so the 4000 MiB cache fits only when enough RAM is
free at launch; cache 2000 + overlap is the dependable everyday setting on this device.

### Turbo top-k — the one lossy option

Every model ships a routing width (how many experts each token uses; Qwen3 uses 8). Forcing it
lower cuts both compute and flash reads. A/B against the model's own width, same session,
back-to-back:

| Configuration | tok/s | Flash/token | Cache hit |
|---|---:|---:|---:|
| Qwen3-30B, default (8 experts) | 4.0 | 225 MiB | 76% |
| **Qwen3-30B, 6 experts** | **5.0** (+24%) | 165 MiB | 77% |
| Gemma-4-26B, default | 4.1 | 144 MiB | 82% |
| **Gemma-4-26B, 6 experts** | **5.0** (+22%) | 98 MiB | 83% |

Everything else in this README changes *how* weights are fetched, never the math. This knob changes
*what* the model computes: output differs from the full model and quality can degrade. Judge it on
your own task before relying on it.

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

## Documentation

[docs/](docs/README.md) is indexed by what you're trying to do: understand the design, extend it,
or reproduce the measurements. Most-wanted entry points:

- [docs/architecture.md](docs/architecture.md): the layer map and the llama.cpp relationship.
- [docs/adding-a-model.md](docs/adding-a-model.md): supporting a new MoE architecture.
- [docs/benchmarks.md](docs/benchmarks.md): measured results and
  [how they were produced](docs/benchmark-method.md).
- [docs/android-memory.md](docs/android-memory.md): what reclaims the engine's memory on a phone.

## License

Apache-2.0. See [LICENSE](LICENSE).
