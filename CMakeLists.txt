# BigMoeOnEdge — top-level build.
#
# The engine (`bmoe_core`) depends only on its own public headers under include/bmoe
# and on llama.cpp's PUBLIC C API (llama.h / ggml.h). The serial streaming path never
# touches llama.cpp internals: MoE expert streaming is driven entirely through the public
# eval-callback and public gguf accessors, so it tracks upstream with nothing more than a
# submodule pointer bump — no in-tree patch. The one exception is the optional `--overlap`
# feature, which needs a per-expert wait point the public API does not expose: the submodule
# pins a fork branch on Helldez/llama.cpp, one commit on upstream, that adds one readiness hook
# to the CPU backend and lists its setter in that backend's `get_proc_address` table. The engine
# resolves it there at run time, so nothing here links against the fork: a build on stock
# upstream starts and refuses `--overlap` with the reason, and the hook sunsets the moment
# upstream ships an equivalent callback. See docs/seam.md § 3.
#
# llama.cpp is an optional dependency at configure time: when the submodule is absent
# only the pure-policy code (config validation) compiles, so the scaffold and a subset
# of CI stay green before the native dependency is fetched.
cmake_minimum_required(VERSION 3.21)

# The version is declared here and nowhere else in the build: the engine reports it (bmoe-cli
# --version, and the run-parameter preamble of every metrics CSV), so a committed benchmark file
# says which engine produced it. Keep it in step with CHANGELOG.md and the app's versionName:
# scripts/check-version.sh compares the three, in CI and against the tag before a release builds.
project(bigmoeonedge VERSION 0.37.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Everything runnable lands beside the libraries it needs. llama.cpp sets this for its own targets
# but only inside its subdirectory scope, so ours were landing elsewhere - which broke two things
# that look unrelated and are not. Running a test needed the library directory on PATH by hand; and
# `ggml_backend_load_all()` looks for dynamically loadable backends NEXT TO THE EXECUTABLE, so a
# GGML_BACKEND_DL build would have found none of them and reported a machine with no accelerator.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(BMOE_BUILD_TESTS "Build byte-identity gates and unit tests" ON)
option(BMOE_BUILD_CLI   "Build the bmoe-cli host tool"              ON)
option(BMOE_BUILD_SERVER "Build bmoe-server, the engine behind the local web UI" ON)
# Off by default: diagnostics, not part of the product. They need no llama.cpp, so they can be
# configured and cross-compiled on their own without building the engine.
option(BMOE_BUILD_TOOLS "Build standalone diagnostic tools (bmoe-iobench)" OFF)
# Every ggml backend as a module loaded from beside the executable at start-up, and the CPU backend
# once per instruction-set tier. The engine then knows no backend at compile time; the same layout
# ships on every OS; a module whose driver library is missing fails to load and is simply not
# there, instead of keeping the binary from starting; and the planner measures what registered
# (docs/backends.md). iOS is the exception the platform imposes: an app there is one signed
# executable, so its build stays linked.
option(BMOE_BACKEND_MODULES "Build the ggml backends as modules loaded from beside the executable" ON)
if(IOS)
    set(BMOE_BACKEND_MODULES OFF CACHE BOOL "" FORCE)
endif()

# --- Warnings and sanitizers ---------------------------------------------------------
# The warning level is this project's, applied to its own targets only (bmoe_apply_warnings, at
# the end of this file): llama.cpp keeps the flags upstream builds it with. Off by default so a
# newer compiler's new warning never stops a user's build; CI turns it on where the compiler is one
# the tree has been made clean on.
option(BMOE_WARNINGS_AS_ERRORS "Treat warnings in this project's own code as errors" OFF)
if(MSVC)
    set(BMOE_WARNING_FLAGS /W4)
    if(BMOE_WARNINGS_AS_ERRORS)
        list(APPEND BMOE_WARNING_FLAGS /WX)
    endif()
else()
    set(BMOE_WARNING_FLAGS -Wall -Wextra)
    if(BMOE_WARNINGS_AS_ERRORS)
        list(APPEND BMOE_WARNING_FLAGS -Werror)
    endif()
endif()

# A sanitizer is the opposite: the whole build, llama.cpp included, so ggml's reads of the buffers
# the streamer fills are checked too (and, under thread, so its threadpool's synchronisation is
# seen rather than reported as races). A comma-separated list; thread does not combine with address.
set(BMOE_SANITIZE "" CACHE STRING "Sanitizers for the whole build: address, undefined, thread (comma-separated)")
set_property(CACHE BMOE_SANITIZE PROPERTY STRINGS "" "address" "undefined" "address,undefined" "thread")
if(BMOE_SANITIZE)
    string(REPLACE "," ";" _bmoe_san_list "${BMOE_SANITIZE}")
    foreach(_san IN LISTS _bmoe_san_list)
        if(NOT _san MATCHES "^(address|undefined|thread)$")
            message(FATAL_ERROR "BMOE_SANITIZE: unknown sanitizer '${_san}' (address, undefined, thread)")
        endif()
    endforeach()
    if("thread" IN_LIST _bmoe_san_list AND "address" IN_LIST _bmoe_san_list)
        message(FATAL_ERROR "BMOE_SANITIZE: thread cannot be combined with address")
    endif()
    if(MSVC)
        if(NOT _bmoe_san_list STREQUAL "address")
            message(FATAL_ERROR "BMOE_SANITIZE: MSVC supports address only")
        endif()
        add_compile_options(/fsanitize=address)
    else()
        string(REPLACE ";" "," _bmoe_san "${_bmoe_san_list}")
        add_compile_options(-fsanitize=${_bmoe_san} -fno-omit-frame-pointer)
        add_link_options(-fsanitize=${_bmoe_san})
    endif()
    message(STATUS "BigMoeOnEdge: sanitizers = ${BMOE_SANITIZE}")
endif()

# --- Optional native dependency: llama.cpp ----------------------------------------
# The streaming seam uses only the public C API (llama.h). We additionally link llama.cpp's
# `common` layer for chat-template rendering + reasoning parsing (common_chat_*),
# speculative decoding, and template-generated grammars. Prompt formatting and reasoning
# extraction are template-driven per model instead of hardcoded. Unlike the streaming seam, `common` is NOT a stable API — it can change between
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
    # cpp-httplib links whatever OpenSSL the build machine has when this is on, and the release
    # archives ship no libssl: bmoe-server speaks plain HTTP on loopback, downloads go through
    # the system curl, so the binary must not depend on a TLS stack (#242).
    set(LLAMA_OPENSSL       OFF CACHE BOOL "" FORCE)
    if(BMOE_BACKEND_MODULES)
        set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
        set(GGML_BACKEND_DL   ON CACHE BOOL "" FORCE)
        # A module tuned to the build machine would be the one thing in the layout bound to it.
        set(GGML_NATIVE       OFF CACHE BOOL "" FORCE)
        # One CPU library per instruction-set tier, the best one picked at start-up. Not forced:
        # a target ggml has no tier list for passes OFF and gets the one baseline library. Apple
        # silicon is one family and gets that one library: its tiers differ in features the
        # compilers Apple ships with the CI images refuse to target. Windows on ARM has no tier
        # list in ggml (it stops the configure), so it gets the one library its toolchain targets.
        if(APPLE OR (WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$"))
            set(GGML_CPU_ALL_VARIANTS OFF CACHE BOOL "ggml: build all variants of the CPU backend")
        else()
            set(GGML_CPU_ALL_VARIANTS ON CACHE BOOL "ggml: build all variants of the CPU backend")
        endif()
        if(APPLE)
            set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "ggml: embed the Metal library")
        endif()
        # Everything a release copies sits in one directory, and the executables find the shared
        # libraries beside themselves wherever that directory is copied to.
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
        set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
        if(APPLE)
            set(CMAKE_INSTALL_RPATH "@loader_path")
        elseif(UNIX)
            # And, after it, any directory a toolkit installs its libraries to system-wide that a
            # module links against and a release does not carry (ROCm's, for the HIP module).
            set(BMOE_EXTRA_RPATH "" CACHE STRING "Directories after $ORIGIN in the binaries' run path")
            set(CMAKE_INSTALL_RPATH "$ORIGIN" ${BMOE_EXTRA_RPATH})
        endif()
    endif()
    # The prefill-device gate (G16) needs a second device to move weights onto, and the one every
    # host has is a loopback rpc-server fronting the CPU. A test fixture, not a feature: host test
    # builds only (releases build without tests), no front-end accepts an RPC endpoint, and it is
    # not forced: -DGGML_RPC=OFF still wins, and G16 then reports itself skipped.
    if(BMOE_BUILD_TESTS AND NOT ANDROID)
        set(GGML_RPC ON CACHE BOOL "ggml: use RPC")
    endif()
    # Apple: Metal is in every build. It is what gives the planner a device to measure and
    # `--prefill-device` one to run on; without it the binary reports a machine with no accelerator,
    # which on this hardware is never true. Carrying it costs a host-only run nothing, because a
    # device with nothing placed on it is not handed to llama.cpp (RunConfig::device_use). Said here
    # rather than left to llama.cpp's own default so that it is this project's decision; not forced,
    # so -DGGML_METAL=OFF still wins.
    if(APPLE)
        set(GGML_METAL ON CACHE BOOL "ggml: use Metal")
    endif()
    if(APPLE AND GGML_METAL)
        enable_language(OBJCXX)
    endif()
    add_subdirectory(third_party/llama.cpp EXCLUDE_FROM_ALL)
    # Upstream's headers are upstream's code: included as system headers, so a warning inside one
    # (common/'s jinja headers have unused static functions) is not reported against our sources.
    foreach(_t llama llama-common ggml ggml-base)
        if(TARGET ${_t})
            get_target_property(_inc ${_t} INTERFACE_INCLUDE_DIRECTORIES)
            if(_inc)
                set_property(TARGET ${_t} APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_inc}")
            endif()
        endif()
    endforeach()
endif()

add_subdirectory(core)

if(BMOE_BUILD_SERVER AND BMOE_HAVE_LLAMA)
    add_subdirectory(server)
endif()

# An app is the only thing that runs on iOS, so there the CLI and the server are built into apps:
# the harness carries the CLI's main.cpp, the app links the server library. No command-line binary.
if(BMOE_HAVE_LLAMA AND IOS)
    add_subdirectory(examples/ios)
elseif(BMOE_BUILD_CLI AND BMOE_HAVE_LLAMA)
    add_subdirectory(cli)
endif()

if(BMOE_BUILD_TESTS AND BMOE_HAVE_LLAMA)
    enable_testing()
    add_subdirectory(tests)
endif()

if(BMOE_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# Every compiled target a directory of ours declares gets the warning level. A directory whose
# source is in the submodule (llama.cpp, and the cpp-httplib server/ builds from its vendor tree) is
# skipped with everything under it, so those targets keep upstream's flags.
function(bmoe_apply_warnings dir)
    get_property(_src DIRECTORY ${dir} PROPERTY SOURCE_DIR)
    string(FIND "${_src}/" "${CMAKE_SOURCE_DIR}/third_party/" _pos)
    if(_pos EQUAL 0)
        return()
    endif()
    get_property(_targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _targets)
        get_target_property(_type ${_t} TYPE)
        if(_type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
            # The C-family sources only: the iOS app's Swift package takes no C compiler flag.
            target_compile_options(${_t} PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:${BMOE_WARNING_FLAGS}>")
        endif()
    endforeach()
    get_property(_subdirs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
    foreach(_sub IN LISTS _subdirs)
        bmoe_apply_warnings(${_sub})
    endforeach()
endfunction()
bmoe_apply_warnings(${CMAKE_CURRENT_SOURCE_DIR})

# --- Install ----------------------------------------------------------------------------
# The layout a release archive has: the executables, the shared libraries and backend modules they
# load from beside themselves, and the web UI and model catalog bmoe-server finds next to it, all in
# one directory (bin/ under the prefix). Component `bmoe`, because llama.cpp's own install rules
# (headers, its libraries under lib/) are not this layout: `cmake --install build --component bmoe`.
if(BMOE_HAVE_LLAMA AND NOT IOS AND (TARGET bmoe-cli OR TARGET bmoe-server))
    include(GNUInstallDirs)
    foreach(_exe bmoe-cli bmoe-server)
        if(TARGET ${_exe})
            install(TARGETS ${_exe} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT bmoe)
            set(_bmoe_bin_dir "$<TARGET_FILE_DIR:${_exe}>")
        endif()
    endforeach()
    # The libraries are whatever the build put beside the executables (the CPU module comes in one
    # file per instruction-set tier, the GPU modules per configuration), so the directory is the
    # list. Symlinks stay symlinks (libggml.so.0 -> libggml.so.0.x.y).
    if(BMOE_BACKEND_MODULES)
        install(DIRECTORY "${_bmoe_bin_dir}/" DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT bmoe
                USE_SOURCE_PERMISSIONS
                FILES_MATCHING PATTERN "*.so" PATTERN "*.so.*" PATTERN "*.dylib" PATTERN "*.dll"
                PATTERN "ui" EXCLUDE PATTERN "catalog" EXCLUDE)
    endif()
    if(TARGET bmoe-server)
        install(FILES catalog/models.json DESTINATION ${CMAKE_INSTALL_BINDIR}/catalog COMPONENT bmoe)
        # The web UI is built outside CMake (cd ui && npm ci && npm run build); installed when built.
        install(DIRECTORY ui/dist/ DESTINATION ${CMAKE_INSTALL_BINDIR}/ui COMPONENT bmoe OPTIONAL)
    endif()
endif()

message(STATUS "BigMoeOnEdge: llama.cpp present = ${BMOE_HAVE_LLAMA}")
