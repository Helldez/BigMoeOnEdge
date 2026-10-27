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

<table align="center">
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
| Qwen3.8-Flash-Next 125B | 66 GB | 16 GB desktop GPU, 32 GB RAM | 2.1× | 19-21 tok/s; 27 from the 768th token |
| Qwen3.8-Flash-Next 125B | 66 GB | 24 GB desktop GPU, 89 GiB RAM | 0.7× | 70 tok/s |
| Qwen3.6-35B-A3B | 21 GB | 24 GB desktop GPU | resident | 217 tok/s |

Configurations and method: [docs/benchmarks.md](docs/benchmarks.md). To add a machine, run
`scripts/bench-report.sh /path/to/model.gguf` and paste the table into a
[benchmark report](https://github.com/Helldez/BigMoeOnEdge/issues/new?template=benchmark-report.yml).

## Quick start

Everything below comes from the [latest release](https://github.com/Helldez/BigMoeOnEdge/releases/latest).
Each installer puts the app on the machine: a window of its own over the engine's server, which a
browser can also open at `http://127.0.0.1:8765/`; pick a model under **Models**. A graphics card
that needs a vendor's runtime (NVIDIA's CUDA, AMD's ROCm) gets its backend from the app's Settings,
downloaded and checked against the release's signed feed ([docs/desktop-app.md](docs/desktop-app.md)).

- **macOS** (Apple silicon): open `BigMoeOnEdge-<version>-macos-arm64.dmg` and drag the app to
  Applications. Closing the window keeps it in the menu bar; **Quit** stops it. Not notarised yet:
  the first start needs the step in the dmg's README.txt.
- **Windows**: run `BigMoeOnEdge-<version>-windows-x86_64-setup.exe` (or `-windows-arm64-setup.exe`).
  No administrator rights; start BigMoeOnEdge from the Start menu. Closing the window keeps it in the
  notification area; **Quit** there stops it.
- **Linux**: `chmod +x BigMoeOnEdge-<version>-linux-x86_64.AppImage` (or `-linux-aarch64`) and run it.
  The window needs WebKitGTK 4.1, which common desktops have; without it the UI opens in the
  browser. `--server` runs the server alone.
- **Android**: add the app to [Obtainium][obtainium], which then installs every release as an
  update, or install `app-dev-release.apk` by hand. **Settings → Serve on the network** shares the
  phone's engine, with a key.

**iOS.** Build the app with Xcode from [examples/ios](examples/ios/README.md). Experimental.

**Terminal, servers and coding agents.** One line installs the command-line tools (`bmoe-cli`, and
`bmoe-server` with its web UI) into your user folder and puts them on `PATH`, no administrator
rights; run it again to update:

```bash
curl -fsSL https://github.com/Helldez/BigMoeOnEdge/releases/latest/download/install.sh | sh
```

```powershell
irm https://github.com/Helldez/BigMoeOnEdge/releases/latest/download/install.ps1 | iex
```

The `bmoe-cli` archives on the release page hold the same tools, for an install by hand.

[obtainium]: https://apps.obtainium.imranr.dev/redirect?r=obtainium://app/%7B%22id%22%3A%22io.bigmoeonedge.example.dev%22%2C%22url%22%3A%22https%3A%2F%2Fgithub.com%2FHelldez%2FBigMoeOnEdge%22%2C%22author%22%3A%22Helldez%22%2C%22name%22%3A%22BigMoeOnEdge%22%2C%22additionalSettings%22%3A%22%7B%5C%22apkFilterRegEx%5C%22%3A%5C%22-release%5C%5C%5C%5C.apk%24%5C%22%7D%22%7D

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

Coding agents send a large system prompt and tool list on every request: load the model with a
context of at least 32768 tokens (`-c 32768`).

**Claude Code** speaks the Anthropic Messages API, which the server also serves (`/v1/messages`).
Use the model id `GET /v1/models` lists, and tell Claude Code the context you loaded so it compacts
in time:

```bash
export ANTHROPIC_BASE_URL=http://127.0.0.1:8765
export ANTHROPIC_API_KEY=local            # or the server's --api-key
export ANTHROPIC_MODEL=<model id from /v1/models>
export ANTHROPIC_DEFAULT_HAIKU_MODEL=$ANTHROPIC_MODEL
export CLAUDE_CODE_MAX_CONTEXT_TOKENS=32768
claude
```

**Codex CLI** speaks the OpenAI Responses API (`/v1/responses`). Add a provider to
`~/.codex/config.toml`:

```toml
model = "<model id from /v1/models>"
model_provider = "bmoe"

[model_providers.bmoe]
name = "bmoe"
base_url = "http://127.0.0.1:8765/v1"
wire_api = "responses"
env_key = "BMOE_API_KEY"   # export BMOE_API_KEY=local, or the server's --api-key
```

### The agent

The server also runs an agent: a conversation that works in a folder you add, reads and edits its
files, runs commands, searches and reads the web, follows skills, hands parts of a job to helpers
and runs on a schedule. Changes wait for your approval unless you choose otherwise, and the
conversation lives in the server, so a run goes on with the page closed. It works with the loaded
model or any OpenAI-compatible service you save. The server also writes the settings that connect
Codex CLI or OpenCode to it, and shows the ones for Claude Code and other clients. See
[docs/agent.md](docs/agent.md).

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

Streaming only reads. The model file is mapped read-only and the expert cache is a region of RAM,
never a file, so a streamed token costs flash reads and no flash writes: it does not wear the
drive the way writes do. What the engine writes is explicit and small: a model you download, your
settings, and a metrics CSV or routing record when asked for.

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
| Accelerators | Hexagon NPU, Vulkan, OpenCL | Metal | Vulkan, CUDA (x86_64, separate download), OpenCL (arm64), Hexagon NPU (arm64, opt-in) | Vulkan, CUDA and HIP (x86_64; CUDA a separate download, HIP where ROCm 7 is installed) | Metal |
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

Tests: `cd build && ctest --output-on-failure` (needs `python3` with the `gguf` package). Install
the desktop layout (the binaries, their backend libraries, the web UI and the catalog, in one
folder): `cmake --install build --component bmoe --prefix <dir>`, then `<dir>/bin/bmoe-server`.
Android: `scripts/build-android.ps1`, then the APK in [`examples/android`](examples/android).
Warning and sanitizer options: [CONTRIBUTING.md](CONTRIBUTING.md).

## Documentation

[docs/](docs/README.md), indexed by task. Start with [hardware-planning.md](docs/hardware-planning.md),
[settings.md](docs/settings.md), [server-api.md](docs/server-api.md),
[platforms.md](docs/platforms.md) and [limitations.md](docs/limitations.md).

## Prior art

AirLLM, Apple's *LLM in a flash*, FlexGen, PowerInfer and EdgeMoE. The closest recent work is
[flash-moe](https://github.com/danveloper/flash-moe), a Metal engine that streams a 397B MoE from SSD
on Apple silicon; BigMoeOnEdge uses llama.cpp's public API across architectures and platforms.

## License

Apache-2.0. See [LICENSE](LICENSE). The binaries and the apps also carry llama.cpp and ggml,
cpp-httplib, nlohmann/json, the web UI's JavaScript libraries (MIT, Apache-2.0) and two fonts
(SIL OFL 1.1): their licenses are in
[THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES), which ships in every archive and APK.
