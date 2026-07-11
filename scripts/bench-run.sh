#!/system/bin/sh
# Instrumented device-side benchmark run.
#   args: N MODEL CSV METRICS [extra bmoe-cli flags...]
# Runs bmoe-cli (which now reports prefill/TTFT/load itself) while sampling device
# pressure at ~1 Hz: peak process RSS, free-RAM floor, battery + SoC temperature, and the
# battery charge counter before/after. The prompt lives here so no quoting has to survive
# adb/PowerShell. Energy note: over USB the phone is powered/charging, so charge deltas are
# indicative only â€” a true joules/token needs an unplugged (wireless-adb) pass.
N="$1"; MODEL="$2"; CSV="$3"; METRICS="$4"; shift 4
cd /data/local/tmp || exit 1
rm -f "$CSV" "$METRICS" /data/local/tmp/cli_stdout.txt

batt_temp() { dumpsys battery 2>/dev/null | sed -n 's/.*temperature: *//p'; }
charge()    { cat /sys/class/power_supply/battery/charge_counter 2>/dev/null; }
memavail()  { sed -n 's/^MemAvailable: *\([0-9]*\).*/\1/p' /proc/meminfo; }
cpu_temp()  { for z in /sys/class/thermal/thermal_zone*; do
                case "$(cat "$z/type" 2>/dev/null)" in cpu-0-0*|cpu-1-0*|cpu-*) cat "$z/temp" 2>/dev/null; return;; esac
              done; }

MEM0=$(memavail); T0=$(batt_temp); CPU0=$(cpu_temp); CH0=$(charge)

LD_LIBRARY_PATH=/data/local/tmp ./bmoe-cli -m "$MODEL" --chatml -n "$N" \
  -p "Write a long detailed essay about the history of computing including its origins its key milestones the people involved and the future directions of the field" \
  "$@" --csv "$CSV" > /data/local/tmp/cli_stdout.txt 2>/dev/null &
PID=$!

PEAK_RSS=0; MEM_MIN=$MEM0; TEMP_MAX=$T0; CPU_MAX=$CPU0
while kill -0 "$PID" 2>/dev/null; do
  R=$(sed -n 's/^VmRSS: *\([0-9]*\).*/\1/p' /proc/$PID/status 2>/dev/null)
  [ -n "$R" ] && [ "$R" -gt "$PEAK_RSS" ] && PEAK_RSS=$R
  M=$(memavail);  [ -n "$M" ] && [ "$M" -lt "$MEM_MIN" ] && MEM_MIN=$M
  B=$(batt_temp); [ -n "$B" ] && [ "$B" -gt "$TEMP_MAX" ] && TEMP_MAX=$B
  C=$(cpu_temp);  [ -n "$C" ] && [ "$C" -gt "$CPU_MAX" ] && CPU_MAX=$C
  sleep 1
done
wait "$PID"
MEM1=$(memavail); T1=$(batt_temp); CH1=$(charge)

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
  echo "charge_before_uah=$CH0"
  echo "charge_after_uah=$CH1"
} > "$METRICS"

grep -E "generation:|prefill:|moe-stream:|moe-cache:" /data/local/tmp/cli_stdout.txt
cat "$METRICS"
