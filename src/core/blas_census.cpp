// BLAS census ([Diagnostics] BlasCensus, 2026-10-17): which BLAS geometry - if any - carries the mesh a raster draw
// draws. The census names no shader itself: CLIENTS register what to watch (rg_host::census_watch) - vertex shaders,
// whose every indexed draw is joined to the BLAS triangle geometries the engine built, by the index buffer's GPU VA +
// index count:
//   STATIC  - a BLAS without ALLOW_UPDATE (built once, compacted): the BVH holds the mesh UNDEFORMED;
//   DYNAMIC - an updatable BLAS (the GPU-deform pool family): refits / rebuilds in the window say how alive it is;
//   NONE    - no BLAS geometry on that exact index range: not in the BVH, or in it with ANOTHER range of the same
//             index buffer (another LOD) - the nearest geometry in the same buffer is printed.
// - and compute shaders (typically the deform pool's writers, m_computeSkinning_*): dispatches and groups per window,
// and inside which CRenderMesh::EvaluatePositions calls (one chunk of one proxy written into its pool slot, by caller,
// material handler and the watched kernels it dispatched).
// Sources: the NvAPI BuildEx call of the engine's BLAS batch builder (rt_lib d_nv, every BLAS build / update with its
// geometry descs), the command list's IASetIndexBuffer / IASetVertexBuffers (vtable slots, the last views per recording
// thread), draw subscriptions on the watched VS, dispatch subscriptions on the watched CS. Off by default; NVIDIA only
// (other vendors build through the D3D12 vtable).

#include <Windows.h>
#include <d3d12.h>
#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "engine_231.hpp"
#include "internal.hpp"

namespace core::blas_census {
namespace {

using namespace e231;

// What the clients registered (census_watch, at registration): VS -> draw joins, CS -> dispatch counts. The draw /
// dispatch subscriptions are the host's own (passes), so they share its subscription tables with every client.
constexpr uint32_t kMaxWatch = 32;    // per stage; a CS index is a bit of a u32 mask
struct Watched {
  char name[48];
  char hash[33];
};
Watched g_watch[kMaxWatch];      // VS
Watched g_watch_cs[kMaxWatch];   // CS
std::atomic<uint32_t> g_nwatch{0}, g_nwatch_cs{0};
struct CsStats {
  std::atomic<uint64_t> dispatches{0}, indirect{0}, groups{0};
};
CsStats g_cs[kMaxWatch];
constexpr ULONGLONG kWindowMs = 10000;
constexpr size_t kMaxGeos = 600000;
constexpr uint32_t kExamples = 3;

// The draw's index / vertex buffers: the mesh batcher's chunk bind (BindBatchGeometry -> 0x1401f2b68, a folded name)
// calls IASetIndexBuffer / IASetVertexBuffers INLINE (GpuApi::BindVertexAndIndexBuffers is only its fallback branch -
// run 1 of 2026-10-17 read stale buffers from it). So the census patches the command list's vtable (slots 43 / 44,
// shared by every list of the device) and keeps the last views per recording thread.
constexpr size_t kSlotSetIb = 43, kSlotSetVb = 44;

constexpr uint32_t kAllowUpdate = 0x1, kPerformUpdate = 0x20;  // NVAPI_D3D12_..._BUILD_FLAG_*_EX

struct Geo {
  uint64_t dest = 0, vb = 0, vstride = 0;
  uint32_t flags = 0, vfmt = 0, vcount = 0, ifmt = 0, type = 0;
  uint32_t builds = 0, updates = 0;          // in the current window
  uint64_t total_builds = 0, total_updates = 0;
};
using Key = std::pair<uint64_t, uint32_t>;  // index buffer VA, index count

std::mutex g_mutex;
std::map<Key, Geo> g_geo;  // g_mutex
uint64_t g_nv_builds = 0, g_nv_updates = 0, g_nv_geoms = 0, g_nv_unread = 0;  // window, g_mutex

enum Cls : uint8_t { kStatic, kDynamic, kNone };
const char* const kClsName[] = {"STATIC", "DYNAMIC", "NONE"};
struct Example {
  uint64_t ib, vb0;
  uint32_t count, cls;
  Geo geo;
  bool vb_same;
  uint64_t near_ib;
  uint32_t near_count, same_buffer;
};
struct VsStats {
  uint64_t draws = 0;
  std::unordered_map<uint64_t, uint8_t> keys;  // hash(ib, count) -> class
  uint32_t per_cls[3] = {};
  Example ex[3][kExamples];
  uint32_t nex[3] = {};
};
VsStats g_vs[kMaxWatch];  // g_mutex
uint32_t g_log_windows = 0;

// Every CRenderMesh::EvaluatePositions call: the proxy, its caller (a BLAS's birth = AddProxies, its refit =
// UpdateSkinnedInstances' helper), the chunk material's handler index material[+0x58] (RTMaterialGenerator's slot - it
// and the vertex factory pick the kernel), the change stamp proxy[+0x90] the refit is gated by, and which pool
// writers the call dispatched (same thread). The caller is told by the return address, so it is right only while this
// detour is the OUTERMOST one: another plugin's detour around ours shows up as `other`.
struct Proxy {
  uint32_t mat58 = 0, stamp_first = 0, stamp_last = 0;
  uint32_t calls_birth = 0, calls_update = 0, calls_other = 0;  // in the window
  uint32_t cs_mask = 0;  // bits of the watched CS dispatched inside its calls (since the start)
};
constexpr size_t kMaxProxies = 16384;
std::unordered_map<uint64_t, Proxy> g_proxies;  // proxy* -> stats, g_mutex
std::atomic<uint64_t> g_eval_calls{0};           // window
uint64_t g_eval_birth = 0, g_eval_update = 0, g_eval_other = 0;  // window, g_mutex
thread_local uint32_t* t_cs_mask = nullptr;      // the current call's cs_mask (unordered_map nodes never move)
thread_local uint32_t t_eval_mat58 = ~0u;        // the current EvaluatePositions call's material[+0x58], ~0 outside
std::map<std::pair<uint32_t, uint32_t>, uint64_t> g_cs_mat58;  // (watched CS, material[+0x58]) -> dispatches inside EvaluatePositions, window, g_mutex
using Eval_t = void (*)(void*, void*, void*, uint64_t, uint64_t, uint64_t, uint64_t, void*);
Eval_t g_orig_eval = nullptr;
bool g_eval_attached = false;

std::atomic<bool> g_on{false};
std::atomic<ULONGLONG> g_next_log{0};
std::atomic<int> g_vt_state{0};  // 0 not patched, 1 patching, 2 patched, 3 failed
void** g_vt = nullptr;

struct Ib {
  uint64_t va = 0, vb0 = 0;  // the views' BufferLocation
  uint32_t size = 0, isz = 0, vb0_size = 0;
};
thread_local Ib t_ib;

uint64_t rd64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}
uint32_t rd32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

using SetIb_t = void(STDMETHODCALLTYPE*)(void*, const D3D12_INDEX_BUFFER_VIEW*);
using SetVb_t = void(STDMETHODCALLTYPE*)(void*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
SetIb_t g_orig_set_ib = nullptr;
SetVb_t g_orig_set_vb = nullptr;

void STDMETHODCALLTYPE d_set_ib(void* cl, const D3D12_INDEX_BUFFER_VIEW* v) {
  Ib& t = t_ib;
  t.va = v ? v->BufferLocation : 0;
  t.size = v ? v->SizeInBytes : 0;
  t.isz = v && v->Format == DXGI_FORMAT_R16_UINT ? 2 : 4;
  g_orig_set_ib(cl, v);
}
void STDMETHODCALLTYPE d_set_vb(void* cl, UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW* v) {
  if (start == 0 && n && v) {
    t_ib.vb0 = v[0].BufferLocation;
    t_ib.vb0_size = v[0].SizeInBytes;
  }
  g_orig_set_vb(cl, start, n, v);
}

// On the first watched draw: the engine's list of this thread -> its vtable, two slots swapped.
void patch_vtable() {
  int expect = 0;
  if (!g_vt_state.compare_exchange_strong(expect, 1)) return;
  using GetCtx_t = uint8_t* (*)();
  uint8_t* ctx = reinterpret_cast<GetCtx_t>(g_base + kGetCommandListContext)();
  void* cl = ctx ? *reinterpret_cast<void**>(ctx + kCtxCmdList) : nullptr;
  void** vt = cl ? *reinterpret_cast<void***>(cl) : nullptr;
  DWORD old = 0;
  if (!vt || !VirtualProtect(vt + kSlotSetIb, 2 * sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
    g_vt_state.store(3);
    g_sdk->logger->Warn(g_handle, "[blas] census: command-list vtable NOT patched - no index buffers");
    return;
  }
  g_orig_set_ib = reinterpret_cast<SetIb_t>(vt[kSlotSetIb]);
  g_orig_set_vb = reinterpret_cast<SetVb_t>(vt[kSlotSetVb]);
  vt[kSlotSetIb] = reinterpret_cast<void*>(&d_set_ib);
  vt[kSlotSetVb] = reinterpret_cast<void*>(&d_set_vb);
  VirtualProtect(vt + kSlotSetIb, 2 * sizeof(void*), old, &old);
  g_vt = vt;
  g_vt_state.store(2);
  g_sdk->logger->InfoF(g_handle, "[blas] census: command-list vtable %p patched (IASetIndexBuffer / IASetVertexBuffers) "
                       "- joins start with the next bind", static_cast<void*>(vt));
}

void log_eval();

void log_window() {
  auto& log = *g_sdk->logger;
  std::lock_guard<std::mutex> lock(g_mutex);
  size_t n_static = 0, n_dyn = 0;
  for (const auto& [k, g] : g_geo) (g.flags & kAllowUpdate ? n_dyn : n_static)++;
  log.InfoF(g_handle, "[blas] window %u: BLAS geometries known %zu (static %zu, updatable %zu); NvAPI this window: "
            "%llu builds, %llu updates, %llu geometries%s", ++g_log_windows, g_geo.size(), n_static, n_dyn,
            static_cast<unsigned long long>(g_nv_builds), static_cast<unsigned long long>(g_nv_updates),
            static_cast<unsigned long long>(g_nv_geoms), g_nv_unread ? " (some params NOT read)" : "");
  {  // a few keys of each family: the VA ranges the draws are compared with
    char line[512];
    int n = std::snprintf(line, sizeof line, "[blas]   sample BLAS ib:");
    uint32_t got[2] = {};
    for (const auto& [k, g] : g_geo) {
      const int f = g.flags & kAllowUpdate ? 1 : 0;
      if (got[f] >= 3 || n >= static_cast<int>(sizeof line) - 64) continue;
      ++got[f];
      n += std::snprintf(line + n, sizeof line - n, " %s 0x%llx x%u vb 0x%llx;", f ? "upd" : "st",
                         static_cast<unsigned long long>(k.first), k.second, static_cast<unsigned long long>(g.vb));
    }
    log.Info(g_handle, line);
  }
  const uint32_t nvs = g_nwatch.load();
  for (uint32_t w = 0; w < nvs; ++w) {
    VsStats& s = g_vs[w];
    if (!s.draws) continue;
    log.InfoF(g_handle, "[blas] VS %.8s %-26s draws %llu, index ranges %zu: STATIC %u, DYNAMIC %u, NONE %u",
              g_watch[w].hash, g_watch[w].name, static_cast<unsigned long long>(s.draws), s.keys.size(), s.per_cls[0],
              s.per_cls[1], s.per_cls[2]);
    for (uint32_t c = 0; c < 3; ++c)
      for (uint32_t i = 0; i < s.nex[c]; ++i) {
        const Example& e = s.ex[c][i];
        if (c == kNone)
          log.InfoF(g_handle, "[blas]   NONE ib 0x%llx x%u (vb0 0x%llx) - same index buffer: %u geometries, nearest "
                    "ib 0x%llx x%u", static_cast<unsigned long long>(e.ib), e.count,
                    static_cast<unsigned long long>(e.vb0), e.same_buffer, static_cast<unsigned long long>(e.near_ib),
                    e.near_count);
        else {
          // The counts NOW (the window's end), not the copy taken at the window's first draw of this range.
          const auto it = g_geo.find({e.ib, e.count});
          const Geo& g = it != g_geo.end() ? it->second : e.geo;
          log.InfoF(g_handle, "[blas]   %s ib 0x%llx x%u -> BLAS 0x%llx flags 0x%x type %u, vb 0x%llx (%s the draw's "
                    "VB) fmt %u stride %llu, %u verts; this window builds %u updates %u, since the start %llu / %llu",
                    kClsName[c], static_cast<unsigned long long>(e.ib), e.count,
                    static_cast<unsigned long long>(g.dest), g.flags, g.type, static_cast<unsigned long long>(g.vb),
                    e.vb_same ? "IN" : "NOT in", g.vfmt, static_cast<unsigned long long>(g.vstride), g.vcount,
                    g.builds, g.updates, static_cast<unsigned long long>(g.total_builds),
                    static_cast<unsigned long long>(g.total_updates));
        }
      }
    s = VsStats{};
  }
  {  // the deform-pool writers in this window
    char line[2048];
    int n = std::snprintf(line, sizeof line, "[blas]   pool CS this window:");
    const uint32_t ncs = g_nwatch_cs.load();
    for (uint32_t i = 0; i < ncs && n < static_cast<int>(sizeof line) - 96; ++i) {
      const uint64_t d = g_cs[i].dispatches.exchange(0), ind = g_cs[i].indirect.exchange(0),
                     gr = g_cs[i].groups.exchange(0);
      n += std::snprintf(line + n, sizeof line - n, " %s %llu%s", g_watch_cs[i].name, static_cast<unsigned long long>(d),
                         d ? "" : ";");
      if (d)
        n += std::snprintf(line + n, sizeof line - n, " (%llu indirect, %llu groups);",
                           static_cast<unsigned long long>(ind), static_cast<unsigned long long>(gr));
    }
    log.Info(g_handle, line);
  }
  for (auto& [k, g] : g_geo) g.builds = g.updates = 0;
  g_nv_builds = g_nv_updates = g_nv_geoms = g_nv_unread = 0;
  if (g_eval_attached) log_eval();
}

void d_eval(void* self, void* ctx, void* proxy, uint64_t lod, uint64_t chunk, uint64_t pool, uint64_t pool_off,
            void* uctx) {
  const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base;
  uint32_t* prev = t_cs_mask;
  const uint32_t prev58 = t_eval_mat58;
  t_cs_mask = nullptr;
  const auto* p = static_cast<const uint8_t*>(proxy);
  // The chunk's material as EvaluatePositions reads it: proxy[+0x118] + (proxy[+0x122 + 2*lod] + chunk) * 48 + 8.
  const auto* mats = p ? reinterpret_cast<const uint8_t*>(rd64(p + 0x118)) : nullptr;
  const int8_t l = static_cast<int8_t>(lod & 0xff);
  const uint8_t* mat = nullptr;
  if (mats && l >= 0) {
    const uint32_t idx = p[0x122 + 2 * l] + static_cast<uint32_t>(chunk & 0xffff);
    mat = reinterpret_cast<const uint8_t*>(rd64(mats + idx * 48ull + 8));
  }
  const uint32_t mat58 = mat ? rd32(mat + 0x58) : 0xffff0000u;  // no material read
  g_eval_calls.fetch_add(1, std::memory_order_relaxed);
  t_eval_mat58 = mat58;
  if (p) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const bool birth = ret == kAddProxiesEvalRet1 || ret == kAddProxiesEvalRet2, update = ret == kEvaluatePositionsRet;
    ++(birth ? g_eval_birth : update ? g_eval_update : g_eval_other);
    auto it = g_proxies.find(reinterpret_cast<uint64_t>(proxy));
    if (it == g_proxies.end() && g_proxies.size() < kMaxProxies) {
      it = g_proxies.emplace(reinterpret_cast<uint64_t>(proxy), Proxy{}).first;
      it->second.stamp_first = rd32(p + 0x90);
    }
    if (it != g_proxies.end()) {
      Proxy& v = it->second;
      v.mat58 = mat58;
      v.stamp_last = rd32(p + 0x90);
      ++(birth ? v.calls_birth : update ? v.calls_update : v.calls_other);
      t_cs_mask = &v.cs_mask;
    }
  }
  g_orig_eval(self, ctx, proxy, lod, chunk, pool, pool_off, uctx);
  t_cs_mask = prev;
  t_eval_mat58 = prev58;
}

// The deform pool by (material handler, kernels): proxies, their calls by caller, stamps moved.
void log_eval() {  // g_mutex held
  auto& log = *g_sdk->logger;
  log.InfoF(g_handle, "[blas] EvaluatePositions this window: %llu calls - birth %llu, refit %llu, other %llu (other = "
            "an unknown caller, or another plugin's detour wraps ours); proxies known %zu",
            static_cast<unsigned long long>(g_eval_calls.exchange(0)), static_cast<unsigned long long>(g_eval_birth),
            static_cast<unsigned long long>(g_eval_update), static_cast<unsigned long long>(g_eval_other),
            g_proxies.size());
  {
    char line[1024];
    int n = std::snprintf(line, sizeof line, "[blas]   watched CS inside EvaluatePositions by material[+0x58] this window:");
    for (const auto& [k, c] : g_cs_mat58)
      if (n < static_cast<int>(sizeof line) - 80)
        n += std::snprintf(line + n, sizeof line - n, " %s 0x%x x%llu;", g_watch_cs[k.first].name, k.second,
                           static_cast<unsigned long long>(c));
    log.Info(g_handle, line);
    g_cs_mat58.clear();
  }
  struct Grp {
    uint32_t proxies = 0, active = 0, birth = 0, update = 0, other = 0, stamp_moved = 0;
  };
  std::map<std::pair<uint32_t, uint32_t>, Grp> grp;  // (material[+0x58], kernels) -> group
  for (auto& [k, v] : g_proxies) {
    Grp& g = grp[{v.mat58, v.cs_mask}];
    ++g.proxies;
    g.active += (v.calls_birth || v.calls_update || v.calls_other) ? 1 : 0;
    g.birth += v.calls_birth;
    g.update += v.calls_update;
    g.other += v.calls_other;
    g.stamp_moved += v.stamp_last != v.stamp_first;
    v.calls_birth = v.calls_update = v.calls_other = 0;
  }
  uint32_t shown = 0;
  for (const auto& [key, g] : grp) {
    if (++shown > 40) break;
    char cs[1024];
    int n = 0;
    cs[0] = 0;
    for (uint32_t i = 0; i < g_nwatch_cs.load() && n < static_cast<int>(sizeof cs) - 48; ++i)
      if (key.second & (1u << i)) n += std::snprintf(cs + n, sizeof cs - n, " %s;", g_watch_cs[i].name);
    log.InfoF(g_handle, "[blas]   mat[+0x58] 0x%x | pool CS:%s | proxies %u (active this window %u), calls birth %u refit %u "
              "other %u, stamp moved on %u", key.first, n ? cs : " none", g.proxies, g.active, g.birth, g.update, g.other,
              g.stamp_moved);
  }
  g_eval_birth = g_eval_update = g_eval_other = 0;
}

void maybe_log() {
  const ULONGLONG now = GetTickCount64();
  ULONGLONG next = g_next_log.load(std::memory_order_relaxed);
  if (now >= next && g_next_log.compare_exchange_strong(next, now + kWindowMs)) log_window();
}

void on_draw(void* user, const rg_draw_info* info) {
  const auto w = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user));
  if (g_vt_state.load(std::memory_order_relaxed) != 2) {
    patch_vtable();
    return;
  }
  const Ib& t = t_ib;
  if (info->kind != RG_DRAW_INDEXED || !info->count || !t.va || w >= g_nwatch.load()) return;
  const uint64_t ib = t.va + uint64_t(info->start) * t.isz;
  const uint64_t key_hash = ib * 0x9E3779B97F4A7C15ull ^ info->count;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    VsStats& s = g_vs[w];
    ++s.draws;
    if (s.keys.find(key_hash) == s.keys.end() && s.keys.size() < 65536) {
      Example e{};
      e.ib = ib;
      e.count = info->count;
      e.vb0 = t.vb0;
      const auto it = g_geo.find({ib, info->count});
      if (it != g_geo.end()) {
        e.geo = it->second;
        e.cls = e.geo.flags & kAllowUpdate ? kDynamic : kStatic;
        e.vb_same = e.geo.vb >= t.vb0 && e.geo.vb < t.vb0 + t.vb0_size;
      } else {
        e.cls = kNone;
        uint64_t best = ~0ull;
        for (auto g = g_geo.lower_bound({t.va, 0}); g != g_geo.end() && g->first.first < t.va + t.size; ++g) {
          ++e.same_buffer;
          const uint64_t d = g->first.first > ib ? g->first.first - ib : ib - g->first.first;
          if (d < best) {
            best = d;
            e.near_ib = g->first.first;
            e.near_count = g->first.second;
          }
        }
      }
      s.keys.emplace(key_hash, static_cast<uint8_t>(e.cls));
      ++s.per_cls[e.cls];
      if (s.nex[e.cls] < kExamples) s.ex[e.cls][s.nex[e.cls]++] = e;
    }
  }
  maybe_log();
}

void on_dispatch(void* user, const rg_dispatch_info* info) {
  const auto i = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user));
  if (i >= g_nwatch_cs.load()) return;
  g_cs[i].dispatches.fetch_add(1, std::memory_order_relaxed);
  if (uint32_t* m = t_cs_mask) {  // inside an EvaluatePositions call (same thread)
    std::lock_guard<std::mutex> lock(g_mutex);
    *m |= 1u << i;
  }
  if (t_eval_mat58 != ~0u) {  // inside an EvaluatePositions call: which material handler picked this kernel
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cs_mat58.size() < 128) ++g_cs_mat58[{i, t_eval_mat58}];
  }
  if (info->flags & RG_DISPATCH_INDIRECT) g_cs[i].indirect.fetch_add(1, std::memory_order_relaxed);
  g_cs[i].groups.fetch_add(uint64_t(info->gx) * info->gy * info->gz, std::memory_order_relaxed);
  maybe_log();  // the window ends here too: without a watched VS no draw ends it
}

}  // namespace

bool on() { return g_on.load(std::memory_order_relaxed); }

void enable() {
  g_on.store(true);
  g_next_log.store(GetTickCount64() + kWindowMs);
  g_sdk->logger->InfoF(g_handle, "[blas] census ON - clients name the shaders (census_watch), a report every %llu s",
                       static_cast<unsigned long long>(kWindowMs / 1000));
}

int32_t watch(uint32_t stage, const uint8_t* hash16, const char* name) {
  if (!registration_open()) return RG_E_LATE;
  if (!hash16 || (stage != RG_STAGE_VS && stage != RG_STAGE_CS)) return RG_E_ARG;
  if (!on()) return RG_E_UNAVAILABLE;  // [Diagnostics] BlasCensus is off: nothing subscribed, nothing costs
  static std::mutex m;
  std::lock_guard<std::mutex> lock(m);
  const bool vs = stage == RG_STAGE_VS;
  std::atomic<uint32_t>& count = vs ? g_nwatch : g_nwatch_cs;
  const uint32_t i = count.load();
  if (i >= kMaxWatch) return RG_E_FULL;
  Watched& w = (vs ? g_watch : g_watch_cs)[i];
  std::snprintf(w.name, sizeof w.name, "%s", name && *name ? name : "?");
  for (int k = 0; k < 16; ++k) std::snprintf(w.hash + 2 * k, 3, "%02x", hash16[k]);
  void* user = reinterpret_cast<void*>(static_cast<uintptr_t>(i));
  const rg_handle h = vs ? passes::draw_subscribe("blas census", RG_STAGE_VS, hash16, &on_draw, user)
                         : passes::subscribe("blas census CS", hash16, &on_dispatch, user);
  if (h <= 0) return static_cast<int32_t>(h);
  count.store(i + 1);
  g_sdk->logger->InfoF(g_handle, "[blas] census watches %s %.8s %s", vs ? "VS" : "CS", w.hash, w.name);
  return 0;
}

void attach() {
  if (!on()) return;
  const bool nv = rtlib::attach_nv_census();
  g_sdk->logger->InfoF(g_handle, "[blas] census: NvAPI BLAS builds %s; index buffers from the command-list vtable at "
                       "the first watched draw", nv ? "HOOKED" : "NOT hooked");
  g_eval_attached = attach_hook(kEvaluatePositions, kEvaluatePositionsRet, reinterpret_cast<void*>(&d_eval),
                                reinterpret_cast<void**>(&g_orig_eval), "CRenderMesh::EvaluatePositions (BLAS census)");
  g_sdk->logger->InfoF(g_handle, "[blas] census: EvaluatePositions %s (per proxy: caller, material handler, stamp, pool CS)",
                       g_eval_attached ? "HOOKED" : "NOT hooked");
}

void detach() {
  if (g_eval_attached) detach_hook(kEvaluatePositions);
  g_eval_attached = false;
  if (g_vt_state.load() != 2 || !g_vt) return;
  DWORD old = 0;
  if (VirtualProtect(g_vt + kSlotSetIb, 2 * sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
    g_vt[kSlotSetIb] = reinterpret_cast<void*>(g_orig_set_ib);
    g_vt[kSlotSetVb] = reinterpret_cast<void*>(g_orig_set_vb);
    VirtualProtect(g_vt + kSlotSetIb, 2 * sizeof(void*), old, &old);
  }
  g_vt_state.store(0);
}

// From rt_lib's NvAPI BuildEx detour, before the build: desc = NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_
// EX_PARAMS.pDesc (the engine's layout, see d_nv).
void on_nv(const uint8_t* d) {
  if (!d) {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_nv_unread;
    return;
  }
  const uint64_t dest = rd64(d);
  const uint32_t type = rd32(d + 8), flags = rd32(d + 0xc), count = rd32(d + 0x10), layout = rd32(d + 0x14);
  const uint64_t stride = rd64(d + 0x18), geoms = rd64(d + 0x20);
  if (type != 1 /* BLAS */ || !geoms) return;
  const bool update = (flags & kPerformUpdate) != 0;
  std::lock_guard<std::mutex> lock(g_mutex);
  (update ? g_nv_updates : g_nv_builds)++;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* g = layout == 0 ? reinterpret_cast<const uint8_t*>(geoms + i * stride)
                                   : reinterpret_cast<const uint8_t*>(rd64(reinterpret_cast<const uint8_t*>(geoms) + 8ull * i));
    if (!g) continue;
    const uint32_t gt = rd32(g);
    if (gt != 0 && gt != 2) continue;  // triangles, triangles + OMM
    ++g_nv_geoms;
    const uint64_t ib = rd64(g + 0x20);
    const uint32_t icount = rd32(g + 0x18);
    if (!ib || !icount) continue;
    auto it = g_geo.find({ib, icount});
    if (it == g_geo.end()) {
      if (g_geo.size() >= kMaxGeos) continue;
      it = g_geo.emplace(Key{ib, icount}, Geo{}).first;
    }
    Geo& o = it->second;
    if (o.dest != dest && !update) o.total_builds = o.total_updates = 0;  // the range now belongs to another BLAS
    o.dest = dest;
    o.flags = flags & ~kPerformUpdate;
    o.type = gt;
    o.ifmt = rd32(g + 0x10);
    o.vfmt = rd32(g + 0x14);
    o.vcount = rd32(g + 0x1c);
    o.vb = rd64(g + 0x28);
    o.vstride = rd64(g + 0x30);
    if (update) {
      ++o.updates;
      ++o.total_updates;
    } else {
      ++o.builds;
      ++o.total_builds;
    }
  }
}

}  // namespace core::blas_census
