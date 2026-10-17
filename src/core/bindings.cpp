// M2 (docs/internals.md "Leases (M2)"): OUR descriptors reach a patched shader through
// the ENGINE'S OWN descriptor tables - no root-signature extension, zero DWORDs.
//
// GpuApi never binds a resource into the root signature directly: SFrameResources::BindDescriptor writes a CPU handle
// into the context's staging array and marks that table dirty; UpdateRootParams (right before every draw / dispatch)
// copies every dirty table into the engine's shader-visible ring and sets it. So before the engine's UpdateRootParams
// we stage OUR handle (a CBV / SRV / UAV in the host's CPU-only heap, on the engine's device) into the leased register
// when the bound shader is one of the lease's owners, and give the engine's previous handle back at the next other
// draw / dispatch.
//
// The broker side: a lease is (stage, kind, register) in space0 for a set of OWNERS - patches (our bytecode replaces
// the engine's) and / or companions (our CS dispatched right after the engine's, same root arguments). Every owner's
// blob must read that register (checked at registration), the vanilla shader must NOT (checked before the engine
// creates its first PSO - failure refuses the owner), and the engine's root-signature definition must cover it
// (checked at the first UpdateRootParams - failure: never staged, the patch never swaps, the companion never runs).
// Leases on the same register of DIFFERENT shaders share one slot group: staging is per draw, so they never meet.
//
// What a lease binds: a host cbuffer (CBV) or a VIEW - of a client-owned buffer (SRV / UAV / CBV, view_set_resource),
// of a client-owned texture (SRV / UAV, view_create_texture + view_set_resource), or of an ENGINE RenderFlow texture by
// name (view_create_texture with a flow handle): that one is resolved at every stage on the recording thread (M5's
// flow_texture) and a descriptor written into the view's ring once per resource per frame; not resolved = not staged.
// Every owner must DECLARE the register as the view's kind (PSV0 / RDAT ResourceKind) - a raw view for a
// ByteAddressBuffer, a Texture2DArray view for a Texture2DArray - checked at registration. A texture view has a NULL
// descriptor of its own dimension after its ring (what an RT lease stages while its source is not there).
//
// RT leases (owners = RT libraries, 2026-10-06): UpdateRootParams with rays = 1 (GpuApi::DispatchRays) clears the VS / PS
// "bound" flags and forces the CS one (0x141e3d3ae: r14b = 1), so the CS tables are what the raygens get. A DispatchRays
// carries no ShaderRef of its state object, so an RT lease stages at EVERY rays = 1 call; it is safe because every
// library the engine creates is checked (RDAT) not to declare the register, and a baked library of ours must always read
// OUR descriptor - never what the last compute dispatch left in that slot - so a source that is not ready stages a NULL
// descriptor. An SRV whose table is STATIC at that moment (BindStaticTable: a resource pack at t0) is not staged:
// BindDescriptor would turn it into a copied table whose other slots the staging array never held.
//
// A companion's leases stage at every dispatch of the engine shader it accompanies - whether or not the companion is
// requested for it: the vanilla shader does not read the register (the lease test), so the descriptor is inert there.
// companion_staged() then answers, from inside the dispatch, whether the table that dispatch uses holds ours.

#include <Windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "core/engine_blocks.hpp"
#include "core/dxbc.hpp"
#include "core/engine_231.hpp"
#include "core/clients.hpp"
#include "core/internal.hpp"
#include "core/prof.hpp"

namespace core::bindings {
namespace {

using namespace e231;

// Per rg_kind (0 CBV, 1 SRV, 2 UAV): the definition's byte map, its per-stage stride, the registers it covers,
// BindDescriptor's `range`, the register letter.
struct KindInfo {
  uintptr_t map;
  uint32_t stride, regs, range;
  char letter;
};
constexpr KindInfo kKinds[3] = {
    {kDefCbvMap, kDefCbvStride, 14, kRangeCbv, 'b'},
    {kDefSrvMap, kDefSrvStride, 128, kRangeSrv, 't'},
    {kDefUavMap, kDefUavStride, 19, kRangeUav, 'u'},
};

// ---- the host's descriptor heap (CPU-only: the engine's CopyDescriptors source) ----
std::once_flag g_build_once;
std::atomic<bool> g_ready{false};
uint32_t g_heap_kind = 0;  // 1 UPLOAD, 2 GPU_UPLOAD
ID3D12DescriptorHeap* g_heap = nullptr;
ID3D12Device* g_dev = nullptr;  // the engine's device, kept for views set after the build
uint64_t g_handle0 = 0, g_handle_end = 0;
uint32_t g_inc = 0;
uint32_t g_null_first = 0;  // three NULL descriptors after the rings: CBV, SRV (raw buffer), UAV (raw buffer)
SRWLOCK g_view_lock = SRWLOCK_INIT;  // views vs build()

bool ours(uint64_t h) { return h >= g_handle0 && h < g_handle_end; }
uint64_t handle_at(uint32_t i) { return g_handle0 + static_cast<uint64_t>(i) * g_inc; }
bool in_ring(uint64_t h, uint32_t first, uint32_t ring) { return h >= handle_at(first) && h < handle_at(first + ring); }

// ---- cbuffers ----
constexpr uint32_t kMaxCb = 8;
constexpr uint32_t kMaxRing = 16;
struct Cb {
  uint32_t bytes = 0, ring = 0;
  uint8_t* shadow = nullptr;  // the CPU copy of the current contents
  ID3D12Resource* buf = nullptr;
  uint8_t* mapped = nullptr;
  uint32_t first = 0;  // first descriptor of its ring in the host heap
  std::atomic<uint32_t> cur{0};
  std::atomic<bool> valid{false};  // filled at least once (or created with initial contents)
  int32_t feed = -1;               // rg_engine_block this cbuffer is fed from, -1 = none
  uint32_t feed_offset = 0, feed_size = 0;
  SRWLOCK lock = SRWLOCK_INIT;
};
Cb g_cb[kMaxCb];
std::atomic<uint32_t> g_ncb{0};

// ---- views: of client-owned buffers / textures, or of an engine RenderFlow texture (flow) ----
constexpr uint32_t kMaxViews = 16;
constexpr uint32_t kMaxViewRing = 8;
struct View {
  uint32_t kind = 0, stride = 0, ring = 0;
  uint32_t first = 0;
  std::atomic<uint32_t> cur{0};
  std::atomic<bool> valid{false};  // a descriptor of a live resource sits at `cur` (a flow view: the heap is built)
  ID3D12Resource* res = nullptr;   // AddRef'd
  uint64_t bytes = 0;
  uint32_t written = 0;  // descriptors written so far (the first goes to slot 0)
  bool tex = false;      // a texture view: `td`, and a NULL descriptor of its dimension at first + ring
  rg_texture_view_desc td{};
  // A flow view: the resource (and its engine id) the descriptor at `cur` was written for, and in which frame -
  // a transient is a new resource every frame, and an address may come back for another one.
  SRWLOCK flock = SRWLOCK_INIT;
  void* fres = nullptr;
  uint32_t fid = 0;
  uint64_t fframe = ~0ull;
  std::atomic<uint32_t> fbad{0};  // the resolved texture does not fit the view (logged once) - never staged again
};
View g_view[kMaxViews];
std::atomic<uint32_t> g_nview{0};

// ---- leases and slot groups ----
// Sizes with headroom, not a measured budget (raised 2026-10-11 for a client patching many material shaders through
// one lease). The bind path walks only what is REGISTERED (groups, their leases, a lease's learned refs), so a larger
// array costs memory, not time.
constexpr uint32_t kMaxLeases = 32;
constexpr uint32_t kMaxLeaseOwners = 16;
constexpr uint32_t kMaxRefs = 64;  // ShaderRefs a lease learns - one per engine shader of its owners it is bound with
constexpr uint32_t kMaxGroups = 32;
constexpr uint32_t kMaxPatchHandles = kMaxPatchCount;
constexpr uint32_t kMaxCompHandles = 8;
constexpr uint32_t kMaxLeasesPerOwner = 8;
static_assert(kMaxGroups <= 32, "FrSave::mask is a uint32_t bit per group");
static_assert(kMaxLeases <= 127, "g_lease_of_patch / g_lease_of_comp hold a lease index in an int8_t");

struct Group {
  uint32_t stage, kind, reg;
  uint32_t leases[kMaxLeases];
  uint32_t nleases;
  std::atomic<int32_t> idx{-1};
  std::atomic<int32_t> bucket{-1};  // the root-signature table (bucket) the register is in
  std::atomic<bool> broken{false};
  std::atomic<uint64_t> restores{0}, lost{0};
};
Group g_groups[kMaxGroups];
uint32_t g_ngroups = 0;

struct Lease {
  const char* name;
  uint32_t stage, kind, reg;
  rg_handle patches[kMaxLeaseOwners];
  uint32_t npatches;
  rg_handle comps[kMaxLeaseOwners];
  uint32_t ncomps;
  int32_t cb;    // index, -1 = a view lease
  int32_t view;  // index, -1 = a cbuffer lease
  uint32_t group;  // index
  std::atomic<uint32_t> state{RG_STATE_PENDING};
  std::atomic<uint32_t> refs[kMaxRefs] = {};
  std::atomic<uint32_t> nrefs{0};
  std::atomic<bool> refs_full{false};
  std::atomic<uint64_t> stages{0}, not_staged{0};
  uint64_t rt_mask = 0;  // RT library owners, bit (handle - 1)
  std::atomic<uint64_t> null_stages{0}, static_skips{0};
  std::atomic<uint32_t> rt_checked{0};
};
Lease g_leases[kMaxLeases];
std::atomic<uint32_t> g_nleases{0};

// Owner -> its leases (-1 = none); written at registration only. Verdict of the vanilla test: 0 unchecked, 1 ok,
// 2 refused.
int8_t g_lease_of_patch[kMaxPatchHandles][kMaxLeasesPerOwner];
int8_t g_lease_of_comp[kMaxCompHandles][kMaxLeasesPerOwner];
std::atomic<uint8_t> g_patch_ok[kMaxPatchHandles] = {};
std::atomic<uint8_t> g_comp_ok[kMaxCompHandles] = {};

struct LeaseOfInit {
  LeaseOfInit() {
    std::memset(g_lease_of_patch, -1, sizeof(g_lease_of_patch));
    std::memset(g_lease_of_comp, -1, sizeof(g_lease_of_comp));
  }
} g_lease_of_init;

std::atomic<uint64_t> g_frame{0};  // frame_tick()

uint64_t flow_handle(View& v, uint32_t vi);

// The source a lease binds: the descriptor to stage now (0 = none: a flow texture not resolved here), whether a handle
// is one of its ring, whether it is usable at all, the NULL descriptor an RT lease stages instead.
uint64_t src_handle(const Lease& l) {
  if (l.cb >= 0) return handle_at(g_cb[l.cb].first + g_cb[l.cb].cur.load(std::memory_order_acquire));
  View& v = g_view[l.view];
  if (v.td.flow > 0) return flow_handle(v, static_cast<uint32_t>(l.view));
  return handle_at(v.first + v.cur.load(std::memory_order_acquire));
}
uint64_t null_handle(const Lease& l) {
  if (l.view >= 0 && g_view[l.view].tex) return handle_at(g_view[l.view].first + g_view[l.view].ring);
  return handle_at(g_null_first + l.kind);
}
bool src_owns(const Lease& l, uint64_t h) {
  return l.cb >= 0 ? in_ring(h, g_cb[l.cb].first, g_cb[l.cb].ring) : in_ring(h, g_view[l.view].first, g_view[l.view].ring);
}
bool src_valid(const Lease& l) {
  return l.cb >= 0 ? g_cb[l.cb].valid.load(std::memory_order_relaxed) : g_view[l.view].valid.load(std::memory_order_relaxed);
}

// Per frame-resources: the engine's handle we displaced, per group, to give back. A context is used by one thread at
// a time. An entry is freed when its last group is given back - and a context the engine retires with one still
// pending never gives it back: its entry is taken over once unused for kFrStaleFrames (2026-10-16: a user session
// filled all 64 after 38 min, then every lease of every NEW context was LOST until the game was restarted). A context
// idle that long has rebound its tables since; what it loses is a handle to restore that it no longer reads.
constexpr uint32_t kMaxFr = 64;
constexpr uint64_t kFrStaleFrames = 600;
struct FrSave {
  std::atomic<uintptr_t> fr{0};
  std::atomic<uint64_t> used{0};  // g_frame of its last stage / restore
  uint64_t old[kMaxGroups];
  uint32_t mask;
};
FrSave g_fr[kMaxFr];
std::atomic<uint32_t> g_fr_live{0}, g_fr_peak{0};
std::atomic<uint64_t> g_fr_taken{0}, g_fr_full{0};

using UpdateRootParams_t = uint64_t (*)(void* fr, void* ctx, uint8_t rays);
using BindDescriptor_t = void (*)(void* fr, uint32_t type, uint32_t range, uint32_t slot,
                                  const D3D12_CPU_DESCRIPTOR_HANDLE* h, bool force);
UpdateRootParams_t g_orig_urp = nullptr;
BindDescriptor_t g_bind = nullptr;
bool g_attached = false;

const char* stage_name(uint32_t s) { return s == 0 ? "VS" : s == 1 ? "PS" : "CS"; }
char letter(uint32_t kind) { return kKinds[kind].letter; }

bool is_ref(const Lease& l, uint32_t id) {
  const uint32_t n = l.nrefs.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n && i < kMaxRefs; ++i)
    if (l.refs[i].load(std::memory_order_relaxed) == id) return true;
  return false;
}

bool lease_enabled(const Lease& l) {
  if (l.state.load(std::memory_order_relaxed) == RG_STATE_REFUSED) return false;
  if (l.ncomps) return true;  // a companion's lease stages at every dispatch of its engine shader (header)
  for (uint32_t i = 0; i < l.npatches; ++i)
    if (pso::patch_selected(l.patches[i])) return true;
  return false;
}

// The register's index into the staging array, as BindDescriptor computes it: byte map -> bucket -> base + slot - base
// register.
int32_t resolve_idx(const uint8_t* fr, uint32_t stage, uint32_t kind, uint32_t reg, int32_t* bucket_out) {
  const auto* def = *reinterpret_cast<const uint8_t* const*>(fr + kFrDef);
  if (!def) return -1;
  const KindInfo& k = kKinds[kind];
  const uint8_t p = def[k.map + stage * k.stride + reg * 2];
  if (p == 0xff) return -1;
  *bucket_out = p;
  const uint8_t* bucket = def + 0x38 + static_cast<size_t>(p) * 16;
  uint32_t first;
  uint16_t base_reg;
  std::memcpy(&first, bucket + 4, 4);
  std::memcpy(&base_reg, bucket + 8, 2);
  if (base_reg > reg) return -1;
  const int64_t idx = static_cast<int64_t>(first) - base_reg + reg;
  return (idx >= 0 && idx < 4096) ? static_cast<int32_t>(idx) : -1;
}

uint64_t* staging_slot(uint8_t* fr, int32_t idx) {
  auto* arr = *reinterpret_cast<uint64_t**>(fr + kFrStaging);
  return arr ? arr + idx : nullptr;
}

FrSave* find_fr(uintptr_t fr) {
  for (FrSave& s : g_fr)
    if (s.fr.load(std::memory_order_acquire) == fr) return &s;
  return nullptr;
}

bool pow2(uint64_t n) { return (n & (n - 1)) == 0; }

// A free entry, else one unused for kFrStaleFrames (see FrSave). nullptr = 64 contexts in use right now.
FrSave* claim_fr(uintptr_t fr) {
  const uint64_t now = g_frame.load(std::memory_order_relaxed);
  for (FrSave& f : g_fr) {
    uintptr_t expect = 0;
    if (f.fr.compare_exchange_strong(expect, fr, std::memory_order_acq_rel)) {
      f.mask = 0;
      f.used.store(now, std::memory_order_relaxed);
      const uint32_t live = g_fr_live.fetch_add(1, std::memory_order_relaxed) + 1;
      uint32_t peak = g_fr_peak.load(std::memory_order_relaxed);
      while (live > peak && !g_fr_peak.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {}
      if (live > peak && live >= 16)
        g_sdk->logger->InfoF(g_handle, "native table: %u contexts hold a displaced engine handle (a new peak; %u "
                             "entries; thread %lu, frame %llu)", live, kMaxFr,
                             static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned long long>(now));
      return &f;
    }
  }
  for (FrSave& f : g_fr) {
    uintptr_t old_fr = f.fr.load(std::memory_order_acquire);
    const uint64_t used = f.used.load(std::memory_order_relaxed);
    if (!old_fr || used + kFrStaleFrames > now) continue;
    if (!f.fr.compare_exchange_strong(old_fr, fr, std::memory_order_acq_rel)) continue;
    f.mask = 0;
    f.used.store(now, std::memory_order_relaxed);
    const uint64_t n = g_fr_taken.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 16 || pow2(n))
      g_sdk->logger->InfoF(g_handle, "native table: took over the entry of a context unused for %llu frames (#%llu; "
                           "%u entries in use; thread %lu, frame %llu)", static_cast<unsigned long long>(now - used),
                           static_cast<unsigned long long>(n), g_fr_live.load(std::memory_order_relaxed),
                           static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned long long>(now));
    return &f;
  }
  const uint64_t n = g_fr_full.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 16 || pow2(n))
    g_sdk->logger->ErrorF(g_handle, "native table: all %u context entries are in use - this lease is NOT staged here "
                          "(#%llu; thread %lu, frame %llu)", kMaxFr, static_cast<unsigned long long>(n),
                          static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned long long>(now));
  return nullptr;
}

// What stage() did: bound ours now / ours was there already / no staging array / FrSave full (the engine's stays).
enum StageOut : uint8_t { kStaged, kAlready, kNoSlot, kLost };
StageOut stage(uint8_t* fr, uint32_t gi, int32_t idx, Lease& l, uint64_t cur) {
  Group& g = g_groups[gi];
  uint64_t* slot = staging_slot(fr, idx);
  if (!slot) return kNoSlot;
  const uint64_t had = *slot;
  if (had == cur) return kAlready;
  if (!ours(had)) {  // remember the engine's handle (an older ring slot of ours is not one)
    FrSave* s = find_fr(reinterpret_cast<uintptr_t>(fr));
    if (s) s->used.store(g_frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
    else s = claim_fr(reinterpret_cast<uintptr_t>(fr));
    if (!s) {  // every entry in use right now - stage nothing rather than lose the engine's handle
      g.lost.fetch_add(1, std::memory_order_relaxed);
      return kLost;
    }
    s->old[gi] = had;
    s->mask |= 1u << gi;
  }
  const D3D12_CPU_DESCRIPTOR_HANDLE h{static_cast<SIZE_T>(cur)};
  g_bind(fr, g.stage, kKinds[g.kind].range, g.reg, &h, false);
  l.stages.fetch_add(1, std::memory_order_relaxed);
  return kStaged;
}

void restore(uint8_t* fr, uint32_t gi, int32_t idx) {
  Group& g = g_groups[gi];
  uint64_t* slot = staging_slot(fr, idx);
  if (!slot || !ours(*slot)) return;  // the engine has bound the register itself since - nothing of ours to take back
  FrSave* s = find_fr(reinterpret_cast<uintptr_t>(fr));
  if (!s || !(s->mask & (1u << gi))) return;
  const uint64_t old = s->old[gi];
  s->mask &= ~(1u << gi);
  if (!s->mask) {
    s->fr.store(0, std::memory_order_release);
    g_fr_live.fetch_sub(1, std::memory_order_relaxed);
  } else {
    s->used.store(g_frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
  }
  if (!old) return;  // never seen - cannot happen for a fully copied table; ours stays (a valid descriptor)
  const D3D12_CPU_DESCRIPTOR_HANDLE h{static_cast<SIZE_T>(old)};
  g_bind(fr, g.stage, kKinds[g.kind].range, g.reg, &h, false);
  g.restores.fetch_add(1, std::memory_order_relaxed);
}

// Once, at the first UpdateRootParams with a definition: which space0 registers the engine's tables cover, per stage
// and kind - what a lease can take (the header's b0-b13 / t0-t127 / u0-u18 is the definition's SIZE, not its coverage).
std::atomic<bool> g_coverage_logged{false};
void log_coverage(const uint8_t* fr) {
  const auto* def = *reinterpret_cast<const uint8_t* const*>(fr + kFrDef);
  if (!def || g_coverage_logged.exchange(true)) return;
  for (uint32_t stage = 0; stage < 3; ++stage) {
    char line[400];
    int n = std::snprintf(line, sizeof line, "native table: %s covers", stage_name(stage));
    for (uint32_t kind = 0; kind < 3; ++kind) {
      const KindInfo& k = kKinds[kind];
      for (uint32_t r = 0; r < k.regs && n < static_cast<int>(sizeof line) - 24;) {
        if (def[k.map + stage * k.stride + r * 2] == 0xff) {
          ++r;
          continue;
        }
        uint32_t e = r;
        while (e + 1 < k.regs && def[k.map + stage * k.stride + (e + 1) * 2] != 0xff) ++e;
        n += e == r ? std::snprintf(line + n, sizeof line - n, " %c%u", k.letter, r)
                    : std::snprintf(line + n, sizeof line - n, " %c%u-%c%u", k.letter, r, k.letter, e);
        r = e + 1;
      }
    }
    g_sdk->logger->Info(g_handle, line);
  }
}

// ---- RT lease delivery (2026-10-14: a client's raygen variant faulted reading VA 0 at a rare DispatchRays, ~once in
// 30 min; the only input it has that the engine's raygen lacks is an RT lease - docs/internals.md "RT lease delivery")
// GpuApi::DispatchRays calls UpdateRootParams(rays = 1) BEFORE GetShaderGroupIdentifier, on one thread. So per
// DispatchRays the recording thread keeps what every RT slot group got - and, AFTER the engine's UpdateRootParams has
// copied the tables, whether OUR descriptor still sits in the staging slot and the table is not a STATIC one (a static
// table is bound as it is, the staging array is not copied). rt_lib asks rt_delivered() before it hands out a variant:
// a variant whose lease did not land runs the engine's raygen for that one dispatch (counted, logged).
enum RtOut : uint8_t { kRtNotSeen, kRtOurs, kRtAlready, kRtNull, kRtStaticSkip, kRtLost, kRtNoSlot, kRtGone,
                       kRtStatic, kRtCount };
const char* const kRtOutName[kRtCount] = {"NOT STAGED", "staged", "already ours", "NULL staged", "skipped: STATIC table",
                                          "LOST (all context entries in use)", "no staging array",
                                          "GONE from the slot after the engine's copy",
                                          "the table stayed STATIC (ours not copied)"};
bool rt_good(uint8_t o) { return o == kRtOurs || o == kRtAlready || o == kRtNull; }
struct RtCall {
  bool seen;  // UpdateRootParams(rays = 1) ran since rt_begin()
  uint8_t out[kMaxGroups];
  int32_t idx[kMaxGroups];
  bool static_before[kMaxGroups];
  int32_t desc[kMaxGroups];  // after the engine's copy: the slot's descriptor, as an index into OUR heap; -1 = not ours
};
thread_local RtCall t_rt{};
std::atomic<uint64_t> g_rt_out[kRtCount] = {};
std::atomic<uint64_t> g_rt_static_before{0};  // the group's table was STATIC when we came (a CBV / UAV: we bind anyway)
std::atomic<uint32_t> g_rt_bad_logged{0};

void rt_after(uint8_t* fr) {
  const uint64_t statics = *reinterpret_cast<const uint64_t*>(fr + kFrStatic);
  for (uint32_t gi = 0; gi < g_ngroups; ++gi) {
    uint8_t& o = t_rt.out[gi];
    if (o == kRtNotSeen) continue;
    const uint64_t* slot = staging_slot(fr, t_rt.idx[gi]);
    t_rt.desc[gi] = slot && ours(*slot) && g_inc ? static_cast<int32_t>((*slot - g_handle0) / g_inc) : -1;
    if (rt_good(o)) {
      const int32_t p = g_groups[gi].bucket.load(std::memory_order_relaxed);
      if (!slot || !ours(*slot)) o = kRtGone;
      else if (p >= 0 && ((statics >> p) & 1)) o = kRtStatic;
    }
    g_rt_out[o].fetch_add(1, std::memory_order_relaxed);
    if (t_rt.static_before[gi]) g_rt_static_before.fetch_add(1, std::memory_order_relaxed);
    if (!rt_good(o) && g_rt_bad_logged.fetch_add(1, std::memory_order_relaxed) < 16) {
      const Group& g = g_groups[gi];
      g_sdk->logger->ErrorF(g_handle, "native table: RT lease %s %c%u at a DispatchRays: %s%s (thread %lu; logged 16 "
                            "times, counted)", stage_name(g.stage), letter(g.kind), g.reg, kRtOutName[o],
                            t_rt.static_before[gi] ? ", its table was STATIC before" : "",
                            static_cast<unsigned long>(GetCurrentThreadId()));
    }
  }
}

// Before the engine's own UpdateRootParams: the dirty bit BindDescriptor sets makes THIS call copy our handle.
uint64_t d_update_root_params(void* fr_v, void* ctx_v, uint8_t rays) {
  prof::Scope prof_scope(prof::kUrp);
  if (g_ready.load(std::memory_order_relaxed) && fr_v && ctx_v) {
    auto* fr = static_cast<uint8_t*>(fr_v);
    if (!g_coverage_logged.load(std::memory_order_relaxed)) log_coverage(fr);
    const auto* ctx = static_cast<const uint8_t*>(ctx_v);
    const uint64_t baked = rays ? rtlib::baked_mask() : 0;
    if (rays) {
      t_rt.seen = true;
      std::memset(t_rt.out, kRtNotSeen, sizeof t_rt.out);
      std::memset(t_rt.static_before, 0, sizeof t_rt.static_before);
    }
    for (uint32_t gi = 0; gi < g_ngroups; ++gi) {
      Group& g = g_groups[gi];
      int32_t idx = g.idx.load(std::memory_order_relaxed);
      if (idx < 0) {
        if (g.broken.load(std::memory_order_relaxed)) continue;
        int32_t bucket = -1;
        idx = resolve_idx(fr, g.stage, g.kind, g.reg, &bucket);
        if (idx < 0) {
          g.broken.store(true);
          g_sdk->logger->ErrorF(g_handle, "native table: the engine's root-signature definition does not cover %s %c%u "
                                "as expected - its patches stay on the engine's shader, its companions never run",
                                stage_name(g.stage), letter(g.kind), g.reg);
          continue;
        }
        g.bucket.store(bucket);
        g.idx.store(idx);
        g_sdk->logger->InfoF(g_handle, "native table: %s %c%u = staging index %d (table %d)", stage_name(g.stage),
                             letter(g.kind), g.reg, idx, bucket);
      }
      Lease* hit = nullptr;
      if (rays) {
        // DispatchRays: an RT lease with a baked owner, whatever the state object (see the header)
        for (uint32_t k = 0; k < g.nleases && !hit; ++k) {
          Lease& l = g_leases[g.leases[k]];
          if ((l.rt_mask & baked) && l.state.load(std::memory_order_relaxed) != RG_STATE_REFUSED) hit = &l;
        }
        if (!hit) {
          restore(fr, gi, idx);
          continue;
        }
        const int32_t p = g.bucket.load(std::memory_order_relaxed);
        const uint64_t statics = *reinterpret_cast<const uint64_t*>(fr + kFrStatic);
        t_rt.idx[gi] = idx;
        t_rt.static_before[gi] = p >= 0 && ((statics >> p) & 1);
        if (g.kind == RG_KIND_SRV && p >= 0 && ((statics >> p) & 1)) {
          t_rt.out[gi] = kRtStaticSkip;
          if (hit->static_skips.fetch_add(1, std::memory_order_relaxed) == 0)
            g_sdk->logger->ErrorF(g_handle, "native table: RT lease '%s' %s %c%u NOT STAGED - its table %d is a STATIC "
                                  "one at this DispatchRays (logged once; counted)", hit->name, stage_name(g.stage),
                                  letter(g.kind), g.reg, p);
          continue;
        }
        uint64_t h = src_valid(*hit) ? src_handle(*hit) : 0;
        const bool null = !h;
        if (null) {
          hit->null_stages.fetch_add(1, std::memory_order_relaxed);
          h = null_handle(*hit);
        }
        const StageOut s = stage(fr, gi, idx, *hit, h);
        t_rt.out[gi] = s == kLost ? kRtLost : s == kNoSlot ? kRtNoSlot : null ? kRtNull : s == kStaged ? kRtOurs
                                                                                                        : kRtAlready;
        continue;
      }
      const uint32_t ref = *reinterpret_cast<const uint32_t*>(ctx + stage_ref_offset(g.stage));
      if (ref)
        for (uint32_t k = 0; k < g.nleases && !hit; ++k) {
          Lease& l = g_leases[g.leases[k]];
          if (is_ref(l, ref) && lease_enabled(l) && src_valid(l)) hit = &l;
        }
      const uint64_t h = hit ? src_handle(*hit) : 0;
      if (h)
        stage(fr, gi, idx, *hit, h);
      else
        restore(fr, gi, idx);
    }
  }
  uint64_t r;
  {
    prof::Pause engine;
    r = g_orig_urp(fr_v, ctx_v, rays);
  }
  if (rays && t_rt.seen && fr_v) rt_after(static_cast<uint8_t*>(fr_v));
  return r;
}

const char* dim_name(uint32_t d) {
  static const char* const k[] = {"?", "1D", "2D", "2DMS", "3D", "CUBE", "1D_ARRAY", "2D_ARRAY", "2DMS_ARRAY",
                                  "CUBE_ARRAY", "typed buffer", "raw buffer", "structured buffer", "cbuffer"};
  return d < sizeof(k) / sizeof(k[0]) ? k[d] : "?";
}

bool typeless(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_UNKNOWN:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC7_TYPELESS:
      return true;
    default:
      return false;
  }
}

// Does texture `r` fit texture view `v`? false + why.
bool tex_fits(const View& v, const D3D12_RESOURCE_DESC& r, char* why, size_t n) {
  const uint32_t d = v.td.dimension;
  const D3D12_RESOURCE_DIMENSION want = (d == RG_TEX_1D || d == RG_TEX_1D_ARRAY) ? D3D12_RESOURCE_DIMENSION_TEXTURE1D
                                        : d == RG_TEX_3D                         ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                                                                 : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  const bool ms = d == RG_TEX_2DMS || d == RG_TEX_2DMS_ARRAY;
  const uint32_t mips = r.MipLevels ? r.MipLevels : 1u;
  const uint32_t slices = d == RG_TEX_3D ? (r.DepthOrArraySize >> (v.td.mip < 16 ? v.td.mip : 15)) : r.DepthOrArraySize;
  const char* bad = nullptr;
  if (r.Dimension != want)
    bad = "its D3D12 dimension does not fit";
  else if (ms != (r.SampleDesc.Count > 1))
    bad = ms ? "it is not multisampled" : "it is multisampled";
  else if ((d == RG_TEX_CUBE || d == RG_TEX_CUBE_ARRAY) && r.DepthOrArraySize % 6)
    bad = "its array size is not a multiple of 6";
  else if (!ms && v.td.mip >= mips)
    bad = "it has no such mip";
  else if (v.td.first_slice && v.td.first_slice >= slices)
    bad = "it has no such slice";
  else if (!v.td.format && typeless(r.Format))
    bad = "its format is TYPELESS and the view names none";
  if (!bad) return true;
  std::snprintf(why, n, "%s (%s view of a %llux%ux%u texture, %u mip(s), %u sample(s), DXGI format %u)", bad,
                dim_name(d), static_cast<unsigned long long>(r.Width), r.Height, r.DepthOrArraySize, mips,
                r.SampleDesc.Count, static_cast<unsigned>(r.Format));
  return false;
}

// A texture view's descriptor of `res` (nullptr: the NULL descriptor of its dimension) at `handle`.
void write_tex(const View& v, ID3D12Resource* res, uint64_t handle) {
  const D3D12_CPU_DESCRIPTOR_HANDLE h{static_cast<SIZE_T>(handle)};
  const rg_texture_view_desc& t = v.td;
  D3D12_RESOURCE_DESC r{};
  if (res) r = res->GetDesc();
  const DXGI_FORMAT fmt = t.format ? static_cast<DXGI_FORMAT>(t.format)
                          : res    ? r.Format
                                   : DXGI_FORMAT_R8G8B8A8_UNORM;  // a NULL descriptor reads 0 whatever its format
  const uint32_t slices = res ? r.DepthOrArraySize : 1u;
  const uint32_t rest = slices > t.first_slice ? slices - t.first_slice : 1u;
  const UINT count = t.slice_count ? t.slice_count : rest;
  if (v.kind == RG_KIND_UAV) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = fmt;
    switch (t.dimension) {
      case RG_TEX_1D:
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
        d.Texture1D.MipSlice = t.mip;
        break;
      case RG_TEX_1D_ARRAY:
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
        d.Texture1DArray = {t.mip, t.first_slice, count};
        break;
      case RG_TEX_2D:
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        d.Texture2D = {t.mip, t.plane};
        break;
      case RG_TEX_2D_ARRAY:
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray = {t.mip, t.first_slice, count, t.plane};
        break;
      default:  // RG_TEX_3D (view_create_texture admits no other UAV dimension)
        d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        d.Texture3D = {t.mip, t.first_slice, t.slice_count ? t.slice_count : UINT(-1)};
        break;
    }
    g_dev->CreateUnorderedAccessView(res, nullptr, &d, h);
    return;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC d{};
  d.Format = fmt;
  d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  const UINT mips = t.mip_count ? t.mip_count : UINT(-1);
  switch (t.dimension) {
    case RG_TEX_1D:
      d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
      d.Texture1D = {t.mip, mips, 0.f};
      break;
    case RG_TEX_1D_ARRAY:
      d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
      d.Texture1DArray = {t.mip, mips, t.first_slice, count, 0.f};
      break;
    case RG_TEX_2D:
      d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      d.Texture2D = {t.mip, mips, t.plane, 0.f};
      break;
    case RG_TEX_2D_ARRAY:
      d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
      d.Texture2DArray = {t.mip, mips, t.first_slice, count, t.plane, 0.f};
      break;
    case RG_TEX_2DMS:
      d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
      break;
    case RG_TEX_2DMS_ARRAY:
      d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
      d.Texture2DMSArray = {t.first_slice, count};
      break;
    case RG_TEX_3D:
      d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
      d.Texture3D = {t.mip, mips, 0.f};
      break;
    case RG_TEX_CUBE:
      d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
      d.TextureCube = {t.mip, mips, 0.f};
      break;
    default:  // RG_TEX_CUBE_ARRAY
      d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
      d.TextureCubeArray = {t.mip, mips, t.first_slice, t.slice_count ? t.slice_count : rest / 6, 0.f};
      break;
  }
  g_dev->CreateShaderResourceView(res, &d, h);
}

// A flow view's descriptor for the texture its name resolves to on this recording thread now, 0 = none. Written into
// the next ring slot when the resource (or the frame) differs from the last one written: the slot a context staged
// before is overwritten only when the ring comes round, long after the engine copied it into its own table.
uint64_t flow_handle(View& v, uint32_t vi) {
  if (v.fbad.load(std::memory_order_relaxed)) return 0;
  uint32_t id = 0;
  auto* res = static_cast<ID3D12Resource*>(render_flow::flow_texture(v.td.flow, &id));
  if (!res) return 0;
  const uint64_t frame = g_frame.load(std::memory_order_relaxed);
  AcquireSRWLockShared(&v.flock);
  bool same = v.fres == res && v.fid == id && v.fframe == frame;
  uint32_t cur = v.cur.load(std::memory_order_relaxed);
  ReleaseSRWLockShared(&v.flock);
  if (same) return handle_at(v.first + cur);
  AcquireSRWLockExclusive(&v.flock);
  same = v.fres == res && v.fid == id && v.fframe == frame;
  cur = v.cur.load(std::memory_order_relaxed);
  if (!same) {
    char why[200];
    if ((v.fres != res || v.fid != id) && !tex_fits(v, res->GetDesc(), why, sizeof why)) {
      if (v.fbad.exchange(1) == 0)
        g_sdk->logger->ErrorF(g_handle, "native table: view #%u - the engine texture it names does not fit: %s; its "
                              "leases never stage again", vi + 1, why);
      ReleaseSRWLockExclusive(&v.flock);
      return 0;
    }
    cur = v.written ? (cur + 1) % v.ring : 0;
    write_tex(v, res, handle_at(v.first + cur));
    if (v.written++ == 0)
      g_sdk->logger->InfoF(g_handle, "native table: view #%u first resolved - engine texture id %u (%p), %s %s", vi + 1,
                           id, static_cast<void*>(res), dim_name(v.td.dimension), v.kind == RG_KIND_UAV ? "UAV" : "SRV");
    v.fres = res;
    v.fid = id;
    v.fframe = frame;
    v.cur.store(cur, std::memory_order_release);
  }
  ReleaseSRWLockExclusive(&v.flock);
  return handle_at(v.first + cur);
}

// One view descriptor into ring slot `slot` (heap built, g_view_lock held).
bool write_view(View& v, uint32_t slot) {
  const D3D12_CPU_DESCRIPTOR_HANDLE h{static_cast<SIZE_T>(handle_at(v.first + slot))};
  if (v.tex) {  // view_set_resource checked that it fits
    write_tex(v, v.res, h.ptr);
    return true;
  }
  if (v.kind == RG_KIND_CBV) {  // view_set_resource checked: 256-aligned size within the buffer, <= 64 KB
    const D3D12_CONSTANT_BUFFER_VIEW_DESC cv{v.res->GetGPUVirtualAddress(), static_cast<UINT>((v.bytes + 255) & ~255ull)};
    g_dev->CreateConstantBufferView(&cv, h);
    return true;
  }
  const bool raw = v.stride == 0;
  const UINT n = static_cast<UINT>(raw ? v.bytes / 4 : v.bytes / v.stride);
  if (!n) return false;
  if (v.kind == RG_KIND_UAV) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = raw ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = n;
    d.Buffer.StructureByteStride = raw ? 0 : v.stride;
    d.Buffer.Flags = raw ? D3D12_BUFFER_UAV_FLAG_RAW : D3D12_BUFFER_UAV_FLAG_NONE;
    g_dev->CreateUnorderedAccessView(v.res, nullptr, &d, h);
  } else {
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = raw ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_UNKNOWN;
    d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Buffer.NumElements = n;
    d.Buffer.StructureByteStride = raw ? 0 : v.stride;
    d.Buffer.Flags = raw ? D3D12_BUFFER_SRV_FLAG_RAW : D3D12_BUFFER_SRV_FLAG_NONE;
    g_dev->CreateShaderResourceView(v.res, &d, h);
  }
  return true;
}

// Publish v.res at the next ring slot (heap built, g_view_lock held).
void publish_view(View& v, uint32_t vi) {
  const uint32_t next = v.written ? (v.cur.load(std::memory_order_relaxed) + 1) % v.ring : 0;
  if (!write_view(v, next)) {
    g_sdk->logger->ErrorF(g_handle, "native table: view #%u - %llu bytes hold no element of stride %u; not ready", vi + 1,
                          static_cast<unsigned long long>(v.bytes), v.stride);
    v.valid.store(false, std::memory_order_release);
    return;
  }
  ++v.written;
  v.cur.store(next, std::memory_order_release);
  v.valid.store(true, std::memory_order_release);
  if (v.written == 1 && v.tex)
    g_sdk->logger->InfoF(g_handle, "native table: view #%u ready - %s %s of texture %p", vi + 1, dim_name(v.td.dimension),
                         v.kind == RG_KIND_UAV ? "UAV" : "SRV", static_cast<void*>(v.res));
  else if (v.written == 1)
    g_sdk->logger->InfoF(g_handle, "native table: view #%u ready - %s of %p, %llu bytes, %s", vi + 1,
                         v.kind == RG_KIND_UAV ? "UAV" : v.kind == RG_KIND_CBV ? "CBV" : "SRV", static_cast<void*>(v.res),
                         static_cast<unsigned long long>(v.bytes), v.stride ? "structured" : "raw");
}

void build() {
  const auto* data = *reinterpret_cast<const uint8_t* const*>(g_base + kDeviceData);
  auto* engine_dev = data ? *reinterpret_cast<IUnknown* const*>(data + kDeviceInData) : nullptr;
  ID3D12Device* dev = nullptr;
  if (!engine_dev || FAILED(engine_dev->QueryInterface(IID_PPV_ARGS(&dev)))) {
    g_sdk->logger->Error(g_handle, "native table: the engine's device is not there yet - leased patches stay on the "
                                   "engine's shader");
    return;
  }
  D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16{};
  const bool gpu_upload = SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &o16, sizeof(o16))) &&
                          o16.GPUUploadHeapSupported;
  const uint32_t ncb = g_ncb.load();
  const uint32_t nview = g_nview.load();
  uint32_t total = 0;
  HRESULT hr = S_OK;
  for (uint32_t i = 0; i < ncb && SUCCEEDED(hr); ++i) {
    Cb& c = g_cb[i];
    c.first = total;
    total += c.ring;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = static_cast<UINT64>(c.ring) * c.bytes;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = E_FAIL;
    for (int attempt = gpu_upload ? 0 : 1; attempt < 2 && FAILED(hr); ++attempt) {
      D3D12_HEAP_PROPERTIES hp{};
      hp.Type = attempt == 0 ? D3D12_HEAP_TYPE_GPU_UPLOAD : D3D12_HEAP_TYPE_UPLOAD;
      hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                        IID_PPV_ARGS(&c.buf));
      if (SUCCEEDED(hr)) g_heap_kind = attempt == 0 ? 2u : 1u;
    }
    if (SUCCEEDED(hr)) {
      void* p = nullptr;
      const D3D12_RANGE none{0, 0};
      hr = c.buf->Map(0, &none, &p);
      c.mapped = static_cast<uint8_t*>(p);
    }
    if (SUCCEEDED(hr)) {
      AcquireSRWLockExclusive(&c.lock);
      for (uint32_t r = 0; r < c.ring; ++r) std::memcpy(c.mapped + r * c.bytes, c.shadow, c.bytes);
      ReleaseSRWLockExclusive(&c.lock);
    }
  }
  for (uint32_t i = 0; i < nview; ++i) {
    g_view[i].first = total;
    total += g_view[i].ring + (g_view[i].tex ? 1u : 0u);  // a texture view: its NULL descriptor after the ring
  }
  g_null_first = total;  // rg_kind order: CBV, SRV, UAV - what an RT lease stages while its source is not ready
  total += 3;
  if (SUCCEEDED(hr)) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = total;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;  // CPU-only: the engine's CopyDescriptors source
    hr = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap));
  }
  if (SUCCEEDED(hr)) {
    g_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE h0 = g_heap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < ncb; ++i) {
      const Cb& c = g_cb[i];
      for (uint32_t r = 0; r < c.ring; ++r) {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cv{c.buf->GetGPUVirtualAddress() + static_cast<UINT64>(r) * c.bytes, c.bytes};
        dev->CreateConstantBufferView(&cv, {h0.ptr + static_cast<SIZE_T>(c.first + r) * g_inc});
      }
    }
    {
      const D3D12_CPU_DESCRIPTOR_HANDLE n0{h0.ptr + static_cast<SIZE_T>(g_null_first) * g_inc};
      dev->CreateConstantBufferView(nullptr, n0);
      D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
      sd.Format = DXGI_FORMAT_R32_TYPELESS;
      sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
      sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
      dev->CreateShaderResourceView(nullptr, &sd, {n0.ptr + g_inc});
      D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
      ud.Format = DXGI_FORMAT_R32_TYPELESS;
      ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
      dev->CreateUnorderedAccessView(nullptr, nullptr, &ud, {n0.ptr + 2 * g_inc});
    }
    g_handle0 = h0.ptr;
    g_handle_end = h0.ptr + static_cast<uint64_t>(total) * g_inc;
    AcquireSRWLockExclusive(&g_view_lock);
    g_dev = dev;  // keeps the reference: views set later are created on it
    dev = nullptr;
    for (uint32_t i = 0; i < nview; ++i) {
      View& v = g_view[i];
      if (v.tex) write_tex(v, nullptr, handle_at(v.first + v.ring));
      if (v.td.flow > 0)
        v.valid.store(true, std::memory_order_release);  // resolved per stage (flow_handle)
      else if (v.res)
        publish_view(v, i);
    }
    g_ready.store(true, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_view_lock);
    g_sdk->logger->InfoF(g_handle, "native table: %u cbuffer(s) + %u view(s) ready - CPU heap %u descriptors, cbuffers "
                         "on the %s heap, on the engine's device %p", ncb, nview, total,
                         g_heap_kind == 2 ? "GPU_UPLOAD" : g_heap_kind == 1 ? "UPLOAD" : "-",
                         static_cast<void*>(g_dev));
  } else if (FAILED(hr)) {
    g_sdk->logger->ErrorF(g_handle, "native table: building the descriptors FAILED hr 0x%08x - leased patches stay on "
                          "the engine's shader", static_cast<unsigned>(hr));
  }
  if (dev) dev->Release();
}

// The k-th lease of an owner, nullptr past the last.
Lease* lease_of(bool comp, rg_handle h, uint32_t k) {
  const uint32_t max = comp ? kMaxCompHandles : kMaxPatchHandles;
  if (h <= 0 || static_cast<uint32_t>(h) > max || k >= kMaxLeasesPerOwner) return nullptr;
  const int8_t li = comp ? g_lease_of_comp[h - 1][k] : g_lease_of_patch[h - 1][k];
  return li >= 0 ? &g_leases[li] : nullptr;
}

void refuse_owner(bool comp, rg_handle h, const char* why) {
  if (comp)
    pso::companion_refuse(h, why);
  else
    pso::patch_refuse(h, why);
}

void refuse_lease(Lease& l, const char* why) {
  l.state.store(RG_STATE_REFUSED);
  clients::refused(clients::kLease, static_cast<rg_handle>(&l - g_leases + 1), why);
  for (uint32_t i = 0; i < l.npatches; ++i) pso::patch_refuse(l.patches[i], why);
  for (uint32_t i = 0; i < l.ncomps; ++i) pso::companion_refuse(l.comps[i], why);
}

// Registration of one owner of a lease being registered: its blobs must read the register, it may not hold it twice.
// An RT library owner: its library must declare the register (RDAT - a library has no PSV0).
// The DXIL ResourceKind an owner must declare at the leased register for this source (13 = cbuffer).
uint32_t want_kind(const rg_lease_desc* d) {
  if (d->kind == RG_KIND_CBV) return 13;
  const View& v = g_view[d->view - 1];
  return v.tex ? v.td.dimension : v.stride ? 12u : 11u;
}

// A declaration of another kind than the source: refused, logged. rk 0 = the blob does not record it (accepted).
int32_t kind_mismatch(uint32_t rk, uint32_t want, const char* name, const char* owner, rg_handle h,
                      const rg_lease_desc* d) {
  if (!rk || rk == want) return 0;
  char why[160];
  std::snprintf(why, sizeof why, "%s #%d declares %c%u as a %s, the lease's source is a %s", owner, h, letter(d->kind),
                d->reg, dim_name(rk), dim_name(want));
  g_sdk->logger->ErrorF(g_handle, "host: lease '%s' REFUSED - %s", name, why);
  clients::reason(why);
  return RG_E_BLOB;
}

int32_t check_rt_owner(rg_handle h, const rg_lease_desc* d, const char* name) {
  rg_blob lib{};
  if (h <= 0 || h > 64 || d->stage != RG_STAGE_CS || !rtlib::info(h, &lib)) return RG_E_ARG;
  uint32_t rk = 0;
  if (dxbc::lib_binds(lib.bytes, lib.size, d->kind, 0, d->reg, &rk) != dxbc::Bound::kYes) {
    g_sdk->logger->ErrorF(g_handle, "host: lease '%s' REFUSED - RT library #%d does not declare %c%u (space0)", name, h,
                          letter(d->kind), d->reg);
    return RG_E_BLOB;
  }
  return kind_mismatch(rk, want_kind(d), name, "RT library", h, d);
}

int32_t check_owner(bool comp, rg_handle h, const rg_lease_desc* d, const char* name) {
  const rg_blob* v = nullptr;
  rg_blob cs{};
  uint32_t stage = RG_STAGE_CS, count = 1;
  const uint32_t max = comp ? kMaxCompHandles : kMaxPatchHandles;
  if (h <= 0 || static_cast<uint32_t>(h) > max) return RG_E_ARG;
  if (comp) {
    if (!pso::companion_info(h, &cs)) return RG_E_ARG;
    v = &cs;
  } else if (!pso::patch_info(h, &stage, &v, &count)) {
    return RG_E_ARG;
  }
  if (stage != d->stage) return RG_E_ARG;
  const int8_t* slots = comp ? g_lease_of_comp[h - 1] : g_lease_of_patch[h - 1];
  uint32_t held = 0;
  for (; held < kMaxLeasesPerOwner && slots[held] >= 0; ++held) {
    const Lease& o = g_leases[slots[held]];
    if (o.stage == d->stage && o.kind == d->kind && o.reg == d->reg) return RG_E_TAKEN;  // that register, twice
  }
  if (held == kMaxLeasesPerOwner) return RG_E_FULL;
  for (uint32_t k = 0; k < count; ++k) {
    uint32_t rk = 0;
    if (dxbc::binds(v[k].bytes, v[k].size, d->kind, 0, d->reg, &rk) != dxbc::Bound::kYes) {
      g_sdk->logger->ErrorF(g_handle, "host: lease '%s' REFUSED - %s #%d%s does not read %s %c%u (space0)", name,
                            comp ? "companion" : "patch", h, comp ? "" : " (a variant)", stage_name(d->stage),
                            letter(d->kind), d->reg);
      return RG_E_BLOB;
    }
    if (const int32_t e = kind_mismatch(rk, want_kind(d), name, comp ? "companion" : "patch", h, d)) return e;
  }
  return 0;
}

void add_owner(bool comp, rg_handle h, int8_t lease) {
  int8_t* slots = comp ? g_lease_of_comp[h - 1] : g_lease_of_patch[h - 1];
  for (uint32_t k = 0; k < kMaxLeasesPerOwner; ++k)
    if (slots[k] < 0) {
      slots[k] = lease;
      break;
    }
}

}  // namespace

rg_handle cbuffer_create(uint32_t bytes, uint32_t ring, const void* initial) {
  if (!registration_open()) return RG_E_LATE;
  if (bytes == 0 || bytes % 256 || bytes > 4096 || ring == 0 || ring > kMaxRing) return RG_E_ARG;
  const uint32_t n = g_ncb.load();
  if (n >= kMaxCb) return RG_E_FULL;
  Cb& c = g_cb[n];
  c.bytes = bytes;
  c.ring = ring;
  c.shadow = new uint8_t[bytes];
  if (initial)
    std::memcpy(c.shadow, initial, bytes);
  else
    std::memset(c.shadow, 0, bytes);
  c.valid.store(initial != nullptr);
  g_ncb.store(n + 1, std::memory_order_release);
  return static_cast<rg_handle>(n + 1);
}

void cbuffer_write(rg_handle h, uint32_t offset, const void* data, uint32_t size) {
  if (h <= 0 || static_cast<uint32_t>(h) > g_ncb.load(std::memory_order_acquire) || !data) return;
  Cb& c = g_cb[h - 1];
  if (offset > c.bytes || size > c.bytes - offset) return;
  AcquireSRWLockExclusive(&c.lock);
  if (!c.valid.load(std::memory_order_relaxed) || std::memcmp(c.shadow + offset, data, size) != 0) {
    std::memcpy(c.shadow + offset, data, size);
    if (c.mapped) {  // not built yet: build() seeds every ring slot from the shadow
      const uint32_t next = (c.cur.load(std::memory_order_relaxed) + 1) % c.ring;
      std::memcpy(c.mapped + next * c.bytes, c.shadow, c.bytes);
      c.cur.store(next, std::memory_order_release);
    }
    c.valid.store(true, std::memory_order_release);
  }
  ReleaseSRWLockExclusive(&c.lock);
}

int32_t cbuffer_feed(rg_handle h, uint32_t which, uint32_t offset, uint32_t size) {
  if (!registration_open()) return RG_E_LATE;
  if (h <= 0 || static_cast<uint32_t>(h) > g_ncb.load() || which >= engine_blocks::count()) return RG_E_ARG;
  Cb& c = g_cb[h - 1];
  if (c.feed >= 0) return RG_E_TAKEN;
  if (size == 0 || offset > c.bytes || size > c.bytes - offset) return RG_E_ARG;
  engine_blocks::want(which);  // a feed is a request: its writer's hook goes on
  c.feed = static_cast<int32_t>(which);
  c.feed_offset = offset;
  c.feed_size = size;
  return 0;
}

void on_engine_block(uint32_t which, const void* data, uint32_t size) {
  const uint32_t n = g_ncb.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n; ++i) {
    const Cb& c = g_cb[i];
    if (c.feed != static_cast<int32_t>(which)) continue;
    const bool first = !c.valid.load(std::memory_order_relaxed);
    const uint32_t m = size < c.feed_size ? size : c.feed_size;
    cbuffer_write(static_cast<rg_handle>(i + 1), c.feed_offset, data, m);
    if (first && g_sdk)
      g_sdk->logger->InfoF(g_handle, "native table: cbuffer #%u first filled from engine block %u %s (%u B) - its "
                           "leases may swap from now on", i + 1, which, engine_blocks::name(which), m);
  }
}

rg_handle view_create(uint32_t kind, uint32_t stride, uint32_t ring) {
  if (!registration_open()) return RG_E_LATE;
  if (kind > RG_KIND_UAV || ring == 0 || ring > kMaxViewRing || stride > 2048 || (stride && stride % 4) ||
      (kind == RG_KIND_CBV && stride))
    return RG_E_ARG;
  const uint32_t n = g_nview.load();
  if (n >= kMaxViews) return RG_E_FULL;
  View& v = g_view[n];
  v.kind = kind;
  v.stride = stride;
  v.ring = ring;
  g_nview.store(n + 1, std::memory_order_release);
  return static_cast<rg_handle>(n + 1);
}

rg_handle view_create_texture(uint32_t kind, const rg_texture_view_desc* d, uint32_t ring) {
  if (!registration_open()) return RG_E_LATE;
  if (!d || (kind != RG_KIND_SRV && kind != RG_KIND_UAV) || ring == 0 || ring > kMaxViewRing ||
      d->dimension < RG_TEX_1D || d->dimension > RG_TEX_CUBE_ARRAY || d->flow < 0)
    return RG_E_ARG;
  if (kind == RG_KIND_UAV && d->dimension != RG_TEX_1D && d->dimension != RG_TEX_1D_ARRAY &&
      d->dimension != RG_TEX_2D && d->dimension != RG_TEX_2D_ARRAY && d->dimension != RG_TEX_3D) {
    clients::reason("a UAV texture view is 1D, 1D_ARRAY, 2D, 2D_ARRAY or 3D");
    return RG_E_ARG;
  }
  if (d->flow > 0 && !render_flow::flow_hash(d->flow)) return RG_E_ARG;
  const uint32_t n = g_nview.load();
  if (n >= kMaxViews) return RG_E_FULL;
  View& v = g_view[n];
  v.kind = kind;
  v.ring = ring;
  v.tex = true;
  v.td = *d;
  g_nview.store(n + 1, std::memory_order_release);
  return static_cast<rg_handle>(n + 1);
}

int32_t view_set_resource(rg_handle h, void* resource, uint64_t bytes) {
  if (h <= 0 || static_cast<uint32_t>(h) > g_nview.load(std::memory_order_acquire)) return RG_E_ARG;
  auto* res = static_cast<ID3D12Resource*>(resource);
  View& v = g_view[h - 1];
  if (v.td.flow > 0) return RG_E_ARG;  // the engine's texture, resolved per stage
  if (v.tex) {
    char why[200];
    if (res && !tex_fits(v, res->GetDesc(), why, sizeof why)) {
      g_sdk->logger->ErrorF(g_handle, "native table: view #%d - view_set_resource refused: %s", h, why);
      return RG_E_ARG;
    }
    bytes = 0;
  }
  const uint64_t need = v.kind == RG_KIND_CBV ? (bytes + 255) & ~255ull : bytes;
  if (res && !v.tex &&
      (bytes == 0 || res->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || res->GetDesc().Width < need ||
       (v.kind == RG_KIND_CBV && need > 65536)))
    return RG_E_ARG;
  AcquireSRWLockExclusive(&g_view_lock);
  if (res == v.res && bytes == v.bytes) {
    ReleaseSRWLockExclusive(&g_view_lock);
    return 0;
  }
  if (res) res->AddRef();
  if (v.res) v.res->Release();
  v.res = res;
  v.bytes = bytes;
  if (!res)
    v.valid.store(false, std::memory_order_release);
  else if (g_ready.load(std::memory_order_acquire))
    publish_view(v, static_cast<uint32_t>(h - 1));
  // else: build() publishes it
  ReleaseSRWLockExclusive(&g_view_lock);
  return 0;
}

rg_handle lease_register(const rg_lease_desc* d) {
  if (!registration_open()) return RG_E_LATE;
  if (!d || d->stage > RG_STAGE_CS || d->kind > RG_KIND_UAV) return RG_E_ARG;
  if ((d->patch_count && !d->patches) || (d->companion_count && !d->companions) || (d->rtlib_count && !d->rtlibs) ||
      d->patch_count + d->companion_count + d->rtlib_count == 0 || d->patch_count > kMaxLeaseOwners ||
      d->companion_count > kMaxLeaseOwners || d->rtlib_count > 64)
    return RG_E_ARG;
  if (d->reg >= kKinds[d->kind].regs) return RG_E_ARG;  // the engine's tables cover b0-b13, t0-t127, u0-u18
  if (d->kind == RG_KIND_CBV && d->view) {  // a CBV view of a client buffer
    if (d->cbuffer || d->view < 0 || static_cast<uint32_t>(d->view) > g_nview.load() ||
        g_view[d->view - 1].kind != RG_KIND_CBV)
      return RG_E_ARG;
  } else if (d->kind == RG_KIND_CBV) {
    if (d->cbuffer <= 0 || static_cast<uint32_t>(d->cbuffer) > g_ncb.load()) return RG_E_ARG;
  } else {
    if (d->cbuffer || d->view <= 0 || static_cast<uint32_t>(d->view) > g_nview.load() ||
        g_view[d->view - 1].kind != d->kind)
      return RG_E_ARG;
  }
  const uint32_t n = g_nleases.load();
  if (n >= kMaxLeases) return RG_E_FULL;
  const char* name = d->name ? d->name : "?";
  // ONE RT lease per register: an RT lease stages at every DispatchRays, whatever the state object, and the first one
  // found wins - a second one's libraries would read the first one's descriptor. Checked before the blobs (the
  // register is taken whatever they read). Shader leases share a register freely.
  if (d->rtlib_count)
    for (uint32_t i = 0; i < n; ++i) {
      const Lease& o = g_leases[i];
      if (!o.rt_mask || o.stage != d->stage || o.kind != d->kind || o.reg != d->reg) continue;
      char why[120];
      std::snprintf(why, sizeof why, "RT lease '%s' already holds %c%u (every DispatchRays stages one descriptor there)",
                    o.name, letter(d->kind), d->reg);
      g_sdk->logger->ErrorF(g_handle, "host: lease '%s' REFUSED - %s", name, why);
      clients::reason(why);
      return RG_E_TAKEN;
    }
  for (uint32_t i = 0; i < d->patch_count; ++i)
    if (const int32_t e = check_owner(false, d->patches[i], d, name)) return e;
  for (uint32_t i = 0; i < d->companion_count; ++i)
    if (const int32_t e = check_owner(true, d->companions[i], d, name)) return e;
  uint64_t rt_mask = 0;
  for (uint32_t i = 0; i < d->rtlib_count; ++i) {
    if (const int32_t e = check_rt_owner(d->rtlibs[i], d, name)) return e;
    rt_mask |= 1ull << (d->rtlibs[i] - 1);
  }
  uint32_t gi = 0;
  for (; gi < g_ngroups; ++gi)
    if (g_groups[gi].stage == d->stage && g_groups[gi].kind == d->kind && g_groups[gi].reg == d->reg) break;
  if (gi == g_ngroups) {
    if (g_ngroups >= kMaxGroups) return RG_E_FULL;
    g_groups[gi].stage = d->stage;
    g_groups[gi].kind = d->kind;
    g_groups[gi].reg = d->reg;
    g_groups[gi].nleases = 0;
    ++g_ngroups;
  }
  Group& g = g_groups[gi];
  if (g.nleases >= kMaxLeases) return RG_E_FULL;
  Lease& l = g_leases[n];
  l.name = name;
  l.stage = d->stage;
  l.kind = d->kind;
  l.reg = d->reg;
  l.npatches = d->patch_count;
  for (uint32_t i = 0; i < d->patch_count; ++i) {
    l.patches[i] = d->patches[i];
    add_owner(false, d->patches[i], static_cast<int8_t>(n));
  }
  l.ncomps = d->companion_count;
  for (uint32_t i = 0; i < d->companion_count; ++i) {
    l.comps[i] = d->companions[i];
    add_owner(true, d->companions[i], static_cast<int8_t>(n));
  }
  l.rt_mask = rt_mask;
  l.cb = d->cbuffer > 0 ? d->cbuffer - 1 : -1;
  l.view = d->cbuffer > 0 ? -1 : d->view - 1;
  l.group = gi;
  g.leases[g.nleases++] = n;
  g_nleases.store(n + 1, std::memory_order_release);
  return static_cast<rg_handle>(n + 1);
}

void lease_stats(rg_handle h, rg_lease_stats* out) {
  if (!out) return;
  *out = {};
  if (h <= 0 || static_cast<uint32_t>(h) > g_nleases.load(std::memory_order_acquire)) return;
  const Lease& l = g_leases[h - 1];
  const Group& g = g_groups[l.group];
  out->state = g.broken.load() ? static_cast<uint32_t>(RG_STATE_REFUSED) : l.state.load();
  out->ready = g_ready.load() && (l.view < 0 || src_valid(l)) ? 1u : 0u;
  out->heap_kind = g_heap_kind;
  out->shader_refs = l.nrefs.load();
  out->staging_index = g.idx.load();
  out->stages = l.stages.load();
  out->restores = g.restores.load();
  out->lost = g.lost.load();
  out->not_staged = l.not_staged.load();
  out->null_stages = l.null_stages.load();
  out->static_skips = l.static_skips.load();
  out->rt_checked = l.rt_checked.load();
  uint32_t baked = 0;
  for (uint64_t m = l.rt_mask & rtlib::baked_mask(); m; m &= m - 1) ++baked;
  out->rt_baked = baked;
}

void rt_begin() { t_rt.seen = false; }

bool rt_delivered(uint64_t lib_bit, char* why, size_t n) {
  if (why && n) why[0] = 0;
  for (uint32_t gi = 0; gi < g_ngroups; ++gi) {
    const Group& g = g_groups[gi];
    bool mine = false;
    for (uint32_t k = 0; k < g.nleases && !mine; ++k) mine = (g_leases[g.leases[k]].rt_mask & lib_bit) != 0;
    if (!mine) continue;
    const uint8_t o = t_rt.seen ? t_rt.out[gi] : kRtNotSeen;
    if (rt_good(o)) continue;
    if (why && n)
      std::snprintf(why, n, "RT lease %s %c%u: %s", stage_name(g.stage), letter(g.kind), g.reg,
                    t_rt.seen ? kRtOutName[o] : "UpdateRootParams did not run in this DispatchRays");
    return false;
  }
  return true;
}

void rt_state_text(char* out, size_t n) {
  if (!out || !n) return;
  out[0] = 0;
  if (!t_rt.seen) {
    std::snprintf(out, n, "leases: no UpdateRootParams");
    return;
  }
  static const char* const kShort[kRtCount] = {"-", "new", "ours", "null", "STATIC-SKIP", "LOST", "NOSLOT", "GONE",
                                               "STATIC"};
  size_t k = static_cast<size_t>(std::snprintf(out, n, "leases:"));
  for (uint32_t gi = 0; gi < g_ngroups && k + 1 < n; ++gi) {
    const uint8_t o = t_rt.out[gi];
    if (o == kRtNotSeen) continue;
    const Group& g = g_groups[gi];
    k += static_cast<size_t>(std::snprintf(out + k, n - k, " %c%u=%s#%d%s", letter(g.kind), g.reg, kShort[o],
                                           t_rt.desc[gi], t_rt.static_before[gi] ? "(was static)" : ""));
  }
}

void rt_counts_text(char* out, size_t n) {
  if (!out || !n) return;
  size_t k = static_cast<size_t>(std::snprintf(out, n, "RT lease delivery this session:"));
  for (uint32_t o = 1; o < kRtCount && k + 1 < n; ++o)
    k += static_cast<size_t>(std::snprintf(out + k, n - k, " %s %llu;", kRtOutName[o],
                                           static_cast<unsigned long long>(g_rt_out[o].load())));
  if (k + 1 < n)
    std::snprintf(out + k, n - k, " table STATIC on arrival %llu; context entries: %u in use, peak %u, taken over "
                  "%llu, all in use %llu", static_cast<unsigned long long>(g_rt_static_before.load()),
                  g_fr_live.load(), g_fr_peak.load(), static_cast<unsigned long long>(g_fr_taken.load()),
                  static_cast<unsigned long long>(g_fr_full.load()));
}

void rt_layout_text(char* out, size_t n) {
  if (!out || !n) return;
  size_t k = static_cast<size_t>(std::snprintf(out, n, "RT leases in our heap:"));
  const uint32_t nl = g_nleases.load();
  for (uint32_t i = 0; i < nl && k + 1 < n; ++i) {
    const Lease& l = g_leases[i];
    if (!l.rt_mask) continue;
    if (l.cb >= 0)
      k += static_cast<size_t>(std::snprintf(out + k, n - k, " '%s' %c%u = cbuffer #%u-#%u (now #%u);", l.name,
                                             letter(l.kind), l.reg, g_cb[l.cb].first,
                                             g_cb[l.cb].first + g_cb[l.cb].ring - 1,
                                             g_cb[l.cb].first + g_cb[l.cb].cur.load()));
    else
      k += static_cast<size_t>(std::snprintf(out + k, n - k, " '%s' %c%u = view #%u-#%u (now #%u);", l.name,
                                             letter(l.kind), l.reg, g_view[l.view].first,
                                             g_view[l.view].first + g_view[l.view].ring - 1,
                                             g_view[l.view].first + g_view[l.view].cur.load()));
  }
  if (k + 1 < n) std::snprintf(out + k, n - k, " NULL CBV/SRV/UAV = #%u/#%u/#%u", g_null_first, g_null_first + 1,
                               g_null_first + 2);
}

void on_rt_library(const void* code, size_t size, const char* name) {
  const uint32_t n = g_nleases.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n; ++i) {
    Lease& l = g_leases[i];
    if (!l.rt_mask || l.state.load() == RG_STATE_REFUSED) continue;
    const dxbc::Bound b = dxbc::lib_binds(code, size, l.kind, 0, l.reg);
    if (b == dxbc::Bound::kNo) {
      if (l.rt_checked.fetch_add(1) == 0) {
        l.state.store(RG_STATE_OK);
        g_sdk->logger->InfoF(g_handle, "host: lease '%s' CS %c%u GRANTED for RT (staged at every DispatchRays) - the "
                             "first engine library ('%s') does not declare it; every later one is checked too",
                             l.name, letter(l.kind), l.reg, name ? name : "?");
      }
      continue;
    }
    l.state.store(RG_STATE_REFUSED);
    char why[120];
    std::snprintf(why, sizeof why, "engine RT library '%s' %s", name ? name : "?",
                  b == dxbc::Bound::kYes ? "declares the register" : "has no readable RDAT");
    clients::refused(clients::kLease, static_cast<rg_handle>(i + 1), why);
    g_sdk->logger->ErrorF(g_handle, "host: lease '%s' CS %c%u REFUSED - engine RT library '%s' %s; it is no longer "
                          "staged, our libraries not baked yet stay vanilla", l.name, letter(l.kind), l.reg,
                          name ? name : "?", b == dxbc::Bound::kYes ? "declares it" : "has no readable RDAT");
  }
}

bool rtlib_may_bake(rg_handle lib) {
  if (lib <= 0 || lib > 64) return true;
  const uint64_t bit = 1ull << (lib - 1);
  const uint32_t n = g_nleases.load(std::memory_order_acquire);
  bool leased = false;
  for (uint32_t i = 0; i < n; ++i) {
    const Lease& l = g_leases[i];
    if (!(l.rt_mask & bit)) continue;
    if (l.state.load() == RG_STATE_REFUSED || g_groups[l.group].broken.load()) return false;
    leased = true;
  }
  if (!leased) return true;
  if (!g_attached) return false;  // nobody would stage its registers
  std::call_once(g_build_once, [] { build(); });
  return g_ready.load(std::memory_order_acquire);
}

bool needed() { return g_nleases.load() != 0; }

void frame_tick() { g_frame.fetch_add(1, std::memory_order_relaxed); }

bool attach() {
  g_bind = reinterpret_cast<BindDescriptor_t>(g_base + kBindDescriptor);
  if (!call_site_ok(g_base, kBindCbDescRet, kBindDescriptor) ||
      !attach_hook(kUpdateRootParams, kSetupForDrawUrpRet, reinterpret_cast<void*>(&d_update_root_params),
                   reinterpret_cast<void**>(&g_orig_urp), "SFrameResources::UpdateRootParams")) {
    const uint32_t n = g_nleases.load();
    for (uint32_t i = 0; i < n; ++i) refuse_lease(g_leases[i], "UpdateRootParams did not attach / not 2.31");
    return false;
  }
  g_attached = true;
  return true;
}

void detach() {
  if (g_attached) detach_hook(kUpdateRootParams);
  g_attached = false;
}

bool attached() { return g_attached; }

bool patch_has_lease(rg_handle patch) { return lease_of(false, patch, 0) != nullptr; }

namespace {

// One lease against the vanilla bytecode: true = granted.
bool check_vanilla(Lease& l, bool comp, rg_handle owner, const void* code, size_t size) {
  const dxbc::Bound b = dxbc::binds(code, size, l.kind, 0, l.reg);
  if (b == dxbc::Bound::kNo) {
    uint32_t expect = RG_STATE_PENDING;
    l.state.compare_exchange_strong(expect, RG_STATE_OK);
    g_sdk->logger->InfoF(g_handle, "host: lease '%s' %s %c%u GRANTED for %s #%d - the engine's shader does not read it",
                         l.name, stage_name(l.stage), letter(l.kind), l.reg, comp ? "companion" : "patch", owner);
    return true;
  }
  char why[160];
  _snprintf_s(why, sizeof(why), _TRUNCATE, "lease '%s': %s", l.name,
              b == dxbc::Bound::kYes ? "the engine's shader reads that register itself"
                                     : "the engine's shader has no readable PSV0 (bindings unknown)");
  refuse_owner(comp, owner, why);
  return false;
}

// Create-time, before the engine creates the PSO: every lease of the owner against the vanilla bytecode, once.
bool owner_matched(bool comp, rg_handle h, const void* code, size_t size) {
  if (!lease_of(comp, h, 0)) return true;
  if (!g_attached) return false;
  std::call_once(g_build_once, [] { build(); });
  std::atomic<uint8_t>& verdict = comp ? g_comp_ok[h - 1] : g_patch_ok[h - 1];
  const uint8_t v = verdict.load();
  if (v) return v == 1;
  bool ok = true;
  for (uint32_t k = 0; ok; ++k) {
    Lease* l = lease_of(comp, h, k);
    if (!l) break;
    ok = l->state.load() != RG_STATE_REFUSED && check_vanilla(*l, comp, h, code, size);
  }
  verdict.store(ok ? 1 : 2);
  return ok;
}

// One lease at a bind: learn the ShaderRef; true iff OUR descriptor of this lease sits in the context's slot now.
bool lease_bind_ok(Lease& l, uint8_t* ctx, bool enabled) {
  const Group& g = g_groups[l.group];
  if (g.broken.load(std::memory_order_relaxed)) return false;
  const uint32_t ref = *reinterpret_cast<const uint32_t*>(ctx + stage_ref_offset(l.stage));
  if (!ref) return false;
  if (!is_ref(l, ref)) {
    const uint32_t n = l.nrefs.load(std::memory_order_acquire);
    if (n < kMaxRefs) {  // several render threads may race here: a duplicate id costs one compare, nothing else
      l.refs[n].store(ref, std::memory_order_relaxed);
      l.nrefs.store(n + 1, std::memory_order_release);
      g_sdk->logger->InfoF(g_handle, "native table: lease '%s' %s ShaderRef id %u learned (%u known)", l.name,
                           stage_name(l.stage), ref, n + 1);
    } else if (!l.refs_full.exchange(true)) {
      g_sdk->logger->ErrorF(g_handle, "native table: lease '%s' %s knows %u ShaderRefs - the most it can: shader ref %u "
                            "and any further one are NOT staged (logged once)", l.name, stage_name(l.stage), kMaxRefs,
                            ref);
    }
    return false;  // this draw's UpdateRootParams has already run without our descriptor
  }
  if (!enabled || !src_valid(l)) return false;
  const int32_t idx = g.idx.load(std::memory_order_relaxed);
  if (idx < 0) return false;
  auto* fr = *reinterpret_cast<uint8_t**>(ctx + kCtxFrameRes);
  if (!fr) return false;
  const uint64_t* slot = staging_slot(fr, idx);
  return slot && src_owns(l, *slot);
}

}  // namespace

bool on_patch_matched(rg_handle patch, const void* code, size_t size) { return owner_matched(false, patch, code, size); }

bool on_companion_matched(rg_handle comp, const void* code, size_t size) {
  return owner_matched(true, comp, code, size);
}

bool bind_ok(rg_handle patch, const void* pso_state, bool compute, bool enabled) {
  if (!pso_state || !g_ready.load(std::memory_order_acquire) || !lease_of(false, patch, 0)) return false;
  auto* ctx = const_cast<uint8_t*>(static_cast<const uint8_t*>(pso_state)) - (compute ? kCtxComputeState : kCtxGraphicsState);
  bool ok = true;
  for (uint32_t k = 0;; ++k) {  // every lease, so each learns its ShaderRef even when an earlier one is not staged
    Lease* l = lease_of(false, patch, k);
    if (!l) break;
    ok = lease_bind_ok(*l, ctx, enabled) && ok;
  }
  return ok;
}

void companion_bind(rg_handle comp, const void* pso_state) {
  if (!pso_state || !g_ready.load(std::memory_order_acquire) || !lease_of(true, comp, 0)) return;
  auto* ctx = const_cast<uint8_t*>(static_cast<const uint8_t*>(pso_state)) - kCtxComputeState;
  for (uint32_t k = 0;; ++k) {
    Lease* l = lease_of(true, comp, k);
    if (!l) break;
    lease_bind_ok(*l, ctx, true);
  }
}

bool companion_staged(rg_handle comp, const void* ctx_v) {
  if (!lease_of(true, comp, 0)) return true;
  if (!ctx_v || !g_ready.load(std::memory_order_acquire)) return false;
  auto* fr = *reinterpret_cast<uint8_t* const*>(static_cast<const uint8_t*>(ctx_v) + kCtxFrameRes);
  bool ok = true;
  for (uint32_t k = 0;; ++k) {
    Lease* l = lease_of(true, comp, k);
    if (!l) break;
    const Group& g = g_groups[l->group];
    const int32_t idx = g.idx.load(std::memory_order_relaxed);
    const uint64_t* slot = (fr && idx >= 0 && !g.broken.load(std::memory_order_relaxed)) ? staging_slot(fr, idx) : nullptr;
    if (!slot || !src_owns(*l, *slot)) {
      if (l->not_staged.fetch_add(1, std::memory_order_relaxed) == 0)
        g_sdk->logger->WarnF(g_handle, "native table: companion #%d NOT STAGED - lease '%s' %s %c%u is not in this "
                             "dispatch's table (source %s, staging index %d); the companion is skipped (logged once)",
                             comp, l->name, stage_name(l->stage), letter(l->kind), l->reg,
                             src_valid(*l) ? "ready" : "NOT READY", idx);
      ok = false;
    }
  }
  return ok;
}

}  // namespace core::bindings
