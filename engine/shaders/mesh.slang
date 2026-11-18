// Rasterized scene pass: meshes and sprites shaded like pixel art.
// Writes linear HDR color and (world normal, view depth) for the present pass.
//
// What makes it read as pixel art rather than filtered 3D:
//  - textures are sampled unfiltered and can be mapped in world space at a fixed
//    texel density, so every surface shows the same pixel size;
//  - light and shadow are evaluated at texture-texel centers ("texel-space
//    lighting"), so shading edges follow the texture's pixel grid;
//  - light is quantized into a few bands, with ordered dithering between bands
//    that is anchored to the texture's texels.
#include "common.slangh"

[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame;
[[vk::binding(1, 0)]] Texture2D shadowMap; // r: light-space depth of the nearest occluder
[[vk::binding(2, 0)]] Texture2D paletteTexture; // rows are color ramps, darkest on the left; transparent = unused
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
  o.uv = v.uv * draw.uvRect.zw + draw.uvRect.xy;
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

// Toon bands (none if bands is 0) with ordered dithering between them.
float banded(float v, float bands, float jitter) {
  return bands > 0.5 ? clamp(ceil(v * bands - 0.001 + jitter), 0.0, bands) / bands : v;
}

FsOut fsMain(VsOut i) {
  float3 n = normalize(i.normal);
  uint tw, th;
  albedoTexture.GetDimensions(tw, th);
  float2 texSize = float2(tw, th);

  // World-space mapping: project along the dominant normal axis, one texel =
  // 1 / texelsPerUnit world units, whatever the object's scale.
  float2 uv = i.uv;
  if (draw.flags.y > 0.5) {
    float3 a = abs(n);
    float2 plane = a.y >= a.x && a.y >= a.z ? i.worldPos.xz : (a.x >= a.z ? float2(-i.worldPos.z, -i.worldPos.y) : float2(i.worldPos.x, -i.worldPos.y));
    uv = plane * frame.params2.y / texSize;
  }
  float4 albedo = albedoTexture.Sample(albedoSampler, uv) * i.color;
  if (albedo.a < 0.5) discard; // hard-edged cutout: pixel art has no soft alpha

  // Texel-space lighting: move the shading point to the center of the texture
  // texel under this pixel, by solving the screen-space UV derivatives.
  float3 shadePos = i.worldPos;
  bool textured = tw > 1 || th > 1;
  int2 ditherCoord = int2(i.position.xy);
  if (textured) {
    float2 texel = floor(uv * texSize);
    ditherCoord = int2(texel);
    if (frame.params2.x > 0.5) {
      float2 d = (texel + 0.5) / texSize - uv;
      float2 dx = ddx(uv), dy = ddy(uv);
      float det = dx.x * dy.y - dx.y * dy.x;
      if (abs(det) > 1e-12) {
        float2 steps = float2(d.x * dy.y - d.y * dy.x, dx.x * d.y - dx.y * d.x) / det; // in screen pixels
        steps = clamp(steps, -16.0, 16.0); // grazing angles: do not extrapolate far off the surface
        shadePos += ddx(i.worldPos) * steps.x + ddy(i.worldPos) * steps.y;
      }
    }
  }

  // A sprite is lit as one piece: it takes the sun or the shade of the spot it
  // stands on, like a character drawn over a painted background.
  bool sprite = draw.flags.x > 0.5;
  float light = sprite ? sunVisibility(draw.origin.xyz + frame.lightDir.xyz * 0.75, frame.lightDir.xyz)
                       : saturate(dot(n, frame.lightDir.xyz));
  if (!sprite && light > 0.0) light *= sunVisibility(shadePos, n);
  float bands = frame.params.x;
  float jitter = sprite ? 0.0 : (bayer4(ditherCoord) - 0.5) * frame.params.w;
  light = banded(light, bands, jitter);
  float3 radiance = frame.lightColor.rgb * light;

  // Point lights: banded the same way, so pools of light have hard pixel-art steps.
  // Sprites take them at their feet, as a whole, like the sun.
  float3 at = sprite ? draw.origin.xyz : shadePos;
  for (int k = 0; k < int(frame.pointInfo.x); k++) {
    float3 toLamp = frame.pointPosition[k].xyz - at;
    float dist = length(toLamp);
    float fade = saturate(1.0 - dist / frame.pointPosition[k].w);
    float facing = sprite ? 1.0 : saturate(dot(n, toLamp / max(dist, 1e-4)));
    radiance += frame.pointColor[k].rgb * banded(facing * fade * fade, bands, jitter);
  }
  float3 lit = albedo.rgb * (frame.ambient.rgb + radiance * (sprite ? 0.25 : 0.318));
  if (frame.params2.z > 0.5 && !sprite) {
    // Ramp shading takes an amount of light. Fully lit = a white light of
    // intensity 3 (a typical daytime sun), so a dim night sun or a far torch
    // lands on the darker colors of each ramp.
    float amount = dot(radiance, float3(0.2126, 0.7152, 0.0722)) / 3.0;
    lit = rampShade(paletteTexture, albedo.rgb, amount, int(frame.params2.w)); // sprites keep their drawn colors
  }
  FsOut o;
  o.color = float4(lit + draw.emissive.rgb, 1.0);
  o.normalDepth = float4(n, i.viewDepth);
  return o;
}
