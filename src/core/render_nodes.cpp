// M6: a NODE of ours in the engine's render graph (docs/internals.md "Our own node"). Not a hook on someone else's
// dispatch: the engine builds the graph with our node in it and runs it like its own.
//
// How: rend::NodeGraphFactory::Register(factory, group, SRenderNodeParameters{type, subtype, node}) is where every
// Create<T> hands its node to the graph builder; with a group open (factory+0xf0) the node is appended to that group's
// members (SharedPtr<CRenderNodeBase>, run in order by the group's Execute). After the engine registers the ANCHOR (a
// node whose GetName equals the client's `after`, e.g. "CRenderNode_VolumetricFog"), we register ours the same way:
// right after it, in its group, with its type / subtype. The object is the engine's own shape - 0x18 bytes of
// CRenderNodeBase (+ our index at +0x18) from the render-graph pool, freed by the engine when the graph is rebuilt -
// with OUR vtable: [0] dtor (nothing to destroy), [1] Execute, [2] GetName, and [3..5] (CreateCommandList,
// GetJobBuilderUsage, GetCommandListUsage) copied from CRenderNode_VolumetricFog's - a plain member that records into
// its group's command list. Only inside a group (v1): a top-level node would need graph edges.
//
// M7, its RenderFlow declarations: what the client declared at registration (node_use / node_alloc) the host issues
// itself in BOTH runs of the graph, in the same order - allocs (RTAlloc), then a RTUseBegin per resource, [fn, real
// run only], then a RTUseEnd per resource. Never conditional: the real run's requests are matched to the
// declaration's by their index on the flow thread, so one request missing in either run would hand every later
// member of the group another resource's barriers (docs/internals.md "Requests").
//
// M8, a WRAP of an engine node: CRenderNodeBase::Process calls Execute through vtable slot [1] for group members and
// top-level nodes alike, with the node's command list already bound (a top-level node / group creates and binds its own
// there). So the first time the engine registers a node named like a wrap's target, its class vtable's slot [1] is
// swapped (VirtualProtect, one aligned pointer store - a thread mid-call keeps the original) for wrap_execute: the
// client's before, the original, the client's after - real run only. Every command-list group shares ONE vtable: a
// vtable whose original is the group Execute is `shared` and wraps are matched by GetName per call.
//
// M9, a REMOVAL rides the same vtable slot: in the declaration run the original runs (it only declares) while the host
// records its RenderFlow requests (render_flow's HandleRequest detour, this thread); in the real run the host replays
// those requests raw instead of the original. The engine's per-thread request indices, every alloc / lifetime /
// aliasing barrier / discard stay exactly as declared - only the node's GPU work is gone. The switch is latched per
// node instance at its declaration run, so both runs of a frame agree. Groups (members run on other threads / in jobs)
// and job-builder nodes are refused: their requests are not all issued on this thread.
//
// The GRAPH DEBUG module (graph_debug.cpp, [Diagnostics] RenderGraphDebug) rides it too: with debug_enable() every
// engine class's slot [1] is wrapped at its first Register (groups keep theirs - a group is removed as its members),
// and any single node INSTANCE can be removed live - a debug slot keyed by the node object, the same record / replay.

#include <Windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <RED4ext/CName.hpp>
#include <RED4ext/CNamePool.hpp>

#include "core/clients.hpp"
#include "core/engine_231.hpp"
#include "core/internal.hpp"
#include "core/prof.hpp"

namespace core::render_nodes {
namespace {

using namespace e231;

using Register_t = uint32_t (*)(void* factory, uint32_t group, const void* params);
using Alloc_t = void* (*)(uint32_t size);
using GetCtx_t = uint8_t* (*)();
using RestoreState_t = void (*)(ID3D12GraphicsCommandList* cl);
using GetName_t = RED4ext::CName* (*)(const void* node, RED4ext::CName* out);
using Execute_t = void (*)(const void* node, const void* ctx, void* builder);
using JobUsage_t = bool (*)(const void* node);
using BindCmdList_t = void (*)(uint32_t id);

Register_t g_orig = nullptr;
Alloc_t g_alloc = nullptr;
GetCtx_t g_get_ctx = nullptr;
BindCmdList_t g_bind_cl = nullptr;
RestoreState_t g_restore = nullptr;
bool g_attached = false;

constexpr uint32_t kMaxNodes = 8;
constexpr uint32_t kMaxUses = 8;
struct Use {
  uint32_t hash;    // the RenderFlow name hash (the tag mixes the view in per request)
  uint32_t access;  // RG_FLOW_*
  bool alloc;       // ours: an RTAlloc with desc first
  uint8_t desc[kFlowDescSize];
};
struct Node {
  const char* name;
  uint64_t after;  // the anchor's CName hash (FNV-1a 64 of its class name, = what its GetName returns)
  rg_node_fn fn;
  void* user;
  RED4ext::CName cname;
  Use uses[kMaxUses];
  uint32_t nuses;  // fixed once registration closes
  uint32_t nreqs;  // requests per run: a begin + an end per use, an alloc per alloc
  std::atomic<uint32_t> inserted{0}, skipped_toplevel{0};
  std::atomic<uint64_t> executes{0}, declares{0}, no_cmdlist{0}, restores{0}, requests{0};
};
Node g_n[kMaxNodes];
std::atomic<uint32_t> g_nn{0};
std::once_flag g_names_once;

constexpr uint32_t kMaxWraps = 8;
struct Wrap {
  const char* name;
  uint64_t target;  // CName hash of the class / group name
  rg_node_fn before, after;
  void* user;
  std::atomic<int32_t> vt{-1};  // index into g_vt once wrapped
  std::atomic<uint64_t> executes{0}, declares{0}, no_cmdlist{0}, restores{0}, jobs{0}, rebinds{0};
  int32_t rm = -1;  // a removal: index into g_rm
};
Wrap g_w[kMaxWraps];

// A removal's per-instance record of the declaration run (a class may have several nodes: HistogramUpdate has 3).
constexpr uint32_t kMaxRemovals = 4, kMaxInstances = 8;
struct Instance {
  std::atomic<const void*> node{nullptr};
  std::atomic<uint64_t> stamp{0};  // the declaration that last used it - the oldest is recycled after a rebuild
  bool active = false;             // latched at the declaration run
  render_flow::FlowRecord rec{};
};
struct Removal {
  std::atomic<bool> on{false};
  Instance inst[kMaxInstances];
  std::atomic<uint64_t> removed{0}, ran{0}, replayed{0}, overflows{0}, refused{0};
};
Removal g_rm[kMaxRemovals];
std::atomic<uint32_t> g_nrm{0};
std::atomic<uint64_t> g_declare_seq{0};
std::mutex g_inst_mu;

Instance* instance_of(Removal& r, const void* node) {
  for (Instance& i : r.inst)
    if (i.node.load(std::memory_order_acquire) == node) return &i;
  return nullptr;
}

Instance* claim_instance(Removal& r, const void* node) {
  std::lock_guard lock(g_inst_mu);
  if (Instance* i = instance_of(r, node)) return i;
  Instance* best = &r.inst[0];
  for (Instance& i : r.inst) {
    if (!i.node.load()) {
      best = &i;
      break;
    }
    if (i.stamp.load() < best->stamp.load()) best = &i;
  }
  best->node.store(node, std::memory_order_release);
  return best;
}
std::atomic<uint32_t> g_nw{0};

struct PatchedVt {  // a class vtable whose slot [1] is wrap_execute
  void* const* vt;
  Execute_t orig;
  bool shared;
};
// 167 concrete classes + the group vtable; found per Execute through an insert-only hash (index + 1, 0 = empty).
constexpr uint32_t kMaxVt = 256, kVtSlots = 512;
PatchedVt g_vt[kMaxVt];
std::atomic<uint32_t> g_nvt{0};
std::atomic<uint16_t> g_vt_hash[kVtSlots];
std::mutex g_patch_mu;

uint32_t vt_slot(const void* vt) {
  return static_cast<uint32_t>(((reinterpret_cast<uintptr_t>(vt) >> 3) * 0x9E3779B97F4A7C15ull) >> 55) & (kVtSlots - 1);
}

// The graph debug module's removals: one slot per removed node INSTANCE (graph_debug.cpp).
std::atomic<bool> g_debug{false};
std::atomic<uint32_t> g_debug_classes{0};
constexpr uint32_t kMaxDebug = 32;
struct DebugSlot {
  std::atomic<const void*> node{nullptr};
  std::atomic<bool> want{false};
  bool active = false;  // latched at the node's declaration run
  render_flow::FlowRecord rec{};
  std::atomic<uint64_t> removed{0}, ran{0}, replayed{0}, overflows{0}, refused{0};
};
DebugSlot g_dbg[kMaxDebug];
std::atomic<uint32_t> g_dbg_used{0};  // slots with a node - 0 = the per-Execute scan is skipped
std::mutex g_dbg_mu;                  // claims (UI thread) vs clear (render thread)

DebugSlot* debug_slot_of(const void* node) {
  if (!g_dbg_used.load(std::memory_order_acquire)) return nullptr;
  for (DebugSlot& d : g_dbg)
    if (d.node.load(std::memory_order_acquire) == node) return &d;
  return nullptr;
}

// [-1] the template's RTTI locator (kept in place, nothing should need it), [0..5] the vtable.
void* g_vtable_storage[1 + kNodeVtSlots];
void** g_vtable = g_vtable_storage + 1;

Node* node_of(const uint8_t* self) {
  uint32_t i;
  std::memcpy(&i, self + kOurNodeIndex, 4);
  return i < g_nn.load(std::memory_order_acquire) ? &g_n[i] : nullptr;
}

void* node_dtor(void* self, uint32_t) { return self; }  // the engine frees the memory (pool) itself

RED4ext::CName* node_get_name(const uint8_t* self, RED4ext::CName* out) {
  std::call_once(g_names_once, [] {
    for (uint32_t i = 0; i < g_nn.load(); ++i) RED4ext::CNamePool::Add(g_n[i].name);
  });
  const Node* n = node_of(self);
  *out = n ? n->cname : RED4ext::CName();
  return out;
}

void declare_begin(Node& n, const uint8_t* ctx) {
  for (uint32_t i = 0; i < n.nuses; ++i)
    if (n.uses[i].alloc) render_flow::request(ctx, kFlowReqAlloc, n.uses[i].hash, n.uses[i].desc, kFlowDescSize);
  for (uint32_t i = 0; i < n.nuses; ++i)
    render_flow::request(ctx, kFlowReqUseBegin, n.uses[i].hash, &n.uses[i].access, 4);
}

void declare_end(Node& n, const uint8_t* ctx) {
  for (uint32_t i = 0; i < n.nuses; ++i) render_flow::request(ctx, kFlowReqUseEnd, n.uses[i].hash, nullptr, 0);
  if (n.nreqs) n.requests.fetch_add(n.nreqs, std::memory_order_relaxed);
}

// A client fn on the command list bound on this thread; restores the engine's state when fn says it touched it.
void run_fn(rg_node_fn fn, void* user, const uint8_t* ctx, std::atomic<uint64_t>& no_cmdlist,
            std::atomic<uint64_t>& restores) {
  uint8_t* lctx = g_get_ctx();
  auto* cl = lctx ? *reinterpret_cast<ID3D12GraphicsCommandList**>(lctx + kCtxCmdList) : nullptr;
  if (!cl) {
    no_cmdlist.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  uint32_t view;
  std::memcpy(&view, ctx + kNodeCtxView, 4);
  const rg_node_info info{0, (ctx[kNodeCtxFlags] & 1) ? 0u : (view & 0xff), cl};
  uint32_t touched;
  {
    prof::Pause client;
    touched = fn(user, &info);
  }
  if (!(touched & RG_PASS_TOUCHED_STATE)) return;
  g_restore(cl);
  *reinterpret_cast<void**>(lctx + kCtxPso) = nullptr;  // the PSO on the list is ours: the next draw/dispatch re-sets
  lctx[kCtxComputeDirty] = 1;                           // both PSO states re-resolved, as passes.cpp does
  lctx[kCtxGraphicsDirty] = 1;
  restores.fetch_add(1, std::memory_order_relaxed);
}

// fn's part of Execute; the declarations around it are issued whatever this returns.
void node_run(Node* n, const uint8_t* ctx) {
  n->executes.fetch_add(1, std::memory_order_relaxed);
  run_fn(n->fn, n->user, ctx, n->no_cmdlist, n->restores);
}

void node_execute(const uint8_t* self, const uint8_t* ctx, void*) {
  Node* n = node_of(self);
  if (!n) return;
  declare_begin(*n, ctx);
  if (render_flow::declaring(ctx))  // the graph's first run of the frame: requests only, nothing is recorded
    n->declares.fetch_add(1, std::memory_order_relaxed);
  else
    node_run(n, ctx);
  declare_end(*n, ctx);
}

uint64_t name_hash_of(const void* engine_node) {
  if (!engine_node) return 0;
  const void* const* vt = *static_cast<const void* const* const*>(engine_node);
  if (!vt) return 0;
  RED4ext::CName cn;
  reinterpret_cast<GetName_t>(vt[kNodeVtGetName])(engine_node, &cn);
  return cn.hash;
}

const PatchedVt* patched_of(const void* vt) {
  for (uint32_t i = vt_slot(vt), n = 0; n < kVtSlots; i = (i + 1) & (kVtSlots - 1), ++n) {
    const uint16_t k = g_vt_hash[i].load(std::memory_order_acquire);
    if (!k) return nullptr;
    if (g_vt[k - 1].vt == vt) return &g_vt[k - 1];
  }
  return nullptr;
}

void wrap_execute(const uint8_t* self, const uint8_t* ctx, void* builder);

// Under g_patch_mu: slot [1] of `vt` becomes wrap_execute (once per vtable). nullptr = table full / VirtualProtect.
const PatchedVt* patch_vtable(void** vt, const char* why) {
  if (const PatchedVt* p = patched_of(vt)) return p;
  const uint32_t n = g_nvt.load();
  if (n >= kMaxVt) return nullptr;
  auto* orig = reinterpret_cast<Execute_t>(vt[kNodeVtExecute]);
  g_vt[n] = PatchedVt{vt, orig, reinterpret_cast<uintptr_t>(orig) == g_base + kGroupExecute};
  g_nvt.store(n + 1, std::memory_order_release);
  // Published before any thread can land in wrap_execute through this vtable.
  for (uint32_t i = vt_slot(vt);; i = (i + 1) & (kVtSlots - 1))
    if (!g_vt_hash[i].load(std::memory_order_relaxed)) {
      g_vt_hash[i].store(static_cast<uint16_t>(n + 1), std::memory_order_release);
      break;
    }
  DWORD old = 0;
  if (!VirtualProtect(&vt[kNodeVtExecute], sizeof(void*), PAGE_READWRITE, &old)) {
    g_sdk->logger->WarnF(g_handle, "render nodes: %s - VirtualProtect failed (%lu), NOT wrapped", why, GetLastError());
    return nullptr;  // stays in the table with its original slot: wrap_execute is never reached through it
  }
  InterlockedExchangePointer(&vt[kNodeVtExecute], reinterpret_cast<void*>(&wrap_execute));
  VirtualProtect(&vt[kNodeVtExecute], sizeof(void*), old, &old);
  return &g_vt[n];
}

bool wrap_matches(const Wrap& w, int32_t vt_index, bool shared, uint64_t name) {
  return w.vt.load(std::memory_order_acquire) == vt_index && (!shared || w.target == name);
}

// The flow thread's list id when no list is bound on this thread now, else 0.
uint32_t flow_list_if_unbound(const uint8_t* ctx) {
  const uint8_t* lctx = g_get_ctx();
  if (lctx && *reinterpret_cast<void* const*>(lctx + kCtxCmdList)) return 0;
  const auto* so = *reinterpret_cast<const uint8_t* const*>(g_base + kRtSoTable);
  const auto* ids = so ? *reinterpret_cast<const uint32_t* const*>(so + kFlowCmdLists) : nullptr;
  return ids ? ids[ctx[kNodeCtxFlowThread] + 1] : 0u;
}

// The engine's Execute, outside the host's profile.
template <class P>
void orig_execute(const P* p, const uint8_t* self, const uint8_t* ctx, void* builder) {
  prof::Pause engine;
  p->orig(self, ctx, builder);
}

// Slot [1] of every wrapped class. The original runs in both runs of the graph; the clients' fns in the real one.
void wrap_execute(const uint8_t* self, const uint8_t* ctx, void* builder) {
  prof::Scope prof_scope(prof::kNodeExec);
  void* const* vt = *reinterpret_cast<void* const* const*>(self);
  const PatchedVt* p = patched_of(vt);
  if (!p) return;  // cannot happen: only a patched vtable points here
  const int32_t vi = static_cast<int32_t>(p - g_vt);
  const uint64_t name = p->shared ? name_hash_of(self) : 0;
  const uint32_t nw = g_nw.load(std::memory_order_acquire);
  const bool declaring = render_flow::declaring(ctx);
  bool any = false;
  Removal* rm = nullptr;
  for (uint32_t i = 0; i < nw; ++i) {
    Wrap& w = g_w[i];
    if (!wrap_matches(w, vi, p->shared, name)) continue;
    any = true;
    if (w.rm >= 0) rm = &g_rm[w.rm];
  }
  if (rm && (p->shared || reinterpret_cast<JobUsage_t>(vt[kNodeVtJobBuilderUsage])(self))) {
    rm->refused.fetch_add(1, std::memory_order_relaxed);
    rm = nullptr;
  }
  DebugSlot* ds = rm ? nullptr : debug_slot_of(self);
  if (ds && (p->shared || !render_flow::requests_attached() ||
             reinterpret_cast<JobUsage_t>(vt[kNodeVtJobBuilderUsage])(self))) {
    ds->refused.fetch_add(1, std::memory_order_relaxed);
    ds = nullptr;
  }
  for (uint32_t i = 0; i < nw; ++i) {
    Wrap& w = g_w[i];
    if (!wrap_matches(w, vi, p->shared, name)) continue;
    if (declaring) {
      w.declares.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    w.executes.fetch_add(1, std::memory_order_relaxed);
    if (reinterpret_cast<JobUsage_t>(vt[kNodeVtJobBuilderUsage])(self)) w.jobs.fetch_add(1, std::memory_order_relaxed);
    if (w.before) run_fn(w.before, w.user, ctx, w.no_cmdlist, w.restores);
  }
  if (rm && declaring) {
    Instance* in = claim_instance(*rm, self);
    in->stamp.store(g_declare_seq.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    in->active = rm->on.load(std::memory_order_relaxed);
    in->rec.n = 0;
    in->rec.overflow = false;
    render_flow::record_into(&in->rec);
    orig_execute(p, self, ctx, builder);
    render_flow::record_into(nullptr);
    if (in->rec.overflow) rm->overflows.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (ds && declaring) {
    ds->active = ds->want.load(std::memory_order_relaxed);
    ds->rec.n = 0;
    ds->rec.overflow = false;
    render_flow::record_into(&ds->rec);
    orig_execute(p, self, ctx, builder);
    render_flow::record_into(nullptr);
    if (ds->rec.overflow) ds->overflows.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (rm) {
    Instance* in = instance_of(*rm, self);
    if (in && in->active && !in->rec.overflow) {
      render_flow::replay(ctx, in->rec);
      rm->removed.fetch_add(1, std::memory_order_relaxed);
      rm->replayed.fetch_add(in->rec.n, std::memory_order_relaxed);
    } else {
      orig_execute(p, self, ctx, builder);
      rm->ran.fetch_add(1, std::memory_order_relaxed);
    }
  } else if (ds) {
    if (ds->active && !ds->rec.overflow) {
      render_flow::replay(ctx, ds->rec);
      ds->removed.fetch_add(1, std::memory_order_relaxed);
      ds->replayed.fetch_add(ds->rec.n, std::memory_order_relaxed);
    } else {
      orig_execute(p, self, ctx, builder);
      ds->ran.fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    orig_execute(p, self, ctx, builder);
  }
  if (!any || declaring) return;
  // A group unbinds its list (BindCommandList(0) first in bit-1 mode, each member after itself): bind the list the
  // engine binds for a member on this flow thread, as CRenderNodeBase::Process does, and unbind it again after.
  const uint32_t rebind = flow_list_if_unbound(ctx);
  if (rebind) g_bind_cl(rebind);
  for (uint32_t i = 0; i < nw; ++i) {
    Wrap& w = g_w[i];
    if (!w.after || !wrap_matches(w, vi, p->shared, name)) continue;
    if (rebind) w.rebinds.fetch_add(1, std::memory_order_relaxed);
    run_fn(w.after, w.user, ctx, w.no_cmdlist, w.restores);
  }
  if (rebind) g_bind_cl(0);
}

// The debug module: wrap the class of `engine_node` - not a group's vtable (a group is removed as its members), not ours.
void debug_patch(const void* engine_node) {
  void** vt = *static_cast<void** const*>(engine_node);
  if (!vt || vt == g_vtable || patched_of(vt)) return;
  if (reinterpret_cast<uintptr_t>(vt[kNodeVtExecute]) == g_base + kGroupExecute) return;
  std::lock_guard lock(g_patch_mu);
  RED4ext::CName cn;
  reinterpret_cast<GetName_t>(vt[kNodeVtGetName])(engine_node, &cn);
  const char* name = cn.ToString();
  if (patch_vtable(vt, name ? name : "?") && g_debug_classes.fetch_add(1) == 0)
    g_sdk->logger->InfoF(g_handle, "render nodes: graph debug - every node class wrapped at its first Register "
                         "(first: '%s')", name ? name : "?");
}

// The engine just registered `engine_node`, named `h`: wrap its class for every wrap targeting that name.
void wrap_class(const void* engine_node, uint64_t h) {
  for (uint32_t i = 0; i < g_nw.load(std::memory_order_acquire); ++i) {
    Wrap& w = g_w[i];
    if (w.target != h || w.vt.load(std::memory_order_acquire) >= 0) continue;
    std::lock_guard lock(g_patch_mu);
    if (w.vt.load() >= 0) continue;
    void** vt = *static_cast<void** const*>(engine_node);
    const PatchedVt* p = patch_vtable(vt, w.name);
    if (!p || vt[kNodeVtExecute] != reinterpret_cast<void*>(&wrap_execute)) continue;
    w.vt.store(static_cast<int32_t>(p - g_vt), std::memory_order_release);
    g_sdk->logger->InfoF(g_handle, "render nodes: wrap '%s' - WRAPPED (vtable rva 0x%llx, Execute rva 0x%llx%s)",
                         w.name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(vt) - g_base),
                         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p->orig) - g_base),
                         p->shared ? ", a group vtable: matched by name" : "");
  }
}

uint32_t d_register(void* factory, uint32_t group, const void* params) {
  const uint32_t nn = g_nn.load(std::memory_order_acquire);
  const auto* p = static_cast<const uint8_t*>(params);
  const void* engine_node = nullptr;
  if (p) std::memcpy(&engine_node, p + kParamsNode, sizeof engine_node);
  const uint64_t h = nn || g_nw.load(std::memory_order_acquire) ? name_hash_of(engine_node) : 0;
  const uint32_t r = g_orig(factory, group, params);
  if (engine_node && g_debug.load(std::memory_order_relaxed)) debug_patch(engine_node);
  if (!h) return r;
  wrap_class(engine_node, h);
  for (uint32_t i = 0; i < nn; ++i) {
    Node& n = g_n[i];
    if (n.after != h) continue;
    const void* open_group = nullptr;
    std::memcpy(&open_group, static_cast<const uint8_t*>(factory) + kFactoryOpenGroup, sizeof open_group);
    if (!open_group) {
      if (n.skipped_toplevel.fetch_add(1) == 0)
        g_sdk->logger->WarnF(g_handle, "render nodes: '%s' - its anchor is a top-level node, not a group member - "
                             "not inserted (v1 inserts into groups only)", n.name);
      continue;
    }
    auto* obj = static_cast<uint8_t*>(g_alloc(kOurNodeSize));  // zeroed
    if (!obj) continue;
    void** vt = g_vtable;
    std::memcpy(obj, &vt, sizeof vt);
    // +8: a per-class mask Create<T> writes (0 / 0x4000000000 VolumetricFog / 0x100000000000 ReflectionProbes - a
    // feature mask, meaning not decoded); 0, as the always-running nodes have. +0x10 bit 0 clear. The pool zeroed both.
    std::memcpy(obj + kOurNodeIndex, &i, 4);
    uint8_t ours[kParamsSize];
    std::memcpy(ours, p, kParamsSize);  // the anchor's type / subtype
    std::memcpy(ours + kParamsNode, &obj, sizeof obj);
    g_orig(factory, group, ours);
    if (n.inserted.fetch_add(1) == 0)
      g_sdk->logger->InfoF(g_handle, "render nodes: '%s' INSERTED after its anchor (group %p) - first graph", n.name,
                           open_group);
  }
  return r;
}

}  // namespace

bool needed() { return g_nn.load() != 0 || g_nw.load() != 0 || g_debug.load(); }

bool attach() {
  if (!needed()) return false;
  g_alloc = reinterpret_cast<Alloc_t>(g_base + kGraphPoolAlloc);
  g_get_ctx = reinterpret_cast<GetCtx_t>(g_base + kGetCommandListContext);
  g_restore = reinterpret_cast<RestoreState_t>(g_base + kRestoreState);
  g_bind_cl = reinterpret_cast<BindCmdList_t>(g_base + kBindCommandList);
  // Our vtable: the template node's, with our dtor / Execute / GetName.
  const auto* tmpl = reinterpret_cast<void* const*>(g_base + kNodeTemplateVtable);
  std::memcpy(g_vtable_storage, tmpl - 1, sizeof g_vtable_storage);
  g_vtable[0] = reinterpret_cast<void*>(&node_dtor);
  g_vtable[kNodeVtExecute] = reinterpret_cast<void*>(&node_execute);
  g_vtable[kNodeVtGetName] = reinterpret_cast<void*>(&node_get_name);
  g_attached = attach_hook(kFactoryRegister, kFactoryRegisterRet, reinterpret_cast<void*>(&d_register),
                           reinterpret_cast<void**>(&g_orig), "rend::NodeGraphFactory::Register");
  g_sdk->logger->InfoF(g_handle, "render nodes: %u node(s) of ours, %u wrap(s)%s, graph builder %s", g_nn.load(),
                       g_nw.load(), g_debug.load() ? ", graph debug (every class wrapped)" : "",
                       g_attached ? "ATTACHED (NodeGraphFactory::Register)" : "NOT attached - none inserted");
  return g_attached;
}

void detach() {
  if (g_attached) detach_hook(kFactoryRegister);
  g_attached = false;
}

rg_handle node_register(const char* name, const char* after, rg_node_fn fn, void* user) {
  if (!registration_open()) return RG_E_LATE;
  if (!name || !after || !fn) return RG_E_ARG;
  const uint32_t n = g_nn.load();
  if (n >= kMaxNodes) return RG_E_FULL;
  // A node's name is its class name in the engine's graph (GetName: scopes, wraps, the debug window match on it).
  for (uint32_t i = 0; i < n; ++i)
    if (std::strcmp(g_n[i].name, name) == 0) {
      clients::reason("a node of that name is already registered (by any client) - names are the graph's class names");
      return RG_E_TAKEN;
    }
  Node& d = g_n[n];
  d.name = name;
  d.after = RED4ext::CName(after).hash;
  d.fn = fn;
  d.user = user;
  d.cname = RED4ext::CName(name);
  g_nn.store(n + 1, std::memory_order_release);
  render_flow::want();  // a node's fn may ask for flow resources; the hook is cheap
  return static_cast<rg_handle>(n + 1);
}

void node_stats(rg_handle h, rg_node_stats* out) {
  if (!out) return;
  *out = {};
  if (h <= 0 || static_cast<uint32_t>(h) > g_nn.load()) return;
  const Node& n = g_n[h - 1];
  out->attached = g_attached ? 1u : 0u;
  out->inserted = n.inserted.load();
  out->skipped_toplevel = n.skipped_toplevel.load();
  out->executes = n.executes.load();
  out->declares = n.declares.load();
  out->no_cmdlist = n.no_cmdlist.load();
  out->restores = n.restores.load();
  out->requests = n.requests.load();
}

int32_t node_use(rg_handle h, rg_handle flow, uint32_t access) {
  if (!registration_open()) return RG_E_LATE;
  if (h <= 0 || static_cast<uint32_t>(h) > g_nn.load()) return RG_E_ARG;
  const uint32_t hash = render_flow::flow_hash(flow);
  if (!hash || !access || (access & ~(RG_FLOW_READ | RG_FLOW_WRITE | RG_FLOW_KEEP))) return RG_E_ARG;
  Node& n = g_n[h - 1];
  if (n.nuses >= kMaxUses) return RG_E_FULL;
  n.uses[n.nuses++] = Use{hash, access, false, {}};
  n.nreqs += 2;
  return 0;
}

rg_handle node_alloc(rg_handle h, const char* name, const rg_flow_desc* d) {
  if (!registration_open()) return RG_E_LATE;
  if (h <= 0 || static_cast<uint32_t>(h) > g_nn.load() || !name || !d) return RG_E_ARG;
  if (!d->width || !d->height || d->width > 0xffff || d->height > 0xffff) return RG_E_ARG;
  Node& n = g_n[h - 1];
  if (n.nuses >= kMaxUses) return RG_E_FULL;
  // The transient is PUBLISHED under its name "<mod>/<name>" (FNV-1a 32; the engine's names are 32-bit constants):
  // another client's node reads it with flow_register(name, 0) + node_use - through the engine's graph, which owns its
  // lifetime, barriers and order. One publisher per name.
  const uint32_t hash = render_flow::client_name_hash(name);
  if (!hash) {
    clients::reason("a transient's name is \"<mod>/<name>\" - published under it");
    return RG_E_ARG;
  }
  const rg_handle flow = render_flow::flow_publish(name, hash);
  if (flow <= 0) return flow;
  Use u{hash, RG_FLOW_WRITE, true, {}};
  // The desc as the engine's ctor leaves it, with t64's absolute-size pattern (the size twice).
  const uint16_t w = static_cast<uint16_t>(d->width), hh = static_cast<uint16_t>(d->height);
  const uint32_t ones = 0x10001u, kind = 0x110000u;
  std::memcpy(u.desc + kFlowDescWidth, &w, 2);
  std::memcpy(u.desc + kFlowDescHeight, &hh, 2);
  std::memcpy(u.desc + kFlowDescWidth2, &w, 2);
  std::memcpy(u.desc + kFlowDescHeight2, &hh, 2);
  std::memcpy(u.desc + kFlowDescOnes, &ones, 4);
  std::memcpy(u.desc + kFlowDescKind, &kind, 4);
  u.desc[kFlowDescFlags] = d->engine_flags;
  u.desc[kFlowDescFormat] = d->engine_format;
  n.uses[n.nuses++] = u;
  n.nreqs += 3;
  return flow;
}

rg_handle node_wrap(const char* target, rg_node_fn before, rg_node_fn after, void* user) {
  if (!registration_open()) return RG_E_LATE;
  if (!target || (!before && !after)) return RG_E_ARG;
  const uint32_t n = g_nw.load();
  if (n >= kMaxWraps) return RG_E_FULL;
  Wrap& w = g_w[n];
  w.name = target;
  w.target = RED4ext::CName(target).hash;
  w.before = before;
  w.after = after;
  w.user = user;
  g_nw.store(n + 1, std::memory_order_release);
  render_flow::want();  // flow_texture inside before / after needs the node context
  return static_cast<rg_handle>(n + 1);
}

void node_wrap_stats(rg_handle h, rg_wrap_stats* out) {
  if (!out) return;
  *out = {};
  if (h <= 0 || static_cast<uint32_t>(h) > g_nw.load()) return;
  const Wrap& w = g_w[h - 1];
  const int32_t vi = w.vt.load();
  out->attached = g_attached ? 1u : 0u;
  out->wrapped = vi >= 0 ? 1u : 0u;
  out->shared = vi >= 0 && g_vt[vi].shared ? 1u : 0u;
  out->executes = w.executes.load();
  out->declares = w.declares.load();
  out->no_cmdlist = w.no_cmdlist.load();
  out->restores = w.restores.load();
  out->jobs = w.jobs.load();
  out->rebinds = w.rebinds.load();
}

const void* original_execute(const void* const* vtable) {
  if (!vtable) return nullptr;
  const PatchedVt* p = patched_of(vtable);
  return p ? reinterpret_cast<const void*>(p->orig) : vtable[kNodeVtExecute];
}

rg_handle node_remove(const char* target, uint32_t on) {
  if (!registration_open()) return RG_E_LATE;
  if (!target) return RG_E_ARG;
  const uint32_t n = g_nw.load(), r = g_nrm.load();
  if (n >= kMaxWraps || r >= kMaxRemovals) return RG_E_FULL;
  // ONE removal per node: removing is replacing, two replacements of one node conflict (and the run picks one switch).
  const uint64_t hash = RED4ext::CName(target).hash;
  for (uint32_t i = 0; i < n; ++i)
    if (g_w[i].rm >= 0 && g_w[i].target == hash) {
      char why[120];
      std::snprintf(why, sizeof why, "'%s' is already removed by another registration (one removal per node)", target);
      g_sdk->logger->ErrorF(g_handle, "host: node_remove REFUSED - %s", why);
      clients::reason(why);
      return RG_E_TAKEN;
    }
  Wrap& w = g_w[n];
  w.name = target;
  w.target = hash;
  w.rm = static_cast<int32_t>(r);
  g_rm[r].on.store(on != 0);
  g_nrm.store(r + 1);
  g_nw.store(n + 1, std::memory_order_release);
  render_flow::want_requests();
  return static_cast<rg_handle>(n + 1);
}

namespace {
Removal* removal_of(rg_handle h) {
  if (h <= 0 || static_cast<uint32_t>(h) > g_nw.load()) return nullptr;
  const int32_t r = g_w[h - 1].rm;
  return r >= 0 ? &g_rm[r] : nullptr;
}
}  // namespace

void node_remove_set(rg_handle h, uint32_t on) {
  if (Removal* r = removal_of(h)) r->on.store(on != 0, std::memory_order_relaxed);
}

void node_remove_stats(rg_handle h, rg_remove_stats* out) {
  if (!out) return;
  *out = {};
  const Removal* r = removal_of(h);
  if (!r) return;
  out->attached = g_attached ? 1u : 0u;
  out->wrapped = g_w[h - 1].vt.load() >= 0 ? 1u : 0u;
  out->on = r->on.load() ? 1u : 0u;
  out->removed = r->removed.load();
  out->ran = r->ran.load();
  out->replayed = r->replayed.load();
  out->overflows = r->overflows.load();
  out->refused = r->refused.load();
}

// ---- the graph debug module ----

void debug_enable() {
  g_debug.store(true);
  render_flow::want_requests();  // the removals' record / replay
}

bool debug_removable(const void* vtable) {
  const PatchedVt* p = vtable ? patched_of(vtable) : nullptr;
  return p && !p->shared &&
         static_cast<void* const*>(vtable)[kNodeVtExecute] == reinterpret_cast<const void*>(&wrap_execute);
}

uint32_t debug_wrapped_classes() { return g_debug_classes.load(); }

rg_handle debug_our_node(const void* node, const void* vtable) {
  if (!node || vtable != static_cast<const void*>(g_vtable)) return 0;
  uint32_t i;  // written by register_hook into the object we allocated
  std::memcpy(&i, static_cast<const uint8_t*>(node) + kOurNodeIndex, 4);
  return i < g_nn.load() ? static_cast<rg_handle>(i + 1) : 0;
}

bool debug_remove(const void* node, bool on) {
  if (!node) return false;
  std::lock_guard lock(g_dbg_mu);
  for (DebugSlot& d : g_dbg)
    if (d.node.load() == node) {
      d.want.store(on, std::memory_order_relaxed);
      return true;
    }
  if (!on) return true;
  // A free slot, else one switched off whose last declaration latched it off (its real run runs the original anyway).
  DebugSlot* pick = nullptr;
  for (DebugSlot& d : g_dbg)
    if (!d.node.load()) {
      pick = &d;
      break;
    }
  if (!pick)
    for (DebugSlot& d : g_dbg)
      if (!d.want.load() && !d.active) {
        pick = &d;
        break;
      }
  if (!pick) return false;
  const bool was_free = !pick->node.load();
  pick->active = false;
  pick->want.store(true, std::memory_order_relaxed);
  pick->removed = 0, pick->ran = 0, pick->replayed = 0, pick->overflows = 0, pick->refused = 0;
  pick->node.store(node, std::memory_order_release);
  if (was_free) g_dbg_used.fetch_add(1, std::memory_order_release);
  return true;
}

void debug_remove_clear() {
  std::lock_guard lock(g_dbg_mu);
  for (DebugSlot& d : g_dbg) {
    d.want.store(false, std::memory_order_relaxed);
    d.active = false;
    d.node.store(nullptr, std::memory_order_release);
  }
  g_dbg_used.store(0, std::memory_order_release);
}

void debug_remove_stats(const void* node, rg_remove_stats* out) {
  if (!out) return;
  *out = {};
  for (const DebugSlot& d : g_dbg) {
    if (!node || d.node.load() != node) continue;
    out->attached = g_attached ? 1u : 0u;
    out->wrapped = 1;
    out->on = d.want.load() ? 1u : 0u;
    out->removed = d.removed.load();
    out->ran = d.ran.load();
    out->replayed = d.replayed.load();
    out->overflows = d.overflows.load();
    out->refused = d.refused.load();
    return;
  }
}

}  // namespace core::render_nodes
