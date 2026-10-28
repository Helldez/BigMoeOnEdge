# 2026-10-28 — DeepSeek V4.1 Flash with its native MXFP4 experts on a rented 96 GB card

One rented box with a 96 GB workstation card and a 283 GB memory limit, on which DeepSeek V4.1 Flash
fits neither the card nor the RAM: about 270 GiB of experts and 7.5 GiB of dense weights against
96 GB of device memory and 263 GiB the process may hold. The engine ran `--auto` (plus
`--routing-dir`, `--plan-explain` and `--csv`), greedy, from the README's prompt, 1024 tokens, the
page cache dropped before every run. The same file ran through llama.cpp's own `llama-completion`
from the same submodule. The CSVs are one row per token plus the `# summary` trailer
([telemetry.md](../../telemetry.md)); the `.plan` files are the plan and the cache counters each run
printed.

## Machine

| | |
|---|---|
| GPU | RTX PRO 6000 Blackwell class, 96 GB, sm_120 |
| CPU | a 64-core server part, of which the container's quota gives 27 |
| memory | a 283 GB control group on a much larger shared host |
| drive | the provider's network file system (the plan measured 4.6-5.3 GiB/s at the model's 6 MiB expert slice over 4 lanes) |
| CUDA | 12.8 toolkit, driver 595 |
| model | unsloth's DeepSeek-V4.1-Flash-1017 UD-Q4_K_XL: 466 GiB in nine shards, routed experts in their native MXFP4, the 189 GiB Engram table in MXFP8, the rest Q8_0 |

The Engram table's MXFP8 is what the second commit of the llama.cpp pin reads
([seam.md](../../seam.md)): upstream dropped the type before its DeepSeek V4.1 support merged.

## What `--auto` chose

Every layer's dense part on the card (11 GB with the context), an **84.5 GB device cache** of the
experts used most (4713 slots, 31% of the experts), a **207-215 GB RAM cache** behind it (75% of the
experts), the 189 GiB Engram table left mapped (the graph gathers a few rows of it per token), 28
threads.

## Decode, 1024 tokens from a cold page cache

| Run | Mean tok/s | Per 128 tokens | Load |
|---|---:|---|---:|
| `--auto`, first load (`auto1`) | 6.2 | 2.8 4.8 7.5 7.3 8.4 8.0 9.5 10.6 | 492 s |
| `--auto`, from the routing record (`auto2`) | 6.6 | 2.7 4.9 8.1 8.6 10.2 8.4 10.5 12.6 | 345 s |
| llama.cpp `llama-completion`, its own fit | 2.8 | | 370 s |

The runs had not settled at 1024 tokens: over the last 256 a token took 87-100 ms, of which 29-32 ms
waiting on 36 MB of reads from the network drive and 55-71 ms of compute, with 95% of the expert reads
served by the RAM cache and 91% of the experts found in the device cache. The 9% that missed it were
uploaded: 376 MB per token over the bus.

## Prefill

The 26-token prompt of the decode runs prefilled at 1.6-1.7 tok/s, from a cold page cache: the
reads, not the card. A session of long prompts (`--session`) was killed by the control group during
its second prompt, after the first, short one, with 259.6 GB resident at session start against the
283 GB limit: the plan's 54 GB margin did not cover what a long prompt's prefill adds on this model.
That is a planner finding, open, not a figure. An `nsys` window of a decode run was killed by the
control group too.
