// M1 (docs/internals.md "PSO patches (M1)"): PSO clone + live swap inside the engine. Four detours, attached per
// pipeline kind only if some client registered a patch / companion of that kind:
//   PSOSubCache<compute|graphics>::Create - every PSO the engine makes, with the FULL desc: hash the stage a patch
//     targets, run the broker's lease test BEFORE the engine creates the PSO (so a refusal is already visible to anyone
//     asking shader_claimed from inside that creation), then build the clones from the same desc.
//   ComputePSO::CreatePSO / GraphicsPSO::CreatePSO - the bind funnel: engine PSO -> table -> clone[selected variants].
//     The engine binds what we return (its `!= ctx+0x3d0` compare); a switch lands at the next state change.
// Patches COMPOSE on one engine PSO (docs/internals.md "PSO patches (M1)"): a graphics PSO may carry a VS patch, a PS
// patch and a GS one (added) of different clients at once; a clone is built for every combination of their variants
// (each stage also vanilla), and the bind picks the one where each patch is as it is now - selected, in scope, its
// leases staged - independently of the others.

#include <Windows.h>
#include <d3d12.h>
#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

#include <RED4ext/CName.hpp>

#include "core/engine_231.hpp"
#include "core/clients.hpp"
#include "core/internal.hpp"
#include "core/prof.hpp"
#include "dxil_hash.hpp"

#pragma comment(lib, "d3d12.lib")

namespace core::pso {
namespace {

using namespace e231;

static_assert(sizeof(D3D12_COMPUTE_PIPELINE_STATE_DESC) == 0x38, "ComputePSO desc ends at state+0x50 (key word)");
static_assert(sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC) == 0x290, "GraphicsPSO desc ends at state+0x2a8 (key words)");

enum Pipe : uint32_t { kCompute = 0, kGraphics = 1 };

struct Counters {
  std::atomic<uint64_t> engine_hits{0}, swaps{0};
  std::atomic<uint32_t> engine_psos{0}, clones{0}, clone_failed{0}, last_hr{0};
  void fill(rg_patch_stats* o) const {
    o->engine_psos = engine_psos.load();
    o->clones = clones.load();
    o->clone_failed = clone_failed.load();
    o->last_hr = last_hr.load();
    o->engine_hits = engine_hits.load();
    o->swaps = swaps.load();
  }
};

// M11: work only inside the nodes of these CName hashes (none = everywhere); uses elsewhere counted.
constexpr uint32_t kMaxScopeNodes = 4;
struct Scope {
  uint64_t hash[kMaxScopeNodes] = {};
  const char* name[kMaxScopeNodes] = {};
  uint32_t n = 0;
  std::atomic<uint64_t> out_of_scope{0};
  std::atomic<bool> logged_in{false}, logged_out{false};
  bool any() const { return n != 0; }
  // Register-time: one more node. RG_E_ARG = no name / already in, RG_E_FULL = kMaxScopeNodes.
  int32_t add(const char* node) {
    if (!node || !*node) return RG_E_ARG;
    const uint64_t h = RED4ext::CName(node).hash;
    for (uint32_t i = 0; i < n; ++i)
      if (hash[i] == h) return RG_E_ARG;
    if (n >= kMaxScopeNodes) return RG_E_FULL;
    hash[n] = h;
    name[n++] = node;
    return 0;
  }
};

constexpr uint32_t kMaxPatches = 96;
struct Patch {
  const char* name;
  uint32_t stage;
  dxil_hash::Hash hash;
  rg_blob v[RG_MAX_VARIANTS];
  uint32_t n;
  int32_t priority;
  std::atomic<int32_t> variant{-1};
  std::atomic<uint32_t> state{RG_STATE_PENDING};
  Counters c;
  Scope scope;  // M11: swapped only inside its node
};
Patch g_p[kMaxPatches];
std::atomic<uint32_t> g_np{0};

constexpr uint32_t kMaxComps = 8;
struct Comp {
  const char* name;
  dxil_hash::Hash hash;
  rg_blob cs;
  std::atomic<uint32_t> state{RG_STATE_PENDING};  // REFUSED = a lease failed its vanilla test: never built, never run
  Counters c;
  Scope scope;  // M11: dispatched only inside its node
};
Comp g_c[kMaxComps];
std::atomic<uint32_t> g_nc{0};

// engine PSO -> clones, open addressing: a writer (Create, rare, under g_insert_lock) stores the value, then publishes
// the key with release; the bind detours read lock-free. A tree VS pairs with many PS / states.
// The key is the engine PSO's ADDRESS, and the engine frees PSOs: GpuApi::Destroy(ShaderRef) ->
// PSOSubCache::ShaderDestroyed (0x14088e30c graphics / 0x14088fc1c compute) releases every cached PSO built with a
// destroyed shader (material streaming). So the host holds a reference on every engine PSO it maps - its address
// cannot go to another PSO while mapped - and sweep() (every kSweepFrames, from the frame hook) drops the entries
// whose PSO only the host still holds: the key becomes kTomb, the clones are released one sweep LATER (a command list
// recorded before may still be on the GPU). Until 2026-10-10 the table was insert-only: a new PSO at a freed address
// found the OLD entry and bound clones built from another desc (seen as "not mapped (duplicate ...)" for VS 1b259993).
constexpr uint32_t kSlots = 4096;
constexpr uint32_t kMaxEntries = kSlots / 2;
constexpr uintptr_t kTomb = 1;  // a removed entry: probing goes on past it, insert may reuse it
constexpr uint32_t kSweepFrames = 64;
// kSubOnly: an engine PSO mapped only for its dispatch subscriptions (M4c) - no clone.
enum SlotKind : uint32_t { kPatch = 0, kComp = 1, kSubOnly = 2 };
// What an engine PSO maps to. kPatch: up to kMaxStages patches (one per stage: VS, PS, an added GS), and a clone per
// COMBINATION of their states - combo = sum over i of (variant_i + 1) * stride[i], 0 = vanilla, so clone[0] is the
// engine's own (nullptr). kComp: clone[0] = the companion PSO, idx[0] its index.
constexpr uint32_t kMaxStages = 3;
struct Mapping {
  ID3D12PipelineState** clone = nullptr;  // nclone entries; nullptr = that build failed (the engine PSO stays bound)
  uint32_t nclone = 0;
  uint32_t kind = kPatch;
  uint32_t np = 0;
  uint32_t idx[kMaxStages] = {};
  uint32_t stride[kMaxStages] = {};
  uint32_t subs = 0;  // passes' subscription mask for the shader (compute)
};
struct Slot {
  std::atomic<uintptr_t> engine{0};
  Mapping m;
};
Slot g_slots[kSlots];
std::atomic<uint32_t> g_entries{0};
SRWLOCK g_insert_lock = SRWLOCK_INIT;

uint32_t slot_of(uintptr_t p) { return static_cast<uint32_t>(((p >> 4) * 0x9E3779B97F4A7C15ull) >> 52) & (kSlots - 1); }

const Slot* find(uintptr_t engine) {
  for (uint32_t i = slot_of(engine), n = 0; n < kSlots; i = (i + 1) & (kSlots - 1), ++n) {
    const uintptr_t k = g_slots[i].engine.load(std::memory_order_acquire);
    if (k == engine) return &g_slots[i];
    if (k == 0) return nullptr;  // kTomb: go on
  }
  return nullptr;
}

bool insert(uintptr_t engine, const Mapping& m) {
  AcquireSRWLockExclusive(&g_insert_lock);
  bool ok = false;
  if (!find(engine) && g_entries.load(std::memory_order_relaxed) < kMaxEntries) {
    // The first tombstone on the probe path, else the empty slot that ends it.
    uint32_t at = kSlots;
    for (uint32_t i = slot_of(engine), n = 0; n < kSlots; i = (i + 1) & (kSlots - 1), ++n) {
      const uintptr_t k = g_slots[i].engine.load(std::memory_order_relaxed);
      if (k == kTomb && at == kSlots) at = i;
      if (k == 0) {
        if (at == kSlots) at = i;
        break;
      }
    }
    if (at < kSlots) {
      Slot& s = g_slots[at];
      s.m = m;
      reinterpret_cast<ID3D12PipelineState*>(engine)->AddRef();  // the address stays this PSO's while mapped
      s.engine.store(engine, std::memory_order_release);
      g_entries.fetch_add(1, std::memory_order_relaxed);
      ok = true;
    }
  }
  ReleaseSRWLockExclusive(&g_insert_lock);
  return ok;
}

// Clones (and their arrays) of entries removed by the previous sweep - released by the next one.
std::vector<ID3D12PipelineState*> g_retired;
std::vector<ID3D12PipelineState**> g_retired_arrays;
std::atomic<uint64_t> g_removed{0};
uint32_t g_sweep_frame = 0;
bool g_said_removed = false;

// Under g_insert_lock. Every live entry whose engine PSO only we still reference: the engine released it.
void sweep_locked() {
  for (ID3D12PipelineState* c : g_retired) c->Release();
  g_retired.clear();
  for (ID3D12PipelineState** a : g_retired_arrays) delete[] a;
  g_retired_arrays.clear();
  uint32_t removed = 0;
  for (Slot& s : g_slots) {
    const uintptr_t k = s.engine.load(std::memory_order_relaxed);
    if (k <= kTomb) continue;
    auto* p = reinterpret_cast<ID3D12PipelineState*>(k);
    p->AddRef();
    if (p->Release() != 1) continue;  // the engine (or anyone else) still holds it
    s.engine.store(kTomb, std::memory_order_release);
    for (uint32_t i = 0; i < s.m.nclone; ++i)
      if (s.m.clone[i]) g_retired.push_back(s.m.clone[i]);
    if (s.m.clone) g_retired_arrays.push_back(s.m.clone);  // a reader that found the slot may still index it
    s.m.clone = nullptr;
    s.m.nclone = 0;
    p->Release();  // ours, the last: the engine's PSO goes now
    g_entries.fetch_sub(1, std::memory_order_relaxed);
    ++removed;
  }
  if (!removed) return;
  g_removed.fetch_add(removed, std::memory_order_relaxed);
  if (!g_said_removed || verbose()) {
    g_said_removed = true;
    g_sdk->logger->InfoF(g_handle, "pso swap: %u engine PSO(s) released by the engine (a shader destroyed) - their "
                         "entries dropped, %zu clone(s) released next sweep (%u mapped; logged once)", removed,
                         g_retired.size(), g_entries.load());
  }
}

using CreatePSO_t = ID3D12PipelineState* (*)(void* state);
using SubCacheCreate_t = void** (*)(void* self, void** out, void* a3, uint64_t a4, uint64_t key, const void* desc);
CreatePSO_t g_orig_cs_bind = nullptr, g_orig_gfx_bind = nullptr;
SubCacheCreate_t g_orig_cs_create = nullptr, g_orig_gfx_create = nullptr;
bool g_attached[2] = {false, false};
bool g_want[2] = {false, false};  // which pipe kinds some registration targets (set at attach)
bool g_want_stage[4] = {false, false, false, false};
std::atomic<bool> g_composed_logged{false};
std::atomic<bool> g_first_create{false};
thread_local uint32_t t_bound_subs = 0;

std::atomic<uint64_t> g_creates[2] = {}, g_creates_unhashed{0};
std::atomic<uint64_t> g_calls[2] = {}, g_cycles[2] = {};
thread_local uint64_t t_cycles[2] = {};
thread_local uint32_t t_calls[2] = {};

std::optional<dxil_hash::Hash> hash_of(const D3D12_SHADER_BYTECODE& s) {
  if (!s.pShaderBytecode || !s.BytecodeLength) return std::nullopt;
  return dxil_hash::of(s.pShaderBytecode, s.BytecodeLength);
}

int32_t find_patch(uint32_t stage, const dxil_hash::Hash& h) {
  const uint32_t n = g_np.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n; ++i)
    if (g_p[i].stage == stage && g_p[i].hash == h && g_p[i].state.load() != RG_STATE_REFUSED) return static_cast<int32_t>(i);
  return -1;
}

struct Match {
  uint32_t kind = kPatch;
  uint32_t n = 0;               // 0 = not ours; kPatch: the patches (one per stage), kComp: 1
  int32_t idx[kMaxStages] = {};  // their indices
  uint32_t subs = 0;            // dispatch subscriptions on the (compute) shader, whatever n says
};

// The lease test of patch i - before the engine's own creation, so a refusal is final before anyone asks
// shader_claimed. The stage's VANILLA bytecode: a GS patch adds a stage, its leases (none can be GS) see the PS.
bool lease_test(Pipe pipe, const void* desc_v, int32_t i) {
  if (!bindings::patch_has_lease(i + 1)) return true;
  const D3D12_SHADER_BYTECODE* code;
  if (pipe == kCompute) code = &static_cast<const D3D12_COMPUTE_PIPELINE_STATE_DESC*>(desc_v)->CS;
  else if (g_p[i].stage != RG_STAGE_VS) code = &static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(desc_v)->PS;
  else code = &static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(desc_v)->VS;
  return bindings::on_patch_matched(i + 1, code->pShaderBytecode, code->BytecodeLength);
}

// Before the engine creates the PSO: which patch / companion, and the broker's lease test on the vanilla bytecode.
Match identify(Pipe pipe, const void* desc_v) {
  Match m;
  if (pipe == kCompute) {
    const auto& cs = static_cast<const D3D12_COMPUTE_PIPELINE_STATE_DESC*>(desc_v)->CS;
    const auto h = hash_of(cs);
    if (!h) {
      if (cs.pShaderBytecode) g_creates_unhashed.fetch_add(1, std::memory_order_relaxed);
      return m;
    }
    m.subs = passes::subs_for(h->data());
    const int32_t p = find_patch(RG_STAGE_CS, *h);
    if (p >= 0) {
      if (lease_test(pipe, desc_v, p)) m.idx[m.n++] = p;
      return m;
    }
    const uint32_t n = g_nc.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i) {
      if (g_c[i].hash != *h || g_c[i].state.load() == RG_STATE_REFUSED) continue;
      // The lease test on the vanilla bytecode, as for a patch: a refusal leaves the engine PSO unmapped.
      if (!bindings::on_companion_matched(static_cast<rg_handle>(i + 1), cs.pShaderBytecode, cs.BytecodeLength))
        return m;
      m.kind = kComp;
      m.idx[m.n++] = static_cast<int32_t>(i);
      return m;
    }
    return m;
  }
  const auto* g = static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(desc_v);
  int32_t found[kMaxStages] = {-1, -1, -1};  // VS, PS, GS (added: keyed on the PS, only where the engine has none)
  if (g_want_stage[RG_STAGE_VS]) {
    const auto h = hash_of(g->VS);
    if (h) found[0] = find_patch(RG_STAGE_VS, *h);
    else if (g->VS.pShaderBytecode) g_creates_unhashed.fetch_add(1, std::memory_order_relaxed);
  }
  if (g_want_stage[RG_STAGE_PS] || g_want_stage[RG_STAGE_GS]) {
    const auto h = hash_of(g->PS);
    if (h && g_want_stage[RG_STAGE_PS]) found[1] = find_patch(RG_STAGE_PS, *h);
    if (h && g_want_stage[RG_STAGE_GS] && !g->GS.pShaderBytecode) found[2] = find_patch(RG_STAGE_GS, *h);
  }
  for (const int32_t p : found)
    if (p >= 0 && lease_test(pipe, desc_v, p)) m.idx[m.n++] = p;
  return m;
}

const char* stage_label(uint32_t s) { return s == RG_STAGE_VS ? "VS" : s == RG_STAGE_PS ? "PS" : s == RG_STAGE_GS ? "GS" : "CS"; }

// Every combination of the matched patches' variants (combo 0 = all vanilla = the engine PSO itself), or the companion.
void build(Pipe pipe, ID3D12PipelineState* engine, const void* desc_v, const Match& m) {
  Mapping map;
  map.kind = m.kind;
  map.np = m.n;
  map.subs = m.subs;
  uint32_t total = 1;
  for (uint32_t i = 0; i < m.n; ++i) {
    map.idx[i] = static_cast<uint32_t>(m.idx[i]);
    map.stride[i] = total;
    total *= m.kind == kComp ? 1u : g_p[m.idx[i]].n + 1;
  }
  if (m.kind == kComp) total = 1;  // clone[0] = the companion
  map.nclone = total;
  map.clone = new ID3D12PipelineState*[total]();
  ID3D12Device* dev = nullptr;
  const HRESULT hr = engine->GetDevice(IID_PPV_ARGS(&dev));
  uint32_t built = 0, failed = 0;
  uint32_t built_with[kMaxStages] = {};
  for (uint32_t combo = m.kind == kComp ? 0u : 1u; combo < total && SUCCEEDED(hr); ++combo) {
    HRESULT vhr;
    uint32_t e[kMaxStages] = {};  // per patch: 0 vanilla, v + 1 = variant v
    if (pipe == kCompute) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC d = *static_cast<const D3D12_COMPUTE_PIPELINE_STATE_DESC*>(desc_v);
      const rg_blob& b = m.kind == kComp ? g_c[m.idx[0]].cs : g_p[m.idx[0]].v[combo - 1];
      e[0] = m.kind == kComp ? 1u : combo;  // a compute patch: combo = variant + 1
      d.CS = {b.bytes, b.size};
      d.CachedPSO = {};
      vhr = dev->CreateComputePipelineState(&d, IID_PPV_ARGS(&map.clone[combo]));
    } else {
      D3D12_GRAPHICS_PIPELINE_STATE_DESC d = *static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(desc_v);
      for (uint32_t i = 0; i < m.n; ++i) {
        const Patch& p = g_p[m.idx[i]];
        e[i] = (combo / map.stride[i]) % (p.n + 1);
        if (!e[i]) continue;
        const rg_blob& b = p.v[e[i] - 1];
        (p.stage == RG_STAGE_GS ? d.GS : p.stage == RG_STAGE_PS ? d.PS : d.VS) = {b.bytes, b.size};
      }
      d.CachedPSO = {};
      vhr = dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&map.clone[combo]));
    }
    if (SUCCEEDED(vhr)) {
      ++built;
      for (uint32_t i = 0; i < m.n; ++i) built_with[i] += e[i] ? 1u : 0u;
      continue;
    }
    map.clone[combo] = nullptr;
    ++failed;
    char what[160];
    int at = 0;
    for (uint32_t i = 0; i < m.n; ++i) {
      if (!e[i]) continue;
      Counters& c = m.kind == kComp ? g_c[m.idx[i]].c : g_p[m.idx[i]].c;
      c.clone_failed.fetch_add(1);
      c.last_hr.store(static_cast<uint32_t>(vhr));
      at += std::snprintf(what + at, sizeof what - at, "%s%s variant %u", at ? " + " : "",
                          m.kind == kComp ? g_c[m.idx[i]].name : g_p[m.idx[i]].name, e[i] - 1);
      if (at >= static_cast<int>(sizeof what)) break;
    }
    g_sdk->logger->ErrorF(g_handle, "pso swap: clone build FAILED for %s (engine PSO %p), hr 0x%08x - engine PSO stays "
                          "for that combination", what, static_cast<void*>(engine), static_cast<unsigned>(vhr));
  }
  if (dev) dev->Release();
  uint32_t k[kMaxStages] = {};
  for (uint32_t i = 0; i < m.n; ++i) {
    Counters& c = m.kind == kComp ? g_c[m.idx[i]].c : g_p[m.idx[i]].c;
    k[i] = c.engine_psos.fetch_add(1) + 1;
    c.clones.fetch_add(built_with[i]);
  }
  const char* name = m.kind == kComp ? g_c[m.idx[0]].name : g_p[m.idx[0]].name;
  if (!insert(reinterpret_cast<uintptr_t>(engine), map)) {
    for (uint32_t i = 0; i < map.nclone; ++i)
      if (map.clone[i]) map.clone[i]->Release();  // a duplicate Create for a PSO we already map, or the table is full
    delete[] map.clone;
    g_sdk->logger->WarnF(g_handle, "pso swap: %s engine PSO %p not mapped (duplicate or table full, %u entries)", name,
                         static_cast<void*>(engine), g_entries.load());
    return;
  }
  if (m.n > 1 && (!g_composed_logged.exchange(true) || verbose())) {
    char who[240];
    int at = 0;
    for (uint32_t i = 0; i < m.n && at < static_cast<int>(sizeof who); ++i)
      at += std::snprintf(who + at, sizeof who - at, "%s%s '%s'", i ? " + " : "", stage_label(g_p[m.idx[i]].stage),
                          g_p[m.idx[i]].name);
    g_sdk->logger->InfoF(g_handle, "pso swap: engine PSO %p carries %u patches - %s: %u of %u combination(s) built, %u "
                         "failed (the first such PSO; more with VerboseLog)", static_cast<void*>(engine), m.n, who,
                         built, total - 1, failed);
  }
  for (uint32_t i = 0; i < m.n; ++i) {
    // the first clone of each patch says it works; more with VerboseLog
    if (!built_with[i] || !(k[i] == 1 || (k[i] <= 3 && verbose()))) continue;
    const bool comp = m.kind == kComp;
    const uint32_t nv = comp ? 1u : g_p[m.idx[i]].n;
    if (m.n == 1)
      g_sdk->logger->InfoF(g_handle, "pso swap: %s engine PSO %p -> clone %p%s (#%u for it, %u of %u variants)",
                           comp ? g_c[m.idx[i]].name : g_p[m.idx[i]].name, static_cast<void*>(engine),
                           static_cast<void*>(map.clone[comp ? 0 : map.stride[i]]), comp ? " (companion)" : "", k[i],
                           built, nv);
    else  // composed: the clones carrying this patch's variants, in every state of the other patches
      g_sdk->logger->InfoF(g_handle, "pso swap: %s engine PSO %p -> clone %p (#%u for it, composed: %u clone(s) carry "
                           "its %u variant(s))", g_p[m.idx[i]].name, static_cast<void*>(engine),
                           static_cast<void*>(map.clone[map.stride[i]]), k[i], built_with[i], nv);
  }
}

void on_create(Pipe pipe, void** out, const void* desc, const Match& m) {
  if (!out || !*out || !desc) return;
  g_creates[pipe].fetch_add(1, std::memory_order_relaxed);
  if (m.n) {
    build(pipe, static_cast<ID3D12PipelineState*>(*out), desc, m);
  } else if (m.subs) {
    Mapping sub;
    sub.kind = kSubOnly;
    sub.subs = m.subs;
    if (!insert(reinterpret_cast<uintptr_t>(*out), sub))
      g_sdk->logger->WarnF(g_handle, "pso swap: subscribed engine PSO %p not mapped (duplicate or table full)", *out);
  }
}

// The engine's first PSO: its device exists - the clients' device_ready callbacks run here, before the engine
// creates that PSO (the same thread; nothing of ours is pending on it).
void first_create() {
  if (!g_first_create.exchange(true)) fire_device_ready();
}

void** d_cs_create(void* self, void** out, void* a3, uint64_t a4, uint64_t key, const void* desc) {
  first_create();
  const Match m = desc ? identify(kCompute, desc) : Match{};
  void** r = g_orig_cs_create(self, out, a3, a4, key, desc);
  on_create(kCompute, out, desc, m);
  if (graph_probe::census() && out && *out && desc) graph_probe::on_pso_created(kCompute, *out, desc);
  return r;
}
void** d_gfx_create(void* self, void** out, void* a3, uint64_t a4, uint64_t key, const void* desc) {
  first_create();
  const Match m = desc ? identify(kGraphics, desc) : Match{};
  void** r = g_orig_gfx_create(self, out, a3, a4, key, desc);
  on_create(kGraphics, out, desc, m);
  if (graph_probe::census() && out && *out && desc) graph_probe::on_pso_created(kGraphics, *out, desc);
  return r;
}

// M11: is the node recording on this thread `sc`'s node? Counts / logs the first use each way.
bool in_node(Scope& sc, const char* who, const char* what) {
  const void* node = render_flow::current_node();
  const uint64_t h = render_flow::node_name_hash(node);
  for (uint32_t i = 0; i < sc.n; ++i) {
    if (h != sc.hash[i]) continue;
    if (!sc.logged_in.exchange(true))
      g_sdk->logger->InfoF(g_handle, "pso swap: %s - first %s INSIDE its node '%s' (scoped)", who, what, sc.name[i]);
    return true;
  }
  sc.out_of_scope.fetch_add(1, std::memory_order_relaxed);
  if (!sc.logged_out.exchange(true)) {
    RED4ext::CName cn;
    cn.hash = h;
    const char* where = node ? cn.ToString() : nullptr;
    g_sdk->logger->WarnF(g_handle, "pso swap: %s - a %s OUTSIDE its node%s '%s'%s (in '%s') - not taken there; logged "
                         "once, counted in out_of_scope", who, what, sc.n > 1 ? "s" : "", sc.name[0],
                         sc.n > 1 ? " ..." : "", node ? (where && *where ? where : "?") : "no node");
  }
  return false;
}

// nullptr from the engine = its async compile is still pending (the draw/dispatch is skipped) - passed through.
ID3D12PipelineState* swap(Pipe pipe, const void* state, ID3D12PipelineState* engine) {
  const uint64_t t0 = __rdtsc();
  ID3D12PipelineState* out = engine;
  if (engine && graph_probe::census()) graph_probe::on_bind(pipe, engine);
  // The graph debug module's shader skip: NULL is the engine's own "still compiling" answer - it skips the draw.
  if (engine && graph_debug::skip(pipe, engine)) out = nullptr;
  else if (engine && g_entries.load(std::memory_order_relaxed) != 0) {
    if (const Slot* s = find(reinterpret_cast<uintptr_t>(engine))) {
      const Mapping& m = s->m;
      if (pipe == kCompute) t_bound_subs = m.subs;
      if (m.kind == kSubOnly) {
        // subscriptions only - the engine PSO stays bound
      } else if (m.kind == kComp) {
        g_c[m.idx[0]].c.engine_hits.fetch_add(1, std::memory_order_relaxed);  // a companion is never bound in its place
        if (pipe == kCompute) bindings::companion_bind(static_cast<rg_handle>(m.idx[0] + 1), state);  // learns the ref
      } else {
        // Each patch on its own: selected, in its node, its leases staged - the clone of exactly that combination.
        uint32_t combo = 0, active = 0;
        for (uint32_t i = 0; i < m.np; ++i) {
          Patch& p = g_p[m.idx[i]];
          p.c.engine_hits.fetch_add(1, std::memory_order_relaxed);
          const int32_t v = p.variant.load(std::memory_order_relaxed);
          const bool in_scope = !p.scope.any() || in_node(p.scope, p.name, "bind");
          const bool on = v >= 0 && v < static_cast<int32_t>(p.n) &&
                          p.state.load(std::memory_order_relaxed) != RG_STATE_REFUSED && in_scope;
          const rg_handle h = static_cast<rg_handle>(m.idx[i] + 1);
          const bool ok = !bindings::patch_has_lease(h) || bindings::bind_ok(h, state, pipe == kCompute, on);
          if (!on || !ok) continue;
          combo += static_cast<uint32_t>(v + 1) * m.stride[i];
          active |= 1u << i;
        }
        if (combo && combo < m.nclone && m.clone[combo]) {
          out = m.clone[combo];
          for (uint32_t i = 0; i < m.np; ++i)
            if (active & (1u << i)) g_p[m.idx[i]].c.swaps.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
  }
  const uint64_t dt = __rdtsc() - t0;
  t_cycles[pipe] += dt;
  if (prof::g_on.load(std::memory_order_relaxed)) prof::add(prof::kBind, dt);
  if ((++t_calls[pipe] & 255) == 0) {
    g_calls[pipe].fetch_add(256, std::memory_order_relaxed);
    g_cycles[pipe].fetch_add(t_cycles[pipe], std::memory_order_relaxed);
    t_cycles[pipe] = 0;
  }
  return out;
}
ID3D12PipelineState* d_cs_bind(void* state) { return swap(kCompute, state, g_orig_cs_bind(state)); }
ID3D12PipelineState* d_gfx_bind(void* state) { return swap(kGraphics, state, g_orig_gfx_bind(state)); }

Patch* patch_of(rg_handle h) {
  return h > 0 && static_cast<uint32_t>(h) <= g_np.load(std::memory_order_acquire) ? &g_p[h - 1] : nullptr;
}
Comp* comp_of(rg_handle h) {
  return h > 0 && static_cast<uint32_t>(h) <= g_nc.load(std::memory_order_acquire) ? &g_c[h - 1] : nullptr;
}

bool is_dxbc(const rg_blob& b) { return b.bytes && b.size >= 32 && std::memcmp(b.bytes, "DXBC", 4) == 0; }

}  // namespace

rg_handle patch_register(const rg_patch_desc* d) {
  if (!registration_open()) return RG_E_LATE;
  if (!d || d->stage > RG_STAGE_GS || !d->variants || d->variant_count == 0 || d->variant_count > RG_MAX_VARIANTS ||
      d->initial_variant < -1 || d->initial_variant >= static_cast<int32_t>(d->variant_count))
    return RG_E_ARG;
  for (uint32_t v = 0; v < d->variant_count; ++v)
    if (!is_dxbc(d->variants[v])) return RG_E_BLOB;
  const uint32_t n = g_np.load();
  if (n >= kMaxPatches) return RG_E_FULL;
  dxil_hash::Hash h;
  std::memcpy(h.data(), d->hash, 16);
  // Two clients on one shader: the higher priority wins, ties go to the first (docs/internals.md "Core rules").
  for (uint32_t i = 0; i < n; ++i) {
    Patch& o = g_p[i];
    if (o.stage != d->stage || o.hash != h || o.state.load() == RG_STATE_REFUSED) continue;
    if (d->priority <= o.priority) {
      g_sdk->logger->WarnF(g_handle, "host: patch '%s' REFUSED - '%s' already holds that shader", d->name, o.name);
      char why[96];
      std::snprintf(why, sizeof why, "'%s' holds that shader with a higher or equal priority", o.name);
      clients::reason(why);
      return RG_E_TAKEN;
    }
    o.state.store(RG_STATE_REFUSED);
    g_sdk->logger->WarnF(g_handle, "host: patch '%s' takes the shader from '%s' (higher priority)", d->name, o.name);
    char why[96];
    std::snprintf(why, sizeof why, "taken by '%s' (higher priority)", d->name ? d->name : "?");
    clients::refused(clients::kPatch, static_cast<rg_handle>(i + 1), why);
  }
  Patch& p = g_p[n];
  p.name = d->name ? d->name : "?";
  p.stage = d->stage;
  p.hash = h;
  for (uint32_t v = 0; v < d->variant_count; ++v) p.v[v] = d->variants[v];
  p.n = d->variant_count;
  p.priority = d->priority;
  p.variant.store(d->initial_variant);
  g_np.store(n + 1, std::memory_order_release);
  return static_cast<rg_handle>(n + 1);
}

void patch_select(rg_handle h, int32_t variant) {
  Patch* p = patch_of(h);
  if (!p || variant < -1 || variant >= static_cast<int32_t>(p->n)) return;
  p->variant.store(variant, std::memory_order_relaxed);
}

void patch_stats(rg_handle h, rg_patch_stats* out) {
  if (!out) return;
  *out = {};
  Patch* p = patch_of(h);
  if (!p) return;
  out->state = p->state.load();
  out->variant = p->variant.load();
  p->c.fill(out);
  out->out_of_scope = p->scope.out_of_scope.load();
}

int32_t patch_scope(rg_handle h, const char* node) {
  if (!registration_open()) return RG_E_LATE;
  Patch* p = patch_of(h);
  if (!p) return RG_E_ARG;
  if (const int32_t r = p->scope.add(node)) return r;
  render_flow::want();  // the node on the recording thread
  g_sdk->logger->InfoF(g_handle, "host: patch '%s' scoped to node '%s' (M11)", p->name, node);
  return 0;
}

rg_handle companion_register(const char* name, const uint8_t* hash16, rg_blob cs) {
  if (!registration_open()) return RG_E_LATE;
  if (!hash16) return RG_E_ARG;
  if (!is_dxbc(cs)) return RG_E_BLOB;
  const uint32_t n = g_nc.load();
  if (n >= kMaxComps) return RG_E_FULL;
  Comp& c = g_c[n];
  c.name = name ? name : "?";
  std::memcpy(c.hash.data(), hash16, 16);
  c.cs = cs;
  g_nc.store(n + 1, std::memory_order_release);
  return static_cast<rg_handle>(n + 1);
}

void companion_stats(rg_handle h, rg_patch_stats* out) {
  if (!out) return;
  *out = {};
  Comp* c = comp_of(h);
  if (!c) return;
  const uint32_t st = c->state.load();
  out->state = st == RG_STATE_REFUSED ? st : g_attached[kCompute] ? RG_STATE_OK : RG_STATE_PENDING;
  out->variant = 0;
  c->c.fill(out);
  out->out_of_scope = c->scope.out_of_scope.load();
}

int32_t companion_scope(rg_handle h, const char* node) {
  if (!registration_open()) return RG_E_LATE;
  Comp* c = comp_of(h);
  if (!c) return RG_E_ARG;
  if (const int32_t r = c->scope.add(node)) return r;
  render_flow::want();  // the node on the recording thread
  g_sdk->logger->InfoF(g_handle, "host: companion '%s' scoped to node '%s' (M11)", c->name, node);
  return 0;
}

bool needed() {
  return g_np.load() != 0 || g_nc.load() != 0 || passes::has_subs() || device_ready_wanted() || graph_probe::census();
}
bool has_companions() { return g_nc.load() != 0; }

bool attach() {
  const uint32_t np = g_np.load();
  for (uint32_t i = 0; i < np; ++i) g_want_stage[g_p[i].stage] = true;
  g_want[kCompute] = g_want_stage[RG_STAGE_CS] || g_nc.load() != 0 || passes::has_subs() || device_ready_wanted();
  g_want[kGraphics] = g_want_stage[RG_STAGE_VS] || g_want_stage[RG_STAGE_PS] || g_want_stage[RG_STAGE_GS];
  if (graph_probe::census()) g_want[kCompute] = g_want[kGraphics] = true;  // the census sees every PSO
  // Create first: an engine PSO must be mapped before a bind detour can recognise it.
  struct H {
    Pipe pipe;
    uintptr_t rva, ret;
    void* detour;
    void** orig;
    const char* name;
  };
  const H hooks[] = {
      {kCompute, kComputeSubCacheCreate, kComputeCompileRet, reinterpret_cast<void*>(&d_cs_create),
       reinterpret_cast<void**>(&g_orig_cs_create), "PSOSubCache<compute>::Create"},
      {kGraphics, kGraphicsSubCacheCreate, kGraphicsCompileRet, reinterpret_cast<void*>(&d_gfx_create),
       reinterpret_cast<void**>(&g_orig_gfx_create), "PSOSubCache<graphics>::Create"},
      {kCompute, kComputeCreatePSO, kSetupForDispatchRet, reinterpret_cast<void*>(&d_cs_bind),
       reinterpret_cast<void**>(&g_orig_cs_bind), "ComputePSO::CreatePSO"},
      {kGraphics, kGraphicsCreatePSO, kSetupForDrawRet, reinterpret_cast<void*>(&d_gfx_bind),
       reinterpret_cast<void**>(&g_orig_gfx_bind), "GraphicsPSO::CreatePSO"},
  };
  size_t done = 0;
  for (; done < sizeof(hooks) / sizeof(hooks[0]); ++done) {
    const H& k = hooks[done];
    if (!g_want[k.pipe]) continue;
    if (!attach_hook(k.rva, k.ret, k.detour, k.orig, k.name)) break;
  }
  if (done < sizeof(hooks) / sizeof(hooks[0])) {
    while (done--)
      if (g_want[hooks[done].pipe]) detach_hook(hooks[done].rva);
    g_sdk->logger->Warn(g_handle, "pso swap: NOT attached - every patch stays with the engine's shader");
    for (uint32_t i = 0; i < np; ++i) g_p[i].state.store(RG_STATE_REFUSED);
    return false;
  }
  g_attached[kCompute] = g_want[kCompute];
  g_attached[kGraphics] = g_want[kGraphics];
  for (uint32_t i = 0; i < np; ++i) {
    uint32_t expect = RG_STATE_PENDING;
    g_p[i].state.compare_exchange_strong(expect, RG_STATE_OK);
  }
  return true;
}

void detach() {
  if (g_attached[kCompute]) detach_hook(kComputeCreatePSO), detach_hook(kComputeSubCacheCreate);
  if (g_attached[kGraphics]) detach_hook(kGraphicsCreatePSO), detach_hook(kGraphicsSubCacheCreate);
  g_attached[kCompute] = g_attached[kGraphics] = false;
}

bool attached() { return g_attached[kCompute] || g_attached[kGraphics]; }

uint32_t claimed(const uint8_t* hash16) {
  if (!attached() || !hash16) return 0;
  const uint32_t n = g_np.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n; ++i)
    if (std::memcmp(g_p[i].hash.data(), hash16, 16) == 0 && g_p[i].state.load() != RG_STATE_REFUSED) return 1;
  return 0;
}

void frame_tick() {
  if (++g_sweep_frame % kSweepFrames || (g_entries.load(std::memory_order_relaxed) == 0 && g_retired.empty())) return;
  AcquireSRWLockExclusive(&g_insert_lock);
  sweep_locked();
  ReleaseSRWLockExclusive(&g_insert_lock);
}

void host_stats(rg_host_stats* out) {
  out->pso_hooks = attached() ? 1u : 0u;
  out->mapped = g_entries.load();
  out->compute_creates = g_creates[kCompute].load();
  out->graphics_creates = g_creates[kGraphics].load();
  out->creates_unhashed = g_creates_unhashed.load();
  out->compute_binds = g_calls[kCompute].load();
  out->compute_bind_cycles = g_cycles[kCompute].load();
  out->graphics_binds = g_calls[kGraphics].load();
  out->graphics_bind_cycles = g_cycles[kGraphics].load();
}

bool patch_info(rg_handle h, uint32_t* stage, const rg_blob** variants, uint32_t* count) {
  Patch* p = patch_of(h);
  if (!p) return false;
  *stage = p->stage;
  *variants = p->v;
  *count = p->n;
  return true;
}

void patch_refuse(rg_handle h, const char* why) {
  Patch* p = patch_of(h);
  if (!p || p->state.exchange(RG_STATE_REFUSED) == RG_STATE_REFUSED) return;
  g_sdk->logger->ErrorF(g_handle, "host: patch '%s' REFUSED - %s; the engine's shader stays", p->name, why);
  clients::refused(clients::kPatch, h, why);
}

bool patch_selected(rg_handle h) {
  Patch* p = patch_of(h);
  return p && p->variant.load(std::memory_order_relaxed) >= 0 &&
         p->state.load(std::memory_order_relaxed) != RG_STATE_REFUSED;
}

ID3D12PipelineState* companion_of(ID3D12PipelineState* engine, rg_handle h) {
  if (!engine || g_entries.load(std::memory_order_relaxed) == 0) return nullptr;
  const Slot* s = find(reinterpret_cast<uintptr_t>(engine));
  if (!s || s->m.kind != kComp || s->m.idx[0] + 1 != static_cast<uint32_t>(h)) return nullptr;
  return s->m.clone[0];
}

bool companion_info(rg_handle h, rg_blob* cs) {
  Comp* c = comp_of(h);
  if (!c) return false;
  *cs = c->cs;
  return true;
}

bool companion_in_scope(rg_handle h) {
  Comp* c = comp_of(h);
  return c && (!c->scope.any() || in_node(c->scope, c->name, "dispatch"));
}

void companion_refuse(rg_handle h, const char* why) {
  Comp* c = comp_of(h);
  if (!c || c->state.exchange(RG_STATE_REFUSED) == RG_STATE_REFUSED) return;
  g_sdk->logger->ErrorF(g_handle, "host: companion '%s' REFUSED - %s; it is never dispatched", c->name, why);
  clients::refused(clients::kCompanion, h, why);
}

void companion_count(rg_handle h) {
  if (Comp* c = comp_of(h)) c->c.swaps.fetch_add(1, std::memory_order_relaxed);
}

void bound_subs_reset() { t_bound_subs = 0; }
uint32_t bound_subs_take() {
  const uint32_t m = t_bound_subs;
  t_bound_subs = 0;
  return m;
}

}  // namespace core::pso
