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

## The floor is measured, not guessed

A cache below **one token's routed working set** can hold nothing between tokens: every token evicts
what the next one needs, so it cannot buy a single hit and still pays the management cost. That
bound — not any particular number of MiB — is what `MoeStreamConfig::cache_min_mb` (1500 MiB)
encodes as a static guess, and the guess is model-specific: it is roughly right for Qwen3-30B at
top-8 (1051 MiB/token) and much too high for gpt-oss-120b at top-2 (536 MiB/token).

So the streamer measures it. The bytes of distinct experts routed between two visits to the same
layer *is* the working set; it is reported as `token_demand_MiB` in the run summary and it is what
floors the governor. That keeps the loop honest on a model nobody has benchmarked yet: below the
floor a smaller cache cannot help, because the misses there are the model's own demand and not the
kernel's doing.

## Reading it

Per token (`--csv`, `BMOE_PROGRESS`): `resident_frac` — the sampled fraction of the cache still in
RAM, `-1` when unmeasured (throttled sample, streaming off, or a host that cannot report).

Per run (`# summary`, `BMOE_DONE`): `token_demand_MiB` (what one token demands), `cache_budget_MiB`
(where the loop settled), `cache_cuts` (how often the device pushed back), `cache_resizes`.

Reading `cache_budget_MiB` against `token_demand_MiB` is how a budget stops being a guess: a budget
near the demand holds about one token of routing history and its hits come from inter-token
correlation only; a budget far above it is either buying real reuse or buying a war, and
`cache_cuts` says which.

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
it was measured. The tunables (0.90 residency, ×3 faults, ×0.7 cut, 64 MiB growth) are compiled in
rather than exposed: they are control-loop policy, not per-device facts, and the loop discovers the
device's concession at runtime regardless.

See also: [android-memory.md](android-memory.md) for what reclaims the engine's memory and which
levers exist, [adaptive-cache.md](adaptive-cache.md) for `--cache-mb auto` and the ceiling.
