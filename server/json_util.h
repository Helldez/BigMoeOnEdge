// JSON at the server's edges: parameter values in their form types, and engine metrics as events.
#pragma once

#include "bmoe/metrics.h"
#include "bmoe/params.h"

#include <nlohmann/json.hpp>

#include <string>

namespace bmoe::server {

using json = nlohmann::ordered_json;

// A form value (bool, number or string) as the string the parameter table's setter reads. False
// for a JSON type no parameter can hold (null, array, object).
bool value_to_param_string(const json & v, std::string & out);

// A parameter's rendered value in the JSON type its schema declares, so a form reads numbers as
// numbers. "auto" stays a string.
json param_string_to_value(const ParamDesc & d, const std::string & s);

// Every parameter's current value, keyed by parameter key.
json config_values(const RunConfig & cfg);

// `s` exactly as a client receives it: every response is serialized with invalid UTF-8 replaced
// by U+FFFD, so text the server compares against what a client sends back (the conversation it
// continues) must be held in that form. A generation can end inside a multi-byte character.
std::string utf8_as_sent(const std::string & s);

json token_json(const TokenMetrics & m);
json summary_json(const RunSummary & s, bool cancelled);

} // namespace bmoe::server
