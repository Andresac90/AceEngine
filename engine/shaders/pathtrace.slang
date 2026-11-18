// Path-traced scene pass. A fragment shader walks a software BVH, so it runs on
// every backend and device without ray-tracing hardware; pixel-art resolutions
// keep the ray count tiny. Output matches the raster pass: linear HDR color in
// target 0 (alpha = accumulation blend weight) and normal/depth in target 1.
//
// It follows the same pixel-art rules as mesh.slang: unfiltered textures (from
// one atlas), optional world-space texel mapping, cutout sprites, and lighting
// gathered at the texel centre with a per-texel random seed, so soft shadows
// and bounce light land on the texture's pixel grid instead of the screen's.
#include "common.slangh"

struct TraceData {
  float4x4 invViewProj;
  float4x4 view;
  float4 lightDir;
  float4 lightColor;
  float4 skyTop;
  float4 skyBottom;
  float4 params;    // x: accumulated frames, y: samples per pixel, z: bounces, w: toon bands
  float4 lightInfo; // x: number of emissive triangles, y: texels per world unit, z: texel-space lighting on, w: primary rays skip sprites
  float4 ramp;      // x: ramp shading on, y: shadow steps, z: luminance of a fully lit white surface
  float4 pointInfo; // x: number of point lights
  float4 pointPosition[8]; // xyz: position, w: range
  float4 pointColor[8];    // rgb * intensity
};
struct BvhNode {
  float4 minLeft;  // xyz: box min, w (bits): left child, or first triangle for leaves
  float4 maxCount; // xyz: box max, w (bits): triangle count (0 = interior node)
};
struct Triangle {
  float4 v0, e1, e2;
  float4 n0, n1, n2;
  float4 color;
  float4 emissive;
  float4 uv01;     // uv of vertex 0 (xy) and vertex 1 (zw)
  float4 uv2Flags; // xy: uv of vertex 2, z: flags (1 world texel mapping, 2 sprite/cutout, 4 textured)
  float4 atlas;    // xy: first texel of this texture in the atlas, zw: its size in texels
};

[[vk::binding(0, 0)]] ConstantBuffer<TraceData> data;
[[vk::binding(1, 0)]] StructuredBuffer<BvhNode> nodes;
[[vk::binding(2, 0)]] StructuredBuffer<Triangle> triangles;
[[vk::binding(3, 0)]] StructuredBuffer<float4> lights; // per emissive triangle: x (bits) = triangle index, y = area
[[vk::binding(4, 0)]] Texture2D atlasTexture;
[[vk::binding(5, 0)]] Texture2D paletteTexture; // ramp shading

struct VsOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
};
// Light and surface are kept apart so the light can be denoised without
// blurring textures (pt_denoise.slang), and palette ramps are chosen from the
// denoised light (pt_compose.slang).
struct FsOut {
  float4 color : SV_Target0;       // light arriving at the surface (averaged over frames)
  float4 normalDepth : SV_Target1;
  float4 albedo : SV_Target2;      // rgb: surface color, a: 1 = may use palette ramps (0 for sprites)
  float4 emission : SV_Target3;    // light the surface gives off itself
};

VsOut vsMain(uint id : SV_VertexID) {
  VsOut o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.position = float4(o.uv.x * 2.0 - 1.0, 1.0 - o.uv.y * 2.0, 0.0, 1.0);
  return o;
}

uint pcg(inout uint state) {
  state = state * 747796405u + 2891336453u;
  uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}
float rand(inout uint state) { return float(pcg(state) >> 8) * (1.0 / 16777216.0); }

int flagsOf(Triangle t) { return int(t.uv2Flags.z); }
float3 faceNormal(Triangle t) { return normalize(cross(t.e1.xyz, t.e2.xyz)); }

// Integer texel coordinate of a point on the triangle (same mapping as mesh.slang).
float2 texelAt(Triangle t, float3 pos, float u, float v) {
  if ((flagsOf(t) & 1) != 0) {
    float3 a = abs(faceNormal(t));
    float2 plane = a.y >= a.x && a.y >= a.z ? pos.xz : (a.x >= a.z ? float2(-pos.z, -pos.y) : float2(pos.x, -pos.y));
    return floor(plane * data.lightInfo.y);
  }
  float2 uv = t.uv01.xy * (1.0 - u - v) + t.uv01.zw * u + t.uv2Flags.xy * v;
  return floor(uv * t.atlas.zw);
}

// Unfiltered, repeating lookup of the triangle's texture inside the atlas.
float4 texelColor(Triangle t, float2 texel) {
  if ((flagsOf(t) & 4) == 0) return float4(1, 1, 1, 1);
  float2 wrapped = texel - floor(texel / t.atlas.zw) * t.atlas.zw;
  return atlasTexture.Load(int3(int2(t.atlas.xy + wrapped), 0));
}

struct Hit {
  float t;
  uint tri;
  float u, v;
};

bool hitBox(float3 origin, float3 invDir, float3 bmin, float3 bmax, float tMax) {
  float3 t0 = (bmin - origin) * invDir;
  float3 t1 = (bmax - origin) * invDir;
  float3 lo = min(t0, t1), hi = max(t0, t1);
  float tNear = max(max(lo.x, lo.y), max(lo.z, 0.0));
  float tFar = min(min(hi.x, hi.y), min(hi.z, tMax));
  return tNear <= tFar;
}

// Closest hit, or any hit when `anyHit` is set (shadow rays). `skipSprites`
// ignores sprite cards (probe captures: sprites are drawn separately).
bool trace(float3 origin, float3 dir, float tMax, bool anyHit, out Hit hit, bool skipSprites = false) {
  hit.t = tMax;
  hit.tri = 0;
  hit.u = 0;
  hit.v = 0;
  bool found = false;
  float3 invDir = 1.0 / dir;
  uint stack[32];
  int sp = 0;
  stack[sp++] = 0;
  while (sp > 0) {
    BvhNode node = nodes[stack[--sp]];
    if (!hitBox(origin, invDir, node.minLeft.xyz, node.maxCount.xyz, hit.t)) continue;
    uint first = asuint(node.minLeft.w);
    uint count = asuint(node.maxCount.w);
    if (count == 0) {
      if (sp < 30) {
        stack[sp++] = first;
        stack[sp++] = first + 1;
      }
      continue;
    }
    for (uint i = first; i < first + count; i++) {
      Triangle tri = triangles[i];
      // Moller-Trumbore
      float3 p = cross(dir, tri.e2.xyz);
      float det = dot(tri.e1.xyz, p);
      if (abs(det) < 1e-9) continue;
      float inv = 1.0 / det;
      float3 s = origin - tri.v0.xyz;
      float u = dot(s, p) * inv;
      if (u < 0.0 || u > 1.0) continue;
      float3 q = cross(s, tri.e1.xyz);
      float v = dot(dir, q) * inv;
      if (v < 0.0 || u + v > 1.0) continue;
      float t = dot(tri.e2.xyz, q) * inv;
      if (t <= 1e-4 || t >= hit.t) continue;
      if (skipSprites && (flagsOf(tri) & 2) != 0) continue;
      // Sprites: transparent texels let the ray through.
      if ((flagsOf(tri) & 2) != 0 && texelColor(tri, texelAt(tri, origin + dir * t, u, v)).a < 0.5) continue;
      hit.t = t;
      hit.tri = i;
      hit.u = u;
      hit.v = v;
      found = true;
      if (anyHit) return true;
    }
  }
  return found;
}

float3 skyColor(float3 dir) { return lerp(data.skyBottom.rgb, data.skyTop.rgb, saturate(dir.y * 0.5 + 0.5)); }

float3 shadingNormal(Triangle tri, Hit hit, float3 dir) {
  float3 n = normalize(tri.n0.xyz * (1.0 - hit.u - hit.v) + tri.n1.xyz * hit.u + tri.n2.xyz * hit.v);
  return dot(n, dir) > 0.0 ? -n : n;
}

float3 cosineSample(float3 n, inout uint rng) {
  float a = 6.2831853 * rand(rng);
  float z = rand(rng) * 2.0 - 1.0;
  float r = sqrt(max(0.0, 1.0 - z * z));
  return normalize(n + float3(r * cos(a), r * sin(a), z) * 0.999);
}

// Moves a point on the triangle to the centre of the texture texel it lies in.
float3 snapToTexel(Triangle t, float3 pos, float2 texel) {
  if ((flagsOf(t) & 1) != 0) {
    float3 n = faceNormal(t);
    float3 a = abs(n);
    float2 c = (texel + 0.5) / data.lightInfo.y;
    float3 v0 = t.v0.xyz;
    if (a.y >= a.x && a.y >= a.z) {
      pos.x = c.x;
      pos.z = c.y;
      pos.y = v0.y - (n.x * (pos.x - v0.x) + n.z * (pos.z - v0.z)) / n.y;
    } else if (a.x >= a.z) {
      pos.z = -c.x;
      pos.y = -c.y;
      pos.x = v0.x - (n.y * (pos.y - v0.y) + n.z * (pos.z - v0.z)) / n.x;
    } else {
      pos.x = c.x;
      pos.y = -c.y;
      pos.z = v0.z - (n.x * (pos.x - v0.x) + n.y * (pos.y - v0.y)) / n.z;
    }
    return pos;
  }
  // UV-mapped: find the barycentric coordinates of the texel centre.
  float2 target = (texel + 0.5) / t.atlas.zw - t.uv01.xy;
  float2 d1 = t.uv01.zw - t.uv01.xy, d2 = t.uv2Flags.xy - t.uv01.xy;
  float det = d1.x * d2.y - d1.y * d2.x;
  if (abs(det) < 1e-12) return pos;
  float u = (target.x * d2.y - target.y * d2.x) / det;
  float v = (d1.x * target.y - d1.y * target.x) / det;
  return t.v0.xyz + t.e1.xyz * u + t.e2.xyz * v;
}

FsOut fsMain(VsOut i) {
  float2 ndc = float2(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0);
  float4 nearP = mul(data.invViewProj, float4(ndc, 0.0, 1.0));
  float4 farP = mul(data.invViewProj, float4(ndc, 1.0, 1.0));
  float3 origin = nearP.xyz / nearP.w;
  float3 dir = normalize(farP.xyz / farP.w - origin);

  FsOut o;
  uint frame = uint(data.params.x);
  o.color = float4(0, 0, 0, 1.0 / float(frame + 1));
  o.normalDepth = float4(0, 1, 0, 10000.0);
  o.albedo = float4(0, 0, 0, 0);
  o.emission = float4(0, 0, 0, 0);

  Hit primary;
  if (!trace(origin, dir, 1e30, false, primary, data.lightInfo.w > 0.5)) return o; // sky: filled in by the present pass
  Triangle ptri = triangles[primary.tri];
  float3 pn = shadingNormal(ptri, primary, dir);
  float3 ppos = origin + dir * primary.t;
  o.normalDepth = float4(pn, -mul(data.view, float4(ppos, 1.0)).z);

  float2 ptexel = texelAt(ptri, ppos, primary.u, primary.v);
  // The first surface is traced as white: its color is applied after denoising.
  o.albedo = float4(ptri.color.rgb * texelColor(ptri, ptexel).rgb, (flagsOf(ptri) & 2) == 0 ? 1.0 : 0.0);
  o.emission = float4(ptri.emissive.rgb, 0.0);
  float3 palbedo = float3(1, 1, 1);
  uint rng = uint(i.position.x) * 1973u + uint(i.position.y) * 9277u + frame * 26699u + 1u;
  if ((flagsOf(ptri) & 4) != 0 && data.lightInfo.z > 0.5) {
    // Texel-space lighting: every screen pixel showing this texel gathers light
    // from the same point with the same random numbers, so they agree exactly.
    ppos = snapToTexel(ptri, ppos, ptexel);
    rng = (asuint(ppos.x) * 73856093u) ^ (asuint(ppos.y) * 19349663u) ^ (asuint(ppos.z) * 83492791u) + frame * 26699u + 1u;
  }

  int spp = max(1, int(data.params.y));
  int bounces = max(1, int(data.params.z));
  float bands = data.params.w;
  uint lightCount = uint(data.lightInfo.x);
  float3 total = 0;
  for (int s = 0; s < spp; s++) {
    // Every sample restarts at the primary hit: no anti-aliasing on purpose, the
    // pixel grid must stay hard.
    float3 pos = ppos, n = pn;
    Triangle tri = ptri;
    float3 albedo = palbedo;
    float3 throughput = 1, radiance = 0;
    for (int b = 0; b < bounces; b++) {
      // Emission seen directly goes to the emission output; light reaching other
      // surfaces comes from sampling the emissive triangles below. Counting it
      // on bounce hits as well would double it (and brings back sparkles).
      throughput *= albedo;
      // A sprite is a flat drawing, not a surface: it takes light as if facing it.
      bool sprite = (flagsOf(tri) & 2) != 0;
      float3 offset = (sprite ? data.lightDir.xyz * 0.75 : n * 1e-3);
      // Direct sun light (next-event estimation).
      float ndl = sprite ? 0.8 : dot(n, data.lightDir.xyz);
      if (ndl > 0.0) {
        Hit sh;
        if (!trace(pos + offset, data.lightDir.xyz, 1e30, true, sh)) {
          float direct = ndl;
          if (b == 0 && bands > 0.5 && !sprite) direct = ceil(ndl * bands - 0.001) / bands; // toon bands on first hit
          radiance += throughput * data.lightColor.rgb * direct * 0.318;
        }
      }
      // Point lights (lamps, torches): same falloff and toon steps as the raster
      // path, plus real shadows.
      for (int k = 0; k < int(data.pointInfo.x); k++) {
        float3 toLamp = data.pointPosition[k].xyz - pos;
        float dist = length(toLamp);
        float fade = saturate(1.0 - dist / data.pointPosition[k].w);
        if (fade <= 0.0) continue;
        toLamp /= max(dist, 1e-4);
        float facing = sprite ? 0.8 : dot(n, toLamp);
        if (facing <= 0.0) continue;
        Hit lh;
        if (trace(pos + (sprite ? toLamp * 0.75 : n * 1e-3), toLamp, max(dist - (sprite ? 0.76 : 2e-3), 1e-3), true, lh)) continue;
        float amount = facing * fade * fade;
        if (b == 0 && bands > 0.5 && !sprite) amount = ceil(amount * bands - 0.001) / bands;
        radiance += throughput * data.pointColor[k].rgb * amount * 0.318;
      }
      // Direct light from one randomly chosen emissive triangle.
      if (lightCount > 0) {
        float4 light = lights[min(uint(rand(rng) * float(lightCount)), lightCount - 1)];
        Triangle lt = triangles[asuint(light.x)];
        float su = sqrt(rand(rng));
        float3 lp = lt.v0.xyz + lt.e1.xyz * (1.0 - su) + lt.e2.xyz * (rand(rng) * su); // uniform point on the triangle
        float3 toLight = lp - pos;
        float dist2 = max(dot(toLight, toLight), 1e-4);
        float dist = sqrt(dist2);
        toLight /= dist;
        float cosSurface = sprite ? 0.8 : dot(n, toLight);
        float cosLight = abs(dot(faceNormal(lt), toLight));
        if (cosSurface > 0.0 && cosLight > 0.0) {
          Hit lh;
          if (!trace(pos + (sprite ? toLight * 0.75 : n * 1e-3), toLight, dist - (sprite ? 0.76 : 2e-3), true, lh))
            radiance += throughput * lt.emissive.rgb * (cosSurface * cosLight / dist2 * light.y * float(lightCount) * 0.318);
        }
      }
      float3 next = cosineSample(sprite ? data.lightDir.xyz : n, rng);
      Hit h;
      if (!trace(pos + offset, next, 1e30, false, h)) {
        radiance += throughput * skyColor(next);
        break;
      }
      tri = triangles[h.tri];
      pos = pos + offset + next * h.t;
      n = shadingNormal(tri, h, next);
      albedo = tri.color.rgb * texelColor(tri, texelAt(tri, pos, h.u, h.v)).rgb;
    }
    total += min(radiance, 16.0); // firefly clamp: slightly biased, removes single-pixel sparkles
  }
  o.color.rgb = total / float(spp);
  return o;
}
