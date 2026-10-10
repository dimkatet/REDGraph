#pragma once

// The host's own CPU cost per frame (readiness item 4), on a TIMER from the CET window (RG_Profile): wait `delay`
// seconds (close the overlay), measure `seconds`, write one report into the log. Every hot detour opens a Scope; the
// engine's original and every client callback inside it run under a Pause, so a point counts only the HOST's code.
// Not measuring, a Scope / Pause is one relaxed load. Frames = calls of CRenderNodeGraph::ExecuteParallel (one call
// site in RenderFrame, docs/internals.md) - the timer is checked there, so the window is whole frames.

#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace core::prof {

enum Point : uint8_t {
  kBind,       // pso: the bind funnel (ComputePSO / GraphicsPSO::CreatePSO) - also counted always (host_stats)
  kUrp,        // bindings: before every UpdateRootParams (lease staging)
  kDispatch,   // passes: around DispatchCompute / DispatchIndirectCompute (subscriptions, companions, passes)
  kProcess,    // render_flow: every node's Process (the node on the thread)
  kJob,        // render_flow: every job::Dispatcher::RunJob (jobs carry their node)
  kRequest,    // render_flow: every RenderFlow request (census / removal records)
  kNodeExec,   // render_nodes: a wrapped node's Execute (wraps, removals, the debug module)
  kRays,       // rt_lib: DispatchRays (variant choice)
  kGsgi,       // rt_lib: GetShaderGroupIdentifier
  kEngineBlock,// cb_hooks: the engine constant-block copies (UpdateBuffer / SetComputeShaderConstsRaw)
  kDraw,       // passes: around every GpuApi draw (draw subscriptions, draw companions, passes after a draw)
  kPoints
};

extern std::atomic<bool> g_on;

void add(Point p, uint64_t cycles);
extern thread_local uint64_t t_paused;

struct Scope {
  uint64_t t0 = 0, p0 = 0;
  Point p;
  explicit Scope(Point point) : p(point) {
    if (g_on.load(std::memory_order_relaxed)) {
      p0 = t_paused;
      t0 = __rdtsc();
    }
  }
  ~Scope() {
    if (!t0) return;
    const uint64_t all = __rdtsc() - t0, paused = t_paused - p0;
    add(p, all > paused ? all - paused : 0);
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

// Not the host's time: the engine's original, a client's callback.
// Nested Scopes inside (the engine calls back into another detour) Pause too; the outer Pause sets the total so their
// time is subtracted once, not twice.
struct Pause {
  uint64_t t0 = 0, p0 = 0;
  Pause() {
    if (g_on.load(std::memory_order_relaxed)) {
      p0 = t_paused;
      t0 = __rdtsc();
    }
  }
  ~Pause() {
    if (t0) t_paused = p0 + (__rdtsc() - t0);
  }
  Pause(const Pause&) = delete;
  Pause& operator=(const Pause&) = delete;
};

void attach_frames();      // after the core's attach: its own ExecuteParallel hook unless graph_probe has one
void detach();
void on_graph_execute();   // from graph_probe's ExecuteParallel detour (when that one is attached)
std::string arm(double delay_s, double seconds);  // "" = armed, else why not
std::string state();       // one line: idle / starts in / measuring, s left
std::string report();      // the last measurement ("" = none yet)

}  // namespace core::prof
