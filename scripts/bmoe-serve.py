#!/usr/bin/env python3
# OpenAI-compatible HTTP bridge over `bmoe-cli --session` (JSON-over-stdin protocol).
#
# The engine has no HTTP server; it serves prompt requests as newline-delimited JSON on stdin
# and BMOE_* events on stdout (docs/telemetry.md). This bridge spawns it once, translates
# OpenAI /v1/chat/completions (streaming and not) and /v1/models onto that protocol, and keeps
# the model loaded between requests so the expert cache stays warm.
#
# Requires: python3 (stdlib only). One entry point, no launcher: it resolves the engine
# (--engine, else $BMOE_ENGINE, else the host build of a repo checkout, else a bundled
# bmoe-cli beside this script) and the model (--model, else $BMOE_MODEL, else the tiny
# test model a host build produced).

import argparse
import json
import os
import shlex
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ENGINE_READY = "BMOE_READY"
ENGINE_BEGIN = "BMOE_BEGIN"
ENGINE_PROGRESS = "BMOE_PROGRESS"
ENGINE_DONE = "BMOE_DONE"
ENGINE_ERROR = "BMOE_ERROR"


class EngineError(Exception):
    """Recoverable engine failure (session still usable)."""


class EngineFatal(Exception):
    """Fatal engine failure; the process must be restarted."""


class Engine:
    """One `bmoe-cli --session` process. Generations are serialized on a lock: the engine
    queues stdin commands but answers them one at a time, so the bridge does the same."""

    def __init__(self, engine, model, engine_args, ready_timeout):
        self.ready_info = {}
        self._lock = threading.Lock()
        self._next_id = 1
        cmd = [engine, "-m", model, "--session"] + engine_args
        print(f"[bmoe-serve] starting engine: {' '.join(cmd)}", flush=True)
        # Binary pipes: token pieces can carry arbitrary byte vocabulary, so the reader
        # decodes per line with errors='replace' instead of trusting a text-mode codec.
        self.proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0,
        )
        deadline = time.time() + ready_timeout
        while time.time() < deadline:
            line = self._readline()
            if line is None:
                sys.exit(f"[bmoe-serve] engine exited during load (rc={self.proc.poll()})")
            if line.startswith(ENGINE_READY):
                try:
                    self.ready_info = json.loads(line.split(" ", 1)[1])
                except (json.JSONDecodeError, IndexError):
                    self.ready_info = {}
                print(f"[bmoe-serve] engine ready: {self.ready_info}", flush=True)
                return
            if line.startswith(ENGINE_ERROR):
                sys.exit(f"[bmoe-serve] engine failed to open the model: {line.strip()}")
        self.proc.kill()
        sys.exit(f"[bmoe-serve] no {ENGINE_READY} within {ready_timeout:g}s")

    def _readline(self):
        """One protocol line from the engine, or None at EOF."""
        raw = self.proc.stdout.readline()
        if not raw:
            return None
        return raw.decode("utf-8", errors="replace").rstrip("\n")

    def _write_line(self, s):
        self.proc.stdin.write((s + "\n").encode())
        self.proc.stdin.flush()

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def cancel(self):
        try:
            self._write_line('{"cmd":"cancel"}')
        except (BrokenPipeError, OSError, AttributeError):
            pass

    def request(self, prompt, n_predict, think=True):
        """Yield ('progress', dict) events; the caller consumes until the generator ends.
        Raises EngineError (recoverable) or EngineFatal (restart needed)."""
        rid = self._next_id
        self._next_id += 1
        payload = {
            "cmd": "generate",
            "id": rid,
            "prompt": prompt,
            "n_predict": n_predict,
            "think": think,
            "clear_kv": True,
        }
        with self._lock:
            if not self.alive():
                raise EngineFatal("engine process is not running")
            try:
                self._write_line(json.dumps(payload))
            except (BrokenPipeError, OSError):
                raise EngineFatal("engine stdin is closed")
            while True:
                line = self._readline()
                if line is None:
                    raise EngineFatal("engine closed stdout mid-generation")
                if not line.startswith("BMOE_"):
                    continue
                event, _, rest = line.partition(" ")
                if event == ENGINE_PROGRESS:
                    try:
                        p = json.loads(rest)
                    except json.JSONDecodeError:
                        continue
                    yield p
                elif event == ENGINE_DONE:
                    try:
                        self.last_done = json.loads(rest)
                    except json.JSONDecodeError:
                        self.last_done = {}
                    return
                elif event == ENGINE_ERROR:
                    try:
                        e = json.loads(rest)
                    except json.JSONDecodeError:
                        e = {"msg": rest}
                    if e.get("fatal"):
                        raise EngineFatal(e.get("msg", "fatal engine error"))
                    raise EngineError(e.get("msg", "engine error"))

    def stop(self):
        if self.proc is None:
            return
        try:
            if self.alive():
                try:
                    self._write_line('{"cmd":"close"}')
                except (BrokenPipeError, OSError):
                    pass
                try:
                    self.proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
        finally:
            self.proc = None


engine = None
model_id = "bmoe-local"


def flatten_messages(messages):
    """OpenAI chat messages -> the single prompt string the engine renders through the model's
    own chat template. The session pushes it as one user turn; system messages are kept inline
    (the engine renders the FULL conversation from its own history, so nothing else is needed)."""
    parts = []
    for m in messages:
        role = m.get("role", "user")
        content = m.get("content", "")
        if isinstance(content, list):
            content = "".join(p.get("text", "") for p in content if isinstance(p, dict))
        if role == "system":
            parts.append(f"[system]\n{content}")
        else:
            parts.append(content)
    return "\n\n".join(parts)


def sse(data):
    return f"data: {json.dumps(data)}\n\n".encode()


def tail_past(prev, cur):
    """The part of cur past its common prefix with prev. For plain appends this is
    cur[len(prev):]; when the engine's reasoning parse retroactively reclassifies text
    (the protocol's reset:1 case), it degrades to resending the corrected tail."""
    n = min(len(prev), len(cur))
    i = 0
    while i < n and prev[i] == cur[i]:
        i += 1
    return cur[i:]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        print(f"[bmoe-serve] {fmt % args}", flush=True)

    def send_json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.rstrip("/")
        if path == "/v1/models":
            self.send_json(200, {
                "object": "list",
                "data": [{
                    "id": model_id,
                    "object": "model",
                    "created": int(time.time()),
                    "owned_by": "bmoe",
                }],
            })
        elif path == "/health":
            self.send_json(200, {"ok": engine.alive()})
        else:
            self.send_json(404, {"error": {"message": "not found"}})

    def do_POST(self):
        if self.path.rstrip("/") != "/v1/chat/completions":
            self.send_json(404, {"error": {"message": "not found"}})
            return
        try:
            length = int(self.headers.get("Content-Length", 0))
            req = json.loads(self.rfile.read(length))
        except (json.JSONDecodeError, ValueError):
            self.send_json(400, {"error": {"message": "invalid JSON body"}})
            return
        messages = req.get("messages", [])
        if not messages:
            self.send_json(400, {"error": {"message": "messages must not be empty"}})
            return

        # Generous default: thinking models spend completion tokens on reasoning before the
        # answer, and agent clients that never send max_tokens would otherwise get truncated.
        # But the budget is clamped to the ceiling: a huge client max_tokens would otherwise
        # sit in the n_ctx window (or, after the overflow retry, generate for tens of minutes).
        # OpenAI clients treat max_tokens as a ceiling, so clamping is behavior-preserving.
        n_predict = min(int(req.get("max_tokens") or req.get("max_completion_tokens") or max_tokens_limit), max_tokens_limit)
        stream = bool(req.get("stream", False))
        prompt = flatten_messages(messages)
        created = int(time.time())

        try:
            if stream:
                self.handle_stream(req, prompt, n_predict, created)
            else:
                self.handle_plain(req, prompt, n_predict, created)
        except EngineFatal as e:
            if not self.headers.get("Content-Length") or stream:
                pass  # headers already sent; report as an SSE error below if possible
            self.send_json(502, {"error": {"message": str(e)}})

    def run_generation(self, prompt, n_predict):
        """Iterate the engine, mapping deltas to (reasoning, text); cancels on client loss."""

        def _iter(np):
            reasoning, text = [], []
            for p in engine.request(prompt, np):
                r, t = p.get("delta_reasoning", ""), p.get("delta_text", "")
                if r:
                    reasoning.append(r)
                if t:
                    text.append(t)
                yield "".join(reasoning), "".join(text), p

        try:
            yield from _iter(n_predict)
        except GeneratorExit:
            engine.cancel()
            raise
        except EngineError as e:
            # Agent clients routinely request more output than fits beside their prompt.
            # A shorter answer beats a failed request: halve the completion budget once
            # (floor 256) and retry. If the prompt alone overflows, the retry fails too
            # and the error surfaces — that genuinely needs a shorter conversation.
            if "exceeds the session n_ctx" not in str(e) or n_predict <= 256:
                raise
            fallback = max(256, n_predict // 2)
            print(f"[bmoe-serve] {e}; retrying with n_predict={fallback}", flush=True)
            yield from _iter(fallback)

    def handle_stream(self, req, prompt, n_predict, created):
        cid = f"chatcmpl-{int(time.time() * 1000)}"
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        # No Content-Length on a stream: close the connection when done (HTTP/1.1 rule).
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

        def chunk(delta, finish=None):
            return {
                "id": cid, "object": "chat.completion.chunk", "created": created,
                "model": model_id,
                "choices": [{"index": 0, "delta": delta, "finish_reason": finish}],
            }

        try:
            self.wfile.write(sse(chunk({"role": "assistant"})))
            self.wfile.flush()
            # The engine yields cumulative text; the wire protocol wants DELTAS, so send
            # only the tail past what this stream has already sent.
            sent_r = sent_t = ""
            for reasoning, text, _ in self.run_generation(prompt, n_predict):
                delta = {}
                if len(reasoning) > len(sent_r):
                    delta["reasoning_content"] = tail_past(sent_r, reasoning)
                    sent_r = reasoning
                if len(text) > len(sent_t):
                    delta["content"] = tail_past(sent_t, text)
                    sent_t = text
                if delta:
                    self.wfile.write(sse(chunk(delta)))
                    self.wfile.flush()
            done = getattr(engine, "last_done", {})
            finish = "length" if done.get("tokens", 0) >= n_predict else "stop"
            self.wfile.write(sse(chunk({}, finish)))
            # No vendor-specific events here: strict client SDKs (the AI SDK behind opencode)
            # validate every SSE payload against the OpenAI chunk schema and reject anything
            # without `choices` — the perf block rides only non-streaming responses.
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
        except (EngineError, EngineFatal, BrokenPipeError) as e:
            if isinstance(e, BrokenPipeError):
                return  # client is gone; nothing to report
            try:
                self.wfile.write(sse({"error": {"message": str(e)}}))
                self.wfile.write(b"data: [DONE]\n\n")
                self.wfile.flush()
            except OSError:
                pass

    def handle_plain(self, req, prompt, n_predict, created):
        try:
            reasoning, text = "", ""
            for reasoning, text, _ in self.run_generation(prompt, n_predict):
                pass
        except (EngineError, EngineFatal) as e:
            self.send_json(502, {"error": {"message": str(e)}})
            return
        done = getattr(engine, "last_done", {})
        finish = "length" if done.get("tokens", 0) >= n_predict else "stop"
        self.send_json(200, {
            "id": f"chatcmpl-{int(time.time() * 1000)}",
            "object": "chat.completion",
            "created": created,
            "model": model_id,
            "choices": [{
                "index": 0,
                "message": {
                    "role": "assistant",
                    "content": text,
                    "reasoning_content": reasoning or None,
                },
                "finish_reason": finish,
            }],
            "usage": {
                "prompt_tokens": done.get("n_prompt", 0),
                "completion_tokens": done.get("tokens", 0),
                "total_tokens": done.get("n_prompt", 0) + done.get("tokens", 0),
            },
            "bmoe": done,
        })


def main():
    global engine, model_id, max_tokens_limit
    ap = argparse.ArgumentParser(description="OpenAI-compatible bridge over bmoe-cli --session")
    ap.add_argument("--engine", default=None,
                    help="engine binary; default: $BMOE_ENGINE, else the host build, else a bmoe-cli beside this script")
    ap.add_argument("-m", "--model", default=None,
                    help="gguf path; default: $BMOE_MODEL, else the tiny test model from a host build")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8017)
    ap.add_argument("--model-id", default="bmoe-local", help="model name reported to clients")
    ap.add_argument("--max-tokens", type=int, default=2048,
                    help="ceiling for any request's completion budget; larger client values are clamped to this")
    ap.add_argument("--ready-timeout", type=float, default=120.0)
    ap.add_argument("--engine-args", default="",
                    help="extra engine args as one quoted string, e.g. '--moe-stream --ctx-size 4096'")
    args = ap.parse_args()
    model_id = args.model_id
    max_tokens_limit = args.max_tokens

    here = Path(__file__).resolve().parent

    engine_path = args.engine or os.environ.get("BMOE_ENGINE")
    if engine_path is None:
        # Repo checkout first (scripts/ sits one level under the root), then a bundle where
        # this script was staged next to bmoe-cli.
        for candidate in (here.parent / "build/cli/bmoe-cli", here / "bmoe-cli"):
            if candidate.is_file():
                engine_path = candidate
                break
        else:
            sys.exit("[bmoe-serve] no engine found — build with scripts/build-host.sh or pass --engine")
    engine_path = Path(engine_path).expanduser()
    if not engine_path.is_file():
        sys.exit(f"[bmoe-serve] engine not found: {engine_path}")
    if not os.access(engine_path, os.X_OK):
        sys.exit(f"[bmoe-serve] engine is not executable: {engine_path}")

    model_path = args.model or os.environ.get("BMOE_MODEL")
    if model_path is None:
        tiny = here.parent / "build/tests/tiny-moe-qwen3moe.gguf"
        if tiny.is_file():
            model_path = tiny
            print("[bmoe-serve] no model given — using the tiny test model "
                  "(proves the plumbing, useless for real work)", flush=True)
        else:
            sys.exit("[bmoe-serve] no model given — pass --model /path/to/model.gguf or set BMOE_MODEL")
    model_path = Path(model_path).expanduser()
    if not model_path.is_file():
        sys.exit(f"[bmoe-serve] model not found: {model_path}")

    engine_args = shlex.split(args.engine_args)
    engine = Engine(str(engine_path), str(model_path), engine_args, args.ready_timeout)

    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"[bmoe-serve] OpenAI-compatible endpoint: http://{args.host}:{args.port}/v1", flush=True)
    print(f"[bmoe-serve] model id: {model_id}", flush=True)

    def shutdown(sig, frame):
        print("[bmoe-serve] shutting down", flush=True)
        engine.stop()
        sys.exit(0)

    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)
    try:
        server.serve_forever()
    finally:
        engine.stop()


if __name__ == "__main__":
    main()
