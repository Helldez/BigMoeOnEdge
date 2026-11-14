// What every front-end (bmoe-cli, bmoe-server and the apps that run it) tells the compute backends
// before any of them is loaded: one table, so every front-end starts them alike.
//
// Some of a ggml backend's behaviour can be chosen only through its environment, read when the
// backend loads; ggml's own default stands unless a measurement here said otherwise. Each entry names
// the backend that reads it and why, and only fills a variable the environment left unset: a value a
// person set stays. A variable is read only by its own backend, so an entry costs nothing on a build
// or a machine without that backend; an entry measured on one platform only (an operating system the
// same backend also runs on) applies there alone until it is measured on the others. What the table
// leaves at ggml's default, and which of those touch this engine's paths and are still to be measured,
// is in docs/backends.md. The engine library itself reads no environment (AGENTS.md, rule 3).
#pragma once

#include <string>
#include <vector>

namespace bmoe::frontend {

// One variable a backend reads, and the value the engine starts it with.
struct BackendDefault {
    const char * backend; // the ggml backend that reads it
    const char * name;
    std::string value;
    const char * why; // what was measured, in one line
    bool applies;     // whether it applies on this platform: false where it has not been measured
};

// Every default, with the values for a front-end whose data folder is `data_dir`.
std::vector<BackendDefault> backend_defaults(const std::string & data_dir);

// Set every default that applies here and the environment leaves unset. Called before any backend is loaded, and before
// any child process is started, so a device-probe child inherits them.
void prepare_compute_backends(const std::string & data_dir);

} // namespace bmoe::frontend
