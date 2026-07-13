# BigMoeOnEdge

Run Mixture-of-Experts language models that are **larger than a device's RAM** at usable
speed, by streaming only the experts each token actually routes to.

A MoE layer holds many experts but a single token uses only its top-k of them. For
Qwen3-30B-A3B that is 8 of 128 per layer — about 6% of the expert weights. BigMoeOnEdge
keeps the small, dense parts of the model resident and reads just those routed expert
slices from flash on demand, so an 18.5 GB model runs on an 11 GB phone, losslessly.

- **Measured:** Qwen3-30B-A3B-Q4_K_M on a OnePlus 15R (11.3 GB RAM) → **1.7 tok/s** with no
  cache, **3.47 tok/s** with a 4 GiB expert cache and 4 read lanes, and **3.98 tok/s** with
  intra-layer I/O–compute overlap on top — byte-identical to a full in-memory run. See the
  table below, or [docs/benchmarks.md](docs/benchmarks.md) for the full matrix (Qwen + Gemma,
  mean/min/max/p95, plus device-pressure numbers).
- **Public-API streaming seam.** Expert streaming is driven entirely through llama.cpp's
  public eval-callback and public gguf accessors and works against stock upstream. The one
  exception is the optional intra-layer overlap feature (`--overlap`), which needs a ~25-line
  per-expert readiness hook carried as a single-commit fork branch — an explicit tide-me-over
  that is dropped the moment upstream ships an equivalent callback. Everything else, and the
  serial streamer in full, is a submodule pointer bump away from upstream. See
  [docs/architecture.md](docs/architecture.md) and
  [docs/seam.md § 3](docs/seam.md).
- **Modular.** A ports-and-adapters engine: the streaming strategy, the metrics sink and
  the target are interfaces. Adding a MoE architecture is one registry row
  ([docs/adding-a-model.md](docs/adding-a-model.md)).

> Prior art, credited: this is an engineering package of ideas from AirLLM, Apple's
> "LLM in a flash", FlexGen, PowerInfer and EdgeMoE — not a novel technique. See
> [docs/limitations.md](docs/limitations.md).

## Benchmarks

Qwen3-30B-A3B-Q4_K_M (18.5 GB, 128 experts, top-8, 48 layers) on a OnePlus 15R
(11.3 GB RAM, UFS 4.x), 4 compute threads, 256-token steady-state runs:

| Expert cache | I/O lanes | tok/s (mean) | flash read/token | cache hit |
|-------------:|----------:|-------------:|-----------------:|----------:|
| mmap only    | —         | 2.00 (unstable) | —             | —         |
| off (stream) | 4         | 1.71         | 1051 MB          | —         |
| 2000 MiB     | 4         | 2.37         | 480 MB           | 53%       |
| 4000 MiB     | 2         | 3.12         | 225 MB           | 76%       |
| 4000 MiB     | 4         | 3.47         | 225 MB           | 76%       |
| **4000 MiB + overlap** | **4** | **3.98** | 225 MB       | 76%       |

Cache size is the dominant lever (2000 → 4000 MiB nearly doubles throughput as the hit rate
climbs); read lanes help mainly when the cache is small. `mmap`-only looks comparable on
average but is unstable (single tokens from 0.15 to 8.15 tok/s) and evicts other apps —
streaming with a bounded cache stays responsive. The cache rule is **0 or ≥ ~2 GB**: a
smaller budget thrashes and is slower than no cache. Gemma-4-26B-A4B-Q4_K_M reaches
**2.78 tok/s** (cache 2000 + overlap — its 17 GB resident footprint won't spare a 4 GiB cache,
so cache 4000 OOMs on this device). Full matrix, device-pressure numbers and method:
[docs/benchmarks.md](docs/benchmarks.md), [docs/benchmark-method.md](docs/benchmark-method.md).

The `--overlap` mode pipelines each layer's expert reads with its compute (via the fork's
per-expert readiness hook) instead of blocking on them, hiding flash I/O behind FFN compute.
It is byte-identical to the serial path (gate G4). It pays off **over a warm cache** — the
best-config gain above (3.47 → 3.98 tok/s, per-token flash stall down to 0.06 s) — but
regresses on a cold cache-0 stream (1.71 → 1.27), where there is far more I/O than compute
can mask.

The same works on desktop for a model larger than the machine's RAM. Qwen3-30B-A3B-Q4_K_M
(17.3 GiB) on a Windows PC with 14.8 GiB RAM (1.17× RAM, so it cannot be held resident),
cache 4000 MiB, 4 I/O lanes, 4 threads → **2.58 tok/s**, 861 MiB/token, 44.8% cache hit,
1.28 GiB/s O_DIRECT, coherent output. Streaming is what makes an over-RAM MoE runnable at
all here; if a model fits in RAM, run it resident instead (faster).

## Quickstart (host)

```bash
git clone --recursive https://github.com/Helldez/BigMoeOnEdge.git
cd BigMoeOnEdge
scripts/build-host.sh

# stream a MoE model, cache 4 GiB, 4 read lanes
build/cli/bmoe-cli -m Qwen3-30B-A3B-Q4_K_M.gguf --moe-stream \
  --cache-mb 4000 --io-threads 4 -t 4 -n 48 --chatml -p "Explain MoE routing."
```

Add `--overlap` to pipeline expert reads with compute (needs the fork submodule), and
`--no-think` to render the chat template with reasoning off. Omit `--moe-stream` entirely
for the plain mmap baseline the streaming modes are compared against.

`--n-expert-used N` overrides the model's top-k routing (e.g. 8 → 6 on Qwen3-30B-A3B),
cutting per-token compute and — under `--moe-stream` — flash I/O roughly in proportion.
It is a speed/quality trade-off: fewer active experts **changes the output**. Implemented
purely through a llama.cpp `kv_override` on the arch-prefixed `expert_used_count` metadata
(no fork, no patch); the value must stay in `[1, n_expert]`. `0` (default) keeps the model's
own count. Works with or without streaming.

Run the byte-identity gates (needs `python3` with the `gguf` package):

```bash
cd build && ctest --output-on-failure
```

## Quickstart (Android)

A **multi-turn chat** app with a live telemetry panel is in
[`examples/android`](examples/android). Build the CLI for arm64 with
`scripts/build-android.ps1`, then build the APK and push a model. Its settings expose the
streaming knobs (expert cache with an auto-ceiling, I/O lanes, O_DIRECT, I/O–compute overlap),
the **active-experts / top-k** speed-quality knob, a reasoning toggle, and an **mmap baseline**
switch that turns streaming off entirely so you can compare modes on the same device — each knob
carries a one-line note on what it does and whether it actually helps tok/s. The defaults are the
measured winning recipe (auto cache capped, 4 lanes, overlap on). The panel reports per-token
compute-vs-flash split, cache hit rate and per-turn tok/s; in the mmap baseline it shows only the
honest metrics (the page-cache I/O is not observable). The conversation keeps the KV between turns
and prefills only each new turn; **New chat** starts over.

## How it works, briefly

1. Load the model file-backed (mmap on, weight repack off).
2. A one-token warm-up capture reads the expert tensor pointers from the compute graph
   via the eval-callback, then rebinds them onto streaming buffers.
3. Each token, the callback sees the routing node, reads the selected expert ids, and the
   expert source reads exactly those slices from flash (O_DIRECT), with an optional LRU
   cache and a parallel read pool — just before that layer's expert matmul runs.

Details: [docs/moe-streaming.md](docs/moe-streaming.md).

## License

Apache-2.0. See [LICENSE](LICENSE).
