# Compute backends: what llama.cpp carries, and what this engine has run on

The engine does not implement a compute backend. Every device it can use is one llama.cpp
registers, reached through the public `ggml_backend_dev_*` registry, so the list of what is
possible is llama.cpp's and the list of what is known to work is much shorter. This page keeps
the two apart.

## What the pinned llama.cpp carries

Read from `ggml/src/` and `ggml/CMakeLists.txt` of the pinned submodule (`dce969851`). A bump can
add or remove rows; redo the reading rather than trusting this table.

The last three columns come from reading the sources, not from running them: the type each backend
registers its devices as, whether its sources name `GGML_OP_MUL_MAT_ID` (the op every routed expert
goes through, so a backend without it sends the experts back to the CPU), and whether it can wrap
memory the caller already owns (`buffer_from_host_ptr`: the capability it advertises, and whether
the function is actually wired in). A backend naming an op is not proof it accepts every weight type
for it.

| backend | build flag | hardware | registers as | names the expert op | wraps host memory (advertised / wired) |
|---|---|---|---|---|---|
| CPU | `GGML_CPU` (on) | every machine | CPU | yes | yes / yes |
| BLAS | `GGML_BLAS` | a host BLAS library (Accelerate, OpenBLAS, ...) | accelerator | no | yes / yes |
| Metal | `GGML_METAL` | Apple GPUs | GPU | yes | yes / yes |
| CUDA | `GGML_CUDA` | NVIDIA GPUs | GPU, or integrated GPU | yes | no / no |
| HIP | `GGML_HIP` | AMD GPUs through ROCm; compiles the CUDA sources | as CUDA | as CUDA | as CUDA |
| MUSA | `GGML_MUSA` | Moore Threads GPUs; compiles the CUDA sources | as CUDA | as CUDA | as CUDA |
| Vulkan | `GGML_VULKAN` | any GPU with a Vulkan driver | GPU, or integrated GPU | yes | no / yes |
| OpenCL | `GGML_OPENCL` | Adreno first, other OpenCL GPUs | GPU | yes | no / yes |
| SYCL | `GGML_SYCL` | Intel GPUs through oneAPI | GPU, or integrated GPU | yes | no / yes |
| WebGPU | `GGML_WEBGPU` | browsers and native WebGPU | GPU | yes | no / no |
| Hexagon | `GGML_HEXAGON` | Qualcomm NPU (HTP) | GPU | yes | no / no |
| CANN | `GGML_CANN` | Huawei Ascend NPU | GPU | yes | no / no |
| OpenVINO | `GGML_OPENVINO` | Intel CPU, GPU and NPU through OpenVINO | GPU | yes | no / no |
| ET | `GGML_ET` | Esperanto ET-SoC | GPU | yes | no / no |
| zDNN | `GGML_ZDNN` | IBM Z | accelerator | no | no / no |
| ZenDNN | `GGML_ZENDNN` | AMD CPUs through ZenDNN | accelerator | yes | yes / yes |
| VirtGPU | `GGML_VIRTGPU` | a host GPU remoted into a virtual machine | (the remoted device's) | (the remoted device's) | no / yes |
| RPC | `GGML_RPC` | another machine, or a loopback server | GPU | (the remote's) | no / no |

Three things in that table decide what this engine can do with a backend.

- **The registered type is not a description of the hardware.** NPUs register as `GPU`, and Metal
  registers as `GPU` on unified memory. Nothing here reads the type as "has memory of its own";
  that is measured ([hardware-planning.md](hardware-planning.md)).
- **The advertised capability and the wired function disagree on four backends.** Vulkan, OpenCL,
  SYCL and VirtGPU report `buffer_from_host_ptr` false and implement it. The planner therefore
  records `true` as yes and anything else as unknown.
- **There is no backend for the Apple Neural Engine, for MediaTek or Samsung NPUs, or for an Intel
  NPU outside OpenVINO.** On those the engine has the CPU and whatever GPU backend the platform has.

## What this engine has run on

| backend | where | state | what it does today |
|---|---|---|---|
| CPU | Android, Windows, macOS, Linux | measured on phones and desktops; gates run in CI on Linux | decode, prefill, expert streaming |
| Hexagon | Android, Snapdragon with Hexagon v73 or later | measured on a phone; in the release APK (`scripts/build-hexagon-android.sh`) | prefill on the NPU, decode on the CPU ([npu-prefill.md](npu-prefill.md)) |
| Metal | macOS | measured on one 16 GB Apple-silicon laptop; on in every macOS build | prefill on the GPU, decode on the CPU ([npu-prefill.md](npu-prefill.md)) |
| RPC | host test builds | a test fixture only: a loopback server fronting the CPU, so the prefill-device gates have a second device | nothing a front-end can select |
| BLAS | macOS, where llama.cpp turns it on by default | linked, never given a weight | nothing placed on it |
| OpenCL | Android | off in every build script; the dense set on a phone's integrated GPU was measured and lost 27 % (`RunConfig::dense_on_device`) | nothing; the prefill device has not been tried on it |
| CUDA, HIP, Vulkan, SYCL | Linux, Windows | `scripts/build-portable.sh` turns each on when it finds the toolchain; none has been run here | unknown |
| MUSA, WebGPU, CANN, OpenVINO, ET, zDNN, ZenDNN, VirtGPU | - | never built here | unknown |

Three backends, then, with a measurement behind them, out of eighteen.

## What each role asks of a backend

The engine uses a device in one of four ways, and each asks for something different.

| role | what the backend must do | where it stands |
|---|---|---|
| decode on the host | be the CPU backend, with the expert-ready hook for the overlap | every platform |
| prefill device (`--prefill-device`) | run wide graphs, accept weights through `ggml_backend_tensor_set`, and take the model's weight types or a type the engine can convert to | Hexagon and Metal measured; the mechanism names no backend, so the others are untested, not excluded |
| layers on a device (`--gpu-layers`, from llama.cpp's capacity fitter) | have memory of its own, or the placement frees nothing | applied by the planner where a device has its own memory; never run on such a machine here. On unified memory the fitter's answer is set aside, and forcing it on the 16 GB laptop ended in the GPU's out-of-memory error |
| streamed experts computed on a device | wrap memory the streamer already reserved (`buffer_from_host_ptr`), which is what the fourth column above is about | not built. Metal offers it; Vulkan, OpenCL and SYCL implement it without advertising it; CUDA has no route through the public API |

## How a backend reaches the planner

No rule names a backend. A device that registers is enumerated, asked for its memory, and
measured: the same matmul on the model's own quantized format as the host, checked against the
host's result, and the cost of one host/device crossing. A device that does not reproduce the
host's result is excluded on correctness, whatever its speed.

So a backend llama.cpp adds needs no code here to be seen. It does need a build that carries it,
and a machine to run it on: the Metal backend, the first one tried on a desktop, surfaced three
faults no test had (one memory pool counted twice, an idle device taking graph nodes, a cache the
kernel compressed). The honest reading of "unknown" above is that the same should be expected of
each of the others.

`scripts/rented-box.sh` is there for that: one session on a machine nobody here owns, which
builds, plans, measures, and writes down what the plan predicted next to what the engine did.
