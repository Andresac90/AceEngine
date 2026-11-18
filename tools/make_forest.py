#!/usr/bin/env python3
"""Generates projects/forest: an open-world forest at sunset, walked in first
person, with a castle on a hill that you can reach and enter.

Everything is generated (deterministic seeds), so the art can be tuned here
and regenerated:   python3 tools/make_forest.py

Writes: palette, textures, glTF meshes (terrain, trees, rocks, castle...) with
flat-shaded faces and vertex colors, the scene and the project file.
Scripts in projects/forest/scripts are hand-written and left alone.
"""
import base64, json, math, os, random, struct
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "projects", "forest")
ART = os.path.join(ROOT, "assets")

# ------------------------------------------------------------------ palette
# One ramp per row, darkest first (ramp shading walks along a row).
RAMPS = {
    "leaf":  ["#0b1712", "#13261a", "#1d3a22", "#2c5229", "#436b2f", "#64873a", "#8fa449"],
    "grass": ["#15200f", "#223316", "#344b1c", "#4c6522", "#6b8030", "#909c3e", "#b9b65a"],
    "bark":  ["#140d0c", "#24160f", "#3a2516", "#553620", "#71492b", "#906339", "#b4834f"],
    "stone": ["#121219", "#1f2029", "#33333d", "#4a4950", "#666169", "#887f80", "#b0a39b"],
    "slate": ["#0d0f1f", "#171b33", "#232a4a", "#33405f", "#4b5a78", "#6b7a92", "#97a2b0"],
    "fire":  ["#3a0f0c", "#6e1f10", "#a83c13", "#d8661a", "#f39a2c", "#ffc94f", "#fff1a8"],
    "dusk":  ["#0e0a1c", "#1f1532", "#3b2248", "#62305a", "#91425f", "#c4625e", "#eb9a6e"],
    "sky":   ["#1a2340", "#2b3a63", "#455a86", "#6b80a6", "#9aa7c0", "#d7b5a4", "#f6d6a8"],
}

def hexrgb(h): return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))
def lin(c): return [((v / 255) ** 2.2) for v in c]          # sRGB -> linear (glTF colors are linear)
R = {k: [hexrgb(c) for c in v] for k, v in RAMPS.items()}

def palette():
    img = Image.new("RGBA", (7, len(R)))
    for y, ramp in enumerate(R.values()):
        for x, c in enumerate(ramp): img.putpixel((x, y), c + (255,))
    img.save(os.path.join(ART, "palette.png"))

def detail_tile(name, seed, lo, hi, clumps):
    """Neutral gray speckle: multiplies the vertex colors, so one tile serves every surface."""
    rnd = random.Random(seed)
    img = Image.new("RGBA", (32, 32))
    for y in range(32):
        for x in range(32):
            v = int(255 * rnd.choice([lo, (lo + hi) / 2, hi, hi, hi]))
            img.putpixel((x, y), (v, v, v, 255))
    for _ in range(clumps):
        cx, cy = rnd.randrange(32), rnd.randrange(32)
        v = int(255 * lo * 0.9)
        for dx, dy in [(0, 0), (1, 0), (0, 1), (1, 1), (-1, 0)][: rnd.randrange(2, 6)]:
            img.putpixel(((cx + dx) % 32, (cy + dy) % 32), (v, v, v, 255))
    img.save(os.path.join(ART, name))

def stone_blocks(name):
    """Gray masonry, neutral so vertex colors tint it."""
    rnd = random.Random(11)
    img = Image.new("RGBA", (32, 32))
    for y in range(32):
        row, off = y // 8, (y // 8) % 2 * 8
        for x in range(32):
            col = (x + off) // 16
            mortar = y % 8 == 7 or (x + off) % 16 == 15
            base = 0.62 if mortar else 0.86 + 0.1 * ((row * 7 + col * 3) % 3 - 1)
            v = base + rnd.uniform(-0.05, 0.05)
            v = int(255 * max(0, min(1, v)))
            img.putpixel((x, y), (v, v, v, 255))
    img.save(os.path.join(ART, name))

def fern_sprite():
    """16x16 fern billboard (cutout)."""
    g = R["leaf"]
    rows = [
        "................", ".......c........", "......cb.....c..", "..c...bc....cb..",
        "..bc..cb...cb...", "...bc.bc..cb..c.", "....bcbc.cb..cb.", ".c...bbccb..cb..",
        ".bc...bbbcccb...", "..bbc..bbbbb..c.", "...bbbc.bbb.cbb.", ".....bbbbbbbb...",
        "..c....bbbb.....", "...bbb.bbb......", ".....bbbaa......", ".......aa.......",
    ]
    key = {"a": g[1], "b": g[3], "c": g[5]}
    img = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    for y, r in enumerate(rows):
        for x, ch in enumerate(r):
            if ch in key: img.putpixel((x, y), key[ch] + (255,))
    img.save(os.path.join(ART, "fern.png"))

# ------------------------------------------------------------------ meshes
class Mesh:
    """Flat-shaded triangles with per-face colors (linear RGB)."""
    def __init__(self): self.p, self.n, self.c = [], [], []
    def tri(self, a, b, c, color):
        u = [b[i] - a[i] for i in range(3)]; v = [c[i] - a[i] for i in range(3)]
        n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
        l = math.sqrt(sum(x * x for x in n))
        if l < 1e-9: return
        n = [x / l for x in n]
        for p in (a, b, c):
            self.p.append(p); self.n.append(n); self.c.append(list(color) + [1.0])
    def tri_smooth(self, ps, ns, cs):
        for p, n, c in zip(ps, ns, cs):
            self.p.append(p); self.n.append(n); self.c.append(list(c) + [1.0])
    def quad(self, a, b, c, d, color): self.tri(a, b, c, color); self.tri(a, c, d, color)
    def tris(self): return len(self.p) // 3

    def box(self, center, size, color, top=None, jitter=None):
        cx, cy, cz = center; hx, hy, hz = [s / 2 for s in size]
        P = [[cx + sx * hx, cy + sy * hy, cz + sz * hz] for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
        if jitter:
            for q in P:
                for i in range(3): q[i] += jitter.uniform(-0.08, 0.08) * size[i]
        i = lambda sx, sy, sz: P[(sx > 0) * 4 + (sy > 0) * 2 + (sz > 0)]
        t = top or color
        self.quad(i(-1, 1, -1), i(-1, 1, 1), i(1, 1, 1), i(1, 1, -1), t)        # +Y
        self.quad(i(-1, -1, -1), i(1, -1, -1), i(1, -1, 1), i(-1, -1, 1), color)  # -Y
        self.quad(i(-1, -1, 1), i(1, -1, 1), i(1, 1, 1), i(-1, 1, 1), color)      # +Z
        self.quad(i(1, -1, -1), i(-1, -1, -1), i(-1, 1, -1), i(1, 1, -1), color)  # -Z
        self.quad(i(1, -1, 1), i(1, -1, -1), i(1, 1, -1), i(1, 1, 1), color)      # +X
        self.quad(i(-1, -1, -1), i(-1, -1, 1), i(-1, 1, 1), i(-1, 1, -1), color)  # -X

    def prism(self, base, radius, height, sides, color, top_radius=None, cap=True, rot=0.0, colors=None):
        """Vertical n-gon prism (or frustum / cone when top_radius is 0)."""
        tr = radius if top_radius is None else top_radius
        bx, by, bz = base
        ring = lambda r, y: [[bx + r * math.cos(rot + 2 * math.pi * k / sides), y, bz + r * math.sin(rot + 2 * math.pi * k / sides)] for k in range(sides)]
        lo, hi = ring(radius, by), ring(tr, by + height)
        for k in range(sides):
            col = colors[k % len(colors)] if colors else color
            a, b = lo[k], lo[(k + 1) % sides]
            if tr <= 1e-6: self.tri(a, [bx, by + height, bz], b, col)
            else: self.quad(a, hi[k], hi[(k + 1) % sides], b, col)
        if cap:
            for k in range(1, sides - 1): self.tri(lo[0], lo[k], lo[k + 1], color)
            if tr > 1e-6:
                for k in range(1, sides - 1): self.tri(hi[0], hi[k + 1], hi[k], color)

    def blob(self, center, radius, color_fn, rnd, squash=1.0, rough=0.18):
        """Lumpy low-poly ball: icosahedron subdivided once, vertices pushed in/out."""
        t = (1 + 5 ** 0.5) / 2
        V = [[-1, t, 0], [1, t, 0], [-1, -t, 0], [1, -t, 0], [0, -1, t], [0, 1, t], [0, -1, -t], [0, 1, -t], [t, 0, -1], [t, 0, 1], [-t, 0, -1], [-t, 0, 1]]
        F = [[0, 11, 5], [0, 5, 1], [0, 1, 7], [0, 7, 10], [0, 10, 11], [1, 5, 9], [5, 11, 4], [11, 10, 2], [10, 7, 6], [7, 1, 8],
             [3, 9, 4], [3, 4, 2], [3, 2, 6], [3, 6, 8], [3, 8, 9], [4, 9, 5], [2, 4, 11], [6, 2, 10], [8, 6, 7], [9, 8, 1]]
        V = [[c / math.sqrt(sum(x * x for x in v)) for c in v] for v in V]
        mid = {}
        def m(a, b):
            k = (min(a, b), max(a, b))
            if k not in mid:
                v = [(V[a][i] + V[b][i]) / 2 for i in range(3)]; l = math.sqrt(sum(x * x for x in v))
                V.append([x / l for x in v]); mid[k] = len(V) - 1
            return mid[k]
        F2 = []
        for a, b, c in F:
            ab, bc, ca = m(a, b), m(b, c), m(c, a)
            F2 += [[a, ab, ca], [b, bc, ab], [c, ca, bc], [ab, bc, ca]]
        P = [[center[0] + v[0] * radius * s, center[1] + v[1] * radius * squash * s, center[2] + v[2] * radius * s]
             for v in V for s in [1 + rnd.uniform(-rough, rough)]]
        for a, b, c in F2:
            ny = (V[a][1] + V[b][1] + V[c][1]) / 3
            self.tri(P[a], P[c], P[b], color_fn(ny, rnd))

def write_gltf(mesh, name, uvs=False):
    """Minimal glTF 2.0 with an embedded buffer: POSITION, NORMAL, COLOR_0 (non-indexed)."""
    n = len(mesh.p)
    pos = b"".join(struct.pack("<3f", *p) for p in mesh.p)
    nrm = b"".join(struct.pack("<3f", *v) for v in mesh.n)
    col = b"".join(struct.pack("<4f", *c) for c in mesh.c)
    buf = pos + nrm + col
    lo = [min(p[i] for p in mesh.p) for i in range(3)]; hi = [max(p[i] for p in mesh.p) for i in range(3)]
    views = [{"buffer": 0, "byteOffset": 0, "byteLength": len(pos)},
             {"buffer": 0, "byteOffset": len(pos), "byteLength": len(nrm)},
             {"buffer": 0, "byteOffset": len(pos) + len(nrm), "byteLength": len(col)}]
    acc = [{"bufferView": 0, "componentType": 5126, "count": n, "type": "VEC3", "min": lo, "max": hi},
           {"bufferView": 1, "componentType": 5126, "count": n, "type": "VEC3"},
           {"bufferView": 2, "componentType": 5126, "count": n, "type": "VEC4"}]
    g = {"asset": {"version": "2.0", "generator": "AceEngine make_forest.py"}, "scene": 0, "scenes": [{"nodes": [0]}],
         "nodes": [{"mesh": 0}], "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "COLOR_0": 2}}]}],
         "buffers": [{"byteLength": len(buf), "uri": "data:application/octet-stream;base64," + base64.b64encode(buf).decode()}],
         "bufferViews": views, "accessors": acc}
    json.dump(g, open(os.path.join(ART, name), "w"))
    return mesh.tris()

# ------------------------------------------------------------------ world layout
SIZE, CELLS = 640.0, 100           # terrain: 640 x 640 units, 6.4-unit cells
CASTLE = (0.0, -175.0)             # castle center (x, z)
CASTLE_R = 34.0                    # flat plateau radius
CIRCLE = (-28.0, 68.0)             # standing stones and campfire
START = (-14.0, 62.0)
PATH = [(12, 178), (10, 150), (6, 128), (-6, 108), (-16, 84), (-14, 50), (4, 18), (18, -20), (14, -62), (2, -100), (0, -128), (0, -148)]

def hash2(ix, iz, seed):
    h = (ix * 374761393 + iz * 668265263 + seed * 2147483647) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0

def vnoise(x, z, seed):
    ix, iz = math.floor(x), math.floor(z); fx, fz = x - ix, z - iz
    ux, uz = fx * fx * (3 - 2 * fx), fz * fz * (3 - 2 * fz)
    a, b = hash2(ix, iz, seed), hash2(ix + 1, iz, seed)
    c, d = hash2(ix, iz + 1, seed), hash2(ix + 1, iz + 1, seed)
    return (a + (b - a) * ux) * (1 - uz) + (c + (d - c) * ux) * uz

def fbm(x, z, seed, octaves=4):
    v, a, f = 0.0, 0.5, 1.0
    for o in range(octaves):
        v += a * vnoise(x * f, z * f, seed + o); a *= 0.5; f *= 2.03
    return v

def path_distance(x, z):
    best = 1e9
    for (ax, az), (bx, bz) in zip(PATH, PATH[1:]):
        dx, dz = bx - ax, bz - az
        t = max(0.0, min(1.0, ((x - ax) * dx + (z - az) * dz) / (dx * dx + dz * dz)))
        best = min(best, math.hypot(x - ax - dx * t, z - az - dz * t))
    return best

def smooth(e0, e1, x):
    t = max(0.0, min(1.0, (x - e0) / (e1 - e0))); return t * t * (3 - 2 * t)

CASTLE_H = 32.0
def height(x, z):
    h = (fbm(x / 70, z / 70, 1) - 0.5) * 14 + (fbm(x / 18, z / 18, 7) - 0.5) * 3.0
    # The castle hill: a broad rise topped by a flat plateau.
    dc = math.hypot(x - CASTLE[0], z - CASTLE[1])
    hill = CASTLE_H * (1 - smooth(CASTLE_R, CASTLE_R + 70, dc))
    h = h * smooth(CASTLE_R, CASTLE_R + 40, dc) + hill
    # Mountains close the world in: past 230 units the ground climbs steeply.
    edge = max(abs(x), abs(z))
    h += smooth(225, 310, edge) * (55 + fbm(x / 30, z / 30, 3) * 40)
    # Clearing around the standing stones.
    dcirc = math.hypot(x - CIRCLE[0], z - CIRCLE[1])
    base = (fbm(CIRCLE[0] / 70, CIRCLE[1] / 70, 1) - 0.5) * 14
    h = base + (h - base) * smooth(12, 24, dcirc)
    # The road: a little sunken and smoothed.
    return h

def ground_color(x, z, ny):
    grass, stone = R["grass"], R["stone"]
    n = fbm(x / 9, z / 9, 21)
    if ny < 0.78: return stone[3 + (n > 0.5)]                                     # steep: bare rock
    if math.hypot(x - CASTLE[0], z - CASTLE[1]) < CASTLE_R - 6: return stone[4] if n > 0.55 else grass[3]
    if math.hypot(x - CIRCLE[0], z - CIRCLE[1]) < 11: return R["bark"][3] if n > 0.6 else grass[2]
    return grass[3] if n < 0.4 else (grass[4] if n < 0.6 else grass[2])

def terrain():
    """Smooth-shaded height grid; colors per vertex (slope, clearing, castle yard)."""
    m = Mesh()
    step = SIZE / CELLS
    P, N, C = {}, {}, {}
    for iz in range(CELLS + 1):
        for ix in range(CELLS + 1):
            x, z = -SIZE / 2 + ix * step, -SIZE / 2 + iz * step
            y = height(x, z)
            dx = (height(x + 1, z) - height(x - 1, z)) / 2; dz = (height(x, z + 1) - height(x, z - 1)) / 2
            n = [-dx, 1.0, -dz]; l = math.sqrt(sum(q * q for q in n)); n = [q / l for q in n]
            P[ix, iz], N[ix, iz], C[ix, iz] = [x, y, z], n, lin(ground_color(x, z, n[1]))
    for iz in range(CELLS):
        for ix in range(CELLS):
            for k in (((ix, iz), (ix, iz + 1), (ix + 1, iz + 1)), ((ix, iz), (ix + 1, iz + 1), (ix + 1, iz))):
                m.tri_smooth([P[q] for q in k], [N[q] for q in k], [C[q] for q in k])
    return m

def road():
    """A strip draped just above the terrain along PATH, 1.5 units per step."""
    m = Mesh(); dirt = R["bark"]
    pts = []
    for (ax, az), (bx, bz) in zip(PATH, PATH[1:]):
        n = max(1, int(math.hypot(bx - ax, bz - az) / 1.5))
        pts += [(ax + (bx - ax) * t / n, az + (bz - az) * t / n) for t in range(n)]
    pts.append(PATH[-1])
    W = 1.9
    def edge(i, side):
        x, z = pts[i]; j0, j1 = max(0, i - 1), min(len(pts) - 1, i + 1)
        dx, dz = pts[j1][0] - pts[j0][0], pts[j1][1] - pts[j0][1]; L = math.hypot(dx, dz)
        ex, ez = x - dz / L * W * side, z + dx / L * W * side
        return [ex, height(ex, ez) + 0.12, ez]
    for i in range(len(pts) - 1):
        a, b, c, d = edge(i, -1), edge(i, 1), edge(i + 1, 1), edge(i + 1, -1)
        col = lin(dirt[4] if fbm(pts[i][0] / 4, pts[i][1] / 4, 9) > 0.45 else dirt[3])
        m.tri_smooth([a, d, c], [[0, 1, 0]] * 3, [col] * 3)
        m.tri_smooth([a, c, b], [[0, 1, 0]] * 3, [col] * 3)
    return m

# ------------------------------------------------------------------ props
def pine(seed, dead=False):
    rnd = random.Random(seed); m = Mesh()
    trunk_h = rnd.uniform(9, 12) if not dead else rnd.uniform(8, 11)
    m.prism([0, -0.3, 0], 0.32, trunk_h, 6, lin(R["bark"][2]), top_radius=0.12, colors=[lin(R["bark"][2]), lin(R["bark"][3])])
    if dead:
        for k in range(5):
            y = rnd.uniform(3, trunk_h - 1); ang = rnd.uniform(0, 6.28); L = rnd.uniform(1.0, 2.2)
            dx, dz = math.cos(ang), math.sin(ang)
            m.box([dx * L / 2, y, dz * L / 2], [abs(dx) * L + 0.12, 0.12, abs(dz) * L + 0.12], lin(R["stone"][3]))
        return m
    tiers = rnd.randint(4, 5); y = 2.2; r = rnd.uniform(2.6, 3.2)
    for k in range(tiers):
        h = rnd.uniform(3.0, 3.6)
        cols = [lin(R["leaf"][rnd.choice([1, 2, 2, 3])]) for _ in range(8)]
        m.prism([rnd.uniform(-.15, .15), y, rnd.uniform(-.15, .15)], r, h, 8, lin(R["leaf"][1]), top_radius=0, rot=rnd.uniform(0, 1), colors=cols)
        y += h * 0.55; r *= 0.76
    return m

def broadleaf(seed):
    rnd = random.Random(seed); m = Mesh()
    h = rnd.uniform(4.0, 5.5)
    m.prism([0, -0.3, 0], 0.45, h, 6, lin(R["bark"][2]), top_radius=0.25, colors=[lin(R["bark"][2]), lin(R["bark"][3])])
    leaf = R["leaf"]
    def color(ny, rnd): return lin(leaf[min(6, max(1, int(3 + ny * 1.6 + rnd.uniform(-0.8, 0.8))))])
    for k in range(4):
        a = rnd.uniform(0, 6.28); d = 0 if k == 0 else rnd.uniform(1.4, 2.4)
        m.blob([math.cos(a) * d, h + rnd.uniform(0.5, 2.5), math.sin(a) * d], rnd.uniform(2.0, 3.0), color, rnd, squash=0.8)
    return m

def rock(seed):
    rnd = random.Random(seed); m = Mesh(); st = R["stone"]
    m.blob([0, 0.3, 0], 1.0, lambda ny, r: lin(st[min(6, 3 + int(ny > 0.3) + int(r.random() < 0.3))]) if ny < 0.55 else lin(R["leaf"][3]), rnd, squash=0.6, rough=0.3)
    return m

def log():
    rnd = random.Random(5); m = Mesh(); M2 = Mesh()
    m.prism([0, 0, 0], 0.45, 7.0, 7, lin(R["bark"][2]), colors=[lin(R["bark"][c]) for c in (2, 3, 2, 1, 3, 2, 3)])
    # Lying down: swap y and x.
    for p in m.p: p[0], p[1] = p[1] - 3.5, p[0] + 0.4
    for n in m.n: n[0], n[1] = n[1], n[0]
    return m

def standing_stone(seed):
    rnd = random.Random(seed); m = Mesh(); st = R["stone"]
    h = rnd.uniform(3.2, 4.6)
    m.box([0, h / 2 - 0.4, 0], [1.3, h, 0.7], lin(st[3]), top=lin(R["leaf"][3]), jitter=rnd)
    return m

def campfire():
    m = Mesh()
    for k in range(5):
        a = k * 2 * math.pi / 5
        m.box([math.cos(a) * 1.0, 0.1, math.sin(a) * 1.0], [0.6, 0.35, 0.5], lin(R["stone"][3]))
    for k in range(3):
        a = k * math.pi / 3
        m.box([0, 0.2, 0], [1.6 * abs(math.cos(a)) + 0.2, 0.2, 1.6 * abs(math.sin(a)) + 0.2], lin(R["bark"][2]))
    return m

def flames():
    m = Mesh(); rnd = random.Random(9)
    for k in range(4):
        a = k * 1.7
        m.prism([math.cos(a) * 0.2, 0.25, math.sin(a) * 0.2], 0.35, rnd.uniform(0.9, 1.4), 5, lin(R["fire"][5]), top_radius=0)
    return m

# ------------------------------------------------------------------ castle
# Square curtain wall with round corner towers, a gatehouse facing the road
# (+Z, towards the forest) and a tall keep at the back. Colliders are separate
# entities; this mesh is only what you see.
WALL = 52.0          # outer wall length
WALL_H, WALL_T = 11.0, 2.4
GATE_W, GATE_H = 6.0, 7.0

def castle():
    m = Mesh(); st, sl = R["stone"], R["slate"]
    rnd = random.Random(42)
    wall = lambda: lin(st[rnd.choice([3, 4, 4])])
    half = WALL / 2
    def merlons(x0, z0, x1, z1, y):
        L = math.hypot(x1 - x0, z1 - z0); n = int(L / 2.4)
        for k in range(n):
            t = (k + 0.5) / n
            m.box([x0 + (x1 - x0) * t, y + 0.7, z0 + (z1 - z0) * t], [1.2 if x1 != x0 else WALL_T, 1.4, 1.2 if z1 != z0 else WALL_T], wall())
    # Walls (front split by the gate).
    for z in (-half, half):
        if z > 0:
            seg = (half - GATE_W / 2) / 2 + GATE_W / 2
            for sx in (-1, 1):
                m.box([sx * seg, WALL_H / 2, z], [half - GATE_W / 2, WALL_H, WALL_T], wall())
            m.box([0, (WALL_H + GATE_H) / 2, z], [GATE_W, WALL_H - GATE_H, WALL_T], wall())
        else:
            m.box([0, WALL_H / 2, z], [WALL, WALL_H, WALL_T], wall())
        merlons(-half, z, half, z, WALL_H)
    for x in (-half, half):
        m.box([x, WALL_H / 2, 0], [WALL_T, WALL_H, WALL], wall())
        merlons(x, -half, x, half, WALL_H)
    # Corner towers with slate roofs.
    for sx in (-1, 1):
        for sz in (-1, 1):
            m.prism([sx * half, -3, sz * half], 4.2, WALL_H + 9, 10, wall(), colors=[lin(st[c]) for c in (3, 4, 3, 4, 4)])
            m.prism([sx * half, WALL_H + 6, sz * half], 5.0, 9.0, 10, lin(sl[2]), top_radius=0, colors=[lin(sl[c]) for c in (2, 3, 2, 3)])
    # Gatehouse towers flanking the gate.
    for sx in (-1, 1):
        m.box([sx * (GATE_W / 2 + 2.5), (WALL_H + 5) / 2 - 1, half + 1.0], [5.0, WALL_H + 5, 5.0], wall())
        m.prism([sx * (GATE_W / 2 + 2.5), WALL_H + 4, half + 1.0], 3.6, 6.0, 4, lin(sl[3]), top_radius=0, rot=math.pi / 4)
    # Keep: a big block, a tall tower and a steep roof.
    m.box([0, 9, -10], [20, 22, 16], wall())
    merlons(-10, -2, 10, -2, 20); merlons(-10, -18, 10, -18, 20)
    m.prism([6, 0, -12], 4.0, 34, 10, wall(), colors=[lin(st[c]) for c in (3, 4, 4, 3, 5)])
    m.prism([6, 33, -12], 4.8, 12, 10, lin(sl[2]), top_radius=0, colors=[lin(sl[c]) for c in (2, 3, 2, 3)])
    m.prism([-4, 20, -10], 9.5, 7, 4, lin(sl[2]), top_radius=0, rot=math.pi / 4, colors=[lin(sl[c]) for c in (2, 3)])
    # Steps up to the keep door.
    for k in range(3):
        m.box([0, 0.25 + k * 0.5, -1.0 + 1.0 - k * 0.6], [6, 0.5, 1.8 - k * 0.6], lin(st[4]))
    return m

def castle_windows():
    """Warm lit windows (emissive entity)."""
    m = Mesh(); c = [1, 1, 1]
    for x in (-6, -2, 2):
        m.box([x, 14, -1.9], [1.0, 2.0, 0.3], c)
    m.box([6, 26, -7.9], [1.0, 2.0, 0.3], c)
    m.box([6, 18, -7.9], [1.0, 2.0, 0.3], c)
    m.box([0, 2.0, -1.85], [3.0, 4.0, 0.3], c)   # the keep door, light spilling out
    return m

# ------------------------------------------------------------------ scene
def look_at(eye, target):
    f = [t - e for e, t in zip(eye, target)]; n = math.sqrt(sum(c * c for c in f)); f = [c / n for c in f]
    r = [-f[2], 0.0, f[0]]; n = math.sqrt(sum(c * c for c in r)); r = [c / n for c in r]
    u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]]
    m = [[r[0], u[0], -f[0]], [r[1], u[1], -f[1]], [r[2], u[2], -f[2]]]
    w = math.sqrt(max(1e-9, 1 + m[0][0] + m[1][1] + m[2][2])) / 2
    return [(m[2][1] - m[1][2]) / (4 * w), (m[0][2] - m[2][0]) / (4 * w), (m[1][0] - m[0][1]) / (4 * w), w]

def yaw(deg): return [0, math.sin(math.radians(deg) / 2), 0, math.cos(math.radians(deg) / 2)]

def scene():
    E = []
    def ent(name, pos=(0, 0, 0), rot=(0, 0, 0, 1), scale=(1, 1, 1), **components):
        E.append({"name": name, "parent": -1, "components": dict(Transform={"position": list(pos), "rotation": list(rot), "scale": list(scale)}, **components)})
    def mesh(path, color=(1, 1, 1, 1), emissive=(0, 0, 0), texture="assets/ground.png", texels=True):
        return {"mesh": path, "texture": texture, "color": list(color), "emissive": list(emissive), "worldTexels": texels}
    def collider(size, offset=(0, 0, 0)): return {"size": list(size), "offset": list(offset), "solid": True}

    gh = height(*START)
    ent("Player", (START[0], gh + 1.7, START[1]), yaw(0),
        Camera={"orthographic": False, "fov": 70, "orthoSize": 5, "nearPlane": 0.1, "farPlane": 600},
        Script={"path": "scripts/first_person.lua"},
        Collider={"size": [0.6, 1.6, 0.6], "offset": [0, -0.9, 0], "solid": True})
    # Low sun behind the castle, a little to the left: long shadows towards the player.
    sun_dir = [-0.42, 0.2, -0.88]
    ent("Sun", (0, 0, 0), look_at(sun_dir, (0, 0, 0)), Light={"color": [1.0, 0.66, 0.38], "intensity": 3.4, "point": False, "range": 10})
    ent("Terrain", (0, 0, 0), **{"MeshRenderer": mesh("assets/terrain.gltf")})
    ent("Road", (0, 0, 0), **{"MeshRenderer": mesh("assets/road.gltf")})
    ent("HUD", Script={"path": "scripts/hud.lua"})

    rnd = random.Random(7)
    placed = []
    def free(x, z, r):
        if math.hypot(x - CASTLE[0], z - CASTLE[1]) < CASTLE_R + 4: return False
        if path_distance(x, z) < 6.0 + r: return False
        if math.hypot(x - CIRCLE[0], z - CIRCLE[1]) < 16: return False
        if math.hypot(x - START[0], z - START[1]) < 6: return False
        return all(math.hypot(x - px, z - pz) > r + pr for px, pz, pr in placed)
    def scatter(count, r, density_fn, place, limit=235):
        tries = 0; n = 0
        while n < count and tries < count * 60:
            tries += 1
            x, z = rnd.uniform(-limit, limit), rnd.uniform(-limit, limit)
            if rnd.random() > density_fn(x, z) or not free(x, z, r): continue
            placed.append((x, z, r)); place(x, z); n += 1

    # Denser forest near the road, thinning on the castle hill.
    # Thick forest within ~60 units of the road, thinning into clearings further out
    # and on the castle hill (open ground, so the castle stays visible).
    def forest(x, z):
        near = 1 - smooth(25, 90, path_distance(x, z))
        hill = smooth(CASTLE_R + 10, CASTLE_R + 60, math.hypot(x - CASTLE[0], z - CASTLE[1]))
        return (0.15 + 0.85 * near) * hill * smooth(0.3, 0.55, fbm(x / 35, z / 35, 31))
    counter = {"pine": 0}
    def put_tree(x, z):
        k = rnd.random(); s = rnd.uniform(0.8, 1.25)
        kind = "pine_%d" % rnd.randrange(3) if k < 0.8 else ("broadleaf_%d" % rnd.randrange(2) if k < 0.92 else "deadpine")
        counter["pine"] += 1
        ent("Tree", (x, height(x, z), z), yaw(rnd.uniform(0, 360)), (s, s, s),
            MeshRenderer=mesh("assets/%s.gltf" % kind), Collider=collider([0.9, 6, 0.9], (0, 3, 0)))
    scatter(820, 2.2, forest, put_tree, limit=230)
    def put_rock(x, z):
        s = rnd.uniform(0.7, 2.6)
        ent("Rock", (x, height(x, z) - 0.2 * s, z), yaw(rnd.uniform(0, 360)), (s, s * rnd.uniform(0.7, 1.1), s),
            MeshRenderer=mesh("assets/rock_%d.gltf" % rnd.randrange(3)), Collider=collider([1.6, 1.0, 1.6], (0, 0.3, 0)))
    scatter(70, 2.0, lambda x, z: 0.8, put_rock)
    def put_log(x, z):
        ent("Fallen log", (x, height(x, z), z), yaw(rnd.uniform(0, 360)), MeshRenderer=mesh("assets/log.gltf"), Collider=collider([7, 1, 1], (0, 0.4, 0)))
    scatter(14, 4.0, lambda x, z: 1.0, put_log, limit=180)
    def put_fern(x, z):
        s = rnd.uniform(0.9, 1.4)
        ent("Fern", (x, height(x, z) + 0.5 * s - 0.05, z), scale=(s, s, s),
            SpriteRenderer={"texture": "assets/fern.png", "color": [1, 1, 1, 1], "size": [1.0, 1.0], "billboard": True, "columns": 1, "rows": 1, "directions": 1, "frame": 0, "fps": 0, "flipX": False})
    # Ferns line the road, where the player walks.
    for k in range(160):
        i = rnd.randrange(len(PATH) - 1); t = rnd.random()
        (ax, az), (bx, bz) = PATH[i], PATH[i + 1]
        side = rnd.choice([-1, 1]) * rnd.uniform(4.0, 12.0)
        dx, dz = bx - ax, bz - az; L = math.hypot(dx, dz)
        x, z = ax + dx * t - dz / L * side, az + dz * t + dx / L * side
        if math.hypot(x - CASTLE[0], z - CASTLE[1]) > CASTLE_R and math.hypot(x - CIRCLE[0], z - CIRCLE[1]) > 9: put_fern(x, z)

    # Standing stones around the campfire.
    cy = height(*CIRCLE)
    for k in range(9):
        a = k * 2 * math.pi / 9 + 0.2
        x, z = CIRCLE[0] + math.cos(a) * 8.5, CIRCLE[1] + math.sin(a) * 8.5
        ent("Standing stone", (x, cy, z), yaw(-math.degrees(a) + 90), MeshRenderer=mesh("assets/stone_%d.gltf" % (k % 3)), Collider=collider([1.3, 4, 0.7], (0, 1.6, 0)))
    ent("Campfire", (CIRCLE[0], cy, CIRCLE[1]), MeshRenderer=mesh("assets/campfire.gltf"), Collider=collider([2.4, 0.6, 2.4], (0, 0.2, 0)))
    ent("Fire", (CIRCLE[0], cy + 0.05, CIRCLE[1]), MeshRenderer=mesh("assets/flames.gltf", (1, 0.75, 0.35, 1), (6.0, 3.2, 0.9), texture="", texels=False),
        Light={"color": [1.0, 0.55, 0.22], "intensity": 7.0, "point": True, "range": 14})

    # The castle: one mesh for looks, simple boxes to collide with.
    ch = CASTLE_H
    ent("Castle", (CASTLE[0], ch, CASTLE[1]), MeshRenderer=mesh("assets/castle.gltf", texture="assets/stone_blocks.png"))
    ent("Castle windows", (CASTLE[0], ch, CASTLE[1]), MeshRenderer=mesh("assets/castle_windows.gltf", (1, 0.8, 0.45, 1), (4.0, 2.4, 0.9), texture="", texels=False))
    half = WALL / 2
    walls = [((0, -half), (WALL, WALL_T)), ((-half, 0), (WALL_T, WALL)), ((half, 0), (WALL_T, WALL)),
             ((-(half + GATE_W / 2) / 2 - 0.0, half), (half - GATE_W / 2, WALL_T)), (((half + GATE_W / 2) / 2, half), (half - GATE_W / 2, WALL_T)),
             ((0, -10), (20, 16))]
    for (x, z), (sx, sz) in walls:
        ent("Castle collider", (CASTLE[0] + x, ch + WALL_H / 2, CASTLE[1] + z), Collider=collider([sx, WALL_H + 20, sz]))
    for sx in (-1, 1):
        ent("Castle collider", (CASTLE[0] + sx * (GATE_W / 2 + 2.5), ch + 6, CASTLE[1] + half + 1), Collider=collider([5, 20, 5]))
        for sz in (-1, 1):
            ent("Castle collider", (CASTLE[0] + sx * half, ch + 6, CASTLE[1] + sz * half), Collider=collider([8, 30, 8]))
        # Torches either side of the gate.
        ent("Gate torch", (CASTLE[0] + sx * (GATE_W / 2 + 0.6), ch + 4.2, CASTLE[1] + half + 4.4), scale=(0.25, 0.5, 0.25),
            MeshRenderer=mesh("builtin:cube", (1, 0.8, 0.4, 1), (6.0, 3.0, 0.8), texture="", texels=False),
            Light={"color": [1.0, 0.58, 0.25], "intensity": 5.0, "point": True, "range": 10})
    ent("Keep light", (CASTLE[0], ch + 3, CASTLE[1] + 1.5), Light={"color": [1.0, 0.62, 0.3], "intensity": 4.0, "point": True, "range": 9})

    settings = {
        "pathTraced": True, "worldPixels": True, "probeResolution": 320, "probeCell": 4.0,
        "pixelArt": True, "pixelHeight": 216, "toonBands": 0, "dither": 0.5, "texelLighting": True, "texelsPerUnit": 8,
        "palette": "assets/palette.png", "rampShading": True, "rampShadowSteps": 2,
        "outlines": True, "outlineStrength": 0.35, "highlightStrength": 0.15,
        "ambient": [0.20, 0.20, 0.36], "skyTop": [0.16, 0.18, 0.36], "skyBottom": [0.93, 0.62, 0.45],
        "sunSize": 2.2, "sunGlow": 1.0, "fogColor": [0.62, 0.45, 0.48], "fogDensity": 0.0035,
        "cloudCoverage": 0.45, "cloudScale": 1.4, "cloudSpeed": 0.004, "stars": 0.5,
        "ptSamples": 1, "ptBounces": 1, "denoise": True, "exposure": 1.0,
    }
    json.dump({"version": 1, "settings": settings, "entities": E}, open(os.path.join(ROOT, "scenes", "forest.scene.json"), "w"), indent=1)
    return len(E)

def main():
    os.makedirs(ART, exist_ok=True); os.makedirs(os.path.join(ROOT, "scenes"), exist_ok=True); os.makedirs(os.path.join(ROOT, "scripts"), exist_ok=True)
    palette(); detail_tile("ground.png", 3, 0.82, 1.0, 40); stone_blocks("stone_blocks.png"); fern_sprite()
    tris = {}
    tris["terrain"] = write_gltf(terrain(), "terrain.gltf")
    tris["road"] = write_gltf(road(), "road.gltf")
    for i in range(3): tris["pine_%d" % i] = write_gltf(pine(100 + i), "pine_%d.gltf" % i)
    tris["deadpine"] = write_gltf(pine(200, dead=True), "deadpine.gltf")
    for i in range(2): tris["broadleaf_%d" % i] = write_gltf(broadleaf(300 + i), "broadleaf_%d.gltf" % i)
    for i in range(3): tris["rock_%d" % i] = write_gltf(rock(400 + i), "rock_%d.gltf" % i)
    for i in range(3): tris["stone_%d" % i] = write_gltf(standing_stone(500 + i), "stone_%d.gltf" % i)
    tris["log"] = write_gltf(log(), "log.gltf")
    tris["campfire"] = write_gltf(campfire(), "campfire.gltf")
    tris["flames"] = write_gltf(flames(), "flames.gltf")
    tris["castle"] = write_gltf(castle(), "castle.gltf")
    tris["castle_windows"] = write_gltf(castle_windows(), "castle_windows.gltf")
    n = scene()
    json.dump({"name": "Shadow Forest", "startScene": "scenes/forest.scene.json", "width": 1280, "height": 720}, open(os.path.join(ROOT, "project.json"), "w"), indent=2)
    print("entities:", n, " triangles per mesh:", tris)

if __name__ == "__main__":
    main()
