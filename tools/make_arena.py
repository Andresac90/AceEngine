#!/usr/bin/env python3
"""Generates projects/arena: a hockey arena with a sold-out crowd of ~18,000
inflatable tube-man fans, made to show the engine's crowd path tracing.

Everything is generated (deterministic seeds):   python3 tools/make_arena.py

Writes into projects/arena:
  assets/fans.crowd.json   the crowd pack: tube-man models (rigid parts, one per
                           bone), and their animation clips baked per model as
                           one rigid transform per bone per frame (12 fps, stepped)
  assets/arena.members.json one row per fan: seat, facing, model, team, idle clip,
                           speed, phase, wave delay and three colors
  assets/*.gltf, *.png     the bowl, rink, boards, goals, jumbotron, roof, palette
  scenes/arena.scene.json, project.json
Scripts in projects/arena/scripts are hand-written and left alone.

Units are metres; +Y up; the rink's long axis is X. NHL rink: 200 x 85 ft
(60.96 x 25.91 m), corner radius 28 ft (8.53 m).
"""
import base64, json, math, os, random, struct, sys
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_forest import Mesh  # flat-shaded triangle builder with box/prism helpers

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "projects", "arena")
ART = os.path.join(ROOT, "assets")

# ------------------------------------------------------------------ palette
RAMPS = {
    "ice":    ["#3c4a63", "#5d6f8c", "#8a9db8", "#b6c6da", "#d9e4ef", "#f2f7fb"],
    "red":    ["#2e0b12", "#5a1420", "#8c1f2a", "#c22f33", "#e8574a", "#ff8f6e"],
    "navy":   ["#0b1024", "#141f45", "#1f3270", "#2f4d9e", "#4c74c8", "#7fa6e6"],
    "gold":   ["#3a2508", "#6b4510", "#a36d16", "#d89c22", "#f2c845", "#fff08a"],
    "steel":  ["#0c0d12", "#181a22", "#262a35", "#3a3f4d", "#555b6b", "#7c8292"],
    "green":  ["#0b2214", "#14401f", "#1f6a2c", "#2f9a3a", "#5cc451", "#a3e67a"],
    "orange": ["#331306", "#64250b", "#a24312", "#dd6a1d", "#f69c3a", "#ffd07a"],
    "purple": ["#1c0b2b", "#341552", "#552483", "#7c3cb5", "#a86ad8", "#d6a8f2"],
    "white":  ["#202020", "#4a4a4a", "#7d7d7d", "#b0b0b0", "#dadada", "#ffffff"],
}
def hexrgb(h): return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))
def lin(c): return [((v / 255) ** 2.2) for v in c]
R = {k: [hexrgb(c) for c in v] for k, v in RAMPS.items()}
def C(ramp, i): return lin(R[ramp][i])          # linear color for glTF meshes
def rgbint(c): return (c[0] << 16) | (c[1] << 8) | c[2]

def palette():
    img = Image.new("RGBA", (6, len(R)))
    for y, ramp in enumerate(R.values()):
        for x, c in enumerate(ramp): img.putpixel((x, y), c + (255,))
    img.save(os.path.join(ART, "palette.png"))

# ------------------------------------------------------------------ 3x5 pixel font
FONT = {
    "A": "010101111101101", "B": "110101110101110", "C": "011100100100011", "D": "110101101101110",
    "E": "111100110100111", "F": "111100110100100", "G": "011100101101011", "H": "101101111101101",
    "I": "111010010010111", "K": "101101110101101", "L": "100100100100111", "M": "101111111101101",
    "N": "110101101101101", "O": "010101101101010", "P": "110101110100100", "R": "110101110101101",
    "S": "011100010001110", "T": "111010010010010", "U": "101101101101111", "V": "101101101101010",
    "W": "101101111111101", "Y": "101101010010010", "0": "111101101101111", "1": "010110010010111",
    "2": "110001010100111", "3": "110001010001110", "4": "101101111001001", "5": "111100110001110",
    "!": "010010010000010", " ": "000000000000000", "-": "000000111000000", "*": "000101010101000",
}
def text_pixels(text):
    """Pixels (x, y) of `text` in the 3x5 font, 1-pixel letter spacing."""
    out = []
    for i, ch in enumerate(text):
        bits = FONT.get(ch, FONT[" "])
        for k, b in enumerate(bits):
            if b == "1": out.append((i * 4 + k % 3, k // 3))
    return out, len(text) * 4 - 1

# ------------------------------------------------------------------ glTF
def write_gltf(mesh, name, uvs=None):
    """Minimal glTF 2.0, embedded buffer: POSITION, NORMAL, COLOR_0 and optional TEXCOORD_0."""
    n = len(mesh.p)
    parts = [b"".join(struct.pack("<3f", *p) for p in mesh.p), b"".join(struct.pack("<3f", *v) for v in mesh.n),
             b"".join(struct.pack("<4f", *c) for c in mesh.c)]
    if uvs is not None: parts.append(b"".join(struct.pack("<2f", *t) for t in uvs))
    lo = [min(p[i] for p in mesh.p) for i in range(3)]; hi = [max(p[i] for p in mesh.p) for i in range(3)]
    views, acc, off = [], [], 0
    for k, (data, typ) in enumerate(zip(parts, ["VEC3", "VEC3", "VEC4", "VEC2"])):
        views.append({"buffer": 0, "byteOffset": off, "byteLength": len(data)}); off += len(data)
        a = {"bufferView": k, "componentType": 5126, "count": n, "type": typ}
        if k == 0: a.update(min=lo, max=hi)
        acc.append(a)
    attrs = {"POSITION": 0, "NORMAL": 1, "COLOR_0": 2}
    if uvs is not None: attrs["TEXCOORD_0"] = 3
    buf = b"".join(parts)
    g = {"asset": {"version": "2.0", "generator": "AceEngine make_arena.py"}, "scene": 0, "scenes": [{"nodes": [0]}],
         "nodes": [{"mesh": 0}], "meshes": [{"primitives": [{"attributes": attrs}]}],
         "buffers": [{"byteLength": len(buf), "uri": "data:application/octet-stream;base64," + base64.b64encode(buf).decode()}],
         "bufferViews": views, "accessors": acc}
    json.dump(g, open(os.path.join(ART, name), "w"))
    return mesh.tris()

# ------------------------------------------------------------------ rink shape
RINK_A, RINK_B, RINK_R = 30.48, 12.955, 8.53          # half length, half width, corner radius
CX, CZ = RINK_A - RINK_R, RINK_B - RINK_R             # corner circle centres (+-CX, +-CZ)

def outline(d, step=1.5, arc_steps=14):
    """Closed rounded rectangle `d` metres outside the boards, counter-clockwise
    seen from above: list of (x, z, nx, nz) with outward normals."""
    r = RINK_R + d
    pts = []
    def line(x0, z0, x1, z1, nx, nz):
        n = max(1, int(math.hypot(x1 - x0, z1 - z0) / step))
        for i in range(n): pts.append((x0 + (x1 - x0) * i / n, z0 + (z1 - z0) * i / n, nx, nz))
    def arc(cx, cz, a0):
        for i in range(arc_steps):
            a = math.radians(a0 + 90 * i / arc_steps)
            pts.append((cx + r * math.cos(a), cz + r * math.sin(a), math.cos(a), math.sin(a)))
    line(CX + r, -CZ, CX + r, CZ, 1, 0);   arc(CX, CZ, 0)
    line(CX, CZ + r, -CX, CZ + r, 0, 1);   arc(-CX, CZ, 90)
    line(-CX - r, CZ, -CX - r, -CZ, -1, 0); arc(-CX, -CZ, 180)
    line(-CX, -CZ - r, CX, -CZ - r, 0, -1); arc(CX, -CZ, 270)
    return pts

def perimeter(d): return 4 * CX + 4 * CZ + 2 * math.pi * (RINK_R + d)

def point_at(d, s):
    """Point, outward normal and a d-independent 'aisle coordinate' at arc length s."""
    r = RINK_R + d
    segs = [("l", (CX + r, -CZ), (0, 1), (1, 0), 2 * CZ), ("a", (CX, CZ), 0), ("l", (CX, CZ + r), (-1, 0), (0, 1), 2 * CX),
            ("a", (-CX, CZ), 90), ("l", (-CX - r, CZ), (0, -1), (-1, 0), 2 * CZ), ("a", (-CX, -CZ), 180),
            ("l", (-CX, -CZ - r), (1, 0), (0, -1), 2 * CX), ("a", (CX, -CZ), 270)]
    u = 0.0
    ref = RINK_R + 6.0  # aisles follow radial lines: arcs advance the aisle coordinate at a fixed radius
    for seg in segs:
        if seg[0] == "l":
            _, p0, dirv, nrm, length = seg
            if s <= length:
                return (p0[0] + dirv[0] * s, p0[1] + dirv[1] * s, nrm[0], nrm[1], u + s)
            s -= length; u += length
        else:
            _, c, a0 = seg
            length = math.pi / 2 * r
            if s <= length:
                a = math.radians(a0) + s / r
                return (c[0] + r * math.cos(a), c[1] + r * math.sin(a), math.cos(a), math.sin(a), u + (s / r) * ref)
            s -= length; u += math.pi / 2 * ref
    return point_at(d, 0.0)

def strip(m, d0, y0, d1, y1, color, pts_fn=outline):
    """Surface swept around the bowl between (offset d0, height y0) and (d1, y1)."""
    a, b = pts_fn(d0), pts_fn(d1)
    rA, rB = RINK_R + d0, RINK_R + d1
    for i in range(len(a)):
        j = (i + 1) % len(a)
        p = lambda q, y: [q[0], y, q[1]]
        m.quad(p(a[i], y0), p(b[i], y1), p(b[j], y1), p(a[j], y0), color)

def ring_strip(m, d, y0, y1, color, inward=True):
    pts = outline(d)
    for i in range(len(pts)):
        j = (i + 1) % len(pts)
        a, b = pts[i], pts[j]
        q = [[a[0], y0, a[1]], [b[0], y0, b[1]], [b[0], y1, b[1]], [a[0], y1, a[1]]]
        if inward: m.quad(q[0], q[3], q[2], q[1], color)
        else: m.quad(*q, color)

# ------------------------------------------------------------------ bowl
LOWER_ROWS, UPPER_ROWS = 20, 19
def lower_row(i): return 3.0 + 0.85 * i, 0.55 + 0.42 * i       # (offset, tread height)
def upper_row(j): return 21.6 + 0.78 * j, 12.6 + 0.50 * j
FASCIA = 20.2
BACK = upper_row(UPPER_ROWS)[0] + 0.2
ROOF_Y = 30.0

def bowl():
    m = Mesh()
    concrete, concrete_top = C("steel", 3), C("steel", 4)
    seat_lo, seat_hi = C("red", 2), C("navy", 2)
    ring_strip(m, 2.55, 0.0, lower_row(0)[1], C("steel", 2))     # front wall of the lower bowl
    strip(m, 0.0, 0.0, 2.55, 0.0, C("steel", 1))                 # floor between boards and seats
    for rows, rowf, seat in ((LOWER_ROWS, lower_row, seat_lo), (UPPER_ROWS, upper_row, seat_hi)):
        for i in range(rows):
            d, h = rowf(i)
            dn, hn = rowf(i + 1)
            strip(m, d - 0.45, h, d + 0.40, h, concrete_top)                       # tread
            if i + 1 < rows: ring_strip(m, d + 0.40, h, hn, concrete)              # riser
            ring_strip(m, d + 0.28, h + 0.38, h + 0.78, seat)                      # seat backs (front face)
            strip(m, d + 0.28, h + 0.78, d + 0.36, h + 0.78, seat)                 # seat back tops
    # Club level: fascia wall between the bowls (the ribbon board sits on it).
    lo_top = lower_row(LOWER_ROWS - 1)[1]
    strip(m, lower_row(LOWER_ROWS - 1)[0] + 0.40, lo_top, FASCIA, lo_top, concrete_top)
    ring_strip(m, FASCIA, lo_top, 9.6, C("steel", 1))
    ring_strip(m, FASCIA, 10.9, 11.2, C("steel", 1))
    ring_strip(m, FASCIA, 11.2, 12.0, C("gold", 1))              # suite windows (warm, dim)
    strip(m, FASCIA, 12.0, upper_row(0)[0] - 0.45, upper_row(0)[1], C("steel", 2))
    # Back wall and roof.
    top = upper_row(UPPER_ROWS - 1)[1]
    ring_strip(m, BACK, top, ROOF_Y, C("steel", 1))
    strip(m, BACK, ROOF_Y, BACK + 6.0, ROOF_Y, C("steel", 0))
    return m

def roof():
    """Ceiling grid over the bowl with a dark lattice of trusses."""
    m = Mesh()
    half_x, half_z = CX + RINK_R + BACK + 1.0, CZ + RINK_R + BACK + 1.0
    n = 12
    for i in range(n):
        for k in range(n):
            x0, x1 = -half_x + 2 * half_x * i / n, -half_x + 2 * half_x * (i + 1) / n
            z0, z1 = -half_z + 2 * half_z * k / n, -half_z + 2 * half_z * (k + 1) / n
            m.quad([x0, ROOF_Y, z0], [x1, ROOF_Y, z0], [x1, ROOF_Y, z1], [x0, ROOF_Y, z1], C("steel", 0))
    for x in range(-48, 49, 8): m.box([x, ROOF_Y - 1.2, 0], [0.4, 0.8, 2 * half_z - 4], C("steel", 2))
    for z in (-24, -12, 0, 12, 24): m.box([0, ROOF_Y - 2.0, z], [2 * half_x - 4, 0.6, 0.4], C("steel", 2))
    return m

def light_rig():
    """Catwalk ring of flood lights above the ice; the bright panels are emissive."""
    frame, panels = Mesh(), Mesh()
    for x in (-24, -12, 0, 12, 24):
        for z in (-9, 9):
            frame.box([x, 26.4, z], [3.2, 0.5, 1.6], C("steel", 1))
            panels.quad([x - 1.4, 26.1, z - 0.6], [x + 1.4, 26.1, z - 0.6], [x + 1.4, 26.1, z + 0.6], [x - 1.4, 26.1, z + 0.6], C("white", 5))
            frame.box([x, 28.2, z], [0.2, 3.4, 0.2], C("steel", 2))
    return frame, panels

# ------------------------------------------------------------------ ice
ICE_PX = 8                                   # texels per metre
ICE_W, ICE_H = 70.0, 36.0                    # textured area (rink + floor margin)
def ice_texture():
    w, h = int(ICE_W * ICE_PX), int(ICE_H * ICE_PX)
    img = Image.new("RGBA", (w, h))
    rnd = random.Random(7)
    ice, scuff, floor = R["ice"][5], R["ice"][4], R["steel"][1]
    red, blue = R["red"][3], R["navy"][3]
    def inside(x, z, grow=0.0):
        qx, qz = max(abs(x) - CX, 0), max(abs(z) - CZ, 0)
        if abs(x) <= CX or abs(z) <= CZ: return abs(x) <= RINK_A + grow and abs(z) <= RINK_B + grow
        return qx * qx + qz * qz <= (RINK_R + grow) ** 2
    def world(px, py): return (px + 0.5) / ICE_PX - ICE_W / 2, (py + 0.5) / ICE_PX - ICE_H / 2
    for py in range(h):
        for px in range(w):
            x, z = world(px, py)
            if not inside(x, z): img.putpixel((px, py), floor + (255,)); continue
            c = ice if rnd.random() > 0.07 else scuff
            ax = abs(x)
            if abs(x) < 0.16: c = red                                  # centre line
            elif 7.47 < ax < 7.77: c = blue                             # blue lines
            elif abs(ax - (RINK_A - 3.35)) < 0.06: c = red              # goal lines
            r_c = math.hypot(x, z)
            if abs(r_c - 4.57) < 0.07: c = blue                         # centre circle
            if r_c < 0.16: c = blue
            for fx in (-21.03, 21.03):
                for fz in (-6.71, 6.71):
                    rf = math.hypot(x - fx, z - fz)
                    if abs(rf - 4.57) < 0.07 or rf < 0.3: c = red
            for fx in (-9.14, 9.14):
                for fz in (-6.71, 6.71):
                    if math.hypot(x - fx, z - fz) < 0.3: c = red
            gx = RINK_A - 3.35
            if ax > gx - 1.83 and ax < gx and math.hypot(ax - gx, z) < 1.83: c = R["navy"][4]   # crease
            img.putpixel((px, py), c + (255,))
    # Centre-ice logo: a pixel spade (the Aces) inside the centre circle.
    spade = ["....r....", "...rrr...", "..rrrrr..", ".rrrrrrr.", "rrrrrrrrr", "rrrrrrrrr", "rrrr.rrrr", ".rr.r.rr.", "....r....", "...rrr..."]
    cx, cy = w // 2, h // 2
    for y, row in enumerate(spade):
        for x, ch in enumerate(row):
            if ch == "r":
                for dy in range(2):
                    for dx in range(2): img.putpixel((cx - 9 + x * 2 + dx, cy - 10 + y * 2 + dy), R["red"][2] + (255,))
    img.save(os.path.join(ART, "ice.png"))
    return inside

def ice_meshes(inside):
    ice, floor = Mesh(), Mesh()
    iu, fu = [], []
    for i in range(int(ICE_W)):
        for k in range(int(ICE_H)):
            x0, z0 = i - ICE_W / 2, k - ICE_H / 2
            target, uvs = (ice, iu) if inside(x0 + 0.5, z0 + 0.5, 0.5) else (floor, fu)
            q = [[x0, 0, z0], [x0, 0, z0 + 1], [x0 + 1, 0, z0 + 1], [x0 + 1, 0, z0]]
            target.quad(*q, [1, 1, 1])
            for t in (0, 1, 2, 0, 2, 3): uvs.append([(q[t][0] + ICE_W / 2) / ICE_W, (q[t][2] + ICE_H / 2) / ICE_H])
    return (ice, iu), (floor, fu)

# ------------------------------------------------------------------ boards, ribbon board, goals, jumbotron
def banner_texture(name, words, height, colors, bg, period):
    """Repeating strip of 3x5 pixel words: `period` pixels wide, `height` high."""
    img = Image.new("RGBA", (period, height), bg + (255,))
    x = 2
    for k, word in enumerate(words):
        px, wpx = text_pixels(word)
        y0 = (height - 5) // 2
        for (dx, dy) in px:
            if x + dx < period: img.putpixel((x + dx, y0 + dy), colors[k % len(colors)] + (255,))
        x += wpx + 6
    img.save(os.path.join(ART, name))

def boards():
    """Dasher boards: inner face textured (ads), yellow kick plate in the texture's bottom row."""
    m, uvs = Mesh(), []
    pts = outline(0.0, step=1.0)
    s = 0.0
    H = 1.07
    for i in range(len(pts)):
        a, b = pts[i], pts[(i + 1) % len(pts)]
        ln = math.hypot(b[0] - a[0], b[1] - a[1])
        u0, u1 = s / 32.0, (s + ln) / 32.0
        s += ln
        q = [[a[0], 0, a[1]], [a[0], H, a[1]], [b[0], H, b[1]], [b[0], 0, b[1]]]
        m.quad(*q, [1, 1, 1])
        for t, uv in zip((0, 1, 2, 0, 2, 3), ([u0, 1], [u0, 0], [u1, 0], [u0, 1], [u1, 0], [u1, 1])): uvs.append(uv)
        o = [[a[0] + a[2] * 0.15, H, a[1] + a[3] * 0.15], [b[0] + b[2] * 0.15, H, b[1] + b[3] * 0.15]]
        m.quad(q[1], o[0], o[1], q[2], [1, 1, 1])  # top rail (white texel row)
        for _ in range(6): uvs.append([u0, 0.02])
    return m, uvs

def goal():
    """Net facing +X (opening towards the centre when placed at -X)."""
    m = Mesh()
    red, white = C("red", 3), C("white", 4)
    for z in (-0.915, 0.915): m.box([0, 0.61, z], [0.1, 1.22, 0.1], red)
    m.box([0, 1.22, 0], [0.1, 0.1, 1.93], red)
    m.box([-0.55, 0.6, 0], [0.04, 1.2, 1.83], white)
    for z in (-0.9, 0.9): m.box([-0.3, 0.55, z], [0.6, 1.1, 0.03], white)
    m.box([-0.3, 1.15, 0], [0.6, 0.03, 1.83], white)
    return m

def jumbotron():
    """Centre-hung scoreboard: dark frame plus emissive screen pixels (3D pixel art)."""
    frame, screens = Mesh(), Mesh()
    cy, half = 18.0, (4.0, 2.2, 2.6)
    frame.box([0, cy, 0], [2 * half[0], 2 * half[1], 2 * half[2]], C("steel", 1))
    frame.box([0, cy - half[1] - 0.6, 0], [2 * half[0] - 1.0, 1.0, 2 * half[2] - 1.0], C("steel", 2))
    for k in range(4): frame.box([(-1 if k % 2 else 1) * 2.5, cy + half[1] + 3.5, (-1 if k < 2 else 1) * 1.5], [0.08, 7.0, 0.08], C("steel", 2))
    # Screen content per face: "ACES" over a score; one quad per lit pixel.
    for face in range(4):
        long_face = face < 2
        width = 2 * half[0] if long_face else 2 * half[2]
        lines = [("ACES", R["red"][4]), ("2-1", R["gold"][4])] if long_face else [("GO", R["red"][4]), ("ACES", R["white"][5])]
        px = 0.26 if long_face else 0.22
        rows = []
        for li, (txt, col) in enumerate(lines):
            pts, wpx = text_pixels(txt)
            for (x, y) in pts: rows.append((x - wpx / 2, y + li * 7, col))
        for (x, y, col) in rows:
            u, v = x * px, (5.5 - y) * px + 0.35
            c = lin(col)
            if long_face:
                z = (half[2] + 0.02) * (1 if face == 0 else -1)
                sgn = 1 if face == 0 else -1
                q = [[sgn * u, cy + v, z], [sgn * (u + px), cy + v, z], [sgn * (u + px), cy + v + px, z], [sgn * u, cy + v + px, z]]
            else:
                xx = (half[0] + 0.02) * (1 if face == 2 else -1)
                sgn = -1 if face == 2 else 1
                q = [[xx, cy + v, sgn * u], [xx, cy + v, sgn * (u + px)], [xx, cy + v + px, sgn * (u + px)], [xx, cy + v + px, sgn * u]]
            screens.quad(*q, c)
            screens.quad(q[0], q[3], q[2], q[1], c)  # both windings: the quad faces outward whichever way it was built
    return frame, screens

def ribbon(uv_period=40.0):
    """Ribbon board ring on the club fascia (emissive, textured)."""
    m, uvs = Mesh(), []
    pts = outline(FASCIA - 0.05, step=1.0)
    s = 0.0
    for i in range(len(pts)):
        a, b = pts[i], pts[(i + 1) % len(pts)]
        ln = math.hypot(b[0] - a[0], b[1] - a[1])
        u0, u1 = s / uv_period, (s + ln) / uv_period
        s += ln
        q = [[a[0], 9.6, a[1]], [b[0], 9.6, b[1]], [b[0], 10.9, b[1]], [a[0], 10.9, a[1]]]
        m.quad(q[0], q[3], q[2], q[1], [1, 1, 1])
        for uv in ([u0, 1], [u0, 0], [u1, 0], [u0, 1], [u1, 0], [u1, 1]): uvs.append(uv)
    return m, uvs

# ------------------------------------------------------------------ tube-man crowd pack
BONES = 9
PARENTS = [-1, 0, 1, 2, 3, 3, 5, 3, 7]
CLIPS = ["idle", "idle2", "clap", "cheer", "flail", "wave", "celebrate", "dejected", "skate"]
FPS = 12
SLOT = {"primary": -1, "secondary": -2, "accent": -3}   # negative colors = the fan's own colors

class Part:
    def __init__(self): self.v, self.n, self.c = [], [], []
    def tri(self, a, b, c, na, nb, nc, color):
        self.v += [a, b, c]; self.n += [na, nb, nc]; self.c.append(color)
    def flat(self, a, b, c, color):
        u = [b[i] - a[i] for i in range(3)]; w = [c[i] - a[i] for i in range(3)]
        n = [u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0]]
        l = math.sqrt(sum(x * x for x in n)) or 1.0
        n = [x / l for x in n]
        self.tri(a, b, c, n, n, n, color)

def frame_of(axis):
    length = math.sqrt(sum(c * c for c in axis))
    ax = [c / length for c in axis]
    up = [0, 0, 1] if abs(ax[2]) < 0.9 else [1, 0, 0]
    s = [ax[1] * up[2] - ax[2] * up[1], ax[2] * up[0] - ax[0] * up[2], ax[0] * up[1] - ax[1] * up[0]]
    l = math.sqrt(sum(x * x for x in s)); s = [x / l for x in s]
    t = [ax[1] * s[2] - ax[2] * s[1], ax[2] * s[0] - ax[0] * s[2], ax[0] * s[1] - ax[1] * s[0]]
    return ax, s, t

def tube(part, p0, axis, length, radii, color, sides=8, cap0=False, cap1=False, bands=None):
    """Inflatable tube: rings along `axis` with the given radii (bulging between
    joints), smooth normals. `bands` maps ring index -> color for stripes."""
    ax, s, t = frame_of(axis)
    nr = len(radii)
    rings = []
    for k, r in enumerate(radii):
        h = length * k / (nr - 1)
        c = [p0[i] + ax[i] * h for i in range(3)]
        rings.append([[c[i] + r * (math.cos(2 * math.pi * j / sides) * s[i] + math.sin(2 * math.pi * j / sides) * t[i]) for i in range(3)] for j in range(sides)])
    def normal(k, j):
        dr = (radii[min(k + 1, nr - 1)] - radii[max(k - 1, 0)]) / (length * (min(k + 1, nr - 1) - max(k - 1, 0)) / (nr - 1))
        rad = [math.cos(2 * math.pi * j / sides) * s[i] + math.sin(2 * math.pi * j / sides) * t[i] for i in range(3)]
        n = [rad[i] - ax[i] * dr for i in range(3)]
        l = math.sqrt(sum(x * x for x in n)); return [x / l for x in n]
    for k in range(nr - 1):
        col = bands.get(k, color) if bands else color
        for j in range(sides):
            jn = (j + 1) % sides
            a, b, c, d = rings[k][j], rings[k][jn], rings[k + 1][jn], rings[k + 1][j]
            part.tri(a, b, c, normal(k, j), normal(k, jn), normal(k + 1, jn), col)
            part.tri(a, c, d, normal(k, j), normal(k + 1, jn), normal(k + 1, j), col)
    for cap, k, sign in ((cap0, 0, -1), (cap1, nr - 1, 1)):
        if not cap: continue
        tip = [p0[i] + ax[i] * (length * k / (nr - 1) + sign * radii[k] * 0.6) for i in range(3)]
        for j in range(sides):
            jn = (j + 1) % sides
            a, b = rings[k][j], rings[k][jn]
            nt = [ax[i] * sign for i in range(3)]
            if sign > 0: part.tri(a, b, tip, normal(k, j), normal(k, jn), nt, color)
            else: part.tri(b, a, tip, normal(k, jn), normal(k, j), nt, color)

def disc(part, center, normal_axis, radius, color, sides=8):
    ax, s, t = frame_of(normal_axis)
    pts = [[center[i] + radius * (math.cos(2 * math.pi * j / sides) * s[i] + math.sin(2 * math.pi * j / sides) * t[i]) for i in range(3)] for j in range(sides)]
    for j in range(sides):
        part.flat(center, pts[(j + 1) % sides], pts[j], color)

def boxp(part, center, size, color):
    m = Mesh(); m.box(center, size, [0, 0, 0])
    for k in range(0, len(m.p), 3): part.flat(m.p[k], m.p[k + 1], m.p[k + 2], color)

def tube_man(kind):
    """Rest pose: standing, arms straight out to the sides (like the real ones),
    facing -Z. Returns (parts per bone, joint pivots)."""
    sc = {"classic": (1.0, 1.0), "chubby": (0.86, 1.45), "kid": (0.72, 1.05)}[kind["body"]]
    H, W = sc
    Y = lambda y: y * H
    r = 0.15 * W
    piv = [[0, 0, 0], [0, Y(0.38), 0], [0, Y(0.70), 0], [0, Y(1.02), 0], [0, Y(1.30), 0],
           [-r * 0.8, Y(1.20), 0], [-r * 0.8 - 0.32, Y(1.20), 0], [r * 0.8, Y(1.20), 0], [r * 0.8 + 0.32, Y(1.20), 0]]
    P = [Part() for _ in range(BONES)]
    prim, sec, acc = SLOT["primary"], SLOT["secondary"], SLOT["accent"]
    black, white = rgbint(R["steel"][0]), rgbint(R["white"][5])
    # Blower base and body segments, overlapping a little so bends do not open cracks.
    tube(P[0], [0, 0, 0], [0, 1, 0], 0.12, [0.20 * W, 0.17 * W], acc, cap1=True)  # blower base (colored, like the real ones)
    tube(P[0], [0, 0.10, 0], [0, 1, 0], Y(0.32), [r * 0.95, r * 1.12, r], prim)
    tube(P[1], [0, Y(0.33), 0], [0, 1, 0], Y(0.42), [r, r * 1.15, r], prim)  # solid, like the real ones: one chest stripe only
    tube(P[2], [0, Y(0.65), 0], [0, 1, 0], Y(0.42), [r, r * 1.15, r], prim)
    tube(P[3], [0, Y(0.97), 0], [0, 1, 0], Y(0.38), [r, r * 1.12, r * 1.02], prim, bands={0: sec})
    hr = r * (1.08 if kind["body"] != "kid" else 1.25)
    tube(P[4], [0, Y(1.25), 0], [0, 1, 0], Y(0.40), [hr * 0.95, hr * 1.1, hr * 1.05, hr * 0.7], prim, cap1=True)
    # Face on -Z: eyes (white, dark pupils) and a smile.
    fy = Y(1.48)
    eye = 0.045 * (1.35 if kind["body"] == "kid" else 1.0) * W ** 0.3
    for sx in (-1, 1):
        disc(P[4], [sx * hr * 0.42, fy, -hr * 1.02], [0, 0, -1], eye, white)
        disc(P[4], [sx * hr * 0.42, fy - eye * 0.25, -hr * 1.04 - 0.004], [0, 0, -1], eye * 0.45, black)
    for k in range(5):
        a0, a1 = math.radians(200 + 28 * k), math.radians(200 + 28 * (k + 1))
        y0, y1 = fy - 0.07 + 0.035 * math.sin(a0), fy - 0.07 + 0.035 * math.sin(a1)  # lowest in the middle: a smile
        x0, x1 = hr * 0.38 * math.cos(a0), hr * 0.38 * math.cos(a1)
        z = -hr * 1.06
        q = [[x0, y0, z], [x1, y1, z], [x1, y1 + 0.016, z], [x0, y0 + 0.016, z]]
        P[4].flat(q[0], q[2], q[1], black); P[4].flat(q[0], q[3], q[2], black)
    top = Y(1.25) + Y(0.40)
    if kind.get("hair"):
        for k in range(6):
            a = 2 * math.pi * k / 6
            base = [math.cos(a) * hr * 0.4, top - 0.02, math.sin(a) * hr * 0.4]
            tipp = [math.cos(a) * hr * 1.0, top + 0.17, math.sin(a) * hr * 1.0]
            tube(P[4], base, [tipp[i] - base[i] for i in range(3)], math.dist(base, tipp), [0.03, 0.022, 0.005], acc, sides=4)
    if kind.get("hat"):
        tube(P[4], [0, top - 0.12, 0], [0, 1, 0], 0.16, [hr * 1.12, hr * 1.0, hr * 0.55], acc, cap1=True, bands={0: sec})
        tube(P[4], [0, top + 0.06, 0], [0, 1, 0], 0.08, [0.03, 0.06, 0.03], white, cap0=True, cap1=True, sides=6)
    # Arms: two segments per side, rounded hands.
    ar = r * 0.62
    for side, (b_up, b_lo) in ((-1, (5, 6)), (1, (7, 8))):
        x0 = side * r * 0.6
        tube(P[b_up], [x0, Y(1.20), 0], [side, 0, 0], 0.36, [ar, ar * 1.1, ar], prim, sides=6, bands={1: sec})
        tube(P[b_lo], [side * (r * 0.8 + 0.30), Y(1.20), 0], [side, 0, 0], 0.32, [ar, ar * 1.05, ar * 0.9], prim, sides=6, cap1=True)
    hand = [r * 0.8 + 0.62, Y(1.20), 0]
    # Finger tufts: a fan of thin spikes at each hand (the flapping "fingers").
    for side, bone in ((-1, 6), (1, 8)):
        if side > 0 and (kind.get("finger") or kind.get("flag") or kind.get("stick")): continue
        for k in range(4):
            a = math.radians(-45 + 30 * k)
            base = [side * (hand[0] - 0.02), hand[1], 0]
            tip = [side * (hand[0] + 0.13 * math.cos(a)), hand[1] + 0.13 * math.sin(a), 0.03 * (k - 1.5)]
            tube(P[bone], base, [tip[i] - base[i] for i in range(3)], math.dist(base, tip), [0.022, 0.016, 0.004], prim, sides=4)
    if kind.get("finger"):  # foam finger on the right hand
        boxp(P[8], [hand[0] + 0.05, hand[1], 0], [0.12, 0.16, 0.10], acc)
        boxp(P[8], [hand[0] + 0.05, hand[1] + 0.20, 0], [0.06, 0.26, 0.06], acc)
    if kind.get("flag"):    # flag on a pole in the right hand
        tube(P[8], [hand[0], hand[1] - 0.1, 0], [0, 1, 0], 0.85, [0.012, 0.012], rgbint(R["steel"][4]), sides=4)
        fl = [[hand[0], hand[1] + 0.40, 0], [hand[0] + 0.45, hand[1] + 0.40, 0], [hand[0] + 0.45, hand[1] + 0.72, 0], [hand[0], hand[1] + 0.72, 0]]
        for a, b, c in ((0, 1, 2), (0, 2, 3)): P[8].flat(fl[a], fl[b], fl[c], sec); P[8].flat(fl[a], fl[c], fl[b], sec)
    if kind.get("stick"):   # hockey stick held by the right hand, blade on the ice
        tube(P[8], [hand[0], hand[1] + 0.1, 0], [0.15, -1, -0.35], 1.35, [0.02, 0.02], rgbint(R["orange"][1]), sides=4)
        boxp(P[8], [hand[0] + 0.21, 0.05, -0.47], [0.08, 0.07, 0.32], rgbint(R["steel"][0]))
    if kind.get("helmet"):
        tube(P[4], [0, top - 0.16, 0], [0, 1, 0], 0.18, [hr * 1.15, hr * 1.12, hr * 0.6], sec, cap1=True)
    return P, piv

MODELS = [
    ("classic", {"body": "classic", "hair": True}),
    ("beanie", {"body": "classic", "hat": True}),
    ("chubby", {"body": "chubby", "hat": True}),
    ("kid", {"body": "kid", "hair": True}),
    ("finger", {"body": "classic", "hair": True, "finger": True}),
    ("flag", {"body": "classic", "flag": True}),
    ("skater", {"body": "classic", "helmet": True, "stick": True}),
]

# --- quaternion helpers (x, y, z, w)
def qmul(a, b):
    ax, ay, az, aw = a; bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz]
def qaxis(axis, a): s = math.sin(a / 2); return [axis[0] * s, axis[1] * s, axis[2] * s, math.cos(a / 2)]
def qrot(q, v):
    p = qmul(qmul(q, [v[0], v[1], v[2], 0]), [-q[0], -q[1], -q[2], q[3]])
    return p[:3]
def euler(x=0.0, y=0.0, z=0.0):   # R = Ry * Rz * Rx
    return qmul(qaxis([0, 1, 0], y), qmul(qaxis([0, 0, 1], z), qaxis([1, 0, 0], x)))

def arm(side, raise_, fwd=0.0):
    """Upper-arm rotation: raise_ > 0 lifts the arm, < 0 lowers it; fwd swings it to the front (-Z)."""
    return euler(z=side * raise_, y=-side * fwd) if side < 0 else euler(z=raise_, y=fwd)

def pose(clip, t, phase_seed):
    """Local rotations per bone (and a root lift) at normalized time t in [0, 1)."""
    S = math.sin; tau = 2 * math.pi
    rot = [euler() for _ in range(BONES)]
    lift = 0.0
    def sides(fn):
        for side, up, lo in ((-1, 5, 6), (1, 7, 8)):
            u, l = fn(side)
            rot[up] = arm(side, *u); rot[lo] = arm(side, *l)
    if clip == "idle":
        for k in range(1, 5): rot[k] = euler(z=0.045 * S(tau * t + k * 0.55), x=-0.03)
        sides(lambda s: ((-0.95 + 0.08 * S(tau * t + s), 0.1), (-0.25, 0.0)))
    elif clip == "idle2":
        for k in range(1, 4): rot[k] = euler(x=-0.04 + 0.03 * S(tau * t), z=0.02 * S(tau * t * 2 + k))
        rot[4] = euler(x=-0.12 * max(0.0, S(tau * t * 2)))
        sides(lambda s: ((-0.85, 0.35 * S(tau * t + (0 if s < 0 else math.pi))), (-0.4, 0.3)))
    elif clip == "clap":
        clapping = 0.5 + 0.5 * S(tau * t)
        for k in range(1, 4): rot[k] = euler(x=-0.05 - 0.03 * clapping)
        sides(lambda s: ((-0.35, 1.15), (0.05, 0.25 + 0.6 * clapping)))
    elif clip == "cheer":
        for k in range(1, 5): rot[k] = euler(z=0.08 * S(tau * t + k * 0.7), x=0.04 * S(tau * 2 * t))
        sides(lambda s: ((1.0 + 0.35 * S(tau * 2 * t + (0 if s < 0 else math.pi)), 0.2), (0.35 * S(tau * 2 * t + 1 + s), 0.0)))
        lift = 0.05 * abs(S(tau * 2 * t))
    elif clip == "flail":
        for k in range(1, 5): rot[k] = euler(z=0.24 * S(tau * t - k * 0.9), x=0.12 * S(tau * 2 * t - k))
        sides(lambda s: ((0.55 * S(tau * 2 * t + s), 0.25 * S(tau * t)), (0.85 * S(tau * 2 * t + s + 1.3), 0.0)))
    elif clip == "wave":                  # one rise and fall, then the idle pose
        up = math.sin(math.pi * min(t / 0.75, 1.0)) if t < 0.75 else 0.0
        for k in range(1, 5): rot[k] = euler(x=-0.03 * (1 - up))
        sides(lambda s: ((-0.95 + 2.45 * up, 0.1 * (1 - up)), (-0.25 * (1 - up), 0.0)))
        lift = 0.28 * up
    elif clip == "celebrate":
        for k in range(1, 5): rot[k] = euler(z=0.16 * S(tau * t - k), x=0.06 * S(tau * t * 2))
        sides(lambda s: ((1.3 + 0.3 * S(tau * 2 * t + (0 if s < 0 else 2)), 0.0), (0.6 * S(tau * 2 * t + s * 1.5), 0.0)))
        lift = 0.32 * abs(S(tau * t))
    elif clip == "dejected":
        rot[1] = euler(x=-0.05); rot[2] = euler(x=-0.22 + 0.02 * S(tau * t)); rot[3] = euler(x=-0.25); rot[4] = euler(x=-0.35)
        sides(lambda s: ((-1.35, 0.15), (-0.1, 0.0)))
    elif clip == "skate":                  # leaning forward, arms swinging; the right hand holds the stick low
        rot[1] = euler(x=-0.18); rot[2] = euler(x=-0.15, z=0.08 * S(tau * t)); rot[3] = euler(x=-0.1); rot[4] = euler(x=0.25)
        rot[5] = arm(-1, -0.9 + 0.3 * S(tau * t), 0.45 * S(tau * t)); rot[6] = arm(-1, -0.2)
        rot[7] = arm(1, -0.75, 0.55); rot[8] = arm(1, -0.35, 0.3)
    return rot, lift

CLIP_SECONDS = {"idle": 2.0, "idle2": 3.0, "clap": 0.667, "cheer": 1.0, "flail": 1.5, "wave": 2.0, "celebrate": 0.833, "dejected": 3.0, "skate": 1.0}
LOOPS = {c: c != "wave" for c in CLIPS}

def bake(clip, piv):
    """Per frame, per bone: rest space -> character space as (quat, translation), by forward kinematics."""
    frames = max(1, round(CLIP_SECONDS[clip] * FPS))
    out = []
    for f in range(frames):
        rot, lift = pose(clip, f / frames, 0)
        world = []
        for b in range(BONES):
            # local: T(pivot) R T(-pivot); world = parent * local
            q = rot[b]
            t = [piv[b][i] - qrot(q, piv[b])[i] for i in range(3)]
            if b == 0: t[1] += lift
            if PARENTS[b] >= 0:
                pq, pt = world[PARENTS[b]]
                t = [pt[i] + qrot(pq, t)[i] for i in range(3)]
                q = qmul(pq, q)
            world.append((q, t))
        out.append([[round(x, 5) for x in q + t] for q, t in world])
    return out

def crowd_pack():
    models, poses = [], []
    for name, kind in MODELS:
        parts, piv = tube_man(kind)
        js = []
        for b, p in enumerate(parts):
            if not p.c: continue
            js.append({"bone": b, "positions": [round(x, 4) for v in p.v for x in v], "normals": [round(x, 3) for v in p.n for x in v], "colors": p.c})
        models.append({"name": name, "parts": js})
        poses.append([bake(c, piv) for c in CLIPS])
    pack = {"bones": BONES, "fps": FPS, "clips": [{"name": c, "loop": LOOPS[c]} for c in CLIPS], "models": models, "poses": poses}
    json.dump(pack, open(os.path.join(ART, "fans.crowd.json"), "w"), separators=(",", ":"))
    return sum(len(p.c) for p in tube_man(MODELS[0][1])[0])

# ------------------------------------------------------------------ fans in seats
TEAMS = {  # (primary, secondary, accent) choices, sRGB
    0: [(R["red"][3], R["white"][5], R["white"][5]), (R["white"][5], R["red"][3], R["red"][3]), (R["red"][3], R["white"][5], R["gold"][4])],
    1: [(R["navy"][3], R["gold"][4], R["gold"][4]), (R["gold"][4], R["navy"][3], R["navy"][2])],
    2: [(R["green"][3], R["white"][5], R["orange"][4]), (R["orange"][3], R["steel"][0], R["green"][4]),
        (R["purple"][3], R["gold"][4], R["white"][5]), (R["steel"][1], R["red"][3], R["white"][5])],
}
WAVE_LAP = 24.0   # seconds for the wave to go once around the bowl

def members():
    rnd = random.Random(2027)
    rows = []
    def add(x, y, z, nx, nz, aisle_u):
        if (aisle_u % 7.5) < 0.7: return                        # radial aisles
        if rnd.random() < 0.035: return                         # empty seats
        yaw = math.degrees(math.atan2(nx, nz))                  # local -Z (the face) points at the rink
        away = 0.0 < x < 26 and z < 0                           # a pocket of visiting fans behind one bench
        r = rnd.random()
        team = 1 if (away and r < 0.75) or r < 0.06 else (2 if r > 0.93 else 0)
        primary, secondary, accent = rnd.choice(TEAMS[team])
        model = rnd.choices(range(6), weights=[34, 22, 14, 12, 9, 9])[0]
        base = rnd.choices([0, 1, 2, 3, 4], weights=[44, 26, 16, 9, 5])[0]
        speed = round(rnd.uniform(0.85, 1.18), 3)
        phase = round(rnd.uniform(0, 4), 3)
        angle = (math.atan2(-z, x) / (2 * math.pi)) % 1.0     # counter-clockwise from +X, seen from above
        wave = round(angle * WAVE_LAP + rnd.uniform(0, 0.25), 3)
        scale = round(rnd.uniform(0.92, 1.08), 3)
        rows.append([round(x, 3), round(y, 3), round(z, 3), round(yaw, 2), scale, model, team, base, speed, phase, wave,
                     rgbint(primary), rgbint(secondary), rgbint(accent)])
    for rowf, count in ((lower_row, LOWER_ROWS), (upper_row, UPPER_ROWS)):
        for i in range(count):
            d, h = rowf(i)
            dd = d - 0.05
            L = perimeter(dd)
            n = int(L / 0.53)
            for k in range(n):
                x, z, nx, nz, u = point_at(dd, (k + 0.5) * L / n)
                add(x, h, z, nx, nz, u)
    json.dump({"waveLap": WAVE_LAP, "members": rows}, open(os.path.join(ART, "arena.members.json"), "w"), separators=(",", ":"))
    return len(rows)

# ------------------------------------------------------------------ scene
def entity(name, comps, parent=-1): return {"name": name, "parent": parent, "components": comps}
def tr(pos, rot=(0, 0, 0, 1), scale=(1, 1, 1)): return {"position": list(pos), "rotation": list(rot), "scale": list(scale)}
def mr(mesh, color=(1, 1, 1, 1), texture="", emissive=(0, 0, 0), **extra):
    d = {"mesh": mesh, "texture": texture, "color": list(color), "emissive": list(emissive), "worldTexels": False}
    d.update(extra)
    return d
def yawq(deg): return [0, math.sin(math.radians(deg) / 2), 0, math.cos(math.radians(deg) / 2)]

def scene(fans):
    settings = {
        "pathTraced": True, "worldPixels": False, "pixelArt": True, "pixelHeight": 240, "integerScale": True,
        "toonBands": 4, "dither": 0.45, "texelLighting": True, "texelsPerUnit": 16, "palette": "assets/palette.png",
        "rampShading": False, "outlines": True, "outlineStrength": 0.45, "highlightStrength": 0.2,
        "ambient": [0.10, 0.11, 0.14], "skyTop": [0.02, 0.02, 0.03], "skyBottom": [0.04, 0.04, 0.05],
        "sunSize": 0.0, "sunGlow": 0.0, "fogDensity": 0.0, "cloudCoverage": 0.0, "stars": 0.0,
        "ptSamples": 1, "ptBounces": 2, "denoise": True, "exposure": 1.25, "shadows": False,
    }
    E = [entity("Camera", {"Transform": tr((0, 17, -44)), "Camera": {"orthographic": False, "fov": 46, "nearPlane": 0.3, "farPlane": 250},
                           "Script": {"path": "scripts/camera.lua"}})]
    E.append(entity("Ice", {"Transform": tr((0, 0, 0)), "MeshRenderer": mr("assets/ice.gltf", texture="assets/ice.png", reflectivity=0.1, roughness=0.06)}))
    E.append(entity("Floor", {"Transform": tr((0, 0, 0)), "MeshRenderer": mr("assets/floor.gltf", texture="assets/ice.png")}))
    E.append(entity("Boards", {"Transform": tr((0, 0, 0)), "MeshRenderer": mr("assets/boards.gltf", texture="assets/boards.png")}))
    E.append(entity("Bowl", {"Transform": tr((0, 0, 0)), "MeshRenderer": mr("assets/bowl.gltf")}))
    E.append(entity("Roof", {"Transform": tr((0, 0, 0)), "MeshRenderer": mr("assets/roof.gltf")}))
    E.append(entity("Light rig", {"Transform": tr((0, 0, 0)), "MeshRenderer": mr("assets/rig.gltf")}))
    E.append(entity("Flood panels", {"Transform": tr((0, 0, 0)), "MeshRenderer": mr("assets/panels.gltf", emissive=(1.0, 0.97, 0.9))}))
    E.append(entity("Ribbon board", {"Transform": tr((0, 0, 0)), "MeshRenderer": mr("assets/ribbon.gltf", texture="assets/ribbon.png", emissive=(1.0, 1.0, 1.0))}))
    E.append(entity("Jumbotron", {"Transform": tr((0, 0, 0)), "MeshRenderer": mr("assets/jumbotron.gltf")}))
    E.append(entity("Jumbotron screens", {"Transform": tr((0, 0, 0)), "MeshRenderer": mr("assets/screens.gltf", emissive=(1.4, 1.4, 1.4))}))
    gx = RINK_A - 3.35
    E.append(entity("Goal west", {"Transform": tr((-gx, 0, 0), yawq(0)), "MeshRenderer": mr("assets/goal.gltf")}))
    E.append(entity("Goal east", {"Transform": tr((gx, 0, 0), yawq(180)), "MeshRenderer": mr("assets/goal.gltf")}))
    # Arena lights: 8 overhead floods (also what raster mode lights with).
    for k, (x, z) in enumerate([(x, z) for x in (-21, -7, 7, 21) for z in (-7, 7)]):
        E.append(entity("Flood %d" % (k + 1), {"Transform": tr((x, 25.5, z)), "Light": {"color": [1.0, 0.97, 0.92], "intensity": 3.2, "point": True, "range": 64}}))
    E.append(entity("Crowd", {"Transform": tr((0, 0, 0)), "Crowd": {"pack": "assets/fans.crowd.json", "members": "assets/arena.members.json", "speed": 1.0},
                               "Script": {"path": "scripts/showcase.lua"}}))
    # Skaters: crowd actors moved by the showcase script (the moving partition).
    for k in range(10):
        home = k < 5
        E.append(entity("Skater %d" % (k + 1), {"Transform": tr((-10 + 4 * k, 0, (-4 if home else 4))),
            "CrowdActor": {"model": 6, "clip": 8, "team": 0 if home else 1, "speed": 1.0,
                           "primary": [c / 255 for c in (R["red"][3] if home else R["white"][5])],
                           "secondary": [c / 255 for c in (R["white"][5] if home else R["navy"][3])],
                           "accent": [c / 255 for c in R["steel"][0]]}}))
    E.append(entity("Puck", {"Transform": tr((0, 0.0254, 0), scale=(0.076, 0.0254, 0.076)), "MeshRenderer": mr("builtin:sphere", color=(0.02, 0.02, 0.02, 1))}))
    doc = {"version": 1, "settings": settings, "entities": E}
    json.dump(doc, open(os.path.join(ROOT, "scenes", "arena.scene.json"), "w"), indent=1)
    return len(E)

def horn():
    """Goal horn: a low, slightly detuned chord of buzzy (odd-harmonic) tones, 2.5 s."""
    import wave
    rate, seconds = 22050, 2.5
    frames = bytearray()
    for i in range(int(rate * seconds)):
        t = i / rate
        env = min(1.0, t / 0.08) * min(1.0, (seconds - t) / 0.6)
        v = 0.0
        for f in (110.0, 138.6, 165.0, 220.5):
            v += sum(math.sin(2 * math.pi * f * k * t) / k for k in (1, 3, 5, 7))
        frames += struct.pack("<h", int(max(-1, min(1, v * 0.16 * env)) * 32000))
    w = wave.open(os.path.join(ART, "horn.wav"), "wb")
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate); w.writeframes(bytes(frames)); w.close()

def main():
    for d in (ART, os.path.join(ROOT, "scenes"), os.path.join(ROOT, "scripts")): os.makedirs(d, exist_ok=True)
    palette()
    horn()
    inside = ice_texture()
    (ice, iu), (floor, fu) = ice_meshes(inside)
    tris = {"ice": write_gltf(ice, "ice.gltf", iu), "floor": write_gltf(floor, "floor.gltf", fu)}
    banner_texture("boards.png", ["ACE ENGINE", "SLANG", "METAL", "VULKAN", "DX12", "LUA"], 9,
                   [R["navy"][3], R["red"][3]], R["white"][5], 256)
    img = Image.open(os.path.join(ART, "boards.png"))
    for x in range(256): img.putpixel((x, 8), R["gold"][4] + (255,))   # kick plate
    img.save(os.path.join(ART, "boards.png"))
    banner_texture("ribbon.png", ["GO ACES GO", "*", "ACE ARENA", "*", "PATH TRACED", "*", "18000 FANS"], 11,
                   [R["gold"][4], R["white"][5], R["red"][4], R["white"][5]], R["steel"][0], 320)
    b, bu = boards(); tris["boards"] = write_gltf(b, "boards.gltf", bu)
    rb, ru = ribbon(); tris["ribbon"] = write_gltf(rb, "ribbon.gltf", ru)
    tris["bowl"] = write_gltf(bowl(), "bowl.gltf")
    tris["roof"] = write_gltf(roof(), "roof.gltf")
    rig, panels = light_rig(); tris["rig"] = write_gltf(rig, "rig.gltf"); tris["panels"] = write_gltf(panels, "panels.gltf")
    jf, js = jumbotron(); tris["jumbotron"] = write_gltf(jf, "jumbotron.gltf"); tris["screens"] = write_gltf(js, "screens.gltf")
    tris["goal"] = write_gltf(goal(), "goal.gltf")
    per_fan = crowd_pack()
    fans = members()
    n = scene(fans)
    json.dump({"name": "Ace Arena", "startScene": "scenes/arena.scene.json", "width": 1280, "height": 720}, open(os.path.join(ROOT, "project.json"), "w"), indent=2)
    print("fans:", fans, " triangles per fan (classic):", per_fan, " entities:", n, " static triangles:", tris, " total:", sum(tris.values()))

if __name__ == "__main__":
    main()
