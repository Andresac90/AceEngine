#include "imgui_rhi.h"

namespace ace {

using namespace rhi;

ImGuiRhi::ImGuiRhi(Device& device, ShaderHandle vs, ShaderHandle fs, Format targetFormat) : device_(device) {
  BindGroupLayoutDesc ubo;
  ubo.bindings[0] = BindingType::UniformBuffer;
  uboLayout_ = device_.createBindGroupLayout(ubo);
  BindGroupLayoutDesc tex;
  tex.bindings[0] = BindingType::Texture;
  tex.bindings[1] = BindingType::Sampler;
  textureLayout_ = device_.createBindGroupLayout(tex);

  ubo_ = device_.createBuffer({256, BufferUsage_Uniform, "imgui ubo"});
  BindGroupDesc g;
  g.layout = uboLayout_;
  g.entries[0].buffer = ubo_;
  g.entries[0].size = 16;
  uboGroup_ = device_.createBindGroup(g);
  sampler_ = device_.createSampler({Filter::Linear, AddressMode::ClampToEdge});

  PipelineDesc p;
  p.name = "imgui";
  p.vertexShader = vs;
  p.fragmentShader = fs;
  p.attributes[0] = {0, VertexFormat::Float2, offsetof(ImDrawVert, pos)};
  p.attributes[1] = {1, VertexFormat::Float2, offsetof(ImDrawVert, uv)};
  p.attributes[2] = {2, VertexFormat::UByte4Norm, offsetof(ImDrawVert, col)};
  p.attributeCount = 3;
  p.vertexStride = sizeof(ImDrawVert);
  p.bindGroupLayouts[0] = uboLayout_;
  p.bindGroupLayouts[1] = textureLayout_;
  p.colorFormats[0] = targetFormat;
  p.cull = CullMode::None;
  p.blend[0] = BlendMode::Alpha;
  pipeline_ = device_.createPipeline(p);
  ACE_ASSERT(pipeline_, "failed to create the ImGui pipeline");

  ImGuiIO& io = ImGui::GetIO();
  io.BackendRendererName = "ace_rhi";
  io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
  unsigned char* pixels = nullptr;
  int w = 0, h = 0;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
  font_ = device_.createTexture({(u32)w, (u32)h, Format::RGBA8, TextureUsage_Sampled, "imgui font"});
  device_.writeTexture(font_, pixels);
  io.Fonts->SetTexID(textureId(font_));
}

void ImGuiRhi::forgetTexture(TextureHandle t) {
  auto it = textureGroups_.find(t.id);
  if (it == textureGroups_.end()) return;
  device_.destroy(it->second);
  textureGroups_.erase(it);
}

void ImGuiRhi::render(CommandList& cmd, ImDrawData* data) {
  f32 fbW = data->DisplaySize.x * data->FramebufferScale.x, fbH = data->DisplaySize.y * data->FramebufferScale.y;
  if (fbW <= 0 || fbH <= 0 || data->TotalVtxCount == 0) return;

  auto fit = [&](BufferHandle& buffer, u64& capacity, u64 bytes, u32 usage) {
    if (bytes <= capacity) return;
    if (buffer) device_.destroy(buffer);
    capacity = bytes * 2;
    buffer = device_.createBuffer({capacity, usage, "imgui geometry"});
  };
  fit(vertices_, vertexCapacity_, data->TotalVtxCount * sizeof(ImDrawVert), BufferUsage_Vertex);
  fit(indices_, indexCapacity_, data->TotalIdxCount * sizeof(ImDrawIdx), BufferUsage_Index);
  u64 vOffset = 0, iOffset = 0;
  for (int n = 0; n < data->CmdListsCount; n++) {
    const ImDrawList* list = data->CmdLists[n];
    device_.writeBuffer(vertices_, vOffset, list->VtxBuffer.Data, list->VtxBuffer.Size * sizeof(ImDrawVert));
    device_.writeBuffer(indices_, iOffset, list->IdxBuffer.Data, list->IdxBuffer.Size * sizeof(ImDrawIdx));
    vOffset += list->VtxBuffer.Size * sizeof(ImDrawVert);
    iOffset += list->IdxBuffer.Size * sizeof(ImDrawIdx);
  }

  // Map ImGui's top-left pixel coordinates to clip space (+Y up).
  f32 sx = 2.0f / data->DisplaySize.x, sy = -2.0f / data->DisplaySize.y;
  f32 st[4] = {sx, sy, -1.0f - data->DisplayPos.x * sx, 1.0f - data->DisplayPos.y * sy};
  device_.writeBuffer(ubo_, 0, st, sizeof(st));

  cmd.setViewport(0, 0, fbW, fbH);
  cmd.setPipeline(pipeline_);
  u32 zero = 0;
  cmd.setBindGroup(0, uboGroup_, {&zero, 1});
  cmd.setVertexBuffer(vertices_);
  cmd.setIndexBuffer(indices_, sizeof(ImDrawIdx) == 4);

  i32 baseVertex = 0;
  u32 baseIndex = 0;
  for (int n = 0; n < data->CmdListsCount; n++) {
    const ImDrawList* list = data->CmdLists[n];
    for (const ImDrawCmd& c : list->CmdBuffer) {
      if (c.UserCallback) continue; // not used by the editor
      ImVec2 min((c.ClipRect.x - data->DisplayPos.x) * data->FramebufferScale.x, (c.ClipRect.y - data->DisplayPos.y) * data->FramebufferScale.y);
      ImVec2 max((c.ClipRect.z - data->DisplayPos.x) * data->FramebufferScale.x, (c.ClipRect.w - data->DisplayPos.y) * data->FramebufferScale.y);
      if (max.x <= min.x || max.y <= min.y) continue;
      cmd.setScissor((i32)min.x, (i32)min.y, (u32)(max.x - min.x), (u32)(max.y - min.y));
      u32 texId = (u32)c.GetTexID();
      BindGroupHandle& group = textureGroups_[texId];
      if (!group) {
        BindGroupDesc g;
        g.layout = textureLayout_;
        g.entries[0].texture = TextureHandle{texId};
        g.entries[1].sampler = sampler_;
        group = device_.createBindGroup(g);
      }
      cmd.setBindGroup(1, group);
      cmd.drawIndexed(c.ElemCount, baseIndex + c.IdxOffset, baseVertex + (i32)c.VtxOffset);
    }
    baseVertex += list->VtxBuffer.Size;
    baseIndex += list->IdxBuffer.Size;
  }
}

} // namespace ace
