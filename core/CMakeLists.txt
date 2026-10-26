# bmoe_core — the streaming engine.
#
# Pure-policy sources (config validation, the expert layout read from tensor shapes, tensor naming, the n-gram draft
# matcher, the hardware planner, decide()'s policy) build with no native dependency. The streaming
# adapters compile only when llama.cpp is present, because they include its public headers
# (llama.h, ggml.h, gguf.h).

add_library(bmoe_core STATIC
    src/config.cpp
    src/params.cpp
    src/log.cpp
    src/moe/expert_layout.cpp
    src/moe/tensor_names.cpp
    src/engine/ngram_draft.cpp
    src/plan/plan.cpp
    src/plan/allocate.cpp
    src/plan/planner.cpp
    src/plan/routing.cpp
    src/engine/logits.cpp
    src/engine/cache_brake.cpp
    src/engine/decide/prompt_split.cpp
    src/engine/decide/choice_scorer.cpp
    src/engine/decide/prefix_cache.cpp
    src/engine/decide/decider.cpp
)

target_include_directories(bmoe_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(bmoe_core PUBLIC cxx_std_17)

# One source of truth for the version: the CMake project version reaches the code only here.
target_compile_definitions(bmoe_core PUBLIC BMOE_VERSION="${PROJECT_VERSION}")

if(BMOE_HAVE_LLAMA)
    target_sources(bmoe_core PRIVATE
        src/io/platform_io.cpp
        src/io/file_reader.cpp
        src/io/mapping_release.cpp
        src/moe/dense_weights.cpp
        src/moe/row_stream.cpp
        src/moe/gguf_offsets.cpp
        src/plan/hardware_probe.cpp
        src/plan/headroom_probe.cpp
        src/plan/bandwidth_probe.cpp
        src/plan/gate_probe.cpp
        src/plan/device_probe.cpp
        src/plan/isolated_probe.cpp
        src/plan/storage_probe.cpp
        src/plan/placement_probe.cpp
        src/plan/model_probe.cpp
        src/plan/auto_plan.cpp
        src/moe/expert_stream_source.cpp
        src/moe/gate_gemv.cpp
        src/moe/router_hook.cpp
        src/moe/decide_probe.cpp
        src/moe/device_arena.cpp
        src/engine/session.cpp
        src/engine/context_checkpoints.cpp
        src/engine/llama_log.cpp
        src/engine/prefill_device.cpp
        src/engine/prefill_path.cpp
        src/engine/decide/llama_backend.cpp
        src/engine/chat_parse.cpp
        src/engine/chat_messages.cpp
        src/engine/thinking_control.cpp
        src/engine/runtime.cpp
        src/metrics/csv_metrics_sink.cpp
        src/metrics/memory_snapshot.cpp
        src/metrics/route_trace_sink.cpp
        src/metrics/decode_trace_sink.cpp
    )
    # `llama` is the stable public API the streaming seam uses. `llama-common` (llama.cpp's
    # `common` utils) supplies chat templates/parsing, speculative decoding and template-generated
    # grammars; it is not a stable API. It propagates the common/ and vendor/ includes.
    target_link_libraries(bmoe_core PUBLIC llama llama-common)
    target_compile_definitions(bmoe_core PUBLIC BMOE_HAVE_LLAMA=1)
    find_package(Threads REQUIRED)
    target_link_libraries(bmoe_core PUBLIC Threads::Threads)

    if(APPLE AND GGML_METAL)
        target_sources(bmoe_core PRIVATE src/plan/metal_memory_probe.mm)
        set_source_files_properties(src/plan/metal_memory_probe.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_compile_definitions(bmoe_core PRIVATE BMOE_HAVE_METAL_MEMORY_PROBE=1)
        target_link_libraries(bmoe_core PUBLIC "-framework Metal" "-framework Foundation")
    endif()

    if(ANDROID)
        # libandroid carries AHardwareBuffer_*, Android's reclaim-exempt allocation behind
        # `--dense-weights pinned` (pio::pinned_alloc; Darwin's needs no extra library).
        target_link_libraries(bmoe_core PUBLIC android)
    endif()
    if(WIN32)
        # QueryWorkingSetEx, behind the dense residency sensor (pio::vm_resident_sample). Newer SDKs
        # resolve it from kernel32; the import library covers the ones that do not.
        target_link_libraries(bmoe_core PUBLIC psapi)
    endif()

    # Intra-layer I/O–compute overlap needs the fork's expert-ready hook in the CPU backend. It is
    # not detected here: the engine asks the CPU backend's registry for it at run time
    # (`ggml_backend_reg_get_proc_address`), which is what lets the backend be a module loaded
    # from beside the executable, and lets a build on stock upstream run with overlap refused
    # rather than fail to link. See bmoe/probe.h (expert_ready_hook_available) and docs/seam.md.
endif()
