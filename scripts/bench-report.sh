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
#   3. runs the model twice, 256 greedy tokens each, with the same prompt every table in the
#      README uses (docs/benchmark-method.md):
#        auto   — `--auto` and nothing else (`--plan-explain` only prints the plan it runs): what
#                 the engine does on this machine. The planner places what fits on the GPUs the build carries, keeps a model that fits in RAM
#                 resident, and streams only what is left. This is the number to compare with
#                 other engines.
#        stream — the fixed streaming protocol the README rows use: streaming forced on with an
#                 auto-sized cache, 4 read lanes, intra-layer overlap, the dense weights kept out
#                 of the page cache, CPU only, and the 512-wide prefill batch the Android app pins.
#                 It measures the streamer, not the machine: where the model fits in RAM or VRAM
#                 it is slower than `auto` by design.
#      `auto` runs first, so the planner sizes its plan on an idle machine;
#   4. parses the CSV trailer of each run and prints one markdown block, with the plan `auto` chose.
#
# Env overrides: MODES ("auto stream"; name one to run only that), THREADS (stream only, default:
# min(8, online cores)), N_PREDICT (256), IO_THREADS (4), CACHE_MB (auto), UBATCH (512),
# BENCH_OUT (.bench-report), BMOE_CLI (build/bin/bmoe-cli).
# Anything after the model path is passed to bmoe-cli verbatim in both modes (e.g. --no-think for
# gpt-oss, --n-expert-used 6 for the turbo top-k rows).
set -euo pipefail

if [ $# -lt 1 ] || [ ! -f "$1" ]; then
    echo "usage: $0 MODEL.gguf [extra bmoe-cli flags...]" >&2
    exit 2
fi
MODEL="$1"; shift

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readf() { cat "$1" 2>/dev/null || true; }   # optional sysfs/procfs files: absent is not an error
OS="$(uname -s)"
NPROC="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
THREADS="${THREADS:-$(( NPROC < 8 ? NPROC : 8 ))}"
MODES="${MODES:-auto stream}"
N_PREDICT="${N_PREDICT:-256}"
IO_THREADS="${IO_THREADS:-4}"
CACHE_MB="${CACHE_MB:-auto}"
UBATCH="${UBATCH:-512}"   # matches the app: a wider graph reserves buffers the expert cache wants
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
    DRIVE_MODEL="unknown (macOS)"
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
# passed on the command line (llama.cpp finds the others from it the same way).
TOTAL_BYTES="$MODEL_BYTES"
BASE="$(basename "$MODEL")"
if echo "$BASE" | grep -qE -- '-[0-9]+-of-[0-9]+\.gguf$'; then
    SHARD_GLOB="$(echo "$BASE" | sed -E 's/-[0-9]+-of-([0-9]+)\.gguf$/-*-of-\1.gguf/')"
    TOTAL_BYTES=0
    for SHARD in "$(dirname "$MODEL")/"$SHARD_GLOB; do
        TOTAL_BYTES=$(( TOTAL_BYTES + $(fsize "$SHARD") ))
    done
fi
MODEL_GIB="$(awk -v b="$TOTAL_BYTES" 'BEGIN { printf "%.1f", b/1024/1024/1024 }')"

# Read 1 GiB of the model itself at 512 KiB, bypassing the page cache, from a random-ish offset
# (one quarter in) so a freshly downloaded file's cached head does not flatter the number.
# The rate comes from the byte count in dd's own summary, never the nominal 1 GiB: dd exits 0
# on a short read (EOF before count), and a filesystem can accept iflag=direct yet serve the
# page cache anyway — that case shows up as a rate no single drive can do, and prints n/a.
probe_read_rate() {
    local skip=$(( MODEL_BYTES / 4 / 524288 ))
    local t0 t1 bytes
    t0="$(date +%s.%N)"
    bytes="$(dd if="$MODEL" of=/dev/null bs=512k count=2048 skip="$skip" $DD_FLAGS 2>&1 |
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
READ_MIBS="$(probe_read_rate || true)"
[ -n "$READ_MIBS" ] || READ_MIBS="n/a"

# --- 3. run ---------------------------------------------------------------------------------
mkdir -p "$BENCH_OUT"
TAG="$(basename "$MODEL" .gguf)"

run_mode() {
    local mode="$1"; shift
    local csv="$BENCH_OUT/$TAG.$mode.csv" log="$BENCH_OUT/$TAG.$mode.log"
    case "$mode" in
        auto)
            echo "running: $TAG, auto, $N_PREDICT tokens" >&2
            "$BMOE_CLI" -m "$MODEL" --chatml -n "$N_PREDICT" --auto --plan-explain \
                --csv "$csv" "$@" -p "$PROMPT" > "$log" 2>&1 ;;
        stream)
            echo "running: $TAG, stream, $THREADS threads, $IO_THREADS lanes, cache $CACHE_MB, ubatch $UBATCH, $N_PREDICT tokens" >&2
            "$BMOE_CLI" -m "$MODEL" --chatml -n "$N_PREDICT" -t "$THREADS" --ubatch "$UBATCH" \
                --moe-stream --cache-mb "$CACHE_MB" --io-threads "$IO_THREADS" --overlap --dense-weights anon \
                --csv "$csv" "$@" -p "$PROMPT" > "$log" 2>&1 ;;
        *) echo "unknown mode '$mode' in MODES (auto, stream)" >&2; exit 2 ;;
    esac || { echo "bmoe-cli failed in mode $mode, see $log" >&2; exit 1; }
}
for MODE in $MODES; do run_mode "$MODE" "$@"; done

# --- 4. report ------------------------------------------------------------------------------
# The trailer and the header lines are whitespace-separated key=value; read by NAME, never by
# position. Each row reads its settings from its own CSV, so the `auto` row shows what the plan
# chose rather than what this script would have passed.
row() {
    local mode="$1" csv="$BENCH_OUT/$TAG.$1.csv"
    local summary; summary="$(grep '^# summary' "$csv" | tail -1)"
    key() { echo "$summary" | tr ' ' '\n' | sed -n "s|^$1=||p" | head -1; }
    hdr() { grep -o "[ #]$1=[^ ]*" "$csv" | head -1 | cut -d= -f2; }
    local streams="resident"; [ "$(hdr moe_stream)" = "1" ] && streams="streams"
    local mib_per_tok
    mib_per_tok="$(awk -v r="$(key read_MiB)" -v n="$(key tokens)" 'BEGIN { if (n>0) printf "%.0f", r/n; else print "n/a" }')"
    echo "| $mode ($streams) | $(hdr threads) | $(hdr n_expert_used) | $(key cache_budget_MiB) MiB | $(hdr io_threads) | $(hdr n_ubatch) | $(key tok/s) | $(key prefill_tps) | $(key stall_s/tok) | $(key compute_s/tok) | $mib_per_tok MiB | $(key cache_hit_pct)% | $(key majflt/tok) | $(key load_s) | $(key experts_dropped) |"
}
FIRST_CSV="$BENCH_OUT/$TAG.${MODES%% *}.csv"
ARCH="$(grep -o 'arch=[^ ]*' "$FIRST_CSV" | head -1 | cut -d= -f2)"

cat <<EOF

<!-- paste everything below into the benchmark-report issue -->
**Machine**

| CPU | Cores | RAM | Storage | Storage read (512 KiB, O_DIRECT) | OS |
|---|---:|---:|---|---:|---|
| $CPU_MODEL | $NPROC | $RAM_GIB GiB | $DRIVE_MODEL | $READ_MIBS MiB/s | $KERNEL |

**Run** — engine $ENGINE_VERSION ($GIT_REV), model \`$(basename "$MODEL")\` ($MODEL_GIB GiB, arch \`$ARCH\`), $N_PREDICT tokens

| Mode | Threads | k | Cache | Lanes | ubatch | Decode tok/s | Prefill tok/s | Stall s/tok | Compute s/tok | Flash/token | Cache hit | majflt/tok | Load s | Experts dropped |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
EOF
for MODE in $MODES; do row "$MODE"; done
echo
echo "Extra flags: \`${*:-none}\`. Raw CSVs: \`$BENCH_OUT/$TAG.<mode>.csv\` (attach them to the issue)."
if [[ " $MODES " == *" auto "* ]]; then
    # The plan is what makes the `auto` row reproducible: the devices it used, what stayed
    # resident, what streamed. It names GPU models, never a path.
    echo
    echo "<details><summary>Plan chosen by <code>--auto</code></summary>"
    echo
    echo '```'
    grep '^plan:' "$BENCH_OUT/$TAG.auto.log" || echo "(no plan lines in the log)"
    echo '```'
    echo
    echo "</details>"
fi
