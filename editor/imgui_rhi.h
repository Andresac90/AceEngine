// Dear ImGui renderer on top of the engine RHI, so the editor runs on every backend.
#pragma once
#include "rhi/rhi.h"
#include <imgui.h>
#include <map>

namespace ace {

class ImGuiRhi {
public:
  ImGuiRhi(rhi::Device& device, rhi::ShaderHandle vs, rhi::ShaderHandle fs, rhi::Format targetFormat);
  // Call inside an open render pass that targets `targetFormat`.
  void render(rhi::CommandList& cmd, ImDrawData* data);
  static ImTextureID textureId(rhi::TextureHandle t) { return (ImTextureID)t.id; }
  void forgetTexture(rhi::TextureHandle t); // before destroying a texture that was shown
  // After textures were destroyed elsewhere (asset reload): their ids can be reused.
  // The GPU must be idle.
  void forgetAllTextures();

private:
  rhi::Device& device_;
  rhi::PipelineHandle pipeline_;
  rhi::BindGroupLayoutHandle uboLayout_, textureLayout_;
  rhi::BindGroupHandle uboGroup_;
  rhi::BufferHandle ubo_, vertices_, indices_;
  u64 vertexCapacity_ = 0, indexCapacity_ = 0;
  rhi::SamplerHandle sampler_;
  rhi::TextureHandle font_;
  std::map<u32, rhi::BindGroupHandle> textureGroups_;
};

} // namespace ace
