# The 0.50.0 release tests on a 12 GB and a 24 GB card (2026-11-25)

The release check before 0.50.0 was promoted: the 0.47.2 and 0.50.0 release archives with their CUDA
packs, side by side on the same rented Linux machines and files. No regression was found. These are
the rows the README's speed table takes for 0.50.

| Box | Card | Host | Memory the plan saw | Drive |
|---|---|---|---|---|
| A | one 12 GB card (RTX 5070 class) | an 8-core quota of a 16-core workstation CPU (Zen 2) | 61 GB available of 125 GB | NVMe |
| B | one 24 GB card (RTX 3090 class) | a 16-thread desktop CPU (Rocket Lake) | 22 GB available of 23 GB | NVMe |

Models, the same files on both boxes:
- Qwen3.6-35B-A3B **Q4_K_M** (ggml-org, 21 GB);
- Qwen3.8-Flash-Next 125B GSQ-RCO **Q2_0** (ISTA-DASLab, 66 GB in two files);
- Inkling 2 Mini **IQ4_NL** (ggml-org).

Method:
- `bmoe-server --auto --ctx-size 16384 -m <model>`, nothing else, from each release's archive and
  CUDA pack. Two loads on one data dir per release, the second planning from the first's records.
- One OpenAI-compatible client (`/v1/chat/completions`, streamed, greedy). Two short prompts ("a" and
  "code"), each answered with 256 tokens, twice per load; on the second load, a 4.6k-token prompt
  answered with 256. A tool call, plain and streamed, on every load.
- The page cache was dropped before every load. Decode is timed from the first streamed token to the
  last.

Decode in tokens per second, the range over the four short answers of a load; prefill and first token
on the 4.6k-token prompt.

## Box A: a 12 GB card, 61 GB of RAM available

| Model | Plan | Release | Load 1 | Load 2 | 4.6k: decode | prefill | first token |
|---|---|---|---:|---:|---:|---:|---:|
| Qwen3.6-35B-A3B Q4_K_M | RAM, GPU cache | 0.47.2 | 98.4-100.8 | 98.6-101.2 | 92.1 | 3602 | 1.28 s |
| | | **0.50.0** | 97.6-98.9 | **98.5-101.2** | 92.3 | 3571 | 1.29 s |
| Qwen3.8-Flash-Next Q2_0 | RAM, GPU cache | 0.47.2 | 44.3-50.4 | 44.4-50.3 | 43.9 | 1368 | 3.41 s |
| | | **0.50.0** | 44.6-50.1 | **44.4-50.4** | 43.8 | 1372 | 3.40 s |

Inkling 2 Mini ran at 15 tok/s on this card in both releases, but its answers were a run of `!`
(#731), so its row is left out.

## Box B: a 24 GB card, 23 GB of RAM

| Model | Plan | Release | Load 1 | Load 2 | 4.6k: decode | prefill | first token |
|---|---|---|---:|---:|---:|---:|---:|
| Qwen3.6-35B-A3B Q4_K_M | whole model on the card | 0.47.2 | 153.0-156.0 | 155.5-155.6 | 149.5 | 7451 | 0.62 s |
| | | **0.50.0** | 154.7-155.9 | **155.1-155.5** | 149.9 | 7492 | 0.62 s |
| Qwen3.8-Flash-Next Q2_0 | experts from flash, a 16.3 GiB card cache | 0.47.2 | 11.4-14.2 | 15.6-18.1 | 17.5 | 743 | 6.27 s |
| | | **0.50.0** | 12.0-14.5 | **15.9-18.4** | 18.1 | 767 | 6.08 s |
| Inkling 2 Mini IQ4_NL | whole model on the card | 0.47.2 | 60.3-61.0 | 60.3-60.8 | 57.1 | 996 | 4.76 s |
| | | **0.50.0** | 60.3-60.7 | 60.3-60.8 | 56.9 | 995 | 4.76 s |

Flash-Next is larger than this box's RAM, so its experts stream from the drive. The second load is
faster because it plans from the first's records.

## Notes

- Tool calls passed on every Qwen3.6 and Flash-Next load, and on Inkling on box B.
- On 0.50.0, a `/completion` call followed by a chat at temperature 0 gave the same answer as the chat
  alone.
- The release archive carries `release-mmap` off on Linux in both versions, so #732's change to it
  (Windows) is not exercised here.
