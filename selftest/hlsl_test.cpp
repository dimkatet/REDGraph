// HlslTest ([Tests] HlslTest = true, rg_host.h compile_hlsl): the host's HLSL compiler, at registration.
//   * a trivial cs_6_0 from text: 0, a signed DXBC container;
//   * a broken ps_6_0: RG_E_COMPILE with the compiler's error text;
//   * lib_6_3: RG_E_UNSUPPORTED (RT libraries are not offered);
//   * no dxcompiler.dll next to REDGraph.dll: every call RG_E_UNAVAILABLE - logged as such, not a failure.
// With LeaseTexTest also on, that test's companion is compiled at runtime from lease_tex.hlsl (deployed next to
// REDGraphSelfTest.dll by build_and_deploy.ps1 -SelfTest) instead of the checked-in blob, and its content checks are
// the proof that a compiled shader RUNS. Nothing on screen.

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "selftest.hpp"

namespace selftest::hlsl_test {
namespace {

int32_t compile(const rg_host* host, const char* src, const char* profile, rg_blob* out, char* err, uint32_t n) {
  rg_hlsl_desc d{};
  d.struct_size = sizeof d;
  d.source = src;
  d.path = "hlsl_test.hlsl";
  d.entry = "main";
  d.profile = profile;
  return host->compile_hlsl(&d, out, err, n);
}

}  // namespace

bool available(const rg_host* host) {
  return host->struct_size >= offsetof(rg_host, compile_hlsl) + sizeof(void*);
}

void register_with(const rg_host* host, bool on) {
  if (!on) return;
  if (!available(host)) {
    host->log(RG_LOG_ERROR, "hlsl test: the host is older than compile_hlsl - not run");
    return;
  }
  char err[512], line[900];
  rg_blob good{};
  const int32_t rg = compile(host, "[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {}", "cs_6_0",
                             &good, err, sizeof err);
  if (rg == RG_E_UNAVAILABLE) {
    std::snprintf(line, sizeof line, "hlsl test: compiler UNAVAILABLE (%s) - nothing else checked", err);
    host->log(RG_LOG_WARN, line);
    return;
  }
  bool signed_ = false;
  const auto* b = static_cast<const uint8_t*>(good.bytes);
  if (rg == 0 && b && good.size > 20)
    for (int i = 4; i < 20; ++i) signed_ |= b[i] != 0;
  const bool ok_good = rg == 0 && b && std::memcmp(b, "DXBC", 4) == 0 && signed_;
  rg_blob bad{};
  char bad_err[512];
  const int32_t rb = compile(host, "float4 main() : SV_Target { return not_declared; }", "ps_6_0", &bad, bad_err,
                             sizeof bad_err);
  const bool ok_bad = rb == RG_E_COMPILE && !bad.bytes && std::strstr(bad_err, "not_declared") != nullptr;
  rg_blob lib{};
  const int32_t rl = compile(host, "[shader(\"raygeneration\")] void main() {}", "lib_6_3", &lib, err, sizeof err);
  const bool ok_lib = rl == RG_E_UNSUPPORTED;
  std::snprintf(line, sizeof line,
                "hlsl test: %s - cs_6_0 -> %d, %zu bytes, %s; broken ps_6_0 -> %d (expected %d), error \"%.160s\"; "
                "lib_6_3 -> %d (expected %d)",
                ok_good && ok_bad && ok_lib ? "PASS" : "FAIL", rg, good.size, signed_ ? "signed" : "NOT signed", rb,
                RG_E_COMPILE, bad_err, rl, RG_E_UNSUPPORTED);
  host->log(ok_good && ok_bad && ok_lib ? RG_LOG_INFO : RG_LOG_ERROR, line);
}

}  // namespace selftest::hlsl_test
