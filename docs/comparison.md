# How BigMoeOnEdge compares

The engines below all run mixture-of-experts models larger than a GPU's memory on hardware people
own. They make different trade-offs: some write their own kernels for a few models on a few GPUs and
are faster there; this one builds on llama.cpp's public API and plans itself for whatever machine it
finds. This page puts both sides next to each other: what each engine runs on, and the speeds each
reports.

Last checked on 2026-10-29 against each project's own pages and the sources linked below. Corrections
are welcome as a pull request or an issue.

## What each engine runs on

| Engine | Runs on | Accelerators | Models | Experts beyond RAM | Configures itself |
|---|---|---|---|---|---|
| **BigMoeOnEdge** | Android, iOS, macOS, Windows, Linux | CPU, Metal, CUDA, HIP, Vulkan, OpenCL, Hexagon NPU | any MoE GGUF llama.cpp loads | streamed from flash, behind a RAM cache and a GPU cache | `--auto`: measures the machine and the model's own layers and routing, sets every knob and gives the reason for each ([hardware-planning.md](hardware-planning.md)) |
| [llama.cpp](https://github.com/ggml-org/llama.cpp) | Android, iOS, macOS, Windows, Linux | CPU, Metal, CUDA, HIP, Vulkan, SYCL, OpenCL | any GGUF | left to the OS page cache (memory-mapped file) | `--fit` places layers by device memory |
| [Strata](https://github.com/Niko1221/Strata) | Windows, Linux, macOS (desktop app and terminal) | NVIDIA RTX 20-50 and DGX Spark, AMD Radeon RX 7000/9000 and Strix Halo, Intel Arc (Linux), Apple silicon | Qwen3.8-Flash-Next, Qwen3.6-35B-A3B, DeepSeek V4.1 Flash | read from SSD for DeepSeek V4.1 Flash | model choice by memory; `--plan` |
| [ds4](https://github.com/antirez/ds4) | macOS, Linux | Metal, CUDA, ROCm (Strix Halo / RDNA), CPU (debugging) | DeepSeek V4 and V4.1 Flash, V4 PRO, GLM 5.2/5.3, Qwen3.8-Flash-Next, from its own GGUFs | streamed from SSD (`--ssd-streaming`) | automatic expert-cache budget |
| [colibri](https://github.com/JustVugg/colibri) | Linux, macOS, Windows | CPU, Vulkan, CUDA, Metal | its own container, one engine per family (GLM, DeepSeek, Kimi, MiMo, Qwen, ...) | streamed from disk | `coli plan` shows the placement, `coli tune` measures and saves settings |
| [KTransformers](https://github.com/kvcache-ai/ktransformers) | Linux, Windows | CPU (AVX2, AVX-512, AMX), NVIDIA, AMD ROCm, Intel Arc, Ascend | listed families (DeepSeek, Kimi, GLM, Qwen3, MiniMax, Mixtral, ...) | held in RAM (hot experts on the GPU, the rest on the CPU), not streamed | not documented |

## Speeds each reports

Single stream, decode and prefill in tokens per second. The figures come from different quantizations,
contexts, prompts and settings (speculative decoding on or off), and most are self-reported, so they
show orders of magnitude rather than a ranking. BigMoeOnEdge's are the README's, measured as
[benchmarks.md](benchmarks.md) describes.

### Qwen3.8-Flash-Next 125B (larger than the GPU, smaller than GPU + RAM)

| Engine | Hardware | Quant | Decode | Prefill | Source |
|---|---|---|---:|---:|---|
| Basalt (a Strata fork for Blackwell) | RTX 5090 32 GB | Q2_0 | 377-400 | 8,573 (42k-token prompt) | [review, 2026-10-27](https://akitaonrails.com/en/2026/10/27/llm-benchmark-best-open-source-llms-for-home-hardware/) |
| Strata | RTX 5090 32 GB | Q2_0 | 223-259 | 6,158 (42k-token prompt) | same review |
| Strata | RTX 4090 24 GB | IQ1_M | 89-106 | ~3,500 (4k-token prompt) | [community report](https://github.com/Niko1221/Strata/issues/2263) |
| Strata | Strix Halo (AMD, ROCm) | Q2_0 | 55-65 | 371 | [review, 2026-10-27](https://akitaonrails.com/en/2026/10/27/llm-benchmark-best-open-source-llms-for-home-hardware/) |
| BigMoeOnEdge | 24 GB desktop GPU, 96 GB RAM | Q2_0 | 70 | | [README](../README.md#performance) |
| BigMoeOnEdge | 16 GB desktop GPU, 32 GB RAM | Q2_0 | 19-21 | | [README](../README.md#performance); Strata 50-66 and llama.cpp 17-19 on the same box ([bench-data](bench-data/2026-10-27-rented-16gb/README.md)) |
| BigMoeOnEdge | 16 GB Apple-silicon laptop | | 7-9 | | [README](../README.md#performance) |
| BigMoeOnEdge | 12 GB Android phone | | 2-4 | | [README](../README.md#performance) |

### DeepSeek V4 and V4.1 Flash

| Engine | Hardware | Model | Decode | Prefill | Source |
|---|---|---|---:|---:|---|
| KTransformers + SGLang | 2x RTX 5070 Ti 16 GB, 512 GB RAM (every expert in RAM) | V4.1 Flash, MXFP4 experts | 35-46 | 1,750-1,870 (35k-131k tokens) | [setup repository](https://github.com/05yuki/deepseek-v4.1-flash-1017-ktransformers) |
| KTransformers + SGLang | 1x RTX 5070 Ti 16 GB, 512 GB RAM | V4.1 Flash, MXFP4 experts | 28-32 | 1,050-1,280 | same |
| ds4 | Apple M5 Max 128 GB | V4 Flash, Q2 | 26 | | [overview](https://insiderllm.com/guides/ssd-streaming-moe-engines/) |
| ds4 | DGX Spark 128 GB | V4 Flash, Q2 | 14-18 | ~825 | [ds4 performance notes](https://github.com/antirez/ds4/blob/main/docs/PERFORMANCE.md) |
| Strata | RTX 5090 32 GB | V4.1 Flash, 2-bit | ~17 | | [review, 2026-10-27](https://akitaonrails.com/en/2026/10/27/llm-benchmark-best-open-source-llms-for-home-hardware/) |
| llama.cpp fork with expert streaming | RTX 5090 32 GB, 126 GB RAM | V4.1 Flash, MXFP4 experts | 5.1 new content, 21 cached | | [discussion #28766](https://github.com/ggml-org/llama.cpp/discussions/28766) |
| colibri | RTX 5080 16 GB, 32 GB RAM, two NVMe | V4 Flash | 1.5-1.6 | | [colibri](https://github.com/JustVugg/colibri) |
| BigMoeOnEdge | 12 GB Android phone | V4 Flash | 1 | | [README](../README.md#performance) |

### Larger still (GLM-5.2 744B and up)

| Engine | Hardware | Model | Decode | Source |
|---|---|---|---:|---|
| pulsar | two 16 GB GPUs | GLM-5.2 / Hy3 295B | ~2 / ~7 | [project listing](https://trendshift.io/repositories/82195) |
| llama.cpp PR with expert streaming | | GLM-5.2 | 1.8-2.2 | [PR #25294](https://github.com/ggml-org/llama.cpp/pull/25294) |
| colibri | six RTX 5090 (every expert in VRAM) / a 25 GB laptop | GLM-5.2 | 6-9 / 0.05-0.1 | [colibri](https://github.com/JustVugg/colibri) |

## Reading the tables

- Where the model fits between the GPU and RAM, the engines written for a few GPUs lead: Strata and
  Basalt decode two to four times faster than BigMoeOnEdge on a 4090 or 5090, and prefill long prompts
  several times faster. They compute the experts the GPU does not hold on the CPU, in parallel with
  the GPU; this engine uploads them to the GPU's cache instead. Closing that gap is on the
  [roadmap](roadmap.md).
- Where the model has to stream from flash, every engine is in the single digits and the drive sets
  the pace. KTransformers' tens of tokens per second on DeepSeek V4.1 come from holding every expert
  in 512 GB of RAM.
- Of these engines, BigMoeOnEdge and llama.cpp are the ones that run on phones, and only
  BigMoeOnEdge streams a model larger than the phone's memory with an expert cache there. It is also
  the one that sets every setting from measurements of the machine and of the model's own layers and
  routing, and says why for each.
