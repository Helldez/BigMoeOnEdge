# BigMoeOnEdge 0.45.0 on 8, 12 and 24 GB cards, against Strata, and the fixes it found (2026-11-18)

Three rented Linux machines, CUDA 12.8 image, NVIDIA driver 580 (CUDA 13.0):

| Box | Card | Host | RAM | Drive |
|---|---|---|---|---|
| A | one 8 GB card (RTX 3060 Ti class), PCIe 3 x16 | a 14-core server CPU (Broadwell) | 62 GB (43 GB this process may hold) | NVMe |
| B | one 12 GB card (RTX 3060 class), PCIe 3 x16 | a 6-core workstation CPU (Skylake) | an 85 GB container on a 125 GB host | NVMe |
| C | one 24 GB card (RTX 3090 class), PCIe 4 x16 | a 6-core desktop CPU (Rocket Lake) | 63 GB | NVMe |

Models, each engine on the same file where it takes it:
- Qwen3.6-35B-A3B **UD-Q4_K_M** (Unsloth, 22.1 GB), the catalog's file;
- Qwen3.6-35B-A3B **UD-Q4_K_XL** with its MTP layer (Unsloth's MTP repository, 22.9 GB): the file
  Strata needs (it refuses a file without the MTP layer), run by both engines;
- Qwen3.8-Flash-Next 125B GSQ-RCO **Q2_0** (ISTA-DASLab, 66 GB in two files).

Method:
- **BigMoeOnEdge**: `bmoe-server --auto --ctx-size 16384 -m <model>`, nothing else, or with `--mtp`
  where the row says so. The 0.45.0 release archive with its CUDA pack: two loads on one data dir,
  the second planning from the first's records, the better load shown. The fix build (main at
  9c1e52e with #646): one load. A row marked "by hand" adds `--ubatch 4096`.
- **Strata** 0.2.15 (engine 0.1.61), through its own launcher, `--context 16384`: its default
  (MTP and drafting as it chooses), and lossless (`--mtp off --drafting off`).
- **Client and prompts**: one OpenAI-compatible client (`/v1/chat/completions`, streamed, greedy).
  Two short prompts ("a" and "code"), each answered with 512 tokens (median of 2-3 repeats), then a
  4.6k-token prompt answered with 256. A tool call, plain and streamed, on every run: all passed.
- **Timing**: the page cache was dropped before every load. Decode is timed from the first streamed
  token to the last.

Decode in tokens per second; prefill and first token on the 4.6k-token prompt.

## Box A: an 8 GB card, 62 GB of RAM

| Model | Engine | "a" | "code" | 4.6k: decode | prefill | first token |
|---|---|---:|---:|---:|---:|---:|
| Qwen3.6-35B-A3B UD-Q4_K_M | **BigMoeOnEdge** 0.45.0 | **22.2** | 20.1 | 20.1 | **1212** | **3.8 s** |
| Qwen3.6-35B-A3B UD-Q4_K_XL | **BigMoeOnEdge** 0.45.0 | **27.4** | 23.9 | 24.4 | 635 | 7.3 s |
| | BigMoeOnEdge 0.45.0, `--mtp` | 25.2 | 24.1 | 27.4 | 601 | 7.7 s |
| | Strata, default and lossless | did not start: "a context of 16384 tokens does not fit on this graphics card" | | | | |
| Qwen3.8-Flash-Next Q2_0 | BigMoeOnEdge 0.45.0 | 15.3 | 13.4 | 11.1 | 193 | 24.2 s |
| | Strata, default | 48.6 | 62.4 | 54.1 | 874 | 5.3 s |
| | Strata, lossless | 50.9 | 47.8 | 49.6 | 1023 | 4.6 s |

## Box B: a 12 GB card, 63 GB of RAM

| Model | Engine | "a" | "code" | 4.6k: decode | prefill | first token |
|---|---|---:|---:|---:|---:|---:|
| Qwen3.6-35B-A3B UD-Q4_K_M | **BigMoeOnEdge** 0.45.0 | **54.9** | **50.5** | 47.7 | **1778** | **2.6 s** |
| | BigMoeOnEdge with #646 | 54.9 | 50.4 | 47.6 | 1777 | 2.6 s |
| Qwen3.6-35B-A3B UD-Q4_K_XL | BigMoeOnEdge 0.45.0 | 52.9 | 49.5 | 47.4 | **1778** | **2.6 s** |
| | **BigMoeOnEdge** 0.45.0, `--mtp` | **58.9** | **60.1** | **58.1** | 1517 | 3.0 s |
| | Strata, default | 40.2 | 36.3 | 28.2 | 906 | 5.1 s |
| | Strata, lossless | 40.4 | 36.5 | 28.4 | 927 | 5.0 s |
| Qwen3.8-Flash-Next Q2_0 | BigMoeOnEdge 0.45.0 | 22.8 | 19.5 | 17.9 | 599 | 7.8 s |
| | BigMoeOnEdge with #646 | 25.8 | 22.0 | 21.1 | 687 | 6.8 s |
| | Strata, default | 44.5 | 46.9 | 44.2 | 1179 | 4.0 s |
| | Strata, lossless | 43.6 | 38.2 | 36.9 | 1204 | 3.9 s |

The CUDA pack of #631 (built per llama.cpp version, 731 MB against the release's 667) in place of
the release's, same archive and box: Qwen3.6 UD-Q4_K_M 54.4 / 50.0 / 47.6 tok/s and 1774 prefill,
Flash-Next 22.6 / 19.4, with every answer's text identical to the release pack's.

## Box C: a 24 GB card, 63 GB of RAM

| Model | Engine | "a" | "code" | 4.6k: decode | prefill | first token |
|---|---|---:|---:|---:|---:|---:|
| Qwen3.6-35B-A3B UD-Q4_K_M | BigMoeOnEdge 0.45.0 | did not load: the context's compute buffers did not fit the card (below) | | | | |
| | **BigMoeOnEdge** with #646 | **151.0** | **150.8** | **144.7** | **6615** | **0.7 s** |
| | BigMoeOnEdge 0.45.0, `--ubatch 4096` by hand | 150.1 | 150.1 | 144.3 | 6184 | 0.8 s |
| Qwen3.6-35B-A3B UD-Q4_K_XL | BigMoeOnEdge 0.45.0 | 134.0 | 134.0 | 128.0 | 5277 | 0.9 s |
| | **BigMoeOnEdge** with #646 | **147.3** | **147.2** | **141.3** | **6172** | **0.8 s** |
| | BigMoeOnEdge 0.45.0, `--mtp` | 187.8 | 213.1 | 208.0 | 4364 | 1.1 s |
| | **BigMoeOnEdge** with #646, `--mtp` | **196.8** | **223.6** | **208.8** | 5058 | 0.9 s |
| | Strata, default | 119.4 | 120.6 | 103.9 | 1343 | 3.4 s |
| | Strata, lossless | 120.2 | 120.7 | 103.5 | 1375 | 3.4 s |
| Qwen3.8-Flash-Next Q2_0 | BigMoeOnEdge 0.45.0 | 68.5 | 62.0 | 59.0 | 1582 | 3.0 s |
| | BigMoeOnEdge with #646 | 70.1 | 66.9 | 63.9 | 1701 | 2.7 s |
| | Strata, default | did not start: its DFlash2 drafter needs Strata's own pack of the model | | | | |
| | **Strata**, lossless | **102.3** | **93.3** | **105.2** | **2700** | **1.7 s** |

## What the run found, and what #646 changes

1. **A model that fills a card with memory of its own did not load (box C, UD-Q4_K_M).** Where the
   whole model fits, the capacity fitter leaves llama.cpp's "every layer" (`n_gpu_layers = -1`),
   and the width rules read it as no layer on a device. The device bound on a pass did not apply,
   the graph went as wide as the 16384 context, and its compute buffers asked 3840 MiB on a card
   holding a 20583 MiB model. Both loads failed the same way (the first load's record does not
   change the width), and the server stayed up with no model and no message of its own. With #646
   the graph is held to the device bound (8192 tokens) and the model loads in 22 s.
2. **A width at which the whole model fits was missed one halving away (box C, UD-Q4_K_XL).** At
   8192 tokens one layer's experts stayed behind the device cache (98% of a token's routed bytes
   served), and 8192 was also the device's share of its memory, the one width the fitter was asked
   at. #646 asks at halves of the probed width: at 4096 the whole model is on the card.
3. **The copy of the experts for the device cache's misses was refused on every box (Flash-Next).**
   It was charged beside the experts' pages in the file's mapping, which it replaces: 34 + 32 GB
   against 35-58 GB the process may hold. With #646 it is charged once and the mapping's pages are
   handed back after the copy ("32400 MiB copied and pinned ... 32399 MiB of the mapping handed
   back"). Misses then upload at 10.9 GiB/s instead of 5.6 (box B) and 21.6 instead of 11.6 (box
   C): decode +7-27%, prefill +8-15%. Most of the gap to Strata on this model is elsewhere.

Not changed by #646, still open:
- Flash-Next on box A: a 2.0 GB device cache with 1.8 GB of the card's memory left free, and a
  1024-token graph (193 tok/s prefill against Strata's 1023).
- Flash-Next against Strata on every box (Strata 1.5-3.5x), above all where the card is small.

## Plan records

Every model on the three boxes also has a `bmoe-planlab` record of its first load (Qwen3.6
UD-Q4_K_M and Flash-Next Q2_0), kept with the maintainer's bench output.
