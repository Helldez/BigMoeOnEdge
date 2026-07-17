#!/system/bin/sh
# Doc benchmark matrix: 3 models x {default top-k, default-2}, all-O_DIRECT dense policy.
#   qwen/gemma: --cache-mb 4000   gpt-oss: --cache-mb 0
# Every run: force-stop the app, 45 s cooldown, then bench-run.sh (which samples pressure).
cd /data/local/tmp || exit 1
OUT=/data/local/tmp/docbench
mkdir -p "$OUT"
COMMON="--moe-stream --io-threads 4 -t 4 --overlap --dense-weights anon"
N=256

Q=/data/local/tmp/shardllm/Qwen3-30B-A3B-Q4_K_M.gguf
G=/data/local/tmp/shardllm/google_gemma-4-26B-A4B-it-Q4_K_M.gguf
O=/data/local/tmp/shardllm/gpt-oss-120b-Q4_K_M.gguf

cool() {
  am force-stop io.bigmoeonedge.example 2>/dev/null
  pkill -f bmoe-cli 2>/dev/null
  sync
  echo "--- cooldown 45s ---"
  sleep 45
}

run() {
  TAG="$1"; MODEL="$2"; shift 2
  cool
  echo "=============== $TAG ==============="
  cat /sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq 2>/dev/null
  sh /data/local/tmp/bench-run-doc.sh "$N" "$MODEL" "$OUT/$TAG.csv" "$OUT/$TAG.metrics" "$@"
  echo "--- text ---"
  sed -n '/^generation:/q;p' /data/local/tmp/cli_stdout.txt 2>/dev/null | tail -20
}

run qwen_kdef  "$Q" $COMMON --cache-mb 4000 --n-expert-used 0
run qwen_k6    "$Q" $COMMON --cache-mb 4000 --n-expert-used 6
run gemma_kdef "$G" $COMMON --cache-mb 4000 --n-expert-used 0
run gemma_k6   "$G" $COMMON --cache-mb 4000 --n-expert-used 6
run gpt_kdef   "$O" $COMMON --cache-mb 0 --no-think --n-expert-used 0
run gpt_k2     "$O" $COMMON --cache-mb 0 --no-think --n-expert-used 2
echo "=============== DONE ==============="
