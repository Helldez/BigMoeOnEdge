#!/usr/bin/env python3
"""Shared reading for the analysis scripts. Stdlib only, like everything else here.

Every artifact the engine writes — the per-token CSV, the route trace, the decode/IO traces, the
device .metrics file — carries its context as `key=value` tokens on `#` lines, with the rows below.
That contract is one thing, and it was being re-implemented per script (decode-analyze.py's reader
said as much: "Mirrors scripts/route-analyze.py's reader"). It lives here now.

The contract is deliberately forgiving in one direction: unknown keys are kept and missing keys are
absent rather than an error, so a file from an older or newer engine still reads.
"""
import csv
import os


def kv_tokens(line):
    """`# a=1 b=2` -> {"a": "1", "b": "2"}. Non-`key=value` tokens are skipped."""
    return dict(tok.split("=", 1) for tok in line.lstrip("#").split() if "=" in tok)


def read_preamble_csv(path):
    """Split a `#`-preamble CSV into (meta, rows): meta merges every `#` line's key=value tokens,
    rows are dicts keyed by the header. Use when the rows are wanted by column NAME."""
    meta, body = {}, []
    with open(path, newline="", encoding="utf-8") as f:
        for ln in f:
            if ln.startswith("#"):
                meta.update(kv_tokens(ln.rstrip()))
            else:
                body.append(ln)
    return meta, list(csv.DictReader(body))


def read_kv_file(path):
    """A flat `key=value` per line file (the device .metrics sidecar). {} when absent."""
    d = {}
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                if "=" in line:
                    k, v = line.strip().split("=", 1)
                    d[k] = v
    return d


def percentile(values, q):
    """Linear-interpolated percentile of an ALREADY SORTED list; 0.0 when empty."""
    if not values:
        return 0.0
    i = q * (len(values) - 1)
    lo = int(i)
    hi = min(lo + 1, len(values) - 1)
    return values[lo] + (values[hi] - values[lo]) * (i - lo)
