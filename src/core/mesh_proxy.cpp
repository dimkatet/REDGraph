// Mesh proxies (2026-10-31; docs/plan-mesh-proxy.md): the engine's mesh render proxies (CRenderProxy_Mesh) to the
// clients - which component / entity / mesh a proxy is, its own chunk table, and a per-proxy switch that takes chunks
// out of passes through the engine's OWN test (the chunk's render mask, EMeshChunkFlags, in the proxy's
// MaterialLodGroups). And, behind [Diagnostics] MeshProxyCensus, a log of every skinned component's proxy as [proxy].
//
// How a proxy is known: a component's SetupRenderProxy(this, initData&) (skinned 0x4b2d80 - the garment's calls it
// too - and plain 0x7156d4) fills the init data on its caller's stack, then CreateProxy runs the proxy ctor with
// RenderProxyInitInfo + 0 == that init data (run 2026-10-31: 82 matched this way, 0 by the CRenderMesh). The ctor's
// return gives the proxy; its dtor (from ??_G) ends it. Morph target components (heads), the distant crowd, world
// nodes and foliage make proxies another way: not seen here.
// The chunk table (MaterialLodGroups::Apply, the ctor's only - a new array per proxy): *(proxy + 0x118), 48 B per
// entry, per LOD l [byte(proxy + 0x122 + 2l) + i, i < byte(proxy + 0x123 + 2l)], LODs = byte(proxy + 0xf4); entry
// +0 CRenderMaterial* (its +0x130 = the template's short name, a CName: "hair"), +0x22 u16 mesh chunk index, +0x28 u16
// render mask. CollectAllLodModeInstances skips a chunk whose mask has none of the bits its pass asks for.
// Every read of engine memory is under SEH; writes are u16 stores into a live proxy's entries, under g_mx, which the
// dtor hook takes before the engine frees the proxy.
// RG_MCF_OUT_OF_BVH is ours, not a mask bit: a post-detour on the engine's per-frame TLAS instance desc writer
// (PrepareGpuGeometryAndMaterialProxyData) zeroes the InstanceMask of a proxy any subscription took out of the BVH,
// and puts a client's BLAS into the desc of a proxy it swapped (mesh_proxy_bvh_swap). The same writer paces
// mesh_proxy_bvh_retire: a resource is released only after a full frame of descs that no longer carry it.

#include <Windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bvh_probe.hpp"
#include "clients.hpp"
#include "engine_231.hpp"
#include "internal.hpp"  // first of the RED4ext headers (RED4EXT_HEADER_ONLY)

#include <RED4ext/RED4ext.hpp>

namespace core::mesh_proxy {
namespace {

using namespace e231;

constexpr uint32_t kMaxSubs = kMaxMeshSubs;  // RG_MAX_MESH_SUBS (limits.hpp)
constexpr uint32_t kSlots = 4096;     // live proxies of subscribed meshes
constexpr uint32_t kMaxLods = 8, kMaxEntries = 64;
constexpr uint32_t kCensusMax = 4000;  // distinct meshes in the census log (400 was hit while the world was still
                                       // streaming in, so the entity looked for never got a line - 2026-11-01)
constexpr uint32_t kOffOwner = 0x50;  // ent::IComponent::owner (RED4ext.SDK)

struct Sub {
  const char* name;
  rg_mesh_proxy_fn fn;
  void* user;
  uint32_t flags;                // RG_MPF_*
  std::vector<uint64_t> meshes;  // sorted; empty = every mesh
};
Sub g_subs[kMaxSubs];
std::atomic<uint32_t> g_nsubs{0};

struct Slot {
  uintptr_t proxy = 0;  // 0 = free
  uint32_t gen = 0;
  uint64_t mesh = 0;
  const void* comp = nullptr;
  const void* entity = nullptr;
  bool skinned = false;
  uint32_t subs = 0;         // the subscriptions that got CREATED
  uint64_t bits[kMaxSubs];   // per subscription: the chunks it hides...
  uint32_t flags[kMaxSubs];  // ... and what it takes from their masks
  uintptr_t table = 0;       // the chunk table the saved masks are from (0 = none saved)
  uint32_t n = 0;
  uint16_t orig[kMaxEntries];  // the engine's masks, entry order
};
Slot g_slot[kSlots];
std::unordered_map<uintptr_t, uint32_t> g_slot_of;  // proxy -> slot, g_mx
std::atomic<uint32_t> g_live{0};
std::mutex g_mx;
// Per slot: the render frame of its last scene collection << 32 | the collection indices of that frame (bit = index
// & 31), written by CollectElements on the job threads.
std::atomic<uint64_t> g_collected[kSlots];
// Per slot: out of the BVH (some subscription has RG_MCF_OUT_OF_BVH on some chunk), read by the instance desc writer on
// the job threads; g_bvh_out_n = how many slots are - 0 skips the lookup for every instance of the scene.
std::atomic<uint8_t> g_bvh_out[kSlots];
std::atomic<uint32_t> g_bvh_out_n{0};
// Per slot: a client's BLAS in its TLAS instance (mesh_proxy_bvh_swap; blas 0 = none) and the engine's own desc of it
// (mesh_proxy_bvh_desc: recorded once asked, `watch`). g_bvh_special_n = slots with either - with g_bvh_out_n, 0 skips
// the lookup. blas / contrib / flags / mask are ONE swap: bvh_swap writes them under `seq` (odd while it writes, g_mx
// held) and the desc writer reads them as a set, so a BLAS never goes out with another swap's contribution - a ray
// would run the records the client keeps for another geometry.
struct BvhSlot {
  std::atomic<uint32_t> seq{0};
  std::atomic<uint64_t> blas{0};
  std::atomic<uint32_t> contrib{0}, flags{0};  // flags: set | clear << 8
  std::atomic<uint32_t> mask{0};               // 0 = the engine's, else RG_BVH_MASK | the mask
  std::atomic<uint8_t> watch{0}, special{0};
  uint8_t owner = 0;  // the subscription (1-based) whose swap it is, 0 = none (g_mx)
  std::atomic<uint32_t> frame{0};
  std::atomic<uint64_t> swapped{0};
  uint8_t desc[64];  // the engine's desc, copied by the writer (a torn read is a diagnostic's problem only)
};
BvhSlot g_bvh[kSlots];
std::atomic<uint32_t> g_bvh_special_n{0};
std::atomic<uint64_t> g_n_bvh_swapped{0}, g_n_bvh_torn{0};
// rt_hit_records' engine_high: the highest InstanceContributionToHitGroupIndex the ENGINE's own descs carried, and the
// render frame it was first seen in. Measured in d_prepare, which exists only while some client needs the BVH.
std::atomic<uint32_t> g_desc_high{0}, g_desc_high_frame{0};
std::atomic<bool> g_swap_first{false};
std::atomic<uint32_t> g_swap_taken{0};  // swaps refused: another subscription's (logged: the first 4)

struct Swap {
  uint64_t blas;
  uint32_t contrib, flags, mask;
};
void write_swap(BvhSlot& b, const Swap& w) {  // g_mx held
  const uint32_t s = b.seq.load(std::memory_order_relaxed);
  b.seq.store(s + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  b.blas.store(w.blas, std::memory_order_relaxed);
  b.contrib.store(w.contrib, std::memory_order_relaxed);
  b.flags.store(w.flags, std::memory_order_relaxed);
  b.mask.store(w.mask, std::memory_order_relaxed);
  b.seq.store(s + 2, std::memory_order_release);
}
// false = a write in progress at every try: the engine's desc stays as it is this once (counted).
bool read_swap(const BvhSlot& b, Swap* out) {
  for (int t = 0; t < 4; ++t) {
    const uint32_t s = b.seq.load(std::memory_order_acquire);
    if (s & 1) {
      YieldProcessor();
      continue;
    }
    out->blas = b.blas.load(std::memory_order_relaxed);
    out->contrib = b.contrib.load(std::memory_order_relaxed);
    out->flags = b.flags.load(std::memory_order_relaxed);
    out->mask = b.mask.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (b.seq.load(std::memory_order_relaxed) == s) return true;
  }
  g_n_bvh_torn.fetch_add(1, std::memory_order_relaxed);
  return false;
}

// mesh_proxy_bvh_retire: resources a TLAS instance desc may still carry (a BLAS whose swap ended, the inputs its
// records read). The engine rewrites EVERY desc of the scene each render frame (d_prepare), so once render frame
// `frame + 2` writes descs, frame `frame + 1` wrote all of its own after the call - none of them carries a swap ended
// before it - and from there gpu_assets::retire's fence (the frame it is retired in and the next) covers every TLAS
// built from the older descs. While no desc is written (no RT) nothing is released: a TLAS is not rebuilt either.
// A resource some live swap still points INTO (a client that retired a BLAS without ending its swap) waits on: the
// count restarts the frame that swap ends - "no desc carries it" is checked, not assumed.
struct Retired {
  void* res;
  uint32_t frame;      // the render frame of the call, or of the last desc that still carried it
  uint64_t va, bytes;  // its GPU range (0 = not a buffer: only the frames count)
};
std::mutex g_retire_mx;
std::vector<Retired> g_retire;  // g_retire_mx
std::atomic<uint32_t> g_retire_n{0}, g_retire_drained{0};
std::atomic<uint64_t> g_n_retired{0};
std::atomic<bool> g_retire_held_said{false};
bool swapped_into(uint64_t va, uint64_t bytes) {
  if (!va || !g_bvh_special_n.load(std::memory_order_acquire)) return false;
  for (uint32_t s = 0; s < kSlots; ++s) {
    if (!g_bvh[s].special.load(std::memory_order_relaxed)) continue;
    const uint64_t b = g_bvh[s].blas.load(std::memory_order_relaxed);
    if (b >= va && b < va + bytes) return true;
  }
  return false;
}
void drain_retired(uint32_t rf) {  // the desc writer, once a render frame
  if (g_retire_drained.exchange(rf, std::memory_order_relaxed) == rf) return;
  std::lock_guard<std::mutex> lk(g_retire_mx);
  size_t k = 0;
  for (Retired& r : g_retire) {
    if (swapped_into(r.va, r.bytes)) {
      if (!g_retire_held_said.exchange(true))
        g_sdk->logger->WarnF(g_handle, "mesh proxies: a resource handed to mesh_proxy_bvh_retire is still a live swap's "
                             "BLAS (0x%llx) - held until that swap ends (a client should end the swap first)",
                             static_cast<unsigned long long>(r.va));
      r.frame = rf;
    }
    if (rf - r.frame >= 2u)
      gpu_assets::retire(r.res);
    else
      g_retire[k++] = r;
  }
  g_n_retired.fetch_add(g_retire.size() - k, std::memory_order_relaxed);
  g_retire.resize(k);
  g_retire_n.store(static_cast<uint32_t>(k), std::memory_order_release);
}
void update_special(uint32_t s) {  // g_mx held
  BvhSlot& b = g_bvh[s];
  const bool on = b.blas.load(std::memory_order_relaxed) || b.watch.load(std::memory_order_relaxed);
  if ((b.special.exchange(on ? 1 : 0, std::memory_order_relaxed) != 0) == on) return;
  if (on)
    g_bvh_special_n.fetch_add(1, std::memory_order_release);
  else
    g_bvh_special_n.fetch_sub(1, std::memory_order_relaxed);
}
void reset_bvh_slot(uint32_t s) {  // g_mx held: a new or dead proxy in the slot
  BvhSlot& b = g_bvh[s];
  write_swap(b, Swap{});
  b.owner = 0;
  b.watch.store(0, std::memory_order_relaxed);
  b.frame.store(0, std::memory_order_relaxed);
  b.swapped.store(0, std::memory_order_relaxed);
  update_special(s);
}
constexpr uint32_t kMaskFlags = 0xffffu;  // the engine's render-mask bits (u16); the rest are ours
void set_bvh_out(uint32_t s, bool out) {  // g_mx held
  if ((g_bvh_out[s].exchange(out ? 1 : 0, std::memory_order_relaxed) != 0) == out) return;
  if (out)
    g_bvh_out_n.fetch_add(1, std::memory_order_release);
  else
    g_bvh_out_n.fetch_sub(1, std::memory_order_relaxed);
}

// proxy -> slot for the hot path (CollectElements, every visible mesh proxy, every view): open addressing, inserted /
// erased under g_mx, read lock-free. Key 0 = empty, 1 = erased.
constexpr uint32_t kLook = 16384, kLookProbe = 64;
std::atomic<uintptr_t> g_lk[kLook];
std::atomic<uint32_t> g_lv[kLook];
uint32_t look_hash(uintptr_t p) { return static_cast<uint32_t>((uint64_t(p) >> 4) * 0x9E3779B97F4A7C15ull >> 50) & (kLook - 1); }
void look_put(uintptr_t p, uint32_t slot) {  // g_mx held
  uint32_t h = look_hash(p);
  for (uint32_t i = 0; i < kLookProbe; ++i, h = (h + 1) & (kLook - 1)) {
    const uintptr_t k = g_lk[h].load(std::memory_order_relaxed);
    if (k > 1 && k != p) continue;
    g_lv[h].store(slot, std::memory_order_relaxed);
    g_lk[h].store(p, std::memory_order_release);
    return;
  }
}
void look_erase(uintptr_t p) {  // g_mx held
  uint32_t h = look_hash(p);
  for (uint32_t i = 0; i < kLookProbe; ++i, h = (h + 1) & (kLook - 1)) {
    const uintptr_t k = g_lk[h].load(std::memory_order_relaxed);
    if (k == 0) return;
    if (k == p) {
      g_lk[h].store(1, std::memory_order_release);
      return;
    }
  }
}
uint32_t look_get(uintptr_t p) {  // ~0u = not ours
  uint32_t h = look_hash(p);
  for (uint32_t i = 0; i < kLookProbe; ++i, h = (h + 1) & (kLook - 1)) {
    const uintptr_t k = g_lk[h].load(std::memory_order_acquire);
    if (k == 0) return ~0u;
    if (k == p) return g_lv[h].load(std::memory_order_relaxed);
  }
  return ~0u;
}
uint32_t render_frame() { return *reinterpret_cast<const volatile uint32_t*>(g_base + kRenderFrame); }

// The setups: init data -> what set it up (g_mx). The init data lives on the caller's stack: an address is reused,
// every setup overwrites its entry (a plain mesh component's too).
struct Setup {
  uint64_t mesh;
  const void* comp;
  bool skinned;
};
std::unordered_map<uintptr_t, Setup> g_setup_of;

std::atomic<bool> g_census{false};
std::unordered_set<uint64_t> g_census_logged;  // g_mx
std::atomic<uint64_t> g_n_setups{0}, g_n_ctors{0}, g_n_created{0}, g_n_destroyed{0}, g_n_hides{0};

std::atomic<uint32_t> g_off_skinned{0}, g_off_mesh{0};  // the components' `mesh` (RTTI)
std::atomic<bool> g_rtti_done{false};
bool g_att_skinned = false, g_att_mesh = false, g_att_ctor = false, g_att_dtor = false, g_att_collect = false;
bool g_att_prepare = false;
std::atomic<uint64_t> g_n_collects{0}, g_n_bvh_masked{0};
std::atomic<bool> g_bvh_first{false};

void resolve_rtti() {
  if (g_rtti_done.exchange(true)) return;
  auto* rtti = RED4ext::CRTTISystem::Get();
  auto off = [&](const char* cls) -> uint32_t {
    auto* c = rtti ? rtti->GetClass(cls) : nullptr;
    auto* p = c ? c->GetProperty("mesh") : nullptr;
    return p ? p->valueOffset : 0;
  };
  g_off_skinned.store(off("entSkinnedMeshComponent"));
  g_off_mesh.store(off("entMeshComponent"));
  g_sdk->logger->InfoF(g_handle, "mesh proxies: component `mesh` at +0x%x (skinned) / +0x%x (mesh)",
                       g_off_skinned.load(), g_off_mesh.load());
}

// ---- SEH-guarded engine reads / writes (no C++ objects with destructors in these frames) ----
bool read_u64(uintptr_t a, uint64_t* out) {
  __try {
    *out = *reinterpret_cast<const uint64_t*>(a);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool copy_guarded(void* dst, uintptr_t src, size_t n) {
  __try {
    std::memcpy(dst, reinterpret_cast<const void*>(src), n);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

struct Entry {
  uint16_t chunk, mask;
  uint8_t lod;
  uint64_t material;  // CName
  uintptr_t mat, params;  // the entry's CRenderMaterial* (+0), CRenderMaterialParameters* (+8)
};
struct Table {
  uintptr_t ptr;
  uint32_t n;
  Entry e[kMaxEntries];
};
// The proxy's chunk table, every LOD; false = unreadable.
bool read_table(uintptr_t proxy, Table* t) {
  __try {
    const auto* p = reinterpret_cast<const uint8_t*>(proxy);
    t->ptr = *reinterpret_cast<const uintptr_t*>(p + 0x118);
    t->n = 0;
    const uint8_t lods = (std::min)(*(p + 0xf4), static_cast<uint8_t>(kMaxLods));
    for (uint8_t l = 0; l < lods && t->ptr; ++l) {
      const uint8_t start = *(p + 0x122 + 2 * l), count = *(p + 0x123 + 2 * l);
      for (uint32_t i = 0; i < count && t->n < kMaxEntries; ++i) {
        const auto* e = reinterpret_cast<const uint8_t*>(t->ptr) + 48ull * (start + i);
        Entry& o = t->e[t->n++];
        o.lod = l;
        o.chunk = *reinterpret_cast<const uint16_t*>(e + 0x22);
        o.mask = *reinterpret_cast<const uint16_t*>(e + 0x28);
        const uintptr_t mat = *reinterpret_cast<const uintptr_t*>(e);
        o.material = mat ? *reinterpret_cast<const uint64_t*>(mat + 0x130) : 0;
        o.mat = mat, o.params = *reinterpret_cast<const uintptr_t*>(e + 8);
      }
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
// Entry k's mask (in the order read_table reads them) <- masks[k]; false = unreadable.
bool write_masks(uintptr_t proxy, const uint16_t* masks, uint32_t n) {
  __try {
    const auto* p = reinterpret_cast<const uint8_t*>(proxy);
    const uintptr_t table = *reinterpret_cast<const uintptr_t*>(p + 0x118);
    const uint8_t lods = (std::min)(*(p + 0xf4), static_cast<uint8_t>(kMaxLods));
    uint32_t k = 0;
    for (uint8_t l = 0; l < lods && table; ++l) {
      const uint8_t start = *(p + 0x122 + 2 * l), count = *(p + 0x123 + 2 * l);
      for (uint32_t i = 0; i < count && k < n; ++i, ++k)
        *reinterpret_cast<volatile uint16_t*>(table + 48ull * (start + i) + 0x28) = masks[k];
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// The RTTI class name hash of an engine object (an ISerializable), 0 = unreadable.
uint64_t type_name_of(const void* obj) {
  __try {
    auto* type = static_cast<RED4ext::ISerializable*>(const_cast<void*>(obj))->GetType();
    return type ? type->GetName().hash : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

const char* name_of(uint64_t h) {
  if (!h) return "-";
  const char* s = RED4ext::CName(h).ToString();
  return s && *s ? s : "?";
}

rg_handle handle_of(uint32_t slot) { return static_cast<rg_handle>((g_slot[slot].gen << 12) | slot); }
// g_mx held. nullptr = not a live proxy handle.
Slot* slot_of(rg_handle h) {
  if (h <= 0) return nullptr;
  const uint32_t s = static_cast<uint32_t>(h) & (kSlots - 1);
  Slot& x = g_slot[s];
  return x.proxy && x.gen == (static_cast<uint32_t>(h) >> 12) ? &x : nullptr;
}

// g_mx held. The slot's masks as its subscriptions want them now: the engine's own, minus every subscription's
// flags on its chunks. The engine's masks are saved at the first change (again if the proxy's table moved).
void apply(Slot& x) {
  uint64_t any = 0;
  for (uint32_t i = 0; i < kMaxSubs; ++i) any |= x.flags[i] & kMaskFlags ? x.bits[i] : 0;
  static thread_local Table t;
  if (!read_table(x.proxy, &t)) return;
  if (!any && !x.table) return;  // nothing hidden, nothing saved
  if (x.table != t.ptr || x.n != t.n) {
    x.table = t.ptr, x.n = t.n;
    for (uint32_t k = 0; k < t.n; ++k) x.orig[k] = t.e[k].mask;
  }
  uint16_t m[kMaxEntries];
  for (uint32_t k = 0; k < t.n; ++k) {
    uint32_t take = 0;
    for (uint32_t i = 0; i < kMaxSubs; ++i)
      if (t.e[k].chunk < 64 && (x.bits[i] >> t.e[k].chunk & 1)) take |= x.flags[i] & kMaskFlags;
    m[k] = static_cast<uint16_t>(x.orig[k] & ~take);
  }
  write_masks(x.proxy, m, t.n);
  g_n_hides.fetch_add(1, std::memory_order_relaxed);
  if (!any) x.table = 0, x.n = 0;  // back to the engine's: nothing saved any more
}

// g_mx held. Slot s out of the BVH as its subscriptions want it now.
void apply_bvh(uint32_t s) {
  const Slot& x = g_slot[s];
  bool out = false;
  for (uint32_t i = 0; i < kMaxSubs; ++i) out |= (x.flags[i] & RG_MCF_OUT_OF_BVH) && x.bits[i];
  set_bvh_out(s, out);
}

void census_log(uintptr_t proxy, const Slot& info) {
  {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_census_logged.size() >= kCensusMax || !g_census_logged.insert(info.mesh).second) return;
  }
  static thread_local Table t;
  if (!read_table(proxy, &t)) {
    g_sdk->logger->WarnF(g_handle, "[proxy] %p: chunk table unreadable", reinterpret_cast<void*>(proxy));
    return;
  }
  const char* ent = info.entity ? name_of(type_name_of(info.entity)) : "-";
  std::string line;
  char buf[256];
  std::snprintf(buf, sizeof buf, "[proxy] mesh %016llx %s component %p entity %p (%s) proxy %p | %u entries",
                static_cast<unsigned long long>(info.mesh), info.skinned ? "skinned" : "mesh", info.comp, info.entity,
                ent, reinterpret_cast<void*>(proxy), t.n);
  line = buf;
  for (uint32_t k = 0; k < t.n; ++k) {
    std::snprintf(buf, sizeof buf, "%s lod %u #%u mask %x %s;", k && t.e[k].lod == t.e[k - 1].lod ? "" : " ||",
                  t.e[k].lod, t.e[k].chunk, t.e[k].mask, name_of(t.e[k].material));
    line += buf;
  }
  g_sdk->logger->Info(g_handle, line.c_str());
}

// ---- hooks ----
void note_setup(const void* comp, const void* init, bool skinned) {
  g_n_setups.fetch_add(1, std::memory_order_relaxed);
  resolve_rtti();
  const uint32_t off = skinned ? g_off_skinned.load() : g_off_mesh.load();
  uint64_t mesh = 0;
  if (!init || !comp || !off || !read_u64(reinterpret_cast<uintptr_t>(comp) + off, &mesh)) return;
  std::lock_guard<std::mutex> lk(g_mx);
  g_setup_of[reinterpret_cast<uintptr_t>(init)] = {mesh, comp, skinned};
}

using SetupFn = void (*)(void*, void*);
using SetupMeshFn = void (*)(void*, bool, void*);  // ent::MeshComponent's: a bool before the init data
SetupFn g_orig_skinned = nullptr;
SetupMeshFn g_orig_mesh = nullptr;
void d_setup_skinned(void* comp, void* init) {
  g_orig_skinned(comp, init);
  note_setup(comp, init, true);
}
void d_setup_mesh(void* comp, bool b, void* init) {
  g_orig_mesh(comp, b, init);
  note_setup(comp, init, false);
}

void fire(uint32_t subs, uint32_t event, const rg_mesh_proxy_info& info) {
  for (uint32_t i = 0; i < kMaxSubs; ++i) {
    if (!(subs >> i & 1)) continue;
    clients::Running running(reinterpret_cast<const void*>(g_subs[i].fn));
    g_subs[i].fn(g_subs[i].user, event, &info);
  }
}

using CtorFn = void* (*)(void*, const void*);
CtorFn g_orig_ctor = nullptr;
void* d_ctor(void* self, const void* info) {
  void* r = g_orig_ctor(self, info);
  g_n_ctors.fetch_add(1, std::memory_order_relaxed);
  uint64_t init = 0;
  if (!self || !info || !read_u64(reinterpret_cast<uintptr_t>(info), &init) || !init) return r;
  Setup st;
  {
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_setup_of.find(static_cast<uintptr_t>(init));
    if (it == g_setup_of.end()) return r;
    st = it->second;
    g_setup_of.erase(it);  // one proxy per setup
  }
  Slot info_only;
  info_only.mesh = st.mesh, info_only.comp = st.comp, info_only.skinned = st.skinned;
  uint64_t owner = 0;
  if (read_u64(reinterpret_cast<uintptr_t>(st.comp) + kOffOwner, &owner)) info_only.entity = reinterpret_cast<const void*>(owner);
  const auto proxy = reinterpret_cast<uintptr_t>(self);
  if (g_census.load(std::memory_order_relaxed) && st.skinned) census_log(proxy, info_only);
  // the subscriptions that want this mesh
  uint32_t subs = 0;
  const uint32_t ns = g_nsubs.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < ns; ++i)
    if (g_subs[i].meshes.empty() || std::binary_search(g_subs[i].meshes.begin(), g_subs[i].meshes.end(), st.mesh))
      subs |= 1u << i;
  if (!subs) return r;
  static thread_local Table t;
  const uint32_t entries = read_table(proxy, &t) ? t.n : 0;
  rg_mesh_proxy_info ev{};
  ev.struct_size = sizeof ev;
  ev.mesh_hash = st.mesh, ev.component = st.comp, ev.entity = info_only.entity;
  ev.chunk_count = entries, ev.skinned = st.skinned ? 1u : 0u;
  {
    std::lock_guard<std::mutex> lk(g_mx);
    uint32_t s = 0;
    while (s < kSlots && g_slot[s].proxy) ++s;
    if (s == kSlots) {
      static std::atomic<bool> warned{false};
      if (!warned.exchange(true))
        g_sdk->logger->WarnF(g_handle, "mesh proxies: %u live proxies of subscribed meshes - the rest NOT reported", kSlots);
      return r;
    }
    Slot& x = g_slot[s];
    const uint32_t gen = (x.gen + 1) & 0x7ffff;
    x = Slot{};
    x.gen = gen ? gen : 1;
    x.proxy = proxy, x.mesh = st.mesh, x.comp = st.comp, x.entity = ev.entity, x.skinned = st.skinned, x.subs = subs;
    g_slot_of[proxy] = s;
    g_collected[s].store(0, std::memory_order_relaxed);
    set_bvh_out(s, false);
    reset_bvh_slot(s);
    look_put(proxy, s);
    g_live.fetch_add(1, std::memory_order_relaxed);
    ev.proxy = handle_of(s);
  }
  g_n_created.fetch_add(1, std::memory_order_relaxed);
  fire(subs, RG_MESH_PROXY_CREATED, ev);
  return r;
}

using DtorFn = void (*)(void*);
DtorFn g_orig_dtor = nullptr;
void d_dtor(void* self) {
  if (g_live.load(std::memory_order_relaxed)) {
    rg_mesh_proxy_info ev{};
    uint32_t subs = 0;
    {
      std::lock_guard<std::mutex> lk(g_mx);
      auto it = g_slot_of.find(reinterpret_cast<uintptr_t>(self));
      if (it != g_slot_of.end()) {
        Slot& x = g_slot[it->second];
        ev.struct_size = sizeof ev;
        ev.proxy = handle_of(it->second);
        ev.mesh_hash = x.mesh, ev.component = x.comp, ev.entity = x.entity, ev.skinned = x.skinned ? 1u : 0u;
        subs = x.subs;
        x.proxy = 0;  // the handle is dead from here: a hide in the callback answers RG_E_ARG
        set_bvh_out(it->second, false);
        reset_bvh_slot(it->second);
        look_erase(reinterpret_cast<uintptr_t>(self));
        g_slot_of.erase(it);
        g_live.fetch_sub(1, std::memory_order_relaxed);
      }
    }
    if (subs) {
      g_n_destroyed.fetch_add(1, std::memory_order_relaxed);
      fire(subs, RG_MESH_PROXY_DESTROYED, ev);
    }
  }
  g_orig_dtor(self);
}

// The scene collection of a proxy in one view: for ours, this frame's bit of that view (a hidden chunk does not stop it
// - the proxy is still collected, its chunk loop just skips the chunk).
using CollectFn = void (*)(void*, void*);
CollectFn g_orig_collect = nullptr;
void d_collect(void* self, void* ctx) {
  if (g_live.load(std::memory_order_relaxed)) {
    const uint32_t s = look_get(reinterpret_cast<uintptr_t>(self));
    if (s < kSlots && ctx) {
      const auto* collector = *reinterpret_cast<const uint8_t* const*>(static_cast<const uint8_t*>(ctx) + 0x18);
      const uint32_t index = collector ? *(collector + kCollectorIndex) & 31u : 0u;
      const uint64_t frame = render_frame();
      uint64_t v = g_collected[s].load(std::memory_order_relaxed), want;
      do {
        want = (v >> 32 == frame ? v : frame << 32) | (1ull << index);
      } while (want != v && !g_collected[s].compare_exchange_weak(v, want, std::memory_order_relaxed));
      g_n_collects.fetch_add(1, std::memory_order_relaxed);
    }
  }
  g_orig_collect(self, ctx);
}

// One TLAS instance desc, every instance of the scene every frame (job threads): after the engine wrote it, a proxy out
// of the BVH gets InstanceMask 0 (the desc's byte +0x33). The proxy is read where the engine itself just read it.
using PrepareFn = uint64_t (*)(void*, const void*, uint64_t, void*, uint64_t, uint64_t, uint64_t, uint64_t);
PrepareFn g_orig_prepare = nullptr;
uint64_t d_prepare(void* self, const void* inst, uint64_t contribution, void* desc, uint64_t a5, uint64_t a6, uint64_t a7,
                   uint64_t a8) {
  const uint64_t r = g_orig_prepare(self, inst, contribution, desc, a5, a6, a7, a8);
  if (g_retire_n.load(std::memory_order_acquire)) drain_retired(render_frame());
  if ((g_bvh_out_n.load(std::memory_order_acquire) || g_bvh_special_n.load(std::memory_order_acquire)) && inst &&
      desc) {
    const uintptr_t proxy = *reinterpret_cast<const uintptr_t*>(static_cast<const uint8_t*>(inst) + 8);
    const uint32_t s = proxy ? look_get(proxy) : ~0u;
    BvhSlot* b = s < kSlots && g_bvh[s].special.load(std::memory_order_relaxed) ? &g_bvh[s] : nullptr;
    if (b && b->watch.load(std::memory_order_relaxed)) {
      std::memcpy(b->desc, desc, 64);
      b->frame.store(render_frame(), std::memory_order_release);
    }
    Swap w{};
    if (b && !read_swap(*b, &w)) w = Swap{};
    const uint64_t blas = w.blas;
    if (blas) {  // the client's geometry in this instance: BLAS, contribution, flags (transform / ID / mask stay)
      auto* d = static_cast<uint8_t*>(desc);
      const uint32_t was = *reinterpret_cast<uint32_t*>(d + 0x34);
      const uint32_t flags = ((was >> 24) | (w.flags & 0xff)) & ~(w.flags >> 8) & 0xff;
      *reinterpret_cast<uint32_t*>(d + 0x34) = (w.contrib & 0xffffffu) | flags << 24;
      *reinterpret_cast<uint64_t*>(d + 0x38) = blas;
      if (w.mask & RG_BVH_MASK) d[0x33] = static_cast<uint8_t>(w.mask);
      b->swapped.fetch_add(1, std::memory_order_relaxed);
      g_n_bvh_swapped.fetch_add(1, std::memory_order_relaxed);
      if (!g_swap_first.exchange(true))
        g_sdk->logger->InfoF(g_handle, "mesh proxies: first TLAS instance with a client's BLAS - proxy %p (slot %u), "
                             "InstanceID %u, mask 0x%02x, contribution %u -> %u, flags 0x%02x -> 0x%02x, BLAS 0x%llx",
                             reinterpret_cast<void*>(proxy), s, *reinterpret_cast<uint32_t*>(d + 0x30) & 0xffffffu,
                             d[0x33], was & 0xffffffu, w.contrib & 0xffffffu, was >> 24, flags,
                             static_cast<unsigned long long>(blas));
    } else if (s < kSlots && g_bvh_out[s].load(std::memory_order_relaxed)) {
      auto* mask = static_cast<uint8_t*>(desc) + 0x33;
      const uint8_t was = *mask;
      *mask = 0;
      g_n_bvh_masked.fetch_add(1, std::memory_order_relaxed);
      if (!g_bvh_first.exchange(true))
        g_sdk->logger->InfoF(g_handle, "mesh proxies: first TLAS instance taken out of the BVH - proxy %p (slot %u), "
                             "InstanceMask was 0x%02x, InstanceID %u, hit-group contribution %u",
                             reinterpret_cast<void*>(proxy), s, was,
                             *reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(desc) + 0x30) & 0xffffffu,
                             *reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(desc) + 0x34) & 0xffffffu);
    }
    // rt_hit_records' engine_high (rg_hit_records): the highest contribution an ENGINE desc carried - a client's own
    // records belong above it. Our swapped descs are ours now, so only the others count. One relaxed load a desc, and a
    // store only when the high-water grows: the writer runs once per scene instance per frame (~78k).
    if (!blas) {
      const uint32_t c = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(desc) + 0x34) & 0xffffffu;
      uint32_t h = g_desc_high.load(std::memory_order_relaxed);
      while (c > h && !g_desc_high.compare_exchange_weak(h, c, std::memory_order_relaxed)) {}
      if (h < c) g_desc_high_frame.store(render_frame(), std::memory_order_relaxed);
    }
    // [Diagnostics] BvhProbe: one write of this proxy's desc, with what it carries now - how many a frame is the count
    // that tells a second instance-desc array (another view) from one, docs/plan-mesh-proxy.md.
    if (b && bvh_probe::on())
      bvh_probe::on_desc(s, reinterpret_cast<const void*>(proxy), static_cast<const uint8_t*>(desc), blas != 0);
  }
  return r;
}

}  // namespace

bool census_on() { return g_census.load(std::memory_order_relaxed); }

void enable_census() {
  g_census.store(true);
  g_sdk->logger->Info(g_handle, "[proxy] census ON - every skinned component's proxy and its chunk table, once per mesh");
}

void desc_high(uint32_t* high, uint32_t* frame) {
  if (high) *high = g_desc_high.load(std::memory_order_relaxed);
  if (frame) *frame = g_desc_high_frame.load(std::memory_order_relaxed);
}

rg_handle subscribe(const char* name, const uint64_t* meshes, uint32_t count, rg_mesh_proxy_fn fn, void* user,
                    uint32_t flags) {
  if (!registration_open()) return RG_E_LATE;
  if (!fn || (count && !meshes)) return RG_E_ARG;
  if (flags & ~RG_MPF_NEEDS_BVH) return RG_E_ARG;  // a bit the host does not know: the client wants something else
  static std::mutex m;
  std::lock_guard<std::mutex> lock(m);
  const uint32_t i = g_nsubs.load();
  if (i >= kMaxSubs) return RG_E_FULL;
  Sub& s = g_subs[i];
  s.name = name && *name ? name : "?";
  s.fn = fn, s.user = user, s.flags = flags;
  s.meshes.assign(meshes, meshes + count);
  std::sort(s.meshes.begin(), s.meshes.end());
  g_nsubs.store(i + 1, std::memory_order_release);
  g_sdk->logger->InfoF(g_handle, "mesh proxies: '%s' subscribed to %u mesh(es)%s%s", s.name, count,
                       count ? "" : " (every one)",
                       flags & RG_MPF_NEEDS_BVH ? " - needs the BVH: the instance descs are edited"
                                                : " - chunks only: the instance descs are left to the engine");
  return static_cast<rg_handle>(i + 1);
}

int32_t chunks(rg_handle proxy, rg_mesh_chunk_info* out, uint32_t max) {
  std::lock_guard<std::mutex> lk(g_mx);
  Slot* x = slot_of(proxy);
  if (!x) return RG_E_ARG;
  static thread_local Table t;
  if (!read_table(x->proxy, &t)) return RG_E_UNAVAILABLE;
  for (uint32_t k = 0; k < t.n && k < max && out; ++k) {
    out[k].chunk = t.e[k].chunk, out[k].lod = t.e[k].lod;
    // the engine's own mask, not ours
    out[k].render_mask = x->table == t.ptr && k < x->n ? x->orig[k] : t.e[k].mask;
    out[k].pad = 0;
    out[k].material = t.e[k].material;
  }
  return static_cast<int32_t>(t.n);
}

int32_t material(rg_handle proxy, uint32_t chunk, uint32_t lod, void** mat, void** params) {
  if (mat) *mat = nullptr;
  if (params) *params = nullptr;
  std::lock_guard<std::mutex> lk(g_mx);  // the dtor takes it: the proxy stays alive while we read it
  Slot* x = slot_of(proxy);
  if (!x) return RG_E_ARG;
  static thread_local Table t;
  if (!read_table(x->proxy, &t)) return RG_E_UNAVAILABLE;
  for (uint32_t k = 0; k < t.n; ++k)
    if (t.e[k].chunk == chunk && t.e[k].lod == lod) {
      if (mat) *mat = reinterpret_cast<void*>(t.e[k].mat);
      if (params) *params = reinterpret_cast<void*>(t.e[k].params);
      return 0;
    }
  return RG_E_ARG;
}

int32_t hide(rg_handle sub, rg_handle proxy, uint64_t chunk_bits, uint32_t flags) {
  if (sub <= 0 || static_cast<uint32_t>(sub) > g_nsubs.load(std::memory_order_acquire)) return RG_E_ARG;
  std::lock_guard<std::mutex> lk(g_mx);
  Slot* x = slot_of(proxy);
  if (!x || !(x->subs >> (sub - 1) & 1)) return RG_E_ARG;
  const uint32_t i = static_cast<uint32_t>(sub - 1);
  x->bits[i] = flags ? chunk_bits : 0;
  x->flags[i] = chunk_bits ? flags : 0;
  apply(*x);
  apply_bvh(static_cast<uint32_t>(x - g_slot));
  return (flags & RG_MCF_OUT_OF_BVH) && chunk_bits && !g_att_prepare ? RG_E_UNAVAILABLE : 0;
}

int32_t frame(rg_handle proxy, rg_mesh_proxy_frame* out) {
  // 2026-11-03: an OLDER struct_size is fine - the fields it does not cover are not written (RG_MESH_PROXY_FRAME_V1 is
  // the layout before local_to_world, appended the same day; a client built then keeps working).
  if (!out || out->struct_size < RG_MESH_PROXY_FRAME_V1) return RG_E_ARG;
  std::lock_guard<std::mutex> lk(g_mx);  // the dtor takes it: the proxy stays alive while we read it
  Slot* x = slot_of(proxy);
  if (!x) return RG_E_ARG;
  const uint64_t c = g_collected[static_cast<uint32_t>(proxy) & (kSlots - 1)].load(std::memory_order_relaxed);
  out->frame = render_frame();
  out->collected_frame = static_cast<uint32_t>(c >> 32);
  out->collected_views = static_cast<uint32_t>(c);
  if (out->struct_size < sizeof(rg_mesh_proxy_frame)) return 0;  // no room for the transform
  return copy_guarded(out->local_to_world, x->proxy + 0x18, sizeof out->local_to_world) ? 0 : RG_E_UNAVAILABLE;
}

int32_t bvh_swap(rg_handle sub, rg_handle proxy, const rg_bvh_swap* swap) {
  if (sub <= 0 || static_cast<uint32_t>(sub) > g_nsubs.load(std::memory_order_acquire)) return RG_E_ARG;
  if (swap && (swap->struct_size < sizeof(rg_bvh_swap) || !swap->blas)) return RG_E_ARG;
  std::lock_guard<std::mutex> lk(g_mx);
  Slot* x = slot_of(proxy);
  if (!x || !(x->subs >> (sub - 1) & 1)) return RG_E_ARG;
  const uint32_t s = static_cast<uint32_t>(x - g_slot);
  BvhSlot& b = g_bvh[s];
  // One owner per instance, as one BLAS per instance: two subscriptions' swaps cannot compose (hide's flags do).
  if (b.owner && b.owner != static_cast<uint8_t>(sub)) {
    if (g_swap_taken.fetch_add(1, std::memory_order_relaxed) < 4)
      g_sdk->logger->WarnF(g_handle, "mesh proxies: proxy %p (slot %u) - swap of subscription '%s' REFUSED, the TLAS "
                           "instance carries subscription '%s''s BLAS until it ends that swap",
                           reinterpret_cast<void*>(x->proxy), s, g_subs[sub - 1].name, g_subs[b.owner - 1].name);
    return RG_E_TAKEN;
  }
  Swap was{};
  read_swap(b, &was);
  Swap now{};
  if (swap) {
    now.blas = swap->blas;
    now.contrib = swap->contribution & 0xffffffu;
    now.flags = (swap->flags_set & 0xff) | (swap->flags_clear & 0xff) << 8;
    now.mask = swap->mask & RG_BVH_MASK ? RG_BVH_MASK | (swap->mask & 0xffu) : 0u;
  }
  write_swap(b, now);
  b.owner = swap ? static_cast<uint8_t>(sub) : 0;
  update_special(s);
  // [hot]: a client may call it every frame. A line for what a ray sees differently - on / off, the records, the flags,
  // the mask; a new BLAS address alone (a rebuild) only with VerboseLog.
  const bool news = !was.blas != !now.blas ||
                    (now.blas && (was.contrib != now.contrib || was.flags != now.flags || was.mask != now.mask));
  if (news || (verbose() && was.blas != now.blas))
    g_sdk->logger->InfoF(g_handle, "mesh proxies: proxy %p (slot %u) TLAS instance %s", reinterpret_cast<void*>(x->proxy),
                         s, !swap ? "back to the engine's" : was.blas ? "with a client's BLAS (changed)"
                                                                       : "with a client's BLAS");
  return g_att_prepare ? 0 : RG_E_UNAVAILABLE;
}

namespace {
// A client's pointer, guarded: a buffer gives its GPU range, anything else (or a bad pointer) only the frame count.
void buffer_range(void* resource, uint64_t* va, uint64_t* bytes) {
  __try {
    auto* res = static_cast<ID3D12Resource*>(resource);
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) *va = res->GetGPUVirtualAddress(), *bytes = d.Width;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *va = *bytes = 0;
  }
}
}  // namespace

void bvh_retire(void* resource) {
  if (!resource) return;
  if (!g_att_prepare) {  // no desc is ever edited: the plain fence retire is enough
    gpu_assets::retire(resource);
    return;
  }
  uint64_t va = 0, bytes = 0;
  buffer_range(resource, &va, &bytes);
  std::lock_guard<std::mutex> lk(g_retire_mx);
  g_retire.push_back({resource, render_frame(), va, bytes});
  g_retire_n.store(static_cast<uint32_t>(g_retire.size()), std::memory_order_release);
}

int32_t bvh_desc(rg_handle proxy, rg_bvh_desc* out) {
  // 2026-11-03: an OLDER struct_size is accepted and only what it holds is written: appending a field must not break a
  // client built before it (RG_BVH_DESC_V1 = the layout before `swapped`). Zeroing / writing past the caller's size would
  // corrupt an older client's stack, so the size is honoured field by field.
  if (!out || out->struct_size < RG_BVH_DESC_V1) return RG_E_ARG;
  std::lock_guard<std::mutex> lk(g_mx);
  Slot* x = slot_of(proxy);
  if (!x) return RG_E_ARG;
  const uint32_t s = static_cast<uint32_t>(x - g_slot);
  BvhSlot& b = g_bvh[s];
  if (!b.watch.exchange(1, std::memory_order_relaxed)) update_special(s);
  const uint32_t ss = out->struct_size;
  // Zero only what this host knows: an older caller's size (its stack is that small) or our own struct for a newer one.
  std::memset(out, 0, ss < sizeof(rg_bvh_desc) ? ss : sizeof(rg_bvh_desc));
  out->struct_size = ss;
  out->frame = b.frame.load(std::memory_order_acquire);
  if (ss >= sizeof(rg_bvh_desc)) out->swapped = b.swapped.load(std::memory_order_relaxed);
  if (!out->frame) return g_att_prepare ? 0 : RG_E_UNAVAILABLE;
  uint8_t d[64];
  std::memcpy(d, b.desc, 64);
  std::memcpy(out->transform, d, 48);
  uint32_t w30 = *reinterpret_cast<const uint32_t*>(d + 0x30);
  const uint32_t w34 = *reinterpret_cast<const uint32_t*>(d + 0x34);
  out->instance_id = w30 & 0xffffffu, out->mask = w30 >> 24;
  out->contribution = w34 & 0xffffffu, out->flags = w34 >> 24;
  std::memcpy(&out->blas, d + 0x38, 8);
  if (ss >= offsetof(rg_bvh_desc, applied_flags) + sizeof out->applied_flags) {
    // What the writer puts into the running desc (d_prepare): a live swap's RG_BVH_MASK override wins, else OUT_OF_BVH
    // zeroes it, else the engine's own; its flags_set / flags_clear are applied to the engine's flags and nothing else
    // (d_prepare's `flags = ((was >> 24) | set) & ~clear`, with the word at +0x34 - out of the BVH the flags stay). The
    // engine's stay in `mask` / `flags` above - a client that swaps needs them. Both fields are the tail of the struct,
    // so the one size check covers them.
    Swap w{};
    if (!read_swap(b, &w)) w = Swap{};
    const uint32_t emask = w30 >> 24, eflags = w34 >> 24;
    out->applied_mask = w.blas ? ((w.mask & RG_BVH_MASK) ? (w.mask & 0xffu) : emask)
                               : (g_bvh_out[s].load(std::memory_order_relaxed) ? 0u : emask);
    out->applied_flags = w.blas ? (((eflags | (w.flags & 0xffu)) & ~(w.flags >> 8)) & 0xffu) : eflags;
  }
  return 0;
}

bool needed() { return census_on() || g_nsubs.load() > 0; }

// 2026-11-03: the instance-desc hook is for the subscriptions that NEED the BVH (RG_MPF_NEEDS_BVH), not for every
// mesh-proxy subscription: it is a post-detour on a writer the engine calls once per scene instance per frame (~78k),
// and a client that only hides chunks gets nothing from it. A swap / desc / OUT_OF_BVH hide from a subscription without
// the flag answers RG_E_UNAVAILABLE, as the API says.
bool needs_bvh() {
  const uint32_t n = g_nsubs.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n; ++i)
    if (g_subs[i].flags & RG_MPF_NEEDS_BVH) return true;
  return false;
}

uint32_t bvh_retire_pending() { return g_retire_n.load(std::memory_order_acquire); }

void attach() {
  if (!needed()) return;
  g_att_skinned = attach_hook(kSkinnedSetupRenderProxy, kSkinnedSetupRenderProxyRet,
                              reinterpret_cast<void*>(&d_setup_skinned), reinterpret_cast<void**>(&g_orig_skinned),
                              "ent::SkinnedMeshComponent::SetupRenderProxy (mesh proxies)");
  g_att_mesh = attach_hook(kMeshSetupRenderProxy, kMeshSetupRenderProxyRet, reinterpret_cast<void*>(&d_setup_mesh),
                           reinterpret_cast<void**>(&g_orig_mesh), "ent::MeshComponent::SetupRenderProxy (mesh proxies)");
  g_att_ctor = attach_hook(kMeshProxyCtor, kMeshProxyCtorRet, reinterpret_cast<void*>(&d_ctor),
                           reinterpret_cast<void**>(&g_orig_ctor), "CRenderProxy_Mesh ctor (mesh proxies)");
  g_att_dtor = g_att_ctor && attach_hook(kMeshProxyDtor, kMeshProxyDtorRet, reinterpret_cast<void*>(&d_dtor),
                                         reinterpret_cast<void**>(&g_orig_dtor), "CRenderProxy_Mesh dtor (mesh proxies)");
  if (g_att_ctor && !g_att_dtor) {  // never hand out a proxy we cannot see die
    detach_hook(kMeshProxyCtor);
    g_att_ctor = false;
  }
  if (g_att_ctor)
    g_att_collect = attach_hook_head(kMeshProxyCollect, kMeshProxyCollectHead, sizeof kMeshProxyCollectHead,
                                     reinterpret_cast<void*>(&d_collect), reinterpret_cast<void**>(&g_orig_collect),
                                     "CRenderProxy_Mesh::CollectElements (mesh proxies)");
  if (g_att_ctor && needs_bvh())
    g_att_prepare = attach_hook_head(kPrepareInstanceDesc, kPrepareInstanceDescHead, sizeof kPrepareInstanceDescHead,
                                     reinterpret_cast<void*>(&d_prepare), reinterpret_cast<void**>(&g_orig_prepare),
                                     "the TLAS instance desc writer (mesh proxies, out of the BVH)");
  g_sdk->logger->InfoF(g_handle, "mesh proxies: SetupRenderProxy skinned %s, mesh %s; proxy ctor + dtor %s, "
                       "CollectElements %s, TLAS instance descs %s; %u subscription(s)%s",
                       g_att_skinned ? "HOOKED" : "NOT hooked", g_att_mesh ? "HOOKED" : "NOT hooked",
                       g_att_ctor ? "HOOKED" : "NOT hooked", g_att_collect ? "HOOKED" : "NOT hooked",
                       g_att_prepare ? "HOOKED" : (needs_bvh() ? "NOT hooked" : "not needed (no subscription needs the "
                                                                        "BVH)"),
                       g_nsubs.load(), census_on() ? ", census on" : "");
}

void detach() {
  if (g_att_prepare) detach_hook(kPrepareInstanceDesc);
  {  // no desc is edited from here: what waited for one goes to the fence retire
    std::lock_guard<std::mutex> lk(g_retire_mx);
    for (const Retired& r : g_retire) gpu_assets::retire(r.res);
    g_n_retired.fetch_add(g_retire.size(), std::memory_order_relaxed);
    g_retire.clear();
    g_retire_n.store(0, std::memory_order_release);
  }
  if (g_att_collect) detach_hook(kMeshProxyCollect);
  if (g_att_dtor) detach_hook(kMeshProxyDtor);
  if (g_att_ctor) detach_hook(kMeshProxyCtor);
  if (g_att_mesh) detach_hook(kMeshSetupRenderProxy);
  if (g_att_skinned) detach_hook(kSkinnedSetupRenderProxy);
  if (g_att_ctor || g_att_skinned)
    g_sdk->logger->InfoF(g_handle, "mesh proxies: %llu setups, %llu proxies; %llu created / %llu destroyed for the "
                         "subscriptions, %llu mask writes, %llu collections of theirs, %llu TLAS instances out of the BVH, "
                         "%llu with a client's BLAS (%llu left the engine's for a swap being written), %llu retired "
                         "after the descs",
                         static_cast<unsigned long long>(g_n_setups.load()), static_cast<unsigned long long>(g_n_ctors.load()),
                         static_cast<unsigned long long>(g_n_created.load()),
                         static_cast<unsigned long long>(g_n_destroyed.load()),
                         static_cast<unsigned long long>(g_n_hides.load()),
                         static_cast<unsigned long long>(g_n_collects.load()),
                         static_cast<unsigned long long>(g_n_bvh_masked.load()),
                         static_cast<unsigned long long>(g_n_bvh_swapped.load()),
                         static_cast<unsigned long long>(g_n_bvh_torn.load()),
                         static_cast<unsigned long long>(g_n_retired.load()));
  g_att_prepare = g_att_collect = g_att_dtor = g_att_ctor = g_att_mesh = g_att_skinned = false;
}

}  // namespace core::mesh_proxy
