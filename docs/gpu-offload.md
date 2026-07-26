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

## Status

Implemented and gated; **not yet measured on device**. The prize argued above is an inference from
the byte split and the compute/stall attribution, not a number anyone has run. Two things worth
checking first, because either could sink it:

- decode is batch-1, i.e. GEMV, which is memory-bound — a GPU's advantage there is bandwidth, not
  FLOPs, and it shares the same LPDDR;
- Adreno's `CL_DEVICE_MAX_MEM_ALLOC_SIZE` may sit below the dense footprint, in which case
  `--gpu-layers N` (offload the last N only) is the knob.
