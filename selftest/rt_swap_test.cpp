// M3 SWAP self-test ([Tests] RtSwapTest, docs/internals.md "RT libraries (M3)"): the RT sun-shadow library 318cee44
// swapped WHOLE for patches/rt_swap_test_318cee44.hpp - the engine's own library (built from the installed game by
// scripts/make_selftest_blobs.py) with every store of rgs_shadow_main written "lit" IN PLACE. It has no variant, so the
// host swaps it in for the engine's library, in the creating thread's subobject list (no lock, the engine's blob
// untouched). A swap is bake-time: no live switch.
//   1 = on: the RT sun shadows are gone for the whole session (cascades keep theirs - another raygen). A client that
//       varies rgs_shadow_main inside CRenderNode_RenderRayTracedGlobalShadow (CRT's grass contact shadow) runs its
//       variant there instead - switch it off for this test.
// Log: rt_swap_test's rtlib_stats every 600 runs of that node - `swapped` > 0, `failed` 0.

#include <atomic>
#include <cstdio>

#include "selftest.hpp"
#include "hash_literal.hpp"
#if RG_SELFTEST_GAME_BLOBS
#include "patches/rt_swap_test_318cee44.hpp"
#endif

namespace selftest::rt_swap_test {
namespace {

constexpr uint32_t kWindow = 600;
const rg_host* g_host = nullptr;
rg_handle g_lib = 0;
std::atomic<uint32_t> g_runs{0};

uint32_t on_shadow(void*, const rg_node_info*) {
  const uint32_t n = g_runs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n % kWindow) return 0;
  rg_rtlib_stats st;
  g_host->rtlib_stats(g_lib, &st);
  char line[240];
  const bool pass = st.state == RG_STATE_OK && st.swapped > 0 && st.added == 0 && st.failed == 0;
  std::snprintf(line, sizeof line, "rt swap test: GlobalShadow run #%u | state %u swapped %u added %u failed %u - %s",
                n, st.state, st.swapped, st.added, st.failed, pass ? "PASS (RT sun shadows should be gone)" : "FAIL");
  g_host->log(pass ? RG_LOG_INFO : RG_LOG_WARN, line);
  return 0;
}

}  // namespace

void register_with(const rg_host* host, int mode) {
  if (mode != 1) return;
#if !RG_SELFTEST_GAME_BLOBS
  host->log(RG_LOG_WARN, "rt swap test: NOT built - configure with RG_GAME_DIR and RG_DXC_DIR (docs/internals.md "
                         "\"Self-tests\")");
#else
  g_host = host;
  static const rg_blob blob = {patches::kRtSwapTest318cee44Bytes, patches::kRtSwapTest318cee44Size};
  const auto h = hash_literal::hash_from_hex("318cee441b4f0c3b27de656dff3c59c0");
  g_lib = host->rtlib_register("rt_swap_test_318cee44 (M3 swap self-test)", h.data(), blob, 100);
  if (g_lib <= 0 || host->node_wrap("CRenderNode_RenderRayTracedGlobalShadow", &on_shadow, nullptr, nullptr) <= 0) {
    char line[120];
    std::snprintf(line, sizeof line, "rt swap test: NOT registered (lib %d)", g_lib);
    host->log(RG_LOG_ERROR, line);
  }
#endif
}

}  // namespace selftest::rt_swap_test
