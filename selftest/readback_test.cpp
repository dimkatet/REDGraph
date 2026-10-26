// ReadbackTest ([Tests] ReadbackTest = true, + REDGraph.ini [Dev] Readback = true; docs/internals.md "GPU readback
// (dev)"): the host's dev readback (rg_dev.h, what CET's Game.RG_Readback calls), driven from a node of ours.
//   writer:  RgNode_ReadbackWriter after CRenderNode_ClassifyMaterials (Lighting) allocates "rgreadback/pattern"
//            (40 x 24, engine format 5 = R8G8B8A8_UNORM, flags 0x39) and clears it every run to a KNOWN pattern:
//            base (10 20 30 ff), x<20 y<12 (ff 00 00 ff), x>=20 y>=12 (00 ff 00 ff), x>=33 y<5 the run counter
//            (lo, hi, 5a, ff). 40 texels = 160 B rows: the copy's 256-B pitch must be repacked.
//   steps, one after the other from run 300, each polled every writer run:
//     1 KNOWN   flow:rgreadback/pattern after:RgNode_ReadbackWriter - the .dds read back and checked texel by texel
//               (the counter within the runs between the request and the result) -> PASS / FAIL
//     2 ENGINE  flow:583c0f7f after:CRenderNode_RenderVolumetricCloudsLighting (t64, the cloud shadow map, right
//               after its writer): done, 512 x 512, DXGI 28, the file's size -> PASS / FAIL; its stats logged
//     3 BEFORE  flow:583c0f7f before:CRenderNode_VolumetricFog - informational (t64 is a lighting global there)
//     4 BLOCK   block:CRenderInterface::CalculateSharedConstants (cb12): done, 2512 bytes -> PASS / FAIL
//     5 REFUSED a bad spec, an unknown block, no node, a node that never runs (frames:30) -> all "failed"
//     9 VSOUT   vsout:69090ccd (the sky-scattering pass's VS, every frame) - Stream Output of the engine's own VS: done,
//               vertices = the draw's count x instances (.json), the file = vertices x stride, SV_Position without NaN /
//               Inf -> PASS / FAIL
//    10 REFUSED vsout with a bad hash, vsout of a VS never drawn (frames:30) -> "failed"
//   then "readback test: DONE - N pass, M fail". Nothing on screen.

#include <Windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "rg_dev.h"
#include "selftest.hpp"

namespace selftest::readback_test {
namespace {

constexpr uint32_t kW = 40, kH = 24, kStart = 300;

const rg_host* g_host = nullptr;
ID3D12Device* g_dev = nullptr;
ID3D12DescriptorHeap* g_rtv_heap = nullptr;
rg_handle g_node = 0, g_pat = 0;
rg_dev_readback_fn g_rb = nullptr;
rg_dev_readback_result_fn g_rb_result = nullptr;
std::atomic<uint32_t> g_wn{0};

// The steps; touched by the writer's fn only (one node, one thread at a time).
struct Step {
  const char* what;
  const char* spec;
  int expect;  // RG_RB_DONE / RG_RB_FAILED; 0 = informational
};
const Step kSteps[] = {
    {"KNOWN", "flow:rgreadback/pattern after:RgNode_ReadbackWriter", RG_RB_DONE},
    {"ENGINE", "flow:583c0f7f after:CRenderNode_RenderVolumetricCloudsLighting", RG_RB_DONE},
    {"BEFORE", "flow:583c0f7f before:CRenderNode_VolumetricFog", 0},
    {"BLOCK", "block:CRenderInterface::CalculateSharedConstants", RG_RB_DONE},
    {"REFUSED garbage", "garbage", RG_RB_FAILED},
    {"REFUSED block", "block:NoSuchBlock", RG_RB_FAILED},
    {"REFUSED no node", "flow:583c0f7f", RG_RB_FAILED},
    {"REFUSED never runs", "flow:583c0f7f after:RgNoSuchNode_ReadbackTest frames:30", RG_RB_FAILED},
    {"VSOUT", "vsout:69090ccdf2e91e0bd1cc70d88a0b8434", RG_RB_DONE},
    {"REFUSED vsout hash", "vsout:69090ccx", RG_RB_FAILED},
    {"REFUSED vsout never drawn", "vsout:0123456789abcdef frames:30", RG_RB_FAILED},
};
constexpr uint32_t kVsOutStep = 8;
constexpr uint32_t kNumSteps = sizeof(kSteps) / sizeof(kSteps[0]);
uint32_t g_step = 0, g_pass = 0, g_fail = 0;
int32_t g_id = 0;
uint32_t g_req_run = 0;
bool g_done = false;

void log(bool ok, const std::string& s) { g_host->log(ok ? RG_LOG_INFO : RG_LOG_ERROR, s.c_str()); }

uint32_t expected(uint32_t x, uint32_t y, uint32_t counter) {
  auto px = [](uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | (g << 8) | (b << 16) | (a << 24); };
  if (x >= 33 && y < 5) return px(counter & 0xff, (counter >> 8) & 0xff, 0x5a, 0xff);
  if (x < 20 && y < 12) return px(0xff, 0, 0, 0xff);
  if (x >= 20 && y >= 12) return px(0, 0xff, 0, 0xff);
  return px(0x10, 0x20, 0x30, 0xff);
}

std::vector<uint8_t> read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  return in ? std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>())
            : std::vector<uint8_t>();
}

uint32_t u32(const std::vector<uint8_t>& b, size_t at) {
  uint32_t v = 0;
  if (at + 4 <= b.size()) std::memcpy(&v, b.data() + at, 4);
  return v;
}

// Step 1: the .dds holds exactly the pattern of one writer run between the request and now.
bool check_known(const rg_readback_info& i, std::string* why) {
  const std::vector<uint8_t> f = read_file(i.path);
  constexpr size_t kHdr = 148;
  char b[200];
  if (f.size() != kHdr + kW * kH * 4 || std::memcmp(f.data(), "DDS ", 4) != 0 || u32(f, 12) != kH ||
      u32(f, 16) != kW || u32(f, 128) != DXGI_FORMAT_R8G8B8A8_UNORM) {
    std::snprintf(b, sizeof b, "file %zu bytes, %ux%u, DXGI %u (expected %zu, %ux%u, 28)", f.size(), u32(f, 16),
                  u32(f, 12), u32(f, 128), kHdr + kW * kH * 4, kW, kH);
    return *why = b, false;
  }
  const uint32_t c = u32(f, kHdr + 33 * 4);  // texel (33, 0): the counter
  const uint32_t counter = (c & 0xff) | (((c >> 8) & 0xff) << 8);
  const uint32_t now = g_wn.load();
  uint32_t bad = 0, first_x = 0, first_y = 0, got = 0;
  for (uint32_t y = 0; y < kH; ++y)
    for (uint32_t x = 0; x < kW; ++x) {
      const uint32_t v = u32(f, kHdr + (size_t(y) * kW + x) * 4);
      if (v != expected(x, y, counter) && !bad++) first_x = x, first_y = y, got = v;
    }
  const bool counter_ok = counter + 2 >= (g_req_run & 0xffff) && counter <= (now & 0xffff) + 2;
  std::snprintf(b, sizeof b, "%u bad texel(s)%s (first at %u,%u: %08x), counter %u in [%u, %u] %s", bad,
                bad ? "" : " of 960", first_x, first_y, got, counter, g_req_run, now, counter_ok ? "ok" : "OUT");
  *why = b;
  return !bad && counter_ok;
}

bool check_engine(const rg_readback_info& i, std::string* why) {
  const std::vector<uint8_t> f = read_file(i.path);
  char b[160];
  const bool ok = i.width == 512 && i.height == 512 && i.format == DXGI_FORMAT_R8G8B8A8_UNORM &&
                  f.size() == 148 + 512ull * 512 * 4 && i.bytes == 512ull * 512 * 4;
  std::snprintf(b, sizeof b, "%ux%u DXGI %u, file %zu bytes (expected 512x512 28, %llu)", i.width, i.height, i.format,
                f.size(), 148 + 512ull * 512 * 4);
  *why = b;
  return ok;
}

bool check_block(const rg_readback_info& i, std::string* why) {
  const std::vector<uint8_t> f = read_file(i.path);
  char b[120];
  std::snprintf(b, sizeof b, "file %zu bytes (expected 2512, cb12 on 2.5)", f.size());
  *why = b;
  return f.size() == 2512;
}

uint64_t json_u64(const std::string& j, const char* key) {
  const size_t at = j.find(std::string("\"") + key + "\": ");
  return at == std::string::npos ? ~0ull : std::strtoull(j.c_str() + at + std::strlen(key) + 4, nullptr, 10);
}

// Step 9: the Stream Output of the sky VS - one draw, every vertex there, positions finite.
bool check_vsout(const rg_readback_info& i, std::string* why) {
  const std::vector<uint8_t> f = read_file(i.path);
  std::string meta = i.path;
  meta = meta.substr(0, meta.size() - 4) + ".json";
  const std::vector<uint8_t> jb = read_file(meta.c_str());
  const std::string j(jb.begin(), jb.end());
  const uint64_t count = json_u64(j, "count"), inst = json_u64(j, "instances"), topo = json_u64(j, "topology");
  const uint64_t want = topo == 5 /* strip */ ? (count - 2) * 3 * inst : count * inst;
  const bool finite = std::string(i.text).find("nan 0 inf 0") != std::string::npos;
  char b[300];
  std::snprintf(b, sizeof b, "%u vertices x %u B = file %zu B; the draw: count %llu x %llu instances, topology %llu -> "
                "expected %llu vertices; SV_Position %s", i.width, i.row_bytes, f.size(),
                static_cast<unsigned long long>(count), static_cast<unsigned long long>(inst),
                static_cast<unsigned long long>(topo), static_cast<unsigned long long>(want),
                finite ? "finite" : "NOT finite");
  *why = b;
  return i.width && i.width == want && f.size() == uint64_t(i.width) * i.row_bytes && finite && i.height == 1;
}

void poll(uint32_t w) {
  if (g_done || w < kStart) return;
  if (!g_id) {
    g_req_run = w;
    g_id = g_rb(kSteps[g_step].spec);
    if (g_id == RG_E_UNAVAILABLE) {
      log(false, "readback test: the host answers RG_E_UNAVAILABLE - set [Dev] Readback = true in REDGraph.ini");
      g_done = true;
    }
    return;
  }
  rg_readback_info i{};
  const int32_t st = g_rb_result(g_id, &i);
  if (st == RG_RB_PENDING) return;
  const Step& s = kSteps[g_step];
  std::string why;
  bool ok = st == s.expect || !s.expect;
  if (ok && st == RG_RB_DONE) {
    if (g_step == 0) ok = check_known(i, &why);
    else if (g_step == 1) ok = check_engine(i, &why);
    else if (g_step == 3) ok = check_block(i, &why);
    else if (g_step == kVsOutStep) ok = check_vsout(i, &why);
  }
  char line[1700];
  std::snprintf(line, sizeof line, "readback test step %u %s: '%s' -> %s | %s%s%s -> %s", g_step + 1, s.what, s.spec,
                st == RG_RB_DONE ? "done" : st == RG_RB_FAILED ? "failed" : "unknown", i.text, why.empty() ? "" : " | ",
                why.c_str(), !s.expect ? "INFO" : ok ? "PASS" : "FAIL");
  log(ok, line);
  if (s.expect) (ok ? g_pass : g_fail)++;
  g_id = 0;
  if (++g_step == kNumSteps) {
    g_done = true;
    char d[96];
    std::snprintf(d, sizeof d, "readback test: DONE - %u pass, %u fail", g_pass, g_fail);
    log(!g_fail, d);
  }
}

void on_device(void*, void* device) {
  g_dev = static_cast<ID3D12Device*>(device);
  const D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  if (FAILED(g_dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtv_heap)))) {
    g_rtv_heap = nullptr;
    g_host->log(RG_LOG_ERROR, "readback test: RTV heap not created - test OFF");
  }
}

uint32_t on_writer(void*, const rg_node_info* info) {
  auto* tex = g_rtv_heap ? static_cast<ID3D12Resource*>(g_host->flow_texture(g_pat, nullptr)) : nullptr;
  if (!tex || !g_host->flow_transition(g_pat, D3D12_RESOURCE_STATE_RENDER_TARGET)) return 0;
  const uint32_t w = g_wn.load(std::memory_order_relaxed) + 1;
  auto* cl = static_cast<ID3D12GraphicsCommandList*>(info->cmdlist);
  const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
  g_dev->CreateRenderTargetView(tex, nullptr, rtv);
  auto f = [](uint32_t v) { return float(v) / 255.f; };
  const float base[4] = {f(0x10), f(0x20), f(0x30), 1.f}, red[4] = {1.f, 0.f, 0.f, 1.f}, green[4] = {0.f, 1.f, 0.f, 1.f};
  const float count[4] = {f(w & 0xff), f((w >> 8) & 0xff), f(0x5a), 1.f};
  const D3D12_RECT a{0, 0, 20, 12}, b{20, 12, 40, 24}, c{33, 0, 40, 5};
  cl->ClearRenderTargetView(rtv, base, 0, nullptr);
  cl->ClearRenderTargetView(rtv, red, 1, &a);
  cl->ClearRenderTargetView(rtv, green, 1, &b);
  cl->ClearRenderTargetView(rtv, count, 1, &c);
  g_wn.store(w, std::memory_order_relaxed);
  poll(w);
  return 0;  // clears only: no PSO, root signature or heaps touched
}

}  // namespace

void register_with(const rg_host* host, bool on) {
  if (!on) return;
  g_host = host;
  HMODULE m = GetModuleHandleW(L"REDGraph.dll");
  g_rb = m ? reinterpret_cast<rg_dev_readback_fn>(GetProcAddress(m, "rg_dev_readback")) : nullptr;
  g_rb_result = m ? reinterpret_cast<rg_dev_readback_result_fn>(GetProcAddress(m, "rg_dev_readback_result")) : nullptr;
  if (!g_rb || !g_rb_result) {
    host->log(RG_LOG_ERROR, "readback test: REDGraph.dll exports no rg_dev_readback - an older host; not run");
    return;
  }
  g_node = host->node_register("RgNode_ReadbackWriter", "CRenderNode_ClassifyMaterials", &on_writer, nullptr);
  const rg_flow_desc desc{kW, kH, 5, 0x39, {}};
  g_pat = g_node > 0 ? host->node_alloc(g_node, "rgreadback/pattern", &desc) : g_node;
  if (g_pat <= 0 || host->device_ready(&on_device, nullptr) <= 0) {
    char line[120];
    std::snprintf(line, sizeof line, "readback test: not registered (node %d, alloc %d)", g_node, g_pat);
    host->log(RG_LOG_ERROR, line);
    return;
  }
  host->log(RG_LOG_INFO, "readback test: registered - RgNode_ReadbackWriter writes rgreadback/pattern; steps from run 300");
}

}  // namespace selftest::readback_test
