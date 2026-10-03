# Standalone diagnostics. These link the I/O layer directly rather than bmoe_core, because that
# layer has no llama.cpp dependency (only stdlib + OS headers) — so a tool builds in seconds and
# can be cross-compiled for a device without building the engine at all.
add_executable(bmoe-iobench
    iobench.cpp
    ${CMAKE_SOURCE_DIR}/core/src/io/file_reader.cpp
    ${CMAKE_SOURCE_DIR}/core/src/io/platform_io.cpp
)
target_include_directories(bmoe-iobench PRIVATE ${CMAKE_SOURCE_DIR}/core/src/io)
target_compile_features(bmoe-iobench PRIVATE cxx_std_17)
find_package(Threads REQUIRED)
target_link_libraries(bmoe-iobench PRIVATE Threads::Threads)

# Memory-side counterpart: bandwidth of the allocations the dense weights can live in. It links
# nothing at all beyond the stdlib — the question is about the allocator and the memory it returns,
# not about any bmoe code — so it also builds on the host, where the `anon` row still runs and the
# `ahwb` row reports itself unavailable.
add_executable(bmoe-membench membench.cpp)
target_compile_features(bmoe-membench PRIVATE cxx_std_17)
target_link_libraries(bmoe-membench PRIVATE Threads::Threads)
if(ANDROID)
    # AHardwareBuffer_* live in libandroid.so.
    target_link_libraries(bmoe-membench PRIVATE android)
endif()

# The one tool that breaks the no-llama rule above, deliberately: bmoe-rsbench reproduces the
# hybrid recurrent-state blockers (ADR-004) against the exact llama.cpp this engine links, so its
# verdicts describe OUR dependency, not some upstream build. Host-only diagnostic — gated on the
# llama.cpp being configured at all (BMOE_HAVE_LLAMA) and on BMOE_BUILD_TOOLS for the user.
if(BMOE_HAVE_LLAMA)
    add_executable(bmoe-rsbench rsbench.cpp)
    target_compile_features(bmoe-rsbench PRIVATE cxx_std_17)
    target_link_libraries(bmoe-rsbench PRIVATE llama)
endif()
