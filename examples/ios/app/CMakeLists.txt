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

# What the app carries beside its code: the catalog the server lists, the logo the screens draw,
# and the face the product speaks in. Each from its one place in the repository.
set(_resources
    "${PROJECT_SOURCE_DIR}/catalog/models.json"
    "${PROJECT_SOURCE_DIR}/docs/assets/logo/icon.svg"
    "${PROJECT_SOURCE_DIR}/docs/assets/fonts/PixelifySans.ttf"
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
    # Where Xcode writes the kit's module: the app's Swift imports it from there.
    XCODE_ATTRIBUTE_SWIFT_INCLUDE_PATHS "$(inherited) ${CMAKE_CURRENT_BINARY_DIR}/$(CONFIGURATION)$(EFFECTIVE_PLATFORM_NAME)"
    XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/Bridge/BmoeApp-Bridging-Header.h"
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${BMOE_IOS_TEAM}"
)

if(BMOE_IOS_MEMORY_ENTITLEMENTS)
    set_target_properties(bmoe-app PROPERTIES
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/../memory.entitlements"
    )
endif()
