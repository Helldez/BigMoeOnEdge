# The model file's mapping serialises the streamer's reads (Windows)

2026-08-29. Why the desktop host has always read at about a third of what its drive can serve, why
more I/O lanes and more compute threads measured dead there, and what `--release-mmap` recovers.

Hosts: a Windows x86 laptop (8 cores, 15.5 GB RAM, NVMe SSD) and, for the control, the 12 GB /
UFS 4.x Android test phone. Model in every cell: Qwen3.6-35B-A3B-Q4_K_M (22.3 GB). Read shape in
every iobench cell: 576 KiB per read, O_DIRECT, random offsets — the shape one routed expert
projection actually has.

## The drive, and what changes it

`bmoe-iobench --model M.gguf --lanes 1,4 --slice-kb 576 --seconds 3`, host, one variable per row:

| variable | 1 lane | 4 lanes |
|---|---:|---:|
| nothing (the drive) | 883-967 MiB/s | **2295-2665 MiB/s** |
| `--range-mb 440` (offsets confined to one region) | 917 | 2651 |
| `--compute-load 8` (lanes read against a busy CPU) | — | 2463 |
| `--fresh` (destination pages committed afresh per read) | 745 | 1952 |
| **`--mmap` (a read-only mapping of the file held open)** | 909 | **895-930** |
| `--mmap --reopen-lanes` (mapping dropped, lanes reopened) | — | **2402** |

Locality, CPU contention and page-commit cost move the number by a few per cent. The mapping moves
it by **2.6x**, and four lanes under it deliver exactly one lane's throughput (4 / 2.42 ms = 1.65
reads/ms against 1 / 0.62 = 1.61). Nothing is ever read through the mapping in these cells: only
its existence is the variable.

Two halves, separable and both necessary:

1. While a section of the file is alive, concurrent unbuffered reads on that file are serialised.
2. A lane opened *while* the section existed keeps serialising against it after the section is
   gone — dropping the mapping alone leaves the rate at 927 MiB/s; dropping it and reopening the
   lanes restores 2402.

Sub-cells that placed the blame precisely, all at 4 lanes: mapping opened before the lanes vs after
them (927 either way, so it is not open order); reading through a hard link while the original is
mapped (927, so it is the file, not the handle); mapping the file and unmapping the view but
keeping the section (926) vs closing the section too (2050-2207, recovered); a plain unbuffered
handle held open with no mapping at all (916 — that handle had been opened under a live section
earlier in the process, which is finding 2 again).

## What it costs the engine, and what the flag returns

Host, `--moe-stream --cache-mb 3000 --io-threads 4 --overlap --dense-weights anon -t 8
--ubatch 256 -c 1024`, 96 generated tokens, cells interleaved:

| | baseline | `--release-mmap` |
|---|---:|---:|
| decode | 3.16 tok/s | **4.63 tok/s (+46%)** |
| s/token | 0.316 | 0.216 |
| compute | 0.107 | 0.114 |
| cache mgmt | 0.027 | 0.028 |
| **flash stall** | **0.182** | **0.074** |
| per-read latency p50 (`--io-trace`) | 2.49 ms | 1.20 ms |
| flash read per token | 192.5 MiB | 192.5 MiB |
| cache hit / evictions / re-reads | 60.9% / 11635 / 7848 | identical |
| generated text | — | byte-identical |

Not one byte fewer is read and no quality knob is touched: the same reads are simply served in
parallel. The remaining decode budget is 0.114 compute + 0.028 mgmt + 0.074 stall, so the ceiling
if flash were free is 7.0 tok/s.

Two earlier verdicts are corrected by this. "Lanes and threads are dead on the desktop host"
(2026-07-24) and "the serial path reads at exactly the one-lane rate with four lanes open"
(2026-08-15) were both this mechanism, not a read-path defect. The "at least 1.7x of headroom" from
the same session is now accounted for and collected.

## The phone: flat, so this is a Windows defect

Same tool, same model, same read shape, on the Android test phone via adb:

| | 1 lane | 2 lanes | 4 lanes |
|---|---:|---:|---:|
| no mapping | 967 MiB/s | 1785 | 2180 |
| mapping held open | 1122 | 1890 | 2200 |

No serialisation: f2fs does not take an exclusive lock against a live mapping, which is also why
lanes have always scaled in the engine there (2 to 4 lanes was worth +15-20%). The engine's own
per-read latency on the phone is 1.24 ms against the drive's 1.03 at 4 lanes, i.e. it already reads
at about 80% of the drive's rate with the compute running alongside.

So the read-path prediction for the phone is "no gain", and the engine confirms it: flash stall is
the same with the flag and without. The flag is still faster there, for another reason. Same model
and device, `--cache-mb 2000 --io-threads 4 --overlap --dense-weights anon -t 4 --ubatch 512
-c 1024`, 48 tokens, two cells per variant run in both orders with a thermal gate at 45 C:

| | baseline | `--release-mmap` |
|---|---:|---:|
| decode | 3.92, 3.56 tok/s | **4.14, 4.03 tok/s** |
| **flash stall** | 0.088, 0.087 | **0.089, 0.093** |
| CPU per token | 0.634, 0.688 cpu-s | **0.592, 0.604 cpu-s** |
| major faults per token | 70.1, 0.85 | 0.00, 0.21 |
| prefill | 7.1, 8.7 tok/s | 8.4, 7.3 tok/s |
| model load | 34.7, 24.6 s | 25.8, 26.8 s |
| bytes, hit rate, evictions, re-reads | — | identical to the digit |

Stall is flat, so this is not the Windows effect; CPU per token falls ~9% and decode follows.
Keeping a 20 GB mapping registered costs a kernel already under memory pressure, and handing it
back removes that cost from the decode's own CPU time. The 70 major faults of the first cell are a
cold-start artefact rather than the explanation — the second baseline had 0.85 and was still the
slowest cell. Prefill and model load are unaffected, which is expected: prefill on this device is
compute-bound and never waits on flash, and unmapping 20 GB costs nothing measurable.

Read this as a direction, not a number. Two cells per variant, 48 tokens each, against a device
whose prefill alone spread 20% (7.1 to 8.7 tok/s) across cells that differ in nothing. What holds
it up is that the ordering is consistent in both directions and that CPU per token moves with it.
A 256-token protocol is owed before any of this becomes a published figure.

Owed: the same cells on macOS, which #182 has just made worth running — a direct request there now
applies `F_NOCACHE` to the descriptor instead of falling back to a buffered read. `F_NOCACHE` is a
caching hint rather than an I/O mode, so whether a live mapping of the same file interacts with it
at all is an open question, and the answer is one `bmoe-iobench --mmap` cell on a Mac.

## What the gates caught: a tied output head keeps a pointer nobody can see

Turning the flag on by default failed the `moe_gates_gemma4` byte-identity gate with a segfault, and
the reason is worth recording because it bounds what this feature can ever promise.

A bisect of the release (unmap the view but keep the section; close the section but keep the view)
put the fault on **unmapping**, so something still dereferenced the mapping. A vectored exception
handler reported the faulting addresses as file offsets 12192 and 79264, both inside
`token_embd.weight`. But the accounting said every gguf tensor was owned by a policy, and it was
right: the tiny gemma4 model has **no `output.weight`**, so llama.cpp builds the output head from
the embedding table and the model carries a *second* tensor over the same mapped bytes. The engine
rebinds the tensor the gguf names; the twin is not a gguf tensor, so no name-based accounting can
ever see it. The qwen3moe model, which has a separate `output.weight`, passes.

The first fix was a blacklist — decline when the gguf has no `output.weight` — and it was the wrong
shape of answer. The twin is not a special case to route around, it is a weight the engine failed to
rebind: under `--dense-weights anon` the output projection was being served by page faults from the
mmap on every tied model, which is exactly what that policy exists to prevent. So the capture now
records every distinct leaf object rather than one per name, the dense policy rebinds every tensor
over a file range onto the same buffer, and the release decision became a question asked of the OS:
does any weight the capture observed still point inside the mapping? With that, gemma4 releases
cleanly, `--dense-weights mmap` reports 39 weights still mapped and stands down, and every gate
passes.

What is left is a residue, and it is why the flag is opt-in: the check answers for pointers the
capture pass observed, and a graph shape the session never builds could hold another.

Two alternatives were measured and refuted while looking for a design that could not fail this way:

- **Close the section handle and keep the view mapped.** Would be safe by construction — no pointer
  can dangle — but it does not lift the serialisation: 792 MiB/s against 790 with the mapping fully
  alive. It is the view, not the handle.
- **Read buffered instead of unbuffered while the mapping is alive.** Buffered reads do escape the
  serialisation, but the microbenchmark cannot price it honestly: `--buffered --mmap` measures
  3406 MiB/s precisely because it is being served from the page cache the mapping populated, which
  is the pollution O_DIRECT exists to avoid on a model many times larger than RAM. Answering it
  properly needs an engine-level A/B on Windows, not an iobench cell.

## Two things that are NOT this, checked because they looked like it

**The expert-cache stall accounting is not a hot-path cost.** `StallUnion` (0.23.0) takes a
process-wide mutex around each stall interval, which reads like a serialization point on the
compute path. It is not: the readiness fast path returns before `enter()`, so the mutex is taken
only when a thread is about to spin and possibly sleep anyway. Measured on the host with a twin
binary whose `StallUnion::enter/exit` are no-ops, in the configuration with the *most* stall
(baseline, no release), interleaved 96-token cells: **3.281 / 3.272 tok/s with, 3.286 / 3.289 /
3.290 without** — 0.4%, inside the run-to-run spread, and the direction is not even consistent
with the cold first cell (3.103) discarded. No regression.

**A second engine on the same drive invalidates everything.** Two of these cells were first
measured while another process was streaming the same model on the same disk; the drive's own
curve read half its real value and the lane counts looked flat for the wrong reason. Every number
above was re-taken with the machine otherwise idle. The rule stands: one engine at a time, and
check before trusting a rate.
