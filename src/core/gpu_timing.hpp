#pragma once

// GPU time of the host's own work items and of the engine's render-graph NODES (docs/plan-gpu-timing-and-hlsl.md
// section 1; the timestamp ring is ported from spuddeh/cp2077-native-shaders `Timing.cpp`, with the author's
// permission). [Diagnostics] GpuTiming, live, off by default: nothing is created and every bracket is one relaxed load
// until it is switched on.
//
// A bracket is two EndQuery(TIMESTAMP) on the engine's own command list around one item and a ResolveQueryData of that
// pair into a READBACK buffer RIGHT AFTER the end, on the same list (the host has no list that is last in a frame):
//   our work   - a companion dispatch, a companion draw, a client pass; with a PIX-style debug event
//                "REDGraph: <client>/<name>" (BeginEvent metadata 0 = a wide string) for Nsight / RenderDoc;
//   a node     - the engine's Execute of one render-graph node (render_nodes' wrap of every class, on when GpuTiming or
//                RenderGraphDebug is on at the START), keyed by its class: what an engine pass costs, and with a
//                client's patch switched on / off, what the patch adds. Groups and job-builder nodes are not timed
//                (their work is on other lists);
//   rays       - a DispatchRays of a state object holding a client library (the raygen's variant, else the library).
// Pairs come from an atomic counter in a ring of kRing frame slots (lists are recorded on many threads); a slot is
// read kRing frames later at the frame hook - never waited on - and cleared, so a pair the GPU has not resolved yet
// reads 0 and is counted LATE instead of returning an older frame's numbers. A reading counts when begin != 0,
// end >= begin and < 50 ms; per item it is the SUM of the frame's pairs, smoothed new = old + (x - old) * 0.05.
// The first bracket of a frame also records an empty pair: the cost of timing itself.
//
// Includes whatever barrier / wait the engine put before the work on that list, and on a compute-queue list ticks of
// that queue read with the direct queue's frequency. Not an absolute truth (memory: GPU timer per-dispatch unreliable
// across sessions) - the number that is always there; ablation stays the arbiter. Timing per DRAW / DISPATCH was tried
// and dropped (2026-10-11): ~2000 pairs a frame serialised the GPU, cost most of the frame rate and inflated the number.

#include <cstdint>
#include <string>

struct ID3D12GraphicsCommandList;

namespace core::gpu_timing {

// An item is a kind + the handle that names it (kNode: render_nodes' class index + 1).
enum Kind : uint8_t { kCompanion, kDrawCompanion, kPass, kNode, kRaysLib, kRaysVariant, kKinds };

void set_enabled(bool on);  // live: the first frame after `on` creates the heap + buffer (once)
bool enabled();
bool active();              // timing now (the per-call check)
void set_node_timing(bool on);  // every node class is wrapped (for the report)
void set_node_name(int32_t handle, const char* name);  // a node class's name (stable for the process)
void frame_tick();          // the frame hook (prof.cpp): reads the slot it is about to reuse

// 0 = not timed (off, not ready, the frame's pairs used up). Pair it with end() on the same list. `event`: a debug
// event around it - our own work; the engine's needs none from us.
uint32_t begin(ID3D12GraphicsCommandList* cl, Kind kind, int32_t handle, bool event = true);
void end(ID3D12GraphicsCommandList* cl, uint32_t token, bool event = true);
void abandon(uint32_t token);  // begun, not ended (its list changed under it): the reader skips it

struct Scope {
  Scope(ID3D12GraphicsCommandList* cl, Kind kind, int32_t handle) : cl(cl), token(begin(cl, kind, handle)) {}
  ~Scope() {
    if (token) end(cl, token);
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
  ID3D12GraphicsCommandList* cl;
  uint32_t token;
};

// Tab-separated lines for the CET window and the status report (format in gpu_timing.cpp); "" = off.
std::string report();

}  // namespace core::gpu_timing
