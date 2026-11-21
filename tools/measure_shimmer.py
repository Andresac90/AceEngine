#!/usr/bin/env python3
"""Measures pixel shimmer: how much of the image changes from frame to frame
while the camera orbits, once the change that is just the picture sliding has
been removed. Lower is steadier.

Usage: python3 tools/measure_shimmer.py [--backend metal|vulkan|d3d12] [--build build]

Renders the pixel_demo village twice (world pixels on, then screen pixels)
with a camera orbiting the village at 30 degrees per second for 60 frames at a
fixed 60 Hz timestep, from a temporary copy of the project (the repository is
not modified). For each pair of consecutive frames and each region (ground,
wall; screen text and the hero sprite removed), it tries every integer shift up to 4 game pixels and keeps the one with
the fewest differing pixels; the result is the mean share of pixels that still
differ (any channel off by more than 8/255).
"""
import argparse, json, os, shutil, subprocess, sys, tempfile
from PIL import Image, ImageChops

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
ORBIT = """-- Orbits the camera around the village centre (measure_shimmer.py).
local Orbit = { radius = 13, height = 6.5, speed = 30 }
function Orbit:update(dt)
  local a = math.rad(ace.time() * self.speed + 40)
  ace.set_position(self.entity, math.sin(a) * self.radius, self.height, math.cos(a) * self.radius)
  ace.set_look(self.entity, math.deg(a), -math.deg(math.atan(self.height - 1, self.radius)))
end
return Orbit
"""
# Regions as fractions of the image (x0, y0, x1, y1), checked against the village view.
REGIONS = {"ground": (0.05, 0.62, 0.40, 0.95), "wall": (0.42, 0.32, 0.58, 0.50)}


def render(project, out_dir, backend, build, world_pixels):
    scene_path = os.path.join(project, "scenes", "village.scene.json")
    scene = json.load(open(scene_path))
    scene["settings"]["worldPixels"] = world_pixels
    # Screen text and the hero sprite do not move with the world; leave them out.
    for e in scene["entities"]:
        c = e["components"]
        if c.get("Script", {}).get("path") == "scripts/hud.lua":
            del c["Script"]
        if e["name"] == "Hero":
            c.pop("SpriteRenderer", None)
    for e in scene["entities"]:
        if "Camera" in e["components"]:
            e["components"]["Script"] = {"path": "scripts/orbit.lua", "params": ""}
    json.dump(scene, open(scene_path, "w"))
    player = os.path.join(build, "bin", "ace_player")
    cmd = [player, project, "--backend", backend, "--fixed-dt", "0.0166667", "--frames", "61",
           "--frames-to", os.path.join(out_dir, "f")]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return sorted(os.path.join(out_dir, f) for f in os.listdir(out_dir) if f.endswith(".png"))


def changed_share(a, b, max_shift):
    best = 1.0
    w, h = a.size
    for dy in range(-max_shift, max_shift + 1):
        for dx in range(-max_shift, max_shift + 1):
            ca = a.crop((max(dx, 0), max(dy, 0), w + min(dx, 0), h + min(dy, 0)))
            cb = b.crop((max(-dx, 0), max(-dy, 0), w + min(-dx, 0), h + min(-dy, 0)))
            diff = ImageChops.difference(ca, cb).convert("L").point(lambda v: 255 if v > 8 else 0)
            share = diff.histogram()[255] / (diff.width * diff.height)
            best = min(best, share)
    return best


def measure(frames):
    shares = {name: [] for name in REGIONS}
    for fa, fb in zip(frames, frames[1:]):
        a, b = Image.open(fa).convert("RGB"), Image.open(fb).convert("RGB")
        # Work in game pixels: shrink by the integer scale so a shift of one is one game pixel.
        scale = max(1, a.height // 216)
        a = a.resize((a.width // scale, a.height // scale), Image.NEAREST)
        b = b.resize((b.width // scale, b.height // scale), Image.NEAREST)
        for name, (x0, y0, x1, y1) in REGIONS.items():
            box = (int(x0 * a.width), int(y0 * a.height), int(x1 * a.width), int(y1 * a.height))
            shares[name].append(changed_share(a.crop(box), b.crop(box), 4))
    return {name: 100 * sum(v) / len(v) for name, v in shares.items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--backend", default="metal")
    ap.add_argument("--build", default=os.path.join(ROOT, "build"))
    args = ap.parse_args()
    tmp = tempfile.mkdtemp(prefix="ace_shimmer_")
    try:
        for label, world in (("world pixels", True), ("screen pixels", False)):
            project = os.path.join(tmp, "project_" + label.split()[0])
            shutil.copytree(os.path.join(ROOT, "projects", "pixel_demo"), project)
            open(os.path.join(project, "scripts", "orbit.lua"), "w").write(ORBIT)
            out = os.path.join(tmp, "frames_" + label.split()[0])
            os.makedirs(out)
            frames = render(project, out, args.backend, args.build, world)
            if len(frames) < 2:
                sys.exit("no frames were written")
            result = measure(frames)
            print(f"{label:14s} " + "  ".join(f"{k} {v:.1f}%" for k, v in result.items()))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
