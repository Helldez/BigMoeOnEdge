#!/usr/bin/env python3
# Replay a --route-trace through hypothetical cache policies, offline.
#
# The engine's expert cache is a global LRU over (layer, expert) entries budgeted in bytes.
# Its hit rate is measured on device, but the measurement cannot say WHY: a low hit rate is
# either a policy failure (LRU chooses badly) or a working-set failure (nothing fits, so no
# policy can do better). Those have opposite conclusions. This replays the recorded routing
# through several policies at several budgets and prints the curve that separates them:
#
#   belady   the offline optimum -- the ceiling no online policy can pass
#   lru      what the engine does today (validated against the recorded hit rate)
#   lfu      frequency instead of recency
#   random   the floor, i.e. what "no information" scores
#   layer    per-layer partitioned LRU: a layer can only evict its own entries
#
# The layer policy exists because global LRU's recency is anti-correlated with next-use
# distance on the layer axis -- the layer cycle 0..N-1 is deterministic, so the least
# recently used layer is the SOONEST to be visited again. Past a small enough budget the
# tail becomes the layer just ahead of the one staging and the cache evicts precisely what
# it is about to need. The sweep is what shows whether that edge is near the operating point.
#
# Stdlib only (like route-analyze.py / bench-analyze.py): no pandas, nothing to install.
#
#   python scripts/route-replay.py trace.csv --validate 3000
#   python scripts/route-replay.py trace.csv --budgets 500,1000,2000,3000,4000
#   python scripts/route-replay.py trace.csv --budgets 1000,2000 --csv out.csv
import argparse
import importlib.util
import os
import random
import sys
from bisect import bisect_right
from collections import defaultdict


def _load_trace_class():
    """route-analyze.py has a hyphen, so it cannot be imported by name."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "route-analyze.py")
    spec = importlib.util.spec_from_file_location("route_analyze", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["route_analyze"] = mod
    spec.loader.exec_module(mod)
    return mod.Trace


Trace = _load_trace_class()

MIB = 1024.0 * 1024.0
POLICIES = ("belady", "lru", "lfu", "random", "layer", "layerlfu")


# ── batches ─────────────────────────────────────────────────────────────────────────────
def batches_from_rows(rows, layers):
    """The load_layer call sequence the engine actually makes, as [(layer, [expert ids])].

    One batch == one load_layer call, because that is the unit the cache accounts in:
    inside a batch each unique expert is looked up once (duplicates only promote), and
    eviction runs once at the end, protecting the batch's own entries.

    Prefill is ONE batched pass: every prompt token is evaluated together, so a layer sees
    all tokens' ids in a single call, token-major (that ordering is load-bearing -- it
    leaves the prompt tail's experts hottest). Decode is one call per layer per token.

    rows: [(phase, step, layer, slot, expert)] -> ordered [(layer, [ids])].
    """
    pre = defaultdict(list)
    dec = defaultdict(list)
    for phase, step, layer, slot, expert in rows:
        if phase == 0:
            pre[layer].append((step, slot, expert))
        else:
            dec[(step, layer)].append((slot, expert))

    out = []
    for il in layers:  # prefill: one pass over the layers, all tokens at once
        if il in pre:
            ids = [e for _s, _sl, e in sorted(pre[il])]
            out.append((il, ids))
    for step in sorted({s for s, _l in dec}):  # decode: layer-major within each token
        for il in layers:
            if (step, il) in dec:
                out.append((il, [e for _sl, e in sorted(dec[(step, il)])]))
    return out


def read_rows(path, turn=None):
    """Parse the trace keeping the phase column, which Trace drops into its filter."""
    rows = []
    with open(path, "r") as f:
        for line in f:
            if line.startswith("#") or line.startswith("turn,"):
                continue
            p = line.rstrip("\n").split(",")
            if len(p) < 9:
                continue
            t = int(p[0])
            if turn is not None and t != turn:
                continue
            rows.append((int(p[1]), int(p[2]), int(p[3]), int(p[4]), int(p[5])))
    return rows


# ── the simulator ───────────────────────────────────────────────────────────────────────
class Sim:
    """Byte-budgeted cache over (layer, expert) keys, replaying load_layer batches.

    Mirrors the engine's accounting so the LRU row can be validated against the recorded
    hit rate before any other row is trusted:
      - one lookup per unique expert per batch (duplicates promote, they do not count)
      - a miss makes the entry resident at entry_bytes(layer)
      - eviction runs after the batch and never evicts an entry the current batch touched
    """

    def __init__(self, policy, budget_bytes, expert_bytes, n_expert, n_layer, seed=1234):
        self.policy = policy
        self.budget = budget_bytes
        self.ebytes = expert_bytes
        self.n_expert = n_expert
        self.n_layer = n_layer
        self.rng = random.Random(seed)

        self.resident = {}            # id -> bytes
        self.bytes_used = 0
        self.order = {}               # id -> recency stamp (lru/layer) or freq (lfu)
        self.freq = defaultdict(int)  # id -> touches so far (lfu)
        self.clock = 0
        self.lookups = 0
        self.hits = 0
        self.read_bytes = 0
        self.dec_lookups = 0
        self.dec_hits = 0

        # per-layer partition: each layer gets an equal slice of the budget
        self.part = budget_bytes // max(1, n_layer)
        self.layer_bytes = defaultdict(int)

        # belady lookahead, filled by run()
        self.occ = None
        self.batch_idx = 0

    def key(self, il, e):
        return il * self.n_expert + e

    def cost(self, il):
        return self.ebytes.get(il, 0)

    # -- eviction victims ---------------------------------------------------------------
    def victim(self, protected, il_cur):
        """Pick an id to evict, or None if nothing evictable remains."""
        pool = [i for i in self.resident if i not in protected]
        if self.policy in ("layer", "layerlfu"):
            # only the staging layer's own entries are candidates -- that is the whole point
            pool = [i for i in pool if i // self.n_expert == il_cur]
        if not pool:
            return None
        if self.policy == "random":
            return self.rng.choice(pool)
        if self.policy in ("lfu", "layerlfu"):
            return min(pool, key=lambda i: (self.freq[i], self.order.get(i, 0)))
        if self.policy == "belady":
            # furthest next use wins; never-again is infinitely far
            return max(pool, key=lambda i: self.next_use(i))
        return min(pool, key=lambda i: self.order.get(i, 0))  # lru / layer

    def next_use(self, i):
        occ = self.occ.get(i)
        if not occ:
            return float("inf")
        p = bisect_right(occ, self.batch_idx)
        return occ[p] if p < len(occ) else float("inf")

    def evict_to_budget(self, protected, il_cur):
        if self.policy in ("layer", "layerlfu"):
            while self.layer_bytes[il_cur] > self.part:
                v = self.victim(protected, il_cur)
                if v is None:
                    return
                self.drop(v)
            return
        while self.bytes_used > self.budget:
            v = self.victim(protected, il_cur)
            if v is None:
                return
            self.drop(v)

    def drop(self, i):
        b = self.resident.pop(i)
        self.bytes_used -= b
        self.layer_bytes[i // self.n_expert] -= b
        self.order.pop(i, None)

    # -- one load_layer call ------------------------------------------------------------
    def batch(self, il, ids, decode):
        seen = set()
        touched = set()
        for e in ids:
            i = self.key(il, e)
            self.clock += 1
            if i in seen:
                self.order[i] = self.clock  # duplicate: promote only, no lookup counted
                continue
            seen.add(i)
            touched.add(i)
            self.lookups += 1
            if decode:
                self.dec_lookups += 1
            if i in self.resident:
                self.hits += 1
                if decode:
                    self.dec_hits += 1
            else:
                c = self.cost(il)
                self.resident[i] = c
                self.bytes_used += c
                self.layer_bytes[il] += c
                self.read_bytes += c
            self.freq[i] += 1
            self.order[i] = self.clock
        self.evict_to_budget(touched, il)

    def run(self, batches, n_prefill):
        if self.policy == "belady":
            self.occ = defaultdict(list)
            for bi, (il, ids) in enumerate(batches):
                for e in set(ids):
                    self.occ[self.key(il, e)].append(bi)
        for bi, (il, ids) in enumerate(batches):
            self.batch_idx = bi
            self.batch(il, ids, decode=bi >= n_prefill)
        return self

    def hit_pct(self):
        return 100.0 * self.hits / self.lookups if self.lookups else 0.0

    def dec_hit_pct(self):
        return 100.0 * self.dec_hits / self.dec_lookups if self.dec_lookups else 0.0


# ── driver ──────────────────────────────────────────────────────────────────────────────
def run_one(batches, n_prefill, tr, policy, budget_mib):
    sim = Sim(policy, int(budget_mib * MIB), tr.expert_bytes, tr.n_expert, tr.n_layer)
    return sim.run(batches, n_prefill)


def main():
    ap = argparse.ArgumentParser(description="replay a route trace through cache policies")
    ap.add_argument("trace")
    ap.add_argument("--budgets", default="", help="MiB, comma separated (default: a sweep)")
    ap.add_argument("--policies", default=",".join(POLICIES))
    ap.add_argument("--validate", type=float, default=None,
                    help="budget MiB whose LRU row must reproduce the recorded hit rate")
    ap.add_argument("--turn", type=int, default=None)
    ap.add_argument("--csv", default=None, help="also write the curve here")
    args = ap.parse_args()

    tr = Trace(args.trace)  # preamble only: expert_bytes, shape
    rows = read_rows(args.trace, args.turn)
    layers = sorted({r[2] for r in rows})
    batches = batches_from_rows(rows, layers)
    prefill_layers = {r[2] for r in rows if r[0] == 0}
    n_prefill = len([il for il in layers if il in prefill_layers])

    eb = next(iter(tr.expert_bytes.values())) if tr.expert_bytes else 0
    cycle = sum(tr.expert_bytes.get(il, 0) for il in layers) * tr.n_expert_used
    print("model   %s" % os.path.basename(tr.model))
    print("shape   %d layers (%d MoE), %d experts, top-%d, %.2f MiB/expert"
          % (tr.n_layer, len(layers), tr.n_expert, tr.n_expert_used, eb / MIB))
    print("batches %d (%d prefill + %d decode)" % (len(batches), n_prefill, len(batches) - n_prefill))
    print("T       %.0f MiB touched per token cycle (all layers x top-k, worst case)" % (cycle / MIB))

    policies = [p for p in args.policies.split(",") if p]
    if args.validate is not None:
        sim = run_one(batches, n_prefill, tr, "lru", args.validate)
        print("\nvalidation @ %.0f MiB: LRU replay = %.1f%% cumulative (%.1f%% decode-only)"
              % (args.validate, sim.hit_pct(), sim.dec_hit_pct()))
        print("compare against cache_hit_pct in the run's .metrics summary")

    budgets = [float(b) for b in args.budgets.split(",") if b] or \
        [250, 500, 750, 1000, 1500, 2000, 3000, 4000, 6000]

    print("\n%-8s %s" % ("budget", " ".join("%9s" % p for p in policies)))
    print("%-8s %s" % ("(MiB)", " ".join("%9s" % "hit%" for _ in policies)))
    out = []
    for b in budgets:
        cells = []
        for p in policies:
            sim = run_one(batches, n_prefill, tr, p, b)
            cells.append(sim.dec_hit_pct())
            out.append((b, p, sim.hit_pct(), sim.dec_hit_pct(), sim.read_bytes / MIB))
        print("%-8.0f %s" % (b, " ".join("%9.1f" % c for c in cells)))
    print("\n(decode-only hit%; C=T boundary is where a budget stops holding one token cycle)")

    if args.csv:
        with open(args.csv, "w") as f:
            f.write("budget_mib,policy,hit_pct_cum,hit_pct_decode,read_mib\n")
            for r in out:
                f.write("%.0f,%s,%.3f,%.3f,%.1f\n" % r)
        print("wrote %s" % args.csv)


if __name__ == "__main__":
    main()
