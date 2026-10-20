# The iOS builds: the app (app/, the five screens over bmoe-server running inside it) and the
# harness (harness/, bmoe-cli inside an app, the measuring instrument).
#
# Only reached when the top-level project is configured for iOS (scripts/build-ios.sh).

# A bundle identifier is unique per signing team, so a device build needs the builder's own.
set(BMOE_IOS_BUNDLE_ID "io.bigmoeonedge.harness" CACHE STRING "Bundle identifier of the iOS harness")
set(BMOE_IOS_APP_BUNDLE_ID "io.bigmoeonedge.app" CACHE STRING "Bundle identifier of the iOS app")
# The signing team, for a build that goes on a device. A simulator build needs none.
set(BMOE_IOS_TEAM "" CACHE STRING "Apple development team id that signs the iOS builds")
# Off by default: an entitlement has to be granted to the signing team, and a build that asks for
# one it was not granted does not sign at all. The memory limit is granted to a free Personal Team
# and is what a streamed 35B needed on a 12 GB phone (#235); the extended address space is not
# granted to one, so it is asked for separately.
option(BMOE_IOS_MEMORY_ENTITLEMENTS "Ask for the increased memory limit" OFF)
option(BMOE_IOS_EXTENDED_ADDRESSING "Also ask for the extended address space (not granted to a Personal Team)" OFF)
# The one place that picks the entitlements file, so the app and the harness ask for the same.
if(BMOE_IOS_EXTENDED_ADDRESSING)
    set(BMOE_IOS_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/memory-extended.entitlements")
elseif(BMOE_IOS_MEMORY_ENTITLEMENTS)
    set(BMOE_IOS_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/memory.entitlements")
else()
    set(BMOE_IOS_ENTITLEMENTS "")
endif()

add_subdirectory(harness)
if(TARGET bmoe_server)
    add_subdirectory(app)
endif()
