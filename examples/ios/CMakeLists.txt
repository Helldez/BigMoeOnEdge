# The iOS builds: the app (app/, the five screens over bmoe-server running inside it) and the
# harness (harness/, bmoe-cli inside an app, the measuring instrument).
#
# Only reached when the top-level project is configured for iOS (scripts/build-ios.sh).

# A bundle identifier is unique per signing team, so a device build needs the builder's own.
set(BMOE_IOS_BUNDLE_ID "io.bigmoeonedge.harness" CACHE STRING "Bundle identifier of the iOS harness")
set(BMOE_IOS_APP_BUNDLE_ID "io.bigmoeonedge.app" CACHE STRING "Bundle identifier of the iOS app")
# The signing team, for a build that goes on a device. A simulator build needs none.
set(BMOE_IOS_TEAM "" CACHE STRING "Apple development team id that signs the iOS builds")
# Off by default: both entitlements have to be granted to the signing team, and a build that asks
# for one it was not granted does not sign at all.
option(BMOE_IOS_MEMORY_ENTITLEMENTS "Ask for the increased memory limit and the extended address space" OFF)

add_subdirectory(harness)
if(TARGET bmoe_server)
    add_subdirectory(app)
endif()
