# A card the wide graph left with no layer (2026-11-10)

One rented machine: a 24 GB card (RTX 3090 class, CUDA 12.8), a 20-core desktop CPU, 94 GB of RAM.
Model: Qwen3.6-35B-A3B UD-Q4_K_M (21.1 GB). `bmoe-server --auto --ctx-size 40960`, nothing else
pinned. One OpenAI-compatible client, greedy: three short prompts of 512 tokens each, then one
28,589-token prompt of 256 tokens. The page cache was dropped before every load.

The 12 GiB case: a small process held the rest of the card (`cudaMalloc` of everything but 12 GiB),
so `--auto` saw the free memory of a smaller card and planned against it.

| Build | Card | Graph width | Layers on the card | Decode, short prompts | 28.6k prompt: first token, decode |
|---|---|---|---|---|---|
| main (9cd9d23) | whole 24 GB | 40960 (the context) | 41 of 41, 8.7 GiB cache | 96-104 tok/s | crash (`mmid.cu` shared-memory assert) |
| #523 + #524 + #526 | whole 24 GB | 5120 | 41 of 41, whole model | 135 tok/s | 5.3 s, 109 tok/s |
| main (9cd9d23) | 12 GiB free | 40960 | **0 of 41** | 12.5 tok/s | 786 s, 8.4 tok/s |
| #523 + #524 + #526 | 12 GiB free | 40960 | **0 of 41** | 12.5 tok/s | 779 s, 8.4 tok/s |
| the 0.41 integration + this fix | 12 GiB free | 10240 | 41 of 41, 5.2 GiB cache | 75-81 tok/s | 8.4 s, 67 tok/s |

The fixed build's second load decoded at 35-36 tok/s: that is the device-cache trial of #524 timing
the fitter's placement without the cache (37.6 tok/s against 86.8 with it). The third load kept the
cache and repeated the first: 75-81 tok/s, 8.4 s and 67 tok/s on the long prompt.

Upstream's own fitter (`llama-fit-params`, same submodule, same 12 GiB free) agrees: at
`-ub 40960` it places `-ngl 0`, at `-ub 5120` all 41 layers with the experts of about 33 of them on the host.

The `mmid.cu` assert on main: the expert-routing helper of CUDA's `MUL_MAT_ID` keeps 4 bytes per
token of the batch in shared memory, so a batch past `smpbo / 4` tokens (about 25k on this card)
aborts. A graph as wide as the context put the whole 28.6k-token prompt in one batch. The narrower
graphs above stay well under it.
