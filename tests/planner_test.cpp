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
#include "bmoe/backend_choice.h"
#include "bmoe/planner.h"
#include "bmoe/probe.h"

#include <cmath>
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

// A platform that answers memory pressure by killing the process.
static HardwareProfile hard_capped() {
    HardwareProfile h;
    h.label = "hard per-process cap";
    h.residency_budget = 5 * GiB;
    h.memory_total = 8 * GiB;
    h.anon_overflow = Overflow::Kill;
    h.reclaim_exempt_max = 0;
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

    // ── the embedding tables the allocation prices as left mapped reach the run ─────
    // A per-layer table of tens of GB is gathered by row, so the plan leaves it mapped; the run's
    // Anonymous policy would copy it whole and the control group would end the run (#424). The names
    // come from the file's grouping, and the run is handed exactly those.
    {
        ModelProfile tabled = model;
        tabled.embedding_tensors = {"token_embd.weight", "per_layer_token_embd.weight"};
        tabled.group(WeightGroup::Embedding).bytes = 28 * GiB;
        tabled.dense_bytes += 28 * GiB - 344 * MiB;
        tabled.file_bytes += 28 * GiB - 344 * MiB;
        tabled.largest_dense_tensor = 28 * GiB;
        const Plan p = plan_run(base_cfg(), desktop(), tabled, PlanRequest{});
        const Lane lane = p.allocation.at(WeightGroup::Embedding).lane;
        check(lane != Lane::Resident && lane != Lane::Pinned, "tables: the plan does not make a 28 GB table resident",
              lane_name(lane));
        check(p.config.moe.dense_weights == DenseWeightsMode::Anonymous, "tables: the dense policy still copies");
        check(p.config.moe.dense_mapped == tabled.embedding_tensors,
              "tables: the run is handed the embedding tables the plan priced as mapped");
        check(find(p, "dense-mapped") != nullptr, "tables: the decision is recorded");
        check(validate(p.config).ok, "tables: the plan is a valid config", validate(p.config).error);

        // A caller who pinned the list keeps it, whatever the allocation says.
        PlanRequest pinned;
        pinned.pinned.push_back("dense-mapped");
        RunConfig keep = base_cfg();
        keep.moe.dense_mapped = {"token_embd.weight"};
        const Plan q = plan_run(keep, desktop(), tabled, pinned);
        check(q.config.moe.dense_mapped == std::vector<std::string>{"token_embd.weight"},
              "tables: a pinned list is the caller's");
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
        // A fatal limit is a reason for a wider margin, not for leaving the dense set where the kernel
        // may drop it: every token reads all of it, and a dropped page is read back from flash inside
        // the decode. Held, it is charged to the ledger like anywhere else, and the ledger closes.
        check(p.config.moe.dense_weights == DenseWeightsMode::Anonymous,
              "hard cap: dense held in memory, not left mapped");
        check(p.dense_pending_bytes > 0, "hard cap: the held dense set is charged to the ledger");
        check(p.allocation.ledger.held <= p.allocation.ledger.holdable_cap, "hard cap: the ledger closes",
              std::to_string(p.allocation.ledger.held >> 20) + " of " +
                  std::to_string(p.allocation.ledger.holdable_cap >> 20) + " MiB");
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
    // The floor turns the CACHE off. The stream stays, because the memory a cache lacks is the
    // memory the page cache lacks too: the mapped file is at its worst exactly here.
    {
        HardwareProfile tight = phone();
        tight.residency_budget = 3 * GiB + 400 * MiB; // dense fits, what is left is under the cycle
        tight.storage.refault_mibs = 24.0;            // measured, so the mapping has a cost to beat
        const Plan p = plan_run(base_cfg(), tight, model, PlanRequest{});
        check(p.config.moe.enabled && !p.streaming_declined,
              "tight machine: the experts stream with no cache rather than being left to refault", p.decline_reason);
        check(p.config.moe.cache_mb == 0 && !p.config.moe.cache_auto && !p.config.moe.force_cache,
              "tight machine: the cache is off outright, not left to size itself under the floor",
              std::to_string(p.config.moe.cache_mb) + " MiB");
        const Decision * c = find(p, "cache-mb");
        check(c && c->reason.find("token cycle") != std::string::npos,
              "tight machine: the reason names the derived floor", c ? c->reason : "no cache-mb decision");
        check(validate(p.config).ok, "tight machine: the plan is a valid config", validate(p.config).error);

        const Allocation & a = p.allocation;
        const GroupPlacement & e = a.at(WeightGroup::Experts);
        check(e.lane == Lane::ExpertStream && e.resident_bytes == 0 && a.cache_bytes == 0,
              "tight machine: the expert lane is the stream, holding nothing");
        check(e.seconds_per_token > 0.0 && e.seconds_per_token < e.seconds_worst_case,
              "tight machine: the stream is chosen on its price against the mapping",
              std::to_string(e.seconds_per_token) + " vs " + std::to_string(e.seconds_worst_case));
        check(ledger_row(p, "dense") && !ledger_row(p, "expert cache"),
              "tight machine: the ledger keeps the dense set and carries no cache");
        check(p.to_flags().find("--cache-mb 0") != std::string::npos,
              "tight machine: the reproduce line states the cache is off, which a front-end would otherwise size",
              p.to_flags());

        // The same machine with a drive nobody measured. There is no price for the stream, so it is
        // not chosen on faith: the plan declines, and is priced as the mapping it leaves.
        HardwareProfile blind = tight;
        blind.storage.rate_curve.clear();
        const Plan q = plan_run(base_cfg(), blind, model, PlanRequest{});
        check(!q.config.moe.enabled && q.streaming_declined,
              "tight machine, unmeasured drive: streaming declined rather than guessed");
        check(q.decline_reason.find("token cycle") != std::string::npos,
              "tight machine, unmeasured drive: the reason names the derived floor", q.decline_reason);

        // A declined run streams nothing and holds nothing: the file stays mapped, so the plan is
        // priced as that rather than as the allocation it has just refused.
        const Allocation & d = q.allocation;
        check(d.seconds_worst_case > 0.0 && d.seconds_per_token == d.seconds_worst_case,
              "tight machine, unmeasured drive: a declined plan predicts the cost of the mapping it leaves",
              std::to_string(d.seconds_per_token) + " vs " + std::to_string(d.seconds_worst_case));
        bool any_held = d.resident_bytes != 0 || d.cache_bytes != 0;
        bool all_mapped = true;
        for (const GroupPlacement & g : d.groups) {
            any_held = any_held || g.resident_bytes != 0;
            all_mapped = all_mapped && g.lane == Lane::Mmap;
        }
        check(!any_held && all_mapped,
              "tight machine, unmeasured drive: a declined plan holds no weights and streams none");
        check(!ledger_row(q, "dense") && !ledger_row(q, "expert cache"),
              "tight machine, unmeasured drive: the ledger of a declined plan carries no weights");
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

    // A hybrid model: 52 blocks, 23 with experts. The fitter's host list speaks in blocks, and a
    // count of 52 "MoE layers" out of 23 would prorate the expert set past itself.
    {
        ModelProfile hybrid = model;
        hybrid.arch = "nemotron_h_moe";
        hybrid.n_layer = 52;
        hybrid.n_moe_layer = 23;
        hybrid.moe_layers.clear();
        for (uint32_t il = 3; il < 52; il += 2)
            if (hybrid.moe_layers.size() < 23) hybrid.moe_layers.push_back(il);
        Placement pl;
        pl.fitted = true;
        pl.n_ctx = 2048;
        for (uint32_t il = 0; il < 52; ++il)
            pl.host_expert_layers.push_back(il);
        pl.host_resident_bytes = 1 * GiB;
        const Plan p = plan_run(base_cfg(), desktop(), hybrid, pl, PlanRequest{});
        const Decision * d = find(p, "placement");
        check(d && d->reason.find("23 of 23 MoE layers") != std::string::npos,
              "hybrid: the host's share counts the blocks that carry experts",
              d ? d->reason.substr(0, 120) : "no line");
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

    // ── route-ahead's gate format: the timing decides, never the machine's kind ──────
    // The int8 mirror trades bytes for arithmetic, and the two machines it was measured on answered
    // in opposite directions. So the SAME machine fixture is handed both answers below and must
    // follow each: a rule that read the platform could get at most one of them right.
    {
        RunConfig c = base_cfg();
        c.moe.route_ahead = 1;
        PlanRequest req;
        req.pinned.push_back("route-ahead");

        HardwareProfile bytes_scarce = phone();
        bytes_scarce.gate_gemv_f32_us = 475.0;
        bytes_scarce.gate_gemv_int8_us = 150.0;
        Plan p = plan_run(c, bytes_scarce, model, req);
        const Decision * d = find(p, "gate-mirror");
        check(p.config.moe.gate_mirror == GateMirror::On && d && d->source == Source::Measured,
              "gate mirror: int8 faster -> on, measured");
        check(d && d->reason.find("150 us int8 against 475 us float") != std::string::npos,
              "gate mirror: the plan quotes both timings", d ? d->reason : "none");

        HardwareProfile bytes_cheap = phone();
        bytes_cheap.gate_gemv_f32_us = 260.0;
        bytes_cheap.gate_gemv_int8_us = 380.0;
        p = plan_run(c, bytes_cheap, model, req);
        d = find(p, "gate-mirror");
        check(p.config.moe.gate_mirror == GateMirror::Off && d && d->source == Source::Measured,
              "gate mirror: float faster -> off on the same machine fixture");

        // A tie goes to the mirror: the probe ran alone, and a decode beside it only makes bytes dearer.
        HardwareProfile tie = phone();
        tie.gate_gemv_f32_us = tie.gate_gemv_int8_us = 200.0;
        p = plan_run(c, tie, model, req);
        check(p.config.moe.gate_mirror == GateMirror::On, "gate mirror: a tie goes to the mirror");

        // Unmeasured, the knob stays auto and the session times it at load; the plan says so.
        p = plan_run(c, phone(), model, req);
        d = find(p, "gate-mirror");
        check(p.config.moe.gate_mirror == GateMirror::Auto && d && d->source == Source::Unprobed,
              "gate mirror: unmeasured -> left auto, recorded as unprobed");

        // Pinned, it is the caller's even against the timing.
        RunConfig pinned = c;
        pinned.moe.gate_mirror = GateMirror::Off;
        PlanRequest preq = req;
        preq.pinned.push_back("gate-mirror");
        p = plan_run(pinned, bytes_scarce, model, preq);
        d = find(p, "gate-mirror");
        check(p.config.moe.gate_mirror == GateMirror::Off && d && d->source == Source::Operator,
              "gate mirror: a pinned value survives a timing that disagrees");

        // Without route-ahead nothing reads it: no decision, and the knob is untouched.
        p = plan_run(base_cfg(), bytes_scarce, model, PlanRequest{});
        check(p.config.moe.gate_mirror == GateMirror::Auto && !find(p, "gate-mirror"),
              "gate mirror: without route-ahead the plan neither decides nor explains it");
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

    // ── the widest graph: its compute buffers are a row, and the width is chosen against it ──
    {
        // An unfitted placement, as on a machine with no device: the context and compute buffers
        // the first stage projected for a graph as wide as the context, and what a token of width
        // costs. Nothing here names a machine; the rule reads the buffers and the holdable figure.
        auto wide = [&](uint32_t ctx, uint64_t per_token) {
            Placement pl;
            pl.n_ctx = ctx;
            pl.raw_host_context_bytes = 256 * MiB;
            pl.raw_host_compute_bytes = (uint64_t) ctx * per_token;
            pl.compute_width = ctx;
            pl.host_compute_per_token = per_token;
            return pl;
        };
        const uint64_t per_token = 256 * KiB; // 16384 tokens wide is 4 GiB of buffers
        const HardwareProfile hw = desktop();
        const PlannerPolicy pol = PlannerPolicy::defaults();
        const uint64_t cap =
            (uint64_t) ((double) (hw.holdable_bytes ? hw.holdable_bytes : hw.residency_budget) * pol.compute_share_max);

        // A small context fits its share whole: nothing changes and the knob keeps its default.
        const uint32_t narrow_ctx = 1024;
        const Plan fits = plan_run(base_cfg(), hw, model, wide(narrow_ctx, per_token), PlanRequest{});
        const Decision * df = find(fits, "ubatch");
        check((uint64_t) narrow_ctx * per_token <= cap, "width: the small context of this test is inside the share");
        check(fits.config.n_ubatch == 0 && df && df->source == Source::Derived,
              "width: buffers inside their share leave the graph as wide as the context");

        // A large one does not: the width is halved until its buffers fit, and no further.
        RunConfig big_ctx = base_cfg();
        big_ctx.n_ctx = 16384;
        const Plan cut = plan_run(big_ctx, hw, model, wide(16384, per_token), PlanRequest{});
        const Decision * dc = find(cut, "ubatch");
        const uint64_t w = (uint64_t) cut.config.n_ubatch;
        check(w > 0 && w < 16384 && dc && dc->source == Source::Policy, "width: a large context is computed in pieces",
              std::to_string(w));
        check(w * per_token <= cap && (w * 2 * per_token > cap || w * 2 > 16384),
              "width: the widest whose buffers fit, not a narrower one", std::to_string(w));
        check(cut.placement.raw_host_compute_bytes == w * per_token && cut.placement.compute_width == w,
              "width: the ledger is charged the buffers of the width chosen",
              std::to_string(cut.placement.raw_host_compute_bytes / MiB));
        // The same run with the graph held at full width by the caller: the buffers are charged in
        // full, and the cache is what pays for them.
        PlanRequest full_pin;
        full_pin.pinned = {"ubatch"};
        const Plan uncut = plan_run(big_ctx, hw, model, wide(16384, per_token), full_pin);
        check(cut.cache_budget_bytes > uncut.cache_budget_bytes || !uncut.config.moe.enabled,
              "width: what the buffers give back goes to the cache",
              std::to_string(cut.cache_budget_bytes / MiB) + " vs " + std::to_string(uncut.cache_budget_bytes / MiB));
        check(validate(cut.config).ok, "width: the plan is a valid config", validate(cut.config).error);

        // The floor holds even when the buffers still do not fit there.
        const Plan floor = plan_run(big_ctx, hw, model, wide(16384, 64 * MiB), PlanRequest{});
        check((uint32_t) floor.config.n_ubatch == pol.ubatch_min, "width: never narrower than the floor",
              std::to_string(floor.config.n_ubatch));

        // A slope nobody measured decides nothing.
        const Plan blind = plan_run(big_ctx, hw, model, wide(16384, 0), PlanRequest{});
        const Decision * du = find(blind, "ubatch");
        check(blind.config.n_ubatch == 0 && du && du->source == Source::Unprobed,
              "width: an unmeasured slope leaves the knob at its default and says so");

        // A width the caller set is the caller's, whatever it costs.
        RunConfig by_hand = big_ctx;
        by_hand.n_ubatch = 8192;
        PlanRequest pr;
        pr.pinned = {"ubatch"};
        Placement at_hand = wide(16384, per_token).at_width(8192);
        const Plan kept = plan_run(by_hand, hw, model, at_hand, pr);
        const Decision * dk = find(kept, "ubatch");
        check(kept.config.n_ubatch == 8192 && dk && dk->source == Source::Operator, "width: a pinned width is kept");

        // A device's buffers count where they come out of this pool, and not where it has its own.
        Placement with_dev = wide(4096, per_token / 2);
        with_dev.device_compute_bytes = 4096ull * per_token;
        with_dev.device_compute_per_token = per_token;
        const Plan shared = plan_run(base_cfg(), unified(), model, with_dev, PlanRequest{});
        HardwareProfile own = unified();
        for (ComputeDevice & d : own.devices) {
            d.memory_total = 24 * GiB;
            d.memory_free = 24 * GiB;
            d.shares_host_memory = Tri::No;
        }
        const Plan apart = plan_run(base_cfg(), own, model, with_dev, PlanRequest{});
        const uint32_t ws = shared.config.n_ubatch ? (uint32_t) shared.config.n_ubatch : 4096;
        const uint32_t wa = apart.config.n_ubatch ? (uint32_t) apart.config.n_ubatch : 4096;
        check(ws <= wa, "width: a device that shares this pool narrows the graph at least as much as one that does not",
              std::to_string(ws) + " vs " + std::to_string(wa));

        // at_width is a line through the projected point, clamped at zero.
        const Placement base_pl = wide(4096, per_token);
        check(base_pl.at_width(2048).raw_host_compute_bytes == 2048ull * per_token &&
                  base_pl.at_width(8192).raw_host_compute_bytes == 8192ull * per_token &&
                  base_pl.at_width(4096).raw_host_compute_bytes == base_pl.raw_host_compute_bytes,
              "width: buffers move along the measured slope, both ways");
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

        // What the fitter wrote is part of the plan, so it is part of the line that reproduces it.
        // The patterns are regexes joined by ';': quoted, or a shell would end the command there.
        Placement pinned_pl = pl;
        pinned_pl.override_patterns = {"blk\\.(0|1)\\.ffn_.*_exps", "blk\\.2\\..*"};
        const std::string placed_flags = plan_run(base_cfg(), desktop(), model, pinned_pl, PlanRequest{}).to_flags();
        check(placed_flags.find("--gpu-layers 24") != std::string::npos &&
                  placed_flags.find("--tensor-overrides 'blk\\.(0|1)\\.ffn_.*_exps;blk\\.2\\..*'") != std::string::npos,
              "placed: the reproduce line carries the fitter's tensor overrides, quoted", placed_flags);
        check(placed_flags.find("model.gguf") == std::string::npos,
              "placed: the reproduce line leaves the model to the caller", placed_flags);

        // The fitter's per-device split is part of the placement: carried as it was projected, and
        // in the line that reproduces it. Without it llama.cpp divides by free memory (#321).
        Placement split_pl = pl;
        split_pl.device_split = {{"discrete", 24}};
        const Plan sp = plan_run(base_cfg(), desktop(), model, split_pl, PlanRequest{});
        check(sp.config.device_split.size() == 1 && sp.config.device_split[0].first == "discrete" &&
                  sp.config.device_split[0].second == 24,
              "placed: the fitter's per-device split is carried into the config");
        check(sp.to_flags().find("--device-split discrete:24") != std::string::npos,
              "placed: the reproduce line carries the per-device split", sp.to_flags());
        check(validate(sp.config).ok, "placed: a split that sums to n_gpu_layers is a valid config",
              validate(sp.config).error);

        // A device that would win the prefill is not armed over placed layers: the prefill device
        // loads as the model's only device, and every placed layer would land on it (#321).
        HardwareProfile fast = desktop();
        fast.host_wide_matmul_gibs = 200.0;
        fast.wide_batch = 128;
        fast.devices[0].wide_matmul_gibs = 1500.0;
        fast.devices[0].wide_identity_ok = Tri::Yes;
        const Plan pp = plan_run(base_cfg(), fast, model, split_pl, PlanRequest{});
        const Decision * pd = find(pp, "prefill-device");
        check(!pp.config.prefill.enabled() && pd && pd->reason.find("already placed layers") != std::string::npos,
              "placed: no prefill device over layers the fitter placed", pd ? pd->reason : "no decision");
        check(validate(pp.config).ok, "placed: and the plan stays a valid config", validate(pp.config).error);

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

        // A denied query is not a zero grant. Try within the same process budget, while keeping
        // the full margin and leaving the requested lock out of the ledger's predicted grant.
        HardwareProfile unknown = hard_capped();
        unknown.lockable_bytes = ~0ull;
        unknown.lock_in_place_bytes = ~0ull;
        unknown.lockable_bytes_known = false;
        unknown.lock_in_place_bytes_known = false;
        unknown.reclaim_exempt_max = 8 * GiB;
        const Plan unknown_plan = plan_run(base_cfg(), unknown, model, PlanRequest{});
        const Plan zero_plan = plan_run(base_cfg(), hard_capped(), model, PlanRequest{});
        check(unknown_plan.config.moe.cache_pin && unknown_plan.config.moe.cache_pin_fit &&
                  unknown_plan.allocation.cache_lock_unverified,
              "ledger: an unknown ceiling arms a bounded runtime pin and fit attempt");
        check(unknown_plan.to_flags().find("--cache-pin-fit") != std::string::npos,
              "ledger: the unknown-ceiling reproduce line carries its runtime fitting policy");
        check(unknown_plan.cache_budget_bytes == zero_plan.cache_budget_bytes &&
                  unknown_plan.allocation.ledger.holdable_cap == zero_plan.allocation.ledger.holdable_cap,
              "ledger: an unknown lock grants no process capacity and removes no margin");
        const LedgerRow * unknown_cache = ledger_row(unknown_plan, "expert cache");
        check(unknown_cache && !unknown_cache->locked && unknown_plan.allocation.cache_locked_bytes == 0 &&
                  !unknown_plan.allocation.ledger.lockable_cap_known,
              "ledger: the unknown runtime grant is not reported as a locked cache");
        check(unknown_plan.config.moe.dense_weights == DenseWeightsMode::Anonymous,
              "ledger: an unknown total never establishes an automatic pinned dense store");
        AllocationInputs unknown_dense;
        unknown_dense.can_pin = true;
        unknown_dense.budget_bytes = 4 * GiB;
        unknown_dense.reserved_bytes = 3 * GiB;
        unknown_dense.lockable_bytes = ~0ull;
        unknown_dense.lockable_bytes_known = false;
        check(!allocate(unknown, model, unknown_dense, PlannerPolicy{}).dense_pinned,
              "ledger: the allocator itself cannot infer a dense grant from an unknown total");
        check(unknown_plan.explain().find("lockable ceiling unmeasured") != std::string::npos &&
                  unknown_plan.explain().find("runtime grant unverified") != std::string::npos,
              "ledger: text distinguishes an unmeasured ceiling from zero");
        check(!zero_plan.config.moe.cache_pin && !zero_plan.config.moe.cache_pin_fit &&
                  !zero_plan.allocation.cache_lock_unverified && zero_plan.allocation.ledger.lockable_cap_known,
              "ledger: a measured zero remains an authoritative refusal");
        HardwareProfile zero_bound = unknown;
        zero_bound.lock_in_place_bytes = 0;
        check(!plan_run(base_cfg(), zero_bound, model, PlanRequest{}).config.moe.cache_pin,
              "ledger: an independent zero bound remains authoritative even if another query was denied");

        RunConfig no_pin = base_cfg();
        no_pin.moe.cache_pin = false;
        PlanRequest no_pin_request;
        no_pin_request.pinned = {"cache-pin"};
        const Plan no_pin_plan = plan_run(no_pin, unknown, model, no_pin_request);
        check(!no_pin_plan.config.moe.cache_pin && !no_pin_plan.config.moe.cache_pin_fit &&
                  !no_pin_plan.allocation.cache_lock_unverified,
              "ledger: an explicit no-pin remains authoritative under an unknown ceiling");
        RunConfig no_fit = base_cfg();
        no_fit.moe.cache_pin_fit = false;
        PlanRequest no_fit_request;
        no_fit_request.pinned = {"cache-pin-fit"};
        check(!plan_run(no_fit, unknown, model, no_fit_request).config.moe.cache_pin_fit,
              "ledger: an explicit no-fit is not replaced by the unknown-ceiling policy");
        RunConfig own_unknown = base_cfg();
        own_unknown.moe.cache_mb = 1500;
        const Plan caller_unknown = plan_run(own_unknown, unknown, model, orq);
        check(caller_unknown.config.moe.cache_mb == 1500 && !caller_unknown.config.moe.cache_pin_fit,
              "ledger: a caller's cache budget is not automatically fitted under an unknown ceiling");

        // An unread part of the ceiling cannot erase a finite bound obtained independently.
        HardwareProfile partial = unknown;
        partial.residency_budget = 8 * GiB;
        partial.lockable_bytes = 1 * GiB;
        partial.lock_in_place_bytes = 1 * GiB;
        const Plan partial_plan = plan_run(base_cfg(), partial, model, PlanRequest{});
        check(partial_plan.cache_budget_bytes >= model.token_cycle_bytes &&
                  partial_plan.cache_budget_bytes < partial.lock_in_place_bytes &&
                  partial_plan.config.moe.cache_pin_fit && partial_plan.allocation.cache_lock_unverified,
              "ledger: a partial finite ceiling still bounds an unverified runtime grant");
        partial.lockable_bytes_known = true;
        partial.lock_in_place_bytes_known = true;
        const Plan finite_plan = plan_run(base_cfg(), partial, model, PlanRequest{});
        check(finite_plan.config.moe.cache_pin_fit && !finite_plan.allocation.cache_lock_unverified &&
                  finite_plan.allocation.cache_locked_bytes == finite_plan.cache_budget_bytes &&
                  finite_plan.allocation.ledger.locked <= finite_plan.allocation.ledger.lockable_cap,
              "ledger: a measured finite ceiling retains its existing bounded-lock policy");

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

        // A chat run keeps context checkpoints, each a copy of the context's fixed part: a line of
        // their own at the count kept, the cache sized after it, and nothing on a run that is not a chat.
        Placement ck = pl;
        ck.context_fixed_bytes = 50 * MiB;
        RunConfig chat = base_cfg();
        chat.chatml = true;
        chat.ctx_checkpoints = 4;
        const Plan cx = plan_run(chat, phone(), model, ck, PlanRequest{});
        const LedgerRow * ck_row = ledger_row(cx, "context checkpoints");
        check(ck_row && ck_row->bytes == 200 * MiB, "ledger: a chat run charges its checkpoints",
              ck_row ? mibs_of(ck_row->bytes) : "no row");
        check(fx.cache_budget_bytes - cx.cache_budget_bytes == 200 * MiB, "ledger: and the cache gives them up",
              mibs_of(fx.cache_budget_bytes) + " against " + mibs_of(cx.cache_budget_bytes));
        check(ledger_row(plan_run(base_cfg(), phone(), model, ck, PlanRequest{}), "context checkpoints") == nullptr,
              "ledger: a run that is not a chat keeps no checkpoints");
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
        const LedgerRow * turn = ledger_row(on, "device slots");
        const uint64_t stays = 2 * 64 * MiB;    // the layer's other weights, held for the session
        const uint64_t in_turn = 2 * 384 * MiB; // the expert slots, held only during a prefill
        check(on.config.prefill.device == "accel",
              "prefill: a device that wins wide and agrees is armed by its own name");
        check(on.config.prefill.best_effort, "prefill: a device the plan armed is never a condition for the load");
        // The expert slots take turns with the cache: it is idle exactly when they are busy, so
        // they are not charged to decode. What the cache gives up is only what stays.
        check(on.config.prefill.slots_on_demand, "prefill: a plan arms slots that take turns with the cache");
        check(row && row->locked && row->bytes == stays && !row->in_turn,
              "prefill: what stays for the session is a row",
              row ? std::to_string((unsigned long long) (row->bytes >> 20)) + " MiB" : "no row");
        check(turn && turn->in_turn && turn->bytes == in_turn,
              "prefill: the expert slots are a row in turn with the cache",
              turn ? std::to_string((unsigned long long) (turn->bytes >> 20)) + " MiB" : "no row");
        check(off.cache_budget_bytes - on.cache_budget_bytes == stays,
              "prefill: and the cache gives up only what stays");
        uint64_t counted = 0;
        for (const LedgerRow & r : on.allocation.ledger.rows)
            if (!r.in_turn) counted += r.bytes;
        check(counted == on.allocation.ledger.held, "prefill: a row in turn is not in what is held while decoding");
        // A caller who wants them held for the session gets the old account: the cache makes room for all of it.
        PlanRequest held_req;
        held_req.pinned = {"prefill-slots-on-demand"};
        const Plan held = plan_run(base_cfg(), machine(1500.0, Tri::Yes), mm, held_req);
        check(!held.config.prefill.slots_on_demand && ledger_row(held, "device slots") == nullptr &&
                  off.cache_budget_bytes - held.cache_budget_bytes == two_layers,
              "prefill: slots held for the session are charged whole, beside the cache");
        check(on.allocation.ledger.locked <= on.allocation.ledger.lockable_cap, "prefill: the ledger still closes");
        check(on.config.device_use != DeviceUse::CpuOnly, "prefill: an armed device is not taken out of the run");
        check(validate(on.config).ok, "prefill: the armed plan is a valid config", validate(on.config).error);
        check(on.to_flags().find("--prefill-device accel") != std::string::npos &&
                  on.to_flags().find("--prefill-best-effort") != std::string::npos &&
                  on.to_flags().find("--prefill-slots-on-demand") != std::string::npos,
              "prefill: the reproduce line carries it");

        const Plan wrong = plan_run(base_cfg(), machine(1500.0, Tri::No), mm, PlanRequest{});
        check(!wrong.config.prefill.enabled() && reason(wrong).find("did not reproduce") != std::string::npos,
              "prefill: a faster device with a different answer is refused on correctness");

        const Plan slow = plan_run(base_cfg(), machine(205.0, Tri::Yes), mm, PlanRequest{});
        check(!slow.config.prefill.enabled() && reason(slow).find("not a win") != std::string::npos,
              "prefill: a device inside the margin is not a second placement");

        // A device that could not be measured at that width is named with the reason, not dropped:
        // the plan reads the same whether the second device is absent or cannot run this model.
        HardwareProfile pair = machine(205.0, Tri::Yes);
        ComputeDevice other = pair.devices[1];
        other.name = "other";
        other.wide_matmul_gibs = 0.0;
        other.runs_expert_op = Tri::No;
        pair.devices.push_back(other);
        const Plan named = plan_run(base_cfg(), pair, mm, PlanRequest{});
        check(!named.config.prefill.enabled() && reason(named).find("not a win") != std::string::npos &&
                  reason(named).find("other was not weighed") != std::string::npos &&
                  reason(named).find("expert matmul") != std::string::npos,
              "prefill: a device that cannot run the model's matmul is named with the reason", reason(named));
        pair.devices.back().runs_expert_op = Tri::Unknown;
        const Plan unmeasured = plan_run(base_cfg(), pair, mm, PlanRequest{});
        check(reason(unmeasured).find("other was not weighed: it gave no measurement") != std::string::npos,
              "prefill: and one that gave no figure says so", reason(unmeasured));

        // Two slots do not fit and one does: the device is armed with one, and the plan says so.
        // The layer is sized so that the difference between the two is exactly what decides it.
        ModelProfile mid = mm;
        mid.largest_expert_layer_bytes = 4 * GiB; // the cache can yield one of these and not two
        const Plan one = plan_run(base_cfg(), machine(1500.0, Tri::Yes), mid, PlanRequest{});
        const LedgerRow * one_row = ledger_row(one, "device slots");
        check(one.config.prefill.device == "accel" && one.config.prefill.slots == 1,
              "prefill: where two expert slots do not fit and one does, one is used",
              std::to_string(one.config.prefill.slots) + " slot(s)");
        check(one_row && one_row->bytes == 4 * GiB, "prefill: and one layer of experts is what takes its turn");
        check(reason(one).find("one expert slot") != std::string::npos,
              "prefill: the plan says it chose the narrower scheme");
        check(one.to_flags().find("--prefill-slots 1") != std::string::npos,
              "prefill: the reproduce line carries the slot count");
        check(on.config.prefill.slots == 2 && on.to_flags().find("--prefill-slots 1") == std::string::npos,
              "prefill: two slots stay the default where they fit");
        check(validate(one.config).ok, "prefill: the one-slot plan is a valid config", validate(one.config).error);

        // A caller who fixed the count is not given the other one.
        RunConfig two = base_cfg();
        two.prefill.slots = 2;
        PlanRequest tr;
        tr.pinned = {"prefill-slots"};
        check(!plan_run(two, machine(1500.0, Tri::Yes), mid, tr).config.prefill.enabled(),
              "prefill: a caller's slot count is not traded down");

        // Too large for what is left: refused, with the figures, and the plan is the one without it.
        ModelProfile big = mm;
        big.largest_expert_layer_bytes = 7 * GiB;
        const Plan nofit = plan_run(base_cfg(), machine(1500.0, Tri::Yes), big, PlanRequest{});
        const Plan nodev = plan_run(base_cfg(), unified(), big, PlanRequest{});
        check(!nofit.config.prefill.enabled() && reason(nofit).find("refused on memory") != std::string::npos,
              "prefill: a device that does not fit is refused with the reason");
        check(nofit.cache_budget_bytes == nodev.cache_budget_bytes && ledger_row(nofit, "device") == nullptr,
              "prefill: and a refusal leaves the ledger as it was");

        // A machine where nothing can be locked: a phone whose platform publishes no wire limit. The
        // device is still a row of what is held, and it is not refused for a lock store the run
        // could never use.
        HardwareProfile nolock = machine(1500.0, Tri::Yes);
        nolock.lockable_bytes = 0;
        nolock.lock_in_place_bytes = 0;
        nolock.anon_overflow = Overflow::Kill;
        const Plan unlocked = plan_run(base_cfg(), nolock, mm, PlanRequest{});
        const LedgerRow * ul_row = ledger_row(unlocked, "device");
        check(unlocked.config.prefill.device == "accel",
              "prefill: where nothing can be locked, the device is not refused for the lock store", reason(unlocked));
        check(ul_row && !ul_row->locked && unlocked.allocation.ledger.locked == 0,
              "prefill: and it is charged to what is held, not to a store of 0");
        check(unlocked.allocation.ledger.held <= unlocked.allocation.ledger.holdable_cap,
              "prefill: the ledger still closes on what the process may hold");

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

        // A device with memory of its own costs this pool nothing, and is refused only by its own.
        HardwareProfile disc = machine(1500.0, Tri::Yes);
        disc.devices[1].shares_host_memory = Tri::No;
        disc.devices[1].memory_total = 8 * GiB;
        disc.devices[1].memory_free = 8 * GiB;
        const Plan own = plan_run(base_cfg(), disc, mm, PlanRequest{});
        check(own.config.prefill.device == "accel" && ledger_row(own, "device") == nullptr &&
                  own.cache_budget_bytes == off.cache_budget_bytes,
              "prefill: a device with its own memory is armed and charged nothing here");
        // "Its own memory" with no size is not a pool to budget against: charged to this one.
        HardwareProfile sizeless = machine(1500.0, Tri::Yes);
        sizeless.devices[1].shares_host_memory = Tri::No;
        const Plan sz = plan_run(base_cfg(), sizeless, mm, PlanRequest{});
        const LedgerRow * szr = ledger_row(sz, "device");
        check(sz.config.prefill.device == "accel" && szr && szr->bytes == stays && ledger_row(sz, "device slots"),
              "prefill: a device that claims its own memory and states no size is charged to the host");

        // The fitter's compute buffers are per device: the prefill device pays its own part, and
        // none of one projected for a device beside it (a GPU's charged to an NPU, on a phone).
        Placement projected;
        projected.device_compute_bytes = 256 * MiB;
        projected.projected_devices = {"gpu"};
        projected.projected_compute_bytes = {256 * MiB};
        const Plan beside = plan_run(base_cfg(), machine(1500.0, Tri::Yes), mm, projected, PlanRequest{});
        const LedgerRow * orow = ledger_row(beside, "device");
        check(beside.config.prefill.device == "accel" && orow && orow->bytes == stays,
              "prefill: a compute buffer projected for another device is not charged to this one",
              orow ? std::to_string((unsigned long long) (orow->bytes >> 20)) + " MiB" : "no row");
        projected.projected_devices = {"accel"};
        const Plan mine = plan_run(base_cfg(), machine(1500.0, Tri::Yes), mm, projected, PlanRequest{});
        const LedgerRow * mrow = ledger_row(mine, "device");
        check(mrow && mrow->bytes == stays + 256 * MiB, "prefill: and the one projected for it is",
              mrow ? std::to_string((unsigned long long) (mrow->bytes >> 20)) + " MiB" : "no row");

        disc.devices[1].memory_free = 256 * MiB;
        check(!plan_run(base_cfg(), disc, mm, PlanRequest{}).config.prefill.enabled(),
              "prefill: and refused when its own memory cannot take even one layer");
    }

    // ── a device probed where it cannot take the plan with it ──────────────────────
    // A device is a claim by a driver, and a driver can end the process that uses it. So a device
    // is asked in another process, and what comes back is its facts or the reason it has none.
    {
        // The facts survive the trip, whole.
        ComputeDevice src;
        src.name = "accel";
        src.shares_host_memory = Tri::Yes;
        src.host_buffer = Tri::Yes;
        src.host_ptr_buffers = Tri::Unknown;
        src.async_copies = Tri::No;
        src.rebindable = true;
        src.runs_expert_op = Tri::Yes;
        src.needs_repack = Tri::No;
        src.memory_bandwidth_gibs = 63.5;
        src.identity_ok = Tri::Yes;
        src.host_ptr_verified = Tri::No;
        src.split_seconds = 0.000125;
        src.graph_splits = 96;
        src.wide_matmul_gibs = 1502.25;
        src.wide_identity_ok = Tri::Yes;
        src.buffers_locked = Tri::Yes;
        src.upload_gibs = 24.25;
        src.upload_host_buffer_gibs = 25.5;
        src.expert_gibs = 12.5;
        src.dense_gibs = 33.25;
        ComputeDevice dst;
        dst.name = "accel";
        const std::string line = device_facts_to_text(src);
        check(device_facts_from_text(line, dst), "isolated: a device's facts parse back", line);
        check(dst.shares_host_memory == Tri::Yes && dst.host_ptr_buffers == Tri::Unknown &&
                  dst.async_copies == Tri::No && dst.rebindable && dst.runs_expert_op == Tri::Yes &&
                  dst.needs_repack == Tri::No && dst.memory_bandwidth_gibs == 63.5 && dst.identity_ok == Tri::Yes &&
                  dst.host_ptr_verified == Tri::No && dst.split_seconds == 0.000125 && dst.graph_splits == 96 &&
                  dst.wide_matmul_gibs == 1502.25 && dst.wide_identity_ok == Tri::Yes &&
                  dst.buffers_locked == Tri::Yes && dst.upload_gibs == 24.25 && dst.upload_host_buffer_gibs == 25.5 &&
                  dst.expert_gibs == 12.5 && dst.dense_gibs == 33.25,
              "isolated: and every one of them is what was sent");
        // A line cut short - a child that died while printing - is applied not at all.
        ComputeDevice untouched;
        untouched.memory_bandwidth_gibs = 7.0;
        check(!device_facts_from_text(line.substr(0, line.size() / 2), untouched) &&
                  untouched.memory_bandwidth_gibs == 7.0,
              "isolated: half a line changes nothing");
        std::string old_line = line;
        old_line.replace(old_line.find("v=3"), 3, "v=2");
        check(!device_facts_from_text(old_line, untouched), "isolated: a line of another version is not applied");
        check(!device_facts_from_text("", untouched) && !device_facts_from_text("garbage in the stream", untouched),
              "isolated: nor does noise");

        // The parent's side: one device answers, one ends its probe, the host is never asked.
        HardwareProfile h = unified();
        ComputeDevice bad;
        bad.name = "broken";
        bad.shares_host_memory = Tri::Yes;
        h.devices.push_back(bad);
        std::vector<std::string> asked;
        probe_devices_isolated(h, [&](const std::string & dev, std::string & facts, std::string & why) {
            asked.push_back(dev);
            if (dev == "broken") {
                why = "its probe ended the process that ran it (signal 11)";
                return false;
            }
            ComputeDevice m = src;
            facts = device_facts_to_text(m);
            return true;
        });
        check(asked.size() == 2 && asked[0] == "accel" && asked[1] == "broken",
              "isolated: every device but the host is asked");
        check(h.devices[1].usable == Tri::Yes && h.devices[1].wide_matmul_gibs == 1502.25 && !h.devices[1].probe_here(),
              "isolated: a device that answered carries its facts and is left alone by the probes here");
        check(h.devices[2].usable == Tri::No && !h.devices[2].probe_here() &&
                  h.devices[2].unusable_reason.find("signal 11") != std::string::npos,
              "isolated: a device that ended its probe is marked unusable, with the reason");

        // And the plan: the broken device is kept out of the run, and the good one is still weighed.
        h.host_wide_matmul_gibs = 200.0;
        h.host_bandwidth_gibs = 100.0;
        h.wide_batch = 128;
        ModelProfile mm = model;
        mm.largest_expert_layer_bytes = 384 * MiB;
        const Plan with_good = plan_run(base_cfg(), h, mm, PlanRequest{});
        check(with_good.config.prefill.device == "accel",
              "isolated: the device that works is still armed for the prefill");
        const Decision * dv = find(with_good, "devices");
        check(dv && dv->source == Source::Measured && dv->reason.find("broken") != std::string::npos,
              "isolated: and the plan names the one that is kept out");
        HardwareProfile only_bad = unified();
        only_bad.devices[1].usable = Tri::No;
        only_bad.devices[1].unusable_reason = "its probe did not finish in 120 s and was stopped";
        only_bad.devices[1].probed_out_of_process = true;
        const Plan cpu_only = plan_run(base_cfg(), only_bad, mm, PlanRequest{});
        check(cpu_only.config.device_use == DeviceUse::CpuOnly && !cpu_only.config.prefill.enabled() &&
                  cpu_only.config.moe.enabled,
              "isolated: with nothing else to use the run is the host's, and it still streams");
        check(validate(cpu_only.config).ok, "isolated: and that plan is a valid config",
              validate(cpu_only.config).error);
        check(cpu_only.to_flags().find("--devices cpu") != std::string::npos,
              "isolated: the reproduce line carries the device choice", cpu_only.to_flags());
        // No runner: nothing is marked, and the probes here do the asking as they always did.
        HardwareProfile plain = unified();
        probe_devices_isolated(plain, nullptr);
        check(plain.devices[1].usable == Tri::Unknown && plain.devices[1].probe_here(),
              "isolated: without a runner nothing changes");
    }

    // ── every decision says itself in one line, on every machine ───────────────────────
    // A screen leads with the brief, so a rule that adds a decision without one, or with a paragraph,
    // is a screen nobody can read. And no line, brief or reason, may cite one machine's story on every
    // other: what is true of the machine being planned is what its profile says.
    {
        HardwareProfile tight = phone();
        tight.residency_budget = 3 * GiB;
        tight.storage.refault_mibs = 24.0;
        HardwareProfile shared = unified();
        shared.host_bandwidth_gibs = 60.0;
        shared.storage.refault_mibs = 40.0;
        shared.storage.rate_spread = 2.0;
        PlanRequest pinned_cache;
        pinned_cache.pinned = {"cache-mb", "ubatch"};
        const std::pair<const char *, Plan> plans[] = {
            {"desktop", plan_run(base_cfg(), desktop(), model, PlanRequest{})},
            {"phone", plan_run(base_cfg(), phone(), model, PlanRequest{})},
            {"hard-capped", plan_run(base_cfg(), hard_capped(), model, PlanRequest{})},
            {"unified", plan_run(base_cfg(), shared, model, PlanRequest{})},
            {"tight", plan_run(base_cfg(), tight, model, PlanRequest{})},
            {"pinned", plan_run(base_cfg(), desktop(), model, pinned_cache)},
        };
        const char * anecdotes[] = {"phone", "Mac", "Windows", "Android", "11 GB machine"};
        const char * jargon[] = {"token cycle", "ledger", "capacity tier", "fitter", "holdable"};
        for (const auto & pl : plans) {
            std::string bad;
            for (const Decision & d : pl.second.decisions) {
                if (d.brief.empty() || d.brief.size() > 120) bad += " [" + d.knob + ": length]";
                for (const char * w : anecdotes)
                    if (d.brief.find(w) != std::string::npos || d.reason.find(w) != std::string::npos)
                        bad += " [" + d.knob + ": " + w + "]";
                for (const char * w : jargon)
                    if (d.brief.find(w) != std::string::npos) bad += " [" + d.knob + " brief: " + w + "]";
                if (d.knob == "cost") bad += " [an unnamed cost note]";
            }
            check(!pl.second.decisions.empty() && bad.empty(),
                  std::string(pl.first) + ": every decision has a plain one-line brief", bad);
        }
    }

    // ── the cache priced on the model's own hit curve (docs/measured-planning.md, Step 1) ──────
    {
        auto measured_machine = [&]() {
            HardwareProfile h = desktop();
            h.host_bandwidth_gibs = 100.0;
            h.storage.refault_mibs = 24.0;
            h.devices.clear();
            return h;
        };
        // A curve shaped like the archived phone traces: little under one token cycle, most of the
        // reads served by a small share of the set, the rest bought slowly.
        auto curve_model = [&](std::vector<std::pair<double, double>> shape) {
            ModelProfile m = model;
            m.hit_curve.measured = true;
            m.hit_curve.decode_tokens = 256;
            m.hit_curve.prefill_tokens = 28;
            m.hit_curve.points.push_back({0, 0.0});
            for (const auto & sh : shape)
                m.hit_curve.points.push_back({(uint64_t) ((double) m.expert_bytes * sh.first), sh.second});
            return m;
        };
        const ModelProfile steep = curve_model({{0.02, 0.0}, {0.05, 0.40}, {0.13, 0.75}, {0.25, 0.85}, {1.0, 1.0}});

        // No trace: the plan is today's. Pinned to the figures this fixture produced before the curve
        // existed (and its whole rationale was diffed byte for byte against that build): the linear
        // credit, the cache it sized and the cost it predicted.
        {
            const Plan p = plan_run(base_cfg(), measured_machine(), model, PlanRequest{});
            const GroupPlacement & e = p.allocation.at(WeightGroup::Experts);
            check(p.allocation.cache_bytes == 9147777024ull, "no trace: the cache is the linear credit's",
                  std::to_string(p.allocation.cache_bytes));
            check(std::fabs(p.allocation.cache_hit_credited - 0.473307) < 1e-6, "no trace: credited linearly",
                  std::to_string(p.allocation.cache_hit_credited));
            check(std::fabs(p.allocation.seconds_per_token - 0.160544713) < 1e-9, "no trace: the same prediction",
                  std::to_string(p.allocation.seconds_per_token));
            check(e.reason == "streamed with a 8724 MiB cache, 47% of the expert set; credited hit rate 47%, which is "
                              "conservative on purpose and is the coefficient a run's own counters correct",
                  "no trace: the experts' rationale is word for word today's", e.reason);
            check(!find(p, "cache-hits"), "no trace: nothing is said about a curve there is none of");

            // A trace that is not this model's is the same plan, plus the one line saying why.
            ModelProfile rejected = model;
            rejected.hit_curve.rejected = "the trace was recorded at top-6 and this run routes top-8";
            const Plan r = plan_run(base_cfg(), measured_machine(), rejected, PlanRequest{});
            std::string without;
            std::string reasons;
            for (const Decision & d : r.decisions)
                if (d.knob != "cache-hits") without += d.knob + "=" + d.value + ":" + d.reason + "|" + d.brief + "\n";
            for (const Decision & d : p.decisions)
                reasons += d.knob + "=" + d.value + ":" + d.reason + "|" + d.brief + "\n";
            check(without == reasons && r.allocation.cache_bytes == p.allocation.cache_bytes &&
                      r.allocation.seconds_per_token == p.allocation.seconds_per_token,
                  "a rejected trace leaves the plan as it is without one");
            const Decision * why = find(r, "cache-hits");
            check(why && why->source == Source::Policy && why->reason.find("top-6") != std::string::npos,
                  "a rejected trace says why it was not used");
        }

        // With the curve: the credit is the curve at the size chosen, and the plan says so.
        {
            const Plan p = plan_run(base_cfg(), measured_machine(), steep, PlanRequest{});
            const GroupPlacement & e = p.allocation.at(WeightGroup::Experts);
            const double expect = steep.hit_curve.hit_share(p.allocation.cache_bytes);
            check(p.allocation.cache_bytes > 0 && std::fabs(p.allocation.cache_hit_credited - expect) < 1e-12,
                  "with a curve: the credited hit rate is the curve at the cache's size",
                  std::to_string(p.allocation.cache_hit_credited) + " vs " + std::to_string(expect));
            check(e.reason.find("[measured on this model]") != std::string::npos,
                  "with a curve: the experts' rationale names its source", e.reason);
            const Decision * d = find(p, "cache-hits");
            check(d && d->source == Source::Measured && d->reason.find("256 decoded") != std::string::npos,
                  "with a curve: a measured decision states the hit rate and what it was replayed from");
            const Plan lin = plan_run(base_cfg(), measured_machine(), model, PlanRequest{});
            check(p.allocation.seconds_per_token < lin.allocation.seconds_per_token,
                  "with a steep curve the same cache is priced cheaper than the linear credit says");
        }

        // The ranking walks the curve: where the cache competes with a read-whole group for the same
        // bytes, a curve that serves most reads from a small cache keeps the cache at its knee and
        // gives the rest to the group, and one that rises late takes more for the cache.
        {
            const HardwareProfile h = measured_machine();
            const PlannerPolicy pol = PlannerPolicy::defaults();
            AllocationInputs in;
            in.budget_bytes = 6 * GiB;
            in.cacheable_bytes = model.expert_bytes;
            const Allocation lin = allocate(h, model, in, pol);
            const Allocation cur = allocate(h, steep, in, pol);
            check(cur.cache_bytes >= model.token_cycle_bytes && cur.cache_bytes <= in.budget_bytes,
                  "with a curve: the cache is sized above its floor, inside the budget",
                  std::to_string(cur.cache_bytes >> 20) + " MiB");
            check(std::fabs(cur.cache_hit_credited - steep.hit_curve.hit_share(cur.cache_bytes)) < 1e-12,
                  "allocate credits the curve at the size it chose");
            check(cur.seconds_per_token <= lin.seconds_per_token + 1e-12,
                  "a curve above the linear credit never prices the token higher");

            // A curve flat above its knee: past 5% of the set the record shows nothing more served, so
            // the ranking prices the cache there; the RAM nothing else wanted is the cache's all the
            // same, since a record is a few hundred tokens and a longer answer routes past what it saw.
            // Measured: 15.7 GB at the curve's knee held 83% over 1024 tokens, 22 GB held 96%.
            const ModelProfile knee = curve_model({{0.05, 0.6}, {1.0, 0.6}});
            in.budget_bytes = 12 * GiB;
            const Allocation k = allocate(h, knee, in, pol);
            const uint64_t at_knee = (uint64_t) ((double) model.expert_bytes * 0.05);
            bool remainder = false;
            for (const AllocationNote & n : k.notes)
                remainder = remainder || n.topic == "cache-remainder";
            check(k.cache_bytes > at_knee + GiB && k.cache_bytes <= in.budget_bytes && remainder,
                  "a curve that stops rising prices the cache at its knee, and the idle rest is the cache's too",
                  std::to_string(k.cache_bytes >> 20) + " MiB against a knee at " + std::to_string(at_knee >> 20));

            // The floor still holds: a budget under one token cycle is no cache, curve or not.
            AllocationInputs tight = in;
            tight.budget_bytes = 3 * GiB;
            tight.reserved_bytes = 3 * GiB - (100 * MiB);
            check(allocate(h, steep, tight, pol).cache_bytes == 0, "with a curve: under the floor there is no cache");

            // The host's share: where the first stage placed half the layers' experts on a device, a
            // host cache of C is read as a whole-set cache of 2C.
            AllocationInputs half = in;
            half.budget_bytes = 6 * GiB;
            half.cacheable_bytes = model.expert_bytes / 2;
            const Allocation hh = allocate(h, steep, half, pol);
            check(hh.cache_bytes > 0 &&
                      std::fabs(hh.cache_hit_credited - steep.hit_curve.hit_share(2 * hh.cache_bytes)) < 1e-6,
                  "a cache for the host's half is credited at the whole-set size it stands for");
        }
    }

    // ── lossless knobs measured on the first answer (docs/measured-planning.md, Step 2) ──────────
    {
        // Ten cores in two classes, a sweep that picked 8, nothing placed on a device.
        HardwareProfile het = desktop();
        het.devices.clear();
        het.n_cores = 10;
        het.core_classes = {4, 6};
        het.best_threads = 8;
        PlanRequest on;
        on.measure_knobs = true;

        // Off, nothing stored: the plan arms no measurement and says what it always said.
        {
            const Plan p = plan_run(base_cfg(), het, model, PlanRequest{});
            check(!p.config.trial.enabled(), "knobs off: no trial is armed");
            check(!find(p, "trial-threads"), "knobs off: no trial decision");
            check(p.to_flags().find("trial") == std::string::npos, "knobs off: the command line names no trial",
                  p.to_flags());
            const Decision * ov = find(p, "overlap");
            check(ov && ov->source == Source::Unprobed && ov->value == "off" &&
                      ov->brief == "Off: it changes no output, but its gain here is unmeasured." &&
                      ov->reason.find("pass --overlap") != std::string::npos,
                  "knobs off: overlap is named and not armed, word for word as before", ov ? ov->reason : "none");
            const Decision * th = find(p, "threads");
            check(p.config.n_threads == 8 && p.config.n_threads_batch == 0 && th && th->source == Source::Measured &&
                      th->reason.find("swept over") != std::string::npos,
                  "knobs off: the sweep's count, as before");
        }

        // On, nothing stored: the first answer measures the counts nearest the incumbent and overlap.
        const Plan armed = plan_run(base_cfg(), het, model, on);
        {
            check(armed.config.trial.threads == std::vector<int>({10, 4}),
                  "knobs on: the class boundaries nearest the incumbent are tried, nearest first",
                  armed.config.trial.threads.size() == 2 ? std::to_string(armed.config.trial.threads[0]) + "," +
                                                               std::to_string(armed.config.trial.threads[1])
                                                         : "size " + std::to_string(armed.config.trial.threads.size()));
            check(armed.config.n_threads == 8 && !armed.config.moe.overlap, "knobs on: the incumbents are unchanged");
            check(armed.config.trial.overlap, "knobs on: overlap is measured too");
            check(validate(armed.config).ok, "knobs on: the armed plan is a valid config",
                  validate(armed.config).error);
            const Decision * t = find(armed, "trial-threads");
            check(t && t->value == "10,4" && t->source == Source::Policy,
                  "knobs on: the trial is a decision of its own");
            const Decision * ov = find(armed, "overlap");
            check(ov && ov->source == Source::Unprobed && ov->brief.find("until the first answer") != std::string::npos,
                  "knobs on: overlap waits for the first answer");
            check(armed.to_flags().find("--trial-threads 10,4") != std::string::npos &&
                      armed.to_flags().find("--trial-overlap") != std::string::npos,
                  "knobs on: the trial is part of the command line that reproduces the plan", armed.to_flags());
        }

        // A stored verdict: applied, stated as measured on this model, and nothing is measured again.
        ModelProfile measured = model;
        measured.knobs.hardware = "0123456789abcdef";
        measured.knobs.engine = "test";
        measured.knobs.threads = 4;
        measured.knobs.threads_tried = {8, 10, 4};
        measured.knobs.threads_gain = 1.15;
        measured.knobs.overlap = 1;
        measured.knobs.overlap_incumbent = 0;
        measured.knobs.overlap_gain = 1.12;
        measured.knobs.rounds = 3;
        {
            const Plan p = plan_run(base_cfg(), het, measured, on);
            check(p.config.n_threads == 4, "stored: the measured count is the plan's");
            check(p.config.n_threads_batch == 8, "stored: prefill keeps the count it would have had",
                  std::to_string(p.config.n_threads_batch));
            check(p.config.moe.overlap, "stored: overlap as measured");
            check(!p.config.trial.enabled(), "stored: nothing is measured again");
            const Decision * th = find(p, "threads");
            check(th && th->source == Source::Measured && th->reason.rfind("measured on this model", 0) == 0 &&
                      th->reason.find("+15%") != std::string::npos &&
                      th->reason.find("prefill keeps 8") != std::string::npos,
                  "stored: the count says it was measured on this model, and by how much", th ? th->reason : "none");
            const Decision * ov = find(p, "overlap");
            check(ov && ov->source == Source::Measured && ov->value == "on" &&
                      ov->reason.rfind("measured on this model", 0) == 0 &&
                      ov->reason.find("+12%") != std::string::npos,
                  "stored: overlap says it was measured on this model", ov ? ov->reason : "none");
            check(validate(p.config).ok, "stored: a valid config", validate(p.config).error);
            // The same verdict with the feature off is applied all the same: it is a fact, not a trial.
            const Plan q = plan_run(base_cfg(), het, measured, PlanRequest{});
            check(q.config.n_threads == 4 && q.config.moe.overlap && !q.config.trial.enabled(),
                  "stored, knobs off: the verdict still applies");
        }
        {
            // A verdict that held: the incumbent stays, and the plan says the other was inside the margin.
            ModelProfile held = measured;
            held.knobs.threads = 8;
            held.knobs.threads_gain = 1.04;
            held.knobs.overlap = 0;
            held.knobs.overlap_gain = 0.97;
            const Plan p = plan_run(base_cfg(), het, held, on);
            const Decision * th = find(p, "threads");
            const Decision * ov = find(p, "overlap");
            check(p.config.n_threads == 8 && p.config.n_threads_batch == 0 && !p.config.moe.overlap && th &&
                      th->reason.find("inside the 10% margin, so 8 held") != std::string::npos && ov &&
                      ov->source == Source::Measured && ov->reason.find("so off held") != std::string::npos,
                  "stored, held: the incumbents stay, measured",
                  (th ? th->reason : "") + " / " + (ov ? ov->reason : ""));
        }
        {
            // Only the count was measured (overlap could not be switched then): the trial measures overlap alone.
            ModelProfile half = measured;
            half.knobs.overlap = -1;
            half.knobs.overlap_incumbent = -1;
            const Plan p = plan_run(base_cfg(), het, half, on);
            check(p.config.n_threads == 4 && p.config.trial.threads.empty() && p.config.trial.overlap,
                  "stored count only: overlap alone is measured");
        }
        {
            // Pinned knobs are the caller's: neither a verdict nor a trial touches them.
            RunConfig b = base_cfg();
            b.n_threads = 6;
            b.moe.enabled = true;
            PlanRequest pinned = on;
            pinned.pinned = {"threads", "overlap"};
            const Plan p = plan_run(b, het, measured, pinned);
            check(p.config.n_threads == 6 && !p.config.moe.overlap && !p.config.trial.enabled(),
                  "pinned: the caller's count and overlap stand, nothing measured");
        }
        {
            // With layers on a device overlap changes how the context offloads, fixed at creation: the trial
            // cannot switch it, so it is neither measured nor taken from a verdict measured without them.
            Placement pl;
            pl.fitted = true;
            pl.outcome = "synthetic";
            pl.n_gpu_layers = 24;
            pl.n_ctx = 4096;
            for (uint32_t il = 0; il < 24; ++il)
                pl.host_expert_layers.push_back(il);
            pl.host_resident_bytes = 4 * GiB;
            pl.device_bytes = 8 * GiB;
            HardwareProfile dev = het;
            dev.devices = desktop().devices;
            const Plan p = plan_run(base_cfg(), dev, measured, pl, on);
            check(p.config.n_gpu_layers == 24 && !p.config.moe.overlap && !p.config.trial.overlap,
                  "layers on a device: overlap is not armed from a verdict nor measured");
            check(validate(p.config).ok, "layers on a device: a valid config", validate(p.config).error);
        }
        {
            // Every core alike: the whole count and half of it are the candidates.
            HardwareProfile uni = het;
            uni.n_cores = 16;
            uni.core_classes = {16};
            uni.best_threads = 0;
            const Plan p = plan_run(base_cfg(), uni, model, on);
            check(p.config.trial.threads == std::vector<int>({8, 16}),
                  "alike cores: half and all of them are tried against the default");
            // Nothing known about the cores: nothing to try.
            HardwareProfile blind = het;
            blind.n_cores = 0;
            blind.core_classes.clear();
            blind.best_threads = 0;
            check(plan_run(base_cfg(), blind, model, on).config.trial.threads.empty(),
                  "unknown cores: no thread trial");
        }
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
    // ── device memory as a tier of the expert cache ──────────────────────────────────
    // A card with memory of its own, every layer's dense part on it, and the fitter's leftover
    // device memory given to whole layers' experts: half the layers here. The plan prices the same
    // bytes as a cache of the most used experts, and chooses on the measured facts alone.
    {
        HardwareProfile card = desktop();
        card.residency_budget = 80 * GiB;
        card.memory_total = 125 * GiB;
        card.host_bandwidth_gibs = 66.0;
        ComputeDevice & gpu = card.devices[0];
        gpu.memory_total = 24 * GiB;
        gpu.memory_free = 23 * GiB;
        gpu.usable = Tri::Yes;
        gpu.runs_expert_op = Tri::Yes;
        gpu.identity_ok = Tri::Yes;
        gpu.memory_bandwidth_gibs = 881.0;
        gpu.upload_gibs = 24.0;
        gpu.split_seconds = 20e-6;
        Placement half;
        half.fitted = true;
        half.n_gpu_layers = (int32_t) model.n_layer + 1;
        for (uint32_t il = 0; il < 24; ++il)
            half.host_expert_layers.push_back(il);
        for (uint32_t il = 0; il < model.n_layer; ++il)
            half.all_expert_patterns.push_back("blk\\." + std::to_string(il) + "\\.ffn_(up|down|gate)_exps");
        half.override_patterns.assign(half.all_expert_patterns.begin(), half.all_expert_patterns.begin() + 24);
        half.device_bytes = 9 * GiB + 3 * GiB;
        const auto plan_on = [&](const HardwareProfile & h, const Placement & pl, const PlanRequest & r) {
            return plan_run(base_cfg(), h, model, pl, r, PlannerPolicy::defaults());
        };

        // No record of the routing yet: the cache still goes on the faster processor, and fills from use
        // as the RAM cache does. A record is what says how much of a token it serves, not whether.
        const Plan linear = plan_on(card, half, PlanRequest{});
        const Decision * d = find(linear, "device-cache-mb");
        check(d && d->source == Source::Measured && linear.config.moe.device_cache_mb == (int) (9 * 1024) &&
                  d->reason.find("the fraction of the expert set it holds") != std::string::npos,
              "device tier, no record: the cache is on the faster processor all the same", d ? d->reason : "none");
        check(linear.config.buft_overrides.size() == model.n_layer && !linear.config.moe.record_unstreamed,
              "device tier, no record: every layer's experts on the host, nothing recorded for a price");

        // The MTP head of a file of its own follows the experts to the device, out of the cache's
        // bytes and never the fitter's layers; a head that does not fit drafts on the host.
        {
            ModelProfile headed = model;
            headed.mtp_head_bytes = 2 * GiB;
            RunConfig cfg = base_cfg();
            cfg.spec.source = DraftSource::mtp;
            cfg.spec.mtp_head = "head.gguf";
            const Plan h = plan_run(cfg, card, headed, half, PlanRequest{}, PlannerPolicy::defaults());
            const Decision * hd = find(h, "mtp-head-on-devices");
            check(hd && hd->value == "true" && hd->source == Source::Measured && h.config.spec.mtp_head_on_devices &&
                      h.config.moe.device_cache_mb == (int) (7 * 1024),
                  "device tier, head file: the head goes on the device and the cache shrinks by its size",
                  std::to_string(h.config.moe.device_cache_mb) + " MiB");
            headed.mtp_head_bytes = 10 * GiB;
            const Plan hh = plan_run(cfg, card, headed, half, PlanRequest{}, PlannerPolicy::defaults());
            hd = find(hh, "mtp-head-on-devices");
            check(hd && hd->value == "false" && !hh.config.spec.mtp_head_on_devices &&
                      hh.config.moe.device_cache_mb == (int) (9 * 1024),
                  "device tier, head too big for the cache: it drafts on the host and the cache is whole");
            PlanRequest pinned;
            pinned.pinned.push_back("mtp-head-on-devices");
            const Plan hp = plan_run(cfg, card, headed, half, pinned, PlannerPolicy::defaults());
            check(!find(hp, "mtp-head-on-devices") && hp.config.moe.device_cache_mb == (int) (9 * 1024),
                  "device tier, head placement pinned: the caller's, and the cache is whole");
        }

        // A device no faster at the experts than the host: the fastest processor is the host, and the
        // fitter's placement stands.
        HardwareProfile even = card;
        even.devices[0].memory_bandwidth_gibs = 70.0;
        const Plan level = plan_on(even, half, PlanRequest{});
        d = find(level, "device-cache-mb");
        check(d && d->value == "0" && level.config.moe.device_cache_mb == 0 && level.config.buft_overrides.size() == 24,
              "device tier, device no faster: the fitter's placement stands", d ? d->reason : "none");

        // Where the cores are slow against the link, the cache wins even on the linear credit: every
        // layer's experts go to the host, and the bytes they held become the cache.
        HardwareProfile slow_cores = card;
        slow_cores.host_bandwidth_gibs = 4.0;
        const Plan tier = plan_on(slow_cores, half, PlanRequest{});
        d = find(tier, "device-cache-mb");
        check(d && d->source == Source::Measured && tier.config.moe.device_cache_mb == (int) (9 * 1024),
              "device tier, cache faster: the expert bytes the fitter placed become the cache",
              std::to_string(tier.config.moe.device_cache_mb));
        check(tier.config.buft_overrides.size() == model.n_layer && tier.config.n_gpu_layers == half.n_gpu_layers,
              "device tier, cache faster: every layer stays on the card and every layer's experts on the host");
        check(!tier.config.prefill.enabled(), "device tier, cache faster: no prefill device beside it");
        check(validate(tier.config).ok, "device tier, cache faster: a valid config", validate(tier.config).error);

        // The device's rate unmeasured: nothing says it is the faster processor, and nothing moves.
        HardwareProfile no_rate = slow_cores;
        no_rate.devices[0].memory_bandwidth_gibs = 0.0;
        no_rate.devices[0].expert_gibs = 0.0;
        const Plan unpriced = plan_on(no_rate, half, PlanRequest{});
        d = find(unpriced, "device-cache-mb");
        check(d && d->value == "0" && unpriced.config.moe.device_cache_mb == 0 &&
                  unpriced.config.buft_overrides.size() == 24,
              "device tier, device rate unmeasured: the fitter's placement stands");

        // A shared pool has no second tier: the stage says nothing and changes nothing.
        HardwareProfile shared = slow_cores;
        shared.devices[0].shares_host_memory = Tri::Yes;
        const Plan on_shared = plan_on(shared, half, PlanRequest{});
        check(on_shared.config.moe.device_cache_mb == 0, "device tier, shared memory: no device cache");

        // With this model's own routing replayed, a cache holding half the expert set serves most of
        // the routed bytes (91% at half the set on a 125B model measured on such a card): the cache
        // wins on the same fast cores the linear credit lost on, and the plan says where its share came
        // from.
        ModelProfile routed = model;
        routed.hit_curve.measured = true;
        routed.hit_curve.decode_tokens = 256;
        routed.hit_curve.points = {
            {0, 0.0}, {model.expert_bytes / 4, 0.78}, {model.expert_bytes / 2, 0.91}, {model.expert_bytes, 1.0}};
        const Plan curve = plan_run(base_cfg(), card, routed, half, PlanRequest{}, PlannerPolicy::defaults());
        d = find(curve, "device-cache-mb");
        check(d && curve.config.moe.device_cache_mb > 0 &&
                  d->reason.find("from this model's own routing") != std::string::npos,
              "device tier, measured routing: the share is the model's own", d ? d->reason : "none");
        check(curve.config.buft_overrides.size() == model.n_layer,
              "device tier, measured routing: every layer's experts on the host");
        check(!curve.config.moe.record_unstreamed, "device tier, measured routing: nothing more to record");

        // Where the cache's misses come from: the backend's own host memory where it measured faster
        // past the margin and the host holds the copy, the file's mapping otherwise.
        {
            HardwareProfile hb = card;
            hb.devices[0].upload_host_buffer_gibs = 48.0;
            const Plan staged = plan_on(hb, half, PlanRequest{});
            const Decision * e = find(staged, "expert-host-buffer");
            check(staged.regime == Regime::Fits && staged.config.moe.expert_host_buffer == "discrete" && e &&
                      e->source == Source::Measured,
                  "expert host buffer: faster from the backend's host memory, and the copy fits",
                  e ? e->reason : "no decision");
            check(validate(staged.config).ok, "expert host buffer: a valid config", validate(staged.config).error);
            hb.devices[0].upload_host_buffer_gibs = 25.0;
            const Plan level_hb = plan_on(hb, half, PlanRequest{});
            check(level_hb.config.moe.expert_host_buffer.empty(),
                  "expert host buffer: inside the margin, the mapping stays");
            hb.devices[0].upload_host_buffer_gibs = 48.0;
            hb.residency_budget = model.file_bytes + model.expert_bytes / 2;
            const Plan tight = plan_on(hb, half, PlanRequest{});
            check(tight.config.moe.expert_host_buffer.empty(),
                  "expert host buffer: a copy the host cannot hold is not made");
        }

        // The cache is sized from the card's free memory, not from the fitter's whole layers: what the
        // fitter left free under one more layer joins the bytes it gave the experts.
        {
            Placement spare = half;
            spare.device_split = {{"discrete", (uint32_t) model.n_layer + 1}};
            spare.device_spare = {{"discrete", 2 * GiB}};
            const Plan more = plan_on(slow_cores, spare, PlanRequest{});
            const Decision * e = find(more, "device-cache-mb");
            check(e && more.config.moe.device_cache_mb == (int) (11 * 1024) &&
                      e->reason.find("left free under one more whole layer") != std::string::npos,
                  "device tier, spare memory: the cache takes the whole layers' experts and the remainder",
                  std::to_string(more.config.moe.device_cache_mb) + " MiB");

            // A small card: every dense part fits and not one whole layer does. The cache is armed
            // all the same, on what the dense part left free; before, the card stayed half empty.
            Placement dense_only = spare;
            dense_only.host_expert_layers.clear();
            for (uint32_t il = 0; il < model.n_layer; ++il)
                dense_only.host_expert_layers.push_back(il);
            dense_only.override_patterns = dense_only.all_expert_patterns;
            dense_only.device_bytes = 3 * GiB;
            dense_only.device_spare = {{"discrete", 6 * GiB}};
            const Plan small = plan_on(slow_cores, dense_only, PlanRequest{});
            e = find(small, "device-cache-mb");
            check(e && e->source == Source::Measured && small.config.moe.device_cache_mb == (int) (6 * 1024) &&
                      small.config.buft_overrides.size() == model.n_layer,
                  "device tier, no whole layer fits: the cache takes what the dense part left", e ? e->reason : "none");
            check(validate(small.config).ok, "device tier, no whole layer fits: a valid config",
                  validate(small.config).error);

            // Under one token cycle a cache returns no hits: nothing is armed and the fitter's
            // placement stands, as the RAM cache refuses the same.
            Placement tiny = dense_only;
            tiny.device_spare = {{"discrete", model.token_cycle_bytes / 2}};
            const Plan under = plan_on(slow_cores, tiny, PlanRequest{});
            e = find(under, "device-cache-mb");
            check(e && e->value == "0" && under.config.moe.device_cache_mb == 0 &&
                      e->reason.find("token cycle") != std::string::npos,
                  "device tier, under one token cycle: no cache", e ? e->reason : "none");

            // Two cards: llama.cpp hands each a share of one budget by its layers, so the budget is
            // what the card with the least spare per layer can take its share of.
            Placement two = half;
            two.device_split = {{"discrete", 37}, {"second", 12}};
            two.device_spare = {{"discrete", 6 * GiB}, {"second", 1 * GiB}};
            const Plan pair = plan_on(slow_cores, two, PlanRequest{});
            const uint64_t want = 9 * GiB + (uint64_t) ((double) GiB * 49.0 / 12.0);
            check(pair.config.moe.device_cache_mb == (int) (want / MiB),
                  "device tier, two cards: the budget each card can take its share of",
                  std::to_string(pair.config.moe.device_cache_mb) + " MiB, want " + std::to_string(want / MiB));
        }

        // The caller's placement is the caller's.
        PlanRequest pinned;
        pinned.pinned = {"gpu-layers"};
        const Plan kept = plan_on(slow_cores, half, pinned);
        check(kept.config.moe.device_cache_mb == 0, "device tier, gpu-layers pinned: left alone");
    }

    // ── a device expert cache keeps the prefill device off ─────────────────────────
    {
        HardwareProfile fast = phone();
        fast.host_wide_matmul_gibs = 200.0;
        fast.wide_batch = 128;
        fast.devices[0].wide_matmul_gibs = 1500.0;
        fast.devices[0].wide_identity_ok = Tri::Yes;
        fast.devices[0].identity_ok = Tri::Yes;
        RunConfig c = base_cfg();
        c.moe.device_cache_mb = 2000;
        const Plan p = plan_run(c, fast, model, PlanRequest{});
        const Decision * d = find(p, "prefill-device");
        check(!p.config.prefill.enabled() && d && d->value == "off",
              "device expert cache set: no prefill device beside it");
    }

    // ── the device crossing, priced: a device's dense-only layers come home, or stay ─────────────
    // A card with memory of its own, every layer's dense part on it and only the first eight layers
    // whole (the fitter's order on one device: whole layers first, then dense-only ones), the experts
    // of the other forty on the host and streamed. Each of those forty crosses twice a token.
    {
        ModelProfile m = model;
        m.largest_expert_layer_bytes = 384 * MiB;
        m.largest_layer_dense_bytes = 42 * MiB;
        HardwareProfile h = desktop();
        h.host_bandwidth_gibs = 40.0;
        ComputeDevice & gpu = h.devices[0];
        gpu.usable = Tri::Yes;
        gpu.identity_ok = Tri::Yes;
        gpu.memory_bandwidth_gibs = 300.0;
        gpu.split_seconds = 20e-6;
        Placement split;
        split.fitted = true;
        split.n_gpu_layers = (int32_t) m.n_layer + 1;
        split.device_split = {{"discrete", m.n_layer + 1}};
        for (uint32_t il = 8; il < m.n_layer; ++il)
            split.host_expert_layers.push_back(il);
        for (uint32_t il = 0; il < m.n_layer; ++il)
            split.all_expert_patterns.push_back("blk\\." + std::to_string(il) + "\\.ffn_(up|down|gate)_exps");
        split.override_patterns.assign(split.all_expert_patterns.begin() + 8, split.all_expert_patterns.end());
        split.device_bytes = 6 * GiB;
        split.device_context_bytes = 512 * MiB;
        split.host_resident_bytes = 1 * GiB;
        split.raw_host_context_bytes = 64 * MiB;
        split.raw_host_compute_bytes = 300 * MiB;
        const auto plan_on = [&](const HardwareProfile & hw, const PlanRequest & r) {
            return plan_run(base_cfg(), hw, m, split, r, PlannerPolicy::defaults());
        };

        // A fast link: the device's dense rate saves more than the crossings cost, and the split stands.
        const Plan fast = plan_on(h, PlanRequest{});
        const Decision * d = find(fast, "device-layers");
        check(d && d->value == "40 split" && d->source == Source::Measured &&
                  d->reason.find("so the split stands") != std::string::npos,
              "device crossing, fast link: the dense-only layers stay on the device, priced", d ? d->reason : "none");
        check(fast.config.n_gpu_layers == split.n_gpu_layers && fast.config.buft_overrides.size() == 40,
              "device crossing, fast link: the fitter's placement is applied unchanged");

        // A slow link (a crossing of 2 ms, as across a network): forty layers cost 160 ms a token in
        // crossings alone. They come home whole, the card keeps its eight whole layers and the head at
        // the top, and the plan is made again from that placement.
        HardwareProfile slow = h;
        slow.devices[0].split_seconds = 2e-3;
        const Plan home = plan_on(slow, PlanRequest{});
        d = find(home, "device-layers");
        check(d && d->value == "40 home" && d->source == Source::Measured,
              "device crossing, slow link: the dense-only layers come home", d ? d->reason : "none");
        check(home.config.n_gpu_layers == 9 && home.config.buft_overrides.empty() &&
                  home.config.device_split.size() == 1 && home.config.device_split[0].second == 9,
              "device crossing, slow link: the card keeps its whole layers at the top, nothing overridden",
              std::to_string(home.config.n_gpu_layers));
        check(home.placement.host_expert_layers.size() == 40 && home.placement.host_expert_layers.back() == 39,
              "device crossing, slow link: the forty layers below the card are the host's, experts and all");
        check(home.placement.host_resident_bytes == split.host_resident_bytes + 40 * 42 * MiB + 512 * MiB,
              "device crossing, slow link: the host is charged their dense part and, bounded, the context");
        check(validate(home.config).ok, "device crossing, slow link: a valid config", validate(home.config).error);
        int n_layers_notes = 0;
        for (const Decision & x : home.decisions)
            if (x.knob == "device-layers") ++n_layers_notes;
        check(n_layers_notes == 1, "device crossing, slow link: said once, in the plan made again");

        // The same saving, and no room for what it costs: the ranking leaves the layers where they are.
        HardwareProfile tight = slow;
        tight.residency_budget = 2 * GiB;
        const Plan kept = plan_on(tight, PlanRequest{});
        d = find(kept, "device-layers");
        check(d && d->value == "40 split" && kept.config.n_gpu_layers == split.n_gpu_layers,
              "device crossing, no room: the layers stay split", d ? d->reason : "none");

        // Unmeasured, nothing moves, and the plan says what it was owed.
        HardwareProfile blind = slow;
        blind.devices[0].split_seconds = 0.0;
        const Plan unpriced = plan_on(blind, PlanRequest{});
        d = find(unpriced, "device-layers");
        check(d && d->source == Source::Unprobed && unpriced.config.n_gpu_layers == split.n_gpu_layers &&
                  unpriced.config.buft_overrides.size() == 40,
              "device crossing, unmeasured: the fitter's placement stands", d ? d->reason : "none");

        // The card keeps as many whole layers, but they become the top ones: where the largest layer the
        // file has would not fit eight times in its free memory, nothing is moved.
        HardwareProfile small = slow;
        small.devices[0].memory_free = 2 * GiB;
        const Plan refused = plan_on(small, PlanRequest{});
        d = find(refused, "device-layers");
        check(d && d->value == "40 split" && d->reason.find("could not be shown") != std::string::npos &&
                  refused.config.n_gpu_layers == split.n_gpu_layers,
              "device crossing, card too small to be sure: the split stands", d ? d->reason : "none");

        // The caller's placement is the caller's.
        PlanRequest pinned;
        pinned.pinned = {"gpu-layers"};
        const Plan as_set = plan_on(slow, pinned);
        check(!find(as_set, "device-layers") && as_set.config.buft_overrides.size() == 40,
              "device crossing, gpu-layers pinned: left alone");

        // The dense rates are the real layer's where host and device were both measured on it: a card
        // whose matrices run slower than the host's on the model's own shapes loses the layers even
        // on the fast link the synthetic GEMV would have kept them on.
        HardwareProfile real = h;
        real.host_dense_gibs = 30.0;
        real.devices[0].dense_gibs = 15.0;
        const Plan real_home = plan_on(real, PlanRequest{});
        d = find(real_home, "device-layers");
        check(d && d->value == "40 home", "device crossing, real layer: slower on its shapes, the layers come home",
              d ? d->reason : "none");
        // Measured on one side only, the pair falls back to the GEMV on both, and the split stands.
        HardwareProfile half_real = h;
        half_real.host_dense_gibs = 30.0;
        const Plan one_side = plan_on(half_real, PlanRequest{});
        d = find(one_side, "device-layers");
        check(d && d->value == "40 split", "device crossing, one side measured: the same experiment on both");
    }

    // ── the device tier reads the host's rate on the real expert block ──────────────────────────
    // The synthetic GEMV read a 48-core host at 77 GiB/s while the decode's experts ran at about 12:
    // priced on the real layer, the cores are slow against the card and the cache wins.
    {
        HardwareProfile card = desktop();
        card.residency_budget = 80 * GiB;
        card.memory_total = 125 * GiB;
        card.host_bandwidth_gibs = 66.0;
        ComputeDevice & gpu = card.devices[0];
        gpu.memory_total = 24 * GiB;
        gpu.memory_free = 23 * GiB;
        gpu.usable = Tri::Yes;
        gpu.runs_expert_op = Tri::Yes;
        gpu.identity_ok = Tri::Yes;
        gpu.memory_bandwidth_gibs = 881.0;
        gpu.upload_gibs = 24.0;
        gpu.split_seconds = 20e-6;
        Placement half;
        half.fitted = true;
        half.n_gpu_layers = (int32_t) model.n_layer + 1;
        for (uint32_t il = 0; il < 24; ++il)
            half.host_expert_layers.push_back(il);
        for (uint32_t il = 0; il < model.n_layer; ++il)
            half.all_expert_patterns.push_back("blk\\." + std::to_string(il) + "\\.ffn_(up|down|gate)_exps");
        half.override_patterns.assign(half.all_expert_patterns.begin(), half.all_expert_patterns.begin() + 24);
        half.device_bytes = 9 * GiB + 3 * GiB;
        HardwareProfile real = card;
        real.host_expert_gibs = 4.0;
        real.devices[0].expert_gibs = 881.0;
        const Plan tier = plan_run(base_cfg(), real, model, half, PlanRequest{}, PlannerPolicy::defaults());
        const Decision * d = find(tier, "device-cache-mb");
        check(d && tier.config.moe.device_cache_mb > 0 &&
                  d->reason.find("measured on this model's own expert block") != std::string::npos,
              "device tier, real layer: the host's own expert rate prices the split", d ? d->reason : "none");
        HardwareProfile host_only = card;
        host_only.host_expert_gibs = 4.0;
        const Plan gemv = plan_run(base_cfg(), host_only, model, half, PlanRequest{}, PlannerPolicy::defaults());
        const Decision * g = find(gemv, "device-cache-mb");
        check(gemv.config.moe.device_cache_mb > 0 && g &&
                  g->reason.find("measured on this model's own expert block") == std::string::npos,
              "device tier, real layer on the host only: both sides from the GEMV, never one of each",
              g ? g->reason : "none");
    }

    // ── one card, two backends: the faster one drives it ────────────────────────────────────────
    // CUDA and Vulkan on one board register a device each, with the same PCI id. The first
    // registered is what llama.cpp keeps; another replaces it only where it measured faster.
    {
        HardwareProfile two = desktop();
        ComputeDevice & a = two.devices[0];
        a.name = "CUDA0";
        a.physical_id = "0000:c1:00.0";
        a.expert_gibs = 489.0;
        ComputeDevice b = a;
        b.name = "Vulkan0";
        b.expert_gibs = 300.0;
        two.devices.push_back(b);
        check(choose_backends(two, 0.10).left_out.empty(), "backends: the first registered holds when it is faster");
        two.devices.back().expert_gibs = 520.0;
        check(choose_backends(two, 0.10).left_out.empty(), "backends: inside the margin, the first holds");
        two.devices.back().expert_gibs = 640.0;
        const BackendChoice won = choose_backends(two, 0.10);
        check(won.left_out == std::vector<std::string>{"CUDA0"} && won.keep == std::vector<std::string>{"Vulkan0"},
              "backends: the faster one past the margin replaces the first");
        const Plan pl = plan_run(base_cfg(), two, model, PlanRequest{});
        const Decision * d = find(pl, "device-list");
        check(pl.config.device_list == std::vector<std::string>{"Vulkan0"} && d && d->source == Source::Measured,
              "backends: the plan lists it and says why", d ? d->reason : "no decision");
        HardwareProfile unknown = two;
        for (ComputeDevice & dv : unknown.devices)
            if (!dv.is_cpu) dv.physical_id.clear();
        check(choose_backends(unknown, 0.10).left_out.empty(),
              "backends: devices of unknown identity are never taken for one card");
        HardwareProfile mixed = two;
        mixed.devices.back().expert_gibs = 0.0;
        mixed.devices.back().memory_bandwidth_gibs = 900.0;
        mixed.devices[0].memory_bandwidth_gibs = 0.0;
        check(choose_backends(mixed, 0.10).left_out.empty(), "backends: figures of different kinds are not compared");
        PlanRequest pinned;
        pinned.pinned = {"devices"};
        check(plan_run(base_cfg(), two, model, pinned).config.device_list.empty(),
              "backends: the caller's devices are the caller's");
    }

    // ── no device of its own: the new facts change nothing they do not measure ──────────────────
    // On a machine with no device, and on one whose devices share its memory, the real layer's rates
    // (here equal to the GEMV's) and a measured crossing leave the plan byte for byte what it was:
    // no layer is priced onto or off a device, and no flag moves.
    {
        HardwareProfile cpu_only = desktop();
        cpu_only.devices.clear();
        ComputeDevice host;
        host.name = "CPU";
        host.is_cpu = true;
        cpu_only.devices.push_back(host);
        cpu_only.host_bandwidth_gibs = 40.0;
        HardwareProfile phone_m = phone();
        phone_m.host_bandwidth_gibs = 20.0;
        phone_m.devices[0].memory_bandwidth_gibs = 35.0;
        phone_m.devices[0].identity_ok = Tri::Yes;
        HardwareProfile unified_m = unified();
        unified_m.host_bandwidth_gibs = 60.0;
        unified_m.devices[1].memory_bandwidth_gibs = 90.0;
        unified_m.devices[1].identity_ok = Tri::Yes;
        const std::pair<const char *, HardwareProfile> machines[] = {
            {"cpu only", cpu_only},           {"phone", phone()},
            {"phone, measured", phone_m},     {"unified", unified()},
            {"unified, measured", unified_m}, {"hard cap", hard_capped()}};
        for (const auto & mc : machines) {
            HardwareProfile with = mc.second;
            with.host_expert_gibs = with.host_bandwidth_gibs;
            with.host_dense_gibs = with.host_bandwidth_gibs;
            for (ComputeDevice & dv : with.devices) {
                dv.expert_gibs = dv.memory_bandwidth_gibs;
                dv.dense_gibs = dv.memory_bandwidth_gibs;
            }
            HardwareProfile crossing = with;
            for (ComputeDevice & dv : crossing.devices)
                if (!dv.is_cpu) dv.split_seconds = 1e-3;
            const Plan before = plan_run(base_cfg(), mc.second, model, PlanRequest{});
            const Plan after = plan_run(base_cfg(), with, model, PlanRequest{});
            check(before.explain() == after.explain() && before.to_flags() == after.to_flags(),
                  std::string("no device of its own, ") + mc.first + ": the plan is unchanged by the real layer");
            // A measured crossing may change what a note says about a device; it moves no layer.
            const Plan crossed = plan_run(base_cfg(), crossing, model, PlanRequest{});
            check(!find(crossed, "device-layers") && crossed.to_flags() == before.to_flags(),
                  std::string("no device of its own, ") + mc.first + ": a measured crossing moves no layer");
        }
    }

    return failures ? 1 : 0;
}
