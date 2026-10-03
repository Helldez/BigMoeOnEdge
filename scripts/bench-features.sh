#!/usr/bin/env bash
# Before/after benchmark for the session-residency features, driven over the
# OpenAI-compatible bridge (scripts/bmoe-serve.py) — the path agent clients take.
#
#   scripts/bench-features.sh MODEL.gguf
#
# Scenarios, each on an isolated engine on PORT (default 8019; a server on another
# port, e.g. the daily driver on 8017, is never touched):
#   1. warmup off/on   — startup warmup replay (bridge --no-warmup vs default). The
#                        first chain run seeds the warmup file with this exact
#                        conversation, so the "on" run replays the same prefix the
#                        "off" run paid for.
#   2. echo off/on     — thinking hybrid only (skipped with a note otherwise):
#                        3-turn chain sending preserve_reasoning and echoing the
#                        model's reasoning back as <think>...</think>answer, vs a
#                        plain chain. Off = full clear every turn (the safe default).
#   3. --rs-seq off/on — snapshot rollback (engine CLI flag). Restore is NOT
#                        bit-exact upstream, so every answer is VERIFIED against
#                        ground truth (42/52/62); a faster run with wrong answers
#                        reports FAIL, never a win.
#
# Each scenario prints one markdown table row: T1 prefill | T2 prefill/reused |
# T3 prefill/reused | T1 tok/s | verdict, and appends a CSV row under BENCH_OUT
# (default .bench-features). Env overrides: PORT, N_PREDICT (default 64), BENCH_OUT.
# NOTE: thinking models often need N_PREDICT >= 192 to finish reasoning AND emit the
# answer — with a smaller budget the reasoning alone consumes it and verification fails.
#
# Safety: the global warmup cache (~/.cache/bmoe-serve/warmup.json) is backed up
# before the first run and restored on exit (including on Ctrl-C); servers and
# their engines are killed on exit and between scenarios.
set -uo pipefail

if [ $# -lt 1 ] || [ ! -f "$1" ]; then
    echo "usage: $0 MODEL.gguf" >&2
    exit 2
fi
MODEL="$(readlink -f "$1")"
PORT="${PORT:-8019}"
N_PREDICT="${N_PREDICT:-64}"
OUT="${BENCH_OUT:-.bench-features}"
BRIDGE="$(dirname "$0")/bmoe-serve.py"
WARMUP_JSON="$HOME/.cache/bmoe-serve/warmup.json"
SRV=""
CHAIN_DIR=""
mkdir -p "$OUT"
CSV="$OUT/results.csv"
[ -f "$CSV" ] || echo "scenario,verdict,t1_prefill_s,t1_tok_s,t2_prefill_s,t2_reused,t3_prefill_s,t3_reused" > "$CSV"

# ---- server lifecycle -------------------------------------------------------
# EXTRA_ENGINE_ARGS overrides the default engine flags (--chatml: template-driven
# message rendering — required for the messages path on every model tested).
start_server() { # $1 = extra bridge args, $2 = extra engine args
    local bargs=() eargs=()
    [ -n "${1:-}" ] && bargs+=($1)
    eargs+=(--engine-args "${EXTRA_ENGINE_ARGS:---chatml} $2")
    setsid nohup python3 -u "$BRIDGE" -m "$MODEL" --port "$PORT" \
        "${bargs[@]}" "${eargs[@]}" > "$OUT/server-$PORT.log" 2>&1 &
    SRV=$!
    for _ in $(seq 1 120); do
        grep -aq "OpenAI-compatible" "$OUT/server-$PORT.log" 2>/dev/null && return 0
        kill -0 "$SRV" 2>/dev/null || { echo "server died:"; tail -5 "$OUT/server-$PORT.log"; return 1; }
        sleep 1
    done
    echo "server not ready in 120s"; return 1
}
stop_server() {
    [ -n "$SRV" ] && kill -- -"$SRV" 2>/dev/null
    pkill -f "bmoe-serve.py.*--port $PORT" 2>/dev/null
    sleep 1; SRV=""
}

# ---- one request ------------------------------------------------------------
ask() { # $1=messages-file $2=reply-file $3=perf-file $4=preserve_reasoning(0/1)
    python3 - "$PORT" "$1" "$2" "$3" "$4" "$N_PREDICT" <<'PY'
import sys, json, urllib.request
port, mf, rf, pf, preserve, n = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5] == "1", int(sys.argv[6])
body = {"model": "bmoe-local", "messages": json.load(open(mf)), "max_tokens": n}
if preserve:
    body["preserve_reasoning"] = True
req = urllib.request.Request(f"http://127.0.0.1:{port}/v1/chat/completions",
    data=json.dumps(body).encode(), headers={"Content-Type": "application/json"})
try:
    r = json.load(urllib.request.urlopen(req, timeout=3600))
except urllib.error.HTTPError as e:
    sys.exit(f"HTTP {e.code} from engine: {e.read().decode('utf-8', 'replace')[:300]}")
ch = r["choices"][0]["message"]
json.dump({"content": ch.get("content") or "", "reasoning": ch.get("reasoning_content") or ""}, open(rf, "w"))
json.dump(r.get("bmoe", {}), open(pf, "w"))
PY
}

verify() { # $1=reply-file $2=expected content value — content-only, word-boundary match
    # ("42" must not match "525"; a \\boxed{42} inside trailing reasoning must not fail it)
    python3 - "$1" "$2" <<'PY'
import sys, re, json
try:
    c = json.load(open(sys.argv[1])).get("content", "")
except Exception:
    sys.exit(1)
sys.exit(0 if re.search(rf"(?<![0-9.]){re.escape(sys.argv[2])}(?![0-9.])", c) else 1)
PY
}

# ---- one 3-turn chain, built incrementally (each turn needs the prior reply) -
# Ground truth: 6*7=42, 42+10=52, 52+10=62. Each question restates its inputs — a
# coreference chain ("add 10 again") tests the model's parsing, not the cache. Returns 0
# iff every answer verified.
run_chain() { # $1=label $2=echo(0/1)
    local d="$OUT/chain-$1"; rm -rf "$d"; mkdir -p "$d"
    local u1='What is 6 times 7? Answer with just the number.'
    local u2='What is 42 plus 10? Answer with just the number.'
    local u3='What is 52 plus 10? Answer with just the number.'
    local preserve=0; [ "$2" = 1 ] && preserve=1

    python3 -c "import json; json.dump([{'role':'user','content':'$u1'}], open('$d/t1.json','w'))"
    ask "$d/t1.json" "$d/r1.json" "$d/m1.json" 0 || return 1

    # Turn 2 messages: history with the assistant reply echoed (reasoning only if echo on).
    python3 - "$d" "$2" <<'PY'
import sys, json
d, echo = sys.argv[1], sys.argv[2] == "1"
a1 = json.load(open(f"{d}/r1.json"))
asst = f"<think>{a1['reasoning']}</think>{a1['content']}" if echo and a1["reasoning"] else a1["content"]
json.dump([{"role": "user", "content": "What is 6 times 7? Answer with just the number."},
           {"role": "assistant", "content": asst},
           {"role": "user", "content": "What is 42 plus 10? Answer with just the number."}],
          open(f"{d}/t2.json", "w"))
PY
    ask "$d/t2.json" "$d/r2.json" "$d/m2.json" "$preserve" || return 1

    python3 - "$d" "$2" <<'PY'
import sys, json
d, echo = sys.argv[1], sys.argv[2] == "1"
def asst_of(f):
    a = json.load(open(f"{d}/{f}"))
    return f"<think>{a['reasoning']}</think>{a['content']}" if echo and a["reasoning"] else a["content"]
json.dump([{"role": "user", "content": "What is 6 times 7? Answer with just the number."},
           {"role": "assistant", "content": asst_of("r1.json")},
           {"role": "user", "content": "What is 42 plus 10? Answer with just the number."},
           {"role": "assistant", "content": asst_of("r2.json")},
           {"role": "user", "content": "What is 52 plus 10? Answer with just the number."}],
          open(f"{d}/t3.json", "w"))
PY
    ask "$d/t3.json" "$d/r3.json" "$d/m3.json" "$preserve" || return 1

    if verify "$d/r1.json" 42 && verify "$d/r2.json" 52 && verify "$d/r3.json" 62; then
        CHAIN_DIR="$d"; return 0
    fi
    CHAIN_DIR="$d"
    echo "  !! chain $1 FAILED answer verification"
    return 1
}

# ---- one table+csv row ------------------------------------------------------
row() { # $1=scenario $2=chaindir $3=verdict
    CHAIN_DIR="$2"
    python3 - "$1" "$2" "$3" "$CSV" <<'PY'
import sys, json, csv, os
label, d, verdict, path = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
m = {}
for t in (1, 2, 3):
    try: m[t] = json.load(open(os.path.join(d, f"m{t}.json")))
    except Exception: m[t] = {}
g = lambda t, k: m[t].get(k, "-")
print(f"| {label} | {g(1,'prefill_s')} | {g(2,'prefill_s')} | {g(2,'n_reused')} | "
      f"{g(3,'prefill_s')} | {g(3,'n_reused')} | {g(1,'tok_s')} | {verdict} |")
with open(path, "a", newline="") as fh:
    csv.writer(fh).writerow([label, verdict, g(1, "prefill_s"), g(1, "tok_s"),
                             g(2, "prefill_s"), g(2, "n_reused"),
                             g(3, "prefill_s"), g(3, "n_reused")])
PY
}

run_scenario() { # $1=scenario-label $2=echo $3=bridge-args $4=engine-args
    echo "== $1 =="
    start_server "$3" "$4" || exit 1
    run_chain "$1" "$2"; local ok=$?
    row "$1" "$CHAIN_DIR" "$([ $ok = 0 ] && echo ok || echo FAIL)"
    stop_server
}

# Back up the user's warmup cache; restore on any exit path. INT/TERM exit first so the
# EXIT trap runs exactly once — a trap that returns without exiting resumes the script.
WARMUP_BACKUP=""
[ -f "$WARMUP_JSON" ] && { WARMUP_BACKUP="$(mktemp)"; cp "$WARMUP_JSON" "$WARMUP_BACKUP"; }
restore_warmup() {
    if [ -n "$WARMUP_BACKUP" ]; then cp "$WARMUP_BACKUP" "$WARMUP_JSON"; else rm -f "$WARMUP_JSON"; fi
    stop_server
}
trap 'exit 130' INT TERM
trap restore_warmup EXIT

printf '| scenario | T1 prefill s | T2 prefill s | T2 reused | T3 prefill s | T3 reused | T1 tok/s | verdict |\n|---|---|---|---|---|---|---|---|\n'

# ---- scenario 1: warmup (first chain seeds the warmup file for the "on" run) -
run_scenario "warmup-off" 0 "--no-warmup" ""
run_scenario "warmup-on"  0 "" ""

# ---- scenario 2: reasoning echo (behavioral probe: did any turn so far emit
# reasoning_content? A template merely mentioning think is not evidence) -------
if python3 -c "
import json,sys
sys.exit(1 if not json.load(open('$OUT/chain-warmup-off/r1.json')).get('reasoning') else 0)" 2>/dev/null; then
    run_scenario "echo-off" 0 "" ""
    run_scenario "echo-on"  1 "" ""
else
    echo "| echo | - | - | - | - | - | - | skipped (model produced no reasoning_content) |"
fi

# ---- scenario 3: snapshot rollback (answers verified; wrong = FAIL) ---------
run_scenario "rs-seq-off" 0 "" ""
run_scenario "rs-seq-on"  0 "" "--rs-seq 64"

echo "done. csv: $CSV  chains: $OUT/chain-*  server log: $OUT/server-$PORT.log"
