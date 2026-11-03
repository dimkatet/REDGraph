// M3 (docs/internals.md "RT libraries (M3)"): the engine bakes a client's DXIL library into its own RT state
// object. One detour on the state-object creator (2.31 0x140315f68; its only caller is the StateObject
// ResourceContainer's Create, reached from CRenderInterface::InitDevice and lazily from
// CRenderAccelerationStructure::PrepareAccelerationStructures).
//
// Its input (feasibility 0.7b): a u32 blob id into the GpuApi blob container. The blob is a "CPOS" wrapper (the bytes
// of staticshader_final.cache): header 0x1c, UTF-16 export names at [+0x10], the DXIL library from [+0x14] to the end.
// The creator reads the DXIL bytes in ONE place (BlobView::Range at 0x140315fd9) into the DXIL-library subobject it
// appends at 0x140316143; the export names stay the engine's. So SWAP (2026-10-10) = that one subobject replaced by
// ours (NumExports 0, = export all) in the subobject-append hook, on this thread, for this creation - the shared
// container entry is never written and no creation waits for another. Until 2026-10-10 the swap rewrote the entry for
// the length of the call under an exclusive lock, which serialized every state-object build in the game behind the
// driver's DXR compile (tens of seconds on a cold driver cache). If the creation fails with ours (CheckHR returns
// false on E_INVALIDARG, no crash), it is called again with the engine's.
//
// M12 (2026-10-08, docs/internals.md "RT per node"): RAYGEN VARIANTS. A baked library may carry an extra raygen
// export; the engine adds libraries with no export list, so it is in the state object.
//
// ADD (2026-10-08, docs/internals.md "ADD instead of swap"): a library WITH variants is never swapped in - the
// engine's own library stays byte-identical, and ours goes in NEXT TO it as a second DXIL-library subobject that
// exports ONLY the variants (D3D12_DXIL_LIBRARY_DESC.pExports). A hook on the creator's subobject append (0x140316398)
// adds it right after the engine's library. Several clients may sit on one engine library (each adds its own
// exports), and their blobs carry only the variants (sdk/tools/rt_raygen_variant.py --only / --strip) - such a blob
// cannot stand in for the engine's library. A library WITHOUT variants is swapped in (M3; one per engine library, the
// highest priority) by the same hook - swap and add are one mechanism: the host edits the creator's subobject list.
// The ONE hook on the global
// DispatchRays (0x142934058 - also the census's rows, graph_probe::on_dispatch_rays) finds the dispatch's state object,
// the library baked into it and its raygen group (a4 -> the entry's names); for a variant of that raygen, on and in
// scope, it arms a thread-local override for the length of the call, and the hook on GetShaderGroupIdentifier - which
// GpuApi::DispatchRays calls for exactly that raygen record - returns the variant's identifier instead.

#include <Windows.h>
#include <d3d12.h>
#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "core/bvh_probe.hpp"
#include "core/engine_231.hpp"
#include "core/clients.hpp"
#include "core/crash_log.hpp"
#include "core/gpu_timing.hpp"
#include "core/internal.hpp"
#include "core/prof.hpp"
#include "dxil_hash.hpp"

#include <RED4ext/CName.hpp>

namespace core::rtlib {
namespace {

using namespace e231;

struct BlobEntry {
  const uint8_t* ptr;
  uint32_t size;
};

constexpr uint32_t kMax = 64;
struct Lib {
  const char* name;
  dxil_hash::Hash hash;
  rg_blob lib;
  int32_t priority;
  bool refused;
  std::atomic<uint32_t> swapped{0}, added{0}, failed{0};  // state objects, rg_rtlib_stats
};
Lib g_libs[kMax];
std::atomic<uint32_t> g_n{0};

using Create_t = bool (*)(void* out, const uint32_t* in);
Create_t g_orig = nullptr;
bool g_attached = false;
// The engine creates state objects on many threads at once, and one blob id goes into several of them. Nothing here
// writes the shared container entry (swap and add both work on the creating thread's own subobject list), so a
// creation reads the engine's bytes untouched and needs no lock. (0.8.0 rewrote the entry unlocked: a creation beside
// a bake hashed OUR library and refused the RT leases; 0.8.1 locked it - exclusively, across the driver's compile.)
std::atomic<uint32_t> g_calls{0};
std::atomic<uint64_t> g_baked{0};  // bit (handle - 1): baked into at least one state object

// M12: container entry -> the libraries baked / added into it (bit = handle - 1). A re-creation overwrites it.
constexpr uint32_t kMaxEntries = 512;
struct BakedAt {
  const void* entry;
  uint64_t libs;
};
BakedAt g_at[kMaxEntries];
uint32_t g_nat = 0;
SRWLOCK g_at_lock = SRWLOCK_INIT;
thread_local uint64_t t_baked = 0;  // create() -> d_create: what this creation baked / added

// ADD: up to kMaxAdd libraries of ours next to the engine's in one creation, each exporting only its variants.
using SubAdd_t = void (*)(uint8_t* arr, const D3D12_STATE_SUBOBJECT* sub);
SubAdd_t g_orig_sub = nullptr;
bool g_sub_attached = false;
constexpr uint32_t kMaxAdd = 8, kMaxExports = 8;
struct AddLib {
  uint32_t lib;
  D3D12_EXPORT_DESC exports[kMaxExports];
  D3D12_DXIL_LIBRARY_DESC desc;  // must live until CreateStateObject returns - the creator runs on this thread
};
// HIT GROUPS (2026-11-01): a client's closest-hit / any-hit in every engine COLLECTION with its payload size. The
// creator builds a collection from one CPOS blob (the engine's library + one TRIANGLES hit group per hit-group export);
// ours go in after the engine's last hit group: our library (only our exports, renamed with the collection's tag - a
// pipeline links several collections, and an export name is one function in it) and our hit group. NO associations:
// our exports take the creator's shader config and root signatures by default, as the engine's own do - a subobject
// associated explicitly is no default any more (2026-11-01: the shader config associated with ours -> the debug
// layer's "Export chs_main is missing a required subobject association of type RAYTRACING_SHADER_CONFIG"; the root
// signatures associated with ours -> creation fine, but the RT reflections hung the GPU at once, three runs).
constexpr uint32_t kMaxHg = 8;
struct HitGroup {
  char name[48];
  uint32_t payload, flags;
  wchar_t chs[64], ahs[64];  // the library's export names, L"" = none
  rg_blob lib;
  std::atomic<uint32_t> collections{0}, failed{0}, pipelines{0}, mismatches{0}, uncovered{0};
};
HitGroup g_hg[kMaxHg];
std::atomic<uint32_t> g_nhg{0};
std::atomic<uint32_t> g_hg_refused{0};  // bit = g_hg index: a pipeline failed linking it - not added any more
std::atomic<uint32_t> g_lss{0}, g_pso_calls{0};  // the LSS pipeline option took; the engine set its options
struct HgAdd {
  uint32_t hg;
  wchar_t group[64], chs[64], ahs[64];  // the export names in this collection
  D3D12_EXPORT_DESC exports[2];
  D3D12_DXIL_LIBRARY_DESC lib;
  D3D12_HIT_GROUP_DESC desc;
};

// One creation's edits of the subobject list: `swap` replaces the engine's library, `libs` go in after it, then the
// hit groups `hgs` (a collection only).
struct AddCtx {
  bool armed = false;
  uint32_t n = 0;
  bool seen = false;
  uint32_t added = 0;  // bit i = libs[i] went in
  int32_t swap = -1;   // a library in place of the engine's, -1 none
  bool swapped = false;
  D3D12_DXIL_LIBRARY_DESC swap_desc{};  // must live until CreateStateObject returns - the creator runs on this thread
  AddLib libs[kMaxAdd];
  uint32_t nhg = 0;
  uint32_t hg_added = 0;  // bit i = hgs[i] went in
  uint32_t hg_wait = 0;   // the engine's own hit groups still to come - ours go in after its last
  uint32_t reserve = 0;   // subobjects the creator appends after them (linked collections)
  HgAdd hgs[kMaxHg];
};
thread_local AddCtx t_add;
uint32_t variant_exports(uint32_t lib, D3D12_EXPORT_DESC* out, uint32_t max);  // below, with the variants
std::atomic<uint64_t> g_said_dup{0}, g_said_noswap{0};

void append_hit_groups(uint8_t* arr);

void d_sub(uint8_t* arr, const D3D12_STATE_SUBOBJECT* sub) {
  AddCtx& c = t_add;
  // Our hit groups go in after the ENGINE's last: its own subobject order stays as it was.
  if (c.armed && c.seen && c.hg_wait && sub && sub->Type == D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP) {
    g_orig_sub(arr, sub);
    if (--c.hg_wait == 0) append_hit_groups(arr);
    return;
  }
  const bool engine_lib = c.armed && !c.seen && sub && sub->Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
  if (!engine_lib) {
    g_orig_sub(arr, sub);
    return;
  }
  c.seen = true;  // the engine's library (the creator's first): swapped for ours, or ours go in right after it
  if (c.swap >= 0) {
    const D3D12_STATE_SUBOBJECT ours = {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &c.swap_desc};
    g_orig_sub(arr, &ours);  // copies the 16 bytes
    c.swapped = true;
  } else {
    g_orig_sub(arr, sub);
  }
  for (uint32_t i = 0; i < c.n; ++i) {
    uint32_t count = 0;
    std::memcpy(&count, arr + kSubobjectCount, 4);
    if (count >= kSubobjectMax) break;  // the engine's array has no bounds check
    const D3D12_STATE_SUBOBJECT ours = {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &c.libs[i].desc};
    g_orig_sub(arr, &ours);  // copies the 16 bytes
    c.added |= 1u << i;
  }
  if (c.nhg && !c.hg_wait) append_hit_groups(arr);  // a collection without hit groups of its own
}

void append_hit_groups(uint8_t* arr) {
  AddCtx& c = t_add;
  for (uint32_t i = 0; i < c.nhg; ++i) {
    uint32_t count = 0;
    std::memcpy(&count, arr + kSubobjectCount, 4);
    if (count + 2 + c.reserve > kSubobjectMax) break;  // the creator's own appends after ours must still fit
    HgAdd& a = c.hgs[i];
    const D3D12_STATE_SUBOBJECT lib = {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &a.lib};
    const D3D12_STATE_SUBOBJECT group = {D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &a.desc};
    g_orig_sub(arr, &lib);
    g_orig_sub(arr, &group);
    c.hg_added |= 1u << i;
  }
}

// [dev] d3d12_debug: the runtime's errors of a failed creation (the debug layer's info queue) into the log.
std::atomic<bool> g_debug_layer{false};
ID3D12InfoQueue* info_queue() {
  if (!g_debug_layer.load(std::memory_order_relaxed)) return nullptr;
  const auto* data = *reinterpret_cast<const uint8_t* const*>(g_base + kDeviceData);
  auto* dev = data ? *reinterpret_cast<IUnknown* const*>(data + kDeviceInData) : nullptr;
  ID3D12InfoQueue* q = nullptr;
  return dev && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&q))) ? q : nullptr;
}
void d3d12_messages(uint32_t n) {
  ID3D12InfoQueue* q = info_queue();
  if (!q) return;
  const UINT64 k = q->GetNumStoredMessages();
  uint32_t said = 0;
  for (UINT64 i = 0; i < k && said < 16; ++i) {
    SIZE_T len = 0;
    if (FAILED(q->GetMessage(i, nullptr, &len)) || !len) continue;
    std::vector<uint8_t> b(len);
    auto* m = reinterpret_cast<D3D12_MESSAGE*>(b.data());
    if (FAILED(q->GetMessage(i, m, &len)) || m->Severity > D3D12_MESSAGE_SEVERITY_ERROR) continue;
    g_sdk->logger->ErrorF(g_handle, "d3d12 (state object #%u): %.1800s", n, m->pDescription);
    ++said;
  }
  q->ClearStoredMessages();
  q->Release();
}

// Arm the hit groups of `mask` for one collection creation, named with the collection's tag.
void arm_hit_groups(uint32_t mask, uint32_t tag) {
  AddCtx& c = t_add;
  c.nhg = 0;
  const uint32_t n = g_nhg.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n && c.nhg < kMaxHg; ++i) {
    if (!(mask >> i & 1)) continue;
    const HitGroup& g = g_hg[i];
    HgAdd& a = c.hgs[c.nhg++];
    a.hg = i;
    std::swprintf(a.group, 64, L"%hs_%08x", g.name, tag);
    a.chs[0] = a.ahs[0] = 0;
    uint32_t ne = 0;
    if (g.chs[0]) {
      std::swprintf(a.chs, 64, L"%ls_%08x", g.chs, tag);
      a.exports[ne++] = {a.chs, g.chs, D3D12_EXPORT_FLAG_NONE};
    }
    if (g.ahs[0]) {
      std::swprintf(a.ahs, 64, L"%ls_%08x", g.ahs, tag);
      a.exports[ne++] = {a.ahs, g.ahs, D3D12_EXPORT_FLAG_NONE};
    }
    a.lib = {};
    a.lib.DXILLibrary = {g.lib.bytes, g.lib.size};
    a.lib.NumExports = ne;
    a.lib.pExports = a.exports;
    a.desc = {};
    a.desc.HitGroupExport = a.group;
    a.desc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;  // LSS: the built-in intersection, as for triangles
    a.desc.ClosestHitShaderImport = a.chs[0] ? a.chs : nullptr;
    a.desc.AnyHitShaderImport = a.ahs[0] ? a.ahs : nullptr;
  }
}

uint32_t hit_groups_for(uint32_t payload) {
  uint32_t m = 0;
  const uint32_t n = g_nhg.load(std::memory_order_acquire);
  const uint32_t refused = g_hg_refused.load(std::memory_order_relaxed);
  for (uint32_t i = 0; i < n; ++i)
    if (g_hg[i].payload == payload && !(refused >> i & 1)) m |= 1u << i;
  return m;
}

// The collections that hold hit groups of ours: which of ours, the engine's hit-group names (for the identifier map at
// each pipeline that links them), and how the collection is known again.
struct Coll {
  uint32_t tag, hgs, payload;
  dxil_hash::Hash hash;  // the collection's own DXIL hash, ALL 16 bytes: the tag is its first 4 only, and two
                         // collections whose tags collide would otherwise be ONE entry (their groups merged, one slot
                         // rebuilt for the other) - 2026-11-03
  std::vector<std::wstring> groups;
  void* slot;     // the creator's `out` (the state-object container's entry) and blob id - to rebuild it vanilla
  uint32_t blob;
};
std::vector<Coll> g_colls;
SRWLOCK g_colls_lock = SRWLOCK_INIT;
std::atomic<uint32_t> g_ncolls{0};
// engine hit-group identifier -> ours from the same collection, per hit group of ours
struct IdPair {
  uint32_t hg, tag;
  uint8_t eng[32], ours[32];
};
constexpr uint32_t kMaxIdPairs = 512;
IdPair g_pairs[kMaxIdPairs];
uint32_t g_npairs = 0;
SRWLOCK g_pairs_lock = SRWLOCK_INIT;
std::atomic<uint32_t> g_pipelines_mapped{0};
std::atomic<bool> g_pairs_full{false};  // kMaxIdPairs reached (logged once)

// EVERY collection created while a hit group is registered: its state-object slot and payload - with g_colls (the
// ones holding ours), what tells a pipeline that links a payload-N collection without our group (g_colls_lock).
struct CollPayload {
  void* slot;
  uint32_t payload;
};
std::vector<CollPayload> g_coll_payload;
std::atomic<uint32_t> g_uncovered_said{0};

void note_collection_payload(void* slot, uint32_t payload) {
  AcquireSRWLockExclusive(&g_colls_lock);
  bool found = false;
  for (CollPayload& c : g_coll_payload)
    if (c.slot == slot) c.payload = payload, found = true;
  if (!found) g_coll_payload.push_back({slot, payload});
  ReleaseSRWLockExclusive(&g_colls_lock);
}

// After a PIPELINE creation: every collection it links whose payload is a hit group's, without that group in it -
// counted per group (rg_rt_hit_group_stats.uncovered) and logged for the first few. A ray of that pipeline that hits a
// record holding the group's identifier runs nothing of it (the identifier is not in the pipeline).
void check_coverage(const uint32_t* in, uint32_t n) {
  const uint32_t nhg = g_nhg.load(std::memory_order_acquire);
  if (!nhg || !in) return;
  const auto* v = reinterpret_cast<const uint8_t*>(in) + 8;
  const uint32_t *b = nullptr, *end = nullptr;
  std::memcpy(&b, v, 8);
  std::memcpy(&end, v + 8, 8);
  auto* dev = *reinterpret_cast<uint8_t* const*>(g_base + kDeviceData);
  if (!dev || !b || end <= b) return;
  uint32_t missing = 0;  // bit = hit group: some linked collection of its payload lacks it
  const uint32_t refused = g_hg_refused.load(std::memory_order_relaxed);  // those are logged as refused already
  AcquireSRWLockShared(&g_colls_lock);
  for (const uint32_t* id = b; id < end; ++id) {
    if (*id == 0) continue;
    const void* slot = dev + kRtStateObjects + size_t(*id - 1) * kRtStateObjectStride;
    uint32_t payload = 0;
    bool known = false;
    for (const CollPayload& c : g_coll_payload)
      if (c.slot == slot) payload = c.payload, known = true;
    if (!known) continue;  // created before any group was registered - cannot happen after the registration window
    uint32_t ours = 0;
    for (const Coll& c : g_colls)
      if (c.slot == slot) ours |= c.hgs;
    for (uint32_t i = 0; i < nhg; ++i)
      if (g_hg[i].payload == payload && !(ours >> i & 1) && !(refused >> i & 1)) missing |= 1u << i;
  }
  ReleaseSRWLockShared(&g_colls_lock);
  for (uint32_t i = 0; i < nhg; ++i) {
    if (!(missing >> i & 1)) continue;
    g_hg[i].uncovered.fetch_add(1, std::memory_order_relaxed);
    if (g_uncovered_said.fetch_add(1, std::memory_order_relaxed) < 8)
      g_sdk->logger->WarnF(g_handle, "rt hit group: '%s' - state object #%u (a pipeline) links a %u-byte payload "
                           "collection WITHOUT it: its rays run no shader of the group (counted in `uncovered`)",
                           g_hg[i].name, n, g_hg[i].payload);
  }
}

// The CPOS export list: a hit-group export is three names (closest-hit, any-hit, the group), the others one.
std::vector<std::wstring> hit_group_names(const BlobEntry* e) {
  std::vector<std::wstring> out;
  const uint8_t* p = e->ptr;
  uint32_t q = 0, end = 0;
  uint64_t mask = 0;
  std::memcpy(&q, p + kCposNames, 4);
  std::memcpy(&end, p + 0x14, 4);
  std::memcpy(&mask, p + kCposHitGroupMask, 8);
  auto next = [&] {
    std::wstring s;
    for (; q + 1 < end; ) {
      const wchar_t ch = static_cast<wchar_t>(p[q] | (p[q + 1] << 8));
      q += 2;
      if (!ch) break;
      s += ch;
    }
    return s;
  };
  for (uint32_t i = 0; i < p[kCposExports] && q + 1 < end; ++i) {
    if (i < 64 && (mask >> i & 1)) {
      next();
      next();
      out.push_back(next());
    } else {
      next();
    }
  }
  return out;
}

// After a collection creation with hit groups of ours in it. Returns true for a collection not seen before.
bool note_collection(const BlobEntry* e, uint32_t tag, const uint8_t hash16[16], uint32_t hgs, void* slot,
                     uint32_t blob) {
  std::vector<std::wstring> groups = hit_group_names(e);
  dxil_hash::Hash hash{};
  std::memcpy(hash.data(), hash16, hash.size());
  AcquireSRWLockExclusive(&g_colls_lock);
  bool fresh = true;
  for (Coll& c : g_colls)
    if (c.hash == hash) {  // the WHOLE hash: the tag (its first 4 bytes) is a name suffix, not an identity
      c.hgs |= hgs;
      c.slot = slot;
      c.blob = blob;
      fresh = false;
    }
  if (fresh) g_colls.push_back({tag, hgs, e->ptr[kCposPayload], hash, std::move(groups), slot, blob});
  g_ncolls.store(static_cast<uint32_t>(g_colls.size()), std::memory_order_release);
  ReleaseSRWLockExclusive(&g_colls_lock);
  return fresh;
}

BlobEntry* entry_of(uint32_t id);  // below (the GpuApi blob container); the rebuild checks its blob here first

// A PIPELINE creation that failed: if it links a collection with hit groups of ours, that collection is rebuilt
// without them (in its own slot, as the engine would re-create it) and those hit groups are refused from then on.
// Returns the collections rebuilt - the caller tries the pipeline again.
//
// 2026-11-03, three things this got wrong (docs/internals.md "Known gaps" 11, item 2):
//   * the engine's creator runs OUTSIDE g_colls_lock: it is a driver compile (seconds), and check_coverage /
//     map_identifiers / note_* take that lock on every other pipeline - so the pipeline retried HERE would block every
//     state object of the session behind it. The lock is now held only to pick the victims and to record the outcome.
//   * the rebuilt collection is written into the slot's StateObjectData (device data + kRtStateObjects, +0 = a ComPtr
//     the slot OWNS). The creator stores its own object without releasing the old one, so the old ID3D12StateObject (M
//     of VRAM) leaked per rebuild - read the owner off the slot and release it once the creator replaced it.
//   * the input is hand-built: the creator's shape is `{u32 blob; u32 pad; u32 vector of the LINKED COLLECTIONS at +8}`
//     (kCposCollection's note, engine_231.hpp; a pipeline passes its collections there, a collection none). A zeroed
//     vector is exactly a collection's, but the blob must really BE a collection - a pipeline-shaped blob would build a
//     pipeline with no linked collections here. It is checked against the container entry first.
uint32_t unlink_ours(const uint32_t* in, uint32_t n) {
  if (!g_ncolls.load(std::memory_order_acquire) || !in) return 0;
  const auto* v = reinterpret_cast<const uint8_t*>(in) + 8;
  const uint32_t *b = nullptr, *end = nullptr;
  std::memcpy(&b, v, 8);
  std::memcpy(&end, v + 8, 8);
  auto* dev = *reinterpret_cast<uint8_t* const*>(g_base + kDeviceData);
  if (!dev || !b || end <= b) return 0;
  uint32_t rebuilt = 0;
  for (const uint32_t* id = b; id < end; ++id) {
    if (*id == 0) continue;
    void* slot = dev + kRtStateObjects + size_t(*id - 1) * kRtStateObjectStride;
    // What to rebuild, picked under the lock; the creator then runs with NO lock held.
    struct Victim {
      uint32_t tag, blob;
      void* slot;
    };
    Victim pick[8];
    uint32_t npick = 0;
    uint32_t refused_hgs = 0;
    AcquireSRWLockExclusive(&g_colls_lock);
    for (size_t k = 0; k < g_colls.size(); ++k) {
      Coll& c = g_colls[k];
      if (c.slot != slot || !c.hgs || npick == 8) continue;
      refused_hgs |= c.hgs;
      pick[npick++] = {c.tag, c.blob, c.slot};
      for (uint32_t i = 0; i < kMaxHg; ++i)
        if (c.hgs >> i & 1) clients::refused(clients::kRtHitGroup, static_cast<rg_handle>(i + 1), "an RT pipeline "
                                             "failed linking it");
      c.hgs = 0;      // no other pipeline rebuilds it again in the meantime
      c.slot = nullptr;
    }
    ReleaseSRWLockExclusive(&g_colls_lock);
    if (!npick) continue;
    g_hg_refused.fetch_or(refused_hgs);
    for (uint32_t k = 0; k < npick; ++k) {
      const Victim& c = pick[k];
      const BlobEntry* e = entry_of(c.blob);
      const bool is_collection = e && e->ptr && e->size >= 0x1c && e->ptr[kCposCollection] != 0;
      void* old = nullptr;  // the slot's own ID3D12StateObject: the creator replaces it without a Release
      __try {
        old = *static_cast<void**>(c.slot);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        old = nullptr;
      }
      bool ok = false;
      if (!is_collection) {
        g_sdk->logger->ErrorF(g_handle, "rt hit group: state object #%u (a pipeline) FAILED linking collection %08x with "
                              "our hit groups - its blob is not a collection any more, NOT rebuilt (%u refused)",
                              n, c.tag, refused_hgs);
      } else {
        struct {
          uint32_t blob, pad;
          uint64_t begin, end, cap;  // no linked collections: a collection's input, as the engine builds it
        } vin = {c.blob, 0, 0, 0, 0};
        AddCtx& cx = t_add;  // this thread may be inside an armed creation: the rebuild is the engine's alone
        const bool armed = cx.armed;
        cx.armed = false;
        ok = g_orig(c.slot, &vin.blob);
        cx.armed = armed;
        g_sdk->logger->ErrorF(g_handle, "rt hit group: state object #%u (a pipeline) FAILED linking collection %08x with "
                              "our hit groups - the collection rebuilt without them (%s), they are refused from now on",
                              n, c.tag, ok ? "ok" : "FAILED too");
      }
      if (ok && old) {
        __try {
          static_cast<IUnknown*>(old)->Release();  // the slot owns one reference; the creator stored another object
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
      }
      ++rebuilt;
    }
    AcquireSRWLockExclusive(&g_pairs_lock);
    for (uint32_t k = 0; k < npick; ++k)
      for (uint32_t p = 0; p < g_npairs;)
        if (g_pairs[p].tag == pick[k].tag) g_pairs[p] = g_pairs[--g_npairs];
        else ++p;
    ReleaseSRWLockExclusive(&g_pairs_lock);
  }
  return rebuilt;
}

// The engine's creator; a pipeline that fails on a collection of ours is tried again without our hit groups.
bool orig_create(void* out, const uint32_t* in, uint32_t n) {
  if (g_orig(out, in)) return true;
  if (!unlink_ours(in, n)) return false;
  AddCtx& c = t_add;  // an armed creation adds its libraries again
  c.seen = c.swapped = false;
  c.added = c.hg_added = 0;
  return g_orig(out, in);
}

// After a PIPELINE creation: for every collection of ours it links (our group's name resolves), every engine hit group
// of that collection -> our group's identifier. A collection's identifiers are the same in every pipeline linking it
// (the engine's own tables rely on that) - a different one for a known engine identifier is counted and logged.
void map_identifiers(void* out, uint32_t n) {
  auto* so = out ? *reinterpret_cast<IUnknown**>(out) : nullptr;
  if (!so) return;
  ID3D12StateObjectProperties* props = nullptr;
  if (FAILED(so->QueryInterface(IID_PPV_ARGS(&props))) || !props) return;
  AcquireSRWLockShared(&g_colls_lock);
  const std::vector<Coll> colls = g_colls;
  ReleaseSRWLockShared(&g_colls_lock);
  uint32_t mapped = 0, linked = 0;
  for (const Coll& c : colls)
    for (uint32_t i = 0; i < g_nhg.load(std::memory_order_acquire); ++i) {
      if (!(c.hgs >> i & 1)) continue;
      wchar_t name[64];
      std::swprintf(name, 64, L"%hs_%08x", g_hg[i].name, c.tag);
      const void* ours = props->GetShaderIdentifier(name);
      if (!ours) continue;
      ++linked;
      g_hg[i].pipelines.fetch_add(1, std::memory_order_relaxed);
      for (const std::wstring& gname : c.groups) {
        const void* eng = gname.empty() ? nullptr : props->GetShaderIdentifier(gname.c_str());
        if (!eng) continue;
        AcquireSRWLockExclusive(&g_pairs_lock);
        uint32_t k = 0;
        while (k < g_npairs && !(g_pairs[k].hg == i && std::memcmp(g_pairs[k].eng, eng, 32) == 0)) ++k;
        if (k < g_npairs) {
          if (std::memcmp(g_pairs[k].ours, ours, 32) != 0 && g_hg[i].mismatches.fetch_add(1) < 4)
            g_sdk->logger->WarnF(g_handle, "rt hit group: '%s' - state object #%u gives collection %08x's identifiers "
                                 "anew (engine group '%ls'): the tables may hold another pipeline's", g_hg[i].name, n,
                                 c.tag, gname.c_str());
        } else if (g_npairs < kMaxIdPairs) {
          g_pairs[g_npairs].hg = i;
          g_pairs[g_npairs].tag = c.tag;
          std::memcpy(g_pairs[g_npairs].eng, eng, 32);
          std::memcpy(g_pairs[g_npairs].ours, ours, 32);
          if (g_npairs < 32 || verbose()) {
            uint64_t e8 = 0, o8 = 0, e8b = 0, o8b = 0;
            std::memcpy(&e8, eng, 8), std::memcpy(&e8b, static_cast<const uint8_t*>(eng) + 8, 8);
            std::memcpy(&o8, ours, 8), std::memcpy(&o8b, static_cast<const uint8_t*>(ours) + 8, 8);
            g_sdk->logger->InfoF(g_handle, "rt hit group: collection %08x '%ls' %016llx %016llx.. -> ours %016llx "
                                 "%016llx..", c.tag, gname.c_str(), static_cast<unsigned long long>(e8),
                                 static_cast<unsigned long long>(e8b), static_cast<unsigned long long>(o8),
                                 static_cast<unsigned long long>(o8b));
          }
          ++g_npairs;
          ++mapped;
        } else if (!g_pairs_full.exchange(true)) {
          // the identifier of this engine group is never ours: rt_hit_group_identifier answers UNAVAILABLE for it
          g_sdk->logger->ErrorF(g_handle, "rt hit group: '%s' - the identifier map is FULL (%u pairs, kMaxIdPairs): "
                                "engine group '%ls' of collection %08x and every later one stay unmapped",
                                g_hg[i].name, kMaxIdPairs, gname.c_str(), c.tag);
        }
        ReleaseSRWLockExclusive(&g_pairs_lock);
      }
    }
  props->Release();
  if (linked && (g_pipelines_mapped.fetch_add(1) == 0 || mapped))
    g_sdk->logger->InfoF(g_handle, "rt hit group: state object #%u links %u collection(s) of ours - %u engine hit "
                         "group(s) newly mapped to our identifiers", n, linked, mapped);
}

// Arm the libraries of `mask` for one creation: export lists, a name already exported by an earlier one left out.
void arm_add(uint64_t mask) {
  AddCtx& c = t_add;
  const uint32_t nlib = g_n.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < nlib && c.n < kMaxAdd; ++i) {
    if (!(mask >> i & 1)) continue;
    AddLib& a = c.libs[c.n];
    D3D12_EXPORT_DESC all[kMaxExports];
    const uint32_t k = variant_exports(i, all, kMaxExports);
    uint32_t m = 0;
    for (uint32_t e = 0; e < k; ++e) {
      bool dup = false;
      for (uint32_t j = 0; j < c.n && !dup; ++j)
        for (uint32_t x = 0; x < c.libs[j].desc.NumExports && !dup; ++x)
          dup = std::wcscmp(c.libs[j].exports[x].Name, all[e].Name) == 0;
      if (!dup) {
        a.exports[m++] = all[e];
      } else if (!(g_said_dup.fetch_or(1ull << i) >> i & 1)) {
        g_sdk->logger->WarnF(g_handle, "rt swap: %s - a variant export name is already taken by another library in the "
                             "same state object: left out (logged once)", g_libs[i].name);
      }
    }
    if (!m) continue;
    a.lib = i;
    a.desc = {};
    a.desc.DXILLibrary = {g_libs[i].lib.bytes, g_libs[i].lib.size};
    a.desc.NumExports = m;
    a.desc.pExports = a.exports;
    ++c.n;
  }
}

void note_entry(const void* entry, uint64_t libs) {
  AcquireSRWLockExclusive(&g_at_lock);
  uint32_t i = 0;
  while (i < g_nat && g_at[i].entry != entry) ++i;
  if (i < g_nat) g_at[i].libs = libs;
  else if (libs && g_nat < kMaxEntries) g_at[g_nat++] = {entry, libs};
  ReleaseSRWLockExclusive(&g_at_lock);
}

uint64_t libs_at(const void* entry) {
  uint64_t libs = 0;
  AcquireSRWLockShared(&g_at_lock);
  for (uint32_t i = 0; i < g_nat; ++i)
    if (g_at[i].entry == entry) {
      libs = g_at[i].libs;
      break;
    }
  ReleaseSRWLockShared(&g_at_lock);
  return libs;
}

BlobEntry* entry_of(uint32_t id) {
  const auto data = *reinterpret_cast<uint8_t* const*>(g_base + kDeviceData);
  if (!data || id == 0 || id > 0x7fffffff) return nullptr;
  return reinterpret_cast<BlobEntry*>(data + kBlobContainer + 0x18 + static_cast<uintptr_t>(id - 1) * 32);
}

// The first export name (UTF-16 at [+0x10]) as ASCII, for the log.
void first_export(const BlobEntry* e, char* out, size_t cap) {
  out[0] = 0;
  uint32_t names = 0;
  std::memcpy(&names, e->ptr + 0x10, 4);
  for (size_t i = 0; i + 1 < cap && names + 2 * i + 1 < e->size; ++i) {
    const uint16_t c = static_cast<uint16_t>(e->ptr[names + 2 * i] | (e->ptr[names + 2 * i + 1] << 8));
    if (c == 0) break;
    out[i] = c < 0x80 ? static_cast<char>(c) : '?';
    out[i + 1] = 0;
  }
}

// Classify the creation and run every one that is not ours. Returns 0 / -1 (passed through,
// succeeded / failed) or 1 with `mask` = every registered library on this hash whose leases stand and `hgs` = the hit
// groups for this collection (`tag` its name suffix).
int create_shared(void* out, const uint32_t* in, uint32_t n, uint64_t* mask, uint32_t* hgs, uint32_t* tag,
                  uint8_t hash16[16]) {
  const auto pass = [&] { return orig_create(out, in, n) ? 0 : -1; };
  BlobEntry* e = in ? entry_of(*in) : nullptr;
  uint32_t magic = 0, dxil_off = 0;
  if (e && e->ptr && e->size >= 0x1c) {
    std::memcpy(&magic, e->ptr, 4);
    std::memcpy(&dxil_off, e->ptr + 0x14, 4);
  }
  if (magic != kCposMagic || e->ptr[4] != 0 || dxil_off < 0x1c || dxil_off >= e->size) {
    g_sdk->logger->WarnF(g_handle, "rt swap: state object #%u - blob id %u not a CPOS v0 wrapper, passed through", n,
                         in ? *in : 0u);
    return pass();
  }
  const auto h = dxil_hash::of(e->ptr + dxil_off, e->size - dxil_off);
  char name[64];
  first_export(e, name, sizeof(name));
  const uint32_t nlib = g_n.load(std::memory_order_acquire);
  uint64_t m = 0;
  if (h)
    for (uint32_t i = 0; i < nlib; ++i)
      if (!g_libs[i].refused && *h == g_libs[i].hash) m |= 1ull << i;
  // Every creation, with VerboseLog: a few dozen per load - the census of what the engine builds, the evidence that
  // this function is the funnel.
  const uint8_t* hb = h ? h->data() : nullptr;
  if (verbose())
    g_sdk->logger->InfoF(g_handle, "rt swap: state object #%u blob %u %s %02x%02x%02x%02x '%s' (%s, %u exports)%s", n, *in,
                       hb ? "lib" : "lib(unhashed)", hb ? hb[0] : 0, hb ? hb[1] : 0, hb ? hb[2] : 0, hb ? hb[3] : 0,
                       name, e->ptr[5] ? "collection" : "pipeline", e->ptr[6], m ? " - TARGET" : "");
  // Every library the engine creates - ours' vanilla original included - against the RT leases (none may declare
  // their registers: they are staged at every DispatchRays).
  bindings::on_rt_library(e->ptr + dxil_off, e->size - dxil_off, name);
  for (uint32_t i = 0; i < nlib; ++i)
    if ((m >> i & 1) && !bindings::rtlib_may_bake(static_cast<rg_handle>(i + 1))) {
      m &= ~(1ull << i);
      g_sdk->logger->WarnF(g_handle, "rt swap: %s NOT baked - a lease of it is refused; state object #%u without it",
                           g_libs[i].name, n);
    }
  const uint32_t hg = h && e->ptr[kCposCollection] ? hit_groups_for(e->ptr[kCposPayload]) : 0;
  if (!m && !hg) return pass();
  *mask = m;
  *hgs = hg;
  if (hg) std::memcpy(tag, hb, 4), std::memcpy(hash16, hb, 16);
  return 1;
}

void say_first(uint32_t ti, const char* what, uint32_t n, size_t bytes, uint32_t exports) {
  const bool first = !(g_baked.fetch_or(1ull << ti, std::memory_order_release) & (1ull << ti));
  if (!first && !verbose()) return;  // one library goes into several state objects: the first one says it works
  if (exports)
    g_sdk->logger->InfoF(g_handle, "rt swap: %s %s next to the engine's library in state object #%u (%zu B DXIL, %u "
                         "export(s))", what, g_libs[ti].name, n, bytes, exports);
  else
    g_sdk->logger->InfoF(g_handle, "rt swap: %s %s into state object #%u (%zu B DXIL)", what, g_libs[ti].name, n, bytes);
}

// One creation with `swap` (-1 none) in place of the engine's library and the libraries of `add` next to it - both
// through d_sub on this thread; the engine's container entry is never touched.
bool create_with(void* out, const uint32_t* in, uint32_t n, int32_t swap, uint64_t add, uint32_t hgs, uint32_t tag,
                 const uint8_t hash16[16]) {
  AddCtx& c = t_add;
  c.n = 0;
  c.seen = false;
  c.added = 0;
  c.swap = -1;
  c.swapped = false;
  c.nhg = 0;
  c.hg_added = 0;
  c.hg_wait = 0;
  c.reserve = 0;
  if (add) arm_add(add);
  if (swap >= 0) {
    c.swap = swap;
    c.swap_desc = {};
    c.swap_desc.DXILLibrary = {g_libs[swap].lib.bytes, g_libs[swap].lib.size};  // NumExports 0 = all, as the engine's
  }
  const BlobEntry* e = hgs ? entry_of(*in) : nullptr;
  if (e) {
    arm_hit_groups(hgs, tag);
    // after its library the creator appends its hit groups and the collections it links (a u32 vector at in + 8)
    const auto* v = reinterpret_cast<const uint8_t*>(in) + 8;
    uint64_t b = 0, end = 0;
    std::memcpy(&b, v, 8);
    std::memcpy(&end, v + 8, 8);
    c.hg_wait = e->ptr[kCposHitGroups];
    c.reserve = end > b ? static_cast<uint32_t>((end - b) / 4) : 0u;
  }
  c.armed = c.n || c.swap >= 0 || c.nhg;
  if (ID3D12InfoQueue* q = info_queue()) {  // [dev] d3d12_debug: only this creation's messages
    q->ClearStoredMessages();
    q->Release();
  }
  bool ok = orig_create(out, in, n);
  if (!ok) d3d12_messages(n);
  const uint32_t armed = c.n, added = c.added, nhg = c.nhg, hg_added = c.hg_added;
  const bool seen = c.seen, swapped = c.swapped;
  c.armed = false;
  c.n = 0;
  c.swap = -1;
  c.nhg = 0;
  uint32_t hg_in = 0;  // hit groups (bit = g_hg index) that went into this collection
  for (uint32_t i = 0; i < nhg; ++i)
    if (hg_added >> i & 1) hg_in |= 1u << c.hgs[i].hg;
  if (ok && e) {
    const bool fresh = hg_in && note_collection(e, tag, hash16, hg_in, out, *in);
    for (uint32_t i = 0; i < nhg; ++i) {
      const HitGroup& g = g_hg[c.hgs[i].hg];
      if (!(hg_added >> i & 1)) {
        g_sdk->logger->WarnF(g_handle, "rt hit group: '%s' NOT added to collection %08x (state object #%u: %s)", g.name,
                             tag, n, !seen ? "no DXIL-library subobject seen" : c.hg_wait ? "fewer engine hit groups "
                             "than its header says" : "no room in the engine's subobject array");
        continue;
      }
      g_hg[c.hgs[i].hg].collections.fetch_add(1, std::memory_order_relaxed);
      if (fresh || verbose())
        g_sdk->logger->InfoF(g_handle, "rt hit group: '%ls' ADDED to collection %08x (state object #%u, %u-byte payload, "
                             "%u engine hit groups)%s", c.hgs[i].group, tag, n, e->ptr[kCposPayload],
                             e->ptr[kCposHitGroups], !(g.flags & RG_HIT_GROUP_LSS) ? ""
                             : g_lss.load() ? ", LSS on"
                             : g_pso_calls.load() ? ", LSS REFUSED by the driver"
                             : ", LSS OFF (the engine set no NvAPI pipeline options: OMM off / not NVIDIA)");
    }
  }
  if (!ok && hg_in) {  // a collection that fails with a hit group of ours would fail with it every time
    g_hg_refused.fetch_or(hg_in);
    for (uint32_t i = 0; i < nhg; ++i)
      if (hg_added >> i & 1) {
        g_hg[c.hgs[i].hg].failed.fetch_add(1, std::memory_order_relaxed);
        g_sdk->logger->ErrorF(g_handle, "rt hit group: '%ls' - collection %08x FAILED with it (state object #%u): "
                              "refused from now on", c.hgs[i].group, tag, n);
        clients::refused(clients::kRtHitGroup, static_cast<rg_handle>(c.hgs[i].hg + 1), "an engine collection failed "
                         "with it");
      }
  }
  if (ok && swap >= 0 && !swapped)
    g_sdk->logger->WarnF(g_handle, "rt swap: %s NOT swapped into state object #%u (no DXIL-library subobject seen) - the "
                         "engine's library ran", g_libs[swap].name, n);
  if (ok) {
    t_baked = 0;
    if (swapped) {
      t_baked |= 1ull << swap;
      g_libs[swap].swapped.fetch_add(1, std::memory_order_relaxed);
      say_first(static_cast<uint32_t>(swap), "BAKED", n, g_libs[swap].lib.size, 0);
    }
    for (uint32_t i = 0; i < armed; ++i) {
      const AddLib& a = c.libs[i];
      if (added >> i & 1) {
        t_baked |= 1ull << a.lib;
        g_libs[a.lib].added.fetch_add(1, std::memory_order_relaxed);
        say_first(a.lib, "ADDED", n, g_libs[a.lib].lib.size, a.desc.NumExports);
      } else {
        g_sdk->logger->WarnF(g_handle, "rt swap: %s NOT added to state object #%u (%s)", g_libs[a.lib].name, n,
                             seen ? "the engine's subobject array is full" : "no DXIL-library subobject seen");
      }
    }
    return true;
  }
  g_sdk->logger->ErrorF(g_handle, "rt swap: creation FAILED with our libraries (state object #%u, %u swapped, %u "
                        "added, %u hit groups) - retrying with the engine's alone (vanilla RT)", n, swapped ? 1u : 0u,
                        armed, nhg);
  if (swap >= 0) g_libs[swap].failed.fetch_add(1, std::memory_order_relaxed);
  for (uint32_t i = 0; i < armed; ++i) g_libs[c.libs[i].lib].failed.fetch_add(1, std::memory_order_relaxed);
  ok = orig_create(out, in, n);
  g_sdk->logger->InfoF(g_handle, "rt swap: vanilla retry %s", ok ? "succeeded" : "FAILED too");
  return ok;
}

bool create(void* out, const uint32_t* in) {
  const uint32_t n = g_calls.fetch_add(1) + 1;
  uint64_t mask = 0;
  uint32_t hgs = 0, tag = 0;
  uint8_t hash16[16] = {};
  const int r = create_shared(out, in, n, &mask, &hgs, &tag, hash16);
  if (r <= 0) return r == 0;
  if (!g_sub_attached) {  // swap, add and the hit groups all go through the subobject append
    for (uint32_t i = 0; i < g_n.load(std::memory_order_acquire); ++i)
      if ((mask >> i & 1) && !(g_said_noswap.fetch_or(1ull << i) >> i & 1))
        g_sdk->logger->WarnF(g_handle, "rt swap: %s NOT baked - the subobject hook is not attached (vanilla there)",
                             g_libs[i].name);
    return orig_create(out, in, n);
  }
  // Every library with variants goes in next to the engine's (never swapped: its blob holds only the variants); of
  // the rest the highest priority swaps (ties: the first registered) - one library in place of the engine's.
  uint64_t add = 0;
  int32_t swap = -1;
  const uint32_t nlib = g_n.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < nlib; ++i) {
    if (!(mask >> i & 1)) continue;
    D3D12_EXPORT_DESC probe[kMaxExports];
    if (variant_exports(i, probe, kMaxExports)) add |= 1ull << i;
    else if (swap < 0 || g_libs[i].priority > g_libs[swap].priority) swap = static_cast<int32_t>(i);
  }
  for (uint32_t i = 0; i < nlib; ++i)
    if ((mask >> i & 1) && !(add >> i & 1) && static_cast<int32_t>(i) != swap &&
        !(g_said_noswap.fetch_or(1ull << i) >> i & 1)) {
      g_sdk->logger->WarnF(g_handle, "rt swap: %s NOT baked - '%s' replaces the same engine library (higher priority)",
                           g_libs[i].name, g_libs[swap].name);
      char why[120];
      std::snprintf(why, sizeof why, "'%s' replaces the same engine library (higher priority)", g_libs[swap].name);
      clients::refused(clients::kRtLib, static_cast<rg_handle>(i + 1), why);
    }
  return create_with(out, in, n, swap, add, hgs, tag, hash16);
}

// The census (graph_probe) maps a DispatchRays to the library it runs: every successful creation reports its
// container entry, the library hash and the export names (UTF-16, consecutive, from [+0x10]; count = byte +6).
void forget_slot(const void* slot);  // below, with the identifier cache
void lss_apply_once();               // below, with the NvAPI pipeline options (LSS): once, at the first state object
void note_pipeline_options(const void* out, const uint32_t* in);  // below: the options a pipeline was created under

bool d_create(void* out, const uint32_t* in) {
  t_baked = 0;
  lss_apply_once();  // LSS (RG_HIT_GROUP_LSS): the host's own NvAPI pipeline options, before the first state object
  forget_slot(out);  // `out` is the state-object slot DispatchRays reads (rt_entry): its cached identifiers go
  const bool ok = create(out, in);
  forget_slot(out);  // and anything cached while it was being built
  if (ok) note_entry(out, t_baked);
  if (ok) note_pipeline_options(out, in);
  if (ok && g_nhg.load(std::memory_order_acquire)) {
    const BlobEntry* pe = in ? entry_of(*in) : nullptr;
    if (pe && pe->ptr && pe->size >= 0x1c) {
      if (pe->ptr[kCposCollection]) {
        note_collection_payload(out, pe->ptr[kCposPayload]);
      } else {  // a pipeline: which of our collections it links, and which of its collections lack ours
        if (g_ncolls.load(std::memory_order_acquire)) map_identifiers(out, g_calls.load());
        check_coverage(in, g_calls.load());
      }
    }
  }
  if (!ok || !graph_probe::census()) return ok;
  const BlobEntry* e = in ? entry_of(*in) : nullptr;
  uint32_t magic = 0, names = 0, dxil_off = 0;
  if (!e || !e->ptr || e->size < 0x1c) return ok;
  std::memcpy(&magic, e->ptr, 4);
  std::memcpy(&names, e->ptr + 0x10, 4);
  std::memcpy(&dxil_off, e->ptr + 0x14, 4);
  if (magic != kCposMagic || dxil_off < 0x1c || dxil_off >= e->size) return ok;
  const auto h = dxil_hash::of(e->ptr + dxil_off, e->size - dxil_off);
  if (!h) return ok;
  std::vector<std::string> ex;
  uint32_t p = names;
  for (uint32_t i = 0; i < e->ptr[6] && p + 1 < dxil_off; ++i) {
    std::string s;
    for (; p + 1 < dxil_off; p += 2) {
      const uint16_t c = static_cast<uint16_t>(e->ptr[p] | (e->ptr[p + 1] << 8));
      if (!c) break;
      s += c < 0x80 ? static_cast<char>(c) : '?';
    }
    p += 2;
    ex.push_back(std::move(s));
  }
  std::vector<const char*> ptrs;
  for (const auto& s : ex) ptrs.push_back(s.c_str());
  graph_probe::on_state_object(out, h->data(), ptrs.data(), static_cast<uint32_t>(ptrs.size()));
  return ok;
}

// ---- M12: raygen variants + the one DispatchRays hook ----
constexpr uint32_t kMaxVar = 128;  // raygen variants, every client together (64 was full in CRT 2026-10-19)
struct Var {
  uint32_t lib;
  wchar_t raygen[64], variant[64];
  char label[160];
  uint64_t node;  // CName hash, 0 = everywhere
  std::atomic<uint32_t> on{1};
  std::atomic<uint64_t> swapped{0}, out_of_scope{0}, off{0}, missing{0};
  std::atomic<uint64_t> undelivered{0};  // a lease of its library did not land - the engine's raygen ran instead
  std::atomic<bool> said_in{false}, said_out{false}, said_missing{false};
};
// RT lease delivery checks at a variant's hand-out (bindings.cpp "RT lease delivery"): all / failed; the log lines.
std::atomic<uint64_t> g_deliv_checks{0}, g_deliv_fail{0};
Var g_var[kMaxVar];
std::atomic<uint32_t> g_nvar{0};

// ADD: the variants of library `lib` as an export list (the names live in g_var for the session).
uint32_t variant_exports(uint32_t lib, D3D12_EXPORT_DESC* out, uint32_t max) {
  const uint32_t n = g_nvar.load(std::memory_order_acquire);
  uint32_t k = 0;
  for (uint32_t i = 0; i < n && k < max; ++i) {
    if (g_var[i].lib != lib) continue;
    bool dup = false;
    for (uint32_t j = 0; j < k; ++j) dup = dup || std::wcscmp(out[j].Name, g_var[i].variant) == 0;
    if (!dup) out[k++] = {g_var[i].variant, nullptr, D3D12_EXPORT_FLAG_NONE};
  }
  return k;
}

using Rays_t = void (*)(uint32_t so, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7,
                        uint32_t a8, uint32_t buffer);
using Gsgi_t = const void* (*)(uint32_t id, uint32_t group);
Rays_t g_orig_rays = nullptr;
Gsgi_t g_orig_gsgi = nullptr;
bool g_rays_attached = false, g_gsgi_attached = false, g_rays_census = false;
struct Override {
  uint32_t id, group;
  Var* v;
  bool set;
};
thread_local Override t_ovr{};
thread_local bool t_ran = false;  // d_gsgi handed out a variant's identifier in this DispatchRays
thread_local bool t_undelivered = false;  // ... did NOT, because a lease of its library did not land
// the raygen group names seen per library, for the log (the first few)
std::atomic<uint32_t> g_names_logged{0};

uint8_t* rt_entry(uint32_t so, uint32_t* id_out) {
  const auto* tbl = *reinterpret_cast<const uint8_t* const*>(g_base + kRtSoTable);
  auto* dev = *reinterpret_cast<uint8_t* const*>(g_base + kDeviceData);
  if (!tbl || !dev) return nullptr;
  const uint8_t* arr = nullptr;
  std::memcpy(&arr, tbl + kRtSoTableArray, sizeof arr);
  uint32_t id = 0;
  if (arr) std::memcpy(&id, arr + size_t(so) * kRtSoEntry, 4);
  if (!id) return nullptr;
  *id_out = id;
  return dev + kRtStateObjects + size_t(id - 1) * kRtStateObjectStride;
}

const wchar_t* group_name(const uint8_t* entry, uint32_t group) {
  const uint8_t* names = nullptr;
  uint32_t count = 0;
  std::memcpy(&names, entry + kRtEntryNames, sizeof names);
  std::memcpy(&count, entry + kRtEntryNameCount, 4);
  if (!names || group >= count) return nullptr;
  const wchar_t* n = nullptr;
  std::memcpy(&n, names + size_t(group) * 16 + 8, sizeof n);
  return n;
}

void arm_variant(uint32_t so, uint32_t group) {
  uint32_t id = 0;
  const uint8_t* entry = rt_entry(so, &id);
  const uint64_t libs = entry ? libs_at(entry) : 0;
  if (!libs) return;
  const wchar_t* name = group_name(entry, group);
  if (!name) return;
  if (g_names_logged.load(std::memory_order_relaxed) < 8) {
    char a[64];
    size_t i = 0;
    for (; name[i] && i + 1 < sizeof a; ++i) a[i] = name[i] < 0x80 ? static_cast<char>(name[i]) : '?';
    a[i] = 0;
    unsigned long first = 0;
    _BitScanForward64(&first, libs);
    if (g_names_logged.fetch_add(1) < 8)
      g_sdk->logger->InfoF(g_handle, "rt variant: DispatchRays so %u (state object %u) with %s%s - raygen group %u = "
                           "'%s'", so, id, g_libs[first].name, (libs & (libs - 1)) ? " (+ more)" : "", group, a);
  }
  // Several clients may vary the same raygen: of those on and in scope, the highest library priority runs.
  Var* best = nullptr;
  const uint32_t n = g_nvar.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n; ++i) {
    Var& v = g_var[i];
    if (!(libs >> v.lib & 1) || std::wcscmp(name, v.raygen) != 0) continue;
    if (!v.on.load(std::memory_order_relaxed)) {
      v.off.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (v.node && render_flow::node_name_hash(render_flow::current_node()) != v.node) {
      v.out_of_scope.fetch_add(1, std::memory_order_relaxed);
      if (!v.said_out.exchange(true))
        g_sdk->logger->InfoF(g_handle, "rt variant: %s - a dispatch OUTSIDE its node: the engine's raygen ran (logged "
                             "once, counted in out_of_scope)", v.label);
      continue;
    }
    if (!best || g_libs[v.lib].priority > g_libs[best->lib].priority) best = &v;
  }
  if (best) t_ovr = {id, group, best, true};
}

// GPU timing (gpu_timing kRays*): a DispatchRays of a state object holding a client library - keyed by the raygen's
// variant registration (whether it runs or not, so switching it compares the two), else by the library. 0 = not.
using GetCtx_t = uint8_t* (*)();
uint32_t time_rays(uint32_t so, uint32_t group, ID3D12GraphicsCommandList** cl_out) {
  uint32_t id = 0;
  const uint8_t* entry = rt_entry(so, &id);
  const uint64_t libs = entry ? libs_at(entry) : 0;
  if (!libs) return 0;
  uint8_t* ctx = reinterpret_cast<GetCtx_t>(g_base + kGetCommandListContext)();
  if (!ctx) return 0;
  gpu_timing::Kind kind = gpu_timing::kRaysLib;
  unsigned long first = 0;
  _BitScanForward64(&first, libs);
  int32_t h = static_cast<int32_t>(first + 1);
  if (const wchar_t* name = group_name(entry, group)) {
    const uint32_t n = g_nvar.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i)
      if ((libs >> g_var[i].lib & 1) && std::wcscmp(name, g_var[i].raygen) == 0) {
        kind = gpu_timing::kRaysVariant;
        h = static_cast<int32_t>(i + 1);
        break;
      }
  }
  *cl_out = *reinterpret_cast<ID3D12GraphicsCommandList**>(ctx + kCtxCmdList);
  return *cl_out ? gpu_timing::begin(*cl_out, kind, h, false) : 0;
}

// ---- GPU markers ([Diagnostics] Dred; 2026-10-12: a user's GPU fault in a raygen, read of VA 0, and DRED's own
// breadcrumbs came back empty; then our own repro faulted in the DRIVER's acceleration-structure build, between two
// DispatchRays) ----
// Every engine DispatchRays and acceleration-structure build takes a slot in a ring: its description on the CPU, and on
// the GPU two WriteBufferImmediate into a READBACK buffer (starts and stays in COPY_DEST, mapped for good) on the
// engine's own list - the seq with MARKER_IN before the call, again with MARKER_OUT after it. At device removal
// (crash_log) a slot whose begin holds its seq and whose end does not is a call the GPU was INSIDE. Lists are recorded
// on many threads and run in submission order, so seq is the recording order only.
// AS builds are also CHECKED on the CPU before they are recorded: an input address of 0 where the build reads one
// (instances, vertices, indices, AABBs, scratch, the source of an update) is logged at once - with no crash needed.
constexpr uint32_t kMarkSlots = 16384;  // ~15-40 DispatchRays + the AS builds a frame: hundreds of frames
enum MarkKind : uint8_t { kMarkRays, kMarkBlasBatch, kMarkBlasNv, kMarkTlas };
struct Mark {
  std::atomic<uint32_t> seq;
  MarkKind kind;
  uint32_t so, id, group, w, h;  // rays: state object, its id, raygen group, grid; AS: w = count, h = zero inputs
  int32_t var;  // rays: the Var that RAN + 1; 0 = the engine's raygen; -1 = a variant was armed, the engine's ran;
                //   -2 = ... because a lease of its library did not land (bindings::rt_delivered)
  uint64_t node;
  bool job;
  DWORD tid;
  char what[96];  // rays: the raygen's name; AS: what was built and the first zero input
  char lease[96];  // rays: what every RT lease got in this dispatch (bindings::rt_state_text)
};
Mark g_marks[kMarkSlots];
std::atomic<uint32_t> g_mark_seq{0};
std::atomic<int> g_mark_state{0};  // 0 not yet, 1 creating, 2 ready, 3 failed
ID3D12Resource* g_mark_buf = nullptr;
D3D12_GPU_VIRTUAL_ADDRESS g_mark_va = 0;
const volatile uint32_t* g_mark_mem = nullptr;  // [2 * slot] begin, [2 * slot + 1] end
bool g_marks_on = false;                         // Dred armed at attach_rays
std::atomic<uint32_t> g_as_bad{0};               // AS builds with a zero input, logged live (the first kAsBadLog)
constexpr uint32_t kAsBadLog = 32;
std::atomic<uint32_t> g_lv_tlas{0}, g_lv_stale{0};  // light-volume TLAS builds checked / with a stale BLAS
std::atomic<uint32_t> g_lv_fresh{0};  // ... with a BLAS built by the same call, just before it

bool mark_create() {
  int expect = 0;
  if (!g_mark_state.compare_exchange_strong(expect, 1)) return g_mark_state.load(std::memory_order_acquire) == 2;
  const auto* data = *reinterpret_cast<const uint8_t* const*>(g_base + kDeviceData);
  auto* engine_dev = data ? *reinterpret_cast<IUnknown* const*>(data + kDeviceInData) : nullptr;
  ID3D12Device* dev = nullptr;
  HRESULT hr = engine_dev ? engine_dev->QueryInterface(IID_PPV_ARGS(&dev)) : E_POINTER;
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = static_cast<UINT64>(kMarkSlots) * 8;
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  if (SUCCEEDED(hr))
    hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                      IID_PPV_ARGS(&g_mark_buf));
  void* p = nullptr;
  if (SUCCEEDED(hr)) hr = g_mark_buf->Map(0, nullptr, &p);  // stays mapped: read after the device is gone
  if (dev) dev->Release();
  if (FAILED(hr)) {
    g_sdk->logger->WarnF(g_handle, "gpu markers: setup FAILED (0x%08x) - no DispatchRays / AS build markers",
                         static_cast<unsigned>(hr));
    if (g_mark_buf) g_mark_buf->Release(), g_mark_buf = nullptr;
    g_mark_state.store(3);
    return false;
  }
  std::memset(p, 0, static_cast<size_t>(rd.Width));
  g_mark_va = g_mark_buf->GetGPUVirtualAddress();
  g_mark_mem = static_cast<const volatile uint32_t*>(p);
  g_mark_state.store(2, std::memory_order_release);
  g_sdk->logger->InfoF(g_handle, "rt markers: ON - every DispatchRays and AS build brackets itself in a %u-slot ring "
                       "(Dred); a device removal logs the ones the GPU was inside", kMarkSlots);
  return true;
}

// The engine's list of the current thread as ID3D12GraphicsCommandList2 (WriteBufferImmediate); nullptr = none.
ID3D12GraphicsCommandList2* mark_list() {
  uint8_t* ctx = reinterpret_cast<GetCtx_t>(g_base + kGetCommandListContext)();
  auto* cl = ctx ? *reinterpret_cast<ID3D12GraphicsCommandList**>(ctx + kCtxCmdList) : nullptr;
  ID3D12GraphicsCommandList2* cl2 = nullptr;
  if (cl && FAILED(cl->QueryInterface(IID_PPV_ARGS(&cl2)))) cl2 = nullptr;
  return cl2;
}

void mark_write(ID3D12GraphicsCommandList2* cl, uint32_t slot, uint32_t which, uint32_t seq) {
  const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER prm{g_mark_va + (static_cast<UINT64>(slot) * 2 + which) * 4, seq};
  const D3D12_WRITEBUFFERIMMEDIATE_MODE mode =
      which == 0 ? D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN : D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
  cl->WriteBufferImmediate(1, &prm, &mode);
}

// A new slot (seq never 0), its common fields filled; the caller fills the rest, then mark_open.
Mark& mark_take(MarkKind kind, uint32_t* seq_out) {
  uint32_t seq = g_mark_seq.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!seq) seq = g_mark_seq.fetch_add(1, std::memory_order_relaxed) + 1;  // wrapped
  Mark& m = g_marks[seq % kMarkSlots];
  m.seq.store(0, std::memory_order_relaxed);  // half-written: the dump skips it
  m.kind = kind;
  m.so = m.id = m.group = m.w = m.h = 0;
  m.var = 0;
  const void* node = render_flow::current_node();
  m.node = node ? render_flow::node_name_hash(node) : 0;
  m.job = node && render_flow::current_is_job();
  m.tid = GetCurrentThreadId();
  m.what[0] = 0;
  m.lease[0] = 0;
  *seq_out = seq;
  return m;
}

void mark_open(ID3D12GraphicsCommandList2* cl, Mark& m, uint32_t seq) {
  m.seq.store(seq, std::memory_order_release);
  mark_write(cl, seq % kMarkSlots, 0, seq);
}

// The slot's seq for this DispatchRays (never 0).
uint32_t mark_begin(ID3D12GraphicsCommandList2* cl, uint32_t so, uint32_t w, uint32_t h, uint32_t group) {
  uint32_t seq = 0;
  Mark& m = mark_take(kMarkRays, &seq);
  uint32_t id = 0;
  const uint8_t* entry = rt_entry(so, &id);
  m.so = so;
  m.id = id;
  m.group = group;
  m.w = w;
  m.h = h;
  if (const wchar_t* n = entry ? group_name(entry, group) : nullptr) {
    size_t i = 0;
    for (; n[i] && i + 1 < sizeof m.what; ++i) m.what[i] = n[i] < 0x80 ? static_cast<char>(n[i]) : '?';
    m.what[i] = 0;
  }
  mark_open(cl, m, seq);
  return seq;
}

void mark_end(ID3D12GraphicsCommandList2* cl, uint32_t seq, const Var* armed) {
  Mark& m = g_marks[seq % kMarkSlots];
  if (armed) m.var = t_ran ? static_cast<int32_t>(armed - g_var) + 1 : t_undelivered ? -2 : -1;
  bindings::rt_state_text(m.lease, sizeof m.lease);
  mark_write(cl, seq % kMarkSlots, 1, seq);
}

void node_text(const Mark& m, char* out, size_t size) {
  std::snprintf(out, size, "(none)");
  if (!m.node) return;
  const char* s = RED4ext::CName(m.node).ToString();
  std::snprintf(out, size, "%s%s", s && *s ? s : "?", m.job ? " (job)" : "");
}

void mark_line(void (*line)(const char*), const char* state, const Mark& m, uint32_t seq) {
  char nm[96];
  node_text(m, nm, sizeof nm);
  char b[600];
  if (m.kind == kMarkRays) {
    char var[200] = "the engine's";
    if (m.var > 0) std::snprintf(var, sizeof var, "OUR VARIANT %s", g_var[m.var - 1].label);
    else if (m.var == -2) std::snprintf(var, sizeof var, "the engine's (a variant was armed; its lease did not land)");
    else if (m.var < 0) std::snprintf(var, sizeof var, "the engine's (a variant was armed, not handed out)");
    std::snprintf(b, sizeof b, "[gpu]   %s DispatchRays #%u: so %u (state object %u), raygen group %u '%s', %ux%u, "
                  "node %s, thread %lu - raygen %s; %s", state, seq, m.so, m.id, m.group, m.what, m.w, m.h, nm,
                  static_cast<unsigned long>(m.tid), var, m.lease[0] ? m.lease : "leases: -");
  } else {
    std::snprintf(b, sizeof b, "[gpu]   %s AS build #%u: %s, node %s, thread %lu", state, seq, m.what, nm,
                  static_cast<unsigned long>(m.tid));
  }
  line(b);
}

// crash_log, at device removal: the calls the GPU was inside; else where it stood (newest done, oldest queued).
void mark_dump(void (*line)(const char*)) {
  if (g_mark_state.load(std::memory_order_acquire) != 2) {
    line("[gpu] rt markers: none (no DispatchRays / AS build since the start, or the buffer failed)");
    return;
  }
  const uint32_t last = g_mark_seq.load();
  uint32_t inside = 0, done = 0, queued = 0, newest_done = 0;
  for (uint32_t i = 0; i < kMarkSlots; ++i) {
    const uint32_t seq = g_marks[i].seq.load(std::memory_order_acquire);
    if (!seq) continue;
    const uint32_t b = g_mark_mem[2 * i], e = g_mark_mem[2 * i + 1];
    if (e == seq) {
      ++done;
      if (seq > newest_done) newest_done = seq;
    } else if (b == seq) {
      if (inside++ < 16) mark_line(line, "INSIDE", g_marks[i], seq);
    } else {
      ++queued;
    }
  }
  char b[300];
  std::snprintf(b, sizeof b, "[gpu] rt markers: %u DispatchRays / AS builds recorded; in the ring: %u the GPU was "
                "INSIDE, %u done (newest #%u), %u not started; AS builds with a zero input this session: %u", last,
                inside, done, newest_done, queued, g_as_bad.load());
  line(b);
  std::snprintf(b, sizeof b, "[gpu] light-volume TLAS builds checked: %u, with a STALE BLAS: %u, with a FRESH BLAS: "
                "%u", g_lv_tlas.load(), g_lv_stale.load(), g_lv_fresh.load());
  line(b);
  std::snprintf(b, sizeof b, "[gpu] variant hand-outs checked: %llu, the engine's raygen instead (lease not landed): %llu",
                static_cast<unsigned long long>(g_deliv_checks.load()),
                static_cast<unsigned long long>(g_deliv_fail.load()));
  line(b);
  char big[700];
  bindings::rt_counts_text(big, sizeof big);
  line(big);
  bindings::rt_layout_text(big, sizeof big);
  line(big);
  for (uint32_t k = 0; k < 6 && newest_done > k; ++k) {
    const uint32_t seq = newest_done - k, i = seq % kMarkSlots;
    if (g_marks[i].seq.load() == seq && g_mark_mem[2 * i + 1] == seq) mark_line(line, "done  ", g_marks[i], seq);
  }
  for (uint32_t seq = newest_done + 1, k = 0; seq <= last && k < 6; ++seq) {
    const uint32_t i = seq % kMarkSlots;
    if (g_marks[i].seq.load() != seq || g_mark_mem[2 * i] == seq) continue;
    mark_line(line, "queued", g_marks[i], seq);
    ++k;
  }
}

// ---- AS builds: the markers + the zero-input check ----
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

void as_bad(const Mark& m, uint32_t seq) {
  if (g_as_bad.fetch_add(1, std::memory_order_relaxed) >= kAsBadLog) return;
  char nm[96];
  node_text(m, nm, sizeof nm);
  g_sdk->logger->WarnF(g_handle, "[gpu] AS build #%u with a ZERO input address: %s, node %s, thread %lu (logged for "
                       "the first %u)", seq, m.what, nm, static_cast<unsigned long>(m.tid), kAsBadLog);
}

// ---- AS lifetime, for the light-volume TLAS (crash 9 in the client's docs: INSIDE its build, garbage VA) ----
// Per AS id (16-bit), the marker seq of its last Create, BLAS build (the batch) and Destroy. The light-volume TLAS's
// instances are the ids GetAccelerationStructureHandle was asked for by the collector on this thread; at its build an
// id is STALE when it was never built since the hooks, destroyed after its last build, or re-created after it.
constexpr uint32_t kAsIds = 65536;
std::atomic<uint32_t> g_as_created[kAsIds], g_as_built[kAsIds], g_as_destroyed[kAsIds];
constexpr uint32_t kLvMax = 256;
thread_local uint32_t t_lv[kLvMax];
thread_local uint32_t t_nlv = 0, t_lv_over = 0, t_lv_seq0 = 0;  // seq0: before the collector's first handle


uint32_t seq_now() { return g_mark_seq.load(std::memory_order_relaxed); }
bool as_id_ok(uint32_t id) { return id - 1u < kAsIds - 1u; }

using CreateAs_t = uint32_t (*)(const void* desc);
using DestroyAs_t = void (*)(const uint32_t* ref);
using AsHandle_t = uint64_t (*)(uint32_t id);
CreateAs_t g_orig_create_as = nullptr;
DestroyAs_t g_orig_destroy_as = nullptr;
AsHandle_t g_orig_as_handle = nullptr;
bool g_create_as_attached = false, g_destroy_as_attached = false, g_as_handle_attached = false;

uint32_t d_create_as(const void* desc) {
  const uint32_t id = g_orig_create_as(desc);
  if (as_id_ok(id)) g_as_created[id].store(seq_now() + 1, std::memory_order_relaxed);
  return id;
}

void d_destroy_as(const uint32_t* ref) {
  if (ref && as_id_ok(*ref)) g_as_destroyed[*ref].store(seq_now() + 1, std::memory_order_relaxed);
  g_orig_destroy_as(ref);
}

uint64_t d_as_handle(uint32_t id) {
  if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_base + kAsHandleLightVolumeRet) {
    if (!t_nlv && !t_lv_over) t_lv_seq0 = seq_now() + 1;
    if (t_nlv < kLvMax) t_lv[t_nlv++] = id;
    else ++t_lv_over;
  }
  return g_orig_as_handle(id);
}

// At the light-volume TLAS build (its thread, after the collector's loop): judge every id it asked a handle for.
void lv_check(Mark& m, uint32_t seq, uint32_t count) {
  const uint32_t n = t_nlv;
  uint32_t stale = 0, never = 0, freed = 0, recreated = 0, first = 0, fb = 0, fd = 0, fc = 0, fresh = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const uint32_t id = t_lv[i];
    if (!as_id_ok(id)) continue;
    const uint32_t b = g_as_built[id].load(std::memory_order_relaxed);
    fresh += b >= t_lv_seq0;  // built by the batch of this same call, just before this TLAS
    const uint32_t d = g_as_destroyed[id].load(std::memory_order_relaxed);
    const uint32_t c = g_as_created[id].load(std::memory_order_relaxed);
    const bool nv = !b, fr = d > b, rc = c > b;
    if (!(nv || fr || rc)) continue;
    never += nv;
    freed += fr;
    recreated += rc;
    if (!stale++) first = id, fb = b, fd = d, fc = c;
  }
  const uint32_t k = g_lv_tlas.fetch_add(1, std::memory_order_relaxed);
  m.h = stale;
  if (fresh) g_lv_fresh.fetch_add(1, std::memory_order_relaxed);
  std::snprintf(m.what, sizeof m.what, "LIGHT-VOLUME TLAS of %u, %u ids%s, %u FRESH, %u STALE", count, n,
                t_lv_over ? " (list full)" : "", fresh, stale);
  static std::atomic<bool> said_first{false};
  static std::atomic<uint32_t> ids_total{0};
  ids_total.fetch_add(n, std::memory_order_relaxed);
  if ((k + 1) % 4096 == 0)  // a heartbeat: the check keeps seeing ids (~ once a minute or two)
    g_sdk->logger->InfoF(g_handle, "rt markers: light-volume TLAS builds checked %u, ids judged %u, with a STALE BLAS "
                         "%u, with a FRESH BLAS %u", k + 1, ids_total.load(), g_lv_stale.load(), g_lv_fresh.load());
  if (n && !said_first.exchange(true)) {
    char eg[96] = "";
    if (n && as_id_ok(t_lv[0]))
      std::snprintf(eg, sizeof eg, " - e.g. id %u: built #%u, created #%u", t_lv[0], g_as_built[t_lv[0]].load(),
                    g_as_created[t_lv[0]].load());
    g_sdk->logger->InfoF(g_handle, "rt markers: first light-volume TLAS #%u - %u instances, %u ids asked, %u stale%s "
                         "(self-test: ids seen = the check runs)", seq, count, n, stale, eg);
  }
  if (stale) {
    g_lv_stale.fetch_add(1, std::memory_order_relaxed);
    char nm[96];
    node_text(m, nm, sizeof nm);
    if (g_as_bad.fetch_add(1, std::memory_order_relaxed) < kAsBadLog)
      g_sdk->logger->WarnF(g_handle, "[gpu] light-volume TLAS #%u has %u STALE BLAS of %u (never built %u, freed after "
                           "build %u, re-created after build %u) - first id %u: built #%u, destroyed #%u, created #%u; "
                           "node %s", seq, stale, n, never, freed, recreated, first, fb, fd, fc, nm);
  }
  t_nlv = 0;
  t_lv_over = 0;
}

using BlasBatch_t = void (*)(void* range, void* geoms, uint32_t buffer);
using NvBuildAs_t = int (*)(void* cl, const void* params);
using Tlas_t = void (*)(uint32_t dest, uint32_t count, uint32_t instances, uint32_t a4, uint32_t scratch,
                        uint32_t scratch_off);
BlasBatch_t g_orig_blas = nullptr;
NvBuildAs_t g_orig_nv = nullptr;
Tlas_t g_orig_tlas = nullptr;
bool g_blas_attached = false, g_nv_attached = false, g_tlas_attached = false;

// The whole batch: the D3D12 path of a BLAS (vtable +0x240, not hookable by call site) is bracketed only here.
void d_blas(void* range, void* geoms, uint32_t buffer) {
  // Each 32 B entry starts with the AS id it builds (+0 -> device data + 0xd1b6a0 + (id - 1) * 0x58).
  if (const auto* r = static_cast<const uint8_t*>(range)) {
    const uint8_t *e = reinterpret_cast<const uint8_t*>(rd64(r)), *end = reinterpret_cast<const uint8_t*>(rd64(r + 8));
    const uint32_t now = seq_now() + 1;
    for (; e && e < end; e += 0x20)
      if (const uint32_t id = rd32(e); as_id_ok(id)) g_as_built[id].store(now, std::memory_order_relaxed);
  }
  ID3D12GraphicsCommandList2* cl = mark_create() ? mark_list() : nullptr;
  uint32_t seq = 0;
  if (cl) {
    Mark& m = mark_take(kMarkBlasBatch, &seq);
    const auto* r = static_cast<const uint8_t*>(range);
    const uint64_t n = r ? (rd64(r + 8) - rd64(r)) / 0x20 : 0;
    m.w = static_cast<uint32_t>(n);
    std::snprintf(m.what, sizeof m.what, "BLAS batch of %llu", static_cast<unsigned long long>(n));
    mark_open(cl, m, seq);
  }
  g_orig_blas(range, geoms, buffer);
  if (cl) {
    mark_write(cl, seq % kMarkSlots, 1, seq);
    cl->Release();
  }
}

// One BLAS through NVAPI: NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS {u32 version, pDesc, ...};
// pDesc = {dest, inputs {u32 type, flags, count, layout, u64 geometry stride, geometries}, source, scratch} - the
// engine's own layout (version 0x10020, stride 0x68). A geometry: u32 type (0 triangles, 1 AABBs, 2 triangles + OMM),
// flags, then D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC / _AABBS_DESC at +8.
int d_nv(void* cl_engine, const void* params) {
  if (readback::blas_armed()) {  // [Dev] Readback: copy this build's inputs before it runs
    const auto* p = static_cast<const uint8_t*>(params);
    if (p && rd32(p) == 0x10020) readback::on_blas_build(cl_engine, reinterpret_cast<const uint8_t*>(rd64(p + 8)));
  }
  if (blas_census::on()) {
    const auto* p = static_cast<const uint8_t*>(params);
    blas_census::on_nv(p && rd32(p) == 0x10020 ? reinterpret_cast<const uint8_t*>(rd64(p + 8)) : nullptr);
  }
  ID3D12GraphicsCommandList2* cl = g_marks_on && mark_create() ? mark_list() : nullptr;
  uint32_t seq = 0;
  if (cl) {
    Mark& m = mark_take(kMarkBlasNv, &seq);
    const auto* p = static_cast<const uint8_t*>(params);
    const uint8_t* d = p && rd32(p) == 0x10020 ? reinterpret_cast<const uint8_t*>(rd64(p + 8)) : nullptr;
    if (!d) {
      std::snprintf(m.what, sizeof m.what, "BLAS (NvAPI), params not read (version 0x%x)", p ? rd32(p) : 0u);
    } else {
      const uint32_t type = rd32(d + 8), flags = rd32(d + 0xc), count = rd32(d + 0x10), layout = rd32(d + 0x14);
      const uint64_t stride = rd64(d + 0x18), geoms = rd64(d + 0x20);
      const bool update = (flags & 0x20) != 0;
      char first[48] = "";
      uint32_t bad = 0;
      auto zero = [&](const char* what, uint32_t i) {
        if (!bad++) std::snprintf(first, sizeof first, "%s of geometry %u", what, i);
      };
      if (!rd64(d)) zero("dest", 0);
      if (!rd64(d + 0x30)) zero("scratch", 0);
      if (update && !rd64(d + 0x28)) zero("update source", 0);
      if (count && !geoms) zero("geometry list", 0);
      for (uint32_t i = 0; i < count && geoms && type == 1 /* BLAS */; ++i) {
        const uint8_t* g = layout == 0 ? reinterpret_cast<const uint8_t*>(geoms + i * stride)
                                       : reinterpret_cast<const uint8_t*>(rd64(reinterpret_cast<const uint8_t*>(geoms) + 8ull * i));
        if (!g) {
          zero("geometry pointer", i);
          continue;
        }
        const uint32_t gt = rd32(g);
        if (gt == 0 || gt == 2) {
          if (rd32(g + 0x1c) && !rd64(g + 0x28)) zero("vertices", i);
          if (rd32(g + 0x10) && rd32(g + 0x18) && !rd64(g + 0x20)) zero("indices", i);
        } else if (gt == 1) {
          if (rd64(g + 8) && !rd64(g + 0x10)) zero("AABBs", i);
        }
      }
      m.w = count;
      m.h = bad;
      std::snprintf(m.what, sizeof m.what, "BLAS %s (NvAPI), %u geometries%s%s", update ? "UPDATE" : "build", count,
                    bad ? ", ZERO: " : "", first);
      if (bad) as_bad(m, seq);
    }
    mark_open(cl, m, seq);
  }
  const int r = g_orig_nv(cl_engine, params);
  if (cl) {
    mark_write(cl, seq % kMarkSlots, 1, seq);
    cl->Release();
  }
  return r;
}

void d_tlas(uint32_t dest, uint32_t count, uint32_t instances, uint32_t a4, uint32_t scratch, uint32_t scratch_off) {
  if (bvh_probe::on()) {  // [Diagnostics] BvhProbe: this build, its instance buffer and its collector, per frame
    const auto* data = *reinterpret_cast<const uint8_t* const*>(g_base + kDeviceData);
    const uint64_t inst = data && instances - 1u <= 0x7ffffffeu
                              ? rd64(data + kBufferVa + size_t(instances - 1u) * kBufferStride)
                              : 0;
    bvh_probe::on_tlas(count, inst, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base));
  }
  ID3D12GraphicsCommandList2* cl = mark_create() ? mark_list() : nullptr;
  uint32_t seq = 0;
  if (cl) {
    Mark& m = mark_take(kMarkTlas, &seq);
    const auto* data = *reinterpret_cast<const uint8_t* const*>(g_base + kDeviceData);
    auto va = [&](uint32_t buf) -> uint64_t {
      return data && buf - 1u <= 0x7ffffffeu ? rd64(data + kBufferVa + size_t(buf - 1) * kBufferStride) : 0;
    };
    const uint64_t inst = va(instances), scr = va(scratch);
    const uint32_t bad = (count && !inst ? 1u : 0u) + (!scr ? 1u : 0u);
    m.w = count;
    m.h = bad;
    std::snprintf(m.what, sizeof m.what, "TLAS of %u instances, instance buffer %u VA 0x%llx, scratch VA 0x%llx%s",
                  count, instances, static_cast<unsigned long long>(inst), static_cast<unsigned long long>(scr),
                  bad ? " - ZERO" : "");
    if (bad) as_bad(m, seq);
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == g_base + kBuildTlasRet) lv_check(m, seq, count);
    mark_open(cl, m, seq);
  }
  t_nlv = t_lv_over = 0;
  g_orig_tlas(dest, count, instances, a4, scratch, scratch_off);
  if (cl) {
    mark_write(cl, seq % kMarkSlots, 1, seq);
    cl->Release();
  }
}

void attach_as() {
  g_blas_attached = attach_hook(kBuildBlasBatch, kBuildBlasBatchRet, reinterpret_cast<void*>(&d_blas),
                                reinterpret_cast<void**>(&g_orig_blas), "the BLAS batch builder");
  if (!g_nv_attached)  // the BLAS census may have attached it first
    g_nv_attached = attach_hook(kNvBuildAsEx, kNvBuildAsExRet, reinterpret_cast<void*>(&d_nv),
                                reinterpret_cast<void**>(&g_orig_nv), "NvAPI_D3D12_BuildRaytracingAccelerationStructureEx");
  g_tlas_attached = attach_hook(kBuildTlas, kBuildTlasRet, reinterpret_cast<void*>(&d_tlas),
                                reinterpret_cast<void**>(&g_orig_tlas), "GpuApi::BuildTopLevelAccelerationStructure");
  g_create_as_attached = attach_hook(kCreateAs, kCreateAsRet, reinterpret_cast<void*>(&d_create_as),
                                     reinterpret_cast<void**>(&g_orig_create_as), "GpuApi::CreateAccelerationStructure");
  g_destroy_as_attached = attach_hook(kDestroyAs, kDestroyAsRet, reinterpret_cast<void*>(&d_destroy_as),
                                      reinterpret_cast<void**>(&g_orig_destroy_as), "GpuApi::Destroy (AS)");
  g_as_handle_attached = attach_hook(kAsHandle, kAsHandleLightVolumeRet, reinterpret_cast<void*>(&d_as_handle),
                                     reinterpret_cast<void**>(&g_orig_as_handle),
                                     "GpuApi::GetAccelerationStructureHandle");
  auto h = [](bool b) { return b ? "HOOKED" : "NOT hooked"; };
  g_sdk->logger->InfoF(g_handle, "rt markers: AS builds - BLAS batch %s, NvAPI BLAS %s, TLAS %s; AS lifetime - create "
                       "%s, destroy %s, light-volume handles %s", h(g_blas_attached), h(g_nv_attached),
                       h(g_tlas_attached), h(g_create_as_attached), h(g_destroy_as_attached), h(g_as_handle_attached));
}

void detach_as() {
  if (g_as_handle_attached) detach_hook(kAsHandle);
  if (g_destroy_as_attached) detach_hook(kDestroyAs);
  if (g_create_as_attached) detach_hook(kCreateAs);
  g_create_as_attached = g_destroy_as_attached = g_as_handle_attached = false;
  if (g_tlas_attached) detach_hook(kBuildTlas);
  if (g_nv_attached) detach_hook(kNvBuildAsEx);
  if (g_blas_attached) detach_hook(kBuildBlasBatch);
  g_blas_attached = g_nv_attached = g_tlas_attached = false;
}

// [Diagnostics] BvhProbe without the DRED markers: only the TLAS builder - the probe's per-frame line needs every
// build, and one that traces a second instance-desc array is the whole question (docs/plan-mesh-proxy.md).
void attach_tlas_probe() {
  if (g_tlas_attached) return;
  g_tlas_attached = attach_hook(kBuildTlas, kBuildTlasRet, reinterpret_cast<void*>(&d_tlas),
                                reinterpret_cast<void**>(&g_orig_tlas),
                                "GpuApi::BuildTopLevelAccelerationStructure (bvh probe)");
  g_sdk->logger->InfoF(g_handle, "bvh probe: the TLAS builder %s", g_tlas_attached ? "HOOKED" : "NOT hooked");
}

void d_rays(uint32_t so, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8,
            uint32_t buffer) {
  // Entered from the hooked function's entry: our return address is the engine caller's.
  prof::Scope prof_scope(prof::kRays);
  if (g_rays_census)
    graph_probe::on_dispatch_rays(so, a2, a3, a5,
                                  static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base - 5));
  if (bvh_probe::on())  // [Diagnostics] BvhProbe: this dispatch's state object and engine call site, per frame
    bvh_probe::on_rays(so, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base - 5));
  t_ovr.set = false;
  t_ran = false;
  t_undelivered = false;
  bindings::rt_begin();
  if (g_gsgi_attached && g_nvar.load(std::memory_order_relaxed)) arm_variant(so, a4);
  const Var* const armed = t_ovr.set ? t_ovr.v : nullptr;
  ID3D12GraphicsCommandList* tcl = nullptr;
  const uint32_t timed = gpu_timing::active() ? time_rays(so, a4, &tcl) : 0;
  ID3D12GraphicsCommandList2* mcl = g_marks_on && mark_create() ? mark_list() : nullptr;
  const uint32_t mseq = mcl ? mark_begin(mcl, so, a2, a3, a4) : 0;
  {
    prof::Pause engine;
    g_orig_rays(so, a2, a3, a4, a5, a6, a7, a8, buffer);
  }
  if (mseq) mark_end(mcl, mseq, armed);
  if (mcl) mcl->Release();
  if (timed) gpu_timing::end(tcl, timed, false);  // DispatchRays records on the list it found
  t_ovr.set = false;
}

// The variant's shader identifier per (state-object slot, props, variant): GetShaderIdentifier is a lookup by NAME in
// the driver (~1.9 us measured 2026-10-10), on every dispatch that runs a variant. The pointer it returns lives as long
// as the state object; the engine re-creates objects in the same slot, so every creation through the creator drops
// that slot's entries (forget_slot, from d_create), and a different props pointer misses too.
struct IdEntry {
  const void* slot;
  const void* props;
  const Var* var;
  const void* ident;
};
constexpr uint32_t kIdCache = 256;
IdEntry g_ids[kIdCache];
uint32_t g_nids = 0;
SRWLOCK g_ids_lock = SRWLOCK_INIT;

void forget_slot(const void* slot) {
  AcquireSRWLockExclusive(&g_ids_lock);
  for (uint32_t i = 0; i < g_nids;)
    if (g_ids[i].slot == slot) g_ids[i] = g_ids[--g_nids];
    else ++i;
  ReleaseSRWLockExclusive(&g_ids_lock);
}

const void* identifier(const void* slot, ID3D12StateObjectProperties* props, const Var& v) {
  AcquireSRWLockShared(&g_ids_lock);
  const void* ident = nullptr;
  for (uint32_t i = 0; i < g_nids && !ident; ++i)
    if (g_ids[i].slot == slot && g_ids[i].props == props && g_ids[i].var == &v) ident = g_ids[i].ident;
  ReleaseSRWLockShared(&g_ids_lock);
  if (ident) return ident;
  ident = props->GetShaderIdentifier(v.variant);
  if (!ident) return nullptr;
  AcquireSRWLockExclusive(&g_ids_lock);
  if (g_nids < kIdCache) g_ids[g_nids++] = {slot, props, &v, ident};  // full: no cache, still correct
  ReleaseSRWLockExclusive(&g_ids_lock);
  return ident;
}

const void* d_gsgi(uint32_t id, uint32_t group) {
  prof::Scope prof_scope(prof::kGsgi);
  Override& o = t_ovr;
  if (o.set && o.id == id && o.group == group) {
    o.set = false;
    Var& v = *o.v;
    // The variant reads OUR leases; the engine's raygen does not. Hand it out only if they landed in THIS dispatch's
    // tables (UpdateRootParams ran just before, in GpuApi::DispatchRays).
    char why[128];
    const uint64_t checks = g_deliv_checks.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!bindings::rt_delivered(1ull << v.lib, why, sizeof why)) {
      t_undelivered = true;
      v.undelivered.fetch_add(1, std::memory_order_relaxed);
      const uint64_t f = g_deliv_fail.fetch_add(1, std::memory_order_relaxed) + 1;
      if (f <= 16 || (f & (f - 1)) == 0) {
        const void* node = render_flow::current_node();
        const char* nn = node ? RED4ext::CName(render_flow::node_name_hash(node)).ToString() : nullptr;
        g_sdk->logger->ErrorF(g_handle, "rt variant: %s - %s, node %s, thread %lu: the ENGINE's raygen ran this "
                              "dispatch instead (#%llu of %llu hand-outs; the first 16 and every 2^n logged)", v.label,
                              why, nn && *nn ? nn : "(none)", static_cast<unsigned long>(GetCurrentThreadId()),
                              static_cast<unsigned long long>(f), static_cast<unsigned long long>(checks));
      }
      prof::Pause engine;
      return g_orig_gsgi(id, group);
    }
    if ((checks & (checks - 1)) == 0 && (checks == 1 || checks >= 1024)) {
      char st[200], cnt[600];
      bindings::rt_state_text(st, sizeof st);
      bindings::rt_counts_text(cnt, sizeof cnt);
      g_sdk->logger->InfoF(g_handle, "rt variant: lease delivery checked at %llu hand-outs (%llu failed); this one %s; "
                           "%s", static_cast<unsigned long long>(checks),
                           static_cast<unsigned long long>(g_deliv_fail.load()), st, cnt);
    }
    auto* dev = *reinterpret_cast<uint8_t* const*>(g_base + kDeviceData);
    const uint8_t* slot = dev ? dev + kRtStateObjects + size_t(id - 1) * kRtStateObjectStride : nullptr;
    ID3D12StateObjectProperties* props = nullptr;
    if (slot) std::memcpy(&props, slot + kRtEntryProps, sizeof props);
    const void* ident = props ? identifier(slot, props, v) : nullptr;
    if (ident) {
      t_ran = true;
      if (v.swapped.fetch_add(1, std::memory_order_relaxed) == 0)
        g_sdk->logger->InfoF(g_handle, "rt variant: %s - first dispatch RUNS the variant (state object %u, group %u)",
                             v.label, id, group);
      return ident;
    }
    v.missing.fetch_add(1, std::memory_order_relaxed);
    if (!v.said_missing.exchange(true))
      g_sdk->logger->WarnF(g_handle, "rt variant: %s - the state object (%u) has NO such export - the engine's raygen "
                           "ran (logged once, counted in missing)", v.label, id);
  }
  prof::Pause engine;
  return g_orig_gsgi(id, group);
}

// LSS (2026-11-01): a ray-tracing pipeline intersects linear swept spheres only when created with NvAPI's
// ENABLE_LSS_SUPPORT pipeline option. The engine sets its options once in GpuApi::InitDevice - ONLY with OMM on (and a
// refused call turns its OMM off) -, so with OMM off a hit group asking for RG_HIT_GROUP_LSS used to get nothing at all
// and the collection said "LSS OFF" forever. 2026-11-03 (docs/internals.md "Known gaps" 11, item 3): the option is the
// HOST's capability now, not a side effect of the engine's call -
//   * the detour still adds our bit to the engine's own call when it happens (the flags have to be one set: the engine's
//     0x01 and ours 0x10),
//   * and when the engine never called, the host makes the call ITSELF with the LSS bit alone, once, at the first state
//     object (lss_apply_once) - before the first pipeline that carries our hit groups is created.
// It is device-global by NvAPI's design (every RT pipeline created afterwards has LSS), which is why the log line says so;
// nothing here can scope it to one collection.
constexpr uint32_t kNvPsoOptionsVer = 0x10008, kNvPsoLss = 0x10;  // _PARAMS_V1 {u32 version, u32 flags}, NV_BIT(4)
using NvPsoOptions_t = int (*)(void* device, uint32_t* params);
NvPsoOptions_t g_orig_pso = nullptr;
bool g_pso_attached = false;
std::atomic<bool> g_engine_lss{false};  // the ENGINE's call went out with our bit (its own OMM option + LSS)
std::atomic<bool> g_own_lss{false};     // the host's own call: tried once, `g_lss` says whether the driver took it

int d_pso_options(void* device, uint32_t* params) {
  g_pso_calls.fetch_add(1, std::memory_order_relaxed);
  if (!params || params[0] != kNvPsoOptionsVer ||
      reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_base + kNvSetCreatePsoOptionsRet)
    return g_orig_pso(device, params);
  const uint32_t flags = params[1];
  params[1] = flags | kNvPsoLss;
  const int r = g_orig_pso(device, params);
  params[1] = flags;
  if (r == 0) {
    g_lss.store(1, std::memory_order_release);
    g_engine_lss.store(true, std::memory_order_release);
    g_sdk->logger->InfoF(g_handle, "rt hit group: LSS ON - the engine's NvAPI pipeline options 0x%x -> 0x%x (every RT "
                         "pipeline intersects linear swept spheres)", flags, flags | kNvPsoLss);
    return r;
  }
  g_sdk->logger->WarnF(g_handle, "rt hit group: the driver REFUSED LSS support (NvAPI %d) - the engine's options alone "
                       "(0x%x); no LSS geometry may enter the BVH", r, flags);
  return g_orig_pso(device, params);
}

// 2026-11-03 (the hair mod's glass probe, docs/internals.md "Known gaps" 13): the glass reflection's pipeline (engine
// state object 78) ran its MISS shader for an opaque LSS hit while the PT pipeline ran the hit group - NvAPI: "if the
// pipeline is not created with the correct primitive support, and that primitive type is encountered during traversal,
// behavior is undefined", and the options are GLOBAL "until this function is called again". The exe hook above sees
// only the engine's InitDevice call through its statically linked NvAPI; any other caller (another call site, another
// module with its own NvAPI) reached the driver unseen and could drop the LSS bit for every pipeline after it. So the
// DRIVER's function itself is hooked (nvapi64.dll's nvapi_QueryInterface(0x5c607a27)): every call from every module is
// logged with its caller and gets the LSS bit, and the flags in force are kept for the state-object log (d_create).
constexpr uint32_t kNvSetCreatePsoOptionsId = 0x5c607a27;
void* g_driver_pso = nullptr;               // the driver's NvAPI_D3D12_SetCreatePipelineStateOptions, hooked
std::atomic<uint32_t> g_opts_now{0};        // the options in force (what the driver last accepted)
std::atomic<uint32_t> g_opts_calls{0};      // the calls the driver accepted
std::atomic<uint32_t> g_opts_unlss{0};      // ... that came WITHOUT the LSS bit (it was added)

// `addr` as "module+0xoffset", for the log.
void caller_of(const void* addr, char* out, size_t cap) {
  HMODULE m = nullptr;
  wchar_t path[MAX_PATH] = {};
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          static_cast<LPCWSTR>(addr), &m) || !m || !GetModuleFileNameW(m, path, MAX_PATH)) {
    std::snprintf(out, cap, "%p (no module)", addr);
    return;
  }
  const wchar_t* base = std::wcsrchr(path, L'\\');
  base = base ? base + 1 : path;
  char name[64] = {};
  size_t i = 0;
  for (; base[i] && i + 1 < sizeof name; ++i) name[i] = base[i] < 0x80 ? static_cast<char>(base[i]) : '?';
  std::snprintf(out, cap, "%s+0x%llx", name,
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(m)));
}

bool lss_wanted();
int d_pso_driver(void* device, uint32_t* params) {
  if (!params || params[0] != kNvPsoOptionsVer) return g_orig_pso(device, params);
  char who[128];
  caller_of(_ReturnAddress(), who, sizeof who);
  const uint32_t flags = params[1];
  const bool add = lss_wanted() && !(flags & kNvPsoLss);
  if (add) params[1] = flags | kNvPsoLss;
  const int r = g_orig_pso(device, params);
  const uint32_t sent = params[1];
  params[1] = flags;
  const uint32_t n = g_opts_calls.fetch_add(r == 0 ? 1u : 0u) + 1;
  if (r == 0) {
    g_opts_now.store(sent, std::memory_order_release);
    if (sent & kNvPsoLss) g_lss.store(1, std::memory_order_release), g_engine_lss.store(true, std::memory_order_release);
    if (add) g_opts_unlss.fetch_add(1, std::memory_order_relaxed);
  }
  g_sdk->logger->InfoF(g_handle, "rt pipeline options: call #%u from %s - flags 0x%x%s -> NvAPI %d, in force now 0x%x", n,
                       who, flags, add ? " (+ the LSS bit)" : "", r, g_opts_now.load());
  if (r != 0 && add) {  // refused with our bit: what the caller asked for alone
    const int r2 = g_orig_pso(device, params);
    if (r2 == 0) g_opts_now.store(flags, std::memory_order_release);
    g_sdk->logger->WarnF(g_handle, "rt pipeline options: the driver REFUSED the LSS bit (NvAPI %d) - the caller's 0x%x "
                         "alone (NvAPI %d); RT pipelines from here have no LSS", r, flags, r2);
    return r2;
  }
  return r;
}

// The driver's function, by nvapi64.dll's own query (the DLL is the driver's; loading it initializes nothing).
bool attach_pso_driver() {
  HMODULE nv = LoadLibraryW(L"nvapi64.dll");
  using Query_t = void* (*)(uint32_t);
  const auto q = nv ? reinterpret_cast<Query_t>(GetProcAddress(nv, "nvapi_QueryInterface")) : nullptr;
  void* fn = q ? q(kNvSetCreatePsoOptionsId) : nullptr;
  if (!fn || !g_sdk->hooking->Attach(g_handle, fn, reinterpret_cast<void*>(&d_pso_driver),
                                     reinterpret_cast<void**>(&g_orig_pso))) {
    g_sdk->logger->WarnF(g_handle, "rt pipeline options: the driver's SetCreatePipelineStateOptions NOT hooked (%s) - "
                         "the engine's InitDevice call only", !nv ? "no nvapi64.dll" : !fn ? "no such interface" : "attach");
    return false;
  }
  g_driver_pso = fn;
  g_sdk->logger->InfoF(g_handle, "rt pipeline options: the driver's SetCreatePipelineStateOptions %p HOOKED - every "
                       "caller logged, the LSS bit added to every call", fn);
  return true;
}

// d_create: one line per PIPELINE (not a collection) - its engine id and the options it was created under.
void note_pipeline_options(const void* out, const uint32_t* in) {
  const BlobEntry* pe = in ? entry_of(*in) : nullptr;
  if (!pe || !pe->ptr || pe->size < 0x1c || pe->ptr[kCposCollection]) return;
  auto* dev = *reinterpret_cast<uint8_t* const*>(g_base + kDeviceData);
  if (!dev) return;
  const uintptr_t first = reinterpret_cast<uintptr_t>(dev + kRtStateObjects), at = reinterpret_cast<uintptr_t>(out);
  const uint32_t id = at >= first ? static_cast<uint32_t>((at - first) / kRtStateObjectStride) + 1 : 0;
  const uint32_t o = g_opts_now.load(std::memory_order_acquire);
  const bool bad = lss_wanted() && !(o & kNvPsoLss);
  if (bad)
    g_sdk->logger->WarnF(g_handle, "rt pipeline options: state object #%u (a pipeline) created WITHOUT the LSS option (in "
                         "force 0x%x, %u call(s) so far) - an LSS hit there is undefined (a miss)", id, o,
                         g_opts_calls.load());
  else
    g_sdk->logger->InfoF(g_handle, "rt pipeline options: state object #%u (a pipeline) created with options 0x%x", id, o);
}

// The host's own ENABLE_LSS_SUPPORT call: once, when a hit group asked for LSS and the engine set no pipeline options at
// all (it does only with OMM on). Called at the first state object, before the engine's creator runs - a PSO built
// afterwards carries LSS, every one built before does not (a state object is immutable).
bool lss_wanted();
void lss_apply_once() {
  if (!g_pso_attached || g_engine_lss.load(std::memory_order_acquire) || g_own_lss.exchange(true)) return;
  if (!lss_wanted()) return;
  const auto* data = *reinterpret_cast<const uint8_t* const*>(g_base + kDeviceData);
  auto* engine_dev = data ? *reinterpret_cast<IUnknown* const*>(data + kDeviceInData) : nullptr;
  ID3D12Device* dev = nullptr;
  if (!engine_dev || FAILED(engine_dev->QueryInterface(IID_PPV_ARGS(&dev))) || !dev) {
    g_sdk->logger->WarnF(g_handle, "rt hit group: LSS NOT enabled - the engine's device is not there yet at the first state "
                         "object (the hit group's LSS geometry has no pipeline option)");
    return;
  }
  uint32_t params[2] = {kNvPsoOptionsVer, kNvPsoLss};  // the LSS bit alone: the engine set nothing for us to keep
  const int r = g_orig_pso(dev, params);
  dev->Release();
  if (r == 0) {
    g_lss.store(1, std::memory_order_release);
    g_opts_now.store(kNvPsoLss, std::memory_order_release);
    g_sdk->logger->InfoF(g_handle, "rt hit group: LSS ON - the host's own NvAPI pipeline options 0x%x (the engine set none: "
                         "OMM off), before the first state object (every RT pipeline from here intersects linear swept "
                         "spheres)", kNvPsoLss);
  } else {
    g_sdk->logger->WarnF(g_handle, "rt hit group: the driver REFUSED the host's LSS pipeline options (NvAPI %d) - no LSS "
                         "geometry may enter the BVH", r);
  }
}

bool lss_wanted() {
  for (uint32_t i = 0; i < g_nhg.load(std::memory_order_acquire); ++i)
    if (g_hg[i].flags & RG_HIT_GROUP_LSS) return true;
  return false;
}

bool valid_name(const char* s, size_t max) {
  if (!s || !*s || std::strlen(s) > max) return false;
  for (; *s; ++s)
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_')) return false;
  return true;
}

}  // namespace

rg_handle hit_group_add(const char* name, const rg_rt_hit_group_desc* d) {
  if (!registration_open()) return RG_E_LATE;
  if (!d || d->struct_size < sizeof(rg_rt_hit_group_desc) || !valid_name(name, 40) || !d->payload_bytes ||
      !d->library.bytes || d->library.size < 32 || (!d->closest_hit && !d->any_hit) ||
      (d->closest_hit && !valid_name(d->closest_hit, 48)) || (d->any_hit && !valid_name(d->any_hit, 48)))
    return RG_E_ARG;
  if (std::memcmp(d->library.bytes, "DXBC", 4) != 0) return RG_E_BLOB;
  const uint32_t n = g_nhg.load();
  if (n >= kMaxHg) return RG_E_FULL;
  for (uint32_t i = 0; i < n; ++i)
    if (std::strcmp(g_hg[i].name, name) == 0) return RG_E_TAKEN;
  HitGroup& g = g_hg[n];
  std::snprintf(g.name, sizeof g.name, "%s", name);
  g.payload = d->payload_bytes;
  g.flags = d->flags;
  std::swprintf(g.chs, 64, L"%hs", d->closest_hit ? d->closest_hit : "");
  std::swprintf(g.ahs, 64, L"%hs", d->any_hit ? d->any_hit : "");
  g.lib = d->library;
  g_nhg.store(n + 1, std::memory_order_release);
  g_sdk->logger->InfoF(g_handle, "host: RT hit group '%s' (closest-hit %s, any-hit %s, %u B DXIL) for every collection "
                       "with a %u-byte payload%s", name, d->closest_hit ? d->closest_hit : "-",
                       d->any_hit ? d->any_hit : "-", d->library.size, d->payload_bytes,
                       (d->flags & RG_HIT_GROUP_LSS) ? ", LSS" : "");
  return static_cast<rg_handle>(n + 1);
}

int32_t hit_group_identifier(rg_handle h, const void* engine_identifier, void* out) {
  if (h <= 0 || static_cast<uint32_t>(h) > g_nhg.load(std::memory_order_acquire) || !engine_identifier || !out)
    return RG_E_ARG;
  int32_t r = RG_E_UNAVAILABLE;
  AcquireSRWLockShared(&g_pairs_lock);
  for (uint32_t i = 0; i < g_npairs; ++i)
    if (g_pairs[i].hg == static_cast<uint32_t>(h - 1) && std::memcmp(g_pairs[i].eng, engine_identifier, 32) == 0) {
      std::memcpy(out, g_pairs[i].ours, 32);
      r = 0;
      break;
    }
  ReleaseSRWLockShared(&g_pairs_lock);
  return r;
}

int32_t hit_group_pairs(rg_handle h, rg_rt_hit_group_pair* out, uint32_t max) {
  if (h <= 0 || static_cast<uint32_t>(h) > g_nhg.load(std::memory_order_acquire) || (max && !out)) return RG_E_ARG;
  int32_t n = 0;
  AcquireSRWLockShared(&g_pairs_lock);
  for (uint32_t i = 0; i < g_npairs; ++i) {
    if (g_pairs[i].hg != static_cast<uint32_t>(h - 1)) continue;
    if (static_cast<uint32_t>(n) < max) {
      std::memcpy(out[n].engine, g_pairs[i].eng, 32);
      std::memcpy(out[n].ours, g_pairs[i].ours, 32);
    }
    ++n;
  }
  ReleaseSRWLockShared(&g_pairs_lock);
  return n;
}

// ---- rt_hit_records: a client's own range of the ENGINE's hit-table records (rg_hit_records) ----
// The indices above the engine's own contributions are shared by every client that shades geometry of its own, and until
// 2026-11-03 each such client picked its own (`199999 - 2i` in the hair mod) - a second RT client would overwrite the
// first with no word (docs/internals.md `Known gaps` 10). The host cannot read those tables (its hit shaders have them
// bound; no CPU-side structure holds them), so the CALLER says how many records they hold - what its own companion read
// with GetDimensions (200001 on 2.5) - and the host does only what no client can do alone: allocate from the top down,
// never twice, and remember whose range is whose for the swap check below.
struct RecRange {
  uint32_t base, count, client;  // [base, base + count)
};
constexpr uint32_t kMaxRecRanges = 16;
RecRange g_recr[kMaxRecRanges];
uint32_t g_nrecr = 0, g_recr_capacity = 0, g_recr_top = 0;
std::mutex g_recr_mx;
std::atomic<uint32_t> g_recr_taken{0};  // swaps refused: a contribution inside ANOTHER client's range
std::atomic<uint32_t> g_recr_unowned{0};  // ... inside no range at all, while ranges exist (warned twice)

int32_t records_alloc(uint32_t client, uint32_t count, uint32_t capacity, rg_hit_records* out) {
  if (!out || out->struct_size < sizeof(rg_hit_records) || !count || !capacity) return RG_E_ARG;
  if (!client) {  // a module the host cannot name: a range nobody can be charged for is not worth handing out
    g_sdk->logger->WarnF(g_handle, "rt hit records: the caller is not a module the host knows - refused");
    return RG_E_ARG;
  }
  std::lock_guard<std::mutex> lk(g_recr_mx);
  if (!g_recr_capacity) g_recr_capacity = capacity, g_recr_top = capacity;
  if (capacity != g_recr_capacity) {
    g_sdk->logger->WarnF(g_handle, "rt hit records: %u records reported where the first caller said %u - refused (one "
                                   "set of tables, one size)",
                         capacity, g_recr_capacity);
    return RG_E_ARG;
  }
  if (g_nrecr >= kMaxRecRanges || g_recr_top < count) {
    g_sdk->logger->WarnF(g_handle, "rt hit records: no room for %u more (%u range(s), %u records left below the top)",
                         count, g_nrecr, g_recr_top);
    return RG_E_FULL;
  }
  const uint32_t base = g_recr_top - count;
  g_recr_top = base;
  g_recr[g_nrecr++] = {base, count, client};
  const uint32_t ss = out->struct_size;
  *out = {};
  out->struct_size = ss;
  out->base = base, out->count = count;
  mesh_proxy::desc_high(&out->engine_high, &out->frame);
  g_sdk->logger->InfoF(g_handle, "rt hit records: client #%u's records at [%u, %u] of %u - the engine's own scene is at "
                                 "or below %u%s",
                       client, base, base + count - 1, capacity, out->engine_high,
                       out->engine_high ? "" : " (not measured: the instance desc hook is not attached)");
  return 0;
}

// mesh_proxy_bvh_swap's contribution against the ranges: inside ANOTHER client's = refused - that is exactly the
// collision the ranges exist for. Inside none = allowed (a swap may keep the ENGINE's contribution, and a client that
// asked for no range keeps the pre-2026-11-03 behaviour), counted and said once: it is the state ranges are meant to end.
int32_t records_check(uint32_t client, uint32_t contribution) {
  bool mine = false, other = false;
  uint32_t other_client = 0;
  {
    std::lock_guard<std::mutex> lk(g_recr_mx);
    for (uint32_t i = 0; i < g_nrecr; ++i)
      if (contribution >= g_recr[i].base && contribution < g_recr[i].base + g_recr[i].count) {
        if (g_recr[i].client == client) mine = true;
        else other = true, other_client = g_recr[i].client;
      }
  }
  if (other) {
    if (g_recr_taken.fetch_add(1, std::memory_order_relaxed) < 4)
      g_sdk->logger->ErrorF(g_handle, "rt hit records: client #%u's swap names contribution %u - that is client #%u's "
                                      "range: its records are not ours to point a BLAS at (refused)",
                            client, contribution, other_client);
    return RG_E_TAKEN;
  }
  if (!mine && g_nrecr && g_recr_unowned.fetch_add(1, std::memory_order_relaxed) < 2)
    g_sdk->logger->WarnF(g_handle, "rt hit records: client #%u's swap names contribution %u, which no client's range "
                                   "holds - allowed, but those records are not checked against another client's",
                         client, contribution);
  return 0;
}

void hit_group_stats(rg_handle h, rg_rt_hit_group_stats* out) {
  if (!out) return;
  *out = {};
  out->lss = g_lss.load(std::memory_order_acquire);
  if (h <= 0 || static_cast<uint32_t>(h) > g_nhg.load(std::memory_order_acquire)) return;
  const HitGroup& g = g_hg[h - 1];
  out->collections = g.collections.load(std::memory_order_relaxed);
  out->failed = g.failed.load(std::memory_order_relaxed);
  out->pipelines = g.pipelines.load(std::memory_order_relaxed);
  out->mismatches = g.mismatches.load(std::memory_order_relaxed);
  out->uncovered = g.uncovered.load(std::memory_order_relaxed);
  AcquireSRWLockShared(&g_pairs_lock);
  for (uint32_t i = 0; i < g_npairs; ++i) out->identifiers += g_pairs[i].hg == static_cast<uint32_t>(h - 1);
  ReleaseSRWLockShared(&g_pairs_lock);
  const bool refused = (g_hg_refused.load(std::memory_order_relaxed) >> (h - 1)) & 1;
  out->state = refused ? RG_STATE_REFUSED : out->collections ? RG_STATE_OK : RG_STATE_PENDING;
}

rg_handle variant_add(rg_handle lib, const char* raygen, const char* variant, const char* node) {
  if (!registration_open()) return RG_E_LATE;
  if (lib <= 0 || static_cast<uint32_t>(lib) > g_n.load()) return RG_E_ARG;
  // Any refusal below takes the library with it: a variants-only blob without its variant would be SWAPPED in for
  // the engine's library (M3) and break that RT pass (CRT 2026-10-19: RG_E_FULL left three such libraries in -> crash).
  const auto refuse_lib = [&](int32_t code, const char* why) {
    g_sdk->logger->ErrorF(g_handle, "host: RT raygen variant '%s' REFUSED - %s; library '%s' refused with it",
                          variant ? variant : "?", why, g_libs[lib - 1].name);
    clients::reason(why);
    g_libs[lib - 1].refused = true;
    clients::refused(clients::kRtLib, lib, why);
    return static_cast<rg_handle>(code);
  };
  if (!raygen || !variant || !*raygen || !*variant || std::strlen(raygen) >= 64 || std::strlen(variant) >= 64)
    return refuse_lib(RG_E_ARG, "bad raygen / variant name");
  const uint32_t n = g_nvar.load();
  if (n >= kMaxVar) return refuse_lib(RG_E_FULL, "the raygen variant table is full");
  // An export name is ONE function in a state object: a second library on the same engine library exporting the same
  // name is left out at the bake (arm_add), and GetShaderIdentifier would hand its variant the FIRST one's shader.
  for (uint32_t i = 0; i < n; ++i) {
    const Var& o = g_var[i];
    if (o.lib == static_cast<uint32_t>(lib - 1) || !(g_libs[o.lib].hash == g_libs[lib - 1].hash)) continue;
    bool same = true;
    for (size_t c = 0; c <= std::strlen(variant) && same; ++c) same = o.variant[c] == static_cast<unsigned char>(variant[c]);
    if (!same) continue;
    char why[160];
    std::snprintf(why, sizeof why, "the export '%s' is already a variant of library '%s' on the same engine library "
                  "(one name per state object - prefix your variants with your mod's tag)", variant, g_libs[o.lib].name);
    g_sdk->logger->ErrorF(g_handle, "host: RT raygen variant '%s' REFUSED - %s; library '%s' refused with it", variant,
                          why, g_libs[lib - 1].name);
    clients::reason(why);
    // Its library goes too: without a variant it would be SWAPPED in for the engine's (M3) - and a variants-only blob
    // in place of the engine's library breaks that RT pass.
    g_libs[lib - 1].refused = true;
    clients::refused(clients::kRtLib, lib, why);
    return RG_E_TAKEN;
  }
  Var& v = g_var[n];
  v.lib = static_cast<uint32_t>(lib - 1);
  for (size_t i = 0; i <= std::strlen(raygen); ++i) v.raygen[i] = static_cast<unsigned char>(raygen[i]);
  for (size_t i = 0; i <= std::strlen(variant); ++i) v.variant[i] = static_cast<unsigned char>(variant[i]);
  v.node = node && *node ? RED4ext::CName(node).hash : 0;
  std::snprintf(v.label, sizeof v.label, "'%s' -> '%s' in %s%s", raygen, variant, g_libs[v.lib].name,
                v.node ? "" : " (everywhere)");
  if (v.node) {
    std::snprintf(v.label + std::strlen(v.label), sizeof v.label - std::strlen(v.label), " inside '%s'", node);
    render_flow::want();  // the node on the recording thread
  }
  g_nvar.store(n + 1, std::memory_order_release);
  g_sdk->logger->InfoF(g_handle, "host: RT raygen variant %s (M12)", v.label);
  return static_cast<rg_handle>(n + 1);
}

int32_t variant_set(rg_handle h, uint32_t on) {
  if (h <= 0 || static_cast<uint32_t>(h) > g_nvar.load(std::memory_order_acquire)) return RG_E_ARG;
  g_var[h - 1].on.store(on ? 1u : 0u, std::memory_order_relaxed);
  return 0;
}

void variant_stats(rg_handle h, rg_rt_variant_stats* out) {
  if (!out) return;
  *out = {};
  if (h <= 0 || static_cast<uint32_t>(h) > g_nvar.load(std::memory_order_acquire)) return;
  const Var& v = g_var[h - 1];
  out->swapped = v.swapped.load();
  out->out_of_scope = v.out_of_scope.load();
  out->off = v.off.load();
  out->missing = v.missing.load();
  out->on = v.on.load();
}

void attach_rays(bool census, bool timing) {
  const bool variants = g_nvar.load() != 0 && g_attached;
  const bool timed = timing && g_attached && g_n.load() != 0;  // GPU timing on at the start + client libraries
  const bool marks = ::crash_log::dred_armed();  // DispatchRays markers for a device-removal report
  const bool probe = bvh_probe::on();  // [Diagnostics] BvhProbe: its per-frame line needs the TLAS builder too
  if (!census && !variants && !timed && !marks && !probe) return;
  g_rays_attached = attach_hook(kDispatchRays, kDispatchRaysRet, reinterpret_cast<void*>(&d_rays),
                                reinterpret_cast<void**>(&g_orig_rays), "DispatchRays (global)");
  g_rays_census = g_rays_attached && census;
  g_marks_on = g_rays_attached && marks;
  if (g_marks_on) ::crash_log::set_extra_dump(&mark_dump);
  if (g_marks_on) attach_as();
  else if (probe) attach_tlas_probe();
  if (g_rays_attached && variants)
    g_gsgi_attached = attach_hook(kGetShaderGroupIdentifier, kGpuDispatchRaysGsgiRet, reinterpret_cast<void*>(&d_gsgi),
                                  reinterpret_cast<void**>(&g_orig_gsgi), "GpuApi::GetShaderGroupIdentifier");
  g_sdk->logger->InfoF(g_handle, "rt swap: DispatchRays %s - census rows %s, raygen variants %s (%u)",
                       g_rays_attached ? "HOOKED" : "NOT hooked", g_rays_census ? "on" : "off",
                       g_gsgi_attached ? "ON" : (variants ? "NOT attached" : "none"), g_nvar.load());
}

bool attach_nv_census() {
  if (!g_nv_attached)
    g_nv_attached = attach_hook(kNvBuildAsEx, kNvBuildAsExRet, reinterpret_cast<void*>(&d_nv),
                                reinterpret_cast<void**>(&g_orig_nv), "NvAPI BLAS builds (BLAS census)");
  return g_nv_attached;
}

void detach_rays() {
  if (g_gsgi_attached) detach_hook(kGetShaderGroupIdentifier);
  if (g_rays_attached) detach_hook(kDispatchRays);
  if (g_marks_on) ::crash_log::set_extra_dump(nullptr);
  detach_as();
  g_gsgi_attached = g_rays_attached = g_rays_census = g_marks_on = false;
}

rg_handle add(const char* name, const uint8_t* hash16, rg_blob lib, int32_t priority) {
  if (!registration_open()) return RG_E_LATE;
  if (!hash16 || !lib.bytes || lib.size < 32) return RG_E_ARG;
  if (std::memcmp(lib.bytes, "DXBC", 4) != 0) return RG_E_BLOB;
  const uint32_t n = g_n.load();
  if (n >= kMax) return RG_E_FULL;
  dxil_hash::Hash h;
  std::memcpy(h.data(), hash16, 16);
  // Several libraries on one hash are fine: those with variants are all added, of those without the creation swaps
  // in the highest priority (rtlib::create) - the variants are registered after this call, so nothing is decided here.
  Lib& l = g_libs[n];
  l.name = name ? name : "?";
  l.hash = h;
  l.lib = lib;
  l.priority = priority;
  l.refused = false;
  g_n.store(n + 1, std::memory_order_release);
  return static_cast<rg_handle>(n + 1);
}

bool needed() { return g_n.load() != 0 || g_nhg.load() != 0; }

bool info(rg_handle h, rg_blob* lib) {
  if (h <= 0 || static_cast<uint32_t>(h) > g_n.load(std::memory_order_acquire) || !lib) return false;
  *lib = g_libs[h - 1].lib;
  return true;
}

void stats(rg_handle h, rg_rtlib_stats* out) {
  if (!out) return;
  *out = {};
  if (h <= 0 || static_cast<uint32_t>(h) > g_n.load(std::memory_order_acquire)) return;
  const Lib& l = g_libs[h - 1];
  out->swapped = l.swapped.load(std::memory_order_relaxed);
  out->added = l.added.load(std::memory_order_relaxed);
  out->failed = l.failed.load(std::memory_order_relaxed);
  out->state = l.refused ? RG_STATE_REFUSED : (out->swapped || out->added) ? RG_STATE_OK : RG_STATE_PENDING;
}

void debug_layer() {
  HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
  using GetDebugInterface = HRESULT(WINAPI*)(REFIID, void**);
  auto* get = d3d12 ? reinterpret_cast<GetDebugInterface>(GetProcAddress(d3d12, "D3D12GetDebugInterface")) : nullptr;
  ID3D12Debug* d = nullptr;
  if (!get || FAILED(get(IID_PPV_ARGS(&d))) || !d) {
    g_sdk->logger->Warn(g_handle, "DEV: d3d12_debug - no D3D12 debug layer (D3D12SDKLayers.dll missing?)");
    return;
  }
  d->EnableDebugLayer();
  d->Release();
  g_debug_layer.store(true);
  g_sdk->logger->Warn(g_handle, "DEV: d3d12_debug - the D3D12 DEBUG LAYER is on (slow): a failed RT creation logs why");
}

uint64_t baked_mask() { return g_baked.load(std::memory_order_acquire); }

bool attach() {
  if (!attach_hook(kCreateStateObject, kContainerCreateRet, reinterpret_cast<void*>(&d_create),
                   reinterpret_cast<void**>(&g_orig), "the state-object creator"))
    return false;
  g_attached = true;
  if (g_n.load() || g_nhg.load())
    g_sub_attached = attach_hook(kSubobjectAdd, kSubobjectAddLibRet, reinterpret_cast<void*>(&d_sub),
                                 reinterpret_cast<void**>(&g_orig_sub), "the state-object creator's subobject append");
  if (g_nhg.load())
    g_sdk->logger->InfoF(g_handle, "rt hit group: %u hit group(s) - %s", g_nhg.load(),
                         g_sub_attached ? "ADDED to the engine's collections by payload size" : "NOT used (hook failed)");
  // Before InitDevice's body: the pipeline options are set there, before any state object exists.
  // The driver's function first (every caller); the engine's InitDevice call site alone is the fallback.
  if (lss_wanted())
    g_pso_attached = attach_pso_driver() ||
                     attach_hook(kNvSetCreatePsoOptions, kNvSetCreatePsoOptionsRet, reinterpret_cast<void*>(&d_pso_options),
                                 reinterpret_cast<void**>(&g_orig_pso), "NvAPI_D3D12_SetCreatePipelineStateOptions (LSS)");
  g_sdk->logger->InfoF(g_handle, "rt swap: %u raygen variant(s) - their libraries %s; libraries without variants %s",
                       g_nvar.load(), g_sub_attached ? "ADDED next to the engine's" : "NOT used (hook failed)",
                       g_sub_attached ? "SWAPPED in the subobject list (no lock)" : "NOT used (hook failed)");
  const uint32_t n = g_n.load();
  g_sdk->logger->InfoF(g_handle, "rt swap: ATTACHED (state-object creator 0x%llx) - %u libraries%s",
                       static_cast<unsigned long long>(0x140000000ull + kCreateStateObject), n, verbose() ? ":" : "");
  for (uint32_t i = 0; i < n && verbose(); ++i)
    if (!g_libs[i].refused) g_sdk->logger->InfoF(g_handle, "rt swap:   %s", g_libs[i].name);
  return true;
}

void detach() {
  if (g_pso_attached) {
    if (g_driver_pso) g_sdk->hooking->Detach(g_handle, g_driver_pso);
    else detach_hook(kNvSetCreatePsoOptions);
  }
  g_pso_attached = false;
  g_driver_pso = nullptr;
  if (g_sub_attached) detach_hook(kSubobjectAdd);
  if (g_attached) detach_hook(kCreateStateObject);
  g_attached = g_sub_attached = false;
}


uint32_t claimed(const uint8_t* hash16) {
  if (!g_attached || !hash16) return 0;
  const uint32_t n = g_n.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n; ++i)
    if (!g_libs[i].refused && std::memcmp(g_libs[i].hash.data(), hash16, 16) == 0) return 1;
  return 0;
}

}  // namespace core::rtlib
