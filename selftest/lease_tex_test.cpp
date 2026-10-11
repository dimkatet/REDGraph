// LeaseTexTest - TEXTURE sources of a lease (docs/internals.md "Leases (M2)"), both kinds, proved by contents.
// A companion of the engine's tonemapping-LUT CS 88c57ef1 (3 dispatches per frame in CRenderNode_GenerateTonemappingLUT,
// group AsyncComputeDuringShadowmaps, every render mode; each writes one 48^3 LUT, RenderFlow 2ae9f45c / 2be9f5ef /
// 2de9f915), scoped to that node, after the THIRD dispatch of the frame, reads (lease_tex.hlsl):
//   t64  - a CLIENT texture (t64: a register the engine's compute tables cover - the fog CS 61f9ce9c reads it; t120,
//          the first choice, is not covered and the companion's PSO did not build): 4 x 4 x 2 R32_UINT, slice 0 = 0xBAD00000 | i, slice 1 = 0xC0DE0000 | i, through a 2D_ARRAY
//          view of slice 1 only (first_slice 1, slice_count 1) - the view's desc, not just its resource, must arrive;
//   u16  - the ENGINE's LUT 2ae9f45c BY NAME, through a 3D UAV view the host resolves on the recording thread;
// and writes both into u17, a raw buffer of ours. Expected, every 300 frames (read back 60 frames later):
//   the 16 texels of t64 = slice 1, exactly;
//   the 2 LUT texels = what a COPY of the same texels of the same resource holds (taken right after the companion,
//          through flow_texture + flow_transition - an independent route to the same memory), decoded from the LUT's
//          DXGI format. A UAV barrier after the frame's second dispatch keeps the first two from overlapping it.
// Registration-time checks, logged once: an owner whose declaration differs from the source is refused (a raw view at
// t64), a texture view takes no buffer, a flow view takes no resource, a UAV view is never a cube.
// Not visible on screen: the companion writes only our buffer.

#include <Windows.h>
#include <d3d12.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "hash_literal.hpp"
#include "ini_file.hpp"
#include "lease_tex_shader.hpp"
#include "selftest.hpp"

namespace selftest::lease_tex_test {
namespace {

constexpr char kNode[] = "CRenderNode_GenerateTonemappingLUT";
constexpr uint32_t kLutHash = 0x2ae9f45c;
constexpr uint32_t kTexReg = 64, kLutReg = 16, kOutReg = 17;
constexpr uint32_t kWindow = 300, kReadDelay = 60, kStart = 60;
constexpr uint32_t kSentinel = 0xfeedf00d;
constexpr uint32_t kRbTexel0 = 512, kRbTexel1 = 1024, kRbBytes = 2048;  // placed-footprint offsets: 512-aligned
constexpr uint32_t kCoord[2][3] = {{0, 0, 0}, {47, 31, 17}};

const rg_host* g_host = nullptr;
rg_handle g_comp = 0, g_flow = 0, g_vtex = 0, g_vlut = 0, g_vout = 0, g_lease[3] = {};
ID3D12Resource* g_tex = nullptr;
ID3D12Resource* g_upload = nullptr;
ID3D12Resource* g_out = nullptr;
ID3D12Resource* g_rb = nullptr;
uint8_t* g_rb_map = nullptr;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT g_fp[2] = {};

// Per frame (the wrap's before): the frame count and the dispatch index inside the node.
std::atomic<uint32_t> g_frame{0}, g_k{0};
std::atomic<bool> g_uploaded{false};
std::atomic<uint32_t> g_copied_frame{0};  // the window frame whose copies were recorded (0 = none pending)
std::atomic<uint32_t> g_lut_format{0};
uint32_t g_windows = 0, g_pass_count = 0, g_fail_count = 0;
uint64_t g_requests = 0, g_accepted = 0;

void log(uint32_t level, const char* fmt, ...) {
  char line[640];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  g_host->log(level, line);
}

void barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
  D3D12_RESOURCE_BARRIER rb{};
  rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  rb.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
  cl->ResourceBarrier(1, &rb);
}

void on_device(void*, void* device) {
  auto* dev = static_cast<ID3D12Device*>(device);
  D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT}, up{D3D12_HEAP_TYPE_UPLOAD}, rbh{D3D12_HEAP_TYPE_READBACK};
  D3D12_RESOURCE_DESC td{};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Width = 4;
  td.Height = 4;
  td.DepthOrArraySize = 2;
  td.MipLevels = 1;
  td.Format = DXGI_FORMAT_R32_UINT;
  td.SampleDesc.Count = 1;
  UINT64 up_bytes = 0;
  dev->GetCopyableFootprints(&td, 0, 2, 0, g_fp, nullptr, nullptr, &up_bytes);
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Height = 1;
  bd.DepthOrArraySize = 1;
  bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  HRESULT hr = dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&g_tex));
  bd.Width = up_bytes;
  if (SUCCEEDED(hr))
    hr = dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                      IID_PPV_ARGS(&g_upload));
  bd.Width = 256;
  bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (SUCCEEDED(hr))
    hr = dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                      IID_PPV_ARGS(&g_out));
  bd.Width = kRbBytes;
  bd.Flags = D3D12_RESOURCE_FLAG_NONE;
  if (SUCCEEDED(hr))
    hr = dev->CreateCommittedResource(&rbh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                      IID_PPV_ARGS(&g_rb));
  void* p = nullptr;
  if (SUCCEEDED(hr)) hr = g_upload->Map(0, nullptr, &p);
  if (SUCCEEDED(hr)) {
    for (uint32_t s = 0; s < 2; ++s)
      for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x) {
          const uint32_t v = (s ? 0xC0DE0000u : 0xBAD00000u) | (y * 4 + x);
          std::memcpy(static_cast<uint8_t*>(p) + g_fp[s].Offset + y * g_fp[s].Footprint.RowPitch + x * 4, &v, 4);
        }
    g_upload->Unmap(0, nullptr);
    const D3D12_RANGE all{0, kRbBytes};
    hr = g_rb->Map(0, &all, &p);  // a READBACK heap stays mapped; read after the GPU is long done
    g_rb_map = static_cast<uint8_t*>(p);
  }
  if (FAILED(hr)) {
    log(RG_LOG_ERROR, "lease tex test: creating the test's resources FAILED hr 0x%08x - nothing runs",
        static_cast<unsigned>(hr));
    return;
  }
  const int32_t a = g_host->view_set_resource(g_vtex, g_tex, 0);
  const int32_t b = g_host->view_set_resource(g_vout, g_out, 256);
  // a texture view takes no buffer, a flow view takes no resource
  const int32_t c = g_host->view_set_resource(g_vtex, g_out, 256);
  const int32_t d = g_host->view_set_resource(g_vlut, g_tex, 0);
  log(a == 0 && b == 0 && c == RG_E_ARG && d == RG_E_ARG ? RG_LOG_INFO : RG_LOG_ERROR,
      "lease tex test: views set (texture %d, buffer %d); a buffer into the texture view %d, a resource into the flow "
      "view %d (expected 0 0 %d %d)", a, b, c, d, RG_E_ARG, RG_E_ARG);
}

// Before the node's Execute: a new frame of the node. Once: the client texture's upload, on the node's own list - its
// dispatches (and our companion) record after it.
uint32_t on_before(void*, const rg_node_info* info) {
  const uint32_t f = g_frame.fetch_add(1) + 1;
  g_k.store(0);
  if (f == kStart && g_tex && info->cmdlist && !g_uploaded.load()) {
    auto* cl = static_cast<ID3D12GraphicsCommandList*>(info->cmdlist);
    for (uint32_t s = 0; s < 2; ++s) {
      D3D12_TEXTURE_COPY_LOCATION dst{g_tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      dst.SubresourceIndex = s;
      D3D12_TEXTURE_COPY_LOCATION src{g_upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      src.PlacedFootprint = g_fp[s];
      cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    barrier(cl, g_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_uploaded.store(true);
    log(RG_LOG_INFO, "lease tex test: client texture uploaded on the node's list (frame %u)", f);
  }
  return 0;
}

uint32_t pass_uav_barrier(void*, void* cmdlist) {
  D3D12_RESOURCE_BARRIER rb{};
  rb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;  // every UAV: the frame's first two LUTs written before the third dispatch
  static_cast<ID3D12GraphicsCommandList*>(cmdlist)->ResourceBarrier(1, &rb);
  return 0;
}

// Right after the companion of the third dispatch: our buffer and the LUT's two texels into the readback buffer.
uint32_t pass_copy(void*, void* cmdlist) {
  auto* cl = static_cast<ID3D12GraphicsCommandList*>(cmdlist);
  uint32_t id = 0;
  auto* lut = static_cast<ID3D12Resource*>(g_host->flow_texture(g_flow, &id));
  barrier(cl, g_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  cl->CopyBufferRegion(g_rb, 0, g_out, 0, 96);
  barrier(cl, g_out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (lut && g_host->flow_transition(g_flow, D3D12_RESOURCE_STATE_COPY_SOURCE)) {
    const D3D12_RESOURCE_DESC ld = lut->GetDesc();
    g_lut_format.store(static_cast<uint32_t>(ld.Format));
    for (uint32_t t = 0; t < 2; ++t) {
      D3D12_TEXTURE_COPY_LOCATION dst{g_rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      dst.PlacedFootprint.Offset = t ? kRbTexel1 : kRbTexel0;
      dst.PlacedFootprint.Footprint = {ld.Format, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
      D3D12_TEXTURE_COPY_LOCATION src{lut, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      src.SubresourceIndex = 0;
      const D3D12_BOX box{kCoord[t][0], kCoord[t][1], kCoord[t][2], kCoord[t][0] + 1, kCoord[t][1] + 1, kCoord[t][2] + 1};
      cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    }
    g_host->flow_transition(g_flow, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  } else {
    g_lut_format.store(0);  // the LUT did not resolve here - logged at the check
  }
  return 0;
}

float half_to_float(uint16_t h) {
  const uint32_t e = (h >> 10) & 31, m = h & 1023;
  const float v = e == 0 ? std::ldexp(static_cast<float>(m), -24) : std::ldexp(static_cast<float>(m | 1024), static_cast<int>(e) - 25);
  return (h & 0x8000) ? -v : v;
}
float small_float(uint32_t bits, uint32_t mbits) {  // R11G11B10's unsigned 5-bit-exponent floats
  const uint32_t e = bits >> mbits, m = bits & ((1u << mbits) - 1);
  return e == 0 ? std::ldexp(static_cast<float>(m), -14 - static_cast<int>(mbits))
                : std::ldexp(static_cast<float>(m | (1u << mbits)), static_cast<int>(e) - 15 - static_cast<int>(mbits));
}

// One texel of the copy route, in what a typed float4 load returns. false = a format this test does not decode.
bool decode(uint32_t fmt, const uint8_t* p, float out[4], bool* has_alpha) {
  uint32_t w;
  std::memcpy(&w, p, 4);
  *has_alpha = true;
  switch (fmt) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      for (int i = 0; i < 4; ++i) {
        uint16_t h;
        std::memcpy(&h, p + 2 * i, 2);
        out[i] = half_to_float(h);
      }
      return true;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
      std::memcpy(out, p, 16);
      return true;
    case DXGI_FORMAT_R11G11B10_FLOAT:
      out[0] = small_float(w & 0x7ff, 6);
      out[1] = small_float((w >> 11) & 0x7ff, 6);
      out[2] = small_float(w >> 22, 5);
      *has_alpha = false;
      return true;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      for (int i = 0; i < 3; ++i) out[i] = static_cast<float>((w >> (10 * i)) & 1023) / 1023.f;
      out[3] = static_cast<float>(w >> 30) / 3.f;
      return true;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      for (int i = 0; i < 4; ++i) out[i] = static_cast<float>(p[i]) / 255.f;
      return true;
    default:
      return false;
  }
}

void check(uint32_t window_frame) {
  ++g_windows;
  uint32_t out[24];
  std::memcpy(out, g_rb_map, sizeof out);
  const uint32_t fmt = g_lut_format.load();
  rg_lease_stats ls[3] = {};
  for (int i = 0; i < 3; ++i) g_host->lease_stats(g_lease[i], &ls[i]);
  rg_patch_stats cs{};
  g_host->companion_stats(g_comp, &cs);
  rg_flow_stats fs{};
  g_host->flow_stats(g_flow, &fs);
  if (out[0] == kSentinel) {
    log(RG_LOG_WARN, "lease tex test #%u (frame %u): the GPU has not written the readback yet - skipped", g_windows,
        window_frame);
    return;
  }
  uint32_t tex_ok = 0, tex_slice0 = 0;
  for (uint32_t i = 0; i < 16; ++i) {
    tex_ok += out[i] == (0xC0DE0000u | i);
    tex_slice0 += out[i] == (0xBAD00000u | i);
  }
  uint32_t lut_ok = 0;
  char lut_line[200] = "the LUT did not resolve in the copy pass";
  if (fmt) {
    float a[4], b[4];
    bool alpha = true;
    for (uint32_t t = 0; t < 2; ++t) {
      std::memcpy(a, out + 16 + 4 * t, 16);  // the companion's typed load
      if (!decode(fmt, g_rb_map + (t ? kRbTexel1 : kRbTexel0), b, &alpha)) {
        std::snprintf(lut_line, sizeof lut_line, "LUT DXGI format %u is not one this test decodes", fmt);
        lut_ok = 0;
        break;
      }
      bool same = true;
      for (int c = 0; c < (alpha ? 4 : 3); ++c) same = same && std::fabs(a[c] - b[c]) <= 1e-6f * std::fmax(1.f, std::fabs(b[c]));
      lut_ok += same;
      if (t == 1)
        std::snprintf(lut_line, sizeof lut_line,
                      "LUT texels %u/2 equal to the copy (DXGI format %u; texel (47,31,17) load %.6g %.6g %.6g, copy "
                      "%.6g %.6g %.6g)", lut_ok, fmt, a[0], a[1], a[2], b[0], b[1], b[2]);
    }
  }
  const bool pass = tex_ok == 16 && lut_ok == 2;
  (pass ? g_pass_count : g_fail_count)++;
  log(pass ? RG_LOG_INFO : RG_LOG_ERROR,
      "lease tex test #%u (frame %u): client texture %u/16 = slice 1 (%u = slice 0); %s; companion requests %llu "
      "accepted %llu, swaps %llu out_of_scope %llu; leases staged %llu / %llu / %llu; flow hits %llu misses %llu "
      "outside %llu -> %s (%u pass, %u fail)",
      g_windows, window_frame, tex_ok, tex_slice0, lut_line, static_cast<unsigned long long>(g_requests),
      static_cast<unsigned long long>(g_accepted), static_cast<unsigned long long>(cs.swaps),
      static_cast<unsigned long long>(cs.out_of_scope), static_cast<unsigned long long>(ls[0].stages),
      static_cast<unsigned long long>(ls[1].stages), static_cast<unsigned long long>(ls[2].stages),
      static_cast<unsigned long long>(fs.hits), static_cast<unsigned long long>(fs.misses),
      static_cast<unsigned long long>(fs.outside), pass ? "PASS" : "FAIL", g_pass_count, g_fail_count);
}

rg_handle g_pass_barrier = 0, g_pass_copy = 0;

// After every engine dispatch of 88c57ef1, on the recording thread. k = the dispatch's index in the node this frame.
void on_dispatch(void*, const rg_dispatch_info*) {
  const uint32_t k = g_k.fetch_add(1) + 1;
  const uint32_t f = g_frame.load();
  if (!g_uploaded.load() || f <= kStart || !g_rb_map) return;
  const uint32_t pending = g_copied_frame.load();
  if (pending && f >= pending + kReadDelay && k == 1) {
    check(pending);
    g_copied_frame.store(0);
  }
  if ((f - kStart) % kWindow != 0 || pending) return;
  if (k == 2) g_host->pass_after_dispatch(g_pass_barrier, &pass_uav_barrier, nullptr);
  if (k != 3) return;
  for (uint32_t i = 0; i < kRbBytes / 4; ++i) std::memcpy(g_rb_map + 4 * i, &kSentinel, 4);
  ++g_requests;
  if (!g_host->companion_request(g_comp, 1, 1, 1)) {
    log(RG_LOG_WARN, "lease tex test (frame %u): companion_request refused at the third dispatch - window skipped", f);
    return;
  }
  ++g_accepted;
  if (g_host->pass_after_dispatch(g_pass_copy, &pass_copy, nullptr)) g_copied_frame.store(f);
}

rg_handle lease(const char* name, uint32_t kind, uint32_t reg, rg_handle view) {
  rg_lease_desc l{};
  l.name = name;
  l.stage = RG_STAGE_CS;
  l.kind = kind;
  l.reg = reg;
  l.companions = &g_comp;
  l.companion_count = 1;
  l.view = view;
  return g_host->lease_register(&l);
}

}  // namespace

// HlslTest's run proof: the companion compiled now, from lease_tex.hlsl next to this DLL. {NULL, 0} = it did not.
rg_blob runtime_blob(const rg_host* host) {
  static int anchor;
  const std::string path = (shared::IniFile::module_dir(&anchor) / "lease_tex.hlsl").string();
  rg_hlsl_desc d{};
  d.struct_size = sizeof d;
  d.path = path.c_str();
  d.entry = "main";
  d.profile = "cs_6_0";
  rg_blob b{};
  char err[400], line[600];
  const int32_t r = host->compile_hlsl(&d, &b, err, sizeof err);
  std::snprintf(line, sizeof line, "lease tex test: companion compiled at runtime from %s -> %d, %zu bytes%s%s",
                path.c_str(), r, b.size, r ? " - " : "", r ? err : "");
  host->log(r ? RG_LOG_ERROR : RG_LOG_INFO, line);
  return r ? rg_blob{} : b;
}

void register_with(const rg_host* host, bool on, bool runtime_hlsl) {
  if (!on) return;
  g_host = host;
  if (host->struct_size < offsetof(rg_host, view_create_texture) + sizeof(void*)) {
    host->log(RG_LOG_ERROR, "lease tex test: the host is older than view_create_texture - not run");
    return;
  }
  static constexpr auto kLutCs = hash_literal::hash_from_hex("88c57ef1799c4d87f1f6b68eaee49f75");
  rg_blob cs{lease_tex::kCSBytes, lease_tex::kCSSize};
  if (runtime_hlsl) {
    cs = runtime_blob(host);
    if (!cs.bytes) return;  // logged: the test would prove nothing about compile_hlsl
  }
  g_comp = host->companion_register("lease tex test: companion of 88c57ef1", kLutCs.data(), cs);
  const int32_t scoped = g_comp > 0 ? host->companion_scope(g_comp, kNode) : g_comp;
  g_flow = host->flow_register("tonemapping LUT 2ae9f45c", kLutHash);
  rg_texture_view_desc td{};
  td.dimension = RG_TEX_2D_ARRAY;
  td.first_slice = 1;
  td.slice_count = 1;
  g_vtex = host->view_create_texture(RG_KIND_SRV, &td, 1);
  rg_texture_view_desc ld{};
  ld.dimension = RG_TEX_3D;
  ld.flow = g_flow;
  g_vlut = host->view_create_texture(RG_KIND_UAV, &ld, 4);
  g_vout = host->view_create(RG_KIND_UAV, 0, 1);
  // the registration-time checks: a raw view where the companion declares a Texture2DArray; a cube UAV
  const rg_handle raw = host->view_create(RG_KIND_SRV, 0, 1);
  const rg_handle wrong = raw > 0 ? lease("lease tex test: raw view at a Texture2DArray (must be refused)", RG_KIND_SRV,
                                          kTexReg, raw)
                                  : raw;
  rg_texture_view_desc cube{};
  cube.dimension = RG_TEX_CUBE;
  const rg_handle cube_uav = host->view_create_texture(RG_KIND_UAV, &cube, 1);
  g_lease[0] = lease("lease tex test: client texture t64", RG_KIND_SRV, kTexReg, g_vtex);
  g_lease[1] = lease("lease tex test: engine LUT u16", RG_KIND_UAV, kLutReg, g_vlut);
  g_lease[2] = lease("lease tex test: out u17", RG_KIND_UAV, kOutReg, g_vout);
  g_pass_barrier = host->pass_register("lease tex test: UAV barrier");
  g_pass_copy = host->pass_register("lease tex test: copies");
  const rg_handle wrap = host->node_wrap(kNode, &on_before, nullptr, nullptr);
  const rg_handle sub = host->dispatch_subscribe("lease tex test: 88c57ef1", kLutCs.data(), &on_dispatch, nullptr);
  const int32_t dev = host->device_ready(&on_device, nullptr);
  const bool ok = g_comp > 0 && scoped == 0 && g_flow > 0 && g_vtex > 0 && g_vlut > 0 && g_vout > 0 && g_lease[0] > 0 &&
                  g_lease[1] > 0 && g_lease[2] > 0 && g_pass_barrier > 0 && g_pass_copy > 0 && wrap > 0 && sub > 0 &&
                  dev > 0 && wrong == RG_E_BLOB && cube_uav == RG_E_ARG;
  char line[400];
  std::snprintf(line, sizeof line,
                "lease tex test: %s - companion %d (scope %d), flow %d, views %d %d %d, leases %d %d %d, passes %d %d, "
                "wrap %d, subscription %d, device_ready %d; a raw view at a Texture2DArray -> %d (expected %d), a cube "
                "UAV view -> %d (expected %d)",
                ok ? "registered" : "registration FAILED", g_comp, scoped, g_flow, g_vtex, g_vlut, g_vout, g_lease[0],
                g_lease[1], g_lease[2], g_pass_barrier, g_pass_copy, wrap, sub, dev, wrong, RG_E_BLOB, cube_uav,
                RG_E_ARG);
  host->log(ok ? RG_LOG_INFO : RG_LOG_ERROR, line);
}

}  // namespace selftest::lease_tex_test
