#!/usr/bin/env python3
"""Session request protocol, end to end through bmoe-cli (docs/telemetry.md).

The request reader in cli/main.cpp is a flat hand-rolled extractor, so what it does with the
history fields is pinned here against the real binary rather than assumed:
  * parallel history arrays seed the conversation, and BMOE_DONE ends with "history_dropped";
  * arrays of unequal length, an unknown role, and history without --chatml are non-fatal
    BMOE_ERRORs, and the session answers the next request;
  * fit_ctx drops whole exchanges instead of failing.

usage: session_protocol_test.py <bmoe-cli> <tiny-moe.gguf>
"""
import json
import subprocess
import sys

CLI, MODEL = sys.argv[1], sys.argv[2]
failures = 0


def check(name, ok, detail=""):
    global failures
    print(("[PASS] " if ok else "[FAIL] ") + name + (("  " + detail) if detail and not ok else ""))
    failures += 0 if ok else 1


def session(requests, chatml=True, ctx=256):
    """Send the requests, then close. Returns the BMOE_DONE / BMOE_ERROR lines as (kind, raw, json)."""
    args = [CLI, "-m", MODEL, "--session", "-c", str(ctx), "-t", "2"]
    if chatml:
        args.append("--chatml")
    stdin = "".join(json.dumps(r) + "\n" for r in requests) + json.dumps({"cmd": "close"}) + "\n"
    # The tiny fixture emits arbitrary bytes, so the output is decoded leniently.
    out = subprocess.run(args, input=stdin.encode(), capture_output=True, timeout=300).stdout.decode(errors="replace")
    lines = []
    for line in out.splitlines():
        for kind in ("BMOE_DONE", "BMOE_ERROR"):
            if line.startswith(kind + " "):
                lines.append((kind, line, json.loads(line[len(kind) + 1:])))
    return lines


def gen(i, prompt, **kw):
    return {"cmd": "generate", "id": i, "n_predict": 8, "prompt": prompt, **kw}


# Seeded history: parallel arrays, and the new key is the LAST one on BMOE_DONE (appended keys
# stay backward-compatible for readers that scan by position).
r = session([gen(1, "what did I say?", history_roles=["user", "assistant"], history_contents=["hi", "Hello!"])])
check("seeded history completes", len(r) == 1 and r[0][0] == "BMOE_DONE", str(r))
if r and r[0][0] == "BMOE_DONE":
    check("BMOE_DONE carries history_dropped=0", r[0][2].get("history_dropped") == 0)
    check("history_dropped is the last key", r[0][1].rstrip("}").rsplit(",", 1)[-1] == '"history_dropped":0')

# Bad requests are recoverable; the request after each one succeeds.
r = session([
    gen(1, "q", history_roles=["user"], history_contents=["a", "b"]),
    gen(2, "q", history_roles=["tool"], history_contents=["x"]),
    gen(3, "q"),
])
check("three answers", len(r) == 3, str(r))
if len(r) == 3:
    check("unequal arrays: non-fatal error", r[0][0] == "BMOE_ERROR" and r[0][2]["fatal"] is False
          and "differ in length" in r[0][2]["msg"], r[0][1])
    check("unknown role: non-fatal error", r[1][0] == "BMOE_ERROR" and r[1][2]["fatal"] is False
          and "unknown history role" in r[1][2]["msg"], r[1][1])
    check("session answers after the errors", r[2][0] == "BMOE_DONE", r[2][1])

# Roles without contents is the same mistake as arrays of unequal length.
r = session([gen(1, "q", history_roles=["user"]), gen(2, "q")])
check("roles without contents: non-fatal error, then an answer",
      len(r) == 2 and r[0][0] == "BMOE_ERROR" and r[0][2]["fatal"] is False and r[1][0] == "BMOE_DONE", str(r))

# An empty history is a request for "no history", not an absent field.
r = session([gen(1, "hi", history_roles=[], history_contents=[])])
check("empty history is accepted", len(r) == 1 and r[0][0] == "BMOE_DONE", str(r))

# History needs chat mode.
r = session([gen(1, "q", history_roles=["user"], history_contents=["hi"]), gen(2, "q")], chatml=False)
check("history without --chatml: non-fatal error, then an answer",
      len(r) == 2 and r[0][0] == "BMOE_ERROR" and r[0][2]["fatal"] is False
      and "chat mode" in r[0][2]["msg"] and r[1][0] == "BMOE_DONE", str(r))

# fit_ctx: the same oversized history fails without it and is trimmed with it.
pad = "x" * 40
roles = ["system"] + ["user", "assistant"] * 4
contents = ["Be brief."] + [pad] * 8
r = session([
    gen(1, "q", history_roles=roles, history_contents=contents),
    gen(2, "q", history_roles=roles, history_contents=contents, fit_ctx=True),
])
check("oversized history: refused without fit_ctx, trimmed with it",
      len(r) == 2 and r[0][0] == "BMOE_ERROR" and r[0][2]["fatal"] is False and r[1][0] == "BMOE_DONE"
      and r[1][2]["history_dropped"] > 0 and r[1][2]["history_dropped"] % 2 == 0, str(r))

sys.exit(1 if failures else 0)
