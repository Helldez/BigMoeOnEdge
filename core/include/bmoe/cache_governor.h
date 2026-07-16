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
// Two states, because a budget the device will not concede is not always a cut away from working.
// On a >RAM model one token routes more than the device will ever hold, so the LRU cache can earn
// no hits at any budget it is allowed — and it still pays the churn of committing and evicting a
// token's worth of pages every step, which is itself a source of the reclaim war. Measured: cutting
// such a cache to the floor did NOT end the war (majflt stayed in the thousands), but turning it
// off — shared slots, zero commit/evict — did. So the governor can also DEMOTE the cache to slot
// mode, and re-arm it (promote back) if the device later concedes the room. See docs/pressure.md.
//
// Pure policy: no syscalls, no clocks, no config reads, no llama.cpp. The caller feeds sensor
// readings per token and applies the returned budget/mode. That keeps it unit-testable against a
// synthetic device, and portable — the Android sensors are getrusage + mincore + /proc/self/status,
// but an iOS port (os_proc_available_memory, DISPATCH_SOURCE_TYPE_MEMORYPRESSURE) only has to fill
// the same struct.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace bmoe {

// What the sensors saw during one token's decode.
struct CacheSignals {
    // Major page faults during this decode (getrusage ru_majflt delta). The fast reflex: it costs
    // one syscall and it is already measured per token for the telemetry. Reads 0 on platforms
    // that cannot report it, which reads as calm. This is the one HEALTH signal — every other
    // reading below only attributes a fault load to a culprit, it does not judge severity.
    uint64_t majflt = 0;

    // Sampled fraction of the cache's own pages still in RAM (mincore over the LRU cold end), or
    // < 0 when not sampled this token (the sampler is throttled) or unsupported. Absolute — no
    // baseline, no device knowledge — and it sees theft of OUR cache before those pages fault back.
    double resident_frac = -1.0;

    // The largest single layer's routed working set in bytes, measured by the streamer (0 = not yet
    // known). This is the floor, and it is a mechanical one: the cache must be able to hold the
    // layer currently being staged. It is NOT "one token's working set" — see the note below.
    size_t floor = 0;

    // Sampled fraction of the DENSE weights (the mmap'd model) still in RAM, or < 0 when not
    // sampled/unsupported. The companion to resident_frac: that one watches our anon cache, this
    // one the file-backed weights it is blind to. A decline here from its own plateau, while the
    // cache holds, means the faults are the MODEL being reclaimed — a different war, and shrinking
    // the cache is the wrong lever for it (dense is file-backed; the cut frees anon).
    double dense_resident_frac = -1.0;

    // Change in VmSwap since the previous token, in bytes (signed; may be negative; 0 on the first
    // token or when unknown). Rising swap with a fault load, while our cache and the dense weights
    // both hold, is the signature of the ANON war: the kernel is compressing our own live memory
    // (KV, cache entries) into zram and we re-fault it — a war that cutting the cache does not end,
    // because the churn of a hit-starved cache is itself feeding it.
    int64_t swap_delta = 0;

    // Distinct expert bytes one token routes, as measured during DECODE (0 = not yet a decode
    // value — the caller withholds the prefill union, which is far larger and would read as
    // infeasible). Below it the cache holds nothing between tokens; far above what the device
    // concedes it can earn no hits at any allowed budget. The feasibility test reads it against the
    // room the device will give.
    size_t token_demand = 0;

    // MemAvailable in bytes (0 = unknown). It LIES for sizing — it counts our own mmap'd weights as
    // reclaimable — so it is never the sole authority. Used only as a coarse growth gate and, with
    // token_demand, a feasibility hint; the measured-residency machinery outranks it.
    uint64_t mem_available = 0;

    // Cumulative cache hits and lookups (the streamer's running totals). The governor diffs its own
    // windows across grow steps to see whether more budget is still buying hits, and stops growing
    // when it is not — the utility bound that replaces a hand-set ceiling. Frozen counters (Δ == 0,
    // e.g. in slot mode or a test that supplies no cache activity) simply disable the plateau test,
    // leaving pure AIMD.
    long long cache_hits = 0;
    long long cache_lookups = 0;
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
// pressure, and the only floor that may not yield is the mechanical one. When even the floor is
// refused — the war survives repeated cuts — the answer is not a smaller cache but no cache: demote
// to slot mode (see the header banner).

struct CacheGovernorParams {
    // user_cap 0 means "unbounded by configuration": the budget is then held only by the physical
    // expert-set size, the learned reclaim ceiling, the hit-rate plateau, and the device's free RAM.
    // A non-zero value (an explicit --cache-mb, or the auto ceiling) is honoured as a hard cap.
    size_t user_cap = 0;
    size_t initial = 0; // starting budget
    size_t min_cap = 0; // hard lower bound, used when CacheSignals::floor is still unknown

    // ── bounds discovered/passed at runtime (0 = not applied) ──
    size_t phys_cap = 0;       // total expert-set bytes: no cache is ever larger than this
    size_t floor_headroom = 0; // RAM to leave the rest of the system free (the cache_floor)

    // ── tunables ──
    // Deliberately compiled in rather than exposed: they are control-loop policy, not per-device
    // facts. The loop discovers the device's concession at runtime; these only shape how fast.
    // Calibrated against the traces in docs/bench-data/ (see docs/pressure.md).

    // Cut when the sampled residency drops below this. Our own cache pages going missing IS the
    // reclaim, so anything short of ~all-resident means the kernel already disagrees with the ask.
    double resident_min = 0.90;

    // Reflex threshold between residency samples: a fault load counts as a war when majflt exceeds
    // baseline*ratio + margin. The margin keeps a near-zero baseline from making single faults look
    // like a war; at roughly 100 us per fault from flash, 32 faults is ~3 ms — noise, not a signal.
    double fault_ratio = 3.0;
    uint64_t fault_margin = 32;
    double baseline_decay = 0.9; // EWMA weight for the calm-token fault baseline

    double cut_factor = 0.7;                // multiplicative decrease for a cache/dense war
    double cut_factor_hard = 0.5;           // sharper decrease for an anon war (cut hard, it does not yield gently)
    int cut_cooldown = 4;                   // tokens to wait after a cut before another (let the shrink land)
    int calm_tokens = 32;                   // consecutive calm tokens before growing
    size_t grow_step = 64ull * 1024 * 1024; // additive increase
    double ceiling_retreat = 0.9;           // grow freely only below ceiling*this
    int probe_after = 512;                  // calm tokens pinned at the ceiling before re-testing it

    // Attribution / demotion.
    uint64_t swap_margin = 32ull * 1024 * 1024; // bytes/token of fresh swap that reads as an anon war
    double dense_decline = 0.85;                // dense war when dense_frac < plateau * this
    int demote_after = 3;                       // unrelieved cuts in one war episode → demote to slots
    double rearm_margin = 1.25;                 // re-arm when free room > token_demand * this
    int rearm_persist = 3;                      // consecutive re-arm probes required before promoting
    int probe_every = 64;                       // tokens between re-arm probes while demoted

    // Utility demotion — the ground truth the attribution signals can miss. In a never-calm war the
    // fault baseline cannot self-calibrate (there is no quiet token to learn from), and MemAvailable
    // is unreliable for feasibility, so the governor also demotes on what it CAN measure directly:
    // the cache is not earning. Two independent reads of that, either sufficient after grace:
    //   • the budget was cut below one token's demand — it holds nothing between tokens; or
    //   • a measured window of near-zero hit rate — the model has no reuse the cache can exploit.
    // (Measured: gpt-oss sits at ~2% hit in a fault war; --cache-mb 0 runs the same model ~6x faster.)
    double demote_hit_floor = 0.10; // windowed hit rate below this reads as "not earning"
    int demote_window = 4;          // tokens per hit-rate window (grace skips the first, cold-fill one)
    // A sustained major-fault load this high (EWMA of majflt/token) is a memory war no cache is
    // winning — the ground truth when hit rate and attribution disagree. Measured on device: a
    // HEALTHY gpt-oss run (the cache earning, ~1.2 tok/s) settles at an EWMA near 1000 with brief
    // spikes to ~1500; the WAR runs (~0.1–0.3 tok/s) sit sustained at 4600–12000. 2500 sits in the
    // gap with margin both ways, and Qwen's booster stays in the low hundreds regardless. When the
    // EWMA holds above this past the warm-up grace, demote — cutting has not, and cannot, end it.
    uint64_t war_fault_abs = 2500;
    double fault_ewma_decay = 0.7;

    // Plateau: after a grow step, compare the hit rate over the next window against the previous
    // cap's. Improvement must clear two binomial standard errors (measurement noise) to count;
    // plateau_confirm consecutive non-improvements freeze growth at the last cap that helped.
    int hit_window = 32;
    int plateau_confirm = 2;
};

// The largest budget the device will sustain, computed once at load before prefill can fill an
// unsustainable one. MemAvailable overstates the room (it counts our own weights as free), so this
// is a clamp, not an authority — the runtime loop still has to discover the true concession.
inline size_t entry_budget(size_t configured, uint64_t mem_available, size_t floor, size_t min_cap) {
    if (mem_available == 0) return configured; // unknown → trust what was asked for
    const size_t sustainable = mem_available > floor ? (size_t) (mem_available - floor) : min_cap;
    size_t e = std::min(configured, sustainable);
    return std::max(e, std::min(min_cap, configured));
}

class CacheGovernor {
public:
    explicit CacheGovernor(const CacheGovernorParams & p);

    // The cache's residency policy, which the governor can switch between at a token boundary.
    enum class Mode : uint8_t {
        keep,  // no change this tick
        lru,   // promote to (or stay) the LRU cache
        slots, // demote to shared-slot mode: no cache, no commit/evict churn
    };

    // Why the loop last acted, for telemetry and tests. Not every token is a war; foreign_faults is
    // a fault load our own memory is demonstrably innocent of — the loop must NOT cut for it.
    enum class War : uint8_t { none, cache, dense, anon, foreign };
    enum class State : uint8_t { on, off };

    struct Decision {
        size_t cap = 0;            // the budget after this tick
        bool changed = false;      // true ⇒ apply cap to the cache now
        Mode mode = Mode::keep;    // a mode switch to apply BEFORE the cap (slots makes cap moot)
        bool rewarm_dense = false; // re-run the dense warm-up once, after a dense-war cut
    };

    // One tick per generated token, after the decode (the only point where the cache has no
    // decode in flight, so an evicting shrink or a mode switch is safe).
    Decision on_token(const CacheSignals & s);

    // Telemetry.
    size_t cap() const { return cap_; }
    long long cuts() const { return cuts_; }
    double baseline() const { return baseline_; }
    State state() const { return state_; }
    War war() const { return war_; }
    // The smallest budget observed to provoke reclaim, i.e. what the device would not concede.
    // no_ceiling while none has been found (or after a probe forgets it).
    size_t ceiling() const { return ceiling_; }

    static constexpr size_t no_ceiling = (size_t) -1;

private:
    size_t floor_of(const CacheSignals & s) const;
    size_t grow_limit(const CacheSignals & s) const;
    War classify(const CacheSignals & s, bool & swap_rising, bool & dense_declining);

    CacheGovernorParams p_;
    size_t cap_ = 0;
    size_t ceiling_ = no_ceiling;
    size_t plateau_cap_ = no_ceiling; // budget past which more RAM stopped buying hits
    double baseline_ = -1.0;          // EWMA of calm-token majflt; < 0 until the first calm token
    State state_ = State::on;
    War war_ = War::none;

    int calm_ = 0;
    int since_cut_ = 0;
    int since_probe_ = 0;
    long long cuts_ = 0;

    // Attribution state.
    double swap_ewma_ = 0.0;
    double dense_plateau_ = -1.0; // running high-watermark of sampled dense residency
    int dense_samples_ = 0;
    int war_cuts_ = 0;      // cuts made since the last calm token (one war episode)
    bool rewarmed_ = false; // dense re-warm already emitted this episode

    // Plateau bookkeeping: sample (hits, lookups) at each grow step and compare the interval just
    // ended against the previous one. Two consecutive non-improvements freeze growth at plateau_cap_.
    long long grow_hits_ = 0, grow_lookups_ = 0;
    bool grow_sampled_ = false;
    double prev_hitrate_ = -1.0;
    int plateau_miss_ = 0;

    // Re-arm bookkeeping (while demoted).
    int rearm_ok_ = 0;
    int since_rearm_probe_ = 0;

    // Utility-demote bookkeeping.
    int below_demand_ = 0;                     // consecutive tokens with cap below one token's demand
    long long hr_hits0_ = 0, hr_lookups0_ = 0; // hit-rate window anchors
    int hr_win_ = 0;                           // tokens elapsed in the current hit-rate window
    int hr_grace_ = 2;                         // hit-rate windows to skip (the cold cache fill)
    double fault_ewma_ = 0.0;                  // smoothed majflt/token, the sustained-war detector

    void plateau_on_grow(const CacheSignals & s);
    bool infeasible(const CacheSignals & s) const;
    Decision demote();
    Decision off_tick(const CacheSignals & s);
    Decision promote(const CacheSignals & s);
};

} // namespace bmoe
