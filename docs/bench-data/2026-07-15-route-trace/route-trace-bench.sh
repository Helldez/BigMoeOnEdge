#!/system/bin/sh
# Route-trace collection: one run per model, each in its documented BEST config.
#
# The point is the routing data (which experts each layer picks), which is deterministic and
# indifferent to clocks. tok/s rides along as context only — so the device state is captured
# around every run and written next to the data, because absolute tok/s means nothing without it.
#
# Per-run outputs in $OUT:  <tag>.route.csv  <tag>.metrics.csv  <tag>.out  <tag>.state
OUT=/data/local/tmp/rt
BIN=/data/local/tmp/bmoe-cli
M=/data/local/tmp/shardllm
COOLDOWN=${COOLDOWN:-90}
P="Explain how a mixture-of-experts model decides which experts handle each token."

mkdir -p "$OUT"
cd /data/local/tmp || exit 1

state() { # $1 = file, $2 = when
    {
        echo "--- $2 ---"
        echo "date: $(date)"
        for p in /sys/devices/system/cpu/cpufreq/policy*; do
            echo "$(basename $p): scaling_max=$(cat $p/scaling_max_freq) hw_max=$(cat $p/cpuinfo_max_freq) cur=$(cat $p/scaling_cur_freq)"
        done
        echo "meminfo: $(grep -E 'MemFree|MemAvailable|^Cached' /proc/meminfo | tr '\n' ' ')"
        echo "battery_temp_dC: $(cat /sys/class/power_supply/battery/temp 2>/dev/null)"
        dumpsys thermalservice 2>/dev/null | grep -E 'mName=CPU[0-3]|Thermal Status' | head -5
    } >> "$1"
}

kill_strays() {
    # A stopped run can leave a CPU-spinning orphan that fakes a throttling regression later.
    pkill -f bmoe-cli 2>/dev/null
    sleep 1
}

run() { # $1=tag  $2=model  $3...=extra flags
    tag=$1; model=$2; shift 2
    echo "===== $tag ====="
    kill_strays
    echo "cooling ${COOLDOWN}s..."
    sleep "$COOLDOWN"
    : > "$OUT/$tag.state"
    state "$OUT/$tag.state" "BEFORE"
    LD_LIBRARY_PATH=/data/local/tmp "$BIN" -m "$model" -p "$P" -t 4 -c 2048 \
        --chatml --no-think --moe-stream --overlap --io-threads 4 \
        --route-trace "$OUT/$tag.route.csv" --csv "$OUT/$tag.metrics.csv" \
        "$@" > "$OUT/$tag.out" 2>&1
    rc=$?
    state "$OUT/$tag.state" "AFTER"
    echo "rc=$rc  $(grep -h '^generation:' "$OUT/$tag.out" 2>/dev/null)"
    echo "trace rows: $(wc -l < "$OUT/$tag.route.csv" 2>/dev/null)"
    kill_strays
}

# ── Qwen3-30B-A3B — best: cache 4000 + overlap + 4 lanes + turbo top-k 6 (5.01 tok/s) ──
run qwen "$M/Qwen3-30B-A3B-Q4_K_M.gguf" -n 128 --cache-mb 4000 --n-expert-used 6

# ── Gemma-4-26B-A4B — best: same recipe; cache 4000 held on a cool device (4.99 tok/s) ──
run gemma "$M/google_gemma-4-26B-A4B-it-Q4_K_M.gguf" -n 128 --cache-mb 4000 --n-expert-used 6

# ── gpt-oss-120b — best: auto cache capped 3000, 4 lanes, prefetch OFF, top-k 2 (0.687 tok/s) ──
# 58 GB = 5.2x RAM. Fewer tokens: it is compute-bound at ~1.5 s/token.
run gptoss "$M/gpt-oss-120b-Q4_K_M.gguf" -n 64 --cache-mb auto --cache-ceil-mb 3000 --n-expert-used 2

echo "ALL DONE"
ls -la "$OUT"
