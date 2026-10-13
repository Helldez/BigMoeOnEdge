#!/usr/bin/env bash
# One version, stated in three places: the engine's (CMakeLists.txt, which bmoe-cli --version and
# every metrics CSV report), the app's (versionName) and the changelog's newest dated section.
# Fails when they disagree, and with a release tag as its argument, when the tag is not that
# version: v0.26.0 and v0.28.0 both shipped an engine reporting an older number, because nothing
# compared them.
#
#   scripts/check-version.sh            # the three agree
#   scripts/check-version.sh v0.28.0    # ...and are what this tag says
#
# A ref that is not a vX.Y.Z tag (a branch, a pr-N label on a dry run) is not compared.
set -euo pipefail
cd "$(dirname "$0")/.."

engine=$(sed -nE 's/^project\(bigmoeonedge VERSION ([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' CMakeLists.txt)
app=$(sed -nE "s/^[[:space:]]*versionName '([0-9]+\.[0-9]+\.[0-9]+)'.*/\1/p" examples/android/app/build.gradle | head -n 1)
changelog=$(sed -nE 's/^## \[([0-9]+\.[0-9]+\.[0-9]+)\] - [0-9]{4}-[0-9]{2}-[0-9]{2}.*/\1/p' CHANGELOG.md | head -n 1)

echo "engine    (CMakeLists.txt)                  : ${engine:-not found}"
echo "app       (examples/android/app/build.gradle): ${app:-not found}"
echo "changelog (newest dated section)            : ${changelog:-not found}"

fail=0
if [ -z "$engine" ] || [ -z "$app" ] || [ -z "$changelog" ]; then
    echo "error: a version could not be read" >&2
    fail=1
elif [ "$engine" != "$app" ] || [ "$engine" != "$changelog" ]; then
    echo "error: the three versions disagree" >&2
    fail=1
fi

tag="${1:-}"
if [[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "tag                                         : $tag"
    if [ "v$engine" != "$tag" ]; then
        echo "error: tag $tag does not match engine version $engine" >&2
        fail=1
    fi
elif [ -n "$tag" ]; then
    echo "ref '$tag' is not a release tag: not compared"
fi
exit $fail
