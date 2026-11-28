#include "render/crowd.h"
#include <algorithm>


namespace ace {

using namespace rhi;
using json = nlohmann::json;

namespace {
using GpuMember = CrowdRenderer::GpuMember;
struct GpuTri { // CrowdTri in pathtrace.slang
  vec4 v0, e1, e2, n0, n1, n2;
};
struct DrawData { // CrowdDraw in crowd.slang
  vec4 time, counts, events[3], wave, bounds;
  vec4 info; // x: first list entry, y: first vertex of the model, z: vertices per person
};

constexpr f32 kBlendMargin = 0.05f; // metres; see the bounds in parsePack

// A character-space box placed in the world for one person (yaw, scale, position).
Aabb placeBox(const Aabb& box, const GpuMember& m) {
  Aabb b;
  for (int c = 0; c < 8; c++) {
    vec3 p = vec3(c & 1 ? box.max.x : box.min.x, c & 2 ? box.max.y : box.min.y, c & 4 ? box.max.z : box.min.z) * m.posScale.w;
    b.grow(vec3(m.facing.x * p.x + m.facing.y * p.z, p.y, -m.facing.y * p.x + m.facing.x * p.z) + vec3(m.posScale));
  }
  return b;
}

f32 boxArea(const Aabb& b) {
  vec3 e = b.max - b.min;
  return e.x * e.y + e.y * e.z + e.z * e.x;
}

u32 packRgb(vec3 srgb) {
  auto c = [](f32 v) { return (u32)(glm::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
  return (c(srgb.r) << 16) | (c(srgb.g) << 8) | c(srgb.b);
}

} // namespace

void CrowdRenderer::buildBoxBvh(const std::vector<Aabb>& boxes, std::vector<u32>& order, std::vector<Node>& nodes, u32 maxLeaf) {
  nodes.clear();
  order.resize(boxes.size());
  for (u32 i = 0; i < order.size(); i++) order[i] = i;
  nodes.push_back(Node{vec3(1e30f), 0, vec3(-1e30f), 0}); // empty root: every ray misses it
  if (boxes.empty()) return;
  auto centre = [&](u32 i) { return (boxes[i].min + boxes[i].max) * 0.5f; };
  struct Range {
    u32 node, first, count;
  };
  std::vector<Range> todo{{0, 0, (u32)boxes.size()}};
  while (!todo.empty()) {
    Range r = todo.back();
    todo.pop_back();
    Aabb box, cbox;
    for (u32 i = r.first; i < r.first + r.count; i++) {
      box.grow(boxes[order[i]].min), box.grow(boxes[order[i]].max);
      cbox.grow(centre(order[i]));
    }
    nodes[r.node].min = box.min;
    nodes[r.node].max = box.max;
    if (r.count <= maxLeaf) {
      nodes[r.node].leftFirst = r.first;
      nodes[r.node].count = r.count;
      continue;
    }
    // Binned SAH, as in PathTracer::buildBvh; above maxLeaf a split is forced.
    constexpr int kBins = 16;
    vec3 extent = cbox.max - cbox.min;
    int bestAxis = -1, bestBin = 0;
    f32 bestCost = 1e30f;
    for (int axis = 0; axis < 3; axis++) {
      if (extent[axis] <= 0.0f) continue;
      Aabb bins[kBins];
      u32 counts[kBins] = {};
      f32 scale = (f32)kBins / extent[axis];
      for (u32 i = r.first; i < r.first + r.count; i++) {
        int b = std::min(kBins - 1, (int)((centre(order[i])[axis] - cbox.min[axis]) * scale));
        counts[b]++;
        bins[b].grow(boxes[order[i]].min), bins[b].grow(boxes[order[i]].max);
      }
      f32 rightCost[kBins] = {};
      Aabb acc;
      u32 n = 0;
      for (int b = kBins - 1; b > 0; b--) {
        n += counts[b];
        if (counts[b]) acc.grow(bins[b].min), acc.grow(bins[b].max);
        rightCost[b] = n ? boxArea(acc) * (f32)n : 0.0f;
      }
      acc = Aabb();
      n = 0;
      for (int b = 1; b < kBins; b++) {
        n += counts[b - 1];
        if (counts[b - 1]) acc.grow(bins[b - 1].min), acc.grow(bins[b - 1].max);
        f32 cost = boxArea(acc) * (f32)n + rightCost[b];
        if (n > 0 && n < r.count && cost < bestCost) {
          bestCost = cost;
          bestAxis = axis;
          bestBin = b;
        }
      }
    }
    u32 mid;
    if (bestAxis >= 0) {
      f32 scale = (f32)kBins / extent[bestAxis];
      auto it = std::partition(order.begin() + r.first, order.begin() + r.first + r.count, [&](u32 i) {
        return std::min(kBins - 1, (int)((centre(i)[bestAxis] - cbox.min[bestAxis]) * scale)) < bestBin;
      });
      mid = (u32)(it - (order.begin() + r.first));
    } else { // identical centres: split in half
      mid = r.count / 2;
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

// A member array by reference (json::value() would copy the whole subtree).
static const json& arrayField(const json& o, const char* key) {
  static const json empty = json::array();
  auto it = o.find(key);
  return it != o.end() && it->is_array() ? *it : empty;
}

bool CrowdRenderer::parsePack(const json& j, Pack& pack, std::string& error) {
  pack = Pack{};
  if (!j.is_object() || j.value("bones", 0) != (int)kBones) {
    error = "expected \"bones\": " + std::to_string(kBones);
    return false;
  }
  pack.fps = (u32)std::max(1, j.value("fps", 12));
  const json& clips = arrayField(j, "clips");
  for (const json& c : clips) {
    pack.clipNames.push_back(c.value("name", std::string()));
    pack.clipLoops.push_back(c.value("loop", true));
  }
  const json& models = arrayField(j, "models");
  const json& poses = arrayField(j, "poses");
  if (pack.clipNames.empty() || models.empty() || poses.size() != models.size()) {
    error = "needs clips, models and one pose list per model";
    return false;
  }
  for (size_t mi = 0; mi < models.size(); mi++) {
    Pack::Model model;
    model.name = models[mi].value("name", std::string());
    for (const json& jp : arrayField(models[mi], "parts")) {
      Pack::Part part;
      part.bone = (u32)jp.value("bone", 0);
      auto pos = jp.value("positions", std::vector<f32>());
      auto nrm = jp.value("normals", std::vector<f32>());
      part.colors = jp.value("colors", std::vector<f32>());
      if (part.bone >= kBones || pos.size() % 9 != 0 || nrm.size() != pos.size() || part.colors.size() * 9 != pos.size()) {
        error = "model " + model.name + ": a part has a bad bone or mismatched array sizes";
        return false;
      }
      for (size_t k = 0; k < pos.size(); k += 3) {
        part.positions.push_back(vec3(pos[k], pos[k + 1], pos[k + 2]));
        part.normals.push_back(vec3(nrm[k], nrm[k + 1], nrm[k + 2]));
      }
      model.parts.push_back(std::move(part));
    }
    const json& mp = poses[mi];
    if (!mp.is_array() || mp.size() != pack.clipNames.size()) {
      error = "model " + model.name + ": needs one pose list per clip";
      return false;
    }
    for (const json& clip : mp) {
      std::vector<std::vector<Pose>> frames;
      for (const json& frame : clip) {
        if (!frame.is_array() || frame.size() != kBones) {
          error = "model " + model.name + ": every frame needs one transform per bone";
          return false;
        }
        std::vector<Pose> bones;
        for (const json& b : frame) {
          if (!b.is_array() || b.size() != 7) {
            error = "model " + model.name + ": a bone transform is not [qx,qy,qz,qw,tx,ty,tz]";
            return false;
          }
          Pose p;
          p.q = glm::normalize(quat(b[3].get<f32>(), b[0].get<f32>(), b[1].get<f32>(), b[2].get<f32>()));
          p.t = vec3(b[4].get<f32>(), b[5].get<f32>(), b[6].get<f32>());
          bones.push_back(p);
        }
        frames.push_back(std::move(bones));
      }
      if (frames.empty()) {
        error = "model " + model.name + ": a clip has no frames";
        return false;
      }
      model.clips.push_back(std::move(frames));
    }
    // Pose-conservative bounds, per pair of clips (a, b): every frame of a and
    // of b, and every blend between them (a person only ever blends its idle
    // clip with one override). Frames are held, never interpolated, so (a, a)
    // is exact. A blend moves a point along an arc, so pairs of frames are
    // blended at every 1/8; between those samples the arc strays less than
    // kBlendMargin (1/4 steps were not enough: a hand 1.9 m from the feet
    // strayed ~14 cm, caught by testCrowd). Seated people never leave these boxes, so trees built
    // over them stay valid while everyone animates.
    const size_t clipCount = model.clips.size();
    model.pairPartBounds.assign(clipCount * clipCount * kBones, Aabb{});
    model.pairBounds.assign(clipCount * clipCount, Aabb{});
    model.frameBounds.resize(clipCount);
    model.framePartBounds.resize(clipCount);
    for (size_t a = 0; a < clipCount; a++) {
      model.frameBounds[a].assign(model.clips[a].size(), Aabb{});
      model.framePartBounds[a].assign(model.clips[a].size(), std::vector<Aabb>(kBones));
    }
    for (u32 bone = 0; bone < kBones; bone++) {
      Aabb rest;
      for (const Pack::Part& part : model.parts)
        if (part.bone == bone)
          for (vec3 p : part.positions) rest.grow(p);
      if (rest.min.x > rest.max.x) continue;
      vec3 centre = (rest.min + rest.max) * 0.5f, extent = (rest.max - rest.min) * 0.5f;
      auto grow = [&](Aabb& box, quat q, vec3 t) { // box around the rotated box: centre' +- |R| extent
        mat3 m = glm::mat3_cast(q);
        vec3 c = m * centre + t, e = glm::abs(m[0]) * extent.x + glm::abs(m[1]) * extent.y + glm::abs(m[2]) * extent.z;
        box.grow(c - e), box.grow(c + e);
      };
      auto part = [&](size_t a, size_t b) -> Aabb& { return model.pairPartBounds[(a * clipCount + b) * kBones + bone]; };
      const auto& clips = model.clips;
      for (size_t a = 0; a < clipCount; a++)
        for (size_t fi = 0; fi < clips[a].size(); fi++) {
          const auto& fa = clips[a][fi];
          grow(model.framePartBounds[a][fi][bone], fa[bone].q, fa[bone].t);
          grow(model.frameBounds[a][fi], fa[bone].q, fa[bone].t);
          for (size_t b = 0; b < clipCount; b++) grow(part(a, b), fa[bone].q, fa[bone].t); // a's frames belong to every pair with a
          for (size_t b = 0; b < clipCount; b++) grow(part(b, a), fa[bone].q, fa[bone].t);
          for (size_t b = a + 1; b < clipCount; b++)
            for (const auto& fb : clips[b]) {
              quat qa = fa[bone].q, qb = glm::dot(qa, fb[bone].q) < 0.0f ? -fb[bone].q : fb[bone].q;
              for (f32 w : {0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f}) {
                quat q = glm::normalize(qa * (1.0f - w) + qb * w);
                vec3 t = glm::mix(fa[bone].t, fb[bone].t, w);
                grow(part(a, b), q, t);
                grow(part(b, a), q, t);
              }
            }
        }
      for (size_t a = 0; a < clipCount; a++)
        for (size_t b = 0; b < clipCount; b++) {
          Aabb& box = part(a, b);
          if (a != b) box.min -= vec3(kBlendMargin), box.max += vec3(kBlendMargin);
          model.pairBounds[a * clipCount + b].grow(box.min), model.pairBounds[a * clipCount + b].grow(box.max);
          model.bounds.grow(box.min), model.bounds.grow(box.max);
        }
    }
    pack.models.push_back(std::move(model));
  }
  return true;
}

CrowdRenderer::CrowdRenderer(Renderer& renderer, BindGroupLayoutHandle frameLayout) : r_(renderer) {
  Device& dev = r_.device();
  BindGroupLayoutDesc t;
  t.bindings[0] = BindingType::UniformBuffer;
  for (int b = 1; b <= 4; b++) t.bindings[b] = BindingType::StorageBuffer;
  traceLayout_ = dev.createBindGroupLayout(t);
  rasterLayout_ = dev.createBindGroupLayout(t); // same shape: uniforms + 4 storage buffers

  PipelineDesc p;
  p.name = "crowd";
  p.vertexShader = r_.loadShader("crowd", ShaderStage::Vertex);
  p.fragmentShader = r_.loadShader("crowd", ShaderStage::Fragment);
  p.bindGroupLayouts[0] = frameLayout;
  p.bindGroupLayouts[1] = rasterLayout_;
  p.colorFormats[0] = p.colorFormats[1] = Format::RGBA16F;
  p.colorCount = 2;
  p.depthFormat = Format::D32F;
  p.cull = CullMode::None; // flags and faces are single sheets
  pipelines_[0] = dev.createPipeline(p);
  p.name = "crowd capture";
  p.colorFormats[1] = Format::RGBA32F;
  pipelines_[1] = dev.createPipeline(p);
  ACE_ASSERT(pipelines_[0] && pipelines_[1], "failed to create the crowd pipelines");

  // Placeholders, so the path tracer can always bind group 1.
  for (BufferHandle* b : {&nodes_, &members_, &table_, &tris_, &verts_, &list_}) *b = dev.createBuffer({256, BufferUsage_Storage, "crowd (empty)"});
  createGroups();
}

CrowdRenderer::~CrowdRenderer() = default;

void CrowdRenderer::createGroups() {
  Device& dev = r_.device();
  if (traceGroup_) dev.destroy(traceGroup_);
  if (rasterGroup_) dev.destroy(rasterGroup_);
  BindGroupDesc g;
  g.layout = traceLayout_;
  g.entries[0].buffer = r_.uniformBuffer();
  g.entries[0].size = sizeof(Data);
  g.entries[1].buffer = nodes_;
  g.entries[2].buffer = members_;
  g.entries[3].buffer = table_;
  g.entries[4].buffer = tris_;
  traceGroup_ = dev.createBindGroup(g);
  BindGroupDesc rg;
  rg.layout = rasterLayout_;
  rg.entries[0].buffer = r_.uniformBuffer();
  rg.entries[0].size = sizeof(DrawData);
  rg.entries[1].buffer = list_;
  rg.entries[2].buffer = members_;
  rg.entries[3].buffer = table_;
  rg.entries[4].buffer = verts_;
  rasterGroup_ = dev.createBindGroup(rg);
}

bool CrowdRenderer::load(Scene& scene, Assets& assets, const Crowd& crowd) {
  std::vector<u8> bytes;
  json packJson, membersJson;
  auto parse = [&](const std::string& path, json& out) {
    bytes.clear();
    if (path.empty() || !readFile(assets.resolve(path), bytes)) {
      ACE_WARN("crowd: cannot read '%s'", path.c_str());
      return false;
    }
    out = json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (out.is_discarded()) ACE_WARN("crowd: '%s' is not valid JSON", path.c_str());
    return !out.is_discarded();
  };
  if (!parse(crowd.pack, packJson) || !parse(crowd.members, membersJson)) return false;

  std::string error;
  if (!parsePack(packJson, pack_, error)) {
    ACE_WARN("crowd: %s: %s", crowd.pack.c_str(), error.c_str());
    return false;
  }
  const u32 modelCount = (u32)pack_.models.size(), clipCount = (u32)pack_.clipNames.size();

  // Members: [x, y, z, yaw deg, scale, model, team, idle clip, speed, phase, wave delay, primary, secondary, accent]
  std::vector<GpuMember> members;
  std::vector<Aabb> boxes;
  for (const json& row : arrayField(membersJson, "members")) {
    if (!row.is_array() || row.size() < 14) continue;
    auto f = [&](int k) { return row[k].is_number() ? row[k].get<f32>() : 0.0f; };
    u32 model = (u32)f(5);
    u32 clip = (u32)f(7);
    if (model >= modelCount || clip >= clipCount) continue; // trust boundary: skip bad rows
    f32 yaw = glm::radians(f(3)), scale = glm::clamp(f(4), 0.1f, 10.0f);
    GpuMember m{};
    m.posScale = vec4(f(0), f(1), f(2), scale);
    m.facing = vec4(cosf(yaw), sinf(yaw), f(10), (f32)(members.size() & 0xFFFFFF));
    m.anim = vec4((f32)clip, f(8), f(9), (f32)model);
    for (int k = 0; k < 3; k++) m.colors[k] = (u32)std::max(f(11 + k), 0.0f) & 0xFFFFFF; // 24-bit values are exact in a float
    m.colors[3] = (u32)glm::clamp((int)f(6), 0, 2);
    members.push_back(m);
    boxes.push_back(placeBox(pack_.models[model].pairBounds[clip * clipCount + clip], m)); // idle clip only
  }
  if (members.empty() && scene.reg.view<CrowdActor>().empty()) {
    ACE_WARN("crowd: '%s' has no valid members", crowd.members.c_str());
    return false;
  }

  // Static top level: one person per leaf, so a leaf's box is that person's
  // box. Built once over the idle-clip boxes; refit (same tree, new boxes)
  // whenever a team event or the wave starts or ends (refitStatic).
  std::vector<u32> order;
  std::vector<Node> nodes;
  buildBoxBvh(boxes, order, nodes, 1);
  std::vector<GpuMember> sorted(members.size());
  for (size_t i = 0; i < order.size(); i++) sorted[i] = members[order[i]];
  staticCount_ = (u32)sorted.size();
  staticMembers_.assign(sorted.begin(), sorted.end());
  staticNodes_ = nodes;
  refitBusy_ = false;

  // Part BVHs in rest pose, one per (model, bone), appended after the top level.
  std::vector<vec4> table((size_t)modelCount * clipCount); // clip headers first
  partHeaderBase_ = (u32)table.size();
  table.resize(table.size() + (size_t)modelCount * kBones, vec4(0.0f)); // per part: x (bits) root node, y 1 = exists
  modelHeaderBase_ = (u32)table.size();
  table.resize(table.size() + modelCount, vec4(0.0f));
  std::vector<GpuTri> tris;
  std::vector<vec4> verts; // raster: per vertex (rest position, bone), (normal, color code), (face normal)
  modelVertexOffset_.assign(modelCount, 0);
  modelVertexCount_.assign(modelCount, 0);
  for (u32 mi = 0; mi < modelCount; mi++) {
    const Pack::Model& model = pack_.models[mi];
    modelVertexOffset_[mi] = (u32)verts.size() / 3;
    for (u32 bone = 0; bone < kBones; bone++) {
      std::vector<GpuTri> boneTris;
      std::vector<Aabb> triBoxes;
      for (const Pack::Part& part : model.parts) {
        if (part.bone != bone) continue;
        for (size_t t = 0; t < part.colors.size(); t++) {
          vec3 a = part.positions[t * 3], b = part.positions[t * 3 + 1], c = part.positions[t * 3 + 2];
          GpuTri g;
          g.v0 = vec4(a, part.colors[t]);
          g.e1 = vec4(b - a, 0);
          g.e2 = vec4(c - a, 0);
          g.n0 = vec4(part.normals[t * 3], 0);
          g.n1 = vec4(part.normals[t * 3 + 1], 0);
          g.n2 = vec4(part.normals[t * 3 + 2], 0);
          boneTris.push_back(g);
          Aabb box;
          box.grow(a), box.grow(b), box.grow(c);
          triBoxes.push_back(box);
          vec3 face = glm::cross(b - a, c - a); // picks the texel grid's axis, as in the path tracer
          for (int k = 0; k < 3; k++) {
            verts.push_back(vec4(part.positions[t * 3 + k], (f32)bone));
            verts.push_back(vec4(part.normals[t * 3 + k], part.colors[t]));
            verts.push_back(vec4(face, 0));
          }
        }
      }
      if (boneTris.empty()) continue;
      std::vector<u32> triOrder;
      std::vector<Node> partNodes;
      buildBoxBvh(triBoxes, triOrder, partNodes, 4);
      u32 nodeBase = (u32)nodes.size(), triBase = (u32)tris.size();
      for (Node n : partNodes) {
        n.leftFirst += n.count ? triBase : nodeBase;
        nodes.push_back(n);
      }
      for (u32 k : triOrder) tris.push_back(boneTris[k]);
      u32 at = partHeaderBase_ + mi * kBones + bone;
      memcpy(&table[at].x, &nodeBase, sizeof(u32));
      table[at].y = 1.0f;
    }
    modelVertexCount_[mi] = (u32)verts.size() / 3 - modelVertexOffset_[mi];
    table[modelHeaderBase_ + mi] = vec4((f32)modelVertexOffset_[mi], (f32)modelVertexCount_[mi], 0, 0);
  }
  // Clip frames: per (model, clip, frame, bone) a quaternion, a translation and the part's box in that frame.
  for (u32 mi = 0; mi < modelCount; mi++) {
    for (u32 ci = 0; ci < clipCount; ci++) {
      const auto& frames = pack_.models[mi].clips[ci];
      table[mi * clipCount + ci] = vec4((f32)table.size(), (f32)frames.size(), pack_.clipLoops[ci] ? 1.0f : 0.0f, 0);
      for (size_t fi = 0; fi < frames.size(); fi++)
        for (u32 bone = 0; bone < kBones; bone++) {
          const Pose& p = frames[fi][bone];
          const Aabb& box = pack_.models[mi].framePartBounds[ci][fi][bone]; // this frame's exact box of the part
          table.push_back(vec4(p.q.x, p.q.y, p.q.z, p.q.w));
          table.push_back(vec4(p.t, 0));
          table.push_back(vec4(box.min, 0));
          table.push_back(vec4(box.max, 0));
        }
    }
  }
  // Per-bone boxes per (model, idle clip, override clip, bone): min, max.
  clipBoxBase_ = (u32)table.size();
  for (u32 mi = 0; mi < modelCount; mi++)
    for (const Aabb& b : pack_.models[mi].pairPartBounds) {
      table.push_back(vec4(b.min, 0));
      table.push_back(vec4(b.max, 0));
    }
  // Room for the moving actors: their members, a small top level and draw-list entries.
  dynamicNodeBase_ = (u32)nodes.size();
  nodes.resize(nodes.size() + 2 * kMaxActors, Node{vec3(1e30f), 0, vec3(-1e30f), 0});
  sorted.resize(sorted.size() + kMaxActors, GpuMember{});

  // Raster draw list: seated people grouped by model (indices into `sorted`).
  std::vector<u32> list;
  staticRanges_.assign(modelCount, {0, 0});
  for (u32 mi = 0; mi < modelCount; mi++) {
    staticRanges_[mi].first = (u32)list.size();
    for (u32 i = 0; i < staticCount_; i++)
      if ((u32)sorted[i].anim.w == mi) list.push_back(i);
    staticRanges_[mi].second = (u32)list.size() - staticRanges_[mi].first;
  }
  listStatic_ = (u32)list.size();
  list.resize(list.size() + kMaxActors, 0);

  Device& dev = r_.device();
  dev.waitIdle();
  auto upload = [&](BufferHandle& buffer, const void* data, u64 bytes, const char* name) {
    dev.destroy(buffer);
    buffer = dev.createBuffer({std::max<u64>(bytes, 256), BufferUsage_Storage, name});
    dev.writeBuffer(buffer, 0, data, bytes);
  };
  upload(nodes_, nodes.data(), nodes.size() * sizeof(Node), "crowd nodes");
  upload(members_, sorted.data(), sorted.size() * sizeof(GpuMember), "crowd members");
  upload(table_, table.data(), table.size() * sizeof(vec4), "crowd clips");
  upload(tris_, tris.data(), tris.size() * sizeof(GpuTri), "crowd triangles");
  upload(verts_, verts.data(), verts.size() * sizeof(vec4), "crowd vertices");
  upload(list_, list.data(), list.size() * sizeof(u32), "crowd draw list");
  createGroups();
  version_++;
  ACE_INFO("crowd: %u people, %u models x %u clips, %zu part triangles, %zu nodes", staticCount_, modelCount, clipCount, tris.size(), nodes.size());
  return true;
}

void CrowdRenderer::update(Scene& scene, Assets& assets, f64 seconds) {
  Crowd* crowd = nullptr;
  for (auto [e, c] : scene.reg.view<Crowd>().each()) {
    crowd = &c;
    break;
  }
  if (!crowd) {
    memberCount_ = 0;
    data_.time.y = 0.0f;
    return;
  }
  if (crowd->pack != packPath_ || crowd->members != membersPath_) {
    packPath_ = crowd->pack;
    membersPath_ = crowd->members;
    staticCount_ = 0;
    if (!load(scene, assets, *crowd)) pack_ = Pack{};
  }
  if (pack_.models.empty()) {
    memberCount_ = 0;
    data_.time.y = 0.0f;
    return;
  }
  const u32 modelCount = (u32)pack_.models.size(), clipCount = (u32)pack_.clipNames.size();
  auto clipIndex = [&](const std::string& name) -> f32 {
    for (u32 i = 0; i < clipCount; i++)
      if (pack_.clipNames[i] == name) return (f32)i;
    return -1.0f;
  };
  for (Crowd::Event& ev : crowd->teams)
    if (ev.pending) {
      ev.start = seconds;
      ev.end = seconds + ev.seconds;
      ev.pending = false;
    }
  if (crowd->wavePending) {
    crowd->waveStart = seconds;
    crowd->wavePending = false;
  }
  f32 speed = std::max(crowd->speed, 0.0f);
  // Seconds can be large (a long session): keep the GPU's float time small
  // relative to the clip lengths by subtracting nothing here, but scale once.
  data_.time = vec4((f32)(seconds * speed), 1.0f, (f32)partHeaderBase_, (f32)modelHeaderBase_);
  data_.counts = vec4((f32)clipCount, -1.0f, (f32)pack_.fps, 0.25f);
  for (int t = 0; t < 3; t++) {
    const Crowd::Event& ev = crowd->teams[t];
    f32 clip = ev.clip.empty() ? -1.0f : clipIndex(ev.clip);
    data_.events[t] = vec4(clip, (f32)(ev.start * speed), (f32)(ev.end * speed), 0);
  }
  f32 waveClip = clipIndex("wave");
  f32 lap = 24.0f; // must match the delays written by the generator
  data_.bounds = vec4(0, 0, (f32)clipBoxBase_, 0);
  data_.wave = crowd->waveStart >= 0.0 && waveClip >= 0.0f ? vec4((f32)(crowd->waveStart * speed), lap, (f32)crowd->waveLaps, waveClip) : vec4(-1.0f, lap, 0, 0);

  // Seated people: while an event or the wave runs, refit their top level
  // every frame to the clips each person can be showing right now; when it
  // is over, once more back to the idle boxes.
  bool busy = false;
  for (int t = 0; t < 3; t++) busy |= data_.events[t].x >= 0.0f && seconds < crowd->teams[t].end + 1.0;
  busy |= data_.wave.x >= 0.0f && seconds < crowd->waveStart + (crowd->waveLaps + 1) * lap + 3.0;
  if (busy || refitBusy_) refitStatic(busy);
  refitBusy_ = busy;

  // Moving actors: rebuilt every frame into their own small top level.
  std::vector<GpuMember> actors;
  std::vector<Aabb> boxes;
  for (auto [e, t, a] : scene.reg.view<Transform, CrowdActor>().each()) {
    if (actors.size() >= kMaxActors) break;
    u32 model = (u32)glm::clamp(a.model, 0, (int)modelCount - 1);
    u32 clip = (u32)glm::clamp(a.clip, 0, (int)clipCount - 1);
    f32 scale = glm::length(vec3(t.world[0]));
    vec3 forward = -vec3(t.world[2]); // the entity faces its local -Z
    vec2 f = glm::length(vec2(forward.x, forward.z)) > 1e-6f ? glm::normalize(vec2(forward.x, forward.z)) : vec2(0, -1);
    GpuMember m{};
    m.posScale = vec4(vec3(t.world[3]), std::max(scale, 0.01f));
    m.facing = vec4(-f.y, -f.x, -1.0f, (f32)(1000000 + actors.size())); // local (0,0,-1) -> (f.x, f.y); never waves
    m.anim = vec4((f32)clip, a.speed, 0.0f, (f32)model);
    m.colors[0] = packRgb(a.primary);
    m.colors[1] = packRgb(a.secondary);
    m.colors[2] = packRgb(a.accent);
    m.colors[3] = (u32)glm::clamp(a.team, 0, 2);
    actors.push_back(m);
    boxes.push_back(placeBox(pack_.models[model].bounds, m)); // rebuilt every frame anyway: always wide
  }
  actorCount_ = (u32)actors.size();
  memberCount_ = staticCount_ + actorCount_;
  actorRanges_.assign(modelCount, {0, 0});
  if (actorCount_ > 0) {
    std::vector<u32> order;
    std::vector<Node> nodes;
    buildBoxBvh(boxes, order, nodes, 1);
    std::vector<GpuMember> sorted(actorCount_);
    for (u32 i = 0; i < actorCount_; i++) sorted[i] = actors[order[i]];
    for (Node& n : nodes) n.leftFirst += n.count ? staticCount_ : dynamicNodeBase_;
    Device& dev = r_.device();
    dev.writeBuffer(nodes_, (u64)dynamicNodeBase_ * sizeof(Node), nodes.data(), nodes.size() * sizeof(Node));
    dev.writeBuffer(members_, (u64)staticCount_ * sizeof(GpuMember), sorted.data(), sorted.size() * sizeof(GpuMember));
    std::vector<u32> list;
    for (u32 mi = 0; mi < modelCount; mi++) {
      actorRanges_[mi].first = listStatic_ + (u32)list.size();
      for (u32 i = 0; i < actorCount_; i++)
        if ((u32)sorted[i].anim.w == mi) list.push_back(staticCount_ + i);
      actorRanges_[mi].second = listStatic_ + (u32)list.size() - actorRanges_[mi].first;
    }
    dev.writeBuffer(list_, (u64)listStatic_ * sizeof(u32), list.data(), list.size() * sizeof(u32));
    data_.counts.y = (f32)dynamicNodeBase_;
  }
}

// Same hash as pcgHash in common.slangh (the reaction delay must match the shader's).
static u32 pcgHash(u32 v) {
  u32 state = v * 747796405u + 2891336453u;
  u32 word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}

// Mirror of crowdFrame in crowd.slangh.
u32 CrowdRenderer::frameAt(u32 clip, u32 frames, f32 seconds) const {
  i32 f = (i32)floorf(seconds * data_.counts.z), n = (i32)frames;
  return (u32)(pack_.clipLoops[clip] ? ((f % n) + n) % n : glm::clamp(f, 0, n - 1));
}

// Mirror of crowdAnim in crowd.slangh: the override clip a person blends in at
// time `t`, its local time and its weight (0 = idle only).
f32 CrowdRenderer::overrideAt(const GpuMember& m, f32 t, u32& clip, f32& clipSeconds) const {
  clip = (u32)m.anim.x;
  clipSeconds = 0.0f;
  f32 fade = data_.counts.w, delay = (f32)(pcgHash((u32)m.facing.w) & 1023u) / 1023.0f * 0.6f;
  const vec4& ev = data_.events[std::min(m.colors[3], 2u)];
  f32 w = 0.0f;
  if (ev.x >= 0.0f) {
    w = glm::clamp((t - ev.y - delay) / fade, 0.0f, 1.0f) * glm::clamp((ev.z + delay - t) / fade, 0.0f, 1.0f);
    clip = (u32)ev.x;
    clipSeconds = (t - ev.y - delay) * m.anim.y;
  }
  if (w <= 0.0f && data_.wave.x >= 0.0f && m.facing.z >= 0.0f) {
    f32 s = t - data_.wave.x - m.facing.z, lap = floorf(s / data_.wave.y);
    const auto& frames = pack_.models[(u32)m.anim.w].clips[(u32)data_.wave.w];
    f32 len = (f32)frames.size() / data_.counts.z, local = s - lap * data_.wave.y;
    if (lap >= 0.0f && lap < data_.wave.z && local < len) {
      w = glm::clamp(local / 0.12f, 0.0f, 1.0f) * glm::clamp((len - local) / 0.12f, 0.0f, 1.0f);
      clip = (u32)data_.wave.w;
      clipSeconds = local;
    }
  }
  return w;
}

void CrowdRenderer::refitStatic(bool busy) {
  if (staticNodes_.empty() || staticMembers_.empty()) return;
  const u32 clipCount = (u32)pack_.clipNames.size();
  const f32 now = data_.time.x, margin = 0.05f; // the GPU computes in float: stay clear of boundaries
  for (size_t k = staticNodes_.size(); k-- > 0;) { // children always come after their parent
    Node& n = staticNodes_[k];
    Aabb b;
    if (n.count) {
      for (u32 i = n.leftFirst; i < n.leftFirst + n.count; i++) {
        const GpuMember& m = staticMembers_[i];
        const Pack::Model& model = pack_.models[(u32)m.anim.w];
        u32 a = (u32)m.anim.x;
        Aabb box;
        auto add = [&](u32 x, u32 y) { box.grow(model.pairBounds[x * clipCount + y].min), box.grow(model.pairBounds[x * clipCount + y].max); };
        if (!busy) {
          add(a, a);
        } else {
          // idle only, fully in the override, or crossfading: sampled around now
          // Held frames have exact boxes; a crossfade takes the pair's box.
          for (f32 t : {now - margin, now, now + margin}) {
            u32 clip;
            f32 local, w = overrideAt(m, t, clip, local);
            auto frameBox = [&](u32 c, f32 seconds) {
              const Aabb& f = model.frameBounds[c][frameAt(c, (u32)model.clips[c].size(), seconds)];
              box.grow(f.min), box.grow(f.max);
            };
            if (w <= 0.0f) frameBox(a, t * m.anim.y + m.anim.z);
            else if (w >= 1.0f) frameBox(clip, local);
            else add(a, clip);
          }
        }
        Aabb placed = placeBox(box, m);
        b.grow(placed.min), b.grow(placed.max);
      }
    } else {
      for (u32 c : {n.leftFirst, n.leftFirst + 1}) b.grow(staticNodes_[c].min), b.grow(staticNodes_[c].max);
    }
    n.min = b.min;
    n.max = b.max;
  }
  r_.device().writeBuffer(nodes_, 0, staticNodes_.data(), staticNodes_.size() * sizeof(Node));
}

void CrowdRenderer::drawRaster(CommandList& cmd, BindGroupHandle frameGroup, u32 frameOffset, bool capture) {
  if (!active()) return;
  cmd.setPipeline(pipelines_[capture ? 1 : 0]);
  cmd.setBindGroup(0, frameGroup, {&frameOffset, 1});
  for (u32 mi = 0; mi < (u32)pack_.models.size(); mi++) {
    for (const auto& range : {staticRanges_[mi], actorRanges_[mi]}) {
      if (range.second == 0 || modelVertexCount_[mi] == 0) continue;
      DrawData d{data_.time, data_.counts, {data_.events[0], data_.events[1], data_.events[2]}, data_.wave, data_.bounds,
                 vec4((f32)range.first, (f32)modelVertexOffset_[mi], (f32)modelVertexCount_[mi], 0)};
      u32 offset = r_.pushUniform(&d, sizeof(d));
      cmd.setBindGroup(1, rasterGroup_, {&offset, 1});
      // No vertex buffer: vertex v of draw = person v / count, corner v % count (crowd.slang).
      cmd.draw(range.second * modelVertexCount_[mi]);
    }
  }
}

void CrowdRenderer::bindTrace(CommandList& cmd) {
  u32 offset = r_.pushUniform(&data_, sizeof(data_));
  cmd.setBindGroup(1, traceGroup_, {&offset, 1});
}

} // namespace ace
