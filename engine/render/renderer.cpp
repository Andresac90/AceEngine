#include "render/renderer.h"
#include "render/path_tracer.h"
#include "render/texel_splat.h"
#include "render/pixel_font.h"
#include <SDL3/SDL.h>

namespace ace {

using namespace rhi;

namespace {
constexpr u32 kUniformRingSize = 4 << 20;
constexpr u32 kUniformAlign = 256; // satisfies every backend's minimum offset alignment
constexpr Format kColorFormat = Format::RGBA16F;
constexpr u32 kShadowMapSize = 2048;

struct FrameData {
  mat4 view, viewProj;
  vec4 lightDir, lightColor, ambient, params, params2;
  mat4 lightViewProj;
  vec4 pointInfo;
  vec4 pointPosition[8], pointColor[8];
};
struct DrawData {
  mat4 model, normalMatrix;
  vec4 color, emissive, flags, origin, uvRect;
};
struct PresentData {
  vec4 rect, outline, skyTop, skyBottom;
  vec4 camRight, camUp, camForward; // world space; camRight.w: texel size (ortho), camUp.w: texel size per unit depth (perspective)
  vec4 paletteInfo; // x: color count, y: dither amount
  vec4 palette[64]; // sRGB
};
} // namespace

Renderer::Renderer(Device& device) : device_(device) {
  uniforms_ = device_.createBuffer({kUniformRingSize, BufferUsage_Uniform, "uniform ring"});
  nearestSampler_ = device_.createSampler({Filter::Nearest, AddressMode::Repeat});

  BindGroupLayoutDesc ubo;
  ubo.bindings[0] = BindingType::UniformBuffer;
  drawLayout_ = device_.createBindGroupLayout(ubo);
  BindGroupLayoutDesc frameDesc = ubo;
  frameDesc.bindings[1] = BindingType::Texture; // shadow map
  frameDesc.bindings[2] = BindingType::Texture; // palette (ramp shading)
  frameLayout_ = device_.createBindGroupLayout(frameDesc);
  u32 target = TextureUsage_RenderTarget | TextureUsage_Sampled;
  shadowMap_ = device_.createTexture({kShadowMapSize, kShadowMapSize, Format::R32F, target, "shadow map"});
  shadowDepth_ = device_.createTexture({kShadowMapSize, kShadowMapSize, Format::D32F, TextureUsage_RenderTarget, "shadow depth"});
  BindGroupLayoutDesc material;
  material.bindings[0] = BindingType::Texture;
  material.bindings[1] = BindingType::Sampler;
  materialLayout_ = device_.createBindGroupLayout(material);
  BindGroupLayoutDesc present;
  present.bindings[0] = BindingType::UniformBuffer;
  present.bindings[1] = BindingType::Texture;
  present.bindings[2] = BindingType::Texture;
  presentLayout_ = device_.createBindGroupLayout(present);

  auto uboGroup = [&](BindGroupLayoutHandle layout, u32 size) {
    BindGroupDesc d;
    d.layout = layout;
    d.entries[0].buffer = uniforms_;
    d.entries[0].size = size;
    return device_.createBindGroup(d);
  };
  drawGroup_ = uboGroup(drawLayout_, sizeof(DrawData));
  shadowFrameGroup_ = uboGroup(drawLayout_, sizeof(FrameData)); // the shadow pass cannot bind the map it renders to

  PipelineDesc p;
  p.name = "mesh";
  p.vertexShader = loadShader("mesh", ShaderStage::Vertex);
  p.fragmentShader = loadShader("mesh", ShaderStage::Fragment);
  p.attributes[0] = {0, VertexFormat::Float3, offsetof(Vertex, position)};
  p.attributes[1] = {1, VertexFormat::Float3, offsetof(Vertex, normal)};
  p.attributes[2] = {2, VertexFormat::Float2, offsetof(Vertex, uv)};
  p.attributes[3] = {3, VertexFormat::UByte4Norm, offsetof(Vertex, color)};
  p.attributeCount = 4;
  p.vertexStride = sizeof(Vertex);
  p.bindGroupLayouts[0] = frameLayout_;
  p.bindGroupLayouts[1] = drawLayout_;
  p.bindGroupLayouts[2] = materialLayout_;
  p.colorFormats[0] = kColorFormat;
  p.colorFormats[1] = kColorFormat;
  p.colorCount = 2;
  p.depthFormat = Format::D32F;
  meshPipeline_ = device_.createPipeline(p);
  p.name = "sprite";
  p.cull = CullMode::None;
  spritePipeline_ = device_.createPipeline(p);
  // Probe capture keeps full-precision depth: splats are placed from it.
  p.colorFormats[1] = Format::RGBA32F;
  p.name = "sprite capture";
  spriteCapturePipeline_ = device_.createPipeline(p);
  p.name = "mesh capture";
  p.cull = CullMode::Back;
  meshCapturePipeline_ = device_.createPipeline(p);
  PipelineDesc sp;
  sp.name = "shadow";
  sp.vertexShader = loadShader("shadow", ShaderStage::Vertex);
  sp.fragmentShader = loadShader("shadow", ShaderStage::Fragment);
  sp.attributes[0] = {0, VertexFormat::Float3, offsetof(Vertex, position)};
  sp.attributes[1] = {2, VertexFormat::Float2, offsetof(Vertex, uv)};
  sp.attributeCount = 2;
  sp.vertexStride = sizeof(Vertex);
  sp.bindGroupLayouts[0] = drawLayout_;
  sp.bindGroupLayouts[1] = drawLayout_;
  sp.bindGroupLayouts[2] = materialLayout_;
  sp.colorFormats[0] = Format::R32F;
  sp.depthFormat = Format::D32F;
  sp.cull = CullMode::None; // single-sided planes must cast shadows too
  shadowPipeline_ = device_.createPipeline(sp);
  ACE_ASSERT(meshPipeline_ && spritePipeline_ && shadowPipeline_, "failed to create scene pipelines");

  presentVs_ = loadShader("present", ShaderStage::Vertex);
  presentFs_ = loadShader("present", ShaderStage::Fragment);
  uiVs_ = loadShader("imgui", ShaderStage::Vertex);
  uiFs_ = loadShader("imgui", ShaderStage::Fragment);

  // Font atlas: glyphs side by side in ASCII order starting at ' ', white with alpha.
  {
    const int count = 95;
    font_.width = (u32)(count * pixel_font::kGlyphW);
    font_.height = (u32)pixel_font::kGlyphH;
    font_.pixels.assign((size_t)font_.width * font_.height * 4, 0);
    for (const pixel_font::Glyph& g : pixel_font::kGlyphs) {
      int index = g.c - ' ';
      for (int y = 0; y < pixel_font::kGlyphH; y++)
        for (int x = 0; x < pixel_font::kGlyphW; x++)
          if (g.rows[y][x] == '#') memset(&font_.pixels[((size_t)y * font_.width + index * pixel_font::kGlyphW + x) * 4], 255, 4);
    }
    font_.handle = device_.createTexture({font_.width, font_.height, Format::RGBA8, TextureUsage_Sampled, "pixel font"});
    device_.writeTexture(font_.handle, font_.pixels.data());
  }
  pathTracer_ = std::make_unique<PathTracer>(*this);
  splatter_ = std::make_unique<TexelSplatter>(*this);
}

Renderer::~Renderer() { device_.waitIdle(); }

ShaderHandle Renderer::loadShader(const char* name, ShaderStage stage) {
  std::string base = engineDataDir() + "shaders/" + name + (stage == ShaderStage::Vertex ? ".vs" : ".fs");
  std::vector<u8> spirv, dxil;
  bool ok = device_.backend() == Backend::D3D12 ? readFile(base + ".dxil", dxil) : readFile(base + ".spv", spirv);
  ACE_ASSERT(ok, "missing compiled shader %s", base.c_str());
  return device_.createShader({stage, spirv, dxil, name});
}

void Renderer::beginFrame(f64 seconds) {
  uniformOffset_ = 0;
  frameIndex_++;
  time_ = seconds;
}

u32 Renderer::pushUniform(const void* data, u32 size) {
  u32 offset = uniformOffset_;
  u32 next = offset + ((size + kUniformAlign - 1) & ~(kUniformAlign - 1));
  ACE_ASSERT(next <= kUniformRingSize, "uniform ring exhausted: too many draws in one frame");
  device_.writeBuffer(uniforms_, offset, data, size);
  uniformOffset_ = next;
  return offset;
}

bool Renderer::findCamera(Scene& scene, CameraView& out) {
  for (auto [e, t, c] : scene.reg.view<Transform, Camera>().each()) {
    out.world = t.world;
    out.camera = c;
    return true;
  }
  return false;
}

int spriteDirection(const SpriteRenderer& s, const mat4& world, vec3 cameraPosition) {
  if (s.directions <= 1) return 0;
  vec2 forward = -vec2(world[2].x, world[2].z);
  vec2 toViewer = vec2(cameraPosition.x - world[3].x, cameraPosition.z - world[3].z);
  if (glm::length(forward) < 1e-6f || glm::length(toViewer) < 1e-6f) return 0;
  // Counter-clockwise seen from above (+Y) in x/z: the sign is flipped because +Z points towards the viewer.
  f32 angle = atan2f(forward.x * toViewer.y - forward.y * toViewer.x, glm::dot(forward, toViewer));
  int row = (int)lroundf(-angle / (glm::two_pi<f32>() / (f32)s.directions));
  return ((row % s.directions) + s.directions) % s.directions;
}

vec4 spriteUvRect(const SpriteRenderer& s, f64 seconds, int directionRow) {
  bool directional = s.directions > 1;
  int columns = std::max(s.columns, 1), rows = directional ? s.directions : std::max(s.rows, 1);
  int count = directional ? columns : columns * rows; // directional sheets animate along their row
  int index = (s.frame + (s.fps > 0.0f ? (int)(seconds * s.fps) : 0)) % count;
  if (index < 0) index += count;
  if (directional) index += directionRow * columns;
  vec2 cell(1.0f / columns, 1.0f / rows), corner((f32)(index % columns) * cell.x, (f32)(index / columns) * cell.y);
  return s.flipX ? vec4(corner.x + cell.x, corner.y, -cell.x, cell.y) : vec4(corner, cell);
}

mat4 uprightCard(const mat4& world, vec3 right) {
  right = glm::normalize(vec3(right.x, 0.0f, right.z));
  vec3 scale(glm::length(vec3(world[0])), glm::length(vec3(world[1])), 1.0f);
  return mat4(vec4(right * scale.x, 0), vec4(0, scale.y, 0, 0), vec4(glm::cross(right, vec3(0, 1, 0)), 0), world[3]);
}

void Renderer::clearTextureCaches() {
  device_.waitIdle();
  for (auto& [id, g] : materialGroups_) device_.destroy(g);
  for (auto& [id, g] : frameGroups_) device_.destroy(g);
  materialGroups_.clear();
  frameGroups_.clear();
  pathTracer_->invalidate();
}

BindGroupHandle Renderer::frameGroup(const Texture* palette) {
  BindGroupHandle& group = frameGroups_[palette->handle.id];
  if (!group) {
    BindGroupDesc d;
    d.layout = frameLayout_;
    d.entries[0].buffer = uniforms_;
    d.entries[0].size = sizeof(FrameData);
    d.entries[1].texture = shadowMap_;
    d.entries[2].texture = palette->handle;
    group = device_.createBindGroup(d);
  }
  return group;
}

BindGroupHandle Renderer::materialGroup(const Texture* texture) {
  auto it = materialGroups_.find(texture->handle.id);
  if (it != materialGroups_.end()) return it->second;
  BindGroupDesc d;
  d.layout = materialLayout_;
  d.entries[0].texture = texture->handle;
  d.entries[1].sampler = nearestSampler_;
  return materialGroups_[texture->handle.id] = device_.createBindGroup(d);
}

void Renderer::ensureTargets(u32 w, u32 h) {
  if (w == lowWidth_ && h == lowHeight_) return;
  device_.waitIdle();
  if (color_) {
    device_.destroy(color_);
    device_.destroy(normalDepth_);
    device_.destroy(depth_);
    device_.destroy(presentGroup_);
  }
  lowWidth_ = w;
  lowHeight_ = h;
  u32 rt = TextureUsage_RenderTarget | TextureUsage_Sampled;
  color_ = device_.createTexture({w, h, kColorFormat, rt, "scene color"});
  normalDepth_ = device_.createTexture({w, h, kColorFormat, rt, "scene normal+depth"});
  depth_ = device_.createTexture({w, h, Format::D32F, TextureUsage_RenderTarget, "scene depth"});
  BindGroupDesc d;
  d.layout = presentLayout_;
  d.entries[0].buffer = uniforms_;
  d.entries[0].size = sizeof(PresentData);
  d.entries[1].texture = color_;
  d.entries[2].texture = normalDepth_;
  presentGroup_ = device_.createBindGroup(d);
}

// shortcut: one shadow map over the whole scene, so texel density drops as levels
// grow; fit to the camera frustum (texel-snapped) or add cascades for big worlds.
f32 Renderer::fitShadowFrustum(Scene& scene, Assets& assets, vec3 lightDir, mat4& lightViewProj) {
  Aabb bounds;
  for (auto [e, t, m] : scene.reg.view<Transform, MeshRenderer>().each()) {
    const Aabb& b = assets.mesh(m.mesh)->bounds;
    for (int c = 0; c < 8; c++) bounds.grow(vec3(t.world * vec4(c & 1 ? b.max.x : b.min.x, c & 2 ? b.max.y : b.min.y, c & 4 ? b.max.z : b.min.z, 1.0f)));
  }
  if (bounds.min.x > bounds.max.x) bounds = Aabb{vec3(-1.0f), vec3(1.0f)}; // no meshes
  vec3 center = (bounds.min + bounds.max) * 0.5f;
  f32 radius = std::max(glm::length(bounds.max - center), 0.5f);
  vec3 up = fabsf(lightDir.y) > 0.99f ? vec3(1, 0, 0) : vec3(0, 1, 0);
  mat4 view = glm::lookAt(center + lightDir * radius, center, up);
  lightViewProj = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, 2.0f * radius) * view;
  return 2.0f * radius / (f32)kShadowMapSize;
}

void Renderer::drawRaster(CommandList& cmd, Scene& scene, Assets& assets, std::span<const RasterView> views, const mat4& spriteCamera) {
  if (views.empty()) return;
  FrameData frame{};
  frame.lightDir = vec4(glm::normalize(vec3(0.4f, 1.0f, 0.3f)), 0.0f);
  frame.lightColor = vec4(0.0f);
  bool haveSun = false;
  struct Lamp {
    f32 distance;
    vec4 position, color;
  };
  std::vector<Lamp> lamps;
  vec3 eye = vec3(spriteCamera[3]);
  for (auto [e, t, l] : scene.reg.view<Transform, Light>().each()) {
    if (l.point) {
      vec3 p = vec3(t.world[3]);
      lamps.push_back({glm::distance(p, eye), vec4(p, std::max(l.range, 0.01f)), vec4(toLinear(l.color) * l.intensity, 0.0f)});
    } else if (!haveSun) {
      frame.lightDir = vec4(glm::normalize(vec3(t.world[2])), 0.0f); // light shines along -Z
      frame.lightColor = vec4(toLinear(l.color) * l.intensity, 0.0f);
      haveSun = true;
    }
  }
  std::sort(lamps.begin(), lamps.end(), [](const Lamp& a, const Lamp& b) { return a.distance < b.distance; });
  int lampCount = (int)std::min<size_t>(lamps.size(), 8);
  for (int i = 0; i < lampCount; i++) {
    frame.pointPosition[i] = lamps[i].position;
    frame.pointColor[i] = lamps[i].color;
  }
  frame.pointInfo = vec4((f32)lampCount, 0, 0, 0);
  frame.ambient = vec4(toLinear(scene.settings.ambient), 0.0f);
  f32 shadowTexel = fitShadowFrustum(scene, assets, vec3(frame.lightDir), frame.lightViewProj);
  frame.params = vec4((f32)scene.settings.toonBands, scene.settings.shadows ? 1.0f : 0.0f, shadowTexel, scene.settings.dither);
  const Texture* palette = assets.texture(scene.settings.palette); // white 1x1 when unset
  bool ramps = scene.settings.rampShading && !scene.settings.palette.empty();
  frame.params2 = vec4(scene.settings.texelLighting ? 1.0f : 0.0f, (f32)std::max(scene.settings.texelsPerUnit, 1), ramps ? 1.0f : 0.0f,
                       (f32)glm::clamp(scene.settings.rampShadowSteps, 0, 15));
  BindGroupHandle frameGroupHandle = frameGroup(palette);
  std::vector<u32> frameOffsets;
  for (const RasterView& v : views) {
    frame.view = v.view;
    frame.viewProj = v.proj * v.view;
    frameOffsets.push_back(pushUniform(&frame, sizeof(frame)));
  }
  u32 frameOffset = frameOffsets[0]; // the shadow pass only reads the light's matrix

  // Gather everything once; the shadow pass and the scene pass draw the same list.
  struct Item {
    const Mesh* mesh;
    const Texture* texture;
    DrawData data;
    mat4 shadowModel; // billboards turn towards the sun for their shadow
    bool sprite;
  };
  std::vector<Item> items;
  for (auto [e, t, m] : scene.reg.view<Transform, MeshRenderer>().each()) {
    Item it{assets.mesh(m.mesh), assets.texture(m.texture), {}, t.world, false};
    it.data.model = t.world;
    it.data.color = toLinear(m.color);
    it.data.emissive = vec4(toLinear(m.emissive), 0.0f);
    it.data.flags = vec4(0, m.worldTexels ? 1.0f : 0.0f, 0, 0);
    it.data.uvRect = vec4(0, 0, 1, 1);
    items.push_back(it);
  }
  const Mesh* quad = assets.mesh("builtin:quad");
  const mat4& camWorld = spriteCamera;
  f64 seconds = time_;
  for (auto [e, t, s] : scene.reg.view<Transform, SpriteRenderer>().each()) {
    auto upright = [&](vec3 right) { return uprightCard(t.world, right); };
    mat4 size = glm::scale(mat4(1.0f), vec3(s.size, 1.0f));
    vec3 sun = vec3(frame.lightDir);
    Item it{quad, assets.texture(s.texture), {}, t.world * size, true};
    it.data.model = (s.billboard ? upright(vec3(camWorld[0])) : t.world) * size;
    if (s.billboard && fabsf(sun.y) < 0.999f) it.shadowModel = upright(glm::cross(vec3(0, 1, 0), sun)) * size;
    it.data.color = toLinear(s.color);
    it.data.flags = vec4(1, 0, 0, 0);
    // Lit from where it stands: the bottom edge of the card.
    it.data.origin = vec4(vec3(t.world[3]) - vec3(0.0f, 0.5f * s.size.y * glm::length(vec3(t.world[1])), 0.0f), 0.0f);
    it.data.uvRect = spriteUvRect(s, seconds, spriteDirection(s, t.world, vec3(camWorld[3])));
    items.push_back(it);
  }

  auto drawItems = [&](bool shadowPass, PipelineHandle spritePipelineHandle, u32 viewOffset, bool meshes = true, bool sprites = true) {
    bool spritePipeline = false;
    for (const Item& it : items) {
      if (!shadowPass && !(it.sprite ? sprites : meshes)) continue;
      if (!shadowPass && it.sprite && !spritePipeline) { // sprites are last in the list
        cmd.setPipeline(spritePipelineHandle);
        cmd.setBindGroup(0, frameGroupHandle, {&viewOffset, 1});
        spritePipeline = true;
      }
      DrawData d = it.data;
      if (shadowPass) d.model = it.sprite ? it.shadowModel : d.model;
      d.normalMatrix = mat4(glm::transpose(glm::inverse(mat3(d.model))));
      u32 offset = pushUniform(&d, sizeof(d));
      cmd.setBindGroup(1, drawGroup_, {&offset, 1});
      cmd.setBindGroup(2, materialGroup(it.texture));
      cmd.setVertexBuffer(it.mesh->vertexBuffer);
      cmd.setIndexBuffer(it.mesh->indexBuffer);
      cmd.drawIndexed((u32)it.mesh->indices.size());
    }
  };

  if (scene.settings.shadows) {
    RenderPassDesc sp;
    sp.name = "shadow";
    sp.color[0].texture = shadowMap_;
    sp.color[0].clear[0] = 1.0f; // far plane: nothing occludes
    sp.depth = shadowDepth_;
    cmd.beginRenderPass(sp);
    cmd.setPipeline(shadowPipeline_);
    cmd.setBindGroup(0, shadowFrameGroup_, {&frameOffset, 1});
    drawItems(true, {}, frameOffset);
    cmd.endRenderPass();
  }

  for (size_t i = 0; i < views.size(); i++) {
    const RasterView& v = views[i];
    RenderPassDesc pass;
    pass.name = v.capture ? "probe capture" : "scene";
    pass.colorCount = 2;
    pass.color[0].texture = v.color;
    pass.color[1].texture = v.normalDepth;
    pass.color[1].clear[3] = 10000.0f; // "sky" depth
    pass.color[0].load = pass.color[1].load = v.clear ? LoadOp::Clear : LoadOp::Load;
    pass.depth = v.depth;
    pass.depthLoad = v.clear ? LoadOp::Clear : LoadOp::Load;
    cmd.beginRenderPass(pass);
    cmd.setViewport(v.viewport.x, v.viewport.y, v.viewport.z, v.viewport.w);
    cmd.setScissor((i32)v.viewport.x, (i32)v.viewport.y, (u32)v.viewport.z, (u32)v.viewport.w);
    cmd.setPipeline(v.capture ? meshCapturePipeline_ : meshPipeline_);
    cmd.setBindGroup(0, frameGroupHandle, {&frameOffsets[i], 1});
    drawItems(false, v.capture ? spriteCapturePipeline_ : spritePipeline_, frameOffsets[i], v.meshes, v.sprites);
    cmd.endRenderPass();
  }
}

int Renderer::paletteColors(Scene& scene, Assets& assets, vec4 out[64]) {
  if (scene.settings.palette.empty()) return 0;
  const Texture* pal = assets.texture(scene.settings.palette);
  int count = 0;
  for (size_t i = 0; i + 3 < pal->pixels.size() && count < 64; i += 4) {
    vec4 c = vec4(pal->pixels[i], pal->pixels[i + 1], pal->pixels[i + 2], 255.0f) / 255.0f;
    bool seen = pal->pixels[i + 3] < 128; // transparent pixels are not colors
    for (int k = 0; k < count && !seen; k++) seen = out[k] == c;
    if (!seen) out[count++] = c;
  }
  return count;
}

int Renderer::textWidth(std::string_view text) {
  int best = 0, line = 0;
  for (char c : text) {
    line = c == '\n' ? 0 : line + pixel_font::kAdvance;
    best = std::max(best, line);
  }
  return best > 0 ? best - 1 : 0; // no spacing after the last glyph
}

void Renderer::drawTexts(CommandList& cmd, std::span<const TextDraw> texts, Format targetFormat, u32 width, u32 height, f32 ppg) {
  struct UiVertex {
    vec2 pos, uv;
    u8 color[4];
  };
  std::vector<UiVertex> verts;
  auto quad = [&](vec2 p0, vec2 p1, vec2 uv0, vec2 uv1, const u8* color) {
    UiVertex v[4] = {{p0, uv0, {}}, {{p1.x, p0.y}, {uv1.x, uv0.y}, {}}, {p1, uv1, {}}, {{p0.x, p1.y}, {uv0.x, uv1.y}, {}}};
    for (UiVertex& x : v) memcpy(x.color, color, 4);
    for (int i : {0, 1, 2, 0, 2, 3}) verts.push_back(v[i]);
  };
  for (const TextDraw& t : texts) {
    int scale = std::max(t.scale, 1);
    for (int pass = t.shadow ? 0 : 1; pass < 2; pass++) {
      vec4 c = pass == 0 ? vec4(0.05f, 0.05f, 0.1f, t.color.a * 0.85f) : t.color;
      u8 color[4];
      for (int i = 0; i < 4; i++) color[i] = (u8)(glm::clamp(c[i], 0.0f, 1.0f) * 255.0f + 0.5f);
      vec2 pen = glm::floor(t.position) + (pass == 0 ? vec2((f32)scale) : vec2(0.0f));
      f32 lineStart = pen.x;
      for (char ch : t.text) {
        if (ch == '\n') {
          pen = vec2(lineStart, pen.y + pixel_font::kLineHeight * scale);
          continue;
        }
        unsigned char u = (unsigned char)(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
        int index = u >= 32 && u < 127 ? u - 32 : '?' - 32;
        if (index != 0) {
          vec2 uv0((f32)(index * pixel_font::kGlyphW) / (f32)font_.width, 0.0f);
          vec2 uv1((f32)((index + 1) * pixel_font::kGlyphW) / (f32)font_.width, 1.0f);
          vec2 size((f32)(pixel_font::kGlyphW * scale), (f32)(pixel_font::kGlyphH * scale));
          quad(pen * ppg, (pen + size) * ppg, uv0, uv1, color);
        }
        pen.x += (f32)(pixel_font::kAdvance * scale);
      }
    }
  }
  if (verts.empty()) return;

  PipelineHandle& pipeline = uiPipelines_[targetFormat];
  if (!pipeline) {
    PipelineDesc p;
    p.name = "ui text";
    p.vertexShader = uiVs_;
    p.fragmentShader = uiFs_;
    p.attributes[0] = {0, VertexFormat::Float2, offsetof(UiVertex, pos)};
    p.attributes[1] = {1, VertexFormat::Float2, offsetof(UiVertex, uv)};
    p.attributes[2] = {2, VertexFormat::UByte4Norm, offsetof(UiVertex, color)};
    p.attributeCount = 3;
    p.vertexStride = sizeof(UiVertex);
    p.bindGroupLayouts[0] = drawLayout_; // one uniform buffer: scale + translate
    p.bindGroupLayouts[1] = materialLayout_;
    p.colorFormats[0] = targetFormat;
    p.cull = CullMode::None;
    p.blend[0] = BlendMode::Alpha;
    pipeline = device_.createPipeline(p);
    ACE_ASSERT(pipeline, "failed to create the text pipeline");
  }
  u64 bytes = verts.size() * sizeof(UiVertex);
  if (bytes > uiCapacity_) {
    if (uiVertices_) device_.destroy(uiVertices_);
    uiCapacity_ = bytes * 2;
    uiVertices_ = device_.createBuffer({uiCapacity_, BufferUsage_Vertex, "ui text"});
  }
  device_.writeBuffer(uiVertices_, 0, verts.data(), bytes);
  vec4 st(2.0f / (f32)width, -2.0f / (f32)height, -1.0f, 1.0f); // pixels -> clip space, +Y up
  u32 offset = pushUniform(&st, sizeof(st));
  cmd.setPipeline(pipeline);
  cmd.setBindGroup(0, drawGroup_, {&offset, 1});
  cmd.setBindGroup(1, materialGroup(&font_));
  cmd.setVertexBuffer(uiVertices_);
  cmd.draw((u32)verts.size());
}

void Renderer::render(CommandList& cmd, Scene& scene, Assets& assets, const CameraView& cv, TextureHandle target, Format targetFormat,
                      u32 width, u32 height, std::span<const TextDraw> texts) {
  if (width == 0 || height == 0) return;
  const RenderSettings& s = scene.settings;
  f32 aspect = (f32)width / (f32)height;
  Camera cam = cv.camera;
  mat4 view = glm::inverse(cv.world);

  // Low-resolution image size. When snapping, one extra texel on every side
  // gives the present pass room to scroll by the sub-texel remainder.
  u32 visH = s.pixelArt ? (u32)glm::clamp(s.pixelHeight, 16, 2160) : height;
  u32 visW = s.pixelArt ? std::max(1u, (u32)lroundf(visH * aspect)) : width;
  vec2 shown((f32)visW, (f32)visH); // texels visible in the output (may end on a partial texel)
  if (s.pixelArt && s.integerScale) {
    // Pick the whole-number scale closest to the requested height. The last row
    // and column may be cut off part-way; the camera is widened to compensate, so
    // the framing stays what the scene asked for.
    u32 scale = std::max(1u, (u32)lroundf((f32)height / (f32)visH));
    shown = vec2((f32)width, (f32)height) / (f32)scale;
    visW = (u32)ceilf(shown.x);
    visH = (u32)ceilf(shown.y);
    f32 grow = (f32)visH / shown.y;
    cam.orthoSize *= grow;
    cam.fov = glm::degrees(2.0f * atanf(tanf(glm::radians(cam.fov) * 0.5f) * grow));
  }
  bool snap = s.pixelArt && s.pixelSnap && cam.orthographic;
  u32 margin = snap ? 1 : 0;
  vec2 scroll{0.0f};
  f32 projAspect = (f32)visW / (f32)visH;
  if (snap) {
    // Move the camera to the nearest texel in view space; keep the remainder.
    f32 texel = 2.0f * cam.orthoSize / (f32)visH;
    vec2 t = vec2(view[3]);
    vec2 snapped = glm::round(t / texel) * texel;
    scroll = (t - snapped) / texel;
    view[3][0] = snapped.x;
    view[3][1] = snapped.y;
    projAspect = (f32)(visW + 2) / (f32)(visH + 2);
    cam.orthoSize *= (f32)(visH + 2) / (f32)visH;
  }
  ensureTargets(visW + 2 * margin, visH + 2 * margin);
  mat4 proj = cam.projection(projAspect);

  // Orthographic views keep screen pixels: grid snapping is already stable for
  // them, and the eye probe needs a single eye point, which they do not have.
  bool worldPixels = s.worldPixels && !cv.camera.orthographic;
  if (worldPixels) {
    // Pixels live in the world (texel splatting), so the image is drawn at the
    // output resolution; pixelHeight only sets the size of UI text.
    ensureTargets(width, height);
    margin = 0;
    scroll = vec2(0.0f);
    mat4 fullProj = cv.camera.projection(aspect);
    splatter_->render(cmd, scene, assets, glm::inverse(cv.world), fullProj, cv.world, width, height);
  } else if (s.pathTraced) {
    PathTracer::Target t;
    t.color = color_;
    t.normalDepth = normalDepth_;
    t.viewport = vec4(0, 0, (f32)lowWidth_, (f32)lowHeight_);
    t.spriteCamera = glm::inverse(view);
    pathTracer_->render(cmd, scene, assets, view, proj, t);
  } else {
    RasterView rv{view, proj, color_, normalDepth_, depth_, vec4(0, 0, (f32)lowWidth_, (f32)lowHeight_), true, false};
    drawRaster(cmd, scene, assets, {&rv, 1}, glm::inverse(view));
  }

  PipelineHandle& presentPipeline = presentPipelines_[targetFormat];
  if (!presentPipeline) {
    PipelineDesc p;
    p.name = "present";
    p.vertexShader = presentVs_;
    p.fragmentShader = presentFs_;
    p.bindGroupLayouts[0] = presentLayout_;
    p.colorFormats[0] = targetFormat;
    p.cull = CullMode::None;
    presentPipeline = device_.createPipeline(p);
    ACE_ASSERT(presentPipeline, "failed to create present pipeline");
  }
  PresentData pd{};
  pd.rect = worldPixels ? vec4(0, 0, (f32)width, (f32)height) : vec4((f32)margin - scroll.x, (f32)margin + scroll.y, shown.x, shown.y);
  pd.outline = vec4(s.outlineStrength, s.highlightStrength, s.exposure, s.outlines && !worldPixels ? 1.0f : 0.0f);
  pd.skyTop = vec4(toLinear(s.skyTop), 1.0f);
  pd.skyBottom = vec4(toLinear(s.skyBottom), 1.0f);
  f32 texelsHigh = (f32)lowHeight_; // cam.orthoSize already covers the margin rows
  pd.camRight = vec4(vec3(cv.world[0]), cam.orthographic ? 2.0f * cam.orthoSize / texelsHigh : 0.0f);
  pd.camUp = vec4(vec3(cv.world[1]), cam.orthographic ? 0.0f : 2.0f * tanf(glm::radians(cam.fov) * 0.5f) / texelsHigh);
  pd.camForward = vec4(-vec3(cv.world[2]), 0.0f);
  // Palette: the distinct colors of the palette image, in reading order.
  // shortcut: searched per output pixel in the shader; move to a low-resolution
  // pass + 3D lookup if it shows up in a profile.
  pd.paletteInfo = vec4((f32)paletteColors(scene, assets, pd.palette), s.dither, 0, 0);
  pd.paletteInfo.w = 1.0f;
  if (worldPixels) { // geometry colors are final; the sky is drawn on a pixelHeight-sized grid
    pd.paletteInfo.y = s.dither;
    pd.paletteInfo.z = 1.0f;
    pd.paletteInfo.w = (f32)std::max(1, (int)lroundf((f32)height / (f32)glm::clamp(s.pixelHeight, 16, 2160)));
  }
  u32 offset = pushUniform(&pd, sizeof(pd));

  RenderPassDesc pass;
  pass.name = "present";
  pass.color[0].texture = target;
  cmd.beginRenderPass(pass);
  cmd.setViewport(0, 0, (f32)width, (f32)height);
  cmd.setScissor(0, 0, width, height);
  cmd.setPipeline(presentPipeline);
  cmd.setBindGroup(0, presentGroup_, {&offset, 1});
  cmd.draw(3);
  gameSize_ = shown;
  if (!texts.empty()) drawTexts(cmd, texts, targetFormat, width, height, (f32)height / shown.y);
  cmd.endRenderPass();
}

} // namespace ace
