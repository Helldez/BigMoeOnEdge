#!/usr/bin/env bash
# The runs a device report needs, in one go: the plan, a run of it, and the same run with the dense
# set left mapped (the planner's choice on iOS before #304), each in a fresh process of the harness,
# with the console output and the per-token CSV of every run brought back to this machine.
#
#   scripts/ios-runs.sh device <model.gguf>    a signed harness on a device (BMOE_IOS_DEVICE)
#   scripts/ios-runs.sh sim    <model.gguf>    the harness in the booted simulator
#
# The model goes in the app's Documents folder first (Files app or Finder for a device, `cp` into
# `xcrun simctl get_app_container booted <bundle id> data` for the simulator) and is named here
# by its file name. Env overrides:
#   BMOE_IOS_DEVICE     device id or name, as `xcrun devicectl list devices` prints it (device only)
#   BMOE_IOS_BUNDLE_ID  (io.bigmoeonedge.harness) the bundle id the harness was signed with
#   RUNS                ("plan auto mmap") any of: plan, auto, mmap, t2, t4 (auto with 2 or 4 threads),
#                       c<MiB> (auto with the expert cache fixed at that size, e.g. c1500)
#   PROMPT              ("The capital of France is")
#   N_PREDICT           (48)
#   OUT                 (ios-runs-<date>-<time>) where the outputs land
#
# The simulator runs on this Mac's kernel, memory and storage: it checks that the harness builds and
# runs, and its numbers say nothing about a phone (examples/ios/README.md).
set -euo pipefail

TARGET="${1:-}"
MODEL="${2:-}"
if [ -z "$TARGET" ] || [ -z "$MODEL" ] || { [ "$TARGET" != device ] && [ "$TARGET" != sim ]; }; then
    echo "usage: $0 device|sim <model.gguf>   (see the header for the environment)" >&2
    exit 2
fi
BUNDLE="${BMOE_IOS_BUNDLE_ID:-io.bigmoeonedge.harness}"
RUNS="${RUNS:-plan auto mmap}"
PROMPT="${PROMPT:-The capital of France is}"
N_PREDICT="${N_PREDICT:-48}"
OUT="${OUT:-ios-runs-$(date +%Y%m%d-%H%M%S)}"
batch_status=0
if [ "$TARGET" = device ] && [ -z "${BMOE_IOS_DEVICE:-}" ]; then
    echo "BMOE_IOS_DEVICE is required for a device: one of \`xcrun devicectl list devices\`" >&2
    exit 2
fi
mkdir -p "$OUT"

launch() {
    if [ "$TARGET" = device ]; then
        xcrun devicectl device process launch --console --device "$BMOE_IOS_DEVICE" "$BUNDLE" --bmoe "$@"
    else
        xcrun simctl launch --console booted "$BUNDLE" --bmoe "$@"
    fi
}

fetch() { # a file from the app's Documents folder into $OUT
    if [ "$TARGET" = device ]; then
        xcrun devicectl device copy from --device "$BMOE_IOS_DEVICE" --domain-type appDataContainer \
            --domain-identifier "$BUNDLE" --source "Documents/$1" --destination "$OUT/$1" >/dev/null
    else
        cp "$(xcrun simctl get_app_container booted "$BUNDLE" data)/Documents/$1" "$OUT/$1"
    fi
}

# The mean of one CSV column over the rows that measured it (-1 is "unmeasured" there).
column_mean() {
    # The C locale: a decimal-comma locale reads "0.849" as 0 and prints its result with a comma.
    LC_ALL=C awk -F, -v name="$2" '
        /^#/ { next }
        !col { for (i = 1; i <= NF; i++) if ($i == name) col = i; next }
        col && $col != "" && $col + 0 >= 0 { sum += $col; n++ }
        END { if (n) printf "%.3f", sum / n; else printf "n/a" }' "$1"
}

run() { # name, then the bmoe-cli arguments
    local name="$1"
    shift
    echo "== $name: $*"
    local status=0
    launch "$@" >"$OUT/$name.txt" 2>&1 || status=$?
    # Keep collecting the other runs and any partial CSV, but do not report a failed batch as success.
    # This assignment must stay outside the summary pipeline, whose left side runs in a subshell.
    if [ "$status" -ne 0 ]; then batch_status=1; fi
    echo "exit $status" >>"$OUT/$name.txt"
    {
        echo "== $name (exit $status)"
        grep -E '^(prefill|generation|compute|mode|moe-stream|moe-cache):|plan:   (dense-weights|cache-mb|threads|memory-ledger|prefill-device) |cache brake:' \
            "$OUT/$name.txt" || echo "(no perf lines: see $name.txt)"
        if fetch "$name.csv" 2>/dev/null; then
            echo "csv: dense_resident_frac mean $(column_mean "$OUT/$name.csv" dense_resident_frac)," \
                "majflt per token mean $(column_mean "$OUT/$name.csv" majflt)"
        fi
        echo
    } | tee -a "$OUT/summary.txt"
}

common=(-m "$MODEL" -p "$PROMPT" -n "$N_PREDICT")
for r in $RUNS; do
    case "$r" in
        plan) run plan --auto --plan -m "$MODEL" ;;
        auto) run auto --auto "${common[@]}" --csv auto.csv ;;
        mmap) run mmap --auto --dense-weights mmap "${common[@]}" --csv mmap.csv ;;
        t2) run t2 --auto --threads 2 "${common[@]}" --csv t2.csv ;;
        t4) run t4 --auto --threads 4 "${common[@]}" --csv t4.csv ;;
        c[0-9]*) run "$r" --auto --cache-mb "${r#c}" --force-cache "${common[@]}" --csv "$r.csv" ;;
        *) echo "unknown run '$r' (plan, auto, mmap, t2, t4, c<MiB>)" >&2 ;;
    esac
done
echo "outputs: $OUT (summary.txt first)"
exit "$batch_status"
