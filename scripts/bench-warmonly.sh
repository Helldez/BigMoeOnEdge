#!/system/bin/sh
# Warm-up-only validation: dense warm-up ON, dense reservation OFF (default binary now).
# One run per model at the higher k tested before, same recipes as the full battery.
# Confirms the gpt-oss startup win survives and gemma/qwen show no budget-driven regression.
GPTOSS=/data/local/tmp/shardllm/gpt-oss-120b-Q4_K_M.gguf
QWEN=/sdcard/Download/Qwen3-30B-A3B-Q4_K_M.gguf
GEMMA=/sdcard/Download/google_gemma-4-26B-A4B-it-Q4_K_M.gguf
PSHORT="What is 17 times 23? Then name the capital of Australia."
PLONG="Write a long detailed essay about the history of computing including its origins its key milestones the people involved and the future directions of the field"
cd /data/local/tmp || exit 1

settle() {
  pkill -f bmoe-cli 2>/dev/null; sleep 2
  while pgrep -x bmoe-cli >/dev/null 2>&1; do pkill -9 -f bmoe-cli 2>/dev/null; sleep 1; done
  echo "--- cooldown ${1}s ---"; sleep "$1"
}

run() {
  TAG="$1"; M="$2"; N="$3"; K="$4"; IO="$5"; CEIL="$6"; shift 6
  echo "==================== $TAG (k=$K io=$IO n=$N) ===================="
  echo "start batt=$(dumpsys battery 2>/dev/null | sed -n 's/.*temperature: *//p')dC"
  rm -f "/data/local/tmp/wo_$TAG.csv"
  LD_LIBRARY_PATH=/data/local/tmp ./bmoe-cli -m "$M" --chatml -n "$N" \
    --moe-stream --cache-mb auto --cache-ceil-mb "$CEIL" --io-threads "$IO" -t 4 --overlap \
    --n-expert-used "$K" --csv "/data/local/tmp/wo_$TAG.csv" "$@" 2>&1 \
    | grep -E "cache auto|dense warm-up|generation:|moe-cache:"
  echo "csv rows: $(wc -l < /data/local/tmp/wo_$TAG.csv)"
}

run gptoss_k4 "$GPTOSS" 24 4 8 3000 --no-think -p "$PSHORT"; settle 90
run qwen_k8   "$QWEN"  256 8 4 4000 -p "$PLONG"; settle 90
run gemma_k8  "$GEMMA" 256 8 4 4000 -p "$PLONG"
settle 1
echo "ALL DONE"
