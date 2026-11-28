# AceEngine rendering

How the renderer works, why it is built this way, what has been measured, and
where its limits are. For project status, decisions and the roadmap see
[HANDOFF.md](HANDOFF.md); for usage see the [README](../README.md).

Contents:

1. Goals
2. Conventions
3. RHI and backends
4. Shaders
5. Frame structure
6. Raster path
7. World pixels (texel splatting)
8. Path tracer
8b. Crowds
9. Atmosphere
10. Present pass and color pipeline
11. Cross-backend consistency
12. Performance
13. Known limitations
14. References

New to a term? Every one used here is defined in [GLOSSARY.md](GLOSSARY.md),
grouped by topic.

## 1. Goals

- **3D pixel art whose pixels belong to the world.** The first game is a
  first-person, open-world, dark-fantasy game in pixel art (references:
  Project Shadowglass, Dylan Ebert's *Texel Splatting*). Pixels must not swim
  over surfaces when a perspective camera moves, so a screen-space pixelation
  filter is not enough.
- **Optional path tracing** for the same look: soft shadows, bounce light,
  emissive lights, still landing on the texture's pixel grid.
- **Three native graphics APIs** (Metal, Vulkan, D3D12) behind one small
  interface, with one shader source.
- **Other styles stay possible:** every pixel-art feature is a Render Setting
  that can be turned off (smooth shading, no palette, full resolution).

## 2. Conventions

Identical on every backend:

- Right-handed world, +Y up, the camera looks down -Z.
- Clip space +Y up, depth 0..1 (`GLM_FORCE_DEPTH_ZERO_TO_ONE`), counter-clockwise
  front faces, texture row 0 at the top. Vulkan reaches this with a
  negative-height viewport; Metal and D3D12 need no flip.
- All shading is in linear light. Render targets and the backbuffer are sRGB
  or float. Colors stored in scenes and shown in the editor are sRGB and
  converted with `toLinear` on the CPU. Textures are sRGB-typed, so shader
  loads return linear values.
- Uniform structs mirrored between C++ and Slang contain only `mat4`/`vec4`
  members, so std140/HLSL packing rules cannot disagree.

## 3. RHI and backends

`engine/rhi/rhi.h` is the only GPU API the rest of the engine sees (read its
header comment first).

- Resources are integer handles (id 0 = null), stored by each backend in a
  `Pool<T>` slot map.
- Binding follows the WebGPU / Vulkan bind-group model: a pipeline declares up
  to 4 group layouts of up to 8 bindings; draws bind matching groups.
- Every uniform-buffer binding takes a **dynamic offset**. Per-draw constants
  are pushed into one 16 MB per-frame ring (`Renderer::pushUniform`, 256-byte
  aligned) instead of thousands of small buffers.
- **One frame in flight.** `beginFrame` waits for the previous frame, so
  buffers can be rewritten each frame. Destruction is deferred by the backend.
- `beginFrame(false)` renders an offscreen frame that never touches the
  swapchain; screenshots, automated tests and exports use it with a hidden
  window (macOS stops handing out drawables to invisible windows).
- Only vertex and fragment stages exist (no compute yet); the path tracer and
  denoiser run as fullscreen fragment passes for that reason.

How each backend maps the interface:

| RHI concept | Metal (`metal_device.mm`) | Vulkan (`vulkan_device.cpp`) | D3D12 (`d3d12_device.cpp`) |
| --- | --- | --- | --- |
| Shaders | SPIR-V -> MSL with SPIRV-Cross at pipeline creation | SPIR-V | DXIL (slangc + dxcompiler, Windows builds) |
| Bind group | flat buffer/texture/sampler indices assigned per pipeline | descriptor set | uniform buffers = root CBVs, storage buffers = root SRVs, textures and samplers = one descriptor-table block each, written at bind-group creation |
| Dynamic offset | buffer offset argument | dynamic UBO offset | GPU address + offset (asserted 256-byte aligned) |
| Pipeline layout | implicit | pipeline layout from the group layouts | root signature per pipeline (version 1.0) |
| Render pass | encoder per pass | classic render pass; framebuffers cached | `OMSetRenderTargets` + clears; full-target viewport and scissor set at pass start |
| Resource states | tracked by Metal | image layouts in render pass operations | every texture rests in one state between passes (shader resource; depth: depth write; swapchain: present); only pass begin/end, uploads and read-backs issue barriers |
| Buffers | shared storage | host-visible memory | upload heap, persistently mapped |
| Upload / read-back | `replaceRegion` / blit + wait | staging buffer + wait | staging buffer on a separate command list + fence wait |
| GPU choice | system default | first suitable device | high-performance adapter (`EnumAdapterByGpuPreference`) |
| Debug | Xcode | `--validation`: Khronos validation layer | `--validation`: debug layer, messages printed after each frame |

Ordering rule: call `setPipeline` before `setBindGroup` (Metal and Vulkan
resolve bindings against the current pipeline; D3D12 applies groups at draw
time and would accept either order).

**D3D12 history.** Written on macOS (2026-11-20) without a Windows machine and
compile-checked with MinGW's Windows headers (`-Wall -Wextra`, every import
resolved). The first Windows run (2026-11-21, RTX 5070 Ti) needed two
changes: choosing the high-performance GPU (laptops list the integrated one
first) and filtering the debug layer's "no optimized clear value" hint, since
clear colors are chosen per pass. Known limits, each marked `shortcut:` in the
file: fixed descriptor heaps (65,536 views, 2,048 samplers); freed descriptor
blocks are reused only by blocks of the same size; every buffer lives in an
upload heap (meshes and the BVH would read faster from a default heap on a
discrete GPU); texture uploads are synchronous.

## 4. Shaders

Slang is the single source language (`engine/shaders/*.slang`, shared code in
`common.slangh`). `cmake/Shaders.cmake` compiles each file's `vsMain` and
`fsMain` to SPIR-V, plus DXIL on Windows, into `ace_data/shaders/`.

Every resource carries both binding schemes, so one source serves all APIs:

```hlsl
[[vk::binding(1, 2)]] Texture2D albedo : register(t1, space2);
```

Binding number -> register number, group -> register space; `b` for
ConstantBuffer, `t` for Texture2D and StructuredBuffer, `s` for SamplerState.
Vertex inputs use `[[vk::location(n)]]` with the semantic `ATTRIB<n>`. Adding
the registers left the SPIR-V byte-identical. A resource missing its register
still compiles and works on Metal and Vulkan but binds the wrong slot on D3D12.

Metal converts SPIR-V to MSL when a pipeline is created, because only then are
the bind-group layouts known; it assigns flat Metal indices by walking groups
and bindings in order (vertex buffer in slot 30). iOS needs SPIRV-Cross's
`ios_support_base_vertex_instance` because Slang lowers `SV_VertexID` with
draw parameters (Vulkan requires the `shaderDrawParameters` feature for the
same reason).

## 5. Frame structure

```
scene pass    -> color RGBA16F (linear HDR) + normalDepth RGBA16F (world normal, view depth)
                 raster:      shadow.slang (sun shadow map) + mesh.slang
                 path traced: pathtrace.slang (reprojects last frame) -> pt_copy.slang
                              (keeps normal/depth) -> pt_denoise.slang x3 -> pt_compose.slang
                 crowds:      crowd.slang (raster) / traced inside pathtrace.slang (section 8b)
world pixels  -> instead of the plain scene pass, for perspective cameras:
                 capture probes (raster or path traced) -> probe_post.slang ->
                 texel_splat.slang squares -> sprites drawn directly
present pass  -> present.slang: sky, haze, outlines, tone map, palette snap,
                 integer upscale; then screen text (imgui.slang, pixel font)
editor        -> Dear ImGui drawn through the same RHI (editor/imgui_rhi.cpp)
```

`normalDepth.w` > 9000 marks sky. The renderer picks the scene pass from the
scene's Render Settings each frame, so modes can be switched live in the editor.

## 6. Raster path (`mesh.slang`, `shadow.slang`)

- **World-space texel mapping** (`worldTexels`): UVs are projected from world
  position along the dominant normal axis at `texelsPerUnit`, so every object
  shows texture pixels of the same world size regardless of scale.
- **Texel-space lighting** (`texelLighting`): the shading point is moved to the
  centre of the texture texel under the pixel, by solving the screen-space UV
  derivatives, so light and shadow edges follow the texture's pixel grid.
- **Toon bands** with 4x4 Bayer dithering between bands (`toonBands`, `dither`).
- **Palette-ramp shading** (`rampShading`): the palette image's rows are
  hand-picked color ramps. The albedo picks its nearest palette entry (compared
  in sRGB, perceptually weighted); the light amount then steps along that
  row instead of darkening the color. Light amount is measured against a fixed
  reference (a white light of intensity 3), so night scenes stay dark.
- **Sun shadows**: one 2048x2048 R32F map fitted to the whole scene; sprites
  cast shadows too.
- **Point lights**: up to 8 nearest the camera, banded like the sun, no raster
  shadows.
- **Sprites**: camera-facing cards with cutout alpha, sheet animation and
  4/8-direction sheets (the row is chosen from the camera angle).
- **Pixel-art resolution** (`pixelArt`, `pixelHeight`): the scene renders at a
  low resolution. With an orthographic camera and `pixelSnap`, the view is
  snapped to whole texels, rendered one texel larger, and the present pass
  scrolls by the sub-texel remainder, which removes crawl when panning.

## 7. World pixels (texel splatting) (`texel_splat.*`, `probe_post.slang`)

Screen pixels crawl over surfaces when a perspective camera moves. World
pixels (after Ebert, arXiv 2603.14587) make each pixel a small object in the
world instead:

1. **Capture.** The scene is rendered into cube-map atlases (6 faces of
   `probeResolution`^2 texels; color RGBA16F, normal + distance RGBA32F because
   splats are placed from that distance) around two probes. The **grid probe**
   sits at a point snapped to a world grid of `probeCell` units, so it is
   stable while the camera moves inside a cell. The **eye probe** sits at the
   camera and fills what the grid probe cannot see. When the camera crosses
   a cell, the previous grid probe is kept and crossfaded out over 0.35 s
   with a Bayer dissolve. Only the faces the camera can see are captured and
   drawn (`visibleFaces`, unit-tested).
2. **Post** (`probe_post.slang`), per captured texel: distance haze, outlines
   and highlights from the atlas's depth and normals, tone mapping, palette
   snap with dithering anchored to the texel, not the screen.
3. **Splat** (`texel_splat.slang`): every texel becomes a square lying on the
   captured surface's plane, drawn from the real camera with no vertex
   buffer (6 vertices per texel, empty texels collapse). Squares are 2%
   oversized to close gaps; to stop the overlap from z-fighting, each texel
   of a 2x2 block is pushed back along the camera ray by a different fixed
   amount (0.05% of the distance per step), which changes depth only.
4. **Sprites** are drawn directly afterwards: they are pixel art already.

Orthographic cameras use screen pixels (with `pixelSnap` they are already
stable). Path-traced capture keeps a running average per probe face and stops
tracing a face after 64 unchanged frames. In captures, sprite cards only cast
shadows, so they face the sun rather than the camera: turning the view never
changes the geometry or rebuilds the BVH.

**Measured stability** (`tools/measure_shimmer.py`, reproducible): the village
camera orbits at 30 degrees per second for 60 frames at 60 Hz; for each pair
of consecutive frames, the image is compared after the best integer shift
(up to 4 game pixels), and the share of game pixels that still differ by
more than 8/255 is averaged, in a ground region and a wall region (HUD text
and the hero sprite removed).

| Mode | Ground | Wall |
| --- | --- | --- |
| World pixels | 35.7% | 24.5% |
| World pixels without the depth tie-break | 35.6% | 24.4% |
| Screen pixels | 39.2% | 26.0% |

How to read it: during an orbit most of the change is real (parallax and
perspective change the picture, and no shift can undo that), so the metric
has a high floor. World pixels lower it modestly; the depth tie-break
(section 11) made no difference here. The metric does not separate the
specific artifact world pixels remove (pixels sliding over a surface) from
legitimate change; a better test would track fixed surface points across
frames. Older figures (19% / 10% vs 34% / 14%) came from an undocumented
one-off measurement and are withdrawn.

## 8. Path tracer (`path_tracer.*`, `pathtrace.slang`)

A software path tracer in a fullscreen fragment shader, so it runs on every
backend without ray-tracing hardware or compute shaders.

**Acceleration structure.** `PathTracer::rebuild` flattens all meshes and
sprite cards to world-space triangles and builds one BVH with a **binned
surface-area heuristic** (16 bins per axis, leaves of up to 4 triangles,
partitioning by the same bin formula as the cost evaluation so no split is
empty). It is rebuilt only when a hash of transforms and materials changes.
The shader traverses **front to back**: both children's boxes are tested,
the nearer is visited first, and stacked entries carry their entry distance
so subtrees behind the closest hit are skipped. Textures are packed into one
1024x1024 atlas. SAH plus ordered traversal took the forest walk (~115k
triangles) from 269 to 109 ms per frame.

**Integrator**, per pixel, one path per frame by default (`ptSamples`,
`ptBounces`):

- The primary hit is snapped to the centre of its texture texel and the
  random seed is derived from that texel, so soft shadows and bounce light land
  on the texture's pixel grid. No sub-pixel jitter: the pixel grid stays hard.
- At each vertex: **next-event estimation** to the sun (shadow ray, Lambert
  1/pi), to each point light (shadowed, same falloff and bands as raster;
  with more than 4 point lights, one is picked in proportion to what it could
  add there and divided by that probability: shadow rays are the cost), and
  to **one emissive triangle** picked in proportion to its power (area x
  brightness, by binary search of a cumulative table) and sampled uniformly by
  area (pdf = p_pick / area). Uniform picking gave a few big flood panels next
  to ~1,000 tiny scoreboard pixels almost no samples. Emissive textures glow in
  their own colors (emission x texture color, also when sampled as a light).
- **Reflections** (`MeshRenderer.reflectivity`, `roughness`): at the first hit,
  one ray around the mirror direction, Schlick Fresnel with reflectivity as
  F0, jittered by roughness so frames average into a glossy blur. The
  direction is taken from the camera to the texel centre, so reflections are
  made of whole texels like everything else. Raster mode has no reflections.
- Continuation by **cosine-weighted** hemisphere sampling. A ray that escapes
  takes the same sky the screen shows (`skyRadiance`) without the sun disc,
  since the sun is already sampled directly. Emission is counted only when
  seen directly (it goes to a separate output), never on bounce hits, to avoid
  double counting with the emissive-triangle samples.
- Toon bands apply to direct light at the first hit, so path-traced and
  raster images share the banded look.
- A firefly clamp (radiance <= 16 per sample) trades a little bias for no
  single-pixel sparkles. No Russian roulette (bounce counts are small).

**Accumulation with temporal reprojection.** Each target slot (the screen,
or one probe face) keeps a per-pixel running average in two images used in
turn (RGBA16F: rgb light, a = frames averaged, up to 511). Each frame the
shader finds where its primary hit point was on screen *last* frame (last
frame's view-projection), and continues that pixel's average if the same
surface was there: last frame's view depth (kept by `pt_copy.slang`) within
2% and normals within ~37 degrees (the disocclusion test); otherwise it
starts over. So a moving camera keeps its history instead of restarting
(before 2026-11-27 any camera change restarted it, so a panning camera
showed one sample per frame). A scene change cuts every pixel to 6 frames
once; a slot that was not traced the previous frame restarts, because other
modes draw into the same targets. With a crowd, still pixels keep at most
32 frames and pixels a moving person may cover keep 4 (section 8b).
The denoiser's tolerance is per pixel: 0.4 x full light / sqrt(frames this
pixel has averaged), so newly uncovered pixels are smoothed hard.

**Surface color** is the material color times the vertex colors, interpolated
at the hit like the raster path. Each corner's sRGB vertex color is stored in
the otherwise unused `w` of `v0`/`e1`/`e2` as `r + 256 g + 65536 b` (exact in a
float, never a NaN pattern). Fixed 2026-11-24: the tracer used only the first
vertex's color, so on the forest terrain (6.4-unit triangles whose variation
is in vertex colors) every triangle became one flat patch: hard-edged
"squares" that raster did not show. Ruled out on the way, each by a debug
render: shadow rays (removing them changed nothing), the shadow terminator,
world-pixel probe seams (screen pixels showed the same) and the normals
(the shading normal is smooth).

**Denoiser.** The tracer writes light, surface color and emission to separate
targets (demodulation). Three à-trous passes (`pt_denoise.slang`, tap
spacing 1, 2, 4) smooth only the light, with edge-stopping on normal, depth
and brightness; the brightness tolerance is 0.4 x full light / sqrt(frames),
so it smooths hard at first and less as the average converges.
`pt_compose.slang` then multiplies the albedo back in (or picks the
palette-ramp color) and adds emission, so textures stay sharp.

Share of pixels differing from a converged image (village, still camera):

| Frames accumulated | World pixels, denoised (raw) | Screen pixels, denoised (raw) |
| --- | --- | --- |
| 1 | 5.9% (13.7%) | 5.4% (9.3%) |
| 4 | 0.6% (6.2%) | 1.1% (3.1%) |
| 16 | 0.4% (3.1%) | 0.8% (1.9%) |

## 8b. Crowds (`crowd.*`, `crowd.slangh`, `crowd.slang`, `pathtrace.slang`)

`projects/arena` (generated by `tools/make_arena.py`) seats 18,125
inflatable tube-man fans (7 models, 9 clips, ~580 triangles each) in an NHL
sized bowl, plus 10 skaters, all animated, path traced on an M1 Pro.
Scene data: a `Crowd` component points at a pack (`*.crowd.json`: models
split into rigid parts, one per bone; clips baked as one rigid transform per
bone per frame) and a members file (one row per person: seat, facing, model,
team, idle clip, speed, phase, wave delay, three colors). `CrowdActor`
entities add people driven by their Transform (the skaters). Scripts start
team events and the wave with `ace.crowd_event` / `ace.crowd_wave`.

**The problem.** A path tracer needs a BVH over what it hits. Rebuilding one
over every posed triangle each frame measured 0.47 ms and 0.11 MB per person
with the engine's binned-SAH builder (2,000 people: 941 ms, 221 MB), i.e.
about 8.5 s and 2 GB per frame for this crowd.

**The design** (after AMD's tetrahedral cages, Gruen et al. 2026, which trace
static rest-pose geometry inside animated cells):

- **Rigid parts in rest pose.** Each (model, bone) part has a small BVH in
  rest pose, built once: 3,746 triangles in total for 18,125 people. A ray
  reaching a person is moved into a part's rest space with the inverse of
  that bone's rigid transform; a rigid map keeps the ray straight and keeps
  its parameter t, so hits compare directly with the rest of the scene.
  Rigid parts cannot stretch, so the tubes are modelled as overlapping
  "inflated" segments that hide the joints (the cage papers keep continuity
  with tetrahedra instead; see limitations).
- **Animation evaluated where it is used.** `crowdAnim` / `crowdBonePose`
  (crowd.slangh) compute each bone's pose from the time and the person's
  own parameters: idle clip, speed, phase, plus at most one override (the
  team's event after a hashed reaction delay of up to 0.6 s, else the wave
  arriving at the person's angle around the bowl), crossfaded with nlerp.
  Clips are stepped at 12 fps (pixel-art animation holds frames), so nothing
  is interpolated between frames. The raster pass (`crowd.slang`, vertex
  pulling, one draw per model) runs the same function, so both modes agree.
  Nothing about the pose is uploaded per frame.
- **Pose-conservative bounds.** Per model, per pair of clips (idle a,
  override b): boxes over every frame of a and b and every crossfade between
  them, per person and per bone. A crossfade moves points along an arc, so
  frame pairs are blended at every 1/8 and the boxes grow by 5 cm;
  `testCrowd` samples 45 million posed vertices at random weights and checks
  all are inside (with 1/4 steps, 1,293 were not: a hand 1.9 m from the feet
  strayed ~14 cm). Held frames also store their exact per-bone box.
- **Top level built once, refit only during events.** Seated people form a
  one-person-per-leaf BVH built over their idle boxes and never rebuilt.
  While an event or the wave runs, the CPU refits it every frame (same tree,
  new boxes) from each person's current state: idle frame, override frame,
  or crossfade (pair box), sampled at now +- 50 ms because the GPU computes
  in float; ~1 ms of CPU and a 1.2 MB upload, only while something happens.
  Moving actors have their own small top level rebuilt each frame (a
  two-partition top level, like DXR's partitioned TLAS).
- **Per-bone culling.** Inside a person, each bone's box for the pose shown
  now is tested before its pose is evaluated.
- **Proxies for secondary rays.** Shadow and bounce rays stop at the bone
  boxes (character space, the held frame's exact box) instead of triangles;
  a ray starting inside a box passes through (no self-shadowing). Camera rays
  and reflections stay exact. Like the simplified scenes games trace for
  indirect light (Lumen's surface cache, foliage LODs).
- **Pixel-art rules hold.** Texel-space lighting snaps the hit to the centre
  of its texel *in rest space* (grid projected along the rest triangle's
  dominant axis, `texelsPerUnit`), so lighting pixels stay attached to a
  body while it bends; the random seed comes from that texel, the person and
  the bone. Raster does the same with the face normal stored per vertex.
- **Temporal stability.** Section 8's reprojection keeps history while the
  camera moves. A ray that crosses a moving person's box in front of its hit
  marks the pixel (a conservative reactive mask): it keeps 4 frames (a pose
  holds 5 frames at 60 fps); a pixel that only reflects one keeps 8; still
  pixels keep 32, because bounce light and shadows from people change.

Memory for the whole crowd: ~1.2 MB of nodes, 0.36 MB of part triangles,
1.2 MB of people, ~0.9 MB of clips and boxes, 0.6 MB of raster vertices.

**Measured** (M1 Pro, Metal, 427x240 internal, 2 bounces, 1 sample, the
high-corner shot frozen; in-engine median with `ace_player --timing`):

| Configuration | ms / frame |
| --- | --- |
| Everything below on | 68.7 |
| Exact triangles for shadow and bounce rays (no proxies) | 128.7 |
| All 8 floods shadow-tested (no light picking) | 105.5 |
| People boxes over every clip (no pair / frame bounds) | 191.0 |
| Camera rays skip the per-bone test | 69.9 (the part BVH root rejects almost as well) |
| Whole-clip bone boxes instead of the held frame's | 60.5 (faster: fatter proxies stop shadow rays sooner, but over-shadow; kept exact) |
| One bounce instead of two | 45.7 |
| During a goal celebration / the wave (refit every frame) | 70.4 / ~70 |
| Camera rays only (no lighting) | 16.3 |
| Same arena without the crowd | 24.9 |
| Raster | 18.3 |
| World pixels, path traced (12 probe faces) | 520 |

Metal and Vulkan (MoltenVK, validation on: 0 messages) give identical images
in both modes, during a celebration (frame 420).

Problems met on the way, each found by measuring:

| Symptom | Cause | Fix |
| --- | --- | --- |
| 592 ms per frame | people boxes covered every clip; every ray visited many people; 8 shadow rays per vertex | pair and frame bounds, per-bone culling, proxies, light picking |
| Load took 14 s | the Mac `build` folder compiled C++ at -O0 (see HANDOFF pitfalls) | `FORCE` the default build type |
| Bounds missed 1,293 / 45M posed vertices | crossfade arcs between 1/4 samples | 1/8 samples + 5 cm, checked by `testCrowd` |
| Panning camera: one sample per frame | any camera change restarted accumulation | temporal reprojection |
| Vulkan rejected `crowd.fs` (SPIR-V 1.4 for a 1.3 target) | `ddx`/`ddy` of a varying made slangc emit 1.4 | face normal stored per vertex |
| Timings that disagreed 3x; a "3x slowdown" that was then "fixed" | wall-clock runs included Metal pipeline compilation and PNG encoding; measured in the loop, the suspected change made no difference (68.7 ms both) | `--timing` measures inside the loop; every number here comes from it |
| Forest image changed after the refactor | depth output written after snapping the hit to its texel | depth of the real hit, as before |

## 9. Atmosphere (`common.slangh`: `Atmosphere`, `skyRadiance`, `applyFog`)

One description of the sky, shared by every pass that needs it:

- Gradient from `skyBottom` to `skyTop`, steepest near the horizon, darker
  below it.
- The sun is the first `Light` without `point` (`findSun`): a disc
  (`sunSize`) and a two-lobe glow (`sunGlow`), tinted by the light's color.
- Pixel clouds: five-octave value noise projected on a ceiling, with a
  coverage threshold, a darker and cooler thick part, and a warm sun side.
- Stars where the sky is dark: the sky is divided into cells (400x400 per
  face of a cube around the camera) and 0.25% of the cells hold a star.
- Distance haze: exponential in distance (`fogDensity`), brighter and warmer
  towards the sun. World pixels apply it per texel in `probe_post`; screen
  pixels apply it in `present`.

All randomness (stars, cloud noise) uses an integer PCG hash, which gives the
same bits on every GPU (section 11).

## 10. Present pass and color pipeline (`present.slang`)

In order, per screen pixel:

1. Sky where nothing was drawn, evaluated per *game* pixel so it stays pixelated.
2. Haze (screen-pixel mode).
3. Outlines where depth jumps (always one texel wide, belonging to the nearer
   surface) and highlights on convex creases from the normals.
4. Tone map: exposure, then a gentle shoulder `c / (1 + 0.25 max(c))` that
   keeps flat colors flat.
5. Palette snap (ordered dithering): a 4x4 Bayer offset scaled by `dither` is
   added to the color, then the nearest of up to 64 palette colors is chosen
   (perceptually weighted distance).
6. Integer upscale (`integerScale`): a whole number of screen pixels per game
   pixel; the camera is widened slightly instead of stretching the image.

## 11. Cross-backend consistency

Images are compared pixel by pixel at a fixed timestep
(`--fixed-dt 0.0166667`). Each backend is deterministic run to run.

| Comparison | `main` | `village` | path traced | `forest` |
| --- | --- | --- | --- | --- |
| Metal vs Vulkan (macOS, MoltenVK) | identical | identical | identical | 0.005% (one band of distant ground) |
| D3D12 vs Vulkan (Windows, RTX 5070 Ti) | 0.003% | 0.001% | max 1/255 | 0.004% |

Metal and MoltenVK share Apple's shader compiler, so they agree exactly.
D3D12 and Vulkan on the same NVIDIA GPU use different compilers (DXC and
NVIDIA's SPIR-V compiler), which round the last bits differently; any hard
decision near a threshold can flip. Three such decisions were found and
fixed:

| Fix | Symptom | Cause | Change | Result |
| --- | --- | --- | --- | --- |
| Splat depth tie-break | `village` differed on 0.4% (0% with world pixels off) | oversized, nearly coplanar splat squares: the depth test in the overlap was decided by rounding noise (z-fighting) | fixed per-texel depth offset in a 2x2 pattern along the camera ray | 0.4% -> 0.001% |
| Stars | some stars in different cells | cells from `floor(atan2/asin)` (precision unspecified on every API) and a float hash (compilers fuse its multiply-adds differently) | cube-projected cells (one division) and an integer PCG hash | stars now ~0.001% of the forest |
| Clouds | forest still 0.28%, all at cloud edges (found by turning stars, clouds and dither off one at a time) | cloud noise used the same float hash; the edge threshold and palette snap amplified the last bits | integer hash of the lattice point, 24 bits kept (exact in a float) | forest 0.28% -> 0.004% |

A baked star texture was considered for the star fix and rejected: it would
still be looked up at a coordinate computed from the view direction, so it
removes neither rounding source. Still accepted: a star can flip in a rare
pixel right on a cell edge (division is not exact either), and the forest's
0.005% on macOS is a band of distant ground sitting on a palette threshold.
Every fix's full write-up (symptom, cause, change, evidence) is in its commit
message.

Rules that came out of this: give overlapping coplanar geometry a fixed depth
tie-break; never feed transcendentals (`atan2`, `asin`, `pow`, `sin`) into
`floor`/`step` where repeatability matters; use integer hashes for randomness
that feeds a threshold.

## 12. Performance

Apple M1 Pro, 2560x1440 output, timings from 300-frame runs (the machine was
often under load; treat as relative numbers):

| Configuration | Frame time |
| --- | --- |
| Screen pixels, raster | ~5 ms |
| World pixels, raster (village) | 9.1 ms Metal, 8.4 ms Vulkan (load average ~4.5; earlier, at load ~10 and before Vulkan framebuffer caching: 10.0 and 13.3 ms) |
| World pixels, path traced, still camera (village) | 16.5 ms (settles, then costs little) |
| Forest, walking, raster world pixels | 15 ms |
| Forest, walking, path traced, 320 texels, 1 bounce (shipped settings) | ~60 ms |
| Forest, walking, path traced, 384 texels, 2 bounces | 109 ms (269 ms before SAH + ordered traversal) |

Walking is the expensive case: the eye probe and every new grid probe are
traced from zero samples. A still camera settles and gets cheap.

The arena (section 8b): 68.7 ms path traced with 18,125 people (2 bounces),
45.7 ms with one bounce, 18 ms raster. Measure with `ace_player --timing
<first frame>`: before 2026-11-27 timings were wall-clock runs, and on the
Mac the C++ was unoptimized (HANDOFF pitfalls), so older CPU-heavy numbers
above are pessimistic.

## 13. Known limitations

- Path-traced walking cost (above). Next steps, cheapest first: reproject the
  previous probe's converged light into a new one; trace the eye probe at half
  resolution; simpler far trees; cap secondary-ray distance. Then a two-level
  BVH (per-mesh BLAS built once, TLAS per frame) so moving objects do not
  rebuild everything, and a temporal denoiser with motion reprojection.
  Hardware ray queries (Metal, Vulkan, DXR) can come later behind the same
  interface.
- Crowds: rigid parts cannot stretch (no skinning); a tetrahedral cage per
  person (Gruen et al. 2026) would allow smooth deformation with the same
  static rest-pose BVHs. Only point/sun lights and one emissive sample per
  vertex (no ReSTIR); a moving person's pixels keep 4 frames, so they are
  noisier than the still arena; secondary rays see people as boxes; people
  are drawn without raster shadows; the raster pass draws all 18,125 people
  every view (no culling or LOD), so world pixels (12 views) is slow.
- Reprojection uses the nearest pixel (no filtering) and only the camera's
  motion: other moving objects ghost for a few frames (the crowd's reactive
  mask covers people).
- Ramp shading uses the *amount* of light, so colored light (a warm lantern,
  the sky's tint in bounce light) loses its hue in ramp mode.
- One shadow map over the whole scene (fine for small levels; cascades are
  needed for large ones); no raster shadows for point lights.
- No compute shaders in the RHI; one frame in flight; all buffers
  CPU-writable (no GPU-local static buffers, no sub-allocation).
- World pixels: outlines stop at cube-face seams; the eye probe's
  disocclusion fill can shimmer at silhouettes; 384 texels per face is heavy
  for phones.

## 14. References

- Dylan Ebert, *Texel Splatting*, arXiv 2603.14587; dylanebert.com/texel-splatting
  (the world-pixels technique).
- tesseractcat, "Replicating the Shadowglass 3D pixel-art technique",
  tesseractc.at/shadowglass (a related cube-map reprojection approach).
- 80.lv interview with the developer of Project Shadowglass (palettes / LUTs,
  point filtering, stepped animation).
- Dammertz et al., "Edge-Avoiding À-Trous Wavelet Transform for fast Global
  Illumination Filtering", HPG 2010 (the denoiser's filter).
- Wald, "On fast Construction of SAH-based Bounding Volume Hierarchies",
  IEEE RT 2007 (binned SAH).
- Gruen, Benthin, Kern, McAllister, "Ray Tracing Massive Amounts of Animated
  Geometry", Proc. ACM CGIT 9(4), HPG 2026 (tetrahedral cages: animate a coarse
  cage, trace static rest-pose mini-BLASes; the idea the crowd follows).
- van Antwerpen et al., "Real-Time Path Tracing of Massive Dynamic Foliage",
  2026, DOI 10.1145/3820021 (partitioned TLAS, clustered LOD, opacity micromaps).
- Microsoft DirectX blog, clusters and partitioned TLAS preview, 2026-10-28.
- Lin, Kettunen, Wyman, "ReSTIR PT Enhanced", Proc. ACM CGIT 9(1), 2026 (many
  lights and resampling; not implemented here).
- Dudash, "Animated Crowd Rendering", GPU Gems 3 ch. 2, 2007 (baked animation
  in a buffer + instancing: the raster crowd).
- Schied et al., "Spatiotemporal Variance-Guided Filtering", HPG 2017
  (reprojection with depth/normal disocclusion tests).
- O'Neill, PCG random number generation (pcg-random.org); the 32-bit
  RXS-M-XS output step used by `pcgHash`.
