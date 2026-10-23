<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/logo/horizontal-ink.svg">
    <img src="docs/assets/logo/horizontal.svg" width="320" alt="BigMoeOnEdge">
  </picture>
</p>

<p align="center">
  <a href="https://github.com/Helldez/BigMoeOnEdge/releases/latest"><img src="https://img.shields.io/github/v/release/Helldez/BigMoeOnEdge" alt="Latest release"></a>
  <a href="https://github.com/Helldez/BigMoeOnEdge/actions/workflows/ci.yml"><img src="https://github.com/Helldez/BigMoeOnEdge/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/Helldez/BigMoeOnEdge" alt="License"></a>
</p>

<p align="center"><a href="README.md">English</a> · <a href="README_zh.md">中文</a> · <a href="README_ja.md">日本語</a> · <a href="README_es.md">Español</a> · <a href="README_fr.md">Français</a> · <a href="README_it.md">Italiano</a></p>

Run Mixture-of-Experts models larger than the device's memory by streaming the experts from flash.
Built on llama.cpp's public API; Android, iOS, macOS, Windows and Linux; a local OpenAI-compatible
server on each.

<table>
<tr>
<td align="center" width="50%"><img src="docs/assets/screens/android.gif" width="220" alt="Qwen3.6-35B-A3B generating an answer on a 12 GB Android phone, in real time."></td>
<td align="center" width="50%"><img src="docs/assets/screens/iphone.gif" width="220" alt="Qwen3.6-35B-A3B generating an answer on a 12 GB iPhone, in real time."></td>
</tr>
<tr>
<td align="center">12 GB Android phone</td>
<td align="center">12 GB iPhone</td>
</tr>
<tr>
<td align="center" colspan="2"><img src="docs/assets/screens/desktop.gif" width="620" alt="Qwen3.6-35B-A3B generating an answer in the desktop app on a 16 GB Apple-silicon laptop, in real time, at 19.9 tok/s."></td>
</tr>
<tr>
<td align="center" colspan="2">16 GB Apple-silicon laptop</td>
</tr>
</table>

Qwen3.6-35B-A3B generating on each device, in real time, in the shipped apps. The model is larger
than each device's memory.

## Description

A Mixture-of-Experts model uses a few of its experts for each token. BigMoeOnEdge keeps in memory
the weights every token needs and a cache of recently used experts, and reads the other experts from
flash when the router asks for them. The output is identical to running the model fully in memory;
the tests check it.

- **Planner.** `--auto` measures the machine (available memory, storage speed, each CPU, GPU and
  NPU) and the model's header, then sets the cache size, what stays resident, and which device runs
  the prompt and which the answer. Its rules name no platform or device. Settings given by hand are
  kept.
- **Models.** Any MoE gguf that llama.cpp loads, in any quantization. Expert tensors are found from
  the file, so a new architecture needs no code here.
- **Backends.** CPU, Metal, Vulkan, OpenCL and Hexagon NPU, loaded as modules; CUDA, HIP and SYCL
  modules built with their own toolchains load the same way.
- **Apps and server.** Native apps on Android and iOS, a web app on the desktop, and an
  OpenAI-compatible server on all of them.

## Performance

Decode speed, measured on the devices below. The engine sizes itself to the machine, so stronger
hardware runs faster.

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/diagrams/speed-dark.svg">
    <img src="docs/assets/diagrams/speed-light.svg" width="820" alt="Qwen3.6-35B-A3B Q4_K_M decode speed: 18-24 tok/s on a 16 GB Apple-silicon laptop, 4.8 on a 16 GB Windows laptop, 4.8-5.0 on a 12 GB Android phone, 2.6-3.0 on a 12 GB iPhone.">
  </picture>
</p>

| Model | On disk | Device | Model / RAM | Decode |
|---|---:|---|---:|---:|
| DeepSeek V4 Flash 284B | 91 GB | 12 GB Android phone | 7.6× | 0.94 tok/s |
| Qwen3.8-Flash-Next 125B | 80 GB | 12 GB Android phone | 6.7× | 2.2-3.5 tok/s |
| gpt-oss-120b | 60 GB | 12 GB Android phone | 5× | 1.3 tok/s |
| Qwen3.6-35B-A3B | 21 GB | 12 GB Android phone | 1.9× | 4.8-5.0 tok/s |
| Qwen3.6-35B-A3B | 21 GB | 12 GB iPhone | 1.9× | 2.6-3.0 tok/s |
| Qwen3-30B-A3B | 18.5 GB | 12 GB Android phone | 1.5× | 5.2 tok/s |
| Gemma-4-26B-A4B | 17 GB | 12 GB Android phone | 1.4× | 4.1 tok/s |
| Qwen3.8-Flash-Next 125B | 76 GB | 16 GB Apple-silicon laptop | 4.8× | 6.5-9 tok/s |
| Qwen3.6-35B-A3B | 21 GB | 16 GB Apple-silicon laptop | 1.3× | 18-24 tok/s |
| Qwen3-Coder-30B-A3B | 18.6 GB | 16 GB Apple-silicon laptop | 1.2× | 23 tok/s; 7-9 inside opencode |
| Qwen3.6-35B-A3B | 21 GB | 16 GB Windows laptop | 1.3× | 4.8 tok/s |

Configurations and method: [docs/benchmarks.md](docs/benchmarks.md). To add a machine, run
`scripts/bench-report.sh /path/to/model.gguf` and paste the table into a
[benchmark report](https://github.com/Helldez/BigMoeOnEdge/issues/new?template=benchmark-report.yml).

## Quick start

**Android.** Install the APK from the [latest release](https://github.com/Helldez/BigMoeOnEdge/releases/latest)
and pick a model under **Models**. **Settings → Serve on the network** shares it, with a key.

**iOS.** Build the app with Xcode from [examples/ios](examples/ios/README.md). Experimental.

**Desktop (macOS, Windows, Linux).** Download the `bmoe-desktop` archive from the same release and run
`bmoe-server --open`; the app opens at `http://127.0.0.1:8765/`.

**Command line.**

```bash
bmoe-cli -m model.gguf --auto -p "Explain MoE routing."   # measure, plan, run
bmoe-cli -m model.gguf --plan                              # print the plan and exit
```

## OpenAI-compatible server

```python
from openai import OpenAI

client = OpenAI(base_url="http://127.0.0.1:8765/v1", api_key="local")
reply = client.chat.completions.create(
    model="local",
    messages=[{"role": "user", "content": "Write a haiku about experts on flash."}],
)
print(reply.choices[0].message.content)
```

Streaming, tool calls and reasoning are supported. For opencode, add the server as an
OpenAI-compatible provider with `baseURL` set to `http://127.0.0.1:8765/v1`. To serve other machines,
start the server with `--host` and `--api-key`. API reference: [docs/server-api.md](docs/server-api.md).

## How it works

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/diagrams/token-dark.svg">
    <img src="docs/assets/diagrams/token-light.svg" width="820" alt="One decode step: the whole model is on flash; RAM holds the dense weights and an expert cache. The router picks k experts per layer; cached ones cost nothing, the others are read from flash.">
  </picture>
</p>

The engine uses llama.cpp's public API: every quantization, tokenizer and chat template llama.cpp
supports works unchanged, and a llama.cpp upgrade is a submodule bump. The routed experts are read
from the file's tensor layout ([docs/which-models-stream.md](docs/which-models-stream.md)).
`ctest` checks that streamed output equals fully resident output across the cache, evictions,
overlap, sessions and split models ([docs/architecture.md](docs/architecture.md)).

## The planner

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/assets/diagrams/plan-dark.svg">
    <img src="docs/assets/diagrams/plan-light.svg" width="820" alt="What --auto chose for Qwen3.6-35B-A3B on a 16 GB Apple-silicon laptop: 1.7 GiB of dense weights and a 7.2 GiB expert cache in RAM, 85% cache hits, 69.6 MiB read from flash per token, the prompt on the GPU and decoding on 8 CPU threads at 17.6 tok/s.">
  </picture>
</p>

The planner reads the model's shape from its header and measures the machine. The same rules give a
phone and a laptop different plans. The Plan screen shows the result: where the memory goes, what
each device does, and the reason for each decision.
[docs/hardware-planning.md](docs/hardware-planning.md).

## Platforms

| | Android | macOS | Windows | Linux | iOS |
|---|---|---|---|---|---|
| Engine and CLI | arm64 | Apple silicon | x86_64, arm64 | x86_64, aarch64 | arm64 (in the app) |
| App and server | native app | web app | web app | web app | native app, built from source |
| Accelerators | Hexagon NPU, Vulkan, OpenCL | Metal | Vulkan, OpenCL (arm64) | Vulkan | Metal |
| Run on hardware | 12 GB phone | 16 GB laptop | 16 GB laptop | CI, community | 12 GB iPhone ([#235](https://github.com/Helldez/BigMoeOnEdge/issues/235)) |

Details and what was measured where: [docs/platforms.md](docs/platforms.md).

## Build from source

```bash
git clone --recursive https://github.com/Helldez/BigMoeOnEdge.git
cd BigMoeOnEdge
cd ui && npm ci && npm run build && cd ..
scripts/build-host.sh
build/bin/bmoe-server --open
```

Tests: `cd build && ctest --output-on-failure` (needs `python3` with the `gguf` package). Android:
`scripts/build-android.ps1`, then the APK in [`examples/android`](examples/android).
[CONTRIBUTING.md](CONTRIBUTING.md).

## Documentation

[docs/](docs/README.md), indexed by task. Start with [hardware-planning.md](docs/hardware-planning.md),
[settings.md](docs/settings.md), [server-api.md](docs/server-api.md),
[platforms.md](docs/platforms.md) and [limitations.md](docs/limitations.md).

## Prior art

AirLLM, Apple's *LLM in a flash*, FlexGen, PowerInfer and EdgeMoE. The closest recent work is
[flash-moe](https://github.com/danveloper/flash-moe), a Metal engine that streams a 397B MoE from SSD
on Apple silicon; BigMoeOnEdge uses llama.cpp's public API across architectures and platforms.

## License

Apache-2.0. See [LICENSE](LICENSE).
