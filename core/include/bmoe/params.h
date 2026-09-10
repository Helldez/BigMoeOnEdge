// The engine's tunables, described once.
//
// Every RunConfig field a person can set is one row of one table: its key, type, bounds, group,
// help text, and how to read and write it. The CLI parses its flags from the table and prints its
// usage from it; a front-end renders its settings form from params_json(). Adding a knob is one row
// here, instead of a flag parser, a usage line, a UI control and a settings-to-argv mapping kept in
// step by hand — the arrangement that let the Android app's settings drift from the engine.
//
// Keys are the long CLI flag without its dashes, which is also the name a planner Decision uses for
// the knob it decided, so a plan and a settings form line up without a mapping table. A flag that
// only turns something off (--no-odirect) gets the positive key (o-direct) and is a switch on it.
//
// The bounds are what a form offers, written with the same named constants validate() checks, so
// they cannot drift apart silently. validate() stays the authority: rules that relate two knobs
// (prefetch needs the cache, route-ahead excludes speculation) live there only, and a front-end
// asks it rather than re-deriving them.
//
// Not in the table, because they are not tunables of a run: the prompt (a request's payload), the
// line-protocol switch (`progress`, an output mode of the CLI) and the compute-trace granularity
// (a property of a diagnostics sink). Front-ends own those.
//
// Pure policy: no llama.cpp, no I/O, no environment.
#pragma once

#include "bmoe/config.h"

#include <functional>
#include <string>
#include <vector>

namespace bmoe {

enum class ParamType { Bool, Int, Float, Choice, Path };

// Groups in the order a settings form shows them.
enum class ParamGroup {
    Model,
    Generation,
    Sampling,
    Streaming,
    Cache,
    Memory,
    Prefetch,
    Speculation,
    Lossy,
    Diagnostics
};

// How far from the defaults a person has to be to want this knob.
enum class ParamLevel {
    Basic,        // what anyone running a model sets
    Advanced,     // measured levers with a known trade-off
    Experimental, // opt-in, pending an A/B on the target device
    Debug,        // tests, diagnostics and A/B baselines; never a speed setting
};

// When a change takes effect in a running session.
enum class ParamScope {
    Request, // per generation (GenerateRequest): no reload
    Session, // fixed when the session opens: changing it reopens the session
};

struct ParamChoice {
    std::string value; // as typed on the command line and stored by a front-end
    std::string label; // as shown in a form
};

// A bare flag that sets its parameter to a fixed value: --no-odirect sets o-direct=false, --mtp sets
// spec=mtp. A deprecated switch still parses but is left out of the usage and of to_args().
struct ParamSwitch {
    std::string flag;
    std::string value;
    bool deprecated = false;
};

struct ParamDesc {
    std::string key;   // stable identifier: the long flag without dashes
    std::string label; // short name for a form
    ParamType type = ParamType::Int;
    ParamGroup group = ParamGroup::Generation;
    ParamLevel level = ParamLevel::Advanced;
    ParamScope scope = ParamScope::Session;
    std::string help; // one paragraph, in the user's terms

    std::string flag;       // "--cache-mb": takes a value. Empty when the parameter has only switches
    std::string short_flag; // "-m", or empty
    std::string value_hint; // "N", "F", "PATH", "N|auto", "mmap|warm|..." for the usage line
    std::vector<ParamSwitch> switches;
    std::vector<ParamChoice> choices; // Choice only

    // What a form offers; validate() decides what a run accepts. Unbounded when bounded is false.
    bool bounded = false;
    double min = 0.0;
    double max = 0.0;
    std::string unit; // "MiB", "tokens", "" ...

    bool lossy = false;              // changes the output: a quality trade, not an optimisation
    bool accepts_auto = false;       // an Int that also takes the literal "auto"
    bool exclusive_switches = false; // two different switches on one command line are a contradiction

    // Read the parameter as the string a flag would carry; write it from one. `set` rejects a
    // malformed value with a reason and leaves the config untouched.
    std::function<std::string(const RunConfig &)> get;
    std::function<bool(RunConfig &, const std::string &, std::string & err)> set;
};

// The table, in form order. Built once; the reference stays valid for the process lifetime.
const std::vector<ParamDesc> & params();

// By key, or nullptr.
const ParamDesc * find_param(const std::string & key);

const char * param_type_name(ParamType t);
const char * param_group_name(ParamGroup g);  // stable identifier, as in params_json()
const char * param_group_label(ParamGroup g); // section title for a form or a usage text
const char * param_level_name(ParamLevel l);
const char * param_scope_name(ParamScope s);

// Match one command-line token against the table. `value` is the token after it, or nullptr when
// there is none. `matched` is false for a token the table does not know (a front-end flag), which
// the caller then handles itself.
struct FlagResult {
    bool matched = false;
    bool consumed_value = false; // the caller must skip `value`
    const ParamDesc * param = nullptr;
    const ParamSwitch * via_switch = nullptr; // set when a bare switch matched
    std::string error;                        // non-empty: matched, but the value was rejected
};
FlagResult apply_flag(RunConfig & cfg, const char * arg, const char * value);

// The config as command-line arguments: every parameter that differs from a default RunConfig, in
// table order, in the spelling apply_flag() reads back. Parsing the result into a default config
// reproduces `cfg` for every parameter the table describes — the property a "copy as command"
// button and a bench cell both depend on, and which the tests check.
std::vector<std::string> to_args(const RunConfig & cfg);

// The table as a JSON array, one object per parameter, with `default` read from `defaults` — so a
// front-end that resolves its own defaults (the CLI turns the cache on with streaming) can say so.
std::string params_json(const RunConfig & defaults);

// The current value of every parameter as a JSON object keyed by parameter key.
std::string config_json(const RunConfig & cfg);

} // namespace bmoe
