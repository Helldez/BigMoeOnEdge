#!/usr/bin/env bash
# Cross-compile BigMoeOnEdge for ARM64 GNU/Linux and stage a self-contained bundle.
#
# Needs the aarch64-linux-gnu GCC toolchain (Ubuntu: g++-15-aarch64-linux-gnu or
# gcc-aarch64-linux-gnu). CPU baseline matches the Android build: armv8.2-a + dotprod + fp16,
# deliberately NOT i8mm — a build that pins i8mm SIGILLs during prefill on pre-2021 SoCs
# (e.g. Snapdragon 865), while dotprod + fp16 covers every SoC that can realistically run a
# >RAM MoE model. See docs/serve.md.
#
# The bundle is bmoe-arm64/: bmoe-cli + the shared libs it links, RUNPATH $ORIGIN/lib, so it
# runs from any directory with no LD_LIBRARY_PATH. Pass --tar to also write bmoe-arm64.tar.gz.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build-arm64}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
MAKE_TAR=0
[ "${1:-}" = "--tar" ] && MAKE_TAR=1

if ! command -v aarch64-linux-gnu-g++ > /dev/null; then
    echo "aarch64-linux-gnu-g++ not found — install the cross toolchain (Ubuntu:" \
         "apt install g++-15-aarch64-linux-gnu)" >&2
    exit 1
fi

TOOLCHAIN="$(mktemp /tmp/bmoe-aarch64-toolchain.XXXXXX.cmake)"
trap 'rm -f "$TOOLCHAIN"' EXIT
cat > "$TOOLCHAIN" <<'EOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_FIND_ROOT_PATH /usr/aarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF

cd "$ROOT"
cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_RPATH='$ORIGIN/lib' \
    -DCMAKE_BUILD_RPATH='$ORIGIN/lib' \
    -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=FALSE \
    -DBMOE_BUILD_TESTS=OFF \
    -DGGML_NATIVE=OFF \
    -DGGML_OPENMP=OFF \
    -DGGML_CPU_ARM_ARCH="armv8.2-a+dotprod+fp16" \
    -DLLAMA_CURL=OFF
cmake --build "$BUILD_DIR" -j "$JOBS"

# Stage the bundle: the CLI plus every lib*.so* the build produced. The explicit glob mirrors
# the Android script's staging list philosophy — the CLI's NEEDED entries resolve against
# $ORIGIN/lib, so anything the build emits for it belongs in the bundle, nothing else does.
BUNDLE="$ROOT/bmoe-arm64"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/lib"
cp "$BUILD_DIR/cli/bmoe-cli" "$BUNDLE/"
cp "$BUILD_DIR"/bin/lib*.so* "$BUNDLE/lib/"
# The bridge rides along so the bundle serves agent tooling standalone — no repo checkout
# needed on the target; the script resolves the engine to the bmoe-cli sitting beside it.
cp "$ROOT/scripts/bmoe-serve.py" "$BUNDLE/"
cat > "$BUNDLE/README.md" <<'EOF'
# bmoe-arm64 — portable ARM64 Linux bundle

`bmoe-cli` plus the shared libraries it links, cross-compiled for ARM64 GNU/Linux (glibc >= 2.38).
Self-contained: RUNPATH is `$ORIGIN/lib`, so it runs from any directory with no LD_LIBRARY_PATH.
This is NOT the Android build — Android binaries link bionic and only run on Android.

## Run

    ./bmoe-cli -m model.gguf -p "hello" --moe-stream

Baseline: armv8.2-a + dotprod + fp16 (any 2018+ ARM64 SoC; no i8mm, so older SoCs do not
SIGILL). The expert-ready hook is compiled in, so --overlap works.

## Serve agent tooling (opencode or anything OpenAI-compatible)

    python3 ./bmoe-serve.py --model model.gguf \
        --port 8017 --engine-args "--chatml --moe-stream --ctx-size 8192 --ubatch 512"

Requires python3 on the target, stdlib only. The bridge is bundled alongside this README and
resolves the engine to ./bmoe-cli automatically. --ubatch 512 caps the compute-buffer
reservation (it scales with ubatch x vocabulary); decode speed is unaffected. See docs/serve.md.
EOF
echo "staged: $BUNDLE"

if [ "$MAKE_TAR" -eq 1 ]; then
    tar czf "$ROOT/bmoe-arm64.tar.gz" -C "$ROOT" bmoe-arm64
    echo "staged: $ROOT/bmoe-arm64.tar.gz"
fi
