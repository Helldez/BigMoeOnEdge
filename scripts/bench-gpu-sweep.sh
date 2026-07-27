#!/system/bin/sh
# GPU offload sweep, run ON the device, detached from adb.
#
# Why detached: this bench takes ~15 minutes, and adb over Wi-Fi does not survive that — the phone
# drops the connection when the screen locks, which kills the shell and the run with it. `setsid`
# puts the sweep in its own session so it keeps going, and every cell writes its result as it
# finishes, so a drop costs the connection and not the data.
#
# Usage (from the host, once the app is installed and the CLI staged):
#   adb shell run-as io.bigmoeonedge.example.dev sh -c \
#     'setsid sh /data/local/tmp/bmoe/bench-gpu-sweep.sh </dev/null >/dev/null 2>&1 &'
# then collect:
#   adb shell run-as io.bigmoeonedge.example.dev cat <APPDATA>/files/gpu-sweep/results.txt
#
# The cell order is deliberately not monotone in --gpu-layers, and the CPU cell runs first AND last:
# thermal drift and accumulated memory pressure both make later cells worse, so a sweep that only
# went 0,1,12,48 could not tell a real effect from the device getting tired. The two CPU cells
# bracket that drift and say how much of any difference is the device rather than the config.
set -u

APP=io.bigmoeonedge.example.dev
LIB=$(pm path $APP 2>/dev/null | head -1 | sed 's|package:||; s|/base.apk||')/lib/arm64
OUT=/data/data/$APP/files/gpu-sweep
MODEL=${MODEL:-/data/local/tmp/bmoe/Qwen3-30B-A3B-Q4_K_M.gguf}
NPRED=${NPRED:-96}
COOLDOWN=${COOLDOWN:-90}

mkdir -p "$OUT"
export LD_LIBRARY_PATH="$LIB:/system/lib64:/vendor/lib64"
CLI="$LIB/libbmoe-cli.so"

# One shared configuration; only --gpu-layers varies between cells. --ubatch is capped because the
# compute buffers are reserved for the widest graph, and at a full-context ubatch that reservation
# (1203 MiB with a GPU in the graph) dominates everything this sweep is trying to measure.
ARGS="--moe-stream --cache-mb 2000 --io-threads 4 --overlap --dense-weights anon -c 2048 --ubatch 256 -n $NPRED"
PROMPT="Explain gravity in one paragraph."

RESULTS="$OUT/results.txt"
: > "$RESULTS"
echo "model=$MODEL n_predict=$NPRED cooldown=${COOLDOWN}s" >> "$RESULTS"

for g in 0 48 12 1 0; do
    if [ "$g" -eq 0 ]; then EXTRA=""; else EXTRA="--gpu-layers $g"; fi
    cell="g$g-$(date +%H%M%S)"
    echo "=== $cell start $(date +%T)" >> "$RESULTS"
    # Capture the whole cell: a crash or an OOM kill has to be visible as such, not as a blank row.
    $CLI -m "$MODEL" $ARGS -p "$PROMPT" $EXTRA > "$OUT/$cell.out" 2>&1
    rc=$?
    grep -E "^generation:|^compute:|^moe-stream:|graph splits|compute buffer size =|OpenCL model buffer" \
        "$OUT/$cell.out" >> "$RESULTS" 2>/dev/null
    echo "=== $cell done rc=$rc $(date +%T)" >> "$RESULTS"
    sleep "$COOLDOWN"
done

echo "ALLDONE $(date +%T)" >> "$RESULTS"
