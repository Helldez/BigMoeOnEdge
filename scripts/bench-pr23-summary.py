#!/usr/bin/env python3
# Summarise the PR2/PR3 device A/B: parse each run's .log (throughput + cache + the new
# prefetch/spec-gate lines) and .metrics (device pressure) into one markdown table per model.
import os, re, sys, statistics

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BENCH = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, ".bench-pr23")

CFG_FULL = [
    ("base", "baseline (overlap)"),
    ("pf2", "+ prefetch 2"),
    ("sg", "+ spec-gate"),
    ("sg_pf2", "+ spec-gate + prefetch 2"),
]
CFG_MAX = [
    ("base", "baseline (overlap)"),
    ("pf1", "+ prefetch 1"),
]

def f(pattern, text, grp=1, cast=float, default=None):
    m = re.search(pattern, text)
    return cast(m.group(grp)) if m else default

def read_metrics(path):
    d = {}
    if os.path.exists(path):
        for line in open(path):
            if "=" in line:
                k, v = line.strip().split("=", 1)
                d[k] = v
    return d

def cache_tps_from_csv(path):
    # aggregate decode tok/s = n / sum(wall_ms)/1000, plus min/max/p95 for the distribution
    walls, col = [], {}
    if not os.path.exists(path):
        return None
    for row in open(path):
        row = row.strip()
        if row.startswith("step,"):
            col = {n: i for i, n in enumerate(row.split(","))}; continue
        if not row or row.startswith("#"):
            continue
        c = row.split(",")
        try: walls.append(float(c[2]))
        except (IndexError, ValueError): pass
    if not walls:
        return None
    tps = sorted(1000.0 / w for w in walls if w > 0)
    n = len(walls); total = sum(walls) / 1000.0
    p95 = tps[min(len(tps) - 1, int(0.95 * (len(tps) - 1)))]
    return {"mean": n / total if total else 0, "min": tps[0], "max": tps[-1],
            "median": statistics.median(tps), "p95": p95, "n": n}

def analyze(model, cfgs):
    rows = []
    for tag, label in cfgs:
        base = os.path.join(BENCH, f"{model}_{tag}")
        log = open(base + ".log", encoding="utf-8", errors="ignore").read() if os.path.exists(base + ".log") else ""
        met = read_metrics(base + ".metrics")
        csv = cache_tps_from_csv(base + ".csv")
        rows.append({
            "label": label,
            "tps": f(r"generation:.*?\(([\d.]+) tok/s\)", log),
            "csv": csv,
            "hit": f(r"moe-cache:\s*([\d.]+)% hit", log),
            "read_tok": f(r"read [\d.]+ MiB \(([\d.]+) MiB/token", log),
            "stall": f(r"stall ([\d.]+) s/token", log),
            "spec_mib": f(r"moe-prefetch:\s*([\d.]+) MiB", log),
            "useful": f(r"experts useful \(([\d.]+)%\)", log),
            "useful_frac": f(r"([\d]+/[\d]+) experts useful", log, cast=str),
            "recall": f(r"moe-spec-gate:\s*([\d.]+)% router", log),
            "ttft": f(r"TTFT ([\d.]+) s", log),
            "prefill": f(r"prefill:.*?\(([\d.]+) tok/s\)", log),
            "peak_rss": (float(met.get("peak_rss_kb", 0)) / 1048576.0) if met.get("peak_rss_kb") else None,
            "ram_floor": (float(met.get("mem_avail_floor_kb", 0)) / 1048576.0) if met.get("mem_avail_floor_kb") else None,
            "cpu_max": (float(met.get("cpu_temp_max_mC", 0)) / 1000.0) if met.get("cpu_temp_max_mC") else None,
            "batt_max": (float(met.get("batt_temp_max_dC", 0)) / 10.0) if met.get("batt_temp_max_dC") else None,
        })
    return rows

def cell(v, fmt="{:.2f}", suf=""):
    return (fmt.format(v) + suf) if v is not None else "—"

md = []
MODELS = [
    ("qwen", "Qwen3-30B-A3B-Q4_K_M — base: cache 4000 MiB, lane 4, overlap", CFG_FULL),
    ("gemma", "Gemma-4-26B-A4B-it-Q4_K_M — base: cache 2000 MiB, lane 4, overlap", CFG_FULL),
    ("qwen5k", "Qwen3-30B-A3B-Q4_K_M — MAX: cache 5000 MiB, lane 4, overlap", CFG_MAX),
]
for model, pretty, cfgs in MODELS:
    if not os.path.exists(os.path.join(BENCH, f"{model}_{cfgs[0][0]}.log")):
        continue
    rows = analyze(model, cfgs)
    md.append(f"\n### {pretty}\n")
    md.append("| Config | decode tok/s | median | p95 | cache hit | flash MiB/tok | stall ms/tok | "
              "spec MiB | useful | recall | TTFT s | peak RSS | CPU max |")
    md.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for r in rows:
        c = r["csv"] or {}
        base_tps = rows[0]["tps"]
        tps = r["tps"]
        delta = f" ({'+' if tps and base_tps and tps>=base_tps else ''}{(tps-base_tps):.2f})" if (tps and base_tps and r is not rows[0]) else ""
        md.append("| {label} | **{tps}**{delta} | {med} | {p95} | {hit} | {read} | {stall} | "
                  "{spec} | {useful} | {recall} | {ttft} | {rss} | {cpu} |".format(
            label=r["label"],
            tps=cell(tps), delta=delta,
            med=cell(c.get("median")), p95=cell(c.get("p95")),
            hit=cell(r["hit"], "{:.0f}", "%"),
            read=cell(r["read_tok"], "{:.0f}"),
            stall=cell(r["stall"], "{:.3f}"),
            spec=cell(r["spec_mib"], "{:.0f}") if r["spec_mib"] else "—",
            useful=(r["useful_frac"] + f" ({r['useful']:.0f}%)") if r["useful_frac"] and r["useful"] is not None else "—",
            recall=cell(r["recall"], "{:.0f}", "%"),
            ttft=cell(r["ttft"], "{:.1f}"),
            rss=cell(r["peak_rss"], "{:.2f}", " GB"),
            cpu=cell(r["cpu_max"], "{:.0f}", "°C"),
        ))

out = os.path.join(BENCH, "summary.md")
open(out, "w", encoding="utf-8").write("\n".join(md) + "\n")
print("\n".join(md))
print(f"\n-> {out}")
