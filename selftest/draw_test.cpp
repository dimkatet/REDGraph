// DrawTest ([Tests] DrawTest = true, docs/internals.md "Work on the engine's command list (M4)", the graphics wing):
//   * a draw SUBSCRIPTION on the visible sky's PS 5f079e10 (m_shaderSkyColorWithSun, CRenderNode_RenderBackground, every
//     frame in every mode) and one on the sky-scattering pass's VS 69090ccd (a VS-keyed one) - each logs the kind of
//     draw that reported it, so the log shows which GpuApi draw entry the sky goes through;
//   * a DRAW COMPANION of 5f079e10 with no bytecode of its own, only FILL_MODE_WIREFRAME: the same draw again, as edges;
//   * a PASS AFTER THE DRAW that touches nothing but returns RG_PASS_TOUCHED_STATE - the host's restore path mid-node;
//   * a LEASED DRAW COMPANION of 5f079e10: our PS (draw_lease.hlsl) painting the color of a cbuffer of ours, leased at
//     PS b11 - a register the engine's draw never binds.
// Cycled in phases of 600 sky draws (~10 s): none, companion, pass, leased companion. Logged per phase: each companion's
// draws must grow exactly by the requests accepted in its phase and not at all in the others; the pass must run exactly
// in its phase. On screen (far off, near the horizon - 5f079e10 is the sky's bottom layer): wireframe sky edges in the
// companion phase; nothing changes in the pass phase; that layer flat green in the leased phase. Registration refusals:
// stage CS, a companion with neither bytecode nor state; a lease on a state-only companion, on a register its PS does
// not read.

#include <atomic>
#include <cstddef>
#include <cstdio>

#include "draw_lease_shader.hpp"
#include "hash_literal.hpp"
#include "selftest.hpp"

namespace selftest::draw_test {
namespace {

constexpr uint32_t kPhase = 600;
constexpr uint32_t kPhases = 4;
constexpr const char* kName[kPhases] = {"none", "companion", "pass after draw", "leased companion"};

const rg_host* g_host = nullptr;
rg_handle g_comp = 0, g_pass = 0, g_lcomp = 0, g_lease = 0;
std::atomic<uint32_t> g_sky{0};  // sky draws reported
std::atomic<uint64_t> g_vs_calls{0}, g_kinds[4] = {}, g_system{0};
std::atomic<uint64_t> g_requested{0}, g_refused{0}, g_pass_runs{0}, g_pass_accepted{0};
std::atomic<uint64_t> g_lrequested{0}, g_lrefused{0};
uint64_t g_draws0 = 0, g_req0 = 0, g_pass0 = 0, g_ldraws0 = 0, g_lreq0 = 0;
uint32_t g_ok = 0, g_bad = 0;

uint64_t comp_draws(rg_handle comp) {
  if (comp <= 0) return 0;
  rg_patch_stats st{};
  g_host->draw_companion_stats(comp, &st);
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
  const uint32_t phase = (n / kPhase) % kPhases, at = n % kPhase;
  if (at == 0) {
    g_draws0 = comp_draws(g_comp);
    g_req0 = g_requested.load();
    g_pass0 = g_pass_runs.load();
    g_ldraws0 = comp_draws(g_lcomp);
    g_lreq0 = g_lrequested.load();
  }
  if (phase == 1) (g_host->draw_companion_request(g_comp) ? g_requested : g_refused).fetch_add(1);
  if (phase == 2 && g_host->pass_after_draw(g_pass, &on_pass, nullptr)) g_pass_accepted.fetch_add(1);
  if (phase == 3) (g_host->draw_companion_request(g_lcomp) ? g_lrequested : g_lrefused).fetch_add(1);
  if (at != kPhase - 1) return;
  // This draw's companion / pass are recorded after we return: the phase's last one is not counted yet.
  const uint64_t d = comp_draws(g_comp) - g_draws0, r = g_requested.load() - g_req0, p = g_pass_runs.load() - g_pass0;
  const uint64_t ld = comp_draws(g_lcomp) - g_ldraws0, lr = g_lrequested.load() - g_lreq0;
  bool ok = phase == 1   ? (r > 0 && d + 1 == r && p == 0)
            : phase == 2 ? (d == 0 && p + 1 >= kPhase)
            : phase == 3 ? (d == 0 && p == 0 && lr > 0 && ld + 1 == lr)
                         : (d == 0 && p == 0);
  if (phase != 3) ok = ok && ld == 0;
  (ok ? g_ok : g_bad)++;
  rg_lease_stats ls{};
  if (g_lease > 0) g_host->lease_stats(g_lease, &ls);
  char line[600];
  std::snprintf(line, sizeof line,
                "draw test phase '%s': companion draws +%llu (requests +%llu, refused %llu), leased companion draws "
                "+%llu (requests +%llu, refused %llu; lease state %u, stages %llu, not staged %llu), passes after draw "
                "+%llu | sky draws by kind: instanced %llu indexed %llu indirect %llu indexed-indirect %llu system %llu "
                "| VS 69090ccd draws %llu -> %s (%u pass, %u fail)",
                kName[phase], static_cast<unsigned long long>(d), static_cast<unsigned long long>(r),
                static_cast<unsigned long long>(g_refused.load()), static_cast<unsigned long long>(ld),
                static_cast<unsigned long long>(lr), static_cast<unsigned long long>(g_lrefused.load()), ls.state,
                static_cast<unsigned long long>(ls.stages), static_cast<unsigned long long>(ls.not_staged),
                static_cast<unsigned long long>(p),
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
  // The leased companion: our PS reads PS b11, a cbuffer of ours (the engine's PS reads b0 b6 b13), through a lease.
  g_lcomp = host->draw_companion_register("selftest: sky PS 5f079e10, leased color", RG_STAGE_PS, sky.data(),
                                          {draw_lease::kPSBytes, draw_lease::kPSSize}, nullptr);
  const float green[64] = {0.0f, 4.0f, 0.0f, 1.0f};  // 256 bytes, the first float4 read
  const rg_handle cb = host->cbuffer_create(256, 2, green);
  rg_lease_desc ld{};
  ld.name = "draw test: leased color";
  ld.stage = RG_STAGE_PS;
  ld.kind = RG_KIND_CBV;
  ld.cbuffer = cb;
  ld.companions = &g_comp;  // state-only: runs the engine's PS, cannot hold a lease
  ld.companion_count = 1;
  ld.reg = 11;
  const rg_handle state_only = host->lease_register(&ld);
  ld.companions = &g_lcomp;
  ld.reg = 12;  // our PS does not read b12
  const rg_handle unread = host->lease_register(&ld);
  ld.reg = 11;
  g_lease = host->lease_register(&ld);
  const bool pass = g_comp > 0 && g_pass > 0 && sub > 0 && vsub > 0 && bad_stage == RG_E_ARG && empty == RG_E_ARG &&
                    g_lcomp > 0 && cb > 0 && g_lease > 0 && state_only == RG_E_ARG && unread == RG_E_BLOB;
  char line[400];
  std::snprintf(line, sizeof line,
                "draw test: registered - companion %d, pass %d, subscriptions %d / %d, leased companion %d, cbuffer %d, "
                "lease %d; stage CS -> %d, a companion with nothing to change -> %d, a lease on a state-only companion "
                "-> %d, on a register its PS does not read -> %d - %s (expected %d %d %d %d)",
                g_comp, g_pass, sub, vsub, g_lcomp, cb, g_lease, bad_stage, empty, state_only, unread,
                pass ? "PASS" : "FAIL", RG_E_ARG, RG_E_ARG, RG_E_ARG, RG_E_BLOB);
  host->log(pass ? RG_LOG_INFO : RG_LOG_ERROR, line);
}

}  // namespace selftest::draw_test
