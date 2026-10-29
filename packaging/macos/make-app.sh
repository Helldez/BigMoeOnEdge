#!/usr/bin/env bash
# BigMoeOnEdge.app and its drag-to-install disk image, from the staged app folder of the release
# workflow (bmoe-desktop, bmoe-server, their libraries and backend modules, ui/, catalog/, skills/
# when there is one, the licenses). Nothing is built here: the app's main executable is
# bmoe-desktop (desktop/), the native window that starts bmoe-server beside it.
#
#   packaging/macos/make-app.sh <staged bmoe-desktop folder> <tag> [out dir]
#
# Writes <out dir>/BigMoeOnEdge.app and <out dir>/BigMoeOnEdge-<tag>-macos-<arch>.dmg, <arch> being
# bmoe-server's (out dir defaults to the current one). The bundle is ad-hoc signed, not notarised (issue #385).
set -euo pipefail

STAGE="${1:?staged bmoe-desktop folder}"
TAG="${2:?tag}"
OUT="${3:-.}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
ICON_PNG="$ROOT/examples/ios/app/Resources/Assets.xcassets/AppIcon.appiconset/icon-1024.png"

[ -x "$STAGE/bmoe-server" ] || { echo "no bmoe-server in $STAGE" >&2; exit 1; }
[ -x "$STAGE/bmoe-desktop" ] || { echo "no bmoe-desktop in $STAGE" >&2; exit 1; }
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
APP="$OUT/BigMoeOnEdge.app"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# The version, the architecture and the oldest macOS come from the server itself, so the bundle never claims to run
# where the engine does not.
VERSION="$("$STAGE/bmoe-server" --version | head -n 1 | tr -d '[:space:]')"
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "unexpected server version '$VERSION'" >&2; exit 1; }
MINOS="$(otool -l "$STAGE/bmoe-server" | awk '/LC_BUILD_VERSION/{b=1} b&&/minos/{print $2; exit}')"
[ -n "$MINOS" ] || { echo "no minimum macOS in bmoe-server's load commands" >&2; exit 1; }
ARCH="$(lipo -archs "$STAGE/bmoe-server")"
[[ "$ARCH" =~ ^[a-z0-9_]+$ ]] || { echo "bmoe-server is not built for exactly one architecture: '$ARCH'" >&2; exit 1; }
DMG="$OUT/BigMoeOnEdge-$TAG-macos-$ARCH.dmg"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

# Code in Contents/MacOS: the app (bmoe-desktop, under the bundle's name), the server, and every
# library and module beside them, as the server looks for its backends beside its own executable.
# -P keeps the soname symlinks.
cp "$STAGE/bmoe-desktop" "$APP/Contents/MacOS/BigMoeOnEdge"
cp -P "$STAGE/bmoe-server" "$STAGE"/*.dylib "$STAGE"/*.so "$APP/Contents/MacOS/"
# Everything else is a resource; the app hands the server ui/ and catalog/ from here.
cp -R "$STAGE/ui" "$STAGE/catalog" "$APP/Contents/Resources/"
if [ -d "$STAGE/skills" ]; then cp -R "$STAGE/skills" "$APP/Contents/Resources/"; fi
cp "$STAGE/LICENSE" "$STAGE/THIRD_PARTY_LICENSES" "$APP/Contents/Resources/"

# The icon, the app icon the iOS app uses, at the sizes an .icns holds.
ICONSET="$WORK/AppIcon.iconset"
mkdir -p "$ICONSET"
for s in 16 32 128 256 512; do
    sips -z "$s" "$s" "$ICON_PNG" --out "$ICONSET/icon_${s}x${s}.png" > /dev/null
    sips -z $((s * 2)) $((s * 2)) "$ICON_PNG" --out "$ICONSET/icon_${s}x${s}@2x.png" > /dev/null
done
iconutil -c icns -o "$APP/Contents/Resources/AppIcon.icns" "$ICONSET"

cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>BigMoeOnEdge</string>
  <key>CFBundleDisplayName</key><string>BigMoeOnEdge</string>
  <key>CFBundleIdentifier</key><string>io.bigmoeonedge.desktop</string>
  <key>CFBundleExecutable</key><string>BigMoeOnEdge</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>LSMinimumSystemVersion</key><string>$MINOS</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.developer-tools</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSHumanReadableCopyright</key><string>MIT License. Third-party licenses are in the app's Resources.</string>
</dict>
</plist>
EOF
plutil -lint "$APP/Contents/Info.plist" > /dev/null

# Ad-hoc signatures, inside out: each library and module, the server, then the bundle, which seals
# the resources and the app's executable. --deep is not used: Apple deprecates it for signing, and signing
# each piece is what it would do anyway.
find "$APP/Contents/MacOS" -type f \( -name '*.dylib' -o -name '*.so' \) -exec codesign --force -s - {} \;
codesign --force -s - "$APP/Contents/MacOS/bmoe-server"
codesign --force -s - "$APP"
codesign --verify --deep --strict "$APP"

# The disk image: the app and a link to /Applications to drag it onto, and the README.
DMGROOT="$WORK/dmg"
mkdir -p "$DMGROOT"
ditto "$APP" "$DMGROOT/BigMoeOnEdge.app"
ln -s /Applications "$DMGROOT/Applications"
cat > "$DMGROOT/README.txt" <<EOF
BigMoeOnEdge $TAG for macOS ($ARCH)

Drag BigMoeOnEdge onto Applications, then open it from there. It opens its window and starts the
engine's server behind it (on http://127.0.0.1:8765/, which a browser can open too). Closing the
window keeps it in the menu bar; Quit there, or Cmd-Q, stops it. Download a model from the Models
page or point it at a gguf you have. Everything runs on this Mac; the server listens on this Mac
only. Settings and downloaded models live in ~/Library/Application Support/BigMoeOnEdge, and the
server's output in its logs/server.log.

The app is not notarised yet, so a copy downloaded with a browser is quarantined and macOS refuses
to open it the first time. Once, in Terminal, after dragging it to Applications:
  xattr -dr com.apple.quarantine /Applications/BigMoeOnEdge.app
or open it once, then allow it under System Settings > Privacy & Security ("Open Anyway"). A
notarised build is planned (https://github.com/Helldez/BigMoeOnEdge/issues/385, and #234).
EOF
rm -f "$DMG"
hdiutil create -quiet -volname "BigMoeOnEdge $TAG" -srcfolder "$DMGROOT" -fs HFS+ -format UDZO -ov "$DMG"
echo "$APP"
echo "$DMG"
