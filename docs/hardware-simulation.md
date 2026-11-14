# Hardware simulation: planning for a machine nobody has at hand

`bmoe-planlab simulate` makes the plan `--auto` would make on a machine that is not in front of
you: two 16 GB cards, an 8 GB laptop part with 32 GB of RAM, a card nobody on the project owns. It
is the step after [plan records](plan-records.md): a record replays a machine that was measured; a
simulation composes one out of measured parts and asks the real capacity fitter to place a real
model on it.

It is a developer's instrument, built with `-DBMOE_BUILD_TOOLS=ON` into `bmoe-planlab` and into
nothing a release carries.

## What is real and what is not

| Part | Where it comes from | How far to trust it |
| --- | --- | --- |
| The rules (`core/src/plan`) | the build at hand | exactly the plan `--auto` would make on those facts |
| The placement (layers per card, experts left on the host, KV, compute buffers) | llama.cpp's own capacity fitter, run on simulated cards that report the memory the spec gives them | see [the check against real fits](#the-check-against-real-fits) |
| The model's shapes | the real gguf's header: a *shell* of the file (its header at its own offsets, the rest a hole) | exact |
| The host: memory, cores, thread sweep, matmul rates, storage | its parts' specifications through the model of parts fitted on every measured machine ([any machine from its parts](#any-machine-from-its-parts)); or, composed, a record measured on a real machine | memory-bound quantizations within 0.82-1.11 of a host left out of the fit; under 4 bits a weight, 0.5-3.8 (see below) |
| A card's rates: memory bandwidth, expert and dense matmul, uploads, a crossing | the same model of parts from its published bandwidth, FP32 and link; or a record of a real card | within 0.81-1.22 of a card left out of the fit |
| The cache's hit rate | the model's own routing from a record of it (`routing=`), or hit rates real runs measured at given cache sizes (`hitcurve=`) | a replay of the model's routing is the planner's own source; measured points are a second best; with neither the plan credits the share of the experts the cache holds, which is far below what a cache of a skewed router serves |
| The decode speed | a sum of the measured rates over the parts a token goes through ([sim_speed.h](../tools/planlab/sim_speed.h)) | see [the speed estimate against measured runs](#the-speed-estimate-against-measured-runs) |

What it cannot tell: whether the driver crashes, whether the kernel kills the process, whether the
load-time calibration (0.43) keeps what the plan nominated, what a card's thermals do over a long
answer. Those are the machine's to answer. A simulation removes the trips to a machine that were only
made to learn what the planner would decide, and gives every other trip a prediction to be checked
against.

## Any machine from its parts

A machine is written as what it is made of, and nothing else:

```
parts=parts.txt
ram=32G                      # available=29G: what the engine may hold (default: ram less 3 GB)
cpu=ryzen7-7700              # or cpu=custom cores=16 ghz=4.8 membw=83.2 core=zen4 simd=avx512
storage=3.5                  # GB/s, the drive's sequential read
gpu=rtx4060 slot=pcie4x8     # one line per card, any number; or gpu=custom vram=8G bw=272 fp32=15.1 link=pcie4x8
```

[parts.txt](bench-data/hardware-sim/parts.txt) is data: published specifications under short names,
and one `rig` line per measured machine saying which parts it had. Every simulation fits the model of
parts on the rigs' records first (`bmoe-planlab parts parts.txt` prints the fit), so a record added
to the data set, with its rig line, moves every simulation with no code changed. The model:

| Rate | Model | Fit on the measured machines |
| --- | --- | --- |
| A card's expert and dense matmul | time a layer = its bytes / (efficiency x published bandwidth) + a latency, at the simulated model's own layer size | experts 0.60 of the bandwidth and 3 us; dense 0.85 and 37 us |
| A card's uploads | efficiency x the link (the slower of the card's and the slot's) | 0.74 from the backend's host memory, 0.45 from the file's mapping |
| A crossing, a host hop | the measured median | 11 us a split |
| The host on 4 bits a weight or more | 0.80 x the memory's published bandwidth: every host measured on Q4 and Q8 sat at 0.73-0.88 of it | |
| The host under 4 bits a weight | threads x one thread's rate: the cores dequantize slower than the memory feeds them (2-bit plateaus at 0.24-0.46 of the memory) | one thread's rate per GHz for that core class and quantization where measured; else the median of the hosts measured under 4 bits with the same SIMD |
| The host's experts, dense, wide matmul | ratios of the plateau | 0.78, 0.82 |
| Storage | a measured drive's curve by request size and lanes, scaled to the drive's sequential rate | |

How far it misses a machine it was not fitted on (each record against the model fitted without its
machine, `bmoe-planlab parts`):

| Left out | Host plateau | Host experts | Card experts | Card dense | Upload pinned / pageable |
| --- | ---: | ---: | ---: | ---: | ---: |
| 8 GB card, 14-core Xeon (Q2_0) | 0.86 | 0.86 | 1.22 | 0.90 | 0.93 / 1.05 |
| 24 GB card, 8-core desktop (Q4_K) | 1.11 | 1.07 | 0.86 | 1.09 | 0.98 / 1.07 |
| 12 GB card, 28-core Xeon (IQ2) | 0.50 | 0.66 | 0.81 | 1.12 | 1.07 / 1.21 |
| 32 GB card, Zen 2 EPYC (Q2_0) | 3.77 | 3.21 | 0.85 | 0.97 | 0.95 / 0.64 |
| 32 GB card, Zen 5 EPYC (Q2_0) | 2.26 | 2.44 | 0.90 | 1.00 | 1.10 / 0.89 |
| Apple silicon laptop, Q4_0 / Q8_0 / Q4_K | 0.82-0.96 | 0.84-1.11 | | | |
| Apple silicon laptop, Q2_K | 2.40 | 2.41 | | | |

The cards and a memory-bound host carry over to parts nobody measured. A host on a quantization
under 4 bits does not yet: how fast one core dequantizes 2 bits differs by generation (0.7 GiB/s a
GHz on a 2016 Xeon, 1.4 on Zen 2, 2.7 on Zen 5, 3.5 on Apple silicon), and six processors are too
few to predict a seventh. Until more are measured, a simulated host on a 2-bit model is a range of
-50%..+100% (the plan says so), or one thread's measured rate can be given (`cpu=... thread=GiB/s`).

The other way to describe a machine, `host=RECORD` and `device=RECORD memory=... bandwidth=...`,
composes it from measured machines directly; the machines rebuilt from their own records
(`self-*.machine`) use it.

## How it works

1. **A machine spec** (`.machine`, one `key=value` a line) gives the machine's parts (`ram=`, `cpu=`,
   `storage=`, `gpu=` lines, [above](#any-machine-from-its-parts)), or composes it from records
   (`host=RECORD`, `device=RECORD[#INDEX] memory=16G bandwidth=0.479 compute=0.54 link=1`); and the
   routing or hit rates of the model, and any fact to change (`set=hw.memory_total=...`, any key of a
   record).
2. **The simulated cards** are ggml backend devices registered by the tool
   ([sim_device.h](../tools/planlab/sim_device.h)). They report the spec's memory, give a quantized
   tensor the row padding and alignment a CUDA buffer gives it, and take ops off the host from the
   same batch size CUDA does; everything else is the CPU's. They never compute: a plan never runs a
   graph on them. They implement ggml's backend interface (`ggml-backend-impl.h`), which is not
   llama.cpp's public API: acceptable for a developer's tool, and a llama.cpp bump that moves the
   interface breaks this file, never the engine.
3. **The model** is a shell made by [gguf_shell.py](../tools/planlab/gguf_shell.py): its header
   fetched with HTTP range requests into a sparse file of the model's full size. A 125 GB model costs
   about 11 MB of download and of disk. Never load a shell to run it: its weights are zeros.
4. **The plan** is the rules on the composed facts, through the same `replan()` a calibration
   plans with; every re-fit the rules ask for (a narrower graph, a context giving way) asks the
   real fitter again on the simulated cards.
5. **The result** is a plan record marked `env.simulated=true`, so `replay`, `--sweep`, `--set`,
   `table` and the rest read it like any other, and the decode estimate printed beside the cost
   model's own prediction.

```bash
# the shells (once per model)
python3 tools/planlab/gguf_shell.py URL [URL...] -o shells/flashnext-q2_0
# a build for simulating: no BLAS, no Metal (see "Build" below)
cmake -S . -B build-sim -G Ninja -DBMOE_BUILD_TOOLS=ON -DGGML_BLAS=OFF -DGGML_METAL=OFF && ninja -C build-sim bmoe-planlab
# the model of parts, and how far it misses each measured machine left out of it
build-sim/bin/bmoe-planlab parts docs/bench-data/hardware-sim/parts.txt
# a machine
build-sim/bin/bmoe-planlab simulate docs/bench-data/hardware-sim/two-16gb-62gb-flashnext.machine \
    -m shells/flashnext-q2_0/Qwen3.8-Flash-Next-GSQ-RCO-Q2_0-00001-of-00002.gguf -o two-16gb.planrec
# and what-ifs on it, with no fitter
build-sim/bin/bmoe-planlab replay two-16gb.planrec --sweep cache-mb=8000,16000,32000
# the speed estimate against every measured run
build-sim/bin/bmoe-planlab speed docs/bench-data/plan-records
```

### Build

Simulate from a build without BLAS and without Metal (`-DGGML_BLAS=OFF -DGGML_METAL=OFF`). llama.cpp
creates a backend for every accelerator device it finds (BLAS is one) beside the CPU's, and each
reserves a host compute buffer of its own: on a macOS build the fitter then projected exactly twice
the host compute a CUDA load reports. A Linux build without BLAS needs nothing.

## The check against real fits

Every rented machine with a record is rebuilt from its own record (`self-*.machine` in
[bench-data/hardware-sim](bench-data/hardware-sim/)) and the simulated fit is compared with the one
the real card made:

| Machine, model | Plan | Placement against the real fit |
| --- | --- | --- |
| 8 GB card, Flash-Next 125B Q2_0 | same, device cache 2053 MiB against 2256 | 1024 tokens wide: identical to the byte. 2048 wide: card +203 MiB, card compute −25 MiB, one tensor group of one layer placed differently |
| 24 GB card, Qwen3.6-35B UD-Q4_K_M | identical | card −128 MiB (compute buffers) |
| 32 GB card, Flash-Next Q2_0 (PCIe 5 and PCIe 4) | identical but the device cache, +55 MiB | card −55 MiB (compute buffers) |
| 12 GB card, DeepSeek V4.1 UD-IQ2_XXS | 23 layers on the card against 33 | host compute +2.6 GB, see below |

The cards' compute buffers come out 25-130 MiB smaller than CUDA's, which is what a CUDA build adds
to some ops' buffers (flash attention's own workspace among them): a plan made here may leave that
much less room than the real one, and so arm a slightly larger device cache.

DeepSeek V4 is the case it gets wrong. llama.cpp turns a fused op off where its node lands on
another device than its layer; on the simulated cards that happens at the boundary between the
card's layers and the host's, on the real card it did not, and the unfused path projects gigabytes
more host compute, so the fitter keeps fewer layers on the card. The simulator counts the fused ops
the fit turned off and says so with the plan: where it does, the real card places more layers than
the simulation shows.

## The speed estimate against measured runs

The cost model predicts a token's time only where the experts stream. Where llama.cpp's placement is
the whole plan (a card holds the layers, a device cache holds experts) it predicts nothing, which is
most of what a rented card runs. The tool's estimate covers both; against every record with a run:

| Record (machine, model) | Estimate ms | Measured ms | Measured / estimate |
| --- | ---: | ---: | ---: |
| 24 GB card, Qwen3.6-35B UD-Q4_K_M, whole on the card | 5.5 | 6.1 | 1.11 |
| 32 GB card (PCIe 5), Flash-Next Q2_0, device cache | 10.5 | 9.9 | 0.94 |
| 32 GB card (PCIe 4), Flash-Next Q2_0, device cache | 9.8 | 10.1 | 1.02 |
| 8 GB card, Flash-Next Q2_0, device cache: no routing / measured hit rates | 193.8 / 103.6 | 71.0 | 0.37 / 0.69 |
| 12 GB card, DeepSeek V4.1 UD-IQ2_XXS, streamed | 460.9 | 744.2 | 1.61 |
| Two 16 GB cards (5080 class), DeepSeek V4 UD-IQ2_M: the #321 report, measured on 0.33.3 without a device cache, simulated from its parts on 0.43's plan | 70.7 | 114.7 | 1.62 |
| 16 GB Apple silicon, four models cold and learned (streamed) | | | 0.47-2.18, median 1.29 |

Where a card holds the model's dense part and its experts come from the card or a device cache,
the estimate is within 11% of the run. The device cache's hit rate is the term everything else
waits on: without the model's routing the plan credits the share of the experts the cache holds,
which on the 8 GB card was 6% where runs of the model on 8 GB cards counted 61%, and the estimate
was 2.7 times too slow; with those measured hit rates (some of them from that class of card, so not
an independent check) it is 1.46 times too slow, the rest being
the upload rate the probe measured (3.3 GiB/s from the mapping). Streamed from flash, it errs both
ways by up to a factor of two, as the cost model it borrows that part from does. Both DeepSeek
machines decoded 1.6 times slower than the sum of their rates: a cost of the streamed path the
estimate does not see yet.

Read a simulated speed as a range: measured / estimated has run from 0.69 to 1.62 on the cards
above, so a simulated 10 tok/s is 6-14 tok/s on the machine, before the parts' own spread.

## The two machines of the 0.43 validation

Described by their parts only ([bench-data/hardware-sim](bench-data/hardware-sim/)): an 8 GB card
(4060 or 3070) with 32 GB of RAM on an 8-core desktop with DDR5, and two 16 GB A4000-class cards with
62 GB on a 16-core desktop, both with a 3.5 GB/s NVMe. Flash-Next and DeepSeek priced with hit rates
measured on rented runs (`hitcurve=`), Qwen3.6 with its own routing replayed. Engine 0.43.0's rules,
`--auto` with no flag. The range is the estimate over the measured / estimated span above.

| Machine, model | Plan | Cost model | Estimate (range) | Measured elsewhere |
| --- | --- | ---: | ---: | --- |
| 8 GB (4060), 32 GB, Flash-Next 125B Q2_0 | streamed; dense on the card, 2917 MiB device cache (64%), 26 GB RAM cache | 23.2 | 15.8 (10-23) | 8 GB desktop card on a 2016 Xeon: 13-14 (0.41.2), Strata 28-32; 8 GB laptop card: 10.9-14.5 (0.38), Strata 38-61 |
| 8 GB (3070), 32 GB, Flash-Next 125B Q2_0 | the same plan | 26.8 | 25.4 (16-37) | |
| 8 GB (4060), 32 GB, Qwen3.6-35B Q4_K_M | fits; 42 layers on the card, a 4795 MiB device cache (85%) over experts in RAM | - | 39.6 (24-57) | 8 GB desktop card, 2.8-3.8 GB device cache: 19-33 |
| 8 GB (3070), 32 GB, Qwen3.6-35B Q4_K_M | the same plan | - | 67.5 (42-98) | |
| Two 16 GB (A4000), 62 GB, DeepSeek V4 Flash UD-IQ2_M | streamed; 44 layers split 7 + 37, 18675 MiB device cache (80%), 52 GB RAM cache | 9.2 | 9.3 (6-13) | two 16 GB 5080s, 60 GB (#321, 0.33.3): 8.72; colibri publishes 1.5-1.6 on one 16 GB card with 32 GB |
| Two 16 GB (A4000), 62 GB, Flash-Next 125B Q2_0 | fits; 49 layers split 20 + 29, 24984 MiB device cache (96%) | - | 63.9 (39-93) | one A4000 with 23 GB: 10-29; one 16 GB card with 32 GB: 19-21, Strata 50-66 |

The host is the widest part of these ranges for Flash-Next and DeepSeek, both under 4 bits a weight:
a desktop Zen 4 or Zen 5 was never measured on them (the nearest is a Zen 5 server).

What a simulation points at before anyone rents the machine:

- **On an 8 GB card the device cache's uploads are most of the token.** Computing part of the
  misses on the CPU while the card computes the rest (`--device-cache-cpu-share`) is what the
  estimate says is worth most there, and the plan does not weigh it on a streamed model ("not
  gated"): the first thing to time on that machine (`replay ... --sweep device-cache-cpu-share=...`).
- **DeepSeek V4 on two equal cards is split 7 + 37 layers.** The device cache is divided among the
  cards as the layers are, so the first card holds a sixth of it. Part of that may be the fused-op
  difference above (the simulated fit turned one off): worth reading the real fitter's split first.
- **Every plan streams or fits with the card's dense part whole on the card**, and none uses a
  prefill device: the device cache keeps the layers where they are.

The competitors' numbers are theirs or ours from other machines ([comparison.md](comparison.md)),
not simulated: what Strata or colibri would do on these exact machines is not something a
simulation of our planner can say.
