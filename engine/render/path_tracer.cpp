#include "render/path_tracer.h"
#include "render/crowd.h"
#include <algorithm>
#include <SDL3/SDL.h>
#include <glm/gtx/component_wise.hpp>
#include <map>

namespace ace {

using namespace rhi;

namespace {
struct TraceData {
  mat4 invViewProj, view;
  vec4 lightDir, lightColor, params, lightInfo, ramp;
  vec4 pointInfo, history;
  vec4 pointPosition[8], pointColor[8];
  mat4 prevViewProj, prevView;
  vec4 viewport, reproject;
  Atmosphere atmosphere;
};
constexpr u32 kAtlasSize = 1024;
constexpr u32 kMovingHistoryFrames = 6; // after a scene change, every pixel keeps at most this many frames
constexpr u32 kMaxAccumulatedFrames = 512; // RGBA16F stops gaining precision beyond this
// While a crowd animates: still pixels average at most this many frames (bounce
// light and shadows from people change slowly), pixels a moving person may
// cover keep this many (pathtrace.slang, the end of fsMain).
constexpr f32 kCrowdStillHistory = 32.0f, kCrowdMovingHistory = 4.0f, kCrowdReflectedHistory = 8.0f; // poses hold 5 frames at 60 fps (12 fps clips)

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
  BindGroupLayoutDesc rl;
  rl.bindings[0] = rl.bindings[1] = BindingType::Texture; // last frame's light and normal/depth
  reprojectLayout_ = dev.createBindGroupLayout(rl);
  p.bindGroupLayouts[0] = layout_;
  p.bindGroupLayouts[1] = r_.crowd().traceLayout();
  p.bindGroupLayouts[2] = reprojectLayout_;
  p.cull = CullMode::None;
  p.colorCount = 4; // light (rgb, a = frames averaged), normal/depth, surface color, emission
  p.colorFormats[0] = p.colorFormats[2] = p.colorFormats[3] = Format::RGBA16F;
  // No blending: the shader averages with the reprojected history itself, and
  // overwriting means nothing left in a new target (even NaN) can leak in.
  for (int capture = 0; capture < 2; capture++) {
    p.colorFormats[1] = capture ? Format::RGBA32F : Format::RGBA16F;
    pipelines_[capture] = dev.createPipeline(p);
    ACE_ASSERT(pipelines_[capture], "failed to create path tracing pipeline");
  }
  BindGroupLayoutDesc cpl;
  cpl.bindings[0] = BindingType::Texture;
  copyLayout_ = dev.createBindGroupLayout(cpl);
  PipelineDesc cpy;
  cpy.name = "pt copy normal/depth";
  cpy.vertexShader = r_.loadShader("pt_copy", ShaderStage::Vertex);
  cpy.fragmentShader = r_.loadShader("pt_copy", ShaderStage::Fragment);
  cpy.bindGroupLayouts[0] = copyLayout_;
  cpy.cull = CullMode::None;
  for (int capture = 0; capture < 2; capture++) {
    cpy.colorFormats[0] = capture ? Format::RGBA32F : Format::RGBA16F;
    copyPipelines_[capture] = dev.createPipeline(cpy);
    ACE_ASSERT(copyPipelines_[capture], "failed to create the normal/depth copy pipeline");
  }
  atlas_ = dev.createTexture({kAtlasSize, kAtlasSize, Format::RGBA8_SRGB, TextureUsage_Sampled, "path tracer atlas"});

  BindGroupLayoutDesc dl;
  dl.bindings[0] = BindingType::UniformBuffer;
  dl.bindings[1] = dl.bindings[2] = BindingType::Texture; // light (a: frames averaged), normal/depth
  denoiseLayout_ = dev.createBindGroupLayout(dl);
  BindGroupLayoutDesc cl;
  cl.bindings[0] = BindingType::UniformBuffer;
  for (int b = 1; b <= 5; b++) cl.bindings[b] = BindingType::Texture;
  composeLayout_ = dev.createBindGroupLayout(cl);
  PipelineDesc dp;
  dp.name = "pt denoise";
  dp.vertexShader = r_.loadShader("pt_denoise", ShaderStage::Vertex);
  dp.fragmentShader = r_.loadShader("pt_denoise", ShaderStage::Fragment);
  dp.bindGroupLayouts[0] = denoiseLayout_;
  dp.colorFormats[0] = Format::RGBA16F;
  dp.cull = CullMode::None;
  denoisePipeline_ = dev.createPipeline(dp);
  dp.name = "pt compose";
  dp.vertexShader = r_.loadShader("pt_compose", ShaderStage::Vertex);
  dp.fragmentShader = r_.loadShader("pt_compose", ShaderStage::Fragment);
  dp.bindGroupLayouts[0] = composeLayout_;
  composePipeline_ = dev.createPipeline(dp);
  ACE_ASSERT(denoisePipeline_ && composePipeline_, "failed to create the denoising pipelines");
}

PathTracer::~PathTracer() {
  for (auto& [id, b] : buffers_) destroy(b);
}

void PathTracer::destroy(Buffers& b) {
  Device& dev = r_.device();
  for (int p = 0; p < 2; p++) {
    for (BindGroupHandle g : b.denoise[p]) dev.destroy(g);
    dev.destroy(b.compose[p]);
    dev.destroy(b.reproject[p]);
  }
  dev.destroy(b.copyNormalDepth);
  for (TextureHandle t : {b.light[0], b.light[1], b.prevNormalDepth, b.albedo, b.emission, b.scratch[0], b.scratch[1]}) dev.destroy(t);
  b = Buffers{};
}

namespace {
struct DenoiseData {
  vec4 rect, params;
};
struct ComposeData {
  vec4 rect, ramp;
};
constexpr int kDenoisePasses = 3; // tap spacing 1, 2, 4
} // namespace

PathTracer::Buffers& PathTracer::buffers(const Target& target, bool denoise, TextureHandle palette) {
  Device& dev = r_.device();
  Buffers& b = buffers_[target.color.id];
  if (b.size != target.textureSize || b.normalDepthId != target.normalDepth.id) {
    if (b.albedo) {
      dev.waitIdle();
      destroy(b);
    }
    b.size = target.textureSize;
    b.normalDepthId = target.normalDepth.id;
    u32 w = (u32)target.textureSize.x, h = (u32)target.textureSize.y, rt = TextureUsage_RenderTarget | TextureUsage_Sampled;
    b.light[0] = dev.createTexture({w, h, Format::RGBA16F, rt, "pt light a"});
    b.light[1] = dev.createTexture({w, h, Format::RGBA16F, rt, "pt light b"});
    b.prevNormalDepth = dev.createTexture({w, h, target.capture ? Format::RGBA32F : Format::RGBA16F, rt, "pt previous normal/depth"});
    b.albedo = dev.createTexture({w, h, Format::RGBA16F, rt, "pt surface color"});
    b.emission = dev.createTexture({w, h, Format::RGBA16F, rt, "pt emission"});
    b.scratch[0] = dev.createTexture({w, h, Format::RGBA16F, rt, "pt denoise a"});
    b.scratch[1] = dev.createTexture({w, h, Format::RGBA16F, rt, "pt denoise b"});
    for (int p = 0; p < 2; p++) {
      TextureHandle inputs[3] = {b.light[p], b.scratch[0], b.scratch[1]};
      for (int k = 0; k < 3; k++) {
        BindGroupDesc g;
        g.layout = denoiseLayout_;
        g.entries[0].buffer = r_.uniformBuffer();
        g.entries[0].size = sizeof(DenoiseData);
        g.entries[1].texture = inputs[k];
        g.entries[2].texture = target.normalDepth;
        b.denoise[p][k] = dev.createBindGroup(g);
      }
      BindGroupDesc rg;
      rg.layout = reprojectLayout_;
      rg.entries[0].texture = b.light[1 - p];
      rg.entries[1].texture = b.prevNormalDepth;
      b.reproject[p] = dev.createBindGroup(rg);
    }
    BindGroupDesc cg;
    cg.layout = copyLayout_;
    cg.entries[0].texture = target.normalDepth;
    b.copyNormalDepth = dev.createBindGroup(cg);
  }
  // Pass k writes scratch[k % 2]; compose reads the last one (or the raw light).
  u32 input = denoise ? (u32)((kDenoisePasses - 1) % 2) + 1 : 0;
  if (!b.compose[0] || b.composeInput != input || b.composePalette != palette.id) {
    for (int p = 0; p < 2; p++) {
      if (b.compose[p]) dev.destroy(b.compose[p]);
      TextureHandle inputs[3] = {b.light[p], b.scratch[0], b.scratch[1]};
      BindGroupDesc g;
      g.layout = composeLayout_;
      g.entries[0].buffer = r_.uniformBuffer();
      g.entries[0].size = sizeof(ComposeData);
      g.entries[1].texture = inputs[input];
      g.entries[2].texture = b.albedo;
      g.entries[3].texture = b.emission;
      g.entries[4].texture = palette;
      g.entries[5].texture = target.normalDepth;
      b.compose[p] = dev.createBindGroup(g);
    }
    b.composeInput = input;
    b.composePalette = palette.id;
  }
  return b;
}

// Binned surface-area heuristic: split where (area x triangles) of the two
// halves is cheapest, trying 16 positions on each axis.
// shortcut: a full rebuild whenever anything moves; upgrade to a two-level BVH
// (per-mesh BLAS + refit TLAS) when moving objects in big scenes matter.
static f32 area(const Aabb& b) {
  vec3 e = b.max - b.min;
  return e.x * e.y + e.y * e.z + e.z * e.x;
}
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
    constexpr int kBins = 16;
    int bestAxis = -1, bestBin = 0;
    f32 bestCost = area(box) * (f32)r.count; // cost of staying a leaf
    for (int axis = 0; axis < 3 && r.count > 2; axis++) {
      if (extent[axis] <= 0.0f) continue;
      Aabb bins[kBins];
      u32 counts[kBins] = {};
      f32 scale = (f32)kBins / extent[axis];
      for (u32 i = r.first; i < r.first + r.count; i++) {
        const Triangle& t = tris[i];
        int b = std::min(kBins - 1, (int)((centroid(t)[axis] - cbox.min[axis]) * scale));
        counts[b]++;
        bins[b].grow(vec3(t.v0));
        bins[b].grow(vec3(t.v0 + t.e1));
        bins[b].grow(vec3(t.v0 + t.e2));
      }
      f32 rightCost[kBins] = {};
      Aabb acc;
      u32 n = 0;
      for (int b = kBins - 1; b > 0; b--) { // right side: bins b..end
        n += counts[b];
        if (counts[b]) acc.grow(bins[b].min), acc.grow(bins[b].max);
        rightCost[b] = n ? area(acc) * (f32)n : 0.0f;
      }
      acc = Aabb();
      n = 0;
      for (int b = 1; b < kBins; b++) { // split between bin b-1 and b
        n += counts[b - 1];
        if (counts[b - 1]) acc.grow(bins[b - 1].min), acc.grow(bins[b - 1].max);
        f32 cost = (n ? area(acc) * (f32)n : 0.0f) + rightCost[b] + area(box) * 0.5f; // + traversal cost
        if (n > 0 && n < r.count && cost < bestCost) {
          bestCost = cost;
          bestAxis = axis;
          bestBin = b;
        }
      }
    }
    if (bestAxis < 0 && r.count <= 4) { // a leaf is cheapest
      nodes[r.node].leftFirst = r.first;
      nodes[r.node].count = r.count;
      continue;
    }
    u32 mid;
    if (bestAxis >= 0) {
      // Same bin formula as above, so both sides are non-empty.
      f32 scale = (f32)kBins / extent[bestAxis];
      auto it = std::partition(tris.begin() + r.first, tris.begin() + r.first + r.count, [&](const Triangle& t) {
        return std::min(kBins - 1, (int)((centroid(t)[bestAxis] - cbox.min[bestAxis]) * scale)) < bestBin;
      });
      mid = (u32)(it - (tris.begin() + r.first));
    } else { // many triangles with the same centroid range: split in half anyway
      int axis = extent.x > extent.y ? (extent.x > extent.z ? 0 : 2) : (extent.y > extent.z ? 1 : 2);
      mid = r.count / 2;
      std::nth_element(tris.begin() + r.first, tris.begin() + r.first + mid, tris.begin() + r.first + r.count,
                       [&](const Triangle& a, const Triangle& b) { return centroid(a)[axis] < centroid(b)[axis]; });
    }
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
  auto addMesh = [&](const Mesh* mesh, const mat4& world, vec4 color, vec3 emissive, vec4 atlasRect, int flags, vec4 uvRect,
                     f32 reflectivity = 0.0f, f32 roughness = 0.0f) {
    mat3 nm = glm::transpose(glm::inverse(mat3(world)));
    if (atlasRect.z > 0.0f) flags |= 4;
    for (size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
      const Vertex& a = mesh->vertices[mesh->indices[i]];
      const Vertex& b = mesh->vertices[mesh->indices[i + 1]];
      const Vertex& c = mesh->vertices[mesh->indices[i + 2]];
      vec3 p0 = vec3(world * vec4(a.position, 1)), p1 = vec3(world * vec4(b.position, 1)), p2 = vec3(world * vec4(c.position, 1));
      // Each corner's sRGB vertex color rides in the spare w of v0/e1/e2 as
      // r + 256 g + 65536 b (exact in a float); the shader interpolates them
      // like the raster path does. One color per triangle showed every terrain
      // triangle as a flat patch ("squares") in path-traced images.
      auto rgb = [](const Vertex& v) { return (f32)v.color[0] + 256.0f * v.color[1] + 65536.0f * v.color[2]; };
      Triangle tri;
      tri.v0 = vec4(p0, rgb(a));
      tri.e1 = vec4(p1 - p0, rgb(b));
      tri.e2 = vec4(p2 - p0, rgb(c));
      tri.n0 = vec4(glm::normalize(nm * a.normal), 0);
      tri.n1 = vec4(glm::normalize(nm * b.normal), 0);
      tri.n2 = vec4(glm::normalize(nm * c.normal), 0);
      tri.color = toLinear(color); // the material color; vertex alpha is unused here (cutouts come from textures)
      tri.emissive = vec4(toLinear(emissive), glm::clamp(reflectivity, 0.0f, 1.0f));
      auto uv = [&](const Vertex& v) { return v.uv * vec2(uvRect.z, uvRect.w) + vec2(uvRect.x, uvRect.y); };
      tri.uv01 = vec4(uv(a), uv(b));
      tri.uv2Flags = vec4(uv(c), (f32)flags, std::max(roughness, 0.0f));
      tri.atlas = atlasRect;
      tris.push_back(tri);
    }
  };
  for (auto [e, t, m] : scene.reg.view<Transform, MeshRenderer>().each())
    addMesh(assets.mesh(m.mesh), t.world, m.color, m.emissive, place(assets.texture(m.texture)), m.worldTexels ? 1 : 0, vec4(0, 0, 1, 1),
            m.reflectivity, m.roughness);
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

  // Emissive triangles, by their final (post-BVH) index, for direct light
  // sampling. Each is picked with probability proportional to its power (area x
  // brightness): with uniform picking, a few big flood panels next to a thousand
  // tiny scoreboard pixels got almost no samples, and the image was noisy.
  // The shader searches the cumulative probabilities (z) and divides by w.
  std::vector<vec4> lights;
  f64 totalPower = 0.0;
  for (u32 i = 0; i < tris.size(); i++) {
    f32 area = 0.5f * glm::length(glm::cross(vec3(tris[i].e1), vec3(tris[i].e2)));
    f32 brightness = glm::dot(vec3(tris[i].emissive), vec3(0.2126f, 0.7152f, 0.0722f));
    if (glm::compMax(vec3(tris[i].emissive)) <= 0.0f || area <= 0.0f) continue;
    vec4 entry(0.0f, area, 0.0f, area * std::max(brightness, 1e-6f));
    memcpy(&entry.x, &i, sizeof(u32));
    lights.push_back(entry);
    totalPower += entry.w;
  }
  f64 running = 0.0;
  for (vec4& l : lights) {
    running += l.w;
    l.w = (f32)(l.w / totalPower);
    l.z = (f32)(running / totalPower);
  }
  if (!lights.empty()) lights.back().z = 1.0f; // rounding must not leave a gap at the end
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
  if (target.skipSprites) {
    // Primary rays ignore sprites here (they are drawn directly), so sprite cards
    // only cast shadows: face them towards the sun. Then turning the camera does
    // not change the scene, rebuild the BVH and reset every probe face's average.
    vec3 sun, sunColor;
    findSun(scene, sun, sunColor);
    cameraRight = fabsf(sun.y) < 0.999f ? glm::cross(vec3(0, 1, 0), sun) : vec3(1, 0, 0);
    cameraPosition = sun * 10000.0f;
  }
  u64 sh = 14695981039346656037ull;
  for (auto [e, t, m] : scene.reg.view<Transform, MeshRenderer>().each()) {
    sh = hashBytes(sh, &t.world, sizeof(mat4));
    sh = hashBytes(sh, m.mesh.data(), m.mesh.size());
    sh = hashBytes(sh, m.texture.data(), m.texture.size());
    sh = hashBytes(sh, &m.color, sizeof(vec4));
    sh = hashBytes(sh, &m.emissive, sizeof(vec3));
    sh = hashBytes(sh, &m.worldTexels, sizeof(bool));
    sh = hashBytes(sh, &m.reflectivity, sizeof(f32));
    sh = hashBytes(sh, &m.roughness, sizeof(f32));
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
    // little ghosting for far less noise. Every pixel's frame count is cut
    // once (pathtrace.slang reads `clamp`).
    // shortcut: no per-object motion vectors; a moved mesh ghosts for a few frames.
    for (auto& [slot, h] : history_) {
      h.frame = std::min(h.frame, kMovingHistoryFrames);
      h.clamp = true;
    }
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

  TraceData d{};
  d.invViewProj = glm::inverse(proj * view);
  d.view = view;
  d.lightDir = vec4(glm::normalize(vec3(0.4f, 1.0f, 0.3f)), 0);
  d.lightColor = vec4(0);
  // The sun is the first non-point light; point lights: the 8 nearest the camera.
  bool haveSun = false;
  std::vector<std::pair<f32, int>> lamps;
  std::vector<Entity> lampEntities;
  for (auto [e, t, l] : scene.reg.view<Transform, Light>().each()) {
    if (l.point) {
      lamps.push_back({glm::distance(vec3(t.world[3]), cameraPosition), (int)lampEntities.size()});
      lampEntities.push_back(e);
    } else if (!haveSun) {
      d.lightDir = vec4(glm::normalize(vec3(t.world[2])), 0);
      d.lightColor = vec4(toLinear(l.color) * l.intensity, 0);
      haveSun = true;
    }
  }
  std::sort(lamps.begin(), lamps.end());
  int lampCount = (int)std::min<size_t>(lamps.size(), 8);
  for (int i = 0; i < lampCount; i++) {
    Entity e = lampEntities[lamps[i].second];
    const Light& l = scene.reg.get<Light>(e);
    d.pointPosition[i] = vec4(vec3(scene.reg.get<Transform>(e).world[3]), std::max(l.range, 0.01f));
    d.pointColor[i] = vec4(toLinear(l.color) * l.intensity, 0);
  }
  d.pointInfo = vec4((f32)lampCount, 0, 0, 0);
  CrowdRenderer& crowd = r_.crowd();
  d.history = crowd.active() ? vec4(kCrowdStillHistory, kCrowdMovingHistory, kCrowdReflectedHistory, 0) : vec4(0.0f);
  // Clouds frozen at time 0: their slow drift would change this data every
  // frame and restart the accumulation; it is invisible in the bounce light.
  d.atmosphere = makeAtmosphere(scene, 0.0);
  d.params = vec4(0, (f32)(target.samples > 0 ? target.samples : scene.settings.ptSamples), (f32)scene.settings.ptBounces, (f32)scene.settings.toonBands);

  d.lightInfo = vec4((f32)lightCount_, (f32)std::max(scene.settings.texelsPerUnit, 1), scene.settings.texelLighting ? 1.0f : 0.0f,
                     target.skipSprites ? 1.0f : 0.0f);
  // Ramp shading needs to know what "fully lit" is. Fixed, as in the raster
  // path: a white surface facing an intensity-3 white light (0.954), plus what a
  // typical daytime sky adds by bouncing (~0.25). A dim night then lands on the
  // dark end of each ramp instead of being stretched to full brightness.
  f32 fullyLit = 3.0f * 0.318f + 0.25f;
  bool ramps = scene.settings.rampShading && !scene.settings.palette.empty();
  d.ramp = vec4(ramps ? 1.0f : 0.0f, (f32)glm::clamp(scene.settings.rampShadowSteps, 0, 15), fullyLit, 0);

  // Anything else that changes the image restarts the history. The camera is
  // left out: a moved camera reprojects last frame's light instead.
  TraceData keyData = d;
  keyData.invViewProj = mat4(0.0f);
  keyData.view = mat4(0.0f);
  u64 vh = hashBytes(14695981039346656037ull, &keyData, sizeof(keyData));
  bool denoise = scene.settings.denoise;
  u32 key[5] = {target.color.id, target.normalDepth.id, target.capture ? 1u : 0u, palette->handle.id, denoise ? 1u : 0u};
  vh = hashBytes(vh, key, sizeof(key));
  vh = hashBytes(vh, &target.viewport, sizeof(vec4));
  u64 crowdVersion = crowd.active() ? crowd.version() : 0; // the people's poses are not hashed: they move every frame by design
  vh = hashBytes(vh, &crowdVersion, sizeof(crowdVersion));
  History& h = history_[target.slot];
  // The history lives in shared targets. If this slot was not traced the
  // previous frame, another mode drew there, or the targets were recreated
  // (possibly with the same handle ids): the history is not ours.
  bool continuous = vh == h.viewHash && r_.frameIndex() == h.lastFrame + 1;
  if (!continuous) h.frame = 0;
  h.viewHash = vh;
  h.lastFrame = r_.frameIndex();
  // Settled: nothing changed for settleFrames frames, so the image is as good as
  // it gets for this target; leave it in place (probe atlases are never cleared
  // in path-traced mode). A moved camera or moving people never settle.
  mat4 viewProj = proj * view;
  bool still = continuous && viewProj == h.prevViewProj;
  h.stillFrames = still ? h.stillFrames : 0;
  if (target.settleFrames > 0 && h.stillFrames >= target.settleFrames && !crowd.active()) return;
  if (h.stillFrames < kMaxAccumulatedFrames) h.stillFrames++;
  d.params.x = (f32)h.frame;
  if (h.frame < kMaxAccumulatedFrames) h.frame++;
  d.prevViewProj = h.prevViewProj;
  d.prevView = h.prevView;
  d.viewport = target.viewport;
  d.reproject = vec4(continuous ? 1.0f : 0.0f, h.clamp ? (f32)kMovingHistoryFrames : 0.0f, (f32)(kMaxAccumulatedFrames - 1), 0);
  h.clamp = false;
  u32 parity = h.parity;
  h.parity ^= 1u;
  h.prevViewProj = viewProj;
  h.prevView = view;
  u32 offset = r_.pushUniform(&d, sizeof(d));
  Buffers& buf = buffers(target, denoise, palette->handle);
  const vec4& v = target.viewport;
  auto region = [&] {
    cmd.setViewport(v.x, v.y, v.z, v.w);
    cmd.setScissor((i32)v.x, (i32)v.y, (u32)v.z, (u32)v.w);
  };

  // 1. trace: light averaged with last frame's (reprojected), normal/depth, surface color, emission
  RenderPassDesc pass;
  pass.name = "pathtrace";
  pass.colorCount = 4;
  pass.color[0].texture = buf.light[parity];
  pass.color[1].texture = target.normalDepth;
  pass.color[2].texture = buf.albedo;
  pass.color[3].texture = buf.emission;
  // Load, never clear: a clear would wipe the whole texture, and probe atlases
  // hold six faces with separate histories. Every traced pixel is overwritten.
  for (int k = 0; k < 4; k++) pass.color[k].load = LoadOp::Load;
  cmd.beginRenderPass(pass);
  region();
  cmd.setPipeline(pipelines_[target.capture ? 1 : 0]);
  cmd.setBindGroup(0, group_, {&offset, 1});
  crowd.bindTrace(cmd);
  cmd.setBindGroup(2, buf.reproject[parity]);
  cmd.draw(3);
  cmd.endRenderPass();

  // Keep this frame's normal/depth for the next frame's disocclusion test.
  RenderPassDesc cp0;
  cp0.name = "pt keep normal/depth";
  cp0.color[0].texture = buf.prevNormalDepth;
  cp0.color[0].load = LoadOp::Load;
  cmd.beginRenderPass(cp0);
  region();
  cmd.setPipeline(copyPipelines_[target.capture ? 1 : 0]);
  cmd.setBindGroup(0, buf.copyNormalDepth);
  cmd.draw(3);
  cmd.endRenderPass();

  // 2. denoise the light: a-trous passes with growing tap spacing. The
  // brightness tolerance shrinks as samples accumulate, so the blur fades out
  // as the image converges and shadow edges come back sharp.
  if (denoise) {
    // Divided per pixel by sqrt(frames averaged) in the shader.
    f32 tolerance = 0.4f * d.ramp.z;
    for (int k = 0; k < kDenoisePasses; k++) {
      DenoiseData dd{v, vec4((f32)(1 << k), tolerance, 0, 0)};
      u32 dOffset = r_.pushUniform(&dd, sizeof(dd));
      RenderPassDesc dp;
      dp.name = "pt denoise";
      dp.color[0].texture = buf.scratch[k % 2];
      dp.color[0].load = LoadOp::Load;
      cmd.beginRenderPass(dp);
      region();
      cmd.setPipeline(denoisePipeline_);
      cmd.setBindGroup(0, buf.denoise[parity][k == 0 ? 0 : 1 + (k - 1) % 2], {&dOffset, 1});
      cmd.draw(3);
      cmd.endRenderPass();
    }
  }

  // 3. compose: surface color x light (or the palette ramp color), plus emission
  ComposeData cd{v, d.ramp};
  u32 cOffset = r_.pushUniform(&cd, sizeof(cd));
  RenderPassDesc cp;
  cp.name = "pt compose";
  cp.color[0].texture = target.color;
  cp.color[0].load = LoadOp::Load;
  cmd.beginRenderPass(cp);
  region();
  cmd.setPipeline(composePipeline_);
  cmd.setBindGroup(0, buf.compose[parity], {&cOffset, 1});
  cmd.draw(3);
  cmd.endRenderPass();
}

} // namespace ace
