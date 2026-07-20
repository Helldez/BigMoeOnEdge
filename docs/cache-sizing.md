# Expert cache sizing

The expert cache budget (`--cache-mb`) is the dominant throughput lever, but the right value is
device- and model-specific: too small and the hit rate collapses **to exactly zero — see the
cliff below, it does not degrade gracefully**; too large and the pinned cache
plus the mmap-resident model push `MemAvailable` to zero and the Android low-memory-killer takes
the process (this is exactly why Gemma cannot use a 4000 MiB cache on an 11 GB phone — see
[benchmarks.md](benchmarks.md)). `--cache-mb auto` removes the guess: the engine sizes the cache to
the device once at load and holds that budget for the whole run.

## The cliff: below one token cycle the hit rate is 0 %, not "low"

Global LRU drops the least *recently* used entry. The model visits its layers in a fixed cycle
0..N-1, so the least recently used entry is also the **soonest to be needed again**. While the
budget holds a whole token cycle the cold end is genuinely stale history and this never shows.
Once it does not, the cache evicts precisely what it is about to read, and the hit rate does not
taper — it goes to **exactly 0.0 %**.

Measured on device (2026-07-20): gpt-oss-120b at top-4 has a `token_demand_MiB` of 1815, and a
**1500 MiB** budget returns 0.0 % hit while reading 1817.05 MiB/token — *identical to running with
no cache at all* (1817.0) — and still spends 0.233 s/token, 24 % of the token, managing it, on top
of holding 1500 MiB of RAM. Dropping the budget 2000 → 1500 does not cost a quarter of the hit
rate; it costs all of it.

Two traps worth knowing:

- **1500 is a legal value.** It is exactly `cache_min_mb`; no `--force-cache` is needed. Today's
  only protection is that this floor happens to sit above the cycle for the shipped models at
  their default top-k. Raise `--n-expert-used` and the cycle moves above the floor (gpt-oss: 908
  MiB at k=2, 1815 at k=4) and the protection silently stops holding.
- **The existing floor is a different quantity.** `layer_demand_MiB` (the widest single layer, 50.4
  on that run) is the mechanical minimum; the cliff is at `token_demand_MiB` (1815). They are
  roughly `n_layer` apart. Both are already printed in the `# summary` line; nothing sizes from them.

So when a budget is being chosen by hand, check it against `token_demand_MiB` from a previous run
of the same model *and top-k*, not against `cache_min_mb`. Below the cycle, `--cache-mb 0` is
strictly better than a cache: same reads, none of the RAM, none of the management cost.

Fixing this by changing the eviction policy was tried and rejected — a per-layer partition is
immune to the cliff by construction but costs ~30 % throughput
([bench-data/2026-07-20-cache-replay/layer-lfu-verdict.md](bench-data/2026-07-20-cache-replay/layer-lfu-verdict.md)).
The cheap fix is a guard: the worst-case cycle is computable at init from model shape alone.

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
  is a harmless no-op. This sweep is the `--dense-weights warm` policy; `--dense-weights mmap`
  disables it for A/B runs, and `--dense-weights anon` (the default) replaces it with an O_DIRECT
  read into anonymous buffers, which is the better answer once the model is well past RAM — the
  case the engine targets.
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

Because expert reads use O_DIRECT they never enter the page cache, shrinking this budget is what
actually hands expert RAM back to the rest of the system. It is not the engine's only large
allocation, though: under `--dense-weights anon` (the default) the whole dense set also lives in
anon buffers. That footprint is **not** part of the cache budget and is not subtracted from it —
see the ordering warning below.

> **The budget is not only a throughput knob — it is what the kernel judges you by.** On Android the
> LRU promotes a page to the protected list only on a *second* reference, and a cache hit is that
> second reference: a cache with a high hit rate defends itself, one with a low hit rate is correctly
> read as cold and reclaimed. Measured on gpt-oss-120b, where 3000 MiB covers 5.2% of the expert bank
> and returns a 13% hit, the cache is taken back *while decoding* and the fight costs far more than
> the hits are worth. `MemAvailable` also over-states the headroom here, since it counts the page
> cache holding this model's own dense weights as free. Before trusting `auto` on a model whose
> expert set dwarfs the budget, read [android-memory.md](android-memory.md).

> **`auto` sizes before the dense weights are allocated.** The budget is chosen in
> `ExpertStreamSource::init` as soon as the expert-set size is known
> (`core/src/moe/expert_stream_source.cpp:68`); the dense policy runs later in the same init
> (`:200`), and under the default `anon` mode it then allocates the entire dense set into anon
> buffers. So the `MemAvailable` reading `auto` sizes from still counts that RAM as free. On a model
> with a large dense set the over-ask is roughly the dense size — `--cache-ceil-mb` is the lever that
> bounds it, which is why the Android example ships a 3000 MiB ceiling by default.

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
| `--dense-weights mmap\|warm\|anon` | the dense (non-expert) weight policy. `warm` is the load-time page-cache sweep described above; `mmap` skips it; `anon` (default) reads the dense set via O_DIRECT into anonymous buffers instead, which is the right answer well past RAM — see [benchmarks-gpt-oss.md](benchmarks-gpt-oss.md). `--no-warm-dense` and `--dense-odirect` are deprecated aliases for `mmap` and `anon` |

`auto` is a real LRU cache, so it satisfies the cache requirement of `--prefetch`.

## Explicit control

Embedders that link the engine can also resize the cache directly with
`Session::set_cache_budget_mb(int)` — for an app's own memory-pressure callback. It must be called
between generations (never during a decode); it evicts to the new budget immediately. The Android
example does not use it: it runs `bmoe-cli` as a subprocess, so its "Auto" cache setting simply
passes `--cache-mb auto` and the load-time sizing above applies.

## Gate

**S3** proves a runtime resize is byte-safe: it opens a session with a warm cache, drops the budget
to force a full eviction, and asserts the next generation still matches the resident reference —
only residency changes, never the produced bytes.
