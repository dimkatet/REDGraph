// bvh_probe.hpp. One [bvh] line per render frame, while the frame's summary keeps changing.

#include "bvh_probe.hpp"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "engine_231.hpp"
#include "internal.hpp"

namespace core::bvh_probe {
namespace {

std::atomic<bool> g_on{false};
std::mutex g_mx;

// ---- the frame being accumulated (g_mx) ----
uint32_t g_frame = 0;  // the render frame the counters below belong to (0 = none seen)
uint64_t g_seen = 0;   // frames flushed

// The desc writes of the swapped / watched proxies, one row each (a client swapping several meshes for a crowd has
// dozens). Past the cap the frame's rows are cut - the totals still count every write - and the first cut is logged.
constexpr uint32_t kMaxSlots = 32;
bool g_slots_cut_said = false;  // g_mx
struct SlotRow {
  uint32_t slot;
  const void* proxy;
  uint32_t writes, swapped;                 // all writes / of those, with the client's BLAS
  uint32_t id, mask, contribution, flags;   // the LAST write's desc
  uint64_t blas;
};
SlotRow g_slots[kMaxSlots];
uint32_t g_nslots = 0;
uint32_t g_writes = 0, g_swapped = 0;

// The TLAS builds of the frame (a few: the scene, the light volumes, and - the question - any second one).
constexpr uint32_t kMaxTlas = 8;
struct TlasRow {
  uint32_t count, site;
  uint64_t instances;
  uint32_t n;
};
TlasRow g_tlas[kMaxTlas];
uint32_t g_ntlas = 0;

// The DispatchRays of the frame: the distinct (state object, engine call site) pairs, with how many times each ran.
constexpr uint32_t kMaxRays = 32;
struct RayRow {
  uint32_t so, site, n;
};
RayRow g_rays[kMaxRays];
uint32_t g_nrays = 0, g_rays_total = 0;

// Logging: the first frames in full, then only what changed, plus a heartbeat so a long stable view is not silent.
constexpr uint64_t kFirstFrames = 8, kHeartbeat = 300;
uint64_t g_last_hash = 0;

uint32_t render_frame() { return *reinterpret_cast<const volatile uint32_t*>(g_base + e231::kRenderFrame); }

uint64_t mix(uint64_t h, uint64_t v) {
  h ^= v;
  return h * 1099511628211ull;
}

std::string summary() {  // g_mx held
  char buf[1280];
  std::snprintf(buf, sizeof buf, "[bvh] frame %u: desc writes %u (%u with a client's BLAS)", g_frame, g_writes,
                g_swapped);
  std::string s(buf);
  for (uint32_t i = 0; i < g_nslots; ++i) {
    const SlotRow& r = g_slots[i];
    std::snprintf(buf, sizeof buf,
                  "; slot %u proxy %p: %u write(s), last {InstanceID %u, mask 0x%02x, contribution %u, flags 0x%02x, "
                  "BLAS 0x%llx}",
                  r.slot, r.proxy, r.writes, r.id, r.mask, r.contribution, r.flags,
                  static_cast<unsigned long long>(r.blas));
    s += buf;
  }
  std::snprintf(buf, sizeof buf, "; TLAS builds %u [", g_ntlas);
  s += buf;
  for (uint32_t i = 0; i < g_ntlas; ++i) {
    const TlasRow& r = g_tlas[i];
    std::snprintf(buf, sizeof buf, "%s%u x %u inst @0x%llx caller +0x%x", i ? ", " : "", r.n, r.count,
                  static_cast<unsigned long long>(r.instances), r.site);
    s += buf;
  }
  std::snprintf(buf, sizeof buf, "]; DispatchRays %u [", g_rays_total);
  s += buf;
  for (uint32_t i = 0; i < g_nrays; ++i) {
    std::snprintf(buf, sizeof buf, "%sso %u caller +0x%x x%u", i ? ", " : "", g_rays[i].so, g_rays[i].site, g_rays[i].n);
    s += buf;
  }
  s += "]";
  return s;
}

void reset_locked() {
  g_nslots = g_ntlas = g_nrays = 0;
  g_writes = g_swapped = g_rays_total = 0;
}

void flush_locked() {
  if (!g_writes && !g_ntlas && !g_nrays) return;  // nothing ran (menu, paused, no RT)
  ++g_seen;
  uint64_t h = 1469598103934665603ull;
  h = mix(h, g_writes), h = mix(h, g_swapped);
  for (uint32_t i = 0; i < g_nslots; ++i) {
    h = mix(mix(mix(mix(h, g_slots[i].slot), g_slots[i].writes), g_slots[i].id), g_slots[i].mask);
    h = mix(mix(h, g_slots[i].contribution), g_slots[i].blas);
  }
  for (uint32_t i = 0; i < g_ntlas; ++i) {
    h = mix(mix(mix(h, g_tlas[i].n), g_tlas[i].count), g_tlas[i].instances);
    h = mix(h, g_tlas[i].site);
  }
  for (uint32_t i = 0; i < g_nrays; ++i) h = mix(mix(mix(h, g_rays[i].so), g_rays[i].site), g_rays[i].n);
  if (g_seen > kFirstFrames && h == g_last_hash && g_seen % kHeartbeat != 0) return;
  g_last_hash = h;
  const std::string line = summary();
  g_sdk->logger->Info(g_handle, line.c_str());
}

// g_mx held. false = a straggler event from an older frame (dropped). A NEW frame flushes the previous one first.
bool current_locked(uint32_t rf) {
  if (rf == g_frame) return true;
  if (rf < g_frame) return false;
  flush_locked();
  reset_locked();
  g_frame = rf;
  return true;
}

}  // namespace

void enable() {
  g_on.store(true);
  g_sdk->logger->Info(g_handle, "[bvh] probe ON - desc writes of subscribed proxies, every TLAS build and every "
                                "DispatchRays, one line a frame while it changes");
}

bool on() { return g_on.load(std::memory_order_relaxed); }

void on_desc(uint32_t slot, const void* proxy, const uint8_t desc[64], bool swapped) {
  if (!g_on.load(std::memory_order_relaxed)) return;
  const uint32_t w30 = *reinterpret_cast<const uint32_t*>(desc + 0x30);
  const uint32_t w34 = *reinterpret_cast<const uint32_t*>(desc + 0x34);
  uint64_t blas = 0;
  std::memcpy(&blas, desc + 0x38, 8);
  std::lock_guard<std::mutex> lk(g_mx);
  if (!current_locked(render_frame())) return;
  ++g_writes;
  if (swapped) ++g_swapped;
  uint32_t i = 0;
  for (; i < g_nslots && g_slots[i].slot != slot; ++i) {}
  if (i == g_nslots) {
    if (g_nslots >= kMaxSlots) {
      if (!g_slots_cut_said) {
        g_slots_cut_said = true;
        g_sdk->logger->WarnF(g_handle, "[bvh] more than %u swapped / watched proxies in a frame - the rows past them "
                             "are left out of the [bvh] lines (the totals count them)", kMaxSlots);
      }
      return;
    }
    g_slots[i] = SlotRow{};
    g_slots[i].slot = slot;
    g_slots[i].proxy = proxy;
    ++g_nslots;
  }
  ++g_slots[i].writes;
  if (swapped) ++g_slots[i].swapped;
  g_slots[i].id = w30 & 0xffffffu;
  g_slots[i].mask = w30 >> 24;
  g_slots[i].contribution = w34 & 0xffffffu;
  g_slots[i].flags = w34 >> 24;
  g_slots[i].blas = blas;
}

void on_tlas(uint32_t count, uint64_t instances, uint32_t site) {
  if (!g_on.load(std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lk(g_mx);
  if (!current_locked(render_frame())) return;
  for (uint32_t i = 0; i < g_ntlas; ++i)
    if (g_tlas[i].instances == instances && g_tlas[i].count == count && g_tlas[i].site == site) {
      ++g_tlas[i].n;
      return;
    }
  if (g_ntlas >= kMaxTlas) return;
  g_tlas[g_ntlas++] = {count, site, instances, 1};
}

void on_rays(uint32_t so, uint32_t site) {
  if (!g_on.load(std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lk(g_mx);
  if (!current_locked(render_frame())) return;
  ++g_rays_total;
  for (uint32_t i = 0; i < g_nrays; ++i)
    if (g_rays[i].so == so && g_rays[i].site == site) {
      ++g_rays[i].n;
      return;
    }
  if (g_nrays >= kMaxRays) return;
  g_rays[g_nrays++] = {so, site, 1};
}

}  // namespace core::bvh_probe
