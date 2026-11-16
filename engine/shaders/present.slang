// Final pass: takes the (possibly low-resolution) scene image and writes the
// output image. Adds sky, depth/normal outlines and tone mapping, and performs
// the hard-pixel upscale with a sub-texel scroll offset for smooth camera motion.
#include "common.slangh"

struct PresentData {
  float4 rect;     // xy: first visible texel (may be fractional), zw: visible size in texels
  float4 outline;  // x: outline strength, y: highlight strength, z: exposure, w: outlines on
  float4 skyTop;
  float4 skyBottom;
  float4 camRight;   // xyz: world-space camera right, w: texel size in world units (orthographic)
  float4 camUp;      // xyz: camera up, w: texel size per unit of depth (perspective)
  float4 camForward;
};

[[vk::binding(0, 0)]] ConstantBuffer<PresentData> data;
[[vk::binding(1, 0)]] Texture2D colorTexture;
[[vk::binding(2, 0)]] Texture2D normalDepthTexture;

struct VsOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
};

VsOut vsMain(uint id : SV_VertexID) {
  VsOut o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.position = float4(o.uv.x * 2.0 - 1.0, 1.0 - o.uv.y * 2.0, 0.0, 1.0);
  return o;
}

float3 tonemap(float3 c) {
  c *= data.outline.z;
  return c / (1.0 + max(c.r, max(c.g, c.b)) * 0.25); // gentle shoulder, keeps flat colors flat
}

float4 fsMain(VsOut i) : SV_Target0 {
  uint w, h;
  colorTexture.GetDimensions(w, h);
  int2 size = int2(w, h);
  int2 p = clamp(int2(floor(data.rect.xy + i.uv * data.rect.zw)), int2(0, 0), size - 1);

  float4 nd = normalDepthTexture.Load(int3(p, 0));
  float3 sky = lerp(data.skyTop.rgb, data.skyBottom.rgb, saturate(i.uv.y));
  float3 color = nd.w > kSkyDepth ? sky : tonemap(colorTexture.Load(int3(p, 0)).rgb);

  if (data.outline.w > 0.5) {
    static const int2 offsets[4] = {int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1)};
    float depthEdge = 0.0;
    float normalEdge = 0.0;
    for (int k = 0; k < 4; k++) {
      float4 q = normalDepthTexture.Load(int3(clamp(p + offsets[k], int2(0, 0), size - 1), 0));
      // Outline pixels belong to the nearer surface, so silhouettes stay 1 texel wide.
      float dd = q.w - nd.w;
      depthEdge += saturate(dd / max(0.25, nd.w * 0.05));
      // Creases get a highlight only where the surface is convex (normals fan out
      // along the step to the neighbor), depth is continuous (else it is a
      // silhouette, handled above) and on one side of the edge, so it stays 1 texel.
      float texel = data.camRight.w + data.camUp.w * nd.w;
      float3 toNeighbor = (data.camRight.xyz * offsets[k].x - data.camUp.xyz * offsets[k].y) * texel + data.camForward.xyz * dd;
      float convex = step(0.01, dot(q.xyz - nd.xyz, toNeighbor));
      float crease = 1.0 - dot(nd.xyz, q.xyz);
      float side = step(0.0, dot(nd.xyz - q.xyz, float3(0.6, 1.0, 0.4)));
      normalEdge += crease * convex * side * step(abs(dd), max(0.25, texel * 2.0));
    }
    if (nd.w < kSkyDepth) {
      if (depthEdge > 0.6) color *= 1.0 - data.outline.x;
      else if (normalEdge > 0.4) color *= 1.0 + data.outline.y;
    }
  }
  return float4(color, 1.0);
}
