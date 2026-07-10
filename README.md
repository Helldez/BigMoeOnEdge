# BigMoeOnEdge

Run Mixture-of-Experts language models that are **larger than a device's RAM** at usable
speed, by streaming only the experts each token actually routes to.

A MoE layer holds many experts but a single token uses only its top-k of them. For
Qwen3-30B-A3B that is 8 of 128 per layer — about 6% of the expert weights. BigMoeOnEdge
keeps the small, dense parts of the model resident and reads just those routed expert
slices from flash on demand, so an 18.5 GB model runs on an 11 GB phone, losslessly.

- **Measured:** Qwen3-30B-A3B-Q4_K_M on a OnePlus 15R (11.3 GB RAM) → **~1.8 tok/s**
  (0.55–0.6 s/token), byte-identical to a full in-memory run. See the table below.
- **No fork of llama.cpp.** Expert streaming is driven entirely through llama.cpp's
  public eval-callback and public gguf accessors. The `third_party/llama.cpp` submodule
  is stock upstream; updating it is a pointer bump, not a rebase. See
  [docs/architecture.md](docs/architecture.md).
- **Modular.** A ports-and-adapters engine: the streaming strategy, the metrics sink and
  the target are interfaces. Adding a MoE architecture is one registry row
  ([docs/adding-a-model.md](docs/adding-a-model.md)).

> Prior art, credited: this is an engineering package of ideas from AirLLM, Apple's
> "LLM in a flash", FlexGen, PowerInfer and EdgeMoE — not a novel technique. See
> [docs/limitations.md](docs/limitations.md).

## Benchmarks

Qwen3-30B-A3B-Q4_K_M (18.5 GB, 128 experts, top-8, 48 layers) on a OnePlus 15R
(11.3 GB RAM, UFS 4.x), 4 compute threads:

| Expert cache | I/O lanes | s/token | tok/s | flash read/token | cache hit |
|-------------:|----------:|--------:|------:|-----------------:|----------:|
| off          | 1         | 0.95    | ~1.05 | 1050 MB          | —         |
| 4000 MiB     | 1         | 0.73    | ~1.4  | 496 MB           | 46%       |
| **4000 MiB** | **4**     | **0.55–0.57** | **~1.8** | 248–538 MB | 59–67% |

Decode is flash-I/O-bound (~79% I/O at the best setting). The cache rule is **0 or
≥ ~2 GB** — a budget smaller than one token's routed working set thrashes and is slower
than no cache at all. Full method: [docs/benchmark-method.md](docs/benchmark-method.md).

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
