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

#include "core/engine_231.hpp"
#include "core/clients.hpp"
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
// One creation's edits of the subobject list: `swap` replaces the engine's library, `libs` go in after it.
struct AddCtx {
  bool armed = false;
  uint32_t n = 0;
  bool seen = false;
  uint32_t added = 0;  // bit i = libs[i] went in
  int32_t swap = -1;   // a library in place of the engine's, -1 none
  bool swapped = false;
  D3D12_DXIL_LIBRARY_DESC swap_desc{};  // must live until CreateStateObject returns - the creator runs on this thread
  AddLib libs[kMaxAdd];
};
thread_local AddCtx t_add;
uint32_t variant_exports(uint32_t lib, D3D12_EXPORT_DESC* out, uint32_t max);  // below, with the variants
std::atomic<uint64_t> g_said_dup{0}, g_said_noswap{0};

void d_sub(uint8_t* arr, const D3D12_STATE_SUBOBJECT* sub) {
  AddCtx& c = t_add;
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
// succeeded / failed) or 1 with `mask` = every registered library on this hash whose leases stand.
int create_shared(void* out, const uint32_t* in, uint32_t n, uint64_t* mask) {
  const auto pass = [&] { return g_orig(out, in) ? 0 : -1; };
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
  if (!m) return pass();
  *mask = m;
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
bool create_with(void* out, const uint32_t* in, uint32_t n, int32_t swap, uint64_t add) {
  AddCtx& c = t_add;
  c.n = 0;
  c.seen = false;
  c.added = 0;
  c.swap = -1;
  c.swapped = false;
  if (add) arm_add(add);
  if (swap >= 0) {
    c.swap = swap;
    c.swap_desc = {};
    c.swap_desc.DXILLibrary = {g_libs[swap].lib.bytes, g_libs[swap].lib.size};  // NumExports 0 = all, as the engine's
  }
  c.armed = c.n || c.swap >= 0;
  bool ok = g_orig(out, in);
  const uint32_t armed = c.n, added = c.added;
  const bool seen = c.seen, swapped = c.swapped;
  c.armed = false;
  c.n = 0;
  c.swap = -1;
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
                        "added) - retrying with the engine's alone (vanilla RT)", n, swapped ? 1u : 0u, armed);
  if (swap >= 0) g_libs[swap].failed.fetch_add(1, std::memory_order_relaxed);
  for (uint32_t i = 0; i < armed; ++i) g_libs[c.libs[i].lib].failed.fetch_add(1, std::memory_order_relaxed);
  ok = g_orig(out, in);
  g_sdk->logger->InfoF(g_handle, "rt swap: vanilla retry %s", ok ? "succeeded" : "FAILED too");
  return ok;
}

bool create(void* out, const uint32_t* in) {
  const uint32_t n = g_calls.fetch_add(1) + 1;
  uint64_t mask = 0;
  const int r = create_shared(out, in, n, &mask);
  if (r <= 0) return r == 0;
  if (!g_sub_attached) {  // swap and add both go through the subobject append
    for (uint32_t i = 0; i < g_n.load(std::memory_order_acquire); ++i)
      if ((mask >> i & 1) && !(g_said_noswap.fetch_or(1ull << i) >> i & 1))
        g_sdk->logger->WarnF(g_handle, "rt swap: %s NOT baked - the subobject hook is not attached (vanilla there)",
                             g_libs[i].name);
    return g_orig(out, in);
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
  return create_with(out, in, n, swap, add);
}

// The census (graph_probe) maps a DispatchRays to the library it runs: every successful creation reports its
// container entry, the library hash and the export names (UTF-16, consecutive, from [+0x10]; count = byte +6).
void forget_slot(const void* slot);  // below, with the identifier cache

bool d_create(void* out, const uint32_t* in) {
  t_baked = 0;
  forget_slot(out);  // `out` is the state-object slot DispatchRays reads (rt_entry): its cached identifiers go
  const bool ok = create(out, in);
  forget_slot(out);  // and anything cached while it was being built
  if (ok) note_entry(out, t_baked);
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
constexpr uint32_t kMaxVar = 64;  // 31 fog + 4 grass + the self-test
struct Var {
  uint32_t lib;
  wchar_t raygen[64], variant[64];
  char label[160];
  uint64_t node;  // CName hash, 0 = everywhere
  std::atomic<uint32_t> on{1};
  std::atomic<uint64_t> swapped{0}, out_of_scope{0}, off{0}, missing{0};
  std::atomic<bool> said_in{false}, said_out{false}, said_missing{false};
};
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

void d_rays(uint32_t so, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8,
            uint32_t buffer) {
  // Entered from the hooked function's entry: our return address is the engine caller's.
  prof::Scope prof_scope(prof::kRays);
  if (g_rays_census)
    graph_probe::on_dispatch_rays(so, a2, a3, a5,
                                  static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base - 5));
  t_ovr.set = false;
  if (g_gsgi_attached && g_nvar.load(std::memory_order_relaxed)) arm_variant(so, a4);
  ID3D12GraphicsCommandList* tcl = nullptr;
  const uint32_t timed = gpu_timing::active() ? time_rays(so, a4, &tcl) : 0;
  {
    prof::Pause engine;
    g_orig_rays(so, a2, a3, a4, a5, a6, a7, a8, buffer);
  }
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
    auto* dev = *reinterpret_cast<uint8_t* const*>(g_base + kDeviceData);
    const uint8_t* slot = dev ? dev + kRtStateObjects + size_t(id - 1) * kRtStateObjectStride : nullptr;
    ID3D12StateObjectProperties* props = nullptr;
    if (slot) std::memcpy(&props, slot + kRtEntryProps, sizeof props);
    const void* ident = props ? identifier(slot, props, v) : nullptr;
    if (ident) {
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

}  // namespace

rg_handle variant_add(rg_handle lib, const char* raygen, const char* variant, const char* node) {
  if (!registration_open()) return RG_E_LATE;
  if (lib <= 0 || static_cast<uint32_t>(lib) > g_n.load() || !raygen || !variant || !*raygen || !*variant ||
      std::strlen(raygen) >= 64 || std::strlen(variant) >= 64)
    return RG_E_ARG;
  const uint32_t n = g_nvar.load();
  if (n >= kMaxVar) return RG_E_FULL;
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
  if (!census && !variants && !timed) return;
  g_rays_attached = attach_hook(kDispatchRays, kDispatchRaysRet, reinterpret_cast<void*>(&d_rays),
                                reinterpret_cast<void**>(&g_orig_rays), "DispatchRays (global)");
  g_rays_census = g_rays_attached && census;
  if (g_rays_attached && variants)
    g_gsgi_attached = attach_hook(kGetShaderGroupIdentifier, kGpuDispatchRaysGsgiRet, reinterpret_cast<void*>(&d_gsgi),
                                  reinterpret_cast<void**>(&g_orig_gsgi), "GpuApi::GetShaderGroupIdentifier");
  g_sdk->logger->InfoF(g_handle, "rt swap: DispatchRays %s - census rows %s, raygen variants %s (%u)",
                       g_rays_attached ? "HOOKED" : "NOT hooked", g_rays_census ? "on" : "off",
                       g_gsgi_attached ? "ON" : (variants ? "NOT attached" : "none"), g_nvar.load());
}

void detach_rays() {
  if (g_gsgi_attached) detach_hook(kGetShaderGroupIdentifier);
  if (g_rays_attached) detach_hook(kDispatchRays);
  g_gsgi_attached = g_rays_attached = g_rays_census = false;
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

bool needed() { return g_n.load() != 0; }

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

uint64_t baked_mask() { return g_baked.load(std::memory_order_acquire); }

bool attach() {
  if (!attach_hook(kCreateStateObject, kContainerCreateRet, reinterpret_cast<void*>(&d_create),
                   reinterpret_cast<void**>(&g_orig), "the state-object creator"))
    return false;
  g_attached = true;
  if (g_n.load())
    g_sub_attached = attach_hook(kSubobjectAdd, kSubobjectAddLibRet, reinterpret_cast<void*>(&d_sub),
                                 reinterpret_cast<void**>(&g_orig_sub), "the state-object creator's subobject append");
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
