# GPU offload of the dense path

`--gpu` runs the **non-expert** half of the model on a GPU. The routed experts always stay on the
CPU. That split is not a tuning choice, and this document is mostly about why.

## Why the experts can never move

The streamer works by rebinding an expert tensor's `->data` to a host buffer it refills from flash
before every token (see [seam.md](seam.md)). This works because the CPU backend reads that pointer
at graph-execution time, so bytes written between the routing node and the `MUL_MAT_ID` node are
the bytes the kernel multiplies.

No GPU backend behaves that way. ggml's OpenCL backend reads `tensor->data` exactly once, in
`init_tensor` at load, and thereafter dispatches against `cl_mem` handles cached in `tensor->extra`;
the pointer it read is not even a real address, just an offset base. There is no zero-copy path —
no `CL_MEM_USE_HOST_PTR`, no SVM, no dma-buf import — so a weight in device memory is a *copy* made
once at load. A streamed expert placed there would silently compute whatever the warm-up decode
left behind, forever.

The deeper reason survives any future backend that fixed the pointer question: the upload-once model
assumes the weights fit. A 22 GB model on a 12 GB phone is the case this engine exists for.

## Why the dense half is worth moving anyway

A MoE uses **100% of its dense weights on every token** but only `k / n_expert` of its expert
weights. That puts the two halves in the same order of magnitude:

| Model | dense weights | expert bytes touched per token |
|---|---:|---:|
| Qwen3-30B-A3B (128 experts, top-8) | 951 MiB | ~1.10 GiB |
| Qwen3.6-35B-A3B (256 experts, top-8) | ~1 GiB | ~0.67 GiB |

(The dense figure is the engine's own `dense-weights=anon — N MiB` line, i.e. every non-expert
tensor in the file.)

So the dense side is roughly half the per-token GEMV work — and decode on this engine is
compute-bound once the I/O levers are in: on Qwen3.6 with cache 3000, `ahwb` and drop-cold, a token
spends ~194 ms computing against ~41 ms stalled on flash. Moving half the arithmetic off four phone
cores is the largest remaining lever that costs no quality.

**It is lossless.** Placement changes where a matmul runs, not what the model is. Unlike
`--n-expert-used` or `--drop-cold-experts`, nothing is skipped. (Floating-point reassociation on a
different backend can still move the last bits of a logit; the byte-identity gates run on the CPU
and are not a claim about cross-backend determinism.)

## How it is arranged

Two things, set together in `session.cpp` before the load:

1. `llama_model_params::devices` gets the discovered GPU, and `n_gpu_layers` says how much to
   offload (`-1`, the default, means every layer).
2. `tensor_buft_overrides` pins the expert tensors to the CPU buffer type. llama.cpp resolves a
   matching override **before** it consults the layer's assigned device, so an expert stays in host
   memory even in a layer that was handed to the GPU.

The pin pattern is **derived from the architecture recipe**, not written out:
`expert_tensor_pattern()` turns a recipe's suffix table into `\.(ffn_gate_exps|ffn_up_exps|...)\.`.
A recipe row added for a new MoE family therefore pins its own experts with nothing else to update.
It deliberately also matches the per-expert companions (`ffn_down_exps.scale`), which the streamer
itself excludes: the streamer asks "do I read this from flash?", placement asks "which device
computes with this?", and the answers differ.

Device discovery (`core/src/engine/gpu_device.h`) goes through the ggml backend registry and names
no vendor or API. A build with no GPU backend and a device with no driver produce the same result —
no device found — and the engine falls back to the CPU with a note. `--gpu-require` turns that
fallback into a failure, which is what a benchmark wants: an A/B that silently compares CPU against
CPU is worse than one that refuses to start.

`--dense-weights anon|ahwb` and this are not in conflict but they do overlap: tensors the GPU took
are skipped by the rebind (they are not under host-memory reclaim pressure any more), and the
engine reports how many it left alone.

## Building it

Off by default. Enabling the OpenCL backend makes `libOpenCL.so` a hard load-time dependency, so
the binary **will not start** on a device without an OpenCL driver — while the default build runs
everywhere. The engine's own GPU support is runtime-probed and degrades cleanly, but that probe
never runs if the process cannot load.

```powershell
pwsh scripts/fetch-opencl-android.ps1      # Khronos headers + an ICD loader to link against
pwsh scripts/build-android.ps1 -OpenCL     # stages libggml-opencl.so into the app
```

The loader built by the first script is a **link stub only** and is not shipped: at runtime
`libOpenCL.so` resolves to the device's own driver in `/vendor/lib64`, which the app already has on
`LD_LIBRARY_PATH`. Shipping the Khronos loader would outrank the vendor driver and then find no ICD
behind it — everything links, nothing crashes, no GPU is ever found.

Host builds take `-DBMOE_OPENCL=ON` the same way; `scripts/build-host.sh` and CI pass no GGML flags,
so the default host build and the byte-identity gates are untouched.

### The Android linker step everyone hits

OpenCL is **not** an NDK library, and an Android app cannot load `/vendor/lib64/libOpenCL.so`
just because the file exists. Without the right declaration the dynamic linker fails the process
before `main` in one of two ways, both confusing:

```
library "libOpenCL.so" not found: needed by libggml-opencl.so in namespace (default)
cannot locate symbol "_ZN7android15PermissionCache15checkPermission..." referenced by libgui.so
```

The second one appears when `LD_LIBRARY_PATH` is widened to `/vendor/lib64:/system/lib64` to get
past the first: the loader then pulls the Adreno driver's own graphics dependencies into the app's
namespace, where they cannot resolve. Widening the path further does not fix it, and neither does
`LD_PRELOAD` — it is a namespace problem, not a search-path one.

The actual fix is one manifest line (already in the example app):

```xml
<uses-native-library android:name="libOpenCL.so" android:required="false" />
```

This works only if the driver is listed in the device's `/vendor/etc/public.libraries.txt`, which is
what makes a vendor library loadable by apps at all. Check before assuming:

```
adb shell cat /vendor/etc/public.libraries.txt | grep -i opencl
```

`required="false"` keeps the app installable on devices without it. Note the declaration is what
lets the app's spawned `bmoe-cli` link — the engine runs as a subprocess of the app, not via JNI.

## Reading what happened

Never trust the request — read the resolution:

- `BMOE_READY` carries `"gpu":"<device>"`, empty when the dense path ran on the CPU.
- The CSV preamble carries `gpu=<device>` or `gpu=cpu`, so a bench cell cannot be mistaken for a GPU
  cell it silently was not.
- The app's **Dense weights on GPU** setting shows the resolved device, and warns when a session
  asked for the GPU and got the CPU.

One caveat for benchmarking: an OpenCL-enabled binary registers and initialises the backend at
startup whether or not the toggle is on, so it carries a fixed init cost the default build does not.
Compare toggle-on against toggle-off **on the same binary**, not against a CPU-only build.

## Status: measured — between neutral and mildly negative, and inside the noise

Full numbers, the two wrong turns that preceded them, and method notes:
[bench-data/2026-07-27-gpu-dense-offload](bench-data/2026-07-27-gpu-dense-offload/findings.md).

At `--ubatch 256`, `Qwen3-30B-A3B`, cache 2000, one pass with CPU first and last:

| `--gpu-layers` | tok/s | graph splits | CPU occupancy |
|---|---:|---:|---:|
| 0 (CPU) | 2.941 … 2.778 | 1 | 88 → 90% |
| 48 (all) | 2.768 | 96 | **54%** |
| 12 | 2.866 | 24 | 62% |
| 1 (output head) | 3.029 | 2 | 68% |

**The GPU genuinely takes work off the CPU** — occupancy drops from 88% to 54% at full offload — and
within a single pass throughput orders perfectly and inversely by split count. But the device's
run-to-run noise is **±6%**, and a repeat of the best-looking cell reversed its sign (`--gpu-layers 1`
at 2.934 against a CPU cell at 3.116). Only the negative survives repetition: full offload is
consistently a little slower than CPU. Treat this as a lever that does not exist here, not as one
worth tuning.

It is never a correctness problem — output is token-identical, which is the losslessness claim
holding.

### Why: the halves interleave

A MoE layer is dense → routed experts → dense → routed experts, 48 times. Splitting those across two
devices crosses the boundary **twice per layer** — 96 graph splits against 1 — and every crossing
copies and synchronises an activation. The freed CPU time is real; the boundary tax eats it. That is
also why `--gpu-layers 1` comes closest: the output head is the one dense component with no expert
layer after it, so it costs 2 splits rather than 96 — but one matmul is too small a share of the
per-token work for its saving to clear the noise.

### The first two verdicts were wrong, in useful ways

The initial reading was "2.6x slower, because the GPU shares the same LPDDR and adds memory
pressure". Most of that pressure was self-inflicted: compute buffers are reserved for the worst-case
graph, and this engine set `n_ubatch = n_ctx` so any fitting prompt prefills in one pass. The
reservation therefore scaled with the **context**, not the offload — 1203 MiB at n_ctx 2048 against
301 MiB at 512, the same 4x the CPU path shows (320 → 80 MiB). Capping it moved the offload from
1.207 to 2.752 tok/s and major faults from 5 958 to 1.06. Hence `--ubatch N`, which is not a GPU fix
at all: the CPU path reserves 240 avoidable MiB of its own.

The second reading was "`--gpu-layers 1` wins by ~7%". It did not replicate. One run per cell was
not enough against ±6% noise.

The *pre*-measurement argument (the dense half is ~half the per-token arithmetic, and decode is
compute-bound) remains true and remains insufficient: it was about *how much work* the dense half
is, and said nothing about *where that work sits in the graph*. A byte-count argument is not a
placement argument.

### What could still change the answer

- **Contiguous layer blocks.** 96 splits is a property of *this* partition, not of offload as such.
  Offloading the first N layers **whole** — experts included, resident rather than streamed — puts
  the boundary in one place (~2 splits). llama.cpp already expresses per-block placement with a
  regex over `blk.<il>.`, so the CPU pin could be made layer-aware instead of global. The cost is
  that those layers stop streaming: ~363 MiB of resident experts per layer on this model, RAM the
  expert cache no longer gets. Untested, and the only remaining shape in which this could win here.
- **The cache curve, not a single point.** Full offload leaves decode more I/O-bound than before
  (compute 0.187 against flash 0.648 s/token), so cache and lane levers should buy *more* with the
  GPU on. The right experiment is the cache sweep under each backend, not GPU-vs-CPU at one budget.
  Note the GPU frees no RAM — the dense weights move from anon buffers to OpenCL buffers, both the
  same physical LPDDR.
- **Prefill**, which is batched and compute-bound — the regime a GPU actually suits. Every number
  here is decode.
- **Discrete GPUs**, where dedicated VRAM changes the memory and transfer stories at once. None of
  this transfers to a desktop.
