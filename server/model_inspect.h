// What a model is before a byte of it is downloaded: its gguf header, read from the repository
// through a range request, parsed by the planner's own model probe (the one reading the listing,
// the plan and the load all use), and set against this machine's memory and disk.
#pragma once

#include "hf_repo.h"
#include "http_fetch.h"
#include "json_util.h"

#include <string>

namespace bmoe::server {

// The /api/hf/inspect object for `model` of `ref`: the model's facts (architecture, experts,
// bytes by group, whether it streams), the machine's (what this process may hold, free disk in
// `models_dir`, what is already there) and the verdict (hf_repo.h: fit_verdict). The headers are
// fetched into `scratch_dir` and removed before this returns. False with the reason when the
// listing's files could not be fetched at all; a header that fetched and did not parse is a fact
// in the answer, not an error.
bool inspect_remote_model(HttpFetch & net,
                          const std::string & endpoint,
                          const HfRef & ref,
                          const HfModel & model,
                          const std::string & token,
                          const std::string & scratch_dir,
                          const std::string & models_dir,
                          json & out,
                          std::string & error);

} // namespace bmoe::server
