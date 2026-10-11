// gpu_timing.hpp. Ported from spuddeh/cp2077-native-shaders (Timing.cpp, the debug events of Main.cpp), with the
// author's permission; reworked for lists recorded on many threads (an atomic pair counter per frame slot, a resolve
// per pair, slots cleared after reading).
//
// Report format (report()): line 1 "gpu\t<frames read>\t<late>\t<overflow frames>\t<overhead ms>", then one line per
// item that was ever timed: "item\t<kind>\t<client>\t<name>\t<ms smoothed>\t<ms last>\t<per frame>\t<frames>\t
// <rejected>\t<late>" (kind: companion / draw companion / pass; ms = the item's sum over one frame).

#include "core/gpu_timing.hpp"

#include <Windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "core/clients.hpp"
#include "core/engine_231.hpp"
#include "core/internal.hpp"

namespace core::gpu_timing {
namespace {

using namespace e231;

constexpr uint32_t kRing = 8;          // frames between recording a slot and reading it
constexpr uint32_t kPairs = 2048;      // pairs per frame (a draw companion is timed per draw)
constexpr uint32_t kPerKind = 8;       // = kMaxComps / kMaxDrawComps (pso_patch.cpp), kMaxPasses (passes.cpp)
constexpr uint32_t kItems = kKinds * kPerKind;
constexpr uint8_t kEmpty = 0xff;       // the slot's empty pair
constexpr double kMaxMs = 50.0, kSmooth = 0.05;

struct Slot {
  std::atomic<uint32_t> n{0};             // pairs taken (may run past kPairs: the excess is refused)
  std::atomic<uint32_t> refused{0};       // items that lost a pair this frame: their frame is incomplete
  std::atomic<bool> empty_done{false};
  bool used = false;                      // recorded into since the last read
  uint8_t item[kPairs];
};
Slot g_slot[kRing];
std::atomic<uint32_t> g_cur{0};

std::atomic<bool> g_want{false}, g_active{false};
bool g_ready = false, g_failed = false;   // frame thread only
ID3D12QueryHeap* g_heap = nullptr;
ID3D12Resource* g_rb = nullptr;
const uint64_t* g_mapped = nullptr;
double g_ms_per_tick = 0;
uint64_t g_frame = 0;

struct Item {
  wchar_t event[96];  // "REDGraph: <client>/<name>"
  double ema = -1, last = 0;
  uint32_t per_frame = 0;
  uint64_t frames = 0, rejected = 0, late = 0;
};
Item g_item[kItems];
std::mutex g_mu;  // the stats: frame_tick writes, report reads
uint64_t g_read = 0, g_late = 0, g_overflow = 0;
double g_overhead = -1;

const clients::Kind kClientKind[kKinds] = {clients::kCompanion, clients::kDrawCompanion, clients::kPass};
const char* const kKindName[kKinds] = {"companion", "draw companion", "pass"};

void name_items() {
  for (uint32_t k = 0; k < kKinds; ++k)
    for (uint32_t i = 0; i < kPerKind; ++i) {
      std::string client = clients::client_of(kClientKind[k], static_cast<rg_handle>(i + 1));
      const std::string name = clients::name_of(kClientKind[k], static_cast<rg_handle>(i + 1));
      if (const size_t dot = client.rfind('.'); dot != std::string::npos) client.resize(dot);  // "X.dll" -> "X"
      std::swprintf(g_item[k * kPerKind + i].event, 96, L"REDGraph: %hs/%hs", client.empty() ? "?" : client.c_str(),
                    name.empty() ? "?" : name.c_str());
    }
}

bool create() {
  const auto* data = *reinterpret_cast<const uint8_t* const*>(g_base + kDeviceData);
  auto* engine_dev = data ? *reinterpret_cast<IUnknown* const*>(data + kDeviceInData) : nullptr;
  auto* queue = data ? *reinterpret_cast<ID3D12CommandQueue* const*>(data + kDirectQueueInData) : nullptr;
  ID3D12Device* dev = nullptr;
  if (!engine_dev || !queue || FAILED(engine_dev->QueryInterface(IID_PPV_ARGS(&dev)))) {
    g_sdk->logger->Warn(g_handle, "gpu timing: the engine's device / direct queue is not there - not timed");
    return false;
  }
  UINT64 freq = 0;
  HRESULT hr = queue->GetTimestampFrequency(&freq);
  if (SUCCEEDED(hr) && !freq) hr = E_FAIL;
  D3D12_QUERY_HEAP_DESC qd{};
  qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
  qd.Count = kRing * kPairs * 2;
  if (SUCCEEDED(hr)) hr = dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_heap));
  D3D12_RESOURCE_DESC rd{};
  rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  rd.Width = static_cast<UINT64>(qd.Count) * sizeof(uint64_t);
  rd.Height = 1;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.SampleDesc.Count = 1;
  rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  // READBACK starts and stays in COPY_DEST - what ResolveQueryData wants; no barrier ever.
  if (SUCCEEDED(hr))
    hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                      IID_PPV_ARGS(&g_rb));
  void* p = nullptr;
  if (SUCCEEDED(hr)) hr = g_rb->Map(0, nullptr, &p);  // stays mapped
  dev->Release();
  if (FAILED(hr)) {
    g_sdk->logger->WarnF(g_handle, "gpu timing: setup FAILED (0x%08x) - not timed", static_cast<unsigned>(hr));
    if (g_rb) g_rb->Release(), g_rb = nullptr;
    if (g_heap) g_heap->Release(), g_heap = nullptr;
    return false;
  }
  g_mapped = static_cast<const uint64_t*>(p);
  std::memset(p, 0, static_cast<size_t>(rd.Width));
  g_ms_per_tick = 1000.0 / static_cast<double>(freq);
  name_items();
  g_sdk->logger->InfoF(g_handle, "gpu timing: ON - %u frame slots x %u pairs, timestamp %llu Hz (direct queue)", kRing,
                       kPairs, static_cast<unsigned long long>(freq));
  return true;
}

void smooth(double* ema, double x) { *ema = *ema < 0 ? x : *ema + (x - *ema) * kSmooth; }

// The slot recorded kRing frames ago: every pair the GPU resolved since, then cleared for this frame.
void read(uint32_t si) {
  Slot& s = g_slot[si];
  const uint32_t n = s.n.load() < kPairs ? s.n.load() : kPairs;
  const uint32_t refused = s.refused.load();
  auto* rb = const_cast<uint64_t*>(g_mapped) + size_t(si) * kPairs * 2;
  double sum[kItems] = {}, overhead = -1;
  uint32_t cnt[kItems] = {};
  uint32_t bad = refused;  // items whose frame is incomplete
  uint64_t late = 0, late_item[kItems] = {}, rej_item[kItems] = {};
  for (uint32_t p = 0; p < n; ++p) {
    const uint64_t b = rb[p * 2], e = rb[p * 2 + 1];
    const uint8_t it = s.item[p];
    if (!b || !e) {
      ++late;
      if (it != kEmpty) ++late_item[it], bad |= 1u << it;
      continue;
    }
    const double ms = e >= b ? static_cast<double>(e - b) * g_ms_per_tick : -1.0;
    if (ms < 0 || ms > kMaxMs) {
      if (it != kEmpty) ++rej_item[it], bad |= 1u << it;
      continue;
    }
    if (it == kEmpty) {
      overhead = ms;
      continue;
    }
    sum[it] += ms;
    ++cnt[it];
  }
  std::memset(rb, 0, size_t(n) * 2 * sizeof(uint64_t));
  std::lock_guard g(g_mu);
  ++g_read;
  g_late += late;
  if (s.n.load() > kPairs) ++g_overflow;
  if (overhead >= 0) smooth(&g_overhead, overhead);
  for (uint32_t i = 0; i < kItems; ++i) {
    Item& t = g_item[i];
    t.late += late_item[i];
    t.rejected += rej_item[i];
    if (!cnt[i] || (bad & (1u << i))) continue;
    smooth(&t.ema, sum[i]);
    t.last = sum[i];
    t.per_frame = cnt[i];
    ++t.frames;
  }
}

uint32_t take_pair(Slot& s, uint8_t item) {
  const uint32_t p = s.n.fetch_add(1, std::memory_order_relaxed);
  if (p >= kPairs) {
    if (item != kEmpty) s.refused.fetch_or(1u << item, std::memory_order_relaxed);
    return UINT32_MAX;
  }
  s.item[p] = item;
  return p;
}

}  // namespace

void set_enabled(bool on) { g_want.store(on); }
bool enabled() { return g_want.load(); }

void frame_tick() {
  if (!g_want.load(std::memory_order_relaxed)) {
    if (g_active.exchange(false))
      for (Slot& s : g_slot) s.used = false;  // stale on the next switch-on
    return;
  }
  if (!g_ready && !g_failed) {
    g_ready = create();
    g_failed = !g_ready;  // once: no retry every frame
  }
  if (!g_ready) return;
  const uint32_t next = static_cast<uint32_t>(++g_frame % kRing);
  Slot& s = g_slot[next];
  if (s.used) read(next);
  s.n.store(0, std::memory_order_relaxed);
  s.refused.store(0, std::memory_order_relaxed);
  s.empty_done.store(false, std::memory_order_relaxed);
  s.used = true;
  g_cur.store(next, std::memory_order_release);
  g_active.store(true, std::memory_order_release);
}

uint32_t begin(ID3D12GraphicsCommandList* cl, Kind kind, int32_t handle) {
  if (!g_active.load(std::memory_order_acquire) || !cl || handle <= 0 || static_cast<uint32_t>(handle) > kPerKind)
    return 0;
  const D3D12_COMMAND_LIST_TYPE type = cl->GetType();
  if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_COMPUTE) return 0;
  const uint32_t si = g_cur.load(std::memory_order_acquire);
  Slot& s = g_slot[si];
  const uint32_t base = si * kPairs;
  if (!s.empty_done.exchange(true, std::memory_order_relaxed)) {  // the timing's own cost, once per frame
    const uint32_t p = take_pair(s, kEmpty);
    if (p != UINT32_MAX) {
      const UINT q = (base + p) * 2;
      cl->EndQuery(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, q);
      cl->EndQuery(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, q + 1);
      cl->ResolveQueryData(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, q, 2, g_rb, UINT64(q) * sizeof(uint64_t));
    }
  }
  const uint8_t item = static_cast<uint8_t>(kind * kPerKind + (handle - 1));
  const uint32_t p = take_pair(s, item);
  if (p == UINT32_MAX) return 0;
  const wchar_t* ev = g_item[item].event;
  cl->BeginEvent(0, ev, static_cast<UINT>((std::wcslen(ev) + 1) * sizeof(wchar_t)));
  cl->EndQuery(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, (base + p) * 2);
  return base + p + 1;
}

void end(ID3D12GraphicsCommandList* cl, uint32_t token) {
  const UINT q = (token - 1) * 2;
  cl->EndQuery(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, q + 1);
  cl->ResolveQueryData(g_heap, D3D12_QUERY_TYPE_TIMESTAMP, q, 2, g_rb, UINT64(q) * sizeof(uint64_t));
  cl->EndEvent();
}

std::string report() {
  if (!g_want.load()) return "";
  std::lock_guard g(g_mu);
  char line[320];
  std::snprintf(line, sizeof line, "gpu\t%llu\t%llu\t%llu\t%.4f\n", static_cast<unsigned long long>(g_read),
                static_cast<unsigned long long>(g_late), static_cast<unsigned long long>(g_overflow), g_overhead);
  std::string o = line;
  for (uint32_t i = 0; i < kItems; ++i) {
    const Item& t = g_item[i];
    if (!t.frames && !t.rejected && !t.late) continue;
    const uint32_t k = i / kPerKind;
    const rg_handle h = static_cast<rg_handle>(i % kPerKind + 1);
    std::snprintf(line, sizeof line, "item\t%s\t%s\t%s\t%.4f\t%.4f\t%u\t%llu\t%llu\t%llu\n", kKindName[k],
                  clients::client_of(kClientKind[k], h).c_str(), clients::name_of(kClientKind[k], h).c_str(), t.ema,
                  t.last, t.per_frame, static_cast<unsigned long long>(t.frames),
                  static_cast<unsigned long long>(t.rejected), static_cast<unsigned long long>(t.late));
    o += line;
  }
  return o;
}

}  // namespace core::gpu_timing
