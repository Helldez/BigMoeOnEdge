#!/usr/bin/env python3
"""Inspect a freshly published MoE release and answer the two questions that decide whether
this engine can stream it: does its expert layout reduce to one registry row, and what else
in it is large enough to be worth streaming?

A new architecture usually lands on Hugging Face hours before llama.cpp can convert it, so
this reads the *source* repository rather than a gguf: config.json plus the safetensors
index, then each shard's header over an HTTP Range request. A safetensors header is an
8-byte length prefix followed by that many JSON bytes, so a few kilobytes per shard yield
the exact shape and dtype of every tensor without downloading a single weight.

What it prints:

  * the routing shape (experts, top-k, shared experts, leading dense blocks, MTP)
  * the tensor inventory, layer and expert indices collapsed so per-layer tensors group
  * the expert layout verdict -- split {gate, up, down}, fused {gate_up, down}, or
    unrecognised -- with the arch_registry.cpp row it implies
  * the largest non-expert tensor groups, which is where a novel parameter block (an
    n-gram embedding table, say) shows up as a streaming candidate that the dense path
    would otherwise map resident in full

The gguf general.architecture string is chosen by the llama.cpp converter, not by
config.json, so a suggested row names model_type as a placeholder and must be confirmed
against the converted model. Use --gguf once one exists.

Usage:
    python scripts/inspect-hf-moe-release.py --repo Qwen/Qwen3.8-Flash-Next
    python scripts/inspect-hf-moe-release.py --dir /path/to/local/checkout
    python scripts/inspect-hf-moe-release.py --gguf model-00001-of-00009.gguf
"""

import argparse
import json
import os
import re
import struct
import sys
import urllib.error
import urllib.request

HF_ENDPOINT = os.environ.get("HF_ENDPOINT", "https://huggingface.co")

# Bytes per element, by safetensors dtype name. An unknown dtype is reported rather than
# guessed: a wrong width silently misprices the exact thing this script exists to size.
DTYPE_BYTES = {
    "F64": 8, "I64": 8,
    "F32": 4, "I32": 4, "U32": 4,
    "F16": 2, "BF16": 2, "I16": 2, "U16": 2,
    "F8_E4M3": 1, "F8_E5M2": 1, "I8": 1, "U8": 1, "BOOL": 1,
}

# Hugging Face projection name -> the gguf tensor suffix llama.cpp's converter emits for it.
PROJECTIONS = ("gate_proj", "up_proj", "down_proj", "gate_up_proj")


def die(msg):
    print("error: " + msg, file=sys.stderr)
    sys.exit(1)


def http_get(url, byte_range=None):
    req = urllib.request.Request(url)
    if byte_range is not None:
        req.add_header("Range", "bytes=%d-%d" % byte_range)
    token = os.environ.get("HF_TOKEN")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        die("HTTP %d for %s" % (e.code, url))
    except urllib.error.URLError as e:
        die("cannot reach %s (%s)" % (url, e.reason))


class Source:
    """A Hugging Face repo or a local directory, behind one read interface."""

    def __init__(self, repo=None, directory=None):
        self.repo = repo
        self.dir = directory

    def url_for(self, name):
        return "%s/%s/resolve/main/%s" % (HF_ENDPOINT, self.repo, name)

    def json_file(self, name, required=True):
        if self.dir:
            path = os.path.join(self.dir, name)
            if not os.path.exists(path):
                if required:
                    die("missing %s in %s" % (name, self.dir))
                return None
            with open(path, "r", encoding="utf-8") as f:
                return json.load(f)
        if not required:
            try:
                urllib.request.urlopen(
                    urllib.request.Request(self.url_for(name), method="HEAD"), timeout=30)
            except Exception:
                return None
        return json.loads(http_get(self.url_for(name)))

    def safetensors_header(self, name):
        """Read one shard's header: an 8-byte little-endian length, then that many JSON bytes."""
        if self.dir:
            with open(os.path.join(self.dir, name), "rb") as f:
                (n,) = struct.unpack("<Q", f.read(8))
                return json.loads(f.read(n))
        url = self.url_for(name)
        (n,) = struct.unpack("<Q", http_get(url, (0, 7)))
        return json.loads(http_get(url, (8, 8 + n - 1)))


def collapse(name):
    """Collapse layer and expert indices so per-layer tensors fall into one pattern."""
    name = re.sub(r"\.layers\.\d+\.", ".layers.<il>.", name)
    name = re.sub(r"\.experts\.\d+\.", ".experts.<ie>.", name)
    # Any surviving bare index -- some stacks number sub-blocks their own way -- also collapses.
    return re.sub(r"\.\d+\.", ".<i>.", name)


def tensor_bytes(info):
    dtype = info.get("dtype", "?")
    if dtype not in DTYPE_BYTES:
        return None
    n = 1
    for d in info.get("shape", []):
        n *= d
    return n * DTYPE_BYTES[dtype]


def human(nbytes):
    if nbytes is None:
        return "?"
    v = float(nbytes)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if v < 1024.0 or unit == "TiB":
            return "%.2f %s" % (v, unit)
        v /= 1024.0


def routing_shape(cfg):
    """Pull the routing numbers out of config.json, tolerating naming drift across families."""
    scopes = [cfg, cfg.get("text_config") or {}]

    def first(*keys):
        for key in keys:
            for scope in scopes:
                if key in scope:
                    return scope[key]
        return None

    return [
        ("architectures", cfg.get("architectures")),
        ("model_type", cfg.get("model_type")),
        ("hidden layers", first("num_hidden_layers")),
        ("routed experts", first("num_experts", "n_routed_experts", "moe_num_experts")),
        ("experts per token", first("num_experts_per_tok", "moe_topk", "moe_k")),
        ("shared experts", first("n_shared_experts", "shared_expert_intermediate_size",
                                 "moe_shared_expert_intermediate_size")),
        ("leading dense blocks", first("first_k_dense_replace", "leading_dense_block_count")),
        ("MoE every N layers", first("decoder_sparse_step")),
        ("dense-only layers", first("mlp_only_layers")),
        ("MTP / nextn layers", first("num_nextn_predict_layers", "nextn_predict_layers")),
    ]


def print_group_table(groups, names, width=58):
    for name in names:
        g = groups[name]
        print("  %-*s x%-5d %10s  %s%s" %
              (width, name, g["count"], human(g["bytes"]), g["dtype"], g["shape"]))


def report(cfg, groups):
    """groups: collapsed name -> {count, bytes, shape, dtype}."""
    print("== routing shape " + "=" * 46)
    for label, value in routing_shape(cfg):
        if value is not None:
            print("  %-22s %s" % (label, value))

    expert_groups = {k: v for k, v in groups.items() if "experts" in k}
    other_groups = {k: v for k, v in groups.items() if "experts" not in k}

    print()
    print("== expert tensors " + "=" * 45)
    if expert_groups:
        print_group_table(groups, sorted(expert_groups))
    else:
        print("  none.")

    print()
    print("== verdict " + "=" * 52)
    present = {p for p in PROJECTIONS
               for k in expert_groups
               if k.endswith(p + ".weight") or ("." + p + ".") in k}
    arch = cfg.get("model_type") or "<arch>"
    row = None
    if not expert_groups:
        # No expert tensors at all is a different answer from an expert layout we failed to
        # parse: a dense model is not a candidate this engine can do anything with, whereas an
        # unparsed layout is a recipe waiting to be written.
        print("  Dense model: no tensor in it is indexed by expert. There is nothing for the")
        print("  streamer to bind, so no registry row applies and the whole model would load")
        print("  resident. If you expected experts here, check that this is the MoE variant.")
    elif {"gate_proj", "up_proj", "down_proj"} <= present:
        row = '{"%s", {"ffn_gate_exps", "ffn_up_exps", "ffn_down_exps"}},' % arch
        print("  Split expert layout. One registry row:")
    elif {"gate_up_proj", "down_proj"} <= present:
        row = '{"%s", {"ffn_gate_up_exps", "ffn_down_exps", nullptr}},' % arch
        print("  Fused gate+up layout. One registry row:")
    else:
        print("  Unrecognised expert projection set: %s" % (sorted(present) or "none"))
        print("  Read the converter before writing a row: the streamer binds exactly the")
        print("  per-layer expert tensors a recipe names, and nothing else.")
    if row:
        print()
        print("      " + row)
        print()
        print("  That arch string is config.json's model_type, a placeholder. The gguf")
        print("  general.architecture is the converter's choice -- confirm it with --gguf.")

    print()
    print("== largest non-expert tensor groups " + "=" * 28)
    print("  These stay resident on the dense path. Anything large and sparsely touched")
    print("  here is a streaming candidate, not a given.")
    ranked = sorted(other_groups, key=lambda k: other_groups[k]["bytes"] or 0, reverse=True)
    print_group_table(groups, ranked[:12])

    total = sum(g["bytes"] or 0 for g in groups.values())
    expert_total = sum(g["bytes"] or 0 for g in expert_groups.values())
    pct = (lambda part: 100.0 * part / total if total else 0.0)
    print()
    print("== totals " + "=" * 53)
    print("  all tensors      %12s" % human(total))
    print("  expert tensors   %12s  (%.1f %%)" % (human(expert_total), pct(expert_total)))
    print("  everything else  %12s  (%.1f %%)" % (human(total - expert_total),
                                                  pct(total - expert_total)))
    print()
    print("  The second line is what expert streaming addresses today; the third bounds")
    print("  what any resident-side change could ever reach.")


def add_tensor(groups, key, nbytes, shape, dtype):
    g = groups.setdefault(key, {"count": 0, "bytes": 0, "shape": None, "dtype": "?"})
    g["count"] += 1
    g["bytes"] = None if (nbytes is None or g["bytes"] is None) else g["bytes"] + nbytes
    g["shape"] = shape
    g["dtype"] = dtype


def from_source(source):
    cfg = source.json_file("config.json")
    index = source.json_file("model.safetensors.index.json", required=False)
    shards = sorted(set(index["weight_map"].values())) if index else ["model.safetensors"]

    groups = {}
    for shard in shards:
        print("reading header: %s" % shard, file=sys.stderr)
        for name, info in source.safetensors_header(shard).items():
            if name == "__metadata__":
                continue
            add_tensor(groups, collapse(name), tensor_bytes(info),
                       info.get("shape"), info.get("dtype", "?"))
    print(file=sys.stderr)
    report(cfg, groups)


def from_gguf(path):
    try:
        from gguf import GGUFReader
    except ImportError:
        die("the gguf package is required for --gguf (pip install gguf)")
    reader = GGUFReader(path)

    def kv(key):
        field = reader.fields.get(key)
        if field is None:
            return None
        try:
            return field.contents()
        except Exception:
            return None

    arch = kv("general.architecture")
    print("== gguf " + "=" * 55)
    print("  general.architecture   %s" % arch)
    for suffix in ("block_count", "expert_count", "expert_used_count", "expert_shared_count",
                   "leading_dense_block_count", "nextn_predict_layers"):
        value = kv("%s.%s" % (arch, suffix))
        if value is not None:
            print("  %-22s %s" % (suffix, value))

    groups = {}
    for t in reader.tensors:
        key = re.sub(r"^blk\.\d+\.", "blk.<il>.", str(t.name))
        add_tensor(groups, key, int(t.n_bytes), list(t.shape), str(t.tensor_type.name))

    print()
    print("== expert tensor suffixes present " + "=" * 30)
    exps = sorted(k for k in groups if "_exps" in k)
    if exps:
        print_group_table(groups, exps, width=46)
        suffixes = sorted({k.split("blk.<il>.")[-1].replace(".weight", "") for k in exps})
        print()
        print("  registry row:")
        print('      {"%s", {%s}},' % (arch, ", ".join('"%s"' % s for s in suffixes)))
    else:
        print("  none. This gguf names no *_exps tensors, so there is nothing for the")
        print("  streamer to bind -- the model would load fully resident.")

    print()
    print("== largest tensor groups " + "=" * 39)
    ranked = sorted(groups, key=lambda k: groups[k]["bytes"], reverse=True)
    print_group_table(groups, ranked[:15], width=46)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--repo", help="Hugging Face repo id, e.g. Qwen/Qwen3.8-Flash-Next")
    src.add_argument("--dir", help="local checkout with config.json and safetensors")
    src.add_argument("--gguf", help="a converted gguf (the first shard is enough)")
    args = ap.parse_args()

    if args.gguf:
        from_gguf(args.gguf)
    else:
        from_source(Source(repo=args.repo, directory=args.dir))


if __name__ == "__main__":
    main()
