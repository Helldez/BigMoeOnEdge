# 2026-11-10: GLM on two 24 GB cards: the device tier, the identity bound, the graph width

A rented workstation, two RTX 3090 class cards over PCIe 4, a 24-core desktop CPU, a 241 GiB control
group. The models are GLM-4.5-Air 106B-A12B UD-Q4_K_XL (68 GB, Q4_K + Q5_0 + Q5_1 experts) and
GLM-4.7 355B-A32B UD-Q2_K_XL (135 GB). Every run is `bmoe-server` at its default 16384 context, with
the same client: prompts `a` and `b` at 512 greedy tokens, and a 9.5k-token prompt where named. The
model file's page cache is dropped before every server start. A run of loads shares one data dir, so
the measurements one load keeps (`*.knobs`, `MeasuredKnobs`) are what the next load plans from.

Builds: `main` 9cd9d23; `integ` = main + #525 (5e-4 bound) + #524; `wm` = #526 (with #523 and #524);
`all` = #526 + #525 at the 1e-3 bound.

## The device tier across loads (#524), `pcie4/`

| GLM-4.7 | decode tok/s | prefill (45 tokens) |
|---|---:|---:|
| main (device cache armed by rule) | 3.14-3.15 | 46.6 |
| integ load 1: the cache timed at 3.18 | 3.14-3.15 | 45.5 |
| integ load 2: the fitter's placement, timed at 6.23 | 6.17-6.21 | 107.9 |
| integ load 3: the fitter's placement kept | 6.18 | 121.1 |

The cache held a tenth of the experts (13.5 GB of ~120 GB on the cards). Every miss crossed PCIe, and
the cards computed nothing the host would not have computed faster.

## The identity bound (#525), `pcie4/diag-exact.txt`, `pcie4-all/diag-all.txt`

Distance from the exact result (NMSE, double precision from the dequantized weights):

| | experts: card / host | dense: card / host |
|---|---|---|
| GLM-4.5-Air | 5.27e-4 / 1.05e-5 | 6.3e-6 / 1.0e-5 |
| GLM-4.7 | 5.9e-6 / 9.7e-6 | 6.1e-6 / 1.0e-5 |

At upstream's 5e-4, the cards were refused on GLM-4.5-Air. At 1e-3 (twice upstream's per-op bound,
since a wrong kernel is off by 1e-1 or more) they are accepted.

## The graph width across loads (#526), `pcie4-width/`

GLM-4.5-Air, cards refused for the experts (the 5e-4 bound), two usage profiles on their own data dirs:

| profile | load 1, full width | load 2, 4096 | load 3, the plan's decision |
|---|---|---|---|
| chat (`a`, `b`) | 13.9-14.1 | 18.2 | 4096: 24.63 s against 18.63 a request |
| documents (the 9.5k prompt, `a`) | decode 11.9-14.2, prefill 1028 | decode 15.2-18.3, prefill 816 | 4096: 17.33 s against 15.15 |

## Everything together, `pcie4-all/`

GLM-4.5-Air, `all`, five loads on one data dir:

| load | ran | decode tok/s |
|---|---|---:|
| 1 | the device cache, timed | 14.7-14.8 |
| 2 | the fitter's placement, timed | 14.1-14.2 |
| 3 | the cache kept (16.10 against 14.05); the full width timed | 14.7-14.8 |
| 4 | 4096 wide (34.9 GB cache), timed | 20.0-20.7 |
| 5 | 4096 kept (21.00 s against 15.75) | 20.1-20.7 |

`main` decoded at 14.1-14.2 on this box, and llama.cpp's own server at 23.8-24.0 (`pcie4/air-llama.json`).
An earlier box with the same cards over PCIe 3 and a 40-core CPU gave GLM-4.7 1.6 tok/s with the cache
and 3.9 with the fitter's placement, and GLM-4.5-Air 10.4 decode and 603-613 prefill on the 9.5k prompt
(llama.cpp 15.7 and 104). That box's raw files were lost when its credit ran out.
