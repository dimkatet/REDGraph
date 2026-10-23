// M11 self-test ([Tests] PatchScopeTest, docs/internals.md "A patch scoped to a node"): a patch of PS 0c359133 (the
// sky cube) whose blob is the GAME'S OWN shader byte for byte (patches/selftest_vanilla_0c359133.hpp, built from the
// installed game by scripts/make_selftest_blobs.py), so nothing can change on screen - only the counters speak.
// Priority 100: it takes the shader from any other client patching it, for the session.
//   1 = scoped to its own node (CRenderNode_RenderSkyScattering): swaps grow, out_of_scope stays 0.
//   2 = scoped to a foreign node (CRenderNode_RenderBackground): swaps stay 0, out_of_scope grows.
// Reported every 600 runs of the sky node.

#include <atomic>
#include <cstdio>

#include "hash_literal.hpp"
#if RG_SELFTEST_GAME_BLOBS
#include "patches/selftest_vanilla_0c359133.hpp"
#endif
#include "selftest.hpp"

namespace selftest::scope_test {
namespace {

const rg_host* g_host = nullptr;
rg_handle g_patch = 0;
int g_mode = 0;
std::atomic<uint32_t> g_runs{0};

uint32_t on_sky(void*, const rg_node_info*) {
  const uint32_t n = g_runs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n % 600) return 0;
  rg_patch_stats st{};
  g_host->patch_stats(g_patch, &st);
  const bool bad = g_mode == 1 ? (st.out_of_scope != 0 || (n >= 1200 && !st.swaps)) : st.swaps != 0;
  char line[240];
  std::snprintf(line, sizeof line, "scope test (mode %d): sky node run #%u | swaps %llu out_of_scope %llu binds %llu%s",
                g_mode, n, static_cast<unsigned long long>(st.swaps), static_cast<unsigned long long>(st.out_of_scope),
                static_cast<unsigned long long>(st.engine_hits), bad ? " - NOT as expected" : "");
  g_host->log(bad ? RG_LOG_WARN : RG_LOG_INFO, line);
  return 0;
}

}  // namespace

void register_with(const rg_host* host, int mode) {
  if (mode != 1 && mode != 2) return;
#if !RG_SELFTEST_GAME_BLOBS
  host->log(RG_LOG_WARN, "scope test: NOT built - configure with RG_GAME_DIR and RG_DXC_DIR (docs/internals.md "
                         "\"Self-tests\")");
#else
  g_host = host;
  g_mode = mode;
  static const rg_blob blob = {patches::kSelftestVanilla0c359133Bytes, patches::kSelftestVanilla0c359133Size};
  rg_patch_desc d{};
  d.name = "selftest: PS 0c359133 swapped for itself (M11)";
  d.stage = RG_STAGE_PS;
  const auto h = hash_literal::hash_from_hex("f8a34e57b02fda801d4a3f0e0a0c1a02");
  for (int k = 0; k < 16; ++k) d.hash[k] = h[k];
  d.variants = &blob;
  d.variant_count = 1;
  d.initial_variant = 0;
  d.priority = 100;
  g_patch = host->patch_register(&d);
  const char* node = mode == 1 ? "CRenderNode_RenderSkyScattering" : "CRenderNode_RenderBackground";
  if (g_patch <= 0 || host->patch_scope(g_patch, node) != 0 ||
      host->node_wrap("CRenderNode_RenderSkyScattering", &on_sky, nullptr, nullptr) <= 0) {
    char line[160];
    std::snprintf(line, sizeof line, "scope test: NOT registered (patch %d)", g_patch);
    host->log(RG_LOG_ERROR, line);
  }
#endif
}

}  // namespace selftest::scope_test
