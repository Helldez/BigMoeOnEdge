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
by the gates. That is the streaming path itself; two opt-in knobs deliberately trade output for
speed on top of it — `--n-expert-used` (fewer experts per token) and
[`--drop-cold-experts`](expert-dropping.md) (skip an expert that would cost a read and was barely
weighted). Both are off unless asked for, which is what keeps the sentence above true by default.

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

## Skipping the bounce copy (`--odirect-zero-copy`)

O_DIRECT requires the file offset, the transfer length and the buffer address to be aligned.
Expert slices are a whole number of pages long, but a gguf's tensor data does not begin on a page
boundary — `general.alignment` is 32 by default, and on the model family measured here every
expert offset lands a constant **1152 bytes** past one. So the reader pulls the enclosing aligned
window into a per-lane bounce buffer and `memcpy`s the payload back by that remainder: **200+ MiB
a token of pure shift correction**, on a device whose decode is already memory-bound.

The shift is only needed because the destination does *not* share the file's remainder. It can.
This engine reserves the per-layer buffers itself (`vm_reserve`, then `tensor->data` is rebound),
so with the flag on each buffer is placed at an address carrying its own tensor's remainder. Every
expert inherits it, because the per-expert stride is a multiple of the page size; the aligned
window then maps onto the buffer at the same relative position and the read goes straight into the
cache, with no copy anywhere.

The window overhangs the payload at both ends, so a read writes a few bytes of its neighbours'
slices. That is safe by construction rather than by luck: buffer and file differ by a constant
offset under this placement, so the overhung bytes receive *their own* correct file contents, and
the pages involved are exactly the ones the entry's page commit already covers. Reads of the same
neighbour concurrent with the overhang see identical values.

The shift has one consequence that is not free, and it cost a full regression before it was found.
Off the page boundary, the first and last page of **every** slice is shared with the neighbouring
expert, where the aligned placement had no partial pages at all. Eviction releases only the pages an
entry fully covers — it must never free one a neighbour is still using — so under the shift each
eviction left two pages behind: bytes the cache counted as freed that the kernel still held. Over a
long session that is tens of MiB a turn, it goes to zram, and the decode pays it back as **major
faults** — measured in the app at 1-7 per token without the flag against 66 on the first turn with
it and 332 on the second, rising turn over turn, which is the shape of a leak rather than a cost.
Eviction now also releases a boundary page once the neighbour sharing it is gone (not resident and
with no speculative read outstanding); the outermost pages of a buffer border its own padding and
are always free to take.

The episode is worth stating because of how it hid: every device run that had measured this feature
was short, single-turn and in a fresh process, where a leak has no time to accumulate. Only the
app's long session shows it.

Measured on device via the CLI (Qwen3.6-35B-A3B Q4_0, cache 2000 MiB, overlap, interleaved cells,
short single-turn runs): **+20%** tok/s, from 3.65 to 4.40 median, with the compute residual falling
0.180 → 0.143 s/token. **That figure predates the boundary-page fix above and was taken in the one
regime where the leak could not show.** In a long app session the same build measured *slower* than
the baseline (4.41-4.57 against 5.02-5.58) until the leak was fixed. Both numbers are owed a
re-measurement on a long session before either belongs in a table.
The bytes read are unchanged (230.77 MiB/token in every cell) and the generated text is byte-identical
with the flag on and off — which is the correctness proof, since the host gates cannot supply one:
on a tiny test model the per-expert stride is not a multiple of the page size, so the placement
declines and the gate proves only that the flag is harmless. The engine says which happened
(`odirect-zero-copy ON|INERT — n/m layer buffers placed`), so a silent no-op cannot be mistaken
for a pass.

It falls back to the bounce whenever the remainder would leave the tensor under-aligned for the
compute kernels (below 64 bytes), the stride is not a multiple of the alignment, or O_DIRECT was
refused. Off by default pending a second confirmation on a cool device.

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
