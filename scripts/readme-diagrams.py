"""The README's three figures, drawn plainly: thin lines, grey fills, the app's accent for the thing
each figure is about, and the app's own face (Oxanium, embedded, since GitHub serves an SVG as an
image that may load nothing). Each is written twice, for GitHub's light and dark themes (`<picture>`
in the README picks one), on a transparent background.

The numbers are measured, not illustrative. RUN is one `bmoe-cli --auto` run of v0.33.0 on a 16 GB
Apple-silicon laptop with 10.3 GiB available at start (256 tokens); SPEED is the README's examples
table. Re-measure before changing either, and change the README table with SPEED.

usage: python3 scripts/readme-diagrams.py [outdir]   (default: docs/assets/diagrams)"""
import base64
import os
import sys

ROOT = os.path.join(os.path.dirname(__file__), "..")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "docs", "assets", "diagrams")
os.makedirs(OUT, exist_ok=True)

# The accents are the web UI's --accent, light and dark (ui/src/app.css).
THEMES = {
    "light": dict(text="#1f2328", muted="#59636e", line="#8c959f", fill="#d0d7de", panel="#f6f8fa", accent="#0fa8c2"),
    "dark": dict(text="#e6edf3", muted="#9198a1", line="#6e7681", fill="#3d444d", panel="#151b23", accent="#21e6ff"),
}
with open(os.path.join(ROOT, "ui", "public", "fonts", "Oxanium.ttf"), "rb") as f:
    FACE = base64.b64encode(f.read()).decode()
FONT = "Oxanium, -apple-system, 'Segoe UI', Helvetica, Arial, sans-serif"

RUN = dict(model="Qwen3.6-35B-A3B Q4_K_M", file_gib=20.8, experts_gib=18.6, dense_gib=1.7, cache_gib=7.2,
           hits=85, read_mib=69.6, decode=17.6, prefill=21.8, threads=8, prompt_on="GPU (Metal)")
SPEED = [("16 GB Apple-silicon laptop", 18, 24), ("16 GB Windows laptop", 4.8, 4.8),
         ("12 GB Android phone", 4.8, 5.0), ("12 GB iPhone", 2.6, 3.0)]


def tps(v):
    """A speed as the README writes it: whole tokens per second; the bars keep the measured value."""
    return str(int(v + 0.5))


def tps_range(lo, hi):
    a, b = tps(lo), tps(hi)
    return a if a == b else f"{a}-{b}"


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;")


def svg(w, h, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" '
            f'font-family="{FONT}"><style>@font-face{{font-family:Oxanium;'
            f'src:url(data:font/ttf;base64,{FACE}) format("truetype")}}</style>{body}</svg>\n')


def text(x, y, s, c, size=13, weight=400, anchor="start"):
    return (f'<text x="{x}" y="{y}" font-size="{size}" fill="{c}" font-weight="{weight}" '
            f'text-anchor="{anchor}">{esc(s)}</text>')


def rect(x, y, w, h, stroke, fill="none", sw=1):
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"/>'


def line(x1, y1, x2, y2, c, sw=1):
    return f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{c}" stroke-width="{sw}"/>'


def arrow(x1, y, x2, c):
    return line(x1, y, x2 - 6, y, c, 1.2) + f'<path d="M{x2 - 8},{y - 4}L{x2},{y}L{x2 - 8},{y + 4}z" fill="{c}"/>'


def token(t):
    """One decode step: experts on flash, the few the router picks, hits in RAM and misses read."""
    hits = {(1, 2), (1, 9), (4, 5), (6, 11)}
    misses = {(2, 7), (5, 1), (7, 13)}
    b = [text(30, 36, "Flash", t["text"], 15, 700),
         text(30, 56, f"the whole model, {RUN['file_gib']} GiB", t["muted"])]
    for r in range(8):
        for c in range(16):
            f = t["accent"] if (r, c) in misses else t["fill"] if (r, c) in hits else "none"
            b.append(rect(30 + c * 20, 72 + r * 20, 16, 16, t["line"], f, 0.8))
    b.append(text(30, 252, "one square: one expert of one layer", t["muted"], 12))
    b.append(text(450, 144, "misses, read on demand", t["text"], anchor="middle"))
    b.append(arrow(375, 154, 525, t["text"]))
    b.append(text(550, 36, "RAM", t["text"], 15, 700))
    b.append(text(550, 56, "what every token needs, and the hot experts", t["muted"]))
    b.append(rect(550, 72, 320, 52, t["line"], t["panel"], 0.8))
    b.append(text(565, 94, "dense weights", t["text"]) + text(855, 94, f"{RUN['dense_gib']} GiB", t["text"], anchor="end"))
    b.append(text(565, 112, "attention, norms, router: always resident", t["muted"], 12))
    b.append(rect(550, 136, 320, 96, t["line"], "none", 0.8))
    b.append(text(565, 158, "expert cache", t["text"]) + text(855, 158, f"{RUN['cache_gib']} GiB", t["text"], anchor="end"))
    b.append(text(565, 176, f"{RUN['hits']}% of the experts a token asks for are here", t["muted"], 12))
    for i, f in enumerate([t["fill"]] * 4 + [t["accent"]]):
        b.append(rect(565 + i * 20, 194, 16, 16, t["line"], f, 0.8))
    return svg(900, 266, "".join(b))


def plan(t):
    """What --auto chose on one machine: three columns, label left and value right, the figures of RUN."""
    cols = [("Flash", [("model file", f"{RUN['file_gib']} GiB"), ("experts", f"{RUN['experts_gib']} GiB"),
                       ("dense", f"{RUN['dense_gib']} GiB")],
             ("read per token", f"{RUN['read_mib']} MiB")),
            ("RAM", [("dense", f"{RUN['dense_gib']} GiB"), ("expert cache", f"{RUN['cache_gib']} GiB"),
                     ("cache size", "set by --auto")],
             ("cache hits", f"{RUN['hits']}%")),
            ("Compute", [("prompt", RUN["prompt_on"]), ("decode", f"CPU, {RUN['threads']} threads"),
                         ("prefill", f"{tps(RUN['prefill'])} tok/s")],
             ("decode", f"{tps(RUN['decode'])} tok/s"))]
    w, gap = 260, 40
    b = []
    for i, (title, rows, key) in enumerate(cols):
        x = 20 + i * (w + gap)
        b.append(rect(x, 20, w, 156, t["line"]))
        b.append(text(x + 16, 46, title, t["text"], 15, 700))
        for j, (k, v) in enumerate(rows):
            b.append(text(x + 16, 74 + j * 22, k, t["muted"]) + text(x + w - 16, 74 + j * 22, v, t["text"], anchor="end"))
        b.append(line(x + 16, 136, x + w - 16, 136, t["fill"]))
        b.append(text(x + 16, 160, key[0], t["accent"], 14, 700)
                 + text(x + w - 16, 160, key[1], t["accent"], 14, 700, anchor="end"))
        if i:
            b.append(arrow(x - gap + 6, 98, x - 4, t["text"]))
    b.append(text(20, 206, f"{RUN['model']} on a 16 GB Apple-silicon laptop, what bmoe-cli --auto chose. "
                  "v0.33.0, one run.", t["muted"], 12))
    return svg(900, 220, "".join(b))


def speed(t):
    """Decode speed of one model on four devices, each smaller than the model."""
    x0, unit = 250, 24
    b = [text(20, 28, f"Qwen3.6-35B-A3B Q4_K_M ({RUN['file_gib']} GiB): decode, tokens per second", t["text"], 15, 700)]
    for i, (name, lo, hi) in enumerate(SPEED):
        y = 52 + i * 40
        b.append(text(20, y + 16, name, t["text"]))
        b.append(rect(x0, y, lo * unit, 22, t["accent"], t["accent"], 0))
        if hi > lo:
            xa, xb = x0 + lo * unit, x0 + hi * unit
            b.append(line(xa, y + 11, xb, y + 11, t["text"]) + line(xb, y + 5, xb, y + 17, t["text"]))
        b.append(text(x0 + hi * unit + 8, y + 16, tps_range(lo, hi), t["text"]))
    b.append(line(x0, 44, x0, 52 + len(SPEED) * 40, t["text"]))
    for v in range(0, 25, 5):
        b.append(text(x0 + v * unit, 230, str(v), t["muted"], 11, anchor="middle"))
    b.append(text(20, 260, "The model is larger than the device's memory on all four; experts stream from flash. "
                  "A line past a bar: the range across runs.", t["muted"], 12))
    return svg(900, 274, "".join(b))


for name, fn in (("token", token), ("plan", plan), ("speed", speed)):
    for theme, t in THEMES.items():
        with open(os.path.join(OUT, f"{name}-{theme}.svg"), "w") as f:
            f.write(fn(t))
