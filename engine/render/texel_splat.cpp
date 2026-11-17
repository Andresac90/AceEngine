#include "render/texel_splat.h"
#include <SDL3/SDL.h>

namespace ace {

using namespace rhi;

namespace {
constexpr Format kAtlasColor = Format::RGBA16F;
constexpr Format kAtlasNormalDepth = Format::RGBA32F; // splats are placed from this depth: keep full precision
constexpr f32 kNear = 0.05f, kFar = 400.0f;
constexpr f32 kFadeSeconds = 0.35f; // grid probe crossfade after crossing a cell

struct PostData {
  vec4 info; // x: face size, y: outline strength, z: highlight strength, w: outlines on
  vec4 tone; // x: exposure, y: palette colors, z: dither
  vec4 palette[64];
};
struct SplatData {
  mat4 viewProj, view;
  vec4 origin; // xyz: probe origin, w: face size
  vec4 forward[6], right[6], up[6];
  vec4 camera;
  vec4 params; // x: quad half-size scale, y: push away from the camera (fraction of the distance), z: dither threshold, w: 1 = keep texels below it, -1 = keep texels above it, 0 = keep all
};
} // namespace

TexelSplatter::Face TexelSplatter::face(int i) {
  static const vec3 forwards[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  static const vec3 ups[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, 1, 0}};
  // Same basis glm::lookAt builds, so capture and splatting agree exactly.
  vec3 f = forwards[i];
  vec3 r = glm::normalize(glm::cross(f, ups[i]));
  return {f, r, glm::cross(r, f)};
}

vec3 TexelSplatter::snapOrigin(vec3 p, f32 cell) {
  cell = std::max(cell, 0.01f);
  return glm::round(p / cell) * cell;
}

TexelSplatter::TexelSplatter(Renderer& renderer) : r_(renderer) {
  Device& dev = r_.device();
  BindGroupLayoutDesc l;
  l.bindings[0] = BindingType::UniformBuffer;
  l.bindings[1] = BindingType::Texture;
  l.bindings[2] = BindingType::Texture;
  postLayout_ = dev.createBindGroupLayout(l);
  splatLayout_ = dev.createBindGroupLayout(l);

  PipelineDesc p;
  p.name = "probe post";
  p.vertexShader = r_.loadShader("probe_post", ShaderStage::Vertex);
  p.fragmentShader = r_.loadShader("probe_post", ShaderStage::Fragment);
  p.bindGroupLayouts[0] = postLayout_;
  p.colorFormats[0] = kAtlasColor;
  p.cull = CullMode::None;
  postPipeline_ = dev.createPipeline(p);

  PipelineDesc s;
  s.name = "texel splat";
  s.vertexShader = r_.loadShader("texel_splat", ShaderStage::Vertex);
  s.fragmentShader = r_.loadShader("texel_splat", ShaderStage::Fragment);
  s.bindGroupLayouts[0] = splatLayout_;
  s.colorFormats[0] = s.colorFormats[1] = Format::RGBA16F; // the renderer's scene targets
  s.colorCount = 2;
  s.depthFormat = Format::D32F;
  s.cull = CullMode::None;
  splatPipeline_ = dev.createPipeline(s);
  ACE_ASSERT(postPipeline_ && splatPipeline_, "failed to create the texel splatting pipelines");
}

TexelSplatter::~TexelSplatter() { destroyProbes(); }

void TexelSplatter::destroyProbes() {
  Device& dev = r_.device();
  for (Probe& p : probes_) {
    if (!p.color) continue;
    dev.destroy(p.postGroup);
    dev.destroy(p.splatGroup);
    dev.destroy(p.color);
    dev.destroy(p.normalDepth);
    dev.destroy(p.depth);
    dev.destroy(p.final);
    p = Probe{};
  }
}

void TexelSplatter::ensureProbes(u32 n) {
  if (n == faceSize_) return;
  Device& dev = r_.device();
  dev.waitIdle();
  destroyProbes();
  faceSize_ = n;
  u32 rt = TextureUsage_RenderTarget | TextureUsage_Sampled;
  for (Probe& p : probes_) {
    // The six faces side by side: face f occupies columns [f*n, (f+1)*n).
    p.color = dev.createTexture({6 * n, n, kAtlasColor, rt, "probe color"});
    p.normalDepth = dev.createTexture({6 * n, n, kAtlasNormalDepth, rt, "probe normal+depth"});
    p.depth = dev.createTexture({6 * n, n, Format::D32F, TextureUsage_RenderTarget, "probe depth"});
    p.final = dev.createTexture({6 * n, n, kAtlasColor, rt, "probe final color"});
    BindGroupDesc g;
    g.layout = postLayout_;
    g.entries[0].buffer = r_.uniformBuffer();
    g.entries[0].size = sizeof(PostData);
    g.entries[1].texture = p.color;
    g.entries[2].texture = p.normalDepth;
    p.postGroup = dev.createBindGroup(g);
    g.layout = splatLayout_;
    g.entries[0].size = sizeof(SplatData);
    g.entries[1].texture = p.normalDepth;
    g.entries[2].texture = p.final;
    p.splatGroup = dev.createBindGroup(g);
  }
}

// shortcut: both probes are re-captured every frame (12 small scene renders) and
// all six faces are captured even though at most three can be seen. Cache the
// grid probe while nothing moves, and skip hidden faces, if it shows up in a profile.
void TexelSplatter::render(CommandList& cmd, Scene& scene, Assets& assets, const mat4& view, const mat4& proj, const mat4& cameraWorld,
                           u32 width, u32 height) {
  const RenderSettings& s = scene.settings;
  u32 n = (u32)glm::clamp(s.probeResolution, 32, 1024);
  ensureProbes(n);
  vec3 cameraPos = vec3(cameraWorld[3]);
  // The grid probe stays put until the camera reaches another cell; then the old
  // and new probes dissolve into each other, texel by texel (Bayer dither).
  f64 now = r_.time();
  f32 dt = hasGrid_ ? (f32)glm::clamp(now - lastTime_, 0.0, 0.25) : 0.0f;
  lastTime_ = now;
  vec3 snapped = snapOrigin(cameraPos, s.probeCell);
  if (!hasGrid_) {
    gridOrigin_ = snapped;
    hasGrid_ = true;
  } else if (snapped != gridOrigin_) {
    previousOrigin_ = gridOrigin_;
    gridOrigin_ = snapped;
    fade_ = 0.0f;
  } else {
    fade_ = std::min(1.0f, fade_ + dt / kFadeSeconds);
  }
  bool fading = fade_ < 1.0f;
  int probeCount = fading ? 3 : 2;
  vec3 origins[3] = {gridOrigin_, cameraPos, previousOrigin_};
  mat4 faceProj = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, kNear, kFar);

  // 1. capture
  std::vector<RasterView> views;
  for (int pi = 0; pi < probeCount; pi++) {
    for (int f = 0; f < 6; f++) {
      Face fc = face(f);
      RasterView v;
      v.view = glm::lookAt(origins[pi], origins[pi] + fc.forward, fc.up);
      v.proj = faceProj;
      v.color = probes_[pi].color;
      v.normalDepth = probes_[pi].normalDepth;
      v.depth = probes_[pi].depth;
      v.viewport = vec4((f32)(f * n), 0.0f, (f32)n, (f32)n);
      v.clear = f == 0;
      v.capture = true;
      v.sprites = false; // sprites are pixel art already: drawn directly below, not re-sampled by the probe
      views.push_back(v);
    }
  }
  r_.drawRaster(cmd, scene, assets, views, cameraWorld);

  // 2. per-texel post: outlines, tone, palette (dither anchored to texels)
  PostData post{};
  post.info = vec4((f32)n, s.outlineStrength, s.highlightStrength, s.outlines ? 1.0f : 0.0f);
  post.tone = vec4(s.exposure, (f32)r_.paletteColors(scene, assets, post.palette), s.dither, 0.0f);
  u32 postOffset = r_.pushUniform(&post, sizeof(post));
  for (int pi = 0; pi < probeCount; pi++) {
    Probe& p = probes_[pi];
    RenderPassDesc pass;
    pass.name = "probe post";
    pass.color[0].texture = p.final;
    cmd.beginRenderPass(pass);
    cmd.setViewport(0, 0, (f32)(6 * n), (f32)n);
    cmd.setScissor(0, 0, 6 * n, n);
    cmd.setPipeline(postPipeline_);
    cmd.setBindGroup(0, p.postGroup, {&postOffset, 1});
    cmd.draw(3);
    cmd.endRenderPass();
  }

  // 3. splat both probes into the scene targets; the eye probe is pushed back a
  // little in depth so it only shows where the grid probe has nothing.
  RenderPassDesc pass;
  pass.name = "texel splat";
  pass.colorCount = 2;
  pass.color[0].texture = r_.color_;
  pass.color[1].texture = r_.normalDepth_;
  pass.color[1].clear[3] = 10000.0f; // "sky" depth
  pass.depth = r_.depth_;
  cmd.beginRenderPass(pass);
  cmd.setViewport(0, 0, (f32)width, (f32)height);
  cmd.setScissor(0, 0, width, height);
  cmd.setPipeline(splatPipeline_);
  // Grid probe, then (while fading) the previous grid probe, then the eye probe behind both.
  std::vector<int> order = fading ? std::vector<int>{0, 2, 1} : std::vector<int>{0, 1};
  for (int pi : order) {
    SplatData d{};
    d.viewProj = proj * view;
    d.view = view;
    d.origin = vec4(origins[pi], (f32)n);
    for (int f = 0; f < 6; f++) {
      Face fc = face(f);
      d.forward[f] = vec4(fc.forward, 0);
      d.right[f] = vec4(fc.right, 0);
      d.up[f] = vec4(fc.up, 0);
    }
    // A hair larger than a texel: no cracks from rounding. During a crossfade the
    // new grid probe keeps texels whose dither value is below the fade, the old one the rest.
    d.camera = vec4(cameraPos, 0.0f);
    d.params = vec4(1.02f, pi == 1 ? 0.01f : (pi == 2 ? 0.003f : 0.0f), fade_, !fading ? 0.0f : (pi == 0 ? 1.0f : (pi == 2 ? -1.0f : 0.0f)));
    u32 offset = r_.pushUniform(&d, sizeof(d));
    cmd.setBindGroup(0, probes_[pi].splatGroup, {&offset, 1});
    cmd.draw(36 * n * n); // 6 faces x n x n texels x 6 vertices; empty texels collapse
  }
  cmd.endRenderPass();

  // 4. sprites, straight from the camera over the splats (depth-tested against them).
  // Their texels are world-space squares on the card already, so they stay stable.
  RasterView sv;
  sv.view = view;
  sv.proj = proj;
  sv.color = r_.color_;
  sv.normalDepth = r_.normalDepth_;
  sv.depth = r_.depth_;
  sv.viewport = vec4(0, 0, (f32)width, (f32)height);
  sv.clear = false;
  sv.meshes = false;
  r_.drawRaster(cmd, scene, assets, {&sv, 1}, cameraWorld);
}

} // namespace ace
