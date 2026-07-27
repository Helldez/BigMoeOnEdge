# GPU offload

`--gpu` runs the **non-expert** half of the model on a GPU. By default the routed experts stay on
the CPU, and the first half of this document is about why that default is the right one. Two flags
move them anyway: `--gpu-experts` for a model whose experts fit in device memory, and
`--gpu-expert-slots N` for one whose experts do not.

> **Measured verdict, and read it before reaching for any flag here: on an integrated mobile GPU,
> decode is slower at every placement we tried** — experts streamed into device memory 0.22×, full
> dense offload 0.73×, output head alone 0.80×, and even the whole model resident with nothing
> streamed ~0.79×, all against the CPU-only path on the same device. Batch-1 decode is
> bandwidth-bound matrix-vector work and an integrated GPU shares the CPU's memory, so there is no
> bandwidth to win. These flags exist because the question deserved an answer with numbers behind
> it, not because they are recommended. The GPU's one unrefuted advantage is batched **prefill**
> (~2.3×), which is a different measurement and still open. Evidence:
> [bench-data/2026-07-27-gpu-expert-slots](bench-data/2026-07-27-gpu-expert-slots/findings.md) and
> [bench-data/2026-07-27-gpu-dense-offload](bench-data/2026-07-27-gpu-dense-offload/findings.md).

## Why the experts cannot simply move

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

That argument is about **rebinding a pointer**, and it still holds. What it does not rule out is
rebinding the *contents*, which is what `--gpu-expert-slots` does — see below.

The other reason stands on its own: the upload-once model assumes the weights fit. A 22 GB model on
a 12 GB phone is the case this engine exists for, and no amount of pointer trickery creates memory.

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

## Moving the experts anyway

Two flags, for two different situations. Both leave the **router** on the CPU: the hook reads
`ffn_moe_topk-<il>` from the host, and a routing node computed on a device backend has no host
address to read.

`--gpu-experts` is for a MoE whose experts fit in what the driver will allocate. It simply stops
pinning them, so the expert matmuls run on the device. It is refused together with `--moe-stream`,
because that combination is the impossible one described above.

`--gpu-expert-slots N` is for a MoE whose experts do not fit — the case this engine exists for. The
model is loaded with each routed-expert tensor narrowed to `N` experts along dim 2 rather than
`n_expert` (`llama_model_params::moe_expert_slots`), turning it into a residency pool. The engine
keeps an LRU over those slots, reads a missing expert's slice from flash with O_DIRECT and writes it
into a slot, then rewrites the routing so the dispatch names slots instead of experts. The weights
never move; their *contents* do.

Two properties make this cheap. The backend's convert kernels address the expert dimension as a
whole multiple of the per-slice size and never read `ne02`, so one expert can be converted into an
arbitrary slot with nothing but sub-buffer offsets — no new kernels. And the router keeps the full
expert count, because `build_moe_ffn` takes it as an argument rather than from the tensor.

**Sizing matters more than anything else.** `N` must clear one token's working set — `k` experts per
layer — or the pool hits exactly 0%, the same cliff the CPU cache has. On Qwen3.6-35B-A3B (k=8) the
difference between `N=8` and `N=24` is 0.720 → 1.116 tok/s (+55%). One slot costs
`sum over projections of bytes-per-expert`, times the layer count: ~70 MiB on that model, so the
~4.5 GiB a phone GPU reports leaves room for roughly 35 once the dense half is resident.

### Measured status: correct, and not a speed win

Bit-exact and producing coherent text, at **1.026 tok/s against 4.689 for the CPU streaming path on
the same model, same binary, same bytes off the flash** (Q4_0, 24 slots; full numbers and raw data
in [bench-data/2026-07-27-gpu-expert-slots](bench-data/2026-07-27-gpu-expert-slots/findings.md)).

The token decomposes as compute 0.244 + flash read 0.302 + slice upload 0.406 + id readback 0.023
seconds. Two of those are worth stating precisely, because both contradict what was expected:

- **The per-layer synchronisation is not the problem.** Draining the GPU queue so the host can read
  the routing costs 2.4% of the token. The published prior art that killed this design on another
  backend measured ~400 µs per sync; here it is noise.
- **The upload is the problem, but fixing it is not enough.** It runs at 0.57 GiB/s against 4.8–5.9
  GiB/s of raw device bandwidth, because every projection of every miss pays a blocking write plus a
  blocking convert dispatch. Yet even with upload *and* read set to zero the GPU still owes
  0.267 s/token — 3.7 tok/s — which is below what the CPU path already delivers.

The floor underneath is the compute: 0.244 s/token against the CPU's 0.126, uniform across all 40
layers. Batch-1 decode is matrix-vector work bound by memory bandwidth, and an integrated GPU reads
the same LPDDR as the CPU while adding per-dispatch overhead and a format it cannot read natively.

So this path is a **research result, not a recommendation**, and the same is true of every other GPU
placement measured on this device — full dense offload is −27% and even offloading the output head
alone is −16 to −21%. Decode belongs to the CPU here. The GPU's one unrefuted advantage is batched
**prefill**, which is measured at ~2.3× and is the experiment still worth running.

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
