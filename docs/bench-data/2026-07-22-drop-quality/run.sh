#!/system/bin/sh
# Quality A/B across drop thresholds. Sequential by construction: never two engines at once.
# Config mirrors the throughput measurement: streamed, cache 3000, 4 lanes, overlap, dense=ahwb.
cd /data/local/tmp/bmoe-q || exit 1
M=/data/local/tmp/bmoe/Qwen_Qwen3.6-35B-A3B-Q4_K_M.gguf
BASE="-m $M -c 4096 -t 4 --chatml --no-think --moe-stream --cache-mb 3000 --io-threads 4 --overlap --dense-weights ahwb --session"
for F in off 0.5 0.75 1.0; do
  if [ "$F" = "off" ]; then EXTRA=""; else EXTRA="--drop-cold-experts $F"; fi
  echo "=== cell $F start $(date) freq=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq) ===" >> qual.log
  LD_LIBRARY_PATH=. ./bmoe-cli $BASE $EXTRA < prompts.jsonl > out-$F.txt 2>> qual.log
  echo "=== cell $F done  $(date) ===" >> qual.log
  sleep 15
done
echo "ALL DONE" >> qual.log
