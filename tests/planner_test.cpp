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
#include "bmoe/auto_plan.h"
#include "bmoe/backend_choice.h"
#include "bmoe/planner.h"
#include "bmoe/probe.h"
#include "bmoe/routing.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

static const LedgerRow * ledger_row(const Plan & p, LedgerKind kind) {
    for (const LedgerRow & r : p.allocation.ledger.rows)
        if (r.kind == kind) return &r;
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

        // A projector on a host-only plan is a row of the host's ledger, and the cache is smaller by
        // it: dense weights the run holds whole, never streamed.
        {
            ModelProfile seeing = model;
            seeing.projector_bytes = 600 * MiB;
            const Plan v = plan_run(base_cfg(), desktop(), seeing, PlanRequest{});
            const LedgerRow * pr = ledger_row(v, LedgerKind::Projector);
            check(pr && pr->bytes == 600 * MiB && !pr->locked, "desktop, projector: a ledger row of its size");
            check(v.config.moe.cache_mb == p.config.moe.cache_mb - 600,
                  "desktop, projector: the cache gives up exactly the projector's bytes",
                  std::to_string(v.config.moe.cache_mb) + " vs " + std::to_string(p.config.moe.cache_mb));
            check(!ledger_row(p, LedgerKind::Projector), "desktop, no projector: no row");
        }

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

    // ── a table left mapped is not charged as dense ─────────────────────────────────
    // Shaped after a 96 GB host with no device of its own and a 75 GB file: 46 GB of experts, a
    // 27 GB per-layer table the graph gathers rows from, under 3 GB of dense weights read whole.
    // The reservation once carried the table the run leaves mapped, so the ledger charged 30 GB of
    // dense and the cache stopped at 92% of an expert set that fits whole beside what is held.
    {
        ModelProfile tabled = model;
        tabled.embedding_tensors = {"token_embd.weight", "per_layer_token_embd.weight"};
        tabled.group(WeightGroup::Embedding) = {27 * GiB, 30 * KiB, true, true};
        tabled.expert_bytes = 46 * GiB;
        tabled.group(WeightGroup::Experts) = {46 * GiB, tabled.token_cycle_bytes, false, true};
        tabled.dense_bytes = 27 * GiB + 2728 * MiB;
        tabled.file_bytes = tabled.expert_bytes + tabled.dense_bytes;
        tabled.largest_dense_tensor = 27 * GiB - 512 * MiB;
        HardwareProfile h = desktop();
        h.residency_budget = 74 * GiB; // what the issue's host offered after its margin
        h.memory_total = 96 * GiB;
        h.devices.clear();
        // Held whole beside the table it leaves mapped, the model fits that host: nothing streams.
        check(plan_run(base_cfg(), h, tabled, PlanRequest{}).regime == Regime::Fits,
              "mapped table: the model held beside its table fits");
        // On a host that cannot hold the expert set, the experts stream and the cache gets what the
        // table would have taken: charged, it would leave the cache under 10 GB of a 40 GB budget.
        h.residency_budget = 40 * GiB;
        const Plan p = plan_run(base_cfg(), h, tabled, PlanRequest{});
        check(p.regime == Regime::ExpertsStream, "mapped table: experts stream", regime_name(p.regime));
        check(p.config.moe.dense_mapped == tabled.embedding_tensors, "mapped table: the run leaves the tables mapped");
        const LedgerRow * dense = ledger_row(p, LedgerKind::Dense);
        check(dense && dense->bytes < tabled.group(WeightGroup::Embedding).bytes,
              "mapped table: the ledger's dense row does not carry the table",
              dense ? std::to_string(dense->bytes >> 20) + " MiB" : "no row");
        check((uint64_t) p.config.moe.cache_mb * MiB >= 20 * GiB, "mapped table: the cache is not short by the table",
              std::to_string(p.config.moe.cache_mb) + " MiB");
        check(p.allocation.ledger.held <= p.allocation.ledger.holdable_cap, "mapped table: what is held fits",
              std::to_string(p.allocation.ledger.held >> 20) + " MiB");
        check(validate(p.config).ok, "mapped table: the plan is a valid config", validate(p.config).error);
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

    // ── a table larger than the machine is not a dense set it must hold ─────────────
    // Shaped after #664: a per-layer table several times the memory this process may hold, beside a
    // dense set read whole that fits in a fraction of it. The graph gathers the table by row and the
    // run leaves it mapped, so the experts stream; weighing the regime with the table in it told the
    // user that not even the dense weights fit.
    {
        ModelProfile tabled = model;
        tabled.embedding_tensors = {"token_embd.weight", "per_layer_token_embd.weight"};
        tabled.group(WeightGroup::Embedding) = {28 * GiB, 30 * KiB, true, true};
        tabled.dense_bytes += 28 * GiB - 344 * MiB;
        tabled.file_bytes += 28 * GiB - 344 * MiB;
        tabled.largest_dense_tensor = 27 * GiB;
        const Plan p = plan_run(base_cfg(), phone(), tabled, PlanRequest{});
        check(p.regime == Regime::ExpertsStream, "table past the machine: experts stream", regime_name(p.regime));
        check(p.config.moe.dense_mapped == tabled.embedding_tensors,
              "table past the machine: the run leaves the tables mapped");
        const Decision * r = find(p, "regime");
        check(r && r->reason.find("embedding tables the run may leave mapped") != std::string::npos,
              "table past the machine: the regime says what it left out", r ? r->reason : "no decision");

        // A caller who pinned a list without the tables holds them: the regime weighs them.
        PlanRequest pinned;
        pinned.pinned.push_back("dense-mapped");
        RunConfig held = base_cfg();
        held.moe.dense_mapped = {"token_embd.weight"};
        const Plan q = plan_run(held, phone(), tabled, pinned);
        check(q.regime == Regime::DenseOversized, "table past the machine: a pinned list that holds it is weighed",
              regime_name(q.regime));
        // One that names every table leaves them mapped as the plan would.
        held.moe.dense_mapped = tabled.embedding_tensors;
        check(plan_run(held, phone(), tabled, pinned).regime == Regime::ExpertsStream,
              "table past the machine: a pinned list that names every table leaves them out");

        // A dense set past the machine with no table in it is still what the regime says it is.
        ModelProfile heavy = model;
        heavy.group(WeightGroup::Attention).bytes += 8 * GiB;
        heavy.dense_bytes += 8 * GiB;
        heavy.file_bytes += 8 * GiB;
        check(plan_run(base_cfg(), phone(), heavy, PlanRequest{}).regime == Regime::DenseOversized,
              "dense past the machine: still oversized");

        // Over a fitted placement, the tables are taken out as the placement's figure counted them.
        Placement pl;
        pl.fitted = true;
        pl.outcome = "synthetic";
        pl.n_gpu_layers = 24;
        pl.n_ctx = 4096;
        for (uint32_t il = 0; il < 24; ++il)
            pl.host_expert_layers.push_back(il);
        pl.host_table_bytes = 14 * GiB;
        pl.host_resident_bytes = pl.host_table_bytes + 2 * GiB;
        pl.device_bytes = 8 * GiB;
        HardwareProfile h = desktop();
        h.residency_budget = 8 * GiB;
        check(plan_run(base_cfg(), h, tabled, pl, PlanRequest{}).regime == Regime::ExpertsStream,
              "table past the machine, placed: experts stream");
        pl.host_table_bytes = 0; // a fitter's figure with no table in it: all of it is held
        check(plan_run(base_cfg(), h, tabled, pl, PlanRequest{}).regime == Regime::DenseOversized,
              "placed, no table counted: the whole figure is weighed");
    }

    // ── a placement's host figure is where llama.cpp puts each part ─────────────────
    // n_gpu_layers counts slots from the top, the output first; the input layer (the embedding
    // tables) never leaves the host. Four blocks of 10/20/30/40, tables 100, head 50, a final norm 5.
    {
        ModelProfile m;
        m.n_layer = 4;
        m.layer_dense_bytes = {10, 20, 30, 40};
        m.group(WeightGroup::Embedding).bytes = 100;
        m.group(WeightGroup::Output).bytes = 50;
        m.dense_bytes = 100 + 50 + 100 + 5;
        const auto host = [&](int32_t ngl) { return host_dense_of(m, layer_slots(ngl, m.n_layer)); };
        check(host(-1).bytes == 105 && host(-1).tables == 100, "slots: every slot leaves the tables and the norm",
              std::to_string(host(-1).bytes));
        check(host(5).bytes == 105, "slots: n_layer + 1 is every slot");
        check(host(4).bytes == 115, "slots: n_layer moves the output and all but the lowest block",
              std::to_string(host(4).bytes));
        check(host(2).bytes == 165, "slots: two slots are the output and the top block", std::to_string(host(2).bytes));
        check(host(0).bytes == m.dense_bytes, "slots: none is the whole dense set");
        check(layer_slots(4, 4).host_blocks() == 1 && layer_slots(-1, 4).host_blocks() == 0 &&
                  layer_slots(0, 4).host_blocks() == 4 && !layer_slots(0, 4).output_on_device(),
              "slots: the blocks the host keeps");

        // Without each block's bytes the blocks are what the tables and the head leave, by count.
        ModelProfile coarse = m;
        coarse.layer_dense_bytes.clear();
        check(host_dense_of(coarse, layer_slots(3, 4)).bytes == 100 + 105 / 2,
              "slots: a share of the blocks where their bytes are unknown",
              std::to_string(host_dense_of(coarse, layer_slots(3, 4)).bytes));
    }

    // ── a placement's tables left mapped are not charged as dense ───────────────────
    // The model above with a card: every slot on it, the experts on the host, so the host figure
    // carries the 27 GB per-layer table whole, as llama.cpp keeps it. The run leaves the table mapped;
    // charged all the same, it would hold the cache short by the table.
    {
        ModelProfile tabled = model;
        tabled.embedding_tensors = {"token_embd.weight", "per_layer_token_embd.weight"};
        tabled.group(WeightGroup::Embedding) = {27 * GiB, 30 * KiB, true, true};
        tabled.expert_bytes = 46 * GiB;
        tabled.group(WeightGroup::Experts) = {46 * GiB, tabled.token_cycle_bytes, false, true};
        tabled.dense_bytes = 27 * GiB + 2728 * MiB;
        tabled.file_bytes = tabled.expert_bytes + tabled.dense_bytes;
        tabled.largest_dense_tensor = 27 * GiB - 512 * MiB;
        Placement pl;
        pl.fitted = true;
        pl.outcome = "synthetic";
        pl.n_gpu_layers = (int32_t) tabled.n_layer + 1;
        pl.n_ctx = 4096;
        for (uint32_t il = 0; il < tabled.n_layer; ++il)
            pl.host_expert_layers.push_back(il);
        const HostDense dense = host_dense_of(tabled, layer_slots(pl.n_gpu_layers, tabled.n_layer));
        pl.host_resident_bytes = dense.bytes + 256 * MiB;
        pl.raw_host_compute_bytes = 256 * MiB;
        pl.host_table_bytes = dense.tables;
        pl.device_bytes = 4 * GiB;
        HardwareProfile h = desktop();
        h.residency_budget = 74 * GiB;
        h.memory_total = 96 * GiB;
        check(plan_run(base_cfg(), h, tabled, pl, PlanRequest{}).regime == Regime::Fits,
              "placed table: the model held beside its table fits");
        h.residency_budget = 40 * GiB;
        const Plan p = plan_run(base_cfg(), h, tabled, pl, PlanRequest{});
        check(p.regime == Regime::ExpertsStream, "placed table: experts stream", regime_name(p.regime));
        check(p.config.moe.dense_mapped == tabled.embedding_tensors, "placed table: the run leaves the tables mapped");
        const LedgerRow * row = ledger_row(p, LedgerKind::Dense);
        check(!row || row->bytes < tabled.group(WeightGroup::Embedding).bytes,
              "placed table: the ledger's dense row does not carry the table",
              row ? std::to_string(row->bytes >> 20) + " MiB" : "no row");
        check((uint64_t) p.config.moe.cache_mb * MiB >= 20 * GiB, "placed table: the cache is not short by the table",
              std::to_string(p.config.moe.cache_mb) + " MiB");
        check(p.allocation.ledger.held <= p.allocation.ledger.holdable_cap, "placed table: what is held fits",
              std::to_string(p.allocation.ledger.held >> 20) + " MiB");
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

        // Knobs that act only on the streamer's reads, set by the caller, are turned off with a note
        // where nothing is streamed: before, `--auto --overlap` on a model that fits refused to start
        // (`moe.overlap requires moe.enabled`), measured on a 16 GB card with 30 GiB of RAM.
        RunConfig asked = base_cfg();
        asked.moe.overlap = true;
        asked.moe.row_stream = true;
        PlanRequest pin;
        pin.pinned = {"overlap", "row-stream"};
        const Plan q = plan_run(asked, big, model, pin);
        const Decision * o = find(q, "overlap");
        check(q.regime == Regime::Fits && !q.config.moe.overlap && !q.config.moe.row_stream && o && o->value == "off",
              "roomy machine, overlap asked: off with a note, not a refusal", o ? o->reason : "no decision");
        check(validate(q.config).ok, "roomy machine, overlap asked: a valid config", validate(q.config).error);
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

        // The reason may name a shorter context that would bring the cache back. Whatever it names
        // has to do that by the figures of the same sentence: the suggestion is rounded down to a
        // power of two, and at the smallest one it offers the rounding once went UP past what was
        // asked, naming a context that frees less than the cache is short of.
        int suggested = 0, unsuggested = 0;
        for (uint64_t budget = 3 * GiB + 800 * MiB; budget <= 4 * GiB + 800 * MiB; budget += 16 * MiB) {
            HardwareProfile sweep = tight;
            sweep.residency_budget = budget;
            Placement pl;
            pl.n_ctx = 4096;
            pl.raw_host_context_bytes = 440 * MiB;
            const Plan q = plan_run(base_cfg(), sweep, model, pl, PlanRequest{});
            const Decision * d = find(q, "cache-mb");
            if (!d) continue;
            unsigned long long left = 0, cycle = 0, kv = 0, shortfall = 0, ctx = 0;
            const size_t at_left = d->reason.find("the "), at_cycle = d->reason.find("token cycle of ");
            const size_t at_kv = d->reason.find("The context reserves "), at_ctx = d->reason.find("ctx-size ");
            if (at_kv == std::string::npos || at_ctx == std::string::npos) {
                unsuggested += q.config.moe.cache_mb == 0;
                continue;
            }
            if (std::sscanf(d->reason.c_str() + at_left, "the %llu MiB left", &left) != 1 ||
                std::sscanf(d->reason.c_str() + at_cycle, "token cycle of %llu MiB", &cycle) != 1 ||
                std::sscanf(d->reason.c_str() + at_kv, "The context reserves %llu MiB here and the cache is %llu MiB",
                            &kv, &shortfall) != 2 ||
                std::sscanf(d->reason.c_str() + at_ctx, "ctx-size %llu", &ctx) != 1) {
                check(false, "tight machine, context suggestion: the reason reads as figures", d->reason);
                continue;
            }
            ++suggested;
            // What the named context frees, against what the cache lacks, a MiB of rounding allowed.
            const double freed = (double) kv * (1.0 - (double) ctx / 4096.0);
            check(ctx < 4096 && freed + 1.0 >= (double) shortfall,
                  "tight machine, context suggestion: the context it names frees what the cache is short of",
                  d->reason);
        }
        check(suggested > 0 && unsuggested > 0,
              "tight machine, context suggestion: the sweep crosses both a named context and none worth naming",
              std::to_string(suggested) + " named, " + std::to_string(unsuggested) + " not");

        const Allocation & a = p.allocation;
        const GroupPlacement & e = a.at(WeightGroup::Experts);
        check(e.lane == Lane::ExpertStream && e.resident_bytes == 0 && a.cache_bytes == 0,
              "tight machine: the expert lane is the stream, holding nothing");
        check(e.seconds_per_token > 0.0 && e.seconds_per_token < e.seconds_worst_case,
              "tight machine: the stream is chosen on its price against the mapping",
              std::to_string(e.seconds_per_token) + " vs " + std::to_string(e.seconds_worst_case));
        check(ledger_row(p, LedgerKind::Dense) && !ledger_row(p, LedgerKind::ExpertCache),
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
        check(!ledger_row(q, LedgerKind::Dense) && !ledger_row(q, LedgerKind::ExpertCache),
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
        check(p.config.n_gpu_layers == 0 && p.config.device_split.empty(),
              "dense model, nothing placed: every layer stays on the CPU");

        // Nothing streams, but what the fitter placed on a card stays there: dropping it left a model
        // that fit the card opening on the CPU alone.
        Placement placed;
        placed.fitted = true;
        placed.n_gpu_layers = (int32_t) dense.n_layer + 1;
        placed.device_split = {{"discrete", dense.n_layer + 1}};
        placed.n_ctx = 8192;
        placed.outcome = "the capacity fitter placed every layer";
        const Plan on_card = plan_run(base_cfg(), desktop(), dense, placed, PlanRequest{}, PlannerPolicy::defaults());
        check(on_card.regime == Regime::NotMoe && !on_card.config.moe.enabled &&
                  on_card.config.n_gpu_layers == placed.n_gpu_layers && on_card.config.device_split.size() == 1 &&
                  on_card.config.device_split[0].first == "discrete" && on_card.config.n_ctx == 8192,
              "dense model, placed by the fitter: the placement is applied, streaming stays off");
        PlanRequest ctx_pinned;
        ctx_pinned.pinned = {"ctx-size"};
        RunConfig ctx_cfg = base_cfg();
        ctx_cfg.n_ctx = 4096;
        check(plan_run(ctx_cfg, desktop(), dense, placed, ctx_pinned, PlannerPolicy::defaults()).config.n_ctx == 4096,
              "dense model, placed: a context set by the caller is the caller's");
        Placement declined = placed;
        declined.fitted = false;
        declined.shared_memory_placement = true;
        check(plan_run(base_cfg(), desktop(), dense, declined, PlanRequest{}, PlannerPolicy::defaults())
                      .config.n_gpu_layers == 0,
              "dense model, placement set aside: every layer stays on the CPU");
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
        // The rules below weigh a device's width by its memory and by measurement; the evidence bound on
        // a device's pass (ubatch_max_device, #533) is checked on its own at the end, so it is off here.
        PlannerPolicy unbounded = pol;
        unbounded.ubatch_max_device = 0;
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

        // The floor is reached from a context that is not a power of two times it: halving 4092 stops at
        // 1023, whose half is under the floor, and the width used to stay there with its buffers past
        // their share.
        RunConfig odd_ctx = base_cfg();
        odd_ctx.n_ctx = 4092;
        const Plan odd = plan_run(odd_ctx, hw, model, wide(4092, 64 * MiB), PlanRequest{});
        check((uint32_t) odd.config.n_ubatch == pol.ubatch_min,
              "width: a context that is not a power of two times the floor still narrows to the floor",
              std::to_string(odd.config.n_ubatch));
        // And a width whose buffers fit above the floor is not taken lower than it has to be.
        const Plan odd_fit = plan_run(odd_ctx, hw, model, wide(4092, per_token), PlanRequest{});
        const uint64_t wo = odd_fit.config.n_ubatch ? (uint64_t) odd_fit.config.n_ubatch : 4092;
        check(wo * per_token <= cap && (wo == 4092 || wo * 2 * per_token > cap || wo == pol.ubatch_min),
              "width: from an odd context, the widest whose buffers fit", std::to_string(wo));

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

        // A device with memory of its own, short of room (the fitter left layers on the host): its
        // buffers are held to their share of its memory, as the host's are of the host's. The same
        // buffers on a device the whole model fits take nothing there, and the graph stays wide.
        {
            HardwareProfile card = desktop();
            ComputeDevice & gpu = card.devices[0];
            gpu.memory_total = 12 * GiB;
            gpu.memory_free = 12 * GiB;
            gpu.shares_host_memory = Tri::No;
            const uint64_t dev_per_token = 400 * KiB; // 16384 wide: 6.25 GiB of the card, as measured on one
            Placement short_card = wide(16384, 4 * KiB);
            short_card.fitted = true;
            short_card.n_gpu_layers = (int32_t) model.n_layer - 1; // two of n_layer + 1 left on the host
            short_card.device_compute_bytes = 16384ull * dev_per_token;
            short_card.device_compute_per_token = dev_per_token;
            short_card.device_bytes = 11 * GiB;
            const Plan narrowed = plan_run(big_ctx, card, model, short_card, PlanRequest{});
            const Decision * dn = find(narrowed, "ubatch");
            const uint64_t wn = (uint64_t) narrowed.config.n_ubatch;
            const uint64_t dcap = (uint64_t) ((double) (12 * GiB) * pol.compute_share_max);
            check(wn > 0 && wn < 16384 && wn * dev_per_token <= dcap && dn && dn->source == Source::Policy &&
                      dn->reason.find("of device memory") != std::string::npos,
                  "width: a device short of room holds its buffers to their share of its memory",
                  dn ? std::to_string(wn) + ": " + dn->reason : "no decision");
            check(wn * 2 * dev_per_token > dcap || wn == pol.ubatch_min,
                  "width: the widest whose device buffers fit, not a narrower one", std::to_string(wn));

            // No layer on the card at all: the wide graph's buffers took the room of the first one. That is
            // the shortest of all, and the graph narrows the same way.
            Placement no_layers = short_card;
            no_layers.n_gpu_layers = 0;
            no_layers.device_bytes = 0;
            const Plan from_none = plan_run(big_ctx, card, model, no_layers, PlanRequest{});
            const Decision * d0 = find(from_none, "ubatch");
            const uint64_t w0 = (uint64_t) from_none.config.n_ubatch;
            check(w0 > 0 && w0 < 16384 && w0 * dev_per_token <= dcap && d0 && d0->source == Source::Policy,
                  "width: a device the wide graph left with no layer narrows it too",
                  d0 ? std::to_string(w0) + ": " + d0->reason : "no decision");

            // Every layer on the card, the experts on the host (a cache's job): the wide graph costs
            // cache, not a dense part, and that trade is not this rule's. The graph stays wide.
            Placement all_layers = short_card;
            all_layers.n_gpu_layers = (int32_t) model.n_layer + 1;
            const Plan layers_kept = plan_run(big_ctx, card, model, all_layers, PlanRequest{}, unbounded);
            check(layers_kept.config.graph_width() == 16384,
                  "width: every layer on the card keeps the widest graph, experts on the host or not",
                  std::to_string(layers_kept.config.graph_width()));

            // The whole model on the card: every layer placed, no experts left on the host.
            Placement whole = short_card;
            whole.n_gpu_layers = (int32_t) model.n_layer + 1;
            whole.host_expert_layers.clear();
            const Plan wide_kept = plan_run(big_ctx, card, model, whole, PlanRequest{}, unbounded);
            check(wide_kept.config.graph_width() == 16384,
                  "width: a device the model fits whole keeps the widest graph, and the session opens that wide",
                  std::to_string(wide_kept.config.graph_width()));
            // The same card under the default policy: the memory would take the wide graph, but a device
            // computes it, and its expert matmul is not known to run a pass past ubatch_max_device (#533).
            const Plan bounded = plan_run(big_ctx, card, model, whole, PlanRequest{});
            const Decision * db = find(bounded, "ubatch");
            check((uint32_t) bounded.config.graph_width() == pol.ubatch_max_device && db &&
                      db->source == Source::Policy && db->reason.find("#533") != std::string::npos &&
                      validate(bounded.config).ok,
                  "width: a device's graph is bounded by the widest pass its expert matmul is known to run",
                  db ? std::to_string(bounded.config.graph_width()) + ": " + db->reason : "no decision");
            check(bounded.placement.compute_width == pol.ubatch_max_device,
                  "width: the bounded width's buffers are the ones charged",
                  std::to_string(bounded.placement.compute_width));
            // Where the whole model fits, the fitter leaves llama.cpp's own "every layer", -1. It is the same
            // placement: a device computes the graph, and the bound holds. Read as "no layer", the graph
            // went as wide as the context on a card the model filled, and the context could not be made.
            Placement whole_all = whole;
            whole_all.n_gpu_layers = -1;
            const Plan bounded_all = plan_run(big_ctx, card, model, whole_all, PlanRequest{});
            check((uint32_t) bounded_all.config.graph_width() == pol.ubatch_max_device &&
                      validate(bounded_all.config).ok,
                  "width: every layer as llama.cpp's -1 is a device computing the graph, bounded the same",
                  std::to_string(bounded_all.config.graph_width()));

            // Every layer on the card and one layer's experts behind the cache at the probed width: the
            // fitter is asked at halves of it, widest first, and the first width that places the whole
            // model is taken, whatever the device's share of its memory says. Fits read from the facts,
            // as a re-fit is (AutoPlan::placement_at_width), so no fitter runs here.
            {
                RunConfig ctx16 = big_ctx;
                const uint32_t probed = pol.ubatch_max_device;
                Placement behind = whole.at_width(probed);
                behind.device_compute_per_token = 1 * KiB; // its share is the probed width itself
                behind.device_compute_bytes = (uint64_t) probed * behind.device_compute_per_token;
                behind.host_expert_layers = {model.n_layer - 1};
                AutoPlan facts;
                facts.hw = card;
                facts.model = model;
                facts.placement = behind;
                auto key_at = [&](uint32_t w) {
                    AutoPlan::FitKey k;
                    k.width = w;
                    k.n_ctx = (uint32_t) ctx16.n_ctx;
                    k.type_k = ctx16.cache_type_k;
                    k.type_v = ctx16.cache_type_v;
                    return k;
                };
                Placement fits_whole = behind.at_width(probed / 2);
                fits_whole.host_expert_layers.clear();
                facts.placement_at_width[key_at(probed / 2)] = fits_whole;
                const Plan found = replan(facts, ctx16, PlanRequest{}, model.knobs);
                const Decision * dw = find(found, "ubatch");
                check(found.config.n_ubatch == (int) (probed / 2) && dw &&
                          dw->brief.find("whole model fits") != std::string::npos,
                      "width: a fit one halving under the probed width is found, past the device's share",
                      dw ? std::to_string(found.config.n_ubatch) + ": " + dw->brief : "no decision");
                // A halving that brings no layer's experts onto the device ends the search: the wide
                // graph stands, and no narrower width is asked of the fitter.
                AutoPlan stuck = facts;
                stuck.placement_at_width.clear();
                stuck.placement_at_width[key_at(probed / 2)] = behind.at_width(probed / 2);
                const Plan kept_wide = replan(stuck, ctx16, PlanRequest{}, model.knobs);
                check(kept_wide.config.graph_width() == (int) probed && stuck.placement_at_width.size() == 1,
                      "width: a halving no closer to fitting ends the search at one fit",
                      std::to_string(kept_wide.config.graph_width()) + ", " +
                          std::to_string(stuck.placement_at_width.size()) + " fits");
            }
            // A graph already inside the bound is left as wide as the context.
            RunConfig small_ctx = big_ctx;
            small_ctx.n_ctx = 4096;
            Placement whole_small = whole.at_width(4096);
            const Plan inside = plan_run(small_ctx, card, model, whole_small, PlanRequest{});
            check(inside.config.n_ubatch == 0, "width: a graph inside the device bound stays as wide as the context",
                  std::to_string(inside.config.n_ubatch));

            // Two cards of different sizes: inside the share of both together, past it on the smaller
            // one, which reserves its own half of the buffers. Each card is held to its own memory.
            HardwareProfile two = card;
            two.devices[0].name = "big";
            two.devices[0].memory_total = 24 * GiB;
            ComputeDevice small = two.devices[0];
            small.name = "small";
            small.memory_total = 8 * GiB;
            small.memory_free = 8 * GiB;
            two.devices.push_back(small);
            Placement split = short_card;
            const uint64_t per_card = (uint64_t) (1.5 * (double) GiB); // 3 GiB together, 32 GiB of cards
            split.device_compute_bytes = 2 * per_card;
            split.device_compute_per_token = split.device_compute_bytes / 16384;
            split.projected_devices = {"big", "small"};
            split.projected_compute_bytes = {per_card, per_card};
            check((double) split.device_compute_bytes <= (double) (32 * GiB) * pol.compute_share_max,
                  "width, two cards: the fixture is inside the share of both cards together");
            const Plan by_card = plan_run(big_ctx, two, model, split, PlanRequest{});
            const uint64_t wc = (uint64_t) by_card.config.graph_width();
            check(wc > 0 && wc < 16384 &&
                      (double) by_card.placement.device_compute_of("small") <=
                          (double) (8 * GiB) * pol.compute_share_max,
                  "width, two cards: the smaller card's buffers are held to its own share",
                  std::to_string(wc) + ", small card " +
                      std::to_string(by_card.placement.device_compute_of("small") / MiB) + " MiB");

            // The width probe_and_plan asks the fitter about: each card held to its share, the
            // projected width where it already is, 0 where nothing was measured.
            const uint32_t share_w = device_share_width(short_card, card, pol);
            check(share_w > 0 && share_w < 16384 &&
                      (double) short_card.at_width(share_w).device_compute_bytes <=
                          (double) (12 * GiB) * pol.compute_share_max,
                  "device share width: the widest whose buffers fit the card's share", std::to_string(share_w));
            const uint32_t share_img = device_share_width(short_card, card, pol, 6000);
            check(share_img >= 6000 && share_img >= share_w,
                  "device share width: never narrower than the most tokens one image takes", std::to_string(share_img));
            Placement roomy = short_card;
            roomy.device_compute_per_token = 1 * KiB;
            roomy.device_compute_bytes = 16384ull * KiB;
            check(device_share_width(roomy, card, pol) == 16384,
                  "device share width: buffers already inside the share keep the projected width");
            Placement no_slope = short_card;
            no_slope.device_compute_per_token = 0;
            check(device_share_width(no_slope, card, pol) == 0, "device share width: an unmeasured slope says nothing");
            check(device_share_width(split, two, pol) == (uint32_t) by_card.config.graph_width(),
                  "device share width: the same per-card answer the width rule acts on");

            // The fitter leaves its count at "every layer" where the model fits the devices whole, and
            // the plan says so rather than "0 layers".
            Placement every = whole;
            every.n_gpu_layers = -1;
            const Plan all_on = plan_run(base_cfg(), card, model, every, PlanRequest{});
            const Decision * pd = find(all_on, "placement");
            check(pd && pd->value == "every layer on devices", "placement: the fitter's default reads as every layer",
                  pd ? pd->value : "none");

            // Every layer on the card, experts on the host, buffers past the card's share: the width is
            // decided across loads (MeasuredKnobs::width_narrow), by the requests this machine serves.
            {
                HardwareProfile no_tier = card;
                no_tier.devices[0].identity_ok = Tri::No;     // no device cache: the width alone is weighed
                no_tier.devices[0].upload_gibs = 24.0;        // what a prompt's pass gathers at
                no_tier.devices[0].wide_matmul_gibs = 1500.0; // a prompt-wide layer on the card
                no_tier.host_expert_gibs = 20.0;              // the host and the card on this model's own layer
                no_tier.host_dense_gibs = 30.0;
                no_tier.devices[0].expert_gibs = 400.0;
                no_tier.devices[0].dense_gibs = 400.0;
                Placement roomy_layers = all_layers;
                for (uint32_t il = 0; il < 8; ++il)
                    roomy_layers.host_expert_layers.push_back(il);
                const uint32_t narrow = device_share_width(roomy_layers, no_tier, pol);
                const auto plan_with = [&](const MeasuredKnobs & k) {
                    ModelProfile mm = model;
                    mm.knobs = k;
                    return plan_run(big_ctx, no_tier, mm, roomy_layers, PlanRequest{}, unbounded);
                };
                // Nothing measured, no request served: both widths are priced from the probed rates
                // over the default mix, the load serves the one priced faster and times it as it serves.
                const Plan first = plan_with(MeasuredKnobs{});
                const Decision * dw = find(first, "ubatch");
                const bool first_narrow = first.config.n_ubatch == (int) narrow;
                check(narrow > 0 && narrow < 16384 && first.config.trial.width_narrow == (int) narrow &&
                          first.config.trial.width_side == (first_narrow ? 0 : 1) && dw &&
                          dw->source == Source::Derived && dw->reason.find("the default mix") != std::string::npos &&
                          dw->reason.find("priced from the probed rates") != std::string::npos,
                      "width, nothing measured: priced on the default mix, the side served is the side timed",
                      dw ? dw->reason : "none");
                check(validate(first.config).ok, "width: a valid config", validate(first.config).error);
                // The mix decides between the same prices: a machine that only decodes takes the narrow
                // graph (more of the experts on the card, nothing to gather twice), and one whose prompts
                // are long and answers short takes whichever its passes cost less on.
                MeasuredKnobs chat_mix;
                chat_mix.mix_requests = 4000; // enough of one kind to be this machine's mix
                chat_mix.mix_gen_tokens = 500;
                const Plan chat_priced = plan_with(chat_mix);
                dw = find(chat_priced, "ubatch");
                check(chat_priced.config.n_ubatch == (int) narrow && chat_priced.config.trial.width_side == 0 && dw &&
                          dw->reason.find("requests served here") != std::string::npos,
                      "width, a machine that only decodes: the narrow graph, priced", dw ? dw->reason : "none");
                // Both decodes measured on served loads, no long prompt: the measurements decide, and the
                // load times nothing more (it still records its requests against the narrow width).
                MeasuredKnobs k = chat_mix;
                k.width_narrow = (int) narrow;
                k.wide_decode_tps = 14.2;
                k.narrow_decode_tps = 19.4;
                const Plan chat = plan_with(k);
                dw = find(chat, "ubatch");
                check(chat.config.n_ubatch == (int) narrow && chat.config.trial.width_side == -1 &&
                          chat.config.trial.width_narrow == (int) narrow && dw && dw->source == Source::Measured,
                      "width, both decodes measured, short prompts only: the narrow graph", dw ? dw->reason : "none");
                // The same decodes on a machine that prefills 9500 tokens a request, both prefills
                // measured: 500 tokens decoded and 9500 prefilled weigh for the wide graph.
                k.mix_wide_prompt_tokens = 9500;
                k.wide_prefill_tps = 610;
                k.narrow_prefill_tps = 104;
                const Plan agent = plan_with(k);
                dw = find(agent, "ubatch");
                check(agent.config.graph_width() == 16384 && agent.config.trial.width_side == -1 && dw &&
                          dw->source == Source::Measured,
                      "width, long prompts measured slower on the narrow graph: the full width wins",
                      dw ? dw->reason : "none");
                // One side measured: both prices are scaled by how far that side's ran from it, so the
                // ratio between the widths stays the price's and no load is opened to time the other.
                MeasuredKnobs one = chat_mix;
                one.width_narrow = (int) narrow;
                one.narrow_decode_tps = 19.4;
                const Plan scaled = plan_with(one);
                dw = find(scaled, "ubatch");
                check(scaled.config.n_ubatch == chat_priced.config.n_ubatch && scaled.config.trial.width_side == -1 &&
                          dw && dw->reason.find("corrected by the width measured") != std::string::npos,
                      "width, one side measured: the price corrected, the same side served, nothing timed again",
                      dw ? dw->reason : "none");
                // A handful of requests does not swing the width: they are weighed with the default mix,
                // so the plan stays where it was with nothing served. Measured on two 16 GB cards: four
                // requests read alone as 1181 prompt tokens, the next load's three short ones as 34, and
                // the graph went narrow and lost 41% of a 4.6k-token prompt's prefill.
                MeasuredKnobs few;
                few.mix_requests = 3;
                few.mix_gen_tokens = 171;
                few.mix_wide_prompt_tokens = 34;
                const Plan few_plan = plan_with(few);
                dw = find(few_plan, "ubatch");
                check(few_plan.config.n_ubatch == first.config.n_ubatch && dw &&
                          dw->reason.find("3 requests served here, weighed with the default mix") != std::string::npos,
                      "width, three short requests recorded: weighed with the default mix, the width holds",
                      dw ? dw->reason : "none");
                // One long prompt among short ones is passes at the narrow width, which their mean hides:
                // 1181 tokens a request fit one pass, a 4.6k-token prompt every fourth request does not.
                // With experts left off the card at both widths, the same mean prompt is priced a pass's
                // gather dearer at the narrow width where a quarter of the prompts are long, and the same
                // at the full width, which they fit.
                HardwareProfile slow_gather = no_tier;
                slow_gather.devices[0].upload_gibs = 0.5;
                Placement most_off = roomy_layers; // more experts off the card than the narrow graph frees
                for (uint32_t il = 8; il < 32; ++il)
                    most_off.host_expert_layers.push_back(il);
                // The seconds the plan's line gives each width's prefill: "... prefills that prompt in W s,
                // at N tokens at R tok/s and X s (...".
                const auto prefill_priced = [&](double long_tokens, double & wide_s, double & narrow_s) {
                    ModelProfile mm = model;
                    mm.knobs.mix_requests = 4000;
                    mm.knobs.mix_gen_tokens = 190;
                    mm.knobs.mix_wide_prompt_tokens = 1181;
                    mm.knobs.width_narrow = (int) narrow;
                    mm.knobs.usage_requests = 4000;
                    mm.knobs.usage_gen_tokens = 190;
                    mm.knobs.usage_long_prompt_tokens = long_tokens;
                    const Plan wp = plan_run(big_ctx, slow_gather, mm, most_off, PlanRequest{}, unbounded);
                    const Decision * x = find(wp, "ubatch");
                    wide_s = narrow_s = 0.0;
                    if (!x) return;
                    const size_t w = x->reason.find("prefills that prompt in "), n = x->reason.find(" tok/s and ", w);
                    if (w == std::string::npos || n == std::string::npos) return;
                    wide_s = std::atof(x->reason.c_str() + w + 24);
                    narrow_s = std::atof(x->reason.c_str() + n + 11);
                };
                double wide_none = 0, narrow_none = 0, wide_long = 0, narrow_long = 0;
                prefill_priced(0.0, wide_none, narrow_none);
                prefill_priced(1155.0, wide_long, narrow_long);
                check(narrow_none > 0.0 && narrow_long > narrow_none * 1.1 && wide_long == wide_none,
                      "width, the same mean prompt: long prompts are more passes at the narrow width only",
                      std::to_string(narrow_none) + " " + std::to_string(narrow_long) + " " +
                          std::to_string(wide_long));
                // A narrow width other than the one measured: the measurements do not answer it.
                MeasuredKnobs stale = k;
                stale.width_narrow = (int) narrow * 2;
                const Plan stale_plan = plan_with(stale);
                dw = find(stale_plan, "ubatch");
                check(dw && dw->source == Source::Derived, "width, another narrow width: priced afresh");
                // A pinned width is the caller's.
                PlanRequest pin_ub;
                pin_ub.pinned.push_back("ubatch");
                ModelProfile mm = model;
                const Plan pinned_ub = plan_run(big_ctx, no_tier, mm, roomy_layers, pin_ub, unbounded);
                check(pinned_ub.config.trial.width_side == -1 && pinned_ub.config.trial.width_narrow == 0,
                      "width, pinned: nothing weighed, nothing timed");

                // The margin guards the width in force: the full width until a load ran the narrow one
                // as decided, then that one. The same rates, weighed again, do not swing it.
                MeasuredKnobs cal = chat_mix;
                cal.width_narrow = (int) narrow;
                cal.wide_decode_tps = 14.2;
                cal.narrow_decode_tps = 14.5; // inside the margin
                check(plan_with(cal).config.graph_width() == 16384 && plan_with(cal).config.trial.width_side == -1,
                      "width, the narrow graph inside the margin: the full width holds");
                MeasuredKnobs held = cal;
                held.width_held = 0;
                held.narrow_decode_tps = 14.2;
                held.wide_decode_tps = 15.0; // the full width 6% ahead
                const Plan held_plan = plan_with(held);
                dw = find(held_plan, "ubatch");
                check(held_plan.config.n_ubatch == (int) narrow && dw &&
                          dw->reason.find("in force") != std::string::npos,
                      "width, the narrow graph in force, the full width inside the margin: it holds",
                      dw ? dw->reason : "none");
                held.wide_decode_tps = 16.0; // 13% ahead
                check(plan_with(held).config.graph_width() == 16384,
                      "width, the narrow graph in force, the full width past the margin: it takes over");
                // A width that failed to open here as often as a plan lets it loses; once is the
                // machine's moment, and the price still decides.
                MeasuredKnobs fails_narrow = chat_mix;
                fails_narrow.width_narrow = (int) narrow;
                fails_narrow.narrow_unviable = 1;
                check(plan_with(fails_narrow).config.n_ubatch == (int) narrow,
                      "width, the narrow graph failed to open once: still the plan's, on its price");
                fails_narrow.narrow_unviable = MeasuredKnobs::unviable_max;
                const Plan wide_kept = plan_with(fails_narrow);
                dw = find(wide_kept, "ubatch");
                check(wide_kept.config.graph_width() == 16384 && wide_kept.config.trial.width_side == -1 && dw &&
                          dw->source == Source::Measured && dw->reason.find("failed") != std::string::npos,
                      "width, the narrow graph could not be opened: the full width stands", dw ? dw->reason : "none");
                MeasuredKnobs fails_wide = fails_narrow;
                fails_wide.narrow_unviable = 0;
                fails_wide.wide_unviable = MeasuredKnobs::unviable_max;
                const Plan narrow_kept = plan_with(fails_wide);
                check(narrow_kept.config.n_ubatch == (int) narrow && narrow_kept.config.trial.width_side == -1,
                      "width, the full width could not be opened: the narrow graph stands");
                // Another narrow width: what failed against the old one answers nothing.
                fails_narrow.width_narrow = (int) narrow * 2;
                check(plan_with(fails_narrow).config.n_ubatch == (int) narrow,
                      "width, a failure at another narrow width: priced afresh");
            }

            // A slope the device side did not measure decides nothing there.
            Placement blind_dev = short_card;
            blind_dev.device_compute_per_token = 0;
            const Plan unmeasured = plan_run(big_ctx, card, model, blind_dev, PlanRequest{}, unbounded);
            check(unmeasured.config.graph_width() == 16384,
                  "width: an unmeasured device slope leaves the graph at the probed width",
                  std::to_string(unmeasured.config.graph_width()));
        }

        // at_width is a line through the projected point, clamped at zero.
        const Placement base_pl = wide(4096, per_token);
        check(base_pl.at_width(2048).raw_host_compute_bytes == 2048ull * per_token &&
                  base_pl.at_width(8192).raw_host_compute_bytes == 8192ull * per_token &&
                  base_pl.at_width(4096).raw_host_compute_bytes == base_pl.raw_host_compute_bytes,
              "width: buffers move along the measured slope, both ways");

        // A long context the probe measured at the default width (RunConfig::graph_width, capped at
        // ubatch_auto_max), at that context: the plan starts from the whole context, priced by extending
        // the probe's slope, and narrows from there.
        {
            RunConfig long_ctx = base_cfg();
            long_ctx.n_ctx = 32768;
            const uint32_t probe_w = (uint32_t) long_ctx.graph_width();
            check(probe_w == (uint32_t) RunConfig::ubatch_auto_max && probe_w < 32768,
                  "width, long context: the probe measures at the default width, under the context");
            auto probed_at_default = [&](uint64_t pt) {
                Placement pl = wide(probe_w, pt);
                pl.n_ctx = 32768;
                return pl;
            };
            // Roomy: the whole context's buffers are half the share. One pass, wider than the probe.
            const uint64_t roomy_pt = cap / 32768 / 2;
            const Plan roomy = plan_run(long_ctx, hw, model, probed_at_default(roomy_pt), PlanRequest{});
            const Decision * dr = find(roomy, "ubatch");
            check(roomy.config.graph_width() == 32768 && roomy.config.n_ubatch == 32768 && dr &&
                      dr->source == Source::Derived && dr->reason.find("priced by extending") != std::string::npos &&
                      validate(roomy.config).ok,
                  "width, long context: a roomy host gets a graph as wide as the context, wider than the probe",
                  dr ? std::to_string(roomy.config.n_ubatch) + ": " + dr->reason : "no decision");
            check(roomy.placement.compute_width == 32768 && roomy.placement.raw_host_compute_bytes == 32768 * roomy_pt,
                  "width, long context: the wider graph's extended buffers are the ones charged",
                  std::to_string(roomy.placement.raw_host_compute_bytes / MiB));
            // As on the host of #566: the whole context's buffers are past the share, the probe's width is
            // inside it. The plan narrows to the widest that fits, which is the default width here.
            const uint64_t tight_pt = cap * 3 / 4 / probe_w;
            const Plan tight = plan_run(long_ctx, hw, model, probed_at_default(tight_pt), PlanRequest{});
            const Decision * dt = find(tight, "ubatch");
            check(tight.config.graph_width() == (int) probe_w && dt && dt->source == Source::Policy &&
                      dt->reason.find("priced by extending") != std::string::npos,
                  "width, long context: whole-context buffers past the share narrow the graph",
                  dt ? std::to_string(tight.config.graph_width()) + ": " + dt->reason : "no decision");
            // Tighter still: narrower than the probe, set explicitly.
            const uint64_t tighter_pt = cap * 3 / 4 / (probe_w / 2);
            const Plan tighter = plan_run(long_ctx, hw, model, probed_at_default(tighter_pt), PlanRequest{});
            check(tighter.config.n_ubatch == (int) probe_w / 2,
                  "width, long context: buffers past the share at the probe's width narrow below it",
                  std::to_string(tighter.config.n_ubatch));
            // A device part of the buffers whose slope was not measured: a wider graph is not priced, and
            // the plan stays at the probe's width even where the host's part would take the whole context.
            Placement half_blind = probed_at_default(roomy_pt);
            half_blind.device_compute_bytes = 64 * MiB;
            half_blind.device_compute_per_token = 0;
            const Plan stays = plan_run(long_ctx, hw, model, half_blind, PlanRequest{});
            const Decision * dh = find(stays, "ubatch");
            check(stays.config.graph_width() == (int) probe_w && dh &&
                      dh->reason.find("not priced") != std::string::npos,
                  "width, long context: an unmeasured slope stays at the probe's width",
                  dh ? std::to_string(stays.config.graph_width()) + ": " + dh->reason : "no decision");
            // A width the config set but did not pin is the starting width, not the context.
            RunConfig set_w = long_ctx;
            set_w.n_ubatch = 4096;
            const Plan from_set = plan_run(set_w, hw, model, wide(4096, roomy_pt), PlanRequest{});
            check(from_set.config.graph_width() == 4096, "width, long context: a set width is not widened",
                  std::to_string(from_set.config.graph_width()));

            // A device that computes the graph: the whole context is the candidate, and the evidence
            // bound (#533) is what stops it, at no more than the width the probe placed the layers at.
            check(pol.ubatch_max_device <= (uint32_t) RunConfig::ubatch_auto_max,
                  "width: the device bound is no wider than the default width the probe measures at");
            HardwareProfile big_card = desktop();
            big_card.devices[0].memory_total = 32 * GiB;
            big_card.devices[0].memory_free = 32 * GiB;
            Placement on_card = probed_at_default(4 * KiB);
            on_card.fitted = true;
            on_card.n_gpu_layers = (int32_t) model.n_layer + 1;
            on_card.device_compute_per_token = 4 * KiB;
            on_card.device_compute_bytes = (uint64_t) probe_w * 4 * KiB;
            on_card.device_bytes = 20 * GiB;
            const Plan card_plan = plan_run(long_ctx, big_card, model, on_card, PlanRequest{});
            const Decision * dd = find(card_plan, "ubatch");
            check((uint32_t) card_plan.config.graph_width() == pol.ubatch_max_device && dd &&
                      dd->reason.find("#533") != std::string::npos && dd->reason.find("32768") != std::string::npos,
                  "width, long context: a device's graph is bounded at ubatch_max_device, not at the context",
                  dd ? std::to_string(card_plan.config.graph_width()) + ": " + dd->reason : "no decision");
            // An image cap past the device's bound does not lift it: every prompt would pass wider on the
            // device. The bound wins, and the plan says such an image is refused.
            {
                RunConfig big_images = long_ctx;
                big_images.mmproj = "projector.gguf";
                big_images.image_max_tokens = 12000;
                check(big_images.graph_width() == 12000, "width, image past the bound: the default follows the cap");
                Placement img_card = on_card;
                img_card.compute_width = 12000;
                img_card.raw_host_compute_bytes = 12000ull * 4 * KiB;
                img_card.device_compute_bytes = 12000ull * 4 * KiB;
                const Plan held = plan_run(big_images, big_card, model, img_card, PlanRequest{});
                const Decision * dh2 = find(held, "ubatch");
                check((uint32_t) held.config.graph_width() == pol.ubatch_max_device && dh2 &&
                          dh2->reason.find("#533") != std::string::npos &&
                          dh2->reason.find("image-max-tokens") != std::string::npos && validate(held.config).ok,
                      "width, image past the bound: a device's bound wins, and the plan says images past it are "
                      "refused",
                      dh2 ? std::to_string(held.config.graph_width()) + ": " + dh2->reason : "no decision");
            }

            // An image whose tokens go in one pass: the narrowing stops at the most one image takes.
            RunConfig with_images = long_ctx;
            with_images.mmproj = "projector.gguf";
            with_images.image_max_tokens = 1000;
            const Plan img_floor = plan_run(with_images, hw, model, probed_at_default(64 * MiB), PlanRequest{});
            const Decision * di = find(img_floor, "ubatch");
            check(img_floor.config.graph_width() >= 1000 && img_floor.config.graph_width() < 2000 && di &&
                      di->reason.find("an image takes") != std::string::npos,
                  "width: never narrower than the most tokens one image takes, and says why",
                  di ? std::to_string(img_floor.config.graph_width()) + ": " + di->reason : "no decision");
            const Plan no_img = plan_run(long_ctx, hw, model, probed_at_default(64 * MiB), PlanRequest{});
            check((uint32_t) no_img.config.graph_width() == pol.ubatch_min,
                  "width: without a projector the floor is ubatch_min", std::to_string(no_img.config.graph_width()));
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

        // The dense policy copies every dense tensor, so a group read whole every token that the
        // budget had no room for is held by the run all the same, and priced as held. Priced as
        // mapped, at its refault, it made a phone's plan predict 10-44 s a token for a model that
        // decoded at 0.5 (Gemma 4 26B-A4B, its 16384 context taking the budget).
        {
            AllocationInputs in = inputs(3 * GiB);
            in.fixed_bytes = 3 * GiB - 64 * MiB; // the context leaves no room for any dense group
            uint64_t read_whole = 0;
            for (WeightGroup g : {WeightGroup::Attention, WeightGroup::DenseFfn, WeightGroup::Other})
                read_whole += model.group(g).bytes;
            in.reserved_bytes = read_whole;
            const Allocation a = allocate(measured_machine(), model, in, pol);
            const GroupPlacement & att = a.at(WeightGroup::Attention);
            check(att.lane == Lane::Resident && att.resident_bytes == model.group(WeightGroup::Attention).bytes,
                  "a read-whole group the dense policy holds is held in the plan too");
            check(att.seconds_per_token > 0.0 && att.seconds_per_token < att.seconds_worst_case,
                  "and priced at its compute, not at its refault");
            check(a.at(WeightGroup::Embedding).resident_bytes == 0,
                  "a row-gathered table stays where the ranking put it: the run leaves it mapped too");
            AllocationInputs none = in;
            none.reserved_bytes = 0;
            const Allocation mapped = allocate(measured_machine(), model, none, pol);
            check(mapped.at(WeightGroup::Attention).lane == Lane::Mmap,
                  "with nothing copied, a group with no room is still priced as mapped");
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
        const LedgerRow * oc = ledger_row(o, LedgerKind::ExpertCache);
        check(!o.config.moe.cache_pin_fit && o.config.moe.cache_mb == 12000,
              "ledger: a caller's cache budget is not fitted");
        check(oc && oc->bytes == 12000 * MiB && o.allocation.ledger.held > o.allocation.ledger.holdable_cap,
              "ledger: a caller's cache is the row, even over the ceiling");
        int n_cache_rows = 0;
        for (const LedgerRow & r : o.allocation.ledger.rows)
            if (r.kind == LedgerKind::ExpertCache) ++n_cache_rows;
        check(n_cache_rows == 1 && oc->name == "expert cache",
              "ledger: a caller's cache replaces the planned row, matched by kind, under its display name");

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
        const LedgerRow * pc = ledger_row(ph, LedgerKind::ExpertCache);
        const LedgerRow * pd = ledger_row(ph, LedgerKind::Dense);
        check(pd && pd->locked && pc && !pc->locked, "ledger: two stores, and only the dense set is in the locked one");
        bool named = !ph.allocation.ledger.rows.empty();
        for (const LedgerRow & r : ph.allocation.ledger.rows)
            named = named && r.name == ledger_kind_name(r.kind);
        check(named, "ledger: every row's display name is its kind's one spelling");
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
        const LedgerRow * unknown_cache = ledger_row(unknown_plan, LedgerKind::ExpertCache);
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
        // A machine that can take memory back by dropping it gets an offered cache, never a locked one:
        // nothing is charged to the lock, and a zero lock does not shape the cache. The kernel takes
        // an offered page back by dropping it, a reclaim that writes nothing, so the margin - room
        // for a reclaim that does write - is not taken out of it: the cache is larger than the same
        // cache as ordinary memory, by the margin on what it no longer counts, and what the run
        // holds still closes under the ceiling.
        HardwareProfile offers = zero_bound;
        offers.discardable_memory = true;
        const Plan offered_plan = plan_run(base_cfg(), offers, model, PlanRequest{});
        const Plan no_pin_known = plan_run(no_pin, zero_bound, model, no_pin_request);
        check(offered_plan.allocation.cache_locked_bytes == 0 && !offered_plan.allocation.cache_lock_unverified &&
                  !offered_plan.config.moe.cache_pin_fit,
              "ledger: a discardable cache is never locked");
        check(offered_plan.config.moe.cache_mb > no_pin_known.config.moe.cache_mb,
              "ledger: an offered cache is outside the margin an ordinary one pays",
              std::to_string(offered_plan.config.moe.cache_mb) + " MiB offered against " +
                  std::to_string(no_pin_known.config.moe.cache_mb) + " MiB ordinary");
        check(offered_plan.allocation.ledger.held <= offered_plan.allocation.ledger.holdable_cap,
              "ledger: an offered cache still closes under what can be held",
              mibs_of(offered_plan.allocation.ledger.held) + " of " +
                  mibs_of(offered_plan.allocation.ledger.holdable_cap));
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
        const LedgerRow * dev = ledger_row(on, LedgerKind::Device);
        check(dev && dev->locked && dev->bytes == 768 * MiB,
              "ledger: an armed device is charged two expert layers, locked", dev ? mibs_of(dev->bytes) : "no row");
        check(ledger_row(off, LedgerKind::Device) == nullptr, "ledger: a device nobody armed is not a line");
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
        const LedgerRow * fr_row = ledger_row(fx, LedgerKind::ContextCompute);
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
        const LedgerRow * ck_row = ledger_row(cx, LedgerKind::Checkpoints);
        check(ck_row && ck_row->bytes == 200 * MiB, "ledger: a chat run charges its checkpoints",
              ck_row ? mibs_of(ck_row->bytes) : "no row");
        check(fx.cache_budget_bytes - cx.cache_budget_bytes == 200 * MiB, "ledger: and the cache gives them up",
              mibs_of(fx.cache_budget_bytes) + " against " + mibs_of(cx.cache_budget_bytes));
        check(ledger_row(plan_run(base_cfg(), phone(), model, ck, PlanRequest{}), LedgerKind::Checkpoints) == nullptr,
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
        const LedgerRow * row = ledger_row(on, LedgerKind::Device);
        const LedgerRow * turn = ledger_row(on, LedgerKind::DeviceSlots);
        const uint64_t stays = 2 * 64 * MiB;    // the layer's other weights, held for the session
        const uint64_t in_turn = 2 * 384 * MiB; // the expert slots, held only during a prefill
        check(on.config.prefill.device == "accel",
              "prefill: a device that wins wide and agrees is armed by its own name");
        // The prefill device computes every pass as wide as the graph: a long context that the memory
        // takes whole is held to the device's bound (#533) once the device is armed, and not otherwise.
        {
            RunConfig long_cfg = base_cfg();
            long_cfg.n_ctx = 32768;
            Placement probed;
            probed.n_ctx = 32768;
            probed.compute_width = 8192;
            probed.host_compute_per_token = 1 * KiB;
            probed.raw_host_compute_bytes = 8192ull * KiB;
            const uint32_t dev_bound = PlannerPolicy::defaults().ubatch_max_device;
            const Plan pre =
                plan_run(long_cfg, machine(1500.0, Tri::Yes), mm, probed, PlanRequest{}, PlannerPolicy::defaults());
            const Decision * dp = find(pre, "ubatch");
            check(pre.config.prefill.device == "accel" && (uint32_t) pre.config.graph_width() == dev_bound && dp &&
                      dp->reason.find("#533") != std::string::npos && validate(pre.config).ok,
                  "width, prefill device armed by the plan: the graph is held to the device's bound",
                  std::to_string(pre.config.graph_width()) + (dp ? ": " + dp->reason : std::string()));
            check(pre.placement.compute_width == dev_bound,
                  "width, prefill device armed by the plan: the bounded width's buffers are the ones charged",
                  std::to_string(pre.placement.compute_width));
            const Plan host_only =
                plan_run(long_cfg, machine(150.0, Tri::Yes), mm, probed, PlanRequest{}, PlannerPolicy::defaults());
            check(!host_only.config.prefill.enabled() && host_only.config.graph_width() == 32768,
                  "width, no prefill device: the same graph stays as wide as the context",
                  std::to_string(host_only.config.graph_width()));
            // Named by the caller: bounded from the start.
            RunConfig named_dev = long_cfg;
            named_dev.prefill.device = "accel";
            PlanRequest pin_dev;
            pin_dev.pinned.push_back("prefill-device");
            const Plan by_caller =
                plan_run(named_dev, machine(1500.0, Tri::Yes), mm, probed, pin_dev, PlannerPolicy::defaults());
            check((uint32_t) by_caller.config.graph_width() == dev_bound,
                  "width, prefill device named by the caller: the graph is held to the device's bound",
                  std::to_string(by_caller.config.graph_width()));
            // And the ledger is the bounded graph's. The plan arms the device after the ledger is
            // sized, and the bound narrows the graph then: the buffers the wide graph would have
            // taken were still charged, on the host and again as the device's own, and the cache was
            // sized under them. A device the plan arms and the same device named by the caller run
            // the same graph, so they hold the same and leave the cache the same.
            for (const uint64_t per_token : {1 * KiB, 16 * KiB, 48 * KiB}) {
                Placement steep = probed;
                steep.host_compute_per_token = per_token;
                steep.raw_host_compute_bytes = 8192ull * per_token;
                const Plan armed =
                    plan_run(long_cfg, machine(1500.0, Tri::Yes), mm, steep, PlanRequest{}, PlannerPolicy::defaults());
                if (armed.config.prefill.device != "accel" || (uint32_t) armed.config.graph_width() != dev_bound)
                    continue; // at this slope the memory already held the graph narrower: nothing to compare
                // The caller's device with the scheme the plan armed: as many slots, in turn with the cache.
                RunConfig same_scheme = named_dev;
                same_scheme.prefill.slots = armed.config.prefill.slots;
                same_scheme.prefill.slots_on_demand = armed.config.prefill.slots_on_demand;
                PlanRequest pin_scheme = pin_dev;
                pin_scheme.pinned.push_back("prefill-slots");
                pin_scheme.pinned.push_back("prefill-slots-on-demand");
                const Plan named =
                    plan_run(same_scheme, machine(1500.0, Tri::Yes), mm, steep, pin_scheme, PlannerPolicy::defaults());
                const LedgerRow * fa = ledger_row(armed, LedgerKind::ContextCompute);
                const LedgerRow * fn = ledger_row(named, LedgerKind::ContextCompute);
                const LedgerRow * da = ledger_row(armed, LedgerKind::Device);
                const LedgerRow * dn2 = ledger_row(named, LedgerKind::Device);
                const uint64_t at_bound =
                    armed.placement.raw_host_context_bytes + armed.placement.raw_host_compute_bytes;
                const std::string at = " at " + std::to_string(per_token >> 10) + " KiB a token";
                check(fa && fa->bytes == at_bound,
                      "width, prefill device armed by the plan: the ledger charges the bounded graph's buffers" + at,
                      (fa ? std::to_string(fa->bytes >> 20) : std::string("no row")) + " MiB charged, " +
                          std::to_string(at_bound >> 20) + " at the bound");
                check(fa && fn && da && dn2 && fa->bytes == fn->bytes && da->bytes == dn2->bytes &&
                          armed.cache_budget_bytes == named.cache_budget_bytes,
                      "width, prefill device armed by the plan: the same ledger as the device named by the caller" + at,
                      std::to_string(armed.cache_budget_bytes >> 20) + " MiB of cache against " +
                          std::to_string(named.cache_budget_bytes >> 20));
            }
        }
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
        check(!held.config.prefill.slots_on_demand && ledger_row(held, LedgerKind::DeviceSlots) == nullptr &&
                  off.cache_budget_bytes - held.cache_budget_bytes == two_layers,
              "prefill: slots held for the session are charged whole, beside the cache");
        check(on.allocation.ledger.locked <= on.allocation.ledger.lockable_cap, "prefill: the ledger still closes");
        check(on.config.device_use != DeviceUse::CpuOnly, "prefill: an armed device is not taken out of the run");
        check(validate(on.config).ok, "prefill: the armed plan is a valid config", validate(on.config).error);
        check(on.to_flags().find("--prefill-device accel") != std::string::npos &&
                  on.to_flags().find("--prefill-best-effort") != std::string::npos &&
                  on.to_flags().find("--prefill-slots-on-demand") != std::string::npos,
              "prefill: the reproduce line carries it");

        // A device an earlier load of this model here did not keep - its check on the model refused it,
        // or the load never said - is not armed again; one it kept is.
        for (const int verdict : {-1, 0, 1}) {
            ModelProfile seen = mm;
            seen.knobs.set_prefill_word({"accel", verdict, verdict == 0 ? MeasuredKnobs::prefill_unconfirmed_max : 0});
            const Plan q = plan_run(base_cfg(), machine(1500.0, Tri::Yes), seen, PlanRequest{});
            const std::string why = reason(q);
            if (verdict > 0)
                check(q.config.prefill.device == "accel", "prefill: a device an earlier load kept is armed again");
            else
                check(!q.config.prefill.enabled() && why.find("not kept by an earlier load") != std::string::npos,
                      verdict < 0 ? "prefill: a device an earlier load handed back is not armed again"
                                  : "prefill: a device a load armed and never finished opening is not armed again",
                      why);
        }
        ModelProfile elsewhere = mm;
        elsewhere.knobs.set_prefill_word({"another", -1, 0});
        check(plan_run(base_cfg(), machine(1500.0, Tri::Yes), elsewhere, PlanRequest{}).config.prefill.device ==
                  "accel",
              "prefill: a verdict about another device leaves this one alone");

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
        const LedgerRow * one_row = ledger_row(one, LedgerKind::DeviceSlots);
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
        check(nofit.cache_budget_bytes == nodev.cache_budget_bytes && ledger_row(nofit, LedgerKind::Device) == nullptr,
              "prefill: and a refusal leaves the ledger as it was");

        // A cache that is off without the device - under one token cycle, so it would return no hits -
        // is not something the device takes away. Refusing the device for it costs the prompt its
        // faster prefill and saves decode nothing: measured against the run without the device, not
        // against a cache the run never had.
        ModelProfile wide = mm;
        wide.token_cycle_bytes = 64 * GiB; // more than any cache here: the run streams uncached
        wide.group(WeightGroup::Experts).bytes_per_token = wide.token_cycle_bytes;
        const Plan uncached = plan_run(base_cfg(), unified(), wide, PlanRequest{});
        const Plan uncached_on = plan_run(base_cfg(), machine(1500.0, Tri::Yes), wide, PlanRequest{});
        check(uncached.cache_budget_bytes == 0, "prefill: (a run whose cache is off without any device)",
              std::to_string((unsigned long long) (uncached.cache_budget_bytes >> 20)) + " MiB");
        check(uncached_on.config.prefill.device == "accel",
              "prefill: a cache already off is not a reason to refuse the device", reason(uncached_on));
        check(uncached_on.allocation.ledger.held <= uncached_on.allocation.ledger.holdable_cap,
              "prefill: and the ledger still closes with it");

        // A cache that streams without the device and falls under one token cycle with it: a trade
        // between every long prompt and every decoded token, priced on this machine's own requests.
        // The token cycle is set just under the cache the run has without the device, so the device's
        // row is what takes it under.
        {
            // A platform with no lock store, as on the phone this was measured on: the cache is then
            // the only thing the device's row takes.
            const auto unlocked_hw = [](HardwareProfile h) {
                h.lockable_bytes = 0;
                h.lock_in_place_bytes = 0;
                h.anon_overflow = Overflow::Kill;
                return h;
            };
            const HardwareProfile bare = unlocked_hw(unified()), with_dev = unlocked_hw(machine(1500.0, Tri::Yes));
            ModelProfile edge = mm;
            const auto with_cycle = [&](uint64_t cycle) {
                edge.token_cycle_bytes = cycle;
                edge.group(WeightGroup::Experts).bytes_per_token = cycle;
            };
            with_cycle(plan_run(base_cfg(), bare, mm, PlanRequest{}).cache_budget_bytes - 32 * MiB);
            with_cycle(plan_run(base_cfg(), bare, edge, PlanRequest{}).cache_budget_bytes - 32 * MiB);
            const Plan cached = plan_run(base_cfg(), bare, edge, PlanRequest{});
            const auto plan_mix = [&](const MeasuredKnobs & k) {
                ModelProfile e = edge;
                e.knobs = k;
                return plan_run(base_cfg(), with_dev, e, PlanRequest{});
            };
            check(cached.cache_budget_bytes >= edge.token_cycle_bytes,
                  "prefill trade: (the cache streams without the device)",
                  std::to_string((unsigned long long) (cached.cache_budget_bytes >> 20)));

            // (c) Nothing recorded of the requests: today's decision, the cache kept, and the plan says
            // the trade is unpriced and what would price it.
            const Plan unpriced = plan_mix(MeasuredKnobs{});
            const Decision * ud = find(unpriced, "prefill-device");
            check(!unpriced.config.prefill.enabled() && unpriced.cache_budget_bytes == cached.cache_budget_bytes &&
                      ud && ud->source == Source::Derived &&
                      ud->reason.find("refused on memory") != std::string::npos &&
                      ud->reason.find("unpriced: no request of this machine is recorded") != std::string::npos,
                  "prefill trade: with no request recorded the cache is kept, and the trade is said unpriced",
                  ud ? ud->reason : "no decision");
            MeasuredKnobs no_rate;
            no_rate.mix_requests = 3;
            no_rate.mix_wide_prompt_tokens = 1054;
            no_rate.mix_gen_tokens = 40;
            const Plan no_rate_p = plan_mix(no_rate);
            check(!no_rate_p.config.prefill.enabled() && no_rate_p.to_flags() == unpriced.to_flags() &&
                      reason(no_rate_p).find("host's prefill of a long prompt is not measured") != std::string::npos,
                  "prefill trade: requests without the host's prefill rate are no price either", reason(no_rate_p));

            // (a) Long prompts, short answers: the device is armed though the cache falls under its floor,
            // and the decision gives both costs.
            MeasuredKnobs chat;
            chat.mix_requests = 3;
            chat.mix_wide_prompt_tokens = 1054;
            chat.mix_gen_tokens = 40;
            chat.host_prefill_tps = 8.6;
            const Plan traded = plan_mix(chat);
            const Decision * td = find(traded, "prefill-device");
            check(traded.config.prefill.device == "accel" && td && td->source == Source::Measured &&
                      td->reason.find("with the device") != std::string::npos &&
                      td->reason.find("without it") != std::string::npos &&
                      td->reason.find("1054 prompt tokens and 40 decoded") != std::string::npos,
                  "prefill trade: long prompts and short answers arm the device, with both costs",
                  td ? td->reason : "no decision");
            check(traded.cache_budget_bytes == 0 && traded.config.moe.enabled && traded.config.moe.cache_mb == 0,
                  "prefill trade: the cache it falls under the floor of is off, and the experts still stream",
                  std::to_string((unsigned long long) (traded.cache_budget_bytes >> 20)));
            check(validate(traded.config).ok, "prefill trade: the armed plan is a valid config",
                  validate(traded.config).error);

            // (b) Long answers from short prompts: the cache wins, priced and said so; the flags are the
            // unpriced plan's.
            MeasuredKnobs gen = chat;
            gen.mix_wide_prompt_tokens = 0;
            gen.mix_gen_tokens = 2000;
            const Plan kept = plan_mix(gen);
            const Decision * kd = find(kept, "prefill-device");
            check(!kept.config.prefill.enabled() && kd && kd->source == Source::Measured &&
                      kd->reason.find("the cache is kept") != std::string::npos &&
                      kd->reason.find("without it") != std::string::npos,
                  "prefill trade: long answers from short prompts keep the cache, priced", kd ? kd->reason : "none");
            check(kept.to_flags() == unpriced.to_flags(), "prefill trade: a kept cache and an unpriced trade are the "
                                                          "same flags");
        }

        // The same run where not even the dense set fits what may be held: the device's row would be
        // new memory on a full pool, so it is refused - and the plan says that, not that a cache
        // which was never there needs the room.
        HardwareProfile tight = machine(1500.0, Tri::Yes);
        tight.residency_budget = 2 * GiB; // under the 3 GiB dense set
        const Plan full = plan_run(base_cfg(), tight, wide, PlanRequest{});
        const Decision * full_d = find(full, "prefill-device");
        check(!full.config.prefill.enabled() && full_d && full_d->brief.find("the cache needs") == std::string::npos &&
                  full_d->brief.find("may hold") != std::string::npos,
              "prefill: on a full pool the device is refused for the memory, not for a cache",
              full_d ? full_d->brief : "no decision");

        // A machine where nothing can be locked: a phone whose platform publishes no wire limit. The
        // device is still a row of what is held, and it is not refused for a lock store the run
        // could never use.
        HardwareProfile nolock = machine(1500.0, Tri::Yes);
        nolock.lockable_bytes = 0;
        nolock.lock_in_place_bytes = 0;
        nolock.anon_overflow = Overflow::Kill;
        const Plan unlocked = plan_run(base_cfg(), nolock, mm, PlanRequest{});
        const LedgerRow * ul_row = ledger_row(unlocked, LedgerKind::Device);
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
        check(own.config.prefill.device == "accel" && ledger_row(own, LedgerKind::Device) == nullptr &&
                  own.cache_budget_bytes == off.cache_budget_bytes,
              "prefill: a device with its own memory is armed and charged nothing here");
        // "Its own memory" with no size is not a pool to budget against: charged to this one.
        HardwareProfile sizeless = machine(1500.0, Tri::Yes);
        sizeless.devices[1].shares_host_memory = Tri::No;
        const Plan sz = plan_run(base_cfg(), sizeless, mm, PlanRequest{});
        const LedgerRow * szr = ledger_row(sz, LedgerKind::Device);
        check(sz.config.prefill.device == "accel" && szr && szr->bytes == stays &&
                  ledger_row(sz, LedgerKind::DeviceSlots),
              "prefill: a device that claims its own memory and states no size is charged to the host");

        // The fitter's compute buffers are per device: the prefill device pays its own part, and
        // none of one projected for a device beside it (a GPU's charged to an NPU, on a phone).
        Placement projected;
        projected.device_compute_bytes = 256 * MiB;
        projected.projected_devices = {"gpu"};
        projected.projected_compute_bytes = {256 * MiB};
        const Plan beside = plan_run(base_cfg(), machine(1500.0, Tri::Yes), mm, projected, PlanRequest{});
        const LedgerRow * orow = ledger_row(beside, LedgerKind::Device);
        check(beside.config.prefill.device == "accel" && orow && orow->bytes == stays,
              "prefill: a compute buffer projected for another device is not charged to this one",
              orow ? std::to_string((unsigned long long) (orow->bytes >> 20)) + " MiB" : "no row");
        projected.projected_devices = {"accel"};
        const Plan mine = plan_run(base_cfg(), machine(1500.0, Tri::Yes), mm, projected, PlanRequest{});
        const LedgerRow * mrow = ledger_row(mine, LedgerKind::Device);
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
        // A card that works beside one that failed its probe keeps what the fitter placed on it, and
        // the run names it, so the failed one is not handed back by llama.cpp's default list (#606).
        HardwareProfile mixed = desktop();
        ComputeDevice dead;
        dead.name = "dead";
        dead.memory_total = 11 * GiB;
        dead.memory_free = 11 * GiB;
        dead.shares_host_memory = Tri::No;
        dead.usable = Tri::No;
        dead.unusable_reason = "its probe ended the process that ran it (signal 6)";
        dead.probed_out_of_process = true;
        mixed.devices.push_back(dead);
        bool dropped = false;
        check(surviving_devices(mixed, {}, &dropped) == std::vector<std::string>{"discrete"} && dropped,
              "survivors: an empty list names the devices that survived");
        check(surviving_devices(mixed, {"discrete"}, &dropped) == std::vector<std::string>{"discrete"} && !dropped,
              "survivors: a list without the failed device is left as it is");
        check(surviving_devices(desktop(), {}, &dropped).empty() && !dropped,
              "survivors: with every device alive, an empty list stays llama.cpp's default");
        HardwareProfile all_dead = mixed;
        all_dead.devices.erase(all_dead.devices.begin());
        check(surviving_devices(all_dead, {}, &dropped).empty() && dropped,
              "survivors: with none alive, nothing is named and the drop is reported");
        Placement on_good;
        on_good.fitted = true;
        on_good.n_gpu_layers = (int32_t) mm.n_layer + 1;
        on_good.device_split = {{"discrete", mm.n_layer + 1}};
        on_good.outcome = "the capacity fitter found a placement that is projected to fit";
        const Plan beside_dead = plan_run(base_cfg(), mixed, mm, on_good, PlanRequest{}, PlannerPolicy::defaults());
        const Decision * kept = find(beside_dead, "devices");
        check(beside_dead.config.n_gpu_layers == on_good.n_gpu_layers &&
                  beside_dead.config.device_use != DeviceUse::CpuOnly &&
                  beside_dead.config.device_list == std::vector<std::string>{"discrete"},
              "isolated: the card that works keeps its layers and is the run's only device");
        check(kept && kept->value == "discrete" && kept->reason.find("signal 6") != std::string::npos,
              "isolated: and the plan says which device was left out and why",
              kept ? kept->value + ": " + kept->reason : "no decision");
        check(validate(beside_dead.config).ok, "isolated: that plan is a valid config",
              validate(beside_dead.config).error);
        check(beside_dead.to_flags().find("dead") == std::string::npos,
              "isolated: the reproduce line does not name the failed device", beside_dead.to_flags());
        // The same when the host half streams: the rule is asked on that path as well.
        Placement half;
        half.fitted = true;
        half.outcome = "synthetic";
        half.n_gpu_layers = 24;
        half.n_ctx = 4096;
        for (uint32_t il = 0; il < 24; ++il)
            half.host_expert_layers.push_back(il);
        half.host_resident_bytes = 4 * GiB;
        half.device_bytes = 8 * GiB;
        half.device_split = {{"discrete", 24}};
        const Plan streamed = plan_run(base_cfg(), mixed, model, half, PlanRequest{}, PlannerPolicy::defaults());
        check(streamed.config.moe.enabled && streamed.config.n_gpu_layers == 24 &&
                  streamed.config.device_list == std::vector<std::string>{"discrete"},
              "isolated, streamed: the card that works keeps its layers and is the only device listed",
              streamed.to_flags());

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

        // No trace: the cache is credited the prior in token cycles (prior_hit_curve), ranked and priced
        // by the same path as a record's curve, and the plan says the figure is a prior.
        {
            const Plan p = plan_run(base_cfg(), measured_machine(), model, PlanRequest{});
            const GroupPlacement & e = p.allocation.at(WeightGroup::Experts);
            const HitCurve prior = prior_hit_curve(model.expert_bytes, model.token_cycle_bytes);
            check(p.allocation.cache_bytes == 9147777024ull,
                  "no trace: every byte the ranking leaves goes to the cache, as before the prior",
                  std::to_string(p.allocation.cache_bytes));
            const double expect = prior.hit_share(p.allocation.cache_bytes);
            check(expect > 0.473307 && std::fabs(p.allocation.cache_hit_credited - expect) < 1e-12,
                  "no trace: credited the prior at the cache's size, above the share of the set it holds",
                  std::to_string(p.allocation.cache_hit_credited) + " vs " + std::to_string(expect));
            check(p.allocation.seconds_per_token < 0.160544713,
                  "no trace: more hits credited, a cheaper token than the linear credit priced",
                  std::to_string(p.allocation.seconds_per_token));
            check(e.reason.find("[prior]") != std::string::npos && e.reason.find("token cycles") != std::string::npos,
                  "no trace: the experts' rationale says the hit rate is the prior", e.reason);
            const Decision * ch = find(p, "cache-hits");
            check(ch && ch->source == Source::Policy && ch->reason.find("no routing record") != std::string::npos,
                  "no trace: the plan says its hits are a prior until a record exists", ch ? ch->reason : "none");
            check(!model.hit_curve.credited(), "no trace: the prior is the plan's, never written into the model");

            // A trace that is not this model's is the same plan, plus the one line saying why.
            ModelProfile rejected = model;
            rejected.hit_curve.rejected = "the trace was recorded at top-6 and this run routes top-8";
            const Plan r = plan_run(base_cfg(), measured_machine(), rejected, PlanRequest{});
            std::string without;
            std::string reasons;
            for (const Decision & d : r.decisions)
                if (d.knob != "cache-hits") without += d.knob + "=" + d.value + ":" + d.reason + "|" + d.brief + "\n";
            for (const Decision & d : p.decisions)
                if (d.knob != "cache-hits") reasons += d.knob + "=" + d.value + ":" + d.reason + "|" + d.brief + "\n";
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
            const Plan pri = plan_run(base_cfg(), measured_machine(), model, PlanRequest{});
            check(pri.allocation.cache_hit_credited != p.allocation.cache_hit_credited,
                  "with a curve: the model's own record replaces the prior");
        }

        // ── the decode's own cost, measured on an earlier load (MeasuredKnobs::decode_*) ──────────
        {
            const HardwareProfile hw = measured_machine();
            const Plan base = plan_run(base_cfg(), hw, model, PlanRequest{});
            const double compute = host_compute_seconds(hw, model);
            auto account = [](const Plan & p) {
                std::string out = p.to_flags() + "\n";
                for (const Decision & d : p.decisions)
                    out += d.knob + "=" + d.value + ":" + d.reason + "|" + d.brief + "\n";
                char nums[96];
                std::snprintf(nums, sizeof(nums), "%.17g %.17g %llu", p.allocation.seconds_per_token,
                              p.allocation.cache_hit_credited, (unsigned long long) p.allocation.cache_bytes);
                return out + nums;
            };
            ModelProfile rec = model;
            rec.knobs.hardware = "0123456789abcdef";
            rec.knobs.engine = "test";
            check(compute > 0.0 && base.config.moe.enabled && !find(base, "decode-cost"),
                  "decode cost: the fixture streams and prices its compute, and says nothing of a measurement");
            check(account(plan_run(base_cfg(), hw, rec, PlanRequest{})) == account(base),
                  "decode cost: a record that measured no decode leaves the plan byte for byte as it is");

            // A decode that computed three times what the matmul rates price: every group's compute
            // moves together, so the token's compute part triples and nothing the ranking weighs moves.
            ModelProfile slow = rec;
            slow.knobs.decode_compute_s = 3.0 * compute;
            slow.knobs.decode_threads = base.config.n_threads;
            const Plan q = plan_run(base_cfg(), hw, slow, PlanRequest{});
            const Decision * d = find(q, "decode-cost");
            check(d && d->source == Source::Measured && d->value == "compute x3.00" &&
                      d->brief.rfind("Decode priced from a load here: compute", 0) == 0 && d->brief.size() <= 120,
                  "decode cost: the measured compute is the plan's, and it says so",
                  d ? d->value + " / " + d->brief : "");
            const double grew = q.allocation.seconds_per_token - base.allocation.seconds_per_token;
            check(std::fabs(grew - 2.0 * compute) < 1e-9 * q.allocation.seconds_per_token,
                  "decode cost: the predicted token's compute part is the measured one, three times the model's",
                  std::to_string(grew) + " added against " + std::to_string(2.0 * compute));
            check(q.allocation.cache_bytes == base.allocation.cache_bytes && q.to_flags() == base.to_flags(),
                  "decode cost: a compute that scales every group alike changes no knob", q.to_flags());

            // Measured at another thread count, or under another placement: not this plan's figure.
            ModelProfile other = slow;
            other.knobs.decode_threads = base.config.n_threads + 1;
            const Plan o = plan_run(base_cfg(), hw, other, PlanRequest{});
            const Decision * od = find(o, "decode-cost");
            check(o.allocation.seconds_per_token == base.allocation.seconds_per_token && od &&
                      od->source == Source::Derived && od->value == "probed",
                  "decode cost: a figure measured at another thread count is not used, and the plan says why");
            ModelProfile placed = slow;
            placed.knobs.decode_gpu_layers = 12;
            const Plan pl = plan_run(base_cfg(), hw, placed, PlanRequest{});
            check(pl.allocation.seconds_per_token == base.allocation.seconds_per_token && find(pl, "decode-cost") &&
                      find(pl, "decode-cost")->source == Source::Derived,
                  "decode cost: a figure measured with layers on a device does not answer for a host plan");

            // Hits: the prior is scaled to what was measured at the load's budget, both ways.
            ModelProfile hits = rec;
            hits.knobs.decode_hit = 0.5 * base.allocation.cache_hit_credited;
            hits.knobs.decode_cache_mib = (double) base.allocation.cache_bytes / (double) MiB;
            const Plan h = plan_run(base_cfg(), hw, hits, PlanRequest{});
            const double expect_h =
                0.5 * prior_hit_curve(model.expert_bytes, model.token_cycle_bytes).hit_share(h.allocation.cache_bytes);
            check(h.allocation.cache_bytes > 0 && std::fabs(h.allocation.cache_hit_credited - expect_h) < 1e-9 &&
                      h.allocation.seconds_per_token > base.allocation.seconds_per_token && find(h, "decode-cost") &&
                      find(h, "decode-cost")->source == Source::Measured,
                  "decode cost: the prior moves to the hits measured at that budget",
                  std::to_string(h.allocation.cache_hit_credited) + " vs " + std::to_string(expect_h));

            // With the model's own curve: lowered toward the measurement, never raised past the replay.
            ModelProfile curved = steep;
            curved.knobs = rec.knobs;
            const Plan c0 = plan_run(base_cfg(), hw, curved, PlanRequest{});
            const double replay = steep.hit_curve.hit_share(c0.allocation.cache_bytes);
            curved.knobs.decode_cache_mib = (double) c0.allocation.cache_bytes / (double) MiB;
            curved.knobs.decode_hit = 0.8 * replay;
            const Plan c1 = plan_run(base_cfg(), hw, curved, PlanRequest{});
            check(c1.allocation.cache_bytes > 0 &&
                      std::fabs(c1.allocation.cache_hit_credited -
                                0.8 * steep.hit_curve.hit_share(c1.allocation.cache_bytes)) < 1e-9 &&
                      c1.allocation.seconds_per_token > c0.allocation.seconds_per_token,
                  "decode cost: the curve is credited at the ratio measured at the load's budget, at every size");
            curved.knobs.decode_hit = std::min(1.0, replay + 0.1);
            const Plan c2 = plan_run(base_cfg(), hw, curved, PlanRequest{});
            check(c2.allocation.cache_hit_credited == c0.allocation.cache_hit_credited &&
                      c2.allocation.seconds_per_token == c0.allocation.seconds_per_token,
                  "decode cost: hits above the replay credit nothing past it");
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

        // The sweep's own rungs: a count above the incumbent that it measured short of the margin is
        // answered and not tried again under a decode; a count below it is still tried, and a count
        // the sweep never asked is tried as before.
        {
            HardwareProfile swept = het;
            swept.thread_rates = {{1, 20.0}, {2, 38.0}, {4, 70.0}, {8, 100.0}, {10, 104.0}};
            const Plan p = plan_run(base_cfg(), swept, model, on);
            check(p.config.trial.threads == std::vector<int>({4}),
                  "sweep answered: 10 (4% over 8, alone) is not tried; 4, below the incumbent, is",
                  std::to_string(p.config.trial.threads.size()));
            const Decision * t = find(p, "trial-threads");
            check(t && t->value == "4" && t->reason.find("10 is not tried") != std::string::npos,
                  "sweep answered: the trial's note says which count it skips and why", t ? t->reason : "none");

            // Past the margin alone, the sweep did not answer it: a decode decides.
            swept.thread_rates.back().gibs = 115.0;
            const Plan q = plan_run(base_cfg(), swept, model, on);
            check(q.config.trial.threads == std::vector<int>({10, 4}),
                  "sweep answered: a count the sweep measured past the margin is still tried");

            // Nothing left to try: the plan says the sweep answered, and arms no thread trial.
            HardwareProfile alike = desktop();
            alike.devices.clear();
            alike.n_cores = 8;
            alike.core_classes = {8};
            alike.best_threads = 4;
            alike.thread_rates = {{1, 20.0}, {2, 38.0}, {4, 70.0}, {8, 60.0}};
            const Plan r = plan_run(base_cfg(), alike, model, on);
            const Decision * n = find(r, "trial-threads");
            check(r.config.trial.threads.empty() && n && n->value == "none" && n->source == Source::Measured,
                  "sweep answered: with every candidate answered no thread trial is armed, and the plan says so",
                  n ? n->value : "none");
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

            // A front-end's default overlap with layers on a device and no cache there: it would take the
            // offload of the host's weights in a prompt's wide matmuls away (offloads_wide_matmuls), so the
            // plan turns it off and says why. Measured 2.1x to 4.5x of the prefill on a discrete GPU (#645).
            pl.device_split = {{"discrete", 24}};
            RunConfig srv = base_cfg();
            srv.moe.overlap = true;
            const Plan d = plan_run(srv, dev, model, pl, PlanRequest{});
            const Decision * od = find(d, "overlap");
            check(d.config.n_gpu_layers == 24 && d.config.moe.device_cache_mb == 0 && !d.config.moe.overlap &&
                      offloads_wide_matmuls(d.config),
                  "layers on a device, a front-end's default overlap: off, and the prompts' wide matmuls offload");
            check(od && od->value == "off" && od->source == Source::Derived &&
                      od->brief == "Off: with layers on discrete, overlap keeps prompts on the host." &&
                      od->reason.find("pass --overlap") == std::string::npos &&
                      od->reason.find("Pass --overlap to keep it") != std::string::npos,
                  "layers on a device, a front-end's default overlap: the plan says why", od ? od->reason : "none");
            check(validate(d.config).ok, "layers on a device, default overlap off: a valid config",
                  validate(d.config).error);

            // The caller who pinned it keeps it, and the plan line says what it costs them.
            PlanRequest pin;
            pin.pinned = {"overlap"};
            HardwareProfile wide = dev;
            wide.host_wide_matmul_gibs = 20.0;
            wide.devices[0].wide_matmul_gibs = 64.0;
            const Plan k = plan_run(srv, wide, model, pl, pin);
            const Decision * kd = find(k, "overlap");
            check(k.config.n_gpu_layers == 24 && k.config.moe.overlap && !offloads_wide_matmuls(k.config),
                  "layers on a device, overlap pinned: kept, and the prompts stay on the cores");
            check(kd && kd->value == "on" && kd->source == Source::Operator &&
                      kd->brief == "On, as you set it: prompts run on the host's cores, not on discrete." &&
                      kd->reason.find("measured 3.2x the host's") != std::string::npos &&
                      kd->reason.find("pass --no-overlap") != std::string::npos,
                  "layers on a device, overlap pinned: the plan says what it costs", kd ? kd->reason : "none");
            check(validate(k.config).ok, "layers on a device, overlap pinned: a valid config",
                  validate(k.config).error);

            // Nothing placed: the default is not this rule's to touch.
            const Plan h = plan_run(srv, het, model, PlanRequest{});
            check(h.config.n_gpu_layers == 0 && h.config.moe.overlap,
                  "every layer on the host: a front-end's default overlap stands");
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

    // ── a model held in memory, on a device that reads that same memory: decided by the first load ─
    // The fitter's placement on such a device is set aside (one pool, counted twice), so what is left
    // is which engine serves this machine's requests faster. No probe of a part of a model says that,
    // so until a load has timed both sides the plan opens the device and asks the load to time the
    // model on it and on the cores, as it compares their answers, and to keep the faster on a request
    // (device_answer_test has the load's rule). Once both rates are measured the plan reads them.
    {
        HardwareProfile roomy = unified();
        roomy.residency_budget = 64 * GiB;
        roomy.memory_total = 64 * GiB;
        roomy.host_wide_matmul_gibs = 1000.0;
        roomy.host_expert_gibs = roomy.host_dense_gibs = 100.0; // level with the device one token wide
        for (ComputeDevice & d : roomy.devices) {
            if (d.is_cpu) continue;
            d.usable = Tri::Yes;
            d.identity_ok = Tri::Yes;
            d.runs_expert_op = Tri::Yes;
            d.needs_repack = Tri::No;
            d.wide_identity_ok = Tri::Yes;
            d.wide_matmul_gibs = 900.0; // no faster on a prompt than the host
            d.expert_gibs = d.dense_gibs = 100.0;
        }
        Placement shared;
        shared.shared_memory_placement = true;
        shared.device_bytes = 20 * GiB;
        shared.device_compute_bytes = 400 * MiB;
        shared.projected_devices = {"accel"};
        shared.projected_compute_bytes = {400 * MiB};
        const auto plan_with = [&](double on, double off, const HardwareProfile & h = HardwareProfile{},
                                   const PlanRequest & r = PlanRequest{}) {
            ModelProfile m = model;
            m.knobs.shared_device_tps = on;
            m.knobs.no_shared_device_tps = off;
            return plan_run(base_cfg(), h.devices.empty() ? roomy : h, m, shared, r, PlannerPolicy::defaults());
        };
        const auto on_device = [&](const Plan & p) {
            return p.config.n_gpu_layers == (int) model.n_layer + 1 && p.config.device_split.size() == 1 &&
                   p.config.device_split[0].first == "accel" &&
                   p.config.device_split[0].second == p.config.n_gpu_layers;
        };
        const auto on_cores = [](const Plan & p) {
            return p.config.n_gpu_layers == 0 && p.config.device_split.empty();
        };

        // The same on a machine whose requests are answers with no long prompt (what its loads recorded).
        const auto plan_chat = [&](double on, double off, const HardwareProfile & h) {
            ModelProfile m = model;
            m.knobs.shared_device_tps = on;
            m.knobs.no_shared_device_tps = off;
            m.knobs.mix_requests = 4000;
            m.knobs.mix_gen_tokens = 400;
            return plan_run(base_cfg(), h, m, shared, PlanRequest{}, PlannerPolicy::defaults());
        };

        // Nothing measured, and probes that read the two sides level: the load decides, on the model.
        const PlannerPolicy defaults = PlannerPolicy::defaults();
        const float default_mix = defaults.mix_prompt_tokens_default / defaults.mix_gen_tokens_default;
        const auto asks_the_load = [&](const Plan & p) {
            return on_device(p) && p.config.trial.answer_device == "accel" &&
                   p.config.trial.answer_prompt_per_token >= 0.0f && p.config.trial.shared_device_side == 1;
        };
        const Plan first = plan_with(0.0, 0.0);
        check(first.regime == Regime::Fits, "shared device: the model is held in memory");
        check(asks_the_load(first),
              "shared device, no side timed: the device is opened and the load times both sides and keeps one");
        check(std::fabs(first.config.trial.answer_prompt_per_token - default_mix) < 1e-3f,
              "shared device, no side timed: the load weighs a prompt as the default request does",
              std::to_string(first.config.trial.answer_prompt_per_token));
        const Decision * d0 = find(first, "shared-device");
        check(d0 && d0->value == "accel" && d0->source == Source::Derived &&
                  d0->reason.find("the default mix") != std::string::npos &&
                  d0->reason.find("no probe of a part of a model") != std::string::npos,
              "shared device: the line says the load decides, and on which request", d0 ? d0->reason : "no line");
        check(validate(first.config).ok, "shared device, the load decides: a valid config",
              validate(first.config).error);
        // One side measured is still no comparison: the load that follows times both.
        check(asks_the_load(plan_with(0.0, 40.0)) && asks_the_load(plan_with(40.0, 0.0)),
              "shared device, one side timed: the load is still asked for both");
        // Probes that read the cores far ahead decide nothing either: they are a part of a model's rate.
        {
            HardwareProfile cores_ahead = roomy;
            cores_ahead.host_expert_gibs = cores_ahead.host_dense_gibs = 143.0;
            for (ComputeDevice & d : cores_ahead.devices)
                if (!d.is_cpu) d.expert_gibs = d.dense_gibs = 82.0;
            check(asks_the_load(plan_with(0.0, 0.0, cores_ahead)),
                  "shared device, probes read the cores 74% ahead: the model's own timing still decides");
        }

        // A device that computes a prompt 2.5x faster, for the plans made once both sides are measured.
        HardwareProfile prompts = roomy;
        for (ComputeDevice & d : prompts.devices)
            if (!d.is_cpu) d.wide_matmul_gibs = 2500.0;
        const Plan moved = plan_with(0.0, 0.0, prompts);
        const Decision * df = find(moved, "shared-device");
        check(asks_the_load(moved) && df && df->value == "accel" &&
                  df->reason.find("copied into the device's own buffers") != std::string::npos,
              "shared device: a line naming the device and how its weights are held", df ? df->reason : "");
        check(validate(moved.config).ok, "shared device, moved: a valid config", validate(moved.config).error);
        // A machine that only decodes asks the load to weigh the decode alone.
        {
            const Plan chat = plan_chat(0.0, 0.0, prompts);
            check(asks_the_load(chat) && chat.config.trial.answer_prompt_per_token < default_mix / 4.0f,
                  "shared device, a machine that sends no long prompt: the load weighs little but the decode",
                  std::to_string(chat.config.trial.answer_prompt_per_token));
        }

        // Both decodes measured: their ratio is the decode's, whatever the probe read.
        const Plan faster = plan_chat(54.0, 40.0, roomy);
        df = find(faster, "shared-device");
        check(on_device(faster) && faster.config.trial.shared_device_side == -1 && df && df->source == Source::Measured,
              "shared device, measured faster on the decode this machine serves: every layer moves");
        // The rule is the load's: the device keeps the model unless the cores are faster by the margin,
        // so a plan made on the load's measurements does not undo what the load decided.
        check(on_device(plan_chat(41.0, 40.0, roomy)) && on_device(plan_chat(38.0, 40.0, roomy)),
              "shared device, level on the decode: the device keeps it, as the load that timed them did");
        check(on_cores(plan_chat(30.0, 40.0, roomy)), "shared device, slower: the cores keep it");
        check(on_device(plan_with(41.0, 40.0, prompts)),
              "shared device, level on the decode and faster on a prompt: the device takes it");
        check(on_cores(plan_chat(30.0, 40.0, prompts)),
              "shared device, slower on the decode of a machine that only decodes: a faster prompt buys nothing");
        // A decode 35% faster against a prompt 10% slower, on the default request: the cores are not
        // faster by the margin, so the device keeps it.
        check(on_device(plan_with(54.0, 40.0)),
              "shared device, the cores not ahead by the margin on the whole request: the device keeps it");

        // Every move of the whole model onto the device carries the cores behind it (back_with_cores),
        // asked to compare answers or not: a device can refuse the allocation all the same, and the load
        // then serves from the cores instead of failing. A model left on the cores carries nothing.
        {
            const auto cores_behind = [&](const Plan & p) {
                const RunConfig * f = p.config.fallback.get();
                return f && f->n_gpu_layers == 0 && f->device_split.empty() && f->device_list.empty() &&
                       f->device_use == DeviceUse::CpuOnly && f->trial.answer_device.empty() && !f->fallback &&
                       f->model_path == p.config.model_path;
            };
            check(cores_behind(first) && first.config.fallback->trial.shared_device_side == 0,
                  "shared device, the load decides: the cores behind the device, timed as the cores' side");
            check(cores_behind(faster) && faster.config.fallback->trial.shared_device_side == -1,
                  "shared device, measured faster: the cores behind the device all the same");
            check(validate(faster.config).ok, "shared device, the cores behind it: a valid config",
                  validate(faster.config).error);
            check(!plan_chat(30.0, 40.0, roomy).config.fallback, "shared device, on the cores: nothing behind them");
        }

        // The probes let the device through; what this model answers there is compared by the load that
        // moves it, once, and the record's word then decides.
        check(moved.config.trial.answer_device == "accel" && df &&
                  df->reason.find("compares its answers") != std::string::npos,
              "shared device, moved with no word on its answers: the load is asked to compare them",
              df ? df->reason : "");
        {
            const Plan cores = plan_chat(30.0, 40.0, roomy);
            check(on_cores(cores) && cores.config.trial.answer_device.empty() &&
                      cores.config.trial.answer_prompt_per_token < 0.0f,
                  "shared device, measured on the cores: nothing to compare, nothing to weigh");
        }
        {
            ModelProfile said = model;
            said.knobs.shared_device_tps = 41.0; // both sides timed, by the load that confirmed the answers
            said.knobs.no_shared_device_tps = 40.0;
            said.knobs.set_device_answer("accel", 1);
            const Plan ok = plan_run(base_cfg(), prompts, said, shared, PlanRequest{}, PlannerPolicy::defaults());
            const Decision * dk = find(ok, "shared-device");
            check(on_device(ok) && ok.config.trial.answer_device.empty() && dk &&
                      dk->reason.find("matched the cores'") != std::string::npos,
                  "shared device, answers confirmed here: the device, and nothing compared again",
                  dk ? dk->reason : "");
            check(validate(ok.config).ok, "shared device, confirmed: a valid config", validate(ok.config).error);

            said.knobs.set_device_answer("accel", -1);
            const Plan no = plan_run(base_cfg(), prompts, said, shared, PlanRequest{}, PlannerPolicy::defaults());
            const Decision * dn = find(no, "shared-device");
            check(on_cores(no) && no.config.trial.answer_device.empty() && no.config.trial.shared_device_side == -1,
                  "shared device, answers differed here: the cores, nothing compared, no side timed");
            check(dn && dn->value == "cpu" && dn->source == Source::Measured &&
                      dn->reason.find("differed from the cores' on this machine") != std::string::npos,
                  "shared device, answers differed: a measured line that says so", dn ? dn->reason : "no line");
            // Another device's word is not this one's.
            said.knobs.device_answers.clear();
            said.knobs.set_device_answer("other", -1);
            const Plan el = plan_run(base_cfg(), prompts, said, shared, PlanRequest{}, PlannerPolicy::defaults());
            check(on_device(el) && el.config.trial.answer_device == "accel",
                  "shared device, another device's answers differed: this one moves, and is compared");

            PlanRequest held;
            held.pinned = {"gpu-layers"};
            const Plan pin = plan_run(base_cfg(), prompts, model, shared, held, PlannerPolicy::defaults());
            check(pin.config.trial.answer_device.empty() && !find(pin, "shared-device"),
                  "shared device, the placement pinned by a person: nothing moved, nothing compared");
        }

        ModelProfile failed = model;
        failed.knobs.shared_device_unviable = MeasuredKnobs::unviable_max;
        const Plan f = plan_run(base_cfg(), prompts, failed, shared, PlanRequest{}, PlannerPolicy::defaults());
        check(on_cores(f) && f.config.trial.shared_device_side == -1,
              "shared device that failed to load the model: the cores, whatever its price");

        // The failure keeps what the device reported free then. It is named again only once it reports
        // more than that by the margin, with the cores behind it; the same room, or a little more, and
        // the cores keep the model with no load paying for a second failure.
        {
            ModelProfile at = failed;
            at.knobs.shared_device_unviable_free_mib = 7941;
            const auto with_free = [&](uint64_t free, PlannerPolicy pol = PlannerPolicy::defaults()) {
                HardwareProfile h = prompts;
                for (ComputeDevice & d : h.devices)
                    if (!d.is_cpu) d.memory_free = free;
                return plan_run(base_cfg(), h, at, shared, PlanRequest{}, pol);
            };
            const Plan same = with_free(7941 * MiB);
            const Decision * ds = find(same, "shared-device");
            check(on_cores(same) && ds &&
                      ds->reason.find("7941 MiB free on it then and 7941 MiB now") != std::string::npos,
                  "shared device failed at this room, the same room now: the cores, and the line says both",
                  ds ? ds->reason : "no line");
            check(on_cores(with_free(8300 * MiB)),
                  "shared device failed, a little more room now (inside the margin): the cores");
            const Plan grown = with_free(12 * GiB);
            check(on_device(grown) && grown.config.fallback && grown.config.fallback->n_gpu_layers == 0,
                  "shared device failed, more room now past the margin: named again, the cores behind it");
            check(on_cores(with_free(0)), "shared device failed, no room reported now: the cores");
            // The regrowth threshold is its own policy field, not the speed margin: moving one leaves
            // the other's answer alone.
            PlannerPolicy tight = PlannerPolicy::defaults();
            tight.shared_device_regrowth = 0.01f;
            check(on_device(with_free(8300 * MiB, tight)),
                  "shared device failed, a little more room now past a smaller regrowth: named again");
            PlannerPolicy wide_win = PlannerPolicy::defaults();
            wide_win.min_backend_win = 1.0f;
            check(on_device(with_free(12 * GiB, wide_win)),
                  "shared device failed, more room now: a wider speed margin does not hold it back");
        }

        HardwareProfile wraps = roomy;
        for (ComputeDevice & d : wraps.devices)
            if (!d.is_cpu) d.host_ptr_buffers = d.host_ptr_verified = Tri::Yes;
        const Plan in_place = plan_chat(54.0, 40.0, wraps);
        const Decision * dw = find(in_place, "shared-device");
        check(dw && dw->reason.find("stay the file's mapping") != std::string::npos,
              "shared device that reads our memory in place: the line says the weights stay mapped",
              dw ? dw->reason : "no line");

        HardwareProfile wrong = roomy;
        for (ComputeDevice & d : wrong.devices)
            if (!d.is_cpu) d.identity_ok = Tri::No;
        const Plan w = plan_with(54.0, 40.0, wrong);
        const Decision * dr = find(w, "shared-device");
        check(on_cores(w) && w.config.trial.shared_device_side == -1 && dr && dr->source == Source::Derived,
              "shared device that has not computed the host's results: never timed, never moved");

        HardwareProfile wide_wrong = roomy;
        for (ComputeDevice & d : wide_wrong.devices)
            if (!d.is_cpu) d.wide_identity_ok = Tri::No;
        const Plan ww = plan_with(54.0, 40.0, wide_wrong);
        check(on_cores(ww) && ww.config.trial.shared_device_side == -1,
              "shared device right one token wide and wrong on a prompt: never timed, never moved (the prompt "
              "would run there too)");

        Placement unpriced = shared;
        unpriced.projected_devices.clear();
        unpriced.projected_compute_bytes.clear();
        const Plan up = plan_run(base_cfg(), roomy, model, unpriced, PlanRequest{}, PlannerPolicy::defaults());
        check(on_cores(up) && up.config.trial.shared_device_side == -1,
              "shared device with no projected compute buffer: not timed, not moved");

        // The model fits and the room this machine asks beside it does not: the plan streams it, and
        // the device, which holds it whole inside what that plan may hold, is offered it with the
        // stream behind (the block below has the rest of that rule).
        HardwareProfile tight = roomy;
        tight.residency_budget = 25 * GiB;
        {
            const Plan measured = plan_with(54.0, 40.0, tight), unmeasured = plan_with(0.0, 0.0, tight);
            check(on_device(measured) && measured.config.fallback && measured.config.fallback->moe.enabled &&
                      on_device(unmeasured) && unmeasured.config.fallback &&
                      unmeasured.config.trial.answer_device == "accel",
                  "shared device without the room residency asks: offered the model, the stream behind it");
        }

        PlanRequest pinned;
        pinned.pinned = {"devices"};
        const Plan pd = plan_with(54.0, 40.0, roomy, pinned);
        check(on_cores(pd) && !find(pd, "shared-device"), "shared device, devices pinned: left alone");

        // ── a model the plan streams, where the device could hold it whole ──────────────────────
        // The shape of a laptop with integrated graphics: the model does not fit beside the room this
        // machine asks for, so the plan streams it with a cache nearly as large as its experts. Held
        // whole on the device it takes no more of the pool than that plan holds, so the first load is
        // asked, and the streaming plan is what the session reopens with where the device does not
        // keep the model.
        {
            const auto streams = [&](const Plan & p) {
                return p.config.moe.enabled && p.config.n_gpu_layers == 0 && !p.config.fallback;
            };
            HardwareProfile laptop = roomy;
            laptop.memory_total = 31 * GiB;
            // No lock ceiling short of the memory itself: the cache is bounded by what may be held.
            laptop.lockable_bytes = laptop.lock_in_place_bytes = laptop.reclaim_exempt_max = 31 * GiB;
            laptop.host_expert_gibs = laptop.host_dense_gibs = 143.0; // the probes read the cores far ahead
            for (ComputeDevice & d : laptop.devices)
                if (!d.is_cpu) d.expert_gibs = d.dense_gibs = 82.0;
            uint64_t budget = 0; // the least this process may hold at which the plan asks the load
            for (uint64_t b = 12 * GiB; b <= 40 * GiB && budget == 0; b += 256 * MiB) {
                laptop.residency_budget = b;
                const Plan q = plan_run(base_cfg(), laptop, model, shared, PlanRequest{}, defaults);
                if (q.config.fallback) budget = b;
            }
            laptop.residency_budget = budget;
            const Plan held = plan_run(base_cfg(), laptop, model, shared, PlanRequest{}, defaults);
            const Decision * dh = find(held, "held-whole");
            check(budget > 0 && on_device(held) && !held.config.moe.enabled && held.config.fallback &&
                      held.config.trial.answer_device == "accel" && held.config.trial.answer_prompt_per_token >= 0.0f,
                  "streamed, could be held whole on the device: the device is opened and the load decides",
                  std::to_string(budget >> 20) + " MiB");
            check(held.config.fallback && held.config.fallback->moe.enabled &&
                      held.config.fallback->n_gpu_layers == 0 && held.config.fallback->moe.cache_mb > 0 &&
                      !held.config.fallback->fallback && held.config.fallback->trial.answer_device.empty(),
                  "streamed, could be held whole: the fallback is the streaming plan, whole and by itself");
            check(validate(held.config).ok, "streamed, could be held whole: a valid config, fallback included",
                  validate(held.config).error);
            check(dh && dh->value == "accel" && dh->reason.find("reopens as the streaming plan") != std::string::npos,
                  "streamed, could be held whole: the plan says both parts", dh ? dh->reason : "no line");
            // The room is the streaming plan's own: what it holds of this memory, and no more.
            {
                HardwareProfile alone = laptop;
                alone.devices.resize(1); // no device: the same machine plans the stream
                const Plan stream = plan_run(base_cfg(), alone, model, Placement{}, PlanRequest{}, defaults);
                check(streams(stream) && stream.regime == Regime::ExpertsStream,
                      "streamed, could be held whole: without the device this machine streams the model");
                const uint64_t whole = model.file_bytes + shared.device_compute_bytes;
                check(stream.allocation.ledger.holdable_cap > 0 && whole <= stream.allocation.ledger.holdable_cap,
                      "streamed, could be held whole: whole on the device stays inside what the stream may hold",
                      std::to_string(whole >> 20) + " MiB against " +
                          std::to_string(stream.allocation.ledger.holdable_cap >> 20));
            }
            // Less memory than that: the stream, as before, with nothing asked of the load.
            HardwareProfile small = laptop;
            small.residency_budget = budget - 4 * GiB;
            check(streams(plan_run(base_cfg(), small, model, shared, PlanRequest{}, defaults)),
                  "streamed, the whole model past what the stream holds: streamed, and nothing else");
            // What the first load measured then decides, by the load's own rule.
            ModelProfile timed = model;
            timed.knobs.set_device_answer("accel", 1);
            timed.knobs.shared_device_tps = 40.0;
            timed.knobs.no_shared_device_tps = 18.0;
            const Plan kept = plan_run(base_cfg(), laptop, timed, shared, PlanRequest{}, defaults);
            check(on_device(kept) && kept.config.trial.answer_device.empty() && kept.config.fallback,
                  "streamed, the device measured faster: held whole on it at once, the stream still behind it");
            timed.knobs.shared_device_tps = 12.0;
            timed.knobs.no_shared_device_tps = 18.0;
            check(streams(plan_run(base_cfg(), laptop, timed, shared, PlanRequest{}, defaults)),
                  "streamed, the cores measured faster than the device: the stream, not the model whole on the cores");
            // A device that lost this model here, by its answers or by failing to open, is not asked.
            ModelProfile refused = model;
            refused.knobs.set_device_answer("accel", -1);
            check(streams(plan_run(base_cfg(), laptop, refused, shared, PlanRequest{}, defaults)),
                  "streamed, the device's answers differed here: the stream");
            ModelProfile failed = model;
            failed.knobs.shared_device_unviable = MeasuredKnobs::unviable_max;
            check(streams(plan_run(base_cfg(), laptop, failed, shared, PlanRequest{}, defaults)),
                  "streamed, the device could not open the model here: the stream");
            // A caller who asked for the stream, or a device that has not shown the cores' results, keeps it.
            PlanRequest asked;
            asked.pinned = {"moe-stream"};
            RunConfig streamed_cfg = base_cfg();
            streamed_cfg.moe.enabled = true;
            check(streams(plan_run(streamed_cfg, laptop, model, shared, asked, defaults)),
                  "streamed by the caller: left alone");
            HardwareProfile unshown = laptop;
            for (ComputeDevice & d : unshown.devices)
                if (!d.is_cpu) d.wide_identity_ok = Tri::Unknown;
            check(streams(plan_run(base_cfg(), unshown, model, shared, PlanRequest{}, defaults)),
                  "streamed, a device not shown to compute a prompt as the cores do: the stream");
        }

        // A model with no experts is asked the same question.
        ModelProfile dense;
        dense.arch = "llama";
        dense.ok = true;
        dense.is_moe = false;
        dense.n_layer = 32;
        dense.file_bytes = dense.dense_bytes = 5 * GiB;
        const Plan dp = plan_run(base_cfg(), roomy, dense, shared, PlanRequest{}, PlannerPolicy::defaults());
        check(dp.regime == Regime::NotMoe && dp.config.trial.answer_device == "accel" &&
                  dp.config.trial.answer_prompt_per_token >= 0.0f && dp.config.n_gpu_layers == 33,
              "shared device, a dense model held in memory: asked the same way, the load times both sides");
        dense.knobs.shared_device_tps = 27.0;
        dense.knobs.no_shared_device_tps = 21.0;
        dense.knobs.mix_requests = 4000;
        dense.knobs.mix_gen_tokens = 400;
        const Plan dm = plan_run(base_cfg(), roomy, dense, shared, PlanRequest{}, PlannerPolicy::defaults());
        check(dm.config.n_gpu_layers == 33 && dm.config.device_split.size() == 1,
              "shared device, a dense model measured faster on the device: every layer moves");

        // A dense model the cores hold without the room residency asks beside it: the cores hold it
        // all the same, so the question is still asked, against what the move adds to this pool.
        {
            ModelProfile snug_model = dense;
            snug_model.knobs = MeasuredKnobs{};
            HardwareProfile snug = roomy;
            snug.residency_budget = 8 * GiB; // the model fits, not with as much again beside it
            HardwareProfile snug_in_place = snug;
            for (ComputeDevice & d : snug_in_place.devices)
                if (!d.is_cpu) d.host_ptr_buffers = d.host_ptr_verified = Tri::Yes;
            const Plan asked =
                plan_run(base_cfg(), snug_in_place, snug_model, shared, PlanRequest{}, PlannerPolicy::defaults());
            const Decision * da = find(asked, "shared-device");
            check(asked.regime == Regime::NotMoe && asked.config.trial.answer_device == "accel" && da &&
                      da->value == "accel",
                  "shared device, a dense model short of residency's room, weights read in place: still asked, "
                  "and the load decides",
                  da ? da->reason : "no line");
            // Copied, the weights become this process's memory: the move adds them, and the bar is on that.
            const Plan copied =
                plan_run(base_cfg(), snug, snug_model, shared, PlanRequest{}, PlannerPolicy::defaults());
            const Decision * dc = find(copied, "shared-device");
            check(on_cores(copied) && copied.config.trial.shared_device_side == -1 && dc && dc->value == "cpu" &&
                      dc->reason.find("what the move adds") != std::string::npos,
                  "shared device, a dense model short of room for a copy of its weights: refused, and said",
                  dc ? dc->reason : "no line");
            // Past what this process may hold at all: refused, and said, never silent.
            HardwareProfile small = snug_in_place;
            small.residency_budget = 4 * GiB;
            const Plan over = plan_run(base_cfg(), small, snug_model, shared, PlanRequest{}, PlannerPolicy::defaults());
            const Decision * dov = find(over, "shared-device");
            check(on_cores(over) && dov && dov->value == "cpu" && dov->reason.find("past the") != std::string::npos,
                  "shared device, a dense model past what may be held: refused, and said",
                  dov ? dov->reason : "no line");
        }

        // Elsewhere nothing changes: a device with memory of its own goes through the fitter, and a
        // model that streams has no whole model to move.
        HardwareProfile big = desktop();
        big.residency_budget = 64 * GiB;
        big.memory_total = 64 * GiB;
        const Plan disc = plan_run(base_cfg(), big, model, PlanRequest{});
        check(!find(disc, "shared-device") && disc.config.trial.shared_device_side == -1,
              "a device with memory of its own: not this question");
        const Plan streams = plan_run(base_cfg(), unified(), model, shared, PlanRequest{}, PlannerPolicy::defaults());
        check(streams.regime != Regime::Fits && !find(streams, "shared-device"),
              "shared device, the model streams: not this question");
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

        // A device expert cache computes the experts on the device: a long context's graph is held to
        // the device's bound (#533) even where the host's memory would take it whole.
        {
            RunConfig long_cfg = base_cfg();
            long_cfg.n_ctx = 32768;
            Placement half_long = half;
            half_long.n_ctx = 32768;
            half_long.compute_width = 8192;
            half_long.host_compute_per_token = 1 * KiB;
            half_long.raw_host_compute_bytes = 8192ull * KiB;
            const Plan cached = plan_run(long_cfg, card, model, half_long, PlanRequest{}, PlannerPolicy::defaults());
            const Decision * dw = find(cached, "ubatch");
            check(cached.config.moe.device_cache_mb > 0 &&
                      (uint32_t) cached.config.graph_width() == PlannerPolicy::defaults().ubatch_max_device && dw &&
                      dw->reason.find("#533") != std::string::npos,
                  "width, device expert cache: the graph is held to the device's bound",
                  std::to_string(cached.config.moe.device_cache_mb) + " MiB cache, " +
                      std::to_string(cached.config.graph_width()) + (dw ? ": " + dw->reason : std::string()));
        }

        // No record of the routing yet: the cache still goes on the faster processor, and fills from use
        // as the RAM cache does. A record is what says how much of a token it serves, not whether.
        const Plan linear = plan_on(card, half, PlanRequest{});
        const Decision * d = find(linear, "device-cache-mb");
        check(d && d->source == Source::Measured && linear.config.moe.device_cache_mb == (int) (9 * 1024) &&
                  d->reason.find("the prior in token cycles") != std::string::npos,
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

        // The projector goes on the device with every layer: its bytes come off the card's spare
        // before the cache does, and the host's ledger has no row for it.
        {
            ModelProfile seeing = model;
            seeing.projector_bytes = 1 * GiB;
            const Plan v = plan_run(base_cfg(), card, seeing, half, PlanRequest{}, PlannerPolicy::defaults());
            check(v.config.moe.device_cache_mb == (int) (8 * 1024) && !ledger_row(v, LedgerKind::Projector),
                  "device tier, projector: the device cache is smaller by it, and the host holds none",
                  std::to_string(v.config.moe.device_cache_mb) + " MiB");

            // The fitter reserved its room before placing layers: the spare already excludes it and
            // the device's bytes include it, so the tier charges it no second time.
            Placement reserved = half;
            reserved.device_projector_bytes = 1 * GiB;
            reserved.device_bytes += 1 * GiB;
            const Plan r = plan_run(base_cfg(), card, seeing, reserved, PlanRequest{}, PlannerPolicy::defaults());
            const Decision * pn = find(r, "placement");
            check(r.config.moe.device_cache_mb == (int) (9 * 1024) && !ledger_row(r, LedgerKind::Projector),
                  "device tier, projector reserved by the fitter: the cache is whole, charged once, no host row",
                  std::to_string(r.config.moe.device_cache_mb) + " MiB");
            // The experts' bytes became the cache: what the device still holds is the dense part and
            // the projector, once.
            check(pn && pn->reason.find("on devices " + std::to_string(4 * 1024) + " MiB") != std::string::npos,
                  "device tier, projector reserved by the fitter: the placement counts it on the device",
                  pn ? pn->reason : "none");

            // A partial placement, the common case on a small card: fewer layers than the model has,
            // so no tier is armed, and the projector is still the device's, through the fitter's
            // reservation alone - nowhere else charges it.
            Placement part = reserved;
            part.n_gpu_layers = (int32_t) model.n_layer - 8;
            const Plan q = plan_run(base_cfg(), card, seeing, part, PlanRequest{}, PlannerPolicy::defaults());
            pn = find(q, "placement");
            check(q.config.moe.device_cache_mb == 0 && !ledger_row(q, LedgerKind::Projector) &&
                      q.config.n_gpu_layers == part.n_gpu_layers,
                  "device tier, partial placement with a projector: no tier, no host row, the fitter's layers stand",
                  std::to_string(q.config.moe.device_cache_mb) + " MiB, " + std::to_string(q.config.n_gpu_layers) +
                      " layers");
            check(pn && pn->reason.find("on devices " + std::to_string(13 * 1024) + " MiB") != std::string::npos,
                  "device tier, partial placement with a projector: counted on the device once",
                  pn ? pn->reason : "none");
            // And the plan says why no cache was armed, rather than saying nothing.
            const Decision * dq = find(q, "device-cache-mb");
            check(dq && dq->value == "0" && dq->source == Source::Derived &&
                      dq->reason.find("dense parts are on the host") != std::string::npos,
                  "device tier, partial placement: the plan says the dense parts are not all on the device",
                  dq ? dq->reason : "no decision");

            // One slot short is partial too. llama.cpp counts n_layer + 1 slots, the output last, and
            // fills the devices from the top: n_layer of them leaves the first block on the host,
            // dense part and all, so a cache on the device would be crossed to on every token.
            Placement one_short = reserved;
            one_short.n_gpu_layers = (int32_t) model.n_layer;
            const Plan os = plan_run(base_cfg(), card, seeing, one_short, PlanRequest{}, PlannerPolicy::defaults());
            const Decision * ds = find(os, "device-cache-mb");
            check(os.config.moe.device_cache_mb == 0 && ds && ds->value == "0" &&
                      ds->reason.find("dense parts are on the host") != std::string::npos,
                  "device tier, one slot short: the first block's dense part is on the host, so no tier",
                  ds ? std::to_string(os.config.moe.device_cache_mb) + " MiB: " + ds->reason : "no decision");

            // A device whose memory is the host's: the fitter's placement is set aside, and the
            // projector is a row of the host's ledger, as on a machine with no device at all.
            Placement aside;
            aside.shared_memory_placement = true;
            aside.device_bytes = 3 * GiB;
            const Plan sh = plan_run(base_cfg(), phone(), seeing, aside, PlanRequest{}, PlannerPolicy::defaults());
            const LedgerRow * pr = ledger_row(sh, LedgerKind::Projector);
            check(pr && pr->bytes == 1 * GiB && sh.config.moe.device_cache_mb == 0,
                  "shared-memory device, projector: a host row of its size, no device cache");
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
        // The server's own default overlap, not asked for, is turned off where the cache is armed:
        // left on, the session refused the pair and every server load failed (a 16 GB card).
        {
            RunConfig srv = base_cfg();
            srv.moe.overlap = true;
            const Plan sp = plan_run(srv, slow_cores, model, half, PlanRequest{}, PlannerPolicy::defaults());
            const Decision * od = find(sp, "overlap");
            check(sp.config.moe.device_cache_mb > 0 && !sp.config.moe.overlap && od && od->value == "off" &&
                      validate(sp.config).ok,
                  "device tier, a front-end's default overlap: off beside the cache", od ? od->reason : "none");
        }
        check(validate(tier.config).ok, "device tier, cache faster: a valid config", validate(tier.config).error);

        // Decided by calculation: each side at its measurement where a load timed it, at its price from
        // the probed rates otherwise; the load serves the faster and times its own decode as it serves
        // (MeasuredKnobs::device_cache_tps). No load is opened to try a side.
        {
            const auto tier_on = [&](const HardwareProfile & h, const MeasuredKnobs & k) {
                ModelProfile mm = model;
                mm.knobs = k;
                return plan_run(base_cfg(), h, mm, half, PlanRequest{}, PlannerPolicy::defaults());
            };
            // Nothing measured: the cache is priced far past the fitter's ceiling (13.4 tok/s: its 24
            // host layers' experts, 291.5 MiB a token, at the host's 4 GiB/s, the rest at the card's 881).
            check(tier.config.trial.device_tier == 1,
                  "device tier, nothing measured: the cache is priced faster, served, and timed as it serves",
                  std::to_string(tier.config.trial.device_tier));
            // The same machine over a link that makes every miss the whole token: the fitter's
            // placement is priced faster, so it is the first load's, with no load spent on the cache.
            HardwareProfile dial_up = slow_cores;
            dial_up.devices[0].upload_gibs = 0.05;
            dial_up.devices[0].upload_host_buffer_gibs = 0.0;
            const Plan fitter_first = tier_on(dial_up, MeasuredKnobs{});
            const Decision * e = find(fitter_first, "device-cache-mb");
            check(fitter_first.config.moe.device_cache_mb == 0 && fitter_first.config.trial.device_tier == 0 &&
                      fitter_first.config.buft_overrides.size() == 24 && e && e->source == Source::Derived &&
                      e->reason.find("priced from the probed rates") != std::string::npos,
                  "device tier, nothing measured, misses too slow to upload: the fitter's placement is served first",
                  e ? e->reason : "no decision");
            check(validate(fitter_first.config).ok, "device tier, the fitter's placement first: a valid config",
                  validate(fitter_first.config).error);

            // The cache measured on a load at 1.6 tok/s, under the fitter's price by the margin: the
            // next load serves the fitter's placement and times it.
            MeasuredKnobs once;
            once.device_cache_tps = 1.6;
            const Plan second = tier_on(slow_cores, once);
            e = find(second, "device-cache-mb");
            check(second.config.moe.device_cache_mb == 0 && second.config.trial.device_tier == 0 &&
                      second.config.buft_overrides.size() == 24 && e && e->source == Source::Derived &&
                      e->reason.find("1.60 tok/s with a device cache (measured on a load here) against 6.7") !=
                          std::string::npos,
                  "device tier, the cache measured under the fitter's price: the fitter's placement is served",
                  e ? e->reason : "no decision");
            check(validate(second.config).ok, "device tier, second load: a valid config",
                  validate(second.config).error);
            // The placement is priced at half its ceiling (6.7 tok/s), what such placements were measured
            // to reach. Measured at 20, or at 15, the cache is past it: it stays, nothing is timed again,
            // and no load runs the slower side. At 5 (5.5 with the margin) the placement's price is past it.
            for (double tps : {20.0, 15.0}) {
                MeasuredKnobs fast;
                fast.device_cache_tps = tps;
                const Plan untimed = tier_on(slow_cores, fast);
                e = find(untimed, "device-cache-mb");
                check(untimed.config.moe.device_cache_mb > 0 && untimed.config.trial.device_tier == -1 && e &&
                          e->reason.find("not past the 10% margin") != std::string::npos && validate(untimed.config).ok,
                      "device tier, the cache measured past the fitter's price: kept, the other side never opened",
                      e ? e->reason : "no decision");
            }
            MeasuredKnobs under;
            under.device_cache_tps = 5.0;
            check(tier_on(slow_cores, under).config.trial.device_tier == 0,
                  "device tier, the cache measured under the fitter's price by the margin: the fitter's placement");

            // A cache measured at 5.8 (6.4 with the margin) is under the placement's 6.7 where the host
            // holds the experts the fitter leaves it. On a host whose memory does not, they stream, a
            // token also waits on flash for them, and the placement's price with those reads is no
            // longer past the cache. Priced on compute alone, an 8 GB card served the fitter's placement
            // for a load at 3.5 tok/s where its price said 11.
            MeasuredKnobs near_cache;
            near_cache.device_cache_tps = 5.8;
            HardwareProfile tight_host = slow_cores;
            tight_host.residency_budget = 3 * GiB;
            const Plan streams = tier_on(tight_host, near_cache);
            e = find(streams, "device-cache-mb");
            check(tier_on(slow_cores, near_cache).config.trial.device_tier == 0 &&
                      streams.config.moe.device_cache_mb > 0 && streams.config.trial.device_tier == -1,
                  "device tier, the fitter's experts stream on this host: its flash reads are in its price",
                  e ? e->reason : "no decision");

            MeasuredKnobs lost = once;
            lost.no_device_cache_tps = 3.94; // the fitter's placement decoded 2.5x faster
            const Plan third = tier_on(slow_cores, lost);
            e = find(third, "device-cache-mb");
            check(third.config.moe.device_cache_mb == 0 && third.config.trial.device_tier == -1 &&
                      third.config.buft_overrides.size() == 24 && e && e->source == Source::Measured &&
                      e->reason.find("1.60 tok/s with a device cache (measured on a load here) against 3.94") !=
                          std::string::npos,
                  "device tier, the fitter's placement measured faster: it is kept, and the plan says by how much",
                  e ? e->reason : "no decision");
            // The fitter's placement measured, the cache only priced: its price past the measurement by
            // the margin brings the cache back, timed as it serves.
            MeasuredKnobs fitter_only;
            fitter_only.no_device_cache_tps = 30.0;
            const Plan back = tier_on(slow_cores, fitter_only);
            check(back.config.moe.device_cache_mb > 0 && back.config.trial.device_tier == 1,
                  "device tier, the fitter's placement measured under the cache's price: the cache, timed");

            MeasuredKnobs won;
            won.device_cache_tps = 17.5;
            won.no_device_cache_tps = 9.9;
            const Plan kept = tier_on(slow_cores, won);
            e = find(kept, "device-cache-mb");
            check(kept.config.moe.device_cache_mb > 0 && kept.config.trial.device_tier == -1 && e &&
                      e->reason.find("17.50 tok/s with a device cache (measured on a load here)") != std::string::npos,
                  "device tier, the cache measured faster: armed, nothing timed again", e ? e->reason : "no decision");

            MeasuredKnobs tie;
            tie.device_cache_tps = 10.0;
            tie.no_device_cache_tps = 10.5; // inside the margin: the cache holds
            const Plan held = tier_on(slow_cores, tie);
            check(held.config.moe.device_cache_mb > 0 && held.config.trial.device_tier == -1,
                  "device tier, the two inside the margin: the cache holds");

            // The device cache's CPU share is a person's setting: the plan uploads every miss, names no
            // side for it, and says how to set it, on a machine where a price used to nominate one (a
            // host at 66 GiB/s behind a link at 3).
            {
                HardwareProfile slow_link = card;
                slow_link.devices[0].upload_gibs = 3.0;
                slow_link.devices[0].host_hop_seconds = 58e-6;
                const Plan uploaded = tier_on(slow_link, won);
                const Decision * sd = find(uploaded, "device-cache-cpu-share");
                check(uploaded.config.moe.device_cache_mb > 0 && uploaded.config.moe.device_cache_cpu_share == 0.0f &&
                          sd && sd->source == Source::Policy && sd->reason.find("sets it by hand") != std::string::npos,
                      "cpu share: never the plan's, every miss uploaded, and the plan says how to set it",
                      sd ? sd->reason : "no decision");
                PlanRequest pin_share;
                pin_share.pinned.push_back("device-cache-cpu-share");
                RunConfig with_share = base_cfg();
                with_share.moe.device_cache_cpu_share = 0.5f;
                ModelProfile mm = model;
                mm.knobs = won;
                const Plan asked = plan_run(with_share, slow_link, mm, half, pin_share, PlannerPolicy::defaults());
                check(asked.config.moe.device_cache_cpu_share == 0.5f,
                      "cpu share: a share the person set is kept as set");
            }

            // A caller who placed it is not measured against.
            PlanRequest pin;
            pin.pinned.push_back("device-cache-mb");
            const Plan pinned = plan_run(base_cfg(), slow_cores, model, half, pin, PlannerPolicy::defaults());
            check(pinned.config.trial.device_tier == -1, "device tier, pinned by the caller: nothing timed");

            // A side that failed to open here as often as a plan lets it loses; once is the machine's
            // moment, and the price still decides.
            MeasuredKnobs no_cache;
            no_cache.device_cache_unviable = 1;
            check(tier_on(slow_cores, no_cache).config.moe.device_cache_mb > 0,
                  "device tier, the cache failed to open once: still the plan's, on its price");
            no_cache.device_cache_unviable = MeasuredKnobs::unviable_max;
            const Plan fitter_kept = tier_on(slow_cores, no_cache);
            e = find(fitter_kept, "device-cache-mb");
            check(fitter_kept.config.moe.device_cache_mb == 0 && fitter_kept.config.trial.device_tier == -1 &&
                      fitter_kept.config.buft_overrides.size() == 24 && e && e->source == Source::Measured,
                  "device tier, the cache could not be opened: the fitter's placement, nothing timed",
                  e ? e->reason : "no decision");
            MeasuredKnobs no_fitter = once; // the cache measured under the ceiling, the other side unopenable
            no_fitter.no_device_cache_unviable = MeasuredKnobs::unviable_max;
            const Plan cache_kept = tier_on(slow_cores, no_fitter);
            e = find(cache_kept, "device-cache-mb");
            check(cache_kept.config.moe.device_cache_mb > 0 && cache_kept.config.trial.device_tier == -1 && e &&
                      e->reason.find("failed") != std::string::npos,
                  "device tier, the fitter's placement could not be opened: the cache, nothing timed",
                  e ? e->reason : "no decision");
        }

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
            // The copy takes the place of the mapped experts the host is already charged for: a host with
            // room for the model and not for the experts twice still takes it, and says it holds them once.
            // Charged twice, every 64 GB machine a 32 GB expert set ran on refused the copy and uploaded
            // its misses at half the rate.
            hb.devices[0].upload_host_buffer_gibs = 48.0;
            hb.residency_budget = model.file_bytes + model.expert_bytes / 2;
            const Plan tight = plan_on(hb, half, PlanRequest{});
            const Decision * et = find(tight, "expert-host-buffer");
            check(tight.regime == Regime::Fits && tight.config.moe.expert_host_buffer == "discrete" && et &&
                      et->reason.find("takes the place") != std::string::npos,
                  "expert host buffer: the copy replaces the mapped experts, it is not charged beside them",
                  et ? et->reason : "no decision");
            // Short of room for the model itself, nothing is resident to copy from: no copy.
            hb.residency_budget = model.dense_bytes * 2;
            const Plan streamed = plan_on(hb, half, PlanRequest{});
            check(streamed.regime != Regime::Fits && streamed.config.moe.expert_host_buffer.empty(),
                  "expert host buffer: a host that cannot hold the model makes no copy");
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

            // Two cards: llama.cpp hands each a share of one budget by its layers, so the layers and the
            // budget are decided together. Each card's room under the cache is what it had free, the
            // experts the fitter put on it and its layers' dense part and context; every card must
            // hold its layers' dense part and its share of the budget within that room.
            const double dense_layer =
                (double) (model.group(WeightGroup::Attention).bytes + model.group(WeightGroup::DenseFfn).bytes +
                          model.group(WeightGroup::Other).bytes) /
                (double) model.n_layer;
            const auto fits_every_card = [&](const Plan & p, const std::vector<double> & room) {
                uint32_t layers = 0;
                for (const auto & e : p.config.device_split)
                    layers += (uint32_t) e.second;
                layers -= 1; // the output's slot
                const double budget = (double) p.config.moe.device_cache_mb * (double) MiB;
                for (size_t i = 0; i < p.config.device_split.size(); ++i) {
                    const double n = (double) p.config.device_split[i].second - (i + 1 == room.size() ? 1 : 0);
                    if (n * dense_layer + budget * n / layers > room[i] + (double) MiB) return false;
                }
                return true;
            };
            Placement two = half;
            two.device_split = {{"discrete", 37}, {"second", 12}};
            two.device_spare = {{"discrete", 6 * GiB}, {"second", 1 * GiB}};
            const Plan pair = plan_on(slow_cores, two, PlanRequest{});
            // Layers 0-36 on the first (24 with their experts home, 13 whole), 37-47 and the output on
            // the second (11 whole): the fitter's 9 GiB of experts over its 24 whole layers.
            const double per_whole = 9.0 * (double) GiB / 24.0;
            const std::vector<double> room = {6.0 * GiB + 13 * per_whole + 37 * dense_layer,
                                              1.0 * GiB + 11 * per_whole + 11 * dense_layer};
            const double kept_budget =
                std::min((room[0] - 37 * dense_layer) * 48 / 37, (room[1] - 11 * dense_layer) * 48 / 11);
            check(fits_every_card(pair, room) && pair.config.moe.device_cache_mb > (int) (kept_budget / MiB) &&
                      pair.config.moe.device_cache_mb <= (int) (16 * 1024),
                  "device tier, two cards: every card holds its share, more than the fitter's split allowed",
                  std::to_string(pair.config.moe.device_cache_mb) + " MiB against " +
                      std::to_string((int) (kept_budget / MiB)));
            const Decision * ds = find(pair, "device-split");
            check(ds && pair.config.device_split.size() == 2 && pair.config.device_split[0].second != 37 &&
                      pair.config.device_split[0].second + pair.config.device_split[1].second == 49 &&
                      validate(pair.config).ok,
                  "device tier, two cards: the layers follow the room, every slot still placed",
                  ds ? ds->reason : "no device-split decision");

            // The case it is for: two equal cards, the fitter's whole layers all on the first (its room
            // is their experts) and dense-only layers on the second. With the fitter's split the first
            // card's share was its 6 layers' worth of the budget, and most of the card stayed empty.
            Placement lopsided = half;
            lopsided.host_expert_layers.clear();
            for (uint32_t il = 6; il < model.n_layer; ++il)
                lopsided.host_expert_layers.push_back(il);
            lopsided.override_patterns.assign(lopsided.all_expert_patterns.begin() + 6,
                                              lopsided.all_expert_patterns.end());
            lopsided.device_split = {{"discrete", 6}, {"second", (uint32_t) model.n_layer + 1 - 6}};
            lopsided.device_spare = {{"discrete", 256 * MiB}, {"second", 4 * GiB}};
            const Plan even = plan_on(slow_cores, lopsided, PlanRequest{});
            const uint64_t six = model.expert_bytes / model.n_moe_layer * 6;
            const std::vector<double> room2 = {256.0 * MiB + (double) six + 6 * dense_layer,
                                               4.0 * GiB + (double) (model.n_layer - 6) * dense_layer};
            const double kept2 =
                std::min((room2[0] - 6 * dense_layer) * model.n_layer / 6,
                         (room2[1] - (model.n_layer - 6) * dense_layer) * model.n_layer / (model.n_layer - 6));
            check(fits_every_card(even, room2) && even.config.moe.device_cache_mb > (int) (kept2 / MiB) &&
                      even.config.device_split[0].second > 6,
                  "device tier, whole layers on one card: the split follows the room the cache leaves",
                  std::to_string(even.config.moe.device_cache_mb) + " MiB against " +
                      std::to_string((int) (kept2 / MiB)));

            // The layers a card takes on are loaded before any cache is made, with nothing to fall back
            // on: each is charged the file's largest dense layer, so a file whose layers differ cannot
            // push the card that takes them past its memory. A card's room for its share is what it
            // holds for the cache (free beyond the fitter's projection and the experts it held), less
            // the bound for each layer it takes on.
            {
                ModelProfile uneven = model;
                uneven.largest_layer_dense_bytes = (uint64_t) (3.0 * dense_layer);
                const Plan safe =
                    plan_run(base_cfg(), slow_cores, uneven, lopsided, PlanRequest{}, PlannerPolicy::defaults());
                const double layers = (double) model.n_layer;
                const double budget = (double) safe.config.moe.device_cache_mb * (double) MiB;
                const double hold[2] = {256.0 * MiB + (double) six, 4.0 * GiB};
                const uint32_t had[2] = {6, model.n_layer - 6};
                bool fits = safe.config.device_split.size() == 2;
                for (size_t i = 0; fits && i < 2; ++i) {
                    const double n = (double) safe.config.device_split[i].second - (i == 1 ? 1 : 0);
                    const double moved = n - (double) had[i];
                    const double charged =
                        moved > 0 ? moved * (double) uneven.largest_layer_dense_bytes : moved * dense_layer;
                    fits = budget * n / layers + charged <= hold[i] + (double) MiB;
                }
                check(fits && safe.config.moe.device_cache_mb > 0,
                      "device tier, layers of different sizes: a card takes on layers at the largest one's size",
                      std::to_string(safe.config.device_split[0].second) + "/" +
                          std::to_string(safe.config.device_split[1].second) + ", " +
                          std::to_string(safe.config.moe.device_cache_mb) + " MiB");

                // Where the file's per-layer bytes are known, the layers a card takes on are charged
                // exactly: the split is contiguous, so it names them. Layer 0, on the first card, is the
                // heavy one; the first card takes on layers from 6 up, of the average size, and is not
                // charged layer 0's size for each.
                ModelProfile known = uneven;
                const double avg = dense_layer;
                known.layer_dense_bytes.assign(model.n_layer, (uint64_t) avg);
                known.layer_dense_bytes[0] = (uint64_t) (3.0 * avg);
                const Plan exact_plan =
                    plan_run(base_cfg(), slow_cores, known, lopsided, PlanRequest{}, PlannerPolicy::defaults());
                const double eb = (double) exact_plan.config.moe.device_cache_mb * (double) MiB;
                bool exact_fits = exact_plan.config.device_split.size() == 2;
                uint32_t at = 0;
                const uint32_t had_from[2] = {0, 6};
                for (size_t i = 0; exact_fits && i < 2; ++i) {
                    const uint32_t n = exact_plan.config.device_split[i].second - (i == 1 ? 1 : 0);
                    double now = 0.0, before = 0.0;
                    for (uint32_t il = at; il < at + n; ++il)
                        now += (double) known.layer_dense_bytes[il];
                    for (uint32_t il = had_from[i]; il < had_from[i] + had[i]; ++il)
                        before += (double) known.layer_dense_bytes[il];
                    exact_fits = eb * n / layers + (now - before) <= hold[i] + (double) MiB;
                    at += n;
                }
                check(exact_fits && exact_plan.config.moe.device_cache_mb > safe.config.moe.device_cache_mb,
                      "device tier, per-layer bytes known: the layers moved are charged exactly, and the cache is "
                      "larger than under the bound",
                      std::to_string(exact_plan.config.moe.device_cache_mb) + " MiB against " +
                          std::to_string(safe.config.moe.device_cache_mb));
            }
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
        // The account is what was measured, then the one outcome: the clause the ranking wrote before it
        // funded the move is not carried along.
        check(d && d->reason.rfind("measured, ", 0) == 0 &&
                  d->reason.find("; they come onto the host whole, for ") != std::string::npos &&
                  d->reason.find("did not fund") == std::string::npos,
              "device crossing, slow link: the account carries one outcome", d ? d->reason : "none");
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
        check(d && d->reason.rfind("measured, ", 0) == 0 &&
                  d->reason.find("; bringing them home costs the host ") != std::string::npos &&
                  d->reason.find("did not fund") != std::string::npos,
              "device crossing, no room: the account says what the ranking did not fund", d ? d->reason : "none");

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

        // An integrated GPU beside the card: the choice keeps it (it is a card of its own), and the
        // fitter must not be asked of it, or one layer there sets the card's placement aside too.
        HardwareProfile igpu = two;
        ComputeDevice shared;
        shared.name = "Vulkan1";
        shared.physical_id = "0000:7a:00.0";
        shared.memory_total = 16 * GiB;
        shared.memory_free = 15 * GiB;
        shared.shares_host_memory = Tri::Yes;
        shared.expert_gibs = 40.0;
        igpu.devices.push_back(shared);
        const BackendChoice with_igpu = choose_backends(igpu, 0.10);
        check(with_igpu.keep == std::vector<std::string>{"Vulkan0", "Vulkan1"},
              "backends: an integrated GPU is a card of its own and stays in the choice");
        check(capacity_devices(igpu, with_igpu.keep) == std::vector<std::string>{"Vulkan0"},
              "capacity: the fitter is asked of the card with memory of its own, not of the integrated GPU");
        HardwareProfile only_shared;
        only_shared.devices.push_back(shared);
        check(capacity_devices(only_shared, {"Vulkan1"}) == std::vector<std::string>{"Vulkan1"},
              "capacity: with no device of its own memory the list is left as it is");
        check(capacity_devices(igpu, {}).empty(), "capacity: an empty list stays empty (llama.cpp's default)");
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

    // ── a row-gathered table larger than the budget, with the model's routing recorded ───────
    // Shaped after DeepSeek V4.1 Flash on a 283 GB control group: a 194 GB Engram table the graph
    // gathers a few KiB of per token, 270 GB of experts, a 210 GB budget. With a recorded hit curve
    // the plan once made the table resident (194 GB held) beside a 210 GB cache, 417 GB committed
    // against 210, and the run was killed when the cache filled.
    {
        ModelProfile big = model;
        big.embedding_tensors = {"token_embd.weight", "per_layer_token_embd.weight"};
        big.group(WeightGroup::Embedding) = {194 * GiB, 13 * KiB, true, true};
        big.expert_bytes = 270 * GiB;
        big.group(WeightGroup::Experts) = {270 * GiB, big.token_cycle_bytes, false, true};
        big.dense_bytes = 194 * GiB + 3 * GiB;
        big.file_bytes = big.expert_bytes + big.dense_bytes;
        big.largest_dense_tensor = 194 * GiB;
        HardwareProfile h = desktop();
        h.residency_budget = 210 * GiB;
        h.memory_total = 2048 * GiB; // a shared host: the control group, not the host, is the bound
        h.host_bandwidth_gibs = 100.0;
        h.storage.refault_mibs = 160.0;
        h.devices.clear();
        ModelProfile curved = big;
        curved.hit_curve.measured = true;
        curved.hit_curve.decode_tokens = 256;
        curved.hit_curve.prefill_tokens = 26;
        curved.hit_curve.points = {{0, 0.0},
                                   {(uint64_t) (0.05 * (double) curved.expert_bytes), 0.30},
                                   {(uint64_t) (0.30 * (double) curved.expert_bytes), 0.85},
                                   {curved.expert_bytes, 0.95}};
        // A short record touches few experts: its curve is flat past them, so a byte of cache there
        // prices at nothing and the table's tiny per-byte saving outranks it.
        ModelProfile flat = big;
        flat.hit_curve.measured = true;
        flat.hit_curve.decode_tokens = 256;
        flat.hit_curve.prefill_tokens = 26;
        flat.hit_curve.points = {{0, 0.0}, {2 * flat.token_cycle_bytes, 0.91}, {flat.expert_bytes, 0.91}};
        for (const auto & mc : {std::make_pair("no record", big), std::make_pair("a record", curved),
                                std::make_pair("a short record", flat)}) {
            const Plan p = plan_run(base_cfg(), h, mc.second, PlanRequest{});
            const GroupPlacement & e = p.allocation.at(WeightGroup::Embedding);
            check(e.lane != Lane::Resident && e.lane != Lane::Pinned && e.resident_bytes == 0,
                  std::string("big table, ") + mc.first + ": a 194 GB row-gathered table is not made resident",
                  std::string(lane_name(e.lane)) + ", " + std::to_string(e.resident_bytes >> 20) + " MiB");
            uint64_t held = p.allocation.cache_bytes;
            for (int i = 0; i < (int) WeightGroup::count; ++i)
                if ((WeightGroup) i != WeightGroup::Experts) held += p.allocation.at((WeightGroup) i).resident_bytes;
            check(held <= h.residency_budget, std::string("big table, ") + mc.first + ": what is held fits the budget",
                  std::to_string(held >> 20) + " MiB held");
            check(p.config.moe.dense_mapped == big.embedding_tensors,
                  std::string("big table, ") + mc.first + ": the run is told to leave the tables mapped");
            // This storage measures a live mapping slowing its reads, which alone would release it;
            // the tables are read through it all run, so it stays.
            const Decision * rel = find(p, "release-mmap");
            check(rel && rel->value == "off" && !p.config.moe.release_mmap,
                  std::string("big table, ") + mc.first + ": a table left mapped keeps the mapping",
                  rel ? rel->value : "no decision");
        }
    }

    return failures ? 1 : 0;
}
