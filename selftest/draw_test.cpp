// DrawTest ([Tests] DrawTest = true, docs/internals.md "Work on the engine's command list (M4)", the graphics wing):
//   * a draw SUBSCRIPTION on the visible sky's PS 5f079e10 (m_shaderSkyColorWithSun, CRenderNode_RenderBackground, every
//     frame in every mode) and one on the sky-scattering pass's VS 69090ccd (a VS-keyed one) - each logs the kind of
//     draw that reported it, so the log shows which GpuApi draw entry the sky goes through;
//   * a DRAW COMPANION of 5f079e10 with no bytecode of its own, only FILL_MODE_WIREFRAME: the same draw again, as edges;
//   * a PASS AFTER THE DRAW that touches nothing but returns RG_PASS_TOUCHED_STATE - the host's restore path mid-node.
// Cycled in phases of 600 sky draws (~10 s): none, companion, pass. Logged per phase: companion draws must grow
// exactly by the requests accepted in the companion phase and not at all in the others; the pass must run exactly in
// its phase. On screen (far off, near the horizon - 5f079e10 is the sky's bottom layer): wireframe sky edges in the
// companion phase; nothing changes in the pass phase. Registration refusals: stage CS, a companion with neither bytecode
// nor state.

#include <atomic>
#include <cstddef>
#include <cstdio>

#include "hash_literal.hpp"
#include "selftest.hpp"

namespace selftest::draw_test {
namespace {

constexpr uint32_t kPhase = 600;
constexpr const char* kName[3] = {"none", "companion", "pass after draw"};

const rg_host* g_host = nullptr;
rg_handle g_comp = 0, g_pass = 0;
std::atomic<uint32_t> g_sky{0};  // sky draws reported
std::atomic<uint64_t> g_vs_calls{0}, g_kinds[4] = {}, g_system{0};
std::atomic<uint64_t> g_requested{0}, g_refused{0}, g_pass_runs{0}, g_pass_accepted{0};
uint64_t g_draws0 = 0, g_req0 = 0, g_pass0 = 0;
uint32_t g_ok = 0, g_bad = 0;

uint64_t comp_draws() {
  rg_patch_stats st{};
  g_host->draw_companion_stats(g_comp, &st);
  return st.swaps;
}

uint32_t on_pass(void*, void*) {
  g_pass_runs.fetch_add(1, std::memory_order_relaxed);
  return RG_PASS_TOUCHED_STATE;  // nothing touched: exercises the host's restore between two engine draws
}

void on_sky(void*, const rg_draw_info* info) {
  const uint32_t n = g_sky.fetch_add(1, std::memory_order_relaxed);
  if (info->kind < 4) g_kinds[info->kind].fetch_add(1, std::memory_order_relaxed);
  if (info->flags & RG_DRAW_SYSTEM) g_system.fetch_add(1, std::memory_order_relaxed);
  const uint32_t phase = (n / kPhase) % 3, at = n % kPhase;
  if (at == 0) {
    g_draws0 = comp_draws();
    g_req0 = g_requested.load();
    g_pass0 = g_pass_runs.load();
  }
  if (phase == 1) (g_host->draw_companion_request(g_comp) ? g_requested : g_refused).fetch_add(1);
  if (phase == 2 && g_host->pass_after_draw(g_pass, &on_pass, nullptr)) g_pass_accepted.fetch_add(1);
  if (at != kPhase - 1) return;
  // This draw's companion / pass are recorded after we return: the phase's last one is not counted yet.
  const uint64_t d = comp_draws() - g_draws0, r = g_requested.load() - g_req0, p = g_pass_runs.load() - g_pass0;
  const bool ok = phase == 1 ? (r > 0 && d + 1 == r && p == 0) : phase == 2 ? (d == 0 && p + 1 >= kPhase) : (d == 0 && p == 0);
  (ok ? g_ok : g_bad)++;
  char line[400];
  std::snprintf(line, sizeof line,
                "draw test phase '%s': companion draws +%llu (requests +%llu, refused %llu), passes after draw +%llu | "
                "sky draws by kind: instanced %llu indexed %llu indirect %llu indexed-indirect %llu system %llu | VS "
                "69090ccd draws %llu -> %s (%u pass, %u fail)",
                kName[phase], static_cast<unsigned long long>(d), static_cast<unsigned long long>(r),
                static_cast<unsigned long long>(g_refused.load()), static_cast<unsigned long long>(p),
                static_cast<unsigned long long>(g_kinds[0].load()), static_cast<unsigned long long>(g_kinds[1].load()),
                static_cast<unsigned long long>(g_kinds[2].load()), static_cast<unsigned long long>(g_kinds[3].load()),
                static_cast<unsigned long long>(g_system.load()), static_cast<unsigned long long>(g_vs_calls.load()),
                ok ? "PASS" : "FAIL", g_ok, g_bad);
  g_host->log(ok ? RG_LOG_INFO : RG_LOG_ERROR, line);
}

void on_vs(void*, const rg_draw_info*) { g_vs_calls.fetch_add(1, std::memory_order_relaxed); }

}  // namespace

void register_with(const rg_host* host, bool on) {
  if (!on) return;
  g_host = host;
  if (host->struct_size < offsetof(rg_host, pass_after_draw) + sizeof(void*)) {
    host->log(RG_LOG_ERROR, "draw test: the host is older than the draw wing - not run");
    return;
  }
  const auto sky = hash_literal::hash_from_hex("b932ca8eb30728a8635f07647830cfd9");
  const auto vs = hash_literal::hash_from_hex("69090ccdf2e91e0bd1cc70d88a0b8434");
  rg_pso_state wire{};
  wire.struct_size = sizeof wire;
  wire.set = RG_PSO_FILL;
  wire.fill_mode = 2;  // D3D12_FILL_MODE_WIREFRAME
  g_comp = host->draw_companion_register("selftest: sky PS 5f079e10, wireframe", RG_STAGE_PS, sky.data(), {nullptr, 0},
                                         &wire);
  g_pass = host->pass_register("selftest: after the sky draw");
  const rg_handle sub = host->draw_subscribe("draw test: sky PS 5f079e10", RG_STAGE_PS, sky.data(), &on_sky, nullptr);
  const rg_handle vsub = host->draw_subscribe("draw test: sky VS 69090ccd", RG_STAGE_VS, vs.data(), &on_vs, nullptr);
  const rg_handle bad_stage = host->draw_subscribe("draw test: CS", RG_STAGE_CS, sky.data(), &on_vs, nullptr);
  const rg_handle empty = host->draw_companion_register("draw test: empty", RG_STAGE_PS, sky.data(), {nullptr, 0}, nullptr);
  const bool pass = g_comp > 0 && g_pass > 0 && sub > 0 && vsub > 0 && bad_stage == RG_E_ARG && empty == RG_E_ARG;
  char line[240];
  std::snprintf(line, sizeof line,
                "draw test: registered - companion %d, pass %d, subscriptions %d / %d; stage CS -> %d, a companion with "
                "nothing to change -> %d - %s (expected -1 -1)",
                g_comp, g_pass, sub, vsub, bad_stage, empty, pass ? "PASS" : "FAIL");
  host->log(pass ? RG_LOG_INFO : RG_LOG_ERROR, line);
}

}  // namespace selftest::draw_test
