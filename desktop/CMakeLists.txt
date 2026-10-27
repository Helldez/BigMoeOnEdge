# bmoe-desktop: the native window around the web UI (docs/desktop-app.md). The system's own web
# view through the webview library (third_party/webview, header-only here): WebKit on macOS,
# WebView2 on Windows (its SDK headers fetched by the library's CMake), WebKitGTK on Linux. Built
# where those are found; a build without them still builds the server and the CLI.

# The rules apart from any window (app_core.h), for tests/desktop_test.cpp.
add_library(bmoe_desktop_core STATIC app_core.cpp)
target_include_directories(bmoe_desktop_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(bmoe_desktop_core SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/third_party/llama.cpp/vendor)
target_compile_features(bmoe_desktop_core PUBLIC cxx_std_17)

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/webview/CMakeLists.txt")
    message(STATUS "bmoe-desktop: third_party/webview is missing (git submodule update --init); not built")
    return()
endif()

set(BMOE_DESKTOP_OS_SOURCES "")
set(BMOE_DESKTOP_OS_LIBS "")
set(BMOE_DESKTOP_DEFS "")
if(APPLE)
    set(BMOE_DESKTOP_OS_SOURCES os_macos.mm)
    set(BMOE_DESKTOP_OS_LIBS "-framework AppKit" "-framework WebKit" "-framework UserNotifications")
elseif(WIN32)
    set(BMOE_DESKTOP_OS_SOURCES os_windows.cpp)
    set(BMOE_DESKTOP_OS_LIBS comctl32 shell32 ole32 user32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_search_module(BMOE_WEBKITGTK QUIET webkit2gtk-4.1 webkit2gtk-4.0)
        pkg_check_modules(BMOE_GTK3 QUIET gtk+-3.0)
        # The tray: only where the indicator library is there to build against. Without it closing
        # the window quits, as there is nowhere to keep the app.
        pkg_check_modules(BMOE_APPINDICATOR QUIET IMPORTED_TARGET ayatana-appindicator3-0.1)
    endif()
    if(NOT BMOE_WEBKITGTK_FOUND OR NOT BMOE_GTK3_FOUND)
        message(STATUS "bmoe-desktop: no WebKitGTK 4.1/4.0 with GTK 3 (libwebkit2gtk-4.1-dev); not built")
        return()
    endif()
    # The library takes the module it was found with rather than searching again.
    string(REGEX MATCH "^webkit2gtk-(4\\.[01])$" _api "${BMOE_WEBKITGTK_MODULE_NAME}")
    set(WEBVIEW_WEBKITGTK_API "${CMAKE_MATCH_1}")
    set(BMOE_DESKTOP_OS_SOURCES os_linux.cpp)
    if(BMOE_APPINDICATOR_FOUND)
        list(APPEND BMOE_DESKTOP_OS_LIBS PkgConfig::BMOE_APPINDICATOR)
        list(APPEND BMOE_DESKTOP_DEFS BMOE_HAVE_APPINDICATOR=1)
        message(STATUS "bmoe-desktop: tray through libayatana-appindicator3")
    else()
        message(STATUS "bmoe-desktop: no libayatana-appindicator3: no tray, closing the window quits")
    endif()
else()
    message(STATUS "bmoe-desktop: no web view for ${CMAKE_SYSTEM_NAME}; not built")
    return()
endif()

# The library header-only: none of its own builds, tests, docs or install rules.
set(WEBVIEW_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_STATIC_LIBRARY OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WEBVIEW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_ENABLE_PACKAGING OFF CACHE BOOL "" FORCE)
set(WEBVIEW_ENABLE_CHECKS OFF CACHE BOOL "" FORCE)
set(WEBVIEW_USE_COMPAT_MINGW OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/webview ${CMAKE_BINARY_DIR}/third_party/webview EXCLUDE_FROM_ALL)
# Upstream's header, upstream's warnings: included as a system header.
set_property(TARGET webview_core_headers PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
             ${CMAKE_SOURCE_DIR}/third_party/webview/core/include)

if(NOT TARGET cpp-httplib)
    add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/llama.cpp/vendor/cpp-httplib
                     ${CMAKE_BINARY_DIR}/vendor/cpp-httplib EXCLUDE_FROM_ALL)
endif()

# WIN32: a windowed program, with no console of its own.
add_executable(bmoe-desktop WIN32 main.cpp supervisor.cpp ${BMOE_DESKTOP_OS_SOURCES})
target_link_libraries(bmoe-desktop PRIVATE bmoe_desktop_core bmoe_server_platform cpp-httplib webview::core
                                           ${BMOE_DESKTOP_OS_LIBS})
target_include_directories(bmoe-desktop PRIVATE ${CMAKE_SOURCE_DIR}/core/include ${CMAKE_SOURCE_DIR}/server)
target_compile_definitions(bmoe-desktop PRIVATE BMOE_VERSION="${PROJECT_VERSION}" ${BMOE_DESKTOP_DEFS})
# Beside bmoe-server, which it starts from its own folder.
set_target_properties(bmoe-desktop PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(TARGET bmoe-server)
    add_dependencies(bmoe-desktop bmoe-server)
endif()
message(STATUS "bmoe-desktop: enabled")
