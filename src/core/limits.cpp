#include "core/internal.hpp"

namespace core {

void log_limits() {
  g_sdk->logger->InfoF(g_handle,
                       "limits: views %u (ring %u), cbuffers %u (ring %u), clients %u, patches %u, leases %u "
                       "(owners %u, refs %u), draw companions %u (%u per PSO), handles %u; RT libs %u, hit groups %u, variants %u; "
                       "mesh subs %u, nodes %u (uses %u), wraps %u, passes %u, device_ready fns %u - override with -DRG_MAX_*",
                       kMaxViews, kMaxViewRing, kMaxCb, kMaxRing, kMaxClients, kMaxPatchCount, kMaxLeases,
                       kMaxLeaseOwners, kMaxRefs, kMaxDrawCompsLimit, kMaxDrawCompsPerPsoLimit, kMaxHandles, kMaxRtLibs,
                       kMaxRtHitGroups, kMaxRtVariants, kMaxMeshSubs, kMaxNodesLimit, kMaxNodeUses, kMaxWrapsLimit,
                       kMaxPassesLimit, kMaxDeviceFns);
}

}  // namespace core
