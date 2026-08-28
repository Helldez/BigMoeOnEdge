#!/usr/bin/env bash
# One reproducible host benchmark, printed as a block you can paste into a benchmark-report issue
# (docs/community-benchmarks.md). Linux first, macOS best effort.
#
#   scripts/bench-report.sh MODEL.gguf [extra bmoe-cli flags...]
#
# What it does, in order:
#   1. builds bmoe-cli if build/cli/bmoe-cli is missing (BMOE_CLI overrides the binary);
#   2. records the machine: CPU, cores, RAM, kernel, the drive the model sits on and its
#      measured O_DIRECT read rate at 512 KiB requests, straight from the model file — the same
#      request size the expert stream issues, so the number is the ceiling this engine can see;
#   3. runs the fixed protocol from docs/benchmark-method.md: 256 greedy tokens, the same prompt
#      every table in the README uses, streaming with an auto-sized cache, 4 read lanes,
#      intra-layer overlap, the dense weights kept out of the page cache, and the same 512-wide
#      prefill batch the Android app pins (the reservation is memory the expert cache does not get);
#   4. parses the CSV trailer the engine writes and prints one markdown block.
#
# Env overrides: THREADS (default: min(8, online cores)), N_PREDICT (256), IO_THREADS (4),
# CACHE_MB (auto), UBATCH (512), BENCH_OUT (.bench-report), BMOE_CLI (build/cli/bmoe-cli).
# Anything after the model path is passed to bmoe-cli verbatim (e.g. --no-think for gpt-oss,
# --n-expert-used 6 for the turbo top-k rows).
set -euo pipefail

if [ $# -lt 1 ] || [ ! -f "$1" ]; then
    echo "usage: $0 MODEL.gguf [extra bmoe-cli flags...]" >&2
    exit 2
fi
MODEL="$1"; shift

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readf() { cat "$1" 2>/dev/null || true; }   # optional sysfs/procfs files: absent is not an error
OS="$(uname -s)"
NPROC="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
THREADS="${THREADS:-$(( NPROC < 8 ? NPROC : 8 ))}"
N_PREDICT="${N_PREDICT:-256}"
IO_THREADS="${IO_THREADS:-4}"
CACHE_MB="${CACHE_MB:-auto}"
UBATCH="${UBATCH:-512}"   # matches the app: a wider graph reserves buffers the expert cache wants
BENCH_OUT="${BENCH_OUT:-$ROOT/.bench-report}"
BMOE_CLI="${BMOE_CLI:-$ROOT/build/cli/bmoe-cli}"
PROMPT="Write a long detailed essay about the history of computing including its origins its key milestones the people involved and the future directions of the field"

# --- 1. binary ------------------------------------------------------------------------------
if [ ! -x "$BMOE_CLI" ]; then
    echo "bmoe-cli not found at $BMOE_CLI, building..." >&2
    "$ROOT/scripts/build-host.sh" >&2
fi
ENGINE_VERSION="$("$BMOE_CLI" --version 2>/dev/null | head -1 || echo unknown)"
GIT_REV="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"

# --- 2. machine -----------------------------------------------------------------------------
if [ "$OS" = "Darwin" ]; then
    CPU_MODEL="$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
    RAM_GIB="$(( $(sysctl -n hw.memsize) / 1024 / 1024 / 1024 ))"
    KERNEL="macOS $(sw_vers -productVersion 2>/dev/null || uname -r)"
    DRIVE_MODEL="unknown (macOS)"
    DD_FLAGS=""
else
    CPU_MODEL="$(readf /proc/cpuinfo | sed -n 's/^model name[[:space:]]*: //p' | head -1 | sed 's/[[:space:]]*$//')"
    # aarch64 boards usually have no "model name"; fall back to the device-tree model.
    [ -n "$CPU_MODEL" ] || CPU_MODEL="$(readf /sys/firmware/devicetree/base/model | tr -d '\0')"
    [ -n "$CPU_MODEL" ] || CPU_MODEL="unknown ($(uname -m))"
    MEM_KB="$(readf /proc/meminfo | sed -n 's/^MemTotal:[[:space:]]*\([0-9]*\).*/\1/p' | head -1)"
    RAM_GIB="$(( ${MEM_KB:-0} / 1024 / 1024 ))"
    KERNEL="$(uname -sr)"
    DEV="$(df --output=source "$MODEL" 2>/dev/null | tail -1)"
    BLOCK="$(basename "$(readlink -f "$DEV" 2>/dev/null || echo "$DEV")" | sed -E 's/p?[0-9]+$//')"
    DRIVE_MODEL="$(readf "/sys/block/$BLOCK/device/model" | tr -d '\n' | sed 's/[[:space:]]*$//')"
    [ -n "$DRIVE_MODEL" ] || DRIVE_MODEL="$BLOCK"
    DD_FLAGS="iflag=direct"
fi
MODEL_GIB="$(du -k "$MODEL" | awk '{printf "%.1f", $1/1024/1024}')"

# Read 1 GiB of the model itself at 512 KiB, bypassing the page cache, from a random-ish offset
# (one quarter in) so a freshly downloaded file's cached head does not flatter the number.
probe_read_rate() {
    local skip=$(( $(du -k "$MODEL" | cut -f1) / 4 / 512 ))
    local t0 t1
    t0="$(date +%s.%N)"
    dd if="$MODEL" of=/dev/null bs=512k count=2048 skip="$skip" $DD_FLAGS 2>/dev/null || return 1
    t1="$(date +%s.%N)"
    awk -v a="$t0" -v b="$t1" 'BEGIN { d=b-a; if (d>0) printf "%.0f", 1024/d; else print "n/a" }'
}
READ_MIBS="$(probe_read_rate || echo n/a)"

# --- 3. run ---------------------------------------------------------------------------------
mkdir -p "$BENCH_OUT"
TAG="$(basename "$MODEL" .gguf)"
CSV="$BENCH_OUT/$TAG.csv"
LOG="$BENCH_OUT/$TAG.log"
echo "running: $TAG, $THREADS threads, $IO_THREADS lanes, cache $CACHE_MB, ubatch $UBATCH, $N_PREDICT tokens" >&2
"$BMOE_CLI" -m "$MODEL" --chatml -n "$N_PREDICT" -t "$THREADS" --ubatch "$UBATCH" \
    --moe-stream --cache-mb "$CACHE_MB" --io-threads "$IO_THREADS" --overlap --dense-weights anon \
    --csv "$CSV" "$@" -p "$PROMPT" > "$LOG" 2>&1 || { echo "bmoe-cli failed, see $LOG" >&2; exit 1; }

# --- 4. report ------------------------------------------------------------------------------
# The trailer is whitespace-separated key=value; read by NAME, never by position.
SUMMARY="$(grep '^# summary' "$CSV" | tail -1)"
key() { echo "$SUMMARY" | tr ' ' '\n' | sed -n "s|^$1=||p" | head -1; }
ARCH="$(grep -o 'arch=[^ ]*' "$CSV" | head -1 | cut -d= -f2)"
TOPK="$(grep -o 'n_expert_used=[^ ]*' "$CSV" | head -1 | cut -d= -f2)"
TOKS="$(key tok/s)"; PREFILL="$(key prefill_tps)"; STALL="$(key stall_s/tok)"
COMPUTE="$(key compute_s/tok)"; HIT="$(key cache_hit_pct)"; READ_MIB="$(key read_MiB)"
TOKENS="$(key tokens)"; BUDGET="$(key cache_budget_MiB)"; MAJFLT="$(key majflt/tok)"
LOAD_S="$(key load_s)"; DROPPED="$(key experts_dropped)"
MIB_PER_TOK="$(awk -v r="$READ_MIB" -v n="$TOKENS" 'BEGIN { if (n>0) printf "%.0f", r/n; else print "n/a" }')"

cat <<EOF

<!-- paste everything below into the benchmark-report issue -->
**Machine**

| CPU | Cores / threads used | RAM | Storage | Storage read (512 KiB, O_DIRECT) | OS |
|---|---:|---:|---|---:|---|
| $CPU_MODEL | $NPROC / $THREADS | $RAM_GIB GiB | $DRIVE_MODEL | $READ_MIBS MiB/s | $KERNEL |

**Run** — engine $ENGINE_VERSION ($GIT_REV), model \`$(basename "$MODEL")\` ($MODEL_GIB GiB, arch \`$ARCH\`), $N_PREDICT tokens

| Model | k | Cache | Lanes | ubatch | Decode tok/s | Prefill tok/s | Stall s/tok | Compute s/tok | Flash/token | Cache hit | majflt/tok | Load s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| $TAG | $TOPK | $BUDGET MiB | $IO_THREADS | $UBATCH | $TOKS | $PREFILL | $STALL | $COMPUTE | $MIB_PER_TOK MiB | $HIT% | $MAJFLT | $LOAD_S |

Extra flags: \`${*:-none}\`. Experts dropped: ${DROPPED:-0}. Raw CSV: \`$CSV\` (attach it to the issue).
EOF
