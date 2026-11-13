# BigMoeOnEdge 0.42.0 and Strata on one 12 GB card (2026-11-13)

One rented machine for both engines:
- a 12 GB card (RTX 3060 class, CUDA 12.8);
- a 15.4-core quota of a server CPU (Zen 2, DDR4);
- 47 GB of RAM;
- a local NVMe drive.

Both engines used the same GGUF, Qwen3.6-35B-A3B **UD-Q4_K_XL** with its MTP head (Unsloth's
`Qwen3.6-35B-A3B-MTP-GGUF`, the size Strata lists first for this model), the same OpenAI-compatible
client (`/v1/chat/completions`, streamed, greedy) and the same two short prompts ("a", 50 tokens, and
"code", 30), each answered with 256 tokens. The page cache was dropped before every load. Decode is
timed from the first streamed token to the last; the first token is the time to it.

- **BigMoeOnEdge**: the 0.42.0 release (CLI and CUDA pack), `bmoe-server --auto --ctx-size 16384 -m <model>`,
  plus `--mtp` in the last series. Two loads per series on one data dir: the second plans from the
  first's record. `--auto` placed every layer on the card with a device cache of the experts used most.
- **Strata**: 0.2.13.1 (engine 0.1.59.1) through its own launcher,
  `strata launch --family qwen36 --model UD-Q4_K_XL --model-path <dir> --context 16384`: its default
  (MTP drafting on), and `--mtp off --drafting off` for the lossless row.

Decode in tokens per second, in the order run:

| Engine | "a" | "code" | first token ("a" / "code") |
|---|---:|---:|---:|
| BigMoeOnEdge, load 1 | 43.8 | 40.9 | 0.85 / 0.57 s |
| BigMoeOnEdge, load 2 | 43.5 | 41.5 | 0.92 / 0.58 s |
| Strata, default | 36.1 | 33.5 | 0.52 / 0.35 s |
| Strata, lossless | 34.7 | 33.9 | 0.55 / 0.33 s |
| BigMoeOnEdge, load 3 | 39.5 | 36.5 | 0.92 / 0.65 s |
| BigMoeOnEdge, load 4 | 44.4 | 43.6 | 0.82 / 0.58 s |
| Strata, default, again | 32.0 | 28.2 | 0.56 / 0.39 s |
| **BigMoeOnEdge `--mtp`, load 1** | **61.6** | **65.3** | 1.09 / 0.64 s |
| **BigMoeOnEdge `--mtp`, load 2** | **56.9** | **59.3** | 1.24 / 0.71 s |

Strata's own figure for this card and this file is 58.5-59.7 tok/s ([its QWEN36.md](https://github.com/Niko1221/Strata/blob/main/docs/QWEN36.md),
"hybrid mode", 37 of 256 experts per layer in VRAM and the rest computed on the CPU, a 20-thread
machine). In that mode most experts run on the CPU, so a 15-core quota of a Zen 2 server part with
DDR4 holds it back, where this engine's device cache leans on the card. The table is both engines on
one box; it is not the machine Strata's figure was measured on.
