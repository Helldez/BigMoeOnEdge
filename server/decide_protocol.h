// The decide request and its answer at the server's edge (docs/decide.md): a request body into a
// DecideRequest, a DecideResult into the response object. JSON in, JSON out, no session and no
// HTTP, so the rules are tested without a model. The keys are the CLI's BMOE_DECIDE keys
// (docs/telemetry.md), so a client reads the two front-ends alike.
#pragma once

#include "json_util.h"

#include "bmoe/decide.h"

#include <string>

namespace bmoe::server {

// Parse a /api/decide body: `prefix` (string, may be empty), `suffix` (string), `choices` (an array
// of strings; the engine refuses an empty one and colliding first tokens), `reuse_prefix` (bool,
// default true). False with `err` set for a body of the wrong shape.
bool parse_decide_request(const json & body, DecideRequest & out, std::string & err);

// The answer: `best`, `choice_logp` (null where the model gives a choice no mass at all), the token
// counts, the prefix state, and the prefill figures as BMOE_DONE's.
json decide_json(const DecideResult & r);

} // namespace bmoe::server
