# Pressure-aware cache sizing

`--cache-dynamic` treats the cache budget as a ceiling rather than an instruction, and lets the
engine find the largest cache the device will actually concede. It is off by default.

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

## The loop

Three roles, one per concern, so the policy is portable and the syscalls are not:

- **Sense** (`core/src/io/platform_io.cpp`, `core/src/moe/expert_stream_source.cpp`)
  - `mincore()` over the cache's own anonymous buffers, walking the LRU cold end — the pages reclaim
    takes first. This is the primary signal: it is absolute (our pages are in RAM or they are not),
    needs no baseline, and sees the theft *before* those pages fault back and cost a read. Bounded
    and throttled; its cost lands in `mgmt_ms`, where a regression is visible rather than hidden.
  - `getrusage(RUSAGE_SELF).ru_majflt` deltas per token — the fast reflex between residency samples.
    Already measured for the telemetry, so it costs nothing new.
- **Decide** (`core/include/bmoe/cache_governor.h`) — AIMD with hysteresis. Pure policy: no
  syscalls, no clocks, no llama.cpp, no config reads. Unit-tested against a synthetic device
  (`tests/cache_governor_test.cpp`).
- **Act** (`ExpertStreamSource::set_cache_budget`) — resize and evict the cold tail. Driven once per
  token from the generation loop, which is the only point where no decode is in flight and an
  evicting shrink is safe.

The shape is TCP's, for the same reason TCP has it: an unobservable bottleneck, probed by watching
your own losses. Growth is additive because being wrong upward is cheap to give back; a cut is
multiplicative because the asymmetry is real — asking too much costs a continuous war mid-decode,
asking too little costs only a few points of hit rate.

```
every token, after the decode:
    if sampled residency < 0.90            → the kernel is taking the cache
    else if majflt > baseline*3 + 32       → reflex, once a calm baseline exists
        → ceiling := cap;  cap := max(cap * 0.7, floor);  wait out a cooldown

    else (calm):
        learn the baseline from this token
        after N calm tokens: cap += 64 MiB, up to min(user cap, ceiling * 0.9)
        after a long calm at the ceiling: forget it and re-test (the device may have changed)
```

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

Per token (`--csv`, `BMOE_PROGRESS`): `resident_frac` — the sampled fraction of the cache still in
RAM, `-1` when unmeasured (throttled sample, streaming off, or a host that cannot report).

Per token, under `--cache-gov2` also: `dense_resident_frac` (the model's own residency, `-1` when
unmeasured), and `gov_state` (0 = LRU, 1 = demoted to slots) with `gov_war` (0 none, 1 cache, 2
dense, 3 anon, 4 foreign) — the classifier's verdict each token. A demotion shows as `gov_state`
flipping to 1 and `cache_budget_mib` dropping to 0.

Per run (`# summary`, `BMOE_DONE`): `token_demand_MiB` (what one token demands), `layer_demand_MiB`
(the mechanical floor), `cache_budget_MiB` (where the loop settled), `cache_cuts` (how often the
device pushed back), `cache_resizes`.

A budget sitting exactly on `token_demand_MiB` with `cache_cuts` stuck at 1 is the failure above:
the loop wanted to cut and something floored it.

Reading `cache_budget_MiB` against `token_demand_MiB` is how a budget stops being a guess: a budget
near the demand holds about one token of routing history and its hits come from inter-token
correlation only; a budget far above it is either buying real reuse or buying a war, and
`cache_cuts` says which.

## `--cache-gov2`: attribution, a plateau, and an off state

`--cache-dynamic` cuts when it senses reclaim and grows when it does not. That is the right reflex
for the case it was built for — the device slowly conceding a Qwen-sized cache — but three device
runs on 2026-07-16 showed it is not enough on its own. `--cache-gov2` is the same loop plus the parts
those runs demanded. It implies `--cache-dynamic`; the plain flag stays the A/B baseline.

**The one health signal is `majflt`. Everything else only attributes it.** A fault load is the only
evidence that pressure is *costing* something; residency and swap say *whose* pages are moving, which
decides *which lever* to pull — but a residency that dips on cold, never-reused pages is not a
problem, and cutting the cache is the wrong answer to a fault load that is not the cache's. So the
governor classifies each token's pressure before it acts:

| War | Signature | Action |
|---|---|---|
| cache | `resident_frac` below 0.90 (our anon cache is being taken) | cut ×0.7 |
| dense | `dense_resident_frac` fell below its own plateau ×0.85 (the model's weights are going) | cut, and re-warm the dense set once |
| anon | swap climbing under a fault load, while cache *and* dense both hold | cut ×0.5 (hard) |
| foreign | faults, but cache and dense are demonstrably resident and swap is flat | **do not cut** — absorb it as the new calm |

The foreign case is why `--cache-dynamic` alone over-cuts: measured on the new device, a run held
`dense_resident_frac` at 1.000 while `majflt` sat in the thousands and `swap_mib` climbed 1.8 → 3.1
GiB — the kernel was compressing our *anon* memory into zram, not touching the cache. v1 read the
resident cache as calm, folded the 3000-fault storm into its baseline, and then *grew* mid-war. v2
attributes it to the anon war, never poisons the baseline (a token any war signal fires on cannot
teach the "calm" rate), and cuts hard.

**Cutting is not always the answer — sometimes the cache has to turn off.** On a >RAM model one token
routes more than the device will ever hold (gpt-oss at top-4: `token_demand_MiB` 1815), so the LRU
earns no hits at any allowed budget *and* pays the commit/evict churn of a token's worth of pages
every step — churn that is itself a source of the war. Measured: the governor cut that cache all the
way to 470 MiB and the war did not end (`majflt/tok` stayed 3000–6000); a same-day `--cache-mb 0` run
of the same model sat at `majflt/tok` ≈ 1. So the governor has a real **off state**: when an anon war
survives repeated cuts, or a decode-measured `token_demand` exceeds what the device will concede, it
**demotes** the cache to shared-slot mode (the byte-identical no-cache path, gates S4a–f), where the
churn disappears. It re-arms — promotes back to the LRU — only after several probes agree the device
has freed room for a demand-sized cache with margin.

**Growth stops where hits stop.** With no configured ceiling (`--cache-mb auto` sizes the ceiling
dynamically to `MemAvailable − floor`, itself not a constant), the budget is held by four measured
bounds: the physical expert-set size, the learned reclaim ceiling, the device's free RAM, and a
**hit-rate plateau** — the governor samples the hit rate at each grow step and freezes when more
budget stops buying hits beyond two binomial standard errors. On the old device this is the
difference between the governed run (1.64 tok/s, cuts once, then calm) and a fixed 4000 MiB run
(same 1.64 tok/s but a permanent 1582 fault/tok war): the extra 10 points of hit rate bought nothing
but the war.

**Entry test.** Prefill runs before the governor's first tick, so a budget the device cannot sustain
is filled — and the war started — before any decision. Measured: `--cache-mb 4000` on gpt-oss had
`swap_mib` at 1.8 GiB by step 1. So under gov2 the *starting* budget is clamped at load to
`entry_budget(configured, MemAvailable, floor, min_cap)` and the streamer shrunk to it before the
first prefill token. `MemAvailable` overstates the room (it counts our own weights as free), so this
is a clamp, not an authority — the runtime loop still discovers the true concession.

None of this hard-codes a model or a device: `token_demand` and the hit rate are measured, the four
bounds are measured or configured, and the same machine routes Qwen to a 2.8 GiB cache and gpt-oss to
the off state on the *same* phone, purely on the numbers.

## Portability

The sensors are the platform's half, and they are deliberately the least exotic syscalls available:
`getrusage` and `mincore` exist on every POSIX target, need no permission, and cannot be disabled by
a vendor kernel the way PSI and `mlock` are here. On a platform that cannot report residency (the
Windows host build) the sample reads as unmeasured, the loop stays calm, and the budget behaves
exactly as a static one — which is the right degradation.

An iOS port would replace only the sensor: `os_proc_available_memory()` plus
`DISPATCH_SOURCE_TYPE_MEMORYPRESSURE`, which unlike `onTrimMemory` fires on both edges. The
governor and the actuator do not change.

## Status

Off by default, and the app exposes it as a switch ("Pressure-aware cache sizing"). It stays off
until the on-device A/B says otherwise — the same discipline that turned the rewarm pass off after
it was measured. `--cache-gov2` (attribution, the plateau, and the off state) is a second, likewise
default-off switch, promoted per model+device by the A/B: Qwen and Gemma must not regress from their
~5 tok/s benchmark, and gpt-oss must reach the off state and match a hand-set `--cache-mb 0`. The
tunables (0.90 residency, ×3 faults, ×0.7 cut, 64 MiB growth, and gov2's ×0.5 anon cut, 0.85 dense
decline, 3-cut demote) are compiled in rather than exposed: they are control-loop policy, not
per-device facts, and the loop discovers the device's concession at runtime regardless.

See also: [android-memory.md](android-memory.md) for what reclaims the engine's memory and which
levers exist, [adaptive-cache.md](adaptive-cache.md) for `--cache-mb auto` and the ceiling.
