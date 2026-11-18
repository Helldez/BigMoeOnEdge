// What a model is before a byte of it is downloaded: its gguf header, read from where the file is
// kept through a range request, parsed by the planner's own model probe (the one reading the listing,
// the plan and the load all use), and set against this machine's memory and disk.
#pragma once

#include "hf_repo.h"
#include "http_fetch.h"
#include "json_util.h"
#include "model_library.h"

#include <string>
#include <vector>

namespace bmoe::server {

// The model loaded now, as the engine host knows it: its file name and what its plan's memory
// ledger holds (0 when it was loaded without a plan). Loading the inspected model unloads it, so
// the verdict gives that back (hf_repo.h: FitInputs). An empty name is no model loaded.
struct LoadedSession {
    std::string model;
    uint64_t held_bytes = 0;
};

// One file of a model as the check needs it: the name it has once downloaded, where its bytes are
// now, and how many they are (0 when not known).
struct RemoteFile {
    std::string name;
    std::string url;
    uint64_t bytes = 0;
};

// The check itself, of a model given as its files in shard order, wherever they are kept: `model`
// (what the header says it is), `machine` (what this process may hold, free disk in `models_dir`,
// what is already there) and `verdict` (hf_repo.h: fit_verdict) are written into `out`, whose
// other keys are the caller's. The headers are fetched into `scratch_dir` and removed before this
// returns. False with the reason when a file could not be fetched at all; a header that fetched
// and did not parse is a fact in the answer, not an error.
bool inspect_remote_files(HttpFetch & net,
                          const std::vector<RemoteFile> & files,
                          const std::string & token,
                          const std::string & scratch_dir,
                          const std::string & models_dir,
                          const LoadedSession & loaded,
                          json & out,
                          std::string & error);

// The /api/models/inspect object for a catalog entry (model_library.h), shipped or added: its id,
// title, quant, size and file count, with the check's three objects. The entry's own URLs are the
// ones read, so a model is checked where its download would come from. No token goes with the
// request: an entry's URL may be on any host (url_entry.h), and a repository's token is for the
// repository alone.
bool inspect_catalog_entry(HttpFetch & net,
                           const CatalogEntry & entry,
                           const std::string & scratch_dir,
                           const std::string & models_dir,
                           const LoadedSession & loaded,
                           json & out,
                           std::string & error);

// The /api/hf/inspect object for `model` of `ref`: the model's facts (architecture, experts,
// bytes by group, whether it streams, what the file is for), the machine's (what this process may
// hold, free disk in `models_dir`, what is already there) and the verdict (hf_repo.h: fit_verdict),
// with `loaded` given back: inspect_remote_files over the repository's resolve URLs.
bool inspect_remote_model(HttpFetch & net,
                          const std::string & endpoint,
                          const HfRef & ref,
                          const HfModel & model,
                          const std::string & token,
                          const std::string & scratch_dir,
                          const std::string & models_dir,
                          const LoadedSession & loaded,
                          json & out,
                          std::string & error);

} // namespace bmoe::server
