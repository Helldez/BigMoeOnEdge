#!/usr/bin/env python3
"""Session-mode request fuzzing for bmoe-cli.

Agent tooling and the bridge are the consumers of the stdin request protocol, so the
request parser is a boundary: malformed, type-bent and oversized lines must come back
as recoverable BMOE_ERROR (or be ignored), never crash the process, and never poison
the session. The load-bearing assertion is the last one: the valid generate AFTER the
garbage must be byte-identical to the one BEFORE it (greedy, seeded, clear_kv on — so
both turns are full independent renders of the same prompt).

Parser contract this pins (observed, deliberate — be careful tightening it):
  * unknown cmd / unparsable JSON / empty line  → silently ignored, no error line
  * missing id                                  → id 0, the request still runs
  * mistyped prompt (e.g. a number)             → json_get_string fails, prompt renders empty
  * n_predict past n_ctx                        → recoverable BMOE_ERROR, session intact
  * unknown keys                                → ignored
  * cancel                                      → applied IMMEDIATELY on the reader thread — it
    interrupts whatever generate is in flight or still queued, by design ("interrupt the
    in-flight generation"). A piped burst therefore cannot test an idle cancel: the reader
    races ahead of the main loop through the whole input. This test drives the CLI the way
    a real client does — write, wait for the matching response, then continue — and sends
    its cancel only after the last generate's BMOE_DONE has been observed.

The driver reads the CLI's stdout as RAW bytes and splits lines itself: a selectors
loop over a buffered (text-mode) stream deadlocks when several lines arrive in one
read — readline() serves one, the rest sit in Python's buffer, and the fd never
becomes readable again. (This is not hypothetical: the sanitizer build's timing hits
exactly that, the fast build merely dodges it.)

    python tests/session_fuzz.py <path-to-bmoe-cli> <tiny-moe.gguf>

Exits 0 and prints a one-line verdict, or exits non-zero naming what broke.
"""
import json
import selectors
import subprocess
import sys
import tempfile
import time


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: session_fuzz.py <bmoe-cli> <tiny-moe.gguf>")
    cli, model = sys.argv[1], sys.argv[2]
    prompt = "alpha beta gamma"

    def gen(i):
        return json.dumps({"cmd": "generate", "id": i, "prompt": prompt, "n_predict": 8, "think": False}).encode()

    garbage = [
        b"",                                                                      # empty line
        b"{{{ not json at all",                                                   # not JSON
        json.dumps({"cmd": "wat", "id": 9}).encode(),                             # unknown command
        json.dumps({"cmd": "generate", "prompt": "x"}).encode(),                  # missing id -> 0
        json.dumps({"cmd": "generate", "id": 2, "prompt": "x", "n_predict": 10 ** 9}).encode(),  # overflow
        json.dumps({"cmd": "generate", "id": 3, "prompt": 123, "think": False}).encode(),  # wrong value type
        json.dumps({"cmd": "generate", "id": 4, "prompt": prompt, "n_predict": 8,
                    "think": False, "extra_key": {"nested": [1, 2, 3]}}).encode(),  # unknown keys
    ]

    with tempfile.TemporaryFile() as errfile:
        proc = subprocess.Popen(
            [cli, "-m", model, "-c", "512", "--chatml", "--session", "--temp", "0", "--seed", "0"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errfile)
        sel = selectors.DefaultSelector()
        sel.register(proc.stdout, selectors.EVENT_READ)
        buf = b""
        seen = []  # complete protocol lines, decoded leniently (byte-fallback vocab emits raw bytes)

        def write(line):
            proc.stdin.write(line + b"\n")
            proc.stdin.flush()

        def wait_response(want_id, deadline_s=300):
            """Read stdout until a DONE/ERROR line for `want_id`; returns its parsed payload."""
            nonlocal buf
            deadline = time.time() + deadline_s
            while time.time() < deadline:
                for key, _ in sel.select(timeout=1.0):
                    chunk = key.fileobj.read1(1 << 16)
                    if not chunk:  # EOF
                        break
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        seen.append(line.decode("utf-8", "replace"))
                for line in seen:
                    for tag in ("BMOE_DONE ", "BMOE_ERROR "):
                        if line.startswith(tag):
                            try:
                                r = json.loads(line[len(tag):])
                            except json.JSONDecodeError:
                                continue
                            if r.get("id") == want_id:
                                return r
            sys.exit(f"fuzz: no response for id {want_id} within {deadline_s}s "
                     f"({len(seen)} lines, last: {seen[-1][:200] if seen else 'none'})")

        # Reference turn, then the hostile burst, then the post-garbage turn — the identity pair.
        write(gen(1))
        ref = wait_response(1)
        for g in garbage:
            write(g)
        write(gen(5))
        post = wait_response(5)
        write(json.dumps({"cmd": "cancel"}).encode())  # idle by construction: id 5's DONE was observed
        write(json.dumps({"cmd": "close"}).encode())
        proc.stdin.close()
        try:
            proc.wait(timeout=120)
        except subprocess.TimeoutExpired:
            proc.kill()
            sys.exit("fuzz: cli did not exit after close")
        sel.unregister(proc.stdout)

        if proc.returncode != 0:
            errfile.seek(0)
            sys.exit(f"fuzz: cli exited {proc.returncode}\n{errfile.read().decode('utf-8', 'replace')[-2000:]}")

    errors = []
    for line in seen:
        if line.startswith("BMOE_ERROR "):
            try:
                errors.append(json.loads(line[len("BMOE_ERROR "):]))
            except json.JSONDecodeError:
                pass

    if ref.get("cancelled") or post.get("cancelled"):
        sys.exit("fuzz: a reference turn was cancelled — the driver raced the reader, test is wrong")
    if ref["text"] != post["text"] or ref["tokens"] != post["tokens"]:
        sys.exit("fuzz: the valid turn after the garbage differs from the reference — session state was poisoned")
    if any(e.get("fatal") for e in errors):
        sys.exit(f"fuzz: recoverable garbage answered fatally: {errors[:3]}")

    print(f"session_fuzz: {len(errors)} recoverable errors over {len(garbage)} hostile lines, "
          f"session survived, post-garbage output byte-identical")


if __name__ == "__main__":
    main()
