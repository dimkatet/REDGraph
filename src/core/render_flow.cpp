// M5: the engine's render graph from the inside (docs/internals.md). One hook, CRenderNodeBase::Process: the node and
// its SRenderNodeImplContext on the recording thread, nested for group members. On it:
//   * current_node() - for graph_probe's census, M11, the debug skip;
//   * JOBS -> node (2026-10-08, render-graph.md "Next" 3): a node that hands work to a job WAITS for it (job::RunJob ->
//     Builder::Sync - e.g. AccelerationStructureUpdateDynamic's skinned-BVH job, our db18e315). Every job submitted
//     while a node (or a job carrying one) runs on the thread is WRAPPED at Dispatcher::RunJob: its descriptor goes
//     to the queue as {our thunk, a record {fn, data, node}}; the thunk sets the node for the job's run. Jobs a job
//     submits inherit it. Work no node submitted (CRenderInterface::FrameTick: surface cache, texture compression)
//     stays "(no node)". (Run 25 tried "the node owning the bound command list" instead: the job binds a flow-table
//     slot no node holds, and a context can be bound on two threads - FrameTick got a stale node. Dropped.)
//   * flow_texture() - an engine RenderFlow resource BY NAME: the name hash a node's code declares (t64 = 583c0f7f),
//     resolved the way the engine's own nodes do it - SRenderFlowInternalData::GetResource<TextureRef>(data, out,
//     hash ^ view << 24, ctx[0x38]) -> GpuApi texture id -> the texture container's ID3D12Resource*. It answers for the
//     node recording on this thread NOW (a dispatch subscription, an after-dispatch pass), at this point of the
//     frame's resource timeline: a transient (t64) is a different resource every frame and may be aliased after its
//     last consumer, so the pointer is good for this recording only.

#include <Windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include <RED4ext/CName.hpp>

#include "core/clients.hpp"
#include "core/engine_231.hpp"
#include "core/internal.hpp"
#include "core/prof.hpp"

namespace core::render_flow {
namespace {

using namespace e231;

using Process_t = void (*)(const void* node, const void* ctx, void* builder);
using GetResource_t = uint32_t* (*)(const void* data, uint32_t* out, const uint32_t* tag, uint8_t thread, bool b);
using HandleRequest_t = bool (*)(void* data, const void* request, uint8_t thread);
using GetCtx_t = void* (*)();
using Transition_t = void (*)(void* cl_ctx, uint32_t texture, uint32_t state, uint32_t subresource);
using Flush_t = void (*)(void* cl_ctx);
struct DescRange {
  ID3D12DescriptorHeap* heap;
  uint32_t offset, count;
};
using AllocateRange_t = DescRange* (*)(void* allocator, DescRange* out, uint32_t n);
using RestoreState_t = void (*)(ID3D12GraphicsCommandList* cl);

Process_t g_orig = nullptr;
GetResource_t g_get_resource = nullptr;
HandleRequest_t g_handle_request = nullptr;
GetCtx_t g_get_ctx = nullptr;
Transition_t g_transition = nullptr;
Flush_t g_flush = nullptr;
AllocateRange_t g_allocate_range = nullptr;
RestoreState_t g_restore_state = nullptr;
std::atomic<uint64_t> g_desc_allocs{0}, g_desc_fails{0};
std::atomic<bool> g_desc_logged{false};
std::atomic<uint64_t> g_desc_own_bind{0};
bool g_attached = false;
std::atomic<bool> g_want{false};
HandleRequest_t g_orig_request = nullptr;  // the detour's trampoline (g_handle_request = the hooked entry)
bool g_req_attached = false;
std::atomic<bool> g_want_req{false};
thread_local FlowRecord* t_rec = nullptr;
thread_local const void* t_node = nullptr;
thread_local const uint8_t* t_ctx = nullptr;

// ---- jobs carry the node that submitted them ----
struct JobDesc {
  void (*fn)(void* data, void* run_ctx);
  void* data;
  void* name;
  void* extra;
};
static_assert(sizeof(JobDesc) == 32);
using RunJob_t = uint64_t (*)(void* dispatcher, const JobDesc* desc, uint8_t priority, void* waiting, void* counter);
RunJob_t g_orig_runjob = nullptr;
bool g_job_attached = false;
struct JobRec {
  void (*fn)(void* data, void* run_ctx);
  void* data;
  const void* node;
  std::atomic<uint32_t> busy{0};
};
constexpr uint32_t kJobRecs = 65536;  // run 26: 4096 overflowed in a heavy window (all freed - in flight at once)
JobRec g_jobs[kJobRecs];
std::atomic<uint32_t> g_job_next{0};
std::atomic<uint64_t> g_job_wrapped{0}, g_job_ran{0}, g_job_full{0};
thread_local const void* t_job_node = nullptr;

void job_thunk(void* data, void* run_ctx) {
  prof::Scope prof_scope(prof::kJob);  // kJob = the submit (d_runjob) + this run
  auto* r = static_cast<JobRec*>(data);
  const auto fn = r->fn;
  void* const orig = r->data;
  const void* const node = r->node;
  r->busy.store(0, std::memory_order_release);  // each job runs once; its fields are copied
  g_job_ran.fetch_add(1, std::memory_order_relaxed);
  const void* const prev = t_job_node;
  t_job_node = node;
  {
    prof::Pause engine;
    fn(orig, run_ctx);
  }
  t_job_node = prev;
}

uint64_t d_runjob(void* dispatcher, const JobDesc* desc, uint8_t priority, void* waiting, void* counter) {
  prof::Scope prof_scope(prof::kJob);
  const void* node = t_node ? t_node : t_job_node;
  if (!node || !desc || !desc->fn) {
    prof::Pause engine;
    return g_orig_runjob(dispatcher, desc, priority, waiting, counter);
  }
  for (uint32_t tries = 0; tries < 64; ++tries) {
    JobRec& r = g_jobs[g_job_next.fetch_add(1, std::memory_order_relaxed) % kJobRecs];
    uint32_t free = 0;
    if (!r.busy.compare_exchange_strong(free, 1, std::memory_order_acquire)) continue;
    r.fn = desc->fn;
    r.data = desc->data;
    r.node = node;
    JobDesc d = *desc;
    d.fn = &job_thunk;
    d.data = &r;
    g_job_wrapped.fetch_add(1, std::memory_order_relaxed);
    prof::Pause engine;
    return g_orig_runjob(dispatcher, &d, priority, waiting, counter);
  }
  g_job_full.fetch_add(1, std::memory_order_relaxed);  // every record in flight: this job runs without its node
  prof::Pause engine;
  return g_orig_runjob(dispatcher, desc, priority, waiting, counter);
}

constexpr uint32_t kMaxRes = 16;
// origin: what the name is - one the engine's code declares (a 32-bit constant from a census), one a client PUBLISHES
// (node_alloc, "<mod>/<name>"), or one a client LOOKS UP by that published name (flow_register with hash 0).
enum Origin : uint8_t { kEngine, kPublished, kByName };
struct Res {
  const char* name;
  uint32_t hash;
  Origin origin;
  std::atomic<uint64_t> hits{0}, misses{0}, outside{0}, changes{0};
  std::atomic<uint32_t> last_id{0};
};
Res g_r[kMaxRes];
static_assert(FlowRecord::kSize == kFlowRequestSize);
std::atomic<uint32_t> g_nr{0};

template <class T>
T rd(const uint8_t* p, uintptr_t off) {
  T v;
  std::memcpy(&v, p + off, sizeof v);
  return v;
}

const uint8_t* flow_data(const uint8_t* ctx) {
  const auto* flow = ctx ? rd<const uint8_t*>(ctx, kNodeCtxFlow) : nullptr;
  return flow ? rd<const uint8_t*>(flow, 0) : nullptr;
}

// As RTAlloc builds a tag: view = ctx+0x34 unless ctx+0x30 bit 0; a 32-bit shift (only its low byte survives).
uint32_t tag_of(const uint8_t* ctx, uint32_t name_hash) {
  const uint32_t view = (rd<uint8_t>(ctx, kNodeCtxFlags) & 1) ? 0u : rd<uint32_t>(ctx, kNodeCtxView);
  return name_hash ^ (view << 24);
}

Res* res_of(rg_handle h) {
  return h > 0 && static_cast<uint32_t>(h) <= g_nr.load(std::memory_order_acquire) ? &g_r[h - 1] : nullptr;
}

// The one funnel of every RenderFlow request: the census sees it, a removal records its node's declaration run.
bool d_request(void* data, const void* request, uint8_t thread) {
  prof::Scope prof_scope(prof::kRequest);
  const auto* req = static_cast<const uint8_t*>(request);
  if (req && graph_probe::census()) graph_probe::on_request(data, req);
  if (FlowRecord* r = t_rec; r && req) {
    if (r->n < FlowRecord::kMax) {
      std::memcpy(r->req[r->n], req, FlowRecord::kSize);
      r->thread[r->n] = thread;
      ++r->n;
    } else {
      r->overflow = true;
    }
  }
  prof::Pause engine;
  return g_orig_request(data, request, thread);
}

void d_process(const void* node, const void* ctx, void* builder) {
  prof::Scope prof_scope(prof::kProcess);
  const void* prev_node = t_node;
  const uint8_t* prev_ctx = t_ctx;
  t_node = node;
  t_ctx = static_cast<const uint8_t*>(ctx);
  {
    prof::Pause engine;
    g_orig(node, ctx, builder);
  }
  t_node = prev_node;
  t_ctx = prev_ctx;
}

// GpuApi texture id -> ID3D12Resource*, from the texture container (docs/internals.md "Resources by name (M5)"): 32768 handles of 0xB0
// after a 0x10 header, refCount at +0 (< 0 = unused), the resource at +8.
void* texture_resource(uint32_t id) {
  if (id == 0 || id > kTexSlots) return nullptr;
  const auto* dev = *reinterpret_cast<const uint8_t* const*>(g_base + kDeviceData);
  if (!dev) return nullptr;
  const uint8_t* h = dev + kTexContainer + kTexHandles + size_t(id - 1) * kTexStride;
  if (rd<int32_t>(h, 0) < 0) return nullptr;
  return rd<void*>(h, kTexResource);
}

}  // namespace

void want() { g_want.store(true); }
bool attached() { return g_attached; }
bool declaring(const void* ctx) {
  const uint8_t* data = flow_data(static_cast<const uint8_t*>(ctx));
  return data && data[kFlowPhase] == kFlowPhaseDeclare;
}
const void* current_node() { return t_node ? t_node : t_job_node; }
bool current_is_job() { return !t_node && t_job_node; }
void job_stats(uint64_t* wrapped, uint64_t* ran, uint64_t* full) {
  *wrapped = g_job_wrapped.load();
  *ran = g_job_ran.load();
  *full = g_job_full.load();
}

uint64_t node_name_hash(const void* node) {
  const auto* vt = node ? *static_cast<const void* const* const*>(node) : nullptr;
  if (!vt) return 0;
  RED4ext::CName cn;
  reinterpret_cast<RED4ext::CName* (*)(const void*, RED4ext::CName*)>(vt[kNodeVtGetName])(node, &cn);
  return cn.hash;
}

bool attach() {
  if (!g_want.load()) return false;
  g_get_resource = reinterpret_cast<GetResource_t>(g_base + kFlowGetTexture);
  g_handle_request = reinterpret_cast<HandleRequest_t>(g_base + kFlowHandleRequest);
  g_get_ctx = reinterpret_cast<GetCtx_t>(g_base + kGetCommandListContext);
  g_transition = reinterpret_cast<Transition_t>(g_base + kTransitionTexture);
  g_flush = reinterpret_cast<Flush_t>(g_base + kFlushPendingBarriers);
  g_allocate_range = reinterpret_cast<AllocateRange_t>(g_base + kAllocateRange);
  g_restore_state = reinterpret_cast<RestoreState_t>(g_base + kRestoreState);
  g_attached = attach_hook(kNodeProcess, kGroupProcessRet, reinterpret_cast<void*>(&d_process),
                           reinterpret_cast<void**>(&g_orig), "CRenderNodeBase::Process");
  if (g_attached)
    g_job_attached = attach_hook(kDispatcherRunJob, kJobRunJobRet, reinterpret_cast<void*>(&d_runjob),
                                 reinterpret_cast<void**>(&g_orig_runjob), "job::Dispatcher::RunJob");
  if (g_attached && (g_want_req.load() || graph_probe::census()))
    g_req_attached = attach_hook(kFlowHandleRequest, kFlowAccessRet, reinterpret_cast<void*>(&d_request),
                                 reinterpret_cast<void**>(&g_orig_request), "SRenderFlowInternalData::HandleRequest");
  // A name looked up that nobody publishes: its flow_texture stays NULL and the engine ignores its node_use.
  for (uint32_t i = 0; i < g_nr.load(); ++i) {
    if (g_r[i].origin != kByName) continue;
    bool found = false;
    for (uint32_t j = 0; j < g_nr.load() && !found; ++j) found = g_r[j].origin == kPublished && g_r[j].hash == g_r[i].hash;
    if (!found)
      g_sdk->logger->WarnF(g_handle, "render flow: '%s' is looked up by name, but no client publishes it (node_alloc) - "
                           "it never resolves", g_r[i].name);
  }
  g_sdk->logger->InfoF(g_handle, "render flow: node context %s%s (%u resource(s) by name)",
                       g_attached ? "ATTACHED (CRenderNodeBase::Process)" : "NOT attached",
                       !g_attached ? "" : g_job_attached ? " + jobs carry their node (job::Dispatcher::RunJob)"
                                                         : " - jobs NOT attributed (Dispatcher::RunJob not attached)",
                       g_nr.load());
  return g_attached;
}

void detach() {
  if (g_req_attached) detach_hook(kFlowHandleRequest);
  g_req_attached = false;
  if (g_job_attached) detach_hook(kDispatcherRunJob);  // records still in flight finish through the thunk
  g_job_attached = false;
  if (g_attached) detach_hook(kNodeProcess);
  g_attached = false;
}

// The module an interface's vtable lives in - "dxgi.dll" there = ReShade's proxy wraps that object.
void module_of_vtable(const void* object, char* out, DWORD size) {
  out[0] = 0;
  HMODULE m = nullptr;
  const void* vt = object ? *static_cast<const void* const*>(object) : nullptr;
  if (vt && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(vt), &m)) {
    char path[MAX_PATH];
    const DWORD n = GetModuleFileNameA(m, path, MAX_PATH);
    const char* base = path;
    for (DWORD i = 0; i < n; ++i)
      if (path[i] == '\\' || path[i] == '/') base = path + i + 1;
    lstrcpynA(out, base, static_cast<int>(size));
  }
}

// M10: as GpuApi::ClearBufferUAV_Zero does - a range from the engine's ring, addressed in its heap with the frame
// resources' descriptor size. The heap must be BOUND on the list: a list the engine has not drawn on yet has none (run
// 18: the driver AV'd on SetGraphicsRootDescriptorTable in Transparents with the clouds - its first drawing node -
// removed). So first the engine's own Hacks::RestoreState: its root signatures (Ensure*RootSignature), then
// SFrameResources::BindDescriptorHeaps (SetDescriptorHeaps + the engine root signature's static tables) - hence the
// client sets ITS root signature only after this call.
uint32_t gpu_descriptors(uint32_t count, rg_descriptors* out) {
  if (out) *out = {};
  if (!out || !count || !g_allocate_range) return 0;
  const auto* lctx = static_cast<const uint8_t*>(g_get_ctx());
  const auto* frame = lctx ? rd<const uint8_t*>(lctx, kCtxFrameRes) : nullptr;
  auto* cl = lctx ? rd<ID3D12GraphicsCommandList*>(lctx, kCtxCmdList) : nullptr;
  auto* dev = *reinterpret_cast<uint8_t* const*>(g_base + kDeviceData);
  if (!frame || !cl || !dev) {
    g_desc_fails.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  DescRange r{};
  g_allocate_range(dev + kDescRangeAllocator, &r, count);
  // The heap must be the ring's (allocator+0x38), shader-visible, and BOUND on this list. SFrameResources[0] is the
  // heap the engine has bound for this context - NULL until the engine's first draw / dispatch on the list (run 20:
  // NULL in Transparents with the clouds removed; BindDescriptorHeaps then returns without binding anything - why
  // runs 18 / 19 AV'd in the driver on a table into nothing). Then we bind what BindDescriptorHeaps would: the ring
  // heap + the sampler heap (allocator+0x60), leaving SFrameResources[0] NULL - the engine still does its own full
  // bind (with its static tables) at its first draw. Otherwise RestoreState (the engine's root signatures + heaps).
  auto* bound = rd<ID3D12DescriptorHeap*>(frame, 0);
  auto* ring_heap = rd<ID3D12DescriptorHeap*>(dev + kDescRangeAllocator, kDescRingHeap);
  auto* sampler_heap = rd<ID3D12DescriptorHeap*>(dev + kDescRangeAllocator, kDescSamplerHeap);
  const D3D12_DESCRIPTOR_HEAP_DESC hd = r.heap ? r.heap->GetDesc() : D3D12_DESCRIPTOR_HEAP_DESC{};
  const bool ok = r.heap && r.heap == ring_heap && (!bound || bound == ring_heap) && sampler_heap &&
                  hd.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
                  (hd.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) && r.offset + count <= hd.NumDescriptors;
  if (!g_desc_logged.exchange(true)) {
    char heap_mod[64], cl_mod[64];
    module_of_vtable(r.heap, heap_mod, sizeof heap_mod);
    module_of_vtable(cl, cl_mod, sizeof cl_mod);
    g_sdk->logger->InfoF(
        g_handle,
        "render flow: engine descriptor range #1 - range heap %p (type %u, flags 0x%x, %u descriptors), offset %u, "
        "count %u | SFrameResources[0] %p, allocator+0x38 %p, allocator+0x60 (samplers) %p | range heap CPU start "
        "0x%llx GPU start 0x%llx, increment %u | vtables: heap in %s, command list in %s | %s",
        static_cast<void*>(r.heap), static_cast<unsigned>(hd.Type), static_cast<unsigned>(hd.Flags),
        hd.NumDescriptors, r.offset, r.count, static_cast<void*>(bound), static_cast<void*>(ring_heap),
        static_cast<void*>(sampler_heap),
        r.heap ? static_cast<unsigned long long>(r.heap->GetCPUDescriptorHandleForHeapStart().ptr) : 0ull,
        r.heap ? static_cast<unsigned long long>(r.heap->GetGPUDescriptorHandleForHeapStart().ptr) : 0ull,
        rd<uint32_t>(frame, kFrameResDescIncrement), heap_mod, cl_mod,
        ok ? (bound ? "HANDED OUT (heaps bound by the engine)" : "HANDED OUT (no heap bound yet - we bind ring + samplers)")
           : "REFUSED - not the ring's shader-visible heap");
  }
  if (!ok) {
    g_desc_fails.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  if (bound) {
    g_restore_state(cl);  // the engine's root signatures, heaps and static tables
  } else {
    ID3D12DescriptorHeap* heaps[2] = {ring_heap, sampler_heap};
    cl->SetDescriptorHeaps(2, heaps);
    g_desc_own_bind.fetch_add(1, std::memory_order_relaxed);
  }
  const uint32_t inc = rd<uint32_t>(frame, kFrameResDescIncrement);
  out->cpu = r.heap->GetCPUDescriptorHandleForHeapStart().ptr + uint64_t(r.offset) * inc;
  out->gpu = r.heap->GetGPUDescriptorHandleForHeapStart().ptr + uint64_t(r.offset) * inc;
  out->increment = inc;
  out->count = count;
  g_desc_allocs.fetch_add(1, std::memory_order_relaxed);
  return 1;
}

void want_requests() {
  g_want_req.store(true);
  g_want.store(true);
}
bool requests_attached() { return g_req_attached; }
void record_into(FlowRecord* r) { t_rec = r; }

// The recorded requests, raw, through the engine's own entry (the census still sees them as phase 2).
void replay(const void* node_ctx, const FlowRecord& r) {
  auto* data = const_cast<uint8_t*>(flow_data(static_cast<const uint8_t*>(node_ctx)));
  if (!data) return;
  alignas(8) uint8_t req[FlowRecord::kSize];
  for (uint32_t i = 0; i < r.n; ++i) {
    std::memcpy(req, r.req[i], sizeof req);
    g_handle_request(data, req, r.thread[i]);
  }
}

namespace {
rg_handle add_res(const char* name, uint32_t hash, Origin origin) {
  const uint32_t n = g_nr.load();
  if (n >= kMaxRes) return RG_E_FULL;
  g_r[n].name = name ? name : "?";
  g_r[n].hash = hash;
  g_r[n].origin = origin;
  g_nr.store(n + 1, std::memory_order_release);
  want();
  return static_cast<rg_handle>(n + 1);
}
}  // namespace

uint32_t client_name_hash(const char* name) {
  // "<mod>/<name>": both parts non-empty, printable ASCII, no blanks.
  const char* slash = name ? std::strchr(name, '/') : nullptr;
  if (!slash || slash == name || !slash[1]) return 0;
  uint32_t hash = 0x811c9dc5u;  // FNV-1a 32 (the engine's names are 32-bit constants)
  for (const char* c = name; *c; ++c) {
    if (*c <= ' ' || *c > '~') return 0;
    hash = (hash ^ static_cast<uint8_t>(*c)) * 0x01000193u;
  }
  return hash == 0xdd5f1d0au ? 0u : hash;  // 0xdd5f1d0a is the engine's "nameless" marker
}

rg_handle flow_register(const char* name, uint32_t name_hash) {
  if (!registration_open()) return RG_E_LATE;
  if (name_hash) return add_res(name, name_hash, kEngine);
  const uint32_t h = client_name_hash(name);
  if (!h) {
    clients::reason("a published name is \"<mod>/<name>\" (hash 0 = look it up by that name)");
    return RG_E_ARG;
  }
  return add_res(name, h, kByName);
}

rg_handle flow_publish(const char* name, uint32_t hash) {
  const uint32_t n = g_nr.load();
  for (uint32_t i = 0; i < n; ++i)
    if (g_r[i].origin == kPublished && g_r[i].hash == hash) {
      char why[160];
      std::snprintf(why, sizeof why, "the name '%s' is already published (by '%s' - one publisher per name)", name,
                    g_r[i].name);
      clients::reason(why);
      return RG_E_TAKEN;
    }
  return add_res(name, hash, kPublished);
}

uint32_t flow_hash(rg_handle h) {
  const Res* r = res_of(h);
  return r ? r->hash : 0u;
}

// The engine's RTAlloc / RTUseBegin / RTUseEnd in one: an SRequest on the stack, handed to HandleRequest with the
// node context's flow data and thread. In the declaration run it is recorded; in the real run it is matched to that
// record by its index within the thread and gets the aliasing barrier / discard / retire Resolve placed on it.
void request(const void* node_ctx, uint8_t type, uint32_t name_hash, const void* payload, uint32_t size) {
  const auto* ctx = static_cast<const uint8_t*>(node_ctx);
  auto* data = const_cast<uint8_t*>(flow_data(ctx));
  if (!data || !g_handle_request) return;
  alignas(8) uint8_t req[kFlowRequestSize] = {};
  req[0] = type;
  req[1] = rd<uint8_t>(ctx, kNodeCtxFlowThread);
  const uint32_t tag = tag_of(ctx, name_hash);
  std::memcpy(req + 4, &tag, 4);
  if (payload) std::memcpy(req + kFlowRequestPayload, payload, size < kFlowDescSize ? size : kFlowDescSize);
  g_handle_request(data, req, req[1]);
}

namespace {
// The texture id the node recording on this thread sees for r now; 0 outside a node / in the declaration pass /
// not alive. Counts the outcome.
uint32_t resolve(Res* r) {
  const uint8_t* ctx = t_ctx;
  const uint8_t* data = flow_data(ctx);
  // Outside a node, or in the declaration pass (the graph's first run of the frame: nothing is recorded).
  if (!g_attached || !data || data[kFlowPhase] == kFlowPhaseDeclare) {
    r->outside.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  const uint32_t tag = tag_of(ctx, r->hash);
  uint32_t id = 0;
  g_get_resource(data, &id, &tag, rd<uint8_t>(ctx, kNodeCtxFlowThread), false);
  if (!texture_resource(id)) {
    r->misses.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  r->hits.fetch_add(1, std::memory_order_relaxed);
  if (r->last_id.exchange(id, std::memory_order_relaxed) != id) r->changes.fetch_add(1, std::memory_order_relaxed);
  return id;
}
}  // namespace

void* flow_texture(rg_handle h, uint32_t* gpuapi_id) {
  if (gpuapi_id) *gpuapi_id = 0;
  Res* r = res_of(h);
  const uint32_t id = r ? resolve(r) : 0;
  if (!id) return nullptr;
  if (gpuapi_id) *gpuapi_id = id;
  return texture_resource(id);
}

// Through the engine's own state tracker, so its next bind of the texture starts from the state we left: the
// transition is recorded on the thread's command list context, flushed at once.
uint32_t flow_transition(rg_handle h, uint32_t d3d12_state) {
  Res* r = res_of(h);
  const uint32_t id = r ? resolve(r) : 0;
  void* cl_ctx = id ? g_get_ctx() : nullptr;
  if (!cl_ctx) return 0;
  g_transition(cl_ctx, id, d3d12_state, 0xffffffffu);
  g_flush(cl_ctx);
  return 1;
}

void flow_stats(rg_handle h, rg_flow_stats* out) {
  if (!out) return;
  *out = {};
  Res* r = res_of(h);
  if (!r) return;
  out->attached = g_attached ? 1u : 0u;
  out->last_id = r->last_id.load();
  out->hits = r->hits.load();
  out->misses = r->misses.load();
  out->outside = r->outside.load();
  out->id_changes = r->changes.load();
}

}  // namespace core::render_flow
