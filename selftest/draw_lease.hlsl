// DrawTest's LEASED draw companion (draw_test.cpp): the PS of a companion of the sky PS 5f079e10, reading a cbuffer the
// engine's draw never binds - it reaches the companion only through a lease (docs/internals.md "Leases (M2)"). Paints
// the sky's bottom layer in the color the test writes there. Compiled into draw_lease_shader.hpp by
// scripts/compile_draw_lease.ps1.

cbuffer Leased : register(b11) {
  float4 g_color;  // linear HDR
};

float4 ps(float4 pos : SV_Position) : SV_Target {
  return g_color;
}
