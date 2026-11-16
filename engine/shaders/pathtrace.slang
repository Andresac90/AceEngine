// Path-traced scene pass. A fragment shader walks a software BVH, so it runs on
// every backend and device without ray-tracing hardware; pixel-art resolutions
// keep the ray count tiny. Output matches the raster pass: linear HDR color in
// target 0 (alpha = accumulation blend weight) and normal/depth in target 1.
#include "common.slangh"

struct TraceData {
  float4x4 invViewProj;
  float4x4 view;
  float4 lightDir;
  float4 lightColor;
  float4 skyTop;
  float4 skyBottom;
  float4 params; // x: accumulated frames, y: samples per pixel, z: bounces, w: toon bands
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
};

[[vk::binding(0, 0)]] ConstantBuffer<TraceData> data;
[[vk::binding(1, 0)]] StructuredBuffer<BvhNode> nodes;
[[vk::binding(2, 0)]] StructuredBuffer<Triangle> triangles;

struct VsOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
};
struct FsOut {
  float4 color : SV_Target0;
  float4 normalDepth : SV_Target1;
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

// Closest hit, or any hit when `anyHit` is set (shadow rays).
bool trace(float3 origin, float3 dir, float tMax, bool anyHit, out Hit hit) {
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
      if (t > 1e-4 && t < hit.t) {
        hit.t = t;
        hit.tri = i;
        hit.u = u;
        hit.v = v;
        found = true;
        if (anyHit) return true;
      }
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

  Hit primary;
  if (!trace(origin, dir, 1e30, false, primary)) return o; // sky: filled in by the present pass
  Triangle ptri = triangles[primary.tri];
  float3 pn = shadingNormal(ptri, primary, dir);
  float3 ppos = origin + dir * primary.t;
  o.normalDepth = float4(pn, -mul(data.view, float4(ppos, 1.0)).z);

  uint rng = uint(i.position.x) * 1973u + uint(i.position.y) * 9277u + frame * 26699u + 1u;
  int spp = max(1, int(data.params.y));
  int bounces = max(1, int(data.params.z));
  float bands = data.params.w;
  float3 total = 0;
  for (int s = 0; s < spp; s++) {
    // Every sample restarts at the primary hit: no anti-aliasing on purpose, the
    // pixel grid must stay hard.
    float3 pos = ppos, n = pn;
    Triangle tri = ptri;
    float3 throughput = 1, radiance = 0;
    for (int b = 0; b < bounces; b++) {
      radiance += throughput * tri.emissive.rgb;
      throughput *= tri.color.rgb;
      // Direct sun light (next-event estimation).
      float ndl = dot(n, data.lightDir.xyz);
      if (ndl > 0.0) {
        Hit sh;
        if (!trace(pos + n * 1e-3, data.lightDir.xyz, 1e30, true, sh)) {
          float direct = ndl;
          if (b == 0 && bands > 0.5) direct = ceil(ndl * bands - 0.001) / bands; // toon bands on first hit
          radiance += throughput * data.lightColor.rgb * direct * 0.318;
        }
      }
      float3 next = cosineSample(n, rng);
      Hit h;
      if (!trace(pos + n * 1e-3, next, 1e30, false, h)) {
        radiance += throughput * skyColor(next);
        break;
      }
      tri = triangles[h.tri];
      pos = pos + n * 1e-3 + next * h.t;
      n = shadingNormal(tri, h, next);
    }
    total += min(radiance, 4.0); // firefly clamp: slightly biased, removes single-pixel sparkles
  }
  o.color.rgb = total / float(spp);
  return o;
}
