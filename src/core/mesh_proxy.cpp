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

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "clients.hpp"
#include "engine_231.hpp"
#include "internal.hpp"  // first of the RED4ext headers (RED4EXT_HEADER_ONLY)

#include <RED4ext/RED4ext.hpp>

namespace core::mesh_proxy {
namespace {

using namespace e231;

constexpr uint32_t kMaxSubs = 16;
constexpr uint32_t kSlots = 4096;     // live proxies of subscribed meshes
constexpr uint32_t kMaxLods = 8, kMaxEntries = 64;
constexpr uint32_t kCensusMax = 400;  // distinct meshes in the census log
constexpr uint32_t kOffOwner = 0x50;  // ent::IComponent::owner (RED4ext.SDK)

struct Sub {
  const char* name;
  rg_mesh_proxy_fn fn;
  void* user;
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
std::atomic<uint64_t> g_n_collects{0};

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
  for (uint32_t i = 0; i < kMaxSubs; ++i) any |= x.flags[i] ? x.bits[i] : 0;
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
      if (t.e[k].chunk < 64 && (x.bits[i] >> t.e[k].chunk & 1)) take |= x.flags[i];
    m[k] = static_cast<uint16_t>(x.orig[k] & ~take);
  }
  write_masks(x.proxy, m, t.n);
  g_n_hides.fetch_add(1, std::memory_order_relaxed);
  if (!any) x.table = 0, x.n = 0;  // back to the engine's: nothing saved any more
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

}  // namespace

bool census_on() { return g_census.load(std::memory_order_relaxed); }

void enable_census() {
  g_census.store(true);
  g_sdk->logger->Info(g_handle, "[proxy] census ON - every skinned component's proxy and its chunk table, once per mesh");
}

rg_handle subscribe(const char* name, const uint64_t* meshes, uint32_t count, rg_mesh_proxy_fn fn, void* user) {
  if (!registration_open()) return RG_E_LATE;
  if (!fn || (count && !meshes)) return RG_E_ARG;
  static std::mutex m;
  std::lock_guard<std::mutex> lock(m);
  const uint32_t i = g_nsubs.load();
  if (i >= kMaxSubs) return RG_E_FULL;
  Sub& s = g_subs[i];
  s.name = name && *name ? name : "?";
  s.fn = fn, s.user = user;
  s.meshes.assign(meshes, meshes + count);
  std::sort(s.meshes.begin(), s.meshes.end());
  g_nsubs.store(i + 1, std::memory_order_release);
  g_sdk->logger->InfoF(g_handle, "mesh proxies: '%s' subscribed to %u mesh(es)%s", s.name, count, count ? "" : " (every one)");
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
  return 0;
}

int32_t frame(rg_handle proxy, rg_mesh_proxy_frame* out) {
  if (!out || out->struct_size < sizeof(rg_mesh_proxy_frame)) return RG_E_ARG;
  std::lock_guard<std::mutex> lk(g_mx);  // the dtor takes it: the proxy stays alive while we read it
  Slot* x = slot_of(proxy);
  if (!x) return RG_E_ARG;
  const uint64_t c = g_collected[static_cast<uint32_t>(proxy) & (kSlots - 1)].load(std::memory_order_relaxed);
  out->frame = render_frame();
  out->collected_frame = static_cast<uint32_t>(c >> 32);
  out->collected_views = static_cast<uint32_t>(c);
  return copy_guarded(out->local_to_world, x->proxy + 0x18, sizeof out->local_to_world) ? 0 : RG_E_UNAVAILABLE;
}

bool needed() { return census_on() || g_nsubs.load() > 0; }

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
  g_sdk->logger->InfoF(g_handle, "mesh proxies: SetupRenderProxy skinned %s, mesh %s; proxy ctor + dtor %s, "
                       "CollectElements %s; %u subscription(s)%s",
                       g_att_skinned ? "HOOKED" : "NOT hooked", g_att_mesh ? "HOOKED" : "NOT hooked",
                       g_att_ctor ? "HOOKED" : "NOT hooked", g_att_collect ? "HOOKED" : "NOT hooked", g_nsubs.load(),
                       census_on() ? ", census on" : "");
}

void detach() {
  if (g_att_collect) detach_hook(kMeshProxyCollect);
  if (g_att_dtor) detach_hook(kMeshProxyDtor);
  if (g_att_ctor) detach_hook(kMeshProxyCtor);
  if (g_att_mesh) detach_hook(kMeshSetupRenderProxy);
  if (g_att_skinned) detach_hook(kSkinnedSetupRenderProxy);
  if (g_att_ctor || g_att_skinned)
    g_sdk->logger->InfoF(g_handle, "mesh proxies: %llu setups, %llu proxies; %llu created / %llu destroyed for the "
                         "subscriptions, %llu mask writes, %llu collections of theirs",
                         static_cast<unsigned long long>(g_n_setups.load()), static_cast<unsigned long long>(g_n_ctors.load()),
                         static_cast<unsigned long long>(g_n_created.load()),
                         static_cast<unsigned long long>(g_n_destroyed.load()),
                         static_cast<unsigned long long>(g_n_hides.load()),
                         static_cast<unsigned long long>(g_n_collects.load()));
  g_att_collect = g_att_dtor = g_att_ctor = g_att_mesh = g_att_skinned = false;
}

}  // namespace core::mesh_proxy
