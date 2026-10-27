# 2026-10-27 — a rented 16 GB card with 30 GiB of RAM, against llama.cpp and Strata

One rented desktop box, the class a home PC with a mid-range card is: the model's experts do not fit
in RAM, so they stream from the drive, and the card's memory holds the dense part and a cache of the
experts used most. The engine ran `--auto` (plus `--routing-dir`, which keeps the routing record
between loads as `bmoe-server` does, `--plan-explain` and `--csv`), greedy, from the README's prompt.
The same file ran through llama.cpp's own offload and through Strata on the same box. The CSVs are one
row per token plus the `# summary` trailer ([telemetry.md](../../telemetry.md)); the `.plan` files are
what `--auto` printed.

What the session found is in the code: #429 (the device cache sized from the card's free memory),
#430 (`--auto --overlap` and `bmoe-server` both failed to start where the device cache was armed) and
#431 (the RAM cache stopped short of the RAM the plan left idle, and llama.cpp's load-time read-ahead
let the kernel swap the cache out).

## Machine

| | |
|---|---|
| GPU | RTX 5060 Ti class, 16 GB, on a PCIe 3.0 x8 slot |
| CPU | 6-core desktop part, 12 threads |
| memory | a 30 GiB control group on a 32 GB host, 8 GB of swap |
| drive | NVMe, 2.2 GB/s at the model's 450 KiB expert slice over 4 lanes, direct reads |
| CUDA | 12.8 toolkit, driver 595 |
| model | Qwen3.8-Flash-Next 125B GSQ-RCO Q2_0: 66 GB in two shards, 32.4 GB of experts, the 28.8 GB second shard the row-gathered embedding |

## What `--auto` chose

Every layer's dense part on the card (4.4 GB), a **10 GB device cache** of the experts used most
(#429 sized it from the card's free memory), every expert served from RAM or the drive, a RAM cache
of 15.7-20.7 GB, the 28.8 GB embedding left mapped (#426), 12 threads after the first answer measured
them.

## Decode, 256 tokens from a cold page cache

| Engine | Decode tok/s | Prefill |
|---|---:|---:|
| bmoe `--auto`, first load, then from the routing record (`main-auto-256-*`) | 13.4, 14.4 | 5 tok/s (38 tokens) |
| bmoe `--auto --mtp --mtp-head` (the head on the card, 6.2 GB device cache) | 13.7, 13.3 | 5 |
| llama.cpp `llama-completion`, its own capacity fit, file memory-mapped | 18.9, 16.6 | 9.2 (26 tokens) |
| llama.cpp `-ngl 99 --n-cpu-moe 99` (every expert on the cores) | 12.3 | 7.2 |
| Strata, its own setup (`--spec 4`, an expert profile filling 9.25 GB of the card at start), first request | 36 | 13 |
| Strata, the requests after it | 52-67 | 245-390 (prefix reused) |

Strata ran through its server with the same client as `bmoe-server`, three prompts twice
(`strata-256.json`); the engine CLI's figures are its own summary line.

## Decode, 1024 tokens: the steady state

| Configuration | Mean tok/s | Last 256 tokens | Hits in decode |
|---|---:|---:|---:|
| the plan's 15.7 GB RAM cache (`cache-plan-1024`) | 18.5 | 21.3 | 96% |
| the same, when the kernel swapped part of the cache out and the cache brake halved it twice (`cache-plan-1024-brake`) | 11.6 | 9.6 | 83% |
| a 22 GB RAM cache set by hand (`cache-22g-1024`) | 20.7 | 27.5 | 97% |
| **#431: plain `--auto`, three loads (`fixed-auto-1024-*`)** | **20.8, 19.3, 20.7** | **27.2-27.3** | 96.7% |
| #431 with the MTP head (`fixed-auto-1024-mtp-head`) | 19.0 (16.1 on a load where the brake fired once) | | 96.4% |
| Strata, three prompts (`strata-1024.json`) | 50, 66, 35 | | 88-95% (its log) |

The 256-token runs read every expert they touch for the first time, so a bigger cache does not change
them (22 GB and 15.7 GB both read 35.8 MiB per token). Over 1024 tokens the answer reaches experts the
first 256 never did, and the cache size is what decides the hit rate.

## What else was measured, and lost

- **The predictors, beside the device cache**: `--prefetch 1` and `2` read nothing (the previous
  token's experts at the next layers were already cached: what misses are experts never seen before),
  14.0 and 13.8 tok/s; `--predict-prefetch` doubled the bytes read and dropped the hit rate, 9.5
  (`main-predict-prefetch`); `--route-ahead 1` committed no routing and doubled the reads, 9.4
  (`main-route-ahead`).
- **The mapping released file by file**: the experts' shard was released (35 GB unmapped), and the
  direct reads stayed at 0.9-1.0 GB/s. On this Linux box the live mapping was not what held them back;
  the reads are latency-bound, one layer's few misses at a time.
- **Upload from pinned memory**: a CUDA copy test of one token's 47 missing experts (61 MiB) took
  14.1 ms from pageable memory with a sync per expert and 10.4 ms from pinned memory: 4 ms of a 48 ms
  token.
- **#418**: two loads back to back without dropping the page cache planned against 30.1 and 27.6 GB;
  the halving #418 measured on another box's control group did not reproduce here.

## What separates this engine from Strata on this box

Strata keeps a fixed set of experts on the card, chosen by a profile, and computes the experts a token
routes outside that set on the CPU, from RAM; nothing crosses the bus while it decodes, and its
speculative decoding pays because a drafted token's extra experts are cheap. Here the device cache is
llama.cpp's: every miss is uploaded over the bus before the card computes it, 48 times a token, and the
streamer holds in RAM the same hot experts the card already has. Those two are the work in front of
this engine on this class of machine, not a ceiling of the card.
