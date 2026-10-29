// The host: the API table (api/rg_host.h), registration window, on-demand hook attachment.

#include "core/host.hpp"

#include <Windows.h>
#include <d3d12.h>

#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

#include "core/engine_blocks.hpp"
#include "core/clients.hpp"
#include "core/ab.hpp"
#include "core/gpu_timing.hpp"
#include "core/crash_log.hpp"
#include "core/engine_231.hpp"
#include "core/internal.hpp"
#include "core/prof.hpp"

namespace core {

RED4ext::v1::PluginHandle g_handle = nullptr;
const RED4ext::v1::Sdk* g_sdk = nullptr;
uintptr_t g_base = 0;

namespace {

std::atomic<bool> g_open{false};

// The registration window's end: GpuApi::InitDevice's entry.
using InitDevice_t = bool (*)(const void* params);
InitDevice_t g_orig_init = nullptr;
bool g_init_hooked = false;
std::atomic<bool> g_attached_once{false};
const char* g_attach_at = "Load";

bool d_init_device(const void* params) {
  if (!g_attached_once.exchange(true)) {
    g_attach_at = "GpuApi::InitDevice";
    attach();
  }
  return g_orig_init(params);
}

bool flag_file(const wchar_t* name) {
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&flag_file), &self))
    return false;
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
  if (!n || n >= MAX_PATH) return false;
  wchar_t* slash = wcsrchr(path, L'\\');
  if (!slash || (slash - path) + 1 + wcslen(name) >= MAX_PATH) return false;
  wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), name);
  return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

std::atomic<bool> g_verbose{false};

void api_log(uint32_t level, const char* message) {
  if (!g_sdk || !message) return;
  if (level == RG_LOG_DEBUG && !g_verbose.load(std::memory_order_relaxed)) return;
  if (level == RG_LOG_ERROR)
    g_sdk->logger->Error(g_handle, message);
  else if (level == RG_LOG_WARN)
    g_sdk->logger->Warn(g_handle, message);
  else
    g_sdk->logger->Info(g_handle, message);
}

uint32_t api_shader_claimed(const uint8_t* hash16) { return pso::claimed(hash16) | rtlib::claimed(hash16); }

// M4c: the engine's device and the device_ready callbacks.
void* api_device() {
  const auto* data = *reinterpret_cast<const uint8_t* const*>(g_base + e231::kDeviceData);
  auto* engine_dev = data ? *reinterpret_cast<IUnknown* const*>(data + e231::kDeviceInData) : nullptr;
  ID3D12Device* dev = nullptr;
  if (!engine_dev || FAILED(engine_dev->QueryInterface(IID_PPV_ARGS(&dev)))) return nullptr;
  dev->Release();  // the engine holds it for the process lifetime
  return dev;
}

constexpr uint32_t kMaxDeviceFns = 8;
struct DeviceFn {
  rg_device_fn fn;
  void* user;
};
DeviceFn g_device_fns[kMaxDeviceFns];
std::atomic<uint32_t> g_ndevice_fns{0};

int32_t api_device_ready(rg_device_fn fn, void* user) {
  if (!registration_open()) return RG_E_LATE;
  if (!fn) return RG_E_ARG;
  const uint32_t n = g_ndevice_fns.load();
  if (n >= kMaxDeviceFns) return RG_E_FULL;
  g_device_fns[n] = {fn, user};
  g_ndevice_fns.store(n + 1, std::memory_order_release);
  return static_cast<int32_t>(n + 1);
}

uint64_t api_engine_block(uint32_t which, void* out, uint32_t max_bytes, uint32_t* size) {
  return engine_blocks::latest(which, out, max_bytes, size);
}

void api_host_stats(rg_host_stats* out) {
  if (!out) return;
  *out = {};
  pso::host_stats(out);
}

void api_log_set_verbose(uint32_t on) {
  if (g_verbose.exchange(on != 0) == (on != 0) || !g_sdk) return;
  g_sdk->logger->InfoF(g_handle, "log: verbose %s", on ? "ON (per-object detail)" : "off ([Diagnostics] VerboseLog)");
}

// ---- the registering entries, attributed to the calling module (core/clients.hpp) ----
// Each runs the core's function, then records the call under the module its return address lies in.

std::string hex(const uint8_t* h16) {
  if (!h16) return "?";
  char s[33];
  for (int i = 0; i < 16; ++i) std::snprintf(s + 2 * i, 3, "%02x", h16[i]);
  return s;
}

const char* stage_name(uint32_t stage) {
  return stage == RG_STAGE_VS ? "VS" : stage == RG_STAGE_PS ? "PS" : stage == RG_STAGE_GS ? "GS" : "CS";
}

std::string str(const char* s) { return s ? s : ""; }

__declspec(noinline) rg_handle w_patch_register(const rg_patch_desc* d) {
  const rg_handle r = pso::patch_register(d);
  const std::string t = d ? std::string(stage_name(d->stage)) + " " + hex(d->hash) : "";
  clients::note(_ReturnAddress(), clients::kPatch, r, 0, d ? d->name : nullptr, t.c_str());
  return r;
}

__declspec(noinline) int32_t w_patch_scope(rg_handle patch, const char* node) {
  int32_t r = clients::owner_check(_ReturnAddress(), clients::sPatch, patch, "patch_scope");
  if (!r) r = pso::patch_scope(patch, node);
  clients::note(_ReturnAddress(), clients::kPatchScope, r, patch, clients::name_of(clients::kPatch, patch).c_str(), node);
  return r;
}

__declspec(noinline) rg_handle w_companion_register(const char* name, const uint8_t* hash16, rg_blob cs) {
  const rg_handle r = pso::companion_register(name, hash16, cs);
  clients::note(_ReturnAddress(), clients::kCompanion, r, 0, name, ("CS " + hex(hash16)).c_str());
  return r;
}

__declspec(noinline) int32_t w_companion_scope(rg_handle comp, const char* node) {
  int32_t r = clients::owner_check(_ReturnAddress(), clients::sCompanion, comp, "companion_scope");
  if (!r) r = pso::companion_scope(comp, node);
  clients::note(_ReturnAddress(), clients::kCompanionScope, r, comp,
                clients::name_of(clients::kCompanion, comp).c_str(), node);
  return r;
}

__declspec(noinline) rg_handle w_cbuffer_create(uint32_t bytes, uint32_t ring, const void* initial) {
  const rg_handle r = bindings::cbuffer_create(bytes, ring, initial);
  char t[48];
  std::snprintf(t, sizeof t, "%u B, ring %u", bytes, ring);
  clients::note(_ReturnAddress(), clients::kCbuffer, r, 0, nullptr, t);
  return r;
}

__declspec(noinline) int32_t w_cbuffer_feed(rg_handle cbuffer, uint32_t which, uint32_t offset, uint32_t size) {
  int32_t r = clients::owner_check(_ReturnAddress(), clients::sCbuffer, cbuffer, "cbuffer_feed");
  if (!r) r = bindings::cbuffer_feed(cbuffer, which, offset, size);
  static const char* const kBlock[] = {"fog b6", "cloud bake b6", "world clouds b6", "cb12"};
  char n[32], t[64];
  std::snprintf(n, sizeof n, "cbuffer #%d", cbuffer);
  std::snprintf(t, sizeof t, "%s -> +%u, %u B", which < 4 ? kBlock[which] : "?", offset, size);
  clients::note(_ReturnAddress(), clients::kFeed, r, cbuffer, n, t);
  return r;
}

// Every handle a lease names must be the caller's own.
int32_t lease_owned(const void* ret, const rg_lease_desc* d) {
  if (!d) return 0;
  int32_t r = 0;
  for (uint32_t i = 0; !r && d->patches && i < d->patch_count; ++i)
    r = clients::owner_check(ret, clients::sPatch, d->patches[i], "lease_register");
  // companions: compute ones at stage CS, draw companions at VS / PS
  const clients::Space cs = d->stage == RG_STAGE_CS ? clients::sCompanion : clients::sDrawCompanion;
  for (uint32_t i = 0; !r && d->companions && i < d->companion_count; ++i)
    r = clients::owner_check(ret, cs, d->companions[i], "lease_register");
  for (uint32_t i = 0; !r && d->rtlibs && i < d->rtlib_count; ++i)
    r = clients::owner_check(ret, clients::sRtLib, d->rtlibs[i], "lease_register");
  if (!r && d->cbuffer > 0) r = clients::owner_check(ret, clients::sCbuffer, d->cbuffer, "lease_register");
  if (!r && d->view > 0) r = clients::owner_check(ret, clients::sView, d->view, "lease_register");
  return r;
}

__declspec(noinline) rg_handle w_lease_register(const rg_lease_desc* d) {
  rg_handle r = lease_owned(_ReturnAddress(), d);
  if (!r) r = bindings::lease_register(d);
  char t[32] = "";
  if (d) std::snprintf(t, sizeof t, "%s %c%u", stage_name(d->stage), "btu"[d->kind < 3 ? d->kind : 0], d->reg);
  const std::string n = d ? str(d->name) + (d->rtlib_count ? " [RT]" : "") : "";  // the clients tab's register map
  clients::note(_ReturnAddress(), clients::kLease, r, 0, n.c_str(), t);
  return r;
}

__declspec(noinline) rg_handle w_view_create(uint32_t kind, uint32_t stride, uint32_t ring) {
  const rg_handle r = bindings::view_create(kind, stride, ring);
  char t[48];
  std::snprintf(t, sizeof t, "%s, stride %u, ring %u",
                kind == RG_KIND_CBV ? "CBV" : kind == RG_KIND_SRV ? "SRV" : "UAV", stride, ring);
  clients::note(_ReturnAddress(), clients::kView, r, 0, nullptr, t);
  return r;
}

__declspec(noinline) int32_t w_patch_state(rg_handle patch, uint32_t variant, const rg_pso_state* s) {
  int32_t r = clients::owner_check(_ReturnAddress(), clients::sPatch, patch, "patch_state");
  if (!r) r = pso::patch_state(patch, variant, s);
  char t[48] = "";
  if (s) std::snprintf(t, sizeof t, "variant %u, groups 0x%x", variant, s->set);
  clients::note(_ReturnAddress(), clients::kPatchState, r, patch, clients::name_of(clients::kPatch, patch).c_str(), t);
  return r;
}

__declspec(noinline) rg_handle w_view_create_texture(uint32_t kind, const rg_texture_view_desc* d, uint32_t ring) {
  rg_handle r = d && d->flow > 0 ? clients::owner_check(_ReturnAddress(), clients::sFlow, d->flow, "view_create_texture")
                                 : 0;
  if (!r) r = bindings::view_create_texture(kind, d, ring);
  char t[96] = "";
  if (d)
    std::snprintf(t, sizeof t, "%s texture (dim %u, format %u), ring %u%s%s", kind == RG_KIND_UAV ? "UAV" : "SRV",
                  d->dimension, d->format, ring, d->flow > 0 ? ", of " : "",
                  d->flow > 0 ? clients::name_of(clients::kFlow, d->flow).c_str() : "");
  clients::note(_ReturnAddress(), clients::kView, r, 0, nullptr, t);
  return r;
}

__declspec(noinline) rg_handle w_rtlib_register(const char* name, const uint8_t* hash16, rg_blob lib,
                                                 int32_t priority) {
  const rg_handle r = rtlib::add(name, hash16, lib, priority);
  clients::note(_ReturnAddress(), clients::kRtLib, r, 0, name, hex(hash16).c_str());
  return r;
}

__declspec(noinline) rg_handle w_rt_variant(rg_handle lib, const char* raygen, const char* variant, const char* node) {
  rg_handle r = clients::owner_check(_ReturnAddress(), clients::sRtLib, lib, "rt_variant");
  if (!r) r = rtlib::variant_add(lib, raygen, variant, node);
  const std::string t = clients::target_of(clients::kRtLib, lib) + " raygen " + str(raygen);
  const std::string n = str(variant) + (node && *node ? " in " + str(node) : "");
  clients::note(_ReturnAddress(), clients::kRtVariant, r, lib, n.c_str(), t.c_str());
  return r;
}

__declspec(noinline) rg_handle w_pass_register(const char* name) {
  const rg_handle r = passes::pass_register(name);
  clients::note(_ReturnAddress(), clients::kPass, r, 0, name, "");
  return r;
}

__declspec(noinline) rg_handle w_subscribe(const char* name, const uint8_t* hash16, rg_dispatch_fn fn, void* user) {
  const rg_handle r = passes::subscribe(name, hash16, fn, user);
  clients::note(_ReturnAddress(), clients::kSubscribe, r, 0, name, ("CS " + hex(hash16)).c_str());
  return r;
}

__declspec(noinline) int32_t w_device_ready(rg_device_fn fn, void* user) {
  const int32_t r = api_device_ready(fn, user);
  clients::note(_ReturnAddress(), clients::kDeviceReady, r, 0, nullptr, "");
  return r;
}

__declspec(noinline) rg_handle w_flow_register(const char* name, uint32_t name_hash) {
  const rg_handle r = render_flow::flow_register(name, name_hash);
  char t[16];
  std::snprintf(t, sizeof t, "%08x", name_hash);
  clients::note(_ReturnAddress(), clients::kFlow, r, 0, name, t);
  return r;
}

__declspec(noinline) rg_handle w_node_register(const char* name, const char* after, rg_node_fn fn, void* user) {
  const rg_handle r = render_nodes::node_register(name, after, fn, user);
  clients::note(_ReturnAddress(), clients::kNode, r, 0, name, after);
  return r;
}

__declspec(noinline) int32_t w_node_use(rg_handle node, rg_handle flow, uint32_t access) {
  int32_t r = clients::owner_check(_ReturnAddress(), clients::sNode, node, "node_use");
  if (!r) r = clients::owner_check(_ReturnAddress(), clients::sFlow, flow, "node_use");
  if (!r) r = render_nodes::node_use(node, flow, access);
  clients::note(_ReturnAddress(), clients::kNodeUse, r, node, clients::name_of(clients::kNode, node).c_str(),
                clients::name_of(clients::kFlow, flow).c_str());
  return r;
}

__declspec(noinline) rg_handle w_node_alloc(rg_handle node, const char* name, const rg_flow_desc* desc) {
  rg_handle r = clients::owner_check(_ReturnAddress(), clients::sNode, node, "node_alloc");
  if (!r) r = render_nodes::node_alloc(node, name, desc);
  clients::note(_ReturnAddress(), clients::kNodeAlloc, r, node, name, clients::name_of(clients::kNode, node).c_str());
  return r;
}

__declspec(noinline) rg_handle w_node_wrap(const char* target, rg_node_fn before, rg_node_fn after, void* user) {
  const rg_handle r = render_nodes::node_wrap(target, before, after, user);
  clients::note(_ReturnAddress(), clients::kWrap, r, 0,
                before && after ? "before + after" : before ? "before" : "after", target);
  return r;
}

__declspec(noinline) int32_t w_node_wrap_use(rg_handle wrap, rg_handle flow, uint32_t access) {
  int32_t r = clients::owner_check(_ReturnAddress(), clients::sWrap, wrap, "node_wrap_use");
  if (!r) r = clients::owner_check(_ReturnAddress(), clients::sFlow, flow, "node_wrap_use");
  if (!r) r = render_nodes::node_wrap_use(wrap, flow, access);
  clients::note(_ReturnAddress(), clients::kWrapUse, r, wrap, clients::target_of(clients::kWrap, wrap).c_str(),
                clients::name_of(clients::kFlow, flow).c_str());
  return r;
}

__declspec(noinline) rg_handle w_draw_subscribe(const char* name, uint32_t stage, const uint8_t* hash16,
                                                rg_draw_fn fn, void* user) {
  const rg_handle r = passes::draw_subscribe(name, stage, hash16, fn, user);
  clients::note(_ReturnAddress(), clients::kDrawSubscribe, r, 0, name,
                (std::string(stage == RG_STAGE_VS ? "VS " : "PS ") + hex(hash16)).c_str());
  return r;
}

__declspec(noinline) rg_handle w_draw_companion_register(const char* name, uint32_t stage, const uint8_t* hash16,
                                                         rg_blob shader, const rg_pso_state* state) {
  const rg_handle r = pso::draw_companion_register(name, stage, hash16, shader, state);
  clients::note(_ReturnAddress(), clients::kDrawCompanion, r, 0, name,
                (std::string(stage == RG_STAGE_VS ? "VS " : "PS ") + hex(hash16)).c_str());
  return r;
}

__declspec(noinline) int32_t w_draw_companion_scope(rg_handle comp, const char* node) {
  int32_t r = clients::owner_check(_ReturnAddress(), clients::sDrawCompanion, comp, "draw_companion_scope");
  if (!r) r = pso::draw_companion_scope(comp, node);
  clients::note(_ReturnAddress(), clients::kDrawCompanionScope, r, comp,
                clients::name_of(clients::kDrawCompanion, comp).c_str(), node);
  return r;
}

__declspec(noinline) rg_handle w_node_remove(const char* target, uint32_t on) {
  const rg_handle r = render_nodes::node_remove(target, on);
  clients::note(_ReturnAddress(), clients::kRemove, r, 0, nullptr, target);
  return r;
}

// ---- the [hot] entries that take a handle: the owner check, then the core (the *_stats calls go straight through) ----
bool own(const void* ret, clients::Space space, rg_handle h, const char* call) {
  return clients::owner_check(ret, space, h, call) == 0;
}

uint32_t h_draw_companion_request(rg_handle comp) {
  return own(_ReturnAddress(), clients::sDrawCompanion, comp, "draw_companion_request")
             ? passes::draw_companion_request(comp)
             : 0u;
}
uint32_t h_after_draw(rg_handle pass, rg_pass_fn fn, void* user) {
  return own(_ReturnAddress(), clients::sPass, pass, "pass_after_draw") ? passes::after_draw(pass, fn, user) : 0u;
}

void h_patch_select(rg_handle patch, int32_t variant) {
  if (own(_ReturnAddress(), clients::sPatch, patch, "patch_select")) pso::patch_select(patch, variant);
}
void h_cbuffer_write(rg_handle cbuffer, uint32_t offset, const void* data, uint32_t size) {
  if (own(_ReturnAddress(), clients::sCbuffer, cbuffer, "cbuffer_write")) bindings::cbuffer_write(cbuffer, offset, data, size);
}
uint32_t h_companion_request(rg_handle comp, uint32_t gx, uint32_t gy, uint32_t gz) {
  return own(_ReturnAddress(), clients::sCompanion, comp, "companion_request") ? passes::companion_request(comp, gx, gy, gz)
                                                                               : 0u;
}
uint32_t h_after_dispatch(rg_handle pass, rg_pass_fn fn, void* user) {
  return own(_ReturnAddress(), clients::sPass, pass, "pass_after_dispatch") ? passes::after_dispatch(pass, fn, user) : 0u;
}
int32_t h_view_set_resource(rg_handle view, void* resource, uint64_t bytes) {
  const int32_t r = clients::owner_check(_ReturnAddress(), clients::sView, view, "view_set_resource");
  return r ? r : bindings::view_set_resource(view, resource, bytes);
}
void* h_flow_texture(rg_handle res, uint32_t* gpuapi_id) {
  if (own(_ReturnAddress(), clients::sFlow, res, "flow_texture")) return render_flow::flow_texture(res, gpuapi_id);
  if (gpuapi_id) *gpuapi_id = 0;
  return nullptr;
}
uint32_t h_flow_transition(rg_handle res, uint32_t state) {
  return own(_ReturnAddress(), clients::sFlow, res, "flow_transition") ? render_flow::flow_transition(res, state) : 0u;
}
__declspec(noinline) int32_t w_engine_block_find(const char* name) {
  int32_t r = RG_E_LATE;
  if (registration_open()) {
    const int32_t id = engine_blocks::find(name);
    r = id < 0 ? RG_E_ARG : (engine_blocks::want(static_cast<uint32_t>(id)), id);
  }
  clients::note(_ReturnAddress(), clients::kEngineBlock, r, 0, name ? name : "", "");
  return r;
}

__declspec(noinline) int32_t w_census_watch(uint32_t stage, const uint8_t* hash16, const char* name) {
  const int32_t r = blas_census::watch(stage, hash16, name);
  if (r != RG_E_UNAVAILABLE)  // census off: not a registration, nothing to show
    clients::note(_ReturnAddress(), clients::kCensusWatch, r, 0, name,
                  hash16 ? (std::string(stage == RG_STAGE_VS ? "VS " : "CS ") + hex(hash16)).c_str() : "");
  return r;
}

__declspec(noinline) int32_t w_node_remove_clear(rg_handle remove, uint32_t name_hash, const float* value) {
  int32_t r = clients::owner_check(_ReturnAddress(), clients::sRemove, remove, "node_remove_clear");
  if (!r) r = render_nodes::node_remove_clear(remove, name_hash, value);
  return r;
}

__declspec(noinline) rg_handle w_ab_switch(const char* name, rg_ab_set_fn set, rg_ab_get_fn get, void* user) {
  const rg_handle r = ab::ab_switch(name, set, get, user);
  clients::note(_ReturnAddress(), clients::kAbSwitch, r, 0, name ? name : "", "");
  return r;
}

void h_node_remove_set(rg_handle remove, uint32_t on) {
  if (own(_ReturnAddress(), clients::sRemove, remove, "node_remove_set")) render_nodes::node_remove_set(remove, on);
}
int32_t h_rt_variant_set(rg_handle variant, uint32_t on) {
  const int32_t r = clients::owner_check(_ReturnAddress(), clients::sRtVariant, variant, "rt_variant_set");
  return r ? r : rtlib::variant_set(variant, on);
}

const rg_host kHost = {
    RG_HOST_API_VERSION,
    sizeof(rg_host),
    &api_log,
    &w_patch_register,
    &h_patch_select,
    &pso::patch_stats,
    &w_cbuffer_create,
    &h_cbuffer_write,
    &w_lease_register,
    &bindings::lease_stats,
    &w_rtlib_register,
    &w_companion_register,
    &h_companion_request,
    &pso::companion_stats,
    &w_pass_register,
    &h_after_dispatch,
    &api_shader_claimed,
    &api_host_stats,
    &w_cbuffer_feed,
    &w_view_create,
    &h_view_set_resource,
    &w_subscribe,
    &api_device,
    &w_device_ready,
    &api_engine_block,
    &api_log_set_verbose,
    &w_flow_register,
    &h_flow_texture,
    &render_flow::flow_stats,
    &w_node_register,
    &render_nodes::node_stats,
    &w_node_use,
    &w_node_alloc,
    &h_flow_transition,
    &w_node_wrap,
    &render_nodes::node_wrap_stats,
    &w_node_remove,
    &h_node_remove_set,
    &render_nodes::node_remove_stats,
    &render_flow::gpu_descriptors,
    &w_patch_scope,
    &w_rt_variant,
    &h_rt_variant_set,
    &rtlib::variant_stats,
    &w_companion_scope,
    &rtlib::stats,
    &w_view_create_texture,
    &w_patch_state,
    &w_node_wrap_use,
    &w_draw_subscribe,
    &w_draw_companion_register,
    &h_draw_companion_request,
    &w_draw_companion_scope,
    &pso::draw_companion_stats,
    &h_after_draw,
    &hlsl::compile,
    &w_node_remove_clear,
    &w_census_watch,
    &w_engine_block_find,
    &w_ab_switch,
    &ab::prof_active,
    &ab::prof_add,
};

}  // namespace

bool verbose() { return g_verbose.load(std::memory_order_relaxed); }

bool registration_open() { return g_open.load(std::memory_order_acquire); }

bool device_ready_wanted() { return g_ndevice_fns.load() != 0; }

void fire_device_ready() {
  void* dev = api_device();
  if (dev) crash_log::set_device(dev);  // device removal + DRED in a crash report
  const uint32_t n = g_ndevice_fns.load(std::memory_order_acquire);
  if (!n) return;
  if (!dev) {
    g_sdk->logger->Error(g_handle, "host: the engine's first PSO, but no device in its device data - device_ready NOT "
                                   "fired (the clients create nothing)");
    return;
  }
  g_sdk->logger->InfoF(g_handle, "host: device %p ready at the engine's first PSO - %u device_ready callback(s)", dev, n);
  for (uint32_t i = 0; i < n; ++i) {
    clients::Running running(reinterpret_cast<const void*>(g_device_fns[i].fn));
    g_device_fns[i].fn(g_device_fns[i].user, dev);
  }
}

bool attach_hook(uintptr_t rva, uintptr_t ret_rva, void* detour, void** orig, const char* what) {
  if (!e231::call_site_ok(g_base, ret_rva, rva)) {
    g_sdk->logger->WarnF(g_handle, "host: %s - the call site is not game 2.31's, NOT attached", what);
    return false;
  }
  if (!g_sdk->hooking->Attach(g_handle, reinterpret_cast<void*>(g_base + rva), detour, orig)) {
    g_sdk->logger->WarnF(g_handle, "host: %s did not attach", what);
    return false;
  }
  return true;
}

void detach_hook(uintptr_t rva) { g_sdk->hooking->Detach(g_handle, reinterpret_cast<void*>(g_base + rva)); }

void open(RED4ext::v1::PluginHandle handle, const RED4ext::v1::Sdk* sdk) {
  g_handle = handle;
  g_sdk = sdk;
  g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  g_open.store(true, std::memory_order_release);
}

void arm() {
  g_init_hooked = attach_hook(e231::kGpuInitDevice, e231::kGpuInitDeviceRet, reinterpret_cast<void*>(&d_init_device),
                              reinterpret_cast<void**>(&g_orig_init), "GpuApi::InitDevice (the registration window)");
  if (g_init_hooked) {
    g_sdk->logger->Info(g_handle, "host: registration OPEN until GpuApi::InitDevice - client plugins loaded after us "
                                  "may register in their Load");
    return;
  }
  g_attached_once.store(true);
  attach();  // the window closes with our own Load
}

void drain_clients() {
  HANDLE lock = nullptr;
  rg_rendezvous* r = rg_rendezvous_open(&lock);
  if (!r) {
    g_sdk->logger->Warn(g_handle, "host: no client rendezvous (a client loaded before us cannot reach us)");
    return;
  }
  r->drained = 1;
  const uint32_t n = r->count < RG_RENDEZVOUS_MAX ? r->count : RG_RENDEZVOUS_MAX;
  decltype(r->pending) waiting;
  std::memcpy(waiting, r->pending, sizeof(waiting));
  r->count = 0;
  ReleaseMutex(lock);
  g_sdk->logger->InfoF(g_handle, "host: %u client(s) waited for us in the rendezvous", n);
  for (uint32_t i = 0; i < n; ++i) {
    if (!waiting[i].fn) continue;
    if (waiting[i].api_version > RG_HOST_API_VERSION) {
      g_sdk->logger->WarnF(g_handle, "host: a waiting client wants API %u, we are %u - skipped", waiting[i].api_version,
                           RG_HOST_API_VERSION);
      clients::skipped(reinterpret_cast<const void*>(waiting[i].fn), waiting[i].api_version);
      continue;
    }
    clients::seen(reinterpret_cast<const void*>(waiting[i].fn), waiting[i].api_version, true);
    clients::running_init(reinterpret_cast<const void*>(waiting[i].fn));
    waiting[i].fn(&kHost, waiting[i].user);
    clients::running_init(nullptr);
  }
}

void attach() {
  g_open.store(false, std::memory_order_release);
  g_sdk->logger->InfoF(g_handle, "host: registration CLOSED - clients: %s", clients::summary().c_str());
  crash_log::attach_engine();  // after every plugin's Load (some compare the prologues of what they hook)
#if RG_NATIVE_HOST
  if (flag_file(L"no_pso_swap")) {
    g_sdk->logger->Info(g_handle, "host: NOT attached (flag file no_pso_swap) - every shader stays vanilla");
    return;
  }
  // Order: PSO hooks before anything that hangs off a matched PSO (leases, companions); all at Load, before the first
  // PSO / root signature / state object exists (feasibility 0.1).
  const bool want_pso = pso::needed(), want_cbv = bindings::needed(), want_rt = rtlib::needed() || graph_probe::census(),
             want_pass = passes::needed(pso::has_companions());
  bool pso_ok = false, cbv_ok = false, rt_ok = false, pass_ok = false;
  if (want_pso) pso_ok = pso::attach();
  if (want_cbv) cbv_ok = pso_ok && bindings::attach();
  if (want_rt) rt_ok = rtlib::attach();
  if (want_pass) pass_ok = passes::attach();
  if (gpu_timing::enabled()) render_nodes::wrap_all_classes();  // GpuTiming on at the start: every node timed
  if (ab::enabled()) render_nodes::wrap_all_classes();          // ABProfile: every node's CPU (and GPU) per class
  render_nodes::attach();  // only if a client registered a node, a wrap, or every class is wrapped
  render_flow::attach();   // only if a census, a flow_register or a node wants it
  if (graph_probe::census()) graph_probe::attach();
  // M12 variants, the census's DispatchRays rows and / or GPU timing of raygens in state objects with our libraries
  rtlib::attach_rays(graph_probe::rays_wanted(), gpu_timing::enabled());
  blas_census::attach();  // [Diagnostics] BlasCensus only; after attach_rays (shares its NvAPI BLAS hook)
  if (readback::on()) rtlib::attach_nv_census();  // [Dev] Readback's blas: the BLAS builds' inputs
  prof::attach_frames();                         // the profile timer (RG_Profile)
  auto s = [](bool want, bool ok) { return want ? (ok ? "on" : "FAILED") : "none"; };
  g_sdk->logger->InfoF(g_handle, "host: ATTACHED at %s - PSO patches %s, engine-table leases %s, RT libraries %s, "
                       "command-list passes %s", g_attach_at, s(want_pso, pso_ok), s(want_cbv, cbv_ok),
                       s(want_rt, rt_ok), s(want_pass, pass_ok));
#else
  g_sdk->logger->Info(g_handle, "host: native mechanisms not in this build (release plugin)");
#endif
}

void detach() {
  if (g_init_hooked) detach_hook(e231::kGpuInitDevice);
  g_init_hooked = false;
  blas_census::detach();
  rtlib::detach_rays();
  prof::detach();
  graph_probe::detach();
  render_flow::detach();
  render_nodes::detach();
  passes::detach();
  rtlib::detach();
  bindings::detach();
  pso::detach();
}

}  // namespace core

// A client built against an OLDER header gets the table too (fields are only appended; struct_size says which exist);
// one built against a NEWER header gets NULL.
// NULL until our Load has run (open): a client finding the module earlier waits in the rendezvous instead.
extern "C" RG_HOST_EXPORT const rg_host* rg_host_get(uint32_t version) {
  if (!core::g_sdk || version < 1 || version > RG_HOST_API_VERSION) return nullptr;
  core::clients::seen(_ReturnAddress(), version, false);  // a client reached the table (our own CET natives do not count)
  return &core::kHost;
}
