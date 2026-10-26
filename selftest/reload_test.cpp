// ReloadTest ([Tests] ReloadTest = true, docs/internals.md "Shader hot reload (dev)"): REDGraph's [Dev] ShaderHotReload
// driven the way a developer drives it - files dropped into shader_reload/ next to REDGraph.dll while the game runs.
// A patch of the visible sky's PS 5f079e10 (m_shaderSkyColorWithSun, CRenderNode_RenderBackground, every frame) starts
// as a flat MAGENTA PS compiled at registration; then, every 600 sky-node runs (~10 s):
//   step 1: <stem>.dxbc = a GREEN PS          -> clones rebuilt (clones grows), the clone still binds (swaps grow)
//   step 2: <stem>.dxbc = a compute shader    -> refused (another stage): clones, clone_failed unchanged, swaps grow
//   step 3: <stem>.ll   = text that is no IR  -> refused (assembly): unchanged, swaps grow
//   step 4: <stem>.ll   = a BLUE PS as IR     -> assembled + signed by the host, clones rebuilt, swaps grow
// PASS / FAIL per step; the host's "DEV reload:" lines say why. The files are removed at the end (and at the start).
// The folder is the host's: [Dev] ShaderReloadDir, else shader_reload/ next to REDGraph.dll.
// Needs [Dev] ShaderHotReload = true in REDGraph.ini and dxcompiler.dll + dxil.dll next to REDGraph.dll
// (build_and_deploy.ps1 -Dxc). On screen only far off: the sky's bottom layer, magenta -> green -> blue.
// Run it ALONE among the 5f079e10 tests (StateTest, DrawTest): one patch per shader.

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "hash_literal.hpp"
#include "ini_file.hpp"
#include "selftest.hpp"

namespace selftest::reload_test {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kPhase = 600, kSteps = 4;
constexpr const char* kStem = "selftest_reload_sky_PS";

// A blue PS (dxc 1.8 -T ps_6_0, disassembled, comments cut) - the .ll a developer would edit.
constexpr const char* kBlueIr = R"IR(target datalayout = "e-m:e-p:32:32-i1:32-i8:32-i16:32-i32:32-i64:64-f16:32-f32:32-f64:64-n8:16:32:64"
target triple = "dxil-ms-dx"

define void @main() {
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 0, float 0x3FB99999A0000000)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 1, float 0x3FC99999A0000000)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 2, float 1.000000e+00)
  call void @dx.op.storeOutput.f32(i32 5, i32 0, i32 0, i8 3, float 1.000000e+00)
  ret void
}

; Function Attrs: nounwind
declare void @dx.op.storeOutput.f32(i32, i32, i32, i8, float) #0

attributes #0 = { nounwind }

!llvm.ident = !{!0}
!dx.version = !{!1}
!dx.valver = !{!2}
!dx.shaderModel = !{!3}
!dx.viewIdState = !{!4}
!dx.entryPoints = !{!5}

!0 = !{!"dxcoob 1.8.2502.11 (239921522)"}
!1 = !{i32 1, i32 0}
!2 = !{i32 1, i32 8}
!3 = !{!"ps", i32 6, i32 0}
!4 = !{[6 x i32] [i32 4, i32 4, i32 0, i32 0, i32 0, i32 0]}
!5 = !{void ()* @main, !"main", !6, null, null}
!6 = !{!7, !10, null}
!7 = !{!8}
!8 = !{i32 0, !"SV_Position", i8 9, i8 3, !9, i8 4, i32 1, i8 4, i32 0, i8 0, null}
!9 = !{i32 0}
!10 = !{!11}
!11 = !{i32 0, !"SV_Target", i8 9, i8 16, !9, i8 0, i32 1, i8 4, i32 0, i8 0, !12}
!12 = !{i32 3, i32 15}
)IR";

const rg_host* g_host = nullptr;
rg_handle g_patch = 0;
rg_blob g_green{}, g_cs{};
fs::path g_dir;
std::atomic<uint32_t> g_runs{0};
rg_patch_stats g_at{};  // the stats at the last step
uint32_t g_pass = 0, g_fail = 0;

rg_patch_stats stats() {
  rg_patch_stats st{};
  g_host->patch_stats(g_patch, &st);
  return st;
}

// Written beside, then renamed over: the host never reads half a file.
void drop(const char* ext, const void* bytes, size_t size) {
  const fs::path dst = g_dir / (std::string(kStem) + ext), tmp = g_dir / (std::string(kStem) + ext + ".tmp");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
  }
  MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void clean() {
  std::error_code ec;
  for (const char* ext : {".dxbc", ".ll", ".dxbc.tmp", ".ll.tmp"}) fs::remove(g_dir / (std::string(kStem) + ext), ec);
}

uint32_t on_background(void*, const rg_node_info*) {
  const uint32_t n = g_runs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n % kPhase || n / kPhase > kSteps + 1) return 0;
  const uint32_t step = n / kPhase - 1;  // 0 = the initial magenta phase ends, 1..4 = the steps above end
  const rg_patch_stats now = stats();
  const bool bound = now.swaps > g_at.swaps;
  bool ok = bound;
  const char* what = "magenta (registered)";
  if (step == 1 || step == 4) {
    ok = ok && now.clones > g_at.clones && now.clone_failed == g_at.clone_failed;
    what = step == 1 ? "green .dxbc reloaded" : "blue .ll assembled + reloaded";
  } else if (step == 2 || step == 3) {
    ok = ok && now.clones == g_at.clones && now.clone_failed == g_at.clone_failed;
    what = step == 2 ? "compute .dxbc refused" : "broken .ll refused";
  }
  if (step) (ok ? g_pass : g_fail)++;
  char line[260];
  std::snprintf(line, sizeof line, "reload test step %u (%s): swaps +%llu, clones %u -> %u, failed %u -> %u -> %s%s",
                step, what, static_cast<unsigned long long>(now.swaps - g_at.swaps), g_at.clones, now.clones,
                g_at.clone_failed, now.clone_failed, ok ? "PASS" : "FAIL",
                ok || !step ? "" : " (is [Dev] ShaderHotReload on, DXC next to REDGraph.dll?)");
  g_host->log(ok ? RG_LOG_INFO : RG_LOG_ERROR, line);
  g_at = now;
  static const char kNotIr[] = "this is not LLVM IR\n";
  switch (step) {
    case 0: drop(".dxbc", g_green.bytes, g_green.size); break;
    case 1: drop(".dxbc", g_cs.bytes, g_cs.size); break;
    case 2: drop(".ll", kNotIr, sizeof kNotIr - 1); break;
    case 3: drop(".ll", kBlueIr, std::char_traits<char>::length(kBlueIr)); break;
    default: {
      clean();
      std::snprintf(line, sizeof line, "reload test: DONE - %u pass, %u fail (files removed)", g_pass, g_fail);
      g_host->log(g_fail ? RG_LOG_ERROR : RG_LOG_INFO, line);
    }
  }
  return 0;
}

int32_t compile(const char* src, const char* profile, rg_blob* out, char* err, uint32_t n) {
  rg_hlsl_desc d{};
  d.struct_size = sizeof d;
  d.source = src;
  d.path = "reload_test.hlsl";
  d.entry = "main";
  d.profile = profile;
  return g_host->compile_hlsl(&d, out, err, n);
}

}  // namespace

void register_with(const rg_host* host, bool on) {
  if (!on) return;
  g_host = host;
  if (!hlsl_test::available(host)) {
    host->log(RG_LOG_ERROR, "reload test: the host is older than compile_hlsl - not run");
    return;
  }
  HMODULE rg = GetModuleHandleW(L"REDGraph.dll");
  wchar_t buf[MAX_PATH];
  const DWORD len = rg ? GetModuleFileNameW(rg, buf, MAX_PATH) : 0;
  if (!len) {
    host->log(RG_LOG_ERROR, "reload test: REDGraph.dll not found in the process - not run");
    return;
  }
  // The host's folder: [Dev] ShaderReloadDir in REDGraph.ini, else shader_reload/ next to REDGraph.dll.
  shared::IniFile ini;
  const std::string* custom = ini.read(fs::path(buf).parent_path() / "REDGraph.ini") ? ini.find("Dev", "ShaderReloadDir")
                                                                                      : nullptr;
  g_dir = custom && !custom->empty() ? fs::u8path(*custom) : fs::path(buf).parent_path() / "shader_reload";
  std::error_code ec;
  fs::create_directories(g_dir, ec);
  clean();  // a run that never finished must not override the patch at this start
  char err[512];
  rg_blob magenta{};
  const int32_t a = compile("float4 main(float4 p : SV_Position) : SV_Target { return float4(1, 0.1, 0.8, 1); }",
                            "ps_6_0", &magenta, err, sizeof err);
  const int32_t b = a ? a : compile("float4 main(float4 p : SV_Position) : SV_Target { return float4(0.1, 1, 0.2, 1); }",
                                    "ps_6_0", &g_green, err, sizeof err);
  const int32_t c = b ? b : compile("[numthreads(8, 8, 1)] void main() {}", "cs_6_0", &g_cs, err, sizeof err);
  if (a || b || c) {
    char line[700];
    std::snprintf(line, sizeof line, "reload test: compile_hlsl failed (%d %d %d: %s) - not run", a, b, c, err);
    host->log(RG_LOG_ERROR, line);
    return;
  }
  rg_patch_desc d{};
  d.name = kStem;
  d.stage = RG_STAGE_PS;
  const auto h = hash_literal::hash_from_hex("b932ca8eb30728a8635f07647830cfd9");
  for (int k = 0; k < 16; ++k) d.hash[k] = h[k];
  d.variants = &magenta;
  d.variant_count = 1;
  d.initial_variant = 0;
  d.priority = 100;
  g_patch = host->patch_register(&d);
  const rg_handle wrap = host->node_wrap("CRenderNode_RenderBackground", &on_background, nullptr, nullptr);
  const bool ok = g_patch > 0 && wrap > 0;
  char line[200];
  std::snprintf(line, sizeof line, "reload test: %s - patch %d, wrap %d; files in %s", ok ? "registered" :
                "registration FAILED", g_patch, wrap, g_dir.string().c_str());
  host->log(ok ? RG_LOG_INFO : RG_LOG_ERROR, line);
}

}  // namespace selftest::reload_test
