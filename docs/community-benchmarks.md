# Community benchmarks

Every number in the README came from two machines: one phone, one Windows laptop. The engine runs
unmodified on any Linux or macOS box with a fast drive, and this page exists to answer one
question: **what does expert streaming do on hardware we do not own?**

Ten minutes and a machine is all it takes. Run the protocol, open a
[benchmark report](https://github.com/Helldez/BigMoeOnEdge/issues/new?template=benchmark-report.yml),
and the row lands in the [table below](#results) with your GitHub handle. You do not need to know
the code, and you do not pick any settings: the protocol is `--auto`, so the engine picks them for
your machine and rows compare.

## Start here

**Which model?** Any MoE llama.cpp loads ([which-models-stream.md](which-models-stream.md)), any quantization. No model
yet? The [catalog table](#models-that-line-up-with-the-readme) lists what the app ships. Larger
than your RAM is the interesting case; one that fits is still a valid row, and there `--auto` runs
it resident (and on your GPU, with a build that carries one). The report asks which of the two you
are in.

### PC or laptop, Linux and macOS

```bash
git clone --recursive https://github.com/Helldez/BigMoeOnEdge.git
cd BigMoeOnEdge
scripts/bench-report.sh /path/to/model.gguf
```

It builds the CLI, records your CPU / RAM / drive, measures the drive at the request size the
engine actually issues, runs the model once with `--auto` (see [the protocol](#the-protocol)),
prints two markdown tables and the plan it chose. Paste all of it. Done.

- **No toolchain?** Every release attaches a prebuilt `bmoe-cli` (Linux x86_64 / aarch64, macOS
  arm64, Windows x86_64). Point the script at it: `BMOE_CLI=/path/to/bmoe-cli scripts/bench-report.sh model.gguf`.
- **A GPU in the box?** `--auto` uses it only if the binary carries its backend. The release
  archives carry Vulkan on Linux and Windows and Metal on macOS. For CUDA, build it in:
  `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=ON && cmake --build build -j`; the
  CUDA module lands beside `bmoe-cli` and loads like the shipped ones. The plan in the report says
  which devices it used.
- **Binary will not start?** The archives carry one CPU module per instruction-set tier and pick the
  best one this machine runs, so no particular CPU is assumed. The Linux aarch64 archive needs
  glibc 2.38 or newer (Ubuntu 24.04); the x86_64 one runs from Ubuntu 22.04. Drop `BMOE_CLI=` and
  it builds from source.
- **Windows:** use the prebuilt binary with the command under [On a phone](#on-a-phone), minus the
  `adb shell`. The script itself is Linux and macOS only.

### Android phone

- **Easy path:** install the release APK, load a model from its catalog, generate at least 256
  tokens, screenshot the telemetry panel. Check two settings first, or the row is measuring
  something else: **Auto-plan On** (the Plan screen; on by default where the planner is built in)
  and **cold-expert dropping Off** (it defaults to 75 %, which is a lossy speedup).
- **Exact path** (comparable with the PC rows): push `bmoe-cli` and run it over adb, see
  [On a phone](#on-a-phone).

Either way the app cannot measure your drive's raw read rate, so a phone row leaves that column
empty unless you run the storage probe yourself.

### Apple hardware

| | Status |
|---|---|
| **Mac**, Apple silicon or Intel | Works. A direct request is served with `F_NOCACHE` (Apple's uncached-descriptor mode — a caching hint, not Linux `O_DIRECT`: no alignment, no DMA promise), and `o_direct` reports what the open actually achieved. Uncached reads can sit below the buffered rate on the same drive, so a Mac row's flash column may read slower than the storage probe suggests; that is the honest number, not a malfunction. Say "macOS" in your report. A 64 or 128 GB Mac is still a row we want. |
| **iPhone, iPad** | Unvalidated. iOS does not run command-line binaries, so [`examples/ios`](../examples/ios) builds `bmoe-cli` into a harness app you sign and sideload yourself. It has not been run on a device here: a row from one is welcome, run with `--auto --plan-explain` and with the plan it printed. |

### What happens to your row

- It lands in the [results table](#results) with your handle.
- Phone rows name the device *class*, never the model, so nothing identifying gets published.
- A slow number is as useful as a fast one. A DRAM-less SSD or a PCIe x1 slot is a result, often
  the most informative kind.

## The protocol

One run, `--auto` and nothing else, 256 greedy tokens, the same prompt as every README table
(`--plan-explain` only prints the plan it runs). That is what the engine does on *this* machine:
the planner hands what fits to the GPUs the build carries, keeps a model that fits in RAM resident
without streaming, and streams only the experts left over. It is the row to compare with other
engines on the same hardware.

Settings are not part of the protocol: `--auto` chooses threads, cache, read lanes and device
placement from what it measures, and the plan in the report says what it chose. A row with
settings forced by hand measures those settings, not the machine, and does not go in the table.

Override with an environment variable, and say so in the issue:

| Variable | Default | Change it if |
|---|---|---|
| `N_PREDICT` | 256 | never below 256, the cache is still warming |

Flags after the model path reach `bmoe-cli` verbatim: only the ones that change the model, not the
engine (`--think` to measure a reasoning run, `--n-expert-used 6` for a turbo top-k row, which is lossy and
sits *next to* the default row). To understand what the plan chose, see
[benchmark-method.md](benchmark-method.md) and [hardware-planning.md](hardware-planning.md).

Check the `mode:` line before reporting a number. It says `expert streaming` when
`--auto` streamed, and the plan says why when it did not: a model that fits runs resident, which is
the right answer on that machine, not a missing flag.

### On a phone

`bench-report.sh` is Linux and macOS only. On Android, push `bmoe-cli` (from
`scripts/build-android.ps1`, or the aarch64 release binary) and run the same protocol:

```bash
adb push MODEL.gguf /data/local/tmp/
adb shell /data/local/tmp/bmoe-cli -m /data/local/tmp/MODEL.gguf --chatml -n 256 \
  --auto --plan-explain --csv /data/local/tmp/run.csv \
  -p "Write a long detailed essay about the history of computing including its origins its key milestones the people involved and the future directions of the field"
```

This command is already the protocol; paste the `plan:` lines with the result. Things to get right
around it:

- Stage the model on `/data/local/tmp` or `/sdcard/Download`. **Not** the app's external files dir
  under `/storage/emulated`: it is FUSE-backed and `O_DIRECT` silently falls back to buffered.
- Check `o_direct=1` in the telemetry before trusting the number.
- Phones throttle: run twice, report the second, say if the two differ by more than a few percent.
- The shell runs under a lower CPU frequency cap than a foreground app on some devices, so an adb
  row can read a little slower than the same configuration in the app. Say which one you used.

### Models that line up with the README

Any MoE in any quant is valid, and a different quant of the same model is its own row: expert bytes
per token change with the quant, so the flash column does too. That comparison is one of the things
the table is for. What the app catalog ships:

| Model | Catalog quant | Size | Download | Notes |
|---|---|---:|---|---|
| Qwen3.6-35B-A3B | Q4_K_M | 22.3 GB | [bartowski/Qwen_Qwen3.6-35B-A3B-GGUF](https://huggingface.co/bartowski/Qwen_Qwen3.6-35B-A3B-GGUF) | the README's reference row |
| Qwen3-30B-A3B | Q4_K_M | 18.5 GB | [unsloth/Qwen3-30B-A3B-GGUF](https://huggingface.co/unsloth/Qwen3-30B-A3B-GGUF) | |
| Gemma-4-26B-A4B-it | Q4_K_M | 17.0 GB | [bartowski/google_gemma-4-26B-A4B-it-GGUF](https://huggingface.co/bartowski/google_gemma-4-26B-A4B-it-GGUF) | |
| gpt-oss-120b | Q4_K_M | 62.8 GB | [unsloth/gpt-oss-120b-GGUF](https://huggingface.co/unsloth/gpt-oss-120b-GGUF) | thinking stays off (the default); pass the first shard |
| DeepSeek V4 Flash 0731 | UD-IQ2_M | 90.9 GB | [unsloth/DeepSeek-V4-Flash-0731-GGUF](https://huggingface.co/unsloth/DeepSeek-V4-Flash-0731-GGUF) | pass the first shard |
| Qwen3.8-Flash-Next | UD-IQ3_XXS | 82.0 GB | [unsloth/Qwen3.8-Flash-Next-GGUF](https://huggingface.co/unsloth/Qwen3.8-Flash-Next-GGUF) | needs engine 0.22.0+; pass the first shard |
| Qwen3.8-Flash-Next | Q2_K | 80.4 GB | [DevQuasar/Qwen.Qwen3.8-Flash-Next-GGUF](https://huggingface.co/DevQuasar/Qwen.Qwen3.8-Flash-Next-GGUF) | dense side at 2-4 bit (2.4 GB pinned); no imatrix; pass the first shard |

### What makes a row comparable

The script fills these from the engine's telemetry ([telemetry.md](telemetry.md)). A bare tok/s
without them cannot be placed in the table.

| Column | Meaning | Why it matters |
|---|---|---|
| Decode tok/s, Prefill tok/s | generation and prompt throughput | the headline, and the two are limited by different things |
| Stall s/tok | time per token compute waited on flash | the flash bottleneck, isolated; what NVMe should shrink |
| Compute s/tok | time per token in expert math and cache management | the CPU bottleneck, isolated |
| Flash/token | MiB read from the drive per generated token | whether the cache is working |
| Cache hit | share of expert reads served from RAM | the same, from the other side |
| majflt/tok | major page faults per token | non-zero means the dense weights were being evicted mid-run: the OS, not the engine, is in charge |
| Storage read, 512 KiB O_DIRECT | the drive's raw rate at the engine's request size | the ceiling. Stall far above `Flash/token` over that rate means a request-size or queue problem, not bandwidth |

On a phone, the app's telemetry panel and the CSV from `scripts/bench-run.sh` carry the same fields.

### Doing it honestly

- Nothing else heavy running. A browser with fifty tabs is heavy.
- Passive-cooled boards throttle: run twice, report the second, say if they differ.
- If the storage probe reads far below the drive's rating, say so. That is a result in itself.
- The first run after a download has the file's head in the page cache. The probe skips a quarter
  into the file for exactly that reason; the engine run uses O_DIRECT and is unaffected.
- A storage column reading `n/a` means the probe refused its own number: the filesystem accepted
  `iflag=direct` but served the page cache anyway (network and overlay filesystems do this), so
  the measured rate was not a physical read. The engine columns are still valid.

## Results

Ordered by hardware class. "maintainer" rows are the README's, measured over `adb` on the reference
phone and on the Windows laptop with the fixed streaming settings the protocol used before
`--auto` replaced it, so they measure the streamer rather than what `--auto` picks. Phone rows name the device class on purpose.

| Hardware | Storage (512 KiB rate) | Model, quant | k | Cache | Decode tok/s | Prefill tok/s | Stall s/tok | Flash/token | Cache hit | Engine | By |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---|---|
| Phone, 12 GB RAM, Snapdragon class | UFS 4.x (~2300 MiB/s) | Qwen3.6-35B-A3B Q4_K_M | 8 | 3000 MiB | 5.0 | n/a | n/a | 144 MiB | 65% | 0.21.0 | maintainer |
| Phone, 12 GB RAM, Snapdragon class | UFS 4.x (~2300 MiB/s) | Qwen3-30B-A3B Q4_K_M | 8 | auto, cap 4000 | 5.2 | n/a | n/a | 225 MiB | 76% | 0.21.0 | maintainer |
| Phone, 12 GB RAM, Snapdragon class | UFS 4.x (~2300 MiB/s) | Gemma-4-26B-A4B Q4_K_M | 8 | 4000 MiB | 4.1 | n/a | n/a | 144 MiB | 82% | 0.21.0 | maintainer |
| Phone, 12 GB RAM, Snapdragon class | UFS 4.x (~2300 MiB/s) | gpt-oss-120b Q4_K_M | 2 | 2000 MiB | 2.2 | n/a | n/a | 590 MiB | 32% | 0.21.0 | maintainer |
| Laptop, x86 8 cores, 16 GB DDR4 | NVMe | Qwen3.6-35B-A3B Q4_K_M | 8 | auto | 7.3 | n/a | n/a | 24 MiB | 92% | 0.21.0 | maintainer (`--drop-cold-experts 0.75`) |
| Desktop, x86 16 cores, 60 GB DDR5, two 16 GB discrete GPUs | PCIe NVMe (4273-4392 MiB/s) | DeepSeek V4 Flash 0731 UD-IQ2_M (85 GiB, 1.4x RAM) | 6 | 41892 MiB, memlock raised | 8.7 | 5.1 | 0.000 | 87 MiB | 91.1% | 0.33.3 | [jordimarsal](https://github.com/Helldez/BigMoeOnEdge/issues/321) (layers split over the two cards, experts streamed) |
| Desktop, x86 16 cores, 60 GB DDR5, two 16 GB discrete GPUs | PCIe NVMe (4273-4392 MiB/s) | DeepSeek V4 Flash 0731 UD-IQ2_M | 6 | 7342 MiB, default memlock | 4.4 | n/a | n/a | n/a | 72.7% | 0.33.3 | [jordimarsal](https://github.com/Helldez/BigMoeOnEdge/issues/321) (same box: the cache bounded by `RLIMIT_MEMLOCK`) |
| Desktop, x86 16 cores, 60 GB DDR5, two 16 GB discrete GPUs | PCIe NVMe (4273-4392 MiB/s) | DeepSeek V4 Flash 0731 UD-IQ2_M | 6 | 41092 MiB | 1.0 | 1.4 | 0.036 | 120 MiB | 90.0% | 0.32.0 | [jordimarsal](https://github.com/Helldez/BigMoeOnEdge/issues/321) (CPU only, the protocol before `--auto`; 7284 majflt/tok) |
| Desktop, x86 16 cores, 60 GB DDR5, two 16 GB discrete GPUs | PCIe NVMe (4273-4392 MiB/s) | Qwen3-30B-A3B Q4_K_M | 8 | resident on the cards | 245.8 | 242.9 | 0.000 | 0 MiB | n/a | 0.33.3 | [jordimarsal](https://github.com/Helldez/BigMoeOnEdge/issues/321) |
| Desktop, x86 16 cores, 60 GB DDR5, two 16 GB discrete GPUs | PCIe NVMe (4273-4392 MiB/s) | MiniCPM-V-4.7-35B-A3B Q4_K_M | 8 | resident on the cards | 220.5 | 201.3 | 0.000 | 0 MiB | n/a | 0.33.1 | [jordimarsal](https://github.com/Helldez/BigMoeOnEdge/issues/321) |
| Laptop, Apple silicon 10 cores, 16 GB unified | NVMe (6358 MiB/s, F_NOCACHE) | Qwen3.6-35B-A3B Q4_K_M | 8 | 4713 MiB | 17.4 | 24.5 | 0.000 | 91 MiB | 81.3% | 0.36.0 | maintainer (`--auto`; prefill on Metal, decode on the cores; the 30-token protocol prompt makes the prefill column mostly load) |

Columns the README rows never recorded are `n/a`; rows from `bench-report.sh` fill all of them. A
resident row (the model fit on the cards) has no cache and no flash column: it measures the engine's
compute ceiling on that machine, which is the number a streamed row on the same box is short of.
The maintainer rows predate `--ubatch 512` in the protocol, so their compute-buffer reservation was
wider than a fresh row's.
Per-token CSVs for the maintainer rows are under `docs/bench-data/`.

## Open questions

Any hardware is a useful row, including a laptop that looks like every other laptop. These are
just the questions we cannot answer ourselves:

| Question | Hardware that answers it |
|---|---|
| Does a real NVMe queue remove the flash stall the phone shows? | Any mini PC or laptop, PCIe 3.0 x4 drive, 16 GB |
| Where does the >RAM regime start on a fast CPU with plenty of memory? | 32-64 GB DDR5 desktops and mini PCs |
| Is ARM Linux with a drive faster than UFS a different picture? | RK3588, Qualcomm Dragonwing, Raspberry Pi 5 (x1 lane, expected slow, still informative) |
| What happens where a 284B model is only 2x memory? | Unified-memory desktops: Ryzen AI Max, Mac Studio, 64-128 GB |
| Does the phone number hold outside one device? | Any recent Snapdragon, Dimensity or Tensor phone |
| What does the macOS page-cache path actually cost? | Any Mac, see the caveat above |
