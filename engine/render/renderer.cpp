#include "render/renderer.h"
#include "render/path_tracer.h"
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
  {
    BindGroupDesc d;
    d.layout = frameLayout_;
    d.entries[0].buffer = uniforms_;
    d.entries[0].size = sizeof(FrameData);
    d.entries[1].texture = shadowMap_;
    frameGroup_ = device_.createBindGroup(d);
  }
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
  pathTracer_ = std::make_unique<PathTracer>(*this);
}

Renderer::~Renderer() { device_.waitIdle(); }

ShaderHandle Renderer::loadShader(const char* name, ShaderStage stage) {
  std::string base = engineDataDir() + "shaders/" + name + (stage == ShaderStage::Vertex ? ".vs" : ".fs");
  std::vector<u8> spirv, dxil;
  bool ok = device_.backend() == Backend::D3D12 ? readFile(base + ".dxil", dxil) : readFile(base + ".spv", spirv);
  ACE_ASSERT(ok, "missing compiled shader %s", base.c_str());
  return device_.createShader({stage, spirv, dxil, name});
}

void Renderer::beginFrame() { uniformOffset_ = 0; }

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

void Renderer::drawRaster(CommandList& cmd, Scene& scene, Assets& assets, const mat4& view, const mat4& proj) {
  FrameData frame;
  frame.view = view;
  frame.viewProj = proj * view;
  frame.lightDir = vec4(glm::normalize(vec3(0.4f, 1.0f, 0.3f)), 0.0f);
  frame.lightColor = vec4(0.0f);
  for (auto [e, t, l] : scene.reg.view<Transform, Light>().each()) {
    frame.lightDir = vec4(glm::normalize(vec3(t.world[2])), 0.0f); // light shines along -Z
    frame.lightColor = vec4(toLinear(l.color) * l.intensity, 0.0f);
    break;
  }
  frame.ambient = vec4(toLinear(scene.settings.ambient), 0.0f);
  f32 shadowTexel = fitShadowFrustum(scene, assets, vec3(frame.lightDir), frame.lightViewProj);
  frame.params = vec4((f32)scene.settings.toonBands, scene.settings.shadows ? 1.0f : 0.0f, shadowTexel, scene.settings.dither);
  frame.params2 = vec4(scene.settings.texelLighting ? 1.0f : 0.0f, (f32)std::max(scene.settings.texelsPerUnit, 1), 0, 0);
  u32 frameOffset = pushUniform(&frame, sizeof(frame));

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
  mat4 camWorld = glm::inverse(view);
  f64 seconds = (f64)SDL_GetTicks() / 1000.0;
  for (auto [e, t, s] : scene.reg.view<Transform, SpriteRenderer>().each()) {
    // Upright card facing a horizontal direction, keeping the entity's scale.
    auto upright = [&](vec3 right) {
      right = glm::normalize(vec3(right.x, 0.0f, right.z));
      vec3 scale(glm::length(vec3(t.world[0])), glm::length(vec3(t.world[1])), 1.0f);
      return mat4(vec4(right * scale.x, 0), vec4(0, scale.y, 0, 0), vec4(glm::cross(right, vec3(0, 1, 0)), 0), t.world[3]);
    };
    mat4 size = glm::scale(mat4(1.0f), vec3(s.size, 1.0f));
    vec3 sun = vec3(frame.lightDir);
    Item it{quad, assets.texture(s.texture), {}, t.world * size, true};
    it.data.model = (s.billboard ? upright(vec3(camWorld[0])) : t.world) * size;
    if (s.billboard && fabsf(sun.y) < 0.999f) it.shadowModel = upright(glm::cross(vec3(0, 1, 0), sun)) * size;
    it.data.color = toLinear(s.color);
    it.data.flags = vec4(1, 0, 0, 0);
    // Lit from where it stands: the bottom edge of the card.
    it.data.origin = vec4(vec3(t.world[3]) - vec3(0.0f, 0.5f * s.size.y * glm::length(vec3(t.world[1])), 0.0f), 0.0f);
    int columns = std::max(s.columns, 1), rows = std::max(s.rows, 1), count = columns * rows;
    int index = (s.frame + (s.fps > 0.0f ? (int)(seconds * s.fps) : 0)) % count;
    if (index < 0) index += count;
    vec2 cell(1.0f / columns, 1.0f / rows), corner((f32)(index % columns) * cell.x, (f32)(index / columns) * cell.y);
    it.data.uvRect = s.flipX ? vec4(corner.x + cell.x, corner.y, -cell.x, cell.y) : vec4(corner, cell);
    items.push_back(it);
  }

  auto drawItems = [&](bool shadowPass) {
    bool spritePipeline = false;
    for (const Item& it : items) {
      if (!shadowPass && it.sprite && !spritePipeline) { // sprites are last in the list
        cmd.setPipeline(spritePipeline_);
        cmd.setBindGroup(0, frameGroup_, {&frameOffset, 1});
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
    drawItems(true);
    cmd.endRenderPass();
  }

  RenderPassDesc pass;
  pass.name = "scene";
  pass.colorCount = 2;
  pass.color[0].texture = color_;
  pass.color[1].texture = normalDepth_;
  pass.color[1].clear[3] = 10000.0f; // "sky" depth
  pass.depth = depth_;
  cmd.beginRenderPass(pass);
  cmd.setViewport(0, 0, (f32)lowWidth_, (f32)lowHeight_);
  cmd.setScissor(0, 0, lowWidth_, lowHeight_);
  cmd.setPipeline(meshPipeline_);
  cmd.setBindGroup(0, frameGroup_, {&frameOffset, 1});
  drawItems(false);
  cmd.endRenderPass();
}

void Renderer::render(CommandList& cmd, Scene& scene, Assets& assets, const CameraView& cv, TextureHandle target, Format targetFormat,
                      u32 width, u32 height) {
  if (width == 0 || height == 0) return;
  const RenderSettings& s = scene.settings;
  f32 aspect = (f32)width / (f32)height;
  Camera cam = cv.camera;
  mat4 view = glm::inverse(cv.world);

  // Low-resolution image size. When snapping, one extra texel on every side
  // gives the present pass room to scroll by the sub-texel remainder.
  u32 visH = s.pixelArt ? (u32)glm::clamp(s.pixelHeight, 16, 2160) : height;
  u32 visW = s.pixelArt ? std::max(1u, (u32)lroundf(visH * aspect)) : width;
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

  if (s.pathTraced) {
    pathTracer_->render(cmd, scene, assets, view, proj, cam, color_, normalDepth_, lowWidth_, lowHeight_);
  } else {
    drawRaster(cmd, scene, assets, view, proj);
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
  pd.rect = vec4((f32)margin - scroll.x, (f32)margin + scroll.y, (f32)visW, (f32)visH);
  pd.outline = vec4(s.outlineStrength, s.highlightStrength, s.exposure, s.outlines ? 1.0f : 0.0f);
  pd.skyTop = vec4(toLinear(s.skyTop), 1.0f);
  pd.skyBottom = vec4(toLinear(s.skyBottom), 1.0f);
  f32 texelsHigh = (f32)lowHeight_; // cam.orthoSize already covers the margin rows
  pd.camRight = vec4(vec3(cv.world[0]), cam.orthographic ? 2.0f * cam.orthoSize / texelsHigh : 0.0f);
  pd.camUp = vec4(vec3(cv.world[1]), cam.orthographic ? 0.0f : 2.0f * tanf(glm::radians(cam.fov) * 0.5f) / texelsHigh);
  pd.camForward = vec4(-vec3(cv.world[2]), 0.0f);
  // Palette: the distinct colors of the palette image, in reading order.
  // shortcut: re-read from the cached texture every frame and searched per output
  // pixel in the shader; move to a low-resolution pass + 3D lookup if it shows up.
  pd.paletteInfo = vec4(0.0f, s.dither, 0, 0);
  if (!s.palette.empty()) {
    const Texture* pal = assets.texture(s.palette);
    int count = 0;
    for (size_t i = 0; i + 3 < pal->pixels.size() && count < 64; i += 4) {
      vec4 c = vec4(pal->pixels[i], pal->pixels[i + 1], pal->pixels[i + 2], 255.0f) / 255.0f;
      bool seen = pal->pixels[i + 3] < 128; // transparent pixels are not colors
      for (int k = 0; k < count && !seen; k++) seen = pd.palette[k] == c;
      if (!seen) pd.palette[count++] = c;
    }
    pd.paletteInfo.x = (f32)count;
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
  cmd.endRenderPass();
}

} // namespace ace
