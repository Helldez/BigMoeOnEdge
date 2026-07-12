
### Qwen3-30B-A3B-Q4_K_M — base: cache 4000 MiB, lane 4, overlap

| Config | decode tok/s | median | p95 | cache hit | flash MiB/tok | stall ms/tok | spec MiB | useful | recall | TTFT s | peak RSS | CPU max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline (overlap) | **5.09** | 5.28 | 8.30 | 76% | 225 | 0.063 | — | — | — | 23.8 | 5.20 GB | 68°C |
| + prefetch 2 | **5.08** (-0.01) | 5.34 | 8.00 | 77% | 225 | 0.063 | 464 | 163/167 (98%) | — | 16.4 | 5.99 GB | 70°C |
| + spec-gate | **2.65** (-2.44) | 2.92 | 4.16 | 88% | 380 | 0.012 | 44287 | 12822/14241 (90%) | 88% | 16.6 | 6.05 GB | 71°C |
| + spec-gate + prefetch 2 | **2.99** (-2.10) | 3.22 | 4.21 | 88% | 387 | 0.006 | 42606 | 12449/13734 (91%) | 88% | 19.6 | 5.70 GB | 81°C |

### Gemma-4-26B-A4B-it-Q4_K_M — base: cache 2000 MiB, lane 4, overlap

| Config | decode tok/s | median | p95 | cache hit | flash MiB/tok | stall ms/tok | spec MiB | useful | recall | TTFT s | peak RSS | CPU max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline (overlap) | **3.15** | 3.24 | 4.23 | 58% | 365 | 0.133 | — | — | — | 21.4 | 5.67 GB | 74°C |
| + prefetch 2 | **3.14** (-0.02) | 3.22 | 4.13 | 58% | 366 | 0.133 | 267 | 56/66 (85%) | — | 24.8 | 5.71 GB | 74°C |
| + spec-gate | **1.60** (-1.55) | 1.62 | 2.16 | 59% | 611 | 0.113 | 24173 | 3584/6169 (58%) | 66% | 21.5 | 5.86 GB | 69°C |
| + spec-gate + prefetch 2 | **1.61** (-1.54) | 1.67 | 2.20 | 59% | 608 | 0.110 | 21349 | 3194/5386 (59%) | 66% | 22.2 | 5.89 GB | 70°C |

### Qwen3-30B-A3B-Q4_K_M — MAX: cache 5000 MiB, lane 4, overlap

| Config | decode tok/s | median | p95 | cache hit | flash MiB/tok | stall ms/tok | spec MiB | useful | recall | TTFT s | peak RSS | CPU max |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline (overlap) | **5.10** | 5.49 | 7.51 | 84% | 148 | 0.041 | — | — | — | 29.3 | 6.05 GB | 76°C |
| + prefetch 1 | **5.14** (+0.05) | 5.36 | 6.88 | 84% | 148 | 0.036 | 36 | 5/5 (100%) | — | 32.4 | 6.11 GB | 70°C |
