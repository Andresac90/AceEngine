// Shadow-map pass: renders the scene from the sun and stores light-space depth
// in a color target (R32F), so the scene shader can read it with a plain Load.
// Texture alpha cuts holes, so sprites cast the shadow of their drawing.
#include "common.slangh"

[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame : register(b0, space0);
[[vk::binding(0, 1)]] ConstantBuffer<DrawData> draw : register(b0, space1);
[[vk::binding(0, 2)]] Texture2D albedoTexture : register(t0, space2);
[[vk::binding(1, 2)]] SamplerState albedoSampler : register(s1, space2);

struct VsIn {
  [[vk::location(0)]] float3 position : ATTRIB0;
  [[vk::location(2)]] float2 uv : ATTRIB2;
};
struct VsOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
};

VsOut vsMain(VsIn v) {
  VsOut o;
  o.position = mul(frame.lightViewProj, mul(draw.model, float4(v.position, 1.0)));
  o.uv = v.uv * draw.uvRect.zw + draw.uvRect.xy;
  return o;
}

float4 fsMain(VsOut i) : SV_Target0 {
  // World-mapped meshes use opaque tiling textures, so plain UVs are enough here.
  if (draw.flags.y < 0.5 && albedoTexture.Sample(albedoSampler, i.uv).a * draw.color.a < 0.5) discard;
  return float4(i.position.z, 0.0, 0.0, 1.0);
}
