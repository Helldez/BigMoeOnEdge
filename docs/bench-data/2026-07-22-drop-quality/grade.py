#!/usr/bin/env python3
"""Grade the drop-threshold quality A/B.

Reads the BMOE_* session transcripts (one per threshold), pulls each question's final answer
text out of the last BMOE_PROGRESS line for that id, and scores it against the GSM8K key.

Grading rule, fixed before looking at any output: take the number following the last '####' in
the reply; if the model never emitted the marker, fall back to the last number in the reply.
That fallback is generous to every cell equally -- it is a formatting allowance, not a
correctness one.
"""
import json, re, sys, glob, os

KEY = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "key.json")))

NUM = re.compile(r"-?\d[\d,]*(?:\.\d+)?")


def norm(s):
    s = s.replace(",", "").rstrip(".")
    try:
        f = float(s)
        return int(f) if f == int(f) else f
    except ValueError:
        return None


def extract(text):
    """(answer, used_marker). Mirrors the documented GSM8K convention."""
    if "####" in text:
        tail = text.rsplit("####", 1)[1]
        m = NUM.search(tail)
        if m:
            return norm(m.group()), True
    nums = NUM.findall(text)
    return (norm(nums[-1]), False) if nums else (None, False)


def parse_transcript(path):
    """id -> final cumulative text. The last BMOE_PROGRESS of a generation carries the whole reply."""
    out, cur_id = {}, None
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("BMOE_BEGIN"):
            m = re.search(r'"id":(\d+)', line)
            cur_id = int(m.group(1)) if m else None
        elif line.startswith("BMOE_PROGRESS") and cur_id is not None:
            m = re.search(r'"text":"(.*)"\}\s*$', line)
            if m:
                out[cur_id] = m.group(1)
    return out


def unescape(s):
    return (s.replace("\\n", "\n").replace("\\t", "\t").replace('\\"', '"').replace("\\\\", "\\"))


def main(paths):
    print(f"{'cell':>6} {'correct':>9} {'accuracy':>9} {'no #### marker':>15} {'empty':>6}")
    detail = {}
    for p in paths:
        cell = os.path.basename(p).replace("out-", "").replace(".txt", "")
        texts = parse_transcript(p)
        ok = nomark = empty = 0
        per = {}
        for qid, want in KEY.items():
            t = unescape(texts.get(int(qid), ""))
            if not t.strip():
                empty += 1
                per[qid] = None
                continue
            got, marked = extract(t)
            if not marked:
                nomark += 1
            per[qid] = got
            if got is not None and norm(str(want)) == got:
                ok += 1
        n = len(KEY)
        detail[cell] = per
        print(f"{cell:>6} {ok:>4}/{n:<4} {100.0*ok/n:8.1f}% {nomark:>15} {empty:>6}")

    print("\nper-question answers (expected in brackets):")
    cells = [os.path.basename(p).replace("out-", "").replace(".txt", "") for p in paths]
    print("  qid  expected  " + "  ".join(f"{c:>8}" for c in cells))
    for qid in sorted(KEY, key=int):
        row = "  ".join(f"{str(detail[c].get(qid)):>8}" for c in cells)
        print(f"  {qid:>3}  {str(KEY[qid]):>8}  {row}")


if __name__ == "__main__":
    args = sys.argv[1:] or sorted(glob.glob("out-*.txt"))
    if not args:
        print("usage: grade.py out-*.txt")
        sys.exit(2)
    order = {"off": 0, "0.5": 1, "0.75": 2, "1.0": 3}
    args.sort(key=lambda p: order.get(os.path.basename(p).replace("out-", "").replace(".txt", ""), 9))
    main(args)
