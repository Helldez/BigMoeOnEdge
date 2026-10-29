#!/usr/bin/env python3
"""Write a release's feed.json from the files the release carries (docs/desktop-app.md, "The feed").

    scripts/make-feed.py --tag v0.39.0 --repo Helldez/BigMoeOnEdge --assets assets/ \
        --packs packaging/packs.json --out feed.json

Every file is named by the release workflow, and named here the same way:
  installers  BigMoeOnEdge-<tag>-<target>-setup.exe | .dmg | .AppImage
  archives    bmoe-cli-<tag>-<target>.tar.gz | .zip       (the command-line tools)
  packs       <asset>-<tag>-<target>.tar.gz | .zip         (packaging/packs.json says which)
Each entry carries the file's download URL on the release, its SHA-256 and its size, which the
server and the install scripts check what they download against. The feed is written one entry a
line, so scripts/install.sh can read it with no JSON parser; the release workflow then signs these
exact bytes (Ed25519), and the server believes nothing in them before the signature checks.

Only the standard library: it runs on the release runner with no install step.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import sys

# A target as the server names its build: <os>-<arch>, nothing more (an archive of a variant, such
# as the NPU one, is not what a fresh machine installs).
TARGET = r"(?P<target>[a-z]+-[a-z0-9_]+)"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def asset(directory, name, base_url):
    path = os.path.join(directory, name)
    return {"url": base_url + name, "sha256": sha256(path), "size": os.path.getsize(path)}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tag", required=True, help="the release tag, vX.Y.Z")
    ap.add_argument("--repo", required=True, help="owner/name of the repository the release is on")
    ap.add_argument("--assets", required=True, help="the folder holding the release's files")
    ap.add_argument("--packs", required=True, help="packaging/packs.json")
    ap.add_argument("--out", required=True)
    ap.add_argument("--published", default=datetime.date.today().isoformat())
    a = ap.parse_args()

    m = re.fullmatch(r"v?(\d+\.\d+\.\d+)", a.tag)
    if not m:
        sys.exit(f"make-feed: '{a.tag}' is not a vX.Y.Z tag")
    if not re.fullmatch(r"[A-Za-z0-9._-]+/[A-Za-z0-9._-]+", a.repo):
        sys.exit(f"make-feed: '{a.repo}' is not owner/name")
    version = m.group(1)
    tag = re.escape(a.tag)
    base = f"https://github.com/{a.repo}/releases/download/{a.tag}/"
    files = sorted(os.listdir(a.assets))

    installers, archives = {}, {}
    for name in files:
        i = re.fullmatch(rf"BigMoeOnEdge-{tag}-{TARGET}(?:-setup\.exe|\.dmg|\.AppImage)", name)
        if i:
            installers[i.group("target")] = asset(a.assets, name, base)
        c = re.fullmatch(rf"bmoe-cli-{tag}-{TARGET}\.(?:tar\.gz|zip)", name)
        if c:
            archives[c.group("target")] = asset(a.assets, name, base)

    with open(a.packs, encoding="utf-8") as f:
        pack_rows = json.load(f)["packs"]
    packs = []
    for p in pack_rows:
        if not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,31}", p["id"]):
            sys.exit(f"make-feed: pack id '{p['id']}' is not lowercase letters, digits and '-'")
        for target, row in sorted(p["targets"].items()):
            found = [n for n in files
                     if re.fullmatch(rf"{re.escape(p['asset'])}-{tag}-{re.escape(target)}\.(?:tar\.gz|zip)", n)]
            if not found:
                print(f"make-feed: no {p['id']} pack for {target} on this release", file=sys.stderr)
                continue
            entry = {"id": p["id"], "title": p["title"], "target": target, "needs_any": row["needs_any"]}
            entry.update(asset(a.assets, found[0], base))
            packs.append(entry)

    if not installers and not archives:
        sys.exit("make-feed: no installer and no archive in " + a.assets)

    def block(name, rows):
        lines = [f'    {json.dumps(k)}: {json.dumps(v)}' for k, v in sorted(rows.items())]
        return f'  "{name}": {{\n' + ",\n".join(lines) + "\n  }"

    head = [f'  "version": {json.dumps(version)}',
            f'  "published": {json.dumps(a.published)}',
            f'  "notes": {json.dumps(f"https://github.com/{a.repo}/releases/tag/{a.tag}")}',
            block("installers", installers),
            block("archives", archives),
            '  "packs": [\n' + ",\n".join(f"    {json.dumps(p)}" for p in packs) + "\n  ]"]
    text = "{\n" + ",\n".join(head) + "\n}\n"
    json.loads(text)  # what is written must read back
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"make-feed: {version}: {len(installers)} installers, {len(archives)} archives, {len(packs)} packs")


if __name__ == "__main__":
    main()
