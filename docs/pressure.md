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

The expert cache is the one lever that trades RAM for flash reads, so the temptation is to set it as
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
There is no control loop to port anymore — the cache budget is fixed at load — so an iOS port needs
only the O_DIRECT reader and, if desired, the same `mincore` telemetry.

## Status

The default is cache-off (shared-slot streaming) plus `--dense-weights anon` — the policy that wins
on the >RAM models the engine targets (3.2× on gpt-oss, see
[benchmarks-gpt-oss.md](benchmarks-gpt-oss.md)). `--cache-mb N` and `--cache-mb auto` are there for
RAM-fitting models, where `--dense-weights warm` (or `mmap`) is the lighter choice. The retired
governor is kept in git history (branch line through `feat/pressure-cache`) if the analysis ever
needs revisiting.

See also: [android-memory.md](android-memory.md) for what reclaims the engine's memory and which
levers exist, [adaptive-cache.md](adaptive-cache.md) for `--cache-mb auto`.
