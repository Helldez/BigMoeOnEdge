# 2026-11-09 — rented 12 GB card: the release archive with and without the CUDA pack

A user reported 3 tok/s with Qwen3.8-Flash-Next IQ3_S on a 12 GB card with 128 GB of RAM, where
Strata gives them 35-45. This session reproduced the setup with the published **v0.40.2** release
archive for x86_64 Linux, not a build of our own, to answer one question: does the engine run on
the card at all if a user installs it the way the release page suggests?

Every run is `bmoe-cli -m <file> --auto -p <prompt> -n 256 --csv run.csv` and nothing else, the same
prompt each time, from a fresh process. Each directory holds what the run printed (`stdout.txt`,
`stderr.txt`) and its per-token CSV with the `# summary` trailer ([telemetry.md](../../telemetry.md)).

## Machine

| | |
|---|---|
| GPU | RTX 3060 class, 12 GB, driver 595 |
| CPU | 6-core desktop CPU (12 threads), a quota of 11.5 CPUs |
| memory control group | 120.7 GiB (128 GB host class, like the report) |
| model | ISTA-DASLab Qwen3.8-Flash-Next GSQ-RCO **IQ3_S**, 2 shards, 83.6 GB (54.8 GB + the 28.8 GB embedding shard) |
| engine | `bmoe-cli-v0.40.2-linux-x86_64.tar.gz` and `bmoe-pack-cuda-v0.40.2-linux-x86_64.tar.gz` from the release page |

The container has no Vulkan driver for the card (`ggml_vulkan: No devices found`), so without CUDA
the engine has the CPU only. A Windows machine with the NVIDIA driver would most likely get Vulkan
instead; that case was not measured here.

## The two archives do not extract into the same folder

The main archive extracts into a folder, `bmoe-cli-v0.40.2-linux-x86_64/`. The CUDA pack has no
folder: its files (`libggml-cuda.so`, the CUDA runtime, cuBLAS) land wherever it is extracted. The
README in the archive says to extract the pack "into this folder", meaning the folder of
`bmoe-cli`. If you extract both archives in the same place, which is what a download folder
invites, the CUDA files end up one level above `bmoe-cli`. The engine never loads them, and
nothing tells you.

## Results

| Run | How it was installed | Backends loaded | Decode tok/s | Prefill tok/s | Load s |
|---|---|---|---:|---:|---:|
| `plain` | the main archive only | CPU (+ Vulkan, no device) | **2.9** | 5.1 | 74.8 (cold) |
| `cudawrong` | both archives extracted in the same folder | CPU (+ Vulkan, no device) | **1.6** | 8.0 | 5.2 |
| `cudaok` | the pack extracted inside `bmoe-cli-v0.40.2-linux-x86_64/` | CUDA, Vulkan, CPU | **17.5** | 19.8 | 6.6 |
| `cudaok2` | the same, a second load | CUDA, Vulkan, CPU | **16.0** | 21.1 | 6.5 |

- The reported 3 tok/s matches the run where CUDA was not loaded. Installed correctly, the same
  release on the same class of machine decodes at **16-17.5 tok/s**, about 6x faster.
- Both CPU-only runs kept the model resident in RAM. `--auto` picked 2 threads for `plain` and 8
  for `cudawrong` on the same machine: 8 threads on 6 cores decoded slower (4.0 CPU-s per token
  against 0.68). The thread choice on the CPU path is not stable between loads.
- With CUDA, `--auto` kept the model resident and armed llama.cpp's expert cache on the card:
  5.3 GB for the experts used most, out of 48 GB of host experts. The cache hit **68%** of the time.
  The misses are uploaded over the bus: 76.7 GB over the run, as the log reports it, probe included.
  The CPU did almost nothing (0.06 CPU-s per token).
- Every run ends with `mode: mmap. Expert streaming is OFF ..., so this run is a baseline, not this
  engine`, even though `--auto` chose that mode. The line is wrong for a plan that `--auto` made.

## What this leaves

- **Packaging**: give the CUDA pack the same top-level folder as the main archive, so extracting
  both in one place does the right thing. Also, say it in the log when an NVIDIA driver is present
  but no CUDA module was loaded.
- **The gap to Strata on this class is real but smaller than reported**: 16-17.5 here against the
  35-45 the user measured with Strata. On the 16 GB card
  ([2026-10-27-rented-16gb](../2026-10-27-rented-16gb/README.md)) the difference was the same:
  Strata computes on the CPU the experts its card does not hold, while this engine uploads each miss
  over the bus. Strata was not run in this session.
