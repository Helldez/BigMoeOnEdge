#!/usr/bin/env python3
"""The same memory-pressure check on any machine: bmoe-cli --auto, alone and beside a process that
holds memory, with the per-token numbers that say whether the run stalled.

    python3 scripts/pressure-check.py --cli build/bin/bmoe-cli -m model.gguf [--hold-gib 0,4] [--runs 2]

For each held size (0 = nothing beside the engine) it runs the CLI once per round, rounds
interleaved, and prints one row per run: decode tok/s, the slowest token after warm-up, the tokens
over one second, and what the cache did (its budget at the end, brakes and recoveries from the log).
The holder is this script itself (--hold), so nothing beyond Python's standard library is needed and
the check reads the same on Linux, macOS and Windows. Raw logs and CSVs go to --out.
"""

import argparse
import csv
import os
import re
import subprocess
import sys
import time

WARMUP_TOKENS = 8  # the cache fills over the first tokens; their compute is not the steady state


def hold(gib):
    # Ordinary memory, touched page by page and then kept in use, like an application beside the engine.
    blocks = [bytearray(1 << 20) for _ in range(int(gib * 1024))]
    page = 4096
    for b in blocks:
        for i in range(0, len(b), page):
            b[i] = 1
    print("holding", len(blocks), "MiB", flush=True)
    while True:
        for b in blocks[:: max(1, len(blocks) // 64)]:
            b[0] = (b[0] + 1) & 0xFF
        time.sleep(0.5)


def read_tokens(path):
    with open(path, newline="") as f:
        rows = [r for r in csv.DictReader(line for line in f if not line.startswith("#"))]
    summary = {}
    with open(path) as f:
        for line in f:
            if line.startswith("# summary"):
                for kv in line.split()[2:]:
                    k, _, v = kv.partition("=")
                    summary[k] = v
    return rows, summary


def one_run(args, held, rnd):
    tag = "hold%s-r%d" % (held, rnd)
    csv_path = os.path.join(args.out, tag + ".csv")
    log_path = os.path.join(args.out, tag + ".log")
    holder = None
    if held > 0:
        holder = subprocess.Popen([sys.executable, "-I", os.path.abspath(__file__), "--hold", str(held)],
                                  stdout=subprocess.PIPE, text=True)
        holder.stdout.readline()  # "holding": the memory is touched before the engine plans
        time.sleep(args.settle)
    cmd = [args.cli, "-m", args.model, "--auto", "-n", str(args.tokens), "--temp", "0", "--csv", csv_path,
           "-p", args.prompt] + args.extra
    try:
        with open(log_path, "w") as log:
            code = subprocess.call(cmd, stdout=log, stderr=subprocess.STDOUT)
    finally:
        if holder:
            holder.kill()
            holder.wait()
    if code != 0 or not os.path.exists(csv_path):
        return {"tag": tag, "error": "exit %d (see %s)" % (code, log_path)}
    rows, summary = read_tokens(csv_path)
    steady = [float(r["compute_ms"]) for r in rows[WARMUP_TOKENS:]]
    walls = [float(r["wall_ms"]) for r in rows[WARMUP_TOKENS:]]
    log = open(log_path, errors="replace").read()
    return {
        "tag": tag,
        "tok_s": float(summary.get("tok/s", 0)),
        "worst_ms": max(walls) if walls else 0.0,
        "over_1s": sum(1 for w in walls if w > 1000.0),
        "median_compute_ms": sorted(steady)[len(steady) // 2] if steady else 0.0,
        "cache_start_mib": float(rows[0]["cache_budget_mib"]) if rows else 0.0,
        "cache_end_mib": float(rows[-1]["cache_budget_mib"]) if rows else 0.0,
        "brakes": len(re.findall(r"cache brake:", log)),
        "recovers": len(re.findall(r"cache recover:", log)),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--hold", type=float, help=argparse.SUPPRESS)
    ap.add_argument("--cli", help="path to bmoe-cli")
    ap.add_argument("-m", "--model", help="the GGUF (its first shard)")
    ap.add_argument("--hold-gib", default="0,4", help="sizes to hold beside the engine, GiB (default 0,4)")
    ap.add_argument("--runs", type=int, default=2, help="rounds, interleaved across sizes (default 2)")
    ap.add_argument("--tokens", type=int, default=256)
    ap.add_argument("--settle", type=float, default=10.0, help="seconds between the holder and the engine")
    ap.add_argument("--pause", type=float, default=30.0, help="seconds between runs, for the memory to settle")
    ap.add_argument("--prompt", default="Explain in detail how a mixture-of-experts language model routes each "
                    "token to its experts, and why that makes it possible to run a model larger than RAM.")
    ap.add_argument("--out", default="pressure-check-out")
    ap.add_argument("extra", nargs="*", help="after --: more bmoe-cli flags (a benchmark keeps --auto alone)")
    args = ap.parse_args()
    if args.hold is not None:
        hold(args.hold)
        return
    if not args.cli or not args.model:
        ap.error("--cli and -m are required")
    os.makedirs(args.out, exist_ok=True)
    sizes = [float(x) for x in args.hold_gib.split(",")]
    print("%-12s %8s %9s %7s %10s %13s %7s %8s" % ("run", "tok/s", "worst ms", ">1 s", "median ms",
                                                   "cache MiB", "brakes", "recovers"))
    first = True
    for rnd in range(1, args.runs + 1):
        for held in sizes:
            if not first:
                time.sleep(args.pause)
            first = False
            r = one_run(args, held, rnd)
            if "error" in r:
                print("%-12s %s" % (r["tag"], r["error"]), flush=True)
                continue
            print("%-12s %8.2f %9.0f %7d %10.0f %6.0f->%-6.0f %7d %8d" % (
                r["tag"], r["tok_s"], r["worst_ms"], r["over_1s"], r["median_compute_ms"], r["cache_start_mib"],
                r["cache_end_mib"], r["brakes"], r["recovers"]), flush=True)


if __name__ == "__main__":
    main()
