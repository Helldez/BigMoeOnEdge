#!/usr/bin/env python3
# Replay a --route-trace through device expert cache policies, offline (issue #648).
#
# The device cache the engine arms is llama.cpp's moe_cache: fixed-size slots over the experts of
# every layer of a device, a ClassLRU (one LRU list per frequency class over a decayed use count,
# CRF, half-life 128 tokens; the victim is the least recently used slot of each list with the
# largest age/2^class), and no lookup at all for a batch wider than 32 tokens: a prompt's prefill
# reads its experts from the host and leaves the cache as it was. This replays the decode of a
# trace through that policy and through policies that start from a routing-frequency profile, the
# way Strata fills its GPU cache, to ask whether a profile buys hits ClassLRU does not.
#
#   classlru   llama.cpp's policy, ported line for line from src/llama-moe-cache.cpp (plan, victim)
#   lru        plain recency, for reference
#   static     the profile's hottest experts fill every slot and stay: a miss is never admitted
#   pin@F      the profile's hottest fill a share F of the slots and stay; ClassLRU runs the rest
#   warm       ClassLRU with its slots filled from the profile and its CRF started from it
#   belady     the offline optimum over the decode, the ceiling no policy passes
#
# The profile is the decode histogram of TRAINING turns; every policy is scored on the decode of
# HELD-OUT turns, so a profile is judged on prompts it never saw. Turns are split by an index
# (turn -> domain, k); a timeline is one user's run of held-out turns, the cache kept across its
# turns as a server keeps it. Stdlib only, like the other route-*.py scripts.
#
#   python scripts/device-cache-replay.py trace.csv index.json [--fracs 0.05,0.1,0.2,0.35,0.5]
import argparse
import heapq
import json
import math
import random
from bisect import bisect_right
from collections import OrderedDict, defaultdict

N_CLASS = 8
HALF_LIFE = 128.0
MAX_BATCH = 32


def load(path):
    """turn -> {"decode": [(layer, [experts]) per token per layer], "n_decode": n}; plus the shape."""
    meta, eb, cur = {}, {}, None
    cells = defaultdict(lambda: defaultdict(list))
    head = None
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                for tok in line[1:].split():
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        if k == "layer":
                            cur = int(v)
                        elif k == "expert_bytes":
                            eb[cur] = int(v)
                        else:
                            meta[k] = v
                continue
            p = line.rstrip("\n").split(",")
            if head is None:
                head = {c: i for i, c in enumerate(p)}
                continue
            if int(p[head["phase"]]) != 1:
                continue  # prefill never reaches the device cache (wider than MAX_BATCH)
            cells[int(p[head["turn"]])][(int(p[head["step"]]), int(p[head["layer"]]))].append(int(p[head["expert"]]))
    n_layer, n_expert = int(meta["n_layer"]), int(meta["n_expert"])
    turns = {}
    for t, c in cells.items():
        steps = sorted({s for s, _ in c})
        dec = []
        for s in steps:
            for il in range(n_layer):
                ex = c.get((s, il))
                if ex:
                    dec.append((il, sorted(set(ex))))
        turns[t] = {"decode": dec, "n_decode": len(steps)}
    return n_layer, n_expert, eb, turns


class ClassLRU:
    """llama.cpp's moe_cache_lru over n_slots, one group (every layer of one device)."""

    def __init__(self, n_layer, n_expert, n_slots):
        self.ne, self.ns = n_expert, n_slots
        self.key_of = [-1] * n_slots
        self.slot = {}  # key -> slot
        self.lists = [OrderedDict() for _ in range(N_CLASS)]  # per class, least recently used first
        self.list_of = [0] * n_slots
        for s in range(n_slots):
            self.lists[0][s] = None
        self.crf = defaultdict(float)
        self.last = {}
        self.clock = [0] * n_layer
        self.pinned = set()

    def decayed(self, il, key):
        return self.crf[key] * 2.0 ** (-(self.clock[il] - self.last.get(key, self.clock[il])) / HALF_LIFE)

    def use(self, il, key):
        self.crf[key] = (self.decayed(il, key) if key in self.last else 0.0) + 1.0
        self.last[key] = self.clock[il]

    def cls(self, key):
        return min(N_CLASS - 1, int(math.log2(self.crf[key]))) if self.crf[key] >= 1.0 else 0

    def move(self, s, c):
        del self.lists[self.list_of[s]][s]
        self.lists[c][s] = None
        self.list_of[s] = c

    def victim(self, il, selected):
        best, best_score = -1, -1.0
        for c in range(N_CLASS):
            for s in self.lists[c]:
                k = self.key_of[s]
                if k in self.pinned:
                    continue
                if k >= 0 and k // self.ne == il and (k % self.ne) in selected:
                    continue
                if k < 0:
                    return s
                score = math.ldexp(float(self.clock[k // self.ne] - self.last[k]), -c)
                if score > best_score:
                    best, best_score = s, score
                break
        return best

    def place(self, key, s):
        old = self.key_of[s]
        if old >= 0:
            del self.slot[old]
        self.key_of[s] = key
        self.slot[key] = s

    def plan(self, il, experts):
        """one decode token's lookups of layer il; returns hits"""
        sel = set(experts)
        hits, misses = 0, []
        for e in experts:
            k = il * self.ne + e
            self.use(il, k)
            s = self.slot.get(k)
            if s is not None:
                self.move(s, self.cls(k))
                hits += 1
            else:
                misses.append(e)
        for e in sorted(misses):
            s = self.victim(il, sel)
            if s < 0:
                continue  # every slot pinned or selected: computed from the host, not cached
            k = il * self.ne + e
            self.place(k, s)
            self.move(s, self.cls(k))
        self.clock[il] += 1
        return hits


class Static:
    """the profile's hottest experts in every slot, never replaced"""

    def __init__(self, hot):
        self.held = set(hot)

    def plan(self, il, experts, ne):
        return sum(1 for e in experts if il * ne + e in self.held)


def belady(seq_lookups, n_slots):
    """seq_lookups: [(token_index, [keys])]; hits of the offline optimum (with bypass) over n_slots"""
    occ = defaultdict(list)
    for i, (_, keys) in enumerate(seq_lookups):
        for k in keys:
            occ[k].append(i)
    inf = float("inf")

    def nxt(k, i):
        o = occ[k]
        p = bisect_right(o, i)
        return o[p] if p < len(o) else inf

    held, heap, hits = {}, [], 0  # held: key -> its next use as last pushed; heap of (-next use, key)
    for i, (_, keys) in enumerate(seq_lookups):
        ks = set(keys)
        for k in keys:
            if k in held:
                hits += 1
        for k in keys:  # every key of this step gets its next use refreshed
            n = nxt(k, i)
            if k in held:
                held[k] = n
                heapq.heappush(heap, (-n, k))
                continue
            while len(held) >= n_slots and heap:
                nn, v = heap[0]
                if v not in held or held[v] != -nn:
                    heapq.heappop(heap)  # stale
                    continue
                if v in ks or -nn <= n:
                    break
                heapq.heappop(heap)
                del held[v]
            if len(held) < n_slots:
                held[k] = n
                heapq.heappush(heap, (-n, k))
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("index", help="json list of {turn_id, domain, k}: k < train_k trains the profile")
    ap.add_argument("--train-k", type=int, default=3)
    ap.add_argument("--fracs", default="0.05,0.10,0.20,0.35,0.50")
    ap.add_argument("--pins", default="0.5,0.8")
    args = ap.parse_args()

    n_layer, n_expert, eb, turns = load(args.trace)
    idx = json.load(open(args.index))
    tids = sorted(turns)
    order = [r["turn_id"] for r in idx]
    if len(tids) != len(order):
        raise SystemExit("index has %d turns, trace %d" % (len(order), len(tids)))
    remap = dict(zip(tids, order))
    turns = {remap[t]: v for t, v in turns.items()}
    dom = {r["turn_id"]: r["domain"] for r in idx}
    kk = {r["turn_id"]: r["k"] for r in idx}
    train = [t for t in turns if kk[t] < args.train_k]
    test = [t for t in turns if kk[t] >= args.train_k]
    n_keys = n_layer * n_expert

    prof = defaultdict(int)
    for t in train:
        for il, ex in turns[t]["decode"]:
            for e in ex:
                prof[il * n_expert + e] += 1
    hot = sorted(prof, key=lambda k: -prof[k])

    domains = sorted(set(dom.values()))
    rng = random.Random(1)
    timelines = {"mono:" + d: [t for t in test if dom[t] == d] for d in domains}
    for d in domains:
        prim = [t for t in test if dom[t] == d]
        other = [t for t in test if dom[t] != d]
        seq = prim + rng.sample(other, min(3, len(other)))
        rng.shuffle(seq)
        timelines["mix:" + d] = seq
    allmix = list(test)
    rng.shuffle(allmix)
    timelines["mix:all"] = allmix

    print("model: %d layers, %d experts, %d train / %d held-out turns, %d timelines"
          % (n_layer, n_expert, len(train), len(test), len(timelines)))
    pins = [float(x) for x in args.pins.split(",") if x]
    names = ["belady", "classlru", "lru", "static", "warm"] + ["pin@%.1f" % p for p in pins]
    print("%-6s %8s " % ("cache", "slots") + " ".join("%9s" % n for n in names))
    for frac in [float(x) for x in args.fracs.split(",")]:
        n_slots = max(1, int(frac * n_keys))
        tot = defaultdict(int)
        look = 0
        for seq in timelines.values():
            caches = {"classlru": ClassLRU(n_layer, n_expert, n_slots), "warm": ClassLRU(n_layer, n_expert, n_slots)}
            for p in pins:
                c = ClassLRU(n_layer, n_expert, n_slots)
                c.pinned = set(hot[: int(p * n_slots)])
                for s, k in enumerate(sorted(c.pinned)):
                    c.place(k, s)
                caches["pin@%.1f" % p] = c
            w = caches["warm"]
            n_train_tokens = sum(turns[t]["n_decode"] for t in train) or 1
            for s, k in enumerate(hot[:n_slots]):
                w.place(k, s)
            for k in hot:
                # the profile's rate, as a CRF in steady state: uses per token over (1 - 2^(-1/half-life))
                w.crf[k] = prof[k] / n_train_tokens / (1.0 - 2.0 ** (-1.0 / HALF_LIFE))
                w.last[k] = 0
            for s in range(n_slots):
                k = w.key_of[s]
                if k >= 0:
                    w.move(s, w.cls(k))
            stat = Static(hot[:n_slots])
            lru, lru_t = OrderedDict(), 0
            seq_look = []
            for t in seq:
                for il, ex in turns[t]["decode"]:
                    keys = [il * n_expert + e for e in ex]
                    look += len(keys)
                    seq_look.append((lru_t, keys))
                    for name, c in caches.items():
                        tot[name] += c.plan(il, ex)
                    tot["static"] += stat.plan(il, ex, n_expert)
                    lru_t += 1
                    for k in keys:
                        if k in lru:
                            tot["lru"] += 1
                            lru.move_to_end(k)
                        else:
                            lru[k] = lru_t
                    while len(lru) > n_slots:
                        v = next((k for k in lru if k not in keys), None)
                        if v is None:
                            break
                        del lru[v]
            tot["belady"] += belady(seq_look, n_slots)
        print("%-6s %8d " % ("%.0f%%" % (frac * 100), n_slots) + " ".join("%8.1f%%" % (100.0 * tot[n] / look) for n in names))
    print("\n(decode hits over every held-out timeline; prefill never reaches the device cache)")


if __name__ == "__main__":
    main()
