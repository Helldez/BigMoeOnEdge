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
expert-selective streaming that stays lossless and requires no fork.

## Limitations

- **n=1 only.** The expert sparsity exists only for single-token decode, so streaming is
  incompatible with speculative decoding, batching, or a diffusion canvas. Prefill streams
  the union of the prompt's routed experts (still far below the full bank, but larger than
  one token's).
- **CPU experts.** Streamed experts are computed on CPU; the rebind targets host memory.
  GPU offload of the streamed experts is not supported (the dense parts can still use the
  GPU). Decode is flash-I/O-bound anyway, so this is rarely the bottleneck.
- **Three separate projections.** Models that pack gate+up into one expert tensor are not
  yet supported (see [adding-a-model.md](adding-a-model.md)).
- **Repack must stay off.** Loading uses `use_extra_bufts=false`; you cannot combine
  streaming with weight repacking.
- **Windows throughput.** The cache's reserve-then-commit-per-slice path is heavier on
  Windows than the POSIX lazy-commit path. The gates run on Windows; the throughput
  targets are stated for Android/Linux.
- **Depends on a ggml scheduling behaviour** (documented in [seam.md](seam.md)) that is
  not a stability-guaranteed contract. Re-verified by the gates on each submodule bump.

## Not goals

- Distributing a model across devices (a different axis).
- Beating a model that already fits in RAM — if it fits, run it resident.
