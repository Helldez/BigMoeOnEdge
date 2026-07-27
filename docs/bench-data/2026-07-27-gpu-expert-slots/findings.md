# Streaming MoE experts into a mobile GPU — measured on device (2026-07-27)

**Verdict: it works, it is bit-exact, and it is four and a half times slower than streaming the
same experts on the CPU. The bottleneck is not synchronisation, which the prior art predicted and
which we measured at 2.4% of the token; it is the upload, and underneath the upload there is a
compute floor that no amount of upload engineering reaches. On an integrated mobile GPU, batch-1
decode belongs to the CPU.**

This closes the GPU question for decode. Every placement was tried, from "one matmul on the
device" to "the whole model resident", and all of them land below the CPU-only path.

## Why this experiment existed at all

The engine's own documentation said the routed experts *can never* move to a GPU, because the
streamer works by rebinding an expert tensor's `->data` to a host buffer it refills, and a device
buffer has no host address to rebind (see [gpu-offload.md](../../gpu-offload.md)).

That argument is about rebinding a **pointer**. It says nothing about rebinding the **contents**.
If each expert tensor is allocated with only `N` experts along dim 2 it becomes a residency pool:
the engine keeps an LRU over the slots, reads a missing expert from flash, writes it into a slot,
and rewrites the routing so the dispatch names slots instead of experts. The weights never move;
what they hold does.

Two properties of the backend make this cheap enough to be worth building. The OpenCL convert
kernels address the expert dimension as a whole multiple of the per-slice size and **never read
`ne02`**, so converting one expert into an arbitrary slot needs nothing but sub-buffer offsets — no
new kernels. And the router keeps the full expert count regardless, because `build_moe_ffn` takes
it as an argument rather than from the tensor.

## Setup

One arm64 phone with an integrated GPU and UFS storage, `Qwen3.6-35B-A3B` at **Q4_0** (20.8 GB,
arch `qwen35moe`, 40 layers, 256 experts, top-8). Q4_0 rather than the Q4_K_M used in the README
tables because the slice writer only handles Q4_0/Q4_1 — **these numbers are therefore not
comparable to the published Q4_K_M figures**, and are not meant to be. Every cell here is
internally comparable: same binary, same file, same prompt, 96 generated tokens, `-c 2048
--ubatch 256`.

The model sits on `/data/local/tmp`, not on shared storage, because that is the only place where
O_DIRECT is real rather than silently buffered.

## The headline A/B

Back to back, same session, the CPU cell running **second** (i.e. on a hotter device) and winning
anyway:

| | GPU, 24 slots/layer | CPU expert streaming |
|---|---:|---:|
| **tok/s** | **1.026** | **4.689** |
| s/token | 0.975 | 0.213 |
| flash read per token | 236.7 MiB | 235.7 MiB |
| cache/pool hit rate | 56.5% | 54.4% |
| CPU occupancy | 15.8% | 71.6% |
| major faults/token | 0.02 | 0.00 |
| prefill (7 tokens) | 11.686 s | 2.734 s |

The two rows that make this a fair fight are the middle ones: **the two paths move the same bytes
off the flash with the same hit rate**. Nothing about the comparison is a difference in how much
work was avoided. What differs is only where the time goes.

## Where the time goes

The slot pool's stall is invisible to the streamer's counters — it happens inside the GPU dispatch,
not in the expert source — so it had to be instrumented separately before any of this could be
attributed. That instrumentation is the `gpu-slots:` report, and it decomposes the token like this:

| | GPU s/token | share | CPU s/token | share |
|---|---:|---:|---:|---:|
| compute | 0.244 | 25% | 0.126 | 59% |
| flash read | 0.302 | 31% | 0.057 (residual after overlap) | 27% |
| upload + convert into slots | 0.406 | 42% | — | — |
| id readback + remap + LRU | 0.023 | 2.4% | 0.031 (cache management) | 14% |
| **total** | **0.975** | | **0.213** | |

Three things to read here, in ascending order of importance.

**The synchronisation is cheap, and the prior art was wrong about this.** Stalling the GPU queue
once per MoE layer to let the host read the routing costs **0.023 s/token — 2.4%**, against the
~400 µs per `clFinish` a published Metal implementation measured and concluded was fatal. On this
device it is noise. The design that everyone assumed would die on synchronisation does not.

**The upload is the immediate bottleneck, and it is engineering, not physics.** 0.406 s/token at
**0.57 GiB/s**, on a device whose raw upload paths measure 4.76 GiB/s (map/unmap), 5.16 (with
`CL_MEM_HOST_WRITE_ONLY` write-combining) and 5.94 (plain `clEnqueueWriteBuffer`). The gap is not
bandwidth: it is one blocking `clEnqueueWriteBuffer` plus one blocking convert dispatch **per
projection per expert**, roughly 400 driver round-trips per token, each paying full latency. A
second command queue, double-buffered staging and a non-blocking wait would recover most of it.
The flash read is similarly unoptimised — one lane, issued serially inside the dispatch, at
0.77 GiB/s where the CPU path's four overlapped lanes hide all but 0.057 s of the same work.

**And none of that would be enough.** Set the upload and the read to zero — a perfect pool that
never misses and never waits — and the GPU still owes 0.244 + 0.023 = **0.267 s/token, which is
3.7 tok/s against the CPU path's measured 4.689**. The idealised version of the GPU loses to the
shipping version of the CPU. Everything above the compute row is worth fixing only if the compute
row were competitive, and it is not.

## The compute floor, layer by layer

`layer-ms.csv` in this directory is the decode-phase wall time per layer per token, averaged over
all 96 tokens, for both cells:

| | GPU | CPU | ratio |
|---|---:|---:|---:|
| mean over 40 layers | 23.3 ms | 4.77 ms | 4.9× |
| range | 3.8 – 41.4 ms | 1.2 – 7.4 ms | 3.0 – 6.5× |

It is uniform. There is no pathological layer, no single stage to fix — every layer pays the same
shape of cost. And the ratio persists in the isolated compute figure (0.244 vs 0.126 s/token,
**1.9×**) once the streaming work is excluded, which is the number that actually bounds the design.

The mechanism is the one thing about this that was predictable from first principles and got
checked last. Batch-1 decode is a chain of matrix-**vector** products: every weight is read once
and used for a single multiply-accumulate. Arithmetic intensity is near the floor, so the work is
bound by memory bandwidth, and on a SoC **the GPU reads the same LPDDR as the CPU** — there is no
separate faster memory to win with. What the GPU adds on top is per-dispatch overhead and a
format it cannot read natively. A GPU is a batch engine; token-by-token generation is the one
workload that never batches.

## The other placements, same day, same model

Two more configurations, to establish that the slot pool is not simply a bad implementation of a
good idea. Both use the full CPU streaming recipe plus `--drop-cold-experts 0.75`, and both were
run as a palindrome (`hyb, cpu, cpu, hyb`) so that drift is bounded rather than assumed. The SoC
changed clock state midway through; cells are compared **within** a clock state.

| placement | clock | tok/s | compute s/tok | majflt/tok |
|---|---|---:|---:|---:|
| output head only (`--gpu-layers 1`) | 3.03 GHz | 5.064 | 0.141 | 7.11 |
| **CPU only** | 3.03 GHz | **6.383** | 0.098 | 0.29 |
| **CPU only** | 2.27 GHz | **5.528** | 0.131 | 0.49 |
| output head only (`--gpu-layers 1`) | 2.27 GHz | 4.663 | 0.157 | 1.65 |
| full dense offload (`--gpu`, 40 layers) | 2.27 GHz | 4.062 | 0.192 | 9.95 |

`--gpu-layers 1` is **−21%** at the high clock and **−16%** at the low one — consistent in both
halves of the palindrome and well outside this device's ±6% run-to-run noise. Full dense offload is
**−27%**. On an earlier model (`Qwen3-30B-A3B`) the same two configurations measured neutral; the
difference is instructive rather than contradictory. The CPU path has since got faster
(`--drop-cold-experts` is worth ~+18% here), so the fixed cost of crossing the device boundary —
80 crossings per token for a 40-layer interleaved dense/expert graph — is now a larger share of a
smaller total.

The full-offload row also shows the trade in miniature: the GPU genuinely frees the CPU (occupancy
70% → 50%, and the overlapped I/O residual actually *improves*, 0.035 → 0.030 s/token), but compute
rises 0.131 → 0.192 and ~10 major faults per token appear from the pressure of a large OpenCL
compute buffer. The freed CPU time is real and the boundary tax eats all of it.

## The complete map

Every GPU placement measured against the same CPU-only baseline on this device:

| placement | relative decode throughput |
|---|---:|
| routed experts streamed into GPU slots | 0.22× |
| full dense offload, 40 layers | 0.73× |
| output head only, 1 layer | 0.80 – 0.84× |
| whole model resident, no streaming (`llama-bench`, different model) | ~0.79× |
| **CPU only** | **1.00×** |

The last row of that table is the finding. Note the fourth row especially: it is the GPU's
best possible case — everything resident, nothing streamed, no host in the loop at all — and it
still loses. No placement strategy can beat a configuration that has already removed every cost a
placement strategy could remove.

## What is not closed

**Prefill.** Every number here is decode. Prefill is batched, compute-bound, and the regime a GPU
actually suits; an independent measurement on this device put GPU prefill at ~2.3× the CPU's. The
prefill cells in this file are 7-token prompts, far too short to say anything. A long-prompt
comparison (1–2k tokens, `--gpu` dense-only, measuring `prefill_seconds`) is the one GPU experiment
left with a plausible positive outcome, and it is the shape of workload where a user would notice.

**Discrete GPUs.** Dedicated VRAM changes both the memory story and the transfer story. Nothing
here transfers to a desktop with a real graphics card.

## Method notes

- `--ubatch` must be capped or the compute-buffer reservation dominates everything (1203 MiB at
  `n_ctx` 2048 with a GPU in the graph, against 320 on CPU); this is why `--ubatch N` exists.
- Run-to-run noise on this device is ±6%. One run per cell decides nothing; a palindrome bounds
  drift without having to estimate it.
- Log `scaling_max_freq` **per cell**, not per sweep. It changed mid-sweep here, and a comparison
  across the change would have been meaningless.
- The device drops off adb over Wi-Fi under load. Run detached with `setsid` and write results per
  cell, so a dropped connection costs the connection and not the data.
- Instrument the new path before comparing it. In slot mode the streamer's counters read zero, so
  the first version of this comparison had a 0.73 s/token hole in it that looked like compute.
