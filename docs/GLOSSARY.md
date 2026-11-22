# Glossary

The words you need to read [RENDERING.md](RENDERING.md), [HANDOFF.md](HANDOFF.md)
and the code. Grouped by topic, in the order that is easiest to learn them:
each group builds on the ones before. Every entry says what the term means
and, in *italics*, where it appears in AceEngine.

Contents:

1. Pixels, color and pixel art
2. 3D basics
3. GPUs and graphics APIs
4. Binding data to shaders
5. Shaders and shader compilers
6. Lighting and shading
7. Path tracing
8. World pixels (texel splatting)
9. Exactness across GPUs
10. Engine, editor and tooling

## 1. Pixels, color and pixel art

| Term | Meaning |
| --- | --- |
| Pixel | One dot of an image on screen. |
| Texel | One pixel of a texture (an image wrapped onto a 3D surface). *With `worldTexels`, every texel has the same size in the world: 1 / `texelsPerUnit` units.* |
| Game pixel | One pixel of the low-resolution image the game renders, before it is scaled up to the screen. *`pixelHeight` rows tall.* |
| Integer scaling | Scaling an image up by a whole number (2x, 3x...) so every game pixel becomes an exact square of screen pixels. *`integerScale`.* |
| Point (nearest) filtering | Reading a texture without blending neighbouring texels, so pixel art stays sharp. *All pixel-art textures use it.* |
| Cutout (alpha test) | Treating a texel as either fully visible or fully invisible (no half-transparency). Pixel art needs hard edges. *Sprites, ferns.* |
| sRGB | The standard way colors are stored in image files and sent to monitors: brightness is encoded non-linearly so dark tones get more precision. *Textures, scene colors and the screen are sRGB.* |
| Linear light | Color values proportional to real light energy. Lighting math (adding and multiplying light) is only correct in linear space. *All shading in AceEngine is linear; `toLinear` and `linearToSrgb` convert.* |
| Gamma | The curve that converts between linear and sRGB (roughly a power of 2.2). |
| HDR (high dynamic range) | Color values allowed to go above 1.0 (brighter than white) during rendering. *Scene targets are RGBA16F, 16-bit floats.* |
| Tone mapping | Squeezing HDR values back into the 0..1 range a screen can show. *`present.slang`: exposure, then a gentle shoulder.* |
| Exposure | A brightness multiplier applied before tone mapping. *Render Setting `exposure`.* |
| Palette | A fixed, small set of allowed colors. *A palette image, up to 64 colors.* |
| Palette snap (quantization) | Replacing every color with the nearest palette color. *Last step of `present.slang` and `probe_post.slang`.* |
| Ramp | One row of the palette image: hand-picked colors from dark to light for one material (grass, stone...). |
| Ramp shading | Lighting by moving along a ramp (lighter or darker hand-picked color) instead of multiplying the color by a brightness. This is how pixel artists shade by hand. *`rampShading`, `rampShade()` in `common.slangh`.* |
| Dithering | Mixing two colors in a pattern of dots so the eye sees an in-between color. |
| Bayer matrix / ordered dithering | A fixed 4x4 pattern of thresholds used for dithering, giving the regular checkerboard look of classic pixel art. *`bayer4()` in `common.slangh`.* |
| Toon bands | Light reduced to a few flat steps instead of a smooth gradient (cartoon look). *`toonBands`.* |
| Outline | A dark line drawn where depth jumps (an object's silhouette). *`present.slang` and `probe_post.slang`.* |

## 2. 3D basics

| Term | Meaning |
| --- | --- |
| Vertex | A corner point of a 3D shape, with position, normal, texture coordinates and color. |
| Triangle | Three vertices; every 3D surface is drawn as triangles. |
| Mesh | A set of triangles forming an object. *glTF files or `builtin:cube` and friends.* |
| Normal | The direction a surface faces (perpendicular to it). Lighting depends on it. |
| UV (texture coordinates) | Per-vertex coordinates saying which part of a texture lands where on the surface. |
| glTF / GLB | The standard file format for 3D models (`.gltf` is text + data, `.glb` is one binary file). *Loaded with cgltf.* |
| Transform | Position, rotation and scale of an object. |
| Matrix | A 4x4 grid of numbers that encodes a transform; multiplying by it moves points between spaces. |
| Quaternion | A 4-number way to store a rotation without the problems of angles (no gimbal lock). *`Transform.rotation`.* |
| Euler angles | A rotation written as three angles (pitch, yaw, roll). Easy to read, awkward to compute with. *Inspector and `ace.rotation`.* |
| Yaw / pitch / roll | Turning left-right / looking up-down / tilting sideways. *`ace.set_look(e, yaw, pitch)`.* |
| World space | Coordinates shared by the whole scene. |
| View (camera) space | Coordinates relative to the camera. |
| Clip space / NDC | The coordinates the GPU uses after projection; NDC (normalized device coordinates) run from -1 to 1 across the screen. |
| Projection | Turning 3D positions into 2D screen positions. **Perspective**: far things look smaller (eyes, cameras). **Orthographic**: no shrinking with distance (technical drawings, isometric games). |
| Depth buffer (z-buffer) | A per-pixel record of the distance to the nearest surface drawn so far, so nearer things hide farther ones. |
| Z-fighting | Two surfaces at almost the same depth flickering because rounding decides which one wins. *Fixed for world-pixel splats with a depth tie-break.* |
| Winding / front face | Whether a triangle's corners go clockwise or counter-clockwise on screen; it tells front from back. *Counter-clockwise = front, on every backend.* |
| Back-face culling | Skipping triangles that face away from the camera. |
| Billboard | A flat card that always turns to face the camera. *Sprites with `billboard`.* |
| Sprite / sprite sheet | A 2D picture placed in the world; a sheet holds many frames (animation) and rows (view directions) in one image. *`SpriteRenderer`, `directions`.* |
| Parallax | Near things moving across the view faster than far things when the camera moves. Real change, not an artifact. |

## 3. GPUs and graphics APIs

| Term | Meaning |
| --- | --- |
| GPU | The graphics processor: thousands of small cores running the same program on many pixels or vertices at once. |
| Graphics API | The programming interface to the GPU. **Metal** (Apple), **Vulkan** (cross-platform, Khronos), **Direct3D 12 / D3D12 / DX12** (Windows, Microsoft). *AceEngine has a backend for each.* |
| MoltenVK | A layer that runs Vulkan programs on top of Metal on Apple devices. *How Vulkan runs on the Mac.* |
| RHI (render hardware interface) | The engine's own small GPU interface, so the rest of the engine never talks to Metal, Vulkan or D3D12 directly. *`engine/rhi/rhi.h`.* |
| Backend | One implementation of the RHI for one API. *`metal_device.mm`, `vulkan_device.cpp`, `d3d12_device.cpp`.* |
| Device | The engine's object representing the GPU; it creates resources. *`rhi::Device`.* |
| Handle | A small integer that refers to a GPU resource, instead of a pointer. *`BufferHandle`, `TextureHandle`; id 0 means "none".* |
| Buffer | A block of GPU memory holding data (vertices, indices, constants, the BVH). |
| Texture | A GPU image that shaders can read. |
| Render target | A texture the GPU draws into. |
| Command list / command buffer | A recorded list of GPU work, submitted at once. *`rhi::CommandList`.* |
| Render pass | A group of draws into the same render targets, with rules for clearing them first. *`beginRenderPass` / `endRenderPass`.* |
| Draw call | One command to draw a set of triangles. |
| Pipeline (PSO, pipeline state object) | Everything fixed about how a draw happens, baked together: shaders, vertex layout, blending, depth test, culling, target formats. *`createPipeline`.* |
| Swapchain | The small set of images the window shows, rotated each frame. |
| Backbuffer | The swapchain image being drawn this frame. |
| Present | Handing the finished backbuffer to the screen. |
| Vsync | Waiting for the monitor's refresh before presenting, to avoid tearing. |
| Frame in flight | A frame the CPU has finished recording but the GPU is still drawing. *AceEngine allows one: `beginFrame` waits for the previous frame.* |
| Fence | A GPU-to-CPU signal meaning "this work is done". *D3D12 and Vulkan backends.* |
| Resource state / barrier | (Vulkan, D3D12) A texture must be told what it is about to be used for (render target, shader input, copy); a barrier switches it. Metal does this automatically. *D3D12: every texture rests in one state between passes.* |
| Heap (upload, default, readback) | (D3D12) A kind of GPU memory: **upload** is CPU-writable, **default** is fastest for the GPU, **readback** lets the CPU read results. |
| Staging buffer | A temporary CPU-visible buffer used to copy data to or from GPU-only memory. *Texture uploads and screenshots.* |
| Offscreen frame | A frame rendered without a window to show it. *`beginFrame(false)`, used by screenshots and tests.* |
| Validation layer / debug layer | An optional checker that reports wrong API use. *`--validation`: Khronos validation for Vulkan, the D3D12 debug layer.* |
| Compute shader | A GPU program not tied to drawing pixels. *The RHI has none yet; the path tracer runs as a fullscreen pixel shader instead.* |

## 4. Binding data to shaders

| Term | Meaning |
| --- | --- |
| Uniform / constant buffer | A small buffer of values that are the same for every pixel of a draw (matrices, light color). D3D calls it a constant buffer (CBV = its view). |
| Storage / structured buffer | A large buffer a shader can index like an array. *The path tracer's BVH nodes and triangles.* |
| Dynamic offset | Pointing a binding at a different position inside one big buffer for each draw, instead of making many small buffers. *Every uniform binding in AceEngine.* |
| Ring buffer | A buffer filled from the start each frame and reused. *The 16 MB per-frame uniform ring, `Renderer::pushUniform`.* |
| Binding | The slot number a shader uses to find a resource. |
| Bind group / descriptor set | A bundle of resources (buffers, textures, samplers) bound together. Vulkan calls it a descriptor set; WebGPU and AceEngine call it a bind group. |
| Bind group layout | The shape of a bind group: which slot holds which kind of resource. |
| Descriptor | A small record that describes one resource to the GPU. |
| Descriptor heap / table | (D3D12) An array of descriptors; a table is a range of it bound at once. *One block per bind group.* |
| Root signature | (D3D12) The list of everything a pipeline's shaders can bind. *One per pipeline, built from its bind group layouts.* |
| Root CBV / root SRV | (D3D12) A buffer bound directly by GPU address, the cheapest binding. *Uniform buffers and storage buffers.* |
| Register / space | (D3D) How HLSL names a binding: `register(t1, space2)` = texture slot 1 in group 2. *Every shader resource declares one beside its `vk::binding`.* |
| Sampler | Settings for reading a texture (filtering, wrapping). |
| Vertex layout / semantic | How the bytes of a vertex map to shader inputs. A semantic is the name tying them together on D3D. *Vertex inputs are `ATTRIB0`, `ATTRIB1`...* |

## 5. Shaders and shader compilers

| Term | Meaning |
| --- | --- |
| Shader | A program that runs on the GPU. |
| Vertex shader | Runs once per vertex; places it on screen. *`vsMain`.* |
| Fragment (pixel) shader | Runs once per covered pixel; computes its color. *`fsMain`.* |
| Fullscreen pass | Drawing one triangle that covers the screen, so a fragment shader runs once per pixel (image processing, path tracing). |
| Slang | The shader language AceEngine is written in; one source compiles to every API. |
| HLSL | Microsoft's shader language; Slang's syntax is close to it. |
| SPIR-V | Vulkan's compiled shader format. *`.spv` files.* |
| DXIL | D3D12's compiled shader format. *`.dxil` files, built on Windows.* |
| MSL | Metal Shading Language. |
| SPIRV-Cross | A tool that converts SPIR-V into MSL. *The Metal backend runs it when a pipeline is created.* |
| DXC | Microsoft's shader compiler, used to produce DXIL. |
| `SV_VertexID` | The vertex's index, available in the vertex shader; lets a shader build geometry without a vertex buffer. *World-pixel splats.* |
| Transcendental functions | `sin`, `atan2`, `asin`, `pow`, `exp`...: computed approximately, and the approximation differs between GPUs and compilers. |
| FMA (fused multiply-add) | Computing `a * b + c` in one step with one rounding. Compilers may or may not use it, which changes the last bits of results. |

## 6. Lighting and shading

| Term | Meaning |
| --- | --- |
| Shading | Computing a surface's color from its material and the light reaching it. |
| Albedo | A surface's own color (how much light of each color it reflects). |
| Diffuse / Lambert | Matte reflection: brightness proportional to the cosine of the angle between the light and the normal. The 1/pi factor keeps energy correct. |
| Directional light (sun) | Light coming from one direction everywhere, as from a far-away sun. *The first `Light` without `point`.* |
| Point light | Light radiating from a point that fades with distance. *Lanterns, torches, the campfire; `Light.point`.* |
| Emissive | A surface that gives off light itself. *`MeshRenderer.emissive`.* |
| Ambient light | A flat amount of light added everywhere to stand in for light bouncing around. *Render Setting `ambient` (raster).* |
| Shadow map | A depth image rendered from the light's point of view; a point is in shadow if something nearer to the light covers it. *One 2048x2048 map for the sun.* |
| Texel-space lighting | Computing light once per texture texel instead of per screen pixel, so light and shadow edges follow the pixel-art grid. *`texelLighting`.* |
| World-space texel mapping | Placing texture coordinates from world position, so texels are the same size on every object. *`worldTexels`.* |
| Fog / haze | Far things fading toward a color with distance. *`fogDensity`, `applyFog()`.* |
| Sky / atmosphere | The color seen where no geometry is. *`skyRadiance()`: gradient, sun disc and glow, clouds, stars.* |
| Value noise / fBm | Smooth random patterns made by blending random values on a grid; fBm (fractal Brownian motion) adds several layers at growing detail. *Clouds.* |

## 7. Path tracing

| Term | Meaning |
| --- | --- |
| Rasterization | Drawing by projecting triangles onto the screen and filling their pixels. Fast; how almost all games render. *The raster path.* |
| Ray tracing | Drawing by shooting rays from the camera and finding what they hit. |
| Path tracing | Ray tracing that follows each ray as it bounces from surface to surface, collecting light along the way. Gives soft shadows and bounce light naturally. *`pathTraced`.* |
| Bounce | One reflection of a path off a surface. *`ptBounces`.* |
| Global illumination | Light that arrives after bouncing off other surfaces (a red wall tinting the floor). |
| Monte Carlo | Estimating an answer by averaging many random samples. Path tracing is a Monte Carlo method. |
| Sample / spp | One random path; spp = samples per pixel. *`ptSamples`.* |
| Noise | The grain of a path-traced image with few samples; it shrinks as samples are averaged. |
| Accumulation / convergence | Averaging samples over frames; converging means approaching the noise-free result. *A running average per slot.* |
| Slot | One image the path tracer accumulates separately (the screen, or one probe face). |
| PDF (probability density) | How likely a sampling method is to pick a given direction or point; results are divided by it to stay correct. |
| Cosine-weighted sampling | Picking bounce directions more often where Lambert reflection is strongest, which reduces noise. |
| NEE (next-event estimation) | At every bounce, also sending a ray straight at a light to check if it is visible, instead of waiting to hit the light by chance. *Sun, point lights, one emissive triangle.* |
| Shadow ray | A ray that only asks "is anything in the way?" (any hit, not the closest). |
| Firefly | A single very bright pixel from a rare lucky path. *Clamped at 16 per sample.* |
| Bias | A systematic error traded for less noise (the firefly clamp is slightly biased). |
| Russian roulette | Randomly ending paths early to save work without bias. *Not used: bounce counts are small.* |
| BVH (bounding volume hierarchy) | A tree of boxes around groups of triangles; a ray skips every group whose box it misses, so it tests few triangles. |
| AABB | Axis-aligned bounding box: a box whose sides line up with the X, Y, Z axes. |
| SAH (surface area heuristic) | A rule for building good BVHs: split where (box surface area x triangles inside) is smallest, because a ray is more likely to enter a bigger box. *Binned: 16 candidate splits per axis.* |
| Traversal (front to back) | Walking the BVH for a ray; visiting the nearer child first lets the search stop early. |
| BLAS / TLAS | Two-level BVH: one bottom-level tree per mesh, one top-level tree over the objects. Moving an object only rebuilds the small top tree. *On the roadmap.* |
| Hardware ray tracing | GPUs with dedicated ray-tracing units (Metal, Vulkan, DXR). *Not used yet; AceEngine's tracer is software.* |
| Denoiser | A filter that removes path-tracing noise. *`pt_denoise.slang`.* |
| À-trous filter | A blur applied several times with growing gaps between taps (1, 2, 4), cheap for wide blurs. |
| Edge-stopping | Not blurring across edges, detected from differences in normal, depth or brightness. |
| Demodulation | Dividing out the surface color before denoising the light alone, then multiplying it back, so textures stay sharp. |
| Temporal reprojection | Reusing last frame's result at the right place after the camera moved. *On the roadmap.* |
| Atlas | Many textures packed into one big texture. *The path tracer's 1024x1024 texture atlas; probe cube faces side by side.* |

## 8. World pixels (texel splatting)

| Term | Meaning |
| --- | --- |
| World pixels | AceEngine's mode where every pixel is a small square placed in the 3D world, so it stays attached to its surface when the camera moves. *`worldPixels`.* |
| Texel splatting | The published technique behind world pixels (Dylan Ebert). |
| Cube map | Six square images (+X, -X, +Y, -Y, +Z, -Z) that together see in every direction from one point. |
| Probe | A point the scene is captured from into a cube map. |
| Grid probe | The probe snapped to a world grid, so it stays still while the camera moves within a grid cell. |
| Eye probe | The probe at the camera, filling in what the grid probe cannot see. |
| Probe cell | The grid spacing for the grid probe. *`probeCell`.* |
| Crossfade / dissolve | Blending from the old grid probe to the new one, here with a Bayer pattern over 0.35 s. |
| Splat | The square drawn in the world for one captured texel. |
| Disocclusion | Something becoming visible that was hidden from where a capture was taken. |
| Visible-face culling | Capturing and drawing only the cube faces the camera can see. *`visibleFaces`.* |
| Shimmer / crawl / swimming | Pixel art flickering or sliding over surfaces as the camera moves, because screen pixels do not stay attached to the world. *Measured with `tools/measure_shimmer.py`.* |

## 9. Exactness across GPUs

| Term | Meaning |
| --- | --- |
| Determinism | Getting exactly the same result every run. *Each backend is deterministic run to run.* |
| Floating point rounding | Computers store decimals with limited precision, so most results are slightly rounded; different compilers can round in different places. |
| Last bits | The least significant digits of a floating-point number, where rounding differences show up. |
| Threshold / hard decision | A `floor`, `step`, comparison or palette choice: tiny input differences near the threshold flip the output completely. |
| Float hash vs integer hash | A hash makes random-looking numbers from an input. Float hashes inherit rounding differences; integer hashes (like **PCG**) give identical bits on every GPU. *`pcgHash` in `common.slangh`.* |
| Depth tie-break | A small fixed depth offset that decides which of two overlapping surfaces wins, instead of leaving it to rounding. |
| Pixel diff | Comparing two images pixel by pixel and counting the differences. *How backends are compared, with `--fixed-dt`.* |
| Fixed timestep | Advancing time by exactly the same amount each frame, so animated scenes can be compared. *`--fixed-dt`.* |

## 10. Engine, editor and tooling

| Term | Meaning |
| --- | --- |
| Engine | Reusable code that runs games: rendering, input, audio, scenes, scripting. |
| Entity / component | An entity is an id; components are plain data attached to it (Transform, MeshRenderer, Light...). |
| ECS | Entity-component-system: organizing a game as entities with components. *EnTT library.* |
| Scene | A saved set of entities plus render settings. *`.scene.json`.* |
| Prefab | A saved group of entities you can place many times. *`.prefab.json`.* |
| Reflection | Describing each component's fields once, so saving, loading and the inspector work automatically. *`componentTypes()`.* |
| Lua / hot reload | The gameplay scripting language; hot reload applies a saved script while the game runs. |
| Dear ImGui / docking | The library the editor's panels are made with; docking lets panels snap together. |
| Gizmo | The on-screen arrows and rings for moving, rotating and scaling objects. *ImGuizmo.* |
| Undo snapshot | Saving the whole scene after each finished edit so it can be restored. |
| Unit test | A small automatic check of one piece of logic. *`ace_tests`.* |
| Autotest | The editor's scripted test that injects real mouse and keyboard input. *`tools/autotest.sh`.* |
| CMake / Ninja | The build system generator and the fast build tool AceEngine uses. |
| MSVC / MinGW | Microsoft's C++ compiler; MinGW is a GCC-based compiler for Windows, used on the Mac only to check that the D3D12 file compiles. |
| `shortcut:` comment | A marked, deliberate simplification with its limit and when to improve it (ponytail rule). *`grep -rn "shortcut:" engine editor`.* |
