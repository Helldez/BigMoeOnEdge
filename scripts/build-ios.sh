#!/usr/bin/env bash
# Configure and build the iOS harness (examples/ios). Needs Xcode, not only the command line tools.
#
#   scripts/build-ios.sh                 the simulator, unsigned
#   scripts/build-ios.sh device          a device, signed by BMOE_IOS_TEAM
#
# Env overrides:
#   BUILD_DIR            (default build-ios-sim / build-ios-device)
#   BUILD_TYPE           (Release)
#   BMOE_IOS_TEAM        Apple development team id; required for a device
#   BMOE_IOS_BUNDLE_ID   (io.bigmoeonedge.harness) must be unique to the team on a device
#   BMOE_IOS_MEMORY_ENTITLEMENTS=ON   ask for the larger memory limit and address space
#   IOS_MIN_VERSION      (16.4, the oldest llama.cpp builds for)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="${1:-sim}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
IOS_MIN_VERSION="${IOS_MIN_VERSION:-16.4}"

cd "$ROOT"
if [ ! -f third_party/llama.cpp/CMakeLists.txt ]; then
    echo "llama.cpp submodule missing — run: git submodule update --init --recursive"
    exit 1
fi
if ! xcrun --sdk iphoneos --show-sdk-path >/dev/null 2>&1; then
    echo "no iOS SDK: install Xcode, then: sudo xcode-select -s /Applications/Xcode.app"
    exit 1
fi

case "$TARGET" in
    sim)
        BUILD_DIR="${BUILD_DIR:-build-ios-sim}"
        SYSROOT=iphonesimulator
        # The architecture of this machine: the simulator runs the host's own instruction set.
        ARCHS="$(uname -m)"
        SIGN=(-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO)
        ;;
    device)
        BUILD_DIR="${BUILD_DIR:-build-ios-device}"
        SYSROOT=iphoneos
        ARCHS=arm64
        if [ -z "${BMOE_IOS_TEAM:-}" ]; then
            echo "a device build is signed: set BMOE_IOS_TEAM to your Apple development team id"
            exit 1
        fi
        SIGN=(-DBMOE_IOS_TEAM="$BMOE_IOS_TEAM")
        ;;
    *)
        echo "usage: $0 [sim|device]"
        exit 1
        ;;
esac

# Static libraries, so the app is one executable with nothing to embed and sign beside it; the
# Metal shaders compiled in for the same reason. The rest is what the host builds already say.
cmake -S . -B "$BUILD_DIR" -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT="$SYSROOT" \
    -DCMAKE_OSX_ARCHITECTURES="$ARCHS" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_MIN_VERSION" \
    -DBUILD_SHARED_LIBS=OFF \
    -DBMOE_BUILD_TESTS=OFF \
    -DGGML_NATIVE=OFF \
    -DGGML_OPENMP=OFF \
    -DGGML_METAL_EMBED_LIBRARY=ON \
    -DBMOE_IOS_BUNDLE_ID="${BMOE_IOS_BUNDLE_ID:-io.bigmoeonedge.harness}" \
    -DBMOE_IOS_MEMORY_ENTITLEMENTS="${BMOE_IOS_MEMORY_ENTITLEMENTS:-OFF}" \
    "${SIGN[@]}"
if [ "$TARGET" = device ]; then
    cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" --target bmoe-harness -- -allowProvisioningUpdates
else
    cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" --target bmoe-harness
fi

APP="$(find "$BUILD_DIR" -maxdepth 4 -name bmoe-harness.app -type d | head -n 1)"
echo "built: $APP"
if [ "$TARGET" = sim ]; then
    echo "run:   xcrun simctl boot <device>; xcrun simctl install booted \"$APP\""
    echo "       see examples/ios/README.md"
fi
