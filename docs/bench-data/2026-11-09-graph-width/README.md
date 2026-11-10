# 2026-11-09: rented cards, graph width against device memory (#523), and #505 beside it

The width rule (`choose_graph_width`, [hardware-planning.md](../../hardware-planning.md#the-widest-graph))
checked compute buffers against host memory only. These runs measured what that cost on cards with
memory of their own, and whether the fix holds without regressions on three classes of card. One of
them also checks whether #523 and #505 (the RAM expert cache's eviction and prompt admission) get in
each other's way.

Every run is `bmoe-server` at its default 16384 context, planning by itself (`--auto` is what the
server does). The same OpenAI client sends the same requests: three short prompts at 1024 greedy
tokens (`a`, `b`, `code`), and one 9756-token prompt (an excerpt of `docs/hardware-planning.md`) at
256 tokens for prefill. A row is decode tok/s as the client timed the stream; `prefill` is the long
prompt's tokens over its time to first token. Each directory has the plans the server reported
(`*-plan.json`, from `GET /api/plan`) and the client's per-request records (`*.json`).

Model: Qwen3.8-Flash-Next 125B GSQ-RCO Q2_0 (66 GB, 32 GB of experts), plus Qwen3.6-35B-A3B
UD-Q4_K_M (22 GB) on the 24 GB card. All cards are NVIDIA, CUDA 12.8, `sm_86`.

## 12 GB card: the bug (`12gb/`)

RTX 3060 class, 6-core desktop CPU, 120 GiB control group.

| build | ubatch | device buffers | layers on the card | device cache | decode a / b / code |
|---|---:|---:|---:|---:|---|
| #519 at fe2d6f5 (main's width rule, a newer llama.cpp) | 16384 | 6644 MiB | 47/49 | none | 12.9-13.0 / 12.9-13.1 / 12.9-13.1 |
| #523 at 76c0f6a | 2048 | 634 MiB | 49/49 | 6114 MiB | 35.4-35.9 / 35.7-36.1 / 32.9-33.3 |

On main, the graph as wide as the context reserved more than half the card, the fitter left two
layers on the host, and the device tier (which needs every dense part on the card) armed nothing.
The final rule still covers this case (the fitter left whole layers on the host); it was not re-run on
this card at the final commit.

## 24 GB card: no regression, and a win where the model fits whole (`24gb/`)

RTX 3090 class, 8-core desktop CPU (16 threads), 120 GiB control group. `flash-pr-*` and `q36-pr-*`
are #523 at 76c0f6a, which narrowed wherever experts were on the host. `*-pr2*` is the final commit,
f33f98a. Main is 9cd9d23.

| model | build | ubatch | placement | decode (short) | prefill 9.7k |
|---|---|---:|---|---|---:|
| Flash-Next Q2_0 | main | 16384 | 49 layers, 12.4 GB device cache | 61.4-72.4 | 1560-1668 |
| | 76c0f6a | 4096 | 17.6 GB device cache | 66.7-76.8 | 1189-1201 |
| | **f33f98a** | 16384 | as main | 63.7-72.6 | 1734 |
| Qwen3.6-35B Q4_K_M | main | 16384 | 41 layers, 16.8 GB device cache | 128.6-139.0 | 4789 |
| | **f33f98a** | 8192 | the whole model on the card | 156.3-156.7 | 5604 |

76c0f6a traded a long prompt's prefill (-24..28%) for a larger cache (+7..12% decode). That trade is
not the rule's to make unpriced, so the final rule narrows only where the placement improves.

## 16 GB card with little RAM: #505 and #523 together (`16gb/`)

RTX A4000 16 GB, 6-core desktop CPU, 23 GB of RAM: Flash-Next streams from flash, with a device cache
of experts on the card and the RAM cache behind it. Four builds: main (9cd9d23), #505 (627bd95), #523
(f33f98a), and the two merged (only `CHANGELOG.md` conflicts). The model file's page cache was dropped
(`posix_fadvise DONTNEED`) before every server start. Without that, each later server read less free
memory (the earlier one's pages) and planned a smaller RAM cache (#418): in a first round, the third
server planned 9.2 GB where the first planned 20.2, and decoded at 10 tok/s where it does 18.

All four planned the same placement: experts-stream, ubatch 0, 49/49 layers, a 4168 MiB device cache,
~20.8 GB of RAM cache. Two rounds:

| build | decode a / b / code | prefill 9.7k |
|---|---|---:|
| main | 24.4 / 26.8-26.9 / 18.4-18.5 | 374 |
| #505 | 24.5 / 28.9 / 18.0 | 441-444 |
| #523 | 24.4 / 26.9 / 18.5-18.6 | 374-379 |
| #505 + #523 | 24.5-24.6 / 28.8-29.1 / 17.9-18.1 | 439-445 |

#523 leaves this card as main does (every layer is on it and the model cannot fit whole). #505's
prefill gain, about 18% in this streamed regime, survives the merge unchanged. The two do not
interfere.
