#!/usr/bin/env bash
# One reproducible host benchmark, printed as a block you can paste into a benchmark-report issue
# (docs/community-benchmarks.md). Linux first, macOS best effort.
#
#   scripts/bench-report.sh MODEL.gguf [extra bmoe-cli flags...]
#
# What it does, in order:
#   1. builds bmoe-cli if build/bin/bmoe-cli is missing (BMOE_CLI overrides the binary);
#   2. records the machine: CPU, cores, RAM, kernel, the drive the model sits on and its
#      measured O_DIRECT read rate at 512 KiB requests, straight from the model file — the same
#      request size the expert stream issues, so the number is the ceiling this engine can see;
#   3. runs the model twice with `--auto`, 256 greedy tokens each, with the same prompt every table
#      in the README uses (docs/benchmark-method.md). `--auto` is the only protocol: the planner
#      places what fits on the GPUs the build carries, keeps a model that fits in RAM resident and
#      streams only what is left, so the row is what the engine does on this machine and the
#      number to compare with other engines. `--plan-explain` only prints the plan it runs.
#      Two runs because the planner learns between them (docs/measured-planning.md): the first
#      load records what the model routed and measures the lossless knobs on its first answer; the
#      second plans on that record, which is the plan the app and the server run from their second
#      load on. Both are reported; the second is the row for the table, the first says what a
#      cold planner predicted. `--routing-dir` under BENCH_OUT is where the record is kept;
#   4. parses each CSV trailer and prints one markdown block, with the plan `--auto` chose.
#
# Env overrides: N_PREDICT (256), BENCH_RUNS (2; 1 reports the first load alone), BENCH_SETTLE
# (30: seconds between the runs, see below), BENCH_OUT (.bench-report), BMOE_CLI (build/bin/bmoe-cli). Anything after the model path is passed to
# bmoe-cli verbatim (e.g. --think for a reasoning row, --n-expert-used 6 for the turbo top-k rows).
set -euo pipefail
export LC_ALL=C # the numbers this prints are parsed by people in every locale: a decimal point, never a comma

if [ $# -lt 1 ] || [ ! -f "$1" ]; then
    echo "usage: $0 MODEL.gguf [extra bmoe-cli flags...]" >&2
    exit 2
fi
MODEL="$1"; shift

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readf() { cat "$1" 2>/dev/null || true; }   # optional sysfs/procfs files: absent is not an error
OS="$(uname -s)"
NPROC="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
N_PREDICT="${N_PREDICT:-256}"
BENCH_RUNS="${BENCH_RUNS:-2}"
BENCH_SETTLE="${BENCH_SETTLE:-30}"
BENCH_OUT="${BENCH_OUT:-$ROOT/.bench-report}"
BMOE_CLI="${BMOE_CLI:-$ROOT/build/bin/bmoe-cli}"
PROMPT="Write a long detailed essay about the history of computing including its origins its key milestones the people involved and the future directions of the field"

# --- 1. binary ------------------------------------------------------------------------------
if [ ! -x "$BMOE_CLI" ]; then
    echo "bmoe-cli not found at $BMOE_CLI, building..." >&2
    "$ROOT/scripts/build-host.sh" >&2
fi
ENGINE_VERSION="$("$BMOE_CLI" --version 2>/dev/null | head -1 || echo unknown)"
GIT_REV="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"

# --- 2. machine -----------------------------------------------------------------------------
if [ "$OS" = "Darwin" ]; then
    CPU_MODEL="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
    RAM_GIB="$(( $(sysctl -n hw.memsize) / 1024 / 1024 / 1024 ))"
    KERNEL="macOS $(sw_vers -productVersion 2>/dev/null || uname -r)"
    # An APFS volume names no media; the container it is part of (or the physical store behind
    # it) does.
    DEV="$(df "$MODEL" 2>/dev/null | tail -1 | awk '{print $1}')"
    dinfo() { diskutil info "$1" 2>/dev/null | sed -n "s/^ *$2: *//p" | head -1; }
    DRIVE_MODEL="$(dinfo "$DEV" 'Device \/ Media Name')"
    [ -n "$DRIVE_MODEL" ] || DRIVE_MODEL="$(dinfo "$(dinfo "$DEV" 'Part of Whole')" 'Device \/ Media Name')"
    [ -n "$DRIVE_MODEL" ] || DRIVE_MODEL="$(dinfo "$(dinfo "$DEV" 'APFS Physical Store')" 'Device \/ Media Name')"
    [ -n "$DRIVE_MODEL" ] || DRIVE_MODEL="unknown (macOS)"
    # No O_DIRECT on macOS: a dd here reads the page cache. The engine's own storage probe reads
    # the file with F_NOCACHE and prints its curve as a `plan: storage:` line, so the column is
    # filled from the first run below instead.
    DD_FLAGS=""
else
    CPU_MODEL="$(readf /proc/cpuinfo | sed -n 's/^model name[[:space:]]*: //p' | head -1 | sed 's/[[:space:]]*$//')"
    # aarch64 boards usually have no "model name"; fall back to the device-tree model.
    [ -n "$CPU_MODEL" ] || CPU_MODEL="$(readf /sys/firmware/devicetree/base/model | tr -d '\0')"
    [ -n "$CPU_MODEL" ] || CPU_MODEL="unknown ($(uname -m))"
    MEM_KB="$(readf /proc/meminfo | sed -n 's/^MemTotal:[[:space:]]*\([0-9]*\).*/\1/p' | head -1)"
    RAM_GIB="$(( ${MEM_KB:-0} / 1024 / 1024 ))"
    KERNEL="$(uname -sr)"
    DEV="$(df --output=source "$MODEL" 2>/dev/null | tail -1)"
    BLOCK="$(basename "$(readlink -f "$DEV" 2>/dev/null || echo "$DEV")" | sed -E 's/p?[0-9]+$//')"
    DRIVE_MODEL="$(readf "/sys/block/$BLOCK/device/model" | tr -d '\n' | sed 's/[[:space:]]*$//')"
    [ -n "$DRIVE_MODEL" ] || DRIVE_MODEL="$BLOCK"
    DD_FLAGS="iflag=direct"
fi
# Sizes must dereference: a Hugging Face cache path is a symlink into blobs/, and `du` on the
# link itself measures the link (0), not the file. `stat -L` follows it on both GNU and BSD.
fsize() { stat -Lc %s "$1" 2>/dev/null || stat -Lf %z "$1" 2>/dev/null || echo 0; }
MODEL_BYTES="$(fsize "$MODEL")"

# A split model is one model: report the sum of every sibling shard, not just the shard that was
# passed on the command line (llama.cpp finds the others from it the same way). The storage probe
# reads the largest shard: the first one can be metadata alone (5 MB beside two 40-50 GB shards),
# and a 1 GiB read a quarter into 5 MB reads nothing and times the open (#321's 960 MiB/s).
TOTAL_BYTES="$MODEL_BYTES"
PROBE_FILE="$MODEL"
PROBE_BYTES="$MODEL_BYTES"
BASE="$(basename "$MODEL")"
if echo "$BASE" | grep -qE -- '-[0-9]+-of-[0-9]+\.gguf$'; then
    SHARD_GLOB="$(echo "$BASE" | sed -E 's/-[0-9]+-of-([0-9]+)\.gguf$/-*-of-\1.gguf/')"
    TOTAL_BYTES=0
    for SHARD in "$(dirname "$MODEL")/"$SHARD_GLOB; do
        SHARD_BYTES="$(fsize "$SHARD")"
        TOTAL_BYTES=$(( TOTAL_BYTES + SHARD_BYTES ))
        if [ "$SHARD_BYTES" -gt "$PROBE_BYTES" ]; then PROBE_FILE="$SHARD"; PROBE_BYTES="$SHARD_BYTES"; fi
    done
fi
MODEL_GIB="$(awk -v b="$TOTAL_BYTES" 'BEGIN { printf "%.1f", b/1024/1024/1024 }')"

# Read 1 GiB of the model itself at 512 KiB, bypassing the page cache, from a random-ish offset
# (one quarter in) so a freshly downloaded file's cached head does not flatter the number.
# The rate comes from the byte count in dd's own summary, never the nominal 1 GiB: dd exits 0
# on a short read (EOF before count), and a filesystem can accept iflag=direct yet serve the
# page cache anyway — that case shows up as a rate no single drive can do, and prints n/a.
probe_read_rate() {
    local skip=$(( PROBE_BYTES / 4 / 524288 ))
    local t0 t1 bytes
    t0="$(date +%s.%N)"
    bytes="$(dd if="$PROBE_FILE" of=/dev/null bs=512k count=2048 skip="$skip" $DD_FLAGS 2>&1 |
        sed -n 's/^\([0-9][0-9]*\) bytes.*/\1/p' | head -1 || true)"
    t1="$(date +%s.%N)"
    awk -v a="$t0" -v b="$t1" -v n="${bytes:-0}" 'BEGIN {
        d = b - a
        if (d <= 0 || n <= 0) { print "n/a"; exit }
        r = n / 1048576 / d
        if (r > 30720) {
            print "storage probe: " int(r) " MiB/s is not a physical read rate; O_DIRECT was likely ignored, reporting n/a" > "/dev/stderr"
            print "n/a"; exit
        }
        printf "%.0f", r
    }'
}
if [ -n "$DD_FLAGS" ]; then
    READ_MIBS="$(probe_read_rate || true)"
    [ -n "$READ_MIBS" ] || READ_MIBS="n/a"
    READ_HOW="512 KiB, O_DIRECT"
else
    READ_MIBS="n/a"
    READ_HOW="engine probe, F_NOCACHE"
fi

# --- 3. run ---------------------------------------------------------------------------------
mkdir -p "$BENCH_OUT/routing"
TAG="$(basename "$MODEL" .gguf)"

# The record of a previous invocation would make the "first load" a second one: start clean.
rm -f "$BENCH_OUT/routing"/*.routing "$BENCH_OUT/routing"/*.knobs
RUN_LABELS=("first load" "on the record")
CSVS=(); LOGS=()
for (( i = 1; i <= BENCH_RUNS && i <= 2; i++ )); do
    # The kernel returns a process's locked memory after it exits, not at once: a plan made in the
    # seconds after the first run sees a machine with gigabytes less than it has (on a 16 GB laptop,
    # 3.4 GB available against 11 GB a minute later) and sizes a cache half the right one. Wait
    # for the memory to come back; the note under the table says if it had not.
    [ "$i" -gt 1 ] && [ "$BENCH_SETTLE" -gt 0 ] && { echo "settling ${BENCH_SETTLE}s for the first run's memory to return" >&2; sleep "$BENCH_SETTLE"; }
    CSV="$BENCH_OUT/$TAG.auto$i.csv"
    LOG="$BENCH_OUT/$TAG.auto$i.log"
    echo "running $i/$BENCH_RUNS: $TAG, --auto (${RUN_LABELS[i-1]}), $N_PREDICT tokens" >&2
    "$BMOE_CLI" -m "$MODEL" --chatml -n "$N_PREDICT" --auto --plan-explain \
        --routing-dir "$BENCH_OUT/routing" --csv "$CSV" "$@" -p "$PROMPT" > "$LOG" 2>&1 ||
        { echo "bmoe-cli failed, see $LOG" >&2; exit 1; }
    CSVS+=("$CSV"); LOGS+=("$LOG")
done

# On macOS the storage column comes from the engine's probe: one lane at the request size of its
# curve nearest 512 KiB, the shape of the dd probe on Linux (one 512 KiB stream), so rows compare.
if [ "$READ_MIBS" = "n/a" ] && [ -z "$DD_FLAGS" ]; then
    PICK="$(grep '^plan: storage:' "${LOGS[0]}" | head -1 | tr ';' '\n' |
        sed -n 's/^ *\([0-9]*\) KiB: 1 lane \([0-9]*\) MiB\/s.*/\1 \2/p' |
        awk 'BEGIN { best = -1 } { d = $1 - 512; if (d < 0) d = -d; if (best < 0 || d < best) { best = d; pick = $0 } } END { print pick }')"
    if [ -n "$PICK" ]; then
        READ_MIBS="${PICK#* }"
        READ_HOW="${PICK%% *} KiB, F_NOCACHE, engine probe"
    fi
fi

# --- 4. report ------------------------------------------------------------------------------
# The trailer and the header lines are whitespace-separated key=value; read by NAME, never by
# position. The row reads its settings from the CSV, so it shows what the plan chose.
row() {
    local label="$1" csv="$2"
    local summary; summary="$(grep '^# summary' "$csv" | tail -1)"
    key() { echo "$summary" | tr ' ' '\n' | sed -n "s|^$1=||p" | head -1; }
    hdr() { grep -o "[ #]$1=[^ ]*" "$csv" | head -1 | cut -d= -f2; }
    local streams="resident"; [ "$(hdr moe_stream)" = "1" ] && streams="streams"
    # n_ubatch=0 in the header is the engine's default, not a width of zero.
    local ubatch; ubatch="$(hdr n_ubatch)"; [ "${ubatch:-0}" != "0" ] || ubatch="default"
    local mib_per_tok
    mib_per_tok="$(awk -v r="$(key read_MiB)" -v n="$(key tokens)" 'BEGIN { if (n>0) printf "%.0f", r/n; else print "n/a" }')"
    echo "| auto, $label ($streams) | $(hdr threads) | $(hdr n_expert_used) | $(key cache_budget_MiB) MiB | $(hdr io_threads) | $ubatch | $(key tok/s) | $(key prefill_tps) | $(key stall_s/tok) | $(key compute_s/tok) | $mib_per_tok MiB | $(key cache_hit_pct)% | $(key majflt/tok) | $(key load_s) | $(key experts_dropped) |"
}
ARCH="$(grep -o 'arch=[^ ]*' "${CSVS[0]}" | head -1 | cut -d= -f2)"

cat <<EOF

<!-- paste everything below into the benchmark-report issue -->
**Machine**

| CPU | Cores | RAM | Storage | Storage read ($READ_HOW) | OS |
|---|---:|---:|---|---:|---|
| $CPU_MODEL | $NPROC | $RAM_GIB GiB | $DRIVE_MODEL | $READ_MIBS MiB/s | $KERNEL |

**Run** — engine $ENGINE_VERSION ($GIT_REV), model \`$(basename "$MODEL")\` ($MODEL_GIB GiB, arch \`$ARCH\`), $N_PREDICT tokens per run. The table row is the last one; the first load is what a cold planner predicted.

| Mode | Threads | k | Cache | Lanes | ubatch | Decode tok/s | Prefill tok/s | Stall s/tok | Compute s/tok | Flash/token | Cache hit | majflt/tok | Load s | Experts dropped |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
EOF
for (( i = 0; i < ${#CSVS[@]}; i++ )); do row "${RUN_LABELS[i]}" "${CSVS[i]}"; done
echo
CSV_NAMES=(); for c in "${CSVS[@]}"; do CSV_NAMES+=("$(basename "$c")"); done
echo "Extra flags: \`${*:-none}\`. Raw CSVs: \`${CSV_NAMES[*]}\` in \`$BENCH_OUT\` (attach them to the issue)."
# The lines that close the loop: whether the engine streamed, and what the plan predicted against
# what the run measured. The second run's gap is the planner's error on this machine.
echo
for (( i = 0; i < ${#LOGS[@]}; i++ )); do
    echo "Run $((i+1)), ${RUN_LABELS[i]}:"
    echo '```'
    grep -E '^(mode:|plan: predicted)' "${LOGS[i]}" || echo "(no mode or prediction line in the log)"
    echo '```'
done
# Each plan starts from the memory it saw; when the second saw much less than the first, the
# first run's memory had not been returned yet and the second row measured a starved plan.
if [ "${#LOGS[@]}" -eq 2 ]; then
    avail() { sed -n 's/^plan: machine [0-9]* cores, \([0-9]*\) MiB available.*/\1/p' "$1" | head -1; }
    A1="$(avail "${LOGS[0]}")"; A2="$(avail "${LOGS[1]}")"
    if [ -n "$A1" ] && [ -n "$A2" ] && [ "$(( A2 * 10 ))" -lt "$(( A1 * 7 ))" ]; then
        echo
        echo "**Note:** the second plan saw $A2 MiB available where the first saw $A1: the first run's memory had not been returned when it planned, so its row is a starved plan. Run again with a longer \`BENCH_SETTLE\` (seconds between the runs, $BENCH_SETTLE here)."
    fi
fi

# The plan is what makes the row reproducible: the devices it used, what stayed resident, what
# streamed. It names GPU models, never a path: the record's path is dropped and the model's is
# masked on the reproduce line, which is kept because it is the plan as flags.
for (( i = 0; i < ${#LOGS[@]}; i++ )); do
    echo
    echo "<details><summary>Plan chosen by <code>--auto</code>, run $((i+1)) (${RUN_LABELS[i]})</summary>"
    echo
    echo '```'
    grep -E '^plan:|^  bmoe-cli -m ' "${LOGS[i]}" |
        grep -v -E "^plan: predicted|^plan: this model's routing read from" |
        sed 's|^  bmoe-cli -m [^ ]* |  bmoe-cli -m MODEL.gguf |' || echo "(no plan lines in the log)"
    echo '```'
    echo
    echo "</details>"
done
