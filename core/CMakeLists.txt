# bmoe_core — the streaming engine.
#
# Pure-policy sources (config validation, the architecture registry, the n-gram draft
# matcher, decide()'s policy) build with no native dependency. The streaming adapters compile only when
# llama.cpp is present, because they include its public headers (llama.h, ggml.h, gguf.h).

add_library(bmoe_core STATIC
    src/config.cpp
    src/moe/arch_registry.cpp
    src/engine/ngram_draft.cpp
    src/engine/logits.cpp
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
        src/moe/expert_stream_source.cpp
        src/moe/router_hook.cpp
        src/moe/device_arena.cpp
        src/engine/session.cpp
        src/engine/prefill_device.cpp
        src/engine/prefill_path.cpp
        src/engine/decide/llama_backend.cpp
        src/engine/chat_parse.cpp
        src/engine/thinking_control.cpp
        src/engine/runtime.cpp
        src/metrics/csv_metrics_sink.cpp
        src/metrics/route_trace_sink.cpp
        src/metrics/decode_trace_sink.cpp
    )
    # `llama` is the stable public API the streaming seam uses. `llama-common` (llama.cpp's
    # `common` utils) is linked only for chat-template rendering + reasoning parsing
    # (common_chat_*); it is not a stable API. It propagates the common/ and vendor/ includes.
    target_link_libraries(bmoe_core PUBLIC llama llama-common)
    target_compile_definitions(bmoe_core PUBLIC BMOE_HAVE_LLAMA=1)
    find_package(Threads REQUIRED)
    target_link_libraries(bmoe_core PUBLIC Threads::Threads)

    if(ANDROID)
        # libandroid carries AHardwareBuffer_*, the reclaim-exempt allocation behind
        # `--dense-weights ahwb` (pio::pinned_alloc). No other platform has an equivalent.
        target_link_libraries(bmoe_core PUBLIC android)
    endif()

    # Intra-layer I/O–compute overlap needs the fork's expert-ready hook in ggml-cpu.h. Detect
    # it by inspecting the submodule header rather than the git remote, so any tree carrying the
    # hook (a rebase, a different fork) enables the feature. PUBLIC so cli/ and tests/ see it.
    # Relative to this file, not CMAKE_SOURCE_DIR: as a subproject the top-level source dir is the
    # parent project, and the header would silently not be found.
    set(_ggml_cpu_h "${CMAKE_CURRENT_LIST_DIR}/../third_party/llama.cpp/ggml/include/ggml-cpu.h")
    if(EXISTS "${_ggml_cpu_h}")
        file(READ "${_ggml_cpu_h}" _ggml_cpu_src)
        if(_ggml_cpu_src MATCHES "ggml_cpu_set_expert_ready_hook")
            target_compile_definitions(bmoe_core PUBLIC BMOE_HAVE_EXPERT_READY_HOOK=1)
            message(STATUS "bmoe: expert-ready hook present (I/O–compute overlap available)")
        else()
            message(STATUS "bmoe: expert-ready hook absent (overlap unavailable)")
        endif()
    else()
        message(STATUS "bmoe: expert-ready hook absent (overlap unavailable)")
    endif()
endif()
