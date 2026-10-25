#!/usr/bin/env python3
"""Fetch the parts of the CUDA toolkit a build needs from NVIDIA's redistributable archives.

    python3 scripts/fetch-cuda-redist.py --version 12.8.1 --platform linux-x86_64 --out cuda

NVIDIA publishes every component of the toolkit as an archive of its own, listed with its SHA-256
in a manifest per release (https://developer.download.nvidia.com/compute/cuda/redist/). This reads
the manifest, downloads the components named, refuses one whose digest is not the manifest's, and
unpacks them into one directory laid out as the toolkit installs (bin, include, lib), which CMake
finds through CUDAToolkit_ROOT. The same script serves Linux and Windows, needs no installer and
no administrator rights, and fetches only what the build reads: the compiler, the runtime, the
C++ core libraries and cuBLAS. Standard library only.
"""

import argparse
import hashlib
import json
import os
import shutil
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

BASE = "https://developer.download.nvidia.com/compute/cuda/redist/"
COMPONENTS = ["cuda_nvcc", "cuda_cudart", "cuda_cccl", "libcublas"]


def fetch(url, dst):
    with urllib.request.urlopen(url, timeout=300) as r, open(dst, "wb") as w:
        shutil.copyfileobj(r, w, 1 << 20)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def merge(src, dst):
    # An archive holds one top directory (<component>-<platform>-<version>-archive) whose contents
    # are the toolkit's own layout; the components' trees are disjoint but for the directories.
    for root, _, files in os.walk(src):
        rel = os.path.relpath(root, src)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for name in files:
            s = os.path.join(root, name)
            d = os.path.join(dst, rel, name)
            if os.path.islink(s):
                if os.path.lexists(d):
                    os.remove(d)
                os.symlink(os.readlink(s), d)
            else:
                shutil.copy2(s, d)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--version", required=True, help="the CUDA release, as the manifest names it (12.8.1)")
    ap.add_argument("--platform", required=True, help="linux-x86_64, windows-x86_64, ...")
    ap.add_argument("--out", required=True)
    ap.add_argument("--component", action="append", help="repeatable; default: " + " ".join(COMPONENTS))
    args = ap.parse_args()

    with urllib.request.urlopen(BASE + "redistrib_%s.json" % args.version, timeout=120) as r:
        manifest = json.loads(r.read())
    os.makedirs(args.out, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        for comp in args.component or COMPONENTS:
            entry = manifest.get(comp, {}).get(args.platform)
            if not entry:
                sys.exit("%s has no %s archive in CUDA %s" % (comp, args.platform, args.version))
            archive = os.path.join(tmp, os.path.basename(entry["relative_path"]))
            fetch(BASE + entry["relative_path"], archive)
            got = sha256(archive)
            if got != entry["sha256"]:
                sys.exit("%s: SHA-256 %s, the manifest says %s" % (archive, got, entry["sha256"]))
            unpacked = os.path.join(tmp, comp)
            if archive.endswith(".zip"):
                with zipfile.ZipFile(archive) as z:
                    z.extractall(unpacked)
            else:
                with tarfile.open(archive) as t:
                    t.extractall(unpacked)
            tops = os.listdir(unpacked)
            if len(tops) != 1:
                sys.exit("%s: expected one top directory, found %s" % (archive, tops))
            merge(os.path.join(unpacked, tops[0]), args.out)
            os.remove(archive)
            shutil.rmtree(unpacked)
            print("%s %s" % (comp, manifest[comp].get("version", "")), flush=True)


if __name__ == "__main__":
    main()
