
### Qwen3-30B-A3B-Q4_K_M (18.5 GB, 128 experts, top-8, 48 layers)

**Throughput** (tok/s; mean = aggregate decode, prefill = prompt-processing):

| Config | decode mean | min | max | median | p95 | prefill tok/s | TTFT (s) | cache hit | flash read/token | stall ms/tok |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| solo mmap (no streaming) | **2.00** | 0.15 | 8.15 | 2.76 | 5.90 | 2.88 | 21.7 | — | 0 MiB | 0.0 |
| streaming O_DIRECT, cache 0, lane 4 | **1.71** | 0.50 | 1.81 | 1.78 | 1.80 | 6.35 | 20.3 | — | 1051 MiB | 0.0 |
| streaming + cache 2000 MiB, lane 2 | **2.01** | 1.17 | 4.66 | 2.04 | 3.17 | 3.82 | 28.2 | 53% | 480 MiB | 0.0 |
| streaming + cache 2000 MiB, lane 4 | **2.37** | 1.45 | 4.88 | 2.40 | 3.57 | 4.64 | 25.9 | 53% | 480 MiB | 0.0 |
| streaming + cache 4000 MiB, lane 2 | **3.12** | 1.24 | 6.66 | 3.30 | 5.46 | 2.55 | 30.7 | 76% | 225 MiB | 0.0 |
| streaming + cache 4000 MiB, lane 4 | **3.47** | 1.09 | 7.62 | 3.64 | 6.00 | 3.14 | 28.9 | 76% | 225 MiB | 0.0 |
| streaming O_DIRECT + overlap, cache 0, lane 4 | **1.27** | 0.58 | 2.02 | 1.44 | 2.00 | 6.67 | 20.9 | — | 1051 MiB | 373.4 |
| streaming + cache 2000 MiB, lane 4, overlap | — | — | — | — | — | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4, overlap | **3.98** | 1.26 | 8.43 | 4.89 | 7.37 | 2.89 | 27.8 | 76% | 225 MiB | 58.1 |

**Device pressure** (peak process RSS, free-RAM floor, SoC/battery temperature):

| Config | peak RSS | free-RAM floor | CPU start | CPU max | battery max |
|---|---:|---:|---:|---:|---:|
| solo mmap (no streaming) | 5.87 GB | 5.76 GB | 39.0 °C | 64.8 °C | 35.1 °C |
| streaming O_DIRECT, cache 0, lane 4 | 6.13 GB | 4.62 GB | 42.8 °C | 66.8 °C | 36.6 °C |
| streaming + cache 2000 MiB, lane 2 | 5.04 GB | 3.22 GB | 44.8 °C | 60.6 °C | 38.1 °C |
| streaming + cache 2000 MiB, lane 4 | 5.62 GB | 3.58 GB | 44.8 °C | 62.9 °C | 38.9 °C |
| streaming + cache 4000 MiB, lane 2 | 5.97 GB | 2.01 GB | 45.5 °C | 63.3 °C | 39.4 °C |
| streaming + cache 4000 MiB, lane 4 | 5.56 GB | 1.79 GB | 48.2 °C | 73.7 °C | 39.4 °C |
| streaming O_DIRECT + overlap, cache 0, lane 4 | 5.82 GB | 5.22 GB | 46.3 °C | 65.6 °C | 40.6 °C |
| streaming + cache 2000 MiB, lane 4, overlap | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4, overlap | 5.80 GB | 1.98 GB | 46.7 °C | 70.2 °C | 39.4 °C |

### Gemma-4-26B-A4B-it-Q4_K_M (17.0 GB, fused gate+up experts)

**Throughput** (tok/s; mean = aggregate decode, prefill = prompt-processing):

| Config | decode mean | min | max | median | p95 | prefill tok/s | TTFT (s) | cache hit | flash read/token | stall ms/tok |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| solo mmap (no streaming) | **0.36** | 0.12 | 4.47 | 0.44 | 3.41 | 2.48 | 33.6 | — | 0 MiB | 0.0 |
| streaming O_DIRECT, cache 0, lane 4 | **1.61** | 1.08 | 1.68 | 1.62 | 1.66 | 6.44 | 30.9 | — | 904 MiB | 0.0 |
| streaming + cache 2000 MiB, lane 2 | **2.08** | 1.21 | 4.02 | 2.14 | 2.98 | 4.54 | 34.0 | 58% | 366 MiB | 0.0 |
| streaming + cache 2000 MiB, lane 4 | **2.24** | 1.19 | 3.86 | 2.29 | 3.09 | 4.10 | 28.1 | 58% | 366 MiB | 0.0 |
| streaming + cache 4000 MiB, lane 2 | — | — | — | — | — | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4 | — | — | — | — | — | — | — | — | — | — |
| streaming O_DIRECT + overlap, cache 0, lane 4 | **1.81** | 1.50 | 1.90 | 1.83 | 1.86 | 7.83 | 24.4 | — | 904 MiB | 366.7 |
| streaming + cache 2000 MiB, lane 4, overlap | **2.78** | 1.78 | 4.04 | 2.82 | 3.58 | 7.19 | 26.4 | 58% | 365 MiB | 123.5 |
| streaming + cache 4000 MiB, lane 4, overlap | — | — | — | — | — | — | — | — | — | — |

**Device pressure** (peak process RSS, free-RAM floor, SoC/battery temperature):

| Config | peak RSS | free-RAM floor | CPU start | CPU max | battery max |
|---|---:|---:|---:|---:|---:|
| solo mmap (no streaming) | 5.85 GB | 5.68 GB | 47.1 °C | 67.2 °C | 41.1 °C |
| streaming O_DIRECT, cache 0, lane 4 | 6.16 GB | 4.96 GB | 45.9 °C | 62.1 °C | 40.9 °C |
| streaming + cache 2000 MiB, lane 2 | 6.05 GB | 3.80 GB | 47.5 °C | 62.9 °C | 41.2 °C |
| streaming + cache 2000 MiB, lane 4 | 6.08 GB | 3.85 GB | 47.1 °C | 63.3 °C | 41.3 °C |
| streaming + cache 4000 MiB, lane 2 | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4 | — | — | — | — | — |
| streaming O_DIRECT + overlap, cache 0, lane 4 | 5.78 GB | 5.11 GB | 53.2 °C | 72.9 °C | 45.4 °C |
| streaming + cache 2000 MiB, lane 4, overlap | 6.06 GB | 4.11 GB | 53.2 °C | 73.7 °C | 45.5 °C |
| streaming + cache 4000 MiB, lane 4, overlap | — | — | — | — | — |
