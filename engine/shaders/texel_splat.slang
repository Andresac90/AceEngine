// Draws the texels of one probe cube face as small squares in the world (one
// draw per visible face). No vertex buffer: vertex v belongs to texel v / 6, corner v % 6.
//
// A texel covers a little cone of directions from the probe origin. Its square
// is where that cone meets the captured surface: the four corner rays are
// intersected with the plane through the texel's point with the texel's normal.
// Neighbouring texels of one flat surface therefore share exact corners and
// tile without cracks. Seen edge-on, the corners are pulled in towards the
// texel (on the surface) so squares never turn into long spikes.
#include "common.slangh"

struct SplatData {
  float4x4 viewProj;
  float4x4 view;
  float4 origin; // xyz: probe origin, w: face size n
  float4 forward[6];
  float4 right[6];
  float4 up[6];
  float4 camera; // xyz: camera position
  float4 params; // x: quad half-size scale, y: push away from the camera (fraction of the distance), z: dither threshold, w: +1 keep below / -1 keep above / 0 keep all
  float4 face;   // x: the cube face this draw covers, y: 1 = corners stay on their rays (eye probe)
};

[[vk::binding(0, 0)]] ConstantBuffer<SplatData> data : register(b0, space0);
[[vk::binding(1, 0)]] Texture2D normalDepthAtlas : register(t1, space0); // xyz: world normal, w: distance along the face axis
[[vk::binding(2, 0)]] Texture2D finalAtlas : register(t2, space0);

struct VsOut {
  float4 position : SV_Position;
  nointerpolation float4 color : COLOR0;
  nointerpolation float4 normalDepth : TEXCOORD0;
};
struct FsOut {
  float4 color : SV_Target0;
  float4 normalDepth : SV_Target1;
};

VsOut vsMain(uint vid : SV_VertexID) {
  static const float2 corners[6] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)};
  uint n = uint(data.origin.w);
  uint local = vid / 6u;
  uint faceIndex = uint(data.face.x);
  uint x = local % n, y = local / n;
  int2 at = int2(int(faceIndex * n + x), int(y));
  float4 nd = normalDepthAtlas.Load(int3(at, 0));

  VsOut o;
  o.color = float4(0, 0, 0, 0);
  o.normalDepth = float4(0, 0, 0, 0);
  float threshold = bayer4(int2(int(x), int(y)));
  bool dropped = (data.params.w > 0.5 && threshold >= data.params.z) || (data.params.w < -0.5 && threshold < data.params.z);
  if (nd.w > kSkyDepth || dropped) {
    o.position = float4(2.0, 2.0, 2.0, 1.0); // nothing captured: collapse outside the view
    return o;
  }

  float3 F = data.forward[faceIndex].xyz, R = data.right[faceIndex].xyz, U = data.up[faceIndex].xyz;
  float3 origin = data.origin.xyz;
  // Texel centre in face coordinates (-1..1); row 0 is the top of the face.
  float cx = (float(x) + 0.5) / float(n) * 2.0 - 1.0;
  float cy = 1.0 - (float(y) + 0.5) / float(n) * 2.0;
  float h = data.params.x / float(n); // half a texel, in face coordinates
  float2 c = corners[vid % 6u];
  float3 rayCentre = F + R * cx + U * cy;
  float3 rayCorner = F + R * (cx + c.x * h) + U * (cy + c.y * h);
  float d = nd.w;
  float3 p = origin + rayCentre * d; // the captured point (F has unit length, so d is its distance along F)
  float3 normal = nd.xyz;

  float3 world = origin + rayCorner * d; // fallback: square facing the cube face
  float denom = dot(normal, rayCorner);
  if (abs(denom) > 1e-4) {
    float3 onSurface = origin + rayCorner * (dot(normal, p - origin) / denom);
    // Seen nearly edge-on from the probe, the surface plane meets the corner ray
    // far away and the square becomes a long spike. Keep the corner on the
    // surface but no further than 3x a plain texel; the eye probe fills the rest.
    if (dot(normal, rayCorner) * dot(normal, rayCentre) > 0.0) {
      if (data.face.y > 0.5) {
        // Eye probe: it sits at the camera, so corners anywhere on their own
        // rays tile the screen exactly. Keep them on the rays (no gaps), just
        // not absurdly far along them.
        float tp = dot(normal, p - origin) / denom;
        world = origin + rayCorner * clamp(tp, d * 0.5, d * 2.0);
      } else {
        float limit = 3.0 * length(world - p), reach = length(onSurface - p);
        world = reach <= limit ? onSurface : p + (onSurface - p) * (limit / reach);
      }
    }
  }

  // Lower-priority probes are pushed slightly away from the camera so that where
  // both probes cover a spot, the stable one wins (depth precision is not uniform,
  // so this is done in world space, not in clip space).
  // Neighbouring squares overlap a little (params.x > 1) and are nearly coplanar,
  // so in the overlap the depth test would pick a winner by rounding noise: a
  // different one per GPU compiler (D3D12 vs Vulkan) and per frame (shimmer).
  // A fixed, tiny push per position in a 2x2 block gives any two neighbours,
  // diagonal ones included, different depths. Pushing along the camera ray
  // moves only the depth, never the square on screen. Kept well below the
  // probe-priority pushes (0.003 and up).
  // shortcut: a fixed fraction of the distance; very far away it nears D32F's
  // precision (standard 0..1 depth). Revisit with reversed-Z depth.
  float tieBreak = float((x & 1u) + 2u * (y & 1u)) * 0.0005;
  world += (world - data.camera.xyz) * (data.params.y + tieBreak);
  o.position = mul(data.viewProj, float4(world, 1.0));
  o.color = float4(finalAtlas.Load(int3(at, 0)).rgb, 0.0); // alpha 0: already tone mapped and palette-snapped
  o.normalDepth = float4(normal, -mul(data.view, float4(p, 1.0)).z);
  return o;
}

FsOut fsMain(VsOut i) {
  FsOut o;
  o.color = i.color;
  o.normalDepth = i.normalDepth;
  return o;
}
