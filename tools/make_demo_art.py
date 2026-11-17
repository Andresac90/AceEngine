#!/usr/bin/env python3
"""Generates the demo project's pixel art (palette, tiling textures, sprites)
and the village scene. Everything is drawn from one palette of colour ramps,
which is what makes separate textures look like they belong together.

    python3 tools/make_demo_art.py        (needs Pillow)
"""
import json, math, os, random
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "projects", "pixel_demo")
ART = os.path.join(ROOT, "assets")

# Ramps run dark -> light; shadows drift towards blue/violet, lights towards yellow.
RAMPS = {
    "grass": ["#1f3b3a", "#2c5a3f", "#3f7d43", "#62a04a", "#9cc35a"],
    "dirt":  ["#3a2a33", "#5a3d3a", "#7d5a44", "#a67d55", "#cfa76f"],
    "stone": ["#2b2a3d", "#474a5e", "#6b7083", "#959aa6", "#c4c8c8"],
    "wood":  ["#33222e", "#54332f", "#7a4a33", "#a8683c", "#d49650"],
    "roof":  ["#3a1f33", "#63263a", "#96353d", "#c4553f", "#e58a56"],
    "sky":   ["#3d5a9c", "#5b84c4", "#86b0df", "#b8d6ee", "#e6f2f7"],
    "skin":  ["#6b3a3f", "#a85f4d", "#d99672", "#f2c9a0", "#fff1d6"],
    "cloth": ["#1d2247", "#2f3f85", "#4a67b8", "#f7d65a", "#ffffff"],
}
def rgb(h): return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))
R = {k: [rgb(c) for c in v] for k, v in RAMPS.items()}

def save(img, name): img.save(os.path.join(ART, name))

def palette():
    colors = [c for ramp in R.values() for c in ramp]
    img = Image.new("RGBA", (8, 5))
    for i, c in enumerate(colors): img.putpixel((i % 8, i // 8), c + (255,))
    save(img, "palette.png")

def noise_tile(name, ramp, size, weights, seed, clumps=0):
    """Random speckle from the ramp's middle tones, plus optional darker/lighter clumps."""
    rnd = random.Random(seed)
    img = Image.new("RGBA", (size, size))
    for y in range(size):
        for x in range(size):
            img.putpixel((x, y), ramp[rnd.choices(range(len(weights)), weights)[0]] + (255,))
    for _ in range(clumps):
        cx, cy, tone = rnd.randrange(size), rnd.randrange(size), rnd.choice([1, 3])
        for dx, dy in [(0, 0), (1, 0), (0, 1), (-1, 0)][: rnd.randrange(2, 5)]:
            img.putpixel(((cx + dx) % size, (cy + dy) % size), ramp[tone] + (255,))
    save(img, name)

def bricks(name, ramp, size=32, bw=16, bh=8, seed=3):
    rnd = random.Random(seed)
    img = Image.new("RGBA", (size, size))
    for row in range(size // bh):
        offset = (bw // 2) * (row % 2)
        for col in range(size // bw + 1):
            tone = rnd.choice([2, 2, 2, 3, 1])
            for y in range(bh):
                for x in range(bw):
                    px, py = (col * bw + x - offset) % size, row * bh + y
                    c = ramp[tone]
                    if y == 0 or x == 0: c = ramp[0]                      # mortar line
                    elif y == 1 or x == 1: c = ramp[min(tone + 1, 4)]      # lit top/left edge
                    elif y == bh - 1 or x == bw - 1: c = ramp[max(tone - 1, 1)]  # shaded bottom/right
                    elif rnd.random() < 0.08: c = ramp[max(tone - 1, 1)]   # pitting
                    img.putpixel((px, py), c + (255,))
    save(img, name)

def planks(name, ramp, size=32, pw=8, seed=5):
    rnd = random.Random(seed)
    img = Image.new("RGBA", (size, size))
    for col in range(size // pw):
        tone, joint = rnd.choice([2, 2, 3]), rnd.randrange(size)
        for y in range(size):
            for x in range(pw):
                c = ramp[tone]
                if x == 0: c = ramp[0]
                elif x == 1: c = ramp[min(tone + 1, 4)]
                elif (y + col * 5) % 11 == 0 and rnd.random() < 0.7: c = ramp[tone - 1]   # grain streak
                if y == joint: c = ramp[0]
                img.putpixel((col * pw + x, y), c + (255,))
    save(img, name)

def shingles(name, ramp, size=32, sw=8, sh=8):
    img = Image.new("RGBA", (size, size))
    for y in range(size):
        for x in range(size):
            row = y // sh
            lx, ly = (x + (sw // 2) * (row % 2)) % sw, y % sh
            tone = 2 + ((x // sw + row * 3) % 3 == 0)
            c = ramp[tone]
            if ly == sh - 1 or (lx == 0 and ly > 1): c = ramp[0]
            elif ly == 0: c = ramp[min(tone + 1, 4)]
            elif ly == sh - 2: c = ramp[1]
            img.putpixel((x, y), c + (255,))
    save(img, name)

def sprite(name, rows, key):
    img = Image.new("RGBA", (len(rows[0]), len(rows)), (0, 0, 0, 0))
    for y, row in enumerate(rows):
        for x, ch in enumerate(row):
            if ch != ".": img.putpixel((x, y), key[ch] + (255,))
    save(img, name)

def art():
    os.makedirs(ART, exist_ok=True)
    palette()
    noise_tile("grass.png", R["grass"], 32, [0, 3, 10, 3, 0], seed=1, clumps=26)
    noise_tile("dirt.png", R["dirt"], 32, [0, 2, 10, 3, 0], seed=2, clumps=14)
    bricks("bricks.png", R["stone"])
    planks("planks.png", R["wood"])
    shingles("shingles.png", R["roof"])
    k = {"o": R["stone"][0], "h": R["wood"][1], "H": R["wood"][2], "s": R["skin"][2], "S": R["skin"][3], "d": R["skin"][1],
         "b": R["cloth"][1], "B": R["cloth"][2], "n": R["cloth"][0], "y": R["cloth"][3], "w": R["cloth"][4], "l": R["wood"][0]}
    body = [
        "....oooooo......", "...ohHHHHho.....", "..ohHHHHHHho....", "..ohHsSSsHho....", "..ohsSSSSsho....",
        "..odSoSSoSdo....", "..odSSSSSSdo....", "...odSddSdo.....", "....osSSso......", "...onBBBBno.....",
        "..onBBByBBno....", ".onBBBByBBBno...", ".osnBBByBBnso...", ".osnBBBBBBnso...", ".osonBBBBnoso...",
        "..o.onnnno.o....",
    ]
    stand = ["....obBBbo", "....obBbBo", "....ob.obo", "....ol.olo", "...oll.ollo", "...ooo.oooo"]
    step_a = ["....obBBbo", "...obBbbBo", "..obb..obo", "..oll..olo", "..ooo..ollo", ".......oooo"]
    step_b = ["....obBBbo", "....obBbbBo", "....obo..bbo", "....olo..llo", "...ollo..ooo", "...oooo"]
    # Walk cycle as a 4-frame sheet: stand, step, stand, other step.
    frames = [body + [r.ljust(16, ".")[:16] for r in legs] for legs in (stand, step_a, stand, step_b)]
    sprite("hero.png", ["".join(f[y] for f in frames) for y in range(len(frames[0]))], k)
    f = {"g": R["grass"][1], "G": R["grass"][3], "r": R["roof"][3], "R": R["roof"][4], "y": R["cloth"][3], "o": R["grass"][0]}
    sprite("flower.png", ["...rR...", "..rRyR..", "...rRr..", "....G...", ".g..G...", ".Gg.G.g.", "..GgGgG.", "...oGo.."], f)

# ------------------------------------------------------------------ scene

def look_at(eye, target):
    """Quaternion (x, y, z, w) for a camera at `eye` looking at `target`, +Y up."""
    f = [t - e for e, t in zip(eye, target)]; n = math.sqrt(sum(c * c for c in f)); f = [c / n for c in f]
    r = [f[1] * 0 - f[2] * 1, f[2] * 0 - f[0] * 0, f[0] * 1 - f[1] * 0]          # f x up
    n = math.sqrt(sum(c * c for c in r)); r = [c / n for c in r]
    u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]]
    m = [[r[0], u[0], -f[0]], [r[1], u[1], -f[1]], [r[2], u[2], -f[2]]]          # columns: right, up, back
    w = math.sqrt(max(0.0, 1 + m[0][0] + m[1][1] + m[2][2])) / 2
    return [(m[2][1] - m[1][2]) / (4 * w), (m[0][2] - m[2][0]) / (4 * w), (m[1][0] - m[0][1]) / (4 * w), w]

def yaw(deg): return [0, math.sin(math.radians(deg) / 2), 0, math.cos(math.radians(deg) / 2)]

def scene():
    E = []
    def ent(name, pos=(0, 0, 0), rot=(0, 0, 0, 1), scale=(1, 1, 1), parent=-1, **components):
        c = {"Transform": {"position": list(pos), "rotation": list(rot), "scale": list(scale)}}
        c.update(components); E.append({"name": name, "parent": parent, "components": c}); return len(E) - 1
    SOLID = {"Collider": {"size": [1, 1, 1], "offset": [0, 0, 0], "solid": True}}
    def box(name, tex, pos, scale, rot=(0, 0, 0, 1), mesh="builtin:cube", parent=-1):
        solid = SOLID if mesh == "builtin:cube" else {}   # ground planes are walked on, not into
        return ent(name, pos, rot, scale, parent, MeshRenderer={"mesh": mesh, "texture": "assets/" + tex, "color": [1, 1, 1, 1],
                                                                "emissive": [0, 0, 0], "worldTexels": True}, **solid)
    eye, target = (10.5, 6.5, 12.5), (0.0, 1.0, 0.0)
    ent("Camera", eye, look_at(eye, target), Camera={"orthographic": False, "fov": 32, "orthoSize": 5, "nearPlane": 0.1, "farPlane": 120},
        Script={"path": "scripts/camera_follow.lua"})
    sun = (-0.45, -0.75, -0.5)
    ent("Sun", rot=look_at((0, 0, 0), sun), Light={"color": [1, 0.95, 0.82], "intensity": 3.4})
    box("Ground", "grass.png", (0, 0, 0), (30, 1, 30), mesh="builtin:plane")
    box("Path", "dirt.png", (1.5, 0.01, 2.5), (2, 1, 14), mesh="builtin:plane")
    box("Path Branch", "dirt.png", (-1.5, 0.012, 3.0), (5, 1, 2), mesh="builtin:plane")
    house = ent("House", (-3.5, 0, -2))
    box("Walls", "bricks.png", (0, 1.25, 0), (4, 2.5, 3.5), parent=house)
    box("Roof", "shingles.png", (0, 2.75, 0), (4.6, 0.5, 4.1), parent=house)
    box("Roof Top", "shingles.png", (0, 3.25, 0), (3.4, 0.5, 2.9), parent=house)
    box("Door", "planks.png", (0.6, 0.9, 1.78), (0.9, 1.8, 0.1), parent=house)
    box("Chimney", "bricks.png", (1.2, 3.6, -0.6), (0.6, 1.2, 0.6), parent=house)
    for i, (x, z) in enumerate([(5.5, -4.5), (7.5, -4.5), (9.5, -4.5)]):
        box("Wall %d" % (i + 1), "bricks.png", (x, 0.75, z), (2, 1.5, 0.5))
    box("Crate", "planks.png", (3.6, 0.5, 0.2), (1, 1, 1), yaw(15))
    box("Crate Small", "planks.png", (4.5, 0.3, 1.0), (0.6, 0.6, 0.6), yaw(-25))
    box("Crate Top", "planks.png", (3.6, 1.3, 0.2), (0.6, 0.6, 0.6), yaw(40))
    ent("Lantern", (0.3, 0.3, 0.6), scale=(0.25, 0.6, 0.25),
        MeshRenderer={"mesh": "builtin:cube", "texture": "", "color": [1, 0.84, 0.35, 1], "emissive": [3.5, 2.2, 0.7], "worldTexels": False})
    for i, (x, z, s) in enumerate([(-8, -6, 1.6), (-9.5, 1.5, 1.3), (7, 4.5, 1.4), (2.5, -8, 1.8), (-5.5, 6.5, 1.1), (10, -8.5, 1.7)]):
        ent("Pine %d" % (i + 1), (x, 0, z), scale=(s, s, s),
            MeshRenderer={"mesh": "assets/pine.gltf", "texture": "", "color": [1, 1, 1, 1], "emissive": [0, 0, 0], "worldTexels": False},
            Collider={"size": [0.5, 2, 0.5], "offset": [0, 1, 0], "solid": True})   # just the trunk
    # 16x22 texel sprite at 16 texels per unit, feet on the ground.
    ent("Hero", (1.6, 0.6875, 3.2), SpriteRenderer={"texture": "assets/hero.png", "color": [1, 1, 1, 1], "size": [1.0, 1.375], "billboard": True,
                                                 "columns": 4, "rows": 1, "frame": 0, "fps": 0, "flipX": False},
        Script={"path": "scripts/hero.lua"}, Collider={"size": [0.5, 1.375, 0.3], "offset": [0, 0, 0], "solid": True})
    for i, (x, z) in enumerate([(-0.5, 5.2), (3.4, 4.0), (-2.2, 1.6), (5.8, 1.8), (0.2, -2.5)]):
        ent("Flower %d" % (i + 1), (x, 0.25, z), SpriteRenderer={"texture": "assets/flower.png", "color": [1, 1, 1, 1], "size": [0.5, 0.5], "billboard": True})
    settings = {"pixelArt": True, "pixelHeight": 216, "toonBands": 3, "dither": 0.6, "texelLighting": True, "texelsPerUnit": 16,
                "palette": "assets/palette.png", "outlines": True, "outlineStrength": 0.45, "highlightStrength": 0.0,
                "ambient": [0.36, 0.38, 0.55], "skyTop": [0.36, 0.52, 0.77], "skyBottom": [0.90, 0.95, 0.97]}
    json.dump({"version": 1, "settings": settings, "entities": E}, open(os.path.join(ROOT, "scenes", "village.scene.json"), "w"), indent=2)

if __name__ == "__main__":
    art()
    scene()
    print("wrote art to", os.path.normpath(ART))
