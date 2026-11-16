// Editor UI (Dear ImGui) and any other screen-space textured triangles.
struct UiData {
  float4 scaleTranslate; // clip = position * xy + zw
};
[[vk::binding(0, 0)]] ConstantBuffer<UiData> ui;
[[vk::binding(0, 1)]] Texture2D uiTexture;
[[vk::binding(1, 1)]] SamplerState uiSampler;

struct VsIn {
  [[vk::location(0)]] float2 position : POSITION;
  [[vk::location(1)]] float2 uv : TEXCOORD0;
  [[vk::location(2)]] float4 color : COLOR0;
};
struct VsOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
  float4 color : COLOR0;
};

VsOut vsMain(VsIn v) {
  VsOut o;
  o.position = float4(v.position * ui.scaleTranslate.xy + ui.scaleTranslate.zw, 0.0, 1.0);
  o.uv = v.uv;
  // UI colors are authored in sRGB; the render targets are sRGB, so linearize.
  o.color = float4(pow(v.color.rgb, 2.2), v.color.a);
  return o;
}

float4 fsMain(VsOut i) : SV_Target0 { return i.color * uiTexture.Sample(uiSampler, i.uv); }
