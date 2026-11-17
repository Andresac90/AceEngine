// CPU side of the path tracer: flattens the scene's meshes into world-space
// triangles, builds a BVH over them and uploads both for pathtrace.slang.
#pragma once
#include "render/renderer.h"

namespace ace {

class PathTracer {
public:
  explicit PathTracer(Renderer& renderer);
  ~PathTracer();
  void render(rhi::CommandList& cmd, Scene& scene, Assets& assets, const mat4& view, const mat4& proj, const Camera& camera,
              rhi::TextureHandle color, rhi::TextureHandle normalDepth, u32 width, u32 height);

  struct Node {
    vec3 min;
    u32 leftFirst;
    vec3 max;
    u32 count;
  };
  struct Triangle {
    vec4 v0, e1, e2, n0, n1, n2, color, emissive;
    vec4 uv01, uv2Flags, atlas; // see pathtrace.slang
  };
  // Builds a BVH in place (reorders `tris`). Exposed for tests.
  static void buildBvh(std::vector<Triangle>& tris, std::vector<Node>& nodes);

private:
  void rebuild(Scene& scene, Assets& assets, vec3 cameraRight);
  Renderer& r_;
  rhi::BindGroupLayoutHandle layout_;
  rhi::BindGroupHandle group_;
  rhi::PipelineHandle pipeline_;
  rhi::BufferHandle nodeBuffer_, triBuffer_, lightBuffer_;
  u64 nodeCapacity_ = 0, triCapacity_ = 0, lightCapacity_ = 0;
  u32 lightCount_ = 0;
  rhi::TextureHandle atlas_; // every texture used by the scene, packed side by side
  u64 sceneHash_ = 0, viewHash_ = 0;
  u32 frame_ = 0;
};

} // namespace ace
