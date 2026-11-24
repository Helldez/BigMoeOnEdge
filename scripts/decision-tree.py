#!/usr/bin/env python3
"""The decision tree in docs/decision-tree.md, held to the code.

Every decision the engine makes has one node in the document and one `// dt:<id>` marker in the
code, on the line above the branch. A node names the file its marker is in and the condition
the code tests, copied from the code; `check` fails when either drifts, so the tree cannot
describe a branch the code no longer has.

  check             nodes, markers and conditions agree; the graphs are up to date (CI runs this)
  render            rewrite the Mermaid graphs from the nodes
  tree [ID]         print the tree (or the subtree under ID) as text
  touched [BASE]    which nodes a diff against BASE (default origin/main) touches, as a text
                    tree and, with --mermaid, as graphs with those nodes marked, for a PR body

Standard library only; runs on Python 3.9.
"""

import argparse
import os
import re
import subprocess
import sys
from collections import OrderedDict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOC = os.path.join(ROOT, "docs", "decision-tree.md")
# Where markers may live: the engine and its front-ends.
SCOPE = ("core/", "cli/", "server/")
SOURCE_EXT = (".cpp", ".h", ".mm", ".c")

ID_RE = r"[a-z][a-z0-9]*(?:\.[a-z0-9][a-z0-9-]*)+"
NODE_RE = re.compile(r"^#### `(" + ID_RE + r")` — (.+?)\s*$")
FIELD_RE = re.compile(r"^- (parent|when|code|condition|reads): (.*)$")
MARKER_RE = re.compile(r"//\s*dt:(" + ID_RE + r")\b")
GRAPH_BEGIN_RE = re.compile(r"^<!-- dt:graph (\S+) -->\s*$")
GRAPH_END = "<!-- /dt:graph -->"
# How far below its marker a node's condition may sit (a long condition is wrapped by clang-format).
WINDOW = 14
# How far a node's further conditions may follow its marker (and never past the next marker).
REGION = 250


class Node(object):
    def __init__(self, nid, title, line):
        self.id = nid
        self.title = title
        self.line = line
        self.parent = None
        self.when = ""
        self.code = None
        self.function = ""
        self.conditions = []
        self.children = []

    @property
    def phase(self):
        return self.id.split(".", 1)[0]


def strip_ticks(s):
    s = s.strip()
    if s.startswith("`") and s.endswith("`") and len(s) >= 2:
        return s[1:-1]
    return s


def parse_doc(path=DOC):
    nodes = OrderedDict()
    errors = []
    cur = None
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    for i, raw in enumerate(lines, 1):
        m = NODE_RE.match(raw)
        if m:
            nid = m.group(1)
            if nid in nodes:
                errors.append("%s:%d: node %s defined twice" % (rel(path), i, nid))
            cur = Node(nid, m.group(2), i)
            nodes[nid] = cur
            continue
        if raw.startswith("#"):
            cur = None
            continue
        if cur is None:
            continue
        m = FIELD_RE.match(raw)
        if not m:
            continue
        key, val = m.group(1), m.group(2).strip()
        if key == "parent":
            cur.parent = None if val in ("—", "-", "") else strip_ticks(val)
        elif key == "when":
            cur.when = val
        elif key == "code":
            # `path` · `Function`
            parts = re.findall(r"`([^`]+)`", val)
            if not parts:
                errors.append("%s:%d: %s: code must be `path` · `function`" % (rel(path), i, cur.id))
            else:
                cur.code = parts[0]
                cur.function = parts[1] if len(parts) > 1 else ""
        elif key == "condition":
            # The condition is the whole backticked span; it may itself contain backticks only if
            # wrapped in double backticks, which no C++ condition needs.
            m2 = re.match(r"^``?\s?(.*?)\s?``?$", val)
            cur.conditions.append(m2.group(1) if m2 else val)
    for n in nodes.values():
        if n.parent is not None:
            if n.parent not in nodes:
                errors.append("%s:%d: %s: parent %s is not a node" % (rel(path), n.line, n.id, n.parent))
            else:
                nodes[n.parent].children.append(n)
        if n.code is None:
            errors.append("%s:%d: %s: no `- code:` line" % (rel(path), n.line, n.id))
    # A parent chain must end at a root.
    for n in nodes.values():
        seen = set()
        p = n
        while p is not None and p.parent in nodes:
            if p.id in seen:
                errors.append("%s:%d: %s: parent chain loops" % (rel(path), n.line, n.id))
                break
            seen.add(p.id)
            p = nodes[p.parent]
    return nodes, lines, errors


def rel(p):
    return os.path.relpath(p, ROOT)


def source_files():
    out = subprocess.run(["git", "ls-files"] + list(SCOPE), cwd=ROOT, stdout=subprocess.PIPE,
                         universal_newlines=True, check=True).stdout.split("\n")
    return [p for p in out if p.endswith(SOURCE_EXT)]


def scan_markers(files):
    """{id: [(path, line)]} and {path: [(line, id)]} for every marker in the given files."""
    by_id = {}
    by_file = {}
    for p in files:
        try:
            with open(os.path.join(ROOT, p), encoding="utf-8", errors="replace") as f:
                text = f.read().split("\n")
        except OSError:
            continue
        for i, line in enumerate(text, 1):
            for m in MARKER_RE.finditer(line):
                by_id.setdefault(m.group(1), []).append((p, i))
                by_file.setdefault(p, []).append((i, m.group(1)))
    return by_id, by_file


def norm(s):
    return re.sub(r"\s+", "", s)


def mermaid_id(nid):
    return re.sub(r"[^A-Za-z0-9]", "_", nid)


def mermaid_text(s):
    s = s.replace("`", "").replace('"', "#quot;").replace("<", "#lt;").replace(">", "#gt;")
    return s


def render_graph(nodes, phase, marked=None):
    marked = marked or set()
    out = ["```mermaid", "flowchart TD"]
    members = [n for n in nodes.values() if n.phase == phase]
    for n in members:
        out.append('    %s["<b>%s</b><br/>%s"]' % (mermaid_id(n.id), n.id, mermaid_text(n.title)))
    for n in members:
        if n.parent and n.parent in nodes and nodes[n.parent].phase == phase:
            label = mermaid_text(n.when)
            if label:
                out.append('    %s -->|"%s"| %s' % (mermaid_id(n.parent), label, mermaid_id(n.id)))
            else:
                out.append("    %s --> %s" % (mermaid_id(n.parent), mermaid_id(n.id)))
    # The roots of a phase are steps the code takes in order: a dotted spine keeps that order and
    # stacks them, instead of a row of unconnected boxes.
    roots = [n for n in members if not (n.parent and n.parent in nodes and nodes[n.parent].phase == phase)]
    for a, b in zip(roots, roots[1:]):
        out.append("    %s -.-> %s" % (mermaid_id(a.id), mermaid_id(b.id)))
    hit = [n for n in members if n.id in marked]
    if hit:
        out.append("    classDef touched fill:#f59e0b,stroke:#b45309,color:#000")
        out.append("    class %s touched" % ",".join(mermaid_id(n.id) for n in hit))
    out.append("```")
    return out


def rerender(nodes, lines):
    """The document with every graph block regenerated."""
    out = []
    i = 0
    while i < len(lines):
        m = GRAPH_BEGIN_RE.match(lines[i])
        if not m:
            out.append(lines[i])
            i += 1
            continue
        out.append(lines[i])
        j = i + 1
        while j < len(lines) and lines[j].strip() != GRAPH_END:
            j += 1
        out.extend(render_graph(nodes, m.group(1)))
        out.append(GRAPH_END)
        i = j + 1
    return out


def cmd_check(_args):
    nodes, lines, errors = parse_doc()
    files = source_files()
    by_id, file_markers = scan_markers(files)
    for nid, sites in sorted(by_id.items()):
        if nid not in nodes:
            for p, ln in sites:
                errors.append("%s:%d: marker dt:%s has no node in docs/decision-tree.md" % (p, ln, nid))
        elif len(sites) > 1:
            where = ", ".join("%s:%d" % s for s in sites)
            errors.append("dt:%s is marked more than once (%s): one node, one place" % (nid, where))
    file_cache = {}
    for n in nodes.values():
        sites = by_id.get(n.id, [])
        if not sites:
            errors.append("docs/decision-tree.md:%d: %s: no `// dt:%s` marker in the code" % (n.line, n.id, n.id))
            continue
        p, ln = sites[0]
        if n.code and p != n.code:
            errors.append("docs/decision-tree.md:%d: %s: code says %s, the marker is in %s:%d" %
                          (n.line, n.id, n.code, p, ln))
        if p not in file_cache:
            with open(os.path.join(ROOT, p), encoding="utf-8", errors="replace") as f:
                file_cache[p] = f.read().split("\n")
        text = file_cache[p]
        # The first condition is the branch the marker sits on. Any further ones follow it in the
        # node's region: up to the next marker in the file, at most REGION lines.
        later = [m for m, _ in file_markers.get(p, []) if m > ln]
        end = min(later[0] - 1 if later else len(text), ln - 1 + REGION)
        window = norm("\n".join(text[ln - 1:ln - 1 + WINDOW]))
        region = norm("\n".join(text[ln - 1:end]))
        for k, c in enumerate(n.conditions):
            if k == 0 and norm(c) not in window:
                errors.append("docs/decision-tree.md:%d: %s: condition `%s` is not within %d lines below %s:%d" %
                              (n.line, n.id, c, WINDOW, p, ln))
            elif k > 0 and norm(c) not in region:
                errors.append("docs/decision-tree.md:%d: %s: condition `%s` is not between %s:%d and the next marker" %
                              (n.line, n.id, c, p, ln))
        if n.function:
            # The function named must enclose the marker: its name appears above it in the file.
            head = "\n".join(text[:ln])
            short = n.function.split("::")[-1].split("(")[0]
            if short not in head:
                errors.append("docs/decision-tree.md:%d: %s: function %s not found above %s:%d" %
                              (n.line, n.id, n.function, p, ln))
    if rerender(nodes, lines) != lines:
        errors.append("docs/decision-tree.md: graphs are stale; run scripts/decision-tree.py render")
    for e in errors:
        print(e)
    if errors:
        print("\n%d problem(s). The tree follows the code: fix the node or the marker in the same change." %
              len(errors))
        return 1
    print("decision tree: %d nodes, all marked, conditions match, graphs current" % len(nodes))
    return 0


def cmd_render(_args):
    nodes, lines, errors = parse_doc()
    if errors:
        for e in errors:
            print(e)
        return 1
    new = rerender(nodes, lines)
    if new != lines:
        with open(DOC, "w", encoding="utf-8") as f:
            f.write("\n".join(new))
        print("graphs rewritten")
    else:
        print("graphs already current")
    return 0


def print_tree(nodes, roots, marked=None, out=sys.stdout):
    marked = marked or set()

    def walk(n, prefix, last, top):
        star = "  <== touched" if n.id in marked else ""
        when = "[%s] " % n.when if n.when else ""
        if top:
            out.write("%s%s — %s%s\n" % (when, n.id, n.title, star))
            child_prefix = ""
        else:
            out.write("%s%s%s%s — %s%s\n" % (prefix, "└─ " if last else "├─ ", when, n.id, n.title, star))
            child_prefix = prefix + ("   " if last else "│  ")
        for k, c in enumerate(n.children):
            walk(c, child_prefix, k == len(n.children) - 1, False)

    for r in roots:
        walk(r, "", True, True)
        out.write("\n")


def cmd_tree(args):
    nodes, _, errors = parse_doc()
    if errors:
        for e in errors:
            print(e)
        return 1
    if args.id:
        if args.id not in nodes:
            print("no node %s" % args.id)
            return 1
        roots = [nodes[args.id]]
    else:
        roots = [n for n in nodes.values() if n.parent is None]
    print_tree(nodes, roots)
    return 0


def changed_lines(base):
    """{path: set(new-side line numbers)} for the diff of the working tree against BASE."""
    diff = subprocess.run(["git", "diff", "-U0", base, "--"] + list(SCOPE), cwd=ROOT,
                          stdout=subprocess.PIPE, universal_newlines=True, check=True).stdout
    out = {}
    path = None
    for line in diff.split("\n"):
        if line.startswith("+++ "):
            path = None if line[4:] == "/dev/null" else line[6:]
        elif line.startswith("@@") and path:
            m = re.match(r"@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", line)
            start, count = int(m.group(1)), int(m.group(2) or "1")
            # A pure deletion (count 0) sits after line `start`: attribute it to that line.
            span = range(start, start + count) if count else range(max(start, 1), max(start, 1) + 1)
            out.setdefault(path, set()).update(span)
    return {p: s for p, s in out.items() if p.endswith(SOURCE_EXT)}


def cmd_touched(args):
    nodes, _, errors = parse_doc()
    if errors:
        for e in errors:
            print(e)
        return 1
    changes = changed_lines(args.base)
    _, by_file = scan_markers(list(changes))
    marked = set()
    outside = []
    for p, lns in sorted(changes.items()):
        marks = sorted(by_file.get(p, []))
        loose = 0
        for ln in sorted(lns):
            # A changed line belongs to the nearest marker above it in the same file.
            owner = None
            for mln, mid in marks:
                if mln <= ln:
                    owner = mid
                else:
                    break
            if owner:
                marked.add(owner)
            else:
                loose += 1
        if loose:
            outside.append("%s (%d line(s) above any marker)" % (p, loose))
    marked = {m for m in marked if m in nodes}
    if not marked:
        print("No decision node touched.")
    else:
        # The touched nodes with their ancestors, so a reviewer sees where in the tree they sit.
        keep = set()
        for m in marked:
            n = nodes[m]
            while n is not None:
                keep.add(n.id)
                n = nodes.get(n.parent) if n.parent else None
        print("Touched decision nodes (%d):" % len(marked))
        for m in sorted(marked):
            path = []
            n = nodes[m]
            while n is not None:
                path.append(n.id)
                n = nodes.get(n.parent) if n.parent else None
            print("  %s — %s\n      path: %s" % (m, nodes[m].title, " > ".join(reversed(path))))
        print()
        pruned = OrderedDict()
        for n in nodes.values():
            if n.id in keep:
                c = Node(n.id, n.title, n.line)
                c.parent, c.when = n.parent, n.when
                pruned[n.id] = c
        for c in pruned.values():
            if c.parent in pruned:
                pruned[c.parent].children.append(c)
        print_tree(pruned, [c for c in pruned.values() if c.parent not in pruned], marked)
        if args.mermaid:
            print("Graphs (touched nodes marked):\n")
            for phase in sorted({nodes[m].phase for m in marked}):
                print("\n".join(render_graph(nodes, phase, marked)))
                print()
    if outside:
        print("Changed code outside every node (no marker above it in its file):")
        for o in outside:
            print("  " + o)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("check")
    sub.add_parser("render")
    t = sub.add_parser("tree")
    t.add_argument("id", nargs="?")
    u = sub.add_parser("touched")
    u.add_argument("base", nargs="?", default="origin/main")
    u.add_argument("--mermaid", action="store_true")
    args = ap.parse_args()
    fn = {"check": cmd_check, "render": cmd_render, "tree": cmd_tree, "touched": cmd_touched}.get(args.cmd)
    if not fn:
        ap.print_help()
        return 2
    return fn(args)


if __name__ == "__main__":
    sys.exit(main())
