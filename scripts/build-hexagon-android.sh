#!/usr/bin/env bash
# Build bmoe-cli with the Hexagon NPU backend, inside upstream's Snapdragon toolchain container
# (ghcr.io/snapdragon-toolchain/arm64-android, which carries the NDK and the Hexagon SDK; no
# Qualcomm account needed). Run from the host as:
#
#   docker run --rm -v <repo>:/workspace ghcr.io/snapdragon-toolchain/arm64-android:v0.7 \
#       bash /workspace/scripts/build-hexagon-android.sh
#
# From a git worktree whose third_party/llama.cpp is a Windows junction, the junction does not
# resolve inside the container: mount the real submodule over it with a second
# `-v <checkout>/third_party/llama.cpp:/workspace/third_party/llama.cpp`.
#
# Output in /workspace/build-hexagon: the CLI, the ggml/llama shared libs, libggml-hexagon.so, the
# DSP-side skel libggml-htp-v<arch>.so, and this NDK's libc++_shared.so (the one the libs were built
# against). scripts/stage-hexagon-jnilibs.ps1 puts them in the app.
set -euo pipefail

HTP_ARCH="${HTP_ARCH:-v81}"
FLAGS="-march=armv8.7a+fp16+dotprod+i8mm -fvectorize -ffp-model=fast -fno-finite-math-only -D_GNU_SOURCE"
OUT=/workspace/build-hexagon

cmake -S /workspace -B "$OUT" \
  -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-34 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS" \
  -DBMOE_BUILD_TESTS=OFF \
  -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DGGML_LLAMAFILE=OFF \
  -DGGML_HEXAGON=ON \
  -DHEXAGON_SDK_ROOT="$HEXAGON_SDK_ROOT" -DHEXAGON_TOOLS_ROOT="$HEXAGON_TOOLS_ROOT" \
  -DPREBUILT_LIB_DIR=android_aarch64 \
  -DLLAMA_CURL=OFF -DLLAMA_OPENSSL=OFF

# The DSP skel is an ExternalProject target, and the llama.cpp subdirectory is EXCLUDE_FROM_ALL here,
# so it is only built when asked for by name.
cmake --build "$OUT" --target bmoe-cli "htp-${HTP_ARCH}" -j "$(nproc)"

cp -f "$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$OUT/"
echo "built:"
find "$OUT" -maxdepth 4 \( -name 'bmoe-cli' -o -name 'lib*.so' \) -newer "$OUT/CMakeCache.txt" | sort
