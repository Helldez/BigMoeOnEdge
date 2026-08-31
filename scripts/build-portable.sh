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
# Backends are still opt-in: pass -DGGML_CUDA=ON and friends for the ones whose SDK is on THIS
# machine. The point is not that one build has everything - it is that a build with three backends
# runs on a machine with one, and says which one it found.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-portable}"

cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DGGML_BACKEND_DL=ON \
    -DGGML_NATIVE=OFF \
    -DGGML_CPU_ALL_VARIANTS=ON \
    "$@"

cmake --build "$BUILD_DIR" -j

echo
echo "built: $BUILD_DIR/bin"
echo "the binary and its backends travel together - copy the whole directory, not the executable"
ls "$BUILD_DIR/bin" 2>/dev/null || ls "$BUILD_DIR/bin/Release" 2>/dev/null || true
echo
echo "what it found on THIS machine:"
"$BUILD_DIR/bin/bmoe-cli" --version 2>/dev/null || "$BUILD_DIR/bin/Release/bmoe-cli.exe" --version 2>/dev/null || true
