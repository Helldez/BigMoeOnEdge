# Compute backends: what llama.cpp carries, and what this engine has run on

The engine does not implement a compute backend. Every device it can use is one llama.cpp
registers, reached through the public `ggml_backend_dev_*` registry, so the list of what is
possible is llama.cpp's and the list of what is known to work is much shorter. This page keeps
the two apart.

The same facts cut by operating system, next to the rest of what each platform has, are in
[platforms.md](platforms.md).

## What the pinned llama.cpp carries

Read from `ggml/src/` and `ggml/CMakeLists.txt` of the pinned submodule (`ad98071a5`, read again at that bump: same backends, same columns). A bump can
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
| OpenCL | `GGML_OPENCL` | Adreno first, other OpenCL GPUs | GPU | yes | no / a function that declines |
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
- **The advertised capability and the wired function disagree on three backends.** Vulkan, SYCL
  and VirtGPU report `buffer_from_host_ptr` false and implement it. OpenCL reports it false and
  wires a function that returns nothing, so there the false holds in effect, and the call is safe
  to make. The planner therefore records `true` as yes and anything else as unknown.
- **There is no backend for the Apple Neural Engine, for MediaTek or Samsung NPUs, or for an Intel
  NPU outside OpenVINO.** On those the engine has the CPU and whatever GPU backend the platform has.

## What this engine has run on

| backend | where | state | what it does today |
|---|---|---|---|
| CPU | Android, Windows, macOS, Linux | measured on phones and desktops; gates run in CI on Linux | decode, prefill, expert streaming |
| Hexagon | Android, Snapdragon with Hexagon v73 or later | measured on a phone; in the release APK (`scripts/build-hexagon-android.sh`). On Windows on Snapdragon, built into its own ARM64 archive (`-npu`), its DSP libraries in a catalog signed in CI: the driver runs them only after the user turns test-signing on and trusts that certificate, by hand ([npu-windows.md](npu-windows.md)); never run on a laptop | prefill on the NPU, decode on the CPU ([npu-prefill.md](npu-prefill.md)) |
| Metal | macOS | measured on one 16 GB Apple-silicon laptop; on in every macOS build | prefill on the GPU, decode on the CPU ([npu-prefill.md](npu-prefill.md)) |
| RPC | host test builds | a test fixture only: a loopback server fronting the CPU, so the prefill-device gates have a second device | nothing a front-end can select |
| BLAS | macOS, where llama.cpp turns it on by default | linked, never given a weight; the planner does not list it as a device, since it registers as an accelerator, part of the host | nothing placed on it |
| OpenCL | Android, Windows on ARM | built, in the Windows ARM64 archive with the Adreno kernels (never run on a Snapdragon laptop; the driver's own loader is used, none is shipped) and in the release APK beside Hexagon (`scripts/build-hexagon-android.sh`), importing the Khronos ICD loader so a phone without OpenCL still starts the engine, and the vendor's own library where there is one; in July the dense set on a phone's integrated GPU was measured and lost 27 % (`RunConfig::dense_on_device`) | run on a Snapdragon-class phone with an Adreno GPU: 344 GiB/s at prefill width against the cores' 36, and it did not reproduce the host's result, so the plan refuses it on correctness and the prefill stays where the decode is; named by hand it is refused by the arena, whose per-expert placement its padded allocations do not fit ([npu-prefill.md](npu-prefill.md)) |
| Vulkan | Android, Linux, Windows | built, in the release APK and in the Linux and Windows archives, as a module that loads only where a Vulkan driver is present. One 2020 phone ran it and its driver ended the probe, which is what the isolated probe contains | run on the same phone: 44 GiB/s one token wide against the cores' 36, and 36 at prefill width, so no prefill win; its fp16 path did not reproduce the host's result and its fp32 one did. Forced by hand, a 1727-token prompt prefilled in 85 s against 65 on the cores ([npu-prefill.md](npu-prefill.md)) |
| CUDA, HIP, SYCL | Linux, Windows | not in any archive: a module built where the toolchain is (`-DGGML_CUDA=ON`, ...) and dropped beside the binary is loaded like the shipped ones; none has been run here | unknown |
| MUSA, WebGPU, CANN, OpenVINO, ET, zDNN, ZenDNN, VirtGPU | - | never built here | unknown |

Three backends, then, with a measurement behind them, out of eighteen.

Two decisions behind the archives, written down so they are not re-made by accident. CUDA and HIP
stay out of the default archives: their runtime libraries weigh more than the rest of the archive,
and Vulkan reaches the same GPUs; a module built where the toolchain is loads from beside the
binary like the shipped ones. And a module that is present and never given a weight is not known
to be free: on one laptop with an integrated GPU, a build with Vulkan linked in loaded the model
five times slower than one without, cause not found. The modules build owes that measurement, on
the same machine, before the load time with an idle GPU module is called a non-event.

## What each role asks of a backend

The engine uses a device in one of four ways, and each asks for something different.

| role | what the backend must do | where it stands |
|---|---|---|
| decode on the host | be the CPU backend, with the expert-ready hook for the overlap | every platform |
| prefill device (`--prefill-device`) | run wide graphs, accept weights through `ggml_backend_tensor_set`, and take the model's weight types or a type the engine can convert to | Hexagon and Metal measured; the mechanism names no backend, so the others are untested, not excluded |
| layers on a device (`--gpu-layers`, from llama.cpp's capacity fitter) | have memory of its own, or the placement frees nothing | applied by the planner where a device has its own memory; never run on such a machine here. On unified memory the fitter's answer is set aside, and forcing it on the 16 GB laptop ended in the GPU's out-of-memory error |
| experts computed on a device from a cache in its memory | have memory of its own, and execute the model's expert matmul on its own layout: llama.cpp's device-side expert cache, fed from where the host holds the experts | planned by `--auto` where the device computes the experts faster than the host (below); measured on a 24 GB discrete card and on Metal |

## Experts computed on a device, from a cache in its memory

`--device-cache-mb N` puts llama.cpp's device-side expert cache (`moe_cache_size`) in front of the
experts the host holds. The layers go to the device (`--gpu-layers`) with their experts kept on the
host (`--tensor-overrides`), resident or served by the streamer as always, and the cache keeps the
experts used most in `N` MiB of the device's memory, computes them there, and uploads a routed
expert it does not hold from the host tensor that holds it. It needs no fork and names no backend:
any device llama.cpp's cache accepts can take it. The capture warm-up runs on a context without the
cache, because with it on the expert matmuls read its slots instead of the host tensors the capture
looks for; the context is then made again with it. It is refused with `--overlap` (whose wait sits
inside the CPU's expert matmul, which no longer runs) and with a prefill device (which moves the
layers); speculation works beside it, with the draft's own head served from the host. Gates G25a-c
(`tests/moe_gates.cpp`) prove the cached runs give the resident placement's bytes, the experts
resident, streamed, and streamed under n-gram speculation, on a device with memory of its own (the
loopback RPC device). Where llama.cpp reports the cache's slots (the fork's accessor,
[seam.md](seam.md#4-the-device-caches-slot-map-fork-extension-experimental)), the streamer leaves out
the routed experts the device holds, so the host's cache and the device's hold different experts. G25d
proves it skipped some and still gave those bytes.

**Planned.** `--auto` arms it where a device has memory of its own and the fitter has given whole
layers' experts some of it: the same bytes become a cache of the experts used most, from every
layer, wherever the device computes the model's experts faster than the host
(`choose_device_tier` in `core/src/plan/planner.cpp`, and
[measured-planning.md](measured-planning.md#step-4-device-memory-as-a-tier-of-the-expert-cache)):
the experts used most live next to the fastest processor, as RAM holds what flash would read. The
cache fills from use, so the first load arms it; a recorded routing says in the plan how much of a
token it serves.

**Where a miss is uploaded from.** A resident expert is a page of the file's mapping, ordinary
memory, and a device's backend copies ordinary memory through a staging buffer of its own. Every
backend may offer a host buffer type of its own (`ggml_backend_dev_host_buffer_type`: page-locked
memory on CUDA, Vulkan and SYCL; none on Metal, OpenCL or RPC), and a copy from it can go straight to
the device. The isolated device probe times the same upload from both (`upload_gibs`,
`upload_host_buffer_gibs`), and where the second wins by the planner's margin and the host holds a
second copy of the experts beside everything else, the plan names the device in
`--expert-host-buffer`: at load the experts are copied into that buffer type, byte for byte in the
file's layout, and the expert tensors pointed at the copy as the streamer points them at its slots.
The capture that finds them runs on the context the device cache then makes again, so the decode
context carries no callback. Experts streamed from flash live in the streamer's slots, which this
does not move.

Measured on a 24 GB card in a rented container (31 CPUs, 241 GiB RAM), Qwen3.8-Flash-Next 125B Q2_0,
`--auto`, 256 greedy tokens: a miss reached the card at 9.9 GiB/s from the mapping and 20.6 from the
backend's host memory; decode went 63.5 → 72.6 tok/s, and with `--mtp --mtp-head` 75.5 → 89.4. The
cost is at load: copying 32 GB into page-locked memory took the load from 4.4 to 36 s. Two greedy
runs on that card differ after a few hundred characters with or without the copy (the device
cache's order of computation), so that card cannot show the copy changes no output, and no gate
does yet: the loopback RPC device the gates run on has no host buffer type of its own. The bytes
are copied unchanged and the tensors' shapes and strides untouched, which is the argument until a
gate runs on a device that has one.

Measured on a 24 GB discrete card (PCIe at about 26 GB/s, 48 host cores, 85 GiB of host memory),
Qwen3.8-Flash-Next 125B GSQ-RCO Q2_0, every layer on the card, greedy:

| experts | tok/s |
|---|---|
| all computed on the cores | 18.9 |
| whole layers on the card, the host's 24 of 48 layers on the cores (the fitter's kind of split, the best of two) | 31.9 |
| resident on the host, behind a 16 GB device cache, 256 tokens (91.5% device hits) | 61.8, 62.8 |
| the same, 1024 tokens (97% hits once settled) | 77.9 overall, 84-88 from the 256th token |
| streamed from flash behind a 16 GB device cache, the host cache cold | 26.2 |

The route that would have avoided a second cache, handing the device the streamer's own memory with
`buffer_from_host_ptr`, does not work on Metal: the device holds the whole wrapped range in memory
at once. Twelve 144 MiB reservations, mostly untouched, wired 1729 MiB, so the full expert set of a
streamed model would be wired.

On a 16 GB unified-memory laptop, 64 greedy tokens:

| | tok/s |
|---|---|
| a 6 GB 64-expert cut of Qwen3.6-35B-A3B, resident, every layer on the GPU | 54 |
| the same, experts on the host behind the device cache, no streaming (77-88% hits) | 19 |
| the same, streamed through the device cache | 18.6 |
| the same, streamed and computed on the cores (today's path) | 27.1 |
| Qwen3.6-35B-A3B Q4_K_M (22 GB) streamed through the device cache, 2.5 GB of it | 6.7-7.0 |

The cost is the cache's, not the streamer's: without the streamer it holds at 19 tok/s from 77% to
88% device hits, about 33 ms a token over the resident run, or 0.8 ms for each of the 40 layers.
The cache reads every layer's selection on the host before it can upload, which synchronizes the
device once per layer; on this machine that is the whole gap. Uploads are cheap by comparison
(409 MB a token at 13.4 tok/s, 62 MB at 18.9). These runs had a further cost. The streamer read every
routed expert from flash, including the ones the device already held, because the live slot map was
not public: the graph shows only the device's copy of it, which a later layer's evictions can make
stale, and skipping a read on its word would upload bytes nobody wrote. The host cache and the device
cache therefore held many of the same experts. The fork's slot-map accessor removes both: the map is
read at the one point where a slot is guaranteed to hold
([seam.md](seam.md#4-the-device-caches-slot-map-fork-extension-experimental)). The teacher-forced perplexity of the three GPU paths
agrees to 0.07% (13.3613 resident, 13.3520 cached, 13.3599 streamed; the cores read 13.3427 resident
and streamed alike), the spread of a different reduction order: the GPU paths are not byte-identical
to each other the way the host's streamed and resident runs are.

So on unified memory it does not pay, and the plan never arms it there: a shared pool has no second
tier to fill. On the discrete card above the same synchronization is cheap and the cache is the
fastest placement measured.

## How a backend reaches the planner

No rule names a backend. A device that registers is enumerated, asked for its memory, and
measured: one layer of the model itself, its expert block and its dense matrices from the file's
own bytes, run as the host runs it and checked against the host's result, and the cost of one
host/device crossing. A device that does not reproduce the
host's result is excluded on correctness, whatever its speed.

So a backend llama.cpp adds needs no code here to be seen. It does need a module beside the
executable, and a machine to run it on. Every release is built that way (`BMOE_BACKEND_MODULES`,
the top-level CMakeLists): the engine knows no backend at compile time, each backend is a library
ggml loads from the executable's directory at start-up, the CPU one comes in one library per
instruction-set tier with the best picked on the machine, and a module whose driver library is
missing fails to load and is simply not there, instead of keeping the binary from starting. The
plan then reports what registered, and keeps "this machine has no accelerator" apart from "this
build carries none". The Metal backend, the first one tried on a desktop, surfaced three
faults no test had (one memory pool counted twice, an idle device taking graph nodes, a cache the
kernel compressed). The honest reading of "unknown" above is that the same should be expected of
each of the others.

**One card, several backends.** A card can be driven by more than one backend, and each registers a
device of its own for it: CUDA and Vulkan on an NVIDIA board, HIP (AMD's ROCm) and Vulkan on an AMD
one, SYCL and Vulkan on an Intel one. llama.cpp recognises them as one card by the PCI id the backend
reports (`ggml_backend_dev_props::device_id`) and keeps the first registered, which is an order,
not a measurement. The plan reads the same id (`ComputeDevice::physical_id`) and compares the
backends of one card on the same measured rate: this model's expert block where the real-layer
probe ran on all of them, the GEMV otherwise, never one of each. Another backend replaces the first
only where it is faster by the planner's margin; the plan then names the devices in
`--device-list`, the capacity fitter places on that list, and every rule after the choice sees one
device per card (`core/src/plan/backend_choice.cpp`). A device whose id the backend does not report
is never taken for another one's card. Nothing here knows which backend is usually faster where:
the release archives carry the modules, and the machine answers.

`scripts/rented-box.sh` is there for that: one session on a machine nobody here owns, which
builds, plans, measures, and writes down what the plan predicted next to what the engine did.
