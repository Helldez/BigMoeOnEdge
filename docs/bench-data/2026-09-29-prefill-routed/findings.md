# Routed expert arena on the phone (2026-09-29)

A 12 GB phone with a Hexagon v81 NPU and UFS 4 storage. Models streamed with the settings an
on-device Android UI agent runs in its NPU mode:

```
--session --decide --moe-stream --overlap --io-threads 4 --dense-weights ahwb -t 6 -c 2048 --chatml
--n-expert-used 4 --prefill-device HTP0 --ubatch 2048
GGML_HEXAGON_HOSTBUF=1 GGML_HEXAGON_OPPOLL=1
```

`decide` requests over six Android screens compacted to lettered UI elements (a shopping app's home,
results and product pages, a system settings page, two screens of a simple app), several tasks per
screen, plus 4 short general questions with four options. Screens and tasks are labelled
generically below (`s1`..`s6` the screen, `t` the task, `q` a question). Each cell pair ran in one
session of the phone, routed first and whole layers second, without cooling between them, with
`--decide-probe` on in both. Each decision is a single prefill graph (no prefix state with a
prefill device). Cells were run with the flag that then enabled the routed arena
(`--prefill-routed`, now the default).

## Summary

| model | decisions | experts routed per layer | whole layers | routed | reads, whole to routed | identical |
|---|---:|---:|---:|---:|---:|---:|
| Qwen3.6-35B-A3B Q4_0 | 35 | ~50% of 256 | 7.68 s | 4.16 s | 17.0 to 9.2 GiB | 35 of 35 |
| Qwen3.6-35B-A3B Q4_K_M | 18 | ~50% of 256 | 9.95 s | 5.37 s | 18.2 to 9.9 GiB | 18 of 18 |
| Gemma 4 26B-A4B Q4_K_M | 18 | ~58% of 128 | 6.69 s | 3.70 s | 14.1 to 8.7 GiB | 18 of 18 |
| Nemotron 3.5 30B-A3B Q4_0 | 18 | ~79% of 128 | 7.42 s | 6.81 s | 15.4 to 13.8 GiB | 18 of 18 |

Times are medians over every decision but the first (which has no prediction). The 18-decision
cells take every second decision of the 35. "Identical" compares `choice_logp` between the two
cells of a model.

## Qwen3.6-35B-A3B Q4_0, per decision

`tok` prompt tokens; `routed` and `demand` are (layer, expert) pairs: routed by the prefill, and
read at the routing node because the prediction (the previous decision's routing) missed them.

| decision | tok | whole s | routed s | MiB | wait s | routed | demand |
|---|---:|---:|---:|---:|---:|---:|---:|
| s1 t1 | 277 | 7.74 | 4.82 | 8972 | 3.57 | 5061 | 4773 |
| s1 t2 | 276 | 7.70 | 4.19 | 9548 | 2.92 | 5060 | 564 |
| s1 t3 | 278 | 7.72 | 4.10 | 9339 | 2.83 | 5044 | 442 |
| s1 t4 | 273 | 7.68 | 4.16 | 9348 | 2.90 | 5035 | 463 |
| s1 t5 | 276 | 7.69 | 4.11 | 9388 | 2.85 | 5096 | 496 |
| s1 t6 | 273 | 7.70 | 4.15 | 9378 | 2.89 | 5055 | 429 |
| s1 t7 | 274 | 7.65 | 4.10 | 9373 | 2.84 | 5076 | 467 |
| s1 t8 | 276 | 7.68 | 4.20 | 9391 | 2.95 | 5058 | 457 |
| s1 t9 | 273 | 7.67 | 4.13 | 9415 | 2.86 | 5072 | 489 |
| s2 t1 | 476 | 7.68 | 4.80 | 10921 | 3.08 | 5913 | 1247 |
| s2 t2 | 459 | 7.66 | 4.91 | 11196 | 3.20 | 6024 | 596 |
| s2 t3 | 459 | 7.66 | 4.83 | 11046 | 3.16 | 5947 | 400 |
| s2 t4 | 459 | 7.66 | 4.81 | 11079 | 3.16 | 5978 | 493 |
| s2 t5 | 464 | 7.66 | 4.79 | 11043 | 3.12 | 5996 | 438 |
| s2 t6 | 460 | 7.67 | 4.81 | 11023 | 3.11 | 5955 | 412 |
| s2 t7 | 458 | 7.67 | 4.76 | 10947 | 3.10 | 5955 | 404 |
| s2 t8 | 456 | 7.67 | 4.78 | 10946 | 3.11 | 5927 | 406 |
| s2 t9 | 458 | 7.64 | 4.74 | 10892 | 3.08 | 5919 | 397 |
| s3 t1 | 335 | 7.72 | 4.78 | 10983 | 3.37 | 5431 | 551 |
| s3 t2 | 299 | 7.72 | 4.59 | 10037 | 3.30 | 5170 | 482 |
| s3 t3 | 297 | 7.67 | 4.19 | 9542 | 2.87 | 5202 | 451 |
| s3 t4 | 297 | 7.70 | 4.21 | 9535 | 2.91 | 5216 | 415 |
| s3 t5 | 297 | 7.69 | 4.19 | 9563 | 2.86 | 5241 | 418 |
| s4 t1 | 132 | 7.70 | 4.14 | 9416 | 3.20 | 3922 | 305 |
| s4 t2 | 131 | 7.78 | 3.28 | 7226 | 2.34 | 3903 | 335 |
| s4 t3 | 133 | 7.78 | 3.26 | 7178 | 2.35 | 3920 | 325 |
| s5 t1 | 129 | 7.68 | 3.28 | 7417 | 2.35 | 4001 | 449 |
| s5 t2 | 129 | 7.67 | 3.27 | 7338 | 2.36 | 3871 | 321 |
| s6 t1 | 206 | 7.69 | 3.75 | 8560 | 2.64 | 4658 | 1169 |
| s6 t2 | 210 | 7.68 | 3.83 | 8670 | 2.70 | 4709 | 448 |
| s6 t3 | 210 | 7.66 | 3.76 | 8561 | 2.65 | 4724 | 333 |
| q1 | 49 | 7.66 | 3.88 | 8863 | 3.32 | 2304 | 495 |
| q2 | 58 | 7.72 | 2.63 | 5754 | 2.03 | 2517 | 1084 |
| q3 | 59 | 7.68 | 2.75 | 6054 | 2.12 | 2757 | 1048 |
| q4 | 55 | 7.66 | 2.88 | 6389 | 2.26 | 2631 | 1004 |
