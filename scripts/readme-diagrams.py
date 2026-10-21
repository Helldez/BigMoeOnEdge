"""The README's three figures, drawn plainly: thin lines, no fill but grey, one accent for the thing
the figure is about. Each is written twice, for GitHub's light and dark themes (`<picture>` in the
README picks one), on a transparent background.

The numbers are measured, not illustrative. RUN is one `bmoe-cli --auto` run of v0.33.0 on a 16 GB
Apple-silicon laptop with 10.3 GiB available at start (256 tokens); SPEED is the README's examples
table. Re-measure before changing either, and change the README table with SPEED.

usage: python3 scripts/readme-diagrams.py [outdir]   (default: docs/assets/diagrams)"""
import os
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "docs", "assets", "diagrams")
os.makedirs(OUT, exist_ok=True)

THEMES = {
    "light": dict(text="#1f2328", muted="#59636e", line="#8c959f", fill="#d0d7de", panel="#f6f8fa", accent="#d1600e"),
    "dark": dict(text="#e6edf3", muted="#9198a1", line="#6e7681", fill="#3d444d", panel="#151b23", accent="#f0883e"),
}
SANS = "-apple-system, 'Segoe UI', Helvetica, Arial, sans-serif"
MONO = "ui-monospace, SFMono-Regular, Menlo, Consolas, monospace"

RUN = dict(model="Qwen3.6-35B-A3B Q4_K_M", file_gib=20.8, experts_gib=18.6, dense_gib=1.7, cache_gib=7.2,
           hits=85, read_mib=69.6, decode=17.6, prefill=21.8, threads=8, prompt_on="GPU (Metal)")
SPEED = [("16 GB Apple-silicon laptop", 18, 24), ("16 GB Windows laptop", 4.8, 4.8),
         ("12 GB Android phone", 4.8, 5.0), ("12 GB iPhone", 2.6, 3.0)]


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;")


def svg(w, h, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" '
            f'font-family="{SANS}">{body}</svg>\n')


def text(x, y, s, c, size=13, weight=400, anchor="start", font=SANS):
    return (f'<text x="{x}" y="{y}" font-size="{size}" fill="{c}" font-weight="{weight}" '
            f'text-anchor="{anchor}" font-family="{font}" xml:space="preserve">{esc(s)}</text>')


def rect(x, y, w, h, stroke, fill="none", sw=1):
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}"/>'


def arrow(x1, y, x2, c):
    return (f'<line x1="{x1}" y1="{y}" x2="{x2 - 6}" y2="{y}" stroke="{c}" stroke-width="1.2"/>'
            f'<path d="M{x2 - 8},{y - 4}L{x2},{y}L{x2 - 8},{y + 4}z" fill="{c}"/>')


def token(t):
    """One decode step: experts on flash, the few the router picks, hits in RAM and misses read."""
    hits = {(1, 2), (1, 9), (4, 5), (6, 11)}
    misses = {(2, 7), (5, 1), (7, 13)}
    b = [text(30, 36, "Flash", t["text"], 14, 700),
         text(30, 54, f"the whole model, {RUN['file_gib']} GiB", t["muted"])]
    for r in range(8):
        for c in range(16):
            f = t["accent"] if (r, c) in misses else t["fill"] if (r, c) in hits else "none"
            b.append(rect(30 + c * 20, 70 + r * 20, 16, 16, t["line"], f, 0.8))
    b.append(text(30, 250, "one square: one expert of one layer", t["muted"], 12))
    b.append(text(450, 142, "misses, read on demand", t["text"], anchor="middle"))
    b.append(arrow(375, 152, 525, t["text"]))
    b.append(text(550, 36, "RAM", t["text"], 14, 700))
    b.append(text(550, 54, "what every token needs, and the hot experts", t["muted"]))
    b.append(rect(550, 70, 320, 50, t["line"], t["panel"], 0.8))
    b.append(text(565, 92, f"dense weights   {RUN['dense_gib']} GiB", t["text"]))
    b.append(text(565, 109, "attention, norms, router: always resident", t["muted"], 12))
    b.append(rect(550, 135, 320, 96, t["line"], "none", 0.8))
    b.append(text(565, 157, f"expert cache   {RUN['cache_gib']} GiB", t["text"]))
    b.append(text(565, 174, f"{RUN['hits']}% of the experts a token asks for are here", t["muted"], 12))
    for i, f in enumerate([t["fill"]] * 4 + [t["accent"]]):
        b.append(rect(565 + i * 20, 192, 16, 16, t["line"], f, 0.8))
    b.append(f'<text x="30" y="292" font-size="12" fill="{t["muted"]}" font-family="{SANS}">'
             f'<tspan font-weight="700" fill="{t["text"]}">Figure 1.</tspan> One decode step. The router picks k '
             f'experts per layer: cached ones (grey) cost nothing, the others (orange) are read from flash.</text>')
    return svg(900, 310, "".join(b))


def plan(t):
    """What --auto chose on one machine: three columns, monospace, the figures of RUN."""
    m = MONO
    cols = [("flash", [f"model.gguf     {RUN['file_gib']} GiB", f"experts        {RUN['experts_gib']} GiB",
                       f"dense           {RUN['dense_gib']} GiB"],
             ["reads misses only", f"{RUN['read_mib']} MiB / token"]),
            ("RAM", [f"dense           {RUN['dense_gib']} GiB", f"expert cache    {RUN['cache_gib']} GiB"],
             [f"cache hits      {RUN['hits']}%", "size chosen by --auto"]),
            ("compute", [f"prompt   {RUN['prompt_on']}", f"decode   CPU, {RUN['threads']} threads"],
             [f"decode   {RUN['decode']} tok/s", f"prefill  {RUN['prefill']} tok/s"])]
    b = []
    for i, (title, top, bottom) in enumerate(cols):
        x = 20 + i * 300
        b.append(rect(x, 20, 260, 200, t["line"]))
        b.append(text(x + 15, 46, title, t["text"], 13, 700, font=m))
        for j, s in enumerate(top):
            b.append(text(x + 15, 74 + j * 20, s, t["text"], 13, font=m))
        for j, s in enumerate(bottom):
            b.append(text(x + 15, 180 + j * 20, s, t["accent"] if j == 0 else t["muted"], 13, font=m))
        if i:
            b.append(arrow(x - 40, 120, x, t["text"]))
    b.append(text(20, 252, f"{RUN['model']} on a 16 GB Apple-silicon laptop: bmoe-cli --auto, v0.33.0, one run.",
                  t["muted"], 12, font=m))
    return svg(900, 266, "".join(b))


def speed(t):
    """Decode speed of one model on four devices, each smaller than the model."""
    x0, unit = 250, 24
    b = [text(20, 28, f"Qwen3.6-35B-A3B Q4_K_M ({RUN['file_gib']} GiB): decode, tokens per second", t["text"], 14, 700)]
    for i, (name, lo, hi) in enumerate(SPEED):
        y = 52 + i * 40
        b.append(text(20, y + 16, name, t["text"]))
        b.append(rect(x0, y, lo * unit, 22, t["accent"], t["accent"], 0))
        if hi > lo:
            b.append(rect(x0 + lo * unit, y, (hi - lo) * unit, 22, t["accent"], "none", 1))
        b.append(text(x0 + hi * unit + 8, y + 16, f"{lo:g}" if hi == lo else f"{lo:.1f}-{hi:.1f}" if lo < 10 else f"{lo:g}-{hi:g}", t["text"]))
    b.append(f'<line x1="{x0}" y1="44" x2="{x0}" y2="{52 + len(SPEED) * 40}" stroke="{t["text"]}"/>')
    for v in range(0, 25, 5):
        b.append(text(x0 + v * unit, 230, str(v), t["muted"], 11, anchor="middle"))
    b.append(text(20, 260, "The model is larger than the device's memory on all four; experts stream from flash. "
                  "Outlined: the range across runs.", t["muted"], 12))
    return svg(900, 274, "".join(b))


for name, fn in (("token", token), ("plan", plan), ("speed", speed)):
    for theme, t in THEMES.items():
        with open(os.path.join(OUT, f"{name}-{theme}.svg"), "w") as f:
            f.write(fn(t))
