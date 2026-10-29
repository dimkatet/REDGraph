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
// The graphics wing of M4 (2026-10-10): a graphics engine PSO also maps its DRAW subscriptions (mask) and its DRAW
// COMPANIONS (a PSO each, built from the engine's desc); the bind funnel leaves {context, PSO it returned, engine PSO,
// mask} in a thread_local, and passes' draw hooks check it against the PSO on the list (ctx+0x3d0) - a batch of draws
// binds once.

#include <Windows.h>
#include <d3d12.h>
#include <intrin.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <string>
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

constexpr uint32_t kMaxPatches = kMaxPatchCount;  // internal.hpp - bindings sizes its per-patch tables by it
struct Patch {
  const char* name;
  uint32_t stage;
  dxil_hash::Hash hash;
  rg_blob v[RG_MAX_VARIANTS];  // {NULL, 0} (VS / PS): the engine's bytecode stays - a state-only variant
  uint32_t n;
  int32_t priority;
  rg_pso_state st[RG_MAX_VARIANTS];  // patch_state: the variant's pipeline-state edits (st.set 0 = none)
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

// Draw companions: a graphics PSO per engine PSO whose `stage` shader has the hash - that stage's bytecode replaced
// (bytes NULL = the engine's) and / or its pipeline state edited.
constexpr uint32_t kMaxDrawComps = 8;
struct DrawComp {
  const char* name;
  uint32_t stage;
  dxil_hash::Hash hash;
  rg_blob shader;
  rg_pso_state st;  // st.set 0 = none
  std::atomic<uint32_t> state{RG_STATE_PENDING};  // REFUSED = a lease failed its vanilla test: never built, never drawn
  Counters c;
  Scope scope;
};
DrawComp g_dc[kMaxDrawComps];
std::atomic<uint32_t> g_ndc{0};

// engine PSO -> clones, open addressing: a writer (Create, rare, under g_insert_lock) stores the value, then publishes
// the key with release; the bind detours read lock-free. A tree VS pairs with many PS / states.
// The key is the engine PSO's ADDRESS, and the engine frees PSOs: GpuApi::Destroy(ShaderRef) ->
// PSOSubCache::ShaderDestroyed (0x14088e30c graphics / 0x14088fc1c compute) releases every cached PSO built with a
// destroyed shader (material streaming). So the host holds a reference on every engine PSO it maps - its address
// cannot go to another PSO while mapped - and sweep() (every kSweepFrames, from the frame hook) drops the entries
// whose PSO only the host still holds: the key becomes kTomb, the clones are released one sweep LATER (a command list
// recorded before may still be on the GPU). Until 2026-10-10 the table was insert-only: a new PSO at a freed address
// found the OLD entry and bound clones built from another desc (seen as "not mapped (duplicate ...)" for VS 1b259993).
constexpr uint32_t kSlotBits = 14;  // 16384 (4096 until 2026-10-11: material patches map hundreds of PSOs each)
constexpr uint32_t kSlots = 1u << kSlotBits;
constexpr uint32_t kMaxEntries = kSlots / 2;
constexpr uintptr_t kTomb = 1;  // a removed entry: probing goes on past it, insert may reuse it
constexpr uint32_t kSweepFrames = 64;
// kSubOnly: an engine PSO mapped only for its dispatch subscriptions (M4c) - no clone.
enum SlotKind : uint32_t { kPatch = 0, kComp = 1, kSubOnly = 2 };
// What an engine PSO maps to. kPatch: up to kMaxStages patches (one per stage: VS, PS, an added GS), and a clone per
// COMBINATION of their states - combo = sum over i of (variant_i + 1) * stride[i], 0 = vanilla, so clone[0] is the
// engine's own (nullptr). kComp: clone[0] = the companion PSO, idx[0] its index.
constexpr uint32_t kMaxStages = 3;
// [Dev] ShaderHotReload: a deep copy of the engine's desc, kept with the mapping - the engine's desc exists only inside
// PSOSubCache::Create, and a reload rebuilds the clones later. Every pointer it holds is ours (the root signature
// AddRef'd). Off: never made.
struct DescCopy {
  Pipe pipe = kCompute;
  D3D12_COMPUTE_PIPELINE_STATE_DESC cs{};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC gfx{};
  std::vector<std::unique_ptr<uint8_t[]>> bytes;
  std::vector<D3D12_INPUT_ELEMENT_DESC> il;
  std::vector<D3D12_SO_DECLARATION_ENTRY> so;
  std::vector<UINT> strides;
  std::vector<std::string> names;  // reserved up front: the descs point into them
  const void* desc() const { return pipe == kCompute ? static_cast<const void*>(&cs) : &gfx; }
  ~DescCopy() {
    ID3D12RootSignature* rs = pipe == kCompute ? cs.pRootSignature : gfx.pRootSignature;
    if (rs) rs->Release();
  }
};
struct Mapping {
  DescCopy* desc = nullptr;  // [Dev] ShaderHotReload only (kPatch / kComp / draw companions), owned
  ID3D12PipelineState** clone = nullptr;  // nclone entries; nullptr = that build failed (the engine PSO stays bound)
  uint32_t nclone = 0;
  uint32_t kind = kPatch;
  uint32_t np = 0;
  uint32_t idx[kMaxStages] = {};
  uint32_t stride[kMaxStages] = {};
  uint32_t subs = 0;  // passes' subscription mask: dispatch subscriptions (compute) / draw subscriptions (graphics)
  // Draw companions of this (graphics) engine PSO: dc[i] = the PSO of g_dc[dc_idx[i]], nullptr = its build failed.
  static constexpr uint32_t kMaxDc = 4;
  uint32_t ndc = 0;
  uint32_t dc_idx[kMaxDc] = {};
  ID3D12PipelineState* dc[kMaxDc] = {};
};
struct Slot {
  std::atomic<uintptr_t> engine{0};
  Mapping m;
};
Slot g_slots[kSlots];
std::atomic<uint32_t> g_entries{0};
SRWLOCK g_insert_lock = SRWLOCK_INIT;

uint32_t slot_of(uintptr_t p) {
  return static_cast<uint32_t>(((p >> 4) * 0x9E3779B97F4A7C15ull) >> (64 - kSlotBits)) & (kSlots - 1);
}

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
    for (uint32_t i = 0; i < s.m.ndc; ++i)
      if (s.m.dc[i]) g_retired.push_back(s.m.dc[i]);
    s.m.ndc = 0;
    delete s.m.desc;  // a reload runs on this (the frame) thread under this lock: nobody reads it now
    s.m.desc = nullptr;
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
std::atomic<bool> g_state_overlap_logged{false};
std::atomic<bool> g_first_create{false};
thread_local uint32_t t_bound_subs = 0;
// The graphics wing: the last graphics bind on this thread (every bind while draw subscriptions exist).
struct GfxBind {
  const uint8_t* ctx;
  ID3D12PipelineState* bound;   // what the funnel returned = what the engine sets at ctx+0x3d0
  ID3D12PipelineState* engine;  // the engine's own PSO (the key of its mapping)
  uint32_t subs;                // its draw subscriptions, 0 = none
};
thread_local GfxBind t_gfx{};
bool g_track_draws = false;  // set at attach: draw subscriptions exist

// [Dev] ShaderHotReload (dev_enable, before attach). g_dev_lock: a creation holds it shared from the clone build to the
// insert, a reload exclusive while it swaps blobs and rebuilds clones - a creation in between sees the old or the new
// blob, never half of one. Replaced clones are released kDevRetireFrames frames later (a list may still be on the GPU).
bool g_dev = false;
SRWLOCK g_dev_lock = SRWLOCK_INIT;
constexpr uint32_t kDevRetireFrames = 16;
std::vector<std::pair<ID3D12PipelineState*, uint32_t>> g_dev_retired;  // the frame thread only

DescCopy* copy_desc(Pipe pipe, const void* desc_v) {
  auto* c = new DescCopy;
  c->pipe = pipe;
  auto own = [c](D3D12_SHADER_BYTECODE& b) {
    if (!b.pShaderBytecode || !b.BytecodeLength) return;
    auto p = std::make_unique<uint8_t[]>(b.BytecodeLength);
    std::memcpy(p.get(), b.pShaderBytecode, b.BytecodeLength);
    b.pShaderBytecode = p.get();
    c->bytes.push_back(std::move(p));
  };
  if (pipe == kCompute) {
    c->cs = *static_cast<const D3D12_COMPUTE_PIPELINE_STATE_DESC*>(desc_v);
    own(c->cs.CS);
    c->cs.CachedPSO = {};
    if (c->cs.pRootSignature) c->cs.pRootSignature->AddRef();
    return c;
  }
  auto& g = c->gfx;
  g = *static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(desc_v);
  for (D3D12_SHADER_BYTECODE* b : {&g.VS, &g.PS, &g.DS, &g.HS, &g.GS}) own(*b);
  c->names.reserve(g.InputLayout.NumElements + g.StreamOutput.NumEntries);
  auto name = [c](const char* s) { return s ? c->names.emplace_back(s).c_str() : nullptr; };
  if (g.InputLayout.pInputElementDescs && g.InputLayout.NumElements) {
    c->il.assign(g.InputLayout.pInputElementDescs, g.InputLayout.pInputElementDescs + g.InputLayout.NumElements);
    for (auto& e : c->il) e.SemanticName = name(e.SemanticName);
    g.InputLayout.pInputElementDescs = c->il.data();
  }
  if (g.StreamOutput.pSODeclaration && g.StreamOutput.NumEntries) {
    c->so.assign(g.StreamOutput.pSODeclaration, g.StreamOutput.pSODeclaration + g.StreamOutput.NumEntries);
    for (auto& e : c->so) e.SemanticName = name(e.SemanticName);
    g.StreamOutput.pSODeclaration = c->so.data();
  }
  if (g.StreamOutput.pBufferStrides && g.StreamOutput.NumStrides) {
    c->strides.assign(g.StreamOutput.pBufferStrides, g.StreamOutput.pBufferStrides + g.StreamOutput.NumStrides);
    g.StreamOutput.pBufferStrides = c->strides.data();
  }
  g.CachedPSO = {};
  if (g.pRootSignature) g.pRootSignature->AddRef();
  return c;
}

std::atomic<uint64_t> g_creates[2] = {}, g_creates_unhashed{0};
std::atomic<uint64_t> g_calls[2] = {}, g_cycles[2] = {};
thread_local uint64_t t_cycles[2] = {};
thread_local uint32_t t_calls[2] = {};

std::optional<dxil_hash::Hash> hash_of(const D3D12_SHADER_BYTECODE& s) {
  if (!s.pShaderBytecode || !s.BytecodeLength) return std::nullopt;
  return dxil_hash::of(s.pShaderBytecode, s.BytecodeLength);
}

// [Dev] Readback's vsout (dev_readback_enable, before attach): every graphics PSO the engine creates, its desc without
// bytecode (the input layout copied, the root signature referenced) and its VS hash; the VS bytes once per hash. Keyed
// by the engine PSO's address - a PSO created later at a freed address overwrites the record (creation precedes every
// bind). Off: never made.
bool g_rb_dev = false;
struct RbRec {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};  // VS / PS / GS / HS / DS and StreamOutput cleared
  std::vector<D3D12_INPUT_ELEMENT_DESC> il;
  std::vector<std::string> names;  // reserved up front: il points into them
  dxil_hash::Hash vs{};
  bool has_vs = false, tess = false;
  ~RbRec() {
    if (d.pRootSignature) d.pRootSignature->Release();
  }
};
SRWLOCK g_rb_lock = SRWLOCK_INIT;
std::unordered_map<uintptr_t, std::unique_ptr<RbRec>> g_rb_recs;     // g_rb_lock
std::map<dxil_hash::Hash, std::vector<uint8_t>> g_rb_vs;              // g_rb_lock; a VS's bytes by its hash

void rb_record(ID3D12PipelineState* engine, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& g) {
  auto r = std::make_unique<RbRec>();
  r->d = g;
  r->d.VS = r->d.PS = r->d.GS = r->d.HS = r->d.DS = {};
  r->d.StreamOutput = {};
  r->d.CachedPSO = {};
  r->tess = g.HS.pShaderBytecode || g.DS.pShaderBytecode;
  if (g.InputLayout.pInputElementDescs && g.InputLayout.NumElements) {
    r->names.reserve(g.InputLayout.NumElements);
    r->il.assign(g.InputLayout.pInputElementDescs, g.InputLayout.pInputElementDescs + g.InputLayout.NumElements);
    for (auto& e : r->il) e.SemanticName = e.SemanticName ? r->names.emplace_back(e.SemanticName).c_str() : nullptr;
    r->d.InputLayout.pInputElementDescs = r->il.data();
  }
  if (r->d.pRootSignature) r->d.pRootSignature->AddRef();
  const auto h = hash_of(g.VS);
  AcquireSRWLockExclusive(&g_rb_lock);
  if (h) {
    r->vs = *h;
    r->has_vs = true;
    if (g_rb_vs.find(*h) == g_rb_vs.end()) {
      const auto* b = static_cast<const uint8_t*>(g.VS.pShaderBytecode);
      g_rb_vs.emplace(*h, std::vector<uint8_t>(b, b + g.VS.BytecodeLength));
    }
  }
  g_rb_recs[reinterpret_cast<uintptr_t>(engine)] = std::move(r);
  ReleaseSRWLockExclusive(&g_rb_lock);
}

// [hot while a vsout request is armed] kDevDrawBit when the engine PSO's VS is one a request waits for.
uint32_t rb_draw_bit(ID3D12PipelineState* engine) {
  dxil_hash::Hash h;
  bool have = false;
  AcquireSRWLockShared(&g_rb_lock);
  const auto it = g_rb_recs.find(reinterpret_cast<uintptr_t>(engine));
  if (it != g_rb_recs.end() && it->second->has_vs) h = it->second->vs, have = true;
  ReleaseSRWLockShared(&g_rb_lock);
  return have && readback::vs_wanted(h.data()) ? kDevDrawBit : 0u;
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
  uint32_t subs = 0;            // dispatch / draw subscriptions on the shader, whatever n says
  uint32_t ndc = 0;             // graphics: draw companions matching
  uint32_t dc[Mapping::kMaxDc] = {};
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
  // The graphics wing: draw subscriptions and draw companions keyed on the VS or the PS.
  const uint32_t ndc = g_ndc.load(std::memory_order_acquire);
  if (passes::has_draw_subs() || ndc) {
    const auto vs = hash_of(g->VS), ps = hash_of(g->PS);
    m.subs = passes::draw_subs_for(vs ? vs->data() : nullptr, ps ? ps->data() : nullptr);
    for (uint32_t i = 0; i < ndc && m.ndc < Mapping::kMaxDc; ++i) {
      const bool on_vs = g_dc[i].stage == RG_STAGE_VS;
      const auto& h = on_vs ? vs : ps;
      if (!h || *h != g_dc[i].hash || g_dc[i].state.load() == RG_STATE_REFUSED) continue;
      // The lease test on that stage's vanilla bytecode, as for a compute companion: a refusal builds no companion.
      const D3D12_SHADER_BYTECODE& code = on_vs ? g->VS : g->PS;
      if (!bindings::on_draw_companion_matched(static_cast<rg_handle>(i + 1), code.pShaderBytecode,
                                               code.BytecodeLength))
        continue;
      m.dc[m.ndc++] = i;
    }
  }
  return m;
}

// A variant's pipeline-state edits on a copy of the engine's desc. `done` = the groups set so far on this clone; a group
// set twice (two composed patches) - the later wins, logged once.
void apply_state(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, const rg_pso_state& s, uint32_t& done, const char* who) {
  if (!s.set) return;
  if ((done & s.set) && !g_state_overlap_logged.exchange(true))
    g_sdk->logger->WarnF(g_handle, "pso swap: '%s' edits pipeline-state group(s) 0x%x another patch on the same PSO "
                         "edited too - the later stage wins (VS, PS, GS); logged once", who, done & s.set);
  done |= s.set;
  auto& r = d.RasterizerState;
  if (s.set & RG_PSO_CULL) r.CullMode = static_cast<D3D12_CULL_MODE>(s.cull_mode);
  if (s.set & RG_PSO_FILL) r.FillMode = static_cast<D3D12_FILL_MODE>(s.fill_mode);
  if (s.set & RG_PSO_DEPTH_BIAS) {
    r.DepthBias = s.depth_bias;
    r.DepthBiasClamp = s.depth_bias_clamp;
    r.SlopeScaledDepthBias = s.slope_scaled_depth_bias;
  }
  if (s.set & RG_PSO_DEPTH) {
    d.DepthStencilState.DepthEnable = s.depth_enable ? TRUE : FALSE;
    d.DepthStencilState.DepthWriteMask = s.depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    d.DepthStencilState.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(s.depth_func);
  }
  if (s.set & (RG_PSO_BLEND | RG_PSO_WRITE_MASK)) {
    auto& b = d.BlendState;
    if (!b.IndependentBlendEnable && (s.rt_mask & ~1u)) {  // per-target edits need per-target descs: RT0's to all first
      for (uint32_t i = 1; i < 8; ++i) b.RenderTarget[i] = b.RenderTarget[0];
      b.IndependentBlendEnable = TRUE;
    }
    for (uint32_t i = 0; i < 8; ++i) {
      if (!(s.rt_mask & (1u << i))) continue;
      auto& t = b.RenderTarget[i];
      if (s.set & RG_PSO_BLEND) {
        t.BlendEnable = s.blend.enable ? TRUE : FALSE;
        t.LogicOpEnable = FALSE;
        t.SrcBlend = static_cast<D3D12_BLEND>(s.blend.src);
        t.DestBlend = static_cast<D3D12_BLEND>(s.blend.dst);
        t.BlendOp = static_cast<D3D12_BLEND_OP>(s.blend.op);
        t.SrcBlendAlpha = static_cast<D3D12_BLEND>(s.blend.src_alpha);
        t.DestBlendAlpha = static_cast<D3D12_BLEND>(s.blend.dst_alpha);
        t.BlendOpAlpha = static_cast<D3D12_BLEND_OP>(s.blend.op_alpha);
      }
      if (s.set & RG_PSO_WRITE_MASK) t.RenderTargetWriteMask = s.write_mask[i];
    }
  }
  if (s.set & RG_PSO_FORMATS) {
    d.NumRenderTargets = s.num_render_targets;
    for (uint32_t i = 0; i < 8; ++i)
      d.RTVFormats[i] = i < s.num_render_targets ? static_cast<DXGI_FORMAT>(s.rtv_formats[i]) : DXGI_FORMAT_UNKNOWN;
    d.DSVFormat = static_cast<DXGI_FORMAT>(s.dsv_format);
  }
}

const char* stage_label(uint32_t s) { return s == RG_STAGE_VS ? "VS" : s == RG_STAGE_PS ? "PS" : s == RG_STAGE_GS ? "GS" : "CS"; }

// Draw companion `c`'s PSO from the engine's graphics desc.
HRESULT create_dc(ID3D12Device* dev, const void* desc_v, const DrawComp& c, ID3D12PipelineState** out) {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = *static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(desc_v);
  if (c.shader.bytes) (c.stage == RG_STAGE_VS ? d.VS : d.PS) = {c.shader.bytes, c.shader.size};
  uint32_t done = 0;
  apply_state(d, c.st, done, c.name);
  d.CachedPSO = {};
  return dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(out));
}

// The clone of combination `combo` of `map` from the engine's desc, with the patches' blobs as they are now (kComp:
// the companion). e[i] out = patch i's part: 0 vanilla, v + 1 = variant v.
HRESULT create_combo(Pipe pipe, ID3D12Device* dev, const void* desc_v, const Mapping& map, uint32_t combo, uint32_t* e,
                     ID3D12PipelineState** out) {
  if (pipe == kCompute) {
    D3D12_COMPUTE_PIPELINE_STATE_DESC d = *static_cast<const D3D12_COMPUTE_PIPELINE_STATE_DESC*>(desc_v);
    const rg_blob& b = map.kind == kComp ? g_c[map.idx[0]].cs : g_p[map.idx[0]].v[combo - 1];
    e[0] = map.kind == kComp ? 1u : combo;  // a compute patch: combo = variant + 1
    d.CS = {b.bytes, b.size};
    d.CachedPSO = {};
    return dev->CreateComputePipelineState(&d, IID_PPV_ARGS(out));
  }
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d = *static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(desc_v);
  uint32_t state_done = 0;
  for (uint32_t i = 0; i < map.np; ++i) {  // idx is in stage order VS, PS, GS (identify)
    const Patch& p = g_p[map.idx[i]];
    e[i] = (combo / map.stride[i]) % (p.n + 1);
    if (!e[i]) continue;
    const rg_blob& b = p.v[e[i] - 1];
    if (b.bytes) (p.stage == RG_STAGE_GS ? d.GS : p.stage == RG_STAGE_PS ? d.PS : d.VS) = {b.bytes, b.size};
    apply_state(d, p.st[e[i] - 1], state_done, p.name);
  }
  d.CachedPSO = {};
  return dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(out));
}

// The draw companions of a graphics engine PSO, into `map` (before it is inserted).
void build_draw_comps(ID3D12PipelineState* engine, const void* desc_v, const Match& m, Mapping& map) {
  if (!m.ndc) return;
  ID3D12Device* dev = nullptr;
  if (FAILED(engine->GetDevice(IID_PPV_ARGS(&dev)))) return;
  for (uint32_t i = 0; i < m.ndc; ++i) {
    DrawComp& c = g_dc[m.dc[i]];
    ID3D12PipelineState* pso = nullptr;
    const HRESULT hr = create_dc(dev, desc_v, c, &pso);
    map.dc_idx[map.ndc] = m.dc[i];
    map.dc[map.ndc++] = SUCCEEDED(hr) ? pso : nullptr;
    const uint32_t k = c.c.engine_psos.fetch_add(1) + 1;
    if (SUCCEEDED(hr)) {
      c.c.clones.fetch_add(1);
      if (k == 1 || (k <= 3 && verbose()))
        g_sdk->logger->InfoF(g_handle, "pso swap: draw companion '%s' engine PSO %p -> %p (#%u for it)", c.name,
                             static_cast<void*>(engine), static_cast<void*>(pso), k);
    } else {
      c.c.clone_failed.fetch_add(1);
      c.c.last_hr.store(static_cast<uint32_t>(hr));
      g_sdk->logger->ErrorF(g_handle, "pso swap: draw companion '%s' build FAILED (engine PSO %p), hr 0x%08x - never "
                            "issued for it", c.name, static_cast<void*>(engine), static_cast<unsigned>(hr));
    }
  }
  dev->Release();
}

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
  if (g_dev) map.desc = copy_desc(pipe, desc_v);
  map.nclone = total;
  map.clone = new ID3D12PipelineState*[total]();
  ID3D12Device* dev = nullptr;
  const HRESULT hr = engine->GetDevice(IID_PPV_ARGS(&dev));
  uint32_t built = 0, failed = 0;
  uint32_t built_with[kMaxStages] = {};
  for (uint32_t combo = m.kind == kComp ? 0u : 1u; combo < total && SUCCEEDED(hr); ++combo) {
    uint32_t e[kMaxStages] = {};  // per patch: 0 vanilla, v + 1 = variant v
    const HRESULT vhr = create_combo(pipe, dev, desc_v, map, combo, e, &map.clone[combo]);
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
  if (pipe == kGraphics) build_draw_comps(engine, desc_v, m, map);
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
    for (uint32_t i = 0; i < map.ndc; ++i)
      if (map.dc[i]) map.dc[i]->Release();
    delete[] map.clone;
    delete map.desc;
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

void on_create_unlocked(Pipe pipe, void** out, const void* desc, const Match& m);
void on_create(Pipe pipe, void** out, const void* desc, const Match& m) {
  if (!g_dev || !(m.n || m.ndc)) return on_create_unlocked(pipe, out, desc, m);
  AcquireSRWLockShared(&g_dev_lock);
  on_create_unlocked(pipe, out, desc, m);
  ReleaseSRWLockShared(&g_dev_lock);
}
void on_create_unlocked(Pipe pipe, void** out, const void* desc, const Match& m) {
  if (!out || !*out || !desc) return;
  g_creates[pipe].fetch_add(1, std::memory_order_relaxed);
  if (m.n) {
    build(pipe, static_cast<ID3D12PipelineState*>(*out), desc, m);
  } else if (m.subs || m.ndc) {
    Mapping sub;
    sub.kind = kSubOnly;
    sub.subs = m.subs;
    if (pipe == kGraphics) build_draw_comps(static_cast<ID3D12PipelineState*>(*out), desc, m, sub);
    if (g_dev && sub.ndc) sub.desc = copy_desc(pipe, desc);
    if (!insert(reinterpret_cast<uintptr_t>(*out), sub)) {
      for (uint32_t i = 0; i < sub.ndc; ++i)
        if (sub.dc[i]) sub.dc[i]->Release();
      delete sub.desc;
      g_sdk->logger->WarnF(g_handle, "pso swap: subscribed engine PSO %p not mapped (duplicate or table full)", *out);
    }
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
  if (g_rb_dev && out && *out && desc)
    rb_record(static_cast<ID3D12PipelineState*>(*out), *static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(desc));
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
  uint32_t draw_subs = 0;
  if (state) render_nodes::note_ctx(static_cast<const uint8_t*>(state) - (pipe == kCompute ? kCtxComputeState : kCtxGraphicsState));
  if (engine && graph_probe::census()) graph_probe::on_bind(pipe, engine);
  // The graph debug module's shader skip: NULL is the engine's own "still compiling" answer - it skips the draw.
  if (engine && graph_debug::skip(pipe, engine)) out = nullptr;
  else if (engine && g_entries.load(std::memory_order_relaxed) != 0) {
    if (const Slot* s = find(reinterpret_cast<uintptr_t>(engine))) {
      const Mapping& m = s->m;
      if (pipe == kCompute) t_bound_subs = m.subs;
      else draw_subs = m.subs;
      // Draw companions' leases learn the ShaderRef (staged at every draw of this shader, inert for the engine's).
      for (uint32_t i = 0; i < m.ndc; ++i) bindings::draw_companion_bind(static_cast<rg_handle>(m.dc_idx[i] + 1), state);
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
  if (pipe == kGraphics && g_rb_dev && out && readback::vs_armed()) draw_subs |= rb_draw_bit(engine);
  // Every graphics bind, mapped or not: a stale entry would match a new PSO at a freed address.
  if (pipe == kGraphics && g_track_draws)
    t_gfx = {static_cast<const uint8_t*>(state) - kCtxGraphicsState, out, engine, out ? draw_subs : 0u};
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
  for (uint32_t v = 0; v < d->variant_count; ++v) {
    const rg_blob& b = d->variants[v];
    const bool keep = !b.bytes && !b.size && (d->stage == RG_STAGE_VS || d->stage == RG_STAGE_PS);  // state-only
    if (!keep && !is_dxbc(b)) return RG_E_BLOB;
  }
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
  for (rg_pso_state& st : p.st) st = {};
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

int32_t patch_state(rg_handle h, uint32_t variant, const rg_pso_state* s) {
  if (!registration_open()) return RG_E_LATE;
  Patch* p = patch_of(h);
  if (!p || !s || variant >= p->n || p->stage == RG_STAGE_CS) return RG_E_ARG;
  if (s->struct_size < sizeof(rg_pso_state) || (s->set & ~127u)) return RG_E_ARG;  // older layouts: none yet
  const bool bad = ((s->set & RG_PSO_CULL) && (s->cull_mode < 1 || s->cull_mode > 3)) ||
                   ((s->set & RG_PSO_FILL) && (s->fill_mode < 2 || s->fill_mode > 3)) ||
                   ((s->set & RG_PSO_DEPTH) && (s->depth_func < 1 || s->depth_func > 8)) ||
                   ((s->set & (RG_PSO_BLEND | RG_PSO_WRITE_MASK)) && (!s->rt_mask || s->rt_mask > 0xff)) ||
                   ((s->set & RG_PSO_WRITE_MASK) && [&] {
                     for (uint8_t w : s->write_mask)
                       if (w > 15) return true;
                     return false;
                   }()) ||
                   ((s->set & RG_PSO_FORMATS) && s->num_render_targets > 8);
  if (bad) return RG_E_ARG;
  p->st[variant] = *s;
  p->st[variant].struct_size = sizeof(rg_pso_state);
  g_sdk->logger->InfoF(g_handle, "host: patch '%s' variant %u edits the pipeline state (groups 0x%x)", p->name, variant,
                       s->set);
  return 0;
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
  return g_np.load() != 0 || g_nc.load() != 0 || passes::has_subs() || passes::has_draw_subs() || g_ndc.load() != 0 ||
         device_ready_wanted() || graph_probe::census() || g_rb_dev;
}
bool has_companions() { return g_nc.load() != 0; }

bool attach() {
  const uint32_t np = g_np.load();
  for (uint32_t i = 0; i < np; ++i) g_want_stage[g_p[i].stage] = true;
  g_want[kCompute] = g_want_stage[RG_STAGE_CS] || g_nc.load() != 0 || passes::has_subs() || device_ready_wanted();
  g_want[kGraphics] = g_want_stage[RG_STAGE_VS] || g_want_stage[RG_STAGE_PS] || g_want_stage[RG_STAGE_GS] ||
                      passes::has_draw_subs() || g_ndc.load() != 0 || g_rb_dev;
  g_track_draws = passes::has_draw_subs() || g_rb_dev;
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
  ++g_sweep_frame;
  if (g_dev) dev_frame_tick();
  if (g_sweep_frame % kSweepFrames || (g_entries.load(std::memory_order_relaxed) == 0 && g_retired.empty())) return;
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

// ---- the graphics wing ----

namespace {
DrawComp* draw_comp_of(rg_handle h) {
  return h > 0 && static_cast<uint32_t>(h) <= g_ndc.load(std::memory_order_acquire) ? &g_dc[h - 1] : nullptr;
}
}  // namespace

rg_handle draw_companion_register(const char* name, uint32_t stage, const uint8_t* hash16, rg_blob shader,
                                  const rg_pso_state* st) {
  if (!registration_open()) return RG_E_LATE;
  if (!hash16 || (stage != RG_STAGE_VS && stage != RG_STAGE_PS)) return RG_E_ARG;
  if (shader.bytes ? !is_dxbc(shader) : shader.size != 0) return RG_E_BLOB;
  if (st && (st->struct_size < sizeof(rg_pso_state) || (st->set & ~127u))) return RG_E_ARG;
  if (!shader.bytes && !(st && st->set)) {
    clients::reason("neither a shader nor a pipeline-state edit - the companion would draw the same thing again");
    return RG_E_ARG;
  }
  const uint32_t n = g_ndc.load();
  if (n >= kMaxDrawComps) return RG_E_FULL;
  DrawComp& c = g_dc[n];
  c.name = name ? name : "?";
  c.stage = stage;
  std::memcpy(c.hash.data(), hash16, 16);
  c.shader = shader;
  c.st = st ? *st : rg_pso_state{};
  c.st.struct_size = sizeof(rg_pso_state);
  g_ndc.store(n + 1, std::memory_order_release);
  return static_cast<rg_handle>(n + 1);
}

int32_t draw_companion_scope(rg_handle h, const char* node) {
  if (!registration_open()) return RG_E_LATE;
  DrawComp* c = draw_comp_of(h);
  if (!c) return RG_E_ARG;
  if (const int32_t r = c->scope.add(node)) return r;
  render_flow::want();
  g_sdk->logger->InfoF(g_handle, "host: draw companion '%s' scoped to node '%s' (M11)", c->name, node);
  return 0;
}

void draw_companion_stats(rg_handle h, rg_patch_stats* out) {
  if (!out) return;
  *out = {};
  DrawComp* c = draw_comp_of(h);
  if (!c) return;
  out->state = c->state.load() == RG_STATE_REFUSED ? static_cast<uint32_t>(RG_STATE_REFUSED)
               : g_attached[kGraphics]                 ? RG_STATE_OK
                                                       : RG_STATE_PENDING;
  c->c.fill(out);
  out->out_of_scope = c->scope.out_of_scope.load();
}

uint32_t draw_bound(const uint8_t* ctx, ID3D12PipelineState** engine, ID3D12PipelineState** bound) {
  const GfxBind& b = t_gfx;
  if (!b.subs || b.ctx != ctx || !ctx) return 0;
  // The PSO on the list is still the one bound for this thread's last graphics bind, and the draw was not skipped (a
  // NULL PSO - still compiling, a debug skip - leaves the graphics-dirty byte set and records nothing).
  if (*reinterpret_cast<ID3D12PipelineState* const*>(ctx + e231::kCtxPso) != b.bound || ctx[e231::kCtxGraphicsDirty]) return 0;
  *engine = b.engine;
  *bound = b.bound;
  return b.subs;
}

bool draw_subs_pending() { return t_gfx.subs != 0; }

ID3D12PipelineState* draw_companion_of(ID3D12PipelineState* engine, rg_handle h) {
  DrawComp* c = draw_comp_of(h);
  if (!c || !engine || c->state.load(std::memory_order_relaxed) == RG_STATE_REFUSED) return nullptr;
  const Slot* s = find(reinterpret_cast<uintptr_t>(engine));
  if (!s) return nullptr;
  for (uint32_t i = 0; i < s->m.ndc; ++i)
    if (s->m.dc_idx[i] + 1 == static_cast<uint32_t>(h)) {
      if (!s->m.dc[i]) return nullptr;
      if (c->scope.any() && !in_node(c->scope, c->name, "draw")) return nullptr;
      return s->m.dc[i];
    }
  return nullptr;
}

void draw_companion_count(rg_handle h) {
  if (DrawComp* c = draw_comp_of(h)) c->c.swaps.fetch_add(1, std::memory_order_relaxed);
}

bool draw_companion_info(rg_handle h, uint32_t* stage, rg_blob* shader) {
  DrawComp* c = draw_comp_of(h);
  if (!c) return false;
  *stage = c->stage;
  *shader = c->shader;
  return true;
}

void draw_companion_refuse(rg_handle h, const char* why) {
  DrawComp* c = draw_comp_of(h);
  if (!c || c->state.exchange(RG_STATE_REFUSED) == RG_STATE_REFUSED) return;
  g_sdk->logger->ErrorF(g_handle, "host: draw companion '%s' REFUSED - %s; it is never drawn", c->name, why);
  clients::refused(clients::kDrawCompanion, h, why);
}

bool owner_target(uint32_t owner, rg_handle h, uint8_t hash16[16]) {
  const dxil_hash::Hash* hash = nullptr;
  if (owner == 0) {
    if (Patch* p = patch_of(h)) hash = &p->hash;
  } else if (owner == 1) {
    if (Comp* c = comp_of(h)) hash = &c->hash;
  } else if (DrawComp* c = draw_comp_of(h)) {
    hash = &c->hash;
  }
  if (!hash) return false;
  std::memcpy(hash16, hash->data(), 16);
  return true;
}

// ---- [Dev] ShaderHotReload (core/shader_reload.cpp drives it from the frame thread) ----

namespace {
enum TargetKind : uint32_t { kTPatch = 0, kTComp = 1, kTDrawComp = 2 };
const char* const kTargetLabel[3] = {"patch", "companion", "draw companion"};

std::string hex_of(const dxil_hash::Hash& h) {
  char s[33];
  for (int i = 0; i < 16; ++i) std::snprintf(s + 2 * i, 3, "%02x", h[i]);
  return s;
}

struct Target {
  uint32_t kind, idx;
  const char* name;
  uint32_t stage, variants;
  const dxil_hash::Hash* hash;
};
// Every target a reload can name, in registration order (a refused patch is none).
std::vector<Target> targets() {
  std::vector<Target> t;
  for (uint32_t i = 0, n = g_np.load(std::memory_order_acquire); i < n; ++i)
    if (g_p[i].state.load() != RG_STATE_REFUSED) t.push_back({kTPatch, i, g_p[i].name, g_p[i].stage, g_p[i].n, &g_p[i].hash});
  for (uint32_t i = 0, n = g_nc.load(std::memory_order_acquire); i < n; ++i)
    if (g_c[i].state.load() != RG_STATE_REFUSED) t.push_back({kTComp, i, g_c[i].name, RG_STAGE_CS, 1, &g_c[i].hash});
  for (uint32_t i = 0, n = g_ndc.load(std::memory_order_acquire); i < n; ++i)
    t.push_back({kTDrawComp, i, g_dc[i].name, g_dc[i].stage, 1, &g_dc[i].hash});
  return t;
}

void dev_retire(ID3D12PipelineState* p) {
  if (p) g_dev_retired.push_back({p, g_sweep_frame});
}
}  // namespace

void dev_enable() { g_dev = true; }

void dev_readback_enable() { g_rb_dev = true; }

bool dev_vs_of(ID3D12PipelineState* engine, ID3D12PipelineState* bound, rg_blob* vs, uint8_t* hash16) {
  if (!g_rb_dev || !engine) return false;
  // A clone: the VS of the patch variant it carries (a state-only variant keeps the engine's).
  if (bound && bound != engine) {
    if (const Slot* s = find(reinterpret_cast<uintptr_t>(engine)); s && s->m.kind == kPatch)
      for (uint32_t c = 1; c < s->m.nclone; ++c) {
        if (s->m.clone[c] != bound) continue;
        for (uint32_t i = 0; i < s->m.np; ++i) {
          const Patch& p = g_p[s->m.idx[i]];
          const uint32_t e = (c / s->m.stride[i]) % (p.n + 1);
          if (p.stage != RG_STAGE_VS || !e || !p.v[e - 1].bytes) continue;
          *vs = p.v[e - 1];
          const auto h = dxil_hash::of(vs->bytes, vs->size);
          if (h) std::memcpy(hash16, h->data(), 16);
          else std::memset(hash16, 0, 16);
          return true;
        }
      }
  }
  AcquireSRWLockShared(&g_rb_lock);
  bool ok = false;
  const auto it = g_rb_recs.find(reinterpret_cast<uintptr_t>(engine));
  if (it != g_rb_recs.end() && it->second->has_vs) {
    const auto b = g_rb_vs.find(it->second->vs);
    if (b != g_rb_vs.end()) {  // the bytes never move or go: kept for the process
      *vs = {b->second.data(), static_cast<uint32_t>(b->second.size())};
      std::memcpy(hash16, it->second->vs.data(), 16);
      ok = true;
    }
  }
  ReleaseSRWLockShared(&g_rb_lock);
  return ok;
}

long dev_so_pso(ID3D12PipelineState* engine, rg_blob vs, const void* so, ID3D12PipelineState** out, bool bare,
                void** root_sig) {
  *out = nullptr;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
  AcquireSRWLockShared(&g_rb_lock);
  const auto it = g_rb_recs.find(reinterpret_cast<uintptr_t>(engine));
  const bool have = it != g_rb_recs.end() && !it->second->tess;
  if (have) d = it->second->d;  // the input layout it points to lives as long as the record
  if (have && d.pRootSignature) d.pRootSignature->AddRef();
  ReleaseSRWLockShared(&g_rb_lock);
  if (!have) return E_INVALIDARG;
  d.VS = {vs.bytes, vs.size};
  d.StreamOutput = *static_cast<const D3D12_STREAM_OUTPUT_DESC*>(so);
  d.DepthStencilState.DepthEnable = FALSE;
  d.DepthStencilState.StencilEnable = FALSE;
  if (root_sig) *root_sig = d.pRootSignature;
  if (bare) {
    d.NumRenderTargets = 0;
    for (auto& f : d.RTVFormats) f = DXGI_FORMAT_UNKNOWN;
    d.DSVFormat = DXGI_FORMAT_UNKNOWN;
    d.BlendState = {};
    for (auto& t : d.BlendState.RenderTarget) t.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleDesc = {1, 0};
  }
  ID3D12Device* dev = nullptr;
  HRESULT hr = engine->GetDevice(IID_PPV_ARGS(&dev));
  if (SUCCEEDED(hr)) {
    hr = dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(out));
    dev->Release();
  }
  if (d.pRootSignature) d.pRootSignature->Release();
  return hr;
}

std::string dev_file_stem(const char* name) {
  std::string s = name ? name : "";
  for (char& c : s)
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) c = '_';
  return s;
}

std::string dev_targets() {
  std::string out;
  for (const Target& t : targets()) {
    char line[400];
    std::snprintf(line, sizeof line, "%-15s %s  %s  variants %u  file: %s[.vN].dxbc|.ll  or  %s[.vN].dxbc|.ll\n",
                  kTargetLabel[t.kind], stage_label(t.stage), t.name, t.variants, dev_file_stem(t.name).c_str(),
                  hex_of(*t.hash).c_str());
    out += line;
  }
  return out;
}

namespace {
// Under g_dev_lock (exclusive) + g_insert_lock: every engine PSO carrying target `t`, the clones holding variant `v`
// rebuilt from the kept desc with the blobs as they are NOW. All or nothing: any failure releases what was built and
// leaves every clone as it was (false). On success the new clones go in place (one pointer store each - the bind
// funnel reads them lock-free) and the old ones are retired.
bool dev_rebuild(const Target& t, uint32_t v, DevReload* r) {
  struct Built {
    ID3D12PipelineState** at;
    ID3D12PipelineState* pso;
  };
  std::vector<Built> built;
  ID3D12Device* dev = nullptr;
  bool ok = true;
  for (Slot& s : g_slots) {
    const uintptr_t k = s.engine.load(std::memory_order_relaxed);
    if (k <= kTomb) continue;
    Mapping& m = s.m;
    const bool gfx = m.desc && m.desc->pipe == kGraphics;
    if (t.kind == kTDrawComp) {
      for (uint32_t i = 0; i < m.ndc; ++i) {
        if (m.dc_idx[i] != t.idx) continue;
        ++r->mapped;
        if (!m.desc) { ++r->no_desc; continue; }
        if (!dev && FAILED(reinterpret_cast<ID3D12PipelineState*>(k)->GetDevice(IID_PPV_ARGS(&dev)))) return false;
        ID3D12PipelineState* pso = nullptr;
        const HRESULT hr = create_dc(dev, m.desc->desc(), g_dc[t.idx], &pso);
        if (FAILED(hr)) { ok = false, r->last_hr = static_cast<uint32_t>(hr), ++r->failed; continue; }
        built.push_back({&m.dc[i], pso});
      }
      continue;
    }
    if (m.kind != (t.kind == kTComp ? kComp : kPatch) || !m.clone) continue;
    int32_t at = -1;  // the target's position among the mapping's patches
    for (uint32_t i = 0; i < m.np; ++i)
      if (m.idx[i] == t.idx) at = static_cast<int32_t>(i);
    if (at < 0) continue;
    ++r->mapped;
    if (!m.desc) { ++r->no_desc; continue; }
    if (!dev && FAILED(reinterpret_cast<ID3D12PipelineState*>(k)->GetDevice(IID_PPV_ARGS(&dev)))) return false;
    const Pipe pipe = gfx ? kGraphics : kCompute;
    for (uint32_t combo = t.kind == kTComp ? 0u : 1u; combo < m.nclone; ++combo) {
      if (t.kind == kTPatch) {
        const uint32_t part = pipe == kCompute ? combo : (combo / m.stride[at]) % (g_p[t.idx].n + 1);
        if (part != v + 1) continue;
      }
      uint32_t e[kMaxStages] = {};
      ID3D12PipelineState* pso = nullptr;
      const HRESULT hr = create_combo(pipe, dev, m.desc->desc(), m, combo, e, &pso);
      if (FAILED(hr)) { ok = false, r->last_hr = static_cast<uint32_t>(hr), ++r->failed; continue; }
      built.push_back({&m.clone[combo], pso});
    }
  }
  if (dev) dev->Release();
  if (!ok) {
    for (const Built& b : built) b.pso->Release();
    return false;
  }
  for (const Built& b : built) {
    dev_retire(static_cast<ID3D12PipelineState*>(
        InterlockedExchangePointer(reinterpret_cast<void* volatile*>(b.at), b.pso)));
  }
  r->rebuilt = static_cast<uint32_t>(built.size());
  return true;
}
}  // namespace

int32_t dev_reload(const char* stem, int32_t variant, rg_blob b, int32_t blob_stage, DevReload* r) {
  *r = {};
  if (!g_dev || !stem) return RG_E_UNAVAILABLE;
  const Target* hit = nullptr;
  const std::vector<Target> all = targets();
  uint32_t matches = 0;
  for (const Target& t : all)
    if (_stricmp(dev_file_stem(t.name).c_str(), stem) == 0 || _stricmp(hex_of(*t.hash).c_str(), stem) == 0) {
      hit = &t;
      ++matches;
    }
  if (!hit) return RG_E_ARG;
  if (matches > 1) return RG_E_TAKEN;  // two targets share the name / hash: name it by the other one
  const Target t = *hit;
  r->name = t.name;
  r->kind = kTargetLabel[t.kind];
  if (variant < 0 || static_cast<uint32_t>(variant) >= t.variants) return RG_E_FULL;
  if (blob_stage >= 0 && static_cast<uint32_t>(blob_stage) != t.stage) return RG_E_BLOB;
  rg_blob* slot = t.kind == kTPatch ? &g_p[t.idx].v[variant] : t.kind == kTComp ? &g_c[t.idx].cs : &g_dc[t.idx].shader;
  AcquireSRWLockExclusive(&g_dev_lock);
  AcquireSRWLockExclusive(&g_insert_lock);
  const rg_blob old = *slot;
  *slot = b;
  const bool ok = dev_rebuild(t, static_cast<uint32_t>(variant), r);
  if (!ok) *slot = old;  // the engine's next PSOs with this shader keep the working blob too
  ReleaseSRWLockExclusive(&g_insert_lock);
  ReleaseSRWLockExclusive(&g_dev_lock);
  Counters& c = t.kind == kTPatch ? g_p[t.idx].c : t.kind == kTComp ? g_c[t.idx].c : g_dc[t.idx].c;
  if (!ok) {
    c.clone_failed.fetch_add(r->failed);
    c.last_hr.store(r->last_hr);
    return RG_E_COMPILE;
  }
  c.clones.fetch_add(r->rebuilt);
  return 0;
}

void dev_frame_tick() {
  if (g_dev_retired.empty()) return;
  size_t keep = 0;
  for (auto& [p, frame] : g_dev_retired) {
    if (g_sweep_frame - frame >= kDevRetireFrames) p->Release();
    else g_dev_retired[keep++] = {p, frame};
  }
  g_dev_retired.resize(keep);
}

void bound_subs_reset() { t_bound_subs = 0; }
uint32_t bound_subs_take() {
  const uint32_t m = t_bound_subs;
  t_bound_subs = 0;
  return m;
}

}  // namespace core::pso
