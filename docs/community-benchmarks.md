# Community benchmarks

Every number in the README was measured on one phone (12 GB RAM, UFS 4.x) and one Windows laptop.
The engine builds and runs unmodified on any Linux or macOS machine with a fast drive, and the
question this page exists to answer is simple: **what does expert streaming do on hardware we do
not own?** NVMe boards, mini PCs, other phones, Apple silicon, ARM SBCs with a PCIe slot.

If you have one of those and ten minutes, run the protocol below and open a
[benchmark report](https://github.com/Helldez/BigMoeOnEdge/issues/new?template=benchmark-report.yml).
Accepted rows land in the table with your GitHub handle. You do not need to know the code.

## Hardware we want to see

In rough priority order, because each one answers a different question:

| Class | Examples | What it tells us |
|---|---|---|
| NVMe mini PC, 16 GB | Intel N100/N150, Ryzen 3500U/5560U with a PCIe 3.0 x4 drive | Does a real NVMe queue remove the flash stall the phone shows? |
| NVMe mini PC, 32-64 GB DDR5 | Ryzen 7840HS/8845HS, Core Ultra | Where the >RAM regime starts on a fast CPU |
| ARM SBC with PCIe | RK3588 boards, Qualcomm Dragonwing boards, Raspberry Pi 5 (x1 lane, expected slow) | ARM Linux with a drive faster than UFS |
| Unified-memory desktops | Ryzen AI Max (Strix Halo) 64/128 GB, Mac mini/Studio | The far end: the 284B DeepSeek past 128 GB |
| Other phones | Snapdragon 8 Gen 2/3/Elite, Dimensity 9x00, Tensor | Same class as the reference device; does the number hold? |

## The protocol

One run, fixed settings, the same prompt as every README table, 256 greedy tokens. The settings
are the ones that win on the reference phone (`docs/benchmark-method.md`), not tuned per machine,
so rows are comparable across hardware.

```bash
git clone --recursive https://github.com/Helldez/BigMoeOnEdge.git
cd BigMoeOnEdge
scripts/bench-report.sh /path/to/any-moe-model.gguf
```

No toolchain? Each release attaches a prebuilt `bmoe-cli` for Linux x86_64 / aarch64, macOS arm64
and Windows x86_64 (`bmoe-cli-<tag>-<target>.tar.gz` / `.zip`). Unpack it and point the script at
it instead of building:

```bash
BMOE_CLI=/path/to/bmoe-cli scripts/bench-report.sh /path/to/any-moe-model.gguf
```

The x86_64 binaries assume AVX2 and the aarch64 one armv8.2-a with dotprod; if the binary does not
start on your CPU, drop `BMOE_CLI=` and the script builds from source.

The script builds the CLI if needed, records CPU / RAM / drive, measures the drive's O_DIRECT
read rate at 512 KiB requests **from the model file itself** (the request size the expert stream
issues), runs the protocol and prints two markdown tables ready to paste. Defaults:
`min(8, cores)` threads, 4 read lanes, auto-sized cache, `--overlap`, `--dense-weights anon`.
Override with `THREADS=`, `IO_THREADS=`, `CACHE_MB=`, `N_PREDICT=`; extra flags after the model
path go to `bmoe-cli` verbatim.

**Any MoE the engine supports, in any quantization, is a valid row.** `bmoe-cli --list-archs`
prints the architectures; the quant is whatever gguf you have, and the row records it. A different
quant of the same model is its own row: the expert bytes per token change with the quant, so the
flash column does too, and that comparison is one of the things the table is for.

The models the app catalog ships, if you want a row that lines up with the README directly:

| Model | Catalog quant | Size | Download | Notes |
|---|---|---:|---|---|
| Qwen3.6-35B-A3B | Q4_K_M | 22.3 GB | [bartowski/Qwen_Qwen3.6-35B-A3B-GGUF](https://huggingface.co/bartowski/Qwen_Qwen3.6-35B-A3B-GGUF) | the README's reference row |
| Qwen3-30B-A3B | Q4_K_M | 18.5 GB | [unsloth/Qwen3-30B-A3B-GGUF](https://huggingface.co/unsloth/Qwen3-30B-A3B-GGUF) | |
| Gemma-4-26B-A4B-it | Q4_K_M | 17.0 GB | [bartowski/google_gemma-4-26B-A4B-it-GGUF](https://huggingface.co/bartowski/google_gemma-4-26B-A4B-it-GGUF) | |
| gpt-oss-120b | Q4_K_M | 62.8 GB | [unsloth/gpt-oss-120b-GGUF](https://huggingface.co/unsloth/gpt-oss-120b-GGUF) | add `--no-think`; pass the first shard |
| DeepSeek V4 Flash 0731 | UD-IQ2_M | 90.9 GB | [unsloth/DeepSeek-V4-Flash-0731-GGUF](https://huggingface.co/unsloth/DeepSeek-V4-Flash-0731-GGUF) | pass the first shard |
| Qwen3.8-Flash-Next | UD-IQ3_XXS | 82.0 GB | [unsloth/Qwen3.8-Flash-Next-GGUF](https://huggingface.co/unsloth/Qwen3.8-Flash-Next-GGUF) | needs engine 0.22.0+; pass the first shard |

A model **smaller than your RAM** is still a valid row: it shows the streaming overhead against
plain llama.cpp on that machine. Say which regime you are in; the script prints model size and RAM
side by side.

### What makes a row comparable

The script fills these in from the engine's own telemetry (`docs/telemetry.md`); a bare tok/s
without them cannot be placed in the table.

| Column | Meaning | Why it matters |
|---|---|---|
| Decode tok/s, Prefill tok/s | generation and prompt throughput | the headline, and the two are limited by different things |
| Stall s/tok | time per token the compute waited for flash | the flash bottleneck, isolated; the number NVMe should shrink |
| Compute s/tok | time per token in the expert math and cache management | the CPU bottleneck, isolated |
| Flash/token | MiB read from the drive per generated token | whether the cache is working |
| Cache hit | share of expert reads served from RAM | same, from the other side |
| majflt/tok | major page faults per token | non-zero means the dense weights were being evicted mid-run: the OS, not the engine, is in charge |
| Storage read, 512 KiB O_DIRECT | the drive's raw rate at the engine's request size | the ceiling; stall well above `Flash/token ÷ rate` means a request-size or queue problem, not bandwidth |

On a phone, the app's telemetry panel and the CSV from `scripts/bench-run.sh` carry the same
fields.

### Doing it honestly

- Nothing else heavy running. A browser with fifty tabs is heavy.
- Passive-cooled boards throttle: run it twice, report the second, and say if the two differ.
- If the storage probe reads far below the drive's rating, say so; a DRAM-less SSD or a PCIe x1
  slot is a result in itself, and often the most useful one.
- The first run after download has the file's head in the page cache. The probe skips a quarter
  into the file for that reason; the engine run uses O_DIRECT and is not affected.

## Results

Rows are ordered by hardware class. "maintainer" rows are the ones already in the README, measured
with the same protocol over `adb` on the reference phone and on the Windows laptop. Phone rows
name the device class, not the model, on purpose.

| Hardware | Storage (512 KiB rate) | Model, quant | k | Cache | Decode tok/s | Prefill tok/s | Stall s/tok | Flash/token | Cache hit | Engine | By |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---|---|
| Phone, 12 GB RAM, Snapdragon class | UFS 4.x (~2300 MiB/s) | Qwen3.6-35B-A3B Q4_K_M | 8 | 3000 MiB | 5.0 | n/a | n/a | 144 MiB | 65% | 0.21.0 | maintainer |
| Phone, 12 GB RAM, Snapdragon class | UFS 4.x (~2300 MiB/s) | Qwen3-30B-A3B Q4_K_M | 8 | auto, cap 4000 | 5.2 | n/a | n/a | 225 MiB | 76% | 0.21.0 | maintainer |
| Phone, 12 GB RAM, Snapdragon class | UFS 4.x (~2300 MiB/s) | Gemma-4-26B-A4B Q4_K_M | 8 | 4000 MiB | 4.1 | n/a | n/a | 144 MiB | 82% | 0.21.0 | maintainer |
| Phone, 12 GB RAM, Snapdragon class | UFS 4.x (~2300 MiB/s) | gpt-oss-120b Q4_K_M | 2 | 2000 MiB | 2.2 | n/a | n/a | 590 MiB | 32% | 0.21.0 | maintainer |
| Laptop, x86 8 cores, 16 GB DDR4 | NVMe | Qwen3.6-35B-A3B Q4_K_M | 8 | auto | 7.3 | n/a | n/a | 24 MiB | 92% | 0.21.0 | maintainer (`--drop-cold-experts 0.75`) |

Columns the README rows did not record are `n/a`; new rows from `bench-report.sh` fill all of
them. Full per-token CSVs for the maintainer rows are under `docs/bench-data/`.
