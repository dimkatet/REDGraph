#pragma once

// The render graph DEBUG module ([Diagnostics] RenderGraphDebug, docs/internals.md "Graph debug module"), for
// plugin.cpp and the CET UI's native functions (ui_api.cpp). Every node of the executing graph with the shaders it
// binds; any node INSTANCE removed live (M9's record / replay); any shader set skipped - everywhere or inside one node -
// by the engine's own path (the bind funnel returns NULL: the draw / dispatch is skipped as for a PSO still compiling).
// Indices are valid for one generation of the report; a call with another generation is refused ("stale").

#include <cstdint>
#include <string>

namespace core::graph_debug {

void enable();  // at Load, before core::attach - with graph_probe::enable (the census) and render_nodes' class wraps
bool on();

// The report, one record per line, fields tab-separated (cet/REDGraph/init.lua parses it):
//   H gen sig mode wrapped_classes census removed_wanted removed_elsewhere rules
//   N index parent depth flags removed name key removed_runs ran_runs refused owner
//       flags: g group, j job-builder (M9 refuses), s structural, o a client's node (node_register - by the host's
//       vtable, owner = its client's module, "-" for an engine node), w removable (its class is wrapped)
//       removed: 0, 1 = by this window, 2 = by a client's node_remove (owner = that client; runs are its class's)
//   S row node pipe vs ps cs binds psos here all skipped_here skipped_all rt
//       node -1 = outside any node (jobs); pipe 0 compute, 1 graphics, 2 DispatchRays (rt set, no skip)
//   R rule vs ps cs node_key skipped
std::string report();
// "" = done, else why not. A group applies to all its members (the ones that can be removed).
std::string set_node(int32_t gen, int32_t index, bool on, bool force);
// where 1 = in the row's node only, 2 = everywhere.
std::string set_shader(int32_t gen, int32_t row, int32_t where, bool on);
std::string rule_off(int32_t rule);
void census();  // a new census window (300 frames) on the executing graph
void reset();   // every removal and every skip off

}  // namespace core::graph_debug
