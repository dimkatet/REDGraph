// M4 (docs/internals.md "Work on the engine's command list (M4)"): client work on the ENGINE's command list, after a compute dispatch.
// One post-hook on GpuApi::DispatchCompute (GetCommandListContext -> SetupForDispatch -> ctx+0x30->Dispatch) and one on
// GpuApi::DispatchIndirectCompute (the same with ExecuteIndirect - the fog's 5c7ab952, the clouds' 3dd31a42, the tiled
// lights), attached only if some client registered a companion, a pass or a subscription.
//
// A request is made from INSIDE the dispatch being recorded - from a client's SUBSCRIPTION (M4c: fn runs right after
// the engine's Dispatch of a shader it subscribed to, before anything below), or from an outside event that fires
// inside ctx+0x30->Dispatch on the recording thread - and
// kept in a thread_local; then the core records it on the same list:
//   companion (4a): SetPipelineState(companion) -> Dispatch -> SetPipelineState(the engine's PSO). Same root signature,
//     every root argument stays bound, ctx+0x3d0 is true again - no restore.
//   pass (4b): fn(user, list); when it returns RG_PASS_TOUCHED_STATE, GpuApi::Hacks::RestoreState(list) (clears
//     ctx+0x3d8/0x3e0, re-ensures both root signatures = all params dirty, re-binds the heaps) + the PSO cleared and
//     both PSO states dirty.
// A request whose context (or, for a companion, bound PSO) changed before the engine's Dispatch returned is dropped; a
// companion with leases (bindings) is accepted only when its descriptors are in the table the engine's dispatch uses.
//
// The GRAPHICS wing (2026-10-10, engine_231.hpp "The graphics wing"): post-hooks on the eight GpuApi draw entries,
// attached only if a client registered a draw subscription. A draw is ours when the bind funnel's last graphics bind
// on this thread carried a subscribed shader and that PSO is still the one on the list (pso::draw_bound) - one
// thread-local load for every other draw. From inside the subscriptions: companion draws (the same draw again with
// another PSO, then the engine's PSO back) and one pass after the draw.

#include <Windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstring>

#include "core/clients.hpp"
#include "core/engine_231.hpp"
#include "core/gpu_timing.hpp"
#include "core/internal.hpp"
#include "core/prof.hpp"

namespace core::passes {
namespace {

using namespace e231;

using DispatchCompute_t = void (*)(uint32_t x, uint32_t y, uint32_t z, uint32_t cu);
using DispatchIndirect_t = void (*)();
using GetD3DBuffer_t = ID3D12Resource* (*)(const uint32_t* buffer_ref);
using GetCtx_t = uint8_t* (*)();
using RestoreState_t = void (*)(ID3D12GraphicsCommandList* cl);
DispatchCompute_t g_orig_dispatch = nullptr;
DispatchIndirect_t g_orig_indirect = nullptr;
GetCtx_t g_get_ctx = nullptr;
RestoreState_t g_restore = nullptr;
bool g_attached = false, g_indirect_attached = false;
GetD3DBuffer_t g_get_d3d_buffer = nullptr;

constexpr uint32_t kMaxPasses = 8;
const char* g_pass_names[kMaxPasses];
std::atomic<uint32_t> g_npasses{0};

struct Pending {
  uint8_t* ctx;
  ID3D12PipelineState* engine;
  ID3D12PipelineState* aux;
  uint32_t gx, gy, gz;
  rg_handle comp;
  bool set;
};
thread_local Pending t_pending{};
std::atomic<uint64_t> g_tail_accepted{0}, g_tail_done{0}, g_tail_stale{0};

struct After {
  rg_pass_fn fn;
  void* user;
  uint8_t* ctx;
  rg_handle pass;
  bool set;
};
thread_local After t_after{};
std::atomic<uint64_t> g_after_accepted{0}, g_after_restored{0}, g_after_stale{0};

// M4c: subscriptions. The engine PSOs carrying a subscribed shader are mapped at creation (pso), and the bind funnel
// leaves the bound PSO's subscription mask in a thread_local for the dispatch it serves (pso::bound_subs_*).
constexpr uint32_t kMaxSubs = 32;  // the mask is a u32
struct Sub {
  const char* name;
  uint8_t hash[16];
  rg_dispatch_fn fn;
  void* user;
  std::atomic<uint64_t> calls{0}, indirect{0};
};
Sub g_subs[kMaxSubs];
std::atomic<uint32_t> g_nsubs{0};

void record_companion(uint8_t* ctx, const Pending& p) {
  auto* cl = *reinterpret_cast<ID3D12GraphicsCommandList**>(ctx + kCtxCmdList);
  cl->SetPipelineState(p.aux);
  {
    gpu_timing::Scope gpu(cl, gpu_timing::kCompanion, p.comp);
    cl->Dispatch(p.gx, p.gy, p.gz);
  }
  cl->SetPipelineState(p.engine);
  pso::companion_count(p.comp);
  if (g_tail_done.fetch_add(1) == 0)
    g_sdk->logger->InfoF(g_handle, "pso swap: first companion dispatch %ux%ux%u after the engine's dispatch (PSO %p -> "
                         "companion %p -> back)", p.gx, p.gy, p.gz, static_cast<void*>(p.engine),
                         static_cast<void*>(p.aux));
}

void run_after(uint8_t* ctx) {
  const After a = t_after;
  t_after.set = false;
  if (ctx != a.ctx) {
    if (g_after_stale.fetch_add(1) == 0)
      g_sdk->logger->Warn(g_handle, "pso swap: after-dispatch request STALE (context changed) - skipped");
    return;
  }
  auto* cl = *reinterpret_cast<ID3D12GraphicsCommandList**>(ctx + kCtxCmdList);
  uint32_t touched;
  {
    prof::Pause client;
    clients::Running running(reinterpret_cast<const void*>(a.fn));
    gpu_timing::Scope gpu(cl, gpu_timing::kPass, a.pass);
    touched = a.fn(a.user, cl);
  }
  if (!(touched & RG_PASS_TOUCHED_STATE)) return;
  g_restore(cl);
  *reinterpret_cast<void**>(ctx + kCtxPso) = nullptr;  // the PSO on the list is ours: the next draw/dispatch re-sets
  ctx[kCtxComputeDirty] = 1;
  ctx[kCtxGraphicsDirty] = 1;
  if (g_after_restored.fetch_add(1) == 0)
    g_sdk->logger->InfoF(g_handle, "pso swap: first after-dispatch pass ('%s') recorded on the engine's list + "
                         "Hacks::RestoreState (PSO cleared, both PSO states dirty)", g_pass_names[a.pass - 1]);
}

void run_subs(uint32_t mask, const rg_dispatch_info& info) {
  const bool indirect = info.flags & RG_DISPATCH_INDIRECT;
  const uint32_t n = g_nsubs.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n; ++i) {
    if (!(mask & (1u << i))) continue;
    Sub& s = g_subs[i];
    if (s.calls.fetch_add(1, std::memory_order_relaxed) == 0)
      g_sdk->logger->InfoF(g_handle, "host: first subscribed dispatch '%s' %ux%ux%u%s", s.name, info.gx, info.gy,
                           info.gz, indirect ? " (indirect)" : "");
    if (indirect && s.indirect.fetch_add(1, std::memory_order_relaxed) == 0 && s.calls.load() > 1)
      g_sdk->logger->InfoF(g_handle, "host: first INDIRECT subscribed dispatch '%s'", s.name);
    prof::Pause client;
    clients::Running running(reinterpret_cast<const void*>(s.fn));
    s.fn(s.user, &info);
  }
}

void before_engine_dispatch() {
  t_pending.set = false;  // a request from a dispatch that did not come through here must not leak into this one
  t_after.set = false;
  pso::bound_subs_reset();
}

void after_engine_dispatch(const rg_dispatch_info& info) {
  if (const uint32_t subs = pso::bound_subs_take()) run_subs(subs, info);
  if (!t_pending.set && !t_after.set) return;
  uint8_t* ctx = g_get_ctx();
  if (t_pending.set) {
    t_pending.set = false;
    const Pending p = t_pending;
    if (ctx != p.ctx || *reinterpret_cast<ID3D12PipelineState**>(ctx + kCtxPso) != p.engine) {
      if (g_tail_stale.fetch_add(1) == 0)
        g_sdk->logger->Warn(g_handle, "pso swap: companion request STALE (context or PSO changed) - skipped");
    } else {
      record_companion(ctx, p);
    }
  }
  if (t_after.set) run_after(ctx);  // after the companion: a pass may read what it wrote
}

void d_dispatch(uint32_t x, uint32_t y, uint32_t z, uint32_t cu) {
  prof::Scope prof_scope(prof::kDispatch);
  before_engine_dispatch();
  {
    prof::Pause engine;
    g_orig_dispatch(x, y, z, cu);
  }
  after_engine_dispatch({x, y, z, 0});
}

void d_dispatch_indirect() {
  prof::Scope prof_scope(prof::kDispatch);
  before_engine_dispatch();
  {
    prof::Pause engine;
    g_orig_indirect();
  }
  after_engine_dispatch({0, 0, 0, RG_DISPATCH_INDIRECT});
}

// ---- the graphics wing ----

constexpr uint32_t kMaxDrawSubs = 32;  // the mask is a u32
struct DrawSub {
  const char* name;
  uint32_t stage;
  uint8_t hash[16];
  rg_draw_fn fn;
  void* user;
  std::atomic<uint64_t> calls{0};
};
DrawSub g_dsubs[kMaxDrawSubs];
std::atomic<uint32_t> g_ndsubs{0};

// The draw being reported on this thread, while its subscriptions run (requests are accepted only then).
constexpr uint32_t kMaxDrawRequests = 4;
struct DrawNow {
  uint8_t* ctx;
  ID3D12PipelineState* engine;
  ID3D12PipelineState* bound;
  rg_draw_info info;
  bool active;
  uint32_t ncomp;
  rg_handle comp[kMaxDrawRequests];
  ID3D12PipelineState* comp_pso[kMaxDrawRequests];
  After after;
};
thread_local DrawNow t_draw{};
std::atomic<uint64_t> g_draws_seen{0}, g_dcomp_done{0}, g_dafter_done{0}, g_draw_reentry{0};

// One draw call on the list, as the engine's entry made it (indirect: its command signature, buffer and offset are still
// the context's - BindIndirectArgs is not undone after the draw).
void issue_draw(uint8_t* ctx, ID3D12GraphicsCommandList* cl, const rg_draw_info& d) {
  switch (d.kind) {
    case RG_DRAW_INSTANCED:
      cl->DrawInstanced(d.count, d.instances, d.start, d.start_instance);
      return;
    case RG_DRAW_INDEXED:
      cl->DrawIndexedInstanced(d.count, d.instances, d.start, d.base_vertex, d.start_instance);
      return;
    default: {
      const auto* dev = *reinterpret_cast<const uint8_t* const*>(g_base + kDeviceData);
      auto* sig = *reinterpret_cast<ID3D12CommandSignature* const*>(
          dev + (d.kind == RG_DRAW_INDEXED_INDIRECT ? kCmdSigDrawIndexed : kCmdSigDraw));
      ID3D12Resource* args = g_get_d3d_buffer(reinterpret_cast<const uint32_t*>(ctx + kCtxIndirectArgs));
      uint32_t offset;
      std::memcpy(&offset, ctx + kCtxIndirectOffset, 4);
      if (sig && args) cl->ExecuteIndirect(sig, 1, args, offset, nullptr, 0);
    }
  }
}

// After an engine draw that carries a subscribed shader: its subscriptions, then the companions they asked for, then
// the pass. (The subscription runs only for the draw that recorded: a NULL PSO skipped it - draw_bound says 0.)
void on_draw(const rg_draw_info& info) {
  uint8_t* ctx = g_get_ctx();
  ID3D12PipelineState *engine = nullptr, *bound = nullptr;
  const uint32_t mask = pso::draw_bound(ctx, &engine, &bound);
  if (!mask) return;
  DrawNow& d = t_draw;
  if (d.active) {  // a client drew through GpuApi from inside a subscription: not reported (no recursion)
    g_draw_reentry.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  d.ctx = ctx;
  d.engine = engine;
  d.bound = bound;
  d.info = info;
  d.ncomp = 0;
  d.after.set = false;
  d.active = true;
  g_draws_seen.fetch_add(1, std::memory_order_relaxed);
  const uint32_t n = g_ndsubs.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n; ++i) {
    if (!(mask & (1u << i))) continue;
    DrawSub& s = g_dsubs[i];
    if (s.calls.fetch_add(1, std::memory_order_relaxed) == 0)
      g_sdk->logger->InfoF(g_handle, "host: first subscribed draw '%s' - kind %u%s, count %u x %u", s.name, info.kind,
                           info.flags & RG_DRAW_SYSTEM ? " (system)" : "", info.count, info.instances);
    prof::Pause client;
    clients::Running running(reinterpret_cast<const void*>(s.fn));
    s.fn(s.user, &info);
  }
  d.active = false;
  if (!d.ncomp && !d.after.set) return;
  auto* cl = *reinterpret_cast<ID3D12GraphicsCommandList**>(ctx + kCtxCmdList);
  // The list must be where the engine left it: its PSO still the one it drew with.
  if (*reinterpret_cast<ID3D12PipelineState**>(ctx + kCtxPso) != bound || g_get_ctx() != ctx) {
    if (g_tail_stale.fetch_add(1) == 0)
      g_sdk->logger->Warn(g_handle, "pso swap: draw requests STALE (a subscription changed the context or PSO) - skipped");
    return;
  }
  if (d.ncomp) {
    for (uint32_t i = 0; i < d.ncomp; ++i) {
      cl->SetPipelineState(d.comp_pso[i]);
      gpu_timing::Scope gpu(cl, gpu_timing::kDrawCompanion, d.comp[i]);
      issue_draw(ctx, cl, info);
      pso::draw_companion_count(d.comp[i]);
    }
    cl->SetPipelineState(bound);
    if (g_dcomp_done.fetch_add(d.ncomp) == 0)
      g_sdk->logger->InfoF(g_handle, "pso swap: first companion draw (kind %u, %u x %u) after the engine's draw (PSO %p "
                           "-> companion %p -> back)", info.kind, info.count, info.instances, static_cast<void*>(bound),
                           static_cast<void*>(d.comp_pso[0]));
  }
  if (d.after.set) {
    d.after.set = false;
    uint32_t touched;
    {
      prof::Pause client;
      clients::Running running(reinterpret_cast<const void*>(d.after.fn));
      gpu_timing::Scope gpu(cl, gpu_timing::kPass, d.after.pass);
      touched = d.after.fn(d.after.user, cl);
    }
    if (touched & RG_PASS_TOUCHED_STATE) {
      g_restore(cl);
      *reinterpret_cast<void**>(ctx + kCtxPso) = nullptr;  // the next draw re-resolves and re-sets its PSO
      ctx[kCtxComputeDirty] = 1;
      ctx[kCtxGraphicsDirty] = 1;
      const uint32_t none = 0;  // D3D_PRIMITIVE_TOPOLOGY_UNDEFINED: the next draw re-sets its topology
      std::memcpy(ctx + kCtxTopology, &none, 4);
    }
    if (g_dafter_done.fetch_add(1) == 0)
      g_sdk->logger->InfoF(g_handle, "pso swap: first pass after a draw ('%s')%s", g_pass_names[d.after.pass - 1],
                           touched & RG_PASS_TOUCHED_STATE ? " + Hacks::RestoreState, PSO / topology re-set" : "");
  }
}

// The eight entries. Each forwards its arguments untouched; a system-primitive entry's are passed as 64-bit words
// (its VertexLayoutRef by value, red::Function refs). DrawSystemPrimitive(type, count, layout, fill, instances,
// start_instance, cu) -> DrawInstanced(count, instances, 0, start_instance); DrawSystemIndexedPrimitive_Impl(?, type,
// vertices, index_count, layout, fill, fill, instances, start_instance) -> DrawIndexedInstanced(index_count, instances,
// 0, 0, start_instance) - both return WITHOUT drawing on a zero count (the PSO on the list is then the previous
// draw's: not reported).
using DrawIndexed_t = void (*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
using DrawPrimitive_t = void (*)(uint32_t, uint32_t, uint32_t, uint32_t);
using DrawNoBuffers_t = void (*)(uint32_t, uint32_t, uint32_t);
using DrawIndirect_t = void (*)(uint32_t, uint32_t);
using DrawSystem7_t = void (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
using DrawSystem9_t = void (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                               uint64_t);
DrawIndexed_t g_orig_draw_indexed = nullptr;
DrawPrimitive_t g_orig_draw_primitive = nullptr;
DrawNoBuffers_t g_orig_draw_nobuffers = nullptr;
DrawIndirect_t g_orig_draw_indexed_indirect = nullptr, g_orig_draw_indirect_nb = nullptr,
               g_orig_draw_indexed_indirect_nb = nullptr;
DrawSystem7_t g_orig_draw_system = nullptr;
DrawSystem9_t g_orig_draw_system_indexed = nullptr;

void d_draw_indexed(uint32_t type, uint32_t base_vertex, uint32_t start, uint32_t count, uint32_t instances,
                    uint32_t start_instance) {
  prof::Scope prof_scope(prof::kDraw);
  {
    prof::Pause engine;
    g_orig_draw_indexed(type, base_vertex, start, count, instances, start_instance);
  }
  if (pso::draw_subs_pending())
    on_draw({RG_DRAW_INDEXED, 0, count, instances, start, static_cast<int32_t>(base_vertex), start_instance});
}
void d_draw_primitive(uint32_t type, uint32_t a2, uint32_t count, uint32_t a4) {
  prof::Scope prof_scope(prof::kDraw);
  {
    prof::Pause engine;
    g_orig_draw_primitive(type, a2, count, a4);
  }
  if (pso::draw_subs_pending()) on_draw({RG_DRAW_INSTANCED, 0, count, 1, 0, 0, 0});
}
void d_draw_nobuffers(uint32_t type, uint32_t count, uint32_t cu) {
  prof::Scope prof_scope(prof::kDraw);
  {
    prof::Pause engine;
    g_orig_draw_nobuffers(type, count, cu);
  }
  if (pso::draw_subs_pending()) on_draw({RG_DRAW_INSTANCED, 0, count, 1, 0, 0, 0});
}
template <DrawIndirect_t* Orig, uint32_t Kind>
void d_draw_indirect(uint32_t type, uint32_t a2) {
  prof::Scope prof_scope(prof::kDraw);
  {
    prof::Pause engine;
    (*Orig)(type, a2);
  }
  if (pso::draw_subs_pending()) on_draw({Kind, 0, 0, 0, 0, 0, 0});
}
void d_draw_system(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7) {
  prof::Scope prof_scope(prof::kDraw);
  {
    prof::Pause engine;
    g_orig_draw_system(a1, a2, a3, a4, a5, a6, a7);
  }
  const auto count = static_cast<uint32_t>(a2);
  if (count && pso::draw_subs_pending())
    on_draw({RG_DRAW_INSTANCED, RG_DRAW_SYSTEM, count, static_cast<uint32_t>(a5), 0, 0, static_cast<uint32_t>(a6)});
}
void d_draw_system_indexed(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                           uint64_t a8, uint64_t a9) {
  prof::Scope prof_scope(prof::kDraw);
  {
    prof::Pause engine;
    g_orig_draw_system_indexed(a1, a2, a3, a4, a5, a6, a7, a8, a9);
  }
  const auto count = static_cast<uint32_t>(a4);
  if (count && static_cast<uint32_t>(a3) && pso::draw_subs_pending())
    on_draw({RG_DRAW_INDEXED, RG_DRAW_SYSTEM, count, static_cast<uint32_t>(a8), 0, 0, static_cast<uint32_t>(a9)});
}

struct DrawHook {
  uintptr_t rva, ret;
  void* detour;
  void** orig;
  const char* name;
  bool attached;
};
DrawHook g_draw_hooks[] = {
    {kDrawIndexedPrimitive, kDrawIndexedPrimitiveRet, reinterpret_cast<void*>(&d_draw_indexed),
     reinterpret_cast<void**>(&g_orig_draw_indexed), "GpuApi::DrawIndexedPrimitive", false},
    {kDrawPrimitive, kDrawPrimitiveRet, reinterpret_cast<void*>(&d_draw_primitive),
     reinterpret_cast<void**>(&g_orig_draw_primitive), "GpuApi::DrawPrimitive", false},
    {kDrawPrimitiveNoBuffers, kDrawPrimitiveNoBuffersRet, reinterpret_cast<void*>(&d_draw_nobuffers),
     reinterpret_cast<void**>(&g_orig_draw_nobuffers), "GpuApi::DrawPrimitiveNoBuffers", false},
    {kDrawIndexedIndirect, kDrawIndexedIndirectRet,
     reinterpret_cast<void*>(&d_draw_indirect<&g_orig_draw_indexed_indirect, RG_DRAW_INDEXED_INDIRECT>),
     reinterpret_cast<void**>(&g_orig_draw_indexed_indirect), "GpuApi::DrawIndexedPrimitiveIndirect", false},
    {kDrawIndirectNoBuffers, kDrawIndirectNoBuffersRet,
     reinterpret_cast<void*>(&d_draw_indirect<&g_orig_draw_indirect_nb, RG_DRAW_INDIRECT>),
     reinterpret_cast<void**>(&g_orig_draw_indirect_nb), "GpuApi::DrawPrimitiveIndirectNoBuffers", false},
    {kDrawIndexedIndirectNoBuffers, kDrawIndexedIndirectNoBuffersRet,
     reinterpret_cast<void*>(&d_draw_indirect<&g_orig_draw_indexed_indirect_nb, RG_DRAW_INDEXED_INDIRECT>),
     reinterpret_cast<void**>(&g_orig_draw_indexed_indirect_nb), "GpuApi::DrawIndexedPrimitiveIndirectNoBuffers",
     false},
    {kDrawSystemPrimitive, kDrawSystemPrimitiveRet, reinterpret_cast<void*>(&d_draw_system),
     reinterpret_cast<void**>(&g_orig_draw_system), "GpuApi::DrawSystemPrimitive", false},
    {kDrawSystemIndexed, kDrawSystemIndexedRet, reinterpret_cast<void*>(&d_draw_system_indexed),
     reinterpret_cast<void**>(&g_orig_draw_system_indexed), "GpuApi::DrawSystemIndexedPrimitive_Impl", false},
};

}  // namespace

rg_handle pass_register(const char* name) {
  if (!registration_open()) return RG_E_LATE;
  const uint32_t n = g_npasses.load();
  if (n >= kMaxPasses) return RG_E_FULL;
  g_pass_names[n] = name ? name : "?";
  g_npasses.store(n + 1, std::memory_order_release);
  return static_cast<rg_handle>(n + 1);
}

uint32_t companion_request(rg_handle h, uint32_t gx, uint32_t gy, uint32_t gz) {
  if (!g_attached || gz == 0 || gx * gy == 0) return 0;
  uint8_t* ctx = g_get_ctx();
  if (!ctx) return 0;
  auto* engine = *reinterpret_cast<ID3D12PipelineState**>(ctx + kCtxPso);
  ID3D12PipelineState* aux = pso::companion_of(engine, h);
  if (!aux) return 0;  // the bound PSO is not one we built this companion for
  if (!pso::companion_in_scope(h)) return 0;  // M11: scoped to a node, and this dispatch is outside it
  // Its leased registers must be in the table this dispatch uses: a slot still holding the ENGINE's descriptor would
  // have the companion read - or write - an engine resource (bindings: NOT STAGED, counted per lease).
  if (!bindings::companion_staged(h, ctx)) return 0;
  t_pending = {ctx, engine, aux, gx, gy, gz, h, true};
  if (g_tail_accepted.fetch_add(1) == 0)
    g_sdk->logger->InfoF(g_handle, "pso swap: first companion request: %ux%u, %u layer(s)", gx, gy, gz);
  return 1;
}

uint32_t after_dispatch(rg_handle h, rg_pass_fn fn, void* user) {
  if (!g_attached || !fn || h <= 0 || static_cast<uint32_t>(h) > g_npasses.load(std::memory_order_acquire)) return 0;
  uint8_t* ctx = g_get_ctx();
  if (!ctx) return 0;
  t_after = {fn, user, ctx, h, true};
  if (g_after_accepted.fetch_add(1) == 0)
    g_sdk->logger->InfoF(g_handle, "pso swap: first after-dispatch request ('%s')", g_pass_names[h - 1]);
  return 1;
}

rg_handle subscribe(const char* name, const uint8_t* hash16, rg_dispatch_fn fn, void* user) {
  if (!registration_open()) return RG_E_LATE;
  if (!hash16 || !fn) return RG_E_ARG;
  const uint32_t n = g_nsubs.load();
  if (n >= kMaxSubs) return RG_E_FULL;
  Sub& s = g_subs[n];
  s.name = name ? name : "?";
  std::memcpy(s.hash, hash16, 16);
  s.fn = fn;
  s.user = user;
  g_nsubs.store(n + 1, std::memory_order_release);
  return static_cast<rg_handle>(n + 1);
}

uint32_t subs_for(const uint8_t* hash16) {
  uint32_t mask = 0;
  const uint32_t n = g_nsubs.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n; ++i)
    if (std::memcmp(g_subs[i].hash, hash16, 16) == 0) mask |= 1u << i;
  return mask;
}

bool has_subs() { return g_nsubs.load() != 0; }

rg_handle draw_subscribe(const char* name, uint32_t stage, const uint8_t* hash16, rg_draw_fn fn, void* user) {
  if (!registration_open()) return RG_E_LATE;
  if (!hash16 || !fn || (stage != RG_STAGE_VS && stage != RG_STAGE_PS)) return RG_E_ARG;
  const uint32_t n = g_ndsubs.load();
  if (n >= kMaxDrawSubs) return RG_E_FULL;
  DrawSub& s = g_dsubs[n];
  s.name = name ? name : "?";
  s.stage = stage;
  std::memcpy(s.hash, hash16, 16);
  s.fn = fn;
  s.user = user;
  g_ndsubs.store(n + 1, std::memory_order_release);
  return static_cast<rg_handle>(n + 1);
}

uint32_t draw_subs_for(const uint8_t* vs16, const uint8_t* ps16) {
  uint32_t mask = 0;
  const uint32_t n = g_ndsubs.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* h = g_dsubs[i].stage == RG_STAGE_VS ? vs16 : ps16;
    if (h && std::memcmp(g_dsubs[i].hash, h, 16) == 0) mask |= 1u << i;
  }
  return mask;
}

bool has_draw_subs() { return g_ndsubs.load() != 0; }

uint32_t draw_companion_request(rg_handle h) {
  DrawNow& d = t_draw;
  if (!d.active || d.ncomp >= kMaxDrawRequests) return 0;
  for (uint32_t i = 0; i < d.ncomp; ++i)
    if (d.comp[i] == h) return 1;  // once per draw
  ID3D12PipelineState* pso = pso::draw_companion_of(d.engine, h);
  if (!pso) return 0;
  d.comp[d.ncomp] = h;
  d.comp_pso[d.ncomp++] = pso;
  return 1;
}

uint32_t after_draw(rg_handle h, rg_pass_fn fn, void* user) {
  DrawNow& d = t_draw;
  if (!d.active || !fn || h <= 0 || static_cast<uint32_t>(h) > g_npasses.load(std::memory_order_acquire)) return 0;
  d.after = {fn, user, d.ctx, h, true};
  return 1;
}

bool needed(bool companions) { return companions || g_npasses.load() != 0 || has_subs() || has_draw_subs(); }

bool attach() {
  g_get_ctx = reinterpret_cast<GetCtx_t>(g_base + kGetCommandListContext);
  g_restore = reinterpret_cast<RestoreState_t>(g_base + kRestoreState);
  g_attached = attach_hook(kDispatchCompute, kRenderShaderComputeRet, reinterpret_cast<void*>(&d_dispatch),
                           reinterpret_cast<void**>(&g_orig_dispatch), "GpuApi::DispatchCompute");
  if (g_attached)
    g_indirect_attached = attach_hook(kDispatchIndirectCompute, kRenderShaderComputeIndirectRet,
                                      reinterpret_cast<void*>(&d_dispatch_indirect),
                                      reinterpret_cast<void**>(&g_orig_indirect), "GpuApi::DispatchIndirectCompute");
  g_sdk->logger->InfoF(g_handle, "pso swap: companions / after-dispatch passes / subscriptions %s%s",
                       g_attached ? "ATTACHED (DispatchCompute post-hook)" : "NOT attached",
                       !g_attached ? "" : g_indirect_attached ? " + DispatchIndirectCompute post-hook"
                                                              : " - indirect dispatches NOT seen");
  if (has_draw_subs()) {
    g_get_d3d_buffer = reinterpret_cast<GetD3DBuffer_t>(g_base + kGetD3DBuffer);
    uint32_t ok = 0;
    for (DrawHook& k : g_draw_hooks) ok += (k.attached = attach_hook(k.rva, k.ret, k.detour, k.orig, k.name)) ? 1u : 0u;
    const uint32_t all = sizeof(g_draw_hooks) / sizeof(g_draw_hooks[0]);
    if (ok == all)
      g_sdk->logger->InfoF(g_handle, "pso swap: draw subscriptions / companion draws / passes after a draw ATTACHED "
                           "(%u GpuApi draw entries)", ok);
    else
      g_sdk->logger->WarnF(g_handle, "pso swap: draw hooks - only %u of %u GpuApi draw entries attached: draws through "
                           "the others are NOT reported", ok, all);
  }
  return g_attached;
}

void detach() {
  for (DrawHook& k : g_draw_hooks)
    if (k.attached) detach_hook(k.rva), k.attached = false;
  if (g_indirect_attached) detach_hook(kDispatchIndirectCompute);
  if (g_attached) detach_hook(kDispatchCompute);
  g_attached = g_indirect_attached = false;
}

}  // namespace core::passes
