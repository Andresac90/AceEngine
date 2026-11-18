# AceEngine

A small cross-platform game engine for stylized 2D and 3D games, with a level
editor. First target look: 3D pixel art (low-resolution render, toon shading,
outlines, optional path tracing).

Status: early but usable on macOS. See [docs/HANDOFF.md](docs/HANDOFF.md) for
exactly what works, what is untested and what is missing.

## Build (macOS)

Requirements: Xcode command line tools, `brew install cmake ninja sdl3`.
Optional, for the Vulkan backend: `brew install molten-vk vulkan-loader vulkan-headers vulkan-validationlayers`.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build          # CPU-side unit tests
```

The first configure downloads the pinned dependencies and the Slang shader
compiler into `build/` (Slang is reused from `~/.cache/ace/slang` if present).

## Run

```sh
build/bin/ace_editor projects/pixel_demo     # level editor
build/bin/ace_player projects/pixel_demo     # the game alone
build/bin/ace_editor --new ~/games/my_game   # create and open a new project
```

Add `--backend vulkan` (or `metal`) to pick the GPU backend.

## Editor

| Action | How |
| --- | --- |
| Orbit / pan / zoom | right-drag / middle-drag or Shift+right-drag / wheel |
| Select | click in the viewport or the Hierarchy. Lights and cameras show as small icons (sun, lamp, camera) you can click; a selected lamp shows its range |
| Select several | Shift+click in the viewport; Cmd+click (Ctrl on Windows) or Shift+click in the Hierarchy. The gizmo, delete, duplicate and copy act on all of them |
| Copy / paste | Cmd+C / Cmd+V, also between scenes (uses the system clipboard; pasted entities keep their world position) |
| Move / rotate / scale | W / E / R, then drag the gizmo |
| Snap the gizmo to a grid | Snap checkbox + step in the toolbar |
| Build levels from blocks | tick **Build**, pick a texture and block size, click any surface to add a block there; Shift+click removes a block |
| Torches, lanterns, lamps | a **Light** with `point` ticked: lights everything within `range`, in toon steps like the sun (up to 8 nearest the camera, no shadows). The first light without `point` is the sun; a dim sun + point lights makes night |
| Edit the palette | **Palette** tab: one row per color ramp (darkest left); click a swatch to change it, `+`/`-` to grow or shrink a ramp, **Add ramp**. The scene updates as you edit; **Save** writes the PNG. A scene without a palette gets a **Create palette** button |
| Reuse an object (prefab) | right-click it in the Hierarchy > **Save as Prefab**; double-click the `.prefab.json` in Assets to place a copy. Scripts use `ace.spawn_prefab` |
| Pick a mesh / texture / script | the arrow button beside the field, or drag from Assets |
| Focus selection | F |
| Create entities | Entity menu; right-click a node for Add Child |
| Re-parent | drag a node onto another (or onto empty space) |
| Assign assets | drag a file from Assets onto a mesh / texture / script field |
| Play / stop | F5 (changes made while playing are discarded on stop) |
| Undo / redo, save, duplicate, delete | Cmd+Z / Cmd+Shift+Z, Cmd+S, Cmd+D, Delete |

The look of a level is in the **Render Settings** tab and is saved with the scene.

## Making it look like pixel art

The demo's `scenes/village.scene.json` is the reference. Two ways to get pixels:

- **World pixels** (`worldPixels`, used by the demo; perspective cameras): the
  scene is captured into small cube maps around a point snapped to a world
  grid, and every captured pixel is drawn as a little square *in the 3D world*.
  The pixels belong to the world, so they don't swim or shimmer when the camera
  turns or moves. `probeResolution` sets their size (384 per cube face by
  default; lower = chunkier), `probeCell` how far you walk before the pixel grid
  re-centres (with a quick dissolve). This is the technique of Dylan Ebert's
  [Texel Splatting](https://dylanebert.com/texel-splatting/) paper.
- **Screen pixels** (`pixelArt` + `pixelHeight`): a low-resolution image scaled
  up. Rock steady with an orthographic camera (`pixelSnap`); with a perspective
  camera the pixels slide over surfaces as you move.

Then, for both:

| What | Where |
| --- | --- |
| Low-resolution image, hard-pixel upscale (screen pixels) | Render Settings: `pixelArt`, `pixelHeight` (also sets the UI text size) |
| Pixel-art textures at one pixel size everywhere | MeshRenderer `worldTexels` + Render Settings `texelsPerUnit` (texture pixels per world unit) |
| Light and shadow that follow the texture's pixel grid | Render Settings `texelLighting`, `toonBands`, `shadows` |
| Dithering between light bands and palette colors | Render Settings `dither` |
| Every on-screen color taken from one palette | Render Settings `palette` (an image; up to 64 distinct colors) |
| Shading by hand-picked color ramps instead of darkening | Render Settings `rampShading`, `rampShadowSteps`: each palette **row** is a ramp, darkest on the left; lit surfaces step one color right, shadowed ones step left |
| Characters and props as sprites in the 3D world | SpriteRenderer with `billboard` |
| Characters seen from front, side and back | SpriteRenderer `directions` = 4 or 8: one sheet row per view (front, left, back, right; 8 adds the diagonals), animation frames across. Turn the entity to face where it goes; the engine picks the row from the camera |

Draw textures and sprites from the same palette image, using each ramp's
middle colors (ramp shading needs room to step both ways). Size sprites so one
texel matches the world: a 16x22 sprite at 16 texels per unit is 1.0 x 1.375.
`python3 tools/make_demo_art.py` regenerates the demo's art and shows the idea.

## Projects

```
my_game/
  project.json            name, start scene, window size
  scenes/*.scene.json     entities + render settings
  scripts/*.lua           gameplay (docs/SCRIPTING.md)
  assets/...              .gltf/.glb meshes, .png/.jpg textures
```

Meshes: `builtin:cube`, `builtin:sphere`, `builtin:plane`, `builtin:quad`, or a
glTF path. Colors in scenes are sRGB.

## Export

In the editor: **File > Export** (macOS app, iOS Simulator app, unsigned iOS
device app); progress appears in the Console. Or from a terminal:

```sh
tools/export_macos.sh projects/pixel_demo    # -> build-export/macos/<Name>.app
tools/export_ios.sh projects/pixel_demo      # iOS simulator .app (add "device" for an unsigned device build)
tools/autotest.sh                            # scripted editor interaction test on each backend
```

Windows and Android export are not done yet; the plan is in docs/HANDOFF.md.

## Layout

```
engine/core      types, logging, math
engine/rhi       GPU interface + Metal and Vulkan backends
engine/render    renderer (raster + path traced scene passes, present pass)
engine/scene     entities, components, reflection, serialization
engine/assets    meshes and textures
engine/script    Lua
engine/platform  window, input, timing (SDL3)
engine/runtime   project files
engine/shaders   Slang shaders
editor/          level editor (Dear ImGui)
player/          game runtime executable
projects/        sample project
tests/           unit tests
```
