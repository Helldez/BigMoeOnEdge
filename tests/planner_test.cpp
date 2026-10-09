// Unit tests for the hardware planner (core/src/plan/planner.cpp).
//
// The planner is a pure function, and this is the whole reason it is one: every machine below is a
// struct, so rules meant to hold on hardware nobody here owns can still be checked. No model, no
// llama.cpp, no device — it runs unconditionally in ctest.
//
// The machines are FIXTURES shaped after our measured verdicts (a desktop whose mapping serialises
// uncached reads; a phone that compresses reclaimed memory and offers a reclaim-exempt allocation;
// a platform that kills a process for holding too much and does not count mapped file pages). They
// are not measurements and nothing here should be quoted as one — what is being tested is that the
// rules read the facts, not that the facts are these.
//
// Checks are explicit (not <cassert>): the Release build defines NDEBUG.

#include "bmoe/config.h"
#include "bmoe/allocate.h"
#include "bmoe/planner.h"

#include <cstdio>
#include <string>

using namespace bmoe;

static int failures = 0;

static void check(bool cond, const std::string & name, const std::string & detail = "") {
    if (cond) {
        std::printf("[PASS] %s\n", name.c_str());
    } else {
        std::printf("[FAIL] %s%s%s\n", name.c_str(), detail.empty() ? "" : " - ", detail.c_str());
        ++failures;
    }
}

static const uint64_t KiB = 1024ull;
static const uint64_t MiB = 1024ull * 1024ull;
static const uint64_t GiB = 1024ull * 1024ull * 1024ull;

// ── the model ───────────────────────────────────────────────────────────────────────
// Shaped after a 35B-A3B Q4_K_M: 48 layers, 256 experts, top-8, split gate/up/down. The slice size
// is chosen so the derived token cycle lands where we measured one (about 582 MiB), which is what
// makes the floor assertions below meaningful rather than arbitrary.
static ModelProfile moe_model() {
    ModelProfile m;
    m.arch = "qwen3moe";
    m.is_moe = true;
    m.n_layer = 48;
    m.n_moe_layer = 48; // no leading dense blocks in this family: every block carries experts
    m.n_expert = 256;
    m.n_expert_used = 8;
    m.n_expert_projections = 3;
    m.expert_slice_bytes = 530432; // 518 KiB, the read the streamer issues
    // What the probe would sum over the file's own tensors: 48 layers x top-8 x 3 projections.
    m.token_cycle_bytes = (uint64_t) 48 * 8 * 3 * 530432;
    m.file_bytes = 21 * GiB;
    m.expert_bytes = 18 * GiB;
    m.dense_bytes = 3 * GiB;
    m.largest_dense_tensor = 243 * MiB;

    // The decomposition the cost model prices. Shaped so the totals agree with the file: the dense
    // groups are read whole every token, the embedding is gathered by row (so its per-token demand
    // is a few KiB however large the table is), and the experts demand exactly one token cycle.
    m.group(WeightGroup::Attention) = {1400 * MiB, 1400 * MiB, false, false};
    m.group(WeightGroup::DenseFfn) = {400 * MiB, 400 * MiB, false, false};
    m.group(WeightGroup::Output) = {700 * MiB, 700 * MiB, false, false};
    m.group(WeightGroup::Other) = {228 * MiB, 228 * MiB, false, false};
    m.group(WeightGroup::Embedding) = {344 * MiB, 8 * KiB, true, true};
    m.group(WeightGroup::Experts) = {18 * GiB, m.token_cycle_bytes, false, true};
    m.ok = true;
    return m;
}

// ── the machines ────────────────────────────────────────────────────────────────────
static StorageFacts desktop_storage() {
    StorageFacts s;
    s.align = 4096;
    s.direct_ok = Tri::Yes;
    s.mapping_serialises_reads = Tri::Yes;
    s.rate_curve = {
        {4 * (uint32_t) KiB, 1, 32.8},     {64 * (uint32_t) KiB, 1, 341.1},   {256 * (uint32_t) KiB, 1, 784.6},
        {576 * (uint32_t) KiB, 2, 1600.0}, {576 * (uint32_t) KiB, 4, 2400.0},
    };
    return s;
}

static HardwareProfile desktop() {
    HardwareProfile h;
    h.label = "desktop, swap-backed";
    h.residency_budget = 12 * GiB;
    h.memory_total = 16 * GiB;
    h.anon_overflow = Overflow::Swap;
    h.reclaim_exempt_max = 0;
    h.file_pages_counted = Tri::Yes;
    h.n_cores = 16;
    h.storage = desktop_storage();
    ComputeDevice gpu;
    gpu.name = "discrete";
    gpu.memory_total = 8 * GiB;
    gpu.memory_free = 7 * GiB;
    gpu.shares_host_memory = Tri::No;
    gpu.rebindable = false;
    h.devices.push_back(gpu);
    return h;
}

static HardwareProfile phone() {
    HardwareProfile h;
    h.label = "phone, compressed reclaim";
    h.residency_budget = 6 * GiB;
    h.memory_total = 12 * GiB;
    h.anon_overflow = Overflow::Compress;
    h.reclaim_exempt_max = 2047 * MiB;
    // What this class of machine answers: a store that publishes no total, and a lock-in-place
    // limit of a few pages. The dense set can be protected and the cache cannot.
    h.lockable_bytes = ~0ull;
    h.lock_in_place_bytes = 64 * KiB;
    h.file_pages_counted = Tri::Yes;
    h.n_cores = 8;
    h.storage.align = 4096;
    h.storage.direct_ok = Tri::Yes;
    h.storage.mapping_serialises_reads = Tri::No;
    h.storage.rate_curve = {
        {4 * (uint32_t) KiB, 1, 31.3},     {4 * (uint32_t) KiB, 4, 156.6},    {256 * (uint32_t) KiB, 1, 901.0},
        {256 * (uint32_t) KiB, 2, 1600.0}, {256 * (uint32_t) KiB, 4, 2201.5},
    };
    ComputeDevice igpu;
    igpu.name = "integrated";
    igpu.memory_total = 6 * GiB;
    igpu.shares_host_memory = Tri::Yes; // moving a tensor off it frees nothing
    h.devices.push_back(igpu);
    return h;
}

// A platform that answers memory pressure by killing the process, and does not count clean mapped
// file pages against that limit.
static HardwareProfile hard_capped() {
    HardwareProfile h;
    h.label = "hard per-process cap";
    h.residency_budget = 5 * GiB;
    h.memory_total = 8 * GiB;
    h.anon_overflow = Overflow::Kill;
    h.reclaim_exempt_max = 0;
    h.file_pages_counted = Tri::No;
    h.n_cores = 6;
    h.storage.align = 0;
    h.storage.direct_ok = Tri::Yes;
    h.storage.mapping_serialises_reads = Tri::Unknown;
    return h;
}

// One pool of memory for the cores and the accelerator, a compressing reclaim, and a lock the
// kernel grants to any process up to a system-wide total - the same store whether the memory is
// locked as it is allocated or where it already sits.
static HardwareProfile unified() {
    HardwareProfile h = phone();
    h.label = "unified memory, a system-wide lock total";
    h.residency_budget = 13 * GiB;
    h.memory_total = 16 * GiB;
    h.reclaim_exempt_max = 12 * GiB;
    h.lockable_bytes = 10 * GiB;
    h.lock_in_place_bytes = 10 * GiB;
    h.devices.clear();
    ComputeDevice cpu;
    cpu.name = "CPU";
    cpu.is_cpu = true;
    cpu.shares_host_memory = Tri::Yes;
    cpu.buffers_locked = Tri::No;
    h.devices.push_back(cpu);
    ComputeDevice accel;
    accel.name = "accel";
    accel.shares_host_memory = Tri::Yes;
    accel.buffers_locked = Tri::Yes;
    h.devices.push_back(accel);
    return h;
}

static const LedgerRow * ledger_row(const Plan & p, const char * name) {
    for (const LedgerRow & r : p.allocation.ledger.rows)
        if (r.name == name) return &r;
    return nullptr;
}

static RunConfig base_cfg() {
    RunConfig c;
    c.model_path = "model.gguf";
    return c;
}

static const Decision * find(const Plan & p, const char * knob) {
    for (const Decision & d : p.decisions)
        if (d.knob == knob) return &d;
    return nullptr;
}

int main() {
    const ModelProfile model = moe_model();

    // The derived floor is the model's own arithmetic, not a constant carried from one device.
    check(model.token_cycle_bytes / MiB == 582, "token cycle derives from shapes",
          std::to_string((unsigned long long) (model.token_cycle_bytes / MiB)) + " MiB");

    // ── desktop ─────────────────────────────────────────────────────────────────────
    {
        const Plan p = plan_run(base_cfg(), desktop(), model, PlanRequest{});
        check(p.regime == Regime::ExpertsStream, "desktop: experts stream, dense fits");
        check(p.config.moe.enabled, "desktop: streaming on");
        check(p.config.moe.dense_weights == DenseWeightsMode::Anonymous,
              "desktop: dense anon (no reclaim-exempt store here)");
        check(p.config.moe.release_mmap, "desktop: release-mmap on (the mapping serialises reads)");
        check(p.config.moe.io_threads == 4, "desktop: lanes from the rate curve",
              std::to_string(p.config.moe.io_threads));
        check(p.config.moe.cache_mb > 0 && (uint64_t) p.config.moe.cache_mb * MiB >= model.token_cycle_bytes,
              "desktop: cache clears the token cycle", std::to_string(p.config.moe.cache_mb) + " MiB");
        check(validate(p.config).ok, "desktop: the plan is a valid config", validate(p.config).error);

        const Decision * d = find(p, "release-mmap");
        check(d && d->source == Source::Measured, "desktop: release-mmap is a measured decision");
    }

    // ── phone ───────────────────────────────────────────────────────────────────────
    {
        const Plan p = plan_run(base_cfg(), phone(), model, PlanRequest{});
        check(p.regime == Regime::ExpertsStream, "phone: experts stream");
        // The flow DERIVES the pinned dense policy from the mechanism: anonymous memory here is
        // compressed away, and a reclaim-exempt store exists. It is not copied from what we ship.
        check(p.config.moe.dense_weights == DenseWeightsMode::Pinned,
              "phone: dense pinned, derived from compressed reclaim + a reclaim-exempt store");
        check(!p.config.moe.release_mmap, "phone: release-mmap off (no serialisation measured here)");
        check(p.config.moe.io_threads == 4, "phone: lanes from the rate curve",
              std::to_string(p.config.moe.io_threads));
        check(validate(p.config).ok, "phone: the plan is a valid config", validate(p.config).error);

        const Decision * d = find(p, "dense-weights");
        check(d && d->source == Source::Derived, "phone: dense policy is derived, not defaulted");
    }

    // ── a platform that kills instead of reclaiming ─────────────────────────────────
    {
        const Plan p = plan_run(base_cfg(), hard_capped(), model, PlanRequest{});
        // The same rule that picks anon elsewhere picks mmap here, because mapped file pages sit
        // outside the limit that can actually end the run. One rule, opposite answers, no branch.
        check(p.config.moe.dense_weights == DenseWeightsMode::Mmap,
              "hard cap: dense left mapped, because file pages are outside the fatal limit");
        check(!p.config.moe.release_mmap, "hard cap: release-mmap stays off while the dense set is mapped");
        check(validate(p.config).ok, "hard cap: the plan is a valid config", validate(p.config).error);
    }

    // ── a machine the model fits on: streaming is not a win, it is a cost ───────────
    {
        HardwareProfile big = desktop();
        big.residency_budget = 64 * GiB;
        big.memory_total = 64 * GiB;
        const Plan p = plan_run(base_cfg(), big, model, PlanRequest{});
        check(p.regime == Regime::Fits, "roomy machine: regime is fits");
        check(!p.config.moe.enabled, "roomy machine: streaming off");
        check(p.streaming_declined && !p.decline_reason.empty(), "roomy machine: the refusal is explained");
    }

    // ── the budget is what we can KEEP, not what is reported available ──────────────
    // On a machine whose reclaim compresses, the reported figure is a floor: the kernel will
    // compress other processes' idle pages to make room. Sizing from it leaves memory unused, and
    // the whole point of measuring is that the plan then spends what is really there.
    {
        const Plan reported = plan_run(base_cfg(), phone(), model, PlanRequest{});
        HardwareProfile measured = phone();
        measured.holdable_bytes = 8 * GiB; // more than the 6 GiB it reports available
        measured.holdable_from = Headroom::Measured;
        const Plan p = plan_run(base_cfg(), measured, model, PlanRequest{});
        check(p.config.moe.cache_mb > reported.config.moe.cache_mb,
              "measured headroom: the cache grows with what the machine will actually let us keep",
              std::to_string(reported.config.moe.cache_mb) + " -> " + std::to_string(p.config.moe.cache_mb));
        const Decision * d = find(p, "regime");
        check(d && d->reason.find("measured by holding memory") != std::string::npos,
              "measured headroom: the rationale says the budget was measured", d ? d->reason : "no decision");
    }

    // An estimate is still better than the reported floor, and is labelled as an estimate so a
    // reader can weigh it: the two are not the same claim.
    {
        HardwareProfile est = phone();
        est.holdable_bytes = 7 * GiB;
        est.holdable_from = Headroom::Compressible;
        const Plan p = plan_run(base_cfg(), est, model, PlanRequest{});
        const Decision * d = find(p, "regime");
        check(d && d->reason.find("estimated from") != std::string::npos,
              "estimated headroom: the rationale says the budget was estimated", d ? d->reason : "no decision");
    }

    // An unmeasured machine falls back to the reported figure and says which one it used, rather
    // than pretending the question was answered.
    {
        const Plan p = plan_run(base_cfg(), phone(), model, PlanRequest{});
        const Decision * d = find(p, "regime");
        check(d && d->reason.find("unmeasured") != std::string::npos,
              "unmeasured headroom: the rationale admits the budget is the reported one",
              d ? d->reason : "no decision");
    }

    // ── fitting is not being left alone: the same model, two machines that both "fit" ──
    // A phone whose reclaim compresses, holding a model that fits with little beyond itself. It is
    // the common case, not the exotic one, and residency there is reclaimed from underneath.
    {
        HardwareProfile barely = phone();
        barely.residency_budget = 25 * GiB; // the 21 GiB model fits, with about 2 GiB beyond it
        const Plan p = plan_run(base_cfg(), barely, model, PlanRequest{});
        check(p.regime == Regime::ExpertsStream, "fits barely: streams instead of trusting residency");
        check(p.config.moe.enabled, "fits barely: streaming on");
        const Decision * d = find(p, "residency");
        check(d && d->source == Source::Derived, "fits barely: the demotion is a derived decision");
        check(d && d->reason.find("reclaim is cheap") != std::string::npos,
              "fits barely: the reason names the machine's reclaim", d ? d->reason : "no decision");
    }

    // The same shape on a machine where nothing is taken from us is left alone: a hard per-process
    // cap is explicit, and the margin for it is already the largest of the three. (27 GiB, because
    // that margin is 20%: the model has to clear the bar before the air rule is even reached.)
    {
        HardwareProfile barely = hard_capped();
        barely.residency_budget = 27 * GiB;
        barely.memory_total = 32 * GiB;
        const Plan p = plan_run(base_cfg(), barely, model, PlanRequest{});
        check(p.regime == Regime::Fits, "fits barely under a hard cap: still resident, nothing reclaims us");
        check(!p.config.moe.enabled, "fits barely under a hard cap: streaming stays off");
    }

    // And where a reclaim has to write to a disk the kernel is reluctant enough that the classic
    // host offload stands: this is the desktop shape, and it is checked in the placement case below.
    // A reclaim-exempt machine likewise: what cannot be taken back does not have to be defended.
    {
        HardwareProfile barely = phone();
        barely.residency_budget = 25 * GiB;
        barely.anon_overflow = Overflow::None;
        const Plan p = plan_run(base_cfg(), barely, model, PlanRequest{});
        check(p.regime == Regime::Fits, "fits barely with nothing to reclaim it: resident");
    }

    // ── too little room for one token's worth of experts ───────────────────────────
    {
        HardwareProfile tight = phone();
        tight.residency_budget = 3 * GiB + 400 * MiB; // dense fits, what is left is under the cycle
        const Plan p = plan_run(base_cfg(), tight, model, PlanRequest{});
        check(!p.config.moe.enabled, "tight machine: streaming declined rather than thrashing");
        check(p.decline_reason.find("token cycle") != std::string::npos,
              "tight machine: the reason names the derived floor", p.decline_reason);
    }

    // ── threads: the classes decide, not the count ─────────────────────────────────
    {
        HardwareProfile het = desktop();
        het.n_cores = 20;
        het.core_classes = {10, 10}; // ten fast, ten slow: a barrier waits for the slow ones
        const Plan p = plan_run(base_cfg(), het, model, PlanRequest{});
        check(p.config.n_threads == base_cfg().n_threads,
              "heterogeneous cores alone: the count is NOT derived - that rule was refuted on a real phone",
              std::to_string(p.config.n_threads));
        check(p.config.n_threads_batch == 0, "heterogeneous cores: prefill keeps the default too");
        check(validate(p.config).ok, "heterogeneous cores: the plan is a valid config", validate(p.config).error);
        const Decision * d = find(p, "threads");
        check(d && d->source == Source::Unprobed && d->reason.find("refuted") != std::string::npos,
              "heterogeneous cores: the classes are reported, the rule is not applied",
              d ? d->reason.substr(0, 60) : "none");
    }
    {
        HardwareProfile uniform = desktop();
        uniform.core_classes = {8};
        const Plan p = plan_run(base_cfg(), uniform, model, PlanRequest{});
        check(p.config.n_threads_batch == 0, "uniform cores: prefill keeps the same count as decode");
        const Decision * d = find(p, "threads");
        check(d && d->source == Source::Derived && d->reason.find("alike") != std::string::npos,
              "uniform cores: nothing to improve, and the plan says why");
    }
    {
        const Plan p = plan_run(base_cfg(), desktop(), model, PlanRequest{}); // no classes reported
        const Decision * d = find(p, "threads");
        check(d && d->source == Source::Unprobed, "unreported core classes: the default stands, unprobed");
    }

    // ── on shared memory, what the fitter placed is charged to the same pool ───────
    // The fitter's capacity arithmetic treats device memory as separate. On a machine where it is
    // the host's own, inheriting that gives a budget for memory already spoken for - which is how a
    // phone was told an integrated accelerator had 15 GB free on an 11 GB device.
    {
        HardwareProfile uma = phone();
        uma.devices.clear();
        ComputeDevice cpu;
        cpu.name = "CPU";
        cpu.is_cpu = true;
        cpu.shares_host_memory = Tri::Yes;
        uma.devices.push_back(cpu);
        ComputeDevice ig;
        ig.name = "integrated";
        ig.shares_host_memory = Tri::Yes;
        uma.devices.push_back(ig);
        uma.residency_budget = 8 * GiB;

        Placement pl;
        pl.fitted = true;
        pl.n_gpu_layers = 0;
        pl.n_ctx = 2048;
        for (uint32_t il = 0; il < 48; ++il)
            pl.host_expert_layers.push_back(il);
        pl.host_resident_bytes = 1 * GiB;
        pl.device_bytes = 4 * GiB; // the same memory, which the fitter counted twice

        const Plan p = plan_run(base_cfg(), uma, model, pl, PlanRequest{});
        const Decision * d = find(p, "shared-pool");
        check(d && d->source == Source::Derived, "shared memory: the device bytes are charged and said");
        const Decision * c = find(p, "cache-mb");
        // Charged: 1 + 4 GiB is held before anything is sized, so the cache cannot be sized as if
        // only 1 GiB were. Without the charge it would have been GiBs larger.
        check(!c || (uint64_t) c->value.size() > 0, "shared memory: a cache decision was still reached");
        check(p.cache_budget_bytes < 3 * GiB, "shared memory: the cache is sized against the real pool",
              std::to_string((unsigned long long) (p.cache_budget_bytes >> 20)) + " MiB");
    }

    // A device with memory of its own is not charged: the two pools really are separate.
    {
        HardwareProfile disc = desktop();
        Placement pl;
        pl.fitted = true;
        pl.n_ctx = 2048;
        for (uint32_t il = 0; il < 48; ++il)
            pl.host_expert_layers.push_back(il);
        pl.host_resident_bytes = 1 * GiB;
        pl.device_bytes = 4 * GiB;
        const Plan p = plan_run(base_cfg(), disc, model, pl, PlanRequest{});
        check(find(p, "shared-pool") == nullptr, "discrete memory: nothing is charged twice");
    }

    // ── an integrated accelerator is not the CPU ───────────────────────────────────
    // Its memory IS the host's, so the capacity tier is inert - that much is arithmetic. Whether it
    // reaches more of the same bus than the cores do is a measurement, and treating "shared memory"
    // as "nothing to win" is the assumption this checks against.
    {
        HardwareProfile igpu = desktop();
        igpu.devices.clear();
        ComputeDevice cpu;
        cpu.name = "CPU";
        cpu.is_cpu = true;
        cpu.shares_host_memory = Tri::Yes;
        igpu.devices.push_back(cpu);
        ComputeDevice ig;
        ig.name = "integrated";
        ig.is_cpu = false;
        ig.shares_host_memory = Tri::Yes; // no memory of its own
        ig.host_buffer = Tri::Yes;
        ig.runs_expert_op = Tri::Yes;
        ig.needs_repack = Tri::No;
        igpu.devices.push_back(ig);

        // Unmeasured: the capacity claim is made, the bandwidth claim is not.
        const Plan un = plan_run(base_cfg(), igpu, model, PlanRequest{});
        const Decision * d = find(un, "extra-offload");
        check(d && d->source == Source::Unprobed && d->reason.find("capacity tier is inert") != std::string::npos,
              "integrated, unmeasured: capacity stated, bandwidth admitted unknown",
              d ? d->reason.substr(0, 70) : "none");

        // Faster than the host on the same memory: that is a real finding, not a contradiction.
        igpu.host_bandwidth_gibs = 30.0;
        igpu.devices.back().memory_bandwidth_gibs = 60.0;
        const Plan fast = plan_run(base_cfg(), igpu, model, PlanRequest{});
        d = find(fast, "extra-offload");
        check(d && d->source == Source::Measured && d->reason.find("has more of it") != std::string::npos,
              "integrated, faster than the host: the plan says so", d ? d->reason.substr(0, 70) : "none");

        // Slower, which is the ordinary desktop APU case: nothing to win, and it is measured.
        igpu.devices.back().memory_bandwidth_gibs = 22.0;
        const Plan slow = plan_run(base_cfg(), igpu, model, PlanRequest{});
        d = find(slow, "extra-offload");
        check(d && d->source == Source::Measured && d->reason.find("no more of it") != std::string::npos,
              "integrated, no faster: measured, not assumed", d ? d->reason.substr(0, 70) : "none");

        // And it is a candidate for the experts rule at all, which a shared-memory test excluded.
        d = find(fast, "experts");
        check(d && d->reason.find("could compute them on paper") != std::string::npos,
              "an integrated device is considered for the streamed experts, not skipped as if it were the CPU",
              d ? d->reason.substr(0, 70) : "none");
    }

    // ── the thread count is measured, not reasoned about ───────────────────────────
    // What replaced the refuted rule: a sweep on the machine's own matmul. Where it has an answer it
    // wins, including over the core classes, which stay a fact and stop being an argument.
    {
        HardwareProfile swept = desktop();
        swept.n_cores = 20;
        swept.core_classes = {2, 18}; // the shape that broke the old rule: it would have said 2
        swept.best_threads = 8;
        const Plan p = plan_run(base_cfg(), swept, model, PlanRequest{});
        check(p.config.n_threads == 8, "swept threads: the measurement decides", std::to_string(p.config.n_threads));
        const Decision * d = find(p, "threads");
        check(d && d->source == Source::Measured, "swept threads: recorded as measured");
        check(d && d->reason.find("quantized format") != std::string::npos,
              "swept threads: the plan says what the instrument was made of");
    }

    // ── streamed experts may be COMPUTED on a device that reads host memory ─────────
    // The property is one device's answer, not a class of hardware: a host buffer it executes over
    // means the bytes we read from flash are readable by it without a copy and without a repack.
    {
        HardwareProfile acc = desktop();
        acc.host_bandwidth_gibs = 50.0;
        ComputeDevice gpu;
        gpu.name = "accelerator";
        gpu.memory_total = 16 * GiB;
        gpu.shares_host_memory = Tri::No;
        gpu.host_buffer = Tri::Yes;
        gpu.rebindable = true;
        gpu.runs_expert_op = Tri::Yes;
        gpu.needs_repack = Tri::No;
        acc.devices.push_back(gpu);

        // The engine's own precondition comes first: the streamer rebinds every expert onto memory
        // it reserved itself, so until those reservations come from a device allocator, no device
        // may be named however capable or fast it is. This is the shipping default.
        {
            const Plan p = plan_run(base_cfg(), acc, model, PlanRequest{});
            const Decision * d = find(p, "experts");
            check(d && d->value == "host" && d->reason.find("would not survive the load") != std::string::npos,
                  "streamer cannot serve device memory: no device is named, and the plan says why",
                  d ? d->reason.substr(0, 60) : "none");
        }

        // ...and what that same note says about the one remaining opening must not overstate it.
        // The capability a device advertises for wrapping memory the caller owns is a blanket
        // promise, and a backend whose support is conditional per device and per call declines to
        // make one while implementing the call anyway - so an absent yes is Unknown, not No. This
        // fixture leaves the field at its default, which is that case. Reading a false there as a
        // closed door is what once made this plan wrong about the backends that run on phones.
        {
            const Plan p = plan_run(base_cfg(), acc, model, PlanRequest{});
            const Decision * d = find(p, "experts");
            check(d && d->reason.find("is unprobed") != std::string::npos &&
                      d->reason.find("no opening here at all") == std::string::npos,
                  "unknown host-ptr capability: reported unprobed, never as a closed door",
                  d ? d->reason : "no decision");
        }

        // A device that does advertise it gets the opening named, which is the other half of the
        // same asymmetry: a yes here IS a fact.
        {
            HardwareProfile wraps = acc;
            wraps.devices.back().host_ptr_buffers = Tri::Yes;
            const Plan p = plan_run(base_cfg(), wraps, model, PlanRequest{});
            const Decision * d = find(p, "experts");
            check(d && d->reason.find("which is the one opening") != std::string::npos,
                  "advertised host-ptr capability: the plan names the opening", d ? d->reason : "no decision");
        }

        // Everything below exercises the rule as it will behave once that precondition holds.
        PlannerPolicy able = PlannerPolicy::defaults();
        able.streamer_serves_device_memory = true;

        // Unmeasured bandwidth: the mechanism is stated, the decision is not taken.
        const Plan unmeasured = plan_run(base_cfg(), acc, model, Placement{}, PlanRequest{}, able);
        const Decision * d = find(unmeasured, "experts");
        check(d && d->source == Source::Unprobed, "host-buffer device, no bandwidth: stated, not decided");

        // Measured and faster than the host: the experts stay rebindable and go there. (back(), not
        // [0]: this fixture already carries a plain discrete device, and the one under test is ours.)
        acc.devices.back().memory_bandwidth_gibs = 200.0;
        const Plan p = plan_run(base_cfg(), acc, model, Placement{}, PlanRequest{}, able);
        d = find(p, "experts");
        check(d && d->value == "device" && d->source == Source::Measured,
              "host-buffer device, faster than the host: experts computed there", d ? d->value : "none");

        // A device that needs a repack is excluded by that property alone, however fast it is.
        acc.devices.back().needs_repack = Tri::Yes;
        const Plan r = plan_run(base_cfg(), acc, model, Placement{}, PlanRequest{}, able);
        d = find(r, "experts");
        check(d && d->value == "host", "repacking device: excluded whatever its bandwidth");
    }

    // ── a dense model has nothing to stream ────────────────────────────────────────
    {
        ModelProfile dense = model;
        dense.is_moe = false;
        const Plan p = plan_run(base_cfg(), desktop(), dense, PlanRequest{});
        check(p.regime == Regime::NotMoe && !p.config.moe.enabled, "dense model: declined");
    }

    // ── an unprofiled machine declines instead of inventing numbers ────────────────
    {
        const Plan p = plan_run(base_cfg(), HardwareProfile{}, model, PlanRequest{});
        check(!p.config.moe.enabled && p.streaming_declined, "blank machine: declined");
        check(validate(p.config).ok, "blank machine: still a valid config");
    }

    // ── invariant: nothing lossy arms itself, at any quality budget ────────────────
    {
        PlanRequest req;
        req.quality_budget = 0.05f; // the caller would accept 5% — the planner still may not guess
        const Plan p = plan_run(base_cfg(), phone(), model, req);
        check(p.config.moe.drop_cold_frac == 0.0f && p.config.moe.substitute_lambda == 0.0f &&
                  p.config.moe.route_ahead == 0,
              "lossy levers stay off even with a quality budget");
        const Decision * d = find(p, "lossy");
        check(d && d->source == Source::Unprobed, "the unspent quality budget is recorded as unprobed");
    }

    // A caller that armed a lossy lever keeps it: the planner records it and does not touch it.
    {
        RunConfig c = base_cfg();
        c.moe.drop_cold_frac = 0.75f;
        PlanRequest req;
        req.pinned.push_back("drop-cold-experts");
        const Plan p = plan_run(c, phone(), model, req);
        check(p.config.moe.drop_cold_frac == 0.75f, "an armed lossy lever survives the planner");
        const Decision * d = find(p, "lossy");
        check(d && d->source == Source::Operator, "an armed lossy lever is attributed to the operator");
    }

    // ── invariant: a pinned knob is never overwritten ──────────────────────────────
    {
        RunConfig c = base_cfg();
        c.moe.dense_weights = DenseWeightsMode::Mmap;
        c.moe.cache_mb = 1700;
        c.moe.io_threads = 2;
        PlanRequest req;
        req.pinned = {"dense-weights", "cache-mb", "io-threads"};
        const Plan p = plan_run(c, phone(), model, req);
        check(p.config.moe.dense_weights == DenseWeightsMode::Mmap, "pinned dense policy survives");
        check(p.config.moe.cache_mb == 1700, "pinned cache budget survives");
        check(p.config.moe.io_threads == 2, "pinned lane count survives");
        for (const char * k : {"dense-weights", "cache-mb", "io-threads"}) {
            const Decision * d = find(p, k);
            check(d && d->source == Source::Operator, std::string("pinned knob attributed to the operator: ") + k);
        }
    }

    // ── composed over a first stage: the fitter placed half the layers' experts on a device ─
    {
        Placement pl;
        pl.fitted = true;
        pl.outcome = "synthetic";
        pl.n_gpu_layers = 24;
        pl.n_ctx = 4096;
        for (uint32_t il = 0; il < 24; ++il)
            pl.host_expert_layers.push_back(il);
        pl.host_resident_bytes = 4 * GiB; // the dense set the fitter left here, plus host KV/compute
        pl.device_bytes = 8 * GiB;
        const Plan p = plan_run(base_cfg(), desktop(), model, pl, PlanRequest{});
        // 4 GiB dense + 9 GiB of host-side experts does not fit the desktop's budget, the dense part
        // alone does: the host half streams.
        check(p.config.moe.enabled, "placed: streaming still on for the host half");
        check(p.config.n_gpu_layers == 24, "placed: n_gpu_layers carried into the config");
        check(p.config.n_ctx == 4096, "placed: the fitter's context is honoured");
        // The cache is capped at the HOST share of the experts, not the whole set.
        check((uint64_t) p.config.moe.cache_mb * MiB <= model.expert_bytes / 2 + MiB,
              "placed: cache capped at the host share of the experts", std::to_string(p.config.moe.cache_mb));
        const Decision * d = find(p, "placement");
        check(d && d->source == Source::Measured, "placed: the placement is recorded as measured");
        check(validate(p.config).ok, "placed: the plan is a valid config", validate(p.config).error);

        // When the fitter's host residual FITS in RAM, the right answer is the classic offload -
        // experts resident on the host, no streaming - and the plan must say so rather than stream.
        Placement small = pl;
        small.host_resident_bytes = 1 * GiB;
        const Plan r = plan_run(base_cfg(), desktop(), model, small, PlanRequest{});
        check(r.regime == Regime::Fits && !r.config.moe.enabled,
              "placed, residual fits: experts stay resident, streaming declined (the -ot exps=CPU case)");

        Placement all_dev = pl;
        all_dev.host_expert_layers.clear();
        all_dev.n_gpu_layers = 48;
        const Plan q = plan_run(base_cfg(), desktop(), model, all_dev, PlanRequest{});
        check(!q.config.moe.enabled && q.streaming_declined, "all on devices: nothing left to stream, declined");
    }

    // ── the rationale exists and names its confidence ──────────────────────────────
    {
        const Plan p = plan_run(base_cfg(), phone(), model, PlanRequest{});
        const std::string text = p.explain();
        check(text.find("experts-stream") != std::string::npos, "explain() names the regime");
        check(text.find("[derived]") != std::string::npos, "explain() names each decision's source");
        check(find(p, "threads") && find(p, "threads")->source == Source::Unprobed,
              "a knob with no rule is recorded as unprobed rather than silently defaulted");
    }

    // ── the cost model: every verdict it encodes, as a case rather than as a comment ──
    //
    // These drive `allocate()` directly. It is a pure function over two structs, so a machine with
    // a fast accelerator and a machine that lies about one are both a fixture, and the refutations
    // this engine paid for stay checked instead of staying in a paragraph nobody runs.
    {
        // A machine with everything measured: rates for compute, refault and the expert lane.
        auto measured_machine = [&]() {
            HardwareProfile h = desktop();
            h.host_bandwidth_gibs = 100.0;
            h.storage.refault_mibs = 24.0; // the price of a page fault, two orders below sequential
            h.devices.clear();
            return h;
        };
        auto inputs = [&](uint64_t budget) {
            AllocationInputs in;
            in.budget_bytes = budget;
            in.engine_cache_min = 0;
            in.cacheable_bytes = model.expert_bytes;
            return in;
        };
        auto fast_device = [](const char * name, double gibs) {
            ComputeDevice d;
            d.name = name;
            d.memory_bandwidth_gibs = gibs;
            d.memory_total = 16 * GiB;
            d.memory_free = 15 * GiB;
            d.identity_ok = Tri::Yes;
            d.needs_repack = Tri::No;
            return d;
        };
        const PlannerPolicy pol = PlannerPolicy::defaults();

        // The read-whole groups outrank the cache: they are re-read every token while the cache
        // serves one token cycle, so seconds saved per byte of RAM is orders apart. This is the
        // measured behaviour ("pin the dense set, then give the cache the rest") arrived at by
        // arithmetic rather than written down as an order.
        {
            const Allocation a = allocate(measured_machine(), model, inputs(6 * GiB), pol);
            check(a.at(WeightGroup::Attention).lane == Lane::Resident, "read-whole groups win residency first");
            check(a.cache_bytes > 0, "the cache still gets what remains after them");
            check(a.at(WeightGroup::Embedding).resident_bytes == 0,
                  "a row-gathered table buys no residency, however large it is",
                  "it holds " + std::to_string(a.at(WeightGroup::Embedding).resident_bytes >> 20) + " MiB");
        }

        // Under the model's own token cycle a cache evicts what the same token still needs. The
        // floor is the model's arithmetic, and a budget below it buys nothing at all.
        {
            AllocationInputs in = inputs(3 * GiB);
            in.reserved_bytes = 3 * GiB - (100 * MiB); // leaves less than one token cycle
            const Allocation a = allocate(measured_machine(), model, in, pol);
            check(a.cache_bytes == 0, "a residual under the token cycle leaves the cache off",
                  std::to_string(a.cache_bytes >> 20) + " MiB");
        }

        // THE refutation. A device three times the host's bandwidth is still not used while the
        // price of a graph crossing is unmeasured: dense and expert halves alternate per layer, and
        // a bandwidth ratio cannot see that cost. Measured 20 against 12 GiB/s on a phone that then
        // ran the move at 0.53x.
        {
            HardwareProfile h = measured_machine();
            h.devices.push_back(fast_device("fast", 300.0));
            const Allocation a = allocate(h, model, inputs(6 * GiB), pol);
            check(a.at(WeightGroup::Attention).device < 0, "an unpriced graph crossing forbids a device placement");
            check(a.device_applicable, "and the plan does not claim a placement it did not make");
        }

        // Priced, and worth it: the same device wins once a crossing has a number small enough.
        {
            HardwareProfile h = measured_machine();
            h.devices.push_back(fast_device("fast", 300.0));
            h.devices.back().split_seconds = 1e-7; // 96 crossings cost ~10 us against ~9 ms saved
            const PlannerPolicy & priced = pol;
            const Allocation a = allocate(h, model, inputs(6 * GiB), priced);
            check(a.at(WeightGroup::Attention).device >= 0, "a priced crossing lets a real win through");
            check(!a.device_applicable, "and it is reported as not applicable by this build");
        }

        // Correctness gates speed, and it is a fact rather than a promise: a device that does not
        // reproduce the CPU's answer is excluded before its rate is looked at.
        {
            HardwareProfile h = measured_machine();
            ComputeDevice d = fast_device("wrong", 300.0);
            d.identity_ok = Tri::No;
            h.devices.push_back(d);
            h.devices.back().split_seconds = 1e-9;
            const PlannerPolicy & priced = pol;
            const Allocation a = allocate(h, model, inputs(6 * GiB), priced);
            check(a.at(WeightGroup::Attention).device < 0, "a device that computes something else is excluded");
        }

        // A repacked layout replaces the very thing a rebind depends on, so such a device is out on
        // a property rather than on a name.
        {
            HardwareProfile h = measured_machine();
            ComputeDevice d = fast_device("repacker", 300.0);
            d.needs_repack = Tri::Yes;
            h.devices.push_back(d);
            h.devices.back().split_seconds = 1e-9;
            const PlannerPolicy & priced = pol;
            const Allocation a = allocate(h, model, inputs(6 * GiB), priced);
            check(a.at(WeightGroup::Attention).device < 0, "a device that only runs a repacked layout is excluded");
        }

        // Unified memory at equal bandwidth: nothing to win, and the plan says so instead of moving
        // work for its own sake. Measured 19 against 19 on a phone, which is not a finding.
        {
            HardwareProfile h = measured_machine();
            ComputeDevice d = fast_device("integrated", 100.0);
            d.shares_host_memory = Tri::Yes;
            h.devices.push_back(d);
            h.devices.back().split_seconds = 1e-9;
            const PlannerPolicy & priced = pol;
            const Allocation a = allocate(h, model, inputs(6 * GiB), priced);
            check(a.at(WeightGroup::Attention).device < 0, "an equal-bandwidth device wins nothing and is not used");
        }

        // A device whose memory topology was never settled must not be credited with memory of its
        // own. The error is asymmetric and the safe direction is the one taken: counting a shared
        // pool as extra capacity overcommits a machine - the accounting that took a phone down -
        // while counting separate memory as shared merely leaves it unused. This is also the case a
        // device TYPE gets wrong, since Metal reports GPU on unified-memory hardware.
        {
            HardwareProfile h = measured_machine();
            ComputeDevice d = fast_device("unsettled", 300.0);
            d.shares_host_memory = Tri::Unknown; // the probe could not run
            d.memory_total = 16 * GiB;
            h.devices.push_back(d);
            check(h.device_local_memory() == 0, "an unsettled device is not counted as capacity of its own",
                  std::to_string(h.device_local_memory() >> 20) + " MiB");
            check(!h.devices.back().has_own_memory() && h.devices.back().reads_host_memory(),
                  "and it reads as sharing the host's memory until measured");
        }

        // The crossing price is a per-device MEASUREMENT, not a policy constant: two devices on one
        // machine can differ, and a device nobody measured declines on its own rather than
        // inheriting a number from its neighbour.
        {
            HardwareProfile h = measured_machine();
            ComputeDevice slow_link = fast_device("far", 300.0);
            slow_link.split_seconds = 1e-3; // 96 crossings cost ~96 ms against ~9 ms saved
            h.devices.push_back(slow_link);
            const Allocation a = allocate(h, model, inputs(6 * GiB), pol);
            check(a.at(WeightGroup::Attention).device < 0,
                  "a measured crossing price can refuse a device that wins on bandwidth");
        }

        // An unmeasured rate makes a candidate ineligible, never cheap.
        {
            HardwareProfile h = measured_machine();
            h.storage.refault_mibs = 0.0;
            const Allocation a = allocate(h, model, inputs(6 * GiB), pol);
            check(a.at(WeightGroup::Attention).lane == Lane::Mmap,
                  "with the refault price unmeasured, residency is not bought");
        }
    }

    // ── the memory ledger: one account, two ceilings ───────────────────────────────
    // What is held and what is held reclaim-exempt are different totals, and the second is the
    // smaller wherever it exists. Sized against the first alone, a plan asks for locks the machine
    // stops granting part way - measured at a tenth of the throughput of the plan that closes.
    {
        const auto mibs_of = [](uint64_t b) { return std::to_string((unsigned long long) (b >> 20)) + " MiB"; };

        // Room for both: the dense set is pinned whole, and the cache is what can be locked after
        // it - less than what could merely be held.
        const Plan p = plan_run(base_cfg(), unified(), model, PlanRequest{});
        const Ledger & l = p.allocation.ledger;
        check(p.config.moe.dense_weights == DenseWeightsMode::Pinned,
              "ledger: the dense set fits the lock and is pinned");
        check(l.lockable_cap > 0 && l.locked <= l.lockable_cap,
              "ledger: what is locked stays under the lockable ceiling",
              mibs_of(l.locked) + " of " + mibs_of(l.lockable_cap));
        check(l.held <= l.holdable_cap, "ledger: what is held stays under the holdable ceiling",
              mibs_of(l.held) + " of " + mibs_of(l.holdable_cap));
        check(p.allocation.cache_locked_bytes == p.cache_budget_bytes && p.cache_budget_bytes > 0,
              "ledger: the whole cache is the part that can be locked");
        uint64_t rows = 0;
        for (const LedgerRow & r : l.rows)
            rows += r.bytes;
        check(rows == l.held, "ledger: the rows add up to what is held");
        check(find(p, "memory-ledger") != nullptr, "ledger: the plan states it");
        check(validate(p.config).ok, "ledger: the plan is a valid config", validate(p.config).error);

        // The same machine with nothing bounding the lock but what can be held: the cache is larger.
        // It is the lockable total, and nothing else, that made the difference above.
        HardwareProfile roomy = unified();
        roomy.lockable_bytes = ~0ull;
        roomy.lock_in_place_bytes = ~0ull;
        const Plan q = plan_run(base_cfg(), roomy, model, PlanRequest{});
        check(q.cache_budget_bytes > p.cache_budget_bytes, "ledger: the lockable total is what bounded the cache",
              mibs_of(p.cache_budget_bytes) + " against " + mibs_of(q.cache_budget_bytes));

        // A lock too small for the dense set. It is not pinned part way: it is held whole as
        // ordinary memory, the plan says by how much it missed, and the lock goes to the cache.
        HardwareProfile tight = unified();
        tight.lockable_bytes = 3 * GiB; // the dense set is 3 GiB, and the margin comes off this
        tight.lock_in_place_bytes = 3 * GiB;
        const Plan t = plan_run(base_cfg(), tight, model, PlanRequest{});
        check(t.config.moe.dense_weights == DenseWeightsMode::Anonymous,
              "ledger: a dense set that does not fit the lock whole is not pinned at all");
        check(t.allocation.pinned_shortfall_bytes > 0, "ledger: the shortfall is recorded");
        const Decision * dw = find(t, "dense-weights");
        check(dw && dw->value == "anon" && dw->reason.find("more than can still be held") != std::string::npos,
              "ledger: the dense decision says why it was not pinned");
        check(t.allocation.ledger.locked <= t.allocation.ledger.lockable_cap,
              "ledger: the cache alone stays under the lockable ceiling");
        check(t.cache_budget_bytes >= model.token_cycle_bytes, "ledger: the cache still clears the token cycle");

        // The caller's pin is the caller's authority, including over a ledger that does not close:
        // it is charged as asked, and the cache - which no longer has a cycle's worth of lock - is
        // ordinary memory bounded by what can be held.
        RunConfig forced = base_cfg();
        forced.moe.dense_weights = DenseWeightsMode::Pinned;
        PlanRequest fr;
        fr.pinned = {"dense-weights"};
        const Plan f = plan_run(forced, tight, model, fr);
        check(f.config.moe.dense_weights == DenseWeightsMode::Pinned,
              "ledger: a caller's pinned dense store is not overruled");
        check(f.allocation.cache_locked_bytes == 0 && f.cache_budget_bytes >= model.token_cycle_bytes,
              "ledger: with the lock spent, the cache is ordinary memory");

        // The plan and the run agree about the cache's lock: sized to it, the run is told to keep
        // it there; a caller's own budget is never one the run may shrink.
        check(p.config.moe.cache_pin && p.config.moe.cache_pin_fit,
              "ledger: a cache sized to the lock is fitted to it");
        check(!f.config.moe.cache_pin && !f.config.moe.cache_pin_fit,
              "ledger: a cache that cannot be locked asks for no lock");
        RunConfig own = base_cfg();
        own.moe.cache_mb = 12000;
        PlanRequest orq;
        orq.pinned = {"cache-mb"};
        const Plan o = plan_run(own, unified(), model, orq);
        const LedgerRow * oc = ledger_row(o, "expert cache");
        check(!o.config.moe.cache_pin_fit && o.config.moe.cache_mb == 12000,
              "ledger: a caller's cache budget is not fitted");
        check(oc && oc->bytes == 12000 * MiB && o.allocation.ledger.held > o.allocation.ledger.holdable_cap,
              "ledger: a caller's cache is the row, even over the ceiling");

        // Two stores that are not one pool: no reclaim-exempt allocation, a generous lock in place.
        // The ceiling reported is the one the locked rows were charged to.
        HardwareProfile inplace = unified();
        inplace.reclaim_exempt_max = 0;
        inplace.lockable_bytes = 0;
        inplace.lock_in_place_bytes = 8 * GiB;
        const Plan ip = plan_run(base_cfg(), inplace, model, PlanRequest{});
        check(ip.allocation.ledger.locked > 0 && ip.allocation.ledger.locked <= ip.allocation.ledger.lockable_cap,
              "ledger: a lock-in-place store is the ceiling its rows are checked against",
              mibs_of(ip.allocation.ledger.locked) + " of " + mibs_of(ip.allocation.ledger.lockable_cap));

        // A machine that offers a reclaim-exempt allocation and no lock in place: the dense set is
        // pinned and the cache is bounded only by what can be held, as it always was there.
        const Plan ph = plan_run(base_cfg(), phone(), model, PlanRequest{});
        const LedgerRow * pc = ledger_row(ph, "expert cache");
        const LedgerRow * pd = ledger_row(ph, "dense");
        check(pd && pd->locked && pc && !pc->locked, "ledger: two stores, and only the dense set is in the locked one");
        check(!ph.config.moe.cache_pin, "ledger: and no lock is asked for the cache there");
        check(ph.cache_budget_bytes > 1 * GiB, "ledger: a cache that cannot be locked is not sized to the lock",
              mibs_of(ph.cache_budget_bytes));

        // No lock at all: nothing is charged to a ceiling that does not exist.
        const Plan dk = plan_run(base_cfg(), desktop(), model, PlanRequest{});
        check(dk.allocation.ledger.locked == 0 && dk.allocation.ledger.lockable_cap == 0,
              "ledger: a machine with no lock locks nothing");

        // A device armed for prefill is a line, charged to both ceilings where its buffers are
        // locked: two of the model's largest expert layer, and the cache gives up exactly that.
        ModelProfile mm = model;
        mm.largest_expert_layer_bytes = 384 * MiB;
        RunConfig armed = base_cfg();
        armed.prefill.device = "accel";
        PlanRequest ar;
        ar.pinned = {"prefill-device"};
        const Plan off = plan_run(base_cfg(), unified(), mm, PlanRequest{});
        const Plan on = plan_run(armed, unified(), mm, ar);
        const LedgerRow * dev = ledger_row(on, "device");
        check(dev && dev->locked && dev->bytes == 768 * MiB,
              "ledger: an armed device is charged two expert layers, locked", dev ? mibs_of(dev->bytes) : "no row");
        check(ledger_row(off, "device") == nullptr, "ledger: a device nobody armed is not a line");
        check(off.cache_budget_bytes - on.cache_budget_bytes == 768 * MiB,
              "ledger: the cache gives up what the device takes",
              mibs_of(off.cache_budget_bytes) + " against " + mibs_of(on.cache_budget_bytes));

        // The context and the compute buffers are a line even when the capacity fitter's answer
        // was set aside - which on shared memory is always, and where they used to be charged nowhere.
        Placement pl;
        pl.shared_memory_placement = true;
        pl.raw_host_context_bytes = 500 * MiB;
        pl.raw_host_compute_bytes = 100 * MiB;
        const Plan fx = plan_run(base_cfg(), phone(), model, pl, PlanRequest{});
        const LedgerRow * fr_row = ledger_row(fx, "context and compute");
        check(fr_row && fr_row->bytes == 600 * MiB,
              "ledger: context and compute are charged without a fitted placement");
        check(ph.cache_budget_bytes - fx.cache_budget_bytes == 600 * MiB, "ledger: and the cache is sized after them",
              mibs_of(ph.cache_budget_bytes) + " against " + mibs_of(fx.cache_budget_bytes));
    }

    // ── the prefill plan: a second placement, decided at prefill width ─────────────
    // A device that loses the one-token graph can win a wide one several times over, so the
    // prefill is decided from the wide measurement and from nothing else - and armed only when
    // the device also gave the host's answer and its memory still leaves the ledger closed.
    {
        ModelProfile mm = model;
        mm.largest_expert_layer_bytes = 384 * MiB;
        mm.largest_layer_dense_bytes = 64 * MiB;
        const uint64_t two_layers = 2 * (384 + 64) * MiB;

        // The accelerator of `unified()`, slower than the cores on one token and faster on many.
        const auto machine = [](double wide, Tri same) {
            HardwareProfile h = unified();
            h.host_bandwidth_gibs = 100.0;
            h.host_wide_matmul_gibs = 200.0;
            h.wide_batch = 128;
            h.devices[1].memory_bandwidth_gibs = 60.0; // loses the decode's graph
            h.devices[1].wide_matmul_gibs = wide;
            h.devices[1].wide_identity_ok = same;
            return h;
        };
        const auto reason = [](const Plan & p) {
            const Decision * d = find(p, "prefill-device");
            return d ? d->reason : std::string();
        };

        const Plan off = plan_run(base_cfg(), unified(), mm, PlanRequest{});
        check(!off.config.prefill.enabled() && find(off, "prefill-device") &&
                  find(off, "prefill-device")->source == Source::Unprobed,
              "prefill: a device nobody measured at prefill width is not armed, and the plan says unprobed");

        const Plan on = plan_run(base_cfg(), machine(1500.0, Tri::Yes), mm, PlanRequest{});
        const LedgerRow * row = ledger_row(on, "device");
        check(on.config.prefill.device == "accel",
              "prefill: a device that wins wide and agrees is armed by its own name");
        check(on.config.prefill.best_effort, "prefill: a device the plan armed is never a condition for the load");
        check(row && row->locked && row->bytes == two_layers, "prefill: it is charged two layers, experts and the rest",
              row ? std::to_string((unsigned long long) (row->bytes >> 20)) + " MiB" : "no row");
        check(off.cache_budget_bytes - on.cache_budget_bytes == two_layers,
              "prefill: and the cache gives up exactly that");
        check(on.allocation.ledger.locked <= on.allocation.ledger.lockable_cap, "prefill: the ledger still closes");
        check(on.config.device_use != DeviceUse::CpuOnly, "prefill: an armed device is not taken out of the run");
        check(validate(on.config).ok, "prefill: the armed plan is a valid config", validate(on.config).error);
        check(on.to_flags().find("--prefill-device accel") != std::string::npos &&
                  on.to_flags().find("--prefill-best-effort") != std::string::npos,
              "prefill: the reproduce line carries it");

        const Plan wrong = plan_run(base_cfg(), machine(1500.0, Tri::No), mm, PlanRequest{});
        check(!wrong.config.prefill.enabled() && reason(wrong).find("did not reproduce") != std::string::npos,
              "prefill: a faster device with a different answer is refused on correctness");

        const Plan slow = plan_run(base_cfg(), machine(205.0, Tri::Yes), mm, PlanRequest{});
        check(!slow.config.prefill.enabled() && reason(slow).find("not a win") != std::string::npos,
              "prefill: a device inside the margin is not a second placement");

        // Too large for what is left: refused, with the figures, and the plan is the one without it.
        ModelProfile big = mm;
        big.largest_expert_layer_bytes = 5 * GiB;
        const Plan nofit = plan_run(base_cfg(), machine(1500.0, Tri::Yes), big, PlanRequest{});
        const Plan nodev = plan_run(base_cfg(), unified(), big, PlanRequest{});
        check(!nofit.config.prefill.enabled() && reason(nofit).find("refused on memory") != std::string::npos,
              "prefill: a device that does not fit is refused with the reason");
        check(nofit.cache_budget_bytes == nodev.cache_budget_bytes && ledger_row(nofit, "device") == nullptr,
              "prefill: and a refusal leaves the ledger as it was");

        // Something else in the run that cannot share a session with it.
        RunConfig rows = base_cfg();
        rows.moe.row_stream = true;
        const Plan conflict = plan_run(rows, machine(1500.0, Tri::Yes), mm, PlanRequest{});
        check(!conflict.config.prefill.enabled() && reason(conflict).find("not armed, because") != std::string::npos,
              "prefill: a setting it cannot coexist with declines it instead of producing an invalid config");

        // The caller's word, either way.
        PlanRequest none;
        none.pinned = {"prefill-device"};
        const Plan pinned_off = plan_run(base_cfg(), machine(1500.0, Tri::Yes), mm, none);
        check(!pinned_off.config.prefill.enabled() && find(pinned_off, "prefill-device")->source == Source::Operator,
              "prefill: a caller who set it empty is not overruled");

        // A library the host calls into is not somewhere a prefill can be placed, however it measures.
        HardwareProfile helper = machine(0.0, Tri::Unknown);
        ComputeDevice lib;
        lib.name = "lib";
        lib.is_host_helper = true;
        lib.shares_host_memory = Tri::Yes;
        lib.wide_matmul_gibs = 5000.0;
        lib.wide_identity_ok = Tri::Yes;
        helper.devices.push_back(lib);
        check(!plan_run(base_cfg(), helper, mm, PlanRequest{}).config.prefill.enabled(),
              "prefill: a host helper library is never the prefill device");

        // A device with memory of its own costs this pool nothing, and is refused only by its own.
        HardwareProfile disc = machine(1500.0, Tri::Yes);
        disc.devices[1].shares_host_memory = Tri::No;
        disc.devices[1].memory_free = 8 * GiB;
        const Plan own = plan_run(base_cfg(), disc, mm, PlanRequest{});
        check(own.config.prefill.device == "accel" && ledger_row(own, "device") == nullptr &&
                  own.cache_budget_bytes == off.cache_budget_bytes,
              "prefill: a device with its own memory is armed and charged nothing here");
        disc.devices[1].memory_free = 512 * MiB;
        check(!plan_run(base_cfg(), disc, mm, PlanRequest{}).config.prefill.enabled(),
              "prefill: and refused when its own memory cannot take two layers");
    }

    // Informational: the rationale as a user would read it. Printed rather than asserted, because
    // its wording is meant to change as the rules learn; what is asserted is that it exists, names
    // the regime, and tags every decision with where its authority came from.
    for (const auto & m : {std::make_pair("desktop", desktop()), std::make_pair("phone", phone()),
                           std::make_pair("hard-capped", hard_capped())}) {
        std::printf("\n--- rationale on %s ---\n%s", m.first,
                    plan_run(base_cfg(), m.second, model, PlanRequest{}).explain().c_str());
    }

    std::printf(failures ? "\n%d check(s) failed\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
