// Final path-traced color: surface color x (denoised) light, or, with ramp
// shading, the palette ramp color picked from the amount of light; plus the
// light the surface gives off itself.
#include "common.slangh"

struct ComposeData {
  float4 rect; // unused by the shader (the viewport limits the pass); kept for symmetry
  float4 ramp; // x: ramp shading on, y: shadow steps, z: brightness of a fully lit white surface
};

[[vk::binding(0, 0)]] ConstantBuffer<ComposeData> data;
[[vk::binding(1, 0)]] Texture2D lightTexture;
[[vk::binding(2, 0)]] Texture2D albedoTexture;
[[vk::binding(3, 0)]] Texture2D emissionTexture;
[[vk::binding(4, 0)]] Texture2D paletteTexture;
[[vk::binding(5, 0)]] Texture2D normalDepth;

struct VsOut {
  float4 position : SV_Position;
};

VsOut vsMain(uint id : SV_VertexID) {
  VsOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
  return o;
}

float4 fsMain(VsOut i) : SV_Target0 {
  int3 p = int3(int2(i.position.xy), 0);
  if (normalDepth.Load(p).w > kSkyDepth) return float4(0, 0, 0, 1); // sky: drawn by the present pass
  float3 light = lightTexture.Load(p).rgb;
  float4 albedo = albedoTexture.Load(p);
  float3 color = albedo.rgb * light;
  if (data.ramp.x > 0.5 && albedo.a > 0.5) {
    float amount = dot(light, float3(0.2126, 0.7152, 0.0722)) / max(data.ramp.z, 1e-4);
    color = rampShade(paletteTexture, albedo.rgb, amount, int(data.ramp.y));
  }
  return float4(color + emissionTexture.Load(p).rgb, 1.0);
}
