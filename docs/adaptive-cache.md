# Adaptive cache sizing

The expert cache budget (`--cache-mb`) is the dominant throughput lever, but the right value is
device- and model-specific: too small and the hit rate collapses; too large and the pinned cache
plus the mmap-resident model push `MemAvailable` to zero and the Android low-memory-killer takes
the process (this is exactly why Gemma cannot use a 4000 MiB cache on an 11 GB phone — see
[benchmarks.md](benchmarks.md)). `--cache-mb auto` removes the guess: the engine sizes the cache to
the device and keeps it there as free memory moves.

## What it does

- **At init**, once the full expert-set size is known, the budget is set to
  `available_RAM − cache_floor_mb`, clamped to `[cache_min_mb, total expert bytes]`. Available memory
  is read from the platform (`/proc/meminfo` `MemAvailable` on Linux/Android, `GlobalMemoryStatusEx`
  on Windows); if it is unknown the budget falls back to the `cache_min_mb` floor.
- **Also at init**, the dense (non-expert) regions of the gguf — header, embeddings, attention,
  norms, lm_head, the tensors the streamer leaves mmap-resident — are warmed into the page cache with
  one sequential buffered sweep (reported as `bmoe: dense warm-up`), so the first tokens do not pay
  for them as random 4 KiB faults. On a model far larger than RAM this is the difference between a
  fast first token and a ~20-token slow-start ramp: measured on gpt-oss-120b, the first-five-token
  wall average drops ~20× (see [benchmarks.md](benchmarks.md)). On models whose dense set is small it
  is a harmless no-op. On by default; `--no-warm-dense` disables the sweep for A/B runs.
  The warm-up is deliberately kept *out* of the budget: it only pre-faults the mmap-resident pages,
  it does not pin or reserve them, so the expert-cache budget above is unchanged and its hit rate is
  identical with and without it. (An alternative that folds the dense bytes into the floor —
  reserving RAM so the expert cache can never evict them — was measured and rejected: on a
  cache-sensitive model it lowers the budget and the hit rate, e.g. Gemma budget 4000→2909 MiB, hit
  83%→73%, trading throughput for OOM headroom that the warm-up already avoids needing. See
  [bench-data/2026-07-14-warmup/](bench-data/2026-07-14-warmup/).)
- **Pinning dense (`--lock-dense`) pays the same price, and the budget is where it is paid.** The
  flag mlocks the dense ranges so expert traffic cannot evict them; because pinned pages leave
  `MemAvailable`, and the lock runs *before* the probe above, the budget lands exactly where the
  rejected reserve landed (Gemma: 2909 MiB). That is not an accounting artifact to engineer away —
  the RAM genuinely has to come out of the cache, and probing before the lock instead would only
  hand the same bill to the runtime re-probe. The flag earns its keep only when cache hit rate is
  cheap to sell: gpt-oss-120b's budget holds ~5 % of its expert set and hits 13–32 %, while it faults
  thousands of dense pages per token. Decide with `majflt/tok` and `locked_dense_MiB` in the summary.
- **During generation**, on the eval thread inside each layer's cache-management window, a throttled
  re-probe (about every 128 layer loads, ≈2–3 tokens — one `/proc` read) tracks free RAM: if it dips
  under the floor the budget shrinks by the shortfall and the normal eviction pass drains the cache
  to it; when memory recovers the budget grows back toward the init size, with hysteresis so it does
  not oscillate. The `moe-cache:` summary reports the live budget and how many times it changed:

  ```
  moe-cache: 74.9% hit, resident 3812.4 MiB, budget 4000 MiB (auto, resized 2×)
  ```

Because expert reads use O_DIRECT they never enter the page cache, so the only large pinned
allocation the engine controls *is* this budget — shrinking it is what actually hands RAM back to
the rest of the system.

## Flags

| Flag | Meaning |
|---|---|
| `--cache-mb auto` | size the cache to the device instead of a fixed MiB (mutually exclusive with a numeric `--cache-mb`) |
| `--cache-floor-mb N` | RAM to leave free for the rest of the system when auto-sizing (default 1536) |
| `--no-warm-dense` | skip the load-time sweep that page-caches the non-expert weights |
| `--lock-dense` | pin the non-expert weights in RAM so expert traffic cannot evict them (off by default; spends cache budget — see above) |

`auto` is a real LRU cache, so it satisfies the cache requirement of `--prefetch`.

## Explicit control

Embedders that link the engine can also resize the cache directly with
`Session::set_cache_budget_mb(int)` — for an app's own memory-pressure callback. It must be called
between generations (never during a decode); it evicts to the new budget immediately. The Android
example instead relies on the automatic tracking above, because it runs `bmoe-cli` as a subprocess
that reads `/proc/meminfo` itself.

## Gate

**S3** proves a runtime resize is byte-safe: it opens a session with a warm cache, drops the budget
to force a full eviction, and asserts the next generation still matches the resident reference —
only residency changes, never the produced bytes.
