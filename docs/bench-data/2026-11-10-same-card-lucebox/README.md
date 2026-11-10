# BigMoeOnEdge and Lucebox on one 24 GB card (2026-11-10)

One rented machine for both engines: a 24 GB card (RTX 3090 class, driver 595, CUDA 12.8 toolkit), a
20-core desktop CPU, 94 GB of RAM, a local NVMe drive. The same GGUF files for both, the same
OpenAI-compatible client (`/v1/chat/completions`, streamed, greedy), the same prompts: three short
ones (30-56 tokens) answered with 512 tokens each, then one ~29k-token prompt (concatenated docs of
this repository, then "Summarize the documents above in five bullet points.") answered with 256.
The page cache was dropped before every load. Decode is timed from the first streamed token to the
last; prefill is the prompt over the time to the first token.

- **BigMoeOnEdge**: `bmoe-server --auto --ctx-size 40960 -m <model>`, nothing else.
  Builds: `main` at 9cd9d23, and the 0.41.0 release candidate (main with the pull requests queued for
  it, among them #505, #523-#526 and #528).
- **Lucebox**: `luce_server` built from Luce-Org/lucebox main at bffb5c25, with each model's RTX 3090
  recipe from its `server/docs/RECOMMENDED_SETUPS.md`, plus `--max-ctx 40960 --pflash off` (PFlash
  is lossy prompt compression; its docs ask for it off in matched-context benchmarks):
  - Qwen3.6-35B-A3B: `--target-device cuda:0 --spark --kvflash auto`
  - Gemma 4 26B-A4B: `--target-device cuda:0 --draft gemma-4-26B-A4B-it-DFlash-q8_0.gguf --draft-device cuda:0 --kvflash auto`

Each engine was loaded twice in a row (three times for Gemma on BigMoeOnEdge), since both learn across
restarts: BigMoeOnEdge its routing record and measured knobs, Lucebox Spark its expert placement
(`<model>.gguf.spark.csv`). The tables give the warmed load; the first is in the raw rows.

The 12 GiB case: a small process held all but 12 GiB of the card (`cudaMalloc`), so both engines saw
the free memory of a smaller card. Qwen3.6-35B-A3B (21.1 GB) no longer fits on it.

## Qwen3.6-35B-A3B UD-Q4_K_M, whole card

| Engine | Decode, short prompts | 28.6k prompt: first token | 28.6k prompt: decode |
|---|---:|---:|---:|
| BigMoeOnEdge 0.41.0 rc | **135** | **5.3 s** (5,435 tok/s) | **109** |
| BigMoeOnEdge main | 96-104 | crash (see below) | |
| Lucebox | 108-109 | 37.3 s (766 tok/s) | 88 |

## Qwen3.6-35B-A3B UD-Q4_K_M, 12 GiB of the card free

| Engine | Decode, short prompts | 28.6k prompt: first token | 28.6k prompt: decode |
|---|---:|---:|---:|
| BigMoeOnEdge 0.41.0 rc + #531 | **75-81** | **8.4 s** (3,410 tok/s) | 67 |
| BigMoeOnEdge 0.41.0 rc, and main | 12.5 | 786 s | 8.4 |
| Lucebox | 51-76 | 78.4 s (365 tok/s) | **72** |

Without #531 the planner kept a graph as wide as the context, whose 11.8 GiB of device buffers left
no room for a layer: 0 of 41 on the card, everything on the host. #531 narrows it to 10240 tokens,
all 41 layers go on the card with a 5.2 GiB cache of the experts used most
([2026-11-10-zero-layer-width](../2026-11-10-zero-layer-width/README.md)). Its second load timed
the placement without that cache, as #524 does once (35-36 tok/s); the third kept the cache and
repeated the first.

Lucebox's first load decoded 40-44 tok/s and took 247 s to the long prompt's first token; Spark's
placement learned from that load nearly doubled the decode on the next.

## Gemma 4 26B-A4B-it Q4_K_M, whole card

| Engine | Decode, two explanation prompts | Decode, code prompt | 29.5k prompt: first token, decode |
|---|---:|---:|---:|
| BigMoeOnEdge 0.41.0 rc | **107-115** | 113-114 | **9.7-10.3 s, 63-66** |
| BigMoeOnEdge main | 26 | 27 | crash |
| Lucebox, DFlash drafter | 106-109 | **249-254** | refused |

Lucebox's drafter is a block drafter trained for this model; it pays where the text is predictable
(code), and the target verifies every drafted token. Lucebox refused the long prompt:
`prompt (29475 @ offset 0) exceeds pool 16384; raise --kvflash or enable pflash compression`.

## The crash on main

`ggml-cuda/mmid.cu:145: GGML_ASSERT(nbytes_shared <= smpbo)`. CUDA's `MUL_MAT_ID` routes the tokens
of a batch to their experts with a helper that keeps 4 bytes per token in shared memory, so a batch
past `smpbo / 4` tokens aborts (about 25k on this card; HIP builds compile the same file). Main put
the whole 28.6k-token prompt in one batch, because its graph was as wide as the context. The
release candidate's graphs are 5120-10240 tokens wide.

## Raw rows

`summary.txt` beside this file: one line per request, `engine-case-load prompt ...` as the client
printed it. Prefill there is the prompt over the time to the first token, so it includes the
server's own request handling.
