#!/usr/bin/env bash
# Publish host-benchmark documentation patches to cjl4hd:main without leaving the arc.
#
#   scripts/publish-host-bench.sh PATCH [PATCH...] -- "commit subject"
#
# Each PATCH is a git diff against docs/ (and optionally README.md) as it exists on
# fork/main. The script: fetches fork, aborts if fork/main is missing anything from
# origin/main (it would clobber upstream commits on push), applies the patches in a
# throwaway worktree of fork/main, scans the diff for identifying data (AGENTS rule 7:
# hostnames, local paths, device codes), commits once, and pushes fork HEAD:main.
# Nothing on the arc branch moves; the patches themselves live on the arc as the
# record of what was published.
#
# Recipe for a new model's rows:
#   1. git worktree add --detach /tmp/pub-wt fork/main
#   2. edit /tmp/pub-wt/docs/host-benchmarks.md (rows + reading bullets)
#   3. git -C /tmp/pub-wt diff > scripts/host-bench-<model>-<cells>.patch
#   4. git worktree remove --force /tmp/pub-wt
#   5. bash scripts/publish-host-bench.sh \
#          scripts/host-bench-<model>-<cells>.patch -- "docs: <model> rows"
# Example of a completed publish: scripts/host-bench-ornith-c4c5.patch -> fork/main
# 5988e17 (Ornith c4/c5 divergence rows, 2026-09-19).
set -euo pipefail

if [ $# -lt 2 ]; then
    echo "usage: $0 PATCH [PATCH...] -- \"commit subject\"" >&2
    exit 2
fi

REMOTE=fork
WT=$(mktemp -d /tmp/publish-bench-XXXX)
trap 'git worktree remove --force "$WT" 2>/dev/null' EXIT

# split args on "--"
PATCHES=()
while [ "$1" != "--" ]; do
    [ -f "$1" ] || { echo "patch not found: $1" >&2; exit 1; }
    PATCHES+=("$(realpath "$1")"); shift
done
shift
SUBJECT="$1"

git fetch "$REMOTE" 2>/dev/null
BASE="$REMOTE/main"

# Drift guard: pushing fork HEAD:main rewrites nothing only if fork/main already
# contains everything on origin/main. If upstream moved past the fork, sync first.
if ! git merge-base --is-ancestor origin/main "$BASE"; then
    echo "ABORT: $BASE is behind origin/main — sync the fork before publishing" >&2
    exit 1
fi

git worktree add --detach "$WT" "$BASE" >/dev/null 2>&1

for p in "${PATCHES[@]}"; do
    git -C "$WT" apply --check "$p" || { echo "ABORT: $p does not apply cleanly" >&2; exit 1; }
done
for p in "${PATCHES[@]}"; do
    git -C "$WT" apply "$p"
done

git -C "$WT" add -u

# Rule 7: scan exactly what is being published. Logs and evidence never land here,
# but a pasted path or hostname in a table would be permanent public record.
LEAKS=$(git -C "$WT" diff --cached | grep -nEi \
    '/home/[a-z]+|/Users/[a-z]+|chris|Satellite|192\.168\.|10\.[0-9]+\.[0-9]+\.|127\.0\.0\.1' || true)
if [ -n "$LEAKS" ]; then
    echo "ABORT: possible identifying data in the publish diff:" >&2
    echo "$LEAKS" >&2
    exit 1
fi

git -C "$WT" commit -q -m "$SUBJECT"
git -C "$WT" push "$REMOTE" HEAD:main

echo "published: $(git -C "$WT" rev-parse --short HEAD) -> $REMOTE/main ($SUBJECT)"
