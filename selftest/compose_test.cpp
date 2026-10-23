// ComposeTest ([Tests] ComposeTest = true, docs/internals.md "PSO patches (M1)", composition): TWO patches of
// different stages on ONE engine PSO - the sky pass's VS 69090ccd (2 variants) and its PS 0c359133 (1 variant), both the
// GAME'S OWN bytecode (patches/selftest_vanilla_*.hpp from scripts/make_selftest_blobs.py), so nothing changes on
// screen; the counters speak. The VS is shared with the paraboloid sky PS 4f1458e1, so the VS patch maps more engine
// PSOs than the PS one, and only the (69090ccd, 0c359133) PSOs carry both.
// The selection cycles through four phases of 600 sky-node runs each (the first 60 runs of a phase not counted: a
// switch lands at the next bind):
//   A  VS 0, PS off  - VS swaps grow, PS swaps do not
//   B  VS 1, PS 0    - both grow (the composed clone: before 2026-10-10 the PS patch never applied here - one
//                      patched stage per PSO, the VS won)
//   C  VS off, PS 0  - PS swaps grow, VS swaps do not
//   D  both off      - neither grows
// Logged per phase with PASS / FAIL. Priority 101: it takes both shaders from any other client (and from
// PatchScopeTest) for the session.

#include <atomic>
#include <cstdio>

#include "hash_literal.hpp"
#if RG_SELFTEST_GAME_BLOBS
#include "patches/selftest_vanilla_0c359133.hpp"
#include "patches/selftest_vanilla_69090ccd.hpp"
#endif
#include "selftest.hpp"

namespace selftest::compose_test {
namespace {

constexpr uint32_t kPhase = 600, kSettle = 60;
struct Phase {
  const char* name;
  int32_t vs, ps;  // the variant selected, -1 = vanilla
};
constexpr Phase kPhases[4] = {{"A: VS 0, PS off", 0, -1}, {"B: VS 1 + PS 0", 1, 0}, {"C: VS off, PS 0", -1, 0},
                              {"D: both off", -1, -1}};

const rg_host* g_host = nullptr;
rg_handle g_vs = 0, g_ps = 0;
std::atomic<uint32_t> g_runs{0};
uint64_t g_vs0 = 0, g_ps0 = 0;  // swaps at the end of the current phase's settle
uint32_t g_pass = 0, g_fail = 0;

uint64_t swaps(rg_handle p) {
  rg_patch_stats st{};
  g_host->patch_stats(p, &st);
  return st.swaps;
}

void select(uint32_t phase) {
  g_host->patch_select(g_vs, kPhases[phase].vs);
  g_host->patch_select(g_ps, kPhases[phase].ps);
}

uint32_t on_sky(void*, const rg_node_info*) {
  const uint32_t n = g_runs.fetch_add(1, std::memory_order_relaxed);
  const uint32_t phase = (n / kPhase) % 4, at = n % kPhase;
  if (at == kSettle) {
    g_vs0 = swaps(g_vs);
    g_ps0 = swaps(g_ps);
  } else if (at == kPhase - 1) {
    const uint64_t dv = swaps(g_vs) - g_vs0, dp = swaps(g_ps) - g_ps0;
    const Phase& ph = kPhases[phase];
    const bool ok = ((ph.vs >= 0) == (dv > 0)) && ((ph.ps >= 0) == (dp > 0));
    (ok ? g_pass : g_fail)++;
    char line[240];
    std::snprintf(line, sizeof line, "compose test %s: VS swaps +%llu, PS swaps +%llu -> %s (%u pass, %u fail)",
                  ph.name, static_cast<unsigned long long>(dv), static_cast<unsigned long long>(dp),
                  ok ? "PASS" : "FAIL", g_pass, g_fail);
    g_host->log(ok ? RG_LOG_INFO : RG_LOG_ERROR, line);
    select((phase + 1) % 4);
  }
  return 0;
}

}  // namespace

void register_with(const rg_host* host, bool on) {
  if (!on) return;
#if !RG_SELFTEST_GAME_BLOBS
  host->log(RG_LOG_WARN, "compose test: NOT built - configure with RG_GAME_DIR and RG_DXC_DIR (docs/internals.md "
                         "\"Self-tests\")");
#else
  g_host = host;
  static const rg_blob vs_blobs[2] = {{patches::kSelftestVanilla69090ccdBytes, patches::kSelftestVanilla69090ccdSize},
                                      {patches::kSelftestVanilla69090ccdBytes, patches::kSelftestVanilla69090ccdSize}};
  static const rg_blob ps_blob = {patches::kSelftestVanilla0c359133Bytes, patches::kSelftestVanilla0c359133Size};
  rg_patch_desc d{};
  d.name = "selftest: VS 69090ccd for itself (compose)";
  d.stage = RG_STAGE_VS;
  const auto hv = hash_literal::hash_from_hex("69090ccdf2e91e0bd1cc70d88a0b8434");
  for (int k = 0; k < 16; ++k) d.hash[k] = hv[k];
  d.variants = vs_blobs;
  d.variant_count = 2;
  d.initial_variant = kPhases[0].vs;
  d.priority = 101;
  g_vs = host->patch_register(&d);
  d.name = "selftest: PS 0c359133 for itself (compose)";
  d.stage = RG_STAGE_PS;
  const auto hp = hash_literal::hash_from_hex("f8a34e57b02fda801d4a3f0e0a0c1a02");
  for (int k = 0; k < 16; ++k) d.hash[k] = hp[k];
  d.variants = &ps_blob;
  d.variant_count = 1;
  d.initial_variant = kPhases[0].ps;
  g_ps = host->patch_register(&d);
  const rg_handle wrap = host->node_wrap("CRenderNode_RenderSkyScattering", &on_sky, nullptr, nullptr);
  char line[200];
  std::snprintf(line, sizeof line, "compose test: %s - VS patch %d, PS patch %d, wrap %d",
                g_vs > 0 && g_ps > 0 && wrap > 0 ? "registered" : "registration FAILED", g_vs, g_ps, wrap);
  host->log(g_vs > 0 && g_ps > 0 && wrap > 0 ? RG_LOG_INFO : RG_LOG_ERROR, line);
#endif
}

}  // namespace selftest::compose_test
