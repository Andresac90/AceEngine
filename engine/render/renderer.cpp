#include "render/renderer.h"
#include "render/path_tracer.h"

namespace ace {

using namespace rhi;

namespace {
constexpr u32 kUniformRingSize = 4 << 20;
constexpr u32 kUniformAlign = 256; // satisfies every backend's minimum offset alignment
constexpr Format kColorFormat = Format::RGBA16F;

struct FrameData {
  mat4 view, viewProj;
  vec4 lightDir, lightColor, ambient, params;
};
struct DrawData {
  mat4 model, normalMatrix;
  vec4 color, emissive;
};
struct PresentData {
  vec4 rect, outline, skyTop, skyBottom;
  vec4 camRight, camUp, camForward; // world space; camRight.w: texel size (ortho), camUp.w: texel size per unit depth (perspective)
};
} // namespace

Renderer::Renderer(Device& device) : device_(device) {
  uniforms_ = device_.createBuffer({kUniformRingSize, BufferUsage_Uniform, "uniform ring"});
  nearestSampler_ = device_.createSampler({Filter::Nearest, AddressMode::Repeat});

  BindGroupLayoutDesc ubo;
  ubo.bindings[0] = BindingType::UniformBuffer;
  frameLayout_ = device_.createBindGroupLayout(ubo);
  drawLayout_ = device_.createBindGroupLayout(ubo);
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
  frameGroup_ = uboGroup(frameLayout_, sizeof(FrameData));
  drawGroup_ = uboGroup(drawLayout_, sizeof(DrawData));

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
  ACE_ASSERT(meshPipeline_ && spritePipeline_, "failed to create scene pipelines");

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
  frame.params = vec4((f32)scene.settings.toonBands, 0, 0, 0);
  u32 frameOffset = pushUniform(&frame, sizeof(frame));

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

  auto drawMesh = [&](const Mesh* mesh, const Texture* tex, const mat4& model, vec4 color, vec4 emissive) {
    DrawData d;
    d.model = model;
    d.normalMatrix = mat4(glm::transpose(glm::inverse(mat3(model))));
    d.color = toLinear(color);
    d.emissive = toLinear(emissive);
    u32 offset = pushUniform(&d, sizeof(d));
    cmd.setBindGroup(1, drawGroup_, {&offset, 1});
    cmd.setBindGroup(2, materialGroup(tex));
    cmd.setVertexBuffer(mesh->vertexBuffer);
    cmd.setIndexBuffer(mesh->indexBuffer);
    cmd.drawIndexed((u32)mesh->indices.size());
  };

  cmd.setPipeline(meshPipeline_);
  cmd.setBindGroup(0, frameGroup_, {&frameOffset, 1});
  for (auto [e, t, m] : scene.reg.view<Transform, MeshRenderer>().each())
    drawMesh(assets.mesh(m.mesh), assets.texture(m.texture), t.world, m.color, vec4(m.emissive, 0.0f));

  cmd.setPipeline(spritePipeline_);
  cmd.setBindGroup(0, frameGroup_, {&frameOffset, 1});
  const Mesh* quad = assets.mesh("builtin:quad");
  for (auto [e, t, s] : scene.reg.view<Transform, SpriteRenderer>().each())
    drawMesh(quad, assets.texture(s.texture), t.world * glm::scale(mat4(1.0f), vec3(s.size, 1.0f)), s.color, vec4(0, 0, 0, 1));

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
  PresentData pd;
  pd.rect = vec4((f32)margin - scroll.x, (f32)margin + scroll.y, (f32)visW, (f32)visH);
  pd.outline = vec4(s.outlineStrength, s.highlightStrength, s.exposure, s.outlines ? 1.0f : 0.0f);
  pd.skyTop = vec4(toLinear(s.skyTop), 1.0f);
  pd.skyBottom = vec4(toLinear(s.skyBottom), 1.0f);
  f32 texelsHigh = (f32)lowHeight_; // cam.orthoSize already covers the margin rows
  pd.camRight = vec4(vec3(cv.world[0]), cam.orthographic ? 2.0f * cam.orthoSize / texelsHigh : 0.0f);
  pd.camUp = vec4(vec3(cv.world[1]), cam.orthographic ? 0.0f : 2.0f * tanf(glm::radians(cam.fov) * 0.5f) / texelsHigh);
  pd.camForward = vec4(-vec3(cv.world[2]), 0.0f);
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
