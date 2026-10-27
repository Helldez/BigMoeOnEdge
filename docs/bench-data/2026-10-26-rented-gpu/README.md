# 2026-10-26 — rented desktop GPUs: the measured-planning stack on a discrete card

The day 0.37.0 and 0.37.1 shipped, the planner stack (#388-#403, #415, #416) ran on rented desktop
GPUs: a 24 GB card for the regimes the stack was written for, and a 12 GB card for the class most
people own. Every engine run is `--auto` and nothing else (plus `--routing-dir`, which keeps the
routing record between loads as `bmoe-server` does, `--plan-explain` and `--csv`), 256 greedy
tokens from the README's prompt. The CSVs are one row per token plus the `# summary` trailer
([telemetry.md](../../telemetry.md)); the `.plan` files are what `--auto` printed.

Two things this session found are in the code and the tracker: a crash on every DeepSeek V4 Flash
file (#416, fixed in 0.37.1) and two planner rules that lose to a simpler offload where the model
fits in RAM (#418, #419).

## Machines

| | 24 GB card | 12 GB card |
|---|---|---|
| GPU | RTX 4090 class, 24 GB | RTX 3060 class, 12 GB |
| cores the process may use | 31 (a quota of 30.7 CPUs on a 32-core host) | 7.7 (quota, on a 16-core host) |
| memory control group | 89 GiB | 42.6 GiB |
| drive, 512 KiB direct reads | NVMe, 1.0 GB/s | NVMe |
| CUDA | 12.8 toolkit, driver 595 | 12.8 toolkit, driver 615 |
| engine | 0.37.0 tag, then #416 | #420 (0.37.1 + `--mtp-head`) |
| host | 93 GB, the whole file in its page cache after a download | 64 GB |

Both are containers on a rented host: the memory limit is the control group's, not the host's, and
the first box's page cache held every file it had just downloaded, which matters below.

## 24 GB card

### Resident on the card: Qwen3.6-35B-A3B Q4_K_M

The capacity fitter placed every layer on the card. **217.3 and 217.4 tok/s** over two loads
(prefill 256-289 tok/s on the 30-token prompt, load 20 s then 3 s). On 2026-10-25 the same card
decoded this file at 184 tok/s on the integration branch; colibri's CUDA path on the same class of
card measured 34-48 that day.

### Experts in RAM, the hot ones on the card: Qwen3.8-Flash-Next 125B GSQ-RCO Q2_0

The file is 66 GB but its RAM need is 32.4 GB of experts (the 28.8 GB second shard is the
row-gathered embedding), so the regime is `fits` and the plan armed llama.cpp's device-side expert
cache on the first load, `[measured]`: 18225 MiB of the card for the experts used most, the rest
in RAM. **69.7 tok/s** on the first load (31 s), **70.7** on the second from the routing record
(load 3 s); the server answered a `/v1/messages` turn. Files: `flash-1.*`, `flash-2.*`.

Strata on this regime and card measured 150-177 tok/s on 2026-10-25, with its multi-token
prediction, which it does not run without; our head for this file is `--mtp-head` (#420), measured
below on the 12 GB card.

### Streamed from the drive: DeepSeek V4 Flash 0731 UD-IQ2_M

85 GB of experts against 72.7 GB the plan allowed itself. On 0.37.0 both loads died in the
real-layer probe (`exit 139`): the file's `ffn_gate_tid2eid.weight` is an `I32` table the probe
priced as a matmul, #416. With the fix:

| | load | regime | device cache | host cache | hits (decode) | decode |
|---|---:|---|---:|---:|---:|---:|
| first load (`deepseek-1`) | 44 s | experts-stream | 13020 MiB | 66034 MiB | 93.9% | **9.33 tok/s** |
| second, from the record (`deepseek-2`) | 48 s | experts-stream | 13020 MiB | 34914 MiB | 92.3% | 7.95 tok/s |
| cold page cache (`deepseek-cold`) | 82 s | experts-stream | 13020 MiB | 66220 MiB | 93.9% | 8.65 tok/s |

The second load sized half the cache because the control group counted the first load's page
cache as taken (51.7 GB available against 90.6): #418. The reference row for this file is
jordimarsal's, two 16 GB cards and the memlock limit raised: 8.72 tok/s (#321).

### The same file, llama.cpp's own offload

`llama-completion -ngl 99 --n-cpu-moe 43 -t 16`, dense parts on the card, every expert on the
cores, the file memory-mapped, from the same cold page cache: load 82 s, **18.5 tok/s**
(`llama-cpp-deepseek-cold.txt`; 18.4 warm). Twice our streamed run on the same bytes, because the
whole 85 GB file fits the control group's page cache: after the load llama.cpp decodes with every
expert in RAM, while our plan kept an 18 GB margin, streamed, and paid a full NVMe read for each of
the 6% of misses (~112 MiB a token, the whole 0.116 s measured). That is #419: the margin is a
fixed share, not what the run allocates, and a set that fits should run resident.

`deepseek-warm-no-device-cache.*` is the worst case of #418, not a measurement of the device tier:
run right after llama.cpp had read the file through the page cache, the plan saw 4256 MiB of 95254
available, sized a 1.8 GB cache and decoded at 2.6 tok/s. (`--n-cpu-moe 33`, ten expert layers
on the card, runs out of memory on this card.)

## 12 GB card (`3060/`)

Qwen3.8-Flash-Next Q2_0 on a 42.6 GiB control group with 7.7 usable cores, the class Strata
quotes its 94 tok/s on (a 5070 12 GB). Engine #420 (0.37.1 with `--mtp-head`), cold page cache
before every run.

| run | what the plan chose | decode |
|---|---|---:|
| `--auto` (`flash-auto-oom`) | experts-stream, 25.8 GB host cache + 6 GB device cache | **killed** at token 224 (3.5 tok/s before): the process reached 43 GB, the ledger said 25.9, #424 |
| `--auto --device-cache-mb 0` (`flash-no-device-tier`) | `fits`: the fitter put 6 GB of experts on the card, 26.4 GB resident in RAM | **15.5 tok/s**, load 135 s |
| the same with `--mtp --mtp-head` (`flash-no-device-tier-mtp-head`) | as above, the Q8_0 head drafting on the cores | 15.2 reported per verify decode, **9.9 effective**: 164/273 drafts accepted (60%), 2.78 tokens per verify, drafting 35 ms a token on these cores |

Two findings. The device tier's host-side memory is not in the ledger and kills the run on a
control group the file does not fit (#424): the second row is a diagnostic with that knob pinned,
not the plan. And a head drafting on a weak host costs more than it saves: on the 24 GB class with
31 cores the same head paid (45 against 31-34 tok/s on 2026-10-25); here 8 slow cores spend 35 ms a
token drafting against a 65 ms decode. The head belongs on the card where there is room for it,
which there is not on a 12 GB card beside 6 GB of experts.

The gap to Strata's figure on this class is a factor of six, and it is the regime: Strata keeps
the hot experts on the card with its own kernels and its drafting on the card; our plan here could
not keep a device cache alive and drafted on the cores.

## The same card through Vulkan (`deepseek-cuda-vulkan.*`)

A build with the CUDA and Vulkan modules together (the Vulkan SDK from LunarG, the host's NVIDIA
ICD), DeepSeek from a cold page cache. Both backends registered the card and both were probed on
the real layer: CUDA0 655.6 GiB/s on the experts and 697.2 on the dense matrices, crossing 9 us;
Vulkan0 418.6 and 503.6, crossing 20 us. The run decoded at **8.45 tok/s** (8.65 with CUDA alone),
and llama.cpp ran it on CUDA0, which it keeps for a card it recognises by PCI id.

What the plan got wrong is in #421: the Vulkan device reported no PCI id through this ICD (ggml
fills it only where `VK_EXT_pci_bus_info` is exposed), so #400's grouping never fired, no
`device-list` decision was made, and the device tier priced the cache on the slower of the two
"devices", Vulkan0. Same card, two numbers, the wrong one quoted.

## Not measured, and why

- Strata on these boxes (measured on 2026-10-25 on the 24 GB class, see above).
- colibri on the 125B models: a per-model engine with no path for them as far as known.
- DeepSeek V4 Flash with speculation: its draft is DSpark (`markov_head`, `confidence_head`), a
  draft model of its own that llama.cpp converts with `--dspark`, not a `nextn` head; `--mtp-head`
  does not apply, and a DSpark draft source is a feature not yet written.
