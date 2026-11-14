#!/usr/bin/env python3
"""A model file's shell: its gguf header at its own offsets, the rest a hole.

bmoe-planlab simulate places a real model on simulated cards, and the capacity fitter and the model
probe read nothing but a gguf's header: its metadata and its tensor table. A shell is that header,
fetched with HTTP range requests, in a sparse file of the model's full size, so a 120 GB model costs
a few MB of download and of disk (docs/hardware-simulation.md). Never load a shell to run it: its
weights are zeros.

    gguf_shell.py URL [URL...] -o DIR      one shell per URL (each shard of a split model)

Standard library only.
"""

import argparse
import os
import struct
import sys
import urllib.parse
import urllib.request

UA = "bmoe-planlab-gguf-shell"

# gguf value types (gguf.h)
U8, I8, U16, I16, U32, I32, F32, BOOL, STR, ARR, U64, I64, F64 = range(13)
FIXED = {U8: 1, I8: 1, U16: 2, I16: 2, U32: 4, I32: 4, F32: 4, BOOL: 1, U64: 8, I64: 8, F64: 8}


class NeedMore(Exception):
    pass


class Reader:
    def __init__(self, buf):
        self.buf, self.pos = buf, 0

    def take(self, n):
        if self.pos + n > len(self.buf):
            raise NeedMore()
        b = self.buf[self.pos:self.pos + n]
        self.pos += n
        return b

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def u64(self):
        return struct.unpack("<Q", self.take(8))[0]

    def string(self):
        return self.take(self.u64()).decode("utf-8", "replace")

    def value(self, t):
        if t in FIXED:
            raw = self.take(FIXED[t])
            return int.from_bytes(raw, "little") if t in (U32, U64, U16, U8) else raw
        if t == STR:
            return self.string()
        if t == ARR:
            et, n = self.u32(), self.u64()
            if et in FIXED:
                self.take(FIXED[et] * n)
            else:
                for _ in range(n):
                    self.value(et)
            return None
        raise ValueError("unknown gguf value type %d" % t)


def data_offset(buf):
    """Where the tensor data starts, or NeedMore when `buf` ends before the header does."""
    r = Reader(buf)
    if r.take(4) != b"GGUF":
        raise ValueError("not a gguf file")
    version = r.u32()
    if version < 2:
        raise ValueError("gguf version %d is too old" % version)
    n_tensors, n_kv = r.u64(), r.u64()
    alignment = 32
    for _ in range(n_kv):
        key = r.string()
        t = r.u32()
        v = r.value(t)
        if key == "general.alignment" and isinstance(v, int):
            alignment = v
    for _ in range(n_tensors):
        r.string()
        n_dims = r.u32()
        r.take(8 * n_dims + 4 + 8)
    return (r.pos + alignment - 1) // alignment * alignment


def fetch(url, start, end):
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Range": "bytes=%d-%d" % (start, end)})
    with urllib.request.urlopen(req, timeout=120) as resp:
        total = None
        cr = resp.headers.get("Content-Range")
        if cr and "/" in cr:
            total = int(cr.rsplit("/", 1)[1])
        return resp.read(), total


def shell(url, out_dir):
    name = os.path.basename(urllib.parse.urlparse(url).path)
    path = os.path.join(out_dir, name)
    buf, total, want = b"", None, 4 << 20
    while True:
        chunk, size = fetch(url, len(buf), want - 1)
        total = total or size
        buf += chunk
        try:
            off = data_offset(buf)
            break
        except NeedMore:
            if total is not None and len(buf) >= total:
                raise ValueError("%s: the header runs past the end of the file" % name)
            want *= 2
    if total is None:
        raise ValueError("%s: the server did not say the file's size (no Content-Range)" % name)
    with open(path, "wb") as f:
        f.write(buf[:off])
        f.truncate(total)  # sparse: the weights are a hole
    print("%s: header %d bytes of %.1f GiB" % (path, off, total / 2**30))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("urls", nargs="+")
    ap.add_argument("-o", "--out-dir", required=True)
    a = ap.parse_args()
    os.makedirs(a.out_dir, exist_ok=True)
    for u in a.urls:
        shell(u, a.out_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
