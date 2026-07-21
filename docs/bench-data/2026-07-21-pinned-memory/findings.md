# Reclaim-exempt memory for the dense weights — bandwidth gate PASSES (2026-07-21)

**Verdict: a locked `AHardwareBuffer` BLOB reads at exactly anonymous-memory speed, so the one
allocation Android will not reclaim is not disqualified by bandwidth. This clears a gate; it does
not show a win. Whether pinning the dense weights makes the engine faster is still unmeasured.**

`docs/android-memory.md` lists every lever for keeping the dense weights resident and finds all of
them closed. The table had one omission: **dma-buf**, whose pages are exempt from reclaim by
construction because a device may DMA from them at any time, and which an unprivileged app can
allocate through `AHardwareBuffer`. The reason to check bandwidth before building anything is that
gralloc decides per allocation whether a buffer is CPU-cacheable — the kernel even ships a dedicated
uncached system heap — and uncached memory would read at or below the flash bandwidth it is meant to
save, making pinned dense weights *worse* than refaulting them. That asymmetry is what made this a
gate rather than a preference. Raw output in [`membench-runs.log`](membench-runs.log).

## Result: the two allocations are indistinguishable

Interleaved, CPU-pinned, `--mib 512`, best MiB/s per row:

| placement | ahwb | anon | ratio |
|---|---|---|---|
| cpu2 (policy0), 1 thread | 30392 / 30399 / 30310 | 30423 / 30394 / 30438 | **1.00** |
| cpu6 (policy6), 1 thread | 45065 / 45175 / 45176 | 45219 / 45183 / 45278 | **1.00** |
| all cores, 4 threads, 1 GiB | 59016 / 58960 | 58902 / 59024 | **1.00** |

Every pair agrees within 0.5%, which is inside this tool's run-to-run spread. For scale, the flash
this would displace serves ~1815–1980 MiB/s (`iobench-ceiling.md`): pinned memory reads **15–30×
faster than the storage path**, exactly as ordinary memory does.

`--usage rarely` produces the same numbers as `--usage often`. Either gralloc ignores the CPU-read
hint for BLOB, or both map cacheable; either way the mapping does not have to be coaxed.

## The binding constraint is size, and it is not where the docs point

Two different ceilings, and the lower one is undocumented:

- **Allocation** succeeds up to 4095 MiB — the cap implied by `AHardwareBuffer_Desc::width` being
  32-bit, which for BLOB *is* the byte count.
- **`AHardwareBuffer_lock`**, which is what turns the buffer into a usable CPU pointer, refuses at
  exactly 2048 MiB with `EINVAL` while 2047 MiB succeeds. A boundary landing precisely on 2^31 is a
  signed-32-bit type in the lock path, not memory exhaustion.

So the usable unit is **2047 MiB**, and any larger working set must be split across several buffers.
That is not an obstacle for this engine — `dense_weights` already allocates per tensor — but it is a
hard constraint on any design that assumed one buffer, and it is invisible to a probe that only
allocates. The first version of `--probe-max` did exactly that and reported double the usable size;
it now locks every candidate.

Allocation cost scales linearly at roughly 7–11 GiB/s (512 MiB ≈ 36–72 ms, 1792 MiB ≈ 222 ms), so a
multi-GiB dense set would pay a few hundred ms once at load.

## Methodology: the first run was wrong, and how

The first, un-pinned run reported **ahwb = 0.67× anon** — a clean, plausible, completely false
result. Swapping the mode order inverted it. Every reading in the session is either ~45400 or
~30400 MiB/s regardless of mode: those are the device's two CPU clusters, and the scheduler's choice
of core moves this number 1.5×, far more than the allocator does. Notably the faster cluster is the
one running at the *lower* clock (1.86 GHz vs 2.27 GHz under the vendor cap) — wider cores, so
frequency alone would have mispredicted the direction too.

`--repeat` (interleaved rounds) and `taskset` were added in response. The general lesson matches the
one already recorded for tok/s benchmarking: **a single A-then-B pass ranks two conditions that were
never controlled for.**

## What this does NOT show

- **That pinning helps.** Reclaim-exempt memory does not create memory. Under a >RAM model the RAM
  the dense weights stop yielding must come out of the expert cache or the page cache that feeds the
  streaming path, and the same reasoning already refuted the rewarm attempt (PR #28) and the
  per-layer LFU cap. The plausible failure mode is not "it doesn't work" but "`dense_resident_frac`
  goes to 1.0 and tok/s falls".
- **That it survives memory pressure the way the theory says.** These runs allocate and read; none
  of them put the device under the pressure that a >RAM decode creates, so "the kernel cannot
  reclaim it" is still an inference from how dma-buf works, not a measurement.
- **That it is portable.** One device, one gralloc. The 2 GiB lock boundary in particular is a
  driver-side property and may differ elsewhere.

## Next step

An in-app A/B of a `--dense-weights ahwb` mode against `--dense-weights anon`, on the same protocol
as the other engine-level tests, with `dense_resident_frac` and majflt/token read alongside tok/s.
Prior expectation stays where it was before this measurement: the two best-argued memory experiments
in this repo's history both measured negative, and this one has the same shape.
