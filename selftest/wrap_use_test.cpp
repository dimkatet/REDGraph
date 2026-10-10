// WrapUseTest ([Tests] WrapUseTest = true, docs/internals.md "Wrapping an engine node", declarations): node_wrap_use -
// a wrapped ENGINE node declares a transient it would otherwise never see, and the engine keeps it alive into the node.
//   writer:  our node RgNode_WrapUseWriter after CRenderNode_ClassifyMaterials (Lighting) allocates "rgwrapuse/tmp"
//            (64 x 64, engine format 5, flags 0x39), clears it to a colour that encodes its run counter (R, G = the
//            counter, B = 0x5a, A = 0xff) and copies texel (0,0) into ring A - the control of the readback itself.
//   user:    a wrap of CRenderNode_ApplyBloomAndTonemapping (PostFX - three groups later) with node_wrap_use(tmp, READ |
//            KEEP); its `before` takes the transient by its published name and copies texel (0,0) into ring B.
//            Without the declaration the transient's lifetime ends in Lighting and its memory is free to be aliased by
//            Transparents / PostFX transients; with it the copy must hold the writer's pattern with a CURRENT
//            counter (within 2 of the writer's latest - groups record in parallel, so PostFX may be recorded on the CPU
//            before this frame's Lighting: the exact counter is CPU-order, not GPU-order; run 2026-10-10 21:43 saw
//            exactly that). Anything that is not the pattern = the memory went to another transient.
//   control: a wrap of CRenderNode_DrawHUD (later in PostFX) with no declaration, the same copy into ring C -
//            informational: after the lifetime's end the engine may resolve the name to aliased memory, or to nothing.
//   group:   node_wrap("PostFX") + node_wrap_use - the host must log that a group's declarations are NOT issued.
//   registration: access 0 / an unknown flow handle refused (RG_E_ARG), the 9th declaration on one wrap RG_E_FULL.
// Rings are read back 4 runs later (a sentinel marks a copy the GPU has not done yet). Logged every 1800 writer runs:
// ring A ok / bad (exact), rings B / C ok (exact among them) / stale (the pattern, an old counter) / bad (not the
// pattern) / miss -> PASS when B has no bad, no stale and no miss and grows. REDGraph's [Diagnostics]
// RenderGraphDump census (graph-NNN-resources.txt) then lists two requests per run under ApplyBloomAndTonemapping
// more than without the test, the same count in both phases. Nothing on screen.

#include <d3d12.h>

#include <atomic>
#include <cstddef>
#include <cstdio>

#include "selftest.hpp"

namespace selftest::wrap_use_test {
namespace {

constexpr uint32_t kRing = 4, kSlotBytes = 512;  // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
constexpr uint32_t kSentinel = 0xdeadbeefu;
enum Ring : uint32_t { kA = 0, kB = 1, kC = 2 };

const rg_host* g_host = nullptr;
ID3D12Device* g_dev = nullptr;
ID3D12Resource* g_rb = nullptr;
uint8_t* g_rb_map = nullptr;
ID3D12DescriptorHeap* g_rtv_heap = nullptr;
rg_handle g_node = 0, g_tmp = 0, g_tmp_by_name = 0, g_wrap_use = 0, g_wrap_ctl = 0;
std::atomic<uint32_t> g_wn{0};  // writer runs = the colour of this frame's clear

struct Tally {
  std::atomic<uint64_t> ok{0}, exact{0}, stale{0}, bad{0}, pending{0}, miss{0};
  std::atomic<uint32_t> last_bad{0};
  std::atomic<uint32_t> n{0};
  uint32_t expect[kRing] = {};  // one thread at a time per ring: the node it is copied in records on one thread
};
Tally g_t[3];

uint32_t pattern(uint32_t w) { return (w & 0xff) | (((w >> 8) & 0xff) << 8) | (0x5au << 16) | (0xffu << 24); }

void copy_texel(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, uint32_t slot) {
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = g_rb;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint.Offset = uint64_t(slot) * kSlotBytes;
  dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};
  D3D12_TEXTURE_COPY_LOCATION from{};
  from.pResource = src;
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  const D3D12_BOX box{0, 0, 0, 1, 1, 1};
  cl->CopyTextureRegion(&dst, 0, 0, 0, &from, &box);
}

// Harvest the ring's slot of 4 runs ago, then copy texel (0,0) of the transient into it, expecting `expect`.
void sample(Ring ring, rg_handle flow, void* cmdlist, uint32_t expect) {
  Tally& t = g_t[ring];
  auto* tex = g_rb_map ? static_cast<ID3D12Resource*>(g_host->flow_texture(flow, nullptr)) : nullptr;
  if (!tex) {
    t.miss.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t s = t.n.fetch_add(1, std::memory_order_relaxed) % kRing, slot = ring * kRing + s;
  auto* p = reinterpret_cast<volatile uint32_t*>(g_rb_map + slot * kSlotBytes);
  const uint32_t v = *p;
  *p = kSentinel;
  if (v == kSentinel) {
    t.pending.fetch_add(1, std::memory_order_relaxed);
  } else if (v == t.expect[s]) {
    t.ok.fetch_add(1, std::memory_order_relaxed);
    t.exact.fetch_add(1, std::memory_order_relaxed);
  } else if (ring != kA && (v >> 16) == 0xff5au) {
    const int32_t d = static_cast<int32_t>(v & 0xffff) - static_cast<int32_t>(t.expect[s] & 0xffff);
    (d >= -2 && d <= 2 ? t.ok : t.stale).fetch_add(1, std::memory_order_relaxed);
  } else {
    t.bad.fetch_add(1, std::memory_order_relaxed);
    t.last_bad.store(v, std::memory_order_relaxed);
  }
  t.expect[s] = expect;
  if (g_host->flow_transition(flow, D3D12_RESOURCE_STATE_COPY_SOURCE))
    copy_texel(static_cast<ID3D12GraphicsCommandList*>(cmdlist), tex, slot);
}

void on_device(void*, void* device) {
  g_dev = static_cast<ID3D12Device*>(device);
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = 3 * kRing * kSlotBytes;
  rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  void* map = nullptr;
  if (FAILED(g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&g_rb))) ||
      FAILED(g_rb->Map(0, nullptr, &map)) || FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtv_heap)))) {
    g_host->log(RG_LOG_ERROR, "wrap use test: readback / RTV heap not created - test OFF");
    return;
  }
  auto* m = static_cast<uint8_t*>(map);
  for (uint32_t i = 0; i < 3 * kRing; ++i) *reinterpret_cast<uint32_t*>(m + i * kSlotBytes) = kSentinel;
  g_rb_map = m;
}

void report(uint32_t n) {
  auto u = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load()); };
  rg_wrap_stats su{}, sc{};
  g_host->node_wrap_stats(g_wrap_use, &su);
  g_host->node_wrap_stats(g_wrap_ctl, &sc);
  const Tally &a = g_t[kA], &b = g_t[kB], &c = g_t[kC];
  const bool bad = a.bad.load() || b.bad.load() || b.stale.load() || b.miss.load();
  const bool pass = !bad && b.ok.load() > 0;  // neither: nothing read back yet (the first reports)
  char line[800];
  std::snprintf(line, sizeof line,
                "wrap use test #%u | writer (control): ok %llu bad %llu pending %llu miss %llu last_bad %08x | "
                "ApplyBloomAndTonemapping, DECLARED: ok %llu (exact %llu) stale %llu bad %llu pending %llu miss %llu "
                "last_bad %08x (wrapped %u, runs %llu) | DrawHUD, not declared (informational): ok %llu (exact %llu) "
                "stale %llu bad %llu pending %llu miss %llu last_bad %08x (wrapped %u, runs %llu) -> %s",
                n, u(a.ok), u(a.bad), u(a.pending), u(a.miss), a.last_bad.load(), u(b.ok), u(b.exact), u(b.stale),
                u(b.bad), u(b.pending), u(b.miss), b.last_bad.load(), su.wrapped,
                static_cast<unsigned long long>(su.executes), u(c.ok), u(c.exact), u(c.stale), u(c.bad),
                u(c.pending), u(c.miss), c.last_bad.load(), sc.wrapped,
                static_cast<unsigned long long>(sc.executes), pass ? "PASS" : bad ? "FAIL" : "WAIT");
  g_host->log(bad ? RG_LOG_ERROR : RG_LOG_INFO, line);
}

uint32_t on_writer(void*, const rg_node_info* info) {
  auto* tmp = g_rb_map && g_rtv_heap ? static_cast<ID3D12Resource*>(g_host->flow_texture(g_tmp, nullptr)) : nullptr;
  if (!tmp) {
    g_t[kA].miss.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  const uint32_t w = g_wn.load(std::memory_order_relaxed) + 1;
  auto* cl = static_cast<ID3D12GraphicsCommandList*>(info->cmdlist);
  const float col[4] = {float(w & 0xff) / 255.f, float((w >> 8) & 0xff) / 255.f, float(0x5a) / 255.f, 1.f};
  if (!g_host->flow_transition(g_tmp, D3D12_RESOURCE_STATE_RENDER_TARGET)) return 0;
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
  g_dev->CreateRenderTargetView(tmp, nullptr, rtv);
  cl->ClearRenderTargetView(rtv, col, 0, nullptr);
  g_wn.store(w, std::memory_order_relaxed);
  sample(kA, g_tmp, cl, pattern(w));
  if (w > 3 && w % 1800 != 0) return 0;
  report(w);
  return 0;  // a clear and a copy: no PSO, root signature or heaps touched
}

uint32_t on_use(void*, const rg_node_info* info) {
  sample(kB, g_tmp_by_name, info->cmdlist, pattern(g_wn.load(std::memory_order_relaxed)));
  return 0;
}

uint32_t on_control(void*, const rg_node_info* info) {
  sample(kC, g_tmp_by_name, info->cmdlist, pattern(g_wn.load(std::memory_order_relaxed)));
  return 0;
}

uint32_t on_group(void*, const rg_node_info*) { return 0; }

}  // namespace

void register_with(const rg_host* host, bool on) {
  if (!on) return;
  g_host = host;
  if (host->struct_size < offsetof(rg_host, node_wrap_use) + sizeof(void*)) {
    host->log(RG_LOG_ERROR, "wrap use test: the host is older than node_wrap_use - not run");
    return;
  }
  g_node = host->node_register("RgNode_WrapUseWriter", "CRenderNode_ClassifyMaterials", &on_writer, nullptr);
  const rg_flow_desc desc{64, 64, 5, 0x39, {}};
  g_tmp = g_node > 0 ? host->node_alloc(g_node, "rgwrapuse/tmp", &desc) : g_node;
  g_tmp_by_name = host->flow_register("rgwrapuse/tmp", 0);
  g_wrap_use = host->node_wrap("CRenderNode_ApplyBloomAndTonemapping", &on_use, nullptr, nullptr);
  g_wrap_ctl = host->node_wrap("CRenderNode_DrawHUD", &on_control, nullptr, nullptr);
  const int32_t use = g_wrap_use > 0 && g_tmp_by_name > 0
                          ? host->node_wrap_use(g_wrap_use, g_tmp_by_name, RG_FLOW_READ | RG_FLOW_KEEP)
                          : RG_E_ARG;
  if (g_tmp <= 0 || use != 0 || g_wrap_ctl <= 0 || host->device_ready(&on_device, nullptr) <= 0) {
    char line[160];
    std::snprintf(line, sizeof line, "wrap use test: not registered (alloc %d, use %d, control wrap %d)", g_tmp, use,
                  g_wrap_ctl);
    host->log(RG_LOG_ERROR, line);
    return;
  }
  // Refusals: access 0, a flow handle that does not exist, a 9th declaration on one wrap.
  const int32_t no_access = host->node_wrap_use(g_wrap_ctl, g_tmp_by_name, 0);
  const int32_t no_flow = host->node_wrap_use(g_wrap_ctl, 999, RG_FLOW_READ);
  // (on a target no graph has - its 8 accepted declarations are never issued)
  const rg_handle full_wrap = host->node_wrap("RgNoSuchNode_WrapUseTest", &on_group, nullptr, nullptr);
  int32_t full = 0;
  for (int i = 0; i < 9 && full_wrap > 0; ++i) full = host->node_wrap_use(full_wrap, g_tmp_by_name, RG_FLOW_READ);
  // A group: accepted at registration, logged by the host as NOT issued when the group is first registered.
  const rg_handle group = host->node_wrap("PostFX", &on_group, nullptr, nullptr);
  const int32_t group_use = group > 0 ? host->node_wrap_use(group, g_tmp_by_name, RG_FLOW_READ) : group;
  const bool pass = no_access == RG_E_ARG && no_flow == RG_E_ARG && full == RG_E_FULL && group_use == 0;
  char line[240];
  std::snprintf(line, sizeof line,
                "wrap use test: registered; access 0 -> %d, no such flow -> %d, 9th declaration -> %d, on a group -> %d "
                "- %s (expected -1 -1 -3 0; the host must log the PostFX declaration as NOT issued)",
                no_access, no_flow, full, group_use, pass ? "PASS" : "FAIL");
  host->log(pass ? RG_LOG_INFO : RG_LOG_ERROR, line);
}

}  // namespace selftest::wrap_use_test
