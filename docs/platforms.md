# Platform support

What runs where, how far each platform has been taken, and what each one still owes. This page is
the summary; the evidence behind every cell is in the page it links to, and a cell with no
measurement behind it says so rather than implying one.

Last reviewed on 2026-11-14, against release 0.43.0.

## How to read the tables

| State | Meaning |
|---|---|
| **Measured** | Run on real hardware by this project, with the figures in the linked page. |
| **Shipped** | Built by CI and attached to a release, or present in the release build. |
| **Built** | Compiles, in CI or with a script in this repo, and has not been run here. |
| **Owed** | Designed or partly built, with the missing work named below. |
| **Not built** | No build in this repo carries it. |
| - | Does not apply to that platform. |

"Measured" is a statement about one machine of that kind, never about the platform as a whole.
The machines are described generically on purpose.

## Operating systems

| | Android (arm64) | macOS (arm64) | Linux (x86_64, aarch64) | Windows (x86_64, arm64) | iOS |
|---|---|---|---|---|---|
| **Engine and `bmoe-cli`** | Shipped, in the APK | Shipped | Shipped | Shipped | Built, compiled into the harness app |
| **`bmoe-server` and the web UI** | Shipped, in the APK as a child process of the app | Shipped: in `BigMoeOnEdge.app` (a `.dmg`), and in the CLI archive | Shipped: in the AppImage, and in the CLI archive | Shipped: in a per-user installer (the `-npu` build: its archive only), and in the CLI archive | Built into the app, run on a thread of its own (no web UI: the app is the UI) |
| **Image input** (a projector beside the model, [server-api.md](server-api.md#chat) "Images") | Run: a streamed 35B-A3B MoE with its projector on a 12 GB phone through the API; built: the composer attaches an image from the photos or the camera, owed its run on the phone | Run: a streamed 35B-A3B MoE with its projector (M-RoPE) and a small dense vision model, through the CLI, the server's three protocols and the web UI | Built | Built | Built; the composer attaches an image from the photos or the camera |
| **As a service** (`bmoe-server service install`, [server-api.md](server-api.md#running-as-a-service)) | - (the app's own foreground service) | Built: a launchd agent; owed a run at a login | Built: a systemd user unit with lingering; owed a run on a headless box | Built: a scheduled task at logon (S4U); owed a run | - |
| **Native app** | Measured: the Compose app, a client of the server | Run: `bmoe-desktop`, the web UI in a native window over the server ([desktop-app.md](desktop-app.md)) | Built: `bmoe-desktop` over WebKitGTK 4.1; owed a run on a desktop | Built: `bmoe-desktop` over WebView2; owed a run | Run: the SwiftUI app, a client of the server in its own process, on one 12 GB iPhone by a contributor ([benchmarks.md](benchmarks.md#iphone)); CI builds it for the simulator only |
| **Planner (`--auto`)** | Measured, in the app's load path | Measured on one laptop | Measured on rented x86_64 machines with NVIDIA cards from 8 to 32 GB, from the release archives and builds from source ([community-benchmarks.md](community-benchmarks.md)) | Owed: one documented run | Run by a contributor on one 12 GB iPhone, through the harness and the app (#235, #574) |
| **Device probes in a process of their own** | Yes | Yes | Yes | Yes, built; owed a run | - (no second process) |
| **CI** | Engine cross-compiled and the APK packaged, on PRs and tags | Compiled and smoke-tested, on PRs and tags | Built, byte-identity gates and UI tests, on every push | Compiled only, on PRs and tags; arm64 on an ARM64 runner, smoke-tested there | App and harness compiled for the simulator and the app's client package tested, on PRs and tags; the harness packaged by the release workflow |
| **Release artifact** | Signed APK, built by CI, with the Hexagon, Vulkan and OpenCL backends as modules | `.dmg` (ad-hoc signed, not notarised) and the CLI archive, with Metal | AppImage and the CLI archive, both architectures, with Vulkan; x86_64 the CUDA and HIP packs | Installer `.exe` (unsigned) and the CLI archive, both architectures, with Vulkan; arm64 adds OpenCL for the Adreno GPU; x86_64 the CUDA pack. All in the signed `feed.json` | Harness app for the simulator, unsigned, built by CI |
| **Run on hardware** | Measured: a 12 GB phone, Snapdragon class, UFS 4.x | Measured: a 16 GB Apple-silicon laptop | Measured: rented x86_64 machines, NVIDIA cards with the CUDA pack of the release (12 GB, [bench-data/2026-11-09-rented-12gb-cuda-pack](bench-data/2026-11-09-rented-12gb-cuda-pack/README.md); 12 GB against Strata, [bench-data/2026-11-13-same-card-strata-12gb](bench-data/2026-11-13-same-card-strata-12gb/README.md)) and a 192 GB AMD card. No desktop run of the AppImage | Measured: an x86 laptop, 8 cores, 16 GB, NVMe. arm64: owed, never run on a Snapdragon laptop | Measured by a contributor: a 12 GB iPhone, a 35B-A3B streamed at 2.6-3.0 tok/s in the app on 0.33.0 and 5.56 through the `--auto` harness on 0.42.0 |

Sources: [application-framework.md](application-framework.md) for the server on each platform,
[hardware-planning.md](hardware-planning.md) for the planner and the isolated probes,
[community-benchmarks.md](community-benchmarks.md) for the measured machines, and
[`examples/ios`](../examples/ios/README.md) for the app and the harness.

## Compute backends by platform

The engine implements no backend of its own; each one is llama.cpp's, reached through the public
device registry. [backends.md](backends.md) lists all eighteen the pinned submodule carries and
what each use of a device asks of one. This table is the subset that has been built or run here.

| Backend | Android | macOS | Linux | Windows | What it does today |
|---|---|---|---|---|---|
| **CPU** | Measured | Measured | Measured by the gates in CI | Measured | Decode, prefill and expert streaming, everywhere |
| **Hexagon** (Qualcomm NPU) | Measured, shipped in the release APK | - | - | Built, in its own Windows ARM64 archive (`-npu`) with the DSP libraries signed in CI; Windows runs them only after a setup the user does by hand, test-signing included ([npu-windows.md](npu-windows.md)); never run on a Snapdragon laptop | Prefill on the NPU, decode on the CPU ([npu-prefill.md](npu-prefill.md)) |
| **Metal** | - | Measured, on in every macOS build | - | - | Prefill on the GPU, decode on the CPU ([npu-prefill.md](npu-prefill.md)) |
| **OpenCL** | Measured on a Snapdragon-class phone, in the release APK: fast at prefill width and wrong, so the plan refuses it; the dense weights on a phone GPU lost 27 % in July | - | Not built | Shipped in the arm64 archive, with the Adreno kernels; never run | Nothing yet: refused on correctness on the phone; on a laptop the probe decides ([npu-prefill.md](npu-prefill.md)) |
| **Vulkan** | Measured on a Snapdragon-class phone, in the release APK: no faster than the cores at prefill width, not armed; on one 2020 phone its driver ended the probe, which the isolated probe contains | Not built | Shipped, a module in the release archives; never run | Shipped, as Linux; never run | Nothing yet: not a win on the phone measured ([npu-prefill.md](npu-prefill.md)) |
| **CUDA** | - | - | Shipped for x86_64 as a pack of its own with the NVIDIA runtime and cuBLAS it links, which the app installs into its data folder (the AppImage too) or which is extracted beside `bmoe-cli`; measured on rented 24 GB cards with builds from source | Shipped for x86_64 as Linux | Every role a device with memory of its own takes ([backends.md](backends.md)) |
| **HIP** (AMD ROCm) | - | - | Shipped for x86_64 as a pack, the module alone: the app installs it where ROCm 7's runtime loads; measured on a rented 192 GB datacenter card with a build from source, the model resident ([bench-data/2026-10-28-rented-rocm](bench-data/2026-10-28-rented-rocm/README.md)) | Not built | As CUDA |
| **SYCL** | - | - | Not built: a module built with the toolchain and dropped beside the binary is loaded like the shipped ones | Same as Linux | Nothing |
| **RPC** | - | - | Test fixture for the gates | - | Nothing a front-end can select |

Every release has the same shape: the engine and, beside it, one module per backend and one CPU
module per instruction-set tier, loaded at start-up ([backends.md](backends.md)). The APK carries
Vulkan and OpenCL beside Hexagon, the Linux and Windows archives carry Vulkan, the Windows arm64
archive OpenCL as well (its `-npu` twin Hexagon too), the macOS archive Metal. A module whose driver library the machine lacks is not loaded, so the same archive serves
a laptop with a GPU and a server without one, and the planner finds whichever devices registered.
CUDA and HIP are the exceptions to "in the archive": with the NVIDIA runtime and cuBLAS the CUDA
module is about twenty times the rest, and HIP needs ROCm installed system-wide, so x86_64 releases
carry each as a pack of its own (`bmoe-pack-cuda-<tag>-<target>`, `bmoe-pack-hip-<tag>-<target>`),
which the desktop app installs into its data folder where it applies and the engine loads from
there ([desktop-app.md](desktop-app.md)). For `bmoe-cli`, extract the pack inside the archive's
folder, next to `bmoe-cli`, where its files load like the others. The pack has no folder of its own,
so extracting both archives in the same place leaves the CUDA files one level above `bmoe-cli`.
The engine does not load them from there and does not say so, and a 12 GB card then decodes on the
CPU at a sixth of its speed
([bench-data/2026-11-09-rented-12gb-cuda-pack](bench-data/2026-11-09-rented-12gb-cuda-pack/README.md)).

A device with memory of its own computes the experts through llama.cpp's device-side expert cache,
resident or streamed, a miss uploaded from the host; `--auto` arms it where the device measures
faster than the cores ([backends.md](backends.md#experts-computed-on-a-device-from-a-cache-in-its-memory)).
On unified memory it does not pay and the plan does not arm it: there the experts are computed on
the cores, and the GPU takes the prefill.

## What each platform owes

- **Android.** The NPU prefill runs on the release APK (#264 closed): on a 12 GB phone it
  prefilled 1906 tokens of Qwen3.6-35B-A3B at 137-140 tok/s with the Hexagon backend at its
  defaults (0.43.0, #542). Owed: the GPU prefill run on the same APK, Vulkan and OpenCL against the
  NPU and the cores. Per-value provenance in the settings, so a reader sees
  what the plan set and what differs from the default ([screens.md](screens.md)).
- **macOS.** The `.dmg` notarised, and an app that updates itself (#385). A run of the
  desktop app with its Metal module on a Mac, before and after each release.
- **Linux.** Measured only on rented headless machines with discrete cards. Owed: a run of the
  AppImage and `bmoe-desktop` on a desktop, a Vulkan run on any GPU, and an aarch64 machine
  ([community-benchmarks.md](community-benchmarks.md)).
- **Windows.** The byte-identity gates run in its CI job, the device probes run in a process of
  their own on an integrated GPU, and one planner run is documented
  ([hardware-planning.md](hardware-planning.md#a-planner-run-on-windows)). Owed: the container
  limit a job object sets, the process's own memory figures, and the installer signed (#385).
- **iOS.** A contributor has run the harness and the app on one 12 GB iPhone, signed by a free
  Personal Team (#235, #574). Owed: a build anyone can install without Xcode, a run on a second
  device, and a longer session under memory pressure than a few chat turns.
- **Every platform.** The planner measures and corrects itself from each run
  ([measured-planning.md](measured-planning.md)); what is owed is a run of that loop on every
  machine class above, each one a row in [community-benchmarks.md](community-benchmarks.md).

## Keeping this page true

A change that moves a cell updates this page in the same PR, as every other doc here. A release
re-reads the table against the release notes. A cell that cannot be backed by a linked page is
written as owed, not as done.
