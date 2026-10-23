// M12 self-test ([Tests] RtVariantTest, docs/internals.md "RT per node"): the RT sun-shadow library 318cee44 gets a
// raygen variant - patches/rt_variant_test_318cee44.hpp (built from the installed game by
// scripts/make_selftest_blobs.py) holds only `rgtest_rgs_shadow_lit`, a copy of `rgs_shadow_main` whose stores all
// write "lit". It is ADDED next to the engine's library; another client's variant of the same raygen may sit in the
// same state object - priority 100 wins while the test's variant is on.
//   1 = the variant scoped to its own node (CRenderNode_RenderRayTracedGlobalShadow), switched LIVE every 600 runs of
//       that node: the RT sun shadows vanish for 10 s, come back for 10 s ... (cascades keep theirs - another raygen).
//   2 = scoped to a foreign node (CRenderNode_VolumetricFog), always on: nothing changes on screen, out_of_scope grows,
//       swapped stays 0.

#include <atomic>
#include <cstdio>

#include "selftest.hpp"
#include "hash_literal.hpp"
#if RG_SELFTEST_GAME_BLOBS
#include "patches/rt_variant_test_318cee44.hpp"
#endif

namespace selftest::rt_variant_test {
namespace {

constexpr uint32_t kWindow = 600;
const rg_host* g_host = nullptr;
rg_handle g_var = 0;
int g_mode = 0;
std::atomic<uint32_t> g_runs{0};

void report(uint32_t n, bool on) {
  rg_rt_variant_stats st;
  g_host->rt_variant_stats(g_var, &st);
  char line[300];
  std::snprintf(line, sizeof line,
                "rt variant test (mode %d): GlobalShadow run #%u, variant %s | swapped %llu out_of_scope %llu off %llu "
                "missing %llu", g_mode, n, on ? "ON" : "off", static_cast<unsigned long long>(st.swapped),
                static_cast<unsigned long long>(st.out_of_scope), static_cast<unsigned long long>(st.off),
                static_cast<unsigned long long>(st.missing));
  const bool bad = st.missing || (g_mode == 2 && st.swapped) || (g_mode == 1 && n >= 2 * kWindow && !st.swapped);
  g_host->log(bad ? RG_LOG_WARN : RG_LOG_INFO, line);
}

uint32_t on_shadow(void*, const rg_node_info*) {
  const uint32_t n = g_runs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n % kWindow) return 0;
  const bool on = g_mode == 1 ? ((n / kWindow) & 1u) != 0 : true;  // mode 1: off, on, off, ... per window
  if (g_mode == 1) g_host->rt_variant_set(g_var, on ? 1u : 0u);
  report(n, on);
  return 0;
}

}  // namespace

void register_with(const rg_host* host, int mode) {
  if (mode != 1 && mode != 2) return;
#if !RG_SELFTEST_GAME_BLOBS
  host->log(RG_LOG_WARN, "rt variant test: NOT built - configure with RG_GAME_DIR and RG_DXC_DIR (docs/internals.md "
                         "\"Self-tests\")");
#else
  g_host = host;
  g_mode = mode;
  static const rg_blob blob = {patches::kRtVariantTest318cee44Bytes, patches::kRtVariantTest318cee44Size};
  const auto h = hash_literal::hash_from_hex("535195dc118b6fb6d0efd79e8be4115a");
  const rg_handle lib = host->rtlib_register("rt_variant_test_318cee44 (M12 self-test)", h.data(), blob, 100);
  const char* node = mode == 1 ? "CRenderNode_RenderRayTracedGlobalShadow" : "CRenderNode_VolumetricFog";
  g_var = lib > 0 ? host->rt_variant(lib, "rgs_shadow_main", "rgtest_rgs_shadow_lit", node) : lib;
  if (g_var > 0 && mode == 1) host->rt_variant_set(g_var, 0);  // the first window is vanilla
  // One RT lease per register (rg_lease_desc): a second RT lease on a register another client holds must be refused with
  // RG_E_TAKEN, before its blobs are checked (it is never staged). CS b3 is taken by a client that holds an RT lease
  // there; with no such client the probe is skipped, not failed.
  if (lib > 0) {
    static const uint8_t zeros[256] = {};
    rg_lease_desc l{};
    l.name = "rt lease conflict probe (self-test)";
    l.stage = RG_STAGE_CS;
    l.kind = RG_KIND_CBV;
    l.reg = 3;
    l.cbuffer = host->cbuffer_create(256, 1, zeros);
    l.rtlibs = &lib;
    l.rtlib_count = 1;
    const rg_handle r = l.cbuffer > 0 ? host->lease_register(&l) : l.cbuffer;
    char line[200];
    std::snprintf(line, sizeof line, "rt variant test: a second RT lease on CS b3 = %d - %s", r,
                  r == RG_E_TAKEN ? "PASS (TAKEN)" : "SKIPPED (no other client holds an RT lease on CS b3)");
    host->log(RG_LOG_INFO, line);
    // One export name per state object: a second library on 318cee44 exporting the same variant name must be refused
    // with RG_E_TAKEN, and its library with it (a variant-less library would be swapped in for the engine's).
    const rg_handle lib2 = host->rtlib_register("rt variant name probe (self-test)", h.data(), blob, 0);
    const rg_handle v2 = lib2 > 0 ? host->rt_variant(lib2, "rgs_shadow_main", "rgtest_rgs_shadow_lit", node) : lib2;
    std::snprintf(line, sizeof line, "rt variant test: the same variant name on a second library = %d - %s", v2,
                  v2 == RG_E_TAKEN ? "PASS (TAKEN)" : "FAIL (expected RG_E_TAKEN = -2)");
    host->log(v2 == RG_E_TAKEN ? RG_LOG_INFO : RG_LOG_WARN, line);
  }
  if (lib <= 0 || g_var <= 0 ||
      host->node_wrap("CRenderNode_RenderRayTracedGlobalShadow", &on_shadow, nullptr, nullptr) <= 0) {
    char line[160];
    std::snprintf(line, sizeof line, "rt variant test: NOT registered (lib %d, variant %d)", lib, g_var);
    host->log(RG_LOG_ERROR, line);
  }
#endif
}

}  // namespace selftest::rt_variant_test
