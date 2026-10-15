#pragma once

// Between the core's modules only - never included from settings.cpp, ui.cpp or a client (they see rg_host.h).

#include <cstdint>
#include <string>
#include <vector>

#include <RED4ext/Api/v1/PluginHandle.hpp>
#include <RED4ext/Api/v1/Sdk.hpp>

#include "rg_host.h"

struct ID3D12PipelineState;

namespace core {

// Patches (pso_patch.cpp) = the patch handles bindings keeps a lease list for (bindings.cpp).
constexpr uint32_t kMaxPatchCount = 256;

// host.cpp
extern RED4ext::v1::PluginHandle g_handle;
extern const RED4ext::v1::Sdk* g_sdk;
extern uintptr_t g_base;
bool registration_open();
// [Diagnostics] VerboseLog: the per-object detail (RG_LOG_DEBUG) is written.
bool verbose();
// Attach one detour at base + rva after its call-site check. false (and a log line naming `what`) on failure.
bool attach_hook(uintptr_t rva, uintptr_t ret_rva, void* detour, void** orig, const char* what);
void detach_hook(uintptr_t rva);
// M4c: the device_ready callbacks - fired once, from pso's first Create detour.
bool device_ready_wanted();
void fire_device_ready();

// pso_patch.cpp - M1 and the companions' PSOs
namespace pso {
rg_handle patch_register(const rg_patch_desc* d);
void patch_select(rg_handle h, int32_t variant);
void patch_stats(rg_handle h, rg_patch_stats* out);
int32_t patch_scope(rg_handle h, const char* node);  // M11
int32_t patch_state(rg_handle h, uint32_t variant, const rg_pso_state* s);  // M1: pipeline state beyond bytecode
rg_handle companion_register(const char* name, const uint8_t* hash16, rg_blob cs);
void companion_stats(rg_handle h, rg_patch_stats* out);
int32_t companion_scope(rg_handle h, const char* node);  // M11 for a companion
bool needed();
bool has_companions();
bool attach();
void detach();
bool attached();
uint32_t claimed(const uint8_t* hash16);
void host_stats(rg_host_stats* out);
// Once per frame (the frame hook, prof.cpp): every 64 frames, drop the entries of engine PSOs the engine released.
void frame_tick();
// For bindings: the patch's blobs / stage, and whether its vanilla check passed.
bool patch_info(rg_handle h, uint32_t* stage, const rg_blob** variants, uint32_t* count);
void patch_refuse(rg_handle h, const char* why);
bool patch_selected(rg_handle h);  // variant >= 0 and not refused
// For passes: the companion PSO of engine PSO `engine` if it is companion `h`'s, else nullptr. Counts a dispatch.
ID3D12PipelineState* companion_of(ID3D12PipelineState* engine, rg_handle h);
// For passes: false = the companion is scoped and this thread records outside its node (counted, logged once).
bool companion_in_scope(rg_handle h);
void companion_count(rg_handle h);
// For passes (M4c): the subscription mask of the engine PSO the bind funnel served on this thread since the reset -
// reset before the engine's DispatchCompute, taken after it (0 = none of ours).
void bound_subs_reset();
uint32_t bound_subs_take();
// For bindings: the companion's blob; refuse = never mapped / dispatched from now on.
bool companion_info(rg_handle h, rg_blob* cs);
void companion_refuse(rg_handle h, const char* why);
// The graphics wing of M4: draw companions, and which PSO drew on this thread.
rg_handle draw_companion_register(const char* name, uint32_t stage, const uint8_t* hash16, rg_blob shader,
                                  const rg_pso_state* state);
int32_t draw_companion_scope(rg_handle h, const char* node);
void draw_companion_stats(rg_handle h, rg_patch_stats* out);
// [hot] false = the last graphics bind on this thread carries no draw subscription (the draw hooks stop here).
bool draw_subs_pending();
// The draw subscriptions of the draw just recorded on context `ctx` (0 = none / skipped / another PSO), with the
// engine PSO and the PSO the engine drew with (a clone when a patch swapped it).
uint32_t draw_bound(const uint8_t* ctx, ID3D12PipelineState** engine, ID3D12PipelineState** bound);
// Companion `h`'s PSO for engine PSO `engine`, nullptr = not built for it / build failed / out of its scope.
ID3D12PipelineState* draw_companion_of(ID3D12PipelineState* engine, rg_handle h);
void draw_companion_count(rg_handle h);
}  // namespace pso

// bindings.cpp - M2 leases + the host's cbuffers
namespace bindings {
rg_handle cbuffer_create(uint32_t bytes, uint32_t ring, const void* initial);
int32_t cbuffer_feed(rg_handle h, uint32_t which, uint32_t offset, uint32_t size);
// From cb_hooks, after it stored an engine block: copy it into every cbuffer fed from `which`.
void on_engine_block(uint32_t which, const void* data, uint32_t size);
void cbuffer_write(rg_handle h, uint32_t offset, const void* data, uint32_t size);
rg_handle lease_register(const rg_lease_desc* d);
rg_handle view_create(uint32_t kind, uint32_t stride, uint32_t ring);
rg_handle view_create_texture(uint32_t kind, const rg_texture_view_desc* d, uint32_t ring);
int32_t view_set_resource(rg_handle h, void* resource, uint64_t bytes);
void lease_stats(rg_handle h, rg_lease_stats* out);
bool needed();
bool attach();
void detach();
bool attached();
// Once per frame (the frame hook, prof.cpp): a flow view rewrites its descriptor at most once per resource per frame.
void frame_tick();
// From pso's Create detour, BEFORE the engine creates the PSO: patch `patch` matched with vanilla bytecode `code`.
// false = a lease of this patch is refused (the vanilla shader reads its register) - the patch is refused too.
bool on_patch_matched(rg_handle patch, const void* code, size_t size);
bool patch_has_lease(rg_handle patch);
// From pso's bind detour for a patch with a lease: learn the ShaderRef, and true iff OUR descriptor sits in that
// context's staging slot now (the clone may be returned).
bool bind_ok(rg_handle patch, const void* pso_state, bool compute, bool enabled);
// The same for COMPANIONS (their leases stage at every dispatch of the engine shader they accompany):
//   on_companion_matched - Create, before the engine creates the PSO; false = refused (the companion too).
//   companion_bind       - ComputePSO::CreatePSO of an engine PSO carrying it: learns the CS ShaderRef.
//   companion_staged     - inside that dispatch (after UpdateRootParams): true iff EVERY lease of the companion has OUR
//                          descriptor in context `ctx`'s staging slot - the table the dispatch uses holds it. No lease
//                          = true. false is counted per lease (not_staged).
bool on_companion_matched(rg_handle comp, const void* code, size_t size);
void companion_bind(rg_handle comp, const void* pso_state);
bool companion_staged(rg_handle comp, const void* ctx);
// RT leases (owners = RT libraries, staged at every DispatchRays): from rt_lib's creator detour, for EVERY library the
// engine creates (its own bytes) - one that declares a leased register refuses that lease; and before baking one of
// ours - false = a lease of it is refused, keep the engine's library.
void on_rt_library(const void* code, size_t size, const char* name);
bool rtlib_may_bake(rg_handle lib);
// RT lease delivery, per DispatchRays on the recording thread (bindings.cpp "RT lease delivery"): rt_begin() at its
// start; after its UpdateRootParams, rt_delivered(bit of a library) = every RT lease of that library holds OUR
// descriptor (or our NULL one) in the table the dispatch uses - false + why otherwise; rt_state_text = the groups'
// outcome of this dispatch for a marker; rt_counts_text = the session's totals.
void rt_begin();
bool rt_delivered(uint64_t lib_bit, char* why, size_t n);
void rt_state_text(char* out, size_t n);
void rt_counts_text(char* out, size_t n);
void rt_layout_text(char* out, size_t n);  // where every RT lease lives in our heap (decodes the #n above)
}  // namespace bindings

// rt_lib.cpp - M3
namespace rtlib {
rg_handle add(const char* name, const uint8_t* hash16, rg_blob lib, int32_t priority);
bool needed();
bool attach();
void detach();
uint32_t claimed(const uint8_t* hash16);
bool info(rg_handle h, rg_blob* lib);  // the client's library
uint64_t baked_mask();                   // bit (handle - 1) = baked into a state object at least once
void stats(rg_handle h, rg_rtlib_stats* out);
// M12: raygen variants; the ONE DispatchRays hook (variants + the census's rows) - after render_flow::attach().
rg_handle variant_add(rg_handle lib, const char* raygen, const char* variant, const char* node);
int32_t variant_set(rg_handle v, uint32_t on);
void variant_stats(rg_handle v, rg_rt_variant_stats* out);
void attach_rays(bool census, bool timing);  // timing: GpuTiming on at the start (raygens of client libraries)
void detach_rays();
}  // namespace rtlib

// passes.cpp - M4
namespace passes {
rg_handle pass_register(const char* name);
uint32_t companion_request(rg_handle h, uint32_t gx, uint32_t gy, uint32_t gz);
uint32_t after_dispatch(rg_handle h, rg_pass_fn fn, void* user);
bool needed(bool companions);
bool attach();
void detach();
// M4c subscriptions: register; the mask of subscriptions on a shader (pso's Create detour); any registered.
rg_handle subscribe(const char* name, const uint8_t* hash16, rg_dispatch_fn fn, void* user);
uint32_t subs_for(const uint8_t* hash16);
bool has_subs();
// The graphics wing: draw subscriptions (keyed on a VS or a PS hash), companion draws and passes after a draw.
rg_handle draw_subscribe(const char* name, uint32_t stage, const uint8_t* hash16, rg_draw_fn fn, void* user);
uint32_t draw_subs_for(const uint8_t* vs16, const uint8_t* ps16);  // NULL = that stage has no bytecode
bool has_draw_subs();
uint32_t draw_companion_request(rg_handle h);
uint32_t after_draw(rg_handle pass, rg_pass_fn fn, void* user);
}  // namespace passes

// render_flow.cpp - M5: the node context on the recording thread (CRenderNodeBase::Process) + resources by name
namespace render_flow {
void want();  // attach the node-context hook (a census or a flow_register asked for it); before attach()
bool attach();
void detach();
bool attached();
// The node whose Process runs on this thread - or, outside any Process, the node that submitted the job running on it
// (job::Dispatcher::RunJob wrapped); nullptr = neither.
const void* current_node();
bool current_is_job();  // current_node() came from the job, not from a Process on this thread
void job_stats(uint64_t* wrapped, uint64_t* ran, uint64_t* full);  // jobs given a node; run; not wrapped (pool full)
uint64_t node_name_hash(const void* node);  // its GetName's CName hash (0 = none) - a virtual call, no cache
bool declaring(const void* node_ctx);  // the RenderFlow declaration run (phase 1): the graph runs twice per frame
rg_handle flow_register(const char* name, uint32_t name_hash);  // hash 0 = a published name, looked up
// A client's published name "<mod>/<name>" -> its 32-bit hash (FNV-1a), 0 = not such a name.
uint32_t client_name_hash(const char* name);
// node_alloc's transient under its published name; RG_E_TAKEN when another node_alloc (any client) has the name.
rg_handle flow_publish(const char* name, uint32_t hash);
uint32_t flow_hash(rg_handle h);  // the name hash of a registered resource, 0 = no such handle
// One RenderFlow request from a node's Execute, the way the engine's RT* wrappers build it (tag = name_hash ^ view <<
// 24); payload = the u32 param or the 40 B desc. Both runs of the graph must issue the same sequence.
void request(const void* node_ctx, uint8_t type, uint32_t name_hash, const void* payload, uint32_t size);
void* flow_texture(rg_handle h, uint32_t* gpuapi_id);
uint32_t flow_transition(rg_handle h, uint32_t d3d12_state);
void flow_stats(rg_handle h, rg_flow_stats* out);
// M9: the node's RenderFlow requests of the declaration run, recorded on this thread (HandleRequest detour) and replayed
// raw in the real run in place of the node - the same sequence the engine's index matching expects.
struct FlowRecord {
  static constexpr uint32_t kMax = 512;
  static constexpr uint32_t kSize = 0x30;  // e231::kFlowRequestSize
  uint32_t n;
  bool overflow;
  uint8_t thread[kMax];
  uint8_t req[kMax][kSize];
};
void want_requests();  // attach the HandleRequest detour (a census or a removal); before attach()
bool requests_attached();
void record_into(FlowRecord* r);  // this thread's requests go into r from now on; nullptr = stop
constexpr uint32_t kMaxRemoveClears = 4;  // rules per removal (render_nodes)
// node_remove_clear's rules: a resource (RenderFlow name hash, 0 = every one the node writes) and its clear value
// (RGBA; a depth target: depth, stencil).
struct RemoveClear {
  uint32_t name_hash;
  float value[4];
};
void replay(const void* node_ctx, const FlowRecord& r, const RemoveClear* clears = nullptr, uint32_t nclears = 0);
void clear_stats(uint64_t* cleared, uint64_t* skipped);
uint32_t gpu_descriptors(uint32_t count, rg_descriptors* out);  // M10
}  // namespace render_flow

// render_nodes.cpp - M6: our nodes in the engine's render graph
namespace render_nodes {
bool needed();
bool attach();
void detach();
rg_handle node_register(const char* name, const char* after, rg_node_fn fn, void* user);
void node_stats(rg_handle h, rg_node_stats* out);
int32_t node_use(rg_handle node, rg_handle flow, uint32_t access);
rg_handle node_alloc(rg_handle node, const char* name, const rg_flow_desc* desc);
rg_handle node_wrap(const char* target, rg_node_fn before, rg_node_fn after, void* user);
void node_wrap_stats(rg_handle h, rg_wrap_stats* out);
int32_t node_wrap_use(rg_handle wrap, rg_handle flow, uint32_t access);
// A wrap's declarations (node_wrap_use): how many, the requests issued, 1 = its target is a group (not issued).
void node_wrap_uses(rg_handle h, uint32_t* uses, uint64_t* requests, uint32_t* group);
rg_handle node_remove(const char* target, uint32_t on);
void node_remove_set(rg_handle h, uint32_t on);
void node_remove_stats(rg_handle h, rg_remove_stats* out);
int32_t node_remove_clear(rg_handle h, uint32_t name_hash, const float* value);
// GPU timing on at the start: every engine node class is wrapped (as for the graph debug module) and each node's
// Execute is timed. Before attach.
void wrap_all_classes();
// The client removal (node_remove) of the class `node_name` that is ON now - its handle, else 0. For the debug window.
rg_handle client_removal(const char* node_name);
// A node vtable's Execute as the engine built it (the original when slot [1] is wrapped) - for the graph dump.
const void* original_execute(const void* const* vtable);
// The graph debug module (graph_debug.cpp): every engine class's slot [1] is wrapped at its first Register (before
// attach), and any single node INSTANCE can be removed live (M9's record / replay, keyed by the node object).
void debug_enable();
bool debug_removable(const void* vtable);       // that class is wrapped by its own vtable (not a group's, not ours)
// A node the HOST inserted for a client (node_register): its handle, else 0. Known by our vtable, not by its name.
rg_handle debug_our_node(const void* node, const void* vtable);
bool debug_remove(const void* node, bool on);   // false = no free slot (on) - nothing changed
void debug_remove_clear();                      // every slot freed - at a graph change, before the graph runs
void debug_remove_stats(const void* node, rg_remove_stats* out);
uint32_t debug_wrapped_classes();
}  // namespace render_nodes

// hlsl.cpp - compile_hlsl (dxcompiler.dll next to the host, optional)
namespace hlsl {
int32_t compile(const rg_hlsl_desc* desc, rg_blob* out, char* error, uint32_t error_size);
}  // namespace hlsl

// graph_probe.cpp - the render graph, read only ([Diagnostics] RenderGraphDump)
namespace graph_probe {
// The shader census is on: pso attaches both pipe kinds and reports every PSO creation and bind.
bool census();
// From pso's Create detours, after the engine made `pso` from `desc` (pipe 0 = compute desc, 1 = graphics desc).
void on_pso_created(uint32_t pipe, const void* pso, const void* desc);
// From pso's bind funnel: the engine PSO the context binds now, on the recording thread.
void on_bind(uint32_t pipe, const void* pso);
// From rtlib's state-object creator detour, after a successful creation: the container entry the engine filled, the
// library's DXIL hash, its export names (CPOS order).
void on_state_object(const void* entry, const uint8_t* hash16, const char* const* exports, uint32_t n);
// From rtlib's DispatchRays detour (census window only does anything): one dispatch, its engine call site.
void on_dispatch_rays(uint32_t so, uint32_t w, uint32_t h, uint32_t a5, uint32_t site);
bool rays_wanted();  // the census is attached (dump + node context): rtlib must hook DispatchRays for it
// From render_flow's HandleRequest detour: every RenderFlow request, before the engine handles it.
void on_request(const void* data, const uint8_t* req);

// For graph_debug: the executing graph's tree (rebuilt at each change of the executing graph, on the render thread at
// ExecuteParallel's entry - no node of the previous frame runs then) and the census of its binds.
struct DebugNode {
  std::string key;   // stable across rebuilds: "<parent key>/<name>#<occurrence among its siblings>"
  std::string name;  // GetName
  const void* obj;
  const void* vtable;
  int32_t parent;  // index in the same tree, -1 = top level
  uint8_t depth;
  bool group;  // a CRenderNodeCommandListGroup
  bool job;    // GetJobBuilderUsage: its work records in a job - M9 refuses it
};
struct DebugShaderRow {
  int32_t node;  // index in the tree of the same graph, -1 = outside any node (jobs)
  uint8_t has;   // bit s = h[s] valid (rg_stage)
  uint8_t pipe;  // 0 compute, 1 graphics, 2 DispatchRays (rt set)
  uint8_t h[3][16];
  std::string rt;
  uint64_t binds;
  uint32_t psos;
};
struct PsoShaders {
  uint8_t has, pipe;
  uint8_t h[3][16];
};
bool pso_shaders(const void* pso, PsoShaders* out);  // false = PSO not seen at creation
void census_request();  // open a census window on the executing graph at the next frame (if none is open)
bool census_running(uint64_t* frames_left);
}  // namespace graph_probe

// graph_debug.cpp - the CET window's graph debug module ([Diagnostics] RenderGraphDebug)
namespace graph_debug {
bool on();
void on_tree(uint64_t sig, const char* mode, std::vector<graph_probe::DebugNode>&& nodes);
void on_census(uint64_t sig, uint64_t frames, std::vector<graph_probe::DebugShaderRow>&& rows);
// From pso's bind funnel: true = return NULL for this PSO (the engine skips the draw / dispatch, as for a PSO still
// compiling). One relaxed load when no rule is set.
bool skip(uint32_t pipe, const void* pso);
}  // namespace graph_debug

}  // namespace core
