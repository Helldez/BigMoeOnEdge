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

## Results: box B (added after its runs; the rows above are unchanged)

Measured on the box with engine 0.43.0, `--auto`, first load. "CLI" is the recorded run of the plan
record (`bmoe-planlab record --run`, 200 tokens); "server" is `bmoe-server` at ctx 16384, two
answers. Records: `docs/bench-data/plan-records/rtx-2x16gb-5060ti-*`.

| Box, model | Context | Plan predicted | Plan on the box | Estimate | Measured tok/s | Measured / estimate |
| --- | --- | --- | --- | ---: | ---: | ---: |
| B, DeepSeek V4 UD-IQ2_M | 2048 | split 7 + 37, device cache 18675, RAM cache 51214 | split 7 + 37, device cache 17239, RAM cache 52127 | 9.4 (6-14) | 5.76 CLI | 0.61 |
| B, DeepSeek V4 UD-IQ2_M | 16384 | split 6 + 38, device cache 13126, RAM cache 49761 | split 6 + 38, device cache 13153, RAM cache 51017 | 8.8 (5-13) | 5.64 CLI; 5.4 / 5.5 server | 0.61-0.64 |
| B, Flash-Next Q2_0 | 2048 | fits, 49 layers split 20 + 29, device cache 24984 | fits, 49 layers split 19 + 30, device cache 23241 | 63.9 (39-93) | 37.1 CLI; 44.7 knob trial | 0.58-0.70 |
| B, Flash-Next Q2_0 | 16384 | fits, split 17 + 32, device cache 21657 | fits, split 16 + 33, device cache 18528 | 62.3 (38-90) | 37.1 CLI; 48.0 / 41.4 server | 0.60-0.77 |

The plans came out as predicted: the same regime, the same split within one layer, every cache within
3-17%. The speeds did not: every estimate was 1.3-1.7x too high, two of the four below its range.

Why, from the records (each rate the box measured against the one the parts model gave it blind,
`bmoe-planlab parts`):

- **Cards right.** Experts 1.03-1.11, dense 0.91-1.09, pinned uploads 0.77-1.08.
- **Uploads from the file's mapping 0.52-0.70.** The model priced them as a share of the link (PCIe
  5.0 x8 here); every box measured 7-9 GiB/s whatever its link, the host's staging copy. In fits, a
  device-cache miss is that upload. Fixed: the parts model now takes the lower of the link's share and
  the host-copy rate (`upload from the mapping, host-copy bound`), 0.81-1.08 on this box left out.
- **Card memory 1.03 too high.** The cards report 3% less than their published size; the sim gave each
  card ~530 MiB too many, hence the larger caches. Fixed: fitted as `card memory reported / published`
  (0.967), 0.98-1.01 on every card left out.
- **Host under 4 bits on Zen 5 0.35-0.79.** The weak spot said before the runs; no fix but more records.
- **Device-cache hit rate.** Flash-Next's hit curve (other boxes' counters) said 94% at this size; the
  box's own counters read 85.8-91.5%: two to three times the misses, each one a host-copy upload.
  DeepSeek: the device cache read 66% on an 8 GB slice in the routing run.
- **DeepSeek's per-token cache management (33 ms of 204 in the routing run) is not in the estimate.**
  The run split as compute 126 + cache management 33 + flash 45 ms; the estimate has no term for the
  second.

Re-simulated after the two fixes, with DeepSeek's own routing from the box (no longer blind): DeepSeek
8.5 / 7.4 tok/s (measured / estimate 0.68-0.76), Flash-Next 57.8 / 54.5 (0.64-0.68, the hit curve).
