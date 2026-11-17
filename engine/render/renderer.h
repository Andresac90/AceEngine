// Scene renderer. Draws a Scene from a camera into any target texture.
//
// Pipeline (both scene modes feed the same present pass):
//   scene pass  -> color (linear HDR) + normal/depth, at low or full resolution
//                  * raster:      toon-shaded meshes and sprites   (mesh.slang)
//                  * path traced: software BVH in a fragment shader (pathtrace.slang)
//   present pass -> sky, outlines, tone map, hard-pixel upscale    (present.slang)
#pragma once
#include "assets/assets.h"
#include "rhi/rhi.h"
#include "scene/scene.h"
#include <map>

namespace ace {

struct CameraView {
  mat4 world{1.0f}; // camera-to-world transform
  Camera camera;
};

// Sheet frame shown at `seconds`, as (offset.xy, scale.zw) applied to the quad's UVs.
vec4 spriteUvRect(const SpriteRenderer& sprite, f64 seconds);
// Upright card facing horizontally along `right`, at the entity's position and scale.
mat4 uprightCard(const mat4& world, vec3 right);

class PathTracer;

class Renderer {
public:
  explicit Renderer(rhi::Device& device);
  ~Renderer();

  void beginFrame(); // once per frame, before any render()
  void render(rhi::CommandList& cmd, Scene& scene, Assets& assets, const CameraView& view, rhi::TextureHandle target,
              rhi::Format targetFormat, u32 width, u32 height);

  // First entity with a Camera component. Returns false if the scene has none.
  static bool findCamera(Scene& scene, CameraView& out);

  rhi::Device& device() { return device_; }
  rhi::ShaderHandle loadShader(const char* name, rhi::ShaderStage stage);
  // Copies `size` bytes into this frame's uniform ring; returns the dynamic offset.
  u32 pushUniform(const void* data, u32 size);
  rhi::BufferHandle uniformBuffer() const { return uniforms_; }

private:
  void ensureTargets(u32 w, u32 h);
  void drawRaster(rhi::CommandList& cmd, Scene& scene, Assets& assets, const mat4& view, const mat4& proj);
  // Fits the sun's orthographic frustum around every mesh; returns the shadow texel size in world units.
  f32 fitShadowFrustum(Scene& scene, Assets& assets, vec3 lightDir, mat4& lightViewProj);
  rhi::BindGroupHandle materialGroup(const Texture* texture);

  rhi::Device& device_;
  rhi::BufferHandle uniforms_;
  u32 uniformOffset_ = 0;

  rhi::BindGroupLayoutHandle frameLayout_, drawLayout_, materialLayout_, presentLayout_;
  rhi::BindGroupHandle frameGroup_, drawGroup_, presentGroup_;
  rhi::PipelineHandle meshPipeline_, spritePipeline_, shadowPipeline_;
  rhi::BindGroupHandle shadowFrameGroup_;
  rhi::TextureHandle shadowMap_, shadowDepth_;
  rhi::ShaderHandle presentVs_, presentFs_;
  std::map<rhi::Format, rhi::PipelineHandle> presentPipelines_;
  std::map<u32, rhi::BindGroupHandle> materialGroups_;
  rhi::SamplerHandle nearestSampler_;

  rhi::TextureHandle color_, normalDepth_, depth_;
  u32 lowWidth_ = 0, lowHeight_ = 0;

  std::unique_ptr<PathTracer> pathTracer_;
};

} // namespace ace
