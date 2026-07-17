#!/system/bin/sh
# gpt-oss-120b, default routing (k=4), cache 2000 MiB, all-O_DIRECT dense: read lanes 4 vs 8.
# Cache 2000 is the smallest budget that is both above cache_min_mb (1500) and above the
# measured 1817 MiB/token working set at k=4 -- below that the cache cannot hold even one
# token's experts and thrashes to a 0 % hit rate.
#
# Cell 1 starts immediately. Between cells we wait for the device to come back under a
# threshold, but only briefly (bounded) -- pass 1 showed a fixed sleep does not reach baseline,
# so instead of pretending, each cell's ENTRY STATE is logged and reported with its number.
# If the two cells enter from materially different states, the lane delta is not trustworthy
# and the pair must be re-run cold.
cd /data/local/tmp || exit 1
OUT=/data/local/tmp/gptlanes
mkdir -p "$OUT"
COMMON="--moe-stream -t 4 --overlap --dense-weights anon --cache-mb 2000 --no-think --n-expert-used 0"
N=256
O=/data/local/tmp/shardllm/gpt-oss-120b-Q4_K_M.gguf

BATT_MAX=380      # deci-C
MEM_MIN=5500000   # kB
WAIT_MAX=180      # bounded: 3 min between cells, then run anyway (state is logged either way)

batt() { dumpsys battery 2>/dev/null | sed -n 's/.*temperature: *//p'; }
mem()  { sed -n 's/^MemAvailable:[[:space:]]*\([0-9]*\).*/\1/p' /proc/meminfo; }
freq() { cat /sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq 2>/dev/null; }

settle() {
  am force-stop io.bigmoeonedge.example 2>/dev/null
  pkill -f bmoe-cli 2>/dev/null
  sync
  W=0
  while [ "$W" -lt "$WAIT_MAX" ]; do
    B=$(batt); M=$(mem)
    [ "$B" -le "$BATT_MAX" ] && [ "$M" -ge "$MEM_MIN" ] && break
    sleep 30; W=$((W+30))
  done
}

run() {
  TAG="$1"; shift
  echo "=============== $TAG ==============="
  echo "ENTRY STATE: batt=$(batt) dC, memavail=$(mem) kB, freq=$(freq), waited=${W:-0}s"
  sh /data/local/tmp/bench-run-doc.sh "$N" "$O" "$OUT/$TAG.csv" "$OUT/$TAG.metrics" "$@"
}

am force-stop io.bigmoeonedge.example 2>/dev/null
pkill -f bmoe-cli 2>/dev/null
run gpt_c2000_l4 $COMMON --io-threads 4
settle
run gpt_c2000_l8 $COMMON --io-threads 8
echo "=============== DONE ==============="
