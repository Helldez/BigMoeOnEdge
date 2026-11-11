# BigMoeOnEdge 0.41.2 and Lucebox on one 24 GB card (2026-11-11)

One rented machine for both engines:
- a 24 GB card (RTX 3090 class, driver 595);
- an 8-core desktop CPU with a 15-core quota;
- 62 GB of RAM;
- a local NVMe drive.

Both engines used the same GGUF files, the same OpenAI-compatible client (`/v1/chat/completions`, streamed, greedy) and the same prompts:
- two short prompts ("a", 50-89 tokens, and "code", 30-70), each answered with 512 tokens;
- one ~28.6k-token prompt (concatenated docs of this repository, then "Summarize the documents above in five bullet points."), answered with 256.

The page cache was dropped before every load. Decode is timed from the first streamed token to the last; prefill is the prompt over the time to the first token.

- **BigMoeOnEdge**: `bmoe-server --auto --ctx-size 40960 -m <model>`, nothing else. Built on the box from `main` at 16143c9, the 0.41.2 release commit. Two loads on one data dir; the table gives the second, which plans from the first's record.
- **Lucebox**: `luce_server` built from Luce-Org/lucebox main at d972beba, `--max-ctx 40960`. Each engine was given two rows:
  - **lossless**: `--target-device cuda:0 --kvflash auto --pflash off`, plus `--spark` for Qwen3.6 and Laguna (its learned expert placement), no drafter. Loaded twice so Spark had learned; the table gives the second load.
  - **best**: its published RTX 3090 recipe, with PFlash at its default and, for Gemma 4, its DFlash drafter (`--draft gemma-4-26B-A4B-it-DFlash-q8_0.gguf --draft-device cuda:0`).

The 12 GiB case: a small process held all but 12 GiB of the card (`cudaMalloc`), so both engines saw the free memory of a smaller card. Qwen3.6-35B-A3B (21.1 GB) no longer fits on it.

Decode in tokens per second.

## Qwen3.6-35B-A3B UD-Q4_K_M, whole card

| Engine | "a" | "code" | 28.6k prompt: first token | 28.6k prompt: decode |
|---|---:|---:|---:|---:|
| BigMoeOnEdge 0.41.2 | **154.0** | **153.7** | **4.7 s** (6,025 tok/s) | **123.6** |
| Lucebox lossless | 122.7 | 121.2 | 37.1 s (770 tok/s) | 102.1 |
| Lucebox best | 123.2 | 120.7 | 37.1 s | 101.7 |

## Qwen3.6-35B-A3B UD-Q4_K_M, 12 GiB of the card free

| Engine | "a" | "code" | 28.6k prompt: first token | 28.6k prompt: decode |
|---|---:|---:|---:|---:|
| BigMoeOnEdge 0.41.2 | 66.7 | **58.9** | **9.2 s** (3,095 tok/s) | 55.8 |
| Lucebox lossless | **69.4** | 41.6 | 73.8 s (387 tok/s) | **77.0** |
| Lucebox best | 69.6 | 41.8 | 73.4 s | 75.8 |

## Laguna XS 2.1 33B Q4_K_M, whole card

Lucebox's README gives ~100 tok/s for this model on an RTX 3090 with Spark.

| Engine | "a" | "code" | 28.6k prompt: first token | 28.6k prompt: decode |
|---|---:|---:|---:|---:|
| BigMoeOnEdge 0.41.2 | **92.8** | **93.1** | **17.0 s** (1,694 tok/s) | 40.0 |
| Lucebox lossless | 84.3 | 86.1 | refused (`resource_exhausted`) | |

## Gemma 4 26B-A4B Q4_K_M, whole card

| Engine | "a" | "code" | 28.6k prompt: first token | 28.6k prompt: decode |
|---|---:|---:|---:|---:|
| BigMoeOnEdge 0.41.2 | 113.7 | 113.9 | **8.8 s** (3,361 tok/s) | 66.0 |
| Lucebox lossless | **130.8** | **131.7** | refused | |
| Lucebox best (DFlash drafter) | 123.2 | **288.4** | refused | |

## Notes

- Against the 2026-11-10 run on another 3090 box ([2026-11-10-same-card-lucebox](../2026-11-10-same-card-lucebox/README.md)), Lucebox's Qwen3.6 decode rose from 108 to 123. BigMoeOnEdge's rose from 135 to 154.
- BigMoeOnEdge's draft-model branch (#534) could not be measured against the DFlash drafter: its server did not finish loading with the drafter on this card.
