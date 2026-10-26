// REDGraph - the render host for Cyberpunk 2077 (docs/internals.md, docs/internals.md): the CORE (core/)
// implements the client API (api/rg_host.h) - PSO patches, leases of the engine's descriptor tables, RT raygen
// variants, the render graph's nodes -, its own settings (settings.cpp, REDGraph.ini) and its CET window's native
// functions (ui.cpp: the graph debug view). Clients are other RED4ext plugins.

#include <RED4ext/Common.hpp>
#include <RED4ext/Api/ApiVersion.hpp>
#include <RED4ext/Api/v1/EMainReason.hpp>
#include <RED4ext/Api/v1/PluginHandle.hpp>
#include <RED4ext/Api/v1/PluginInfo.hpp>
#include <RED4ext/Api/v1/Runtime.hpp>
#include <RED4ext/Api/v1/Sdk.hpp>
#include <RED4ext/Api/v1/SemVer.hpp>
#include <RED4ext/Api/v1/Version.hpp>

#include "rg_host.h"
#include "core/ab.hpp"
#include "core/engine_blocks.hpp"
#include "core/crash_log.hpp"
#include "core/graph_debug.hpp"
#include "core/host.hpp"
#include "settings.hpp"
#include "ui.hpp"

#ifndef RG_VERSION_STRING
#define RG_VERSION_STRING "dev"
#endif
#ifndef RG_VERSION_MAJOR  // set from the project version by CMakeLists.txt
#define RG_VERSION_MAJOR 0
#define RG_VERSION_MINOR 0
#define RG_VERSION_PATCH 0
#endif

namespace {
// [Dev] ReadbackDir, else <ShaderReloadDir>/readback, else readback/ next to this DLL.
std::string readback_dir() {
  std::string d = rg_settings::text("Dev.ReadbackDir");
  if (!d.empty()) return d;
  d = rg_settings::text("Dev.ShaderReloadDir");
  if (!d.empty()) return d + "\\readback";
  const std::string ini = rg_settings::path();  // REDGraph.ini sits next to the DLL
  return ini.substr(0, ini.find_last_of("\\/") + 1) + "readback";
}
}  // namespace

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::v1::PluginHandle handle, RED4ext::v1::EMainReason reason,
                                        const RED4ext::v1::Sdk* sdk) {
  if (reason == RED4ext::v1::EMainReason::Load) {
    sdk->logger->InfoF(handle, "REDGraph %s loaded (host API %u)", RG_VERSION_STRING, RG_HOST_API_VERSION);
    engine_blocks::attach(handle, sdk);
    core::open(handle, sdk);  // rg_host_get answers from here on
    rg_settings::load();
    float dred = 0.0f, graph = 0.0f, debug = 0.0f;
    rg_settings::get("Diagnostics.Dred", &dred);  // DRED must be on before the engine's D3D12CreateDevice
    crash_log::open(handle, sdk, dred > 0.5f);
    rg_settings::get("Diagnostics.RenderGraphDump", &graph);
    rg_settings::get("Diagnostics.RenderGraphDebug", &debug);
    if (graph > 0.5f || debug > 0.5f) core::graph_probe::enable();  // before attach: it widens pso's hooks
    if (debug > 0.5f) core::graph_debug::enable();                   // the CET window's graph debug module
    float blas = 0.0f;
    rg_settings::get("Diagnostics.BlasCensus", &blas);
    if (blas > 0.5f) core::blas_census::enable();  // draw subscriptions: before the registration window closes
    float reload = 0.0f;
    rg_settings::get("Dev.ShaderHotReload", &reload);
    if (reload > 0.5f) core::shader_reload::enable(rg_settings::text("Dev.ShaderReloadDir"));  // before attach
    float ab = 0.0f;
    rg_settings::get("Diagnostics.ABProfile", &ab);
    if (ab > 0.5f) core::ab::enable();  // before attach: every node class wrapped, jobs carry their node
    float readback = 0.0f;
    rg_settings::get("Dev.Readback", &readback);
    if (readback > 0.5f) core::readback::enable(readback_dir());  // before attach: it wraps every node class
    core::drain_clients();  // the clients that loaded before us register now
    core::arm();            // registration stays open for the clients loading after us, until GpuApi::InitDevice
    rg_ui::attach();
  } else if (reason == RED4ext::v1::EMainReason::Unload) {
    rg_ui::log_status("game closing");
    crash_log::close();
    core::detach();
    engine_blocks::detach(handle, sdk);
  }
  return true;
}

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::v1::PluginInfo* info) {
  info->name = L"REDGraph";
  info->author = L"REDGraph contributors";
  info->version = RED4EXT_V1_SEMVER(RG_VERSION_MAJOR, RG_VERSION_MINOR, RG_VERSION_PATCH);
  // NOT runtime-independent: we read SDK struct layouts (GpuApi::SDeviceData) pinned to game 2.31.
  info->runtime = RED4EXT_V1_RUNTIME_VERSION_LATEST;
  info->sdk = RED4EXT_V1_SDK_VERSION_CURRENT;
}

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports() { return RED4EXT_API_VERSION_1; }
