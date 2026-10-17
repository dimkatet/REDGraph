#pragma once

// The core's lifecycle, for plugin.cpp only. Clients see api/rg_host.h.
//   open(): at Load, before any client registers (the log works from here on).
//   arm(): at the end of our Load - hooks GpuApi::InitDevice; at its entry (every RED4ext plugin's Load has run, so a
//     client plugin loaded after us has registered too) attach() runs. If that hook cannot attach, attach() runs now.
//   attach(): closes registration and attaches only the engine hooks the registrations need (docs/internals.md
//     "Core rules"). In a release plugin (RG_NATIVE_HOST off) or with the flag file `no_pso_swap` next to the DLL, nothing native
//     attaches and no shader is claimed.

#include <RED4ext/Api/v1/PluginHandle.hpp>
#include <RED4ext/Api/v1/Sdk.hpp>

namespace core {

void open(RED4ext::v1::PluginHandle handle, const RED4ext::v1::Sdk* sdk);
// After open(): every client that loaded before us and waits in the rendezvous (rg_host_when_ready) registers now.
void drain_clients();
void arm();
void attach();
void detach();

// [Diagnostics] RenderGraphDump: the engine's render graph written to render-graph/ (read only; graph_probe.cpp).
// enable() before attach(): the core then also attaches the probe (with the shader census, which needs pso's hooks).
namespace graph_probe {
void enable();
void attach();
void detach();
}  // namespace graph_probe

// [Diagnostics] BlasCensus: raster draws of a watch list of VS joined to the BLAS geometries (blas_census.cpp).
// enable() before arm(): it registers draw subscriptions.
namespace blas_census {
void enable();
}  // namespace blas_census

}  // namespace core
