#pragma once

// The expert cache's size while a run goes: it gives memory back when the system is taking the
// engine's memory, and takes back what it gave once the machine has shown it can hold it again.
//
// No figure a platform reports says how much a process can keep resident: a phone granted a 6 GB
// share compressed 2.5 GB of an engine holding 4.6 GB and ended it, and another phone compressed
// 2 GB of the same engine with 5 GB "available". So the question is answered while the run goes,
// from things every machine shows a process about itself: how long its own work takes, and whether
// its own memory is still in RAM. A token whose compute stalls far past this machine's own reference
// says something went wrong; memory of ours that the system has just taken says it was memory and
// not the cores. Only both together move the budget.
//
// "Ours" is the whole process, not the cache alone. A locked cache cannot be taken, so a system
// short of memory takes what is not locked: on a 16 GB laptop with 7 GB of cache and dense weights
// locked, it compressed 680 MB of the compute buffers every token reads, and tokens went from 0.12 to
// 2-8 s while every sampled cache page was still in place. Giving cache back is what relieves that
// too, because the cache is the one holding the engine can drop at no cost: its bytes are on flash.
//
// The retired governor (docs/pressure.md) grew the cache back whenever things were calm, and on a
// machine that could not concede the budget that turned a reclaim into a standing war. Recovery here
// is bounded instead: it never climbs to a size the machine refused (each brake lowers the ceiling
// under the size it braked from), it waits longer after every brake, and it does not climb while the
// system is still taking memory. A run on a machine that is not pressed never brakes, so it never
// moves from the planned budget.
//
// Pure policy: the session feeds it each decoded token and the residency facts, and applies the
// budget it returns. The samples are the streamer's and the platform's, the budget change the
// session's.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace bmoe {

// Not measured constants: a reference wide enough that one slow token does not move it, and a
// stall factor wide enough that ordinary variance is not one. A false stall costs one residency
// check, never a budget change, because the check has to agree.
struct ResidencyPolicy {
    int warmup_tokens = 8;     // the first tokens fill the cache and are not this machine's steady state
    int window = 16;           // tokens the reference is the median of
    double stall_factor = 2.0; // a token this many times slower than the reference is a stall
    // Clean tokens before the first step back up after a brake, doubled by every further brake: a
    // machine that keeps refusing is asked less and less often.
    int recover_after = 64;
    // Memory of ours taken by the system counts as a loss only past this share of the budget: the
    // accounting moves by a few pages on its own.
    int loss_margin_div = 64;
};

// What the platform says about the process's own memory after a token. `lost_bytes` is its
// anonymous memory the system has already taken (compressed or swapped); unknown where the platform
// does not publish it, and then only the page samples can confirm a stall.
struct OwnMemory {
    bool known = false;
    uint64_t lost_bytes = 0;
};

enum class ResidencyEvent { none, brake, recover };

class ResidencyController {
public:
    explicit ResidencyController(ResidencyPolicy pol = {}) : pol_(pol) {}

    // The planned budget is the ceiling the run never climbs over; the floor is one token's
    // worst-case cycle, under which an LRU cache thrashes to no hits at all, and also the step a
    // recovery climbs by.
    void start(size_t planned, size_t floor);

    // One decoded token: its compute time, the process's own memory after it, and the budget now.
    // `pages_lost` samples the cache's own pages and returns the share no longer
    // in RAM, or a negative value where nothing could be sampled; it is called only to confirm a
    // stall the other facts did not. Returns the budget to apply (equal to `budget` to hold).
    size_t on_token(double compute_s, const OwnMemory & own, size_t budget, const std::function<double()> & pages_lost);

    // What the last on_token did, and why, for the caller's log line.
    ResidencyEvent last_event() const { return event_; }
    double last_lost_share() const { return lost_share_; }
    uint64_t last_lost_bytes() const { return lost_delta_; }

    // The reference a stall is measured against, 0 until the warm-up is over.
    double reference() const;
    size_t ceiling() const { return ceiling_; }

    // One decoded token's compute time. True when it stalled against the reference this machine
    // set in its own earlier tokens; a stalled token does not enter the reference, so a run that
    // keeps stalling is still compared with what it did before it started to.
    bool stalled(double compute_s);

    // The budget after a brake: half, never under the floor. Equal to `budget` when it is already there.
    static size_t braked(size_t budget, size_t floor);

private:
    ResidencyPolicy pol_;
    int seen_ = 0;
    std::vector<double> recent_; // the last `window` steady tokens, oldest first

    size_t floor_ = 0;
    size_t ceiling_ = 0;    // never climbed over: the plan, then under every size a brake left
    int recover_every_ = 0; // clean tokens between steps up; 0 until the first brake
    int clean_streak_ = 0;  // tokens since the last stall, brake or step
    bool clean_known_ = false;
    uint64_t clean_lost_ = 0;  // own memory lost at the last clean token: what a stall is compared with
    bool in_episode_ = false;  // braked, and not calm for a window since: stalls still belong to it
    uint64_t streak_lost_ = 0; // own memory lost when the clean streak began

    ResidencyEvent event_ = ResidencyEvent::none;
    double lost_share_ = 0.0;
    uint64_t lost_delta_ = 0;
};

} // namespace bmoe
