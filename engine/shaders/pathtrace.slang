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
#include "crowd.slangh"

struct TraceData {
  float4x4 invViewProj;
  float4x4 view;
  float4 lightDir;
  float4 lightColor;
  float4 params;    // x: accumulated frames, y: samples per pixel, z: bounces, w: toon bands
  float4 lightInfo; // x: number of emissive triangles, y: texels per world unit, z: texel-space lighting on, w: primary rays skip sprites
  float4 ramp;      // x: ramp shading on, y: shadow steps, z: luminance of a fully lit white surface
  float4 pointInfo; // x: number of point lights
  float4 history;   // frames a pixel may average while a crowd moves: x still pixels (0 = unlimited), y pixels a moving person may cover, z pixels that only reflect one
  float4 pointPosition[8]; // xyz: position, w: range
  float4 pointColor[8];    // rgb * intensity
  float4x4 prevViewProj;   // last frame's camera (temporal reprojection)
  float4x4 prevView;
  float4 viewport;         // x, y, width, height of this target's region
  float4 reproject;        // x: 1 = last frame's light is ours to reuse, y: > 0 cut every pixel's frames to this once (scene changed), z: max frames
  Atmosphere atmosphere;   // the same sky the present pass draws, so bounce light matches it
};
struct BvhNode {
  float4 minLeft;  // xyz: box min, w (bits): left child, or first triangle for leaves
  float4 maxCount; // xyz: box max, w (bits): triangle count (0 = interior node)
};
struct Triangle {
  float4 v0, e1, e2; // w: the corner's sRGB vertex color as r + 256 g + 65536 b
  float4 n0, n1, n2;
  float4 color;
  float4 emissive; // rgb: light given off (times the texture color), w: reflectivity (Schlick F0)
  float4 uv01;     // uv of vertex 0 (xy) and vertex 1 (zw)
  float4 uv2Flags; // xy: uv of vertex 2, z: flags (1 world texel mapping, 2 sprite/cutout, 4 textured), w: reflection roughness
  float4 atlas;    // xy: first texel of this texture in the atlas, zw: its size in texels
};

[[vk::binding(0, 0)]] ConstantBuffer<TraceData> data : register(b0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<BvhNode> nodes : register(t1, space0);
[[vk::binding(2, 0)]] StructuredBuffer<Triangle> triangles : register(t2, space0);
[[vk::binding(3, 0)]] StructuredBuffer<float4> lights : register(t3, space0); // per emissive triangle: x (bits) = triangle index, y = area, z = cumulative pick probability, w = pick probability
[[vk::binding(4, 0)]] Texture2D atlasTexture : register(t4, space0);
[[vk::binding(5, 0)]] Texture2D paletteTexture : register(t5, space0); // ramp shading

// The crowd (render/crowd.h): a top level over people, then rest-pose part BVHs.
struct CrowdTri {
  float4 v0, e1, e2; // rest space; v0.w: color code (crowdColor)
  float4 n0, n1, n2;
};
[[vk::binding(0, 1)]] ConstantBuffer<CrowdData> crowd : register(b0, space1);
[[vk::binding(1, 1)]] StructuredBuffer<BvhNode> crowdNodes : register(t1, space1);
[[vk::binding(2, 1)]] StructuredBuffer<CrowdMember> crowdMembers : register(t2, space1);
[[vk::binding(3, 1)]] StructuredBuffer<float4> crowdTable : register(t3, space1);
[[vk::binding(4, 1)]] StructuredBuffer<CrowdTri> crowdTris : register(t4, space1);
static const uint kNoMember = 0xFFFFFFFFu;

// Last frame's output of this target (temporal reprojection).
[[vk::binding(0, 2)]] Texture2D prevLight : register(t0, space2);        // rgb: averaged light, a: frames averaged
[[vk::binding(1, 2)]] Texture2D prevNormalDepth : register(t1, space2);
// Set when a tracked ray crosses a moving person's bounds in front of what it
// hit: that pixel may change next frame, so it keeps a short history.
static bool gTrackCrowd = false;
static bool gCrowdTouched = false;
static bool gCrowdReflected = false; // the same, seen in a reflection
// Secondary rays (shadows, bounces) see each body part as one box in its rest
// pose, moved by the current pose, instead of its triangles: a proxy, like
// the simplified scenes games trace for indirect light. Camera rays and
// reflections stay exact.
static bool gCrowdProxy = false;
static const uint kProxyTri = 0xFFFFFFFEu;

struct VsOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
};
// Light and surface are kept apart so the light can be denoised without
// blurring textures (pt_denoise.slang), and palette ramps are chosen from the
// denoised light (pt_compose.slang).
struct FsOut {
  float4 color : SV_Target0;       // rgb: light arriving at the surface (averaged over frames), a: frames averaged
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

// Linear surface color at barycentrics (u, v): the material color times the
// interpolated vertex colors, as mesh.slang does (vertex colors are sRGB).
float3 surfaceColor(Triangle t, float u, float v) {
  float3 c[3];
  float packed[3] = {t.v0.w, t.e1.w, t.e2.w};
  for (int k = 0; k < 3; k++) {
    uint bits = uint(packed[k]);
    c[k] = pow(float3(bits & 255u, (bits >> 8) & 255u, (bits >> 16) & 255u) / 255.0, 2.2);
  }
  return t.color.rgb * (c[0] * (1.0 - u - v) + c[1] * u + c[2] * v);
}

// Unfiltered, repeating lookup of the triangle's texture inside the atlas.
float4 texelColor(Triangle t, float2 texel) {
  if ((flagsOf(t) & 4) == 0) return float4(1, 1, 1, 1);
  float2 wrapped = texel - floor(texel / t.atlas.zw) * t.atlas.zw;
  return atlasTexture.Load(int3(int2(t.atlas.xy + wrapped), 0));
}

struct Hit {
  float t;
  uint tri;    // scene triangle, or crowd triangle when member != kNoMember
  float u, v;
  uint member; // crowd person hit, or kNoMember
  uint bone;
};

// Entry distance into the box, or a huge number on a miss.
float boxEntry(float3 origin, float3 invDir, float3 bmin, float3 bmax, float tMax) {
  float3 t0 = (bmin - origin) * invDir;
  float3 t1 = (bmax - origin) * invDir;
  float3 lo = min(t0, t1), hi = max(t0, t1);
  float tNear = max(max(lo.x, lo.y), max(lo.z, 0.0));
  float tFar = min(min(hi.x, hi.y), min(hi.z, tMax));
  return tNear <= tFar ? tNear : 1e30;
}

// One rest-pose part: the ray is already in that part's rest space. A rigid
// map keeps the ray straight and keeps its parameter t, so hits compare
// directly with the rest of the scene.
bool tracePart(uint root, float3 origin, float3 dir, bool anyHit, inout Hit hit, uint member, uint bone) {
  float3 invDir = 1.0 / dir;
  uint stack[16];
  int sp = 0;
  bool found = false;
  stack[sp++] = root;
  while (sp > 0) {
    BvhNode node = crowdNodes[stack[--sp]];
    if (boxEntry(origin, invDir, node.minLeft.xyz, node.maxCount.xyz, hit.t) > 1e29) continue;
    uint first = asuint(node.minLeft.w), count = asuint(node.maxCount.w);
    if (count == 0) {
      if (sp < 14) { stack[sp++] = first + 1; stack[sp++] = first; }
      continue;
    }
    for (uint i = first; i < first + count; i++) {
      CrowdTri tri = crowdTris[i];
      float3 p = cross(dir, tri.e2.xyz);
      float det = dot(tri.e1.xyz, p);
      if (abs(det) < 1e-12) continue;
      float inv = 1.0 / det;
      float3 s = origin - tri.v0.xyz;
      float u = dot(s, p) * inv;
      if (u < 0.0 || u > 1.0) continue;
      float3 q = cross(s, tri.e1.xyz);
      float v = dot(dir, q) * inv;
      if (v < 0.0 || u + v > 1.0) continue;
      float t = dot(tri.e2.xyz, q) * inv;
      if (t <= 1e-4 || t >= hit.t) continue;
      hit.t = t;
      hit.tri = i;
      hit.u = u;
      hit.v = v;
      hit.member = member;
      hit.bone = bone;
      found = true;
      if (anyHit) return true;
    }
  }
  return found;
}

// One person: into character space (yaw, scale), then per bone into rest space
// with the inverse of that bone's pose, evaluated now (crowd.slangh).
// Table index of a bone's box in character space, valid for the pose shown
// now: a held frame has its own exact box; a crossfade uses the pair's box.
uint crowdBoneBox(CrowdAnim anim, uint bone) {
  if (anim.weight <= 0.0) return anim.frameA + bone * 4u + 2u;
  if (anim.weight >= 1.0) return anim.frameB + bone * 4u + 2u;
  uint clips = uint(crowd.counts.x);
  return uint(crowd.bounds.z) + (((anim.model * clips + anim.clipA) * clips + anim.clipB) * kCrowdBones + bone) * 2u;
}

bool traceMember(uint mi, float3 origin, float3 dir, bool anyHit, inout Hit hit) {
  CrowdMember m = crowdMembers[mi];
  float invScale = 1.0 / m.posScale.w;
  float3 oc = crowdDirToCharacter(m, origin - m.posScale.xyz) * invScale;
  float3 dc = crowdDirToCharacter(m, dir) * invScale;
  float3 invDc = 1.0 / dc;
  CrowdAnim anim = crowdAnim(crowdTable, crowd, m);
  bool found = false;
  for (uint bone = 0; bone < kCrowdBones; bone++) {
    // Part header: x (bits) its rest-pose BVH root, y 1 = the model has this part.
    float4 part = crowdTable[uint(crowd.time.z) + anim.model * kCrowdBones + bone];
    if (part.y < 0.5) continue;
    // The bone's box for the pose shown now: missing it skips the pose
    // evaluation and the triangles.
    uint box = crowdBoneBox(anim, bone);
    float tBox = boxEntry(oc, invDc, crowdTable[box].xyz, crowdTable[box + 1u].xyz, hit.t);
    if (tBox > 1e29) continue;
    if (anyHit || gCrowdProxy) {
      // Secondary rays stop at the box itself (a proxy, see gCrowdProxy). A
      // ray starting inside it (leaving this body part) passes through.
      if (tBox > 1e-4) {
        hit.t = tBox;
        hit.tri = kProxyTri;
        hit.u = hit.v = 0.0;
        hit.member = mi;
        hit.bone = bone;
        found = true;
        if (anyHit) return true;
      }
      continue;
    }
    CrowdPose p = crowdBonePose(crowdTable, crowd, anim, bone);
    float4 inverse = float4(-p.q.xyz, p.q.w);
    float3 orr = quatRotate(inverse, oc - p.t), drr = quatRotate(inverse, dc);
    if (tracePart(asuint(part.x), orr, drr, anyHit, hit, mi, bone)) {
      found = true;
      if (anyHit) return true;
    }
  }
  return found;
}

// Both top levels: seated people (built once over pose-conservative boxes)
// and the moving actors (rebuilt every frame on the CPU).
bool traceCrowd(float3 origin, float3 dir, bool anyHit, inout Hit hit) {
  if (crowd.time.y < 0.5) return false;
  float3 invDir = 1.0 / dir;
  bool found = false;
  for (int level = 0; level < 2; level++) {
    uint root = level == 0 ? 0u : uint(max(crowd.counts.y, 0.0));
    if (level == 1 && crowd.counts.y < 0.0) break;
    uint stack[32];
    float stackT[32];
    int sp = 0;
    BvhNode r = crowdNodes[root];
    float tr = boxEntry(origin, invDir, r.minLeft.xyz, r.maxCount.xyz, hit.t);
    if (tr > 1e29) continue;
    stack[sp] = root;
    stackT[sp++] = tr;
    while (sp > 0) {
      sp--;
      if (stackT[sp] > hit.t) continue;
      BvhNode node = crowdNodes[stack[sp]];
      uint first = asuint(node.minLeft.w), count = asuint(node.maxCount.w);
      if (count == 0) {
        BvhNode a = crowdNodes[first], b = crowdNodes[first + 1];
        float ta = boxEntry(origin, invDir, a.minLeft.xyz, a.maxCount.xyz, hit.t);
        float tb = boxEntry(origin, invDir, b.minLeft.xyz, b.maxCount.xyz, hit.t);
        uint na = first, nb = first + 1;
        if (tb < ta) {
          float tt = ta; ta = tb; tb = tt;
          na = first + 1; nb = first;
        }
        if (tb < 1e29 && sp < 31) { stack[sp] = nb; stackT[sp++] = tb; }
        if (ta < 1e29 && sp < 31) { stack[sp] = na; stackT[sp++] = ta; }
        continue;
      }
      if (gTrackCrowd) gCrowdTouched = true; // this person's box lies in front of the closest hit so far
      for (uint i = first; i < first + count; i++) {
        if (traceMember(i, origin, dir, anyHit, hit)) {
          found = true;
          if (anyHit) return true;
        }
      }
    }
  }
  return found;
}

// Closest hit, or any hit when `anyHit` is set (shadow rays). `skipSprites`
// ignores sprite cards (probe captures: sprites are drawn separately).
bool trace(float3 origin, float3 dir, float tMax, bool anyHit, out Hit hit, bool skipSprites = false) {
  hit.t = tMax;
  hit.tri = 0;
  hit.u = 0;
  hit.v = 0;
  hit.member = kNoMember;
  hit.bone = 0;
  bool found = false;
  float3 invDir = 1.0 / dir;
  // Front to back: the nearer child first, so closest hits shrink hit.t early
  // and whole subtrees behind them are skipped. Stack entries carry the entry
  // distance, re-checked when popped.
  uint stack[48];
  float stackT[48];
  int sp = 0;
  BvhNode root = nodes[0];
  stack[0] = 0;
  stackT[0] = 0.0;
  if (boxEntry(origin, invDir, root.minLeft.xyz, root.maxCount.xyz, hit.t) <= 1e29) {
    stack[sp] = 0;
    stackT[sp++] = 0.0;
  }
  while (sp > 0) {
    sp--;
    if (stackT[sp] > hit.t) continue;
    BvhNode node = nodes[stack[sp]];
    uint first = asuint(node.minLeft.w);
    uint count = asuint(node.maxCount.w);
    if (count == 0) {
      BvhNode a = nodes[first], b = nodes[first + 1];
      float ta = boxEntry(origin, invDir, a.minLeft.xyz, a.maxCount.xyz, hit.t);
      float tb = boxEntry(origin, invDir, b.minLeft.xyz, b.maxCount.xyz, hit.t);
      uint na = first, nb = first + 1;
      if (tb < ta) { // a = nearer
        float tt = ta; ta = tb; tb = tt;
        na = first + 1; nb = first;
      }
      if (tb < 1e29 && sp < 47) { stack[sp] = nb; stackT[sp++] = tb; }
      if (ta < 1e29 && sp < 47) { stack[sp] = na; stackT[sp++] = ta; }
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
  if (traceCrowd(origin, dir, anyHit, hit)) found = true;
  return found;
}


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

// What a ray hit, ready to shade: scene triangles and crowd people alike.
struct Surface {
  float3 pos, n;     // hit point (moved to its texel centre when snapped), normal facing the ray
  float3 albedo;     // linear surface color (texture included)
  float3 emission;
  float reflectivity, roughness;
  bool sprite;       // a flat drawing lit as a whole (cutout sprite cards)
  bool seeded;       // texel-space lighting applies: (seed, seedLow) identify the texel
  uint seed, seedLow; // the random seed is seed ^ (seedLow + frame * 26699 + 1)
};

// Crowd person at a hit: rest-space triangle -> world through the bone pose.
// With `snap`, the point moves to the centre of its rest-space texel (texels
// projected along the triangle's dominant axis), so lighting pixels stick to
// the body however it bends.
// A proxy hit (see gCrowdProxy): the face of the bone's box, in the person's main color.
Surface crowdProxySurface(Hit h, float3 origin, float3 dir) {
  CrowdMember m = crowdMembers[h.member];
  uint box = crowdBoneBox(crowdAnim(crowdTable, crowd, m), h.bone);
  float3 lo = crowdTable[box].xyz, hi = crowdTable[box + 1u].xyz;
  Surface s;
  s.pos = origin + dir * h.t;
  float3 local = crowdDirToCharacter(m, s.pos - m.posScale.xyz) / m.posScale.w;
  float3 d = (local - (lo + hi) * 0.5) / max((hi - lo) * 0.5, 1e-4);
  float3 a = abs(d);
  float3 nLocal = a.x >= a.y && a.x >= a.z ? float3(sign(d.x), 0, 0) : (a.y >= a.z ? float3(0, sign(d.y), 0) : float3(0, 0, sign(d.z)));
  s.n = crowdDirToWorld(m, nLocal);
  if (dot(s.n, dir) > 0.0) s.n = -s.n;
  s.albedo = crowdColor(-1.0, m);
  s.emission = 0;
  s.reflectivity = 0;
  s.roughness = 0;
  s.sprite = false;
  s.seeded = false;
  s.seed = 0;
  s.seedLow = 0;
  return s;
}

Surface crowdSurface(Hit h, float3 dir, bool snap) {
  CrowdMember m = crowdMembers[h.member];
  CrowdTri tri = crowdTris[h.tri];
  CrowdPose p = crowdPose(crowdTable, crowd, m, h.bone);
  float u = h.u, v = h.v;
  float3 rest = tri.v0.xyz + tri.e1.xyz * u + tri.e2.xyz * v;
  Surface s;
  s.seeded = false;
  s.seed = 0;
  s.seedLow = 0;
  if (snap && data.lightInfo.z > 0.5) {
    float3 fn = cross(tri.e1.xyz, tri.e2.xyz);
    float3 a = abs(fn);
    float tpu = data.lightInfo.y * 1.0;
    int axis = a.y >= a.x && a.y >= a.z ? 1 : (a.x >= a.z ? 0 : 2);
    float3 c = rest;
    int2 texel;
    if (axis == 1) { texel = int2(floor(rest.xz * tpu)); c.xz = (float2(texel) + 0.5) / tpu; c.y = tri.v0.y - (fn.x * (c.x - tri.v0.x) + fn.z * (c.z - tri.v0.z)) / fn.y; }
    else if (axis == 0) { texel = int2(floor(rest.zy * tpu)); c.zy = (float2(texel) + 0.5) / tpu; c.x = tri.v0.x - (fn.y * (c.y - tri.v0.y) + fn.z * (c.z - tri.v0.z)) / fn.x; }
    else { texel = int2(floor(rest.xy * tpu)); c.xy = (float2(texel) + 0.5) / tpu; c.z = tri.v0.z - (fn.x * (c.x - tri.v0.x) + fn.y * (c.y - tri.v0.y)) / fn.z; }
    // Barycentrics of the texel centre, kept inside the triangle.
    float3 d = c - tri.v0.xyz;
    float d11 = dot(tri.e1.xyz, tri.e1.xyz), d12 = dot(tri.e1.xyz, tri.e2.xyz), d22 = dot(tri.e2.xyz, tri.e2.xyz);
    float b1 = dot(d, tri.e1.xyz), b2 = dot(d, tri.e2.xyz), den = d11 * d22 - d12 * d12;
    if (abs(den) > 1e-20) {
      u = saturate((d22 * b1 - d12 * b2) / den);
      v = saturate((d11 * b2 - d12 * b1) / den);
      if (u + v > 1.0) { float k = 1.0 / (u + v); u *= k; v *= k; }
      rest = tri.v0.xyz + tri.e1.xyz * u + tri.e2.xyz * v;
    }
    s.seeded = true;
    s.seed = pcgHash(uint(texel.x) * 73856093u ^ uint(texel.y) * 19349663u ^ (h.member * 9u + h.bone) * 83492791u ^ uint(axis));
  }
  float3 nRest = normalize(tri.n0.xyz * (1.0 - u - v) + tri.n1.xyz * u + tri.n2.xyz * v);
  s.pos = crowdToWorld(m, quatRotate(p.q, rest) + p.t);
  s.n = crowdDirToWorld(m, quatRotate(p.q, nRest));
  if (dot(s.n, dir) > 0.0) s.n = -s.n;
  s.albedo = crowdColor(tri.v0.w, m);
  s.emission = 0;
  s.reflectivity = 0;
  s.roughness = 0;
  s.sprite = false;
  return s;
}

Surface describe(Hit h, float3 origin, float3 dir, bool snap) {
  if (h.member != kNoMember) return h.tri == kProxyTri ? crowdProxySurface(h, origin, dir) : crowdSurface(h, dir, snap);
  Triangle tri = triangles[h.tri];
  Surface s;
  s.pos = origin + dir * h.t;
  s.n = shadingNormal(tri, h, dir);
  float2 texel = texelAt(tri, s.pos, h.u, h.v);
  float3 tex = texelColor(tri, texel).rgb;
  s.albedo = surfaceColor(tri, h.u, h.v) * tex;
  s.emission = tri.emissive.rgb * tex; // emissive textures (screens, ribbon boards) glow in their own colors
  s.reflectivity = tri.emissive.w;
  s.roughness = tri.uv2Flags.w;
  s.sprite = (flagsOf(tri) & 2) != 0;
  s.seeded = false;
  s.seed = 0;
  s.seedLow = 0;
  if (snap && (flagsOf(tri) & 4) != 0 && data.lightInfo.z > 0.5) {
    s.pos = snapToTexel(tri, s.pos, texel);
    s.seeded = true;
    s.seed = (asuint(s.pos.x) * 73856093u) ^ (asuint(s.pos.y) * 19349663u);
    s.seedLow = asuint(s.pos.z) * 83492791u;
  }
  return s;
}

// Emissive triangle for a uniform random number: binary search of the
// cumulative probabilities (proportional to area x brightness), so big bright
// panels get most samples and tiny screen pixels few.
uint pickLight(float r, uint count) {
  uint lo = 0, hi = count - 1;
  while (lo < hi) {
    uint mid = (lo + hi) / 2;
    if (lights[mid].z < r) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

// Light arriving at a surface point straight from the sun, the point lights
// and one emissive triangle (next-event estimation), all shadowed. `bandIt`:
// toon bands on the first hit, as in the raster path.
float3 directLight(float3 pos, float3 n, bool sprite, bool bandIt, inout uint rng) {
  float3 radiance = 0;
  float bands = data.params.w;
  float3 offset = (sprite ? data.lightDir.xyz * 0.75 : n * 1e-3);
  float ndl = sprite ? 0.8 : dot(n, data.lightDir.xyz);
  if (ndl > 0.0) {
    Hit sh;
    if (!trace(pos + offset, data.lightDir.xyz, 1e30, true, sh)) {
      float direct = ndl;
      if (bandIt && bands > 0.5 && !sprite) direct = ceil(ndl * bands - 0.001) / bands;
      radiance += data.lightColor.rgb * direct * 0.318;
    }
  }
  // Up to 4 point lights: every one is shadow-tested. More (an arena's
  // floods): one is picked, in proportion to how much it could add here,
  // and divided by that probability. Shadow rays are the cost, not the lights.
  int lampCount = int(data.pointInfo.x);
  int firstLamp = 0, lastLamp = lampCount;
  float lampWeight = 1.0;
  if (lampCount > 4) {
    float weights[8];
    float sum = 0.0;
    for (int k = 0; k < lampCount; k++) {
      float d = length(data.pointPosition[k].xyz - pos);
      float f = saturate(1.0 - d / data.pointPosition[k].w);
      weights[k] = f * f * dot(data.pointColor[k].rgb, float3(0.2126, 0.7152, 0.0722)) + 1e-6;
      sum += weights[k];
    }
    float r = rand(rng) * sum;
    int pick = lampCount - 1;
    for (int k = 0; k < lampCount; k++) {
      if (r < weights[k]) { pick = k; break; }
      r -= weights[k];
    }
    firstLamp = pick;
    lastLamp = pick + 1;
    lampWeight = sum / weights[pick];
  }
  for (int k = firstLamp; k < lastLamp; k++) {
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
    if (bandIt && bands > 0.5 && !sprite) amount = ceil(amount * bands - 0.001) / bands;
    radiance += data.pointColor[k].rgb * amount * 0.318 * lampWeight;
  }
  uint lightCount = uint(data.lightInfo.x);
  if (lightCount > 0) {
    float4 light = lights[pickLight(rand(rng), lightCount)];
    Triangle lt = triangles[asuint(light.x)];
    float su = sqrt(rand(rng));
    float bv = rand(rng) * su;
    float3 lp = lt.v0.xyz + lt.e1.xyz * (1.0 - su) + lt.e2.xyz * bv; // uniform point on the triangle
    float3 toLight = lp - pos;
    float dist2 = max(dot(toLight, toLight), 1e-4);
    float dist = sqrt(dist2);
    toLight /= dist;
    float cosSurface = sprite ? 0.8 : dot(n, toLight);
    float cosLight = abs(dot(faceNormal(lt), toLight));
    if (cosSurface > 0.0 && cosLight > 0.0) {
      Hit lh;
      if (!trace(pos + (sprite ? toLight * 0.75 : n * 1e-3), toLight, dist - (sprite ? 0.76 : 2e-3), true, lh)) {
        float3 glow = lt.emissive.rgb * texelColor(lt, texelAt(lt, lp, 1.0 - su, bv)).rgb;
        radiance += glow * (cosSurface * cosLight / dist2 * light.y / light.w * 0.318);
      }
    }
  }
  return radiance;
}

FsOut fsMain(VsOut i) {
  float2 ndc = float2(i.uv.x * 2.0 - 1.0, 1.0 - i.uv.y * 2.0);
  float4 nearP = mul(data.invViewProj, float4(ndc, 0.0, 1.0));
  float4 farP = mul(data.invViewProj, float4(ndc, 1.0, 1.0));
  float3 origin = nearP.xyz / nearP.w;
  float3 dir = normalize(farP.xyz / farP.w - origin);

  FsOut o;
  uint frame = uint(data.params.x);
  o.color = float4(0, 0, 0, 0);
  o.normalDepth = float4(0, 1, 0, 10000.0);
  o.albedo = float4(0, 0, 0, 0);
  o.emission = float4(0, 0, 0, 0);

  gTrackCrowd = true;
  Hit primary;
  bool hitSomething = trace(origin, dir, 1e30, false, primary, data.lightInfo.w > 0.5);
  if (!hitSomething) return o; // sky: filled in by the present pass
  Surface ps = describe(primary, origin, dir, true);
  // Depth of the real hit, not of the texel centre: world-pixel splats are placed from it.
  o.normalDepth = float4(ps.n, -mul(data.view, float4(origin + dir * primary.t, 1.0)).z);
  // The first surface is traced as white: its color is applied after denoising.
  o.albedo = float4(ps.albedo, ps.sprite ? 0.0 : 1.0);
  o.emission = float4(ps.emission, 0.0);
  uint rng = uint(i.position.x) * 1973u + uint(i.position.y) * 9277u + frame * 26699u + 1u;
  // Texel-space lighting: every screen pixel showing this texel gathers light
  // from the same point with the same random numbers, so they agree exactly.
  if (ps.seeded) rng = ps.seed ^ (ps.seedLow + frame * 26699u + 1u);

  int spp = max(1, int(data.params.y));
  int bounces = max(1, int(data.params.z));
  float3 total = 0;
  for (int s = 0; s < spp; s++) {
    // Every sample restarts at the primary hit: no anti-aliasing on purpose, the
    // pixel grid must stay hard.
    Surface surf = ps;
    surf.albedo = float3(1, 1, 1);
    float3 throughput = 1, radiance = 0;
    for (int b = 0; b < bounces; b++) {
      // Emission seen directly goes to the emission output; light reaching other
      // surfaces comes from sampling the emissive triangles. Counting it on
      // bounce hits as well would double it (and brings back sparkles).
      throughput *= surf.albedo;
      gTrackCrowd = false;
      radiance += throughput * directLight(surf.pos, surf.n, surf.sprite, b == 0, rng);
      float3 offset = (surf.sprite ? data.lightDir.xyz * 0.75 : surf.n * 1e-3);
      float3 next = cosineSample(surf.sprite ? data.lightDir.xyz : surf.n, rng);
      Hit h;
      gCrowdProxy = true;
      bool bounced = trace(surf.pos + offset, next, 1e30, false, h);
      gCrowdProxy = false;
      if (!bounced) {
        radiance += throughput * skyRadiance(data.atmosphere, next, false);
        break;
      }
      surf = describe(h, surf.pos + offset, next, false);
    }
    total += min(radiance, 16.0); // firefly clamp: slightly biased, removes single-pixel sparkles
  }
  float3 diffuse = total / float(spp);
  o.color.rgb = diffuse;

  // Reflection (ice): one ray per frame around the mirror direction, blurred
  // by `roughness` as frames accumulate. The direction comes from the camera
  // to the texel centre, so reflections are made of whole texels too.
  if (ps.reflectivity > 0.0 && !ps.sprite) {
    float3 view = normalize(ps.pos - origin);
    float cosi = saturate(-dot(view, ps.n));
    float fresnel = ps.reflectivity + (1.0 - ps.reflectivity) * pow(1.0 - cosi, 5.0);
    float3 r = reflect(view, ps.n);
    float3 jitter = float3(rand(rng), rand(rng), rand(rng)) * 2.0 - 1.0;
    float3 rr = normalize(r + jitter * ps.roughness);
    if (dot(rr, ps.n) <= 0.0) rr = r;
    bool direct = gCrowdTouched;
    gTrackCrowd = true; // a reflected person moves too
    Hit rh;
    float3 reflected;
    bool reflectedHit = trace(ps.pos + ps.n * 1e-3, rr, 1e30, false, rh);
    gCrowdReflected = gCrowdTouched && !direct;
    gCrowdTouched = direct;
    if (reflectedHit) {
      Surface rs = describe(rh, ps.pos + ps.n * 1e-3, rr, false);
      gTrackCrowd = false;
      reflected = rs.emission + rs.albedo * directLight(rs.pos, rs.n, rs.sprite, false, rng);
    } else {
      reflected = skyRadiance(data.atmosphere, rr, false);
    }
    // Light is stored without the surface color (compose multiplies it back),
    // so the reflection is divided by it here.
    o.color.rgb = diffuse * (1.0 - fresnel) + min(reflected, 16.0) * fresnel / max(ps.albedo, 0.02);
  }

  // Temporal accumulation with reprojection: find where this surface point
  // was on screen last frame; if the same surface was there (same depth and
  // facing: the disocclusion test), continue its average, else start over.
  float3 hitPoint = origin + dir * primary.t; // the real hit, not the texel centre
  float n = 0.0;
  float3 history = 0;
  if (data.reproject.x > 0.5) {
    float4 c = mul(data.prevViewProj, float4(hitPoint, 1.0));
    if (c.w > 1e-6) {
      float2 ndcPrev = c.xy / c.w;
      float2 px = data.viewport.xy + float2(ndcPrev.x * 0.5 + 0.5, 0.5 - ndcPrev.y * 0.5) * data.viewport.zw;
      if (all(px >= data.viewport.xy) && all(px < data.viewport.xy + data.viewport.zw)) {
        int3 q = int3(int2(px), 0);
        float4 was = prevNormalDepth.Load(q);
        float expected = -mul(data.prevView, float4(hitPoint, 1.0)).z;
        if (abs(was.w - expected) <= 0.02 * expected + 0.02 && dot(was.xyz, ps.n) > 0.8) {
          float4 prev = prevLight.Load(q);
          n = prev.a;
          history = prev.rgb;
        }
      }
    }
  }
  // How many frames to keep: the scene just changed (cut once), a crowd moves
  // (still pixels: bounce light and shadows from people change), a moving
  // person may cover this pixel or shows in its reflection (short, so people
  // do not smear). The rest of the image still converges.
  if (data.reproject.y > 0.0) n = min(n, data.reproject.y);
  if (data.history.x > 0.0) n = min(n, data.history.x);
  if (gCrowdReflected) n = min(n, data.history.z);
  if (gCrowdTouched) n = min(n, data.history.y);
  n = min(n, data.reproject.z);
  o.color.rgb = lerp(history, o.color.rgb, 1.0 / (n + 1.0));
  o.color.a = n + 1.0;
  return o;
}
