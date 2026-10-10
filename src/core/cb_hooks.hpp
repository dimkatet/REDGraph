#pragma once

// GpuApi constant-buffer hooks - what ships (docs/internals.md "Engine constant blocks"). Two hooks, attached
// in every build after a call-site check of the exe, at BaseInitialization (after every other plugin's Load - see
// attach() for why):
//   GpuApi::UpdateBuffer(BufferRef, size, data)       -> cb12, from CRenderInterface::CalculateSharedConstants
//   GpuApi::SetComputeShaderConstsRaw(size, data)     -> the fog CS b6, the cloud bake's b6, the world clouds' b6
// Each kept as "the latest" for the host API's rg_engine_block (engine_raw_consts) and fed to the cbuffers built from
// it. Not attached: flag file `no_hooks` next to the plugin DLL (the A/B baseline), or an exe whose call sites are not
// 2.31's.
//
// Addresses are 2.31 RVAs (cp2077-symbols); the plugin is pinned to that runtime.

#include <cstdint>

#include <RED4ext/Api/v1/PluginHandle.hpp>
#include <RED4ext/Api/v1/Sdk.hpp>


namespace cb_hooks {

void attach(RED4ext::v1::PluginHandle handle, const RED4ext::v1::Sdk* sdk);
void detach(RED4ext::v1::PluginHandle handle, const RED4ext::v1::Sdk* sdk);

// which = rg_engine_block (api/rg_host.h). Returns the block's write count (0 = never seen); copies up to max_bytes.
uint64_t engine_raw_consts(uint32_t which, void* out, uint32_t max_bytes, uint32_t* size);

}  // namespace cb_hooks
