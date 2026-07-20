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
