#!/usr/bin/env python3
"""Replay a route trace against the cache-aware expert-dropping policy (docs/expert-dropping.md).

The policy skips a routed expert when it is a cache MISS and the router weighted it below
`frac x (1 / n_expert_used)`. A resident expert costs no flash read, so it is never dropped:
quality is spent only where it buys I/O. This script answers, per threshold, what that trade
would have been on an already-recorded run:

  io_saved   fraction of MISS BYTES the policy never reads   -- the win
  mass_lost  fraction of total router weight discarded       -- the proxy for the damage

The static-k baseline (`--n-expert-used`) is replayed on the same rows, because the only question
that matters is comparative: at equal io_saved, which policy discards less weight?

Two limits, both deliberate:
  * This is a STATIC replay. Skipping a read changes what the cache holds later, so the real hit
    pattern drifts from the recorded one. io_saved is an UPPER BOUND, not a prediction.
  * mass_lost is a proxy. It says how much of the router's mass went away, not what that did to
    the output. Only a quality A/B answers that.

A trace recorded with dropping already ON reports its `dropped` column instead of re-deriving it,
which is how the upper bound above gets checked against a real run.

Usage:  route-drop-replay.py <route.csv> [<route.csv> ...]
Stdlib only, like the other analysis scripts here.
"""
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from trace_io import read_preamble_csv  # noqa: E402

MISS = 0
DECODE, PREFILL = 1, 0


def cells(rows, phase):
    """Group rows into routing cells: (turn, step, layer) -> [(slot, weight, residency, bytes, dropped)]."""
    out = defaultdict(list)
    for r in rows:
        if int(r["phase"]) != phase:
            continue
        try:
            w = float(r["weight"])
        except ValueError:
            continue  # 'nan': the graph exposed no weight node, so no threshold can be applied
        out[(r["turn"], r["step"], r["layer"])].append(
            (int(r["slot"]), w, int(r["residency"]), int(r["expert_bytes"]), int(r.get("dropped", 0) or 0))
        )
    return out


def replay_threshold(cs, thr):
    """Drop a miss weighted below thr, never the cell's top expert (which the engine also pins)."""
    miss_bytes = dropped_bytes = 0
    total_mass = lost_mass = 0.0
    kept_hist = defaultdict(int)
    for entries in cs.values():
        best = max(range(len(entries)), key=lambda i: entries[i][1])
        kept = 0
        for i, (_slot, w, res, nb, _d) in enumerate(entries):
            total_mass += w
            if res == MISS:
                miss_bytes += nb
                if i != best and w < thr:
                    dropped_bytes += nb
                    lost_mass += w
                    continue
            kept += 1
        kept_hist[kept] += 1
    return miss_bytes, dropped_bytes, total_mass, lost_mass, kept_hist


def replay_static_k(cs, keep_k):
    """Baseline --n-expert-used: keep the top keep_k slots whatever the cache holds."""
    miss_bytes = dropped_bytes = 0
    total_mass = lost_mass = 0.0
    for entries in cs.values():
        for slot, w, res, nb, _d in entries:
            total_mass += w
            if res == MISS:
                miss_bytes += nb
            if slot >= keep_k:
                lost_mass += w
                if res == MISS:
                    dropped_bytes += nb
    return miss_bytes, dropped_bytes, total_mass, lost_mass


def observed(cs):
    """What a trace recorded with dropping ON actually did. (dropped rows, weight mass, miss bytes)."""
    n_dropped = 0
    lost_mass = total_mass = 0.0
    for entries in cs.values():
        for _slot, w, _res, _nb, d in entries:
            total_mass += w
            if d:
                n_dropped += 1
                lost_mass += w
    return n_dropped, lost_mass, total_mass


def pct(num, den):
    return 100.0 * num / den if den else 0.0


def report(path):
    meta, rows = read_preamble_csv(path)
    k = int(meta.get("n_expert_used", 0) or 0)
    if k <= 0:
        print(f"{path}: no n_expert_used in the preamble; cannot express a threshold")
        return
    print("=" * 78)
    print(f"{os.path.basename(path)}   arch={meta.get('arch')}  n_expert={meta.get('n_expert')}  k={k}")
    print("=" * 78)

    for phase, label in ((DECODE, "DECODE"), (PREFILL, "PREFILL")):
        cs = cells(rows, phase)
        if not cs:
            print(f"\n[{label}] no rows")
            continue
        n_tot = sum(len(e) for e in cs.values())
        n_miss = sum(1 for e in cs.values() for x in e if x[2] == MISS)
        print(f"\n[{label}] {len(cs)} routing cells, {n_tot} routed experts, {pct(n_miss, n_tot):.1f}% misses")

        n_drop, lost, total = observed(cs)
        if n_drop:
            print(f"  recorded: dropping was ON for this run -- {n_drop} routings dropped "
                  f"({pct(n_drop, n_tot):.1f}%), {pct(lost, total):.2f}% of the weight mass")

        uniform = 1.0 / k
        print(f"\n  cache-aware threshold, as a fraction of the uniform share 1/k = {100 * uniform:.2f}%")
        print(f"  {'frac':>6}  {'thr':>8}  {'io_saved':>9}  {'mass_lost':>10}   surviving experts per cell")
        for frac in (0.25, 0.5, 0.75, 1.0):
            thr = uniform * frac
            mb, db, tm, lm, hist = replay_threshold(cs, thr)
            h = " ".join(f"{kk}:{pct(v, len(cs)):.0f}%" for kk, v in sorted(hist.items()))
            print(f"  {frac:6.2f}  {100 * thr:7.2f}%  {pct(db, mb):8.1f}%  {pct(lm, tm):9.2f}%   {h}")

        print(f"\n  static-k baseline (--n-expert-used), same rows, for comparison at equal io_saved")
        print(f"  {'keep_k':>6}  {'io_saved':>9}  {'mass_lost':>10}")
        for keep in range(k - 1, 0, -1):
            mb, db, tm, lm = replay_static_k(cs, keep)
            print(f"  {keep:6d}  {pct(db, mb):8.1f}%  {pct(lm, tm):9.2f}%")
    print()


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    for p in argv[1:]:
        report(p)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
