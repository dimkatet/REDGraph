// The node-REPLACEMENT self-test's pass (selftest/flow_probe.cpp, [Diagnostics] RenderGraphDump): a full-screen
// triangle at depth z, drawn with DepthFunc EQUAL against the scene's read-only depth, so only the pixels nothing
// covered (the sky - depth still at its clear value) take moving diagonal stripes: magenta, and t64's .r. Compiled by
// scripts/compile_sky_paint.ps1 into sky_paint_shader.hpp.

cbuffer Params : register(b0) {
  float g_z;      // 0 or 1: the clear value is not assumed - the test draws both
  float g_time;   // seconds
  float g_scale;  // linear HDR level of the stripes
  float g_pad;
};

float4 vs(uint id : SV_VertexID) : SV_Position {
  const float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), g_z, 1);
}

// M10: an ENGINE texture read through a descriptor from the engine's own ring - t64, the cloud shadow map (512 x 512
// RGBA8; its .r is the shadow), tiled 1:1 over the screen in every second stripe.
Texture2D<float4> t_engine : register(t0);

float4 ps(float4 pos : SV_Position) : SV_Target {
  const float band = frac((pos.x + pos.y) / 96.0 + g_time * 0.5);
  const float3 a = float3(1.0, 0.1, 0.8);
  const float3 b = t_engine.Load(int3(uint2(pos.xy) & 511, 0)).rrr;
  return float4((band < 0.5 ? a : b) * g_scale, 1);
}
