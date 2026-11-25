#!/usr/bin/env python3
"""Check that the GPU modules cover the GPUs llama.cpp's own release covers.

    python3 scripts/check-gpu-targets.py

Which GPUs a module has code for is llama.cpp's decision, not ours: a list written here by hand
once left out every integrated AMD GPU (780M, Strix Point, Strix Halo) and NVIDIA's GB10 while the
pinned llama.cpp built them. So:

  - HIP: HIP_TARGETS in .github/workflows/gpu-modules.yml must be the target list of llama.cpp's
    Linux ROCm release (third_party/llama.cpp/.github/workflows/release.yml, job ubuntu-24-rocm),
    as a set. A submodule bump that changes upstream's list fails here until ours follows.
  - CUDA: the architectures are ggml's default list for our toolkit, read from the pinned
    ggml-cuda CMakeLists, with one change: an architecture ggml ships as PTX only (`XX-virtual`) is
    shipped as PTX and machine code (`XX`). PTX is compiled by the driver at load, which needs a
    driver at least as new as the toolkit (R575 for CUDA 12.9); machine code runs on any driver
    CUDA 12 runs on (R525), so a Turing, Volta, Pascal, Maxwell, A100 or H100 card on an older
    driver gets CUDA instead of a crashed probe (#694). The architectures stay upstream's: only how
    each is shipped changes. gpu-modules.yml builds with `--cuda-archs` below, and no workflow may
    write a list by hand.

    python3 scripts/check-gpu-targets.py --cuda-archs 12.9.1   # the list the CUDA module builds

Standard library only; needs the submodule checked out.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UPSTREAM = os.path.join(ROOT, "third_party", "llama.cpp", ".github", "workflows", "release.yml")
OURS = os.path.join(ROOT, ".github", "workflows", "gpu-modules.yml")
WORKFLOWS = os.path.join(ROOT, ".github", "workflows")
GGML_CUDA = os.path.join(ROOT, "third_party", "llama.cpp", "ggml", "src", "ggml-cuda", "CMakeLists.txt")


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


def version(v):
    return tuple(int(x) for x in v.split("."))


def ggml_cuda_default(toolkit):
    """ggml's non-native default list for a toolkit version, as its CMakeLists builds it.

    Reads the else() branch of `if (GGML_NATIVE ...)` up to its endif(): unconditional appends, and appends under
    `if (CUDAToolkit_VERSION VERSION_LESS|VERSION_GREATER_EQUAL "X")`. Anything else there is a form
    this reader does not know, and it fails rather than guess.
    """
    with open(GGML_CUDA, encoding="utf-8") as f:
        text = f.read()
    m = re.search(r"if \(GGML_NATIVE[^\n]*\n[^\n]*\n\s*else\(\)\n(.*)", text, re.S)
    if not m:
        sys.exit("check-gpu-targets: no default CUDA architecture list in %s" % GGML_CUDA)
    tv = version(toolkit)
    out, stack = [], []
    for raw in m.group(1).split("\n"):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        c = re.match(r'if \(CUDAToolkit_VERSION (VERSION_LESS|VERSION_GREATER_EQUAL) "([0-9.]+)"\)$', line)
        if c:
            bound = version(c.group(2))
            stack.append(tv < bound if c.group(1) == "VERSION_LESS" else tv >= bound)
            continue
        if re.match(r"endif\s*\(\s*\)$", line):
            if not stack:
                break  # the endif() of the native branch: the list is complete
            stack.pop()
            continue
        a = re.match(r"list\(APPEND CMAKE_CUDA_ARCHITECTURES ([^)]+)\)$", line)
        if a:
            if all(stack):
                out += a.group(1).split()
            continue
        sys.exit("check-gpu-targets: ggml's CUDA list has a line this reader does not know: " + line)
    if stack or not out:
        sys.exit("check-gpu-targets: could not read ggml's CUDA list for %s" % toolkit)
    return out


def shipped_cuda(toolkit):
    """ggml's list with every PTX-only architecture also shipped as machine code."""
    out = []
    for a in ggml_cuda_default(toolkit):
        if not re.match(r"^[0-9]+[af]?(-real|-virtual)?$", a):
            sys.exit("check-gpu-targets: unknown CUDA architecture form in ggml's list: " + a)
        out.append(a[: -len("-virtual")] if a.endswith("-virtual") else a)
    return out


def our_cuda_toolkit():
    with open(OURS, encoding="utf-8") as f:
        m = re.search(r"^\s*CUDA_VERSION:\s*([0-9.]+)", f.read(), re.M)
    if not m:
        sys.exit("check-gpu-targets: no CUDA_VERSION in %s" % OURS)
    return m.group(1)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--cuda-archs":
        print(";".join(shipped_cuda(sys.argv[2])))
        return
    if len(sys.argv) != 1:
        sys.exit("usage: check-gpu-targets.py [--cuda-archs TOOLKIT_VERSION]")
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
                m = re.search(r"-DCMAKE_CUDA_ARCHITECTURES=(\S*)", line)
                if m and m.group(1).strip("\"'") != "$CUDA_ARCHS":
                    ok = False
                    print("%s:%d writes CUDA architectures by hand; use $CUDA_ARCHS from --cuda-archs" % (name, n))
    toolkit = our_cuda_toolkit()
    cuda = shipped_cuda(toolkit)
    if not ok:
        sys.exit(1)
    print(
        "GPU targets: HIP %d targets as llama.cpp's release; CUDA %s: %s (ggml's list, PTX-only ones also as machine code)"
        % (len(ours), toolkit, ";".join(cuda))
    )


if __name__ == "__main__":
    main()
