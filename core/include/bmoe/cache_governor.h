// Pressure-aware cache sizing: the policy half.
//
// A streaming engine on a phone competes with the kernel for RAM, and it loses. Ask for more
// than the device concedes and reclaim becomes a standing condition: pages are taken mid-decode,
// faulted back, taken again. Measured on a >RAM model, that war costs ~5x throughput, and
// restoring the stolen pages does not win it — the ask itself has to shrink (docs/pressure.md).
//
// Nothing unprivileged can read the device's concession: MemAvailable counts our own mmap'd
// weights as free, PSI is SELinux-blocked for apps, and onTrimMemory is late and one-sided. So
// this does not ask the OS how much memory it has. It watches what happens to memory we already
// hold and reacts — AIMD, the same shape TCP uses against an unobservable bottleneck. Growth is
// additive and cheap to give back; a cut is multiplicative because the asymmetry is real: asking
// too much costs a continuous war mid-decode, asking too little costs only a few points of hit
// rate.
//
// Pure policy: no syscalls, no clocks, no config reads, no llama.cpp. The caller feeds sensor
// readings per token and applies the returned budget. That keeps it unit-testable against a
// synthetic device, and portable — the Android sensors are getrusage + mincore, but an iOS port
// (os_proc_available_memory, DISPATCH_SOURCE_TYPE_MEMORYPRESSURE) only has to fill the same
// struct.
#pragma once

#include <cstddef>
#include <cstdint>

namespace bmoe {

// What the sensors saw during one token's decode.
struct CacheSignals {
    // Major page faults during this decode (getrusage ru_majflt delta). The fast reflex: it costs
    // one syscall and it is already measured per token for the telemetry. Reads 0 on platforms
    // that cannot report it, which reads as calm.
    uint64_t majflt = 0;

    // Sampled fraction of the cache's own pages still in RAM (mincore over the LRU cold end), or
    // < 0 when not sampled this token (the sampler is throttled) or unsupported. This is the
    // primary signal: it is absolute — no baseline, no device knowledge — and it sees the theft
    // before those pages fault back and cost a read.
    double resident_frac = -1.0;

    // The largest single layer's routed working set in bytes, measured by the streamer (0 = not yet
    // known). This is the floor, and it is a mechanical one: the cache must be able to hold the
    // layer currently being staged. It is NOT "one token's working set" — see the note below.
    size_t floor = 0;
};

// A note on the floor, because the obvious choice is wrong and it was measured to be wrong.
//
// The tempting floor is one TOKEN's routed working set: below it, every token evicts what the next
// one needs, so the cache stops holding anything between tokens and its hit rate collapses. That is
// true, and it is what MoeStreamConfig::cache_min_mb encodes. It is still the wrong floor.
//
// Measured on gpt-oss-120b at top-4 (docs/bench-data/2026-07-15-pressure/): one token routes
// 1815 MiB. Flooring the governor there pinned the budget at exactly 1815 MiB — a 9% cut from 2000
// — where it bought an 8% hit rate and went on losing the memory war at 0.37 tok/s, with the fault
// reflex firing into a floor that would not yield. Meanwhile a hand-set 1000 MiB budget, far BELOW
// that floor, runs the same model well.
//
// The flaw is the comparison. "Below the floor the cache can only thrash" weighs the hits lost and
// ignores that the memory itself is the cost: an unaffordable cache does not merely fail to earn
// its hits, it starts a reclaim war that costs several times what any hit rate could return. Losing
// inter-token hits is bounded and cheap; losing the war is neither. So usefulness must yield to
// pressure, and the only floor that may not yield is the mechanical one.

struct CacheGovernorParams {
    size_t user_cap = 0; // never exceed: the configured budget (--cache-mb, or the auto ceiling)
    size_t initial = 0;  // starting budget
    size_t min_cap = 0;  // hard lower bound, used when CacheSignals::floor is still unknown

    // ── tunables ──
    // Deliberately compiled in rather than exposed: they are control-loop policy, not per-device
    // facts. The loop discovers the device's concession at runtime; these only shape how fast.
    // Calibrated against the traces in docs/bench-data/ (see docs/pressure.md).

    // Cut when the sampled residency drops below this. Our own cache pages going missing IS the
    // reclaim, so anything short of ~all-resident means the kernel already disagrees with the ask.
    double resident_min = 0.90;

    // Reflex threshold between residency samples: cut when majflt exceeds baseline*ratio + margin.
    // The margin keeps a near-zero baseline from making single faults look like a war; at roughly
    // 100 us per fault from flash, 32 faults is ~3 ms — noise against a token, not a signal.
    double fault_ratio = 3.0;
    uint64_t fault_margin = 32;
    double baseline_decay = 0.9; // EWMA weight for the calm-token fault baseline

    double cut_factor = 0.7;                // multiplicative decrease
    int cut_cooldown = 4;                   // tokens to wait after a cut before another (let the shrink land)
    int calm_tokens = 32;                   // consecutive calm tokens before growing
    size_t grow_step = 64ull * 1024 * 1024; // additive increase
    double ceiling_retreat = 0.9;           // grow freely only below ceiling*this
    int probe_after = 512;                  // calm tokens pinned at the ceiling before re-testing it
};

class CacheGovernor {
public:
    explicit CacheGovernor(const CacheGovernorParams & p);

    struct Decision {
        size_t cap = 0;       // the budget after this tick
        bool changed = false; // true ⇒ the caller must apply cap to the cache now
    };

    // One tick per generated token, after the decode (the only point where the cache has no
    // decode in flight, so an evicting shrink is safe).
    Decision on_token(const CacheSignals & s);

    // Telemetry.
    size_t cap() const { return cap_; }
    long long cuts() const { return cuts_; }
    double baseline() const { return baseline_; }
    // The smallest budget observed to provoke reclaim, i.e. what the device would not concede.
    // no_ceiling while none has been found (or after a probe forgets it).
    size_t ceiling() const { return ceiling_; }

    static constexpr size_t no_ceiling = (size_t) -1;

private:
    size_t floor_of(const CacheSignals & s) const;
    size_t grow_limit() const;

    CacheGovernorParams p_;
    size_t cap_ = 0;
    size_t ceiling_ = no_ceiling;
    double baseline_ = -1.0; // EWMA of calm-token majflt; < 0 until the first calm token
    int calm_ = 0;
    int since_cut_ = 0;
    int since_probe_ = 0;
    long long cuts_ = 0;
};

} // namespace bmoe
