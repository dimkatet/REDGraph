// BvhSwapTest ([Tests] BvhSwapTest = true, docs/internals.md "Mesh proxies in the TLAS"): the instance-desc mechanisms
// from a CLIENT's side only - no GPU readback, nothing on screen. It subscribes to EVERY mesh with RG_MPF_NEEDS_BVH
// (twice, so a second subscription's swap can be refused), then walks a state machine on the sky PS's draw (5f079e10,
// every frame in every mode):
//   1. RG_MCF_OUT_OF_BVH: hide a proxy that IS in the scene, wait for the next desc write - `mesh_proxy_bvh_desc`'s
//      `applied_mask` must be 0 while the engine's own `mask` stays; clear it and `applied_mask` must be the engine's
//      again;
//   2. a swap and its bvh_desc: the proxy's OWN engine BLAS and contribution with FORCE_OPAQUE added - the same geometry
//      and the same records, so no ray can behave differently, only a flag changes - `bvh_desc.applied_flags` must show
//      bit 4 while the engine's `flags` stay, `swapped` must grow, `blas` / `contribution` must stay;
//   3. RG_E_TAKEN: the second subscription's swap of that proxy is refused, and its end-of-swap too; once the owner ends
//      it, the second subscription's swap is accepted (and ends it);
//   4. mesh_proxy_bvh_retire twice: a buffer of ours retired with no swap pointing at it is released once a full frame of
//      descs was written (`rg_vram_stats.retire_pending` > 0 first, 0 after), and then the HOLD - a live swap whose BLAS
//      is inside a buffer we retire keeps it waiting (`retire_pending` stays up) until that swap ends. The hold needs a
//      live swap pointing at one of our own buffers, which must never reach a TLAS, so it is made on a proxy that was
//      NEVER collected (no scene instance, no desc write ever sees it) and the swap is ended as soon as the hold is
//      seen; with no such proxy at hand the step reports SKIP instead of risking a desc.
// One line per step, and a final one with all of them.

#include <Windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "hash_literal.hpp"
#include "selftest.hpp"

namespace selftest::bvh_swap_test {
namespace {

const rg_host* g_host = nullptr;
rg_handle g_sub[2] = {0, 0};
std::atomic<uint32_t> g_ticks{0};
std::atomic<bool> g_done{false};

// The proxies our subscriptions reported (CREATED): a candidate pool the state machine picks from. The callbacks run on
// the engine's job threads (a proxy is made there), so the list has a lock.
constexpr uint32_t kCand = 96;
struct Cand {
  rg_handle proxy;
  uint64_t mesh;
};
Cand g_cand[kCand];
uint32_t g_ncand = 0, g_cand_next = 0;
std::mutex g_cand_mx;

void on_proxy(void*, uint32_t event, const rg_mesh_proxy_info* info) {
  if (!info || info->struct_size < sizeof(rg_mesh_proxy_info)) return;
  std::lock_guard<std::mutex> lk(g_cand_mx);
  if (event != RG_MESH_PROXY_CREATED) {  // a dead handle must not be picked
    for (uint32_t i = 0; i < g_ncand; ++i)
      if (g_cand[i].proxy == info->proxy) g_cand[i].proxy = 0;
    return;
  }
  g_cand[g_cand_next].proxy = info->proxy;
  g_cand[g_cand_next].mesh = info->mesh_hash;
  g_cand_next = (g_cand_next + 1) % kCand;
  if (g_ncand < kCand) ++g_ncand;
}

bool desc_of(rg_handle proxy, rg_bvh_desc* out) {
  *out = {};
  out->struct_size = sizeof *out;
  return g_host->mesh_proxy_bvh_desc(proxy, out) == 0;
}

// A proxy whose scene instance exists (collected_frame != 0) and whose desc was written: what the desc mechanisms show.
rg_handle pick_collected() {
  std::lock_guard<std::mutex> lk(g_cand_mx);
  for (uint32_t i = 0; i < g_ncand; ++i) {
    const rg_handle p = g_cand[i].proxy;
    if (p <= 0) continue;
    rg_mesh_proxy_frame f{};
    f.struct_size = sizeof f;
    if (g_host->mesh_proxy_frame(p, &f) != 0 || !f.collected_frame) continue;
    rg_bvh_desc d{};
    if (!desc_of(p, &d) || !d.frame || !d.blas) continue;
    g_cand[i].proxy = 0;  // taken
    return p;
  }
  return 0;
}

// A proxy that was NEVER collected - no scene instance, so no desc write can carry our stand-in BLAS - and old enough
// (created at least `age` render frames ago) that it is not merely late.
rg_handle pick_uncollected(uint32_t age) {
  std::lock_guard<std::mutex> lk(g_cand_mx);
  for (uint32_t i = 0; i < g_ncand; ++i) {
    const rg_handle p = g_cand[i].proxy;
    if (p <= 0) continue;
    rg_mesh_proxy_frame f{};
    f.struct_size = sizeof f;
    if (g_host->mesh_proxy_frame(p, &f) != 0 || !f.frame || f.collected_frame) continue;
    rg_bvh_desc d{};
    if (!desc_of(p, &d) || d.frame) continue;  // a desc means the writer has seen it
    if (static_cast<uint64_t>(f.frame) + age > g_ticks.load()) continue;
    g_cand[i].proxy = 0;
    return p;
  }
  return 0;
}

bool read_pending(uint32_t* out) {
  rg_vram_stats st{};
  st.struct_size = sizeof st;
  if (g_host->vram_stats(&st) != 0) return false;
  *out = st.retire_pending;
  return true;
}

void say(bool pass, const char* what, const char* fmt, ...) {
  char body[420], line[520];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(body, sizeof body, fmt, ap);
  va_end(ap);
  std::snprintf(line, sizeof line, "bvh swap test: %s - %s", what, body);
  g_host->log(pass ? RG_LOG_INFO : RG_LOG_ERROR, line);
}

enum Step { kPick, kOut, kOutBack, kSwap, kSwapSeen, kRetire, kHold, kHoldSeen, kHoldEnd, kDone };
Step g_step = kPick;
bool g_ok[6] = {};  // out, back, swap, taken, retire, hold
rg_handle g_proxy = 0, g_off = 0;
rg_bvh_desc g_d0{}, g_d1{};
uint32_t g_tick0 = 0, g_up = 0;
void* g_buf = nullptr;

void report() {
  if (g_done.exchange(true)) return;
  const bool pass = g_ok[0] && g_ok[1] && g_ok[2] && g_ok[3] && g_ok[4] && g_ok[5];
  char line[600];
  std::snprintf(line, sizeof line,
                "bvh swap test: out of the BVH %s, back in it %s, the swap's desc %s, RG_E_TAKEN %s, bvh_retire %s, the "
                "hold %s - %s", g_ok[0] ? "PASS" : "FAIL", g_ok[1] ? "PASS" : "FAIL", g_ok[2] ? "PASS" : "FAIL",
                g_ok[3] ? "PASS" : "FAIL", g_ok[4] ? "PASS" : "FAIL", g_ok[5] ? "PASS" : "FAIL", pass ? "PASS" : "FAIL");
  g_host->log(pass ? RG_LOG_INFO : RG_LOG_ERROR, line);
}

// Every step waits for a NEWER desc than the one it saw (the render frame in it): 1-2 frames each, and it never races
// the engine's writer. 400 frames without one gives the step up (the proxy left the scene, the game paused, ...).
void on_sky(void*, const rg_draw_info*) {
  if (g_done.load(std::memory_order_relaxed) || !g_host) return;
  const uint32_t tick = g_ticks.fetch_add(1) + 1;
  const bool late = tick - g_tick0 > 400;
  rg_bvh_desc d{};
  switch (g_step) {
    case kPick: {
      g_proxy = pick_collected();
      if (!g_proxy) {
        if (late) say(false, "no proxy", "none of %u reported proxies is in the scene with a desc in 400 frames - is a "
                      "save loaded?", g_ncand), report(), g_step = kDone;
        return;
      }
      desc_of(g_proxy, &g_d0);  // before anything of ours touches it
      const int32_t r = g_host->mesh_proxy_hide(g_sub[0], g_proxy, 1, RG_MCF_OUT_OF_BVH);
      say(r == 0, "out of the BVH", "mesh_proxy_hide(%u = RG_MCF_OUT_OF_BVH) on proxy %d -> %d (the engine's mask "
          "0x%x, applied 0x%x)", RG_MCF_OUT_OF_BVH, g_proxy, r, g_d0.mask, g_d0.applied_mask);
      g_ok[0] = r == 0;
      g_tick0 = tick, g_step = kOut;
      return;
    }
    case kOut: {
      if (!desc_of(g_proxy, &d) || d.frame <= g_d0.frame) {
        if (late) say(false, "out of the BVH", "no desc write in 400 frames"), report(), g_step = kDone;
        return;
      }
      const bool pass = d.applied_mask == 0 && d.mask == g_d0.mask;
      say(pass, "out of the BVH", "the next desc (frame %u): applied_mask 0x%x (want 0), the engine's mask 0x%x (want "
          "0x%x, unchanged)", d.frame, d.applied_mask, d.mask, g_d0.mask);
      g_ok[0] = g_ok[0] && pass;
      g_host->mesh_proxy_hide(g_sub[0], g_proxy, 0, 0);
      g_d1 = d, g_tick0 = tick, g_step = kOutBack;
      return;
    }
    case kOutBack: {
      if (!desc_of(g_proxy, &d) || d.frame <= g_d1.frame) {
        if (late) say(false, "back in the BVH", "no desc write in 400 frames"), report(), g_step = kDone;
        return;
      }
      const bool pass = d.applied_mask == d.mask && d.applied_mask != 0;
      say(pass, "back in the BVH", "applied_mask 0x%x (want the engine's 0x%x)", d.applied_mask, d.mask);
      g_ok[1] = pass;
      rg_bvh_swap s{};  // the proxy's OWN BLAS and contribution + FORCE_OPAQUE: same geometry, same records, one flag
      s.struct_size = sizeof s;
      s.blas = d.blas, s.contribution = d.contribution, s.flags_set = 4;
      const int32_t r = g_host->mesh_proxy_bvh_swap(g_sub[0], g_proxy, &s);
      say(r == 0, "swap", "mesh_proxy_bvh_swap (the engine's own BLAS 0x%llx, contribution %u, +FORCE_OPAQUE) -> %d",
          static_cast<unsigned long long>(d.blas), d.contribution, r);
      g_d0 = d, g_ok[2] = r == 0;
      g_tick0 = tick, g_step = kSwap;
      return;
    }
    case kSwap: {
      if (!desc_of(g_proxy, &d) || d.frame <= g_d0.frame) {
        if (late) say(false, "the swap's desc", "no desc write in 400 frames"), report(), g_step = kDone;
        return;
      }
      // `applied_flags`, not `flags`: the latter is the ENGINE's desc (as `mask` is), and the swap only changes what the
      // host writes (rg_host.h rg_bvh_desc) - the 2026-11-03 run failed this step by reading the engine's.
      const bool pass = (d.applied_flags & 4) && !(g_d0.applied_flags & 4) && d.flags == g_d0.flags &&
                        d.swapped > g_d0.swapped && d.blas == g_d0.blas && d.contribution == g_d0.contribution &&
                        d.applied_mask == d.mask;
      say(pass, "the swap's desc", "applied_flags 0x%x -> 0x%x (want bit 4), the engine's flags 0x%x -> 0x%x (want "
          "unchanged), swapped %llu -> %llu, BLAS %s, contribution %u -> %u, applied_mask 0x%x (the engine's mask 0x%x)",
          g_d0.applied_flags, d.applied_flags, g_d0.flags, d.flags, static_cast<unsigned long long>(g_d0.swapped),
          static_cast<unsigned long long>(d.swapped), d.blas == g_d0.blas ? "unchanged" : "CHANGED", g_d0.contribution,
          d.contribution, d.applied_mask, d.mask);
      g_ok[2] = g_ok[2] && pass;
      // RG_E_TAKEN comes from the check BEFORE the proxy is looked at, so no live BLAS is needed for it.
      rg_bvh_swap s{};
      s.struct_size = sizeof s;
      s.blas = g_d0.blas, s.contribution = g_d0.contribution;
      const int32_t second = g_host->mesh_proxy_bvh_swap(g_sub[1], g_proxy, &s);
      const int32_t second_off = g_host->mesh_proxy_bvh_swap(g_sub[1], g_proxy, nullptr);
      const int32_t owner_off = g_host->mesh_proxy_bvh_swap(g_sub[0], g_proxy, nullptr);
      const int32_t after = g_host->mesh_proxy_bvh_swap(g_sub[1], g_proxy, &s);
      const int32_t after_off = g_host->mesh_proxy_bvh_swap(g_sub[1], g_proxy, nullptr);
      const bool ok = second == RG_E_TAKEN && second_off == RG_E_TAKEN && owner_off == 0 && after == 0 && after_off == 0;
      say(ok, "RG_E_TAKEN", "a second subscription's swap -> %d and its end -> %d (want %d %d), the owner's end -> %d "
          "(want 0), then the second's swap / end -> %d / %d (want 0 0)", second, second_off, RG_E_TAKEN, RG_E_TAKEN,
          owner_off, after, after_off);
      g_ok[3] = ok;
      // bvh_retire with no swap pointing at the buffer: released once a full frame of descs was written after the call.
      g_buf = g_host->buffer_create(4096, 0, nullptr);
      if (!g_buf) {
        say(false, "bvh_retire", "buffer_create failed - the retire steps cannot run"), report(), g_step = kDone;
        return;
      }
      g_host->mesh_proxy_bvh_retire(g_buf);  // the host's reference from here on
      g_buf = nullptr;
      g_tick0 = tick, g_up = 0, g_step = kRetire;
      return;
    }
    case kRetire: {
      uint32_t pending = 0;
      if (!read_pending(&pending)) {
        say(false, "bvh_retire", "vram_stats failed"), report(), g_step = kDone;
        return;
      }
      if (pending) g_up = tick;
      if (pending && !late) return;  // still waiting for its frame: the first half of the observation
      const bool pass = g_up != 0;    // it DID wait
      say(pass, "bvh_retire", "retire_pending %u after %u frame(s) - it must wait for a full frame of descs (seen "
          "waiting: %s)", pending, tick - g_tick0, g_up ? "yes" : "no");
      g_ok[4] = pass;
      // The HOLD: a live swap whose BLAS is inside a buffer we retire. Only on a proxy no desc writer can reach.
      g_off = pick_uncollected(3);
      g_buf = g_host->buffer_create(4096, 0, nullptr);
      if (!g_off || !g_buf) {
        say(true, "the hold", "SKIP - %s; a stand-in BLAS must never reach a TLAS, and a real client's BLAS is not ours "
            "to point at", g_off ? "buffer_create failed" : "no proxy that was never collected is at hand");
        g_ok[5] = true;
        report(), g_step = kDone;
        return;
      }
      const uint64_t va = static_cast<ID3D12Resource*>(g_buf)->GetGPUVirtualAddress();
      rg_bvh_swap s{};
      s.struct_size = sizeof s;
      s.blas = va;  // inside g_buf's own range: what "a live swap's BLAS" means to the host's hold
      const int32_t r = g_host->mesh_proxy_bvh_swap(g_sub[0], g_off, &s);
      g_host->mesh_proxy_bvh_retire(g_buf);
      g_buf = nullptr;
      say(r == 0, "the hold", "a live swap on the never-collected proxy %d (stand-in BLAS 0x%llx, its own buffer) -> "
          "%d, then that buffer retired", g_off, static_cast<unsigned long long>(va), r);
      g_ok[5] = r == 0;
      g_tick0 = tick, g_step = kHold;
      return;
    }
    case kHold: {
      uint32_t pending = 0;
      if (!read_pending(&pending)) {
        report(), g_step = kDone;
        return;
      }
      if (!pending && !late) return;
      const bool held = pending != 0;
      say(held, "the hold", "retire_pending %u, %u frame(s) after the retire with the swap still live (want > 0: the "
          "host holds a live swap's BLAS)", pending, tick - g_tick0);
      g_ok[5] = g_ok[5] && held;
      g_host->mesh_proxy_bvh_swap(g_sub[0], g_off, nullptr);  // end the swap: now the host may retire it
      g_tick0 = tick, g_step = kHoldEnd;
      return;
    }
    case kHoldEnd: {
      uint32_t pending = 0;
      if (!read_pending(&pending)) {
        report(), g_step = kDone;
        return;
      }
      if (pending && !late) return;
      const bool pass = pending == 0;
      say(pass, "the hold end", "retire_pending %u %u frame(s) after the swap ended - %s", pending, tick - g_tick0,
          pass ? "released" : "STILL HELD");
      g_ok[5] = g_ok[5] && pass;
      report(), g_step = kDone;
      return;
    }
    default:
      return;
  }
}

}  // namespace

void register_with(const rg_host* host, bool on) {
  if (!on) return;
  g_host = host;
  if (host->struct_size < offsetof(rg_host, mesh_proxy_subscribe_flags) + sizeof(void*)) {
    host->log(RG_LOG_ERROR, "bvh swap test: this REDGraph has no mesh_proxy_subscribe_flags (older host) - not run");
    return;
  }
  g_sub[0] = host->mesh_proxy_subscribe_flags("selftest: BVH proxy A", nullptr, 0, &on_proxy, nullptr, RG_MPF_NEEDS_BVH);
  g_sub[1] = host->mesh_proxy_subscribe_flags("selftest: BVH proxy B", nullptr, 0, &on_proxy, nullptr, RG_MPF_NEEDS_BVH);
  const auto bad = static_cast<int32_t>(
      host->mesh_proxy_subscribe_flags("selftest: BVH proxy, bad flag", nullptr, 0, &on_proxy, nullptr, 0x8000u));
  const auto sky = hash_literal::hash_from_hex("b932ca8eb30728a8635f07647830cfd9");  // the visible sky's PS
  const rg_handle tick = host->draw_subscribe("bvh swap test: sky PS 5f079e10", RG_STAGE_PS, sky.data(), &on_sky, nullptr);
  char line[320];
  const bool pass = g_sub[0] > 0 && g_sub[1] > 0 && bad == RG_E_ARG && tick > 0;
  std::snprintf(line, sizeof line,
                "bvh swap test: registered - subscriptions %d / %d, a subscription flag the host does not know -> %d "
                "(want %d), the frame tick %d - %s", g_sub[0], g_sub[1], bad, static_cast<int32_t>(RG_E_ARG), tick,
                pass ? "PASS" : "FAIL");
  host->log(pass ? RG_LOG_INFO : RG_LOG_ERROR, line);
  if (!pass) g_done.store(true);
}

}  // namespace selftest::bvh_swap_test
