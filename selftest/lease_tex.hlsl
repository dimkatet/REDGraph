// LeaseTexTest's companion CS (lease_tex_test.cpp) - dispatched after the engine's tonemapping-LUT CS 88c57ef1, with
// the engine's compute root signature: every register below reaches it only through a lease (docs/internals.md
// "Leases (M2)"). Compiled into lease_tex_shader.hpp by scripts/compile_lease_tex.ps1.
//   t64  - the test's own 4 x 4 x 2 R32_UINT texture, through a 2D_ARRAY view of slice 1 only;
//   u16  - the engine's LUT 2ae9f45c (48^3) BY NAME, through a 3D UAV view the host resolves per dispatch;
//   u17  - the test's raw buffer: [0, 64) the 16 texels of t64, [64, 96) two LUT texels as float4 bits.

Texture2DArray<uint> g_tex : register(t64);
RWTexture3D<float4> g_lut : register(u16);
RWByteAddressBuffer g_out : register(u17);

[numthreads(16, 1, 1)] void main(uint i : SV_GroupThreadID) {
  g_out.Store(i * 4, g_tex.Load(int4(i & 3, i >> 2, 0, 0)));
  if (i < 2) {
    const uint3 c = i == 0 ? uint3(0, 0, 0) : uint3(47, 31, 17);
    g_out.Store4(64 + i * 16, asuint(g_lut[c]));
  }
}
