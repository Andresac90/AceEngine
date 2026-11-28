// Large animated crowds for both renderers (raster and path traced).
//
// The problem: a path tracer needs an acceleration structure (BVH) over the
// geometry it hits. 18,000 animated people x ~450 triangles is ~8 million
// moving triangles; rebuilding or refitting a BVH over them every frame is
// what costs real engines hundreds of milliseconds and gigabytes.
//
// The answer here (after AMD's tetrahedral cages, Gruen et al. HPG 2026, which
// trace rays against static rest-pose geometry inside animated cells):
//  - Every character is split into rigid parts, one per bone. Each part keeps
//    a small BVH in *rest pose*, built once at load ("mini-BLAS").
//  - A ray that reaches a person is moved into each part's rest space with the
//    inverse of that bone's current rigid transform. A rigid (affine) map keeps
//    rays straight, so the static rest-pose BVH answers the query exactly.
//  - The pose is evaluated *in the shader* from the time and the person's
//    parameters (crowd.slangh), so nothing is uploaded per frame.
//  - The top level (one leaf per seated person) is built once over boxes that
//    hold every frame of each person's idle clip. While a team event or the
//    wave runs, it is refit (same tree, new boxes) every frame to the pose
//    each person can be showing; precomputed per-pair-of-clips boxes cover
//    crossfades ("pose-conservative bounds"). The few moving actors (skaters)
//    live in a second, tiny top level rebuilt each frame.
//  - Shadow and bounce rays see each bone as its box (a proxy); camera rays
//    and reflections see the triangles.
// The same data drives the raster pass (crowd.slang) by vertex pulling.
// Design, measurements and the problems met: docs/RENDERING.md section 8b.
#pragma once
#include "render/renderer.h"

namespace ace {

class CrowdRenderer {
public:
  static constexpr u32 kBones = 9;      // must match kCrowdBones in crowd.slangh
  static constexpr u32 kMaxActors = 64; // CrowdActor entities drawn per frame

  struct Node { // same layout as PathTracer::Node / BvhNode in the shaders
    vec3 min;
    u32 leftFirst; // interior: left child (right = left + 1); leaf: first item
    vec3 max;
    u32 count;     // 0 = interior
  };
  // Binned-SAH BVH over boxes. `order` receives the item order the leaves
  // refer to (leaf items are order[first .. first + count)). Node 0 is the root;
  // child and item indices are relative (callers add their own offsets).
  static void buildBoxBvh(const std::vector<Aabb>& boxes, std::vector<u32>& order, std::vector<Node>& nodes, u32 maxLeaf);

  struct GpuMember { // CrowdMember in crowd.slangh
    vec4 posScale, facing, anim;
    u32 colors[4];
  };

  struct Pose {
    quat q{1, 0, 0, 0};
    vec3 t{0.0f};
  };
  struct Pack {
    struct Part {
      u32 bone = 0;
      std::vector<vec3> positions, normals; // 3 per triangle, rest pose
      std::vector<f32> colors;              // 1 per triangle: 0xRRGGBB, or -1/-2/-3 = the person's colors
    };
    struct Model {
      std::string name;
      std::vector<Part> parts;
      std::vector<std::vector<std::vector<Pose>>> clips; // [clip][frame][bone]
      // Character space, conservative over every pose a person can show:
      // per pair of clips (idle a, override b), every frame of both and every
      // blend between them; (a, a) is the idle clip alone.
      Aabb bounds;                       // all pairs (moving actors)
      std::vector<Aabb> pairBounds;      // [a * clips + b]
      std::vector<Aabb> pairPartBounds;  // [(a * clips + b) * kBones + bone]
      // Exact boxes of one held frame (animation is stepped, never interpolated).
      std::vector<std::vector<Aabb>> frameBounds;                 // [clip][frame]
      std::vector<std::vector<std::vector<Aabb>>> framePartBounds; // [clip][frame][bone]
    };
    u32 fps = 12;
    std::vector<std::string> clipNames;
    std::vector<bool> clipLoops;
    std::vector<Model> models;
  };
  // Parses and checks a *.crowd.json document; on failure returns false with `error` set.
  static bool parsePack(const nlohmann::json& j, Pack& pack, std::string& error);

  CrowdRenderer(Renderer& renderer, rhi::BindGroupLayoutHandle frameLayout);
  ~CrowdRenderer();

  // Once per frame: (re)loads the scene's first Crowd when its files change,
  // places the CrowdActors and fills the per-frame constants.
  void update(Scene& scene, Assets& assets, f64 seconds);
  bool active() const { return memberCount_ > 0; }
  u64 version() const { return version_; } // changes whenever the seated crowd is (re)loaded
  u32 people() const { return staticCount_ + actorCount_; }

  // Draws every person into the current raster pass (group 0 = the frame group).
  void drawRaster(rhi::CommandList& cmd, rhi::BindGroupHandle frameGroup, u32 frameOffset, bool capture);
  // Path tracing: bind group 1 of pathtrace.slang (empty but valid without a crowd).
  rhi::BindGroupLayoutHandle traceLayout() const { return traceLayout_; }
  void bindTrace(rhi::CommandList& cmd);

private:
  bool load(Scene& scene, Assets& assets, const Crowd& crowd);
  // Refits the seated top level: busy = to the clips each person shows right
  // now (every frame while an event or the wave runs), else to the idle clips.
  void refitStatic(bool busy);
  // Mirror of crowdAnim (crowd.slangh): override clip, its local time and weight.
  f32 overrideAt(const GpuMember& m, f32 seconds, u32& clip, f32& clipSeconds) const;
  u32 frameAt(u32 clip, u32 frames, f32 seconds) const; // mirror of crowdFrame
  void createGroups();
  struct Gpu;
  Renderer& r_;
  rhi::BindGroupLayoutHandle traceLayout_, rasterLayout_;
  rhi::PipelineHandle pipelines_[2]; // [capture]
  rhi::BufferHandle nodes_, members_, table_, tris_, verts_, list_;
  rhi::BindGroupHandle traceGroup_, rasterGroup_;
  Pack pack_;
  std::string packPath_, membersPath_;
  u32 staticCount_ = 0, actorCount_ = 0, memberCount_ = 0;
  u32 dynamicNodeBase_ = 0, listStatic_ = 0;
  u32 partHeaderBase_ = 0, modelHeaderBase_ = 0, clipBoxBase_ = 0;
  std::vector<Node> staticNodes_;       // CPU copy of the seated top level (for refits)
  std::vector<GpuMember> staticMembers_; // seated people in tree order
  bool refitBusy_ = false;
  std::vector<u32> modelVertexOffset_, modelVertexCount_;
  std::vector<std::pair<u32, u32>> staticRanges_, actorRanges_; // per model: (first list entry, count)
  u64 version_ = 0;
  struct Data {
    vec4 time, counts, events[3], wave, bounds;
  } data_{};
};

} // namespace ace
