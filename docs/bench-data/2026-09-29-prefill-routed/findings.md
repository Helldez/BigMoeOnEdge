# `--prefill-routed` on the phone (2026-09-29)

A 12 GB phone with a Hexagon v81 NPU and UFS 4 storage. Qwen3.6-35B-A3B Q4_0 (20.8 GB), streamed,
with the settings an on-device Android UI agent runs in its NPU mode:

```
--session --decide --moe-stream --overlap --io-threads 4 --dense-weights ahwb -t 6 -c 2048 --chatml
--n-expert-used 4 --prefill-device HTP0 --ubatch 2048
GGML_HEXAGON_HOSTBUF=1 GGML_HEXAGON_OPPOLL=1
```

35 `decide` requests in one session: 31 over six Android screens compacted to lettered UI elements
(an online shop's home, results and product pages, a settings page, two screens of an app), several
tasks per screen, and 4 short general questions with four options. Both cells ran with
`--decide-probe` on, in the same session of the phone, one after the other, without cooling between
them: `routed` first (193 s), then `whole layers` (315 s). Each decision is a single prefill graph
(no prefix state with a prefill device).

| | whole layers | `--prefill-routed` |
|---|---:|---:|
| prefill, median of decisions 2 to 35 | 7.68 s | 4.16 s |
| arena reads, median | 17 360 MiB | 9 403 MiB |
| graph waiting on the arena, median | 6.41 s | 2.90 s |
| `choice_logp` identical | | 35 of 35 |

Routed experts the prediction (the previous decision's routing) missed and the arena read at the
routing node: 11% over decisions 2 to 35. The first decision has no useful prediction.

Per decision (`tok` prompt tokens; `routed` and `demand` are (layer, expert) pairs):

| decision | tok | whole s | routed s | MiB | wait s | routed | demand |
|---|---:|---:|---:|---:|---:|---:|---:|
| home_buy | 277 | 7.74 | 4.82 | 8972 | 3.57 | 5061 | 4773 |
| home_cart | 276 | 7.70 | 4.19 | 9548 | 2.92 | 5060 | 564 |
| home_account | 278 | 7.72 | 4.10 | 9339 | 2.83 | 5044 | 442 |
| home_address | 273 | 7.68 | 4.16 | 9348 | 2.90 | 5035 | 463 |
| home_scan | 276 | 7.69 | 4.11 | 9388 | 2.85 | 5096 | 496 |
| home_browse | 273 | 7.70 | 4.15 | 9378 | 2.89 | 5055 | 429 |
| home_essentials | 274 | 7.65 | 4.10 | 9373 | 2.84 | 5076 | 467 |
| home_leave | 276 | 7.68 | 4.20 | 9391 | 2.95 | 5058 | 457 |
| home_batteries | 273 | 7.67 | 4.13 | 9415 | 2.86 | 5072 | 489 |
| results_buy | 476 | 7.68 | 4.80 | 10921 | 3.08 | 5913 | 1247 |
| results_prime | 459 | 7.66 | 4.91 | 11196 | 3.20 | 6024 | 596 |
| results_players | 459 | 7.66 | 4.83 | 11046 | 3.16 | 5947 | 400 |
| results_dobble | 459 | 7.66 | 4.81 | 11079 | 3.16 | 5978 | 493 |
| results_newsearch | 464 | 7.66 | 4.79 | 11043 | 3.12 | 5996 | 438 |
| results_condition | 460 | 7.67 | 4.81 | 11023 | 3.11 | 5955 | 412 |
| results_filters | 458 | 7.67 | 4.76 | 10947 | 3.10 | 5955 | 404 |
| results_cart | 456 | 7.67 | 4.78 | 10946 | 3.11 | 5927 | 406 |
| results_back | 458 | 7.64 | 4.74 | 10892 | 3.08 | 5919 | 397 |
| product_buy | 335 | 7.72 | 4.78 | 10983 | 3.37 | 5431 | 551 |
| product_reviews | 299 | 7.72 | 4.59 | 10037 | 3.30 | 5170 | 482 |
| product_store | 297 | 7.67 | 4.19 | 9542 | 2.87 | 5202 | 451 |
| product_photos | 297 | 7.70 | 4.21 | 9535 | 2.91 | 5216 | 415 |
| product_search | 297 | 7.69 | 4.19 | 9563 | 2.86 | 5241 | 418 |
| app_settings | 132 | 7.70 | 4.14 | 9416 | 3.20 | 3922 | 305 |
| app_load | 131 | 7.78 | 3.28 | 7226 | 2.34 | 3903 | 335 |
| app_choose | 133 | 7.78 | 3.26 | 7178 | 2.35 | 3920 | 325 |
| now_unload | 129 | 7.68 | 3.28 | 7417 | 2.35 | 4001 | 449 |
| now_type | 129 | 7.67 | 3.27 | 7338 | 2.36 | 3871 | 321 |
| smoke_off | 206 | 7.69 | 3.75 | 8560 | 2.64 | 4658 | 1169 |
| smoke_code | 210 | 7.68 | 3.83 | 8670 | 2.70 | 4709 | 448 |
| smoke_qr | 210 | 7.66 | 3.76 | 8561 | 2.65 | 4724 | 333 |
| gen_geo | 49 | 7.66 | 3.88 | 8863 | 3.32 | 2304 | 495 |
| gen_math | 58 | 7.72 | 2.63 | 5754 | 2.03 | 2517 | 1084 |
| gen_code | 59 | 7.68 | 2.75 | 6054 | 2.12 | 2757 | 1048 |
| gen_bio | 55 | 7.66 | 2.88 | 6389 | 2.26 | 2631 | 1004 |
