# BigMoeOnEdge

Run Mixture-of-Experts language models that are **larger than a device's RAM** at usable
speed, by streaming only the experts each token actually routes to.

A MoE layer holds many experts but a single token uses only its top-k of them. For
Qwen3-30B-A3B that is 8 of 128 per layer — about 6% of the expert weights. BigMoeOnEdge
keeps the small, dense parts of the model resident and reads just those routed expert
slices from flash on demand, so an 18.5 GB model runs on an 11 GB phone, losslessly.

- **Measured:** Qwen3-30B-A3B-Q4_K_M on a OnePlus 15R (11.3 GB RAM) → **1.8 tok/s** with no
  cache, up to **3.75 tok/s** with a 4 GiB expert cache and 4 read lanes, byte-identical to
  a full in-memory run. See the table below, or [docs/benchmarks.md](docs/benchmarks.md) for
  the full matrix (Qwen + Gemma, mean/min/max/p95).
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
| mmap only    | —         | 1.86 (unstable) | —             | —         |
| off (stream) | 4         | 1.78         | 1051 MB          | —         |
| 2000 MiB     | 4         | 2.47         | 480 MB           | 53%       |
| 4000 MiB     | 2         | 3.38         | 225 MB           | 76%       |
| **4000 MiB** | **4**     | **3.75**     | 225 MB           | 76%       |

Cache size is the dominant lever (2000 → 4000 MiB nearly doubles throughput as the hit rate
climbs); read lanes help mainly when the cache is small. `mmap`-only looks comparable on
average but is unstable (single tokens from 0.36 to 8.34 tok/s) and evicts other apps —
streaming with a bounded cache stays responsive. The cache rule is **0 or ≥ ~2 GB**: a
smaller budget thrashes and is slower than no cache. Gemma-4-26B-A4B-Q4_K_M reaches
**3.48 tok/s** at the same best setting. Full matrix and method:
[docs/benchmarks.md](docs/benchmarks.md), [docs/benchmark-method.md](docs/benchmark-method.md).

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

Run the byte-identity gates (needs `python3` with the `gguf` package):

```bash
cd build && ctest --output-on-failure
```

## Quickstart (Android)

A minimal chat app with a live telemetry panel is in
[`examples/android`](examples/android). Build the CLI for arm64 with
`scripts/build-android.ps1`, then build the APK and push a model.

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
