# 2026-10-28 — a rented AMD datacenter card through ROCm, the model resident

The first run of the HIP backend: one rented AMD box, engine 0.38.0 built from source with
`GGML_HIP`, Qwen3.8-Flash-Next against llama.cpp's own `llama-completion` built from the same
submodule. The card holds the whole model, so nothing streams: this measures that the engine runs on
ROCm and what it costs over llama.cpp when the plan has nothing to stream, not the streamer. The
engine ran `--auto` (plus `--routing-dir`, `--plan-explain` and `--csv`), greedy, from the README's
prompt, 1024 tokens, the page cache dropped before every run.

## Machine

| | |
|---|---|
| GPU | MI300X class, 192 GB, gfx942 |
| CPU | a 48-core server part |
| memory | 263 GiB this process may hold |
| ROCm | 7.2, the vendor's development image |
| model | Qwen3.8-Flash-Next 125B GSQ-RCO Q2_0: 66 GB in two shards |

## Building

ROCm's clang hangs compiling ggml-cpu's AMX kernels (`amx/mmq.cpp`, the Sapphire Rapids CPU tier):
the build sat on that one file for 28 minutes. The host code builds with the system's gcc and only
the device code with ROCm's clang:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGGML_HIP=ON -DAMDGPU_TARGETS=gfx942 \
  -DCMAKE_HIP_COMPILER=/opt/rocm/llvm/bin/clang++
cmake --build build --target bmoe-cli
```

## What `--auto` chose

Every layer on the card, experts included; nothing on the host to stream or cache. The plan's
`placement` line still reads "0 layers on devices" here, though every layer ran on the card: a
wording bug in that line, open.

## Decode, 1024 tokens

| Engine | Decode tok/s | Prefill (short prompt) | Load |
|---|---:|---:|---:|
| bmoe `--auto`, first load, then from the routing record | 53.4, 53.3 | 47 tok/s (38 tokens) | 10.4 s |
| llama.cpp `llama-completion -ngl 99` | 55.8 | 41 tok/s | |
| llama.cpp `llama-completion`, its own capacity fit | 55.6 | 41 tok/s | |
| bmoe `--auto --mtp --mtp-head` (the head converted with `scripts/convert-mtp-head.py`) | 56.9 counting the drafting; 88.5 for the verify decodes alone | | |

Resident, the engine is llama.cpp plus its own bookkeeping: 4 % behind. With the head, 59 % of the
drafts were accepted (2.78 tokens per verify decode), but drafting cost 6.3 ms per token on top of
11.3 ms of verify decodes, against 18.7 ms per token without the head: 7 % faster in all.

## Prefill, one session

A session of four prompts after one page-cache drop (the `--session` protocol of `bmoe-cli`):

| Prompt | Tokens | Prefill tok/s |
|---|---:|---:|
| short | 19 | 24 |
| long, first | 1592 | 2122 |
| long, second | 1587 | 2247 |
| long, third | 1547 | 2270 |

A short prompt's figure is the first graph's set-up, not the card's speed.
