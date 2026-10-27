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

# The release target this build is, as the feed names it (docs/desktop-app.md): which installer,
# archive and backend packs are this machine's. Empty where the releases ship nothing (a phone).
set(BMOE_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(BMOE_TARGET_ARCH "${CMAKE_OSX_ARCHITECTURES}")
endif()
string(TOLOWER "${BMOE_TARGET_ARCH}" BMOE_TARGET_ARCH)
if(BMOE_TARGET_ARCH MATCHES "^(x86_64|amd64|x64)$")
    set(BMOE_TARGET_ARCH x86_64)
elseif(BMOE_TARGET_ARCH MATCHES "^(arm64|aarch64)$")
    set(BMOE_TARGET_ARCH arm64)
else()
    set(BMOE_TARGET_ARCH "")
endif()
set(BMOE_BUILD_TARGET "")
if(BMOE_TARGET_ARCH AND NOT IOS AND NOT ANDROID)
    if(APPLE)
        set(BMOE_BUILD_TARGET "macos-${BMOE_TARGET_ARCH}")
    elseif(WIN32)
        set(BMOE_BUILD_TARGET "windows-${BMOE_TARGET_ARCH}")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # Linux names its 64-bit ARM the way the kernel does.
        string(REPLACE "arm64" "aarch64" BMOE_BUILD_TARGET "linux-${BMOE_TARGET_ARCH}")
    endif()
endif()

# The public key the release feed is checked against (server/feed_key.h). A fork that signs its own
# feed sets this to its own key's base64.
set(BMOE_FEED_PUBLIC_KEY "" CACHE STRING "Base64 Ed25519 public key the release feed is verified with (empty: server/feed_key.h's)")

# What the server and the desktop app ask the operating system (platform.h): no engine in it, so
# the app links it without the engine.
add_library(bmoe_server_platform STATIC platform.cpp)
target_include_directories(bmoe_server_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(bmoe_server_platform SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/third_party/llama.cpp/vendor)
target_compile_definitions(bmoe_server_platform PRIVATE BMOE_BUILD_TARGET="${BMOE_BUILD_TARGET}")
target_compile_features(bmoe_server_platform PUBLIC cxx_std_17)
if(WIN32)
    target_link_libraries(bmoe_server_platform PUBLIC shell32 ole32)
elseif(NOT APPLE)
    target_link_libraries(bmoe_server_platform PUBLIC ${CMAKE_DL_LIBS})
endif()

# Ed25519 verification for the feed's signature: Monocypher 4.0.2 (third_party/monocypher), its
# src/monocypher.{c,h} and src/optional/monocypher-ed25519.{c,h} as released in
# monocypher-4.0.2.tar.gz (SHA-512 bf275d4c53ff94af...dc084714, the one monocypher.org and the
# GitHub release both publish), unmodified, used for nothing else. Compiled as C++, which it is written to be, since this project
# enables no C compiler of its own.
set(BMOE_MONOCYPHER_SOURCES
    ${CMAKE_SOURCE_DIR}/third_party/monocypher/monocypher.c
    ${CMAKE_SOURCE_DIR}/third_party/monocypher/monocypher-ed25519.c)
set_source_files_properties(${BMOE_MONOCYPHER_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(bmoe_monocypher STATIC ${BMOE_MONOCYPHER_SOURCES})
target_include_directories(bmoe_monocypher SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/third_party/monocypher)

# The server's own policy, with no HTTP, no session and no platform code, so tests/ can drive it:
# the chat protocol, the line turns wait in, the layered settings, the event fan-out, the JSON
# edges and the plan as a person reads it (plan_view).
add_library(bmoe_server_core STATIC
    access.cpp
    agent_protocol.cpp
    anthropic_messages.cpp
    decide_protocol.cpp
    openai_chat.cpp
    openai_responses.cpp
    turn_queue.cpp
    event_bus.cpp
    json_util.cpp
    model_files.cpp
    download_policy.cpp
    sha256.cpp
    hf_repo.cpp
    url_entry.cpp
    curl_fetch.cpp
    model_delete.cpp
    settings_store.cpp
    plan_view.cpp
    agent_tools.cpp
    agent_skills.cpp
    agent_schedule.cpp
    agent_store.cpp
    provider_protocol.cpp
    connect.cpp
    feed.cpp
    backend_packs.cpp
    update_service.cpp
)
target_link_libraries(bmoe_server_core PUBLIC bmoe_core PRIVATE bmoe_monocypher)
target_include_directories(bmoe_server_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# The vendored libraries (cpp-httplib, nlohmann/json) as system headers: upstream's code, upstream's
# warnings.
target_include_directories(bmoe_server_core SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/third_party/llama.cpp/vendor)

# The whole server as a library (server.h): the engine host, the model library, the planner
# adapter, the platform and the HTTP API. The executable is main() over it; an app that may not
# start a process links it instead and calls run() on a thread.
add_library(bmoe_server STATIC
    http_server.cpp
    engine_host.cpp
    model_library.cpp
    model_inspect.cpp
    planner_adapter.cpp
    routing_store.cpp
    agent.cpp
    agent_routes.cpp
)
target_link_libraries(bmoe_server PUBLIC bmoe_server_core bmoe_server_platform PRIVATE cpp-httplib)
if(BMOE_FEED_PUBLIC_KEY)
    target_compile_definitions(bmoe_server PRIVATE BMOE_FEED_PUBLIC_KEY_OVERRIDE="${BMOE_FEED_PUBLIC_KEY}")
endif()
target_include_directories(bmoe_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(WIN32)
    target_link_libraries(bmoe_server PUBLIC ws2_32)
endif()

# The hardware planner is a library the engine may or may not carry. Detected by its header, the
# same way the overlap hook is detected by its own: the server offers /api/plan when it is there
# and says it is not otherwise, with nothing to configure.
if(EXISTS "${CMAKE_SOURCE_DIR}/core/include/bmoe/planner.h")
    target_compile_definitions(bmoe_server PRIVATE BMOE_HAVE_PLANNER=1)
    message(STATUS "bmoe-server: hardware planner present (/api/plan enabled)")
endif()

# On iOS an app links the library instead (examples/ios/app): no executable, nothing to stage.
if(IOS)
    return()
endif()

add_executable(bmoe-server main.cpp)
target_link_libraries(bmoe-server PRIVATE bmoe_server)

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
