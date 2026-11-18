#include "render/path_tracer.h"
#include <algorithm>
#include <SDL3/SDL.h>
#include <glm/gtx/component_wise.hpp>
#include <map>

namespace ace {

using namespace rhi;

namespace {
struct TraceData {
  mat4 invViewProj, view;
  vec4 lightDir, lightColor, skyTop, skyBottom, params, lightInfo, ramp;
};
constexpr u32 kAtlasSize = 1024;
constexpr u32 kMovingHistoryFrames = 6;
constexpr u32 kMaxAccumulatedFrames = 512; // RGBA16F stops gaining precision beyond this

u64 hashBytes(u64 h, const void* data, size_t size) { // FNV-1a
  for (size_t i = 0; i < size; i++) h = (h ^ ((const u8*)data)[i]) * 1099511628211ull;
  return h;
}
} // namespace

PathTracer::PathTracer(Renderer& renderer) : r_(renderer) {
  Device& dev = r_.device();
  BindGroupLayoutDesc l;
  l.bindings[0] = BindingType::UniformBuffer;
  l.bindings[1] = BindingType::StorageBuffer;
  l.bindings[2] = BindingType::StorageBuffer;
  l.bindings[3] = BindingType::StorageBuffer;
  l.bindings[4] = BindingType::Texture;
  l.bindings[5] = BindingType::Texture;
  layout_ = dev.createBindGroupLayout(l);
  PipelineDesc p;
  p.name = "pathtrace";
  p.vertexShader = r_.loadShader("pathtrace", ShaderStage::Vertex);
  p.fragmentShader = r_.loadShader("pathtrace", ShaderStage::Fragment);
  p.bindGroupLayouts[0] = layout_;
  p.colorCount = 2;
  p.cull = CullMode::None;
  p.colorFormats[0] = Format::RGBA16F;
  for (int capture = 0; capture < 2; capture++) {
    for (int first = 0; first < 2; first++) {
      p.colorFormats[1] = capture ? Format::RGBA32F : Format::RGBA16F;
      // Later frames blend into the running average; the first one overwrites,
      // so whatever was there (even NaN) cannot leak in.
      p.blend[0] = first ? BlendMode::Opaque : BlendMode::Alpha;
      pipelines_[capture][first] = dev.createPipeline(p);
      ACE_ASSERT(pipelines_[capture][first], "failed to create path tracing pipeline");
    }
  }
  atlas_ = dev.createTexture({kAtlasSize, kAtlasSize, Format::RGBA8_SRGB, TextureUsage_Sampled, "path tracer atlas"});
}

PathTracer::~PathTracer() = default;

// Median split on the longest axis of the centroid bounds.
// shortcut: no SAH and a full rebuild whenever anything moves; upgrade to a
// two-level BVH (per-mesh BLAS + refit TLAS) when scenes outgrow ~100k triangles.
void PathTracer::buildBvh(std::vector<Triangle>& tris, std::vector<Node>& nodes) {
  nodes.clear();
  nodes.reserve(tris.size() * 2 + 1);
  nodes.push_back(Node{vec3(0), 0, vec3(0), 0});
  if (tris.empty()) return;
  auto centroid = [](const Triangle& t) { return vec3(t.v0) + (vec3(t.e1) + vec3(t.e2)) / 3.0f; };
  struct Range {
    u32 node, first, count;
  };
  std::vector<Range> todo{{0, 0, (u32)tris.size()}};
  while (!todo.empty()) {
    Range r = todo.back();
    todo.pop_back();
    Aabb box, cbox;
    for (u32 i = r.first; i < r.first + r.count; i++) {
      const Triangle& t = tris[i];
      box.grow(vec3(t.v0));
      box.grow(vec3(t.v0 + t.e1));
      box.grow(vec3(t.v0 + t.e2));
      cbox.grow(centroid(t));
    }
    nodes[r.node].min = box.min;
    nodes[r.node].max = box.max;
    vec3 extent = cbox.max - cbox.min;
    int axis = extent.x > extent.y ? (extent.x > extent.z ? 0 : 2) : (extent.y > extent.z ? 1 : 2);
    if (r.count <= 4 || extent[axis] <= 0.0f) {
      nodes[r.node].leftFirst = r.first;
      nodes[r.node].count = r.count;
      continue;
    }
    u32 mid = r.count / 2;
    std::nth_element(tris.begin() + r.first, tris.begin() + r.first + mid, tris.begin() + r.first + r.count,
                     [&](const Triangle& a, const Triangle& b) { return centroid(a)[axis] < centroid(b)[axis]; });
    u32 left = (u32)nodes.size();
    nodes.push_back({});
    nodes.push_back({});
    nodes[r.node].leftFirst = left;
    nodes[r.node].count = 0;
    todo.push_back({left, r.first, mid});
    todo.push_back({left + 1, r.first + mid, r.count - mid});
  }
}

void PathTracer::rebuild(Scene& scene, Assets& assets, vec3 cameraRight, vec3 cameraPosition) {
  // Pack every texture in use into the atlas, left to right in rows ("shelves").
  // shortcut: fixed 1024x1024 atlas, rebuilt with the BVH; textures that do not
  // fit are drawn untextured (with a warning). Grow or page it when that happens.
  std::vector<u8> atlasPixels((size_t)kAtlasSize * kAtlasSize * 4, 255);
  std::map<const Texture*, vec4> rects;
  u32 penX = 0, penY = 0, shelf = 0;
  auto place = [&](const Texture* tex) -> vec4 {
    if (tex->width <= 1 && tex->height <= 1) return vec4(0.0f); // plain white: no texture
    auto it = rects.find(tex);
    if (it != rects.end()) return it->second;
    if (penX + tex->width > kAtlasSize) {
      penX = 0;
      penY += shelf;
      shelf = 0;
    }
    if (tex->width > kAtlasSize || penY + tex->height > kAtlasSize) {
      ACE_WARN("path tracer: texture atlas is full; a %ux%u texture is drawn untextured", tex->width, tex->height);
      return rects[tex] = vec4(0.0f);
    }
    for (u32 y = 0; y < tex->height; y++)
      memcpy(&atlasPixels[((size_t)(penY + y) * kAtlasSize + penX) * 4], &tex->pixels[(size_t)y * tex->width * 4], (size_t)tex->width * 4);
    vec4 rect((f32)penX, (f32)penY, (f32)tex->width, (f32)tex->height);
    penX += tex->width;
    shelf = std::max(shelf, tex->height);
    return rects[tex] = rect;
  };

  std::vector<Triangle> tris;
  auto addMesh = [&](const Mesh* mesh, const mat4& world, vec4 color, vec3 emissive, vec4 atlasRect, int flags, vec4 uvRect) {
    mat3 nm = glm::transpose(glm::inverse(mat3(world)));
    if (atlasRect.z > 0.0f) flags |= 4;
    for (size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
      const Vertex& a = mesh->vertices[mesh->indices[i]];
      const Vertex& b = mesh->vertices[mesh->indices[i + 1]];
      const Vertex& c = mesh->vertices[mesh->indices[i + 2]];
      vec3 p0 = vec3(world * vec4(a.position, 1)), p1 = vec3(world * vec4(b.position, 1)), p2 = vec3(world * vec4(c.position, 1));
      Triangle tri;
      tri.v0 = vec4(p0, 0);
      tri.e1 = vec4(p1 - p0, 0);
      tri.e2 = vec4(p2 - p0, 0);
      tri.n0 = vec4(glm::normalize(nm * a.normal), 0);
      tri.n1 = vec4(glm::normalize(nm * b.normal), 0);
      tri.n2 = vec4(glm::normalize(nm * c.normal), 0);
      // shortcut: one vertex color per triangle (the first vertex's).
      tri.color = toLinear(color * vec4(a.color[0], a.color[1], a.color[2], a.color[3]) / 255.0f);
      tri.emissive = vec4(toLinear(emissive), 0);
      auto uv = [&](const Vertex& v) { return v.uv * vec2(uvRect.z, uvRect.w) + vec2(uvRect.x, uvRect.y); };
      tri.uv01 = vec4(uv(a), uv(b));
      tri.uv2Flags = vec4(uv(c), (f32)flags, 0);
      tri.atlas = atlasRect;
      tris.push_back(tri);
    }
  };
  for (auto [e, t, m] : scene.reg.view<Transform, MeshRenderer>().each())
    addMesh(assets.mesh(m.mesh), t.world, m.color, m.emissive, place(assets.texture(m.texture)), m.worldTexels ? 1 : 0, vec4(0, 0, 1, 1));
  const Mesh* quad = assets.mesh("builtin:quad");
  f64 seconds = r_.time();
  for (auto [e, t, s] : scene.reg.view<Transform, SpriteRenderer>().each()) {
    mat4 model = (s.billboard ? uprightCard(t.world, cameraRight) : t.world) * glm::scale(mat4(1.0f), vec3(s.size, 1.0f));
    addMesh(quad, model, s.color, vec3(0.0f), place(assets.texture(s.texture)), 2, spriteUvRect(s, seconds, spriteDirection(s, t.world, cameraPosition)));
  }
  r_.device().writeTexture(atlas_, atlasPixels.data());

  std::vector<Node> nodes;
  buildBvh(tris, nodes);
  if (tris.empty()) tris.push_back(Triangle{});

  // Emissive triangles, by their final (post-BVH) index, for direct light sampling.
  std::vector<vec4> lights;
  for (u32 i = 0; i < tris.size(); i++) {
    f32 area = 0.5f * glm::length(glm::cross(vec3(tris[i].e1), vec3(tris[i].e2)));
    if (glm::compMax(vec3(tris[i].emissive)) <= 0.0f || area <= 0.0f) continue;
    vec4 entry(0.0f, area, 0.0f, 0.0f);
    memcpy(&entry.x, &i, sizeof(u32));
    lights.push_back(entry);
  }
  lightCount_ = (u32)lights.size();
  if (lights.empty()) lights.push_back(vec4(0.0f)); // buffers cannot be empty

  Device& dev = r_.device();
  bool grew = false;
  auto fit = [&](BufferHandle& buffer, u64& capacity, u64 bytes, const char* name) {
    if (bytes <= capacity) return;
    if (buffer) dev.destroy(buffer);
    capacity = bytes * 3 / 2;
    buffer = dev.createBuffer({capacity, BufferUsage_Storage, name});
    grew = true;
  };
  dev.waitIdle(); // the previous frame may still be reading these buffers
  fit(nodeBuffer_, nodeCapacity_, nodes.size() * sizeof(Node), "bvh nodes");
  fit(triBuffer_, triCapacity_, tris.size() * sizeof(Triangle), "bvh triangles");
  dev.writeBuffer(nodeBuffer_, 0, nodes.data(), nodes.size() * sizeof(Node));
  fit(lightBuffer_, lightCapacity_, lights.size() * sizeof(vec4), "bvh lights");
  dev.writeBuffer(triBuffer_, 0, tris.data(), tris.size() * sizeof(Triangle));
  dev.writeBuffer(lightBuffer_, 0, lights.data(), lights.size() * sizeof(vec4));
  if (grew && group_) {
    dev.destroy(group_); // recreated in render() against the new buffers
    group_ = {};
  }
}

void PathTracer::render(CommandList& cmd, Scene& scene, Assets& assets, const mat4& view, const mat4& proj, const Target& target) {
  // Anything that changes the geometry or its materials forces a BVH rebuild.
  vec3 cameraRight = vec3(target.spriteCamera[0]);
  vec3 cameraPosition = vec3(target.spriteCamera[3]);
  u64 sh = 14695981039346656037ull;
  for (auto [e, t, m] : scene.reg.view<Transform, MeshRenderer>().each()) {
    sh = hashBytes(sh, &t.world, sizeof(mat4));
    sh = hashBytes(sh, m.mesh.data(), m.mesh.size());
    sh = hashBytes(sh, m.texture.data(), m.texture.size());
    sh = hashBytes(sh, &m.color, sizeof(vec4));
    sh = hashBytes(sh, &m.emissive, sizeof(vec3));
    sh = hashBytes(sh, &m.worldTexels, sizeof(bool));
  }
  f64 seconds = r_.time();
  for (auto [e, t, s] : scene.reg.view<Transform, SpriteRenderer>().each()) {
    vec4 uvRect = spriteUvRect(s, seconds, spriteDirection(s, t.world, cameraPosition)); // animation frame and view angle
    sh = hashBytes(sh, &t.world, sizeof(mat4));
    sh = hashBytes(sh, s.texture.data(), s.texture.size());
    sh = hashBytes(sh, &s.color, sizeof(vec4));
    sh = hashBytes(sh, &s.size, sizeof(vec2));
    sh = hashBytes(sh, &uvRect, sizeof(vec4));
    if (s.billboard) sh = hashBytes(sh, &cameraRight, sizeof(vec3)); // billboards turn with the camera
  }
  if (sh != sceneHash_ || !nodeBuffer_) {
    sceneHash_ = sh;
    rebuild(scene, assets, cameraRight, cameraPosition);
    // Moving objects: keep a short history instead of restarting, trading a
    // little ghosting for far less noise.
    // shortcut: replace with motion-vector reprojection + a spatial denoiser.
    for (auto& [slot, h] : history_) h.frame = std::min(h.frame, kMovingHistoryFrames);
  }
  const Texture* palette = assets.texture(scene.settings.palette); // white 1x1 when unset
  if (!group_ || palette->handle.id != paletteId_) {
    Device& dev = r_.device();
    if (group_) dev.destroy(group_);
    BindGroupDesc g;
    g.layout = layout_;
    g.entries[0].buffer = r_.uniformBuffer();
    g.entries[0].size = sizeof(TraceData);
    g.entries[1].buffer = nodeBuffer_;
    g.entries[2].buffer = triBuffer_;
    g.entries[3].buffer = lightBuffer_;
    g.entries[4].texture = atlas_;
    g.entries[5].texture = palette->handle;
    group_ = dev.createBindGroup(g);
    paletteId_ = palette->handle.id;
  }

  TraceData d;
  d.invViewProj = glm::inverse(proj * view);
  d.view = view;
  d.lightDir = vec4(glm::normalize(vec3(0.4f, 1.0f, 0.3f)), 0);
  d.lightColor = vec4(0);
  for (auto [e, t, l] : scene.reg.view<Transform, Light>().each()) {
    if (l.point) continue; // the path tracer lights with emissive meshes instead
    d.lightDir = vec4(glm::normalize(vec3(t.world[2])), 0);
    d.lightColor = vec4(toLinear(l.color) * l.intensity, 0);
    break;
  }
  d.skyTop = vec4(toLinear(scene.settings.skyTop), 0);
  d.skyBottom = vec4(toLinear(scene.settings.skyBottom), 0);
  d.params = vec4(0, (f32)(target.samples > 0 ? target.samples : scene.settings.ptSamples), (f32)scene.settings.ptBounces, (f32)scene.settings.toonBands);

  d.lightInfo = vec4((f32)lightCount_, (f32)std::max(scene.settings.texelsPerUnit, 1), scene.settings.texelLighting ? 1.0f : 0.0f,
                     target.skipSprites ? 1.0f : 0.0f);
  // Ramp shading needs to know what "fully lit" is: a white surface facing the
  // sun, plus roughly what the sky adds.
  auto luminance = [](vec3 c) { return glm::dot(c, vec3(0.2126f, 0.7152f, 0.0722f)); };
  f32 fullyLit = luminance(vec3(d.lightColor)) * 0.318f + luminance(vec3(d.skyTop) + vec3(d.skyBottom)) * 0.25f;
  bool ramps = scene.settings.rampShading && !scene.settings.palette.empty();
  d.ramp = vec4(ramps ? 1.0f : 0.0f, (f32)glm::clamp(scene.settings.rampShadowSteps, 0, 15), fullyLit, 0);

  // Anything else that changes the image restarts the running average.
  u64 vh = hashBytes(14695981039346656037ull, &d, sizeof(d));
  u32 key[3] = {target.color.id, target.normalDepth.id, target.capture ? 1u : 0u};
  vh = hashBytes(vh, key, sizeof(key));
  vh = hashBytes(vh, &target.viewport, sizeof(vec4));
  History& h = history_[target.slot];
  // The running average lives in shared targets. If this slot was not traced
  // the previous frame, another mode drew there, or the targets were recreated
  // (possibly with the same handle ids): the history is not ours.
  if (vh != h.viewHash || r_.frameIndex() != h.lastFrame + 1) h.frame = 0;
  h.viewHash = vh;
  h.lastFrame = r_.frameIndex();
  // Settled: nothing changed for settleFrames frames, so the image is as good as
  // it gets for this target; leave it in place (probe atlases are never cleared
  // in path-traced mode).
  if (target.settleFrames > 0 && h.frame >= target.settleFrames) return;
  bool first = h.frame == 0;
  d.params.x = (f32)h.frame;
  if (h.frame < kMaxAccumulatedFrames) h.frame++;
  u32 offset = r_.pushUniform(&d, sizeof(d));

  RenderPassDesc pass;
  pass.name = "pathtrace";
  pass.colorCount = 2;
  pass.color[0].texture = target.color;
  pass.color[1].texture = target.normalDepth;
  // Load, never clear: a clear would wipe the whole texture, and probe atlases
  // hold six faces with separate histories. The first frame overwrites instead.
  pass.color[0].load = pass.color[1].load = LoadOp::Load;
  cmd.beginRenderPass(pass);
  const vec4& v = target.viewport;
  cmd.setViewport(v.x, v.y, v.z, v.w);
  cmd.setScissor((i32)v.x, (i32)v.y, (u32)v.z, (u32)v.w);
  cmd.setPipeline(pipelines_[target.capture ? 1 : 0][first ? 1 : 0]);
  cmd.setBindGroup(0, group_, {&offset, 1});
  cmd.draw(3);
  cmd.endRenderPass();
}

} // namespace ace
