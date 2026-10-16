# Platform support

What runs where, how far each platform has been taken, and what each one still owes. This page is
the summary; the evidence behind every cell is in the page it links to, and a cell with no
measurement behind it says so rather than implying one.

Last reviewed on 2026-10-16, against release 0.30.0.

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

| | Android (arm64) | macOS (arm64) | Linux (x86_64, aarch64) | Windows (x86_64) | iOS |
|---|---|---|---|---|---|
| **Engine and `bmoe-cli`** | Shipped, in the APK | Shipped | Shipped | Shipped | Built, compiled into the harness app |
| **`bmoe-server` and the web UI** | Shipped, in the APK as a child process of the app | Shipped, desktop archive | Shipped, desktop archive | Shipped, desktop archive | Not built: an app may not start a process, and the in-process mode is owed |
| **Native app** | Measured: the Compose app, a client of the server | - (web UI in the browser) | - (web UI in the browser) | - (web UI in the browser) | Owed: a measuring harness only, not a chat app |
| **Planner (`--auto`)** | Measured, in the app's load path | Measured on one laptop | Built, not run on Linux hardware | Owed: one documented run | Built, run in the simulator only |
| **Device probes in a process of their own** | Yes | Yes | Yes | Yes, built; owed a run | - (no second process) |
| **CI** | Engine cross-compiled and the APK packaged, on PRs and tags | Compiled and smoke-tested, on PRs and tags | Built, byte-identity gates and UI tests, on every push | Compiled only, on PRs and tags | Compiled for the simulator, on PRs and tags |
| **Release artifact** | Signed APK, built by CI, with the Hexagon, Vulkan and OpenCL backends as modules | CLI and desktop archives, with Metal | CLI and desktop archives, both architectures, with Vulkan | CLI and desktop archives, with Vulkan | None |
| **Run on hardware** | Measured: a 12 GB phone, Snapdragon class, UFS 4.x | Measured: a 16 GB Apple-silicon laptop | Owed: the gates only, no throughput run | Measured: an x86 laptop, 8 cores, 16 GB, NVMe | Owed: the simulator only |

Sources: [application-framework.md](application-framework.md) for the server on each platform,
[hardware-planning.md](hardware-planning.md) for the planner and the isolated probes,
[community-benchmarks.md](community-benchmarks.md) for the measured machines, and
[`examples/ios`](../examples/ios/README.md) for the harness.

## Compute backends by platform

The engine implements no backend of its own; each one is llama.cpp's, reached through the public
device registry. [backends.md](backends.md) lists all eighteen the pinned submodule carries and
what each use of a device asks of one. This table is the subset that has been built or run here.

| Backend | Android | macOS | Linux | Windows | What it does today |
|---|---|---|---|---|---|
| **CPU** | Measured | Measured | Measured by the gates in CI | Measured | Decode, prefill and expert streaming, everywhere |
| **Hexagon** (Qualcomm NPU) | Measured, shipped in the release APK | - | - | - | Prefill on the NPU, decode on the CPU ([npu-prefill.md](npu-prefill.md)) |
| **Metal** | - | Measured, on in every macOS build | - | - | Prefill on the GPU, decode on the CPU ([npu-prefill.md](npu-prefill.md)) |
| **OpenCL** | Measured on a Snapdragon-class phone, in the release APK: fast at prefill width and wrong, so the plan refuses it; the dense weights on a phone GPU lost 27 % in July | - | Not built | Not built | Nothing yet: refused on correctness ([npu-prefill.md](npu-prefill.md)) |
| **Vulkan** | Measured on a Snapdragon-class phone, in the release APK: no faster than the cores at prefill width, not armed; on one 2020 phone its driver ended the probe, which the isolated probe contains | Not built | Shipped, a module in the release archives; never run | Shipped, as Linux; never run | Nothing yet: not a win on the phone measured ([npu-prefill.md](npu-prefill.md)) |
| **CUDA, HIP, SYCL** | - | - | Not built: a module built with the toolchain and dropped beside the binary is loaded like the shipped ones | Same as Linux | Nothing |
| **RPC** | - | - | Test fixture for the gates | - | Nothing a front-end can select |

Every release has the same shape: the engine and, beside it, one module per backend and one CPU
module per instruction-set tier, loaded at start-up ([backends.md](backends.md)). The APK carries
Vulkan and OpenCL beside Hexagon, the Linux and Windows archives carry Vulkan, the macOS archive
Metal. A module whose driver library the machine lacks is not loaded, so the same archive serves
a laptop with a GPU and a server without one, and the planner finds whichever devices registered.

Computing the streamed experts on a GPU, rather than on the CPU cores, is half built on every
platform: the device is measured, and the streamer's own memory is not yet handed to it
([roadmap.md](roadmap.md#hardware-planning--what-is-measured-and-what-is-still-owed)).

## What each platform owes

- **Android.** The NPU prefill run again on the release APK, the one build with the Hexagon
  backend: the 0.30.0 phone run used a CPU-only build. The GPU prefill run on the same APK, Vulkan
  and OpenCL against the NPU and the cores. Per-value provenance in the settings, so a reader sees
  what the plan set and what differs from the default ([screens.md](screens.md)).
- **macOS.** A signed installer that updates itself, in place of the archive. The byte-identity
  gates in CI, which today run on Linux only.
- **Linux.** A throughput run on real hardware, which the community protocol can supply
  ([community-benchmarks.md](community-benchmarks.md)). A discrete GPU, for the planner's capacity
  fitter, prefill-device and graph-width rules.
- **Windows.** A run of the device probes in a process of their own, now built, on a machine with
  a GPU; the cache lock, the residency sample, the byte-identity gates in its CI job, one
  documented planner run, and a signed installer. Tracked in
  [#234](https://github.com/Helldez/BigMoeOnEdge/issues/234).
- **iOS.** A device, an Apple development team and the memory entitlements, none of which this
  project has, then an app on the shared framework running the server in-process. Tracked in
  [#235](https://github.com/Helldez/BigMoeOnEdge/issues/235) as a call for a contributor.
- **Every platform.** The planner's owed measurements and the loop that compares its prediction
  with what each run measured, tracked in [#237](https://github.com/Helldez/BigMoeOnEdge/issues/237).

## Keeping this page true

A change that moves a cell updates this page in the same PR, as every other doc here. A release
re-reads the table against the release notes. A cell that cannot be backed by a linked page is
written as owed, not as done.
