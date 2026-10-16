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
| **Device probes in a process of their own** | Yes | Yes | Yes | Owed: in-process only | - (no second process) |
| **CI** | Engine cross-compiled and the APK packaged, on PRs and tags | Compiled and smoke-tested, on PRs and tags | Built, byte-identity gates and UI tests, on every push | Compiled only, on PRs and tags | Compiled for the simulator, on PRs and tags |
| **Release artifact** | Signed APK, built by CI, with the Hexagon backend | CLI and desktop archives | CLI and desktop archives, both architectures | CLI and desktop archives | None |
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
| **OpenCL** | Tried, off in every build: the dense weights on a phone GPU lost 27 % | - | Not built | Not built | Nothing; the prefill device has not been tried on it |
| **Vulkan** | Tried on one 2020 phone: its driver ended the process, which the isolated probe now contains | Not built | Built by `scripts/build-portable.sh` when `glslc` is present, never run | Same as Linux | Nothing |
| **CUDA, HIP, SYCL** | - | - | Built by `scripts/build-portable.sh` when the toolchain is present, never run | Same as Linux | Nothing |
| **RPC** | - | - | Test fixture for the gates | - | Nothing a front-end can select |

No release archive carries a GPU backend other than Metal: the Linux and Windows archives are
CPU-only, and a GPU build comes from `scripts/build-portable.sh` on the machine that will run it.

Computing the streamed experts on a GPU, rather than on the CPU cores, is half built on every
platform: the device is measured, and the streamer's own memory is not yet handed to it
([roadmap.md](roadmap.md#hardware-planning--what-is-measured-and-what-is-still-owed)).

## What each platform owes

- **Android.** The NPU prefill run again on the release APK, the one build with the Hexagon
  backend: the 0.30.0 phone run used a CPU-only build. Per-value provenance in the settings, so a reader sees what the plan set and what differs from the
  default ([screens.md](screens.md)).
- **macOS.** A signed installer that updates itself, in place of the archive. The byte-identity
  gates in CI, which today run on Linux only.
- **Linux.** A throughput run on real hardware, which the community protocol can supply
  ([community-benchmarks.md](community-benchmarks.md)). A discrete GPU, for the planner's capacity
  fitter, prefill-device and graph-width rules.
- **Windows.** Device probes in a process of their own, the cache lock, the residency sample, the
  byte-identity gates in its CI job, one documented planner run, and a signed installer. Tracked
  in [#234](https://github.com/Helldez/BigMoeOnEdge/issues/234).
- **iOS.** A device, an Apple development team and the memory entitlements, none of which this
  project has, then an app on the shared framework running the server in-process. Tracked in
  [#235](https://github.com/Helldez/BigMoeOnEdge/issues/235) as a call for a contributor.
- **Every platform.** The planner's owed measurements and the loop that compares its prediction
  with what each run measured, tracked in [#237](https://github.com/Helldez/BigMoeOnEdge/issues/237).

## Keeping this page true

A change that moves a cell updates this page in the same PR, as every other doc here. A release
re-reads the table against the release notes. A cell that cannot be backed by a linked page is
written as owed, not as done.
