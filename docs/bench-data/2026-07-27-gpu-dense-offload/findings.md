# Dense-path GPU offload — measured on device (2026-07-27)

> **Corrected the same day. The first verdict below ("2.6x slower, the GPU adds memory pressure")
> blamed the GPU for a cost this engine was inflicting on itself.** Most of the loss came from the
> compute buffers being reserved for a full-context prefill graph — our own `n_ubatch = n_ctx`
> choice — not from the offload. Capping the ubatch took the GPU from 1.207 to **2.752 tok/s** and
> major faults from 5 958 to **1.06**. The section "What the first pass got wrong" has the detail;
> the headline numbers in the table are kept because they are real, they were just misattributed.

**Verdict (as it stands): the offload still loses to the CPU, but by ~15% rather than 2.6x, and the
remaining gap is the graph-split cost alone. Not yet refuted — the sweep that would settle it
(`--gpu-layers 1`, i.e. output head only, which crosses the device boundary once instead of 96
times) is owed.** It is correct throughout — the output is token-identical — just slower so far.

## Setup

One device (arm64 phone, integrated Adreno-class GPU, UFS 4.x), `Qwen3-30B-A3B-Q4_K_M`
(`qwen3moe`, 48 layers, 128 experts, top-8). Every cell: `--moe-stream --cache-mb 2000
--io-threads 4 --overlap --dense-weights anon`, same prompt. The engine binary is identical across
cells — the OpenCL backend is linked in for all of them, so the backend's fixed init cost is not
what separates them.

The matched pair ran **GPU first**, which favours the GPU: the device was coolest for it.

## Numbers

| Config | tok/s | compute s/tok | majflt/token | graph splits | OpenCL model buf |
|---|---:|---:|---:|---:|---:|
| CPU (no offload) | **3.175** | 0.124 | **0** | 1 | — |
| `--gpu-layers 12` | 2.985 | — | 234 | 24 | 357 MiB |
| `--gpu` (all 48 layers) | **1.207** | 0.614 | **5 958** | 98 | 785 MiB |

Flash traffic is identical across cells (485.5 vs 486.0 MiB/token), so the streamer did exactly the
same I/O work: nothing here is an artefact of a different read pattern.

## Why — two compounding costs, both of which scale with the offload

**1. The halves interleave, so the split is paid 96 times per token.** A MoE layer is
dense → routed experts → dense → routed experts, 48 times. Putting the dense half on the GPU and
the experts on the CPU therefore crosses the device boundary twice per layer: **98 graph splits**
against 1 for the CPU-only graph. Every crossing copies and synchronises an activation. This is
structural, not a tuning problem — it is what "dense on one device, experts on another" means for
this architecture. At 12 layers the splits drop to 24 and most of the loss goes with them.

**2. The GPU has no memory of its own.** This is the deeper one. On a phone the GPU shares the same
LPDDR, so "offloading" does not relieve memory pressure — it **adds** to it. The OpenCL allocations
(785 MiB of weights plus a **1203 MiB compute buffer**, and note that buffer is a fixed cost that
does not shrink with `--gpu-layers`) land on top of the 2000 MiB expert cache and the mmap'd model,
and push the system into reclaim. The result is a fault storm: **5 958 major faults per token
against zero on the CPU run**.

That zero is the point. `--dense-weights anon` exists precisely to keep the dense weights out of
the page cache so a reclaim cannot drop them to a flash refault, and it achieves exactly that on the
CPU path. The GPU offload reintroduces the memory war that this engine's whole dense-weight policy
was built to end.

## What the first pass got wrong — the compute buffer, not the GPU

Cost 2 above is largely **self-inflicted and fixable**. The compute buffer is reserved for the
worst-case graph, and this engine sets `n_ubatch = n_ctx` so that any fitting prompt prefills in one
pass. That makes the reservation scale with the context, not with the offload:

| n_ctx | OpenCL compute buffer | CPU compute buffer |
|---|---:|---:|
| 2048 | 1203 MiB | 320 MiB |
| 512 | 301 MiB | 80 MiB |

Exactly 4x for a 4x context — nothing to do with the GPU, which needs the same buffer per token
either way. Re-running the offload with the smaller reservation:

| Config | tok/s | compute s/tok | majflt/token |
|---|---:|---:|---:|
| `--gpu`, n_ctx 2048 | 1.207 | 0.614 | 5 958 |
| `--gpu`, n_ctx 512 | **2.752** | 0.188 | **1.06** |
| CPU, n_ctx 2048 | 3.175 | 0.124 | 0 |

The fault storm was never the GPU "having no memory of its own" in any interesting sense — it was
~900 MiB of avoidable reservation pushing an already-tight system into reclaim. With it gone the
offload costs ~15%, not 2.6x, and what remains is cost 1: the 98 graph splits, whose per-token
compute overhead (0.188 against 0.124) is the whole residual.

This is why `--ubatch N` now exists as its own knob. Note it is not a GPU fix: 240 MiB of the CPU
path's own reservation is equally avoidable, and on this engine every MiB reserved is a MiB the
expert cache does not get.

**Owed, and the reason this is not filed as refuted:** a `--gpu-layers` sweep at a capped ubatch,
in particular `--gpu-layers 1`. That offloads only the output head — a single large matmul at the
end of the graph, with no expert layer after it to hand control back to — so it should cost 1-2
splits instead of 98 while still moving real work. If the residual is entirely split cost, that is
the configuration where the offload can win.

## What the pre-measurement argument got wrong

The case for this feature was: a MoE uses 100% of its dense weights every token against
`k/n_expert` of its experts, so the dense half is ~half the per-token arithmetic, and decode is
compute-bound. Both premises are still true — and both were irrelevant.

The reasoning was about **how much work** the dense half is. It said nothing about **where that work
sits in the graph** (interleaved with the experts, so the boundary is crossed constantly) or about
**what the accelerator costs to feed** (physical RAM, on a device already losing a memory war). A
byte-count argument is not a placement argument, in the same way a hit-rate curve is not a
throughput argument.

## What this does not settle

- **One device class.** An integrated mobile GPU sharing LPDDR is the worst case for both mechanisms.
  A discrete GPU with its own VRAM inverts cost 2 entirely, and this says nothing about it.
- **One model, one run per cell.** The gap is far larger than any plausible drift, and the
  dose-response across 0/12/48 layers orders perfectly, but these are not repeated cells.
- **Prefill is unmeasured.** It is compute-bound and batched, which is the regime a GPU is actually
  good at; every number here is decode.
- **It is not a correctness problem.** GPU output was token-identical to CPU output. The feature
  works; it is simply not worth using here.
