#pragma once

// A/B PROFILE (2026-10-26): what a client's switch costs the frame, and where. [Diagnostics] ABProfile at the start;
// Game.RG_AB(switches, block s, pairs) runs it, the result goes into the log as [ab] lines and to RG_ABState.
//
// A run switches the named A/B switches (rg_host::ab_switch - a client registers one per feature) OFF (A) and ON (B)
// in blocks of `block` seconds, interleaved A B, B A, A B ... (pairs alternate their order: a linear drift cancels),
// with `settle` seconds after every switch left out (a PSO swap lands at the next state change, the GPU timestamps
// arrive kRing frames late, caches refill). Every metric is a per-frame mean of its block; a pair gives d = B - A; the
// report is mean(d) +- a 95 % interval over the pairs. A pair with a gap (a frame over 0.25 s: menu, loading) is
// dropped. The switches are set from the runner's own thread, never the render thread; the start values come back
// at the end.
//
// What a block measures - the same instruments in A and B, so their own cost cancels:
//   frame        the interval between two CRenderNodeGraph::ExecuteParallel calls (one per rendered frame);
//   record       the CPU's recording of the frame: its kick-off (ExecuteParallel - it only hands the nodes to jobs and
//                returns in ~25 us) to the end of its last node Execute on any thread;
//   GPU span     first to last timestamp of the frame's engine NODES on the direct queue (gpu_timing) - GPU-bound when
//                it is close to the frame; GPU nodes = their sum;
//   GPU per node / per item of ours (gpu_timing); an item records the node class it was recorded in;
//   CPU per node class: the engine's Execute of the node (record thread, waits included) and the jobs it submitted
//                (render_flow carries the node), the host's and the clients' time inside left out - all threads
//                together, CPU WORK, not the critical path;
//   CPU host     prof.cpp's points; CPU clients: every client callback by its registration name, and what clients
//                time themselves (rg_host::prof_add: their own engine hooks).
// Decomposition: dFrame vs dGPU span vs dRecord says which side bounds; dGPU span = the DispatchRays of state objects
// with our libraries (whole, the engine's raygen work included - a node's bracket does not hold them) + our companions
// / passes + the engine nodes' own work (noisy) + the rest.

#include <intrin.h>

#include <cstdint>
#include <string>

#include "rg_host.h"

namespace core::ab {

void enable();     // [Diagnostics] ABProfile at Load, before attach: jobs carry their node (render_flow), nodes wrapped
bool enabled();

// The frame hook (prof.cpp's / graph_probe's ExecuteParallel detour): before and after the engine's original.
void frame_begin();
void frame_end();

// A node class's CPU (render_nodes' class index, = gpu_timing's kNode handle - 1). Exclusive: nested spans on the
// thread and our own time (host Scopes, client callbacks, prof_add) inside are left out.
bool measuring();  // one relaxed load: a run is between its first and last block
struct Span {
  uint64_t t0 = 0, nested = 0, ours = 0;
  int32_t cls;
  bool job;
  Span(int32_t node_class, bool is_job);
  ~Span();
  Span(const Span&) = delete;
  Span& operator=(const Span&) = delete;
};
// A client callback (what = "pass", "subscription", ...) or a client's own timing (prof_add, what = "own"), keyed by
// the two STABLE pointers.
void client_add(const char* what, const char* name, uint64_t cycles);

// rg_host entries.
rg_handle ab_switch(const char* name, rg_ab_set_fn set, rg_ab_get_fn get, void* user);
uint32_t prof_active();
void prof_add(const char* name, uint64_t tsc_cycles);

// CET: "" = started, else why not; state = one line + the last report.
std::string run(const std::string& spec, double block_s, double settle_s, uint32_t pairs);
std::string state();
std::string switches();  // the registered switches, one per line: name \t client \t on now

}  // namespace core::ab
