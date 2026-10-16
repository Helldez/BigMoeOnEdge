#!/usr/bin/env bash
# Build bmoe-cli and bmoe-server with the Hexagon NPU, Vulkan and OpenCL backends, inside upstream's
# Snapdragon toolchain container
# (ghcr.io/snapdragon-toolchain/arm64-android, which carries the NDK and the Hexagon SDK; no
# Qualcomm account needed). This is the release recipe: .github/workflows/release-apk.yml runs it.
# From the host:
#
#   docker run --rm -v <repo>:/workspace ghcr.io/snapdragon-toolchain/arm64-android:v0.7 \
#       bash /workspace/scripts/build-hexagon-android.sh
#
# From a git worktree whose third_party/llama.cpp is a Windows junction, the junction does not
# resolve inside the container: mount the real submodule over it with a second
# `-v <checkout>/third_party/llama.cpp:/workspace/third_party/llama.cpp`.
#
# The CPU side is built exactly as the CPU-only release (scripts/build-android.ps1): same API level,
# same ARM target, so the decode, which stays on the CPU, is the same binary code on every phone,
# and the app still installs where it did. The Hexagon backend only registers where the phone has
# the fastrpc driver, and the engine only opens it when asked for the prefill.
#
# The GPU backends ride in the same build, so the one APK carries every backend a phone may have a
# device for and the planner measures whichever its drivers register (docs/hardware-planning.md).
# scripts/android-gpu-deps.sh fetches what the NDK does not carry for them - the Khronos headers
# and the OpenCL ICD loader, which is what lets a phone without OpenCL start the engine at all.
#
# Output in $OUT (default /workspace/build-hexagon): the CLI, the server, the ggml/llama shared
# libs, libggml-hexagon.so, one DSP-side skel libggml-htp-v<arch>.so per NPU generation (the
# backend loads the one matching the phone), libggml-vulkan.so, libggml-opencl.so with the
# libOpenCL.so loader it links, and this NDK's libc++_shared.so (the one the libs were built
# against). scripts/stage-hexagon-jnilibs.ps1 puts them in the app.
#
#   HTP_ARCHS  skels to build (default: every generation the backend supports, v73..v81)
#   CPU_ARCH   GGML_CPU_ARM_ARCH (default: the release's armv8.2-a+dotprod+fp16)
#   GPU_DEPS   where android-gpu-deps.sh puts the Vulkan and OpenCL dependencies ($OUT/gpu-deps)
set -euo pipefail

HTP_ARCHS="${HTP_ARCHS:-v73 v75 v79 v81}"
CPU_ARCH="${CPU_ARCH:-armv8.2-a+dotprod+fp16}"
OUT="${OUT:-/workspace/build-hexagon}"
GPU_DEPS="${GPU_DEPS:-$OUT/gpu-deps}"

ANDROID_NDK_HOME="$ANDROID_NDK_ROOT" bash "$(dirname "$0")/android-gpu-deps.sh" "$GPU_DEPS"

cmake -S /workspace -B "$OUT" \
  -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="-D_GNU_SOURCE" -DCMAKE_CXX_FLAGS="-D_GNU_SOURCE" \
  -DBMOE_BUILD_TESTS=OFF -DBMOE_BUILD_SERVER=ON \
  -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DGGML_LLAMAFILE=OFF \
  -DGGML_VULKAN=ON -DGGML_OPENCL=ON $(cat "$GPU_DEPS/cmake-flags") \
  -DGGML_CPU_ARM_ARCH="$CPU_ARCH" \
  -DGGML_HEXAGON=ON \
  -DHEXAGON_SDK_ROOT="$HEXAGON_SDK_ROOT" -DHEXAGON_TOOLS_ROOT="$HEXAGON_TOOLS_ROOT" \
  -DPREBUILT_LIB_DIR=android_aarch64 \
  -DLLAMA_CURL=OFF -DLLAMA_OPENSSL=OFF

# The DSP skels are ExternalProject targets, and the llama.cpp subdirectory is EXCLUDE_FROM_ALL here,
# so they are only built when asked for by name.
targets=(bmoe-cli bmoe-server)
for v in $HTP_ARCHS; do targets+=("htp-$v"); done
cmake --build "$OUT" --target "${targets[@]}" -j "$(nproc)"

cp -f "$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$OUT/"
echo "built:"
find "$OUT" -maxdepth 6 \( -name 'bmoe-cli' -o -name 'bmoe-server' -o -name 'lib*.so' \) -newer "$OUT/CMakeCache.txt" | sort
echo "$GPU_DEPS/lib/libOpenCL.so"
