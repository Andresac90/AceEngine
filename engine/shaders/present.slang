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
  float4 paletteInfo;  // x: number of palette colors (0 = no palette), y: dither amount, z: 1 = geometry colors are final (world pixels), w: sky pixel size
  float4 palette[64];  // sRGB colors
  float4x4 invViewProj; // of the projection the scene targets were drawn with
  float4 cameraPos;
  Atmosphere atmosphere;
};

[[vk::binding(0, 0)]] ConstantBuffer<PresentData> data : register(b0, space0);
[[vk::binding(1, 0)]] Texture2D colorTexture : register(t1, space0);
[[vk::binding(2, 0)]] Texture2D normalDepthTexture : register(t2, space0);

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

float3 rayDirection(float2 uv) { // uv 0..1 across the scene targets, y down
  float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  float4 a = mul(data.invViewProj, float4(ndc, 0.0, 1.0));
  float4 b = mul(data.invViewProj, float4(ndc, 1.0, 1.0));
  return normalize(b.xyz / b.w - a.xyz / a.w);
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
  // Sky per game pixel, not per screen pixel, so it keeps the same hard grid.
  // In world-pixel mode the output is full resolution, so the sky gets its own
  // coarse pixel grid (paletteInfo.w screen pixels per sky pixel).
  int skyPixel = max(int(data.paletteInfo.w), 1);
  int2 sp = p / skyPixel;
  float2 skyAt = (float2(sp) + 0.5) * float(skyPixel);
  float3 skyDir = rayDirection(skyAt / float2(size));
  float3 sky = skyRadiance(data.atmosphere, skyDir);
  bool final = data.paletteInfo.z > 0.5;
  bool isSky = nd.w > kSkyDepth;
  float4 sceneColor = colorTexture.Load(int3(p, 0));
  bool done = final && sceneColor.a < 0.5; // world-pixel splats arrive finished (alpha 0); sprites over them do not
  float3 color = isSky ? sky : (done ? sceneColor.rgb : tonemap(sceneColor.rgb));
  if (!isSky && !done) { // haze (world-pixel splats got theirs per texel already)
    float3 dir = rayDirection((float2(p) + 0.5) / float2(size));
    float dist = nd.w / max(dot(dir, data.camForward.xyz), 1e-3);
    color = applyFog(data.atmosphere, color, dist, dir);
  }

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
  // Limited palette: snap to the nearest palette color, with ordered dithering
  // on the low-resolution pixel grid. Compared in sRGB, which is closer to how
  // differences are perceived than linear light.
  int count = int(data.paletteInfo.x);
  if (count > 0 && !done) {
    float ditherAmount = final && !isSky ? 0.0 : data.paletteInfo.y; // full-resolution sprites: no screen-space dither
    float3 c = pow(saturate(color), 1.0 / 2.2) + (bayer4(sp) - 0.5) * ditherAmount * 0.12;
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
