// The self-tests of M5 and M6 (docs/internals.md), [Diagnostics] RenderGraphDump only.
// M6: our node "RgNode_SelfTest" right after CRenderNode_VolumetricFog (group Lighting) - it records nothing, counts
// its runs and asks for t64 by name from INSIDE our own node; the graph dump must list it as the member after
// CRenderNode_VolumetricFog.
// M7: the node DECLARES a read of t64 and its own transient "rgselftest/tmp" (t64's desc: 512 x 512, engine format
// 5, flags 0x39) and takes the transient by name from inside the node - it must resolve to a 512 x 512
// R8G8B8A8_UNORM texture every run; the census (graph-NNN-resources.txt) must list the node's requests, phase 1 = phase 2.
// M7, ACROSS GROUPS: the transient lives from RgNode_SelfTest (Lighting) to a second node, RgNode_SelfTestReader
// after CRenderNode_RenderVolumetricClouds (Transparents - later in the frame), which node_use's it - through its
// PUBLISHED name (flow_register("rgselftest/tmp", 0)), the path another client takes. Proved by its
// CONTENTS: the writer clears it (through flow_transition, raw D3D12) to a colour that encodes its own run counter
// (R, G = the counter, B = 0x5a, A = 0xff) and copies texel (0,0) into readback ring A - the control, the readback
// itself; the reader copies the same texel into ring B. Both rings are read back 4 runs later (a sentinel marks a copy
// the GPU has not done yet). Expected: ring A exact every run; ring B always the pattern (B = 0x5a, A = 0xff) with a
// new counter each run. Garbage in ring B = the memory was handed to another transient between the two groups.
// M8, WRAPS of engine nodes: (1) CRenderNode_VolumetricFog (a group member) - every dispatch of its CS a58d01db must
// (a direct DispatchCompute) fall BETWEEN our before and after (a dispatch subscription checks a thread-local window), and
// so must every dispatch of 5c7ab952 - INDIRECT (DispatchIndirectCompute; run 13 never saw it, the host hooked only
// DispatchCompute until 2026-10-08): each must arrive with RG_DISPATCH_INDIRECT and counts 0 (`direct` 0), and RgNode_SelfTest, the
// member right after it, must run next on the SAME command list as our after; (2) the group "Lighting" (a top-level
// node, the group vtable shared by every group - matched by name): before / after on the group's command list, and in
// after a copy of rgselftest/tmp's texel (0,0) into readback ring C - it must hold THIS frame's clear (the
// writer's latest counter, exactly), proving after(Lighting) records on the GPU after the group's members.
// M9, a REMOVAL you can SEE: CRenderNode_RenderVolumetricClouds (Transparents - the world's volumetric clouds; it
// composites onto the scene colour 3d7e6258 that RenderBackground & co. wrote, its own transients b035527c / b035527e
// have no other user, so without it the sky simply has no clouds) is removed in alternating 600-frame windows (frames
// counted by RgNode_SelfTest), switched live with node_remove_set, each switch logged. Its CS 3dd31a42 dispatches are
// counted per window, 3 frames after each switch excluded (the switch latches at the next declaration run). Expected:
// no dispatch while removed, dispatches while not; removed / replayed grow, overflows 0, refused 0; on screen the
// clouds vanish for 10 s out of every 20 s, nothing else changes. (Run 15 did the same with HistogramUpdate: exact,
// but invisible - a frozen exposure shows only when the scene's brightness changes.)
// REPLACEMENT (remove + wrap, docs/internals.md "Replacing an engine node"): while the clouds are removed, an
// `after` on the same class draws OUR pass in their slot, into the same scene colour: a full-screen triangle with
// DepthFunc EQUAL against the scene depth (debf0c27, read-only DSV), at z = 0 and at z = 1 (the clear value is not
// assumed), so only the sky - where nothing wrote depth - takes moving magenta / cyan stripes. RTV / DSV in our own
// CPU heaps, our root signature (4 root constants) and PSO (made once per format pair, on the render thread); states
// through the engine's tracker (flow_transition). Expected: in the REMOVED windows the sky is striped and cloudless,
// buildings / characters in front of it are not; outside them the sky is normal.
// M5: t64 BY
// NAME (RenderFlow hash 583c0f7f) right after its writer 53c455b6 dispatches. Logged on the first three dispatches and
// every 1800th: the GpuApi id, the D3D12 desc and the counters. What t64 must look like (measured
// on hardware): 512 x 512 R8G8B8A8_UNORM, render-target + UAV flags, an id from the top of the id range. The
// id changes per graph build / load, not per frame (2026-10-07: 5 changes in 3600 dispatches).

#include <d3d12.h>

#include <atomic>
#include <cstdio>

#include "selftest.hpp"
#include "hash_literal.hpp"
#include "sky_paint_shader.hpp"

namespace selftest::flow_probe {
namespace {

const rg_host* g_host = nullptr;
rg_handle g_t64 = 0;
std::atomic<uint32_t> g_calls{0};
rg_handle g_node = 0;
rg_handle g_tmp = 0;
rg_handle g_tmp_by_name = 0;  // the reader's handle: the published name looked up, as another client would
std::atomic<uint64_t> g_tmp_hits{0}, g_tmp_misses{0};
std::atomic<uint32_t> g_node_calls{0};

// ---- the cross-group content test ----
constexpr uint32_t kRing = 4, kSlotBytes = 512;  // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
constexpr uint32_t kSentinel = 0xdeadbeefu;
ID3D12Device* g_dev = nullptr;
ID3D12Resource* g_rb = nullptr;  // readback: ring A (writer) slots 0..3, ring B (reader) 4..7
uint8_t* g_rb_map = nullptr;
ID3D12DescriptorHeap* g_rtv_heap = nullptr;
D3D12_CPU_DESCRIPTOR_HANDLE g_rtv{};
rg_handle g_reader = 0;
std::atomic<uint32_t> g_wn{0}, g_rn{0};
uint32_t g_expect[kRing] = {};  // writer only
uint32_t g_rd_prev = 0;         // reader only
struct Tally {
  std::atomic<uint64_t> ok{0}, bad{0}, pending{0}, fresh{0}, miss{0};
  std::atomic<uint32_t> last_bad{0};
};
Tally g_ctl, g_rd;

// ---- the wrap test (M8) ----
rg_handle g_wrap_fog = 0, g_wrap_lighting = 0;
thread_local bool t_in_fog = false;
thread_local void* t_after_fog_cl = nullptr;  // set by after(VolumetricFog), taken by the next RgNode_SelfTest
thread_local void* t_lighting_cl = nullptr;   // the list before(Lighting) saw, until after(Lighting)
struct WrapTally {
  std::atomic<uint64_t> fog_before{0}, fog_after{0}, fog_unpaired{0}, disp_in{0}, disp_out{0};
  std::atomic<uint64_t> ind_in{0}, ind_out{0}, ind_direct{0};  // 5c7ab952: indirect, inside / outside the window
  std::atomic<uint64_t> adjacent{0}, not_adjacent{0};
  std::atomic<uint64_t> lit_before{0}, lit_after{0}, lit_same_cl{0}, node_in_lit{0}, node_out_lit{0};
};
WrapTally g_wt;
Tally g_lc;                      // ring C: after(Lighting)'s copy of the transient
uint32_t g_expect_c[kRing] = {};  // after(Lighting) only
std::atomic<uint32_t> g_ln{0};

// ---- the removal test (M9) ----
constexpr uint32_t kRemoveWindow = 600, kGuard = 3;
rg_handle g_remove = 0;
std::atomic<uint32_t> g_frame{0};  // RgNode_SelfTest's count
std::atomic<uint64_t> g_disp_on{0}, g_disp_off{0}, g_disp_guard{0}, g_toggles{0};
bool removal_window(uint32_t frame) { return (frame / kRemoveWindow) & 1u; }

void on_removed_dispatch(void*, const rg_dispatch_info*) {
  const uint32_t f = g_frame.load(std::memory_order_relaxed);
  if (f % kRemoveWindow < kGuard) g_disp_guard.fetch_add(1, std::memory_order_relaxed);
  else (removal_window(f) ? g_disp_on : g_disp_off).fetch_add(1, std::memory_order_relaxed);
}

// ---- the replacement pass ----
rg_handle g_color = 0, g_depth = 0;
ID3D12RootSignature* g_sky_rs = nullptr;
ID3D12DescriptorHeap *g_sky_rtv_heap = nullptr, *g_sky_dsv_heap = nullptr;
struct SkyPso {
  DXGI_FORMAT color, depth;
  ID3D12PipelineState* pso;
};
SkyPso g_sky_pso[4] = {};
uint32_t g_nsky = 0;  // the clouds' node records on one thread at a time
std::atomic<uint64_t> g_sky_draws{0}, g_sky_missing{0}, g_sky_unsupported{0}, g_sky_srv{0}, g_sky_no_srv{0};
std::atomic<uint32_t> g_sky_fmt{0};  // color << 16 | depth of the last draw, for the log

DXGI_FORMAT rtv_format(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    default: return f;
  }
}

DXGI_FORMAT dsv_format(DXGI_FORMAT f, bool* stencil) {
  *stencil = false;
  switch (f) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: *stencil = true; return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: *stencil = true; return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_D32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_D16_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

ID3D12PipelineState* sky_pso(DXGI_FORMAT color, DXGI_FORMAT depth);

void sky_create_device_objects() {
  D3D12_ROOT_PARAMETER rp[2]{};
  rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  rp[0].Constants = {0, 0, 4};
  rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  const D3D12_DESCRIPTOR_RANGE srv{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};  // t0: an engine texture (M10)
  rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  rp[1].DescriptorTable = {1, &srv};
  rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  const D3D12_ROOT_SIGNATURE_DESC rsd{2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ID3DBlob *blob = nullptr, *err = nullptr;
  if (SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
    g_dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_sky_rs));
  if (blob) blob->Release();
  if (err) err->Release();
  const D3D12_DESCRIPTOR_HEAP_DESC rh{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  const D3D12_DESCRIPTOR_HEAP_DESC dh{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  g_dev->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&g_sky_rtv_heap));
  g_dev->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&g_sky_dsv_heap));
  if (!g_sky_rs || !g_sky_rtv_heap || !g_sky_dsv_heap)
    g_host->log(RG_LOG_ERROR, "node replace test: root signature / heaps not created - the pass is OFF");
  else
    sky_pso(DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT);  // the formats run 17 saw: no PSO build mid-frame
}

ID3D12PipelineState* sky_pso(DXGI_FORMAT color, DXGI_FORMAT depth) {
  for (uint32_t i = 0; i < g_nsky; ++i)
    if (g_sky_pso[i].color == color && g_sky_pso[i].depth == depth) return g_sky_pso[i].pso;
  if (g_nsky >= 4) return nullptr;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
  d.pRootSignature = g_sky_rs;
  d.VS = {selftest::sky_paint::kVSBytes, selftest::sky_paint::kVSSize};
  d.PS = {selftest::sky_paint::kPSBytes, selftest::sky_paint::kPSSize};
  d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.SampleMask = UINT_MAX;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.RasterizerState.DepthClipEnable = TRUE;
  d.DepthStencilState.DepthEnable = TRUE;
  d.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
  d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL;
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  d.NumRenderTargets = 1;
  d.RTVFormats[0] = color;
  d.DSVFormat = depth;
  d.SampleDesc.Count = 1;
  ID3D12PipelineState* pso = nullptr;
  const HRESULT hr = g_dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
  char line[160];
  std::snprintf(line, sizeof line, "node replace test: PSO for colour fmt %u / depth fmt %u -> %s (hr 0x%08lx)",
                static_cast<unsigned>(color), static_cast<unsigned>(depth), pso ? "created" : "FAILED",
                static_cast<unsigned long>(hr));
  g_host->log(pso ? RG_LOG_INFO : RG_LOG_ERROR, line);
  g_sky_pso[g_nsky++] = {color, depth, pso};  // a failure is remembered too: not retried every frame
  return pso;
}

uint32_t on_clouds_after(void*, const rg_node_info* info) {
  if (!removal_window(g_frame.load(std::memory_order_relaxed)) || !g_sky_rs || !g_sky_rtv_heap || !g_sky_dsv_heap)
    return 0;
  auto* color = static_cast<ID3D12Resource*>(g_host->flow_texture(g_color, nullptr));
  auto* depth = static_cast<ID3D12Resource*>(g_host->flow_texture(g_depth, nullptr));
  auto* t64 = static_cast<ID3D12Resource*>(g_host->flow_texture(g_t64, nullptr));
  if (!color || !depth || !t64) {
    g_sky_missing.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  const D3D12_RESOURCE_DESC cd = color->GetDesc(), dd = depth->GetDesc();
  bool stencil = false;
  const DXGI_FORMAT cf = rtv_format(cd.Format), df = dsv_format(dd.Format, &stencil);
  ID3D12PipelineState* pso = df != DXGI_FORMAT_UNKNOWN && cd.SampleDesc.Count == 1 && dd.SampleDesc.Count == 1 &&
                                     cd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D
                                 ? sky_pso(cf, df)
                                 : nullptr;
  if (!pso) {
    g_sky_unsupported.fetch_add(1, std::memory_order_relaxed);
    g_sky_fmt.store((uint32_t(cd.Format) << 16) | uint32_t(dd.Format), std::memory_order_relaxed);
    return 0;
  }
  rg_descriptors desc{};
  if (!g_host->gpu_descriptors(1, &desc)) {  // M10: t0 from the engine's own ring
    g_sky_no_srv.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  if (!g_host->flow_transition(g_color, D3D12_RESOURCE_STATE_RENDER_TARGET) ||
      !g_host->flow_transition(g_depth, D3D12_RESOURCE_STATE_DEPTH_READ) ||
      !g_host->flow_transition(g_t64, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE))
    return 0;
  D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
  sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sv.Texture2D.MipLevels = 1;
  g_dev->CreateShaderResourceView(t64, &sv, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(desc.cpu)});
  g_sky_srv.fetch_add(1, std::memory_order_relaxed);
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_sky_rtv_heap->GetCPUDescriptorHandleForHeapStart();
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = g_sky_dsv_heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_RENDER_TARGET_VIEW_DESC rv{};
  rv.Format = cf;
  rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  g_dev->CreateRenderTargetView(color, &rv, rtv);
  D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
  dv.Format = df;
  dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  dv.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH | (stencil ? D3D12_DSV_FLAG_READ_ONLY_STENCIL : D3D12_DSV_FLAG_NONE);
  g_dev->CreateDepthStencilView(depth, &dv, dsv);
  auto* cl = static_cast<ID3D12GraphicsCommandList*>(info->cmdlist);
  const uint32_t w = static_cast<uint32_t>(cd.Width < dd.Width ? cd.Width : dd.Width);
  const uint32_t h = cd.Height < dd.Height ? cd.Height : dd.Height;
  const D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
  const D3D12_RECT sc{0, 0, LONG(w), LONG(h)};
  cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
  cl->RSSetViewports(1, &vp);
  cl->RSSetScissorRects(1, &sc);
  cl->SetGraphicsRootSignature(g_sky_rs);
  cl->SetPipelineState(pso);
  cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cl->SetGraphicsRootDescriptorTable(1, D3D12_GPU_DESCRIPTOR_HANDLE{desc.gpu});
  const float t = float(GetTickCount64() % 100000) / 1000.f;
  for (const float z : {0.f, 1.f}) {
    const float c[4] = {z, t, 8.f, 0.f};
    cl->SetGraphicsRoot32BitConstants(0, 4, c, 0);
    cl->DrawInstanced(3, 1, 0, 0);
  }
  g_sky_draws.fetch_add(1, std::memory_order_relaxed);
  g_sky_fmt.store((uint32_t(cd.Format) << 16) | uint32_t(dd.Format), std::memory_order_relaxed);
  return RG_PASS_TOUCHED_STATE;  // root signature, PSO, RTs, viewport: the host restores the engine's
}

void log_remove_test(uint32_t n) {
  rg_remove_stats st;
  g_host->node_remove_stats(g_remove, &st);
  auto u = [](uint64_t a) { return static_cast<unsigned long long>(a); };
  char line[600];
  std::snprintf(line, sizeof line,
                "node remove test: at RgNode_SelfTest #%u (window %s) | RenderVolumetricClouds CS dispatches while REMOVED "
                "%llu, while running %llu, guard %llu, switches %llu | wrapped %u on %u removed %llu ran %llu replayed "
                "%llu overflows %llu refused %llu",
                n, removal_window(n) ? "REMOVED" : "running", u(g_disp_on.load()), u(g_disp_off.load()),
                u(g_disp_guard.load()), u(g_toggles.load()), st.wrapped, st.on, u(st.removed), u(st.ran),
                u(st.replayed), u(st.overflows), u(st.refused));
  const bool bad = g_disp_on.load() || st.overflows || st.refused;
  g_host->log(bad ? RG_LOG_WARN : RG_LOG_INFO, line);
  const uint32_t fmt = g_sky_fmt.load();
  std::snprintf(line, sizeof line,
                "node replace test: sky passes drawn %llu, resources missing %llu, unsupported %llu (colour fmt %u, "
                "depth fmt %u) | M10 t64 SRVs from the engine ring %llu, ring refused %llu",
                u(g_sky_draws.load()), u(g_sky_missing.load()), u(g_sky_unsupported.load()), fmt >> 16, fmt & 0xffff,
                u(g_sky_srv.load()), u(g_sky_no_srv.load()));
  g_host->log(g_sky_missing.load() || g_sky_unsupported.load() || g_sky_no_srv.load() ? RG_LOG_WARN : RG_LOG_INFO,
              line);
}

uint32_t pattern(uint32_t w) { return (w & 0xff) | (((w >> 8) & 0xff) << 8) | (0x5au << 16) | (0xffu << 24); }

uint32_t take(uint32_t slot) {  // the u32 the GPU copied into `slot`, and the sentinel back in its place
  auto* p = reinterpret_cast<volatile uint32_t*>(g_rb_map + slot * kSlotBytes);
  const uint32_t v = *p;
  *p = kSentinel;
  return v;
}

void copy_texel(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, uint32_t slot) {
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = g_rb;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Offset = uint64_t(slot) * kSlotBytes;
  dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
  D3D12_TEXTURE_COPY_LOCATION from{};
  from.pResource = src;
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  from.SubresourceIndex = 0;
  const D3D12_BOX box{0, 0, 0, 1, 1, 1};
  cl->CopyTextureRegion(&dst, 0, 0, 0, &from, &box);
}

void on_device(void*, void* device) {
  g_dev = static_cast<ID3D12Device*>(device);
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = 3 * kRing * kSlotBytes;  // rings A (writer), B (reader), C (after Lighting)
  rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  void* map = nullptr;
  if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&g_rb))) ||
      FAILED(g_rb->Map(0, nullptr, &map)) || FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtv_heap)))) {
    g_host->log(RG_LOG_ERROR, "node content test: readback / RTV heap not created - test OFF");
    return;
  }
  g_rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
  sky_create_device_objects();
  auto* m = static_cast<uint8_t*>(map);
  for (uint32_t i = 0; i < 3 * kRing; ++i) *reinterpret_cast<uint32_t*>(m + i * kSlotBytes) = kSentinel;
  g_rb_map = m;
}

// Writer, inside RgNode_SelfTest: harvest the slot of 4 runs ago, clear, copy the control texel.
void write_and_control(ID3D12GraphicsCommandList* cl, ID3D12Resource* tmp) {
  const uint32_t w = g_wn.fetch_add(1, std::memory_order_relaxed) + 1, s = w % kRing;
  const uint32_t v = take(s);
  if (v == kSentinel) {
    g_ctl.pending.fetch_add(1, std::memory_order_relaxed);
  } else if (v == g_expect[s]) {
    g_ctl.ok.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_ctl.bad.fetch_add(1, std::memory_order_relaxed);
    g_ctl.last_bad.store(v, std::memory_order_relaxed);
  }
  g_expect[s] = pattern(w);
  const float c[4] = {float(w & 0xff) / 255.f, float((w >> 8) & 0xff) / 255.f, float(0x5a) / 255.f, 1.f};
  if (!g_host->flow_transition(g_tmp, D3D12_RESOURCE_STATE_RENDER_TARGET)) return;
  g_dev->CreateRenderTargetView(tmp, nullptr, g_rtv);
  cl->ClearRenderTargetView(g_rtv, c, 0, nullptr);
  g_host->flow_transition(g_tmp, D3D12_RESOURCE_STATE_COPY_SOURCE);
  copy_texel(cl, tmp, s);
}

uint32_t on_reader(void*, const rg_node_info* info) {
  if (!g_rb_map) return 0;
  uint32_t id = 0;
  auto* tmp = static_cast<ID3D12Resource*>(g_host->flow_texture(g_tmp_by_name, &id));
  if (!tmp) {
    g_rd.miss.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  const uint32_t r = g_rn.fetch_add(1, std::memory_order_relaxed) + 1, s = kRing + r % kRing;
  const uint32_t v = take(s);
  if (v == kSentinel) {
    g_rd.pending.fetch_add(1, std::memory_order_relaxed);
  } else if ((v >> 16) == 0xff5au) {
    g_rd.ok.fetch_add(1, std::memory_order_relaxed);
    if ((v & 0xffff) != g_rd_prev) g_rd.fresh.fetch_add(1, std::memory_order_relaxed);
    g_rd_prev = v & 0xffff;
  } else {
    g_rd.bad.fetch_add(1, std::memory_order_relaxed);
    g_rd.last_bad.store(v, std::memory_order_relaxed);
  }
  if (g_host->flow_transition(g_tmp_by_name, D3D12_RESOURCE_STATE_COPY_SOURCE))
    copy_texel(static_cast<ID3D12GraphicsCommandList*>(info->cmdlist), tmp, s);
  if (r > 3 && r % 1800 != 0) return 0;
  char line[400];
  std::snprintf(line, sizeof line,
                "node content test: reader #%u (id %u) | control (writer's own copy): ok %llu bad %llu pending %llu "
                "last_bad %08x | ACROSS GROUPS (reader): ok %llu fresh %llu bad %llu pending %llu miss %llu "
                "last_bad %08x last %04x",
                r, id, static_cast<unsigned long long>(g_ctl.ok.load()),
                static_cast<unsigned long long>(g_ctl.bad.load()), static_cast<unsigned long long>(g_ctl.pending.load()),
                g_ctl.last_bad.load(), static_cast<unsigned long long>(g_rd.ok.load()),
                static_cast<unsigned long long>(g_rd.fresh.load()), static_cast<unsigned long long>(g_rd.bad.load()),
                static_cast<unsigned long long>(g_rd.pending.load()), static_cast<unsigned long long>(g_rd.miss.load()),
                g_rd.last_bad.load(), g_rd_prev);
  g_host->log(g_rd.bad.load() || g_ctl.bad.load() ? RG_LOG_WARN : RG_LOG_INFO, line);
  return 0;  // a clear and copies only: no PSO, root signature or heaps touched
}

// The wrap test's report - from RgNode_SelfTest, which runs every frame whatever the wraps do.
void log_wrap(const char* what, rg_handle h, char* out, size_t size);
void log_wrap_test(uint32_t n) {
  char fog[200], lit[200], line[900];
  log_wrap("fog", g_wrap_fog, fog, sizeof fog);
  log_wrap("Lighting", g_wrap_lighting, lit, sizeof lit);
  auto u = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load()); };
  std::snprintf(line, sizeof line,
                "node wrap test: at RgNode_SelfTest #%u | VolumetricFog: before %llu after %llu unpaired %llu, "
                "a58d01db dispatches INSIDE %llu outside %llu, 5c7ab952 (indirect) INSIDE %llu outside %llu direct %llu, "
                "next member adjacent %llu NOT %llu | Lighting: before "
                "%llu after %llu same_cl %llu, member on the group's list %llu not %llu | ring C (transient after the "
                "group): ok %llu bad %llu pending %llu miss %llu last_bad %08x | %s | %s",
                n, u(g_wt.fog_before), u(g_wt.fog_after), u(g_wt.fog_unpaired), u(g_wt.disp_in),
                u(g_wt.disp_out), u(g_wt.ind_in), u(g_wt.ind_out), u(g_wt.ind_direct), u(g_wt.adjacent), u(g_wt.not_adjacent), u(g_wt.lit_before), u(g_wt.lit_after),
                u(g_wt.lit_same_cl), u(g_wt.node_in_lit), u(g_wt.node_out_lit), u(g_lc.ok), u(g_lc.bad),
                u(g_lc.pending), u(g_lc.miss), g_lc.last_bad.load(), fog, lit);
  const bool bad = g_wt.disp_out.load() || g_wt.fog_unpaired.load() || g_lc.bad.load() || !g_wt.ind_in.load() ||
                   g_wt.ind_out.load() || g_wt.ind_direct.load();
  g_host->log(bad ? RG_LOG_WARN : RG_LOG_INFO, line);
}

uint32_t on_node(void*, const rg_node_info* info) {
  const uint32_t n = g_node_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  g_frame.store(n, std::memory_order_relaxed);
  if (g_remove > 0 && n % kRemoveWindow == 0) {
    const bool on = removal_window(n);
    g_host->node_remove_set(g_remove, on ? 1u : 0u);
    g_toggles.fetch_add(1, std::memory_order_relaxed);
    char line[120];
    std::snprintf(line, sizeof line, "node remove test: frame #%u - world clouds %s", n,
                  on ? "REMOVED (10 s)" : "BACK");
    g_host->log(RG_LOG_INFO, line);
  }
  (t_after_fog_cl && t_after_fog_cl == info->cmdlist ? g_wt.adjacent : g_wt.not_adjacent)
      .fetch_add(1, std::memory_order_relaxed);
  t_after_fog_cl = nullptr;
  (t_lighting_cl && t_lighting_cl == info->cmdlist ? g_wt.node_in_lit : g_wt.node_out_lit)
      .fetch_add(1, std::memory_order_relaxed);
  uint32_t id = 0;
  void* t64 = g_host->flow_texture(g_t64, &id);
  uint32_t tmp_id = 0;
  auto* tmp = g_tmp > 0 ? static_cast<ID3D12Resource*>(g_host->flow_texture(g_tmp, &tmp_id)) : nullptr;
  (tmp ? g_tmp_hits : g_tmp_misses).fetch_add(1, std::memory_order_relaxed);
  if (tmp && g_rb_map) write_and_control(static_cast<ID3D12GraphicsCommandList*>(info->cmdlist), tmp);
  if (n > 3 && n % 1800 != 0) return 0;
  log_wrap_test(n);
  if (g_remove > 0) log_remove_test(n);
  rg_node_stats st;
  g_host->node_stats(g_node, &st);
  D3D12_RESOURCE_DESC d{};
  if (tmp) d = tmp->GetDesc();
  char line[480];
  std::snprintf(line, sizeof line,
                "node self-test: run #%u, cmdlist %p, view %u, t64 by name -> id %u %p | own transient -> id %u %p "
                "%llu x %u fmt %u flags 0x%x (hits %llu misses %llu) | inserted %u, executes %llu, declares %llu, "
                "no_cmdlist %llu, requests %llu, skipped_toplevel %u",
                n, info->cmdlist, info->view, id, t64, tmp_id, static_cast<void*>(tmp),
                static_cast<unsigned long long>(d.Width), d.Height, static_cast<unsigned>(d.Format),
                static_cast<unsigned>(d.Flags), static_cast<unsigned long long>(g_tmp_hits.load()),
                static_cast<unsigned long long>(g_tmp_misses.load()), st.inserted,
                static_cast<unsigned long long>(st.executes), static_cast<unsigned long long>(st.declares),
                static_cast<unsigned long long>(st.no_cmdlist), static_cast<unsigned long long>(st.requests),
                st.skipped_toplevel);
  g_host->log(t64 && tmp ? RG_LOG_INFO : RG_LOG_WARN, line);
  return 0;  // recorded nothing
}

uint32_t on_fog_before(void*, const rg_node_info*) {
  g_wt.fog_before.fetch_add(1, std::memory_order_relaxed);
  t_in_fog = true;
  t_after_fog_cl = nullptr;
  return 0;
}

uint32_t on_fog_after(void*, const rg_node_info* info) {
  g_wt.fog_after.fetch_add(1, std::memory_order_relaxed);
  if (!t_in_fog) g_wt.fog_unpaired.fetch_add(1, std::memory_order_relaxed);
  t_in_fog = false;
  t_after_fog_cl = info->cmdlist;
  return 0;
}

void on_fog_dispatch(void*, const rg_dispatch_info*) {
  (t_in_fog ? g_wt.disp_in : g_wt.disp_out).fetch_add(1, std::memory_order_relaxed);
}

void on_fog_indirect(void*, const rg_dispatch_info* info) {
  (t_in_fog ? g_wt.ind_in : g_wt.ind_out).fetch_add(1, std::memory_order_relaxed);
  if (!(info->flags & RG_DISPATCH_INDIRECT) || info->gx | info->gy | info->gz)
    g_wt.ind_direct.fetch_add(1, std::memory_order_relaxed);
}

uint32_t on_lighting_before(void*, const rg_node_info* info) {
  g_wt.lit_before.fetch_add(1, std::memory_order_relaxed);
  t_lighting_cl = info->cmdlist;
  return 0;
}

void log_wrap(const char* what, rg_handle h, char* out, size_t size) {
  rg_wrap_stats st;
  g_host->node_wrap_stats(h, &st);
  std::snprintf(out, size, "%s: wrapped %u shared %u executes %llu declares %llu no_cmdlist %llu rebinds %llu jobs %llu", what,
                st.wrapped, st.shared, static_cast<unsigned long long>(st.executes),
                static_cast<unsigned long long>(st.declares), static_cast<unsigned long long>(st.no_cmdlist),
                static_cast<unsigned long long>(st.rebinds), static_cast<unsigned long long>(st.jobs));
}

uint32_t on_lighting_after(void*, const rg_node_info* info) {
  g_wt.lit_after.fetch_add(1, std::memory_order_relaxed);
  if (t_lighting_cl == info->cmdlist) g_wt.lit_same_cl.fetch_add(1, std::memory_order_relaxed);
  t_lighting_cl = nullptr;
  const uint32_t n = g_ln.fetch_add(1, std::memory_order_relaxed) + 1;
  // Ring C: this frame's clear must be in the transient after the whole group.
  uint32_t id = 0;
  auto* tmp = g_rb_map ? static_cast<ID3D12Resource*>(g_host->flow_texture(g_tmp, &id)) : nullptr;
  if (!tmp) {
    g_lc.miss.fetch_add(1, std::memory_order_relaxed);
  } else {
    const uint32_t s = n % kRing, v = take(2 * kRing + s);
    if (v == kSentinel) {
      g_lc.pending.fetch_add(1, std::memory_order_relaxed);
    } else if (v == g_expect_c[s]) {
      g_lc.ok.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_lc.bad.fetch_add(1, std::memory_order_relaxed);
      g_lc.last_bad.store(v, std::memory_order_relaxed);
    }
    g_expect_c[s] = pattern(g_wn.load(std::memory_order_relaxed));
    if (g_host->flow_transition(g_tmp, D3D12_RESOURCE_STATE_COPY_SOURCE))
      copy_texel(static_cast<ID3D12GraphicsCommandList*>(info->cmdlist), tmp, 2 * kRing + s);
  }
  return 0;  // copies only
}

void on_bake(void*, const rg_dispatch_info*) {
  uint32_t id = 0;
  auto* res = static_cast<ID3D12Resource*>(g_host->flow_texture(g_t64, &id));
  const uint32_t n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n > 3 && n % 1800 != 0) return;
  rg_flow_stats st;
  g_host->flow_stats(g_t64, &st);
  char line[320];
  if (res) {
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    std::snprintf(line, sizeof line,
                  "flow probe: t64 by name at 53c455b6 #%u -> id %u, %p, %llu x %u fmt %u flags 0x%x dim %u | hits %llu "
                  "misses %llu outside %llu id_changes %llu",
                  n, id, static_cast<void*>(res), static_cast<unsigned long long>(d.Width), d.Height,
                  static_cast<unsigned>(d.Format), static_cast<unsigned>(d.Flags), static_cast<unsigned>(d.Dimension),
                  static_cast<unsigned long long>(st.hits), static_cast<unsigned long long>(st.misses),
                  static_cast<unsigned long long>(st.outside), static_cast<unsigned long long>(st.id_changes));
  } else {
    std::snprintf(line, sizeof line,
                  "flow probe: t64 by name at 53c455b6 #%u -> NULL (attached %u) | hits %llu misses %llu outside %llu",
                  n, st.attached, static_cast<unsigned long long>(st.hits),
                  static_cast<unsigned long long>(st.misses), static_cast<unsigned long long>(st.outside));
  }
  g_host->log(res ? RG_LOG_INFO : RG_LOG_WARN, line);
}

}  // namespace

void register_with(const rg_host* host) {
  g_host = host;
  g_t64 = host->flow_register("t64 (cloud shadow map)", 0x583c0f7fu);
  const auto bake = hash_literal::hash_from_hex("53c455b664225ba5849d29badc41d3fe");
  const rg_handle sub = g_t64 > 0 ? host->dispatch_subscribe("flow probe: t64 at 53c455b6", bake.data(), &on_bake, nullptr)
                                   : g_t64;
  if (g_t64 <= 0 || sub <= 0) host->log(RG_LOG_ERROR, "flow probe: not registered");
  g_node = host->node_register("RgNode_SelfTest", "CRenderNode_VolumetricFog", &on_node, nullptr);
  if (g_node <= 0) {
    host->log(RG_LOG_ERROR, "node self-test: not registered");
    return;
  }
  const rg_flow_desc tmp_desc{512, 512, 5, 0x39, {}};
  g_tmp = host->node_alloc(g_node, "rgselftest/tmp", &tmp_desc);
  {  // published names: "<mod>/<name>", one publisher each (RG_E_ARG = -1, RG_E_TAKEN = -2)
    const rg_handle bad = host->node_alloc(g_node, "selftest_tmp", &tmp_desc);
    const rg_handle twice = host->node_alloc(g_node, "rgselftest/tmp", &tmp_desc);
    const rg_handle lookup_bad = host->flow_register("selftest_tmp", 0);
    const bool pass = bad == RG_E_ARG && twice == RG_E_TAKEN && lookup_bad == RG_E_ARG;
    char line[200];
    std::snprintf(line, sizeof line, "node self-test: published names - no tag %d, the same name twice %d, lookup "
                  "without a tag %d - %s", bad, twice, lookup_bad, pass ? "PASS" : "FAIL (expected -1 -2 -1)");
    host->log(pass ? RG_LOG_INFO : RG_LOG_ERROR, line);
  }
  g_tmp_by_name = host->flow_register("rgselftest/tmp", 0);
  const int32_t use = g_t64 > 0 ? host->node_use(g_node, g_t64, RG_FLOW_READ | RG_FLOW_KEEP) : g_t64;
  if (g_tmp <= 0 || use != 0) {
    host->log(RG_LOG_ERROR, "node self-test: declarations not registered");
    return;
  }
  g_reader = host->node_register("RgNode_SelfTestReader", "CRenderNode_RenderVolumetricClouds", &on_reader, nullptr);
  if (g_reader <= 0 || g_tmp_by_name <= 0 ||
      host->node_use(g_reader, g_tmp_by_name, RG_FLOW_READ | RG_FLOW_KEEP) != 0 ||
      host->device_ready(&on_device, nullptr) <= 0)
    host->log(RG_LOG_ERROR, "node content test: not registered");
  const auto fog_cs = hash_literal::hash_from_hex("a58d01db6b07b209253f108b5a0e8760");
  const auto fog_ind = hash_literal::hash_from_hex("5c7ab952808010eb4005e1858d4a864b");
  g_wrap_fog = host->node_wrap("CRenderNode_VolumetricFog", &on_fog_before, &on_fog_after, nullptr);
  g_wrap_lighting = host->node_wrap("Lighting", &on_lighting_before, &on_lighting_after, nullptr);
  if (g_wrap_fog <= 0 || g_wrap_lighting <= 0 ||
      host->dispatch_subscribe("wrap test: fog CS a58d01db", fog_cs.data(), &on_fog_dispatch, nullptr) <= 0 ||
      host->dispatch_subscribe("indirect test: fog CS 5c7ab952", fog_ind.data(), &on_fog_indirect, nullptr) <= 0)
    host->log(RG_LOG_ERROR, "node wrap test: not registered");
  g_remove = host->node_remove("CRenderNode_RenderVolumetricClouds", 0);
  {  // one removal per node: a second one is refused (RG_E_TAKEN = -2)
    const rg_handle second = host->node_remove("CRenderNode_RenderVolumetricClouds", 0);
    char line[160];
    std::snprintf(line, sizeof line, "node remove test: a second removal of RenderVolumetricClouds = %d - %s", second,
                  second == RG_E_TAKEN ? "PASS (TAKEN)" : "FAIL (expected -2)");
    host->log(second == RG_E_TAKEN ? RG_LOG_INFO : RG_LOG_ERROR, line);
  }
  g_color = host->flow_register("scene colour", 0x3d7e6258u);
  g_depth = host->flow_register("scene depth", 0xdebf0c27u);
  if (g_color <= 0 || g_depth <= 0 ||
      host->node_wrap("CRenderNode_RenderVolumetricClouds", nullptr, &on_clouds_after, nullptr) <= 0)
    host->log(RG_LOG_ERROR, "node replace test: not registered");
  const auto clouds_cs = hash_literal::hash_from_hex("3dd31a426b94f2634802d20d2a04e88b");
  if (g_remove <= 0 ||
      host->dispatch_subscribe("remove test: clouds CS 3dd31a42", clouds_cs.data(), &on_removed_dispatch, nullptr) <= 0)
    host->log(RG_LOG_ERROR, "node remove test: not registered");
}

}  // namespace selftest::flow_probe
