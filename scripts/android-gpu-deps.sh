#!/usr/bin/env bash
# What the Vulkan and OpenCL backends need to cross-compile for Android, and the NDK does not carry.
#
# The NDK has libvulkan.so, glslc and the Vulkan C headers, but not the C++ bindings ggml-vulkan
# includes (vulkan.hpp), not SPIRV-Headers, and nothing of OpenCL. This script fetches the four
# Khronos repositories at pinned tags, builds the OpenCL ICD loader for the target and installs
# everything under one prefix:
#
#   <prefix>/include/vulkan/            Vulkan-Headers (C and C++)
#   <prefix>/share/cmake/SPIRV-Headers/ SPIRV-Headers' CMake package
#   <prefix>/include/CL/                OpenCL-Headers
#   <prefix>/lib/libOpenCL.so           the Khronos ICD loader, built with the NDK
#   <prefix>/cmake-flags                the cmake arguments that point a build at all of the above
#
# The ICD loader is a link-time stand-in and nothing more. The OpenCL backend imports libOpenCL.so,
# so its module needs one to link against; on the phone the module loads only where the system's
# own libOpenCL.so resolves (the app puts the phone's library on the linker path), and a phone
# without one simply has no OpenCL module loaded. Nothing from this prefix ships in the APK. (Loaded
# through the Khronos loader instead, a vendor library binds its own cl* calls to the loader's
# exports and the two deadlock at the first platform query, measured on a Snapdragon phone.)
#
#   scripts/android-gpu-deps.sh <prefix>        needs ANDROID_NDK_HOME or ANDROID_NDK_ROOT
#   ANDROID_PLATFORM=android-29                  (default: the APK's minimum)
#
# Then: cmake ... $(cat <prefix>/cmake-flags) -DGGML_VULKAN=ON -DGGML_OPENCL=ON
set -euo pipefail

PREFIX="${1:?usage: android-gpu-deps.sh <prefix>}"
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
[ -n "$NDK" ] && [ -d "$NDK" ] || { echo "set ANDROID_NDK_HOME (or ANDROID_NDK_ROOT) to the NDK"; exit 1; }
PLATFORM="${ANDROID_PLATFORM:-android-29}"

# One tag family per SDK release, so the headers and the shader tooling agree with each other; the
# OpenCL pair is one Khronos release. Bump the four together.
VULKAN_HEADERS_TAG=vulkan-sdk-1.4.357.0
SPIRV_HEADERS_TAG=vulkan-sdk-1.4.357.0
OPENCL_HEADERS_TAG=v2025.07.22
OPENCL_ICD_LOADER_TAG=v2025.07.22

mkdir -p "$PREFIX"
PREFIX="$(cd "$PREFIX" && pwd)"
SRC="$PREFIX/src"
mkdir -p "$SRC"

fetch() { # repo tag dir
    if [ ! -d "$3/.git" ]; then
        git clone --quiet --depth 1 --branch "$2" "https://github.com/KhronosGroup/$1" "$3"
    fi
}

fetch Vulkan-Headers    "$VULKAN_HEADERS_TAG"    "$SRC/Vulkan-Headers"
fetch SPIRV-Headers     "$SPIRV_HEADERS_TAG"     "$SRC/SPIRV-Headers"
fetch OpenCL-Headers    "$OPENCL_HEADERS_TAG"    "$SRC/OpenCL-Headers"
fetch OpenCL-ICD-Loader "$OPENCL_ICD_LOADER_TAG" "$SRC/OpenCL-ICD-Loader"

# Headers: host-side installs, nothing compiled. Ninja everywhere: the release container has no
# make, and the generator is otherwise whatever cmake defaults to.
cmake -S "$SRC/Vulkan-Headers" -B "$SRC/Vulkan-Headers/build" -G Ninja -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
cmake --install "$SRC/Vulkan-Headers/build" >/dev/null
cmake -S "$SRC/SPIRV-Headers" -B "$SRC/SPIRV-Headers/build" -G Ninja -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
cmake --install "$SRC/SPIRV-Headers/build" >/dev/null
cmake -S "$SRC/OpenCL-Headers" -B "$SRC/OpenCL-Headers/build" -G Ninja -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DBUILD_TESTING=OFF -DOPENCL_HEADERS_BUILD_TESTING=OFF -DOPENCL_HEADERS_BUILD_CXX_TESTS=OFF >/dev/null
cmake --install "$SRC/OpenCL-Headers/build" >/dev/null

# The loader: a target build with the NDK. No versioned soname (CMake's Android platform already
# refuses them), so the file is libOpenCL.so, the only name an APK extracts and the one the
# backend's import names.
cmake -S "$SRC/OpenCL-ICD-Loader" -B "$SRC/OpenCL-ICD-Loader/build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM="$PLATFORM" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib \
    -DOPENCL_ICD_LOADER_HEADERS_DIR="$PREFIX/include" \
    -DBUILD_TESTING=OFF -DOPENCL_ICD_LOADER_BUILD_TESTING=OFF >/dev/null
cmake --build "$SRC/OpenCL-ICD-Loader/build" >/dev/null
cmake --install "$SRC/OpenCL-ICD-Loader/build" >/dev/null
[ -f "$PREFIX/lib/libOpenCL.so" ] && [ ! -L "$PREFIX/lib/libOpenCL.so" ] ||
    { echo "the ICD loader did not produce a plain libOpenCL.so under $PREFIX/lib"; exit 1; }

# glslc runs on the build host; the NDK carries one per host.
case "$(uname -s)" in
    Darwin) HOST=darwin-x86_64 ;;
    Linux)  HOST=linux-x86_64 ;;
    *)      HOST=windows-x86_64 ;;
esac
GLSLC="$NDK/shader-tools/$HOST/glslc"
[ -x "$GLSLC" ] || GLSLC="$(command -v glslc || true)"
[ -n "$GLSLC" ] || { echo "no glslc: the NDK's shader-tools are missing and none is on PATH"; exit 1; }

# The NDK toolchain confines find_path, find_library and find_package to CMAKE_FIND_ROOT_PATH and
# appends the NDK to whatever the caller set, so the prefix listed first wins for the headers
# both have (vulkan/vulkan.h) and the NDK still supplies libvulkan.so. Programs are looked up on
# the host, hence glslc by path.
cat > "$PREFIX/cmake-flags" <<EOF
-DCMAKE_FIND_ROOT_PATH=$PREFIX
-DVulkan_GLSLC_EXECUTABLE=$GLSLC
-DOpenCL_INCLUDE_DIR=$PREFIX/include
-DOpenCL_LIBRARY=$PREFIX/lib/libOpenCL.so
EOF
echo "android GPU dependencies in $PREFIX:"
cat "$PREFIX/cmake-flags"
