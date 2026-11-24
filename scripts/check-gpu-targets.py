#!/usr/bin/env python3
"""Check that the GPU modules cover the GPUs llama.cpp's own release covers.

    python3 scripts/check-gpu-targets.py

Which GPUs a module has code for is llama.cpp's decision, not ours: a list written here by hand
once left out every integrated AMD GPU (780M, Strix Point, Strix Halo) and NVIDIA's GB10 while the
pinned llama.cpp built them. So:

  - HIP: HIP_TARGETS in .github/workflows/gpu-modules.yml must be the target list of llama.cpp's
    Linux ROCm release (third_party/llama.cpp/.github/workflows/release.yml, job ubuntu-24-rocm),
    as a set. A submodule bump that changes upstream's list fails here until ours follows.
  - CUDA: no workflow of ours may set CMAKE_CUDA_ARCHITECTURES, so ggml's default list applies,
    as in llama.cpp's release.

Standard library only; needs the submodule checked out.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UPSTREAM = os.path.join(ROOT, "third_party", "llama.cpp", ".github", "workflows", "release.yml")
OURS = os.path.join(ROOT, ".github", "workflows", "gpu-modules.yml")
WORKFLOWS = os.path.join(ROOT, ".github", "workflows")


def targets(text):
    return {t.strip() for t in text.split(";") if t.strip()}


def upstream_hip():
    with open(UPSTREAM, encoding="utf-8") as f:
        text = f.read()
    # The job's block: from its key to the next job at the same indent.
    m = re.search(r"^  ubuntu-24-rocm:\n(.*?)(?=^  [A-Za-z0-9_-]+:\n)", text, re.M | re.S)
    if not m:
        sys.exit("check-gpu-targets: no ubuntu-24-rocm job in %s" % UPSTREAM)
    lists = re.findall(r'gpu_targets:\s*"([^"]+)"', m.group(1))
    if not lists:
        sys.exit("check-gpu-targets: no gpu_targets in upstream's ubuntu-24-rocm job")
    out = set()
    for l in lists:
        out |= targets(l)
    return out


def ours_hip():
    with open(OURS, encoding="utf-8") as f:
        m = re.search(r'^\s*HIP_TARGETS:\s*"([^"]+)"', f.read(), re.M)
    if not m:
        sys.exit("check-gpu-targets: no HIP_TARGETS in %s" % OURS)
    return targets(m.group(1))


def main():
    ok = True
    up, ours = upstream_hip(), ours_hip()
    if up != ours:
        ok = False
        print("HIP targets differ from llama.cpp's release:")
        if up - ours:
            print("  missing here: " + ";".join(sorted(up - ours)))
        if ours - up:
            print("  not upstream: " + ";".join(sorted(ours - up)))
    for name in sorted(os.listdir(WORKFLOWS)):
        if not name.endswith((".yml", ".yaml")):
            continue
        with open(os.path.join(WORKFLOWS, name), encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                if re.search(r"-DCMAKE_CUDA_ARCHITECTURES", line):
                    ok = False
                    print("%s:%d sets CMAKE_CUDA_ARCHITECTURES; leave it to ggml's default list" % (name, n))
    if not ok:
        sys.exit(1)
    print("GPU targets: HIP %d targets as llama.cpp's release; CUDA on ggml's default list" % len(ours))


if __name__ == "__main__":
    main()
