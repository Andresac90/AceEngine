// Copies a region of a texture (the viewport limits it): the path tracer keeps
// last frame's normal/depth for the disocclusion test of its reprojection.
[[vk::binding(0, 0)]] Texture2D source : register(t0, space0);

struct VsOut {
  float4 position : SV_Position;
};

VsOut vsMain(uint id : SV_VertexID) {
  VsOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
  return o;
}

float4 fsMain(VsOut i) : SV_Target0 { return source.Load(int3(int2(i.position.xy), 0)); }
