#!/system/bin/sh
# gpt-oss benchmark matrix: cache auto (ceil 3000) + O_DIRECT + overlap + --no-think.
# Sweep io-threads {4,8} x prefetch {0,4} x top-k {2,3,4} = 12 cells.
# Per cell: full bmoe-cli stdout (the direct answer + perf lines). Deterministic greedy,
# so the answer depends only on k -> quality read once per k, perf per cell.
M=/data/local/tmp/shardllm/gpt-oss-120b-Q4_K_M.gguf
P="What is 17 times 23? Then name the capital of Australia."
OUT=/data/local/tmp/gptoss-matrix.out
COOLDOWN=20
cd /data/local/tmp || exit 1
: > "$OUT"
for K in 2 3 4; do
  for IO in 4 8; do
    for PF in 0 4; do
      PFFLAG=""
      [ "$PF" -gt 0 ] && PFFLAG="--prefetch $PF"
      TAG="k${K}_io${IO}_pf${PF}"
      echo "==================== $TAG ====================" >> "$OUT"
      sleep "$COOLDOWN"
      LD_LIBRARY_PATH=/data/local/tmp ./bmoe-cli -m "$M" --chatml --no-think -c 2048 -n 24 \
        --moe-stream --cache-mb auto --cache-ceil-mb 3000 --overlap \
        --io-threads "$IO" --n-expert-used "$K" $PFFLAG \
        -p "$P" >> "$OUT" 2>/dev/null
      echo "" >> "$OUT"
    done
  done
done
echo "ALL DONE" >> "$OUT"
