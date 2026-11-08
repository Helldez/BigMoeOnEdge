#!/usr/bin/env python3
"""Draws the README's planner diagram, one PNG per README language.

Draws each as SVG and, on macOS, renders it to docs/assets/diagrams/plan-<lang>.png with Quick Look
and sips (no browser or extra package needed); elsewhere it leaves the SVGs there to render by hand. The PNG is what the READMEs show: an SVG
in an <img> takes the viewer's fonts, and the labels then overflow their boxes.

    python3 scripts/plan-diagram.py
"""
import os
import shutil
import subprocess
import sys
import tempfile

# Each label is (bold first line, second line); the last entry is the loop's label.
LABELS = {
    "en": [("Machine:", "memory, storage, devices"), ("Model:", "size, experts, routing"), ("Plan", ""),
           ("Where each weight goes:", "VRAM, RAM or flash"),
           ("Which device reads the prompt,", "which one generates"), ("Cache sizes", "and threads"),
           ("Run", ""), ("Measure the first answer,", "keep what is faster"), "kept for the next load"],
    "it": [("Macchina:", "memoria, storage, dispositivi"), ("Modello:", "dimensione, esperti, routing"),
           ("Piano", ""), ("Dove va ogni peso:", "VRAM, RAM o flash"),
           ("Quale dispositivo legge il prompt,", "quale genera i token"),
           ("Dimensione delle cache", "e thread"), ("Esecuzione", ""),
           ("Misura la prima risposta,", "tiene ciò che è più veloce"), "salvato per il prossimo caricamento"],
    "es": [("Máquina:", "memoria, almacenamiento, dispositivos"), ("Modelo:", "tamaño, expertos, enrutado"),
           ("Plan", ""), ("Dónde va cada peso:", "VRAM, RAM o flash"),
           ("Qué dispositivo lee el prompt,", "cuál genera los tokens"), ("Tamaño de las cachés", "e hilos"),
           ("Ejecución", ""), ("Mide la primera respuesta,", "se queda con lo más rápido"),
           "guardado para la próxima carga"],
    "fr": [("Machine :", "mémoire, stockage, appareils"), ("Modèle :", "taille, experts, routage"),
           ("Plan", ""), ("Où va chaque poids :", "VRAM, RAM ou flash"),
           ("Quel appareil lit le prompt,", "lequel génère les tokens"), ("Taille des caches", "et threads"),
           ("Exécution", ""), ("Mesure la première réponse,", "garde ce qui est plus rapide"),
           "gardé pour le prochain chargement"],
    "zh": [("机器：", "内存、存储、设备"), ("模型：", "大小、专家、路由"), ("方案", ""),
           ("每个权重放在哪里：", "VRAM、RAM 或闪存"), ("哪个设备处理提示词，", "哪个生成 token"),
           ("缓存大小", "和线程数"), ("运行", ""), ("测量第一个回答，", "保留更快的设置"), "保存供下次加载使用"],
    "ja": [("マシン：", "メモリ、ストレージ、デバイス"), ("モデル：", "サイズ、エキスパート、ルーティング"),
           ("プラン", ""), ("各重みの置き場所：", "VRAM、RAM、フラッシュ"),
           ("プロンプトを処理するデバイス、", "トークンを生成するデバイス"), ("キャッシュサイズ", "とスレッド数"),
           ("実行", ""), ("最初の回答を計測し、", "速い方を残す"), "次回の読み込みのために保存"],
}

FONT = ("Helvetica Neue, Helvetica, Arial, PingFang SC, Hiragino Sans, "
        "Hiragino Kaku Gothic ProN, sans-serif")
W, H = 1320, 760          # the drawing
PAD = (W - H) // 2        # Quick Look renders a square: the drawing is centred, then cropped

# Box centres and sizes: two inputs, the plan, three decisions, run, measure.
BOXES = [(400, 80, 400, 78), (880, 80, 400, 78), (640, 221, 320, 62),
         (220, 369, 400, 78), (640, 369, 400, 78), (1060, 369, 400, 78),
         (640, 521, 320, 62), (640, 659, 400, 78)]
EDGES = [(0, 2), (1, 2), (2, 3), (2, 4), (2, 5), (3, 6), (4, 6), (5, 6), (6, 7)]


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def svg(labels):
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{W}" viewBox="0 0 {W} {W}" '
           f'font-family="{FONT}">',
           '<defs><marker id="a" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="9" markerHeight="9" '
           'orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="#5b6270"/></marker></defs>',
           f'<rect width="{W}" height="{W}" fill="#ffffff"/>', f'<g transform="translate(0,{PAD})">',
           '<g stroke="#5b6270" stroke-width="2.5" fill="none" marker-end="url(#a)">']
    for a, b in EDGES:
        ax, ay, _, ah = BOXES[a]
        bx, by, bw, bh = BOXES[b]
        # fan into the target's top edge, a third of the way in from each side
        tx = bx + max(-bw / 4, min(bw / 4, (ax - bx) * 0.25))
        out.append(f'<path d="M{ax} {ay + ah / 2} L{tx:.0f} {by - bh / 2 - 2}"/>')
    mx, my, mw, _ = BOXES[7]
    px, py, pw, _ = BOXES[2]
    rx = W - 28  # right of the decision row's last box
    out.append(f'<path d="M{mx + mw / 2} {my} L{rx} {my} L{rx} {py} L{px + pw / 2 + 4} {py}"/></g>')
    for i, (cx, cy, w, h) in enumerate(BOXES):
        plan = i == 2
        fill, stroke = ("#2f5d8a", "#2f5d8a") if plan else ("#f3f1ec", "#8a8270")
        out.append(f'<rect x="{cx - w / 2}" y="{cy - h / 2}" width="{w}" height="{h}" rx="14" '
                   f'fill="{fill}" stroke="{stroke}" stroke-width="2"/>')
        first, second = labels[i]
        colour = "#ffffff" if plan else "#2b2620"
        if second:
            out.append(f'<text x="{cx}" y="{cy - 6}" fill="{colour}" font-size="21" font-weight="700" '
                       f'text-anchor="middle">{esc(first)}</text>')
            out.append(f'<text x="{cx}" y="{cy + 22}" fill="{colour}" font-size="21" '
                       f'text-anchor="middle">{esc(second)}</text>')
        else:
            out.append(f'<text x="{cx}" y="{cy + 8}" fill="{colour}" font-size="23" font-weight="700" '
                       f'text-anchor="middle">{esc(first)}</text>')
    out.append(f'<text x="{(mx + mw / 2 + rx) / 2:.0f}" y="{my - 12}" fill="#5b6270" font-size="19" '
               f'font-style="italic" text-anchor="middle">{esc(labels[8])}</text>')
    out.append("</g></svg>")
    return "\n".join(out)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    outdir = os.path.join(root, "docs", "assets", "diagrams")
    os.makedirs(outdir, exist_ok=True)
    render = sys.platform == "darwin" and shutil.which("qlmanage") and shutil.which("sips")
    for lang, labels in LABELS.items():
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp if render else outdir, f"plan-{lang}.svg")
            with open(path, "w", encoding="utf-8") as f:
                f.write(svg(labels))
            if not render:
                continue
            subprocess.run(["qlmanage", "-t", "-s", str(2 * W), "-o", tmp, path], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            png = os.path.join(outdir, f"plan-{lang}.png")
            subprocess.run(["sips", "-c", str(2 * (H + 40)), str(2 * W), path + ".png", "--out", png],
                           check=True, stdout=subprocess.DEVNULL)
        print(png)
    if not render:
        print("SVGs written; render the PNGs on macOS (Quick Look and sips).")


if __name__ == "__main__":
    main()
