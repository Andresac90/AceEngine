// CPU side of the path tracer: flattens the scene's meshes into world-space
// triangles, builds a BVH over them and uploads both for pathtrace.slang.
#pragma once
#include "render/renderer.h"
#include <unordered_map>

namespace ace {

class PathTracer {
public:
  explicit PathTracer(Renderer& renderer);
  ~PathTracer();
  // Where to trace into. Each slot keeps its own running average (the screen,
  // and each probe cube face in world-pixel mode).
  struct Target {
    rhi::TextureHandle color, normalDepth;
    vec4 viewport{0.0f};      // x, y, width, height in pixels
    u32 slot = 0;
    bool capture = false;     // normal/depth target is RGBA32F (probe atlas)
    bool skipSprites = false; // primary rays pass through sprites (they are drawn separately)
    mat4 spriteCamera{1.0f};  // camera-to-world of the real camera: billboards face it
    int samples = 0;          // rays per pixel per frame; 0 = the scene's ptSamples
    u32 settleFrames = 0;     // > 0: once this many frames are averaged and nothing changed, stop tracing (keep the image)
  };
  void render(rhi::CommandList& cmd, Scene& scene, Assets& assets, const mat4& view, const mat4& proj, const Target& target);

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
  // Forget cached texture data (atlas, palette binding): rebuilt on the next render.
  void invalidate() {
    sceneHash_ = 0;
    paletteId_ = 0;
    history_.clear();
  }

private:
  void rebuild(Scene& scene, Assets& assets, vec3 cameraRight, vec3 cameraPosition);
  Renderer& r_;
  rhi::BindGroupLayoutHandle layout_;
  rhi::BindGroupHandle group_;
  rhi::PipelineHandle pipelines_[2][2]; // [RGBA32F normal/depth][first frame: no blending]
  rhi::BufferHandle nodeBuffer_, triBuffer_, lightBuffer_;
  u64 nodeCapacity_ = 0, triCapacity_ = 0, lightCapacity_ = 0;
  u32 lightCount_ = 0;
  u32 paletteId_ = 0; // palette texture the bind group was built with
  rhi::TextureHandle atlas_; // every texture used by the scene, packed side by side
  u64 sceneHash_ = 0;
  struct History {
    u32 frame = 0;      // frames averaged so far
    u64 viewHash = 0;   // what was traced: anything else restarts the average
    u64 lastFrame = 0;  // Renderer::frameIndex() of the last trace into this slot
  };
  std::unordered_map<u32, History> history_;
};

} // namespace ace
