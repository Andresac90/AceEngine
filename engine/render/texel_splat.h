// World pixels ("texel splatting", after Dylan Ebert, arXiv 2603.14587):
//  1. capture: the scene is rasterized (with the normal shading) into a cube-map
//     atlas around a probe point, twice: a *grid* probe snapped to a world grid
//     (stable) and an *eye* probe at the camera (fills what the grid probe cannot see);
//  2. post: per captured texel, outlines, tone mapping and palette snapping with
//     dithering anchored to the texel grid;
//  3. splat: every captured texel becomes a small square in the world, lying on
//     the surface it came from, drawn from the real camera.
// Pixels are world objects, so camera rotation and movement do not make them
// swim, even with a perspective camera.
#pragma once
#include "render/renderer.h"

namespace ace {

class TexelSplatter {
public:
  explicit TexelSplatter(Renderer& renderer);
  ~TexelSplatter();
  // Draws into the renderer's scene targets (color_, normalDepth_, depth_) at width x height.
  void render(rhi::CommandList& cmd, Scene& scene, Assets& assets, const mat4& view, const mat4& proj, const mat4& cameraWorld,
              u32 width, u32 height);

  // The six cube faces: forward, right and up vectors, matching the capture views.
  struct Face {
    vec3 forward, right, up;
  };
  static Face face(int index);
  static vec3 snapOrigin(vec3 position, f32 cell); // grid probe position for a camera position

private:
  struct Probe {
    rhi::TextureHandle color, normalDepth, depth, final;
    rhi::BindGroupHandle postGroup, splatGroup;
  };
  void ensureProbes(u32 faceSize);
  void destroyProbes();

  Renderer& r_;
  Probe probes_[3]; // 0: grid probe, 1: eye probe, 2: previous grid probe (while crossfading)
  vec3 gridOrigin_{0.0f}, previousOrigin_{0.0f};
  bool hasGrid_ = false;
  f32 fade_ = 1.0f; // 0 -> 1 after the grid probe moved to a new cell
  f64 lastTime_ = 0.0;
  u32 faceSize_ = 0;
  rhi::BindGroupLayoutHandle postLayout_, splatLayout_;
  rhi::PipelineHandle postPipeline_, splatPipeline_;
};

} // namespace ace
