#!/usr/bin/env python3
"""Builds editor/resources/AppIcon.icns: the demo hero (front view, first walk
frame) on a dark rounded tile, scaled with hard pixels. Needs Pillow and
macOS's iconutil.    python3 tools/make_app_icon.py"""
import os, shutil, subprocess, tempfile
from PIL import Image, ImageDraw

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sheet = Image.open(os.path.join(ROOT, "projects/pixel_demo/assets/hero.png")).convert("RGBA")
hero = sheet.crop((0, 0, 16, 22))  # row 0 = front view, column 0 = standing

def icon(size):
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    pad = size // 10  # macOS icons leave a margin around the tile
    ImageDraw.Draw(img).rounded_rectangle((pad, pad, size - pad, size - pad), radius=size // 5, fill=(32, 34, 61, 255))
    scale = max(1, (size - 3 * pad) // 22)
    h = hero.resize((16 * scale, 22 * scale), Image.NEAREST)
    img.alpha_composite(h, ((size - h.width) // 2, (size - h.height) // 2))
    return img

work = tempfile.mkdtemp()
iconset = os.path.join(work, "AppIcon.iconset")
os.makedirs(iconset)
for base in (16, 32, 128, 256, 512):
    icon(base).save(os.path.join(iconset, "icon_%dx%d.png" % (base, base)))
    icon(base * 2).save(os.path.join(iconset, "icon_%dx%d@2x.png" % (base, base)))
out = os.path.join(ROOT, "editor/resources/AppIcon.icns")
subprocess.run(["iconutil", "-c", "icns", iconset, "-o", out], check=True)
shutil.rmtree(work)
print("wrote", os.path.normpath(out))
