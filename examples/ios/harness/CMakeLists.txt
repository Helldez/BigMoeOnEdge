# bmoe-harness: bmoe-cli inside an iOS app bundle. See main.mm for why it is built this way.
#
# The bundle id, the team and the entitlements file are set in ../CMakeLists.txt.

enable_language(OBJCXX)

set(_cli_main "${PROJECT_SOURCE_DIR}/cli/main.cpp")

add_executable(bmoe-harness MACOSX_BUNDLE
    main.mm
    ${_cli_main}
)

# The CLI's entry point becomes an ordinary function the app calls; the app has its own main().
set_source_files_properties(${_cli_main} PROPERTIES COMPILE_DEFINITIONS "main=bmoe_cli_main")
set_source_files_properties(main.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

target_link_libraries(bmoe-harness PRIVATE
    bmoe_core
    "-framework UIKit"
    "-framework Foundation"
)

set_target_properties(bmoe-harness PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "bmoe"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${BMOE_IOS_BUNDLE_ID}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${BMOE_IOS_BUNDLE_ID}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${BMOE_IOS_TEAM}"
)

if(BMOE_IOS_ENTITLEMENTS)
    set_target_properties(bmoe-harness PROPERTIES
        XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${BMOE_IOS_ENTITLEMENTS}"
    )
endif()
