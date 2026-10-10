// StateTest ([Tests] StateTest = true, docs/internals.md "PSO patches (M1)", pipeline state): a patch of the visible
// sky's PS 5f079e10 (m_shaderSkyColorWithSun, CRenderNode_RenderBackground, every frame in every mode) whose two
// variants carry NO bytecode - the engine's PS stays - only pipeline-state edits (rg_host::patch_state):
//   variant 0: FILL_MODE_WIREFRAME           - the sky is drawn as the edges of its geometry only
//   variant 1: render target 0's write mask 0 - the sky pass writes nothing
// Cycled in phases of 600 sky-node runs (~10 s): vanilla, wireframe, no write. On screen ONLY far off: 5f079e10 is the
// bottom layer of the visible sky - the volumetric clouds, fog and aerial composite cover most of it (seen 2026-10-10). Logged per phase: swaps must grow exactly in the two
// edited phases. Priority 100.

#include <atomic>
#include <cstddef>
#include <cstdio>

#include "hash_literal.hpp"
#include "selftest.hpp"

namespace selftest::state_test {
namespace {

constexpr uint32_t kPhase = 600, kSettle = 60;
constexpr const char* kName[3] = {"vanilla", "wireframe", "write mask 0"};

const rg_host* g_host = nullptr;
rg_handle g_patch = 0;
std::atomic<uint32_t> g_runs{0};
uint64_t g_swaps0 = 0;
uint32_t g_pass = 0, g_fail = 0;

uint64_t swaps() {
  rg_patch_stats st{};
  g_host->patch_stats(g_patch, &st);
  return st.swaps;
}

uint32_t on_background(void*, const rg_node_info*) {
  const uint32_t n = g_runs.fetch_add(1, std::memory_order_relaxed);
  const uint32_t phase = (n / kPhase) % 3, at = n % kPhase;
  if (at == 0) g_host->patch_select(g_patch, static_cast<int32_t>(phase) - 1);  // -1 vanilla, 0 wireframe, 1 no write
  if (at == kSettle) g_swaps0 = swaps();
  if (at != kPhase - 1) return 0;
  const uint64_t d = swaps() - g_swaps0;
  const bool ok = (phase != 0) == (d > 0);
  (ok ? g_pass : g_fail)++;
  char line[200];
  std::snprintf(line, sizeof line, "state test phase '%s': swaps +%llu -> %s (%u pass, %u fail)%s", kName[phase],
                static_cast<unsigned long long>(d), ok ? "PASS" : "FAIL", g_pass, g_fail,
                phase == 2 ? " - next: vanilla" : "");
  g_host->log(ok ? RG_LOG_INFO : RG_LOG_ERROR, line);
  return 0;
}

}  // namespace

void register_with(const rg_host* host, bool on) {
  if (!on) return;
  g_host = host;
  if (host->struct_size < offsetof(rg_host, patch_state) + sizeof(void*)) {
    host->log(RG_LOG_ERROR, "state test: the host is older than patch_state - not run");
    return;
  }
  static const rg_blob keep[2] = {{nullptr, 0}, {nullptr, 0}};  // the engine's PS stays
  rg_patch_desc d{};
  d.name = "selftest: sky PS 5f079e10, state only";
  d.stage = RG_STAGE_PS;
  const auto h = hash_literal::hash_from_hex("5f079e105fa45ba904cd166a052372b8");
  for (int k = 0; k < 16; ++k) d.hash[k] = h[k];
  d.variants = keep;
  d.variant_count = 2;
  d.initial_variant = -1;
  d.priority = 100;
  g_patch = host->patch_register(&d);
  rg_pso_state wire{};
  wire.struct_size = sizeof wire;
  wire.set = RG_PSO_FILL;
  wire.fill_mode = 2;  // D3D12_FILL_MODE_WIREFRAME
  rg_pso_state nowrite{};
  nowrite.struct_size = sizeof nowrite;
  nowrite.set = RG_PSO_WRITE_MASK;
  nowrite.rt_mask = 1;
  nowrite.write_mask[0] = 0;
  rg_pso_state bad = wire;
  bad.fill_mode = 7;  // not a fill mode: refused
  const int32_t a = g_patch > 0 ? host->patch_state(g_patch, 0, &wire) : g_patch;
  const int32_t b = g_patch > 0 ? host->patch_state(g_patch, 1, &nowrite) : g_patch;
  const int32_t c = g_patch > 0 ? host->patch_state(g_patch, 0, &bad) : RG_E_ARG;
  const int32_t e = g_patch > 0 ? host->patch_state(g_patch, 2, &wire) : RG_E_ARG;  // no such variant
  const rg_handle wrap = host->node_wrap("CRenderNode_RenderBackground", &on_background, nullptr, nullptr);
  const bool ok = g_patch > 0 && a == 0 && b == 0 && c == RG_E_ARG && e == RG_E_ARG && wrap > 0;
  char line[240];
  std::snprintf(line, sizeof line,
                "state test: %s - patch %d, states %d %d, a bad fill mode -> %d, a variant out of range -> %d "
                "(expected 0 0 %d %d), wrap %d",
                ok ? "registered" : "registration FAILED", g_patch, a, b, c, e, RG_E_ARG, RG_E_ARG, wrap);
  host->log(ok ? RG_LOG_INFO : RG_LOG_ERROR, line);
}

}  // namespace selftest::state_test
