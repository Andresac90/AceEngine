# AceEngine handoff manual

For the next engineer (human or model) continuing this work. Read this whole
file before changing anything. Last updated 2026-11-17, branch `engine-v2`.

## 1. What the owner asked for

A flexible engine for 2D and 3D games that renders through DX12, Metal and
Vulkan, is intuitive, has a very powerful level editor, and exports to Windows,
macOS, Android and iOS. The first game is a stylized **3D pixel-art** game, and
the engine must support a **path-traced** renderer for it. Other art styles
must remain possible.

**Art direction, clarified by the owner on 2026-11-16:** "3D pixel art" means a
real 3D game (perspective camera, free movement) whose world *is* pixel art —
hand-pixelled textures and sprites — not smooth 3D pushed through a pixelation
filter. Their references: github.com/siliconjungle/shapeshift-labs-ink-studio
(palette ramps + dithering, though built on splats), github.com/remorses/castle-valley
(a painterly world with pixel sprites), and a Reddit r/IndieDev post titled
"After a year of solo dev, my 3D pixel art medieval..." that could not be
fetched (Reddit blocks it) — ask the owner for screenshots of it. The engine's
answer is in README "Making it look like pixel art"; `scenes/village.scene.json`
is the current reference image. Judge every rendering change against that goal.

Decisions the owner made explicitly (do not revisit without asking):

| Topic | Decision |
| --- | --- |
| Graphics layer | **Hand-written backends** behind our own RHI (not SDL_GPU, not wgpu) |
| Languages | Engine in C++20; gameplay in **Lua** |
| Shaders | **Slang** as the single source language |
| Old OpenGL exercises | Removed (they are in git history before `5bbfab7`) |
| Git | Commit locally on a branch; **the owner does every push** |
| Installing tools | Ask first. Approved so far: cmake, ninja, the four Vulkan brew packages, the Slang release in `~/.cache/ace/slang` |

SDL3 is used for window/input/files only (never for rendering). That choice
was mine, not the owner's; it keeps five platforms' windowing out of scope.

## 2. State of the build: verified vs not

Verified on this machine (Apple M1 Pro, macOS 27.2) by building, running and
looking at screenshots produced with `--screenshot`:

- Metal backend: player (raster and path traced) and editor.
- Vulkan backend through MoltenVK with validation layers on: player (raster
  and path traced) and editor, zero validation messages.
- Editor: default dock layout, hierarchy with children, inspector, transform
  gizmo drawn on a selected entity, asset list, play mode running Lua scripts
  from the scene camera.
- Lua scripts (`spin.lua` visibly moves the Orb in play mode).
- `tools/export_macos.sh`: produces a self-contained `.app` (system libraries
  only) that runs from another working directory.
- Unit tests (`build/bin/ace_tests`): scene round trip, tolerant loading,
  hierarchy, BVH build, move-and-slide, raycasts/overlaps, script params,
  prefabs (including a damaged file with a parent cycle), sprite directions.
- `ace_editor --new <dir>`, then in that project on both backends: a textured
  cube, a `SpriteRenderer`, a perspective camera, the missing-mesh fallback
  (warns, draws a cube) and `pixelArt = false`. Images matched across backends.
- A from-scratch `cmake -S . -B <dir> -G Ninja` + build with default options.

- Editor interactions, by `tools/autotest.sh` on Metal and Vulkan (it injects
  real mouse/keyboard input): orbit, wheel zoom, viewport click-select, gizmo
  X-axis drag, one undo step per drag, undo/redo, duplicate, delete, save to
  disk, play/stop restoring the scene, Build mode place and Shift-erase.
  When you add an editor feature, add steps to `Editor::autotestStep`.
- Raster sun shadows, direct sampling of emissive triangles in the path tracer,
  a hand-made glTF model (`assets/pine.gltf`), and the pixel-art feature set
  (world-space texel mapping, texel-space lighting, dithering, palette, billboard
  sprites) in `scenes/village.scene.json`: Metal and Vulkan images are identical.

- Gameplay end to end with `ace_player projects/pixel_demo --hold W --frames 420
  --screenshot x.png`: key input -> `hero.lua` -> `ace.move` -> the hero walks,
  animates from its sprite sheet, is stopped by the house's colliders, and the
  camera script follows. `--hold <key>` exists for exactly this kind of test.
- Sprite sheets/animation, sprite shadows, `Collider` + `Scene::moveAndSlide`
  (unit-tested).

- **iOS simulator**: `tools/export_ios.sh projects/pixel_demo simulator`, then
  `xcrun simctl boot <device>; simctl install <device> <app>; simctl launch
  <device> com.aceengine.pixeldemo`. The village scene renders full screen on
  an iPhone 17 Pro simulator (Metal). If the screen is black, shut the
  simulator down and use a fresh device; the first attempt hit stale state.
- The path tracer with textures, world texel mapping, cutout sprites, palette
  and per-texel light gathering: `village` with `pathTraced` on, both backends.
- Level switching end to end: `ace_player projects/pixel_demo --hold W
  --frames 420` walks the hero into the village door trigger (`door.lua`,
  `ace.overlaps`), `ace.load_scene` loads `house.scene.json`, and the HUD shows
  that scene's title from Script `params`. Raycasts, overlaps and params parsing
  are unit-tested. Gamepad input is written but no gamepad was connected.
- Sound (`engine/audio`, `ace.play_sound`): with `SDL_AUDIO_DRIVER=disk
  SDL_AUDIO_DISK_OUTPUT_FILE=out.raw`, a walk produced one footstep burst per
  step at the requested volume. This session had no usable speaker
  (CoreAudio refused to start), so nobody has *heard* it; the engine logs one
  warning and stays silent when there is no device.
- Directional sprites (`directions`): unit-tested direction choice; the demo
  hero shows back, side and front views when walking away, sideways and
  towards the camera (`--hold W/A/S` screenshots).
- Offscreen frames (`Device::beginFrame(false)`): `--screenshot`, `--autotest`
  and `--export` runs use a hidden window and never touch the swapchain, so they
  work while the display is asleep. Before this, such runs could stall forever
  at night: macOS stops handing out drawables for an invisible window, so every
  frame was skipped. (A real game skipping frames while hidden is correct.)
- Integer pixel scaling (`integerScale`, on by default): measured on 2560x1440
  screenshots, every row and column edge falls on a multiple of the scale (7 in
  `village`, 8 in `main`). The sky gradient is computed per game pixel.

**Not verified — treat as unknown until someone tries it:**

- Editor interactions the autotest does not cover: pan, drag-and-drop
  re-parenting and asset assignment, Save As, New Scene, the menus, text fields.
  Nobody has used the editor by hand yet.
- **iOS on a real device.** The simulator works (see above); a device build
  (`tools/export_ios.sh <project> device`) is produced unsigned and has never
  been installed. Touch control (`ace.pointer`, hold to walk) is written but
  has only been exercised with scripted keys, never with a finger or mouse.
- Vulkan on a real Vulkan driver (Android, Windows, Linux). Only MoltenVK ran.
- glTF from real tools. `assets/pine.gltf` (hand-written, embedded buffer,
  two materials) loads and renders, but no Blender/Blockbench export with
  textures, multiple nodes or skinning has been tried.
- Audio through real speakers (see the Sound item above).
- Gamepad input (no controller was connected).

**Not written at all:**

- **D3D12 backend.** `engine/CMakeLists.txt` references
  `rhi/d3d12/d3d12_device.cpp`, which does not exist, so a Windows configure
  fails today. `cmake/Shaders.cmake` has an untested DXIL rule.
- Windows and Linux builds have never been attempted (the GCC-only
  `format` attribute in `core/base.h` is already guarded for MSVC; expect other
  MSVC errors). The editor's Export menu uses `popen` and is disabled on Windows.
- Android build and packaging (the NDK and a JDK are not installed here).
- Rigid-body physics, skeletal animation, tilemaps, particles, point/spot
  lights in the raster renderer, OGG/MP3 audio, UI images/panels.

## 3. Architecture

Dependency direction is strictly downward; nothing below may include anything
above it.

```
editor/  player/                executables
engine/script  engine/runtime   Lua, project files
engine/render                   Renderer, PathTracer
engine/assets  engine/scene     meshes/textures; entities/components
engine/platform                 App (window, input, timing)
engine/rhi                      Device + CommandList interface, backends
engine/core                     types, log, math
```

### RHI (`engine/rhi/rhi.h`) — read the header comment first

- Integer handles, id 0 is null. Backends keep their structs in `Pool<T>`.
- WebGPU-style bind groups. In shaders: `[[vk::binding(binding, group)]]`.
  The index in `BindGroupLayoutDesc::bindings[]` *is* the binding number.
- Every uniform-buffer binding takes a dynamic offset. Per-draw constants come
  from one per-frame ring (`Renderer::pushUniform`, 256-byte aligned, 4 MB).
- `setPipeline` must precede `setBindGroup` (both backends need the layout).
- **One frame in flight.** `beginFrame` waits for the previous frame's GPU
  work, which is why buffers can be rewritten every frame. If you add frames
  in flight you must first give every per-frame buffer a ring of copies.
- Destruction is deferred by the backend. `waitIdle()` is a no-op mid-frame.
- Conventions, identical on every backend: right-handed, +Y up, camera looks
  down -Z, depth 0..1 (`GLM_FORCE_DEPTH_ZERO_TO_ONE`), +Y up in clip space,
  counter-clockwise front faces, texture row 0 at the top. Vulkan gets there
  with a negative-height viewport. A D3D12 backend needs no flip.
- Render targets and the backbuffer are sRGB or float; all shading is linear.
  Scene/editor colors are sRGB and converted with `toLinear` on the CPU.

### Shaders (`engine/shaders/*.slang`, `cmake/Shaders.cmake`)

`slangc` compiles each file twice (`vsMain`, `fsMain`) to
`ace_data/shaders/<name>.vs.spv|.fs.spv` next to the executables. Vulkan loads
SPIR-V directly. Metal converts SPIR-V to MSL with SPIRV-Cross **when the
pipeline is created**, because only then are the bind-group layouts known; it
assigns flat Metal indices by walking groups and bindings in order (vertex
buffer is slot 30). C++ structs mirrored in shaders are all `mat4`/`vec4` so
layout rules cannot differ; keep it that way.

### Renderer (`engine/render/renderer.cpp`)

```
scene pass   -> color RGBA16F (linear HDR) + normalDepth RGBA16F (world normal, view depth)
                raster:      shadow.slang + mesh.slang (sun shadow map, texel-space
                             lighting, toon bands or palette-ramp shading, dithering,
                             world-space texel mapping, cutout sprites)
                path traced: pathtrace.slang  (software BVH in the fragment shader;
                             textures from a 1024 atlas, same texel/ramp rules)
present pass -> present.slang: sky, outlines (depth) + convex-edge highlights
                (normals), tone map, palette snap, hard-pixel upscale; then
                screen text (pixel font, imgui.slang) in the same pass
```

- Shared shader code (frame/draw constants, `bayer4`, `linearToSrgb`,
  `rampShade`) lives in `common.slangh`; C++ mirrors are in `renderer.cpp`.
- `integerScale` (default on) picks a whole-number screen-pixels-per-game-pixel
  factor and widens the camera slightly instead of stretching.

- Depth > 9000 in `normalDepth.w` means sky.
- Pixel-art mode renders at `pixelHeight` rows. With an orthographic camera
  and `pixelSnap`, the view is snapped to whole texels, the image is rendered
  one texel larger on every side, and the present pass scrolls by the
  sub-texel remainder. This is what removes shimmer when the camera moves.
- Path tracer: `PathTracer::rebuild` flattens all meshes to world-space
  triangles and builds a median-split BVH whenever a hash of transforms and
  materials changes. Accumulation is alpha blending into the color target
  (`alpha = 1/(n+1)`), cleared on frame 0. A scene change keeps 6 frames of
  history instead of restarting; a view change restarts.

### Scene (`engine/scene/scene.h`)

EnTT registry. Components are plain structs described once in
`componentTypes()` (name + `Field` list). Serialization, the inspector, and
duplication all run off that table. **To add a component: write the struct,
add one `makeType` entry.** Nothing else is needed for it to save, load and be
editable. Loading is tolerant by design (unknown/mistyped keys keep defaults).
Parents are saved as indices into the entity array.

### Editor (`editor/editor.cpp`)

Dear ImGui (docking) drawn through the RHI by `editor/imgui_rhi.cpp`, so it
works on any backend. Undo is whole-scene JSON snapshots taken in `commit()`
after an edit *finishes* (`IsItemDeactivatedAfterEdit`, gizmo release). Play
mode snapshots the scene and restores it on stop.

## 4. Build, run, verify

```sh
cmake -S . -B build -G Ninja && cmake --build build
build/bin/ace_tests
build/bin/ace_player projects/pixel_demo --screenshot /tmp/a.png --frames 5
build/bin/ace_player projects/pixel_demo --scene scenes/pathtraced.scene.json --screenshot /tmp/b.png --frames 60
build/bin/ace_editor projects/pixel_demo --select Tower --screenshot /tmp/c.png   # add --play to capture play mode
build/bin/ace_player projects/pixel_demo --hold W --frames 420 --screenshot /tmp/e.png   # scripted gameplay
tools/autotest.sh                     # editor interaction test, Metal then Vulkan
# Vulkan with validation (must print nothing containing "VUID"):
VK_LAYER_PATH=/opt/homebrew/opt/vulkan-validationlayers/share/vulkan/explicit_layer.d \
  build/bin/ace_player projects/pixel_demo --backend vulkan --validation --screenshot /tmp/d.png
```

**Always look at the screenshot.** Both backends must produce the same image
(compare them pixel by pixel; they have matched exactly so far). Run every
renderer or RHI change on both backends before committing. Screenshot,
autotest and export runs use a hidden window and offscreen frames.

## 5. Working rules

The owner asked that code follow https://github.com/DietrichGebert/ponytail :

- Write only what the task needs. Before adding code ask, in order: does this
  need to exist; is it already in the codebase; does the standard library or
  the platform do it; does a dependency we already have do it.
- Never cut validation at trust boundaries (files, scripts, user input), error
  handling, data-loss protection or security to save lines.
- A deliberate simplification gets a `// shortcut: <what>; <when to revisit>`
  comment. `grep -rn "shortcut:" engine editor` lists the current debt.
- Logic with branches, loops or parsers gets one small test in `tests/`.
- End each report with what was skipped or not checked.

Also: comments explain *why*; public headers start with a comment a newcomer
can read; no new dependency without a line in `cmake/Deps.cmake` pinned to a
tag (or a full commit hash for projects without release tags, as `stb` and
`ImGuizmo` are).

Pitfalls already hit:

- `ace_fetch_sources` must stay a CMake **macro**; as a function the
  `<name>_SOURCE_DIR` variables do not escape.
- A freshly created render target can contain NaN. Never `LoadOp::Load` a
  target that has not been written; NaN survives blending with weight 0.
- Slang lowers `SV_VertexID` using SPIR-V `DrawParameters`, so the Vulkan
  backend requires `shaderDrawParameters` (asserted at startup). If an Android
  device lacks it, draw the fullscreen triangle from a vertex buffer instead.
- Vulkan needs one "rendered" semaphore per swapchain image.
- ImGui colors are sRGB; `imgui.slang` linearizes them because targets are sRGB.
- EnTT is pinned to 3.13.2 and ImGui to `v1.91.9b-docking`; newer majors
  change APIs used here (ImGui 1.92 changed the texture backend contract).
- Textures are sRGB-typed, so shader `Load`s return *linear* values. Compare
  palette colors after `linearToSrgb` (exactly, not `pow(1/2.2)`), or close
  colors swap ramps.
- macOS stops giving drawables to a window that cannot be seen (display asleep,
  hidden, other Space): `nextDrawable` waits a second and returns nil. Anything
  automated must use `beginFrame(false)` with a hidden window.
- SDL turns SIGTERM into a quit event, so `timeout`/`kill` only stops the engine
  if its main loop is still running.
- Each iOS executable gets wrapped in a bare `.app` by CMake; the export script
  copies the binary out of it. iOS needs SPIRV-Cross's
  `ios_support_base_vertex_instance`.
- The editor captures keys through ImGui, which maps Cmd to "Ctrl" on macOS;
  injected test input must send `ImGuiMod_Super` there.

## 6. Roadmap, in order

Each task lists what "done" means. Do them in order unless the owner says
otherwise; ask the owner before installing anything.

1. **Hand-test the editor on macOS** and fix what breaks: every row of the
   README's editor table, plus resize, Save As, New Scene, play/stop while an
   entity is selected, and deleting a parent. Load a real glTF model on both
   backends. Done: all of section 2's "not verified" macOS items are
   confirmed or fixed.
2. ~~Raster shadows~~ done (one 2048 map over the whole scene, sprites cast
   theirs; see the `shortcut:` in `Renderer::fitShadowFrustum` for big levels).
2b. **Pixel-art look** (the owner's main interest). Done: texel-space lighting,
   world-space texel density, palette snap + dithering, palette-ramp shading
   in both render modes, integer scaling, sprite sheets/animation, sprite
   shadows, 4/8-direction sprites. Next: get the owner's verdict on
   `village.scene.json` against their references; a palette editor panel;
   water and foliage (wind-swayed sprite cards); colored light in ramp mode
   (path-traced ramp shading uses light *amount*, so the lantern's warm tint
   is lost); point/spot lights in the raster path.
3. **Path tracer quality**: ~~sample emissive triangles directly~~ done;
   ~~textures (one 1024 atlas) and sprites~~ done; then a two-level BVH (per-mesh
   BLAS built once + TLAS rebuilt per frame) so moving objects are cheap; then
   a denoiser. Hardware ray tracing (Metal/Vulkan/DXR ray queries) is a later
   optimization behind the same interface, not a prerequisite.
4. **D3D12 backend** `engine/rhi/d3d12/d3d12_device.cpp`, implementing
   `createD3D12Device`. Mirror `vulkan_device.cpp` structure. Bind groups map
   to descriptor tables (CBVs as root descriptors so dynamic offsets are just
   a GPU address + offset). Decide register assignment first: either add
   explicit `register(xN, spaceG)` to every shader resource beside the
   `vk::binding`, or read Slang's `-reflection-json`. Fix the MSVC issues in
   `core/base.h`. Needs a Windows machine; nothing here can compile it. Done:
   the four verify commands produce the same images with `--backend d3d12`.
5. **Windows export** (zip of `ace_player.exe`, `ace_data`, `project/`), then
   **iOS** (simulator export works; remaining: device signing, testing touch
   with a finger, an app icon and launch screen), **Android** (Gradle project using SDL3's `android-project` template;
   assets inside the APK are already readable because all file reads go through
   `SDL_LoadFile`; honor `currentTransform`, see the shortcut in
   `createSwapchain`).
   The Android NDK and a JDK are not installed on this machine.
6. **2D feature set**: done: sprite sheets/animation, screen text with a
   built-in 5x7 pixel font (`render/pixel_font.h`, `ace.text`). Next: tilemaps
   with an editor paint tool, sprite sorting for overlapping transparent
   sprites, images/panels for UI (`ace.image`), lowercase glyphs. The old
   anti-aliased `assets/fonts/font_atlas.png` is unused (it clashes with
   pixel art); delete it once the owner agrees.
7. **Gameplay basics**: physics. A kinematic layer exists (`Collider` boxes,
   `Scene::moveAndSlide`, triggers via `overlapping`, `raycast`); boxes are
   axis-aligned and there are no dynamics. Evaluate Jolt (3D) and Box2D (2D)
   before writing rigid bodies. Done: sound (WAV), Lua hot reload, scene loading, trigger
   overlaps, raycasts, gamepad, per-entity script params. Still to do:
   skeletal animation from glTF, compressed audio (OGG; stb_vorbis). (Save
   files `ace.save_data`/`ace.load_data` and Lua globals surviving scene
   changes are done and were tested with a throwaway project.)
8. **Editor power features** (done: gizmo snapping, asset pickers, collider
   outline, Build mode, File > Export, verified with `ace_editor <project>
   --export macos` on a path containing a space; prefabs, which are
   independent copies with no live link to the file). Next: multi-select, copy/paste,
   camera/light icons, a file dialog (`SDL_ShowOpenFileDialog`), crisp fonts on HiDPI displays, a
   game-resolution preview, asset thumbnails.
9. **Engine scale**: replace `Scene::children`' linear scans with cached child
   lists, GPU sub-allocation in Vulkan (VMA), frames in flight, a render graph
   once there are more than ~6 passes, an asset cooking step for release
   builds (precompiled Metal libraries instead of load-time SPIRV-Cross).
