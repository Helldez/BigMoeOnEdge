# Cache policy under memory pressure

**History note.** This document described a runtime *governor* — `--cache-dynamic` / `--cache-gov2` —
that watched the engine's own pages and resized the expert cache token by token to chase the budget
the device would concede. On-device measurement retired it: the governor thrashed (gpt-oss: 0.156
tok/s governed vs 0.517 cache-off) because a budget the device cannot concede starts a reclaim war
whose churn the loop itself feeds. The governor, `--cache-dynamic`, and the runtime sensors are gone.
The analysis below is why — and it is the reason the current, deliberately simple policy is what it
is:

- **cache-off (shared-slot) is the default**, and the safe answer whenever the dense weights are
  page-cached.
- **`--cache-mb N`** is a fixed-budget LRU that keeps the hottest experts resident. It pays where the
  working set fits without forcing reclaim (53–82% hit on RAM-fitting Qwen/Gemma).

> **Updated 2026-07-17 — "cache-off is the ceiling past RAM" was true *of a configuration*, not of
> the model.** That conclusion was measured with the dense weights left in the page cache, where an
> expert cache and the dense set fight over the same scarce RAM and the cache loses. Take the dense
> set out of the page cache (`--dense-weights anon`) and the fight is gone: on gpt-oss-120b a
> **2000 MiB cache now beats cache-off**, 0.998 vs 0.711 tok/s at the same lane count — and it won
> while clocked at 1.9 GHz against the cache-off cell's 2.27 GHz. The war described below is real;
> the way to win it turned out to be removing the *other* claimant on RAM, not shrinking the ask to
> zero. Sizing still matters: the budget must clear one token's working set (1815 MiB at k=4 there),
> which is why 2000 MiB works and 1000 MiB is rejected. See
> [benchmarks-gpt-oss.md](benchmarks-gpt-oss.md).
- **`--cache-mb auto`** sizes that budget once from device RAM at load, then leaves it fixed — a
  convenience, not a runtime loop. `--cache-ceil-mb` / `--cache-floor-mb` bound it.
- The **dense weights** get their own policy (`--dense-weights`, below), which is where the measured
  win — O_DIRECT — actually lives.

## Why a budget cannot be a constant

The expert cache is the one lever that trades RAM for flash reads (
[`--drop-cold-experts`](expert-dropping.md) is the other kind of trade — quality for flash reads —
and the two interact: a squeezed cache raises the miss rate, which raises the drop rate, so memory
pressure degrades output quality there instead of only throughput). The temptation is to set it as
large as the device seems to allow. On a phone that is the wrong shape of decision, for three
reasons that are measured rather than argued:

**A budget the device refuses does not go unused — it starts a war.** Ask for more than the device
concedes and reclaim stops being an event and becomes a standing condition: pages are taken *during*
the decode, faulted back in, and taken again. On gpt-oss-120b the engine asking ~3.8 GiB on a device
that conceded ~3.0 ran a turn at 0.3 tok/s against 1.45 tok/s for the same turn without the fight.
The cache was not slightly too big; it was net negative.

**Restoring the stolen pages does not win it.** Bulk-restoring the whole cache after an idle reclaim
works mechanically — 2.0 GiB back in 6.4 s — and the kernel takes it back within 8 s. Throughput
does not move. The ask has to shrink; re-fetching what was taken is treating the scoreboard.

**The number is not knowable in advance.** It depends on the model (one token of gpt-oss at top-2
routes ~536 MiB, Qwen3-30B at top-8 routes ~1051 MiB), on the top-k, on the device, and on whatever
else the user has open right now. Any constant is wrong for some model on some phone on some day.

## Why the OS cannot be asked

The obvious design — subscribe to the platform's memory-pressure signal — does not survive contact
with Android:

| Signal | Why it fails |
| --- | --- |
| `/proc/meminfo` `MemAvailable` | Counts the page cache holding our *own* mmap'd dense weights as free, so it over-reports by roughly the model's resident size. This is what `--cache-mb auto` sizes from, and why it over-asks. |
| PSI (`/proc/pressure/memory`) | SELinux label `proc_pressure_mem`; no `untrusted_app` allow rule. Unreadable by an app. |
| `onTrimMemory` | Late, coarse, one-sided — it never signals that pressure *eased*, and the `RUNNING_*` levels are deprecated since API 34. |
| `mlock` / memcg `min`/`low` | `RLIMIT_MEMLOCK` is 64 KiB on this device; cgroup v1 has no protection knobs an app can set. Nothing unprivileged protects anonymous memory. |

So the engine does not ask the OS anything. It watches what happens to memory it already holds.
A process may always ask about its **own** pages, on any device, with no permission and no vendor
cooperation — and pages we wrote, then lost, *are* reclaim, by definition.

## Why there is no runtime loop

The retired governor was a TCP-shaped control loop: sense the reclaim (`mincore` over the cache's own
pages + `getrusage` major-fault deltas), decide a new budget (AIMD — cut ×0.7 on reclaim, grow +64 MiB
when calm), act (resize + evict), once per token. It was well-formed, and it did not help. The reason
is the asymmetry it was built on: it assumed asking too much costs "a continuous war mid-decode" while
asking too little costs "a few points of hit rate" — and on a >RAM model there is no budget large
enough to earn those hits *and* small enough to avoid the war, so the loop just paid the sensing cost
and the churn while converging on what cache-off gives for free. So the engine no longer probes: it
either holds a fixed cache (`--cache-mb N`, which pays only where the whole working set fits) or runs
cache-off (the default), and lets the dense-weights policy handle the other half.

## The cache under pressure: brake, then a bounded recovery

There is one runtime move, and it sizes the expert cache while the run goes
(`core/src/engine/residency_controller.h`). `--cache-brake` (on by default, `--no-cache-brake` to
turn it off) halves the cache when the system takes the engine's memory mid-run, keeps halving while
it goes on taking it, and once the run has been calm climbs back one token cycle at a time. It never
goes under one token's worst-case cycle, never climbs to a size the machine refused, and never over
the planned budget. A run on a machine that is not pressed never moves from the plan.

It moves only when two signals agree, both about the engine's own work and its own memory, so it
reads the same on every platform:

1. **A stall.** A decoded token whose compute took at least twice the median of this machine's
   recent tokens. The reference is this run's, not a constant.
2. **Memory of ours taken.** The process's own anonymous memory the system has taken (compressed
   or swapped: `pio::process_memory`), grown past what it was the last time the run was calm. Where
   the platform does not publish that figure, a sample of one page from each of up to 64 cache
   entries, spread hot to cold, of which some are no longer in RAM (`pio::vm_resident_sample`).

The whole process, not the cache alone, because a locked cache cannot be taken. On a 16 GB laptop
running a 125B model with 7 GB of cache and dense weights locked, the system compressed 680 MB of
the compute buffers every token reads instead: tokens went from 0.12 s to 2-8 s while every sampled
cache page was still in place, so a brake that asked only the cache never fired. Giving cache back
is what relieves that too: the cache is the one holding the engine can drop at no cost, since its
bytes are on flash.

A slow token with nothing taken (the cores, the clock, a neighbour) moves nothing. A run that never
stalls never pays for a page sample.

**One episode, one ceiling.** The stalls after a brake belong to the same episode until the run has
been calm for a window of tokens; each one with the memory still gone halves again, so a machine
that keeps taking reaches the floor in a few tokens instead of crawling at half. The ceiling is
lowered once per episode, under the size the episode started from.

**Recovery is bounded,** which is what keeps it from being the governor this document retired. It
climbs one token cycle every 64 clean tokens, and every new episode doubles that wait; it never
climbs while the system is still taking memory, stall or not; and the ceiling only comes down. The
governor grew whenever things were calm, which on a machine that could not concede the budget
turned a reclaim into a standing war. Here every refusal lowers the ceiling and makes the next
attempt rarer, so the number of attempts a machine can refuse is bounded. A long-lived server is
why it climbs at all: without it, one minute of pressure from another program would cost the rest
of the session's speed.

What it answers is what the planner cannot know at load: how much of the budget this machine will
let the process keep, now. A 12 GB phone given a 6 GB share compressed 2.5 GB of an engine holding
4.6 GB and ended it (Jetsam `proc-thrashing`, #304). Its own compressed memory jumped at token 26
and compute collapsed from 28; the policy, replayed on that trace, brakes at 26.

Measured on a 16 GB laptop, `--auto` only, arms alternated with pauses between runs:

| model and condition | before | with this controller |
|---|---|---|
| Qwen3.6-35B-A3B Q4_K_M, nothing else running, 256 tokens | 20.7, 20.9 tok/s | 20.6, 21.3 tok/s |
| the same, another process holding 4 GiB from before the start | 20.2, 20.0 tok/s | 19.7, 20.2 tok/s |
| Qwen3.8-Flash-Next 125B Q2_0, six requests in one session, 5 GiB in active use beside it from the first request (two runs each) | first request 190 s and 1088 s; slowest token 158 s and 927 s | first request 88 s and 85 s; slowest token 6.0 s and 6.5 s |

On the unpressed runs it never moved. Under pressure it braked four times per run, to the floor,
and climbed back over the following requests, which then started a little slower while the cache
refilled. Other machines are owed (`scripts/pressure-check.py` runs the same check anywhere Python
does: the engine alone and beside a process holding memory, with the slowest token and what the
cache did).

## The floor is measured — and it is not the obvious one

The tempting floor is **one token's routed working set**. Below it, every token evicts what the next
one needs, so the cache holds nothing between tokens and its hit rate collapses. That is true, and
it is the bound `MoeStreamConfig::cache_min_mb` (1500 MiB) encodes as a static guess. It is still
the wrong floor, and the device said so.

Measured on gpt-oss-120b at top-4, `--cache-mb 2000 --cache-dynamic`:

| | |
|---|---|
| `token_demand_MiB` | 1815.4 |
| `cache_budget_MiB` | 1815.4 — the budget *is* the floor, to the decimal |
| `cache_cuts` | 1 — cut once, then pinned |
| `cache_hit_pct` | 8.2 — 1.8 GiB of RAM, for 8% of hits |
| `majflt/tok` | 5174, at 0.37 tok/s |

The loop cut 2000 → max(1400, 1815.4) = 1815.4 and stopped, because `cap > floor` was now false.
The sensor kept firing (`resident_frac` 1.000 → 0.937, 47k faults on one token) into a floor that
could not yield. A 9% cut, and the war went on.

The flaw is the comparison. "Below the floor the cache can only thrash" weighs the hits given up and
ignores that **the memory itself is the cost**: an unaffordable cache does not merely fail to earn
its hits, it starts a reclaim war worth several times any hit rate — and here it was buying 8%. The
falsifying evidence was already on the table: a hand-set 1000 MiB budget, far below that "floor",
runs this model well.

So usefulness yields to pressure. The only floor that may not yield is the **mechanical** one — the
widest layer of a pass, which the cache must be able to stage — and it is measured the same way
(`layer_demand_MiB`). `token_demand_MiB` stays in the telemetry, because where hits start is worth
knowing; it is simply not the same claim as where the cache must stop.

## Reading it

Per run (`# summary`, `BMOE_DONE`): `token_demand_MiB` (what one token routes — where hits *start*,
not a floor), `layer_demand_MiB` (the mechanical floor), `cache_budget_MiB` (the fixed budget in
effect), `cache_hit_pct`. Per token, `dense_resident_frac` says whether the dense set is holding in
RAM (the live signal now that the cache-residency governor sensor is gone).

This sizing procedure assumes dropping is off. With
[`--drop-cold-experts`](expert-dropping.md) on, dropped routings are misses that never reach the
cache, so `cache_hit_pct` reads high and `token_demand_MiB` reads low for the same budget — size
the cache first, then turn dropping on.

Reading `cache_hit_pct` against `token_demand_MiB` is how you tell whether a fixed `--cache-mb N` is
earning its RAM: a budget near or below one token's demand holds no history between tokens and its
hits are only inter-token correlation; well above it, a high hit rate means real reuse.

A *low* hit rate on a budget above one token's demand does not automatically mean "turn the cache
off" — check `majflt/token` first. If it is in the hundreds, what you are seeing is the **dense** set
refaulting, and the fix is `--dense-weights anon`, not a smaller expert cache: on gpt-oss a 2000 MiB
budget returns only 27–32 % yet still beats cache-off once the dense weights are out of the page
cache. Turn the cache off when the budget is unaffordable *after* that — i.e. when free RAM collapses
and majflt stays high with the dense set already anonymous.

## `--dense-weights anon`: make the dense weights anonymous

The cache policy above manages the *expert cache*. The other half of the war is the *dense*
(non-expert)
weights, whose policy is a single knob `--dense-weights mmap|warm|anon` (`DenseWeightsMode`, owned by
`core/src/moe/dense_weights.h`). `mmap` leaves them file-backed (the kernel reclaims them by dropping
the pages, and a later touch demand-faults them 4 KiB at a time — the slow-start baseline); `warm`
page-caches them once at load; `anon` (the default) reads each dense tensor once via O_DIRECT into an
aligned anonymous buffer and rebinds the tensor onto it. A reclaim of anonymous memory swaps to zram
rather than dropping to flash, so a refault is a fast zram decompress instead of a scattered flash
read. (`--dense-odirect` and `--no-warm-dense` remain as deprecated aliases for `anon` and `mmap`.)

The `anon` read reuses the expert-capture path: the router hook already records every weight leaf it
sees in the warm-up graph, and the runtime pairs the non-expert ones with their gguf offsets. It reads
through the dense module's own `FileReader`, so its O_DIRECT choice is independent of the expert
stream's — the experts can be streamed buffered while the dense set is pulled cache-bypassing, or the
reverse. It is a strict A/B lever, and the analysis says its upside is narrow — Q4 weights are close
to incompressible, so on a model whose dense set already fits resident, moving it flash→zram frees
nothing and is a wash. Where it can pay is a model actively losing its dense set to reclaim.

Two details keep the A/B honest rather than measuring load-time artefacts. First, the capture warm-up
decode faults the dense pages in *mmap-resident* before the read copies them into the anon buffers, so
for a moment the dense set is resident twice; once every tensor is rebound, the module drops those
now-unreferenced mmap pages with `MADV_DONTNEED` (`pio::vm_drop_file_pages`), so prefill starts from
the true anonymous-only footprint instead of a doubled peak the entry test would misread. Second, the
dense-residency sensor does not go blind: it samples the anon buffers directly (`mincore` reports
resident anon pages too), so `dense_resident_frac` keeps its meaning under the flag — whether zram is
holding the dense set — rather than reading unmeasured.

Byte-identity gates: `G6` proves the rebind changes not a single output byte against the mmap-resident
reference, and `G7` proves the same with O_DIRECT off (the read may bypass the page cache or not; the
rebound bytes are identical either way).

## Portability of what remains

The one runtime signal left is diagnostic: the dense-residency telemetry (`dense_resident_frac`),
`mincore` over the dense set — the least exotic syscall available, present on every POSIX target, no
permission, undisablable by a vendor kernel the way PSI and `mlock` are here. On a platform that
cannot report it (the Windows host build) it reads unmeasured and nothing downstream depends on it.
The controller ports as it is: it reads the process's own memory where the platform publishes it
and the same residency primitive every platform already implements where it does not.

## Status

The default is cache-off (shared-slot streaming) plus `--dense-weights anon` — the policy that wins
on the >RAM models the engine targets (3.2× on gpt-oss, see
[benchmarks-gpt-oss.md](benchmarks-gpt-oss.md)). `--cache-mb N` and `--cache-mb auto` are there for
RAM-fitting models, where `--dense-weights warm` (or `mmap`) is the lighter choice; under either,
the controller gives back what the system shows it cannot keep, and takes it back only as the
machine allows. The retired
governor is kept in git history (branch line through `feat/pressure-cache`) if the analysis ever
needs revisiting.

See also: [android-memory.md](android-memory.md) for what reclaims the engine's memory and which
levers exist, [cache-sizing.md](cache-sizing.md) for `--cache-mb auto`.
