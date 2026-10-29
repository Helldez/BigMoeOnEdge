# BigMoeOnEdge for iOS: the five screens in SwiftUI over bmoe-server, which the app links and runs
# on a thread of its own (docs/application-framework.md). Swift sources from two places: BmoeKit
# (../BmoeKit, the client side with no UI, tested with `swift test` on any Mac) and Sources/ (the
# screens and the engine's life in the app). The bridge is the one Objective-C++ file.
#
# Only reached when the top-level project is configured for iOS (scripts/build-ios.sh).

enable_language(Swift)
enable_language(OBJCXX)

file(GLOB_RECURSE _kit_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../BmoeKit/Sources/BmoeKit/*.swift")
file(GLOB_RECURSE _app_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Sources/*.swift")

# The kit as a module of its own, so the app imports it as the tests do.
add_library(BmoeKit STATIC ${_kit_sources})
set_target_properties(BmoeKit PROPERTIES
    Swift_MODULE_NAME BmoeKit
    XCODE_ATTRIBUTE_SWIFT_VERSION "5.0"
)

# The chat's Markdown: MarkdownUI 2.4.1 (third_party/swift-markdown-ui, its Sources/MarkdownUI at
# tag 2.4.1, commit 5f613358148239d0292c0cef674a3c2314737f9e, without the DocC catalog; MIT) over
# swift-cmark 0.5.0 (third_party/swift-cmark, its src/ and extensions/ at tag 0.5.0, commit
# 3ccff77b2dc5b96b77db3da0d68d28068593fa53, the version MarkdownUI's own Package.resolved pins;
# BSD-2-Clause and others, COPYING). Both unmodified, built here because the app is a CMake target,
# not a SwiftPM package. MarkdownUI's NetworkImage dependency is left out: its two files that use
# it are swapped at build time for MarkdownUI/NoNetworkImageProviders.swift, so an answer that names
# an image fetches nothing.
enable_language(C)
set(_cmark "${PROJECT_SOURCE_DIR}/third_party/swift-cmark")
file(GLOB _cmark_sources CONFIGURE_DEPENDS "${_cmark}/src/*.c")
file(GLOB _cmark_ext_sources CONFIGURE_DEPENDS "${_cmark}/extensions/*.c")
# CMARK_THREADING as swift-cmark's Package.swift builds it: MarkdownUI parses on whatever thread
# SwiftUI renders on. Upstream's code, upstream's warnings.
add_library(bmoe_cmark_gfm STATIC ${_cmark_sources} ${_cmark_ext_sources})
target_include_directories(bmoe_cmark_gfm SYSTEM PUBLIC "${_cmark}/src/include" "${_cmark}/extensions/include")
target_compile_definitions(bmoe_cmark_gfm PUBLIC CMARK_THREADING)
target_compile_options(bmoe_cmark_gfm PRIVATE -w)

file(GLOB_RECURSE _mdui_sources CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/third_party/swift-markdown-ui/Sources/MarkdownUI/*.swift")
list(FILTER _mdui_sources EXCLUDE REGEX "/Default(Inline)?ImageProvider\\.swift$")
add_library(MarkdownUI STATIC ${_mdui_sources} "${CMAKE_CURRENT_SOURCE_DIR}/MarkdownUI/NoNetworkImageProviders.swift")
target_link_libraries(MarkdownUI PUBLIC bmoe_cmark_gfm)
set_target_properties(MarkdownUI PROPERTIES
    Swift_MODULE_NAME MarkdownUI
    XCODE_ATTRIBUTE_SWIFT_VERSION "5.0"
    # The module maps of cmark_gfm and cmark_gfm_extensions, which MarkdownUI imports
    # implementation-only, so the app never sees them.
    XCODE_ATTRIBUTE_SWIFT_INCLUDE_PATHS "$(inherited) ${_cmark}/src/include ${_cmark}/extensions/include"
    XCODE_ATTRIBUTE_OTHER_SWIFT_FLAGS "$(inherited) -Xcc -DCMARK_THREADING -suppress-warnings"
)

# What the app carries beside its code: the catalog the server lists, the logo the screens draw,
# and the faces every word is set in (Inter, JetBrains Mono: static instances of their official
# releases, with their SIL OFL licences beside them). Each from its one place in the repository.
file(GLOB _fonts CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Fonts/*.ttf"
                                   "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Fonts/OFL-*.txt")
set(_resources
    "${PROJECT_SOURCE_DIR}/catalog/models.json"
    "${PROJECT_SOURCE_DIR}/docs/assets/logo/icon.svg"
    ${_fonts}
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Assets.xcassets"
)

add_executable(bmoe-app MACOSX_BUNDLE
    ${_app_sources}
    Bridge/EngineBridge.mm
    ${_resources}
)
set_source_files_properties(${_resources} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
set_source_files_properties(Bridge/EngineBridge.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_include_directories(bmoe-app PRIVATE Bridge)

target_link_libraries(bmoe-app PRIVATE
    BmoeKit
    MarkdownUI
    bmoe_server
    "-framework UIKit"
    "-framework SwiftUI"
    "-framework Foundation"
)

set_target_properties(bmoe-app PROPERTIES
    OUTPUT_NAME "BigMoeOnEdge"
    RESOURCE "${_resources}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "BigMoeOnEdge"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${BMOE_IOS_APP_BUNDLE_ID}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${BMOE_IOS_APP_BUNDLE_ID}"
    XCODE_ATTRIBUTE_SWIFT_VERSION "5.0"
    # Where Xcode writes the kit's and MarkdownUI's modules: the app's Swift imports them from there.
    XCODE_ATTRIBUTE_SWIFT_INCLUDE_PATHS "$(inherited) ${CMAKE_CURRENT_BINARY_DIR}/$(CONFIGURATION)$(EFFECTIVE_PLATFORM_NAME)"
    XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/Bridge/BmoeApp-Bridging-Header.h"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${BMOE_IOS_TEAM}"
)

if(BMOE_IOS_ENTITLEMENTS)
    set_target_properties(bmoe-app PROPERTIES
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${BMOE_IOS_ENTITLEMENTS}"
    )
endif()
