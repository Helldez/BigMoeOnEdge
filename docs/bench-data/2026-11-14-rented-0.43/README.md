# BigMoeOnEdge 0.43.0 on an 8 GB card and on two 16 GB cards (2026-11-14)

Two rented Linux machines, CUDA 12.8, the 0.43.0 release archive with its CUDA pack:

| Box | Card(s) | Host | RAM | Drive |
|---|---|---|---|---|
| A | one 8 GB card (RTX 3070 class), PCIe 4 x16 | a 6-core desktop CPU (12 threads, Alder Lake) | 31 GB | NVMe |
| B | two 16 GB cards (RTX 5060 Ti class) | an 8-core desktop CPU (16 threads, Zen 5) | 60 GB | NVMe |

Models, the same GGUF for every engine on a box:
- Qwen3.6-35B-A3B **UD-Q4_K_M** (Unsloth, 22.1 GB), and Strata's own **UD-Q4_K_XL** with its MTP
  layer (22.9 GB);
- Qwen3.8-Flash-Next 125B GSQ-RCO **Q2_0** (ISTA-DASLab, 66 GB);
- DeepSeek V4 Flash 284B **UD-IQ2_M** (Unsloth, 91 GB).

Method:
- **BigMoeOnEdge**: `bmoe-server --auto --ctx-size 16384 -m <model>`, nothing else. Two loads on one data
  dir per series, the second planning from the first's records. The better load is shown.
- **Strata** 0.2.13.1 (engine 0.1.59.1): through its own launcher, lossless (`--mtp off --drafting off`),
  `--context 16384`.
- **colibri** (main at 0a5504a, CUDA build): `coli import` of the GGUF, then `coli serve --gpu auto`.
- **Client and prompts**: one OpenAI-compatible client (`/v1/chat/completions`, streamed, greedy). Two short
  prompts ("a" and "code"), each answered with 512 tokens, then a 4.7k-token prompt answered with
  256.
- **Timing**: the page cache was dropped before every load. Decode is timed from the first streamed token to the
  last.

## Box A: one 8 GB card, 31 GB of RAM

Decode in tokens per second; prefill and first token on the 4.7k-token prompt.

| Model | Where it lives | Engine | "a" | "code" | 4.7k prompt: decode | prefill | first token |
|---|---|---|---:|---:|---:|---:|---:|
| Qwen3.6-35B-A3B UD-Q4_K_M | RAM, a device cache of the experts used most | **BigMoeOnEdge** | **36.6** | 32.9 | **32.8** | **873** | **5.3 s** |
| | | colibri | 21.4 | 24.6 | 28.3 | 41 | 113 s |
| Qwen3.6-35B-A3B UD-Q4_K_XL (Strata's file) | RAM, device cache | **BigMoeOnEdge**, 16k context | **35.2** | **35.2** | | | |
| | | Strata, 16k context | did not start: 1159 MiB of buffers asked, 880 MiB free on the card | | | | |
| | | Strata, 8k context | 35.0 | 34.9 | 30.7 | 319 | 14.5 s |
| Qwen3.8-Flash-Next Q2_0 | streamed from the drive, RAM cache, device cache | BigMoeOnEdge | 16.3 | 8.6 | | | |
| | | Strata | 34.1 | 27.0 | 25.9 | 300 | 15.5 s |
| | | colibri | 9.9 | 8.3 | | | |

The release CLI on the same box, Qwen3.6-35B-A3B UD-Q4_K_M, `bmoe-cli --auto`, 256 tokens: **39.9**
tok/s at a 2048 context and **43.9** at 16384.

## Box B: two 16 GB cards, 60 GB of RAM

| Model | Where it lives | Engine | "a" | "code" | 4.7k prompt: decode | prefill | first token |
|---|---|---|---:|---:|---:|---:|---:|
| Qwen3.8-Flash-Next Q2_0 | RAM and both cards, a device cache of 18.5 GB | BigMoeOnEdge | 48.0 | 41.4 | 40.4 | 811 | 5.8 s |
| | | Strata | 77.2 | 72.7 | 84.7 | 1965 | 2.4 s |
| | | colibri | 14.8 | 13.6 | 14.4 | 39 | 120 s |
| DeepSeek V4 Flash UD-IQ2_M | streamed from the drive, 50 GB RAM cache, 13 GB on the cards | BigMoeOnEdge | 5.4 | 5.5 | | | |
| | | colibri | 5.8 | 5.8 | 4.6 | 14 | 306 s |

Strata does not run DeepSeek V4 Flash (it runs V4.1 Flash from its own files).

## Plan records

Every model on both boxes also has `bmoe-planlab` records: first load, a 16k context, and a run
replayed from the model's own routing record. Each comes with the box's parts and the PCIe link
read during a run. They feed the hardware simulator and are kept with its data, not here.
