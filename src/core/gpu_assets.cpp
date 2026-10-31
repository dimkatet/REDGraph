// A client's own assets in VRAM (rg_host::buffer_create / buffer_ready / retire / vram_stats, 2026-10-31).
//
// The host's FENCE: one ID3D12Fence, signalled on the engine's direct queue at the start of every graph execution
// (frame_tick, from the frame hook - prof.cpp) with the host's frame number. Before the signal, the copies of the
// buffers created since the last frame (up to kCopyBudget) go in one command list of ours, submitted on that queue - so
// every list the engine submits for this frame comes after them on the queue, and the fence says when they are done
// (buffer_ready: any queue may read the buffer then).
//
// RETIRE: a resource retired while frame F is recorded is Released once the fence passed F + 2. Frame F's lists are
// submitted before frame F + 1's graph runs (the signal F + 1 follows them on the queue); the second frame is the
// margin for an engine list submitted late (a submission job) - never seen, not ruled out.
//
// Buffers are created in COMMON and need no barriers: a buffer is promoted implicitly by its first use on a list
// (copy dest here, a shader read / UAV on the client's list) and decays to COMMON when a list ends.

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/gpu_assets.hpp"

#include "core/engine_231.hpp"
#include "core/internal.hpp"

namespace core::gpu_assets {
namespace {

using namespace e231;

constexpr uint64_t kCopyBudget = 64ull << 20;  // bytes copied per frame (at least one buffer)
constexpr uint32_t kAllocators = 3;            // command allocators in rotation, each reused once its frame passed
constexpr uint64_t kRetireFrames = 2;          // a retired resource waits for the fence to pass its frame + this

struct Buf {
  uint64_t bytes = 0;
  HMODULE owner = nullptr;
  ID3D12Resource* staging = nullptr;   // until the copy is submitted
  uint64_t copied = UINT64_MAX;        // the frame whose signal follows its copy (UINT64_MAX = not submitted)
};
struct Copy {
  ID3D12Resource* dst;
  ID3D12Resource* staging;
  uint64_t bytes;
};
struct Dead {
  IUnknown* r;
  uint64_t after;  // Released once the fence completed this
};

std::mutex g_mu;  // everything below but the D3D objects of the frame thread
std::unordered_map<ID3D12Resource*, Buf> g_bufs;
std::deque<Copy> g_copies;
std::vector<Dead> g_dead;
uint64_t g_bytes = 0, g_pending_bytes = 0;
std::atomic<uint64_t> g_frame{0};

// The frame thread's (frame_tick)
ID3D12Fence* g_fence = nullptr;
ID3D12CommandAllocator* g_alloc[kAllocators] = {};
uint64_t g_alloc_frame[kAllocators] = {};
ID3D12GraphicsCommandList* g_list = nullptr;
bool g_init_failed = false;
std::atomic<uint64_t> g_completed{0};
uint64_t g_lists = 0, g_copied_bytes = 0, g_stalls = 0;

std::atomic<uint32_t> g_fail_logs{0};

ID3D12Device* device() {
  const auto* data = *reinterpret_cast<const uint8_t* const*>(g_base + kDeviceData);
  auto* engine_dev = data ? *reinterpret_cast<IUnknown* const*>(data + kDeviceInData) : nullptr;
  ID3D12Device* dev = nullptr;
  if (!engine_dev || FAILED(engine_dev->QueryInterface(IID_PPV_ARGS(&dev)))) return nullptr;
  dev->Release();  // the engine holds it for the process lifetime
  return dev;
}

ID3D12CommandQueue* queue() {
  const auto* data = *reinterpret_cast<const uint8_t* const*>(g_base + kDeviceData);
  return data ? *reinterpret_cast<ID3D12CommandQueue* const*>(data + kDirectQueueInData) : nullptr;
}

HMODULE module_of(const void* addr) {
  HMODULE m = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     static_cast<LPCWSTR>(addr), &m);
  return m;
}

void fail(const char* what, HRESULT hr, uint64_t bytes) {
  if (g_fail_logs.fetch_add(1) < 16)
    g_sdk->logger->ErrorF(g_handle, "gpu assets: %s FAILED (hr 0x%08x, %llu B)", what, static_cast<unsigned>(hr),
                          static_cast<unsigned long long>(bytes));
}

bool init() {
  if (g_list) return true;
  if (g_init_failed) return false;
  ID3D12Device* dev = device();
  if (!dev || !queue()) return false;  // not yet: the next frame
  HRESULT hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
  for (uint32_t i = 0; SUCCEEDED(hr) && i < kAllocators; ++i)
    hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc[i]));
  if (SUCCEEDED(hr))
    hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, IID_PPV_ARGS(&g_list));
  if (SUCCEEDED(hr)) hr = g_list->Close();
  if (FAILED(hr)) {
    g_init_failed = true;
    g_sdk->logger->ErrorF(g_handle, "gpu assets: fence / command list FAILED (hr 0x%08x) - buffer_create copies and "
                                    "retire are off this session", static_cast<unsigned>(hr));
    return false;
  }
  if (g_fence) g_fence->SetName(L"REDGraph frame fence");
  if (g_list) g_list->SetName(L"REDGraph uploads");
  g_sdk->logger->Info(g_handle, "gpu assets: frame fence and upload list on the engine's direct queue");
  return true;
}

// Released: every dead resource whose fence value passed.
void reap(uint64_t completed) {
  std::vector<IUnknown*> go;
  {
    std::lock_guard<std::mutex> g(g_mu);
    size_t k = 0;
    for (const Dead& d : g_dead)
      if (d.after <= completed) go.push_back(d.r);
      else g_dead[k++] = d;
    g_dead.resize(k);
  }
  for (IUnknown* r : go) r->Release();
}

}  // namespace

void* buffer_create(uint64_t bytes, uint32_t flags, const void* data, const void* caller) {
  ID3D12Device* dev = device();
  if (!dev || !bytes) {
    fail(dev ? "buffer_create (0 bytes)" : "buffer_create (no device yet)", E_INVALIDARG, bytes);
    return nullptr;
  }
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = bytes;
  d.Height = 1, d.DepthOrArraySize = 1, d.MipLevels = 1, d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  d.Flags = (flags & RG_BUFFER_UAV) ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
  const D3D12_HEAP_PROPERTIES hd{D3D12_HEAP_TYPE_DEFAULT}, hu{D3D12_HEAP_TYPE_UPLOAD};
  ID3D12Resource *res = nullptr, *staging = nullptr;
  HRESULT hr = dev->CreateCommittedResource(&hd, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&res));
  if (FAILED(hr)) {
    fail("buffer_create (DEFAULT)", hr, bytes);
    return nullptr;
  }
  if (data) {
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    void* p = nullptr;
    hr = dev->CreateCommittedResource(&hu, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                      IID_PPV_ARGS(&staging));
    if (SUCCEEDED(hr)) hr = staging->Map(0, nullptr, &p);
    if (FAILED(hr)) {
      fail("buffer_create (staging)", hr, bytes);
      if (staging) staging->Release();
      res->Release();
      return nullptr;
    }
    std::memcpy(p, data, bytes);
    staging->Unmap(0, nullptr);
  }
  Buf b;
  b.bytes = bytes, b.owner = module_of(caller), b.staging = staging;
  if (!data) b.copied = 0;
  std::lock_guard<std::mutex> g(g_mu);
  g_bufs[res] = b;
  g_bytes += bytes;
  if (data) {
    g_copies.push_back({res, staging, bytes});
    g_pending_bytes += bytes;
  }
  return res;
}

int32_t buffer_ready(void* resource) {
  uint64_t copied;
  {
    std::lock_guard<std::mutex> g(g_mu);
    const auto it = g_bufs.find(static_cast<ID3D12Resource*>(resource));
    if (it == g_bufs.end()) return RG_E_ARG;
    copied = it->second.copied;
  }
  if (copied == 0) return 1;
  if (copied == UINT64_MAX || !g_fence) return 0;
  if (copied <= g_completed.load(std::memory_order_relaxed)) return 1;
  const uint64_t c = g_fence->GetCompletedValue();
  if (c != UINT64_MAX) g_completed.store(c, std::memory_order_relaxed);  // UINT64_MAX: the device is removed
  return copied <= c && c != UINT64_MAX ? 1 : 0;
}

void retire(void* resource) {
  if (!resource) return;
  auto* r = static_cast<IUnknown*>(resource);
  const uint64_t after = g_frame.load() + kRetireFrames;
  std::lock_guard<std::mutex> g(g_mu);
  const auto it = g_bufs.find(static_cast<ID3D12Resource*>(resource));
  if (it != g_bufs.end()) {
    g_bytes -= it->second.bytes;
    if (it->second.copied == UINT64_MAX) {  // its copy never went: out of the queue, its staging dies with it
      for (auto c = g_copies.begin(); c != g_copies.end(); ++c)
        if (c->dst == it->first) {
          g_pending_bytes -= c->bytes;
          g_dead.push_back({c->staging, after});
          g_copies.erase(c);
          break;
        }
    }
    g_bufs.erase(it);
  }
  g_dead.push_back({r, after});
}

int32_t vram_stats(rg_vram_stats* out, const void* caller) {
  if (!out || out->struct_size < sizeof(rg_vram_stats)) return RG_E_ARG;
  const uint32_t size = out->struct_size;
  std::memset(out, 0, sizeof(rg_vram_stats));
  out->struct_size = size;
  // DXGI: the engine's adapter by its LUID; dxgi.dll is the game's (a proxy, if one is installed) - not imported by
  // REDGraph.dll, which loads before the game's imports are settled
  if (ID3D12Device* dev = device()) {
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    static auto create = reinterpret_cast<CreateFactory>(
        GetModuleHandleW(L"dxgi.dll") ? GetProcAddress(GetModuleHandleW(L"dxgi.dll"), "CreateDXGIFactory1") : nullptr);
    IDXGIFactory4* f = nullptr;
    IDXGIAdapter3* a = nullptr;
    if (create && SUCCEEDED(create(IID_PPV_ARGS(&f))) &&
        SUCCEEDED(f->EnumAdapterByLuid(dev->GetAdapterLuid(), IID_PPV_ARGS(&a)))) {
      DXGI_QUERY_VIDEO_MEMORY_INFO m{};
      if (SUCCEEDED(a->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &m)))
        out->local_budget = m.Budget, out->local_usage = m.CurrentUsage;
      if (SUCCEEDED(a->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &m)))
        out->nonlocal_budget = m.Budget, out->nonlocal_usage = m.CurrentUsage;
    }
    if (a) a->Release();
    if (f) f->Release();
  }
  const HMODULE me = module_of(caller);
  std::lock_guard<std::mutex> g(g_mu);
  out->buffers = static_cast<uint32_t>(g_bufs.size());
  out->bytes = g_bytes;
  for (const auto& [r, b] : g_bufs)
    if (b.owner == me) ++out->own_buffers, out->own_bytes += b.bytes;
  out->uploads_pending = static_cast<uint32_t>(g_copies.size());
  out->upload_pending_bytes = g_pending_bytes;
  out->retire_pending = static_cast<uint32_t>(g_dead.size());
  out->frame = g_frame.load();
  out->fence_completed = g_completed.load();
  return 0;
}

void frame_tick() {
  const uint64_t frame = g_frame.fetch_add(1) + 1;
  if (!init()) return;
  ID3D12CommandQueue* q = queue();
  if (!q) return;
  const uint64_t completed = g_fence->GetCompletedValue();
  if (completed == UINT64_MAX) return;  // the device is removed
  g_completed.store(completed, std::memory_order_relaxed);
  // this frame's copies, within the budget
  std::vector<Copy> batch;
  const uint32_t slot = static_cast<uint32_t>(frame % kAllocators);
  {
    std::lock_guard<std::mutex> g(g_mu);
    if (!g_copies.empty() && g_alloc_frame[slot] > completed) {
      ++g_stalls;  // the GPU is kAllocators frames behind: the copies wait a frame
    } else {
      uint64_t sum = 0;
      while (!g_copies.empty() && (batch.empty() || sum + g_copies.front().bytes <= kCopyBudget)) {
        batch.push_back(g_copies.front());
        sum += g_copies.front().bytes;
        g_pending_bytes -= g_copies.front().bytes;
        g_copies.pop_front();
      }
    }
  }
  if (!batch.empty()) {
    HRESULT hr = g_alloc[slot]->Reset();
    if (SUCCEEDED(hr)) hr = g_list->Reset(g_alloc[slot], nullptr);
    if (SUCCEEDED(hr)) {
      for (const Copy& c : batch) g_list->CopyBufferRegion(c.dst, 0, c.staging, 0, c.bytes);
      hr = g_list->Close();
    }
    if (FAILED(hr)) {
      fail("upload list", hr, 0);
      std::lock_guard<std::mutex> g(g_mu);  // back to the queue, tried again next frame
      for (auto it = batch.rbegin(); it != batch.rend(); ++it) {
        g_copies.push_front(*it);
        g_pending_bytes += it->bytes;
      }
    } else {
      ID3D12CommandList* lists[] = {g_list};
      q->ExecuteCommandLists(1, lists);
      g_alloc_frame[slot] = frame;
      ++g_lists;
      std::lock_guard<std::mutex> g(g_mu);
      for (const Copy& c : batch) {
        g_copied_bytes += c.bytes;
        g_dead.push_back({c.staging, frame});
        const auto it = g_bufs.find(c.dst);
        if (it != g_bufs.end()) it->second.copied = frame, it->second.staging = nullptr;
      }
      if (verbose())
        g_sdk->logger->InfoF(g_handle, "gpu assets: frame %llu - %zu copies submitted (%llu B so far, %llu lists)",
                             static_cast<unsigned long long>(frame), batch.size(),
                             static_cast<unsigned long long>(g_copied_bytes), static_cast<unsigned long long>(g_lists));
    }
  }
  q->Signal(g_fence, frame);
  reap(completed);
}

std::string summary() {
  std::lock_guard<std::mutex> g(g_mu);
  char t[256];
  std::snprintf(t, sizeof t,
                "gpu assets: %zu buffers %.1f MB, %zu copies pending, %zu retired waiting; frame %llu, fence %llu, "
                "%llu upload lists (%.1f MB), %llu stalls",
                g_bufs.size(), g_bytes / 1048576.0, g_copies.size(), g_dead.size(),
                static_cast<unsigned long long>(g_frame.load()), static_cast<unsigned long long>(g_completed.load()),
                static_cast<unsigned long long>(g_lists), g_copied_bytes / 1048576.0,
                static_cast<unsigned long long>(g_stalls));
  return t;
}

}  // namespace core::gpu_assets
