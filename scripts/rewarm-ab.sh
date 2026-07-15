#!/system/bin/sh
# A/B for the rewarm pass: does a session that idles through a reclaim recover?
#
# Two turns with a real idle gap between them, which is when Android's reclaim daemon compresses an
# idle process's anonymous memory into zram. Turn 2 is the measurement: without --rewarm it refaults
# its working set inside the decode, with it the pass restores the pages first. Same binary both
# runs — only the flag differs, so no build difference can explain the gap.
#
# The gap is timed from turn 1's BMOE_DONE (not from process start), so the model load and turn 1
# do not eat into it. VmSwap/VmRSS are sampled through the gap: that log is the evidence that a
# reclaim actually happened, without which turn 2's number means nothing.
#
#   usage: rewarm-ab.sh <tag> [extra bmoe-cli flags...]
#     e.g. rewarm-ab.sh off --no-rewarm
#          rewarm-ab.sh on
set -u
TAG="${1:?usage: rewarm-ab.sh <tag> [flags...]}"
shift

M=/data/local/tmp/shardllm/gpt-oss-120b-Q4_K_M.gguf
BIN=/data/local/tmp/bmoe-cli-rewarm
IDLE="${IDLE:-240}"
D=/data/local/tmp
OUT="$D/rewarm-ab-$TAG.out"
MEM="$D/rewarm-ab-$TAG.mem"
IN="$D/rewarm-ab-$TAG.in"

cd "$D" || exit 1
rm -f "$OUT" "$MEM" "$IN"
: > "$IN"

# Requests are appended to a file that `tail -f` streams into the session's stdin: SELinux forbids
# mkfifo under /data/local/tmp, and tail keeps stdin open the same way a FIFO would.
# Same argv the Android app uses, so the run reproduces the app's configuration rather than the
# benchmark recipe's: 4096 ctx, fixed 2000 MiB cache, 4 I/O lanes, top-2.
tail -f -n +1 "$IN" 2>/dev/null |
    LD_LIBRARY_PATH="$D" "$BIN" -m "$M" --session --chatml --no-think -c 4096 \
        --moe-stream --cache-mb 2000 --io-threads 4 --overlap --n-expert-used 2 "$@" \
        > "$OUT" 2>&1 &

engine_pid() { pgrep -f bmoe-cli-rewarm | head -1; }

wait_for() { # wait_for <pattern> <timeout_s>
    _w=0
    while [ "$_w" -lt "$2" ]; do
        grep -q "$1" "$OUT" 2>/dev/null && return 0
        [ -n "$(engine_pid)" ] || return 1 # engine died; stop waiting on a corpse
        sleep 2
        _w=$((_w + 2))
    done
    return 1
}

wait_for BMOE_READY 300 || {
    echo "engine failed to load"
    tail -5 "$OUT"
    exit 1
}
PID=$(engine_pid)
echo "== $TAG: engine pid $PID, flags: $*"

echo '{"cmd":"generate","id":1,"prompt":"What is 17 times 23?","n_predict":24,"clear_kv":true}' >> "$IN"
wait_for 'BMOE_DONE {"id":1' 600 || {
    echo "turn 1 never finished"
    exit 1
}

# The gap. Sampling doubles as the wait, so the idle is measured, not assumed.
echo "-- idle ${IDLE}s from turn 1 done --" | tee "$MEM"
_t=0
while [ "$_t" -lt "$IDLE" ]; do
    printf 't=%s %s %s\n' "$_t" \
        "$(grep VmSwap /proc/"$PID"/status 2>/dev/null | tr -s ' ')" \
        "$(grep VmRSS /proc/"$PID"/status 2>/dev/null | tr -s ' ')" >> "$MEM"
    sleep 10
    _t=$((_t + 10))
done

echo '{"cmd":"generate","id":2,"prompt":"Name the capital of Australia.","n_predict":24,"clear_kv":true}' >> "$IN"
wait_for 'BMOE_DONE {"id":2' 900 || echo "turn 2 never finished"
echo '{"cmd":"close"}' >> "$IN"

# Reap both ends: the engine exits on close, then the tail that feeds it has no reader left. A
# stray tail (or a stray engine) would spin CPU into the next run and fake a regression there.
_w=0
while [ -n "$(engine_pid)" ] && [ "$_w" -lt 30 ]; do
    sleep 2
    _w=$((_w + 2))
done
kill "$(engine_pid)" 2>/dev/null
pkill -f "tail -f -n +1 $IN" 2>/dev/null
rm -f "$IN"

echo "== $TAG swap through the idle gap:"
cat "$MEM"
echo "== $TAG turns:"
grep -E 'BMOE_DONE|rewarm' "$OUT" | sed 's/,"text".*/}/'
