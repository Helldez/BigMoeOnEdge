#!/usr/bin/env python3
"""tinyMMLU under a lossy routing setting, from the desktop.

tinyMMLU (tinyBenchmarks, arXiv:2402.14992) is 100 MMLU questions chosen so that accuracy on them
estimates accuracy on the full 14k. Each question is one pass of `--ppl --ppl-step --ppl-choices`:
the question and its four options are decoded token by token, in the decode regime a cache-aware
policy actually acts in, and the answer is the letter with the highest log-probability at the end.
Zero-shot, plain prompt, no chat template, no thinking. One model load per cell.

The absolute score is below the model's published one (quantized, zero-shot, no reasoning); what
this measures is the DIFFERENCE between cells, on the same 100 questions, deterministically.

Usage:
    python scripts/tinymmlu-bench.py --parquet test.parquet --cli build/cli/Release/bmoe-cli.exe \
        --model M.gguf --out results/ --lambda 0 --lambda 0.15 [--limit N]

The parquet is data/test-00000-of-00001.parquet from huggingface.co/datasets/tinyBenchmarks/tinyMMLU.
"""

import argparse
import json
import os
import subprocess
import sys

LETTERS = ["A", "B", "C", "D"]


def build_prompts(parquet, out_dir, limit):
    import pandas as pd

    df = pd.read_parquet(parquet)
    if limit:
        df = df.iloc[:limit]
    os.makedirs(out_dir, exist_ok=True)
    keys = []
    for i, row in df.iterrows():
        subject = str(row["subject"]).replace("_", " ")
        lines = [f"The following is a multiple choice question about {subject}.", "", str(row["question"]).strip()]
        for letter, choice in zip(LETTERS, list(row["choices"])):
            lines.append(f"{letter}. {str(choice).strip()}")
        lines.append("Answer:")
        path = os.path.join(out_dir, f"q{i:03d}.txt")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines))
        keys.append({"path": path, "answer": int(row["answer"]), "subject": str(row["subject"])})
    with open(os.path.join(out_dir, "list.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(k["path"] for k in keys) + "\n")
    with open(os.path.join(out_dir, "key.json"), "w", encoding="utf-8") as f:
        json.dump(keys, f, indent=1)
    return keys


def done_paths(log_path):
    """Questions a previous, interrupted run of this cell already scored."""
    done = set()
    if not os.path.exists(log_path):
        return done
    current = None
    with open(log_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("ppl-file: "):
                current = os.path.normcase(line[len("ppl-file: "):].strip())
            elif line.startswith("ppl-choices:") and current:
                done.add(current)
    return done


def run_cell(cli, model, list_path, lam, log_path, threads, cache_mb, ctx):
    # Resumable: a cell interrupted half-way keeps what it scored and continues from the first
    # question without a result, appending to the same log. A long cell on a slow host can then
    # survive a lost terminal.
    done = done_paths(log_path)
    with open(list_path, encoding="utf-8") as f:
        todo = [p.strip() for p in f if p.strip() and os.path.normcase(p.strip()) not in done]
    if not todo:
        return
    if done:
        print(f"  resuming: {len(done)} scored, {len(todo)} to go", flush=True)
        list_path = log_path + ".todo"
        with open(list_path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(todo) + "\n")
    cmd = [
        cli, "-m", model, "-t", str(threads), "-c", str(ctx), "--ubatch", str(ctx), "--moe-stream",
        "--cache-mb", str(cache_mb), "--io-threads", "4", "--overlap",
        "--ppl-list", list_path, "--ppl-step", "--ppl-choices", " A, B, C, D",
    ]
    if lam > 0:
        cmd += ["--expert-substitute", str(lam)]
    # On Windows the CLI's DLLs live in build/bin/<config> next to build/cli/<config>; put that
    # directory on PATH so the runner does not depend on the caller's shell.
    env = dict(os.environ)
    cli_dir = os.path.dirname(os.path.abspath(cli))
    bin_dir = os.path.normpath(os.path.join(cli_dir, "..", "..", "bin", os.path.basename(cli_dir)))
    env["PATH"] = bin_dir + os.pathsep + env.get("PATH", "")
    with open(log_path, "a", encoding="utf-8") as log:
        rc = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=False, env=env).returncode
    if rc != 0:
        print(f"bmoe-cli exited with {rc} (see {log_path})", file=sys.stderr)


def parse_cell(log_path, keys):
    results = []
    current = None
    substituted = reranked = 0
    with open(log_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("ppl-file: "):
                current = line[len("ppl-file: "):].strip()
            elif line.startswith("ppl-choices:"):
                parts = line.split()[1:]
                logp = [float(p.split("=")[1]) for p in parts]
                results.append((current, logp))
            elif line.startswith("ppl-policy:"):
                # "... dropped, S/R reranked slots substituted"
                tail = line.split("dropped,")[1].split()[0]
                s, r = tail.split("/")
                substituted += int(s)
                reranked += int(r)
    by_path = {os.path.normcase(k["path"]): k for k in keys}
    correct = 0
    rows = []
    for path, logp in results:
        k = by_path.get(os.path.normcase(path))
        if not k:
            continue
        pred = max(range(len(logp)), key=lambda j: logp[j])
        ok = pred == k["answer"]
        correct += ok
        rows.append({"path": os.path.basename(path), "subject": k["subject"], "pred": LETTERS[pred],
                     "answer": LETTERS[k["answer"]], "correct": ok, "logp": logp})
    return correct, len(rows), substituted, reranked, rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--parquet", required=True)
    ap.add_argument("--cli", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--lambda", dest="lambdas", type=float, action="append", required=True)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--cache-mb", type=int, default=2000)
    ap.add_argument("--ctx", type=int, default=512)
    args = ap.parse_args()

    keys = build_prompts(args.parquet, os.path.join(args.out, "prompts"), args.limit)
    list_path = os.path.join(args.out, "prompts", "list.txt")
    print(f"{len(keys)} questions", flush=True)
    summary = []
    for lam in args.lambdas:
        log_path = os.path.join(args.out, f"cell_L{lam:g}.log")
        print(f"cell L={lam:g} ...", flush=True)
        run_cell(args.cli, args.model, list_path, lam, log_path, args.threads, args.cache_mb, args.ctx)
        correct, n, sub, rer, rows = parse_cell(log_path, keys)
        pct = 100.0 * correct / n if n else 0.0
        frac = 100.0 * sub / rer if rer else 0.0
        summary.append((lam, correct, n, pct, frac))
        with open(os.path.join(args.out, f"cell_L{lam:g}.json"), "w", encoding="utf-8") as f:
            json.dump(rows, f, indent=1)
        print(f"L={lam:g}: {correct}/{n} ({pct:.1f}%), {frac:.1f}% of slots substituted", flush=True)
    print()
    print("| `--expert-substitute` | tinyMMLU | slots substituted |")
    print("|---|---:|---:|")
    for lam, correct, n, pct, frac in summary:
        print(f"| {lam:g} | {correct}/{n} ({pct:.1f} %) | {frac:.1f} % |")


if __name__ == "__main__":
    sys.exit(main())
