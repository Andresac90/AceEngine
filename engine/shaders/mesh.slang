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
[[vk::binding(1, 0)]] Texture2D shadowMap; // r: light-space depth of the nearest occluder
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
  float3 worldPos : POSITION1;
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
  o.color = float4(pow(v.color.rgb, 2.2), v.color.a) * draw.color; // vertex colors are sRGB-encoded
  o.viewDepth = -mul(frame.view, world).z;
  o.worldPos = world.xyz;
  return o;
}

// 1 = lit, 0 = in shadow. Unfiltered on purpose: pixel art wants a hard edge.
float sunVisibility(float3 worldPos, float3 n) {
  if (frame.params.y < 0.5) return 1.0;
  // Offset along the normal by ~1.5 shadow texels instead of a large depth bias:
  // removes acne without detaching shadows from their casters.
  float4 lp = mul(frame.lightViewProj, float4(worldPos + n * frame.params.z * 1.5, 1.0));
  float2 uv = float2(lp.x * 0.5 + 0.5, 0.5 - lp.y * 0.5);
  if (any(uv < 0.0) || any(uv >= 1.0) || lp.z >= 1.0) return 1.0;
  uint w, h;
  shadowMap.GetDimensions(w, h);
  float occluder = shadowMap.Load(int3(int2(uv * float2(w, h)), 0)).r;
  return lp.z - 0.0015 > occluder ? 0.0 : 1.0;
}

FsOut fsMain(VsOut i) {
  float4 albedo = albedoTexture.Sample(albedoSampler, i.uv) * i.color;
  if (albedo.a < 0.5) discard; // hard-edged cutout: pixel art has no soft alpha
  float3 n = normalize(i.normal);
  float3 lit = albedo.rgb;
  if (draw.emissive.a < 0.5) {
    float ndl = saturate(dot(n, frame.lightDir.xyz));
    if (ndl > 0.0) ndl *= sunVisibility(i.worldPos, n);
    float bands = frame.params.x;
    if (bands > 0.5) ndl = ceil(ndl * bands - 0.001) / bands;
    lit = albedo.rgb * (frame.ambient.rgb + frame.lightColor.rgb * ndl * 0.318);
  }
  FsOut o;
  o.color = float4(lit + draw.emissive.rgb, 1.0);
  o.normalDepth = float4(n, i.viewDepth);
  return o;
}
