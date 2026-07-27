#!/system/bin/sh
# Does a freed CPU make the expert cache worth more?
#
# The question this answers is NOT "is the GPU faster" — that was measured and it is not
# (docs/bench-data/2026-07-27-gpu-dense-offload/). It is whether the cache CURVE differs between
# backends. Full offload cuts CPU occupancy from 88% to 54% and leaves decode more I/O-bound
# (compute 0.187 against flash 0.648 s/token), so if compute is no longer competing, each extra MiB
# of cache should convert into throughput more efficiently with the GPU on than off. That is a
# difference of differences: (gpu_big - gpu_small) against (cpu_big - cpu_small).
#
# Which is a demanding thing to measure on a device whose run-to-run noise is +-6%. Two defences:
#
#   * every configuration runs TWICE, and
#   * the cell order is a PALINDROME. Under a drift that is linear in time — which is what thermal
#     drift looked like in the previous sweep, -5.5% over 13 minutes — a palindrome gives every
#     configuration the same mean timestamp, so the drift cancels in the averages instead of being
#     estimated and subtracted.
#
# Usage (from the host):
#   adb shell 'dumpsys deviceidle disable'          # or the phone dozes and kills this
#   adb shell run-as io.bigmoeonedge.example.dev sh -c \
#     'setsid sh /data/local/tmp/bmoe/bench-gpu-cache.sh </dev/null >/dev/null 2>&1 &'
set -u

APP=io.bigmoeonedge.example.dev
LIB=$(pm path $APP 2>/dev/null | head -1 | sed 's|package:||; s|/base.apk||')/lib/arm64
OUT=/data/data/$APP/files/gpu-cache
MODEL=${MODEL:-/data/local/tmp/bmoe/Qwen3-30B-A3B-Q4_K_M.gguf}
NPRED=${NPRED:-96}
COOLDOWN=${COOLDOWN:-90}
SMALL=${SMALL:-2000}
BIG=${BIG:-3500}

mkdir -p "$OUT"
export LD_LIBRARY_PATH="$LIB:/system/lib64:/vendor/lib64"
CLI="$LIB/libbmoe-cli.so"

# --ubatch is capped for the same reason as everywhere else: at a full-context ubatch the compute
# buffer reservation (1203 MiB with a GPU in the graph) dominates whatever else is being measured.
BASE="--moe-stream --io-threads 4 --overlap --dense-weights anon -c 2048 --ubatch 256 -n $NPRED"
PROMPT="Explain gravity in one paragraph."

RESULTS="$OUT/results.txt"
: > "$RESULTS"
echo "model=$MODEL n_predict=$NPRED small=$SMALL big=$BIG cooldown=${COOLDOWN}s" >> "$RESULTS"

run_cell() {
    backend=$1; cache=$2
    if [ "$backend" = "gpu" ]; then EXTRA="--gpu-require"; else EXTRA=""; fi
    cell="$backend-$cache-$(date +%H%M%S)"
    echo "=== $cell start $(date +%T)" >> "$RESULTS"
    $CLI -m "$MODEL" $BASE --cache-mb "$cache" -p "$PROMPT" $EXTRA > "$OUT/$cell.out" 2>&1
    rc=$?
    grep -E "^generation:|^compute:|^moe-stream:|^moe-cache:" "$OUT/$cell.out" >> "$RESULTS" 2>/dev/null
    echo "=== $cell done rc=$rc $(date +%T)" >> "$RESULTS"
    sleep "$COOLDOWN"
}

# The palindrome. Read outward from the middle: each configuration appears once in each half.
run_cell cpu "$SMALL"
run_cell gpu "$SMALL"
run_cell cpu "$BIG"
run_cell gpu "$BIG"
run_cell gpu "$BIG"
run_cell cpu "$BIG"
run_cell gpu "$SMALL"
run_cell cpu "$SMALL"

echo "ALLDONE $(date +%T)" >> "$RESULTS"
