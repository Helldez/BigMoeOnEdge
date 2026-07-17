#!/system/bin/sh
# Same as scripts/bench-run.sh, but keeps stderr in a per-run log so the effective
# n_expert_used / dense policy is on the record next to the numbers.
#   args: N MODEL CSV METRICS [extra bmoe-cli flags...]
N="$1"; MODEL="$2"; CSV="$3"; METRICS="$4"; shift 4
cd /data/local/tmp || exit 1
LOG="${CSV%.csv}.log"
rm -f "$CSV" "$METRICS" "$LOG" /data/local/tmp/cli_stdout.txt

batt_temp() { dumpsys battery 2>/dev/null | sed -n 's/.*temperature: *//p'; }
memavail()  { sed -n 's/^MemAvailable:[[:space:]]*\([0-9]*\).*/\1/p' /proc/meminfo; }
vmhwm()     { sed -n 's/^VmHWM:[[:space:]]*\([0-9]*\).*/\1/p' /proc/$1/status 2>/dev/null; }
cpu_temp()  { for z in /sys/class/thermal/thermal_zone*; do
                case "$(cat "$z/type" 2>/dev/null)" in cpu-0-0*|cpu-1-0*|cpu-*) cat "$z/temp" 2>/dev/null; return;; esac
              done; }

MEM0=$(memavail); T0=$(batt_temp); CPU0=$(cpu_temp)

LD_LIBRARY_PATH=/data/local/tmp ./bmoe-cli -m "$MODEL" --chatml -n "$N" \
  -p "Write a long detailed essay about the history of computing including its origins its key milestones the people involved and the future directions of the field" \
  "$@" --csv "$CSV" > /data/local/tmp/cli_stdout.txt 2> "$LOG" &
PID=$!

PEAK_RSS=0; MEM_MIN=$MEM0; TEMP_MAX=$T0; CPU_MAX=$CPU0
while kill -0 "$PID" 2>/dev/null; do
  R=$(vmhwm "$PID");   [ -n "$R" ] && [ "$R" -gt "$PEAK_RSS" ] && PEAK_RSS=$R
  M=$(memavail);       [ -n "$M" ] && [ "$M" -lt "$MEM_MIN" ] && MEM_MIN=$M
  B=$(batt_temp);      [ -n "$B" ] && [ "$B" -gt "$TEMP_MAX" ] && TEMP_MAX=$B
  C=$(cpu_temp);       [ -n "$C" ] && [ "$C" -gt "$CPU_MAX" ] && CPU_MAX=$C
  sleep 1
done
wait "$PID"
MEM1=$(memavail); T1=$(batt_temp)

{
  echo "peak_rss_kb=$PEAK_RSS"
  echo "mem_avail_before_kb=$MEM0"
  echo "mem_avail_floor_kb=$MEM_MIN"
  echo "mem_avail_after_kb=$MEM1"
  echo "batt_temp_before_dC=$T0"
  echo "batt_temp_max_dC=$TEMP_MAX"
  echo "batt_temp_after_dC=$T1"
  echo "cpu_temp_before_mC=$CPU0"
  echo "cpu_temp_max_mC=$CPU_MAX"
} > "$METRICS"

grep -E "n_expert|n_expert_used" "$LOG" | head -4
grep -E "generation:|prefill:|moe-stream:|moe-cache:|moe-dense:|moe-overlap:|moe-prefetch:" /data/local/tmp/cli_stdout.txt
cat "$METRICS"
cp /data/local/tmp/cli_stdout.txt "${CSV%.csv}.out"
