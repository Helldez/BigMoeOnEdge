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
    gpu.host_memory = false;
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
    igpu.host_memory = true; // moving a tensor off it frees nothing
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
