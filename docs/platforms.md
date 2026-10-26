# Platform support

What runs where, how far each platform has been taken, and what each one still owes. This page is
the summary; the evidence behind every cell is in the page it links to, and a cell with no
measurement behind it says so rather than implying one.

Last reviewed on 2026-10-19, against release 0.32.0.

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
| **`bmoe-server` and the web UI** | Shipped, in the APK as a child process of the app | Shipped: `BigMoeOnEdge.app` in a `.dmg`, and the desktop archive | Shipped: an AppImage, and the desktop archive | Shipped: a per-user installer (the `-npu` build: archive only), and the desktop archive | Built into the app, run on a thread of its own (no web UI: the app is the UI) |
| **Native app** | Measured: the Compose app, a client of the server | - (web UI in the browser) | - (web UI in the browser) | - (web UI in the browser) | Built: the SwiftUI app, a client of the server in its own process; run in the simulator only |
| **Planner (`--auto`)** | Measured, in the app's load path | Measured on one laptop | Built, not run on Linux hardware | Owed: one documented run | Run through the harness on one 12 GB phone by a contributor (#235); its dense policy corrected in #304 |
| **Device probes in a process of their own** | Yes | Yes | Yes | Yes, built; owed a run | - (no second process) |
| **CI** | Engine cross-compiled and the APK packaged, on PRs and tags | Compiled and smoke-tested, on PRs and tags | Built, byte-identity gates and UI tests, on every push | Compiled only, on PRs and tags; arm64 on an ARM64 runner, smoke-tested there | App and harness compiled for the simulator and the app's client package tested, on PRs and tags; the harness packaged by the release workflow |
| **Release artifact** | Signed APK, built by CI, with the Hexagon, Vulkan and OpenCL backends as modules; Obtainium follows the releases | `.dmg` (ad-hoc signed, not notarised), CLI and desktop archives, with Metal | AppImage, CLI and desktop archives, both architectures, with Vulkan | Installer `.exe` (unsigned), CLI and desktop archives, both architectures, with Vulkan; arm64 adds OpenCL for the Adreno GPU | Harness app for the simulator, unsigned, built by CI |
| **Run on hardware** | Measured: a 12 GB phone, Snapdragon class, UFS 4.x | Measured: a 16 GB Apple-silicon laptop | Owed: the gates only, no throughput run | Measured: an x86 laptop, 8 cores, 16 GB, NVMe. arm64: owed, never run on a Snapdragon laptop | Measured by a contributor: a 12 GB phone, a 35B-A3B streamed at 1.2 tok/s before #304 |

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
| **CUDA** | - | - | Shipped for x86_64 as a pack of its own with the NVIDIA runtime and cuBLAS it links, extracted beside an archive's binaries (not the AppImage, which is read-only); measured on rented 24 GB cards with builds from source | Shipped for x86_64 as Linux, extracted beside an archive's binaries or the installed app | Every role a device with memory of its own takes ([backends.md](backends.md)) |
| **HIP** (AMD ROCm) | - | - | Shipped in the x86_64 archives and packages as the module alone: it loads where ROCm 7 is installed and is skipped where it is not; never run | Not built | As CUDA |
| **SYCL** | - | - | Not built: a module built with the toolchain and dropped beside the binary is loaded like the shipped ones | Same as Linux | Nothing |
| **RPC** | - | - | Test fixture for the gates | - | Nothing a front-end can select |

Every release has the same shape: the engine and, beside it, one module per backend and one CPU
module per instruction-set tier, loaded at start-up ([backends.md](backends.md)). The APK carries
Vulkan and OpenCL beside Hexagon, the Linux and Windows archives carry Vulkan (Linux x86_64 also
HIP, without ROCm, which an AMD machine installs system-wide), the Windows arm64
archive OpenCL as well (its `-npu` twin Hexagon too), the macOS archive Metal. A module whose driver library the machine lacks is not loaded, so the same archive serves
a laptop with a GPU and a server without one, and the planner finds whichever devices registered.
CUDA is the exception to "in the archive": with the NVIDIA runtime and cuBLAS its module is about
twenty times the rest, so x86_64 releases carry it as a pack of its own (`bmoe-cuda-<tag>-<target>`)
whose files go beside the binaries, where the loader finds them as it finds the others.

Computing the streamed experts on a GPU, rather than on the CPU cores, is half built on every
platform: the device is measured, and the streamer's own memory is not yet handed to it
([roadmap.md](roadmap.md#hardware-planning--what-is-measured-and-what-is-still-owed)).

## What each platform owes

- **Android.** The NPU prefill run again on the release APK, the one build with the Hexagon
  backend: the 0.30.0 and 0.31.0 phone runs used CPU-only builds, and the last load with the NPU
  armed on the full-backend APK ended in the allocator (#264). The GPU prefill run on the same APK,
  Vulkan and OpenCL against the NPU and the cores. Per-value provenance in the settings, so a reader sees
  what the plan set and what differs from the default ([screens.md](screens.md)).
- **macOS.** The `.dmg` notarised, and an app that updates itself (#385). A run of the
  desktop app with its Metal module on a Mac, before and after each release.
- **Linux.** A throughput run on real hardware, which the community protocol can supply
  ([community-benchmarks.md](community-benchmarks.md)). A discrete GPU, for the planner's capacity
  fitter, prefill-device and graph-width rules.
- **Windows.** A run of the device probes in a process of their own, now built, on a machine with
  a GPU; the cache lock, the residency sample, the byte-identity gates in its CI job, one
  documented planner run, and the installer signed (#385). Tracked in
  [#234](https://github.com/Helldez/BigMoeOnEdge/issues/234).
- **iOS.** The app on the shared framework builds and runs in the simulator, and the release
  carries the harness for it. A contributor has run the harness on a device signed by a free
  Personal Team (#235), and the plan it read there is corrected in #304; owed are a device run of
  that plan, then the app on a device. Tracked in
  [#235](https://github.com/Helldez/BigMoeOnEdge/issues/235).
- **Every platform.** The planner's owed measurements and the loop that compares its prediction
  with what each run measured, tracked in [#237](https://github.com/Helldez/BigMoeOnEdge/issues/237).

## Keeping this page true

A change that moves a cell updates this page in the same PR, as every other doc here. A release
re-reads the table against the release notes. A cell that cannot be backed by a linked page is
written as owed, not as done.
