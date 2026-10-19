"""The README's three diagrams, in the logo's pixel style (docs/design.md). Each SVG carries its own
dark card, so it reads the same on GitHub's light and dark themes. The figures in memory() are the
Plan screen's for Qwen3.6-35B-A3B on a 16 GB laptop; re-measure before changing them.

usage: python3 scripts/readme-diagrams.py [outdir]   (default: docs/assets/diagrams)"""
import os
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "docs", "assets", "diagrams")
os.makedirs(OUT, exist_ok=True)

BG, PANEL, LINE = "#0A0512", "#160D27", "#2E2148"
CYAN, PURPLE, YELLOW, PINK = "#21E6FF", "#6C5AA8", "#FFC53D", "#FF3E9E"
TEXT, MUTED, DIM = "#F2EEFF", "#B3A9D1", "#3A2C5C"
MONO = "ui-monospace, SFMono-Regular, Menlo, Consolas, monospace"
SANS = "-apple-system, 'Segoe UI', Helvetica, Arial, sans-serif"


def svg(w, h, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" '
            f'shape-rendering="geometricPrecision" font-family="{SANS}">'
            f'<rect width="{w}" height="{h}" rx="18" fill="{BG}"/>'
            f'<g opacity="0.35">{grid(w, h)}</g>{body}</svg>')


def grid(w, h, step=24):
    s = "".join(f'<line x1="{x}" y1="0" x2="{x}" y2="{h}" stroke="{LINE}" stroke-width="1"/>' for x in range(step, w, step))
    return s + "".join(f'<line x1="0" y1="{y}" x2="{w}" y2="{y}" stroke="{LINE}" stroke-width="1"/>' for y in range(step, h, step))


def text(x, y, s, size=16, fill=TEXT, weight=400, anchor="start", font=SANS):
    s = s.replace("&", "&amp;").replace("<", "&lt;")
    return f'<text x="{x}" y="{y}" font-size="{size}" fill="{fill}" font-weight="{weight}" text-anchor="{anchor}" font-family="{font}">{s}</text>'


def box(x, y, w, h, stroke=LINE, fill=PANEL, rx=10, sw=2, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{d}/>'


def pill(x, y, s, color, size=15, fill=None, tcolor=None, pad=12):
    w = int(len(s) * size * 0.6) + 2 * pad
    f = fill or BG
    return box(x, y, w, size + 14, stroke=color, fill=f, rx=6, sw=2) + text(x + pad, y + size + 3, s, size, tcolor or color, 700, font=MONO), w


def arrow(x1, y1, x2, y2, color=MUTED, sw=3, head=10, dash=None):
    import math
    a = math.atan2(y2 - y1, x2 - x1)
    hx1, hy1 = x2 - head * math.cos(a - 0.45), y2 - head * math.sin(a - 0.45)
    hx2, hy2 = x2 - head * math.cos(a + 0.45), y2 - head * math.sin(a + 0.45)
    d = f' stroke-dasharray="{dash}"' if dash else ""
    return (f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" stroke-width="{sw}"{d}/>'
            f'<polygon points="{x2},{y2} {hx1:.1f},{hy1:.1f} {hx2:.1f},{hy2:.1f}" fill="{color}"/>')


def elbow(points, color=MUTED, sw=3, dash=None):
    """A polyline ending in an arrowhead: right angles only, so it never cuts across a label."""
    d = f' stroke-dasharray="{dash}"' if dash else ""
    pts = " ".join(f"{x},{y}" for x, y in points[:-1])
    (x1, y1), (x2, y2) = points[-2], points[-1]
    return (f'<polyline points="{pts} {x2},{y2}" fill="none" stroke="{color}" stroke-width="{sw}" stroke-linejoin="round"{d}/>'
            + arrow(x1, y1, x2, y2, color, sw))


def step(x, y, n, title, sub, color):
    return (box(x, y, 34, 34, stroke=color, fill=color, rx=6) + text(x + 17, y + 24, str(n), 18, BG, 800, "middle", MONO)
            + text(x + 46, y + 15, title, 16, TEXT, 700) + text(x + 46, y + 34, sub, 13.5, MUTED))


# 1 · How a token runs ----------------------------------------------------------------------------
def streaming():
    W, H = 1200, 560
    b = []
    b.append(text(40, 54, "How one token runs", 28, TEXT, 800))
    b.append(text(40, 82, "Each layer asks for a few experts. Hot ones are already in RAM; the rest are read from storage, just in time.", 16, MUTED))

    # storage: the whole model as a grid of experts
    sx, sy = 40, 110
    b.append(box(sx, sy, 470, 330, stroke=PURPLE))
    b.append(text(sx + 20, sy + 32, "STORAGE", 15, PURPLE, 800, font=MONO))
    b.append(text(sx + 20, sy + 54, "the whole model file: every expert of every layer", 14, MUTED))
    cols, rows, cell, gap = 18, 9, 18, 5
    gx, gy = sx + 22, sy + 74
    picked = {(2, 1), (7, 1), (12, 1), (15, 1), (4, 4), (9, 4), (13, 4), (16, 7)}
    cached = {(2, 1), (12, 1), (9, 4)}
    for r in range(rows):
        for c in range(cols):
            x, y = gx + c * (cell + gap), gy + r * (cell + gap)
            if (c, r) in cached:
                fill, st = CYAN, CYAN
            elif (c, r) in picked:
                fill, st = YELLOW, YELLOW
            else:
                fill, st = DIM, DIM
            b.append(f'<rect x="{x}" y="{y}" width="{cell}" height="{cell}" fill="{fill}" stroke="{st}"/>')
    b.append(text(sx + 20, sy + 314, "much larger than RAM: 21 GB to 284 GB in the examples", 13.5, MUTED))

    # RAM
    rx, ry = 690, 110
    b.append(box(rx, ry, 470, 330, stroke=CYAN))
    b.append(text(rx + 20, ry + 32, "RAM", 15, CYAN, 800, font=MONO))
    b.append(text(rx + 20, ry + 54, "only what tokens need", 14, MUTED))
    b.append(box(rx + 20, ry + 72, 430, 58, stroke=PURPLE, fill="#2A1C4A", rx=6))
    b.append(text(rx + 36, ry + 98, "always-needed weights", 16, TEXT, 700))
    b.append(text(rx + 36, ry + 119, "attention, router, embeddings: read every token", 13.5, MUTED))
    b.append(box(rx + 20, ry + 146, 430, 120, stroke=CYAN, fill="#0E2230", rx=6))
    b.append(text(rx + 36, ry + 172, "expert cache", 16, TEXT, 700))
    b.append(text(rx + 36, ry + 193, "the experts used most, kept from token to token", 13.5, MUTED))
    for i in range(14):
        x = rx + 36 + i * 29
        col = CYAN if i in (1, 4, 9) else "#1D4E5E"
        b.append(f'<rect x="{x}" y="{ry + 212}" width="22" height="22" fill="{col}" stroke="{col}"/>')
    b.append(text(rx + 20, ry + 300, "sized by the planner to what this machine can spare", 13.5, MUTED))

    # flows
    b.append(arrow(512, 330, 708, 330, YELLOW, 4))
    b.append(text(610, 298, "misses:", 15, YELLOW, 700, "middle"))
    b.append(text(610, 318, "read on demand", 15, YELLOW, 700, "middle"))

    # steps
    y0 = 470
    b.append(step(40, y0, 1, "The router picks k experts", "per layer, per token", YELLOW))
    b.append(step(330, y0, 2, "Hits come from the cache", "nothing to read", CYAN))
    b.append(step(610, y0, 3, "Misses stream from storage", "only those slices, in parallel", YELLOW))
    b.append(step(910, y0, 4, "The layer computes as usual", "same output as fully in memory", PINK))
    return svg(W, H, "".join(b))


# 2 · How the planner adapts ---------------------------------------------------------------------
def planner():
    W, H = 1200, 560
    b = []
    b.append(text(40, 54, "How it adapts to your machine", 28, TEXT, 800))
    b.append(text(40, 82, "The planner measures, then writes a plan for this device. Same rules everywhere, a different plan on each machine.", 16, MUTED))

    # measure
    b.append(box(40, 110, 300, 400, stroke=PURPLE))
    b.append(text(60, 142, "1 · MEASURE", 15, PURPLE, 800, font=MONO))
    facts = [("the model", "its header: layers, experts, sizes"), ("memory", "what this process may hold"),
             ("storage", "read speed at the expert's size"), ("CPU", "matmul speed, threads"),
             ("GPU / NPU", "each one, in its own process")]
    for i, (a, s) in enumerate(facts):
        y = 166 + i * 66
        b.append(box(60, y, 260, 54, stroke=LINE, fill="#1F1436", rx=6))
        b.append(text(76, y + 24, a, 16, TEXT, 700))
        b.append(text(76, y + 43, s, 13, MUTED))

    b.append(arrow(350, 310, 420, 310, MUTED, 4))

    # plan
    b.append(box(430, 110, 300, 400, stroke=YELLOW))
    b.append(text(450, 142, "2 · PLAN", 15, YELLOW, 800, font=MONO))
    plan = [("what stays in memory", "and what streams"), ("expert cache size", "as large as is worth it"),
            ("which chip reads the prompt", "only if it computes right"), ("which chip writes the answer", "and how many threads"),
            ("a reason for every choice", "your own settings win")]
    for i, (a, s) in enumerate(plan):
        y = 166 + i * 66
        b.append(box(450, y, 260, 54, stroke=LINE, fill="#2A2210", rx=6))
        b.append(text(466, y + 24, a, 16, TEXT, 700))
        b.append(text(466, y + 43, s, 13, MUTED))

    b.append(elbow([(732, 310), (776, 310), (776, 195), (816, 195)], MUTED, 4))
    b.append(elbow([(776, 310), (776, 425), (816, 425)], MUTED, 4))

    # two results
    def device(y, name, prompt, answer, color):
        out = [box(820, y, 340, 170, stroke=color)]
        out.append(text(840, y + 34, name, 18, TEXT, 800))
        p, w = pill(840, y + 54, "prompt", color, 14)
        out.append(p)
        out.append(text(840 + w + 12, y + 74, prompt, 15, TEXT, 700))
        p, w = pill(840, y + 98, "answer", MUTED, 14)
        out.append(p)
        out.append(text(840 + w + 12, y + 118, answer, 15, TEXT, 700))
        out.append(text(840, y + 154, "experts stream, cache sized to free RAM", 13, MUTED))
        return "".join(out)

    b.append(device(110, "16 GB laptop", "GPU (Metal)", "CPU", CYAN))
    b.append(device(340, "12 GB phone", "NPU (Hexagon)", "CPU", PINK))
    b.append(text(1160, 540, "the plans measured on the machines in the video", 13, MUTED, anchor="end"))
    return svg(W, H, "".join(b))


# 3 · Where memory goes -----------------------------------------------------------------------------
def memory():
    W, H = 1200, 480
    b = []
    b.append(text(40, 54, "Where the memory goes", 28, TEXT, 800))
    b.append(text(40, 82, "Qwen3.6-35B-A3B on a 16 GB laptop, as the Plan screen showed it.", 16, MUTED))

    x0, scale = 40, 1100 / 21.0  # px per GB, the model file is the longest bar

    # the model on disk
    b.append(text(x0, 128, "THE MODEL ON DISK · 21 GB", 15, PURPLE, 800, font=MONO))
    dense_w = 2.2 * scale
    b.append(box(x0, 140, dense_w, 46, stroke=PURPLE, fill="#2A1C4A", rx=4))
    b.append(text(x0 + 10, 160, "dense 2.2", 14, TEXT, 700))
    b.append(text(x0 + 10, 178, "always needed", 11.5, MUTED))
    b.append(box(x0 + dense_w + 4, 140, 1100 - dense_w - 4, 46, stroke=PURPLE, fill=PANEL, rx=4))
    for i in range(60):
        x = x0 + dense_w + 14 + i * 16
        if x > x0 + 1090:
            break
        b.append(f'<rect x="{x}" y="152" width="10" height="22" fill="{DIM}"/>')
    b.append(text(x0 + dense_w + 18, 212, "routed experts, 18.6 GB: they stay on storage and stream when a token asks", 14, MUTED))

    # RAM bar

    segs = [("context", 0.45, "#6C5AA8"), ("GPU", 0.27, "#21E6FF"), ("dense", 1.7, "#B9A8FF"), ("expert cache", 6.1, "#3FE0A0")]
    x = x0
    for name, gb, col in segs:
        w = gb * scale
        b.append(f'<rect x="{x:.1f}" y="300" width="{w:.1f}" height="46" rx="3" fill="{col}"/>')
        x += w + 3
    b.append(box(x, 300, 16 * scale - (x - x0), 46, stroke=LINE, fill=BG, rx=3, dash="6 5"))
    b.append(text(x + 12, 329, "rest of the machine", 14, MUTED))
    b.append(text(x0, 376, "WHAT THIS PROCESS HOLDS IN RAM · 8.5 GB OF 16", 15, CYAN, 800, font=MONO))
    # legend
    lx = x0
    for name, gb, col in segs:
        b.append(f'<rect x="{lx}" y="392" width="16" height="16" fill="{col}"/>')
        label = f"{name} {gb:g} GB"
        b.append(text(lx + 24, 406, label, 14, TEXT, 700))
        lx += 30 + len(label) * 8.6
    # mapping arrows
    dense_x = x0 + (0.45 + 0.27 + 0.85) * scale + 3
    cache_x = x0 + (0.45 + 0.27 + 1.7 + 3.05) * scale + 9
    b.append(elbow([(dense_x, 188), (dense_x, 296)], PURPLE, 3))
    b.append(elbow([(x0 + 900, 188), (x0 + 900, 262), (cache_x, 262), (cache_x, 296)], "#3FE0A0", 3, dash="7 6"))
    b.append(text(cache_x + 24, 252, "the hot experts, cached", 14, "#3FE0A0", 700))
    b.append(text(x0, 450, "The planner sizes each part from what it measured; on a machine with more memory the cache simply grows.", 15, MUTED))
    return svg(W, H, "".join(b))


for name, fn in (("streaming", streaming), ("planner", planner), ("memory", memory)):
    with open(os.path.join(OUT, f"{name}.svg"), "w") as f:
        f.write(fn())
