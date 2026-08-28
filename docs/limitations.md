# Limitations and prior art

## Prior art

BigMoeOnEdge is an engineering package, not a new technique. The ideas it combines:

- **AirLLM** — layer-by-layer streaming of >RAM models from disk.
- **Apple, "LLM in a flash"** — flash-aware weight streaming, windowing, sparsity-driven
  loading.
- **FlexGen** — offloading and I/O-bound throughput scheduling for large models.
- **PowerInfer / EdgeMoE** — hot/cold expert locality and expert-granularity residency on
  the edge.

The contribution here is a clean, modular, llama.cpp-native implementation of
expert-selective streaming that stays lossless and runs on the public API — no fork for the
serial path, and only a single ~25-line hook (with an explicit sunset) for the optional
`--overlap` feature. See [seam.md § 3](seam.md).

## Limitations

- **Two settings make output non-reproducible.** Every other knob is deterministic given a
  configuration: `--n-expert-used` changes the output, but changes it the same way on every run.
  [`--drop-cold-experts`](expert-dropping.md) and
  [`--expert-substitute`](cache-aware-substitution.md) decide per routing from live cache state,
  so the same prompt and the same flags can decode differently run to run, and the byte-identity
  gates cannot cover their output — only their machinery. Both off by default in the CLI, and
  neither can be priced by a wide scoring batch: use `--ppl --ppl-step`.
- **n=1 only.** The expert sparsity exists only for single-token decode, so streaming is
  incompatible with speculative decoding or batching. Prefill streams the union of the
  prompt's routed experts (still far below the full bank, but larger than one token's).
- **CPU experts.** Streamed experts are computed on CPU; the rebind targets host memory.
  GPU offload of the streamed experts is not supported (the dense parts can still use the
  GPU). Decode is flash-I/O-bound anyway, so this is rarely the bottleneck.
- **Shared experts stay resident.** Architectures with an always-on shared expert (e.g.
  `gemma4`, `deepseek4`) stream the routed experts but keep the shared expert — and any dense layers —
  resident (in the page cache, or in the engine's own buffers under `--dense-weights anon`),
  so the streamed fraction (and the memory saving) is smaller than for a purely routed model
  like `qwen3moe`. The same applies to architectures whose first blocks are dense by design
  (`lfm2moe` has a `leading_dense_block_count`): those blocks name no expert tensors, so they
  are never streamed.
- **A resident tensor can be larger than RAM, and then it is only ever mmap'd.** `qwen4exp`
  (Qwen3.8-Flash-Next) carries a 51B n-gram embedding table (`per_layer_token_embd`, ~28.8 GB at
  IQ4_NL) that the graph reads sixteen rows at a time through `get_rows`. It is not indexed by
  expert, so the streamer does not bind it, and it is bigger than any phone's memory, so no dense
  policy can make it resident: such a tensor stays mmap'd whatever `--dense-weights` asks, and
  the engine says so at load. The streamed fraction of this architecture is therefore unusually
  low, and its per-token cost on that table is page faults on kilobyte reads rather than
  streamed expert bytes. That cost has not been measured on a device yet.
- **Streaming does not help a model that fits.** The engine's reason to exist is a model
  larger than RAM. Registering an architecture says the layout streams losslessly, not that
  streaming is the fast way to run every model using it — a small MoE that fits in memory is
  faster loaded resident, and the registry rows are about coverage, not a recommendation.
- **Repack must stay off.** Loading uses `use_extra_bufts=false`; you cannot combine
  streaming with weight repacking.
- **macOS reads uncached, and `o_direct` says so — but it is not `O_DIRECT`.** A direct request on
  Apple is served by `fcntl(F_NOCACHE)` on each descriptor: the kernel stops caching that file's
  pages, which is the property the design wants. It is a caching hint, not an I/O mode — no
  alignment contract, no DMA promise — so reads keep ordinary `pread` semantics and the raw-read
  ceiling can sit below Linux `O_DIRECT`'s. Measured on one 16 GB Apple-silicon Mac with the model
  on an external volume (256-token protocol, buffered vs `F_NOCACHE`, interleaved A B B A A B):
  decode 0.94 vs 0.61 tok/s (+56 %, arm ranges non-overlapping), with the byte stream
  bit-identical between arms (40 822.8 MiB read by both) and cache-hit equal — the gain is not
  fewer reads but the buffered arm's page-cache pollution doubling the compute residual
  (1.22 → 0.60 s/tok) while stall stays flat (0.43 → 0.46 s/tok). The `o_direct` field records
  the open's real outcome on every platform, so a refused or downgraded run reports `0`.
- **No iOS target.** The core is portable C++ and the streaming path has no Android dependency, but
  there is no Xcode project here and iOS does not run command-line binaries, so there is no
  supported way to run or benchmark the engine on an iPhone or iPad.
- **Windows throughput.** The cache's reserve-then-commit-per-slice path is heavier on
  Windows than the POSIX lazy-commit path. The gates run on Windows; the throughput
  targets are stated for Android/Linux.
- **Depends on a ggml scheduling behaviour** (documented in [seam.md](seam.md)) that is
  not a stability-guaranteed contract. Re-verified by the gates on each submodule bump.

## Not goals

- Distributing a model across devices (a different axis).
- Beating a model that already fits in RAM — if it fits, run it resident.
