#pragma once

// Engine constant blocks (docs/internals.md "Engine constant blocks"): a CATALOG of the constant data the engine
// writes, each caught at its write - a call of GpuApi::UpdateBuffer(BufferRef, size, data) or
// GpuApi::SetComputeShaderConstsRaw(size, data) from one known engine function (matched by the return address). An
// entry is named after that writer, the engine's own name. The host keeps the latest copy of every entry a client asked
// for (rg_host::engine_block_find / cbuffer_feed) and feeds the cbuffers built from it; a hook goes on only when one of
// its entries is wanted. The catalog grows by appending (an entry's index is its id, forever).
//
// The first four entries are rg_engine_block of API v1 and are always captured - clients built before the catalog
// read them without asking. Hooks attach at BaseInitialization (after every other plugin's Load - see attach() for
// why), after a call-site check of each wanted entry. Not attached: flag file `no_hooks` next to the DLL.
//
// Addresses are 2.31 RVAs (cp2077-symbols); the plugin is pinned to that runtime.

#include <cstdint>

#include <RED4ext/Api/v1/PluginHandle.hpp>
#include <RED4ext/Api/v1/Sdk.hpp>

namespace engine_blocks {

void attach(RED4ext::v1::PluginHandle handle, const RED4ext::v1::Sdk* sdk);
void detach(RED4ext::v1::PluginHandle handle, const RED4ext::v1::Sdk* sdk);

uint32_t count();  // catalog entries
// A client wants entry `id` (its hook goes on). false = no such entry.
bool want(uint32_t id);
// The entry named `name` (the engine writer's name, exactly as the catalog has it), or -1.
int32_t find(const char* name);
const char* name(uint32_t id);

// Returns the block's write count (0 = never seen); copies up to max_bytes.
uint64_t latest(uint32_t id, void* out, uint32_t max_bytes, uint32_t* size);

}  // namespace engine_blocks
