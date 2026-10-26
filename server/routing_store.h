// Where the server keeps each model's recorded routing between loads.
//
// The engine records what a model routed during its first tokens (Session::routing_record) and the
// planner prices the cache on it (bmoe/routing.h, AutoPlanOptions::routing); where the record lives
// is the front-end's decision, as the plan's own settings are. Here it is one file per model in the
// data folder, named by the model file's content fingerprint (model_fingerprint in bmoe/probe.h): a
// file copied or renamed finds its record, another model never does. The fingerprint is a few MiB of
// reads, so it is computed once per file and remembered by path, size and modification time, the key
// the model library already uses for what it reads from a file.
#pragma once

#include "bmoe/measured_knobs.h"
#include "bmoe/routing.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bmoe::server {

class RoutingStore {
public:
    explicit RoutingStore(std::string dir) : dir_(std::move(dir)) {}

    // The record kept for the model at `model_path`, if there is one that reads.
    bool load(const std::string & model_path, RoutingTrace & out);

    // Keep `rec` for that model, unless the one already kept decoded further: a short session does
    // not replace a longer record. Written beside and renamed, so a reader never sees half of one.
    void keep(const std::string & model_path, const RoutingTrace & rec);

    // The file a model's record lives in; "" when the model cannot be fingerprinted.
    std::string path_for(const std::string & model_path, const char * ext = ".routing");

    // The knob trial's verdicts kept for the model, one per machine and engine (bmoe/measured_knobs.h),
    // beside its routing. `keep_knobs` replaces the record of the machine and engine `rec` names.
    bool load_knobs(const std::string & model_path, std::vector<MeasuredKnobs> & out);
    void keep_knobs(const std::string & model_path, const MeasuredKnobs & rec);

private:
    void write(const std::string & path, const std::string & text, const char * what);
    struct Key {
        uint64_t size = 0;
        long long mtime = 0;
        std::string fingerprint;
    };
    std::string dir_;
    std::mutex m_;
    std::map<std::string, Key> fingerprints_;
};

} // namespace bmoe::server
