#!/usr/bin/env bash
# Configure and build BigMoeOnEdge for the host (Linux/macOS). Env overrides:
#   BUILD_DIR (default build), BUILD_TYPE (Release), JOBS (nproc).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"

cd "$ROOT"
if [ ! -f third_party/llama.cpp/CMakeLists.txt ]; then
    echo "llama.cpp submodule missing — run: git submodule update --init --recursive"
    exit 1
fi

cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR" -j "$JOBS"

echo "built: $BUILD_DIR/cli/bmoe-cli"
echo "run gates: (cd $BUILD_DIR && ctest --output-on-failure)"
