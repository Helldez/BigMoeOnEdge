#!/usr/bin/env python3
# Parse the benchmark CSVs (+ sibling .metrics) and emit two tables:
#   1. Throughput  — decode tok/s (mean/min/max/median/p5/p95), prefill tok/s, TTFT.
#   2. Pressure    — peak RSS, free-RAM floor, CPU temperature rise (thermal cost).
# Writes both as markdown to .bench/summary.md for the docs.
#
# decode tok/s per token = 1000 / wall_ms (wall_ms = per-token decode time).
#   mean = aggregate n_tokens / total_seconds ; min/max = slowest/fastest single token.
import os, sys, statistics

BENCH = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\raffa\Documents\BigMoeOnEdge\.bench"
ORDER = ["mmap", "stream", "c2000_l2", "c2000_l4", "c4000_l2", "c4000_l4",
         "stream_ov", "c4000_l4_ov"]
LABEL = {
    "mmap": "solo mmap (no streaming)",
    "stream": "streaming O_DIRECT, cache 0, lane 4",
    "c2000_l2": "streaming + cache 2000 MiB, lane 2",
    "c2000_l4": "streaming + cache 2000 MiB, lane 4",
    "c4000_l2": "streaming + cache 4000 MiB, lane 2",
    "c4000_l4": "streaming + cache 4000 MiB, lane 4",
    "stream_ov": "streaming O_DIRECT + overlap, cache 0, lane 4",
    "c4000_l4_ov": "streaming + cache 4000 MiB, lane 4, overlap",
}

def pct(v, q):
    if not v:
        return 0.0
    i = q * (len(v) - 1); lo = int(i); hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (i - lo)

def read_metrics(path):
    d = {}
    if os.path.exists(path):
        for line in open(path):
            if "=" in line:
                k, v = line.strip().split("=", 1)
                d[k] = v
    return d

def num(d, k):
    try:
        return float(d.get(k, ""))
    except ValueError:
        return None

def analyze(csv_path):
    walls, stalls, mgmts, summ = [], [], [], {}
    # Column layout is read from the header so trailing additive columns (stall_ms, then mgmt_ms)
    # are picked up by NAME; older CSVs that lack them are handled transparently.
    col = {}
    with open(csv_path) as f:
        for row in f:
            row = row.strip()
            if row.startswith("step,"):
                col = {name: i for i, name in enumerate(row.split(","))}
                continue
            if not row:
                continue
            if row.startswith("# summary"):
                for tok in row.replace("# summary", "").split():
                    if "=" in tok:
                        k, v = tok.split("=", 1); summ[k] = v
                continue
            cols = row.split(",")
            try:
                walls.append(float(cols[2]))
            except (IndexError, ValueError):
                continue
            def by_name(name, dst):
                i = col.get(name)
                if i is not None and i < len(cols):
                    try:
                        dst.append(float(cols[i]))
                    except ValueError:
                        pass
            # stall_ms and mgmt_ms are optional trailing columns; absent in older CSVs.
            by_name("stall_ms", stalls)
            by_name("mgmt_ms", mgmts)
    if not walls:
        return None
    n = len(walls); tps = sorted(1000.0 / w for w in walls if w > 0)
    total_s = sum(walls) / 1000.0
    m = read_metrics(csv_path.replace(".csv", ".metrics"))
    def g(k):
        try: return float(summ.get(k, ""))
        except ValueError: return 0.0
    return {
        "n": n, "mean": n / total_s if total_s else 0.0, "min": tps[0], "max": tps[-1],
        "median": statistics.median(tps), "p5": pct(tps, 0.05), "p95": pct(tps, 0.95),
        "cache_hit": summ.get("cache_hit_pct", "-"),
        "read_MiB_tok": (g("read_MiB") / n) if n else 0.0,
        "stall_ms_tok": (statistics.mean(stalls) if stalls else None),
        "mgmt_ms_tok": (statistics.mean(mgmts) if mgmts else None),
        "prefill_tps": g("prefill_tps"), "load_s": g("load_s"),
        "prefill_s": g("prefill_s"), "ttft": g("load_s") + g("prefill_s"),
        "peak_rss_gb": (num(m, "peak_rss_kb") or 0) / 1048576.0,
        "mem_floor_gb": (num(m, "mem_avail_floor_kb") or 0) / 1048576.0,
        "cpu0": (num(m, "cpu_temp_before_mC") or 0) / 1000.0,
        "cpu_max": (num(m, "cpu_temp_max_mC") or 0) / 1000.0,
        "batt_max": (num(m, "batt_temp_max_dC") or 0) / 10.0,
    }

md = []
for model, pretty in (("qwen", "Qwen3-30B-A3B-Q4_K_M (18.5 GB, 128 experts, top-8, 48 layers)"),
                      ("gemma", "Gemma-4-26B-A4B-it-Q4_K_M (17.0 GB, fused gate+up experts)")):
    rows = []
    for key in ORDER:
        p = os.path.join(BENCH, f"{model}_{key}.csv")
        rows.append((key, analyze(p) if os.path.exists(p) else None))

    print(f"\n===== {model.upper()} — throughput =====")
    print(f"{'config':36s} {'mean':>6s} {'min':>6s} {'max':>6s} {'med':>6s} {'p95':>6s} {'pref':>6s} {'TTFT':>6s} {'hit':>5s} {'MiB/t':>6s} {'stall':>7s}")
    md.append(f"\n### {pretty}\n\n**Throughput** (tok/s; mean = aggregate decode, prefill = prompt-processing):\n")
    md.append("| Config | decode mean | min | max | median | p95 | prefill tok/s | TTFT (s) | cache hit | flash read/token | stall ms/tok |")
    md.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for key, r in rows:
        if r is None:
            md.append(f"| {LABEL[key]} | — | — | — | — | — | — | — | — | — | — |"); continue
        hit = f"{float(r['cache_hit']):.0f}%" if r['cache_hit'] not in ("-", "-1.0") else "—"
        stall = f"{r['stall_ms_tok']:.1f}" if r['stall_ms_tok'] is not None else "—"
        print(f"{LABEL[key]:36s} {r['mean']:6.2f} {r['min']:6.2f} {r['max']:6.2f} {r['median']:6.2f} {r['p95']:6.2f} {r['prefill_tps']:6.2f} {r['ttft']:6.1f} {hit:>5s} {r['read_MiB_tok']:5.0f}M {stall:>7s}")
        md.append(f"| {LABEL[key]} | **{r['mean']:.2f}** | {r['min']:.2f} | {r['max']:.2f} | {r['median']:.2f} | {r['p95']:.2f} | {r['prefill_tps']:.2f} | {r['ttft']:.1f} | {hit} | {r['read_MiB_tok']:.0f} MiB | {stall} |")

    print(f"\n===== {model.upper()} — device pressure =====")
    print(f"{'config':36s} {'peakRSS':>8s} {'RAMfloor':>9s} {'CPU0':>6s} {'CPUmax':>7s} {'battMax':>8s}")
    md.append(f"\n**Device pressure** (peak process RSS, free-RAM floor, SoC/battery temperature):\n")
    md.append("| Config | peak RSS | free-RAM floor | CPU start | CPU max | battery max |")
    md.append("|---|---:|---:|---:|---:|---:|")
    for key, r in rows:
        if r is None:
            md.append(f"| {LABEL[key]} | — | — | — | — | — |"); continue
        print(f"{LABEL[key]:36s} {r['peak_rss_gb']:6.2f}GB {r['mem_floor_gb']:7.2f}GB {r['cpu0']:5.1f}C {r['cpu_max']:6.1f}C {r['batt_max']:7.1f}C")
        md.append(f"| {LABEL[key]} | {r['peak_rss_gb']:.2f} GB | {r['mem_floor_gb']:.2f} GB | {r['cpu0']:.1f} °C | {r['cpu_max']:.1f} °C | {r['batt_max']:.1f} °C |")

out = os.path.join(BENCH, "summary.md")
open(out, "w", encoding="utf-8").write("\n".join(md) + "\n")
print(f"\nmarkdown -> {out}")
