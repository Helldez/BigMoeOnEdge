#!/usr/bin/env bash
# One binary that adapts to the machine it lands on.
#
# The default build links its backends in, so the binary is bound to the machine that compiled it:
# a CPU-only build has no accelerator wherever it runs, and a CUDA build needs CUDA present. This
# one builds the backends as SEPARATE SHARED LIBRARIES and lets ggml find them at start-up, which
# is how llama.cpp ships its own releases - `bmoe-cli` plus `ggml-cpu-*.dll`, `ggml-cuda.dll`,
# `ggml-vulkan.dll` beside it, and whatever is present is what the machine gets.
#
#   ./scripts/build-portable.sh [extra cmake flags...]
#   ./scripts/build-portable.sh -DGGML_VULKAN=ON -DGGML_CUDA=ON
#
# Three things this trades away, all of them real:
#
#   * `--overlap` is gone. It needs the fork's expert-ready hook, whose symbol lives in the CPU
#     backend; a backend loaded at runtime is not there to link against. Reaching it in this build
#     would mean exposing the hook through `ggml_backend_reg_get_proc_address`, the way ggml exposes
#     every other backend-specific function - a change to the fork commit, not to this script.
#   * `-march=native` is gone: GGML_NATIVE and GGML_BACKEND_DL are mutually exclusive upstream. In
#     its place GGML_CPU_ALL_VARIANTS builds one CPU library per instruction-set tier and ggml picks
#     the best at start-up. Portable by construction, and possibly a shade slower than a build tuned
#     for the exact machine.
#   * The layout matters. ggml looks for those libraries NEXT TO THE EXECUTABLE, so keep them
#     together when you copy the build somewhere; a lone binary silently reports a machine with no
#     accelerator on it.
#
# Backends whose SDK is present on THIS machine are turned on automatically, because leaving them
# all opt-in is how a build silently reports a machine with no accelerator: the planner enumerates
# only what was compiled in or found beside the binary, so a backend nobody asked for is a device no
# rule can ever consider. Anything detected here can still be overridden by passing the flag
# yourself - an explicit -DGGML_CUDA=OFF wins, since your flags come after these.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-portable}"

# Detected by looking for the toolchain each backend actually needs, not by guessing from the OS.
# A missing SDK leaves the backend off and the plan says the build carries none, which is the honest
# outcome; a present one costs a longer compile and gives the planner a device to weigh.
DETECTED=()
have() { command -v "$1" >/dev/null 2>&1; }

have nvcc && DETECTED+=(-DGGML_CUDA=ON) && echo "detected: CUDA (nvcc)"
have hipcc && DETECTED+=(-DGGML_HIP=ON) && echo "detected: ROCm (hipcc)"
have glslc && DETECTED+=(-DGGML_VULKAN=ON) && echo "detected: Vulkan (glslc)"
have icpx && DETECTED+=(-DGGML_SYCL=ON) && echo "detected: SYCL (icpx)"
[ "$(uname -s 2>/dev/null)" = "Darwin" ] && DETECTED+=(-DGGML_METAL=ON) && echo "detected: Metal"

if [ ${#DETECTED[@]} -eq 0 ]; then
    echo "detected: no accelerator SDK on this machine - building CPU-only."
    echo "  That is a property of THIS build: the binary will report no accelerator wherever it runs."
fi

cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DGGML_BACKEND_DL=ON \
    -DGGML_NATIVE=OFF \
    -DGGML_CPU_ALL_VARIANTS=ON \
    "${DETECTED[@]}" \
    "$@"

cmake --build "$BUILD_DIR" -j

echo
echo "built: $BUILD_DIR/bin"
echo "the binary and its backends travel together - copy the whole directory, not the executable"
ls "$BUILD_DIR/bin" 2>/dev/null || ls "$BUILD_DIR/bin/Release" 2>/dev/null || true
echo
echo "what it found on THIS machine:"
"$BUILD_DIR/bin/bmoe-cli" --version 2>/dev/null || "$BUILD_DIR/bin/Release/bmoe-cli.exe" --version 2>/dev/null || true
