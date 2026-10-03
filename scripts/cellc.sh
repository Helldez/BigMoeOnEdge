#!/usr/bin/env bash
# Cell (c) for one model, ONE scenario per invocation (processes do not survive the
# caller here, so each call carries its own server lifecycle inside one window).
#
#   scripts/cellc.sh MODEL.gguf OUTDIR c1   # warmup OFF  (also seeds the warmup file)
#   scripts/cellc.sh MODEL.gguf OUTDIR c2   # warmup ON   (replays what c1 seeded)
#   scripts/cellc.sh MODEL.gguf OUTDIR c3   # warmup ON + --auto-echo (echoed chain)
#   scripts/cellc.sh MODEL.gguf OUTDIR c4   # divergence turn (regenerated answer), rs-seq OFF
#   scripts/cellc.sh MODEL.gguf OUTDIR c5   # divergence turn (regenerated answer), rs-seq ON
#
# c4/c5 shape: T1(42) A1 T2'(62) where T2' = "52 plus 10 again? Answer with just the
# number." — a NEW question that reuses the resident history but is not what a plain
# append chain would send after A1. Reuse wants a rewind to just after A1 (hybrids:
# partial seq_rm). c4 runs the engine as shipped (rs-seq off); c5 passes --rs-seq 64 to
# the engine, which requires a llama build carrying the snapshot-rollback fixes
# (bench/host-rs) — on the unfixed pin rs-seq stays a full-clear fallback.
#
# OOM law (2026-09-19, three kernel kills): snapshot planes cost ~60 MiB each on a 35B
# qwen35moe — --rs-seq 160 reserved 10.1 GiB and the OOM-killer shot bmoe-cli mid-load.
# Keep the budget 64: it only needs to cover one turn's rewind depth (generated length,
# ~39 tok here), never MAXTOK. Depth > budget is an instant honest refusal -> full clear,
# so an oversized budget buys nothing and OOMs the host.
#
# Prints one JSON line {"verdict":..., "turns":{...}} with per-turn n_prompt / n_reused /
# prefill_s / tok_s parsed from the bridge TELEMETRY log. Answers verified (42 then 62);
# a fast-but-wrong run reports FAIL, never a win. The user's global warmup cache is
# backed up (first invocation only) and restored when a c-series run finishes or on error.
set -u
M="$1"; OUT="$2"; SC="$3"
ENG="/home/chris/projects/BigMoeOnEdge/build-bench/cli/bmoe-cli"
BRIDGE="/home/chris/projects/BigMoeOnEdge/scripts/bmoe-serve.py"
PORT=8019
STREAM="--moe-stream --cache-mb auto --io-threads 4 --overlap --dense-weights anon --ctx-size 8192"
WJ="$HOME/.cache/bmoe-serve/warmup.json"
mkdir -p "$OUT"
SRV=""
cleanup() {
    [ -n "$SRV" ] && kill -- -"$SRV" 2>/dev/null
    if [ "$SC" = "c3" ] && [ -f "$OUT/warmup.json.bak" ]; then
        cp "$OUT/warmup.json.bak" "$WJ"
    fi
}
trap cleanup EXIT

# first invocation backs up the user's warmup cache (cp -n: later calls keep the original)
cp -n "$WJ" "$OUT/warmup.json.bak" 2>/dev/null

BARGS=""
[ "$SC" = "c1" ] && BARGS="--no-warmup"
[ "$SC" = "c3" ] && BARGS="--auto-echo"
EARGS="$STREAM --chatml"
[ "$SC" = "c5" ] && EARGS="$STREAM --rs-seq 64 --chatml"  # 64 planes ~3.9 GiB on a 35B; depth > budget = honest full clear

echo "starting server ($SC)..." >&2
setsid nohup python3 -u "$BRIDGE" -m "$M" --port "$PORT" $BARGS \
    --engine "$ENG" --engine-args "$EARGS" \
    > "$OUT/server-$SC.log" 2>&1 &
SRV=$!
for _ in $(seq 1 420); do
    grep -aq "OpenAI-compatible" "$OUT/server-$SC.log" 2>/dev/null && break
    sleep 1
done
# 420 s, not 150: a 35B streamed load alone measured ~178 s on the 4-core host, plus the
# warmup replay c2/c3 run before announcing. A short poll false-FAILs a healthy server.
grep -aq "OpenAI-compatible" "$OUT/server-$SC.log" || { echo "SERVER-FAILED"; tail -5 "$OUT/server-$SC.log"; exit 1; }

ask() { # $1=messages.json $2=reply.json $3=preserve(0|1)
    python3 - "$1" "$2" "$PORT" "$3" "${MAXTOK:-64}" <<'PY'
import sys, json, time, urllib.request
mp, rp, port, preserve, maxtok = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4] == "1", int(sys.argv[5])
msgs = json.load(open(mp))
body = {"messages": msgs, "max_tokens": maxtok, "temperature": 0}
if preserve:
    body["preserve_reasoning"] = True
t0 = time.time()
r = urllib.request.urlopen(urllib.request.Request(
    f"http://127.0.0.1:{port}/v1/chat/completions", data=json.dumps(body).encode(),
    headers={"Content-Type": "application/json"}), timeout=400)
msg = json.loads(r.read())["choices"][0]["message"]
json.dump({"content": msg.get("content", ""),
           "reasoning": msg.get("reasoning_content") or "",
           "client_s": round(time.time() - t0, 2)}, open(rp, "w"))
PY
}

echoq() { python3 -c "
import json,sys
json.dump([{'role':'user','content':'What is 6 times 7? Answer with just the number.'}], open(sys.argv[1],'w'))" "$1"; }

# rebuild the next chain file, echoing reasoning back per the scenario (c3 only)
extend() { # $1=dir $2=echo(0|1) $3=out.json
    python3 - "$1" "$2" "$3" <<'PY'
import sys, json
d, echo, out = sys.argv[1], sys.argv[2] == "1", sys.argv[3]
def a(f):
    x = json.load(open(f"{d}/{f}"))
    return f"<think>{x['reasoning']}</think>{x['content']}" if echo and x["reasoning"] else x["content"]
msgs = [{"role": "user", "content": "What is 6 times 7? Answer with just the number."},
        {"role": "assistant", "content": a("r1.json")},
        {"role": "user", "content": "What is 42 plus 10? Answer with just the number."}]
json.dump(msgs, open(out, "w"))
PY
}

verify() { # $1=reply.json $2=expected — word-boundary match, not substring (52 ⊄ 525)
    python3 -c "
import json, re, sys
c = json.load(open(sys.argv[1]))['content']
sys.exit(0 if re.search(r'\\b' + sys.argv[2] + r'\\b', c) else 1)" "$1" "$2"
}

PRESERVE="$([ "$SC" = "c3" ] && echo 1 || echo 0)"
D="$OUT/$SC"; mkdir -p "$D"
echoq "$D/t1.json"
ok=1
ask "$D/t1.json" "$D/r1.json" "$PRESERVE" || ok=0

if [ "$SC" = "c4" ] || [ "$SC" = "c5" ]; then
    # Divergence: the second request resends T1 + the engine's own reply (plain, as a
    # plain OpenAI client would) and asks a NEW question. The engine must rewind the
    # hybrid to just after T1 (its stored span between T1 and the reply's end is not
    # in the request) - full clear without --rs-seq (c4), bounded rewind with it (c5).
    python3 - "$D" <<'PY'
import sys, json
d = sys.argv[1]
a1 = json.load(open(f"{d}/r1.json"))["content"]
json.dump([{"role": "user", "content": "What is 6 times 7? Answer with just the number."},
           {"role": "assistant", "content": a1},
           {"role": "user", "content": "What is 52 plus 10? Answer with just the number."}],
          open(f"{d}/t2.json", "w"))
PY
    [ $ok = 1 ] && ask "$D/t2.json" "$D/r2.json" 0 || ok=0
else
    extend "$D" 0 "$D/t2.json" 2>/dev/null || ok=0
    [ $ok = 1 ] && ask "$D/t2.json" "$D/r2.json" "$PRESERVE" || ok=0
    if [ $ok = 1 ]; then
        # t3 needs r2 present: rebuild with both assistant turns
        python3 - "$D" "$([ "$SC" = "c3" ] && echo 1 || echo 0)" <<'PY'
import sys, json
d, echo = sys.argv[1], sys.argv[2] == "1"
def a(f, e=False):
    x = json.load(open(f"{d}/{f}"))
    return f"<think>{x['reasoning']}</think>{x['content']}" if e and x["reasoning"] else x["content"]
msgs = [{"role": "user", "content": "What is 6 times 7? Answer with just the number."},
        {"role": "assistant", "content": a("r1.json", echo)},
        {"role": "user", "content": "What is 42 plus 10? Answer with just the number."},
        {"role": "assistant", "content": a("r2.json", echo)},
        {"role": "user", "content": "What is 52 plus 10? Answer with just the number."}]
json.dump(msgs, open(f"{d}/t3.json", "w"))
PY
        ask "$D/t3.json" "$D/r3.json" "$PRESERVE" || ok=0
    fi
fi

V=FAIL
if [ "$SC" = "c4" ] || [ "$SC" = "c5" ]; then
    if [ $ok = 1 ] && verify "$D/r1.json" 42 && verify "$D/r2.json" 62; then
        V=ok
    fi
else
    if [ $ok = 1 ] && verify "$D/r1.json" 42 && verify "$D/r2.json" 52 && verify "$D/r3.json" 62; then
        V=ok
    fi
fi

python3 - "$OUT/server-$SC.log" "$D" "$SC" "$V" <<'PY'
import sys, json
log, d, sc, v = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
tel = []
for l in open(log, errors="replace"):
    if "TELEMETRY" in l and "n_prompt" in l:
        tel.append(json.loads(l.split("TELEMETRY ", 1)[1]))
tel = tel[-3:]
out = {"scenario": sc, "verdict": v, "turns": {}}
for i, t in enumerate(tel, 1):
    out["turns"][str(i)] = {k: t.get(k) for k in ("n_prompt", "n_reused", "prefill_s", "tok_s")}
print(json.dumps(out))
PY
