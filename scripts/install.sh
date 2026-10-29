#!/bin/sh
# BigMoeOnEdge's command-line tools for this user, from the latest release: bmoe-cli, and
# bmoe-server with its web UI (docs/desktop-app.md). No administrator rights, nothing outside the
# user's home.
#
#   curl -fsSL https://github.com/Helldez/BigMoeOnEdge/releases/latest/download/install.sh | sh
#
# It reads the release's feed, checks the feed's signature where this machine's OpenSSL can
# (Ed25519 needs OpenSSL 3), picks the archive for this machine, checks its size and SHA-256
# against the feed, unpacks it into ~/.local/lib/bigmoeonedge (replacing an earlier install), and
# links the commands into ~/.local/bin, which it puts on PATH in the shell's profile if it is not
# there yet. Run again to update. Overrides: BMOE_PREFIX, BMOE_BIN_DIR, BMOE_FEED_URL.
set -eu

FEED_URL="${BMOE_FEED_URL:-https://github.com/Helldez/BigMoeOnEdge/releases/latest/download/feed.json}"
# The release replaces this with the key the server checks the feed with (server/feed_key.h).
FEED_PUBLIC_KEY="@FEED_PUBLIC_KEY@"
PREFIX="${BMOE_PREFIX:-$HOME/.local/lib/bigmoeonedge}"
BIN="${BMOE_BIN_DIR:-$HOME/.local/bin}"
COMMANDS="bmoe-cli bmoe-server"

say() { printf '%s\n' "$*"; }
die() { printf 'install: %s\n' "$*" >&2; exit 1; }

case "$(uname -s)-$(uname -m)" in
    Darwin-arm64) TARGET=macos-arm64 ;;
    Linux-x86_64 | Linux-amd64) TARGET=linux-x86_64 ;;
    Linux-aarch64 | Linux-arm64) TARGET=linux-aarch64 ;;
    *) die "there is no release for $(uname -s) on $(uname -m); build from source (scripts/build-host.sh)" ;;
esac
for tool in curl tar awk; do
    command -v "$tool" > /dev/null 2>&1 || die "$tool is needed and was not found"
done
if command -v sha256sum > /dev/null 2>&1; then
    sha256() { sha256sum "$1" | awk '{print $1}'; }
elif command -v shasum > /dev/null 2>&1; then
    sha256() { shasum -a 256 "$1" | awk '{print $1}'; }
else
    die "sha256sum or shasum is needed and was not found"
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT INT TERM

say "Reading the release feed..."
curl -fsSL --proto '=https' --proto-redir '=https' -o "$TMP/feed.json" "$FEED_URL" || die "could not read $FEED_URL"

# The signature, where it can be checked: the raw Ed25519 key wrapped in its DER header.
case "$FEED_PUBLIC_KEY" in
    @*) say "  (this copy of the script carries no feed key: the feed's signature is not checked)" ;;
    *)
        if command -v openssl > /dev/null 2>&1 && openssl version 2> /dev/null | grep -q '^OpenSSL [3-9]'; then
            curl -fsSL --proto '=https' --proto-redir '=https' -o "$TMP/feed.sig" "$FEED_URL.sig" ||
                die "could not read the feed's signature"
            openssl base64 -d -A -in "$TMP/feed.sig" -out "$TMP/feed.sig.bin"
            printf '\060\052\060\005\006\003\053\145\160\003\041\000' > "$TMP/key.der"
            printf '%s' "$FEED_PUBLIC_KEY" | openssl base64 -d -A >> "$TMP/key.der"
            openssl pkeyutl -verify -pubin -keyform DER -inkey "$TMP/key.der" -rawin -in "$TMP/feed.json" \
                -sigfile "$TMP/feed.sig.bin" > /dev/null 2>&1 ||
                die "the feed's signature does not match: it is not from this project's releases"
            say "  signature checked"
        else
            say "  (no OpenSSL 3 here: the feed's signature is not checked; the archive's SHA-256 still is)"
        fi
        ;;
esac

# The feed is written one entry a line (scripts/make-feed.py): this target's row of "archives".
ROW="$(awk -v key="\"$TARGET\":" '
    /^  "archives": \{/ { inside = 1; next }
    inside && /^  [}\]]/ { inside = 0 }
    inside && index($0, key) { print; exit }' "$TMP/feed.json")"
[ -n "$ROW" ] || die "the latest release has no archive for $TARGET"
field() { printf '%s' "$ROW" | sed -n "s/.*\"$1\": \"\{0,1\}\([^\",}]*\)\"\{0,1\}.*/\1/p"; }
URL="$(field url)"
SHA="$(field sha256)"
SIZE="$(field size)"
VERSION="$(sed -n 's/^  "version": "\([0-9.]*\)".*/\1/p' "$TMP/feed.json")"
case "$URL" in https://*) ;; *) die "the feed's archive URL is not https" ;; esac
printf '%s' "$SHA" | grep -Eq '^[0-9a-f]{64}$' || die "the feed's digest is not a SHA-256"

say "Downloading BigMoeOnEdge $VERSION for $TARGET..."
curl -fL --proto '=https' --proto-redir '=https' --progress-bar -o "$TMP/archive.tar.gz" "$URL" ||
    die "could not download $URL"
GOT_SIZE="$(wc -c < "$TMP/archive.tar.gz" | tr -d ' ')"
[ "$GOT_SIZE" = "$SIZE" ] || die "the download is $GOT_SIZE bytes, the feed says $SIZE"
[ "$(sha256 "$TMP/archive.tar.gz")" = "$SHA" ] || die "the download's SHA-256 is not the one the feed names"

mkdir -p "$TMP/x"
tar -xzf "$TMP/archive.tar.gz" -C "$TMP/x"
TOP="$(find "$TMP/x" -mindepth 1 -maxdepth 1 -type d | head -n 1)"
[ -n "$TOP" ] && [ -x "$TOP/bmoe-cli" ] || die "the archive does not hold bmoe-cli"

# The new copy beside the old, then swapped: a failure half-way leaves the old one working.
mkdir -p "$(dirname "$PREFIX")" "$BIN"
rm -rf "$PREFIX.new" "$PREFIX.old"
mv "$TOP" "$PREFIX.new"
[ ! -e "$PREFIX" ] || mv "$PREFIX" "$PREFIX.old"
mv "$PREFIX.new" "$PREFIX"
rm -rf "$PREFIX.old"
for c in $COMMANDS; do
    [ -x "$PREFIX/$c" ] && ln -sf "$PREFIX/$c" "$BIN/$c"
done

case ":$PATH:" in
    *":$BIN:"*) ;;
    *)
        case "${SHELL:-}" in
            */zsh) PROFILE="$HOME/.zshrc" ;;
            */bash) PROFILE="$HOME/.bashrc" ;;
            *) PROFILE="$HOME/.profile" ;;
        esac
        LINE="export PATH=\"$BIN:\$PATH\" # BigMoeOnEdge's command-line tools"
        if ! grep -qF "$LINE" "$PROFILE" 2> /dev/null; then
            printf '\n%s\n' "$LINE" >> "$PROFILE"
            say "Added $BIN to PATH in $PROFILE: open a new terminal, or run: export PATH=\"$BIN:\$PATH\""
        fi
        ;;
esac

say "Installed in $PREFIX:"
for c in $COMMANDS; do
    [ -x "$BIN/$c" ] && say "  $BIN/$c ($("$BIN/$c" --version 2> /dev/null || echo '?'))"
done
say "Start the web UI with: bmoe-server --open"
