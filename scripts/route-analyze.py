#!/usr/bin/env python3
# Read a --route-trace file and answer the questions that shape streaming speed:
#
#   matrix   the step x layer matrix itself: which expert ids each layer routed, and which
#            of them repeat from the previous step (the '*' marks)
#   hot      how concentrated routing is per layer -> what a warm-up should preload
#   reuse    reuse distance per (layer, expert) -> LRU vs pinning, and how big a cache buys what
#   overlap  agreement between consecutive steps -> whether temporal prefetch can predict
#   workset  cumulative unique experts vs the cache budget -> does the working set saturate
#   cache    hit rate and prefetch usefulness per layer -> where the reads actually come from
#   entropy  routing entropy per layer -> is top-1 dominant enough to speculate on
#   bytes    flash bytes per step and per layer -> where the I/O time goes
#
# Stdlib only (like bench-analyze.py): no pandas, nothing to install.
#
#   python scripts/route-analyze.py trace.csv                    # the default view set
#   python scripts/route-analyze.py trace.csv --view matrix --steps 0-15
#   python scripts/route-analyze.py trace.csv --view hot --top 12
import argparse
import math
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from trace_io import kv_tokens

PHASES = {"prefill": 0, "decode": 1}
RESIDENCY = {0: "miss", 1: "hit", 2: "prefetch"}
MIB = 1024.0 * 1024.0


def parse_span(spec, hi):
    """'0-15' or '0,5,10' or '7' -> a set of ints, clamped to [0, hi)."""
    if not spec:
        return None
    out = set()
    for part in spec.split(","):
        part = part.strip()
        if "-" in part[1:]:
            a, b = part.split("-", 1)
            out.update(range(int(a), int(b) + 1))
        elif part:
            out.add(int(part))
    return {v for v in out if 0 <= v < hi} if hi else out


class Trace:
    def __init__(self, path, phase=None, turn=None):
        self.model = self.arch = "?"
        self.n_layer = self.n_expert = self.n_expert_used = 0
        self.expert_bytes = {}  # layer -> bytes of one expert
        self.dense_bytes = {}   # layer -> non-streamed bytes
        self.rows = []          # (step, layer, slot, expert, weight, residency, ebytes)
        self._read(path, phase, turn)

    def _read(self, path, phase, turn):
        with open(path, "r") as f:
            for line in f:
                if line.startswith("#"):
                    self._preamble(line)
                    continue
                if line.startswith("turn,"):
                    continue
                p = line.rstrip("\n").split(",")
                if len(p) < 9:
                    continue
                t, ph = int(p[0]), int(p[1])
                if turn is not None and t != turn:
                    continue
                if phase is not None and ph != phase:
                    continue
                w = float("nan") if p[6] == "nan" else float(p[6])
                self.rows.append((int(p[2]), int(p[3]), int(p[4]), int(p[5]), w, int(p[7]), int(p[8])))

    def _preamble(self, line):
        kv = kv_tokens(line)
        if "model" in kv:
            self.model = kv["model"]
            self.arch = kv.get("arch", "?")
            self.n_layer = int(kv.get("n_layer", 0))
            self.n_expert = int(kv.get("n_expert", 0))
            self.n_expert_used = int(kv.get("n_expert_used", 0))
        elif "layer" in kv and "expert_bytes" in kv:
            il = int(kv["layer"])
            self.expert_bytes[il] = int(kv["expert_bytes"])
            self.dense_bytes[il] = int(kv.get("dense_bytes", 0))

    def moe_layers(self):
        return sorted({r[1] for r in self.rows})

    def steps(self):
        return sorted({r[0] for r in self.rows})

    def by_step_layer(self):
        """(step, layer) -> rows, each sorted by slot."""
        d = defaultdict(list)
        for r in self.rows:
            d[(r[0], r[1])].append(r)
        for v in d.values():
            v.sort(key=lambda r: r[2])
        return d

    def seq_by_layer(self):
        """layer -> [(step, [expert ids in slot order])], ordered by step."""
        tmp = defaultdict(lambda: defaultdict(list))
        for step, layer, slot, expert, _w, _res, _b in self.rows:
            tmp[layer][step].append((slot, expert))
        out = {}
        for layer, steps in tmp.items():
            out[layer] = [(s, [e for _sl, e in sorted(v)]) for s, v in sorted(steps.items())]
        return out


def head(title):
    print("\n=== %s ===" % title)


# ── views ───────────────────────────────────────────────────────────────────────────────
def view_matrix(tr, args):
    """The matrix the trace exists for: rows = steps, columns = layers, cells = expert ids.
    A '*' marks an id the same layer also routed on the previous step — repetition, read at
    a glance by scanning a column."""
    layers = [l for l in tr.moe_layers() if args.layer_set is None or l in args.layer_set]
    steps = [s for s in tr.steps() if args.step_set is None or s in args.step_set]
    if not layers or not steps:
        print("no rows for that layer/step selection")
        return
    if args.step_set is None:
        steps = steps[: args.max_steps]
        print("(showing the first %d steps; use --steps to pick a window)" % len(steps))

    cells = tr.by_step_layer()
    prev = {}
    head("step x layer matrix - cells: %s  ('*' = also routed at this layer on the previous step)"
         % args.cells)
    width = max(14, args.width)
    print("%6s | " % "step" + " | ".join("L%-*d" % (width - 1, l) for l in layers))
    for s in steps:
        out = []
        for l in layers:
            rows = cells.get((s, l), [])
            ids = [r[3] for r in rows]
            if args.cells == "experts":
                txt = " ".join(("*%d" % e if e in prev.get(l, ()) else str(e)) for e in ids)
            elif args.cells == "weights":
                txt = " ".join("%d:%s" % (r[3], "--" if math.isnan(r[4]) else "%.0f%%" % (100 * r[4]))
                               for r in rows)
            elif args.cells == "mb":
                txt = "%.1f" % (sum(r[6] for r in rows) / MIB)
            else:  # hit
                txt = "".join({0: "M", 1: "H", 2: "P"}[r[5]] for r in rows)
            out.append("%-*s" % (width, txt[:width]))
            prev[l] = set(ids)
        print("%6d | " % s + " | ".join(out))


def view_hot(tr, args):
    """Routing concentration per layer: if a few ids carry most activations, a warm-up can
    preload them and turn misses into hits."""
    per_layer = defaultdict(lambda: defaultdict(int))
    for _s, layer, _sl, e, _w, _r, _b in tr.rows:
        per_layer[layer][e] += 1
    head("hot experts per layer (top %d) - 'coverage' = share of this layer's activations" % args.top)
    print("%5s %7s %8s  %s" % ("layer", "unique", "coverage", "expert:activations"))
    for l in sorted(per_layer):
        counts = sorted(per_layer[l].items(), key=lambda kv: -kv[1])
        total = sum(c for _e, c in counts)
        top = counts[: args.top]
        cov = 100.0 * sum(c for _e, c in top) / total if total else 0.0
        print("%5d %7d %7.1f%%  %s" % (l, len(counts), cov,
                                       " ".join("%d:%d" % (e, c) for e, c in top)))


def view_reuse(tr, args):
    """Reuse distance: steps between two activations of the same (layer, expert). This is the
    number that picks a cache policy — short distances mean LRU works, a long tail with a few
    very hot ids means pinning the hot set beats recency."""
    seqs = tr.seq_by_layer()
    head("reuse distance per (layer, expert), in steps")
    print("%5s %8s %8s %8s %8s %9s" % ("layer", "n_reuse", "p50", "p90", "max", "never_2nd"))
    for l in sorted(seqs):
        last, dists, singles = {}, [], set()
        for idx, (_step, ids) in enumerate(seqs[l]):
            for e in ids:
                if e in last:
                    dists.append(idx - last[e])
                    singles.discard(e)
                else:
                    singles.add(e)
                last[e] = idx
        if not dists:
            print("%5d %8d %8s %8s %8s %9d" % (l, 0, "-", "-", "-", len(singles)))
            continue
        dists.sort()
        q = lambda f: dists[min(len(dists) - 1, int(f * len(dists)))]
        print("%5d %8d %8d %8d %8d %9d" % (l, len(dists), q(0.5), q(0.9), dists[-1], len(singles)))
    print("\nnever_2nd = experts this layer routed exactly once (pure cache pollution under LRU).")


def view_overlap(tr, args):
    """How much of a step's routing the previous step already knew — the ceiling on what
    temporal prefetch (which predicts from the previous token) can deliver."""
    seqs = tr.seq_by_layer()
    head("routing overlap with earlier steps (mean % of the top-k shared)")
    lags = [1, 2, 4, 8]
    print("%5s " % "layer" + " ".join("%9s" % ("lag %d" % g) for g in lags))
    agg = defaultdict(list)
    for l in sorted(seqs):
        seq = [set(ids) for _s, ids in seqs[l]]
        cells = []
        for g in lags:
            vals = [100.0 * len(seq[i] & seq[i - g]) / len(seq[i]) for i in range(g, len(seq)) if seq[i]]
            cells.append(sum(vals) / len(vals) if vals else 0.0)
            agg[g].extend(vals)
        print("%5d " % l + " ".join("%8.1f%%" % c for c in cells))
    print("%5s " % "ALL" + " ".join("%8.1f%%" % (sum(agg[g]) / len(agg[g]) if agg[g] else 0.0) for g in lags))


def view_workset(tr, args):
    """Cumulative unique experts over steps. If the curve flattens, the working set is bounded
    and preloading it is a one-off cost; if it keeps climbing, no cache size ever wins."""
    seqs = tr.seq_by_layer()
    total_steps = max((len(v) for v in seqs.values()), default=0)
    if not total_steps:
        return
    marks = [m for m in (1, 2, 5, 10, 25, 50, 100, 200, 500, 1000) if m <= total_steps]
    if total_steps not in marks:
        marks.append(total_steps)
    head("cumulative unique experts (working-set growth)")
    print("%5s %7s " % ("layer", "of") + " ".join("%7s" % ("s%d" % m) for m in marks))
    grand = []
    for l in sorted(seqs):
        seen, curve = set(), {}
        for idx, (_s, ids) in enumerate(seqs[l], start=1):
            seen.update(ids)
            curve[idx] = len(seen)
        grand.append((l, curve))
        print("%5d %7d " % (l, tr.n_expert) + " ".join("%7d" % curve.get(m, 0) for m in marks))
    tot_bytes = sum(tr.expert_bytes.get(l, 0) * c.get(marks[-1], 0) for l, c in grand)
    print("\nAt step %d the union of routed experts is %.1f MiB of expert weights across all layers."
          % (marks[-1], tot_bytes / MIB))
    print("Compare that to the cache budget: a working set that fits is a cache that stops missing.")


def view_cache(tr, args):
    """Where a routing was served from. residency is per routing; bytes are charged once per
    unique (layer, expert) per decode, which is what the streamer actually reads."""
    per = defaultdict(lambda: [0, 0, 0])
    for _s, layer, _sl, _e, _w, res, _b in tr.rows:
        per[layer][res] += 1
    head("cache outcome per layer (share of routings)")
    print("%5s %9s %9s %9s %10s" % ("layer", "miss", "hit", "prefetch", "routings"))
    tot = [0, 0, 0]
    for l in sorted(per):
        m, h, p = per[l]
        n = m + h + p
        for i, v in enumerate((m, h, p)):
            tot[i] += v
        print("%5d %8.1f%% %8.1f%% %8.1f%% %10d" % (l, 100.0 * m / n, 100.0 * h / n, 100.0 * p / n, n))
    n = sum(tot) or 1
    print("%5s %8.1f%% %8.1f%% %8.1f%% %10d" % ("ALL", 100.0 * tot[0] / n, 100.0 * tot[1] / n,
                                                100.0 * tot[2] / n, n))
    # Prefetch is only worth its bandwidth if it lands on experts that get used.
    spec = tot[2]
    if spec:
        by_slot = defaultdict(int)
        for _s, _l, slot, _e, _w, res, _b in tr.rows:
            if res == 2:
                by_slot[slot] += 1
        print("\nprefetch hits by rank slot (0 = the router's top choice): "
              + " ".join("s%d:%d" % (s, by_slot[s]) for s in sorted(by_slot)))
    else:
        print("\nno prefetch hits recorded (--prefetch off, or every guess missed).")


def view_entropy(tr, args):
    """Entropy over each routing's top-k weights. Low entropy = the top expert carries the mass,
    so speculating on it alone is cheap and usually right."""
    cells = tr.by_step_layer()
    per = defaultdict(list)
    top1 = defaultdict(list)
    for (_s, l), rows in cells.items():
        w = [r[4] for r in rows if not math.isnan(r[4])]
        if not w:
            continue
        tot = sum(w)
        if tot <= 0:
            continue
        p = [x / tot for x in w]
        per[l].append(-sum(x * math.log2(x) for x in p if x > 0))
        top1[l].append(100.0 * max(p))
    if not per:
        head("routing entropy")
        print("no weights in this trace (the graph exposed none) - nothing to measure.")
        return
    k = tr.n_expert_used or max((len(v) for v in cells.values()), default=0)
    head("routing entropy per layer (bits; max = %.2f at k=%d) and top-1 weight share"
         % (math.log2(k) if k > 1 else 0.0, k))
    print("%5s %9s %14s" % ("layer", "entropy", "top-1 share"))
    for l in sorted(per):
        print("%5d %9.2f %13.1f%%" % (l, sum(per[l]) / len(per[l]), sum(top1[l]) / len(top1[l])))


def view_bytes(tr, args):
    """Flash bytes the routing implies, split by layer and by step."""
    per_layer = defaultdict(int)
    per_step = defaultdict(int)
    for step, layer, _sl, _e, _w, _r, b in tr.rows:
        per_layer[layer] += b
        per_step[step] += b
    total = sum(per_layer.values())
    head("expert bytes read")
    print("total %.1f MiB over %d steps (%.2f MiB/step average)"
          % (total / MIB, len(per_step), total / MIB / max(1, len(per_step))))
    print("\n%5s %12s %12s %12s" % ("layer", "read MiB", "expert MiB", "dense MiB"))
    for l in sorted(per_layer):
        print("%5d %12.1f %12.2f %12.1f" % (l, per_layer[l] / MIB, tr.expert_bytes.get(l, 0) / MIB,
                                            tr.dense_bytes.get(l, 0) / MIB))
    dense_tot = sum(tr.dense_bytes.values())
    print("\ndense total %.1f MiB - static: it is mmap-resident, never streamed, so this is what a"
          % (dense_tot / MIB))
    print("cold layer costs to page in, not a per-step measurement. See docs/telemetry.md.")


def view_sanity(tr, args):
    """Cheap checks that the trace means what it says before any conclusion is drawn from it."""
    cells = tr.by_step_layer()
    head("sanity")
    k = tr.n_expert_used
    bad_k = sum(1 for rows in cells.values() if k and len(rows) != k)
    sums = [sum(r[4] for r in rows) for rows in cells.values() if not any(math.isnan(r[4]) for r in rows)]
    print("rows              %d" % len(tr.rows))
    print("cells (step,layer) %d over %d steps x %d MoE layers"
          % (len(cells), len(tr.steps()), len(tr.moe_layers())))
    print("cells not k=%-4s  %d %s" % (k, bad_k, "" if not bad_k else "<-- expected 0"))
    # The last layer routes only the tokens whose logits were wanted, so in prefill it has one
    # step where every other layer has the whole prompt. That is llama.cpp, not a gap in the
    # trace -- say so, because steps x layers then does not equal the cell count.
    steps_per_layer = defaultdict(set)
    for (s, l) in cells:
        steps_per_layer[l].add(s)
    counts = {l: len(v) for l, v in steps_per_layer.items()}
    if counts and len(set(counts.values())) > 1:
        short = sorted(counts, key=lambda l: counts[l])[0]
        print("steps per layer   uneven: layer %d has %d, the rest up to %d"
              % (short, counts[short], max(counts.values())))
        print("                  expected in prefill - llama.cpp gathers only the output token")
        print("                  before the LAST layer's FFN. See docs/telemetry.md.")
    if sums:
        lo, hi = min(sums), max(sums)
        print("weight sum/cell   min %.3f  max %.3f %s"
              % (lo, hi, "(normalized: ~1.0 expected)" if abs(hi - 1.0) < 0.05 else
                 "(this gating does not normalize - not an error)"))
    else:
        print("weight sum/cell   n/a (no weights captured)")


VIEWS = {"matrix": view_matrix, "hot": view_hot, "reuse": view_reuse, "overlap": view_overlap,
         "workset": view_workset, "cache": view_cache, "entropy": view_entropy, "bytes": view_bytes,
         "sanity": view_sanity}
DEFAULT_VIEWS = ["sanity", "hot", "reuse", "overlap", "workset", "cache", "entropy", "bytes"]


def main():
    ap = argparse.ArgumentParser(description="Analyse a bmoe --route-trace file.")
    ap.add_argument("trace")
    ap.add_argument("--view", default="default",
                    help="default, all, or one of: " + ", ".join(VIEWS))
    ap.add_argument("--phase", default="decode", choices=["decode", "prefill", "all"],
                    help="decode is the steady state and the default; prefill is one batched pass")
    ap.add_argument("--turn", type=int, default=None, help="session-mode turn (default: all)")
    ap.add_argument("--layers", default=None, help="matrix view: '0-11' or '0,5,10'")
    ap.add_argument("--steps", default=None, help="matrix view: '0-15' or '3,4,5'")
    ap.add_argument("--max-steps", type=int, default=20, help="matrix view: rows when --steps is absent")
    ap.add_argument("--cells", default="experts", choices=["experts", "weights", "mb", "hit"],
                    help="matrix view: what a cell shows")
    ap.add_argument("--width", type=int, default=22, help="matrix view: column width")
    ap.add_argument("--top", type=int, default=8, help="hot view: experts listed per layer")
    args = ap.parse_args()

    phase = None if args.phase == "all" else PHASES[args.phase]
    tr = Trace(args.trace, phase=phase, turn=args.turn)
    if not tr.rows:
        print("no rows for phase=%s turn=%s - is this a route trace?" % (args.phase, args.turn))
        return 1
    args.layer_set = parse_span(args.layers, tr.n_layer)
    args.step_set = parse_span(args.steps, 0)

    print("model   %s" % tr.model)
    print("arch    %s   n_layer %d   n_expert %d   n_expert_used %d"
          % (tr.arch, tr.n_layer, tr.n_expert, tr.n_expert_used))
    print("phase   %s   rows %d" % (args.phase, len(tr.rows)))

    names = DEFAULT_VIEWS if args.view == "default" else (
        list(VIEWS) if args.view == "all" else [args.view])
    for n in names:
        if n not in VIEWS:
            print("unknown view: %s (have: %s)" % (n, ", ".join(VIEWS)))
            return 1
        VIEWS[n](tr, args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
