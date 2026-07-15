#!/usr/bin/env python3
"""Read a --compute-trace / --io-trace pair without a spreadsheet.

The per-token CSV says how long a token took and calls the leftover "compute". These traces say
what that leftover is actually made of, and what the flash floor under it really is. Stdlib only.

  decode-analyze.py compute ct.csv          # where compute goes, by op / by layer / faults
  decode-analyze.py io io.csv               # the flash floor: latency, size, waste, lanes
  decode-analyze.py io io.csv --adjacent    # how much of a token's reads could coalesce

A traced run is not a benchmark run: isolating every node forbids ggml the operator coalescing it
would normally do, and the I/O rows take a lock per read. Read the proportions, not the absolutes.
"""
import argparse
import csv
import sys
from collections import defaultdict


def read_trace(path):
    """Split the `#` preamble from the rows. Mirrors scripts/route-analyze.py's reader."""
    meta, rows = {}, []
    with open(path, newline="", encoding="utf-8") as f:
        lines = f.readlines()
    body = []
    for ln in lines:
        if ln.startswith("#"):
            for tok in ln.lstrip("# ").rstrip().split():
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    meta[k] = v
        else:
            body.append(ln)
    rows = list(csv.DictReader(body))
    return meta, rows


def fmt_ms(ns):
    return f"{ns / 1e6:9.2f}"


def bar(frac, width=28):
    n = int(round(frac * width))
    return "#" * n + "." * (width - n)


def decode_only(rows):
    """Steady-state decode: phase 1. Prefill is one batch of many tokens, a different regime."""
    d = [r for r in rows if r["phase"] == "1"]
    return d if d else rows


def cmd_compute(args):
    meta, rows = read_trace(args.path)
    rows = decode_only(rows)
    if not rows:
        sys.exit("no rows")
    steps = sorted({int(r["step"]) for r in rows})
    n_steps = len(steps)
    total = sum(int(r["wall_ns"]) for r in rows)
    faults = sum(int(r["majflt"]) for r in rows)

    print(f"model={meta.get('model','?')} arch={meta.get('arch','?')} "
          f"n_layer={meta.get('n_layer','?')} threads={meta.get('n_threads','?')}")
    print(f"decode steps={n_steps}  nodes/step={len(rows)//max(1,n_steps)}  "
          f"measured compute={fmt_ms(total/max(1,n_steps))} ms/token  majflt={faults/max(1,n_steps):.0f}/token")
    print("\nNOTE: measured, not a residual - each node was isolated and synchronized. The barrier\n"
          "      that makes it measurable also inflates it; compare shares, not absolutes.\n")

    # ── by op ──
    by_op = defaultdict(lambda: [0, 0, 0])  # ns, count, majflt
    for r in rows:
        e = by_op[r["op"]]
        e[0] += int(r["wall_ns"])
        e[1] += 1
        e[2] += int(r["majflt"])
    print(f"{'op':<18}{'ms/token':>10}{'share':>8}  {'majflt/tok':>10}  {'':<28}")
    for op, (ns, cnt, mf) in sorted(by_op.items(), key=lambda kv: -kv[1][0])[: args.top]:
        share = ns / total if total else 0
        print(f"{op:<18}{fmt_ms(ns/max(1,n_steps))}{share*100:7.1f}%  {mf/max(1,n_steps):10.1f}  {bar(share)}")

    # ── faults: the question the residual could never answer ──
    if faults:
        print("\nfault attribution - where the >RAM stall is billed as compute")
        by_fault = defaultdict(lambda: [0, 0])
        for r in rows:
            mf = int(r["majflt"])
            if not mf:
                continue
            e = by_fault[r["op"]]
            e[0] += mf
            e[1] += int(r["wall_ns"])
        for op, (mf, ns) in sorted(by_fault.items(), key=lambda kv: -kv[1][0])[:8]:
            print(f"  {op:<16}{mf/max(1,n_steps):9.1f} majflt/tok  in {fmt_ms(ns/max(1,n_steps))} ms/tok "
                  f"({ns/total*100:4.1f}% of compute)")
        faulting = sum(v[1] for v in by_fault.values())
        print(f"  {'TOTAL':<16}{faults/max(1,n_steps):9.1f} majflt/tok  in {fmt_ms(faulting/max(1,n_steps))} ms/tok "
              f"({faulting/total*100:4.1f}% of compute)")
        print("  ^ nodes that faulted. Their time is flash wait, not arithmetic - subtract it before\n"
              "    calling this model compute-bound.")

    # ── by layer ──
    if args.layers:
        by_layer = defaultdict(int)
        for r in rows:
            by_layer[int(r["layer"])] += int(r["wall_ns"])
        print("\nby layer (-1 = no layer: embeddings, output head, masks)")
        mx = max(by_layer.values()) or 1
        for il in sorted(by_layer):
            ns = by_layer[il]
            print(f"  layer {il:>3} {fmt_ms(ns/max(1,n_steps))} ms/tok  {bar(ns/mx)}")


def cmd_io(args):
    meta, rows = read_trace(args.path)
    rows = decode_only(rows)
    if not rows:
        sys.exit("no rows")
    steps = sorted({int(r["step"]) for r in rows})
    n_steps = len(steps)
    lat = [int(r["latency_ns"]) for r in rows]
    got = sum(int(r["read_bytes"]) for r in rows)
    want = sum(int(r["req_bytes"]) for r in rows)
    busy = sum(lat)

    print(f"model={meta.get('model','?')} io_threads={meta.get('io_threads','?')} "
          f"o_direct={meta.get('o_direct','?')} overlap={meta.get('overlap','?')}")
    print(f"decode steps={n_steps}  reads={len(rows)} ({len(rows)/max(1,n_steps):.0f}/token)  "
          f"read={got/2**20/max(1,n_steps):.1f} MiB/token")
    # Per-lane time is what the drive was actually asked to do; wall is hidden by overlap.
    print(f"lane-busy bandwidth={got/2**20/(busy/1e9):.0f} MiB/s aggregated over lanes "
          f"({busy/1e9/max(1,n_steps)*1000:.0f} ms lane-busy/token)")
    waste = (got - want) / got * 100 if got else 0
    print(f"alignment waste={waste:.1f}% ({(got-want)/2**20/max(1,n_steps):.2f} MiB/token read but not wanted)")

    # ── latency distribution: is the floor seek-bound or size-bound? ──
    lat.sort()
    def pct(p):
        return lat[min(len(lat) - 1, int(len(lat) * p))] / 1e3
    print("\nper-read latency (us)")
    for p in (0.5, 0.9, 0.99):
        print(f"  p{int(p*100):<3} {pct(p):9.1f}")
    print(f"  max  {lat[-1]/1e3:9.1f}")

    # ── size vs bandwidth: the coalescing case, in one table ──
    print("\nby request size - the per-read cost of scattering")
    buckets = defaultdict(lambda: [0, 0, 0])  # count, bytes, ns
    for r in rows:
        kb = int(r["read_bytes"]) // 1024
        b = 1 << (kb.bit_length() - 1) if kb else 0
        e = buckets[b]
        e[0] += 1
        e[1] += int(r["read_bytes"])
        e[2] += int(r["latency_ns"])
    print(f"  {'size':>8}{'reads':>9}{'MiB':>9}{'MiB/s':>9}{'us/read':>9}")
    for b in sorted(buckets):
        cnt, by, ns = buckets[b]
        print(f"  {b:>6} K{cnt:>9}{by/2**20:9.1f}{by/2**20/(ns/1e9):9.0f}{ns/cnt/1e3:9.1f}")

    # ── lanes ──
    by_lane = defaultdict(lambda: [0, 0])
    for r in rows:
        e = by_lane[int(r["lane"])]
        e[0] += int(r["read_bytes"])
        e[1] += int(r["latency_ns"])
    print("\nby lane - uneven busy time means a lane is starved, not that the drive is full")
    for ln in sorted(by_lane):
        by, ns = by_lane[ln]
        print(f"  lane {ln}: {by/2**20/max(1,n_steps):7.1f} MiB/tok  busy {ns/1e9/max(1,n_steps)*1000:7.1f} ms/tok"
              f"  {by/2**20/(ns/1e9):6.0f} MiB/s")

    spec = [r for r in rows if r["spec"] == "1"]
    if spec:
        sb = sum(int(r["read_bytes"]) for r in spec)
        print(f"\nspeculative: {len(spec)} reads, {sb/2**20/max(1,n_steps):.1f} MiB/token "
              f"({sb/got*100:.0f}% of bytes read)")

    if args.adjacent:
        # How much of a step's reads are back-to-back in the file? That is the ceiling on what an
        # expert-contiguous layout / runtime coalescing could merge — the roadmap's read-bandwidth item.
        print("\nadjacency - the coalescing ceiling")
        merged_tot = runs_tot = 0
        for st in steps:
            ext = sorted((int(r["offset"]), int(r["offset"]) + int(r["read_bytes"]))
                         for r in rows if int(r["step"]) == st)
            if not ext:
                continue
            runs, cur_end = 1, ext[0][1]
            for a, b in ext[1:]:
                if a > cur_end:
                    runs += 1
                cur_end = max(cur_end, b)
            merged_tot += len(ext)
            runs_tot += runs
        if merged_tot:
            print(f"  {merged_tot/max(1,n_steps):.0f} reads/token span {runs_tot/max(1,n_steps):.0f} "
                  f"contiguous runs/token")
            print(f"  perfectly coalesced, that is {merged_tot/max(1,runs_tot):.1f}x fewer requests "
                  f"for the same bytes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("compute", help="what the compute residual is actually made of")
    c.add_argument("path")
    c.add_argument("--top", type=int, default=12, help="how many ops to list")
    c.add_argument("--layers", action="store_true", help="also break down by layer")
    c.set_defaults(fn=cmd_compute)

    i = sub.add_parser("io", help="the flash floor: latency, size, waste, lanes")
    i.add_argument("path")
    i.add_argument("--adjacent", action="store_true", help="estimate the coalescing ceiling")
    i.set_defaults(fn=cmd_io)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
