// REDGraphSelfTest - the host's self-tests as a client plugin (selftest.hpp). Which tests run: REDGraphSelfTest.ini
// next to the DLL, [Tests] FlowProbe = true/false (needs REDGraph's [Diagnostics] RenderGraphDump for its census
// files), RtVariantTest = 0/1/2, RtSwapTest = 0/1, PatchScopeTest = 0/1/2. The tests write into REDGraph's log (host->log).

#include <RED4ext/Common.hpp>
#include <RED4ext/Api/ApiVersion.hpp>
#include <RED4ext/Api/v1/EMainReason.hpp>
#include <RED4ext/Api/v1/PluginHandle.hpp>
#include <RED4ext/Api/v1/PluginInfo.hpp>
#include <RED4ext/Api/v1/Runtime.hpp>
#include <RED4ext/Api/v1/Sdk.hpp>
#include <RED4ext/Api/v1/SemVer.hpp>
#include <RED4ext/Api/v1/Version.hpp>

#include <cstdio>
#include <cstdlib>

#include "ini_file.hpp"
#include "selftest.hpp"

namespace {

struct Wanted {
  bool flow = false;
  int rtv = 0, swap = 0, scope = 0;
} g_want;

void on_host(const rg_host* host, void*) {
  char line[200];
  std::snprintf(line, sizeof line,
                "selftest: REDGraph ready - FlowProbe %s, RtVariantTest %d, RtSwapTest %d, PatchScopeTest %d",
                g_want.flow ? "on" : "off", g_want.rtv, g_want.swap, g_want.scope);
  host->log(RG_LOG_INFO, line);
  if (g_want.flow) selftest::flow_probe::register_with(host);
  selftest::rt_variant_test::register_with(host, g_want.rtv);
  selftest::rt_swap_test::register_with(host, g_want.swap);
  selftest::scope_test::register_with(host, g_want.scope);
}

}  // namespace

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::v1::PluginHandle handle, RED4ext::v1::EMainReason reason,
                                        const RED4ext::v1::Sdk* sdk) {
  if (reason != RED4ext::v1::EMainReason::Load) return true;
  shared::IniFile ini;
  const auto path = shared::IniFile::module_dir(&g_want) / "REDGraphSelfTest.ini";
  if (ini.read(path)) {
    if (const std::string* s = ini.find("Tests", "FlowProbe")) g_want.flow = *s == "true" || *s == "1";
    if (const std::string* s = ini.find("Tests", "RtVariantTest")) g_want.rtv = std::atoi(s->c_str());
    if (const std::string* s = ini.find("Tests", "RtSwapTest")) g_want.swap = std::atoi(s->c_str());
    if (const std::string* s = ini.find("Tests", "PatchScopeTest")) g_want.scope = std::atoi(s->c_str());
  }
  const int r = rg_host_when_ready(&on_host, nullptr);
  sdk->logger->InfoF(handle, "REDGraphSelfTest: %s, host %s", path.string().c_str(),
                     r > 0 ? "ready" : (r == 0 ? "waited for" : "NOT FOUND"));
  return true;
}

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::v1::PluginInfo* info) {
  info->name = L"REDGraph Self-Test";
  info->author = L"REDGraph contributors";
  info->version = RED4EXT_V1_SEMVER(0, 1, 0);
  info->runtime = RED4EXT_V1_RUNTIME_VERSION_LATEST;
  info->sdk = RED4EXT_V1_SDK_VERSION_CURRENT;
}

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports() { return RED4EXT_API_VERSION_1; }
