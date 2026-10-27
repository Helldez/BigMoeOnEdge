# Byte-identity gates. The tiny synthetic models are generated at test time by
# scripts/make-tiny-moe.py (requires python3 with the gguf package); if it is
# unavailable the tests are skipped rather than failing the build. One model per expert
# layout the streamer supports: qwen3moe (split gate/up/down), gemma4 (fused gate_up) and
# nemotron_h_moe (gate-less up/down).

add_executable(bmoe_moe_gates moe_gates.cpp)
target_link_libraries(bmoe_moe_gates PRIVATE bmoe_core)
# G16 probes the loopback rpc-server port before registering it.
if(WIN32)
    target_link_libraries(bmoe_moe_gates PRIVATE ws2_32)
endif()
target_include_directories(bmoe_moe_gates PRIVATE ${CMAKE_SOURCE_DIR}/core/src/moe)

# A real Session::open with the full device confirmation and a page-aware fake mlock allowance.
# The test adapter object supplies the production platform_io symbols, so the static archive's
# copy is not selected; syscall substitutions never reach the production library.
if(UNIX AND NOT ANDROID)
    add_executable(bmoe_startup_pin_fit_test startup_pin_fit_test.cpp startup_pin_platform_io.cpp)
    target_link_libraries(bmoe_startup_pin_fit_test PRIVATE bmoe_core)
endif()

add_executable(bmoe_lock_limits_test lock_limits_test.cpp)
target_link_libraries(bmoe_lock_limits_test PRIVATE bmoe_core)
target_include_directories(bmoe_lock_limits_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src/io)
add_test(NAME lock_limits COMMAND bmoe_lock_limits_test)

# Reasoning-parser wiring regression (issue #49). Needs no model — it drives the extracted
# bmoe::detail::build_parse_params directly — so it runs unconditionally, unlike the gates below.
add_executable(bmoe_chat_parse_test chat_parse_test.cpp)
target_link_libraries(bmoe_chat_parse_test PRIVATE bmoe_core)
target_include_directories(bmoe_chat_parse_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src/engine)
target_compile_definitions(bmoe_chat_parse_test PRIVATE
    BMOE_QWEN3_TEMPLATE_PATH="${CMAKE_SOURCE_DIR}/third_party/llama.cpp/models/templates/Qwen3.5-4B.jinja")
add_test(NAME chat_parse COMMAND bmoe_chat_parse_test)

# Tool calls through the engine's chat types (bmoe/chat.h): tools into a real vendored template, a
# call parsed back out, the template's grammar holding a call to its syntax, and the next prompt
# extending the previous one. Needs no model: the grammar runs over llama.cpp's vendored vocab file.
add_executable(bmoe_chat_messages_test chat_messages_test.cpp)
target_link_libraries(bmoe_chat_messages_test PRIVATE bmoe_core)
target_include_directories(bmoe_chat_messages_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src/engine)
target_compile_definitions(bmoe_chat_messages_test PRIVATE
    BMOE_QWEN3_AUTO_TEMPLATE_PATH="${CMAKE_SOURCE_DIR}/third_party/llama.cpp/models/templates/Qwen-Qwen3-0.6B.jinja"
    BMOE_QWEN3_TEMPLATE_PATH="${CMAKE_SOURCE_DIR}/third_party/llama.cpp/models/templates/Qwen3.5-4B.jinja"
    BMOE_QWEN35_VOCAB_PATH="${CMAKE_SOURCE_DIR}/third_party/llama.cpp/models/ggml-vocab-qwen35.gguf")
add_test(NAME chat_messages COMMAND bmoe_chat_messages_test)

# The real Session path must apply request-local grammar to both token selection sites.
add_executable(bmoe_output_constraint_session_test output_constraint_session_test.cpp)
target_link_libraries(bmoe_output_constraint_session_test PRIVATE bmoe_core)
target_include_directories(bmoe_output_constraint_session_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src/engine)

# Thinking-control tests (issue #82). Drives bmoe::detail::probe_think_control and the prefill
# against real vendored templates, so it needs no model and runs unconditionally.
add_executable(bmoe_think_control_test think_control_test.cpp)
target_link_libraries(bmoe_think_control_test PRIVATE bmoe_core)
target_include_directories(bmoe_think_control_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src/engine)
set(_tmpl_dir "${CMAKE_SOURCE_DIR}/third_party/llama.cpp/models/templates")
target_compile_definitions(bmoe_think_control_test PRIVATE
    BMOE_TMPL_QWEN3="${_tmpl_dir}/Qwen3.5-4B.jinja"
    BMOE_TMPL_LFM25="${_tmpl_dir}/LFM2.5-8B-A1B.jinja"
    BMOE_TMPL_GPTOSS="${_tmpl_dir}/openai-gpt-oss-120b.jinja"
    BMOE_TMPL_LFM2="${_tmpl_dir}/LFM2-8B-A1B.jinja"
    BMOE_TMPL_LFM25_INSTRUCT="${_tmpl_dir}/LFM2.5-Instruct.jinja"
    BMOE_TMPL_GEMMA4="${_tmpl_dir}/google-gemma-4-31B-it.jinja")
add_test(NAME think_control COMMAND bmoe_think_control_test)

# validate() unit tests (issue #51 + the pre-existing rules). Pure config, no model — runs
# unconditionally, and makes good on config.h's "unit-tested" claim.
add_executable(bmoe_config_test config_test.cpp)
target_link_libraries(bmoe_config_test PRIVATE bmoe_core)
add_test(NAME config_validate COMMAND bmoe_config_test)

# The parameter table (bmoe/params.h): every flag the CLI used to parse by hand must set the same
# field to the same value, and to_args() must read back into the config it came from. Pure policy.
add_executable(bmoe_params_test params_test.cpp)
target_link_libraries(bmoe_params_test PRIVATE bmoe_core)
add_test(NAME params_table COMMAND bmoe_params_test)

# bmoe-server's policy: the chat protocol, the turn queue, the settings layers and the JSON edges.
# No HTTP, no session, no model.
if(TARGET bmoe_server_core)
    add_executable(bmoe_server_test server_test.cpp)
    target_link_libraries(bmoe_server_test PRIVATE bmoe_server_core)
    add_test(NAME server_logic COMMAND bmoe_server_test)
endif()

# bmoe-server's engine host: loads, turns, cancel and unload across threads, and which failures
# close the session, over a scripted session behind the host's seam (server/hosted_session.h).
# No model.
if(TARGET bmoe_server)
    add_executable(bmoe_engine_host_test engine_host_test.cpp)
    target_link_libraries(bmoe_engine_host_test PRIVATE bmoe_server)
    add_test(NAME engine_host COMMAND bmoe_engine_host_test)

    # The agent: its run loop over the engine host with a scripted session (approvals, denials, modes,
    # stop, the round limit, a short context, helpers, tasks), and the rules of its tools and stores.
    # No model, no network.
    add_executable(bmoe_agent_test agent_test.cpp)
    target_link_libraries(bmoe_agent_test PRIVATE bmoe_server)
    add_test(NAME agent COMMAND bmoe_agent_test)
endif()

# release_file_mappings: on Windows it must find and drop this process's own view + section of a
# file and leave the placeholders in place; on POSIX it must report unsupported and touch nothing.
add_executable(bmoe_mapping_release_test mapping_release_test.cpp)
target_link_libraries(bmoe_mapping_release_test PRIVATE bmoe_core)
target_include_directories(bmoe_mapping_release_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src/io)
add_test(NAME mapping_release COMMAND bmoe_mapping_release_test)

# FileReader: a read delivers every byte asked for or fails; a range past the end of the file is
# an error, buffered and direct (#348). A scratch file, no model.
add_executable(bmoe_file_reader_test file_reader_test.cpp)
target_link_libraries(bmoe_file_reader_test PRIVATE bmoe_core)
target_include_directories(bmoe_file_reader_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src/io)
add_test(NAME file_reader COMMAND bmoe_file_reader_test)

# The llama.cpp log bridge: DEBUG dropped unless asked, CONT following its message, a requested
# cancel's decode errors demoted while it is in flight. Drives the installed callback; no model.
add_executable(bmoe_llama_log_test llama_log_test.cpp)
target_link_libraries(bmoe_llama_log_test PRIVATE bmoe_core)
target_include_directories(bmoe_llama_log_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src/engine)
add_test(NAME llama_log COMMAND bmoe_llama_log_test)

# probe_devices against this build's real ggml registry: an accelerator (BLAS, AMX, ...) is left out
# of the profile by its device type, every other device is kept. No model needed.
add_executable(bmoe_device_probe_test device_probe_test.cpp)
target_link_libraries(bmoe_device_probe_test PRIVATE bmoe_core)
add_test(NAME device_probe COMMAND bmoe_device_probe_test)

# probe_bandwidth on the expert formats ggml cannot encode without importance weights (#321): the
# probe quantizes into the model's own type, and those formats used to abort `--auto`. Host only.
add_executable(bmoe_bandwidth_probe_test bandwidth_probe_test.cpp)
target_link_libraries(bmoe_bandwidth_probe_test PRIVATE bmoe_core)
add_test(NAME bandwidth_probe COMMAND bmoe_bandwidth_probe_test)

# The n-gram draft matcher. Pure policy over token ids (no llama.cpp, no model), so the drafting
# decision is covered exhaustively here; what it costs is a bench question, not a test.
add_executable(bmoe_ngram_test ngram_test.cpp)
target_link_libraries(bmoe_ngram_test PRIVATE bmoe_core)
add_test(NAME ngram_draft COMMAND bmoe_ngram_test)

# The interval-union stall clock. Injected timestamps, no wall clock, so overlap/nesting/
# separation and the open-interval snapshot rule are verified exactly (issue #98).
add_executable(bmoe_stall_union_test stall_union_test.cpp)
target_link_libraries(bmoe_stall_union_test PRIVATE bmoe_core)
target_include_directories(bmoe_stall_union_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src/moe)
add_test(NAME stall_union COMMAND bmoe_stall_union_test)

# How llama.cpp's tensor and graph-node names are read, and the fitter's expert-override patterns.
# Pure string rules over the conventions every reader shares, so they run without a model.
add_executable(bmoe_tensor_names_test tensor_names_test.cpp)
target_link_libraries(bmoe_tensor_names_test PRIVATE bmoe_core)
target_include_directories(bmoe_tensor_names_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src/moe)
add_test(NAME tensor_names COMMAND bmoe_tensor_names_test)

# Which tensors of a file are its routed experts, from their shapes and the declared expert count:
# every layout the gates run, the companions that share a name but not a rank, and the refusals.
# Pure, so it runs without a model; the gates then prove each layout streams byte-identical.
add_executable(bmoe_expert_layout_test expert_layout_test.cpp)
target_link_libraries(bmoe_expert_layout_test PRIVATE bmoe_core)
add_test(NAME expert_layout COMMAND bmoe_expert_layout_test)

# The CSV `# summary` trailer serialization. Drives the sink directly with a filled RunSummary —
# no model, no llama.cpp — so the emitted key names and units are pinned by a test rather than by
# whatever script last read them. The prefill keys must match BMOE_DONE's, token for token.
add_executable(bmoe_csv_summary_test csv_summary_test.cpp)
target_link_libraries(bmoe_csv_summary_test PRIVATE bmoe_core)
add_test(NAME csv_summary COMMAND bmoe_csv_summary_test)

add_executable(bmoe_csv_memory_test csv_memory_test.cpp)
target_link_libraries(bmoe_csv_memory_test PRIVATE bmoe_core)
target_include_directories(bmoe_csv_memory_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src/metrics)
add_test(NAME csv_memory COMMAND bmoe_csv_memory_test)

# The control-group remainder over fake hierarchies; plain files, so it runs on every POSIX host.
if(NOT WIN32)
    add_executable(bmoe_cgroup_memory_test cgroup_memory_test.cpp)
    target_link_libraries(bmoe_cgroup_memory_test PRIVATE bmoe_core)
    target_include_directories(bmoe_cgroup_memory_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src/io)
    add_test(NAME cgroup_memory COMMAND bmoe_cgroup_memory_test)
endif()

if(APPLE AND NOT IOS)
    add_executable(bmoe_process_memory_test process_memory_test.cpp ../core/src/io/platform_io.cpp)
    target_include_directories(bmoe_process_memory_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src/io)
    target_compile_definitions(bmoe_process_memory_test PRIVATE task_info=bmoe_test_task_info)
    add_test(NAME process_memory COMMAND bmoe_process_memory_test)
endif()

# decide()'s policy (the prompt split, choice checks and scoring, when the prefix state is
# restored, stored or left alone) over a scripted fake backend. No model, no llama.cpp, so it runs
# unconditionally; the same path on a real model is gate G18.
add_executable(bmoe_decide_test decide_test.cpp)
target_link_libraries(bmoe_decide_test PRIVATE bmoe_core)
target_include_directories(bmoe_decide_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src/engine/decide)
add_test(NAME decide_policy COMMAND bmoe_decide_test)

find_program(PYTHON3 NAMES python3 python)

# The overlap gates (G4, G5b, S2) need the fork's expert-ready hook, which a module build only finds
# at run time, in the CPU backend it loaded. By default they skip themselves where it is absent, so a
# build on stock upstream still passes its gates; the build whose job is to prove --overlap turns
# this on, and a missing hook is then a failure (CI's host-linux job).
option(BMOE_GATES_REQUIRE_HOOK "The byte-identity gates fail, instead of skipping, where the expert-ready hook is absent" OFF)
set(_gate_args)
if(BMOE_GATES_REQUIRE_HOOK)
    set(_gate_args --require-hook)
endif()

if(PYTHON3)
    # arch → (fixture id, gguf filename). Each gets a generate fixture + a gate run.
    foreach(spec "qwen3moe" "gemma4" "nemotron_h_moe")
        set(_model "${CMAKE_CURRENT_BINARY_DIR}/tiny-moe-${spec}.gguf")
        set(_fixture "TINY_MOE_MODEL_${spec}")

        add_test(
            NAME moe_make_tiny_model_${spec}
            COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/scripts/make-tiny-moe.py --arch ${spec} --out ${_model}
        )
        set_tests_properties(moe_make_tiny_model_${spec} PROPERTIES FIXTURES_SETUP ${_fixture})

        add_test(NAME moe_gates_${spec} COMMAND bmoe_moe_gates ${_model} ${_gate_args})
        set_tests_properties(moe_gates_${spec} PROPERTIES FIXTURES_REQUIRED ${_fixture})
    endforeach()

    add_test(NAME output_constraint_session COMMAND bmoe_output_constraint_session_test
             "${CMAKE_CURRENT_BINARY_DIR}/tiny-moe-qwen3moe.gguf")
    set_tests_properties(output_constraint_session PROPERTIES FIXTURES_REQUIRED TINY_MOE_MODEL_qwen3moe)

    if(TARGET bmoe_startup_pin_fit_test)
        add_test(NAME startup_pin_fit COMMAND bmoe_startup_pin_fit_test
                 "${CMAKE_CURRENT_BINARY_DIR}/tiny-moe-qwen3moe.gguf")
        set_tests_properties(startup_pin_fit PROPERTIES
                             FIXTURES_REQUIRED TINY_MOE_MODEL_qwen3moe SKIP_RETURN_CODE 77)
    endif()

    # The same qwen3moe fixture, written as a 4-shard split gguf (metadata-only first shard —
    # the layout large models actually ship in, since Hugging Face rejects files above 50 GB).
    # 51 tensors at 20 per shard is deterministic: -00001-of-00004 .. -00004-of-00004. The gate
    # binary gets the FIRST shard, exactly as a user would pass it; the streamer must resolve
    # every expert tensor to its (shard, offset) and produce byte-identical output.
    add_test(
        NAME moe_make_tiny_model_split
        COMMAND ${PYTHON3} ${CMAKE_SOURCE_DIR}/scripts/make-tiny-moe.py --arch qwen3moe
                --split-max-tensors 20 --out "${CMAKE_CURRENT_BINARY_DIR}/tiny-moe-split.gguf"
    )
    set_tests_properties(moe_make_tiny_model_split PROPERTIES FIXTURES_SETUP TINY_MOE_MODEL_split)

    add_test(NAME moe_gates_split
             COMMAND bmoe_moe_gates "${CMAKE_CURRENT_BINARY_DIR}/tiny-moe-split-00001-of-00004.gguf" ${_gate_args})
    set_tests_properties(moe_gates_split PROPERTIES FIXTURES_REQUIRED TINY_MOE_MODEL_split)
else()
    message(WARNING "python3 not found: MoE gates will not run (models cannot be generated)")
endif()

# The hardware planner. Pure policy over synthetic machine profiles — no model, no llama.cpp, no
# device — which is the point: rules meant to hold on hardware we do not own are checked here.
add_executable(bmoe_planner_test planner_test.cpp)
target_link_libraries(bmoe_planner_test PRIVATE bmoe_core)
add_test(NAME planner COMMAND bmoe_planner_test)

# The routing trace the planner prices the cache from: its LRU replay against scripts/route-replay.py
# on the archived phone traces, the curve, the hottest experts and the text form. Pure policy.
add_executable(bmoe_routing_test routing_test.cpp)
target_link_libraries(bmoe_routing_test PRIVATE bmoe_core)
target_compile_definitions(bmoe_routing_test PRIVATE
    BMOE_ROUTE_TRACE_DIR="${CMAKE_SOURCE_DIR}/docs/bench-data/2026-07-15-route-trace")
add_test(NAME routing COMMAND bmoe_routing_test)

add_executable(bmoe_memory_topology_test memory_topology_test.cpp)
target_link_libraries(bmoe_memory_topology_test PRIVATE bmoe_core)
target_include_directories(bmoe_memory_topology_test PRIVATE ${CMAKE_SOURCE_DIR}/core/src/plan)
if(APPLE AND GGML_METAL)
    target_compile_definitions(bmoe_memory_topology_test PRIVATE BMOE_HAVE_METAL_MEMORY_PROBE=1)
endif()
add_test(NAME memory_topology COMMAND bmoe_memory_topology_test)

# The cache brake's policy, replayed on measured per-token compute traces.
add_executable(bmoe_cache_brake_test cache_brake_test.cpp)
target_link_libraries(bmoe_cache_brake_test PRIVATE bmoe_core)
add_test(NAME cache_brake COMMAND bmoe_cache_brake_test)

# The knob trial's A/B policy (core/src/engine/knob_trial.h) on synthetic decode steps - noise, a real
# win, a tie inside the margin, drift, an outlier window, the cache settling - and the record its
# verdict is kept as (bmoe/measured_knobs.h). Pure policy, no model.
add_executable(bmoe_knob_trial_test knob_trial_test.cpp)
target_link_libraries(bmoe_knob_trial_test PRIVATE bmoe_core)
add_test(NAME knob_trial COMMAND bmoe_knob_trial_test)
