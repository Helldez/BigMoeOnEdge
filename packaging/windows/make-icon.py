#!/usr/bin/env python3
"""Writes packaging/windows/bigmoeonedge.ico from the app icon the other platforms use.

The source is the iOS app's 1024-pixel icon, the same file packaging/macos/make-app.sh and
packaging/linux/make-appimage.sh take, so the three desktop apps show one icon. The .ico holds the
sizes Windows draws (notification area, title bar, taskbar, Explorer) as PNG entries, which Windows
Vista and later read. Resizing uses Pillow when it is installed, macOS `sips` otherwise.

    python3 packaging/windows/make-icon.py
"""
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile

SIZES = (16, 20, 24, 32, 40, 48, 64, 256)


def resized_pngs(src):
    try:
        from PIL import Image  # noqa: PLC0415
    except ImportError:
        Image = None
    out = {}
    if Image is not None:
        with Image.open(src) as im:
            im = im.convert("RGBA")
            for s in SIZES:
                buf = io.BytesIO()
                im.resize((s, s), Image.LANCZOS).save(buf, format="PNG")
                out[s] = buf.getvalue()
        return out
    if not shutil.which("sips"):
        sys.exit("needs Pillow (pip install pillow) or macOS sips to resize the icon")
    with tempfile.TemporaryDirectory() as tmp:
        for s in SIZES:
            p = os.path.join(tmp, f"{s}.png")
            subprocess.run(["sips", "-z", str(s), str(s), src, "--out", p], check=True, stdout=subprocess.DEVNULL)
            with open(p, "rb") as f:
                out[s] = f.read()
    return out


def ico(pngs):
    # ICONDIR, then one ICONDIRENTRY per image, then the images; 0 in a size byte means 256.
    head = struct.pack("<HHH", 0, 1, len(pngs))
    offset = len(head) + 16 * len(pngs)
    entries, blobs = b"", b""
    for s, data in sorted(pngs.items()):
        b = 0 if s >= 256 else s
        entries += struct.pack("<BBBBHHII", b, b, 0, 0, 1, 32, len(data), offset)
        blobs += data
        offset += len(data)
    return head + entries + blobs


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    src = os.path.join(root, "examples", "ios", "app", "Resources", "Assets.xcassets", "AppIcon.appiconset",
                       "icon-1024.png")
    dst = os.path.join(root, "packaging", "windows", "bigmoeonedge.ico")
    with open(dst, "wb") as f:
        f.write(ico(resized_pngs(src)))
    print(dst)


if __name__ == "__main__":
    main()
