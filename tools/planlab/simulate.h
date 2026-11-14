// A machine nobody has at hand, planned for: a host measured on one machine, compute devices measured
// on others, sized to the cards being asked about, and a real model file, which llama.cpp's own
// capacity fitter places on simulated devices of those sizes (sim_device.h). The result is a plan
// record like one `bmoe-planlab record` makes on the machine itself, marked as simulated, so every
// other command (replay, --sweep, --set, errors, table) works on it unchanged.
//
// What it is made of, and how far each part can be trusted, is docs/hardware-simulation.md. In
// short: the rules and the fitter are the real ones; every rate is a measured one, moved to another
// card by the ratio the spec states where the card was never measured; the speed is the cost
// model's, whose error against measured runs `bmoe-planlab errors` reports.
#pragma once

#include "plan_record.h"
#include "sim_parts.h"

#include "bmoe/config.h"
#include "bmoe/planner.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bmoe {

// One device of the simulated machine: the facts a record measured for a real card, sized and
// scaled to the one asked about.
struct SimDeviceFrom {
    std::string record;           // the record holding the measured card
    int index = -1;               // its index in that record's hw.devices; -1: the first that is not the CPU
    uint64_t memory_total = 0;    // the simulated card's memory; 0: the measured card's
    double bandwidth_scale = 1.0; // its memory bandwidth against the measured card's
    double compute_scale = 1.0;   // its wide-matmul (compute-bound) rate against the measured card's
    double link_scale = 1.0;      // its host link (uploads) against the measured card's
    std::string description;      // what it reports; empty: "<measured card's> (simulated, sized)"
};

// A machine as a text file (`.machine`), one `key=value` per line, `#` for comments; relative paths
// are read from the file's folder:
//
//   note=2x 16 GB cards (RTX A4000 class), 62 GB RAM
//   host=../plan-records/5070-12g-64g-deepseek-v4.1-ud-iq2xxs.planrec
//   device=../plan-records/3090-24g-qwen3.6-35b-q4km.planrec memory=16G bandwidth=0.479 compute=0.54
//   device=...                                   (one line per card, in order)
//   routing=<record of the same model>           (its measured hit curve; optional)
//   hitcurve=2.2G:0.61,6G:0.86,26G:0.97          (measured hit rates at cache sizes; optional, instead of routing=)
//   set=hw.residency_budget=60000000000          (any record key, applied last; repeatable)
//
// or, for any machine, its parts (docs/bench-data/hardware-sim/parts.txt names some):
//
//   parts=parts.txt
//   ram=32G                                      (available=29G: what the engine may hold; default ram - 3G)
//   cpu=ryzen7-7700 cores=16                     (or cpu=custom cores= ghz= membw= core= simd=)
//   storage=3.5                                  (GB/s, the drive's sequential read)
//   gpu=rtx4060 slot=pcie4x8                     (one line per card; or gpu=custom vram= bw= fp32= link=)
struct MachineSpec {
    std::string note;
    std::string host;    // the record whose host facts (memory, cores, rates, storage) are the machine's
    std::string routing; // a record of the same model whose hit curve the plan prices the cache with
    // Measured cache hit rates of this model, as (cache size, share served) points, where no routing
    // trace of it exists: what real runs' cache counters said at the sizes they ran with. A curve made
    // of them is a measurement of the model's locality, not a replay of its routing, and is said so.
    std::vector<std::pair<uint64_t, double>> hit_points;
    std::vector<SimDeviceFrom> devices;
    std::vector<std::string> sets;
    // Or the machine as its parts (`parts=CATALOG`, then `ram=`, `available=`, `cpu=`, `storage=`,
    // `gpu=` lines): every rate from the model of parts the catalog's measured machines fit.
    std::string parts_catalog;
    PartsCatalog catalog;
    MachineParts parts;
    std::string parts_text; // the parts lines as written, kept in the record
};

bool parse_machine_spec(const std::string & text, const std::string & base_dir, MachineSpec & out, std::string & error);

struct Simulation {
    PlanRecord record; // the simulated machine's record, its plan made by the real rules and fitter
    Plan plan;
    // Where the result stands on less than a measurement: a card scaled from another, host rates
    // measured on another model, a hit curve from another file. Said with the plan, never hidden.
    std::vector<std::string> caveats;
    std::string error; // non-empty: no plan was made
};

// Plan `cfg` (with `req`'s pins) for `model_path` on the machine `spec` describes. Registers the
// simulated devices, so it is called once per process.
Simulation simulate_machine(const MachineSpec & spec,
                            const std::string & model_path,
                            const RunConfig & cfg,
                            const PlanRequest & req);

} // namespace bmoe
