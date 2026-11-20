#pragma once

// The host's CLIENT-VISIBLE capacity limits - the ones that answer a client RG_E_FULL (or refuse a registration) - in one
// place. Each is a compile-time default that can be overridden: `cmake -DRG_MAX_VIEWS=256 ...` (CMakeLists.txt lists them
// as cache variables) or `/DRG_MAX_VIEWS=256`. A bigger number costs memory (fixed arrays), not frame time: the hot paths
// walk only what is registered. The ceilings that come from a data type (a bit mask, an int8_t index) are static_asserted
// below, so an override past them fails the build with the reason instead of corrupting memory.
//
// The defaults are what the host was built and tested with; a number raised past its default is not exercised by the
// self-tests (they stay inside the defaults), so raise one on purpose and watch the log.
//
// Whatever is NOT here (internal pools such as the PSO slot table, readback request ring, RT mark slots...) stays next to
// the code that owns it. Raising a limit: bump the default HERE, add a line to the changelog, and sdk/README.md "Limits".
// The values in force are logged once at startup ("limits: ..."), so a bug report names them.

#include <cstdint>

#ifndef RG_MAX_VIEWS  // SRV/UAV views of client buffers / textures / flow textures (rg_host view_create*)
#define RG_MAX_VIEWS 128  // 16 until 2026-11-14 (requests for 64-128 buffers)
#endif
#ifndef RG_MAX_VIEW_RING  // descriptors per view (a ring of frames in flight)
#define RG_MAX_VIEW_RING 8
#endif
#ifndef RG_MAX_CBUFFERS  // client cbuffers (each: a ring of up to RG_MAX_CB_RING copies, 4096 B at most)
#define RG_MAX_CBUFFERS 32  // 8 until 2026-11-14 (a ring of 16 x 4096 B is 64 KB: 32 of them are 2 MB at most)
#endif
#ifndef RG_MAX_CB_RING
#define RG_MAX_CB_RING 16
#endif
#ifndef RG_MAX_CLIENTS  // client plugins
#define RG_MAX_CLIENTS 32
#endif
#ifndef RG_MAX_PATCHES  // shader patches, every client together
#define RG_MAX_PATCHES 256
#endif
#ifndef RG_MAX_LEASES  // leases (space0 register claims), every client together
#define RG_MAX_LEASES 64
#endif
#ifndef RG_MAX_LEASE_OWNERS  // patches / companions that may share ONE lease
#define RG_MAX_LEASE_OWNERS 32
#endif
#ifndef RG_MAX_LEASE_REFS  // engine shaders ONE lease can learn (a ShaderRef each)
#define RG_MAX_LEASE_REFS 128
#endif
#ifndef RG_MAX_DRAW_COMPANIONS  // draw companions, every client together
#define RG_MAX_DRAW_COMPANIONS 32
#endif
#ifndef RG_MAX_DRAW_COMPANIONS_PER_PSO  // draw companions ONE engine PSO carries
#define RG_MAX_DRAW_COMPANIONS_PER_PSO 8
#endif

#ifndef RG_MAX_HANDLES  // handles per registration space the owner table tracks (clients.cpp): past it a handle has no owner
#define RG_MAX_HANDLES 1024
#endif
#ifndef RG_MAX_RT_LIBS  // RT shader libraries (rtlib_register), every client together
#define RG_MAX_RT_LIBS 64
#endif
#ifndef RG_MAX_RT_HIT_GROUPS  // RT hit groups, every client together
#define RG_MAX_RT_HIT_GROUPS 8
#endif
#ifndef RG_MAX_RT_VARIANTS  // raygen variants, every client together
#define RG_MAX_RT_VARIANTS 128  // 64 was full in CRT 2026-10-19
#endif
#ifndef RG_MAX_MESH_SUBS  // mesh-proxy subscriptions, every client together (per-chunk bit tables grow with it)
#define RG_MAX_MESH_SUBS 16
#endif
#ifndef RG_MAX_NODES  // render-graph nodes of clients
#define RG_MAX_NODES 8
#endif
#ifndef RG_MAX_NODE_USES  // node_use / node_alloc / node_wrap_use entries on ONE node or wrap
#define RG_MAX_NODE_USES 8
#endif
#ifndef RG_MAX_WRAPS  // node wraps + removals, every client together
#define RG_MAX_WRAPS 32
#endif
#ifndef RG_MAX_PASSES  // compute passes of clients
#define RG_MAX_PASSES 8
#endif
#ifndef RG_MAX_DEVICE_FNS  // device_ready callbacks, every client together
#define RG_MAX_DEVICE_FNS 32  // 8 until 2026-11-21 (3 heavy mods filled it; later clients got RG_E_FULL)
#endif

namespace core {

constexpr uint32_t kMaxViews = RG_MAX_VIEWS;
constexpr uint32_t kMaxViewRing = RG_MAX_VIEW_RING;
constexpr uint32_t kMaxCb = RG_MAX_CBUFFERS;
constexpr uint32_t kMaxRing = RG_MAX_CB_RING;
constexpr uint32_t kMaxClients = RG_MAX_CLIENTS;
constexpr uint32_t kMaxPatchCount = RG_MAX_PATCHES;
constexpr uint32_t kMaxLeases = RG_MAX_LEASES;
constexpr uint32_t kMaxLeaseOwners = RG_MAX_LEASE_OWNERS;
constexpr uint32_t kMaxRefs = RG_MAX_LEASE_REFS;
constexpr uint32_t kMaxDrawCompsLimit = RG_MAX_DRAW_COMPANIONS;
constexpr uint32_t kMaxDrawCompsPerPsoLimit = RG_MAX_DRAW_COMPANIONS_PER_PSO;

constexpr uint32_t kMaxHandles = RG_MAX_HANDLES;
constexpr uint32_t kMaxRtLibs = RG_MAX_RT_LIBS;
constexpr uint32_t kMaxRtHitGroups = RG_MAX_RT_HIT_GROUPS;
constexpr uint32_t kMaxRtVariants = RG_MAX_RT_VARIANTS;
constexpr uint32_t kMaxMeshSubs = RG_MAX_MESH_SUBS;
constexpr uint32_t kMaxNodesLimit = RG_MAX_NODES;
constexpr uint32_t kMaxNodeUses = RG_MAX_NODE_USES;
constexpr uint32_t kMaxWrapsLimit = RG_MAX_WRAPS;
constexpr uint32_t kMaxPassesLimit = RG_MAX_PASSES;
constexpr uint32_t kMaxDeviceFns = RG_MAX_DEVICE_FNS;

static_assert(kMaxViews >= 1 && kMaxViewRing >= 1 && kMaxCb >= 1 && kMaxRing >= 1 && kMaxClients >= 1 &&
                  kMaxPatchCount >= 1 && kMaxLeases >= 1 && kMaxLeaseOwners >= 1 && kMaxRefs >= 1 &&
                  kMaxDrawCompsLimit >= 1 && kMaxDrawCompsPerPsoLimit >= 1 && kMaxHandles >= 1 && kMaxRtLibs >= 1 &&
                  kMaxRtHitGroups >= 1 && kMaxRtVariants >= 1 && kMaxMeshSubs >= 1 && kMaxNodesLimit >= 1 &&
                  kMaxNodeUses >= 1 && kMaxWrapsLimit >= 1 && kMaxPassesLimit >= 1 && kMaxDeviceFns >= 1,
              "every RG_MAX_* limit is at least 1");
static_assert(kMaxLeases <= 127, "RG_MAX_LEASES: g_lease_of_patch / g_lease_of_comp hold a lease index in an int8_t");
static_assert(kMaxClients <= 255, "RG_MAX_CLIENTS: clients.cpp's per-handle owner table holds a client index in a uint8_t");
static_assert(kMaxDrawCompsLimit <= kMaxHandles, "RG_MAX_DRAW_COMPANIONS: a handle is an index in clients.cpp's owner table (RG_MAX_HANDLES)");
static_assert(kMaxPatchCount <= kMaxHandles, "RG_MAX_PATCHES: a handle is an index in clients.cpp's owner table (RG_MAX_HANDLES)");
static_assert(kMaxDrawCompsPerPsoLimit <= kMaxDrawCompsLimit, "RG_MAX_DRAW_COMPANIONS_PER_PSO exceeds RG_MAX_DRAW_COMPANIONS");

static_assert(kMaxRefs >= kMaxLeaseOwners, "RG_MAX_LEASE_REFS must outgrow RG_MAX_LEASE_OWNERS (one ShaderRef per owner shader)");

static_assert(kMaxRtLibs <= kMaxHandles && kMaxMeshSubs <= kMaxHandles && kMaxNodesLimit <= kMaxHandles &&
                  kMaxWrapsLimit <= kMaxHandles && kMaxPassesLimit <= kMaxHandles && kMaxViews <= kMaxHandles &&
                  kMaxCb <= kMaxHandles,
              "RG_MAX_HANDLES: a handle is an index in clients.cpp's owner table - it must hold every table's size");
static_assert(kMaxHandles <= 0x7fffffff, "RG_MAX_HANDLES must fit an rg_handle");

// Logs the values in force, once (plugin load).
void log_limits();

}  // namespace core
