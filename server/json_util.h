// JSON at the server's edges: parameter values in their form types, and engine metrics as events.
#pragma once

#include "bmoe/metrics.h"
#include "bmoe/params.h"

#include <nlohmann/json.hpp>

#include <cstddef>
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

// True for a Content-Type header that names JSON: `application/json` in any case, with or without
// parameters (`; charset=utf-8`). The types an HTML form can send (urlencoded, multipart,
// text/plain) and a missing header are all false, which is the point of asking.
bool is_json_content_type(const std::string & content_type);

// Size of the longest well-formed UTF-8 prefix. A token may end partway through a code point;
// hold those bytes until later tokens complete it instead of replacing them during JSON dump.
size_t complete_utf8_prefix_size(const std::string & text);

json token_json(const TokenMetrics & m);
json summary_json(const RunSummary & s, bool cancelled);

} // namespace bmoe::server
