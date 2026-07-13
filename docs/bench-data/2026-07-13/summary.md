
### Qwen3-30B-A3B-Q4_K_M (18.5 GB, 128 experts, top-8, 48 layers)

**Throughput** (tok/s; mean = aggregate decode, prefill = prompt-processing):

| Config | decode mean | min | max | median | p95 | prefill tok/s | TTFT (s) | cache hit | flash read/token | stall ms/tok |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| solo mmap (no streaming) | — | — | — | — | — | — | — | — | — | — |
| streaming O_DIRECT, cache 0, lane 4 | — | — | — | — | — | — | — | — | — | — |
| streaming + cache 2000 MiB, lane 2 | — | — | — | — | — | — | — | — | — | — |
| streaming + cache 2000 MiB, lane 4 | — | — | — | — | — | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 2 | — | — | — | — | — | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4 | — | — | — | — | — | — | — | — | — | — |
| streaming O_DIRECT + overlap, cache 0, lane 4 | — | — | — | — | — | — | — | — | — | — |
| streaming + cache 2000 MiB, lane 4, overlap | — | — | — | — | — | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4, overlap | — | — | — | — | — | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4, prefetch 1 | — | — | — | — | — | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4, prefetch 2 | — | — | — | — | — | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4, prefetch 4 | — | — | — | — | — | — | — | — | — | — |
| fixed cache + overlap (reference) | **5.02** | 1.69 | 9.67 | 5.25 | 8.04 | 6.63 | 15.9 | 76% | 225 MiB | 63.4 |
| adaptive cache (--cache-mb auto) + overlap | **4.37** | 0.30 | 9.25 | 5.67 | 8.23 | 4.49 | 18.6 | 82% | 169 MiB | 50.9 |
| adaptive cache, capped + overlap | **5.23** | 2.44 | 9.29 | 5.43 | 7.91 | 5.58 | 19.1 | 76% | 225 MiB | 58.3 |
| fixed cache + overlap + speculative gating | **3.18** | 1.93 | 6.23 | 3.23 | 4.75 | 3.83 | 22.7 | 78% | 358 MiB | 45.2 |

**Device pressure** (peak process RSS, free-RAM floor, SoC/battery temperature):

| Config | peak RSS | free-RAM floor | CPU start | CPU max | battery max |
|---|---:|---:|---:|---:|---:|
| solo mmap (no streaming) | — | — | — | — | — |
| streaming O_DIRECT, cache 0, lane 4 | — | — | — | — | — |
| streaming + cache 2000 MiB, lane 2 | — | — | — | — | — |
| streaming + cache 2000 MiB, lane 4 | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 2 | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4 | — | — | — | — | — |
| streaming O_DIRECT + overlap, cache 0, lane 4 | — | — | — | — | — |
| streaming + cache 2000 MiB, lane 4, overlap | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4, overlap | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4, prefetch 1 | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4, prefetch 2 | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4, prefetch 4 | — | — | — | — | — |
| fixed cache + overlap (reference) | 6.16 GB | 2.16 GB | 43.2 °C | 69.1 °C | 38.7 °C |
| adaptive cache (--cache-mb auto) + overlap | 6.02 GB | 1.44 GB | 45.1 °C | 86.9 °C | 39.1 °C |
| adaptive cache, capped + overlap | 6.00 GB | 2.36 GB | 45.9 °C | 71.8 °C | 39.8 °C |
| fixed cache + overlap + speculative gating | 5.88 GB | 2.27 GB | 46.7 °C | 67.2 °C | 40.4 °C |

**Adaptive cache & speculative gating** (budget & resizes under `--cache-mb auto`; router-prediction recall, useful-hit rate and auto-off under `--spec-gate`):

| Config | cache budget | resizes | resident | spec recall | spec useful | spec read/token | auto-off |
|---|---:|---:|---:|---:|---:|---:|:--:|
| fixed cache + overlap (reference) | 4000 MiB | 0 | 3998 MiB | — | — | — | — |
| adaptive cache (--cache-mb auto) + overlap | 4675 MiB | 0 | 4675 MiB | — | — | — | — |
| adaptive cache, capped + overlap | 4000 MiB | 0 | 3998 MiB | — | — | — | — |
| fixed cache + overlap + speculative gating | 4000 MiB | 0 | 3998 MiB | 88% | 87% | 22 MiB | no |

### Gemma-4-26B-A4B-it-Q4_K_M (17.0 GB, fused gate+up experts)

**Throughput** (tok/s; mean = aggregate decode, prefill = prompt-processing):

| Config | decode mean | min | max | median | p95 | prefill tok/s | TTFT (s) | cache hit | flash read/token | stall ms/tok |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| solo mmap (no streaming) | — | — | — | — | — | — | — | — | — | — |
| streaming O_DIRECT, cache 0, lane 4 | — | — | — | — | — | — | — | — | — | — |
| streaming + cache 2000 MiB, lane 2 | — | — | — | — | — | — | — | — | — | — |
| streaming + cache 2000 MiB, lane 4 | — | — | — | — | — | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 2 | — | — | — | — | — | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4 | — | — | — | — | — | — | — | — | — | — |
| streaming O_DIRECT + overlap, cache 0, lane 4 | — | — | — | — | — | — | — | — | — | — |
| streaming + cache 2000 MiB, lane 4, overlap | — | — | — | — | — | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4, overlap | — | — | — | — | — | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4, prefetch 1 | — | — | — | — | — | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4, prefetch 2 | — | — | — | — | — | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4, prefetch 4 | — | — | — | — | — | — | — | — | — | — |
| fixed cache + overlap (reference) | **3.18** | 2.06 | 4.95 | 3.25 | 4.23 | 8.42 | 20.5 | 58% | 365 MiB | 133.4 |
| adaptive cache (--cache-mb auto) + overlap | **1.88** | 0.07 | 7.61 | 4.78 | 6.42 | 3.67 | 26.9 | 85% | 111 MiB | 57.8 |
| adaptive cache, capped + overlap | **3.05** | 2.04 | 5.09 | 3.10 | 4.08 | 8.14 | 21.4 | 58% | 365 MiB | 132.0 |
| fixed cache + overlap + speculative gating | **2.93** | 1.02 | 5.13 | 3.00 | 3.96 | 7.45 | 22.3 | 58% | 366 MiB | 132.3 |

**Device pressure** (peak process RSS, free-RAM floor, SoC/battery temperature):

| Config | peak RSS | free-RAM floor | CPU start | CPU max | battery max |
|---|---:|---:|---:|---:|---:|
| solo mmap (no streaming) | — | — | — | — | — |
| streaming O_DIRECT, cache 0, lane 4 | — | — | — | — | — |
| streaming + cache 2000 MiB, lane 2 | — | — | — | — | — |
| streaming + cache 2000 MiB, lane 4 | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 2 | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4 | — | — | — | — | — |
| streaming O_DIRECT + overlap, cache 0, lane 4 | — | — | — | — | — |
| streaming + cache 2000 MiB, lane 4, overlap | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4, overlap | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4, prefetch 1 | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4, prefetch 2 | — | — | — | — | — |
| streaming + cache 4000 MiB, lane 4, prefetch 4 | — | — | — | — | — |
| fixed cache + overlap (reference) | 6.00 GB | 4.07 GB | 46.3 °C | 71.8 °C | 41.0 °C |
| adaptive cache (--cache-mb auto) + overlap | 6.35 GB | 1.57 GB | 47.1 °C | 78.4 °C | 41.8 °C |
| adaptive cache, capped + overlap | 6.14 GB | 4.14 GB | 48.6 °C | 86.9 °C | 42.2 °C |
| fixed cache + overlap + speculative gating | 6.19 GB | 4.02 GB | 49.0 °C | 84.5 °C | 42.7 °C |

**Adaptive cache & speculative gating** (budget & resizes under `--cache-mb auto`; router-prediction recall, useful-hit rate and auto-off under `--spec-gate`):

| Config | cache budget | resizes | resident | spec recall | spec useful | spec read/token | auto-off |
|---|---:|---:|---:|---:|---:|---:|:--:|
| fixed cache + overlap (reference) | 2000 MiB | 0 | 1996 MiB | — | — | — | — |
| adaptive cache (--cache-mb auto) + overlap | 4610 MiB | 0 | 4608 MiB | — | — | — | — |
| adaptive cache, capped + overlap | 2000 MiB | 0 | 1996 MiB | — | — | — | — |
| fixed cache + overlap + speculative gating | 2000 MiB | 0 | 1996 MiB | 63% | — | — | yes |
