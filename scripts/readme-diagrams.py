"""The README's figure of one decode step, drawn plainly: thin lines, grey fills and the app's
accent for the experts read. GitHub serves an SVG as an image that loads no font, so the text names
Inter and falls back to the system's sans. It is written twice, for GitHub's light and dark themes (`<picture>`
in the README picks one), on a transparent background.

The numbers are measured, not illustrative: RUN is one `bmoe-cli --auto` run of v0.33.0 on a 16 GB
Apple-silicon laptop with 10.3 GiB available at start (256 tokens). Re-measure before changing it.

usage: python3 scripts/readme-diagrams.py [outdir]   (default: docs/assets/diagrams)"""
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
FONT = "Inter, -apple-system, 'Segoe UI', Helvetica, Arial, sans-serif"

RUN = dict(model="Qwen3.6-35B-A3B Q4_K_M", file_gib=20.8, experts_gib=18.6, dense_gib=1.7, cache_gib=7.2,
           hits=85, read_mib=69.6, decode=17.6, prefill=21.8, threads=8, prompt_on="GPU (Metal)")


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;")


def svg(w, h, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" '
            f'font-family="{FONT}">{body}</svg>\n')


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



for name, fn in (("token", token),):
    for theme, t in THEMES.items():
        with open(os.path.join(OUT, f"{name}-{theme}.svg"), "w") as f:
            f.write(fn(t))
