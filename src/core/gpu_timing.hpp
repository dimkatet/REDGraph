#pragma once

// GPU time of the host's own work items (docs/plan-gpu-timing-and-hlsl.md section 1; the timestamp ring is ported
// from spuddeh/cp2077-native-shaders `Timing.cpp`, with the author's permission). [Diagnostics] GpuTiming, live, off by
// default: nothing is created and every bracket is one relaxed load until it is switched on.
//
// A bracket is two EndQuery(TIMESTAMP) on the engine's own command list around one item - a companion dispatch, a
// companion draw, a client pass - and a ResolveQueryData of that pair into a READBACK buffer RIGHT AFTER the end, on
// the same list (the host has no list that is last in a frame). With them, a PIX-style debug event
// "REDGraph: <client>/<name>" (BeginEvent metadata 0 = a wide string) so the item is findable in Nsight / RenderDoc.
// Pairs come from an atomic counter in a ring of kRing frame slots (lists are recorded on many threads); a slot is
// read kRing frames later at the frame hook - never waited on - and cleared, so a pair the GPU has not resolved yet
// reads 0 and is counted LATE instead of returning an older frame's numbers. A reading counts when begin != 0,
// end >= begin and < 50 ms; per item it is the SUM of the frame's pairs, smoothed new = old + (x - old) * 0.05.
// The first bracket of a frame also records an empty pair: the cost of timing itself.
//
// Includes whatever barrier / wait the engine put before our work on that list, and on a compute-queue list ticks of
// that queue read with the direct queue's frequency. Not an absolute truth (memory: GPU timer per-dispatch unreliable
// across sessions) - the number that is always there; ablation stays the arbiter.

#include <cstdint>
#include <string>

struct ID3D12GraphicsCommandList;

namespace core::gpu_timing {

enum Kind : uint8_t { kCompanion, kDrawCompanion, kPass, kKinds };

void set_enabled(bool on);  // live: the first frame after `on` creates the heap + buffer (once)
bool enabled();
void frame_tick();          // the frame hook (prof.cpp): reads the slot it is about to reuse

// 0 = not timed (off, not ready, the frame's pairs used up). Pair it with end() on the same list.
uint32_t begin(ID3D12GraphicsCommandList* cl, Kind kind, int32_t handle);
void end(ID3D12GraphicsCommandList* cl, uint32_t token);

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
