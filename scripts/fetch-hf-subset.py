#!/usr/bin/env python3
"""Fetch some tensors of a Hugging Face safetensors checkpoint, not the checkpoint.

    python3 scripts/fetch-hf-subset.py Qwen/Qwen3.8-Flash-Next --include '^mtp\\.' \\
        --include 'embed_tokens' --include '^lm_head' --out mtp-src

A safetensors shard starts with a JSON header naming every tensor's dtype, shape and byte range, so
HTTP range requests read the headers and then only the tensors asked for. The result is a directory
llama.cpp's converter reads like the checkpoint itself: the repository's small files (config,
tokenizer, generation config), one shard per source shard holding the selected tensors, and an index
naming only them. Its use here is a model's multi-token-prediction head, which some quantised files
leave out and the converter writes on its own (convert_hf_to_gguf.py --mtp), but nothing in it knows
about heads: the patterns choose the tensors.

A range request must come back as 206 with the range asked for. A server or proxy that ignores the
header returns the file from its start, which would otherwise be written as tensor bytes.
Standard library only. Re-running continues: a shard already written with the expected size is kept.
"""

import argparse
import concurrent.futures
import json
import os
import re
import struct
import sys
import time
import urllib.request

SMALL_LIMIT = 64 << 20  # repository files under this size are copied whole (configs, tokenizers)


def url_of(endpoint, repo, revision, path):
    return f"{endpoint}/{repo}/resolve/{revision}/{path}"


def get(url, rng=None, token=None, tries=6):
    # A dropped or timed-out read is retried with a growing pause: a range is idempotent.
    for attempt in range(tries):
        try:
            return get_once(url, rng, token)
        except (IOError, OSError) as e:
            if attempt == tries - 1 or "asked for bytes" in str(e):
                raise
            time.sleep(2 ** attempt)


def get_once(url, rng=None, token=None):
    req = urllib.request.Request(url)
    if rng:
        req.add_header("Range", f"bytes={rng[0]}-{rng[1] - 1}")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(req, timeout=300) as r:
        if rng:
            cr = r.headers.get("Content-Range", "")
            if r.status != 206 or not cr.startswith(f"bytes {rng[0]}-{rng[1] - 1}/"):
                raise IOError(f"{url}: asked for bytes {rng[0]}-{rng[1] - 1}, got status {r.status} ({cr or 'no range'})")
        return r.read()


def repo_files(endpoint, repo, revision, token):
    req = urllib.request.Request(f"{endpoint}/api/models/{repo}/tree/{revision}?recursive=1")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.loads(r.read())


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("repo")
    ap.add_argument("--revision", default="main")
    ap.add_argument("--include", action="append", required=True, help="regex over tensor names; repeatable")
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=8, help="range requests in flight at once")
    ap.add_argument("--endpoint", default=os.environ.get("HF_ENDPOINT", "https://huggingface.co").rstrip("/"))
    args = ap.parse_args()
    token = os.environ.get("HF_TOKEN")
    pats = [re.compile(p) for p in args.include]
    os.makedirs(args.out, exist_ok=True)

    files = repo_files(args.endpoint, args.repo, args.revision, token)
    for f in files:
        p, size = f.get("path", ""), f.get("size", 0)
        if f.get("type") != "file" or p.endswith(".safetensors") or size > SMALL_LIMIT or "/" in p:
            continue
        dst = os.path.join(args.out, p)
        if not os.path.exists(dst) or os.path.getsize(dst) != size:
            open(dst, "wb").write(get(url_of(args.endpoint, args.repo, args.revision, p), token=token))

    index = json.loads(open(os.path.join(args.out, "model.safetensors.index.json")).read())
    wanted = {n: s for n, s in index["weight_map"].items() if any(p.search(n) for p in pats)}
    if not wanted:
        sys.exit("no tensor matches the patterns")
    by_shard = {}
    for n, s in wanted.items():
        by_shard.setdefault(s, []).append(n)

    new_map, total = {}, 0
    for shard, names in sorted(by_shard.items()):
        url = url_of(args.endpoint, args.repo, args.revision, shard)
        n_hdr = struct.unpack("<Q", get(url, (0, 8), token))[0]
        hdr = json.loads(get(url, (8, 8 + n_hdr), token))
        base = 8 + n_hdr
        out_hdr, chunks, off = {}, [], 0
        for n in sorted(names):
            meta = hdr[n]
            a, b = meta["data_offsets"]
            out_hdr[n] = {"dtype": meta["dtype"], "shape": meta["shape"], "data_offsets": [off, off + (b - a)]}
            chunks.append((n, base + a, base + b))
            off += b - a
        blob = json.dumps(out_hdr, separators=(",", ":")).encode()
        blob += b" " * (-len(blob) % 8)  # the data section starts 8-aligned
        dst = os.path.join(args.out, shard)
        expected = 8 + len(blob) + off
        if not (os.path.exists(dst) and os.path.getsize(dst) == expected):
            # Pieces of every selected tensor, fetched by several requests at once and written at
            # their own offsets: one range at a time is bound by the round trip, not the link.
            pieces, pos = [], 8 + len(blob)
            for n, a, b in chunks:
                step = 16 << 20
                for s0 in range(a, b, step):
                    e0 = min(b, s0 + step)
                    pieces.append((s0, e0, pos))
                    pos += e0 - s0
            with open(dst + ".part", "wb") as w:
                w.write(struct.pack("<Q", len(blob)))
                w.write(blob)
                w.truncate(pos)
            with open(dst + ".part", "r+b") as w, concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
                for (s0, e0, at), data in zip(pieces, ex.map(lambda pc: get(url, (pc[0], pc[1]), token), pieces)):
                    w.seek(at)
                    w.write(data)
            os.replace(dst + ".part", dst)
        for n in names:
            new_map[n] = shard
        total += off
        print(f"{shard}: {len(names)} tensors, {off / 2**20:.1f} MiB", flush=True)

    index["weight_map"] = new_map
    index.setdefault("metadata", {})["total_size"] = total
    with open(os.path.join(args.out, "model.safetensors.index.json"), "w") as w:
        json.dump(index, w, indent=1)
    print(f"{len(new_map)} tensors, {total / 2**30:.2f} GiB in {args.out}")


if __name__ == "__main__":
    main()
