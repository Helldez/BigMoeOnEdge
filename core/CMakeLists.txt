# bmoe_core — the streaming engine.
#
# Pure-policy sources (config validation, the architecture registry) build with no
# native dependency. The streaming adapters compile only when llama.cpp is present,
# because they include its public headers (llama.h, ggml.h, gguf.h).

add_library(bmoe_core STATIC
    src/config.cpp
    src/moe/arch_registry.cpp
)

target_include_directories(bmoe_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(bmoe_core PUBLIC cxx_std_17)

if(BMOE_HAVE_LLAMA)
    target_sources(bmoe_core PRIVATE
        src/io/platform_io.cpp
        src/moe/gguf_offsets.cpp
        src/moe/expert_stream_source.cpp
        src/moe/router_hook.cpp
        src/engine/runtime.cpp
        src/metrics/csv_metrics_sink.cpp
    )
    target_link_libraries(bmoe_core PUBLIC llama)
    target_compile_definitions(bmoe_core PUBLIC BMOE_HAVE_LLAMA=1)
    find_package(Threads REQUIRED)
    target_link_libraries(bmoe_core PUBLIC Threads::Threads)
endif()
