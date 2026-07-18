# Per-token warm-up dynamics

A single mean tok/s hides a transient. Every streamed run starts cold — an empty
expert cache and a resident working set that is not yet faulted into RAM — and only
climbs to its steady rate after a warm-up window. The per-token CSV (`--csv`) exposes
that window token by token, and it turns out the *shape* of the warm-up depends on one
ratio: **model size vs. device RAM**.

This note reads the warm-up out of the CSVs for two regimes measured on the reference
device (OnePlus 15R, 11.3 GB RAM, UFS 4.x):

- **Models near RAM** — Qwen3-30B-A3B (18.5 GB, ≈1.6× RAM) and Gemma-4-26B-A4B
  (17.0 GB, ≈1.5× RAM): warm-up is **I/O-bound** and the overlap path largely hides it.
- **A model dwarfing RAM** — gpt-oss-120b (58.5 GB, ≈5.2× RAM): warm-up is
  **memory-residency-bound** and surfaces inside compute, where overlap cannot hide it.

The takeaway is not new headline numbers — the headline best-cases live in
[benchmarks.md](benchmarks.md). It is *why* a short generation, or a >>RAM model, reads
slower than the steady-state table, and why that is expected rather than a regression.

## Method

- **Recipe:** the winning config — `--moe-stream --cache-mb auto --cache-ceil-mb 4000
  --io-threads 4 -t 4 --overlap` for Qwen/Gemma; the documented gpt-oss recipe
  (`--cache-ceil-mb 3000`, `--no-think`) for gpt-oss. Greedy decode.
- **Length:** Qwen/Gemma at `-n 256` (their steady-state protocol); gpt-oss at `-n 24`
  (its exploratory-probe protocol — see the gpt-oss caveats in [benchmarks.md](benchmarks.md)).
- **Data:** the raw per-token CSVs are in
  [`bench-data/2026-07-14/warmup/`](bench-data/2026-07-14/warmup/).

Per-token CSV schema (one row per generated token):

| column | meaning |
|---|---|
| `wall_ms` | real time for this token — the number that becomes tok/s (`1000 / wall_ms`) |
| `compute_ms` | FFN + attention compute; **absorbs synchronous page-fault stalls** on resident weights (a residual — `majflt`/`cpu_ms` below tell you which) |
| `io_ms` | expert flash-read time, summed across the read lanes (so it can exceed `wall_ms` when overlapped) |
| `read_bytes` | expert bytes streamed from flash this token |
| `cache_hit_pct` | running LRU expert-cache hit rate |
| `stall_ms` | residual flash wait not hidden by overlap |
| `mgmt_ms` | cache bookkeeping (eviction/admission) |
| `majflt` | major page faults this token — non-zero means dense weights re-faulted from flash *inside* the decode (the Regime 2 stall, made explicit instead of hiding in `compute_ms`) |
| `cpu_ms` | CPU time summed across threads; `cpu_ms / (wall_ms × threads)` is occupancy — near 1 is compute-bound, well below flags a throttled/preempted core |

## Regime 1 — models near RAM: I/O-bound warm-up

Here the resident, non-streamed part of the model (attention, embeddings, router, norms,
plus Gemma's always-on shared expert) fits comfortably in RAM, so `compute_ms` is roughly
flat from the first token. The only thing that warms up is the **expert cache**: it starts
empty, so early tokens read the full routed set from flash (~670 MiB) and climb toward the
steady hit rate; late tokens are mostly cache hits and read a fraction of that. The overlap
path pipelines those reads behind compute, so tok/s recovers within a few tokens.

**Qwen3-30B-A3B — k=8** (mean 4.92 tok/s, steady hit 77%):

| step | wall_ms | compute_ms | cache_hit% | tok/s |
|---:|---:|---:|---:|---:|
| 1 | 742 | 521 | 4.5 | 1.35 |
| 2 | 252 | 86 | 10.2 | 3.97 |
| 3 | 276 | 87 | 14.2 | 3.62 |
| … | | | | |
| 254 | 200 | 102 | 77.1 | 5.00 |
| 255 | 151 | 100 | 77.2 | 6.62 |

**Qwen3-30B-A3B — k=6** (mean 7.01 tok/s, steady hit 80%):

| step | wall_ms | compute_ms | cache_hit% | tok/s |
|---:|---:|---:|---:|---:|
| 1 | 311 | 176 | 4.8 | 3.22 |
| 2 | 170 | 75 | 10.4 | 5.88 |
| 3 | 193 | 78 | 14.3 | 5.19 |
| … | | | | |
| 254 | 99 | 88 | 79.9 | 10.08 |
| 255 | 131 | 89 | 79.9 | 7.63 |

**Gemma-4-26B-A4B — k=8** (mean 4.76 tok/s, steady hit 83%):

| step | wall_ms | compute_ms | cache_hit% | tok/s |
|---:|---:|---:|---:|---:|
| 1 | 565 | 394 | 5.4 | 1.77 |
| 2 | 318 | 118 | 9.5 | 3.15 |
| 3 | 211 | 108 | 15.5 | 4.74 |
| … | | | | |
| 254 | 197 | 122 | 82.8 | 5.08 |
| 255 | 190 | 153 | 82.8 | 5.27 |

**Gemma-4-26B-A4B — k=6** (mean 4.79 tok/s, steady hit 83%):

| step | wall_ms | compute_ms | cache_hit% | tok/s |
|---:|---:|---:|---:|---:|
| 1 | 3198 | 2961 | 6.5 | 0.31 |
| 2 | 253 | 120 | 11.7 | 3.96 |
| 3 | 184 | 110 | 17.5 | 5.43 |
| … | | | | |
| 253 | 167 | 117 | 82.7 | 5.98 |
| 255 | 425 | 404 | 82.8 | 2.35 |

Reading these:

- **`cache_hit%` climbs 4.5% → 77–83%.** That *is* the warm-up. Early tokens read the full
  routed set (~670 MiB); by steady state most experts are cached and each token reads
  ~100–200 MiB.
- **`compute_ms` is essentially flat (~85–170 ms)** — no fault storm, because the resident
  working set fits. Contrast this with Regime 2 below.
- **tok/s recovers by token ~3** and reaches the steady rate the headline table reports.
  Qwen k=6 even averages 7.01 tok/s here, above the documented 5.01 best-case, because this
  session's device ran a touch cooler than the reference run.
- The lone `gemma_k6` token-1 spike (3.2 s / 2961 ms compute) is a cold outlier: this run
  executed **last** in the batch, on the warmest device with a 19 s model load; it settles
  from token 2. It is a good reminder that the very first token also pays first-touch faults
  on the resident set — small here, catastrophic in Regime 2.

## Regime 2 — a model dwarfing RAM: memory-residency-bound warm-up

gpt-oss-120b is 5.2× device RAM. Streaming still bounds the **expert** memory correctly —
experts are read via O_DIRECT straight into the bounded cache (`resident ≈ 2988 MiB`,
budget 3000), bypassing the page cache. But the model is loaded `use_mmap=true`, so the
**non-expert** resident set lives as reclaimable, file-backed pages. With free RAM near
zero, the kernel evicts those clean pages under pressure and they must be **re-faulted from
flash during the FFN compute** — a synchronous major fault that the `compute_ms` timer
absorbs. Overlap cannot hide this: it hides *flash I/O*, and this stall is inside *compute*.

**gpt-oss-120b — k=2 (io4)** — 24-token probe:

| step | wall_ms | compute_ms | io_ms | cache_hit% | tok/s |
|---:|---:|---:|---:|---:|---:|
| 1 | 18444 | 18058 | 1435 | 0.44 | 0.05 |
| 2 | 12048 | 11697 | 1119 | 1.32 | 0.08 |
| 3 | 10765 | 10498 | 753 | 2.99 | 0.09 |
| … | | | | | |
| 22 | 909 | 650 | 675 | 19.41 | 1.10 |
| 24 | 1244 | 983 | 630 | 20.36 | 0.80 |

**gpt-oss-120b — k=4 (io8)** — 24-token probe:

| step | wall_ms | compute_ms | io_ms | cache_hit% | tok/s |
|---:|---:|---:|---:|---:|---:|
| 1 | 18534 | 17859 | 4316 | 0.31 | 0.05 |
| 2 | 17781 | 17205 | 3386 | 1.81 | 0.06 |
| 3 | 15984 | 15421 | 2964 | 3.54 | 0.06 |
| … | | | | | |
| 22 | 716 | 231 | 2609 | 12.82 | 1.40 |
| 24 | 1104 | 593 | 2688 | 13.42 | 0.91 |

Reading these:

- **`compute_ms` collapses ~18 000 ms → ~300 ms** over the run. That is not thermal
  throttling (which worsens over time) and not the expert stream (`io_ms` stays small): it
  is the resident working set faulting in under memory pressure and then staying hot.
- **The 24-token mean is dominated by the cold head**, so it is a *floor*. The steady tail
  is far faster — the last third averages ~0.74 tok/s (k=2) and ~1.2 tok/s (k=4).
- **k=4, steady tail:** note `io_ms` ≈ 2600–3800 ms while `wall_ms` ≈ 800 ms — the overlap
  path is hiding the entire (large) expert I/O behind the now-warm compute. Direct evidence
  that overlap works; without it, `wall_ms` would track `io_ms`.

> **Honest caveat on the gpt-oss absolute numbers.** These two runs were captured at the end
> of a long benchmarking session, on a thermally-degraded and memory-dirty device: Thermal
> Status 2 (moderate throttle), CPU pinned at ~1.9 GHz, and ~2.4 GB of zram still in use at
> idle. Their **means (0.163 / 0.134 tok/s) are far below the current numbers in
> [benchmarks-gpt-oss.md](benchmarks-gpt-oss.md) (2.191 tok/s at k=2)** and should not be read as
> throughput figures. They are included only to show the warm-up *shape*; a clean headline number
> needs a cool, freshly-booted device.

## The fix — dense warm-up

Regime 2 is not inherent: the cold head exists only because the non-expert pages fault in
*lazily*, one random 4 KiB page at a time, during the first decodes. Reading them **eagerly and
sequentially at load** removes it. The engine now does exactly that — one buffered sweep over the
file's non-expert byte ranges (the complement of the expert tensor offsets) right after the streamer
initialises, before the first token. Sequential flash bandwidth turns thousands of scattered major
faults into a single ~1 s read, paid once inside `load_seconds` instead of inside `compute_ms`.

It is the `--dense-weights warm` policy (`--dense-weights mmap` disables it; `anon`, the default,
replaces it with an O_DIRECT read into anon buffers) and touches neither the expert cache nor the
budget, so the streaming path is byte-for-byte unchanged — `cache_hit%` is identical step-for-step
with and without it.

> **Superseded well past RAM.** The warm sweep front-loads the dense pages into the *page cache*,
> which fixes the cold head but not what follows: at 5.2× RAM the kernel reclaims those pages
> mid-decode and they refault for the rest of the run. `--dense-weights anon` reads the same bytes
> into anonymous buffers instead, so they survive — major faults per token drop from the hundreds to
> 6–10. On a model that far past RAM, prefer `anon` over `warm`; see
> [benchmarks-gpt-oss.md](benchmarks-gpt-oss.md).

Re-measured on the same device, warm-up on vs. the old lazy-fault binary
([`bench-data/2026-07-14-warmup/`](bench-data/2026-07-14-warmup/)):

| model | first-5 wall avg | tok/s | hit% | budget |
|---|---|---|---|---|
| gpt-oss-120b k4 | 16641 → **829 ms (20×)** | 0.134 → 1.150 | 13.4 = 13.4 | 3000 = 3000 |
| Qwen3-30B-A3B k8 | 367 → 399 ms | 4.92 → 4.57 | 77.1 = 77.1 | 4000 = 4000 |
| Gemma-4-26B-A4B k8 | 329 → 397 ms | 4.76 → 4.76 | 82.9 = 82.9 | 4000 = 4000 |

The Regime-2 cold head collapses ~20× on gpt-oss-120b; the first token drops from ~18 s to ~1 s.
Regime-1 models are neutral (their dense set is ~1 GB and the kernel pages it in fast regardless),
with no change to hit rate or budget — confirming the warm-up only moves cost out of the hot path.

**Why not just reserve the dense pages in the budget?** The obvious alternative — subtract the dense
bytes from the auto-cache floor so the expert cache can never evict them — was measured and rejected.
It lowers the budget on a cache-sensitive model and, with it, the hit rate (Gemma:
budget 4000→2909 MiB, hit 83%→73%, tok/s 4.76→3.76), trading throughput for OOM headroom the warm-up
makes unnecessary. The warm-up pre-faults the same pages without touching the budget, so it keeps the
full hit rate — a strictly better trade on every model measured (`reserve_gemma_k8.csv` alongside the
data above).

## Takeaways

- **The >>RAM cold head is fixed, not inherent.** Dense warm-up front-loads the non-expert working
  set at load, collapsing Regime 2's first-token stall ~20× (gpt-oss-120b: ~18 s → ~1 s). The points
  below describe the *un-warmed* dynamics the fix addresses.
- **Streaming bounds memory correctly regardless of model size.** Experts always stream via
  O_DIRECT into the fixed cache; the 62 GB model never loads into RAM. What is *not* bounded
  by streaming is the mmap-resident, non-expert working set — and that is what thrashes once
  the model-to-RAM ratio leaves no free RAM to hold it.
- **Warm-up cost scales with the model/RAM ratio.** Near RAM it is a gentle, I/O-bound climb
  the overlap path hides. Well past RAM it is a steep, memory-bound stall inside compute that
  nothing hides — so a short probe measures mostly warm-up.
- **For a headline tok/s, report a steady window** (discard the warm-up tokens) from a cool,
  freshly-booted device. For a >>RAM model this matters most, because the cold head can be
  the majority of a short run.
- **Practically:** brief replies never leave the warm-up window, so in-app tok/s on short
  turns will sit below the steady-state table — expected, not a regression.
