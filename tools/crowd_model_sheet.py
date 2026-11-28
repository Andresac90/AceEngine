"""Renders a model sheet of a crowd pack with Blender (a visual check of the
models and poses, outside the engine). Run headless:

  Blender -b --python tools/crowd_model_sheet.py -- <pack.crowd.json> <out.png>

Row 1: every model in rest pose. Row 2: one model in a frame of each clip.
Each part is placed with its bone's baked transform exactly as the engine
does (rest -> character space: rotate by q, then add t), so a wrong pose or a
crack at a joint shows up here too.
"""
import json, math, sys
import bpy

args = sys.argv[sys.argv.index("--") + 1:]
pack = json.load(open(args[0]))
out = args[1]
SLOTS = {-1: (0.80, 0.12, 0.14), -2: (0.95, 0.95, 0.95), -3: (0.95, 0.72, 0.15)}  # home colors

def srgb(c):  # 0xRRGGBB -> linear RGB for Blender
    return tuple(((c >> s & 255) / 255) ** 2.2 for s in (16, 8, 0))

def color_of(code):
    return SLOTS[int(code)] if code < 0 else srgb(int(code))

def qrot(q, v):
    x, y, z, w = q
    ux, uy, uz = 2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])
    return (v[0] + w * ux + (y * uz - z * uy), v[1] + w * uy + (z * ux - x * uz), v[2] + w * uz + (x * uy - y * ux))

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
materials = {}

def material(rgb):
    key = tuple(round(c, 3) for c in rgb)
    if key not in materials:
        m = bpy.data.materials.new("c%d" % len(materials))
        m.diffuse_color = (*rgb, 1.0)
        materials[key] = m
    return materials[key]

def build(model, pose, offset):
    """One person: engine space (+Y up, faces -Z) -> Blender (+Z up, faces -Y)."""
    verts, faces, mats = [], [], []
    for part in model["parts"]:
        q, t = pose[part["bone"]][:4], pose[part["bone"]][4:]
        p = part["positions"]
        for k in range(len(part["colors"])):
            base = len(verts)
            for j in range(3):
                v = p[(k * 3 + j) * 3:(k * 3 + j) * 3 + 3]
                x, y, z = (a + b for a, b in zip(qrot(q, v), t))
                verts.append((x + offset, z, y))
            faces.append((base, base + 2, base + 1))  # handedness flips with the axis swap
            mats.append(material(color_of(part["colors"][k])))
    mesh = bpy.data.meshes.new(model["name"])
    mesh.from_pydata(verts, [], faces)
    obj = bpy.data.objects.new(model["name"], mesh)
    for m in dict.fromkeys(mats):
        obj.data.materials.append(m)
    index = {m: i for i, m in enumerate(dict.fromkeys(mats))}
    for poly, m in zip(mesh.polygons, mats):
        poly.material_index = index[m]
        poly.use_smooth = False
    scene.collection.objects.link(obj)

clips = [c["name"] for c in pack["clips"]]
rest = [[0, 0, 0, 1, 0, 0, 0]] * pack["bones"]
SPACING = 2.1
for i, model in enumerate(pack["models"]):
    build(model, rest, i * SPACING)
showcase = ["idle", "clap", "cheer", "flail", "wave", "celebrate", "dejected", "skate"]
for i, name in enumerate(c for c in showcase if c in clips):
    frames = pack["poses"][0][clips.index(name)]
    pose = frames[len(frames) // 3 if name != "wave" else len(frames) // 5]
    before = set(scene.collection.objects)
    build(pack["models"][0 if name != "skate" else len(pack["models"]) - 1], pose, i * SPACING)
    for o in set(scene.collection.objects) - before:
        o.location.z = -2.6  # second row, below the first

cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
cam.data.type = "ORTHO"
cam.data.ortho_scale = 16.0
cam.location = (7.2, -20.0, -0.4)
cam.rotation_euler = (math.radians(90), 0, 0)
scene.collection.objects.link(cam)
scene.camera = cam
sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
sun.rotation_euler = (math.radians(50), math.radians(10), math.radians(-30))
scene.collection.objects.link(sun)
scene.render.engine = "BLENDER_WORKBENCH"
scene.display.shading.light = "STUDIO"
scene.display.shading.color_type = "MATERIAL"
scene.display.shading.show_cavity = True
scene.render.resolution_x, scene.render.resolution_y = 2400, 1300
scene.render.filepath = out
world = bpy.data.worlds.new("w")
world.color = (0.12, 0.13, 0.17)
scene.world = world
bpy.ops.render.render(write_still=True)
print("wrote", out)
