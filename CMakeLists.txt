# BigMoeOnEdge — top-level build.
#
# The engine (`bmoe_core`) depends only on its own public headers under include/bmoe
# and on llama.cpp's PUBLIC C API (llama.h / ggml.h). It never touches llama.cpp
# internals: MoE expert streaming is driven entirely through the public eval-callback
# and public gguf accessors. That is what lets the llama.cpp submodule track upstream
# with nothing more than a pointer bump — there is no fork and no in-tree patch.
#
# llama.cpp is an optional dependency at configure time: when the submodule is absent
# only the pure-policy code (config validation) compiles, so the scaffold and a subset
# of CI stay green before the native dependency is fetched.
cmake_minimum_required(VERSION 3.21)

project(bigmoeonedge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(BMOE_BUILD_TESTS "Build byte-identity gates and unit tests" ON)
option(BMOE_BUILD_CLI   "Build the bmoe-cli host tool"              ON)

# --- Optional native dependency: llama.cpp ----------------------------------------
# The streaming seam uses only the public C API (llama.h). We additionally link llama.cpp's
# `common` layer for one thing: chat-template rendering + reasoning parsing (common_chat_*),
# so prompt formatting and thinking-extraction are template-driven per model instead of
# hardcoded. Unlike the streaming seam, `common` is NOT a stable API — it can change between
# upstream versions, so a submodule bump may need the chat glue in runtime.cpp updated (the
# gates catch it at build time). That trade-off is deliberate; see docs/seam.md.
set(BMOE_HAVE_LLAMA OFF)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/llama.cpp/CMakeLists.txt")
    set(BMOE_HAVE_LLAMA ON)
    # Upstream examples/tests/server stay off; build the common utils we link for chat.
    set(LLAMA_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER  OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_COMMON  ON  CACHE BOOL "" FORCE)
    set(LLAMA_CURL          OFF CACHE BOOL "" FORCE)  # common's model-download path unused
    add_subdirectory(third_party/llama.cpp EXCLUDE_FROM_ALL)
endif()

add_subdirectory(core)

if(BMOE_BUILD_CLI AND BMOE_HAVE_LLAMA)
    add_subdirectory(cli)
endif()

if(BMOE_BUILD_TESTS AND BMOE_HAVE_LLAMA)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "BigMoeOnEdge: llama.cpp present = ${BMOE_HAVE_LLAMA}")
