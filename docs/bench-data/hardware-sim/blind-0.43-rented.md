# Blind predictions: the two rented 0.43 validation boxes

Made with `bmoe-planlab simulate` from the boxes' parts only, before any engine ran on them, and
committed before their results came in. The results go beside them when they land; nothing here is
edited after that.

- **Box A**: one RTX 3070 8 GB, 31 GB RAM, i5-12400F (12 threads), NVMe ~3.3 GB/s.
- **Box B**: two RTX 5060 Ti 16 GB, 60 GB RAM, Ryzen 7 9700X (16 threads), NVMe ~3.6 GB/s.

Assumed where unknown: DDR4-3200 dual channel on A (51.2 GB/s), DDR5-5600 dual channel on B
(89.6 GB/s), the 3070 on PCIe 4.0 x16, each 5060 Ti on its own PCIe 5.0 x8. Hit rates: Flash-Next
and DeepSeek from rented runs' counters (`hitcurve=`), Qwen3.6 from its routing replayed on another
Q4_K_M file. Engine 0.43.0's rules, `--auto`, first load. Range: the estimate over measured /
estimated 0.69-1.62.

| Box, model | Context | Plan | Cost model | Estimate tok/s (range) |
| --- | --- | --- | ---: | ---: |
| A, Flash-Next Q2_0 | 2048 | streamed; 49 layers on the card, device cache 2917 MiB (64%), RAM cache 25103 MiB | 14.4 | 24.8 (15-36) |
| A, Flash-Next Q2_0 | 16384 | streamed; device cache 723 MiB (20%), RAM cache 24739 MiB | 14.4 | 16.3 (10-24) |
| A, Qwen3.6-35B UD-Q4_K_M | 2048 | fits; 41 layers on the card, device cache 4770 MiB (85%) | - | 63.0 (39-91) |
| A, Qwen3.6-35B UD-Q4_K_M | 16384 | fits; device cache 3356 MiB (78%) | - | 52.7 (33-76) |
| B, DeepSeek V4 Flash UD-IQ2_M | 2048 | streamed; 44 layers split 7 + 37, device cache 18675 MiB (80%), RAM cache 51214 MiB | 9.1 | 9.4 (6-14) |
| B, DeepSeek V4 Flash UD-IQ2_M | 16384 | streamed; split 6 + 38, device cache 13126 MiB (75%), RAM cache 49761 MiB | 9.1 | 8.8 (5-13) |
| B, Flash-Next Q2_0 | 2048 | fits; 49 layers split 20 + 29, device cache 24984 MiB (96%) | - | 63.9 (39-93) |
| B, Flash-Next Q2_0 | 16384 | fits; split 17 + 32, device cache 21657 MiB (95%) | - | 62.3 (38-90) |

Known weak spots, said before the runs: the hosts' rates under 4 bits a weight (Flash-Next, DeepSeek)
come from processors of another generation (an Alder Lake and a desktop Zen 5 were never measured);
every simulated fit turned a fused op off at least once, so the real cards may take more layers;
the DeepSeek 7 + 37 split on two equal cards may be that difference.

## Addendum, still before any result: box A's Qwen3.6 file

Box A runs a plain Q4_K_M of Qwen3.6-35B-A3B, not the UD-Q4_K_M simulated above (unsloth's repository
lists only UD variants, so the exact file is still to be confirmed). Simulated on the plain Q4_K_M
(ggml-org's), with the same machine:

| Box, model | Context | Plan | Estimate tok/s (range) |
| --- | --- | --- | ---: |
| A, Qwen3.6-35B Q4_K_M | 2048 | fits; 41 layers on the card, device cache 4346 MiB (83%) | 61.1 (38-89) |
| A, Qwen3.6-35B Q4_K_M | 16384 | fits; device cache 3085 MiB (76%) | 51.9 (32-75) |
