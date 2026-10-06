#!/usr/bin/env bash
# Install or remove a staged bmoe-arm64 bundle.
#
# Ships inside the bundle next to bmoe-cli, and also works when pointed at a
# bmoe-arm64.tar.gz. Installs the bundle to $PREFIX/lib/bmoe and puts a symlink in
# $PREFIX/bin, so the CLI is on PATH without copying the libraries into a system directory.
#
# The symlink is what makes this work: RUNPATH is $ORIGIN/lib and $ORIGIN is taken from the
# *resolved* path of the binary, so a symlink in bin/ still finds ../lib/bmoe/lib.
set -euo pipefail

PREFIX="/usr/local"
ACTION="install"
FORCE=0
SRC=""
TMPDIR_CREATED=""

PKG="bmoe"
DEST_SUBDIR="lib/$PKG"

usage() {
    cat <<'EOF'
Usage: install.sh [SOURCE] [OPTIONS]

Install a staged bmoe-arm64 bundle into a prefix (default /usr/local). SOURCE may be a
bundle directory or a bmoe-arm64.tar.gz; it defaults to this script's own directory.

Options:
  --prefix DIR   install under DIR instead of /usr/local
  --uninstall    remove a previously installed bundle
  --force        overwrite a $PREFIX/bin entry that is not one of our symlinks
  -h, --help     show this help

Layout:
  $PREFIX/lib/bmoe/{bmoe-cli,README.md,lib/}
  $PREFIX/bin/bmoe-cli  -> ../lib/bmoe/bmoe-cli

Examples:
  ./install.sh                          # from an extracted bundle
  ./install.sh ~/bmoe-arm64.tar.gz      # straight from the archive
  sudo ./install.sh --prefix /opt       # somewhere else
  sudo ./install.sh --uninstall
EOF
}

die() { echo "install.sh: $*" >&2; exit 1; }
note() { echo "  $*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) [ $# -ge 2 ] || die "--prefix needs a directory"; PREFIX="$2"; shift 2 ;;
        --prefix=*) PREFIX="${1#*=}"; shift ;;
        --uninstall) ACTION="uninstall"; shift ;;
        --force) FORCE=1; shift ;;
        -h | --help) usage; exit 0 ;;
        -*) die "unknown option: $1 (try --help)" ;;
        *) [ -z "$SRC" ] || die "unexpected extra argument: $1"; SRC="$1"; shift ;;
    esac
done

[ -n "$PREFIX" ] || die "--prefix must not be empty"
# Normalise a trailing slash so $PREFIX/bin and $PREFIX/lib/bmoe are predictable.
while [ "$PREFIX" != "/" ] && [ "${PREFIX%/}" != "$PREFIX" ]; do PREFIX="${PREFIX%/}"; done
[ "$PREFIX" = "/" ] && die "refusing to install into /"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# The EXIT trap's status becomes the script's on the natural-exit paths, so the no-tarball
# case must not fail the test — otherwise a successful install exits 1.
cleanup() { [ -z "$TMPDIR_CREATED" ] || rm -rf "$TMPDIR_CREATED"; }
trap cleanup EXIT

# A bundle is valid only if it has the binary and a populated lib/ — checking up front means a
# wrong SOURCE is reported before anything under $PREFIX is touched.
validate_bundle() {
    local dir="$1"
    [ -x "$dir/bmoe-cli" ] || return 1
    ls "$dir"/lib/lib*.so* > /dev/null 2>&1 || return 1
    return 0
}

resolve_source() {
    if [ -z "$SRC" ]; then
        if validate_bundle "$HERE"; then
            SRC="$HERE"
            return
        fi
        # We were run from beside an archive rather than from inside one.
        local cand
        for cand in "$HERE/bmoe-arm64.tar.gz" "$HERE/../bmoe-arm64.tar.gz"; do
            [ -f "$cand" ] && { SRC="$cand"; return; }
        done
        die "no bundle found next to this script and no source given; pass a directory or .tar.gz"
    fi

    if [ -f "$SRC" ]; then
        case "$SRC" in
            *.tar.gz | *.tgz) ;;
            *) die "source file must be a .tar.gz: $SRC" ;;
        esac
        TMPDIR_CREATED="$(mktemp -d)"
        echo "extracting $(basename "$SRC") ..."
        tar xzf "$SRC" -C "$TMPDIR_CREATED"
        # The archive contains a single bmoe-arm64/ top level, but do not assume its name.
        local found=""
        while IFS= read -r d; do
            validate_bundle "$d" && found="$d" && break
        done < <(find "$TMPDIR_CREATED" -mindepth 1 -maxdepth 2 -type d)
        [ -n "$found" ] || die "archive does not contain a bundle (need bmoe-cli and lib/lib*.so*)"
        SRC="$found"
    elif [ ! -d "$SRC" ]; then
        die "source not found: $SRC"
    fi

    validate_bundle "$SRC" || die "not a bmoe bundle (need bmoe-cli and lib/lib*.so*): $SRC"
}

# Refuse to clobber something that is not our symlink: a distro package or the user's own
# script should not disappear because an installer was run.
check_bin_entry() {
    local path="$1" target="$2"
    [ -e "$path" ] || [ -L "$path" ] || return 0
    if [ -L "$path" ] && [ "$(readlink "$path")" = "$target" ]; then
        return 0
    fi
    [ "$FORCE" -eq 1 ] && return 0
    die "$path already exists and is not one of our symlinks; pass --force to replace it"
}

do_uninstall() {
    local dest="$PREFIX/$DEST_SUBDIR"
    echo "Uninstalling $PKG from $PREFIX"
    local removed=0
    local name target path
    for name in bmoe-cli; do
        path="$PREFIX/bin/$name"
        target="../$DEST_SUBDIR/$name"
        if [ -L "$path" ] && [ "$(readlink "$path")" = "$target" ]; then
            rm -f "$path"
            note "removed $path"
            removed=1
        elif [ -e "$path" ]; then
            note "left $path (not one of our symlinks)"
        fi
    done
    if [ -d "$dest" ]; then
        rm -rf "$dest"
        note "removed $dest"
        removed=1
    fi
    if [ "$removed" -eq 0 ]; then
        note "nothing to remove under $PREFIX"
    else
        # $PREFIX/bin and $PREFIX/lib may hold other packages, so only report, never rmdir.
        note "kept $PREFIX/bin and $PREFIX/lib (may hold other packages)"
    fi
}

do_install() {
    local dest="$PREFIX/$DEST_SUBDIR"
    echo "Installing $PKG from $SRC into $PREFIX"

    if ! mkdir -p "$PREFIX" 2> /dev/null || [ ! -w "$PREFIX" ]; then
        if [ "$(id -u)" -eq 0 ]; then
            die "$PREFIX is not writable even as root — pass --prefix somewhere writable"
        fi
        die "$PREFIX is not writable. Re-run with sudo, or use --prefix \$HOME/.local"
    fi

    mkdir -p "$PREFIX/bin"
    check_bin_entry "$PREFIX/bin/bmoe-cli" "../$DEST_SUBDIR/bmoe-cli"

    # Replace the tree wholesale: copying over a previous install can leave behind libraries
    # from an older llama.cpp whose sonames no longer exist in this build.
    rm -rf "$dest"
    mkdir -p "$dest"
    cp -a "$SRC/bmoe-cli" "$SRC/lib" "$dest/"
    [ -f "$SRC/README.md" ] && cp -a "$SRC/README.md" "$dest/"
    note "installed $dest"

    ln -sfn "../$DEST_SUBDIR/bmoe-cli" "$PREFIX/bin/bmoe-cli"
    note "linked $PREFIX/bin/bmoe-cli"

    # Smoke test the install on this machine. It is advisory: the install is correct even when
    # this fails, which is exactly the case when the bundle is staged on an x86 host for an
    # ARM64 target, so report and continue rather than aborting a good install.
    if "$dest/bmoe-cli" --version > /dev/null 2>&1; then
        note "verified: $("$dest/bmoe-cli" --version 2>&1 | head -1)"
    else
        note "note: could not run $dest/bmoe-cli here (wrong architecture, or glibc older than the bundle needs)"
    fi

    case ":$PATH:" in
        *":$PREFIX/bin:"*) ;;
        *) echo "  $PREFIX/bin is not on your PATH; add it to use 'bmoe-cli' by name" ;;
    esac
}

resolve_source
if [ "$ACTION" = "uninstall" ]; then
    do_uninstall
else
    do_install
fi
