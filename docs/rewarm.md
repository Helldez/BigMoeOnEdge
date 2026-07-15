# Rewarm after reclaim — a measured failure

**This does not work. It is off by default and documented here so the next attempt does not repeat
it.** The pass does exactly what it claims and the problem is untouched; the reason why is the useful
part. For the mechanics underneath — what reclaims, when, and which levers exist — see
[android-memory.md](android-memory.md).

A session keeps the model loaded and the expert cache warm so the second prompt does not re-pay the
first one's costs (see [session mode](telemetry.md)). Android disagrees. Its reclaim daemon targets
exactly what a session is: a process holding gigabytes of anonymous memory and doing nothing between
turns. Within seconds of a generation ending, that memory is compressed into zram — the cache is
still resident in the LRU's books, but its pages are gone.

Nothing announces this. The next prompt starts, the cache reports the same hit rate it always did,
and every hit touches a page that has to be decompressed back out of zram first — one major fault at
a time, per expert, per layer, for the whole answer. The result reads as a mysteriously slow turn on
a session that should have started warm.

That diagnosis is correct, and the conclusion drawn from it — restore the pages first — is not. See
[what actually happened](#what-actually-happened) below.

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

## What actually happened

In the app, on a turn that started with 1.76 GiB swapped out, the pass fired and worked:

| | before the turn | after the pass (t=4 s) |
|---|---|---|
| expert cache resident (`RssAnon`) | 0.65 GiB | **2.02 GiB** — the full budget |
| dense resident (`RssFile`) | 0.30 GiB | **1.29 GiB** |
| `VmSwap` | 1.76 GiB | **0.46 GiB** |

It cost 6.4 s. Eight seconds later the kernel had started taking it back, and it kept taking it, all
the way through the decode (`app-turn-after-rewarm.csv`):

```
t=4    RssAnon 2.02 GB   swap 463 MB     <- restored
t=12   RssAnon 1.86 GB   swap 620 MB     <- already going
t=20   RssAnon 1.57 GB   swap 917 MB
t=44   RssAnon 1.46 GB   swap 1.02 GB
t=68   RssAnon 2.02 GB   swap 468 MB     <- the engine faults it back
t=88   RssAnon 1.88 GB   swap 596 MB     <- and loses it again
```

The turn ran at 0.3 tok/s — no better than with no rewarm at all. `MemFree` sat between 35 and
300 MiB for the entire generation.

That oscillation is the whole finding. This is not a session that was reclaimed while idle and could
be made whole again; it is a process asking for about 3.8 GiB (2.0 of expert cache plus 1.77 of dense
weights) on a device that will concede roughly 3.0, with the kernel balancing the books continuously
while it works. Restoring residency cannot win that: it buys seconds of truce for a several-second
pause. **The ask has to shrink — re-fetching what was taken is treating the scoreboard.**

Where to look next, on this evidence: the expert cache costs 2 GiB and returns an 8–9% hit rate on
gpt-oss-120b (128 experts at top-2 — the working set is orders of magnitude past any budget that
fits). It buys one expert in eleven at the price of the memory war that is costing far more, so the
first thing worth measuring is simply turning it off for >RAM models. Also note `--cache-mb auto`
sizes itself from `MemAvailable`, which counts the page cache holding this model's own dense weights
as free — so it over-asks by construction here.

## The A/B that led here

On a OnePlus 15R running gpt-oss-120b-Q4_K_M with the Android example's own configuration (4096 ctx,
fixed 2000 MiB cache, 4 lanes, top-2), two turns of a session separated by a four-minute idle:

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

The first warning that restoring pages would not be enough came from the second arm of this same
A/B, before the app test settled it. That run was reclaimed very differently — `VmSwap` stayed flat
at 346 MiB, RSS lost only 170 MiB, so the pass never even fired — and its turn 2 was *equally* slow
(0.754 tok/s, 387 major faults/token). Two runs an order of magnitude apart in swap, one identical
slowdown: anonymous swap was never the whole cause. The mmap'd dense weights are the rest of it, and
they are file-backed, so reclaim drops them without `VmSwap` moving at all — which also means the
trigger was blind to half the problem it was built for.

## Why the threshold

`process_swapped_bytes()` reads `VmSwap` from `/proc/self/status`. It is the only field that
separates "the kernel took my pages" from "I never allocated them": RSS falling could equally mean
the cache was evicted on purpose, whereas `VmSwap` rising means the pages are still ours, just
compressed and waiting.

Below a few hundred MiB the refault cost is spread thin enough that the decode absorbs it, while the
bulk pass would still stall the first token — hence the 256 MiB default. `0` means "always rewarm",
which is how gate S4 forces the pass on a host that has nothing swapped out.

The threshold also explains why the first app test looked like a bug and was not: the turn ran fast,
no `rewarm` appeared, and the reply degraded anyway. `VmSwap` was still low when that turn *started*
— the kernel did its taking during the generation. Reading the signal once, up front, cannot see a
thief that arrives later, which is the same reason the pass cannot fix this.

## Flags

| Flag | Meaning |
|---|---|
| `--rewarm` | opt in to the per-prompt bulk restore (off by default: measured ineffective) |
| `--rewarm-threshold-mb N` | swapped-out MiB above which the pass runs (default 256; 0 = always) |

## Scope

If someone revisits this, these still hold:

- **The mechanism is sound and universal.** `madvise` and `/proc/self/status` are plain Linux,
  available to any app without permissions or vendor APIs, and zram-backed reclaim of idle processes
  is the norm across Android vendors. It measurably restores what it targets. On Windows the
  primitives report "nothing swapped" and the pass never runs.
- **Nothing app-reachable can prevent reclaim.** `mlock` is capped at a few tens of KiB for an
  unprivileged process, far below a multi-GiB cache. That constraint is what pushed this design
  toward recovery — and recovery is what failed. The remaining lever is the size of the ask.
- **A trigger read once per turn is structurally wrong here.** Reclaim is continuous during the
  decode, not a one-off during the idle. Anything reactive would have to run *inside* generation, and
  would then be competing with the kernel on its own schedule.
- **`VmSwap` sees only half the reclaim.** The dense weights are file-backed and vanish without it
  moving; `RssFile` from the same read sees them. Any future trigger needs both.
- **A genuinely cold start is a different problem.** Nothing is in swap when a process has just
  started: the model load and the expert cache's first fill are physics, not reclaim, and the
  load-time dense warm-up is what addresses their first-token share.
