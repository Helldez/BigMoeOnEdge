#!/usr/bin/env bash
# BigMoeOnEdge-<tag>-<target>.AppImage, from the staged app folder of the release workflow
# (bmoe-desktop, bmoe-server, their libraries and backend modules, ui/, catalog/, skills/ when there
# is one, the licenses). The folder goes into the AppImage as it is, under usr/bin, so the server
# finds its backends, ui/ and catalog/ beside itself. Its entry (AppRun) is bmoe-desktop, the app's
# window; WebKitGTK 4.1 is the system's, as the image does not carry a web engine.
#
#   packaging/linux/make-appimage.sh <staged bmoe-desktop folder> <tag> <target> [out dir]
#
# <target> is linux-x86_64 or linux-aarch64, and must be this machine's: appimagetool runs here.
set -euo pipefail

STAGE="${1:?staged bmoe-desktop folder}"
TAG="${2:?tag}"
TARGET="${3:?target (linux-x86_64 or linux-aarch64)}"
OUT="${4:-.}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
ICON_PNG="$ROOT/examples/ios/app/Resources/Assets.xcassets/AppIcon.appiconset/icon-1024.png"

# appimagetool and the runtime it prepends, pinned: a release of each, checked against the SHA-256
# GitHub lists for the asset. The runtime is passed explicitly, since appimagetool otherwise fetches
# the newest continuous build of it. Bump a version and its sums together.
TOOL_VERSION=1.9.1
RUNTIME_VERSION=20251108
case "$TARGET" in
    linux-x86_64)
        ARCH=x86_64
        TOOL_SHA=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
        RUNTIME_SHA=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
        ;;
    linux-aarch64)
        ARCH=aarch64
        TOOL_SHA=f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158
        RUNTIME_SHA=00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444
        ;;
    *) echo "unknown target $TARGET" >&2; exit 1 ;;
esac
[ "$(uname -m)" = "$ARCH" ] || { echo "target $TARGET needs an $ARCH machine, this is $(uname -m)" >&2; exit 1; }
[ -x "$STAGE/bmoe-server" ] || { echo "no bmoe-server in $STAGE" >&2; exit 1; }
[ -x "$STAGE/bmoe-desktop" ] || { echo "no bmoe-desktop in $STAGE" >&2; exit 1; }

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
IMAGE="$OUT/BigMoeOnEdge-$TAG-$TARGET.AppImage"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fetch() { # <url> <sha256> <dest>
    curl -fsSL -o "$3" "$1"
    echo "$2  $3" | sha256sum -c --quiet - || { echo "SHA-256 mismatch for $1" >&2; exit 1; }
}
fetch "https://github.com/AppImage/appimagetool/releases/download/$TOOL_VERSION/appimagetool-$ARCH.AppImage" \
    "$TOOL_SHA" "$WORK/appimagetool"
fetch "https://github.com/AppImage/type2-runtime/releases/download/$RUNTIME_VERSION/runtime-$ARCH" \
    "$RUNTIME_SHA" "$WORK/runtime"
chmod +x "$WORK/appimagetool"

APPDIR="$WORK/BigMoeOnEdge.AppDir"
mkdir -p "$APPDIR/usr/bin"
cp -a "$STAGE"/. "$APPDIR/usr/bin/"
install -m 755 "$HERE/AppRun" "$APPDIR/AppRun"
cp "$HERE/bigmoeonedge.desktop" "$APPDIR/"
cp "$ICON_PNG" "$APPDIR/bigmoeonedge.png" # also the tray's icon (desktop/os_linux.cpp)
cp "$ICON_PNG" "$APPDIR/.DirIcon"

# The tool is itself an AppImage: extract-and-run needs no FUSE on the build machine.
ARCH="$ARCH" APPIMAGE_EXTRACT_AND_RUN=1 "$WORK/appimagetool" --no-appstream \
    --runtime-file "$WORK/runtime" "$APPDIR" "$IMAGE"
echo "$IMAGE"
