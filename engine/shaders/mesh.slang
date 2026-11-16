// Rasterized scene pass: toon-lit meshes and sprites.
// Writes linear HDR color and (world normal, view depth) for the present pass.
#include "common.slangh"

struct DrawData {
  float4x4 model;
  float4x4 normalMatrix;
  float4 color;
  float4 emissive; // rgb: emissive, a: 1 = unlit (sprites)
};

[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame;
[[vk::binding(0, 1)]] ConstantBuffer<DrawData> draw;
[[vk::binding(0, 2)]] Texture2D albedoTexture;
[[vk::binding(1, 2)]] SamplerState albedoSampler;

struct VsIn {
  [[vk::location(0)]] float3 position : POSITION;
  [[vk::location(1)]] float3 normal : NORMAL;
  [[vk::location(2)]] float2 uv : TEXCOORD0;
  [[vk::location(3)]] float4 color : COLOR0;
};
struct VsOut {
  float4 position : SV_Position;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;
  float4 color : COLOR0;
  float viewDepth : DEPTH;
};
struct FsOut {
  float4 color : SV_Target0;
  float4 normalDepth : SV_Target1;
};

VsOut vsMain(VsIn v) {
  VsOut o;
  float4 world = mul(draw.model, float4(v.position, 1.0));
  o.position = mul(frame.viewProj, world);
  o.normal = mul(draw.normalMatrix, float4(v.normal, 0.0)).xyz;
  o.uv = v.uv;
  o.color = v.color * draw.color;
  o.viewDepth = -mul(frame.view, world).z;
  return o;
}

FsOut fsMain(VsOut i) {
  float4 albedo = albedoTexture.Sample(albedoSampler, i.uv) * i.color;
  if (albedo.a < 0.5) discard; // hard-edged cutout: pixel art has no soft alpha
  float3 n = normalize(i.normal);
  float3 lit = albedo.rgb;
  if (draw.emissive.a < 0.5) {
    float ndl = saturate(dot(n, frame.lightDir.xyz));
    float bands = frame.params.x;
    if (bands > 0.5) ndl = ceil(ndl * bands - 0.001) / bands;
    lit = albedo.rgb * (frame.ambient.rgb + frame.lightColor.rgb * ndl * 0.318);
  }
  FsOut o;
  o.color = float4(lit + draw.emissive.rgb, 1.0);
  o.normalDepth = float4(n, i.viewDepth);
  return o;
}
