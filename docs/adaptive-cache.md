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
  is a harmless no-op. On by default (`--dense-weights warm`); `--dense-weights mmap` disables the
  sweep for A/B runs, and `--dense-weights anon` replaces it with an O_DIRECT read into anonymous
  buffers, which is the better answer once the model is well past RAM.
  The warm-up is deliberately kept *out* of the budget: it only pre-faults the mmap-resident pages,
  it does not pin or reserve them, so the expert-cache budget above is unchanged and its hit rate is
  identical with and without it. (An alternative that folds the dense bytes into the floor —
  reserving RAM so the expert cache can never evict them — was measured and rejected: on a
  cache-sensitive model it lowers the budget and the hit rate, e.g. Gemma budget 4000→2909 MiB, hit
  83%→73%, trading throughput for OOM headroom that the warm-up already avoids needing. See
  [bench-data/2026-07-14-warmup/](bench-data/2026-07-14-warmup/).)
- **During generation, nothing resizes it.** `auto` is one shot at load, not a control loop: the
  budget chosen at init is held for the whole run. A runtime governor that tracked free RAM and
  shrank the budget under pressure did exist and was **retired** — it was measured a net loss on
  the models it was built for (see [pressure.md](pressure.md)). The `moe-cache:` summary reports the
  budget and what actually stayed resident:

  ```
  moe-cache: 77.1% hit, resident 4000.0 MiB
  ```

Because expert reads use O_DIRECT they never enter the page cache, so the only large pinned
allocation the engine controls *is* this budget — shrinking it is what actually hands RAM back to
the rest of the system.

> **The budget is not only a throughput knob — it is what the kernel judges you by.** On Android the
> LRU promotes a page to the protected list only on a *second* reference, and a cache hit is that
> second reference: a cache with a high hit rate defends itself, one with a low hit rate is correctly
> read as cold and reclaimed. Measured on gpt-oss-120b, where 3000 MiB covers 5.2% of the expert bank
> and returns a 13% hit, the cache is taken back *while decoding* and the fight costs far more than
> the hits are worth. `MemAvailable` also over-states the headroom here, since it counts the page
> cache holding this model's own dense weights as free. Before trusting `auto` on a model whose
> expert set dwarfs the budget, read [android-memory.md](android-memory.md).

> **`auto` sizes from a signal that lies, so keep it modest.** `auto` reads `MemAvailable`, which
> reports memory the device will not actually concede (it counts the model's own mmap'd weights as
> free), so it over-asks — and an over-ask is not a wasted budget but a running fight. The runtime
> governor that once tried to correct this from the other end (`--cache-dynamic`) was retired as a
> net loss (see [pressure.md](pressure.md)); `auto` now sizes **once at load** and stays fixed, so
> bound it with `--cache-ceil-mb` on a model whose expert set dwarfs the device, or use cache-off.

## Flags

| Flag | Meaning |
|---|---|
| `--cache-mb auto` | size the cache to the device instead of a fixed MiB (mutually exclusive with a numeric `--cache-mb`) |
| `--cache-floor-mb N` | RAM to leave free for the rest of the system when auto-sizing (default 1536) |
| `--cache-ceil-mb N` | upper bound on the auto-sized budget (0 = no cap). Use it — uncapped `auto` over-asks |
| `--dense-weights mmap\|warm\|anon` | the dense (non-expert) weight policy. `warm` (default) is the load-time page-cache sweep described above; `mmap` skips it; `anon` reads the dense set via O_DIRECT into anonymous buffers instead, which is the right answer well past RAM — see [benchmarks-gpt-oss.md](benchmarks-gpt-oss.md). `--no-warm-dense` and `--dense-odirect` are deprecated aliases for `mmap` and `anon` |

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
