#!/usr/bin/env bash
# One session on a machine we do not own.
#
# The planner's rules are unit-tested against synthetic machines, which is what lets them claim
# anything about hardware nobody here has. What they cannot do is tell us whether the PROBES are
# faithful on a machine none of them has ever run on. This script is that session: build, plan,
# measure, and write down what the plan predicted next to what the engine did.
#
# It is deliberately linear and quotes everything it runs. A rented box is billed by the hour and
# is gone afterwards, so the output has to be self-contained enough to read a week later.
#
#   ./scripts/rented-box.sh /path/to/model.gguf [outdir]
#
# What it will NOT do: install anything, touch the network beyond the model you already placed, or
# run the lossy levers. It cools nothing either - on a thermally limited machine, read
# docs/benchmark-method.md first.

set -euo pipefail

MODEL="${1:?usage: rented-box.sh /path/to/model.gguf [outdir]}"
OUT="${2:-bmoe-rented-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"

say() { printf '\n=== %s\n' "$*" | tee -a "$OUT/log.txt"; }
run() { printf '$ %s\n' "$*" | tee -a "$OUT/log.txt"; "$@" 2>&1 | tee -a "$OUT/log.txt"; }

say "machine"
{
    uname -a
    echo "--- cpu"
    lscpu 2>/dev/null | head -20 || true
    echo "--- core max frequencies (the planner's thread rule reads these)"
    for f in /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq; do
        [ -r "$f" ] && echo "$f $(cat "$f")"
    done | sort -u -k2 -n || echo "(not published: the plan will say threads [unprobed])"
    echo "--- memory"
    grep -E 'MemTotal|MemAvailable|SwapTotal|Inactive\(anon\)' /proc/meminfo 2>/dev/null || true
    echo "--- compressed swap (decides the dense policy and the headroom estimate)"
    ls -d /sys/block/zram0 2>/dev/null && cat /sys/block/zram0/mm_stat 2>/dev/null || echo "(no zram)"
    echo "--- accelerators"
    (command -v nvidia-smi >/dev/null && nvidia-smi --query-gpu=name,memory.total,memory.free --format=csv) || echo "(no nvidia-smi)"
    echo "--- storage under the model"
    df -h "$MODEL" || true
} > "$OUT/machine.txt" 2>&1
cat "$OUT/machine.txt"

say "build"
# Backends are opt-in at configure time. Enable what the box has; a build without a device is a
# valid run of this script and answers the CPU half of every question below.
#
# The alternative is scripts/build-portable.sh, which builds the backends as separate libraries and
# lets ggml find them at start-up - one binary that adapts. It costs `--overlap` and the native CPU
# tuning, so it is the right answer for shipping and the wrong one for a benchmark that has to be
# comparable with the numbers already on record.
CMAKE_EXTRA="${BMOE_CMAKE_EXTRA:-}"
run cmake -S . -B build-rented -DCMAKE_BUILD_TYPE=Release -DBMOE_BUILD_TESTS=ON ${CMAKE_EXTRA}
run cmake --build build-rented -j
say "gates (correctness before speed, always)"
(cd build-rented && ctest --output-on-failure) 2>&1 | tee -a "$OUT/log.txt"

CLI=build-rented/bin/bmoe-cli

say "the plan, from free facts only"
run "$CLI" -m "$MODEL" --plan-only --no-probe-io

say "the plan, with the storage probe (this is the shipping path)"
for i in 1 2 3; do
    run "$CLI" -m "$MODEL" --plan-only
done

say "the plan, with the headroom measured (first run of this probe on any machine)"
# The one probe that puts a live machine under real pressure. It grows in steps and stops at the
# first sign of loss, but this is its first outing outside a synthetic test: if the box wedges,
# that IS the finding, and it belongs in the report rather than in a retry loop.
run "$CLI" -m "$MODEL" --plan-only --probe-mem

say "does the plan's prediction hold? interleaved A/B on the lane count"
# The plan picks a lane count from a rate curve. The only thing that settles whether the curve is
# a good instrument is the engine, at the same cache budget, interleaved so a warm cache cannot
# favour whichever ran first.
LANES_PLANNED=$("$CLI" -m "$MODEL" --plan-only 2>&1 | sed -n 's/.*io-threads *\([0-9]*\).*/\1/p' | head -1)
echo "plan picked ${LANES_PLANNED:-?} lanes" | tee -a "$OUT/log.txt"
for rep in 1 2 3; do
    for lanes in 1 2 4 8; do
        printf 'rep %s lanes %s: ' "$rep" "$lanes" | tee -a "$OUT/ab-lanes.txt"
        "$CLI" -m "$MODEL" --auto --io-threads "$lanes" -n 64 -p "Explain gravity in one sentence." 2>&1 |
            sed -n 's/.*generation:.*(\([0-9.]*\) tok\/s).*/\1/p' | tee -a "$OUT/ab-lanes.txt"
    done
done

say "auto against the historical default, interleaved"
for rep in 1 2 3; do
    printf 'rep %s auto: ' "$rep" | tee -a "$OUT/ab-auto.txt"
    "$CLI" -m "$MODEL" --auto -n 64 -p "Explain gravity in one sentence." 2>&1 |
        sed -n 's/.*generation:.*(\([0-9.]*\) tok\/s).*/\1/p' | tee -a "$OUT/ab-auto.txt"
    printf 'rep %s manual: ' "$rep" | tee -a "$OUT/ab-auto.txt"
    "$CLI" -m "$MODEL" --moe-stream --cache-mb 2000 --io-threads 4 --overlap -n 64 \
        -p "Explain gravity in one sentence." 2>&1 |
        sed -n 's/.*generation:.*(\([0-9.]*\) tok\/s).*/\1/p' | tee -a "$OUT/ab-auto.txt"
done

say "done — $OUT"
cat <<'NOTES' | tee -a "$OUT/log.txt"

What to read out of this, in order of what it settles:

  1. Does the plan DECLINE anything it should not, or arm anything it should not? The rationale
     names a fact for every line; a line that says [unprobed] on a machine that could have answered
     is a probe that did not work here.
  2. Lane count: does the engine's best match what the plan picked? On the desktop this was measured
     wrong once already - the probe reads throughput, and a lane also buys latency.
  3. --probe-mem: does the measured headroom exceed the reported available figure, and by how much?
     On a machine that compresses it should. On one that does not, the two should agree closely, and
     a large gap means the probe is measuring something else.
  4. With a GPU present: the plan should NAME the device and arm nothing. If it arms something, that
     is a bug - see docs/hardware-planning.md, the buffer-type route is closed.
  5. auto vs the historical fixed knobs: the whole point, in one number.
NOTES
