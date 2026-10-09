// How much memory this process can hold and expect to KEEP.
//
// `MemAvailable` and its equivalents answer a different question: how much could be allocated right
// now. On a machine whose reclaim compresses, that figure is wrong in both directions at once. It
// is a floor, because the kernel will compress other processes' idle pages to make room for us — a
// phone held 3.8 GB of pinned dense set plus cache while reporting 3.6 GB available, and reported
// 5.6 GB available afterwards. And it is an over-promise, because the same cheap reclaim takes our
// pages back just as readily, which is why a model that merely fits is not a model that survives.
//
// Two ways to answer, and the caller chooses how much it is willing to spend:
//
//   Compressible — free. What the compressor is currently achieving on this machine, applied to the
//   set the kernel would compress first. It reads accounting, allocates nothing, and is an estimate.
//
//   Measured — expensive and intrusive. Hold memory in steps and watch for the moment the kernel
//   takes the first of it back. It is the real answer, it costs seconds and it churns the machine,
//   so it never runs unless asked.
//
// Both are best-effort: where the accounting is not readable — an unprivileged process on a phone
// may not read the compressor's own statistics — the fact stays Unknown and every rule above falls
// back to the reported budget and says so. This file is an adapter: platform conditionals live
// here, and nothing above it knows what a zram device is.

#include "bmoe/probe.h"

#include "../io/platform_io.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace bmoe {

namespace {

#if !defined(_WIN32) && !defined(__APPLE__)
// One "Key: value kB" line out of /proc/meminfo, in bytes. Returns false when the key is absent or
// the file cannot be read, which is a fact about this machine rather than an error.
bool proc_meminfo_bytes(const char * key, uint64_t * out) {
    FILE * f = std::fopen("/proc/meminfo", "re");
    if (!f) return false;
    const size_t klen = std::strlen(key);
    char line[256];
    bool found = false;
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strncmp(line, key, klen) != 0 || line[klen] != ':') continue;
        unsigned long long v = 0;
        if (std::sscanf(line + klen + 1, " %llu kB", &v) == 1) {
            *out = (uint64_t) v * 1024ull;
            found = true;
        }
        break;
    }
    std::fclose(f);
    return found;
}

// What the in-RAM compressor is achieving right now: stored bytes over the bytes they occupy. The
// ratio is the only honest multiplier for "how much more could be freed by compressing", because it
// is this machine's own measurement on this machine's own data rather than a figure for a
// compression algorithm in general. Unreadable to an unprivileged process on some systems, which is
// a "this machine will not say" rather than a failure.
bool compressor_ratio(double * ratio) {
    FILE * f = std::fopen("/sys/block/zram0/mm_stat", "re");
    if (!f) return false;
    unsigned long long orig = 0, compr = 0;
    const int n = std::fscanf(f, "%llu %llu", &orig, &compr);
    std::fclose(f);
    if (n != 2 || orig == 0 || compr == 0 || compr >= orig) return false;
    *ratio = (double) orig / (double) compr;
    return true;
}
#endif

// Hold memory in steps and find where the machine starts taking it back.
//
// The signal is the FIRST chunk: it is touched before any of the others exist, so it is the oldest
// and the first candidate for reclaim. Once a page of it is gone, the boundary has been found, and
// what was held before that step is what this process can keep.
//
// Deliberately timid, because this is the one thing here that can hurt the machine it is measuring.
// It grows in steps and stops at the first sign of loss, so the usual outcome is that it never
// reaches its ceiling; it releases everything before returning, on every path; and it is reached
// only because a caller asked for it.
uint64_t measure_holdable(uint64_t ceiling, uint64_t enough) {
    const size_t page = pio::vm_page();
    if (page == 0 || ceiling < (uint64_t) page * 2) return 0;

    // Enough steps that the answer is not rounded to something useless, few enough that the walk is
    // not itself the cost. The step size is the resolution of the answer and nothing more.
    const int max_steps = 24;
    size_t step = (size_t) (ceiling / (uint64_t) max_steps);
    step &= ~(page - 1);
    if (step < page) step = page;

    std::vector<void *> held;
    uint64_t total = 0;
    uint64_t last_good = 0;
    void * first = nullptr;

    for (int i = 0; i < max_steps; ++i) {
        if (total + (uint64_t) step > ceiling) break;
        void * p = pio::vm_reserve(step);
        if (!p) break;
        if (!pio::vm_commit(p, step)) {
            pio::vm_release(p, step);
            break;
        }
        // One byte per page: enough to make the page real, cheaper than writing all of it.
        for (size_t off = 0; off < step; off += page)
            static_cast<volatile char *>(p)[off] = 1;
        held.push_back(p);
        total += (uint64_t) step;
        if (!first) first = p;

        // Has anything been taken from the oldest chunk yet? An unmeasurable answer ends the probe
        // rather than being read as "nothing was taken".
        size_t sampled = 0, resident = 0;
        if (!pio::vm_resident_sample(first, step, &sampled, &resident) || sampled == 0) break;
        if (resident < sampled) break; // the boundary: what was held BEFORE this step is the answer
        last_good = total;
        if (last_good >= enough) break; // the question was "can this be held", and it can
    }

    for (void * p : held)
        pio::vm_release(p, step);
    return last_good;
}

} // namespace

void probe_headroom(HardwareProfile & hw, bool allow_active, uint64_t target_bytes) {
    if (hw.residency_budget == 0) return; // nothing to compare against; the plan already declines

#if !defined(_WIN32) && !defined(__APPLE__)
    // The free estimate. The kernel compresses inactive anonymous pages first, so that set is what
    // could still be freed, and the ratio the compressor is achieving is what it would be freed to.
    // Both numbers are this machine's own; neither is a constant.
    double ratio = 0.0;
    uint64_t inactive_anon = 0;
    if (compressor_ratio(&ratio) && proc_meminfo_bytes("Inactive(anon)", &inactive_anon)) {
        const double recoverable = (double) inactive_anon * (1.0 - 1.0 / ratio);
        hw.holdable_bytes = hw.residency_budget + (uint64_t) recoverable;
        hw.holdable_from = Headroom::Compressible;
    }
#else
    (void) hw;
#endif

    if (!allow_active) return;

    // Probe FOR THE TARGET, not for the maximum. Climbing towards what the machine physically has
    // provokes the reclaim it is meant to observe and then reports the pressure it caused: on the
    // test phone, which holds 4.4 GB in practice, that version answered 924 MiB - less than the
    // figure it was supposed to correct upwards. A quarter above the target is enough headroom to
    // see a boundary if one is there, and little enough to stay out of the way if it is not.
    if (target_bytes == 0) return;

    // The question worth asking is only ever "can MORE be held than was reported", because this
    // figure is only ever used to raise the reported one. A target under what the kernel already
    // promises teaches nothing: the probe spends real pressure, confirms it can hold less than it
    // was told it could, and the answer is then discarded. Measured on a phone with a 26B model —
    // target 2702 MiB against 3418 MiB reported, so `--probe-mem` churned and changed nothing.
    //
    // So the target is lifted to a quarter above the reported budget when the model's own is
    // smaller. A quarter is this file's existing coefficient rather than a new one, and it stays a
    // bounded overshoot rather than a climb: the boundary check inside `measure_holdable` stops at
    // the first page taken back, which is what separates this from the unbounded version above.
    uint64_t enough = target_bytes;
    const uint64_t above_reported = hw.residency_budget + hw.residency_budget / 4;
    if (enough < above_reported) enough = above_reported;

    uint64_t ceiling = enough;
    if (hw.memory_total && ceiling > hw.memory_total) ceiling = hw.memory_total;

    const uint64_t measured = measure_holdable(ceiling, enough);
    if (measured == 0) return;

    // What the probe establishes is a LOWER BOUND that was actually held, so it only ever raises the
    // figure. Where it stopped early it has found a real boundary under its own allocation, which is
    // evidence about that allocation rather than proof the reported figure is wrong.
    if (measured > hw.holdable_bytes && measured > hw.residency_budget) {
        hw.holdable_bytes = measured;
        hw.holdable_from = Headroom::Measured;
    }
}

} // namespace bmoe
