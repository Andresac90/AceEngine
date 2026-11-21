# AceEngine handoff manual

For the next engineer (human or model) continuing this work. Read this whole
file before changing anything. Last updated 2026-11-21, branch `main`.

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
fetched (Reddit blocks it); it is very likely Project Shadowglass (Godot,
dark fantasy, "pixel stabilization" with a perspective camera). The owner
re-sent the note on 2026-11-17 with "I want actual pixels art in a 3D world":
pixels must belong to the world, not to the screen.

Research behind the current answer:
- Dylan Ebert, *Texel Splatting* (arXiv 2603.14587, dylanebert.com/texel-splatting,
  MIT code at github.com/dylanebert/texel-splatting, WebGPU): capture cube maps
  from a grid-snapped probe + an eye probe, draw each texel as a world-space quad,
  Bayer-crossfade on cell changes, shade/posterize/outline per texel. 384 texels
  per face; 40 fps on an iPhone 15. Does not handle animated characters.
- tesseractcat, "Replicating the Shadowglass 3D pixel-art technique"
  (tesseractc.at/shadowglass): the same idea with screen-space ray-marched
  reprojection of near/mid/far cube maps; notes texel splatting shimmers less.
- 80.lv interview with Shadowglass's developer: limited palettes/LUTs, point
  filtering, stepped animation, readability from every angle; details withheld.

The engine's answer is `worldPixels` (engine/render/texel_splat.*), on in
`scenes/village.scene.json`, which is the current reference image. Judge every
rendering change against that goal and get the owner's reaction to it.

Decisions the owner made explicitly (do not revisit without asking):

| Topic | Decision |
| --- | --- |
| Graphics layer | **Hand-written backends** behind our own RHI (not SDL_GPU, not wgpu) |
| Languages | Engine in C++20; gameplay in **Lua** |
| Shaders | **Slang** as the single source language |
| Old OpenGL exercises | Removed (they are in git history before `5bbfab7`) |
| Git | Commit locally on `main` (since 2026-11-18; `engine-v2` was merged and retired); **the owner does every push**; no AI co-author lines in commit messages; commit titles under 72 characters (GitHub Desktop cuts longer ones), details in the body |
| `docs/notes/` | The owner's OpenGL study notes (Phong checklist, Blender UV guide). Not engine docs; keep them |
| Installing tools | Ask first. Approved so far: cmake, ninja, the four Vulkan brew packages, the Slang release in `~/.cache/ace/slang`, `mingw-w64` (D3D12 compile check on macOS); on Windows the LunarG Vulkan SDK (winget `KhronosGroup.VulkanSDK`) |

SDL3 is used for window/input/files only (never for rendering). That choice
was mine, not the owner's; it keeps five platforms' windowing out of scope.

## 2. State of the build: verified vs not

Verified by building, running and looking at screenshots produced with
`--screenshot`. Everything is on macOS 27.2 / Apple M1 Pro unless marked
**Windows**.

Backend status at a glance (2026-11-21):

| Backend | Platform tested | Builds | Runs player + editor | Validation | Image match |
| --- | --- | --- | --- | --- | --- |
| Metal | macOS, M1 Pro; iOS simulator | yes | yes | n/a | reference |
| Vulkan | macOS via MoltenVK | yes | yes | 0 messages | identical to Metal (`main`, `village`, `pathtraced`); `forest` 0.005% |
| Vulkan | Windows 11, RTX 5070 Ti, NVIDIA driver | yes | yes | 0 messages | reference for D3D12 |
| D3D12 | Windows 11, RTX 5070 Ti | yes (MSVC 14.50) | yes | 0 messages (debug layer) | within 0.003-0.3% of Vulkan, see below |


- Metal backend: player (raster and path traced) and editor.
- Vulkan backend through MoltenVK with validation layers on: player (raster
  and path traced) and editor, zero validation messages.
- Editor: default dock layout, hierarchy with children, inspector, transform
  gizmo drawn on a selected entity, asset list, play mode running Lua scripts
  from the scene camera.
- Lua scripts (`spin.lua` visibly moves the Orb in play mode).
- `tools/export_macos.sh`: produces a self-contained `.app` (system libraries
  only) that runs from another working directory.
- Unit tests (`build/bin/ace_tests`, also `ctest --test-dir build`): scene
  round trip, tolerant loading, hierarchy, BVH build, move-and-slide,
  raycasts/overlaps, script params, prefabs (including a damaged file with a
  parent cycle), sprite directions, copy/paste, texel-splat geometry and
  visible-face culling, mesh raycasts (`raycastMesh`, used by `ace.ground`).
  Passes on macOS and Windows.
- `ace_editor --new <dir>`, then in that project on both backends: a textured
  cube, a `SpriteRenderer`, a perspective camera, the missing-mesh fallback
  (warns, draws a cube) and `pixelArt = false`. Images matched across backends.
- A from-scratch `cmake -S . -B <dir> -G Ninja` + build with default options.

- Editor interactions, by `tools/autotest.sh` on Metal and Vulkan (it injects
  real mouse/keyboard input): orbit, wheel zoom, viewport click-select, gizmo
  X-axis drag, one undo step per drag, undo/redo, duplicate, delete, save to
  disk, play/stop restoring the scene, Build mode place and Shift-erase,
  prefab save/place/undo, Shift+click multi-select, copy/paste through the
  system clipboard (the test saves and restores the user's clipboard), delete
  of a multi-selection, a group gizmo drag, and the Palette panel (create,
  edit with a live GPU update including a resize, save, read back from disk),
  and selecting a light by clicking its viewport icon.
  When you add an editor feature, add steps to `Editor::autotestStep`.
- Raster sun shadows, direct sampling of emissive triangles in the path tracer,
  a hand-made glTF model (`assets/pine.gltf`), and the pixel-art feature set
  (world-space texel mapping, texel-space lighting, dithering, palette, billboard
  sprites) in `scenes/village.scene.json`: Metal and Vulkan images are identical.

- **Open-world forest** (`projects/forest`, generated by `tools/make_forest.py`;
  the owner's target is a first-person Skyrim-like game in the style of
  Project Shadowglass): 640x640 terrain, ~820 trees, rocks, logs, ferns, a
  standing-stone circle with a campfire, and a castle on a hill that the
  player walks to and enters (`scripts/first_person.lua`: `ace.ground`,
  `ace.set_look`, `ace.move`, mouse lock). Sunset sun behind the castle, pixel
  clouds, stars, haze (`Atmosphere` in common.slangh). ~115k triangles.
  Verified: Metal and Vulkan images match, no validation messages; a scripted
  walk from the forest to the keep (gravity, slopes, gate, colliders).
  Cost while walking on an M1 Pro (frame time, probes re-tracing every frame):
  path traced 384/2 bounces 109 ms, 320/1 ~60 ms, 256/1 50 ms; raster world
  pixels 15 ms. A still camera settles and gets cheap. Not verified: playing
  it by hand with a real mouse (mouse lock, Esc, look feel).
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
- **World pixels** (`worldPixels`, `engine/render/texel_splat.*`): a unit test
  checks capture and splat agree on texel positions; Metal and Vulkan images are
  pixel-identical. On a 60-frame camera orbit at a fixed timestep
  (`ace_player ... --fixed-dt 0.0166667 --frames 60 --frames-to out/`), the share
  of pixels that change between frames after the best shift is 19% (ground) and
  10% (wall), versus 34% and 14% with screen pixels, with no spikes when the grid
  probe moves. Only the cube faces the camera can see are captured and drawn
  (`TexelSplatter::visibleFaces`, unit-tested; images identical to drawing all
  six). Cost at 2560x1440 on an M1 Pro, 300-frame runs including ~1 s startup:
  10.0 ms/frame on Metal, 13.3 ms on Vulkan, versus about 5 ms for screen
  pixels. Vulkan framebuffers are now cached (zero validation messages); a
  re-measurement under heavy machine load (load average 10) showed Vulkan 12%
  slower than Metal instead of 33%, but absolute times were not trustworthy:
  re-time on an idle machine.
- Point lights (`Light.point`, up to 8 nearest the camera, banded; shadows only
  when path traced; every mode, path-traced night checked on both backends): lantern light in `house.scene.json`; a night
  copy of the village renders dark with the lantern lighting the path, Metal and
  Vulkan identical. Ramp shading treats a white intensity-3 light as fully lit.
- `tools/make_editor_app.sh` -> `build-export/AceEngine.app` (Metal only, SDL
  static, ad-hoc signed, icon from `tools/make_app_icon.py`): no non-system
  libraries (`otool -L`); started from `/` it reopened the remembered project
  and rendered. Not checked by hand: the folder picker, File > Open Project,
  and the unsaved-changes dialog (native dialogs cannot be driven by the
  autotest). The editor keeps `imgui.ini` and `last_project.txt` in
  `~/Library/Application Support/AceEngine/Editor/`; automated runs read but never
  write them.
- Path-traced world pixels (`pathTraced` + `worldPixels`): probe faces are
  captured by the path tracer (1 ray per texel per frame, a running average per
  face, tracing stops once a face has 64 unchanged frames), sprites skipped by
  primary rays but casting shadows. Metal = Vulkan pixel for pixel. Still
  camera on the village: 16.5 ms/frame vs 11.4 ms raster world pixels (machine
  under load, load average ~14).
- Path-tracing denoiser (`denoise`, default on): the path tracer writes light,
  surface color and emission separately; three a-trous passes (pt_denoise.slang,
  normal/depth/brightness edge-stopping, brightness tolerance 0.4 x full-light /
  sqrt(frames)) smooth only the light, then pt_compose.slang applies color or
  picks the palette-ramp color. Share of pixels differing from a converged image
  (village, still camera): world pixels 13.7% -> 5.9% after 1 frame, 6.2% -> 0.6%
  after 4, 3.1% -> 0.4% after 16; screen pixels 9.3% -> 5.4%, 3.1% -> 1.1%,
  1.9% -> 0.8%. Measured with a throwaway script (render frames 1/4/16/300,
  count pixels differing by >24 from frame 300).
- Integer pixel scaling (`integerScale`, on by default): measured on 2560x1440
  screenshots, every row and column edge falls on a multiple of the scale (7 in
  `village`, 8 in `main`). The sky gradient is computed per game pixel.

- **Windows (2026-11-21, Windows 11, RTX 5070 Ti Laptop + Intel iGPU, MSVC
  14.50 from VS 2026 Build Tools, Vulkan SDK 1.4.363).** The whole engine
  builds warning-free with MSVC; `ace_tests` passes; the DXIL rule in
  `cmake/Shaders.cmake` works. **D3D12 and Vulkan** both run section 4's
  verify commands plus `village` (world pixels) and `projects/forest`
  (path-traced world pixels) with **zero validation messages** (D3D12 debug
  layer, `VK_LAYER_KHRONOS_validation`), and `tools/autotest.sh d3d12 vulkan`
  passes in Git Bash. Images look right. D3D12 picks the high-performance GPU
  (`EnumAdapterByGpuPreference`; the Intel iGPU is listed first) and filters
  the "no optimized clear value" hint (clear colors are per pass).
- **D3D12 vs Vulkan images** (same GPU, `--fixed-dt 0.0166667`; time-based
  scenes differ otherwise). Each backend is deterministic run to run. Share of
  pixels that differ: `main` 0.003% (max 54/255), path traced at most 1/255,
  `village` and the scripted walk 0.001%, editor 0.02%, `forest` 0.3%.
  Why they are not identical, when Metal and MoltenVK were: those two share
  Apple's shader compiler; here DXC and NVIDIA's SPIR-V compiler round the
  last bits differently, and any hard decision near a boundary can flip.
  The big one was in world pixels (`village` was 0.4%; 0.000% with
  `worldPixels` off): splat squares are 2% oversized and nearly coplanar, so
  the depth test in the overlap was decided by rounding noise, i.e.
  z-fighting. `texel_splat.slang` now pushes each texel of a 2x2 block back
  by a different fixed amount (0.05% of the camera distance per step, along
  the camera ray so the square does not move on screen); 0.4% -> 0.001%.
  The forest's remaining 0.3% was in the sky: stars and dither dots at cloud
  edges where smooth noise sits on a palette/dither threshold.
- **Fix: stars that agree across GPUs (2026-11-21, Mac).**
  *Symptom:* D3D12 and Vulkan showed some stars in different places
  (part of the forest's 0.3%). *Cause:* two decisions used math whose
  result differs between GPU compilers: the star's cell came from
  `floor(atan2(...) * 220)` / `floor(asin(...) * 220)` (no API specifies
  their precision), and "is there a star in this cell" came from a float
  hash whose multiply-adds a compiler may fuse or not. A difference in the
  last bit flips either decision. *Fix* (`skyRadiance` in `common.slangh`):
  the cell is now found on a cube around the camera (largest axis, then one
  division; 400x400 cells per face, about the old density), and the star
  test is an integer PCG hash of the cell index, which is bit-exact
  everywhere. A baked star texture was considered and rejected: it would
  still be looked up at a coordinate computed from the view direction, so it
  removes neither rounding source. *Evidence:* on the Mac the sky looks the
  same (same density; stars moved because the cell layout changed), Metal
  and Vulkan still match, zero validation messages. *Open:* re-run the
  D3D12-vs-Vulkan forest comparison on Windows and record the new number
  here. Cell boundaries can still flip in rare pixels (division is not
  exact either), and cloud-edge dither is unchanged: both are accepted.
- **Windows editor program**: `tools/make_editor_windows.ps1` (Release,
  `ACE_EDITOR_WINDOWED=ON` = no console, icon from `editor/resources/editor.rc`
  + `AppIcon.ico`, both from `tools/make_app_icon.py`) -> `build-export\AceEngine\`
  and a Start menu shortcut. Started from another folder it found its shaders
  and rendered. Release runtime only (`MSVCP140.dll`, from the VC++
  redistributable); `vulkan-1.dll` is delay-loaded so PCs without a Vulkan
  driver still start on D3D12. Settings live in `%APPDATA%\AceEngine\Editor\`.
  Not checked by hand: the folder picker, Open script (now builds
  `file:///C:/...` URLs), the unsaved-changes dialog, HiDPI (no
  `SDL_WINDOW_HIGH_PIXEL_DENSITY`, so Windows may scale the window blurrily).
- Not checked on Windows: playing or editing by hand, AMD GPUs, the Intel
  iGPU (D3D12 or Vulkan), Windows export.
- **Mac re-check after the Windows changes (2026-11-21):** with the splat
  tie-break, `main`, `village` and `pathtraced` are still pixel-identical
  between Metal and Vulkan (`--fixed-dt 0.0166667 --frames 60`), zero
  validation messages; `forest` differs on 0.005% of pixels (179 pixels in
  one band of distant ground, at most 19/255: a value sitting on a palette /
  dither threshold; the sky matched here); unit tests and
  `tools/autotest.sh` pass on both. The world-pixel shimmer numbers above
  were *not* re-measured; the tie-break should only lower them.

**Not verified — treat as unknown until someone tries it:**

- Editor interactions the autotest does not cover: pan, drag-and-drop
  re-parenting and asset assignment, Save As, New Scene, the menus, text fields.
  Nobody has used the editor by hand yet.
- **iOS on a real device.** The simulator works (see above); a device build
  (`tools/export_ios.sh <project> device`) is produced unsigned and has never
  been installed. Touch control (`ace.pointer`, hold to walk) is written but
  has only been exercised with scripted keys, never with a finger or mouse.
- Vulkan on Android or Linux (Windows/NVIDIA is verified, see above).
- glTF from real tools. `assets/pine.gltf` (hand-written, embedded buffer,
  two materials) loads and renders, but no Blender/Blockbench export with
  textures, multiple nodes or skinning has been tried.
- Audio through real speakers (see the Sound item above).
- Gamepad input (no controller was connected).

**Not written at all:**

- A Linux build has never been attempted. The editor's Export menu uses
  `popen` and is disabled on Windows.
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
  from one per-frame ring (`Renderer::pushUniform`, 256-byte aligned, 16 MB;
  the forest's ~1000 objects x several probe views needed more than 4 MB).
- Call `setPipeline` before `setBindGroup`: Metal and Vulkan resolve bindings
  against the current pipeline's layout. (D3D12 would accept either order; it
  applies groups at draw time.)
- **One frame in flight.** `beginFrame` waits for the previous frame's GPU
  work, which is why buffers can be rewritten every frame. If you add frames
  in flight you must first give every per-frame buffer a ring of copies.
- Destruction is deferred by the backend. `waitIdle()` is a no-op mid-frame.
- Conventions, identical on every backend: right-handed, +Y up, camera looks
  down -Z, depth 0..1 (`GLM_FORCE_DEPTH_ZERO_TO_ONE`), +Y up in clip space,
  counter-clockwise front faces, texture row 0 at the top. Vulkan gets there
  with a negative-height viewport; Metal and D3D12 need no flip.
- Render targets and the backbuffer are sRGB or float; all shading is linear.
  Scene/editor colors are sRGB and converted with `toLinear` on the CPU.

### Backends (`engine/rhi/<api>/`)

How each backend maps the RHI. All three share one frame in flight, deferred
destruction and the conventions above.

| RHI concept | Metal (`metal_device.mm`) | Vulkan (`vulkan_device.cpp`) | D3D12 (`d3d12_device.cpp`) |
| --- | --- | --- | --- |
| Shaders | SPIR-V -> MSL with SPIRV-Cross at pipeline creation | SPIR-V | DXIL (built by slangc + dxcompiler on Windows) |
| Bind group | flat buffer/texture/sampler indices assigned per pipeline | descriptor set | uniform buffers = root CBVs, storage buffers = root SRVs, textures and samplers = one descriptor-table block each, written at bind-group creation |
| Dynamic offset | buffer offset argument | dynamic UBO offset | GPU address + offset (asserted 256-byte aligned) |
| Pipeline layout | implicit | pipeline layout from the group layouts | root signature per pipeline (version 1.0) |
| Render pass | encoder per pass | classic render pass; framebuffers cached | `OMSetRenderTargets` + clears; full-target viewport/scissor set at pass start |
| Resource states | tracked by Metal | layouts in render pass ops | every texture rests in one state between passes (shader resource; depth: depth write; swapchain: present); only pass begin/end, uploads and read-backs issue barriers |
| Buffers | shared storage | host-visible memory | upload heap, persistently mapped |
| Texture upload / read-back | `replaceRegion` / blit + wait | staging + wait | staging buffer on a separate list + fence wait |
| GPU choice | system default | first suitable device | high-performance adapter (`EnumAdapterByGpuPreference`) |
| Debug | Xcode | `--validation` = Khronos validation layer | `--validation` = debug layer; messages printed after each frame |

D3D12 history: written on macOS on 2026-11-20 and compile-checked with
MinGW's Windows headers (`-Wall -Wextra`, all imports resolve); the first
Windows run (2026-11-21) needed only two changes (high-performance adapter
choice, filtering the "no optimized clear value" hint) and the rest of the
engine built warning-free under MSVC. Known limits, each marked `shortcut:` in
the file: fixed descriptor heaps (65536 views, 2048 samplers), freed
descriptor blocks reused only by blocks of the same size, every buffer in an
upload heap (meshes and the BVH would read faster from a default heap on
discrete GPUs), synchronous texture uploads.

Vulkan's role on Windows: it runs on the same GPU as D3D12, so it is the
reference for D3D12 images (Metal only exists on Apple hardware). The two use
different shader compilers (DXC vs NVIDIA's SPIR-V compiler), so results can
differ in the last bits; see section 2 for the numbers and causes.

### Shaders (`engine/shaders/*.slang`, `cmake/Shaders.cmake`)

`slangc` compiles each file twice (`vsMain`, `fsMain`) to
`ace_data/shaders/<name>.vs.spv|.fs.spv` (and `.dxil` on Windows) next to the
executables. Every resource declares both bindings, e.g.
`[[vk::binding(1, 2)]] Texture2D albedo : register(t1, space2);` (binding ->
register number, group -> space; `b` ConstantBuffer, `t` Texture2D /
StructuredBuffer, `s` SamplerState), and vertex inputs use
`[[vk::location(n)]] ... : ATTRIB<n>`. A new resource without its register
compiles but binds the wrong slot on D3D12. Vulkan loads SPIR-V directly. Metal converts SPIR-V to MSL with SPIRV-Cross **when the
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
                path-traced denoise: pt_denoise.slang (3 a-trous passes on the
                             light only) -> pt_compose.slang (albedo, emission,
                             palette ramp)
world pixels -> texel_splat.cpp: capture into 6-face atlases (raster, or the
                path tracer with one running average per face), grid + eye
                [+ previous grid while crossfading]; only faces the camera can
                see -> probe_post.slang (per-texel haze, outlines, tone,
                palette, dither) -> texel_splat.slang (one square per texel,
                no vertex buffer, fixed 2x2 depth tie-break) -> sprites direct
present pass -> present.slang: sky (Atmosphere: gradient, sun disc + glow, pixel
                clouds, stars), distance haze (screen pixels), outlines (depth)
                + convex-edge highlights (normals), tone map, palette snap,
                hard-pixel upscale; then screen text (pixel font, imgui.slang)
```

- Shared shader code (frame/draw constants, `bayer4`, `linearToSrgb`,
  `rampShade`, `Atmosphere` with `skyRadiance`/`applyFog`) lives in
  `common.slangh`; C++ mirrors are in `renderer.h`/`renderer.cpp`. The sun is
  the first `Light` without `point` (`findSun`); sky and haze settings are
  Render Settings (`sunSize`, `sunGlow`, `fog*`, `cloud*`, `stars`).
- `integerScale` (default on) picks a whole-number screen-pixels-per-game-pixel
  factor and widens the camera slightly instead of stretching.

- Depth > 9000 in `normalDepth.w` means sky.
- Pixel-art mode renders at `pixelHeight` rows. With an orthographic camera
  and `pixelSnap`, the view is snapped to whole texels, the image is rendered
  one texel larger on every side, and the present pass scrolls by the
  sub-texel remainder. This is what removes shimmer when the camera moves.
- Path tracer: `PathTracer::rebuild` flattens all meshes to world-space
  triangles and builds a binned-SAH BVH (16 bins per axis) whenever a hash of
  transforms and materials changes; the shader walks it front to back (nearer
  child first, skipping subtrees behind the closest hit). SAH + ordered
  traversal took the forest walk from 269 to 109 ms per frame. Sprite cards in
  probe captures only cast shadows, so they face the sun, not the camera:
  turning the view never rebuilds the BVH. Accumulation is alpha blending into the color target
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
# Windows, from a Developer PowerShell (or after Launch-VsDevShell.ps1 -Arch amd64):
#   same cmake lines; build\bin\ace_player.exe ... --backend d3d12|vulkan --validation
#   tools/autotest.sh d3d12 vulkan   (in Git Bash)
# D3D12 compile check on macOS (no run; needs `brew install mingw-w64`, must print nothing):
x86_64-w64-mingw32-g++ -std=c++20 -fsyntax-only -Wall -Wextra -Wno-unused-parameter \
  -Wno-missing-field-initializers -DACE_RHI_D3D12=1 -DACE_RHI_METAL=0 -DACE_RHI_VULKAN=0 \
  -Iengine -I/opt/homebrew/opt/sdl3/include engine/rhi/d3d12/d3d12_device.cpp
# Vulkan with validation (must print nothing containing "VUID"):
VK_LAYER_PATH=/opt/homebrew/opt/vulkan-validationlayers/share/vulkan/explicit_layer.d \
  build/bin/ace_player projects/pixel_demo --backend vulkan --validation --screenshot /tmp/d.png
```

**Always look at the screenshot.** Compare backends pixel by pixel with
`--fixed-dt 0.0166667` (time-based scenes differ otherwise): on a Mac, Metal
and Vulkan must match exactly (they share Apple's shader compiler); on
Windows, D3D12 and Vulkan may differ on well under 1% of pixels (section 2
explains why), and anything above that is a bug. Run every renderer or RHI
change on every backend you can reach before committing. Screenshot,
autotest and export runs use a hidden window and offscreen frames.

## 5. Working rules

The owner asked that code follow https://github.com/DietrichGebert/ponytail :

- Write only what the task needs. Before adding code ask, in order: does this
  need to exist; is it already in the codebase; does the standard library or
  the platform do it; does a dependency we already have do it.
- Never cut validation at trust boundaries (files, scripts, user input), error
  handling, data-loss protection or security to save lines.
- Use what the codebase already has, then the standard library or platform,
  then an installed dependency; never add a dependency for a few lines.
- No speculative abstraction, option or "for later" code; keep the existing
  layers and conventions. Finish every part the change needs (callers, tests,
  docs).
- Bug fixes: find every caller first and fix the root cause once.
- A deliberate simplification gets a `// shortcut: <the limit>, <when to
  upgrade>` comment. `grep -rn "shortcut:" engine editor` lists the current debt.
- Logic with branches, loops or parsers gets one small test in `tests/`.
- Comments say only the *why* the code cannot show.
- Every bug fix is recorded where the next person will look: the commit body
  and, for anything about rendering or a platform, a short entry in this file
  (symptom, cause, fix, evidence, what is still open), like the splat
  tie-break and star entries in section 2. Pitfalls that apply beyond one
  fix also go in the list below.
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
- Anything that accumulates over frames in the renderer's scene targets (the
  path tracer) must restart when it did not draw the previous frame: other
  modes draw into the same targets, and recreated targets can reuse handle ids,
  so comparing ids/sizes is not enough (`PathTracer::render`, autotest step 44).
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
- World pixels: a lower-priority probe must be pushed back in *world* space
  (fraction of the camera distance), not clip space: perspective depth is
  nearly 1.0 beyond a few metres, so a clip-space push sends splats to the far
  plane and they vanish. Orthographic cameras use screen pixels instead.
- A new editor panel will float off-screen for anyone whose `imgui.ini` predates
  it; dock it on first use (see `drawPalette`, `SetNextWindowDockID`).
- After `Assets::reload()` or a texture changes size, call
  `Renderer::clearTextureCaches()`: bind groups are cached by texture id and ids
  are reused.
- MSVC: `project()` caches `CMAKE_BUILD_TYPE=Debug` when none is given, so
  the RelWithDebInfo default must be set *before* `project()` (it is). A Debug
  build is slow and needs Visual Studio's debug runtime DLLs to start.
- `vswhere -latest` may pick a Visual Studio install without the CMake/Ninja
  component; ask for `Microsoft.VisualStudio.Component.VC.CMake.Project` too.
- Anything drawn as overlapping, nearly coplanar pieces needs a fixed depth
  tie-break, or the winner depends on the GPU compiler and flickers.
- Shader transcendentals (`atan2`, `asin`, `pow`, `sin`) differ between GPU
  compilers; do not feed them into `floor`/`step` where exact repeatability
  across backends matters. For random-looking discrete choices use an
  integer hash (`pcgHash` in `common.slangh`), not a float one.
- The editor captures keys through ImGui, which maps Cmd to "Ctrl" on macOS;
  injected test input must send `ImGuiMod_Super` there.
- D3D12 root CBVs need 256-byte aligned addresses; the ring is aligned, and
  the backend asserts it for any other uniform offset.
- Every shader resource needs its D3D `register(...)` beside `vk::binding`
  (see Shaders). Metal and Vulkan will not notice a missing one; D3D12 will.

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
2a. **World pixels, next steps.** Show the owner and tune `probeResolution` /
   `probeCell` / palette with them. Then: cache the grid probe while nothing in
   view changes (visible-face culling and Vulkan framebuffer caching are done); crossfade the eye probe's disocclusion fill (it can
   shimmer at silhouettes); outlines across cube-face seams (post pass stops at
   face edges); sprites with the same pixel size as the world; a
   mobile budget (384 per face is heavy for phones; try 256).
2b. **Pixel-art look** (the owner's main interest). Done: texel-space lighting,
   world-space texel density, palette snap + dithering, palette-ramp shading
   in both render modes, integer scaling, sprite sheets/animation, sprite
   shadows, 4/8-direction sprites, a Palette editor panel. Next: get the
   owner's verdict on `village.scene.json` against their references;
   water and foliage (wind-swayed sprite cards); colored light in ramp mode
   (path-traced ramp shading uses light *amount*, so the lantern's warm tint
   is lost); raster shadows for point lights; a
   day/night setting that animates sun, ambient and sky together.
3. **Path tracer quality**: ~~sample emissive triangles directly~~ done;
   ~~denoiser~~ done (spatial only; a temporal pass with motion reprojection
   would keep history while the camera moves, which is the next big win);
   ~~textures (one 1024 atlas) and sprites~~ done; ~~SAH~~ done. **Forest
   performance is the priority**: walking re-traces the eye probe and each new
   grid probe from scratch. Ideas, cheapest first: reproject the previous
   probe's converged light into the new one instead of starting at frame 0;
   trace the eye probe at half resolution; LOD meshes for far trees (one
   cone); cap the distance of secondary rays. Then a two-level BVH (per-mesh
   BLAS built once + TLAS rebuilt per frame) so moving objects are cheap. Hardware ray tracing (Metal/Vulkan/DXR ray queries) is a later
   optimization behind the same interface, not a prerequisite.
4. ~~**D3D12 backend**~~ done 2026-11-21: written, then verified on Windows
   with Vulkan as the reference (sections 2 and 3). Open: AMD and Intel GPUs,
   and the `shortcut:` limits listed under Backends.
5. **Windows export** (zip of `ace_player.exe`, `ace_data`, `project/`; link
   the C runtime statically or ship the VC++ redistributable; the editor's
   Export menu needs a replacement for `popen` + shell scripts, e.g.
   `SDL_CreateProcess` and a PowerShell script), then
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
   outline, Build mode, light/camera icons and lamp range, File > Export, verified with `ace_editor <project>
   --export macos` on a path containing a space; prefabs, which are
   independent copies with no live link to the file; multi-select; copy/paste).
   Next: box (marquee) selection, a file dialog (`SDL_ShowOpenFileDialog`), crisp fonts on HiDPI displays, a
   game-resolution preview, asset thumbnails.
9. **Engine scale**: replace `Scene::children`' linear scans with cached child
   lists, GPU sub-allocation in Vulkan (VMA) and D3D12 (placed resources /
   D3D12MA), static buffers in GPU-local memory on all backends, frames in flight, a render graph
   once there are more than ~6 passes, an asset cooking step for release
   builds (precompiled Metal libraries instead of load-time SPIRV-Cross).
