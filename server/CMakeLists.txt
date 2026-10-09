# bmoe-server: the engine behind a local web UI (docs/server-api.md).
#
# A front-end like bmoe-cli: it composes the engine and owns everything core/ may not touch:
# HTTP, files, the platform. HTTP is cpp-httplib from llama.cpp's vendor tree, built here because
# llama.cpp only builds it for its own server, which stays off. No TLS: the server speaks plain
# HTTP on loopback, and downloads go through the system curl.

if(NOT TARGET cpp-httplib)
    add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/llama.cpp/vendor/cpp-httplib
                     ${CMAKE_BINARY_DIR}/vendor/cpp-httplib EXCLUDE_FROM_ALL)
endif()

# The server's own policy, with no HTTP, no session and no platform code, so tests/ can drive it:
# conversation continuity, the layered settings, the event fan-out and the JSON edges.
add_library(bmoe_server_core STATIC
    conversation.cpp
    event_bus.cpp
    json_util.cpp
    settings_store.cpp
)
target_link_libraries(bmoe_server_core PUBLIC bmoe_core)
target_include_directories(bmoe_server_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/llama.cpp/vendor)

add_executable(bmoe-server
    main.cpp
    engine_host.cpp
    model_library.cpp
    planner_adapter.cpp
    platform.cpp
)
target_link_libraries(bmoe-server PRIVATE bmoe_server_core cpp-httplib)
if(WIN32)
    target_link_libraries(bmoe-server PRIVATE ws2_32 shell32 ole32)
endif()

# The hardware planner is a library the engine may or may not carry. Detected by its header, the
# same way the overlap hook is detected by its own: the server offers /api/plan when it is there
# and says it is not otherwise, with nothing to configure.
if(EXISTS "${CMAKE_SOURCE_DIR}/core/include/bmoe/planner.h")
    target_compile_definitions(bmoe-server PRIVATE BMOE_HAVE_PLANNER=1)
    message(STATUS "bmoe-server: hardware planner present (/api/plan enabled)")
endif()

# Beside the libraries, so the build tree runs without touching PATH.
set_target_properties(bmoe-server PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The web UI build (ui/dist, when it exists) and the model catalog, staged beside the binary on
# every build: the server finds both relative to itself, so a build tree, a package and an install
# all look the same, and no source path is compiled into the binary.
add_custom_target(bmoe-server-assets ALL
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DDEST=$<TARGET_FILE_DIR:bmoe-server>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/stage_assets.cmake
    COMMENT "Staging the web UI and model catalog beside bmoe-server"
    VERBATIM)
add_dependencies(bmoe-server-assets bmoe-server)
