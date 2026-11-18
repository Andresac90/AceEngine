// Per captured texel of a probe atlas (six n x n faces side by side): outlines,
// tone mapping and palette snapping. Dithering is indexed by the texel, so the
// dither pattern sits on the world's pixels instead of the screen's.
#include "common.slangh"

struct PostData {
  float4 info; // x: face size, y: outline strength, z: highlight strength, w: outlines on
  float4 tone; // x: exposure, y: palette colors, z: dither
  float4 palette[64]; // sRGB
  float4 forward[6], right[6], up[6]; // cube face bases (texel_splat.cpp)
  Atmosphere atmosphere;
};

[[vk::binding(0, 0)]] ConstantBuffer<PostData> data;
[[vk::binding(1, 0)]] Texture2D colorAtlas;
[[vk::binding(2, 0)]] Texture2D normalDepthAtlas;

struct VsOut {
  float4 position : SV_Position;
};

VsOut vsMain(uint id : SV_VertexID) {
  VsOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
  return o;
}

float3 tonemap(float3 c) {
  c *= data.tone.x;
  return c / (1.0 + max(c.r, max(c.g, c.b)) * 0.25); // same curve as present.slang
}

float4 fsMain(VsOut i) : SV_Target0 {
  int n = int(data.info.x);
  int2 p = int2(i.position.xy);
  int faceStart = (p.x / n) * n;
  float4 nd = normalDepthAtlas.Load(int3(p, 0));
  if (nd.w > kSkyDepth) return float4(0, 0, 0, 0); // nothing here: never splatted
  float3 color = tonemap(colorAtlas.Load(int3(p, 0)).rgb);
  // Haze, per texel: distance from the probe (close to the camera's) along this texel's direction.
  {
    int face = p.x / n;
    float cx = (float(p.x - faceStart) + 0.5) / float(n) * 2.0 - 1.0;
    float cy = 1.0 - (float(p.y) + 0.5) / float(n) * 2.0;
    float3 ray = data.forward[face].xyz + data.right[face].xyz * cx + data.up[face].xyz * cy;
    color = applyFog(data.atmosphere, color, nd.w * length(ray), normalize(ray));
  }

  if (data.info.w > 0.5) {
    static const int2 offsets[4] = {int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1)};
    float depthEdge = 0.0, normalEdge = 0.0;
    for (int k = 0; k < 4; k++) {
      int2 q = p + offsets[k];
      if (q.x < faceStart || q.x >= faceStart + n || q.y < 0 || q.y >= n) continue; // stay on this face
      float4 o = normalDepthAtlas.Load(int3(q, 0));
      float dd = o.w - nd.w;
      // Outline texels belong to the nearer surface, so silhouettes stay 1 texel wide.
      depthEdge += saturate(dd / max(0.25, nd.w * 0.05));
      float crease = 1.0 - dot(nd.xyz, o.xyz);
      float side = step(0.0, dot(nd.xyz - o.xyz, float3(0.6, 1.0, 0.4)));
      normalEdge += crease * side * step(abs(dd), max(0.25, nd.w * 0.02));
    }
    if (depthEdge > 0.6) color *= 1.0 - data.info.y;
    else if (normalEdge > 0.4) color *= 1.0 + data.info.z;
  }

  int count = int(data.tone.y);
  if (count > 0) {
    float3 c = linearToSrgb(color) + (bayer4(p) - 0.5) * data.tone.z * 0.12;
    float best = 1e9;
    float3 chosen = c;
    for (int k = 0; k < count; k++) {
      float3 diff = data.palette[k].rgb - c;
      float dist = dot(diff * diff, float3(0.30, 0.59, 0.11));
      if (dist < best) {
        best = dist;
        chosen = data.palette[k].rgb;
      }
    }
    color = pow(chosen, 2.2);
  }
  return float4(color, 1.0);
}
