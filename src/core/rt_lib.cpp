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
  ID3D12GraphicsCommandList2* cl = mark_create() ? mark_list() : nullptr;
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

void d_rays(uint32_t so, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8,
            uint32_t buffer) {
  // Entered from the hooked function's entry: our return address is the engine caller's.
  prof::Scope prof_scope(prof::kRays);
  if (g_rays_census)
    graph_probe::on_dispatch_rays(so, a2, a3, a5,
                                  static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base - 5));
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
  const bool marks = ::crash_log::dred_armed();  // DispatchRays markers for a device-removal report
  if (!census && !variants && !timed && !marks) return;
  g_rays_attached = attach_hook(kDispatchRays, kDispatchRaysRet, reinterpret_cast<void*>(&d_rays),
                                reinterpret_cast<void**>(&g_orig_rays), "DispatchRays (global)");
  g_rays_census = g_rays_attached && census;
  g_marks_on = g_rays_attached && marks;
  if (g_marks_on) ::crash_log::set_extra_dump(&mark_dump);
  if (g_marks_on) attach_as();
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
