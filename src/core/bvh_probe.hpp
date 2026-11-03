#pragma once

// [Diagnostics] BvhProbe (2026-11-01): a proxy's TLAS instance from both sides - what the instance descs carry and who
// builds / traces the TLAS made from them. For a client that swaps its own BLAS into a proxy's instance
// (mesh_proxy_bvh_swap) or watches its desc (mesh_proxy_bvh_desc), three counts per render frame, side by side:
//   * the desc WRITES of those proxies (mesh_proxy.cpp's d_prepare): 1 a frame = one instance-desc array holds the
//     proxy; more = the engine writes its desc into several arrays, and whether every one got the client's BLAS;
//   * the TLAS BUILDS (rt_lib.cpp's d_tlas, GpuApi::BuildTopLevelAccelerationStructure): how many a frame, from which
//     instance buffer, with how many instances, and from which collector (the caller's RVA) - a TLAS built from a
//     buffer the desc writer never touched would trace the engine's own descs;
//   * the DispatchRays (rt_lib.cpp's d_rays): the state objects and their engine call sites, so the pass that sees
//     the engine's geometry can be told from one that sees the client's.
// The first use (2026-11-01, a client's swapped proxy in a reflection) found ONE desc write and ONE scene TLAS a frame.
// Read-only. Off unless [Diagnostics] BvhProbe = true at Load (rt_lib attaches its TLAS hook for it; DispatchRays is
// attached anyway). The counters are aggregated per render frame (kRenderFrame) and one line is logged per frame - while
// the view is stable the line repeats and is dropped, so what shows up in the log is a change.

#include <cstdint>

namespace core::bvh_probe {

// [Diagnostics] BvhProbe at Load. `on()` is the cheap gate every hook checks first.
void enable();
bool on();

// From mesh_proxy.cpp's desc-writer detour, for a proxy a client gave a BLAS to (or asked to watch): one write of one
// instance desc, `desc` as it stands now (64 B, after the client's swap), `swapped` = the client's BLAS is in it.
void on_desc(uint32_t slot, const void* proxy, const uint8_t desc[64], bool swapped);

// From rt_lib.cpp's TLAS-build detour: one build, `count` instances from the buffer whose GPU VA is `instances`,
// `site` = the caller's RVA (kBuildTlasRet is the light-volume collector's).
void on_tlas(uint32_t count, uint64_t instances, uint32_t site);

// From rt_lib.cpp's DispatchRays detour: one dispatch, its state object and its engine call site (`site` as
// graph_probe::on_dispatch_rays computes it).
void on_rays(uint32_t so, uint32_t site);

}  // namespace core::bvh_probe
