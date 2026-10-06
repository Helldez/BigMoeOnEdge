#!/usr/bin/env bash
# Cross-compile BigMoeOnEdge for ARM64 GNU/Linux and stage a self-contained bundle.
#
# Needs the aarch64-linux-gnu GCC toolchain (Ubuntu: g++-15-aarch64-linux-gnu or
# gcc-aarch64-linux-gnu). CPU baseline matches the Android build: armv8.2-a + dotprod + fp16,
# deliberately NOT i8mm — a build that pins i8mm SIGILLs during prefill on pre-2021 SoCs
# (e.g. Snapdragon 865), while dotprod + fp16 covers every SoC that can realistically run a
# >RAM MoE model.
#
# The bundle is bmoe-arm64/: bmoe-cli + the shared libs it links, RUNPATH $ORIGIN/lib, so it
# runs from any directory with no LD_LIBRARY_PATH, plus install.sh to put it on PATH.
# Pass --tar to also write bmoe-arm64.tar.gz and its .sha256.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build-arm64}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
MAKE_TAR=0

usage() {
    cat <<'EOF'
Usage: scripts/build-arm64.sh [--tar]

Cross-compiles bmoe-cli for ARM64 GNU/Linux and stages the self-contained bmoe-arm64/
bundle next to the repo root.

  --tar   also write bmoe-arm64.tar.gz and bmoe-arm64.tar.gz.sha256

Environment:
  BUILD_DIR   cmake build directory (default: build-arm64)
  JOBS        parallel build jobs (default: online CPU count)
EOF
}

for arg in "$@"; do
    case "$arg" in
        --tar) MAKE_TAR=1 ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $arg (try --help)" >&2
            exit 2
            ;;
    esac
done

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
    -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=FALSE \
    -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
    -DBMOE_BUILD_TESTS=OFF \
    -DGGML_NATIVE=OFF \
    -DGGML_OPENMP=OFF \
    -DGGML_CPU_ARM_ARCH="armv8.2-a+dotprod+fp16" \
    -DLLAMA_CURL=OFF
cmake --build "$BUILD_DIR" -j "$JOBS"

CLI="$BUILD_DIR/cli/bmoe-cli"
[ -x "$CLI" ] || { echo "build did not produce $CLI" >&2; exit 1; }

READELF="$(command -v aarch64-linux-gnu-readelf || command -v readelf || true)"
if [ -z "$READELF" ]; then
    echo "no readelf found — cannot resolve the bundle's library closure" >&2
    exit 1
fi

BUNDLE="$ROOT/bmoe-arm64"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/lib"
cp -a "$CLI" "$BUNDLE/"
cp -a "$ROOT/scripts/bundle-install.sh" "$BUNDLE/install.sh"
chmod +x "$BUNDLE/install.sh"

# Stage libraries by walking the ELF NEEDED closure from the CLI rather than globbing
# $BUILD_DIR/bin/lib*.so*. The build dir is reused across llama.cpp bumps, so a glob also picks
# up libraries from whatever version was linked last time (two ABIs of the same soname in one
# bundle), and cp without -a dereferences the soname symlinks into full copies, which doubles
# the bundle size for no benefit. -a keeps the chains as symlinks; the closure is what decides
# membership, so stale leftovers are simply never reached.
needed_of() { "$READELF" -d "$1" 2> /dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }

declare -A staged_sonames=()
queue=("$CLI")
while [ "${#queue[@]}" -gt 0 ]; do
    binary="${queue[0]}"
    queue=("${queue[@]:1}")
    for soname in $(needed_of "$binary"); do
        # Anything the target's own toolchain provides is not ours to ship.
        case "$soname" in
            libc.so.* | libm.so.* | libdl.so.* | libpthread.so.* | librt.so.* | \
                libstdc++.so.* | libgcc_s.so.* | ld-linux-*.so.*) continue ;;
        esac
        [ -n "${staged_sonames[$soname]:-}" ] && continue

        chain="$BUILD_DIR/bin/$soname"
        if [ ! -e "$chain" ]; then
            echo "missing $soname (needed by $(basename "$binary")) — not in $BUILD_DIR/bin" >&2
            exit 1
        fi
        staged_sonames["$soname"]=1

        # Copy the symlink chain: the soname the loader asks for, plus whatever it resolves to.
        current="$chain"
        cp -a "$current" "$BUNDLE/lib/"
        while [ -L "$current" ]; do
            current="$(dirname "$current")/$(readlink "$current")"
            cp -a "$current" "$BUNDLE/lib/"
        done
        # And the unversioned development symlink (libggml.so -> libggml.so.0), which the loader
        # never asks for but tools and humans expect a shared library to have.
        base="${soname%%.so.*}.so"
        [ -L "$BUILD_DIR/bin/$base" ] && cp -a "$BUILD_DIR/bin/$base" "$BUNDLE/lib/"

        queue+=("$current")
    done
done

# The staged CLI must not carry the builder's absolute build path: the bundle is a redistributable
# artifact and CMAKE_BUILD_WITH_INSTALL_RPATH=ON above is what keeps the build-tree directory out
# of the RUNPATH. Fail loudly rather than shipping a bundle that only works on this machine.
runpath="$("$READELF" -d "$BUNDLE/bmoe-cli" |
    sed -n 's/.*\(R\|RUN\)PATH.*\[\(.*\)\]/\2/p')"
for entry in ${runpath//:/ }; do
    case "$entry" in
        /*)
            echo "staged RUNPATH leaks an absolute build path: $runpath" >&2
            exit 1
            ;;
    esac
done

cat > "$BUNDLE/README.md" <<'EOF'
# bmoe-arm64 — portable ARM64 Linux bundle

`bmoe-cli` plus the shared libraries it links, cross-compiled for ARM64 GNU/Linux (glibc >= 2.38).
Self-contained: RUNPATH is `$ORIGIN/lib`, so it runs from any directory with no LD_LIBRARY_PATH.
This is NOT the Android build — Android binaries link bionic and only run on Android.

## Install

    ./install.sh                     # into /usr/local (sudo if the prefix is not writable)
    ./install.sh --prefix ~/.local   # or anywhere else
    ./install.sh --uninstall

`install.sh` puts the bundle in `<prefix>/lib/bmoe/` and symlinks `bmoe-cli` into
`<prefix>/bin/`, so the libraries are never copied into a system directory. Run it straight
from the archive instead with `./install.sh ../bmoe-arm64.tar.gz`.

## Run without installing

    ./bmoe-cli -m model.gguf -p "hello" --moe-stream

Baseline: armv8.2-a + dotprod + fp16 (any 2018+ ARM64 SoC; no i8mm, so older SoCs do not
SIGILL). The expert-ready hook is compiled in, so --overlap works.
EOF
echo "staged: $BUNDLE"

if [ "$MAKE_TAR" -eq 1 ]; then
    TAR="$ROOT/bmoe-arm64.tar.gz"
    rm -f "$TAR" "$TAR.sha256"
    tar czf "$TAR" -C "$ROOT" bmoe-arm64
    # Ship a checksum next to the archive: it gets scp'd to a device by hand, and an
    # interrupted copy of a 20 MB gz is otherwise indistinguishable from a good one.
    if command -v sha256sum > /dev/null; then
        (cd "$ROOT" && sha256sum bmoe-arm64.tar.gz > bmoe-arm64.tar.gz.sha256)
    else
        shasum -a 256 "$TAR" | sed "s|$TAR|bmoe-arm64.tar.gz|" > "$TAR.sha256"
    fi
    echo "staged: $TAR"
    echo "staged: $TAR.sha256"
fi
