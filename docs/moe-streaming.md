# MoE expert-selective streaming

## The lever

A MoE layer stores `n_expert` experts (128 for Qwen3-30B-A3B) but each token is routed to
only its top-`k` (8). The other experts' weights are never read for that token. If the
model does not fit in RAM, streaming just the routed experts from flash turns "the whole
expert bank per token" into "top-k/n_expert of it" — about 6% for that model.

This sparsity is real **only for autoregressive, one-token-at-a-time decoding**. A batch
of `T` tokens routes the *union* of `T × k` experts, which approaches all of them for any
useful `T`. So streaming deliberately runs at n=1 and is incompatible with speculative
decoding or a canvas — the engine keeps decode single-token by construction.

## Mechanism

1. **Bind.** After a one-token warm-up capture (see [seam.md](seam.md)), every layer's
   three expert tensors (`ffn_{gate,up,down}_exps`) are rebound onto streaming buffers and
   never read from the mmap again.
2. **Route.** The eval-callback marks only the routing node `ffn_moe_topk-<il>` as needed.
   ggml computes it alone, synchronizes, and calls back with the selected expert ids. They
   are gathered **respecting the view strides** — `selected_experts` is a view of the full
   argsort with row stride `nb[1]`, so a flat read would grab the wrong experts and corrupt
   the KV cache.
3. **Load.** The expert source reads exactly those experts' slices from the gguf
   (`O_DIRECT`, page cache bypassed) into each expert's canonical offset inside the bound
   tensor, just before that layer's expert matmul runs.

Ordering is guaranteed by ggml's eval-callback loop: the node we mark is computed and
`ggml_backend_synchronize`'d before the non-ask callback fires, and the following compute
(the expert matmul) runs only after our load returns. The next layer cannot overwrite the
buffers until this layer's matmul has synchronized. Correct on any backend.

The result is **lossless**: byte-identical to running with every expert resident, asserted
by the gates.

## Residency modes

- **Cache off (shared slots).** Three heap buffers (full `n_expert` size) are shared
  across layers — one layer computes at a time. Routed slices are re-read fresh every
  token. Lowest RAM, highest I/O.
- **LRU cache (`--cache-mb N`).** Each `(layer, projection)` gets a reserved,
  lazily-committed address range. A routed expert already resident is a **hit** (no read);
  a miss is read once and kept; over budget, the coldest `(layer, expert)` is evicted and
  its pages physically released (`madvise(MADV_DONTNEED)` / `MEM_DECOMMIT`). RAM is bounded
  for real.

### The cache rule: 0 or ≥ ~2 GB

Expert reuse is broad, not skewed: hit rate rises roughly linearly with budget, with no
small-cache plateau. A budget below one token's routed working set (~1 GB for
Qwen3-30B-A3B) yields zero hits **and** pays eviction overhead — measurably slower than no
cache. So `validate()` rejects a budget in the `1..1499 MiB` band unless you force it. Use
`0`, or `≥ 2000`.

## Parallel reads (`--io-threads N`)

Routed slices are read across `N` lanes, each with a private fd and bounce buffer; the
calling thread participates as lane 0. On UFS 4.x, 4 lanes roughly triples effective read
bandwidth over serial. Compute threads (`-t`) show a U-shape — 4 is the measured optimum;
8 regresses badly because ggml's spin-wait contends with the synchronous reads.

## Why repack must stay off

The streamer rebinds `tensor->data` to a buffer it fills from the file's native byte
layout. `use_extra_bufts=true` would repack Q4_K weights into a different in-memory layout
(e.g. `q4_K_8x8`), so the file offsets would no longer describe what the matmul reads.
The engine loads with `use_extra_bufts=false`; this is load-bearing, not a tuning knob.

## Assumptions to re-check on a submodule bump

- The routing node is named `ffn_moe_topk-<il>` and the expert tensors
  `blk.<il>.ffn_{gate,up,down}_exps.weight`. The recipe isolates these names.
- The eval-callback fires per decode (not skipped by graph reuse) and computes a
  marked node alone before the non-ask callback. The gates catch a regression here.
