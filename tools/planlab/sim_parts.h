// Any machine from its parts' specifications, priced by what measured machines measured.
//
// A record carries the rates a probe measured on one machine. This file turns that set of machines
// into a model of parts: how a card's expert and dense matmul, its probe bandwidth, its uploads and a
// crossing follow from its published memory bandwidth, FP32 throughput and link, and how a host's
// per-thread matmul rate, its plateau and its storage follow from its cores, clock, memory bandwidth
// and drive. The coefficients are fitted every time from the records the parts catalog maps to parts
// (`rig` lines), so a record added to the data set moves every simulation, with no code changed. The
// residual of each fit is kept and printed with the plan: a simulated machine is a range.
//
// The catalog (docs/bench-data/hardware-sim/parts.txt) is data: public specifications under short
// names, for convenience in a machine spec and to say which parts a record was measured on. Nothing
// here names a part in code, and nothing in the engine reads any of it.
#pragma once

#include "plan_record.h"

#include "bmoe/hardware_profile.h"
#include "bmoe/model_profile.h"

#include <map>
#include <string>
#include <vector>

namespace bmoe {

// A part's published figures. Units: GB/s (10^9) for bandwidths and links, TFLOPS for FP32.
struct GpuSpec {
    std::string id, description;
    uint64_t vram = 0;
    double bw = 0.0, fp32 = 0.0, link = 0.0; // link: the card's own (gen x lanes)
};
struct CpuSpec {
    std::string id, description;
    uint32_t cores = 0;       // the cores (or the quota) the engine may use
    double ghz = 0.0;         // sustained all-core clock
    double membw = 0.0;       // GB/s, channels x transfer rate x 8
    std::string core;         // the microarchitecture class the per-thread rate is keyed by
    std::string simd;         // avx2, avx512, neon: the fallback when the class was never measured
    double thread_gibs = 0.0; // one thread's matmul rate on the model's quantization, where someone measured it
};
// A measured machine as parts: which record, which card and processor, and the link the card
// actually had (the slower of the card's and the slot's).
struct Rig {
    std::string record, gpu, cpu;
    double link = 0.0;
};

struct PartsCatalog {
    std::map<std::string, GpuSpec> gpus;
    std::map<std::string, CpuSpec> cpus;
    std::vector<Rig> rigs;
    std::string records_dir; // where the rigs' records are, from the catalog's own folder
};

bool parse_parts_catalog(const std::string & text,
                         const std::string & base_dir,
                         PartsCatalog & out,
                         std::string & error);
// "pcie4x8" -> GB/s; 0 when not understood.
double link_gbs(const std::string & s);

// One fitted term: the value used, and the spread of measured / modelled over the machines it was
// fitted on (1.0 / 1.0 when only one machine says anything).
struct Term {
    double value = 0.0;
    double lo = 1.0, hi = 1.0;
    int n = 0;
};

struct PartsModel {
    // Cards: time per layer = bytes / (efficiency x spec bandwidth) + latency.
    Term gpu_expert_eff, gpu_expert_lat_s, gpu_dense_eff, gpu_dense_lat_s;
    Term gpu_probe_bw;          // the probe's bandwidth / spec bandwidth
    Term gpu_pinned;            // upload from the backend's host memory / link
    Term gpu_pageable;          // upload from the file's mapping / link, where the link is what bounds it
    Term gpu_pageable_cap_gibs; // upload from the mapping where the host's staging copy bounds it, GiB/s
    Term gpu_reported_memory;   // the memory the driver reports / the card's published size
    Term gpu_split_s;           // a graph split, seconds
    Term gpu_host_hop_s;        // a host half in a device graph, seconds
    Term gpu_wide_per_tflop;
    // Hosts.
    std::map<std::string, Term> r1_per_ghz;         // "core|quant" -> one thread's GiB/s per GHz
    std::map<std::string, Term> core_k;             // core class -> log factor (fit over every quant)
    std::map<int, Term> quant_q;                    // ggml type -> log factor
    std::map<std::string, Term> low_bit_r1_per_ghz; // simd ("*": any) -> one thread per GHz under 4 bits
    Term host_plateau_of_membw;                     // memory-bound plateau / spec memory bandwidth
    Term host_parallel_eff;                         // compute-bound plateau / (threads x one thread)
    Term host_expert_of_plateau, host_dense_of_plateau, host_wide_of_plateau;
    std::vector<RateSample> storage_shape; // a measured curve, normalised to its own peak = 1
    PlanRecord host_template;              // non-rate host facts (overflow policy, storage flags)
    ComputeDevice device_template;         // non-rate card facts (a CUDA card's capabilities)
    std::vector<std::string> fitted_on;    // the records used
};

// Fit on every rig whose record parses, leaving out `skip` (a record's file name) when given.
bool fit_parts_model(const PartsCatalog & cat, PartsModel & out, std::string & error, const std::string & skip = "");

// A machine spec's parts section, read from `.machine` lines (simulate.h documents them).
struct MachineParts {
    uint64_t ram = 0, available = 0;
    CpuSpec cpu;
    double storage_gbs = 0.0;
    struct Card {
        GpuSpec spec;
        double link = 0.0; // the link the card gets in that slot
    };
    std::vector<Card> cards;
};

// The hardware profile the probes would have reported there, for `model`: every rate from the
// parts model at this model's own layer sizes and expert quantization. `notes` says, per term, how
// far the fit's machines spread around it and where it extrapolated.
HardwareProfile build_hardware(const MachineParts & parts,
                               const PartsModel & pm,
                               const ModelProfile & model,
                               std::vector<std::string> & notes);

// Leave-one-out: every rig's measured rates against the model fitted without it, as text.
std::string check_parts_model(const PartsCatalog & cat);

} // namespace bmoe
