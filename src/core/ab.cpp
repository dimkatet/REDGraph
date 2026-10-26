// ab.hpp.

#include "core/ab.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/gpu_timing.hpp"
#include "core/internal.hpp"
#include "core/prof.hpp"

namespace core::ab {
namespace {

std::atomic<bool> g_enabled{false}, g_measuring{false}, g_running{false}, g_abort{false};

// ---- the frame (render thread writes, the runner reads) ----
std::atomic<uint64_t> g_frames{0}, g_interval{0}, g_gaps{0};  // QPC ticks
// record: the frame's kick-off (ExecuteParallel, which only hands the nodes to jobs and returns in ~25 us) to the END
// of its last node Execute on any thread - the CPU's recording of the frame (rdtsc).
std::atomic<uint64_t> g_record{0}, g_rec_n{0}, g_exec_end{0};
LONGLONG g_last_begin = 0;  // render thread only
uint64_t g_tsc_begin = 0;   // render thread only
constexpr double kGapS = 0.25;

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

// ---- CPU per node class (render_nodes' class index) ----
constexpr uint32_t kCls = 256;
std::atomic<uint64_t> g_exec[kCls] = {}, g_job[kCls] = {};
thread_local uint64_t t_nested = 0;  // the closed Spans' time inside the open one on this thread

// ---- client time, by (what, name) pointers ----
constexpr uint32_t kClients = 64;
struct ClientRow {
  std::atomic<const char*> what{nullptr}, name{nullptr};
  std::atomic<uint64_t> cycles{0};
};
ClientRow g_cl[kClients];
std::atomic<uint32_t> g_ncl{0};
std::atomic<bool> g_cl_full{false};
std::mutex g_cl_mu;

// ---- switches ----
constexpr uint32_t kSwitches = 32;
struct Switch {
  const char* name;
  rg_ab_set_fn set;
  rg_ab_get_fn get;
  void* user;
};
Switch g_sw[kSwitches];
std::atomic<uint32_t> g_nsw{0};
std::mutex g_sw_mu;

// ---- a run ----
struct Snap {
  LONGLONG qpc;
  uint64_t tsc;
  uint64_t frames, interval, record, rec_n, gaps;
  uint64_t exec[kCls], job[kCls];
  uint64_t host[prof::kPoints];
  uint64_t cl[kClients];
  gpu_timing::Totals gpu;
};

// The per-frame metrics of a block, ms per frame.
enum Fixed : uint32_t { mFrame, mRecord, mGpuSpan, mGpuNodes, mHost, mClients, mExecSum, mJobSum, kFixed };
const char* const kFixedName[kFixed] = {"frame",        "CPU record (start -> last Execute)", "GPU span", "GPU nodes sum",
                                        "host CPU",     "clients CPU",   "engine Execute CPU", "engine jobs CPU"};
constexpr uint32_t kGpu0 = kFixed, kExec0 = kGpu0 + gpu_timing::kItemCount, kJob0 = kExec0 + kCls,
                   kHost0 = kJob0 + kCls, kCl0 = kHost0 + prof::kPoints, kMetrics = kCl0 + kClients;

struct Block {
  bool valid = false;
  double frames = 0;
  std::vector<double> m = std::vector<double>(kMetrics, 0.0);
};

std::mutex g_mu;  // the state line and the report (runner writes, CET reads)
std::string g_state = "idle", g_report;

void snap(Snap* s) {
  s->qpc = qpc_now();
  s->tsc = __rdtsc();
  s->frames = g_frames.load();
  s->interval = g_interval.load();
  s->record = g_record.load();
  s->rec_n = g_rec_n.load();
  s->gaps = g_gaps.load();
  for (uint32_t i = 0; i < kCls; ++i) {
    s->exec[i] = g_exec[i].load(std::memory_order_relaxed);
    s->job[i] = g_job[i].load(std::memory_order_relaxed);
  }
  prof::totals(s->host);
  for (uint32_t i = 0; i < kClients; ++i) s->cl[i] = g_cl[i].cycles.load(std::memory_order_relaxed);
  gpu_timing::totals(&s->gpu);
}

Block measure(const Snap& a, const Snap& b, double qpf) {
  Block r;
  const double frames = static_cast<double>(b.frames - a.frames), recs = static_cast<double>(b.rec_n - a.rec_n);
  const double secs = static_cast<double>(b.qpc - a.qpc) / qpf;
  const double tsc_per_ms = static_cast<double>(b.tsc - a.tsc) / (secs * 1000.0);
  const uint64_t read = b.gpu.read - a.gpu.read;
  r.frames = frames;
  r.valid = b.gaps == a.gaps && frames >= 10 && recs >= 10 && read > 0;
  if (frames <= 0 || recs <= 0 || tsc_per_ms <= 0) return r;
  auto cpu = [&](uint64_t c) { return static_cast<double>(c) / tsc_per_ms / frames; };
  std::vector<double>& m = r.m;
  m[mFrame] = static_cast<double>(b.interval - a.interval) / qpf * 1000.0 / frames;
  m[mRecord] = static_cast<double>(b.record - a.record) / tsc_per_ms / recs;
  const uint64_t sf = b.gpu.span_frames - a.gpu.span_frames;
  m[mGpuSpan] = sf ? (b.gpu.span_ms - a.gpu.span_ms) / static_cast<double>(sf) : 0.0;
  for (uint32_t i = 0; i < gpu_timing::kItemCount; ++i) {
    const uint64_t bad = b.gpu.item_bad[i] - a.gpu.item_bad[i];
    const double n = read > bad ? static_cast<double>(read - bad) : 0.0;
    const double v = n > 0 ? (b.gpu.item_ms[i] - a.gpu.item_ms[i]) / n : 0.0;
    m[kGpu0 + i] = v;
    if (gpu_timing::item_kind(i) == gpu_timing::kNode) m[mGpuNodes] += v;
  }
  for (uint32_t i = 0; i < kCls; ++i) {
    m[kExec0 + i] = cpu(b.exec[i] - a.exec[i]);
    m[kJob0 + i] = cpu(b.job[i] - a.job[i]);
    m[mExecSum] += m[kExec0 + i];
    m[mJobSum] += m[kJob0 + i];
  }
  for (uint32_t p = 0; p < prof::kPoints; ++p) {
    m[kHost0 + p] = cpu(b.host[p] - a.host[p]);
    m[mHost] += m[kHost0 + p];
  }
  for (uint32_t i = 0; i < kClients; ++i) {
    m[kCl0 + i] = cpu(b.cl[i] - a.cl[i]);
    m[mClients] += m[kCl0 + i];
  }
  return r;
}

// Two-sided 95 % t quantile, n - 1 degrees of freedom.
double t95(size_t n) {
  static const double t[] = {0, 12.71, 4.30, 3.18, 2.78, 2.57, 2.45, 2.36, 2.31, 2.26, 2.23,
                             2.20, 2.18, 2.16, 2.14, 2.13, 2.12, 2.11, 2.10, 2.09, 2.09};
  const size_t df = n > 1 ? n - 1 : 1;
  return df < sizeof(t) / sizeof(t[0]) ? t[df] : (df < 30 ? 2.05 : 2.0);
}

struct Stat {
  double a = 0, b = 0, d = 0, ci = 0;
};

struct Result {
  std::vector<Stat> s = std::vector<Stat>(kMetrics);
  size_t n = 0;
};

Result stats(const std::vector<Block>& A, const std::vector<Block>& B) {
  Result r;
  std::vector<size_t> ok;
  for (size_t p = 0; p < A.size(); ++p)
    if (A[p].valid && B[p].valid) ok.push_back(p);
  r.n = ok.size();
  if (ok.empty()) return r;
  const double n = static_cast<double>(ok.size());
  for (uint32_t k = 0; k < kMetrics; ++k) {
    Stat& s = r.s[k];
    for (size_t p : ok) {
      s.a += A[p].m[k];
      s.b += B[p].m[k];
    }
    s.a /= n;
    s.b /= n;
    s.d = s.b - s.a;
    double var = 0;
    for (size_t p : ok) {
      const double e = (B[p].m[k] - A[p].m[k]) - s.d;
      var += e * e;
    }
    s.ci = ok.size() > 1 ? t95(ok.size()) * std::sqrt(var / (n - 1)) / std::sqrt(n) : 0.0;
  }
  return r;
}

void addf(std::string& o, const char* fmt, ...) {
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  o += "[ab] ";
  o += line;
  o += "\n";
}

std::string row(const char* label, const Stat& s) {
  char line[256];
  std::snprintf(line, sizeof line, "%-44s A %8.3f  B %8.3f  d %+8.3f +- %.3f ms%s", label, s.a, s.b, s.d, s.ci,
                std::fabs(s.d) > s.ci && s.ci > 0 ? "" : "  (noise)");
  return line;
}

// The metrics in [first, first + count) worth a line: |d| over its interval, else the `top` largest |d|.
std::vector<uint32_t> pick(const Result& r, uint32_t first, uint32_t count, size_t top, double floor_ms) {
  std::vector<uint32_t> v;
  for (uint32_t k = first; k < first + count; ++k)
    if (std::fabs(r.s[k].d) >= floor_ms || std::fabs(r.s[k].a) >= floor_ms || std::fabs(r.s[k].b) >= floor_ms)
      v.push_back(k);
  std::sort(v.begin(), v.end(), [&](uint32_t x, uint32_t y) { return std::fabs(r.s[x].d) > std::fabs(r.s[y].d); });
  std::vector<uint32_t> out;
  for (size_t i = 0; i < v.size(); ++i)
    if (i < top || std::fabs(r.s[v[i]].d) > r.s[v[i]].ci) out.push_back(v[i]);
  return out;
}

std::string client_label(uint32_t i) {
  const char* w = g_cl[i].what.load();
  const char* n = g_cl[i].name.load();
  return std::string(w ? w : "?") + " '" + (n ? n : "?") + "'";
}

std::string report(const Result& r, const std::string& spec, size_t pairs, double block, double settle,
                   const std::vector<Block>& A, const std::vector<Block>& B) {
  std::string o;
  addf(o, "RESULT '%s': %zu valid pairs of %zu, blocks %.1f s + %.1f s settle; A = off, B = on; per frame, "
       "d = B - A, +- = 95 %% over the pairs", spec.c_str(), r.n, pairs, block, settle);
  if (!r.n) {
    addf(o, "no valid pair (every block had a gap over %.2f s, or no GPU frames read) - nothing to report", kGapS);
    return o;
  }
  const auto& s = r.s;
  for (uint32_t k = 0; k < kFixed; ++k) addf(o, "%s", row(kFixedName[k], s[k]).c_str());
  addf(o, "fps A %.1f -> B %.1f", s[mFrame].a > 0 ? 1000.0 / s[mFrame].a : 0.0,
       s[mFrame].b > 0 ? 1000.0 / s[mFrame].b : 0.0);
  auto ratio = [](double x, double f) { return f > 0 ? x / f : 0.0; };
  const double ga = ratio(s[mGpuSpan].a, s[mFrame].a), gb = ratio(s[mGpuSpan].b, s[mFrame].b);
  const double ra = ratio(s[mRecord].a, s[mFrame].a), rb = ratio(s[mRecord].b, s[mFrame].b);
  addf(o, "bound: GPU span / frame A %.2f B %.2f, record / frame A %.2f B %.2f -> %s", ga, gb, ra, rb,
       gb > 0.9 ? "GPU-bound: the frame follows the GPU span" : rb > 0.9 ? "CPU-bound on the render thread"
                                                                        : "neither close to the frame (game thread / "
                                                                          "present / sync)");
  addf(o, "NOTE: GPU span is UNRELIABLE (known issue 2026-10-26: reads far below the frame since the context fix) - "
          "trust frame, rays and our items; do not read the bound verdict above off it");

  // GPU: per node, and the items of ours recorded inside each.
  addf(o, "-- GPU per engine node (ms/frame; [ours inside] = our items recorded in that node's Execute) --");
  const auto nodes = pick(r, kGpu0 + gpu_timing::kNodeBase, kCls, 12, 0.002);
  double ours_in_nodes = 0, ours_free = 0, rays = 0;
  for (uint32_t i = 0; i < gpu_timing::kItemCount; ++i) {
    const auto k = gpu_timing::item_kind(i);
    if (k == gpu_timing::kNode) continue;
    if (k == gpu_timing::kRaysLib || k == gpu_timing::kRaysVariant) rays += s[kGpu0 + i].d;
    else if (gpu_timing::item_node(i) >= 0) ours_in_nodes += s[kGpu0 + i].d;
    else ours_free += s[kGpu0 + i].d;
  }
  for (uint32_t m : nodes) {
    const int32_t cls = static_cast<int32_t>(m - kGpu0 - gpu_timing::kNodeBase);
    std::string inside;
    for (uint32_t i = 0; i < gpu_timing::kItemCount; ++i) {
      if (gpu_timing::item_kind(i) == gpu_timing::kNode || gpu_timing::item_node(i) != cls) continue;
      if (s[kGpu0 + i].a == 0 && s[kGpu0 + i].b == 0) continue;
      char t[160];
      std::snprintf(t, sizeof t, " [%s %s/%s %+.3f]", gpu_timing::kind_name(gpu_timing::item_kind(i)),
                    gpu_timing::item_client(i).c_str(), gpu_timing::item_name(i).c_str(), s[kGpu0 + i].d);
      inside += t;
    }
    addf(o, "  %s%s", row(render_nodes::class_name(cls), s[m]).c_str(), inside.c_str());
  }
  // A node's bracket is one list of its Execute: a DispatchRays of a ray-traced node was measured OUTSIDE it (2026-10-26:
  // RayTracedReflections 2.1 ms, its rays 4.3 ms) - the rays are their own term; the nodes' sum is noisy (async work).
  const double engine_nodes = s[mGpuNodes].d - ours_in_nodes;
  addf(o, "GPU decomposition: d span %+.3f = rays with our libraries %+.3f (whole DispatchRays) + our companions / "
       "passes %+.3f + engine nodes' own work %+.3f (+- %.3f, noisy) + the rest %+.3f", s[mGpuSpan].d, rays,
       ours_in_nodes + ours_free, engine_nodes, s[mGpuNodes].ci, s[mGpuSpan].d - rays - ours_in_nodes - ours_free -
       engine_nodes);
  addf(o, "-- GPU items of ours (rays = the whole DispatchRays, the engine's work included) --");
  for (uint32_t i = 0; i < gpu_timing::kItemCount; ++i) {
    if (gpu_timing::item_kind(i) == gpu_timing::kNode) continue;
    const Stat& t = s[kGpu0 + i];
    if (t.a == 0 && t.b == 0) continue;
    const int32_t in = gpu_timing::item_node(i);
    const std::string label = std::string(gpu_timing::kind_name(gpu_timing::item_kind(i))) + " " +
                              gpu_timing::item_client(i) + "/" + gpu_timing::item_name(i);
    addf(o, "  %s  in %s", row(label.c_str(), t).c_str(), in >= 0 ? render_nodes::class_name(in) : "(no node)");
  }

  // CPU.
  addf(o, "-- CPU, all threads (WORK per frame, not the critical path; the record line above is the render thread) --");
  for (uint32_t p = 0; p < prof::kPoints; ++p) {
    const Stat& t = s[kHost0 + p];
    if (t.a == 0 && t.b == 0) continue;
    addf(o, "  %s", row((std::string("host ") + prof::point_name(p)).c_str(), t).c_str());
  }
  for (uint32_t i = 0; i < g_ncl.load() && i < kClients; ++i) {
    const Stat& t = s[kCl0 + i];
    if (t.a == 0 && t.b == 0) continue;
    addf(o, "  %s", row(("client " + client_label(i)).c_str(), t).c_str());
  }
  for (uint32_t m : pick(r, kExec0, kCls, 10, 0.005))
    addf(o, "  %s", row((std::string("Execute ") + render_nodes::class_name(static_cast<int32_t>(m - kExec0))).c_str(),
                        s[m]).c_str());
  for (uint32_t m : pick(r, kJob0, kCls, 8, 0.005))
    addf(o, "  %s", row((std::string("jobs of ") + render_nodes::class_name(static_cast<int32_t>(m - kJob0))).c_str(),
                        s[m]).c_str());
  addf(o, "CPU decomposition: d record %+.3f (render thread); work: host %+.3f + clients %+.3f + engine Execute "
       "%+.3f + engine jobs %+.3f", s[mRecord].d, s[mHost].d, s[mClients].d, s[mExecSum].d, s[mJobSum].d);

  addf(o, "-- pairs (order, frame A / B, record A / B, GPU span A / B, frames A / B; * = dropped) --");
  for (size_t p = 0; p < A.size(); ++p)
    addf(o, "  %2zu %s%s %7.3f %7.3f  %7.3f %7.3f  %7.3f %7.3f  %5.0f %5.0f", p, p % 2 ? "BA" : "AB",
         A[p].valid && B[p].valid ? " " : "*", A[p].m[mFrame], B[p].m[mFrame], A[p].m[mRecord], B[p].m[mRecord],
         A[p].m[mGpuSpan], B[p].m[mGpuSpan], A[p].frames, B[p].frames);
  return o;
}

void log_lines(const std::string& o) {
  size_t a = 0;
  while (a < o.size()) {
    size_t b = o.find('\n', a);
    if (b == std::string::npos) b = o.size();
    g_sdk->logger->Info(g_handle, o.substr(a, b - a).c_str());
    a = b + 1;
  }
}

void set_state(const char* fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  std::lock_guard g(g_mu);
  g_state = line;
}

// false = aborted.
bool wait(double s) {
  const LONGLONG end = qpc_now() + static_cast<LONGLONG>(s * qpc_freq());
  while (qpc_now() < end) {
    if (g_abort.load()) return false;
    Sleep(20);
  }
  return !g_abort.load();
}

void set_all(const std::vector<uint32_t>& sw, uint32_t on) {
  for (uint32_t i : sw) g_sw[i].set(g_sw[i].user, on);
}

void runner(std::vector<uint32_t> sw, std::string spec, double block, double settle, uint32_t pairs) {
  const double qpf = qpc_freq();
  std::vector<uint32_t> orig;
  for (uint32_t i : sw) orig.push_back(g_sw[i].get ? g_sw[i].get(g_sw[i].user) : 1u);
  const bool gpu_was = gpu_timing::enabled();
  if (!gpu_was) gpu_timing::set_enabled(true);
  g_measuring.store(true);
  std::vector<Block> A(pairs), B(pairs);
  auto a = std::make_unique<Snap>(), b = std::make_unique<Snap>();
  bool ok = true;
  for (uint32_t p = 0; p < pairs && ok; ++p) {
    for (uint32_t k = 0; k < 2 && ok; ++k) {
      const uint32_t on = (p % 2 == 0) ? k : 1 - k;  // AB, BA, AB, ...
      set_all(sw, on);
      set_state("running '%s' - pair %u / %u, %s: settling", spec.c_str(), p + 1, pairs, on ? "B (on)" : "A (off)");
      if (!(ok = wait(settle))) break;
      snap(a.get());
      set_state("running '%s' - pair %u / %u, %s: measuring", spec.c_str(), p + 1, pairs, on ? "B (on)" : "A (off)");
      if (!(ok = wait(block))) break;
      snap(b.get());
      (on ? B : A)[p] = measure(*a, *b, qpf);
    }
  }
  for (size_t i = 0; i < sw.size(); ++i) g_sw[sw[i]].set(g_sw[sw[i]].user, orig[i]);
  g_measuring.store(false);
  if (!gpu_was) gpu_timing::set_enabled(false);
  prof::set_ab(false);
  std::string o;
  if (!ok) {
    addf(o, "ABORTED '%s' - the switches are back as they were", spec.c_str());
  } else {
    o = report(stats(A, B), spec, pairs, block, settle, A, B);
  }
  log_lines(o);
  {
    std::lock_guard g(g_mu);
    g_report = o;
    g_state = ok ? "idle - last run done" : "idle - last run aborted";
  }
  g_running.store(false);
}

}  // namespace

void enable() {
  g_enabled.store(true);
  render_flow::want();  // jobs carry their node: a job-builder node's CPU is in its jobs
}
bool enabled() { return g_enabled.load(); }
bool measuring() { return g_measuring.load(std::memory_order_relaxed); }

void frame_begin() {
  if (!g_measuring.load(std::memory_order_relaxed)) {
    g_last_begin = 0;
    g_tsc_begin = 0;
    return;
  }
  const uint64_t tsc = __rdtsc();
  // The previous frame's recording ended at its last Execute (the next frame starts after it: the graph is synced).
  const uint64_t end = g_exec_end.exchange(0, std::memory_order_relaxed);
  if (g_tsc_begin && end > g_tsc_begin) {
    g_record.fetch_add(end - g_tsc_begin, std::memory_order_relaxed);
    g_rec_n.fetch_add(1, std::memory_order_relaxed);
  }
  g_tsc_begin = tsc;
  const LONGLONG now = qpc_now();
  if (g_last_begin) {
    const LONGLONG dt = now - g_last_begin;
    if (dt > 0 && static_cast<double>(dt) < kGapS * qpc_freq()) {
      g_frames.fetch_add(1, std::memory_order_relaxed);
      g_interval.fetch_add(static_cast<uint64_t>(dt), std::memory_order_relaxed);
    } else {
      g_gaps.fetch_add(1, std::memory_order_relaxed);
    }
  }
  g_last_begin = now;
}

void frame_end() {}

Span::Span(int32_t node_class, bool is_job) : cls(node_class), job(is_job) {
  nested = t_nested;
  t_nested = 0;
  ours = prof::t_ours;
  t0 = __rdtsc();
}

Span::~Span() {
  const uint64_t t1 = __rdtsc(), dt = t1 - t0;
  if (!job) {
    uint64_t e = g_exec_end.load(std::memory_order_relaxed);
    while (t1 > e && !g_exec_end.compare_exchange_weak(e, t1, std::memory_order_relaxed)) {
    }
  }
  const uint64_t inner = t_nested + (prof::t_ours - ours);
  if (cls >= 0 && static_cast<uint32_t>(cls) < kCls)
    (job ? g_job : g_exec)[cls].fetch_add(dt > inner ? dt - inner : 0, std::memory_order_relaxed);
  prof::t_ours = ours;  // what ran inside is accounted here: the enclosing Span sees it in t_nested
  t_nested = nested + dt;
}

void client_add(const char* what, const char* name, uint64_t cycles) {
  const uint32_t n = g_ncl.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n; ++i)
    if (g_cl[i].name.load(std::memory_order_relaxed) == name && g_cl[i].what.load(std::memory_order_relaxed) == what) {
      g_cl[i].cycles.fetch_add(cycles, std::memory_order_relaxed);
      return;
    }
  std::lock_guard g(g_cl_mu);
  const uint32_t m = g_ncl.load();
  for (uint32_t i = n; i < m; ++i)
    if (g_cl[i].name.load() == name && g_cl[i].what.load() == what) {
      g_cl[i].cycles.fetch_add(cycles, std::memory_order_relaxed);
      return;
    }
  if (m >= kClients) {
    if (!g_cl_full.exchange(true))
      g_sdk->logger->WarnF(g_handle, "A/B profile: more than %u client time names - the rest not counted", kClients);
    return;
  }
  g_cl[m].what.store(what);
  g_cl[m].name.store(name);
  g_cl[m].cycles.store(cycles);
  g_ncl.store(m + 1, std::memory_order_release);
}

rg_handle ab_switch(const char* name, rg_ab_set_fn set, rg_ab_get_fn get, void* user) {
  if (!name || !*name || !set) return RG_E_ARG;
  std::lock_guard g(g_sw_mu);
  const uint32_t n = g_nsw.load();
  for (uint32_t i = 0; i < n; ++i)
    if (_stricmp(g_sw[i].name, name) == 0) return RG_E_ARG;
  if (n >= kSwitches) return RG_E_FULL;
  g_sw[n] = Switch{name, set, get, user};
  g_nsw.store(n + 1, std::memory_order_release);
  return static_cast<rg_handle>(n + 1);
}

uint32_t prof_active() { return g_measuring.load(std::memory_order_relaxed) ? 1u : 0u; }

void prof_add(const char* name, uint64_t tsc_cycles) {
  if (!name || !g_measuring.load(std::memory_order_relaxed)) return;
  client_add("own", name, tsc_cycles);
  prof::t_ours += tsc_cycles;
}

std::string run(const std::string& spec, double block_s, double settle_s, uint32_t pairs) {
  if (_stricmp(spec.c_str(), "stop") == 0) {
    if (!g_running.load()) return "not running";
    g_abort.store(true);
    return "";
  }
  if (!g_enabled.load()) return "[Diagnostics] ABProfile is off - set it in REDGraph.ini and restart the game";
  if (block_s < 0.5 || block_s > 60 || settle_s < 0 || settle_s > 30 || pairs < 1 || pairs > 100)
    return "block 0.5..60 s, settle 0..30 s, pairs 1..100";
  std::vector<uint32_t> sw;
  const uint32_t n = g_nsw.load(std::memory_order_acquire);
  if (spec == "*") {
    for (uint32_t i = 0; i < n; ++i) sw.push_back(i);
  } else {
    size_t a = 0;
    while (a <= spec.size()) {
      size_t b = spec.find(',', a);
      if (b == std::string::npos) b = spec.size();
      std::string one = spec.substr(a, b - a);
      while (!one.empty() && one.front() == ' ') one.erase(0, 1);
      while (!one.empty() && one.back() == ' ') one.pop_back();
      if (!one.empty()) {
        uint32_t i = 0;
        while (i < n && _stricmp(g_sw[i].name, one.c_str()) != 0) ++i;
        if (i == n) return "no switch '" + one + "' - RG_ABSwitches() lists them";
        sw.push_back(i);
      }
      a = b + 1;
    }
  }
  if (sw.empty()) return "no switch named (a comma list, or * for all)";
  if (g_running.exchange(true)) return "already running";
  if (!prof::set_ab(true)) {
    g_running.store(false);
    return "the host's timer profile (RG_Profile) is running";
  }
  g_abort.store(false);
  std::string names;
  for (uint32_t i : sw) names += std::string(names.empty() ? "" : ",") + g_sw[i].name;
  g_sdk->logger->InfoF(g_handle, "[ab] START '%s': %u pairs x 2 blocks of %.1f s + %.1f s settle (~%.0f s)",
                       names.c_str(), pairs, block_s, settle_s, pairs * 2 * (block_s + settle_s));
  set_state("starting '%s'", names.c_str());
  std::thread(runner, sw, names, block_s, settle_s, pairs).detach();
  return "";
}

std::string state() {
  std::lock_guard g(g_mu);
  return g_state + "\n" + g_report;
}

std::string switches() {
  std::string o;
  const uint32_t n = g_nsw.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n; ++i) {
    const Switch& s = g_sw[i];
    o += std::string(s.name) + "\t" + (s.get ? (s.get(s.user) ? "on" : "off") : "?") + "\n";
  }
  return o;
}

}  // namespace core::ab
