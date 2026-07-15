# Rewarm after reclaim

A session keeps the model loaded and the expert cache warm so the second prompt does not re-pay the
first one's costs (see [session mode](telemetry.md)). Android disagrees. Its reclaim daemon targets
exactly what a session is: a process holding gigabytes of anonymous memory and doing nothing between
turns. Within seconds of a generation ending, that memory is compressed into zram — the cache is
still resident in the LRU's books, but its pages are gone.

Nothing announces this. The next prompt starts, the cache reports the same hit rate it always did,
and every hit touches a page that has to be decompressed back out of zram first — one major fault at
a time, per expert, per layer, for the whole answer. The result reads as a mysteriously slow turn on
a session that should have started warm.

## What it does

Before each generation, if the kernel has swapped out at least `--rewarm-threshold-mb` of this
process's memory, the engine restores it in one bulk pass:

- **Expert cache.** One `MADV_WILLNEED` per `(layer, projection)` buffer, in ascending address order.
  The kernel's swapin walk acts only on pages that actually hold a swap entry, so the reserved-but-
  uncommitted holes between cached experts cost a page-table skip and nothing else — no memory is
  committed for experts that were never cached. What comes back is what was taken.
- **Dense regions.** The same sequential sweep the load-time warm-up runs (see
  [adaptive-cache.md](adaptive-cache.md)). Dense weights are file-backed, so reclaim *drops* them
  rather than swapping them: `VmSwap` never accounts for them and re-reading the file is the only way
  back. Skipped when the session opted out of dense warming, so an A/B keeps measuring what it set
  out to.

Both restore where the cache's bytes live, never what they are — no entry is added, evicted or
invalidated. Gate **S4** asserts that: a session with a forced rewarm before every generate still
matches the resident reference byte for byte.

The pass is reported on stderr, in the run summary, and in `BMOE_DONE`'s `rewarm_s`/`rewarm_mib`; its
seconds belong to TTFT rather than to the decode:

```
bmoe: rewarm — 0 MiB back from swap in 0.5 s (67 MiB still swapped)
```

What comes back is measured, not assumed — the line above is a real capture of the pass finding
almost nothing to do, which is what a session that was left alone should report.

That is the intended trade: the same bytes move either way, but sequentially and once, before the
first token, instead of scattered across every token of the answer.

## What is measured, and what is still open

The reclaim and its cost are established. On a OnePlus 15R running gpt-oss-120b-Q4_K_M with the
Android example's own configuration (4096 ctx, fixed 2000 MiB cache, 4 lanes, top-2), two turns of a
session separated by a four-minute idle:

| | turn 1 | turn 2, after the idle |
|---|---|---|
| decode | 1.454 tok/s | 0.746 tok/s |
| major faults / token | 10.1 | 412.1 |
| CPU-seconds / token | 1.35 | 4.07 |
| flash I/O / token | 1.21 s | 0.91 s |

The signature is unmistakable: flash I/O *fell* while the turn halved, the time reappeared as
"compute" (0.36 → 1.00 s/token) and CPU per token tripled. That is not arithmetic, it is fault
handling. Meanwhile `VmSwap` rose from 337 MiB to 1.46 GiB and RSS fell from 3.80 to 1.69 GiB. The
Android app is hit far harder than this: its engine drops from 3.5 GiB resident to 3.5 MiB within
five seconds of a reply finishing, where an adb session takes minutes to lose half as much.

**What is not yet established is that restoring the swapped pages is what fixes it.** A second run
of the same A/B was reclaimed very differently — `VmSwap` stayed flat at 346 MiB, RSS lost only
170 MiB — and its turn 2 was *equally* slow (0.754 tok/s, 387 major faults/token). Two runs, an
order of magnitude apart in swap, identical slowdown. Anonymous swap therefore cannot be the whole
cause, and the prime suspect for the rest is the mmap'd dense weights: they are file-backed, so
reclaim drops them without `VmSwap` ever moving, and refaulting them reads flash inside the decode.

That matters for the trigger. The pass restores both (the `MADV_WILLNEED` hints and the dense sweep),
but it only *runs* when `VmSwap` crosses the threshold — a signal blind to the dense case. Sizing
that effect, and picking a trigger that sees it (`RssFile` from the same `/proc/self/status` read is
the obvious candidate), is the open work. Until then this is a mechanism with a partial trigger, not
a settled fix.

## Why the threshold

`process_swapped_bytes()` reads `VmSwap` from `/proc/self/status`. It is the only field that
separates "the kernel took my pages" from "I never allocated them": RSS falling could equally mean
the cache was evicted on purpose, whereas `VmSwap` rising means the pages are still ours, just
compressed and waiting.

Below a few hundred MiB the refault cost is spread thin enough that the decode absorbs it, while the
bulk pass would still stall the first token — hence the default of 256 MiB. On a device that never
swaps (enough RAM, or no zram) `VmSwap` stays at zero, the threshold never trips, and the feature
costs one `/proc` read per generation. That is why it is on by default and needs no per-device
tuning. `0` means "always rewarm", which is how the gates force the pass on a host that has nothing
swapped out.

## Flags

| Flag | Meaning |
|---|---|
| `--no-rewarm` | skip the per-prompt bulk restore (A/B measurements) |
| `--rewarm-threshold-mb N` | swapped-out MiB above which the pass runs (default 256; 0 = always) |

## Scope

- **Universal, not device-specific.** `madvise` and `/proc/self/status` are plain Linux, available to
  any app without permissions or vendor APIs, and zram-backed reclaim of idle processes is the norm
  across Android vendors. Nothing here keys on a device or a model. On Windows the primitives report
  "nothing swapped" and the pass never runs.
- **It does not prevent reclaim.** No app-reachable API can: `mlock` is capped at a few tens of KiB
  for unprivileged processes, far below a multi-GiB cache. This makes recovery cheap rather than
  making confiscation impossible, and the pages it faults back are pages the decode was about to
  touch anyway — it changes when they arrive, not how many.
- **The KV cache is not covered.** It belongs to llama.cpp, which exposes no buffer pointer to hint
  on, so it refaults on first touch as before. It is far smaller than the expert cache and is touched
  linearly, so it costs much less than either.
- **A genuinely cold start is a different problem.** Nothing is in swap when a process has just
  started: the model load and the expert cache's first fill are physics, not reclaim, and the
  load-time dense warm-up is what addresses their first-token share.
