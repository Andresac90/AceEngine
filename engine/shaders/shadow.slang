// Shadow-map pass: renders the scene from the sun and stores light-space depth
// in a color target (R32F), so the scene shader can read it with a plain Load.
#include "common.slangh"

struct DrawData {
  float4x4 model;
  float4x4 normalMatrix;
  float4 color;
  float4 emissive;
};

[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame;
[[vk::binding(0, 1)]] ConstantBuffer<DrawData> draw;

struct VsOut {
  float4 position : SV_Position;
};

VsOut vsMain([[vk::location(0)]] float3 position : POSITION) {
  VsOut o;
  o.position = mul(frame.lightViewProj, mul(draw.model, float4(position, 1.0)));
  return o;
}

float4 fsMain(VsOut i) : SV_Target0 { return float4(i.position.z, 0.0, 0.0, 1.0); }
