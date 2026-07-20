# Flash read ceiling vs lane count and read size

Measured with `tools/bmoe-iobench` (new standalone tool: drives `bmoe::FileReader`, the engine's
own read path, but links nothing else — no engine, no llama.cpp). Random O_DIRECT reads against
the 62 GB gpt-oss-120b gguf on `/data/local/tmp`, 5 s per row, entry state 2.27 GHz / 32 °C.

This is the microbenchmark issue #75 asks for before building a lane pool: if bandwidth is
already flat at the current lane count, the pool is not worth building.

## Lane sweep, 4 MiB reads

| lanes | MiB/s | mean latency |
|---:|---:|---:|
| 1 | 1809.9 | 2.21 ms |
| **2** | **2459.8** | 3.25 ms |
| 4 | 2457.9 | 6.50 ms |
| 8 | 2420.0 | 13.20 ms |
| 12 | 2360.1 | 20.29 ms |
| 16 | 2243.9 | 28.42 ms |
| 24 | 2308.6 | 41.40 ms |
| 32 | 2254.8 | 56.42 ms |

**Bandwidth saturates at 2 lanes.** Past that, throughput is flat-to-declining while latency
grows linearly with lane count — the textbook saturation signature: extra lanes only queue.

## Read-size sweep

MiB/s, by read size and lane count:

| read size | 2 lanes | 4 lanes | 8 lanes |
|---|---:|---:|---:|
| 128 KiB | 953.6 | 1486.1 | 1858.8 |
| 256 KiB | 1536.4 | 2099.4 | 2144.2 |
| 512 KiB | 1882.4 | 2114.0 | 2108.2 |
| 1 MiB | 2084.8 | 2235.3 | 2251.4 |
| 2 MiB | 2043.7 | 2155.4 | 2063.5 |
| 4 MiB | 2172.0 | 2205.7 | 2263.0 |
| 8 MiB | 2529.2 | 2558.1 | 2474.1 |

Above ~256 KiB the device is essentially flat at 2100-2550 MiB/s. Only sub-256 KiB reads lose
bandwidth, and the streamer's slices are MiB-class, so read size is not a lever either.

## What this means

**#75's premise does not hold on this device.** Neither more lanes nor bigger reads buy
throughput: `io_threads_max = 8` is already past saturation, and raising it would cost one fd
plus one bounce buffer per lane for nothing. The earlier 4→8 lane gain (+9.7 %, measured the
same day) is real but is *not* bandwidth — at 4 lanes the engine's duty cycle is low enough
that extra lanes fill gaps rather than add device throughput.

**How far the engine is from the ceiling is NOT settled by these numbers.** The engine at 8
lanes moves 587.39 MiB/token in 0.414 s = 1419 MiB/s, which looks like 58 % of the 2460 MiB/s
above — but the two were measured in different thermal states and **the comparison does not
hold**. The sweep above ran on a cold device (2.27 GHz, 32 °C); the engine cell entered at
44 °C and throttled to 2.19 GHz mid-run. Repeating the sweep later the same session, after an
hour of benching (2.02 GHz, 35 °C), the same rows peaked at **1645 MiB/s, not 2460** — i.e. the
device ceiling itself falls by a third under exactly the conditions a long decode creates. A
1419 MiB/s engine against a same-state ceiling of ~1645 is a far smaller gap than against 2460,
and possibly not a gap worth chasing.

**Owed:** an interleaved re-measure — engine cell, then `bmoe-iobench`, back to back at the same
entry state — is the only way to size the duty-cycle gap honestly. Until then, treat "the engine
leaves bandwidth on the table" as unproven.

A related contention hypothesis (the lanes are starved of CPU by ggml's compute threads, which
would make #75's "compute-isolated" framing the real lever rather than its queue-depth framing)
was tested with `--compute-load 2` and `--compute-load 4` and came back **inconclusive** — the
rows were noisy and the device was already throttling. It needs a cold device to answer.

## Temporal prefetch is a 2x loss on gpt-oss

Tested because it is the mechanism that would fill idle read capacity: `--prefetch 1` on the
`lanes8_k2` configuration (cache 2000, 8 lanes, top-2). Stopped at token 222 of 256 once the
result was unambiguous — per-token wall was **828-895 ms against the 414 ms baseline**, and the
cache hit rate did not improve (31.2 % vs 32.3 %).

This is what the 2026-07-15 predictor table already implied: temporal prefetch guesses the next
token's routing from the previous token's, and on gpt-oss that is right **17.9 %** of the time —
worse than a static hot list (26.7 %). At top-2 a full read-ahead of one layer speculates
908 MiB/token against 587 MiB/token of real demand, so a 1-in-6 hit rate cannot pay for itself.

The signal that does exist is **popularity, not recency** — which is an argument for a
frequency-aware cache policy (see `findings.md`, where `layer+lfu` wins), not for speculation.

## Reproduce

```
cmake -S . -B build-tools -DBMOE_BUILD_TOOLS=ON -DBMOE_BUILD_TESTS=OFF -DBMOE_BUILD_CLI=OFF
cmake --build build-tools --target bmoe-iobench
bmoe-iobench --model MODEL.gguf --lanes 1,2,4,8,16,32 --slice-kb 4096 --seconds 5
```
