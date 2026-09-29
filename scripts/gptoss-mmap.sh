#!/system/bin/sh
# Pure-mmap baseline for gpt-oss: NO expert streaming (no --moe-stream, no cache/lanes/
# O_DIRECT/overlap/prefetch) -- llama.cpp mmaps the whole 58 GB model and the OS pages it
# in/out of the page cache on demand. This is the >RAM baseline the streaming modes beat.
# top-k override still applies (it is a load-time kv_override, valid without streaming).
# Appends to the same log as the streaming matrix. k=4 then k=2, per request.
#
# Overridable from the environment, and M/OUT must match the streaming matrix run this is the
# baseline for: OUT is appended to, not replaced.
M=${M:-/data/local/tmp/bmoe/gpt-oss-120b-Q4_K_M.gguf}
P="What is 17 times 23? Then name the capital of Australia."
OUT=${OUT:-/data/local/tmp/gptoss-matrix.out}
COOLDOWN=${COOLDOWN:-20}
cd /data/local/tmp || exit 1
for K in 4 2; do
  TAG="mmap_k${K}"
  echo "==================== $TAG ====================" >> "$OUT"
  sleep "$COOLDOWN"
  LD_LIBRARY_PATH=/data/local/tmp ./bmoe-cli -m "$M" --chatml --no-think -c 2048 -n 24 \
    --n-expert-used "$K" --csv /data/local/tmp/gptoss_${TAG}.csv \
    -p "$P" >> "$OUT" 2>/dev/null
  echo "" >> "$OUT"
done
echo "MMAP DONE" >> "$OUT"
