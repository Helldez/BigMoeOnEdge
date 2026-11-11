// What a test of the server's own policy puts in place of the engine and the planner, so the HTTP
// path runs on a real socket with no model (tests/server_app_test.cpp). Handed to run() through
// Embedding::seams; every field left empty is the real one. Kept out of server.h, which an app that
// links the server includes and which stays standard-library-only.
#pragma once

#include "hosted_session.h"
#include "idle_timer.h"
#include "planner_adapter.h"

#include <atomic>
#include <chrono>
#include <functional>

namespace bmoe::server {

struct ServerSeams {
    // Opens what a load installs (hosted_session.h).
    SessionOpener open_session;
    // Makes an auto load's plan of a config (make_plan).
    std::function<PlanOutcome(const RunConfig & cfg)> plan;
    // Runs a load-time calibration of a plan from a record, stopping when `stop` is raised
    // (calibrate_plan_outcome).
    std::function<CalibrationOutcome(const PlanOutcome & plan,
                                     const MeasuredKnobs & start,
                                     const std::atomic<bool> & stop,
                                     const PlanStepFn & on_step)>
        calibrate;
    // How long the server is idle after an answer before it calibrates; zero: the server's own.
    std::chrono::milliseconds calibration_idle_delay{0};
    // Hears the port the server listens on as soon as it is bound, before any slow start-up work (with
    // --port 0, the one the system chose).
    std::function<void(int port)> bound;
    // Load the compute backends at start. A test with a scripted engine computes nothing, and leaves
    // them out so its start does not wait on the machine's driver (a GPU library compiled at load).
    bool load_backends = true;
    // Hears every turn's end, with whether a calibration was armed then: what a test waits for to
    // know the server has looked, instead of waiting for time to pass.
    std::function<void(bool armed)> turn_ended;
};

} // namespace bmoe::server
