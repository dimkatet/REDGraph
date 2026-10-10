// REDGraph example client - the smallest working client of the render host (REDGraph.dll), as a RED4ext plugin of its
// own. It sees only rg_host.h. In its Load it hands its init to rg_host_when_ready(): RED4ext loads plugins in
// alphabetical order, and this works whether the host loads before or after it. init registers while the host's
// window is open (until the engine's GpuApi::InitDevice) - here one wrap of the volumetric fog node, whose `before`
// logs every 600 runs into REDGraph's log. Nothing changes on screen.
//
// Expect in red4ext/logs/redgraph-*.log:
//   example client: host API 1 (table N B) - node_wrap(CRenderNode_VolumetricFog) = 1
//   host: registration CLOSED - clients: ... REDGraphExampleClient (API 1): 1 registration(s), 0 refused
//   example client: VolumetricFog before #600 (host wrap stats: executes 600)

#include <windows.h>

#include <RED4ext/Common.hpp>
#include <RED4ext/Api/ApiVersion.hpp>
#include <RED4ext/Api/v1/EMainReason.hpp>
#include <RED4ext/Api/v1/PluginHandle.hpp>
#include <RED4ext/Api/v1/PluginInfo.hpp>
#include <RED4ext/Api/v1/Runtime.hpp>
#include <RED4ext/Api/v1/Sdk.hpp>
#include <RED4ext/Api/v1/SemVer.hpp>
#include <RED4ext/Api/v1/Version.hpp>

#include <atomic>
#include <cstdio>

#include "rg_host.h"

namespace {

const rg_host* g_host = nullptr;
rg_handle g_wrap = 0;
std::atomic<uint32_t> g_runs{0};

// Runs on the engine's recording thread, before every real run of the node; returns RG_PASS_TOUCHED_STATE only if it
// changed the root signature / PSO / heaps on the command list.
uint32_t on_fog(void*, const rg_node_info*) {
  const uint32_t n = g_runs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n % 600 == 0) {
    rg_wrap_stats st{};
    g_host->node_wrap_stats(g_wrap, &st);
    char line[200];
    std::snprintf(line, sizeof line, "example client: VolumetricFog before #%u (host wrap stats: executes %llu)", n,
                  static_cast<unsigned long long>(st.executes));
    g_host->log(RG_LOG_INFO, line);
  }
  return 0;
}

// Called once the host is there: now (it loaded first) or in the host's own Load.
void init(const rg_host* host, void*) {
  g_host = host;
  g_wrap = host->node_wrap("CRenderNode_VolumetricFog", &on_fog, nullptr, nullptr);
  char line[200];
  std::snprintf(line, sizeof line, "example client: host API %u (table %u B) - node_wrap(CRenderNode_VolumetricFog) = %d",
                host->version, host->struct_size, g_wrap);
  host->log(g_wrap > 0 ? RG_LOG_INFO : RG_LOG_ERROR, line);  // a negative handle is an RG_E_* code
}

}  // namespace

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::v1::PluginHandle handle, RED4ext::v1::EMainReason reason,
                                        const RED4ext::v1::Sdk* sdk) {
  if (reason != RED4ext::v1::EMainReason::Load) return true;
  if (rg_host_when_ready(&init, nullptr) < 0)
    sdk->logger->Error(handle, "REDGraph is not installed, or older than this client - nothing registered");
  return true;
}

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::v1::PluginInfo* info) {
  info->name = L"REDGraph Example Client";
  info->author = L"REDGraph";
  info->version = RED4EXT_V1_SEMVER(0, 1, 0);
  info->runtime = RED4EXT_V1_RUNTIME_VERSION_LATEST;
  info->sdk = RED4EXT_V1_SDK_VERSION_CURRENT;
}

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports() { return RED4EXT_API_VERSION_1; }
