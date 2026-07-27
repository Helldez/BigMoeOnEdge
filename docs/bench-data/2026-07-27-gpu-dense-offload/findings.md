# Dense-path GPU offload — measured on device (2026-07-27)

**Verdict: with the memory confound removed, GPU offload of the dense path is between neutral and
mildly negative on a phone-class integrated GPU — and the residual differences are inside this
device's run-to-run noise, which is ±6%. It is never a correctness problem: output is
token-identical. It is simply not a lever here.**

This file records three passes, because the first two were wrong in instructive ways and the
corrections are the useful part.

## Setup

One device (arm64 phone, integrated Adreno-class GPU, UFS 4.x), `Qwen3-30B-A3B-Q4_K_M`
(`qwen3moe`, 48 layers, 128 experts, top-8). Shared configuration in every cell:
`--moe-stream --cache-mb 2000 --io-threads 4 --overlap --dense-weights anon`, same prompt, 96
tokens. The binary is identical across cells — the OpenCL backend is linked in even for the CPU
cells — so backend init cost is not what separates them.

## Pass 1: the wrong answer (n_ctx 2048, uncapped ubatch)

| Config | tok/s | compute s/tok | majflt/token | graph splits |
|---|---:|---:|---:|---:|
| CPU | 3.175 | 0.124 | 0 | 1 |
| `--gpu-layers 12` | 2.985 | — | 234 | 24 |
| `--gpu` (all 48) | 1.207 | 0.614 | 5 958 | 98 |

Read at the time as "2.6x slower, because the GPU shares the same LPDDR and adds memory pressure".
That attribution was wrong.

## Pass 2: it was our own compute buffer

Compute buffers are reserved for the worst-case graph, and this engine set `n_ubatch = n_ctx` so any
fitting prompt prefills in one pass. The reservation therefore scaled with the **context**, not with
the offload:

| n_ctx | OpenCL compute buffer | CPU compute buffer |
|---|---:|---:|
| 2048 | 1203 MiB | 320 MiB |
| 512 | 301 MiB | 80 MiB |

Exactly 4x for a 4x context, on both backends — nothing to do with the GPU. Re-run with the smaller
reservation: **1.207 → 2.752 tok/s, and 5 958 → 1.06 major faults per token.**

The fault storm was ~900 MiB of avoidable reservation pushing an already-tight system into reclaim.
This is why `--ubatch N` now exists, and note it is not a GPU fix: 240 MiB of the CPU path's own
reservation is equally avoidable, and on this engine every MiB reserved is a MiB the expert cache
does not get.

## Pass 3: the honest sweep (n_ctx 2048, `--ubatch 256`)

Five cells, one pass, CPU first **and** last so thermal drift is bounded rather than assumed:

| `--gpu-layers` | tok/s | graph splits | CPU occupancy | majflt/token |
|---|---:|---:|---:|---:|
| 0 (CPU, first) | 2.941 | 1 | 87.9% | 0 |
| 48 (all) | 2.768 | 96 | 54.3% | 1.12 |
| 12 | 2.866 | 24 | 62.1% | 15.36 |
| 1 (output head only) | **3.029** | 2 | 67.9% | 17.15 |
| 0 (CPU, last) | 2.778 | 1 | 90.4% | 0.11 |

Two things to read here. **The GPU really does take work off the CPU** — occupancy falls from 88% to
54% at full offload. And within this pass, throughput orders perfectly and inversely by split count
(96 → 2.768, 24 → 2.866, 2 → 3.029), which is what a boundary-crossing cost looks like.

The two CPU cells bound the drift: 2.941 → 2.778, i.e. **-5.5% over thirteen minutes**, so later
cells are handicapped. Drift-corrected, `--gpu-layers 1` looked like roughly +7%.

## The repeat that killed it

`--gpu-layers 1` against CPU, alternating, same session:

| cell | tok/s |
|---|---:|
| `--gpu-layers 1` | 2.934 |
| CPU | **3.116** |

The opposite ordering, by a similar margin. Across both passes `--gpu-layers 1` reads 3.029 and
2.934 while CPU reads 2.941, 2.778 and 3.116 — the two distributions overlap completely.

**So the +7% was noise.** Run-to-run variation on this device is around ±6%, which is larger than
every GPU effect measured except the full-offload penalty. The one result that survives repetition
is the negative one: full offload (96 splits) is consistently a little slower than CPU.

The repeat was cut short at two cells — the phone entered doze and killed the run, and
`svc power stayon true` did not prevent it. Four more cells are owed, but they would be deciding
between "neutral" and "slightly negative", not resurrecting a win.

## Why full offload loses, mechanically

The dense and expert halves **interleave**: a MoE layer is dense → routed experts → dense → routed
experts, 48 times. Splitting them across two devices therefore crosses the device boundary twice per
layer — 96 splits against 1 — and every crossing copies and synchronises an activation. The freed
CPU time (88% → 54% occupancy) is real; the boundary tax simply eats it.

That is also why `--gpu-layers 1` is the only configuration that even gets close: the output head is
the one dense component with no expert layer after it to hand control back to, so it costs 2 splits
instead of 96. It is still not a win, because one matmul (243 MiB of weights) is too small a share
of the per-token work for its saving to clear the noise.

## What the pre-measurement argument got wrong

The case for this feature was: a MoE uses 100% of its dense weights every token against
`k/n_expert` of its experts, so the dense half is ~half the per-token arithmetic, and decode is
compute-bound. Both premises are still true, and both were insufficient.

The reasoning was about **how much work** the dense half is. It said nothing about **where that work
sits in the graph** — interleaved, so the boundary is crossed constantly. A byte-count argument is
not a placement argument, in the same way a hit-rate curve is not a throughput argument.

## Open, and worth more than another repeat

- **Contiguous layer blocks.** The 96 splits are a property of *this* partition (dense-GPU /
  experts-CPU), not of GPU offload as such. Offloading the first N layers **whole** — experts
  included, resident rather than streamed — would put the boundary in one place: ~2 splits total.
  llama.cpp already expresses this (a per-block regex over `blk.<il>.`), so the pin could be made
  layer-aware rather than global. The cost is that those layers' experts stop streaming and become
  resident: ~363 MiB per layer on this model, RAM the expert cache no longer gets. That trade is
  untested and is the only remaining shape in which GPU offload could plausibly win here.
- **Does a freed CPU make the cache worth more?** Full offload cuts CPU occupancy to 54% and leaves
  decode more I/O-bound than before (compute 0.187 against flash 0.648 s/token). If compute is no
  longer competing, cache and lane levers should buy more with the GPU on than off — i.e. the right
  comparison is not GPU-vs-CPU at a fixed cache but the *cache curve* under each. Untested. Note the
  GPU frees no RAM: the dense weights move from anon buffers to OpenCL buffers, both of which are
  the same physical LPDDR (951 MiB became 726 on GPU + 214 on CPU).
- **Prefill is unmeasured.** It is batched and compute-bound, the regime a GPU actually suits, and
  every number here is decode.
- **Discrete GPUs are a different question entirely.** Dedicated VRAM changes the memory story and
  the transfer story at once; none of this transfers to a desktop.

## Method notes for whoever repeats this

- `--ubatch` must be capped, or the compute-buffer reservation dominates everything else.
- One run per cell is not enough: this device's noise is ±6%, and it swallowed a 7% effect.
- Bracket every sweep with the baseline at both ends. The drift here was -5.5% in 13 minutes.
- The phone dozes and kills detached runs; `svc power stayon true` was not sufficient. Keep the
  screen genuinely awake, or expect to lose the tail of a long sweep.
- adb over Wi-Fi drops repeatedly under this load — run the sweep with `setsid` on the device and
  write results per cell as they complete (`scripts/bench-gpu-sweep.sh`), so a dropped connection
  costs the connection and not the data.
