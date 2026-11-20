// Edge-aware smoothing of path-traced light (one "a-trous" wavelet pass, as in
// SVGF): a 5x5 B3-spline kernel whose taps are `step` pixels apart, weighted
// down across changes of surface orientation, depth and brightness. Run with
// steps 1, 2, 4 for a wide, cheap blur that still stops at edges and shadow
// lines. Only light is smoothed: textures are applied afterwards (pt_compose).
#include "common.slangh"

struct DenoiseData {
  float4 rect;   // region of the texture to filter (a cube face, or the whole image): x, y, width, height
  float4 params; // x: tap spacing in pixels, y: brightness tolerance (smaller = sharper)
};

[[vk::binding(0, 0)]] ConstantBuffer<DenoiseData> data : register(b0, space0);
[[vk::binding(1, 0)]] Texture2D lightIn : register(t1, space0);
[[vk::binding(2, 0)]] Texture2D normalDepth : register(t2, space0);

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
  static const float kernel[5] = {1.0 / 16.0, 1.0 / 4.0, 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0};
  static const float3 lumaWeights = float3(0.2126, 0.7152, 0.0722);
  int2 p = int2(i.position.xy);
  float4 nd = normalDepth.Load(int3(p, 0));
  float4 centre = lightIn.Load(int3(p, 0));
  if (nd.w > kSkyDepth) return centre;
  int step = int(data.params.x);
  int2 lo = int2(data.rect.xy), hi = int2(data.rect.xy + data.rect.zw) - 1; // never cross into another cube face
  float luma = dot(centre.rgb, lumaWeights);
  float tolerance = max(data.params.y, 1e-4);
  float3 sum = 0;
  float weights = 0;
  for (int dy = -2; dy <= 2; dy++) {
    for (int dx = -2; dx <= 2; dx++) {
      int2 q = clamp(p + int2(dx, dy) * step, lo, hi);
      float4 qnd = normalDepth.Load(int3(q, 0));
      if (qnd.w > kSkyDepth) continue;
      float4 qlight = lightIn.Load(int3(q, 0));
      float facing = pow(saturate(dot(nd.xyz, qnd.xyz)), 64.0);
      float depth = exp(-abs(qnd.w - nd.w) / (0.05 * nd.w * float(step) + 1e-3));
      float bright = exp(-abs(dot(qlight.rgb, lumaWeights) - luma) / tolerance);
      float w = kernel[dx + 2] * kernel[dy + 2] * facing * depth * bright;
      sum += qlight.rgb * w;
      weights += w;
    }
  }
  return float4(weights > 0.0 ? sum / weights : centre.rgb, centre.a);
}
