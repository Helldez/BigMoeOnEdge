#!/usr/bin/env python3
"""HumanEval (a fixed prefix of it) under a lossy routing setting, from the desktop.

Where tinyMMLU scores one token after a teacher-forced prompt, this generates: 100 to 200 tokens
of function body per problem, greedy, so a routing perturbation compounds the way it does in a
real reply. Each cell is one `bmoe-cli --session` (one model load, warm cache between problems,
KV cleared per problem); the completion is cut at the usual HumanEval stop sequences and graded by
running the canonical tests in a subprocess. pass@1, deterministic.

The subset is the first N problems of the dataset, declared as such; N is a time budget, not a
statistical design, and the number to read is the difference between cells on the same problems.

Usage:
    python scripts/humaneval-bench.py --data HumanEval.jsonl.gz --cli build/cli/Release/bmoe-cli.exe \
        --model M.gguf --out results/ --lambda 0 --lambda 0.15 [--limit 50]

HumanEval.jsonl.gz is data/HumanEval.jsonl.gz from github.com/openai/human-eval (MIT).
"""

import argparse
import gzip
import json
import os
import subprocess
import sys
import tempfile

STOP = ["\nclass ", "\ndef ", "\n#", "\nif __name__", "\nprint("]


def load_problems(path, limit):
    rows = [json.loads(l) for l in gzip.open(path, "rt", encoding="utf-8")]
    return rows[:limit] if limit else rows


def truncate(completion):
    cut = len(completion)
    for s in STOP:
        i = completion.find(s)
        if i != -1 and i < cut:
            cut = i
    return completion[:cut]


def grade(problem, completion, timeout):
    program = problem["prompt"] + completion + "\n\n" + problem["test"] + f"\n\ncheck({problem['entry_point']})\n"
    with tempfile.NamedTemporaryFile("w", suffix=".py", delete=False, encoding="utf-8") as f:
        f.write(program)
        path = f.name
    try:
        r = subprocess.run([sys.executable, path], capture_output=True, timeout=timeout)
        if r.returncode == 0:
            return True, ""
        # Keep the verdict line only: a full traceback names the temporary file, and these
        # records are meant to be committed.
        lines = [l.strip() for l in r.stderr.decode("utf-8", "replace").splitlines() if l.strip()]
        return False, (lines[-1] if lines else f"exit {r.returncode}")
    except subprocess.TimeoutExpired:
        return False, "timeout"
    finally:
        os.unlink(path)


def cli_env(cli):
    env = dict(os.environ)
    cli_dir = os.path.dirname(os.path.abspath(cli))
    bin_dir = os.path.normpath(os.path.join(cli_dir, "..", "..", "bin", os.path.basename(cli_dir)))
    env["PATH"] = bin_dir + os.pathsep + env.get("PATH", "")
    return env


def run_cell(args, lam, problems, results_path):
    done = {}
    if os.path.exists(results_path):
        with open(results_path, encoding="utf-8") as f:
            for line in f:
                r = json.loads(line)
                done[r["task_id"]] = r
    todo = [p for p in problems if p["task_id"] not in done]
    if not todo:
        return done
    if done:
        print(f"  resuming: {len(done)} done, {len(todo)} to go", flush=True)

    cmd = [
        args.cli, "-m", args.model, "-t", str(args.threads), "-c", str(args.ctx), "--ubatch", str(args.ctx),
        "--moe-stream", "--cache-mb", str(args.cache_mb), "--io-threads", "4", "--overlap", "--session",
    ]
    if lam > 0:
        cmd += ["--expert-substitute", str(lam)]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            text=True, encoding="utf-8", bufsize=1, env=cli_env(args.cli))

    def read_until(prefix):
        for line in proc.stdout:
            if line.startswith(prefix):
                return json.loads(line[len(prefix):])
            if line.startswith("BMOE_ERROR "):
                err = json.loads(line[len("BMOE_ERROR "):])
                if err.get("fatal"):
                    raise RuntimeError(err.get("msg", "fatal"))
                return None
        raise RuntimeError("engine exited")

    read_until("BMOE_READY ")
    with open(results_path, "a", encoding="utf-8") as out:
        for i, p in enumerate(todo):
            req = {"cmd": "generate", "id": i + 1, "prompt": p["prompt"], "n_predict": args.n_predict,
                   "think": False, "clear_kv": True}
            proc.stdin.write(json.dumps(req) + "\n")
            proc.stdin.flush()
            d = read_until("BMOE_DONE ")
            if d is None:
                r = {"task_id": p["task_id"], "passed": False, "error": "engine error", "completion": ""}
            else:
                completion = truncate(d["text"])
                ok, err = grade(p, completion, args.timeout)
                r = {"task_id": p["task_id"], "passed": ok, "error": err, "completion": completion,
                     "tokens": d.get("tokens"), "tok_s": d.get("tok_s"), "read_mib": d.get("read_mib"),
                     "cache_hit_pct": d.get("cache_hit_pct")}
            out.write(json.dumps(r) + "\n")
            out.flush()
            done[p["task_id"]] = r
            print(f"  {p['task_id']}: {'pass' if r['passed'] else 'FAIL'}  ({len(done)}/{len(problems)})", flush=True)
    proc.stdin.write(json.dumps({"cmd": "close"}) + "\n")
    proc.stdin.flush()
    proc.wait(timeout=60)
    return done


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--cli", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--lambda", dest="lambdas", type=float, action="append", required=True)
    ap.add_argument("--limit", type=int, default=50)
    ap.add_argument("--n-predict", type=int, default=256)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--cache-mb", type=int, default=2000)
    ap.add_argument("--ctx", type=int, default=1024)
    ap.add_argument("--timeout", type=float, default=10.0)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    problems = load_problems(args.data, args.limit)
    print(f"{len(problems)} problems", flush=True)
    summary = []
    for lam in args.lambdas:
        print(f"cell L={lam:g} ...", flush=True)
        results = run_cell(args, lam, problems, os.path.join(args.out, f"cell_L{lam:g}.jsonl"))
        passed = sum(1 for r in results.values() if r["passed"])
        toks = [r["tok_s"] for r in results.values() if r.get("tok_s")]
        reads = [r["read_mib"] / r["tokens"] for r in results.values() if r.get("tokens") and r.get("read_mib")]
        summary.append((lam, passed, len(results), sum(toks) / len(toks) if toks else 0.0,
                        sum(reads) / len(reads) if reads else 0.0))
        print(f"L={lam:g}: {passed}/{len(results)} pass@1", flush=True)
    print()
    print("| `--expert-substitute` | HumanEval pass@1 | mean tok/s | mean flash per token |")
    print("|---|---:|---:|---:|")
    for lam, passed, n, tps, mib in summary:
        print(f"| {lam:g} | {passed} / {n} | {tps:.2f} | {mib:.0f} MiB |")


if __name__ == "__main__":
    sys.exit(main())
