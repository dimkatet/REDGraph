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

#include <Windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstring>

#include "core/engine_231.hpp"
#include "core/internal.hpp"
#include "core/prof.hpp"

namespace core::passes {
namespace {

using namespace e231;

using DispatchCompute_t = void (*)(uint32_t x, uint32_t y, uint32_t z, uint32_t cu);
using DispatchIndirect_t = void (*)();
using GetCtx_t = uint8_t* (*)();
using RestoreState_t = void (*)(ID3D12GraphicsCommandList* cl);
DispatchCompute_t g_orig_dispatch = nullptr;
DispatchIndirect_t g_orig_indirect = nullptr;
GetCtx_t g_get_ctx = nullptr;
RestoreState_t g_restore = nullptr;
bool g_attached = false, g_indirect_attached = false;

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
constexpr uint32_t kMaxSubs = 8;
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
  cl->Dispatch(p.gx, p.gy, p.gz);
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

bool needed(bool companions) { return companions || g_npasses.load() != 0 || has_subs(); }

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
  return g_attached;
}

void detach() {
  if (g_indirect_attached) detach_hook(kDispatchIndirectCompute);
  if (g_attached) detach_hook(kDispatchCompute);
  g_attached = g_indirect_attached = false;
}

}  // namespace core::passes
