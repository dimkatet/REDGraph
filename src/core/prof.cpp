// prof.hpp.

#include "core/prof.hpp"

#include <Windows.h>

#include <cstdio>
#include <mutex>

#include "core/engine_231.hpp"
#include "core/gpu_timing.hpp"
#include "core/internal.hpp"

namespace core::prof {

std::atomic<bool> g_on{false};
thread_local uint64_t t_paused = 0;

namespace {

constexpr uint32_t kFlush = 16;  // calls per thread between two adds to the totals

const char* const kName[kPoints] = {"bind funnel", "UpdateRootParams", "Dispatch", "node Process", "RunJob",
                                    "RenderFlow request", "wrapped Execute", "DispatchRays", "GetShaderGroupId",
                                    "engine blocks", "Draw"};

// A thread's partial sums belong to one measurement: a new one (epoch) drops what an earlier one left unflushed.
std::atomic<uint32_t> g_epoch{0};
// Per thread as well: the frame does not get longer by work on an idle worker, only by work on its critical path.
constexpr uint32_t kThreads = 256;
struct Thread {
  std::atomic<uint32_t> tid{0};
  std::atomic<uint64_t> cycles[kPoints] = {}, calls[kPoints] = {};
};
Thread g_thr[kThreads];
std::atomic<uint32_t> g_nthr{0};
uint32_t g_frame_tid = 0;  // the thread that runs ExecuteParallel (RenderFrame)

struct Local {
  uint32_t epoch = 0;
  Thread* t = nullptr;  // nullptr: more than kThreads threads - counted in the totals only
  uint64_t cycles[kPoints] = {};
  uint32_t calls[kPoints] = {};
};
thread_local Local t_local;
std::atomic<uint64_t> g_cycles[kPoints] = {}, g_calls[kPoints] = {};

bool g_hooked = false;   // frames are counted (our own hook or graph_probe's)
bool g_own_hook = false;
using ExecuteParallel_t = void (*)(const void* graph, const void* ctx, void* builder);
ExecuteParallel_t g_orig = nullptr;

enum State { kIdle, kArmed, kMeasuring };
std::mutex g_mu;
std::atomic<State> g_state{kIdle};
LONGLONG g_start = 0, g_end = 0;  // QPC: measuring starts / ends
double g_seconds = 0;
uint64_t g_frames = 0, g_tsc0 = 0;
LARGE_INTEGER g_qpc0{};
std::string g_report;

LONGLONG qpc_now() {
  LARGE_INTEGER q;
  QueryPerformanceCounter(&q);
  return q.QuadPart;
}
double qpc_freq() {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  return static_cast<double>(f.QuadPart);
}

void begin(LONGLONG now) {
  for (uint32_t p = 0; p < kPoints; ++p) {
    g_cycles[p].store(0);
    g_calls[p].store(0);
  }
  for (Thread& t : g_thr) {
    t.tid.store(0);
    for (uint32_t p = 0; p < kPoints; ++p) {
      t.cycles[p].store(0);
      t.calls[p].store(0);
    }
  }
  g_nthr.store(0);
  g_frame_tid = GetCurrentThreadId();
  g_epoch.fetch_add(1);
  g_frames = 0;
  g_tsc0 = __rdtsc();
  g_qpc0.QuadPart = now;
  g_state = kMeasuring;
  g_on.store(true);
  g_sdk->logger->InfoF(g_handle, "host profile: measuring %.0f s", g_seconds);
}

void finish(LONGLONG now) {
  g_on.store(false);
  g_state = kIdle;
  const double secs = static_cast<double>(now - g_qpc0.QuadPart) / qpc_freq();
  const double tsc_per_us = static_cast<double>(__rdtsc() - g_tsc0) / (secs * 1e6);  // the TSC rate, measured
  const double frames = static_cast<double>(g_frames ? g_frames : 1);
  char line[200];
  std::string o;
  double total = 0;
  for (uint32_t p = 0; p < kPoints; ++p) {
    const double dc = static_cast<double>(g_cycles[p].load()), dn = static_cast<double>(g_calls[p].load());
    if (dn == 0) continue;
    const double us_frame = dc / tsc_per_us / frames;
    total += us_frame;
    std::snprintf(line, sizeof line, "host profile:   %-18s %8.1f calls/frame %8.1f us/frame %7.0f ns/call\n",
                  kName[p], dn / frames, us_frame, dc / tsc_per_us * 1000.0 / dn);
    o += line;
  }
  std::snprintf(line, sizeof line,
                "host profile: %llu frames in %.1f s (%.1f fps) - HOST CPU %.1f us/frame (all threads)\n",
                static_cast<unsigned long long>(g_frames), secs, frames / secs, total);
  o = line + o;
  // The frame thread, the busiest other one, the rest.
  const uint32_t nt = g_nthr.load() < kThreads ? g_nthr.load() : kThreads;
  auto sum = [&](const Thread& t) {
    uint64_t c = 0;
    for (uint32_t p = 0; p < kPoints; ++p) c += t.cycles[p].load();
    return static_cast<double>(c) / tsc_per_us / frames;
  };
  auto points = [&](const Thread& t) {
    for (uint32_t p = 0; p < kPoints; ++p) {
      const double dc = static_cast<double>(t.cycles[p].load()), dn = static_cast<double>(t.calls[p].load());
      if (dn == 0) continue;
      std::snprintf(line, sizeof line, "host profile:     %-18s %8.1f calls/frame %8.1f us/frame\n", kName[p],
                    dn / frames, dc / tsc_per_us / frames);
      o += line;
    }
  };
  const Thread* ft = nullptr;
  const Thread* busiest = nullptr;
  double rest = 0, busiest_us = -1;
  uint32_t others = 0;
  for (uint32_t i = 0; i < nt; ++i) {
    const Thread& t = g_thr[i];
    if (t.tid.load() == g_frame_tid) {
      ft = &t;
      continue;
    }
    const double us = sum(t);
    rest += us;
    ++others;
    if (us > busiest_us) {
      busiest_us = us;
      busiest = &t;
    }
  }
  std::snprintf(line, sizeof line, "host profile: FRAME thread (RenderFrame, tid %u): %.1f us/frame\n", g_frame_tid,
                ft ? sum(*ft) : 0.0);
  o += line;
  if (ft) points(*ft);
  if (busiest) {
    std::snprintf(line, sizeof line, "host profile: busiest other thread (tid %u): %.1f us/frame\n",
                  busiest->tid.load(), busiest_us);
    o += line;
    points(*busiest);
  }
  std::snprintf(line, sizeof line, "host profile: %u other threads together: %.1f us/frame%s\n", others, rest,
                g_nthr.load() > kThreads ? " (more threads than slots - the rest only in the totals)" : "");
  o += line;
  g_report = o;
  size_t a = 0;
  while (a < o.size()) {
    const size_t b = o.find('\n', a);
    g_sdk->logger->Info(g_handle, o.substr(a, b - a).c_str());
    a = b + 1;
  }
}

// Every frame: the timer.
void frame() {
  if (g_state == kIdle) return;  // read without the lock: only arm() leaves kIdle, the next frame sees it
  std::lock_guard<std::mutex> g(g_mu);
  const LONGLONG now = qpc_now();
  if (g_state == kArmed && now >= g_start) {
    begin(now);
    return;
  }
  if (g_state != kMeasuring) return;
  ++g_frames;
  if (now >= g_end) finish(now);
}

void frame_timer_tick();  // [Diagnostics] FrameTimer, below - ticked here too: without the census this is the only hook

void d_execute_parallel(const void* graph, const void* ctx, void* builder) {
  frame_timer_tick();
  frame();
  pso::frame_tick();
  bindings::frame_tick();
  gpu_timing::frame_tick();
  g_orig(graph, ctx, builder);
}

}  // namespace

void add(Point p, uint64_t cycles) {
  Local& l = t_local;
  const uint32_t e = g_epoch.load(std::memory_order_relaxed);
  if (l.epoch != e) {
    l = Local{e};
    const uint32_t i = g_nthr.fetch_add(1);
    if (i < kThreads) {
      l.t = &g_thr[i];
      l.t->tid.store(GetCurrentThreadId());
    }
  }
  l.cycles[p] += cycles;
  if (++l.calls[p] >= kFlush) {
    g_cycles[p].fetch_add(l.cycles[p], std::memory_order_relaxed);
    g_calls[p].fetch_add(l.calls[p], std::memory_order_relaxed);
    if (l.t) {
      l.t->cycles[p].fetch_add(l.cycles[p], std::memory_order_relaxed);
      l.t->calls[p].fetch_add(l.calls[p], std::memory_order_relaxed);
    }
    l.cycles[p] = 0;
    l.calls[p] = 0;
  }
}

// [Diagnostics] FrameTimer (2026-10-17): the frame time in 10-s windows, measured between two ExecuteParallel calls
// (one per rendered frame) - the instrument of an on/off A/B of any client's feature, whose switches are in the
// client's own log with their time. Gaps over 0.25 s (menus, loading) are left out; a window with no frame is not logged.
namespace {
std::atomic<bool> g_ft_on{false};
LONGLONG g_ft_last = 0, g_ft_next = 0;  // the frame thread only
uint64_t g_ft_frames = 0;
double g_ft_sec = 0, g_ft_worst = 0;

void frame_timer_tick() {
  if (!g_ft_on.load(std::memory_order_relaxed)) {
    g_ft_last = g_ft_next = 0;
    return;
  }
  const LONGLONG now = qpc_now();
  const double f = qpc_freq();
  if (g_ft_last) {
    const double dt = static_cast<double>(now - g_ft_last) / f;
    if (dt > 0 && dt < 0.25) {
      ++g_ft_frames;
      g_ft_sec += dt;
      if (dt > g_ft_worst) g_ft_worst = dt;
    }
  }
  g_ft_last = now;
  if (!g_ft_next) {
    g_ft_next = now + static_cast<LONGLONG>(10.0 * f);
    g_ft_frames = 0;
    g_ft_sec = g_ft_worst = 0;
    return;
  }
  if (now < g_ft_next) return;
  if (g_ft_frames)
    g_sdk->logger->InfoF(g_handle, "[frame] last 10 s: %llu frames, %.2f fps, frame %.3f ms (worst %.1f)",
                         static_cast<unsigned long long>(g_ft_frames), g_ft_frames / g_ft_sec,
                         1000.0 * g_ft_sec / g_ft_frames, 1000.0 * g_ft_worst);
  g_ft_next = now + static_cast<LONGLONG>(10.0 * f);
  g_ft_frames = 0;
  g_ft_sec = g_ft_worst = 0;
}
}  // namespace

void set_frame_timer(bool on) {
  if (g_ft_on.exchange(on) != on)
    g_sdk->logger->InfoF(g_handle, "[frame] frame timer %s", on ? "ON - a line every 10 s" : "off");
}

void on_graph_execute() {
  frame_timer_tick();
  if (g_own_hook) return;
  frame();
  pso::frame_tick();
  bindings::frame_tick();
  gpu_timing::frame_tick();
}

void attach_frames() {
  if (graph_probe::census()) {  // graph_probe's ExecuteParallel detour calls on_graph_execute
    g_hooked = true;
    return;
  }
  g_own_hook = attach_hook(e231::kGraphExecuteParallel, e231::kRenderFrameExecuteRet,
                           reinterpret_cast<void*>(&d_execute_parallel), reinterpret_cast<void**>(&g_orig),
                           "CRenderNodeGraph::ExecuteParallel (host profile frames)");
  g_hooked = g_own_hook;
  if (!g_hooked) g_sdk->logger->Warn(g_handle, "host profile: NO frame hook - the profile cannot run");
}

void detach() {
  g_on.store(false);
  if (g_own_hook) detach_hook(e231::kGraphExecuteParallel);
  g_own_hook = false;
}

std::string arm(double delay_s, double seconds) {
  if (!g_hooked) return "no frame hook (the host is not attached)";
  if (seconds < 1 || seconds > 600 || delay_s < 0 || delay_s > 600) return "delay 0..600 s, length 1..600 s";
  std::lock_guard<std::mutex> g(g_mu);
  if (g_state != kIdle) return "already running";
  const double f = qpc_freq();
  g_start = qpc_now() + static_cast<LONGLONG>(delay_s * f);
  g_end = g_start + static_cast<LONGLONG>(seconds * f);
  g_seconds = seconds;
  g_state = kArmed;
  g_sdk->logger->InfoF(g_handle, "host profile: armed - starts in %.0f s, measures %.0f s", delay_s, seconds);
  return "";
}

std::string state() {
  std::lock_guard<std::mutex> g(g_mu);
  char s[96];
  const double f = qpc_freq(), now = static_cast<double>(qpc_now());
  if (g_state == kArmed && now >= static_cast<double>(g_start))  // starts at a frame of the graph: none in a menu
    std::snprintf(s, sizeof s, "waiting for the world to render (no frame of the render graph)");
  else if (g_state == kArmed)
    std::snprintf(s, sizeof s, "starts in %.0f s", (static_cast<double>(g_start) - now) / f);
  else if (g_state == kMeasuring)
    std::snprintf(s, sizeof s, "measuring - %.0f s left, %llu frames", (static_cast<double>(g_end) - now) / f,
                  static_cast<unsigned long long>(g_frames));
  else
    std::snprintf(s, sizeof s, "idle");
  return s;
}

std::string report() {
  std::lock_guard<std::mutex> g(g_mu);
  return g_report;
}

}  // namespace core::prof
