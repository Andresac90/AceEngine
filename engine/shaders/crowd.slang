// Raster pass for crowds (render/crowd.h). One draw per character model; no
// vertex buffer: vertex v belongs to person list[first + v / n], corner v % n
// of the model's rest-pose triangles ("vertex pulling"). The pose is computed
// here from the time, exactly as the path tracer computes it (crowd.slangh).
//
// Shading follows mesh.slang (bands, dithering, point lights, palette ramps),
// with texel-space lighting done in the person's *rest* space, so lighting
// pixels stay attached to the body while it bends.
#include "common.slangh"
#include "crowd.slangh"

struct CrowdDraw {
  CrowdData crowd;
  float4 info; // x: first draw-list entry, y: first vertex of the model, z: vertices per person
};

[[vk::binding(0, 0)]] ConstantBuffer<FrameData> frame : register(b0, space0);
[[vk::binding(1, 0)]] Texture2D shadowMap : register(t1, space0);
[[vk::binding(2, 0)]] Texture2D paletteTexture : register(t2, space0);
[[vk::binding(0, 1)]] ConstantBuffer<CrowdDraw> draw : register(b0, space1);
[[vk::binding(1, 1)]] StructuredBuffer<uint> drawList : register(t1, space1);
[[vk::binding(2, 1)]] StructuredBuffer<CrowdMember> members : register(t2, space1);
[[vk::binding(3, 1)]] StructuredBuffer<float4> table : register(t3, space1);
[[vk::binding(4, 1)]] StructuredBuffer<float4> vertices : register(t4, space1); // (rest position, bone), (normal, color code), (face normal)

struct VsOut {
  float4 position : SV_Position;
  float3 normal : NORMAL;
  float3 worldPos : POSITION1;
  float3 restPos : POSITION2;
  float viewDepth : DEPTH;
  nointerpolation float3 color : COLOR0;
  nointerpolation float3 faceRest : NORMAL1; // rest-space face normal: picks the texel grid's axis
  nointerpolation float4 rotation : TEXCOORD0; // rest -> world rotation of this part (yaw folded in)
  nointerpolation float2 scaleSeed : TEXCOORD1;
};
struct FsOut {
  float4 color : SV_Target0;
  float4 normalDepth : SV_Target1;
};

VsOut vsMain(uint vid : SV_VertexID) {
  uint perPerson = uint(draw.info.z);
  CrowdMember m = members[drawList[uint(draw.info.x) + vid / perPerson]];
  uint v = uint(draw.info.y) + vid % perPerson;
  float4 pv = vertices[v * 3u], nv = vertices[v * 3u + 1u];
  CrowdPose p = crowdPose(table, draw.crowd, m, uint(pv.w));
  float3 world = crowdToWorld(m, quatRotate(p.q, pv.xyz) + p.t);
  VsOut o;
  o.position = mul(frame.viewProj, float4(world, 1.0));
  o.normal = crowdDirToWorld(m, quatRotate(p.q, nv.xyz));
  o.worldPos = world;
  o.restPos = pv.xyz;
  o.viewDepth = -mul(frame.view, float4(world, 1.0)).z;
  o.color = crowdColor(nv.w, m);
  o.faceRest = vertices[v * 3u + 2u].xyz;
  // Yaw (about +Y, the sign crowdDirToWorld uses) times the bone rotation.
  // Half-angle formulas instead of atan2/sin/cos (their precision differs between GPU compilers).
  float c = m.facing.x, s = m.facing.y;
  float4 yaw = float4(0.0, (s < 0.0 ? -1.0 : 1.0) * sqrt(max(0.0, 0.5 - 0.5 * c)), 0.0, sqrt(max(0.0, 0.5 + 0.5 * c)));
  o.rotation = float4(yaw.w * p.q.xyz + p.q.w * yaw.xyz + cross(yaw.xyz, p.q.xyz), yaw.w * p.q.w - dot(yaw.xyz, p.q.xyz));
  o.scaleSeed = float2(m.posScale.w, m.facing.w);
  return o;
}

float banded(float v, float bands, float jitter) {
  return bands > 0.5 ? clamp(ceil(v * bands - 0.001 + jitter), 0.0, bands) / bands : v;
}

FsOut fsMain(VsOut i) {
  float3 n = normalize(i.normal);
  // Texel-space lighting in rest space: the texel grid (texelsPerUnit) is
  // projected along the triangle's dominant axis, like world-texel meshes, but
  // on the undeformed body, then the texel centre is carried back to the world
  // through this part's rigid transform.
  float3 a = abs(i.faceRest);
  float tpu = frame.params2.y;
  float3 restCentre = i.restPos;
  int2 texel;
  if (a.y >= a.x && a.y >= a.z) {
    texel = int2(floor(i.restPos.xz * tpu));
    restCentre.xz = (float2(texel) + 0.5) / tpu;
  } else if (a.x >= a.z) {
    texel = int2(floor(i.restPos.zy * tpu));
    restCentre.zy = (float2(texel) + 0.5) / tpu;
  } else {
    texel = int2(floor(i.restPos.xy * tpu));
    restCentre.xy = (float2(texel) + 0.5) / tpu;
  }
  float3 shadePos = i.worldPos;
  if (frame.params2.x > 0.5) shadePos += quatRotate(i.rotation, restCentre - i.restPos) * i.scaleSeed.x;

  float bands = frame.params.x;
  float jitter = (bayer4(texel + int2(int(i.scaleSeed.y) & 3, 0)) - 0.5) * frame.params.w;
  float3 radiance = frame.lightColor.rgb * banded(saturate(dot(n, frame.lightDir.xyz)), bands, jitter);
  for (int k = 0; k < int(frame.pointInfo.x); k++) {
    float3 toLamp = frame.pointPosition[k].xyz - shadePos;
    float dist = length(toLamp);
    float fade = saturate(1.0 - dist / frame.pointPosition[k].w);
    float facing = saturate(dot(n, toLamp / max(dist, 1e-4)));
    radiance += frame.pointColor[k].rgb * banded(facing * fade * fade, bands, jitter);
  }
  float3 lit = i.color * (frame.ambient.rgb + radiance * 0.318);
  if (frame.params2.z > 0.5) {
    float amount = dot(radiance, float3(0.2126, 0.7152, 0.0722)) / 3.0;
    lit = rampShade(paletteTexture, i.color, amount, int(frame.params2.w));
  }
  FsOut o;
  o.color = float4(lit, 1.0);
  o.normalDepth = float4(n, i.viewDepth);
  return o;
}
