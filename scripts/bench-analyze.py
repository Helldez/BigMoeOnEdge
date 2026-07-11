#!/usr/bin/env python3
# Parse the benchmark CSVs and emit mean/min/max (+ median, p5, p95) tok/s tables,
# both as a console table and as a markdown file for the docs.
#
# tok/s per token = 1000 / wall_ms (wall_ms is the per-token decode time).
#   mean = aggregate n_tokens / total_seconds (the throughput a user actually sees)
#   min  = slowest single token = 1000 / max(wall_ms)   (worst-case stall)
#   max  = fastest single token = 1000 / min(wall_ms)   (best-case, cache-warm)
# min/max are single-token extremes; median and p5/p95 describe the steady state.
import glob, os, sys, statistics

BENCH = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\raffa\Documents\BigMoeOnEdge\.bench"

ORDER = ["mmap", "stream", "c2000_l2", "c2000_l4", "c4000_l2", "c4000_l4"]
LABEL = {
    "mmap": "solo mmap (no streaming)",
    "stream": "streaming O_DIRECT, cache 0, lane 4",
    "c2000_l2": "streaming + cache 2000 MiB, lane 2",
    "c2000_l4": "streaming + cache 2000 MiB, lane 4",
    "c4000_l2": "streaming + cache 4000 MiB, lane 2",
    "c4000_l4": "streaming + cache 4000 MiB, lane 4",
}

def pct(sorted_vals, q):
    if not sorted_vals:
        return 0.0
    i = q * (len(sorted_vals) - 1)
    lo = int(i)
    hi = min(lo + 1, len(sorted_vals) - 1)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (i - lo)

def analyze(path):
    walls, summary = [], {}
    with open(path, newline="") as f:
        for row in f:
            row = row.strip()
            if not row or row.startswith("step,"):
                continue
            if row.startswith("# summary"):
                for tok in row.replace("# summary", "").split():
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        summary[k] = v
                continue
            parts = row.split(",")
            try:
                walls.append(float(parts[2]))
            except (IndexError, ValueError):
                pass
    if not walls:
        return None
    n = len(walls)
    tps = sorted(1000.0 / w for w in walls if w > 0)
    total_s = sum(walls) / 1000.0
    return {
        "n": n,
        "mean": n / total_s if total_s > 0 else 0.0,
        "min": tps[0],
        "max": tps[-1],
        "median": statistics.median(tps),
        "p5": pct(tps, 0.05),
        "p95": pct(tps, 0.95),
        "std": statistics.pstdev(tps),
        "cache_hit": summary.get("cache_hit_pct", "-"),
        "read_MiB_tok": (float(summary.get("read_MiB", "0")) / n) if summary.get("read_MiB") and n else 0.0,
    }

md = []
for model, pretty in (("qwen", "Qwen3-30B-A3B-Q4_K_M (18.5 GB, 128 experts, top-8, 48 layers)"),
                       ("gemma", "Gemma-4-26B-A4B-it-Q4_K_M (17.0 GB, fused gate+up experts)")):
    print(f"\n===== {model.upper()} =====")
    hdr = f"{'config':38s} {'n':>4s} {'mean':>6s} {'min':>6s} {'max':>6s} {'med':>6s} {'p5':>6s} {'p95':>6s} {'hit%':>5s} {'MiB/t':>7s}"
    print(hdr)
    md.append(f"\n### {pretty}\n")
    md.append("| Config | mean | min | max | median | p5 | p95 | cache hit | flash read/token |")
    md.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for key in ORDER:
        p = os.path.join(BENCH, f"{model}_{key}.csv")
        if not os.path.exists(p):
            print(f"{LABEL[key]:38s}  (no CSV)")
            md.append(f"| {LABEL[key]} | — | — | — | — | — | — | — | — |")
            continue
        r = analyze(p)
        if r is None:
            continue
        hit = f"{float(r['cache_hit']):.0f}%" if r['cache_hit'] not in ("-", "-1.0") else "—"
        print(f"{LABEL[key]:38s} {r['n']:4d} {r['mean']:6.2f} {r['min']:6.2f} {r['max']:6.2f} "
              f"{r['median']:6.2f} {r['p5']:6.2f} {r['p95']:6.2f} {hit:>5s} {r['read_MiB_tok']:6.0f}M")
        md.append(f"| {LABEL[key]} | **{r['mean']:.2f}** | {r['min']:.2f} | {r['max']:.2f} | "
                  f"{r['median']:.2f} | {r['p5']:.2f} | {r['p95']:.2f} | {hit} | {r['read_MiB_tok']:.0f} MiB |")

out = os.path.join(BENCH, "summary.md")
with open(out, "w", encoding="utf-8") as f:
    f.write("\n".join(md) + "\n")
print(f"\nmarkdown -> {out}")
print("tok/s: mean=aggregate throughput  min/max=slowest/fastest single token  med/p5/p95=steady-state distribution")
