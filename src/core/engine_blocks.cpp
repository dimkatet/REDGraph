#include "core/engine_blocks.hpp"

#include <Windows.h>
#include <intrin.h>

#include <atomic>
#include <cstring>
#include <cwchar>
#include <mutex>

#include <RED4ext/Api/v1/GameState.hpp>
#include <RED4ext/Api/v1/GameStates.hpp>
#include <RED4ext/GameStates.hpp>

#include "core/internal.hpp"
#include "core/prof.hpp"

namespace engine_blocks {
namespace {

// 2.31 RVAs (cp2077-symbols data/names_by_address.csv, ea - 0x140000000).
constexpr uintptr_t kUpdate3 = 0x1f088c;  // ?UpdateBuffer@GpuApi@@YAXUBufferRef@1@IPEBX@Z
constexpr uintptr_t kRawCS = 0x1ee3cc;    // ?SetComputeShaderConstsRaw@GpuApi@@YAXIPEBX@Z
enum Fn : uint8_t { kFnUpdate3 = 0, kFnRawCS = 1, kFns = 2 };

// THE CATALOG - append only (an index is an id clients hold). name = the writing engine function (cp2077-symbols);
// ret = the return address right after its call into the hooked GpuApi function; size = the exact byte count it
// writes (0 = whatever it passes, kept up to kMaxBytes).
struct Entry {
  const char* name;
  Fn fn;
  uintptr_t ret;
  uint32_t size;
  const char* what;
};
const Entry kCatalog[] = {
    // 0-3: rg_engine_block of API v1 (RG_ENGINE_FOG_B6, _CLOUD_BAKE_B6, _CLOUD_WORLD_B6, _CB12) - always captured.
    {"CRenderVolumetricFogManager::Update", kFnRawCS, 0x61afb0, 0,
     "the volumetric fog CS constants (b6, SVolFogConstants)"},
    {"CRenderNode_RenderVolumetricCloudsLighting::Execute", kFnRawCS, 0x61c047, 0,
     "the volumetric clouds lighting CS constants (b6 of its self-shadow bake, 53c455b6)"},
    {"CRenderNode_RenderVolumetricClouds::Execute", kFnRawCS, 0x61b89c, 0,
     "the volumetric clouds CS constants (b6, VolumetricCludsState, 192 B; 3dd31a42)"},
    {"CRenderInterface::CalculateSharedConstants", kFnUpdate3, 0x78673a, 1584,
     "the shared per-frame constants (cb12)"},
};
constexpr uint32_t kCount = sizeof(kCatalog) / sizeof(kCatalog[0]);
constexpr uint32_t kLegacy = 4;      // always captured
constexpr uint32_t kMaxBytes = 2048; // the most of one block kept

// Every parameter is declared register-wide and forwarded untouched: BufferRef is 4 bytes and trivially copyable
// (passed in a register).
using Update3_t = void (*)(uint64_t buf, uint64_t a, const void* data);
using RawCS_t = void (*)(uint64_t size, const void* data);
Update3_t g_update3 = nullptr;
RawCS_t g_raw_cs = nullptr;

struct Block {
  std::atomic<bool> wanted{false};
  uintptr_t ret = 0;  // absolute, set at attach (0 = not captured)
  uint64_t seq = 0;   // writes seen so far (0 = none yet)
  uint32_t size = 0;
  uint8_t data[kMaxBytes];
};
Block g_blocks[kCount];
SRWLOCK g_lock = SRWLOCK_INIT;

std::mutex g_attach_mutex;
bool g_settled = false;            // BaseInitialization passed: a hook wanted from now on attaches at once
bool g_fn_attached[kFns] = {};
bool g_no_hooks = false;
RED4ext::v1::PluginHandle g_handle = nullptr;
const RED4ext::v1::Sdk* g_sdk = nullptr;

void store(uint32_t id, const void* data, uint32_t n) {
  Block& b = g_blocks[id];
  const uint32_t c = n < kMaxBytes ? n : kMaxBytes;  // the engine reads n bytes
  AcquireSRWLockExclusive(&g_lock);
  std::memcpy(b.data, data, c);
  b.size = n;
  ++b.seq;
  ReleaseSRWLockExclusive(&g_lock);
  core::bindings::on_engine_block(id, data, c);  // cbuffers fed from it (host API)
}

// One writer per entry, at most a handful of entries per function: a linear compare of the return address.
void capture(Fn fn, uintptr_t ret, const void* data, uint64_t n) {
  if (!data) return;
  for (uint32_t i = 0; i < kCount; ++i)
    if (kCatalog[i].fn == fn && g_blocks[i].ret == ret) {
      if (!kCatalog[i].size || n == kCatalog[i].size) store(i, data, static_cast<uint32_t>(n));
      return;
    }
}

void d_update3(uint64_t buf, uint64_t a, const void* data) {
  core::prof::Scope prof_scope(core::prof::kEngineBlock);
  capture(kFnUpdate3, reinterpret_cast<uintptr_t>(_ReturnAddress()), data, a);
  core::prof::Pause engine;
  g_update3(buf, a, data);
}

void d_raw_cs(uint64_t size, const void* data) {
  core::prof::Scope prof_scope(core::prof::kEngineBlock);
  capture(kFnRawCS, reinterpret_cast<uintptr_t>(_ReturnAddress()), data, size);
  core::prof::Pause engine;
  g_raw_cs(size, data);
}

// A flag file next to this DLL.
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

// The exe is 2.31 iff the return address sits right after an E8 call whose target is exactly the hooked function.
bool call_site_ok(uintptr_t base, uintptr_t ret_rva, uintptr_t target_rva) {
  const auto* p = reinterpret_cast<const uint8_t*>(base + ret_rva - 5);
  if (p[0] != 0xE8) return false;
  int32_t rel;
  std::memcpy(&rel, p + 1, 4);
  return base + ret_rva + static_cast<intptr_t>(rel) == base + target_rva;
}

// Entry `id` starts being captured: its call site checked, its function hooked if it is not yet. g_attach_mutex held.
void arm_locked(uint32_t id) {
  if (g_no_hooks || g_blocks[id].ret) return;
  const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  const Entry& e = kCatalog[id];
  const uintptr_t target = e.fn == kFnUpdate3 ? kUpdate3 : kRawCS;
  if (!call_site_ok(base, e.ret, target)) {
    g_sdk->logger->WarnF(g_handle, "engine blocks: %s - the call site is not game 2.31's, NOT captured", e.name);
    return;
  }
  if (!g_fn_attached[e.fn]) {
    // A jmp already in a prologue = another plugin hooked it before us (FilmGrainFix does SetComputeShaderConstsRaw).
    g_sdk->logger->InfoF(g_handle, "engine blocks: prologue byte of %s 0x%02x",
                         e.fn == kFnUpdate3 ? "UpdateBuffer" : "SetComputeShaderConstsRaw",
                         *reinterpret_cast<const uint8_t*>(base + target));
    const bool ok = e.fn == kFnUpdate3
                        ? g_sdk->hooking->Attach(g_handle, reinterpret_cast<void*>(base + target),
                                                 reinterpret_cast<void*>(&d_update3), reinterpret_cast<void**>(&g_update3))
                        : g_sdk->hooking->Attach(g_handle, reinterpret_cast<void*>(base + target),
                                                 reinterpret_cast<void*>(&d_raw_cs), reinterpret_cast<void**>(&g_raw_cs));
    if (!ok) {
      g_sdk->logger->WarnF(g_handle, "engine blocks: %s did not attach - %s NOT captured",
                           e.fn == kFnUpdate3 ? "UpdateBuffer" : "SetComputeShaderConstsRaw", e.name);
      return;
    }
    g_fn_attached[e.fn] = true;
  }
  g_blocks[id].ret = base + e.ret;
  g_sdk->logger->InfoF(g_handle, "engine blocks: #%u %s captured - %s", id, e.name, e.what);
}

// The hooks attach at BaseInitialization's OnEnter, not in Main(Load): by then every RED4ext plugin has loaded (and
// registered with us). FilmGrainFix byte-checks SetComputeShaderConstsRaw's prologue before hooking it and unloads
// itself on a mismatch - our jmp, written first (plugins load alphabetically), made it unload. Attaching last also keeps
// OUR detour the outermost one, which the _ReturnAddress() match in the bodies requires.
bool on_base_init_enter(RED4ext::CGameApplication*) {
  std::lock_guard<std::mutex> lock(g_attach_mutex);
  g_settled = true;
  g_no_hooks = flag_file(L"no_hooks");
  if (g_no_hooks) {
    g_sdk->logger->Info(g_handle, "engine blocks: flag file no_hooks - nothing captured");
    return true;
  }
  for (uint32_t i = 0; i < kCount; ++i)
    if (g_blocks[i].wanted.load()) arm_locked(i);
  return true;
}

}  // namespace

void attach(RED4ext::v1::PluginHandle handle, const RED4ext::v1::Sdk* sdk) {
  g_handle = handle;
  g_sdk = sdk;
  for (uint32_t i = 0; i < kLegacy; ++i) g_blocks[i].wanted.store(true);
  static RED4ext::v1::GameState state{&on_base_init_enter, nullptr, nullptr};
  if (!sdk->gameStates->Add(handle, RED4ext::EGameStateType::BaseInitialization, &state)) {
    sdk->logger->Warn(handle, "engine blocks: could not register the BaseInitialization state - attaching now");
    on_base_init_enter(nullptr);
  }
}

void detach(RED4ext::v1::PluginHandle handle, const RED4ext::v1::Sdk* sdk) {
  std::lock_guard<std::mutex> lock(g_attach_mutex);
  const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  if (g_fn_attached[kFnUpdate3]) sdk->hooking->Detach(handle, reinterpret_cast<void*>(base + kUpdate3));
  if (g_fn_attached[kFnRawCS]) sdk->hooking->Detach(handle, reinterpret_cast<void*>(base + kRawCS));
  g_fn_attached[kFnUpdate3] = g_fn_attached[kFnRawCS] = false;
  for (Block& b : g_blocks) b.ret = 0;
}

uint32_t count() { return kCount; }

bool want(uint32_t id) {
  if (id >= kCount) return false;
  if (g_blocks[id].wanted.exchange(true)) return true;
  std::lock_guard<std::mutex> lock(g_attach_mutex);
  if (g_settled) arm_locked(id);  // asked after BaseInitialization: hooked now
  return true;
}

int32_t find(const char* name) {
  if (!name) return -1;
  for (uint32_t i = 0; i < kCount; ++i)
    if (std::strcmp(kCatalog[i].name, name) == 0) return static_cast<int32_t>(i);
  return -1;
}

const char* name(uint32_t id) { return id < kCount ? kCatalog[id].name : "?"; }

uint64_t latest(uint32_t id, void* out, uint32_t max_bytes, uint32_t* size) {
  if (id >= kCount) return 0;
  const Block& b = g_blocks[id];
  AcquireSRWLockShared(&g_lock);
  const uint64_t s = b.seq;
  if (size) *size = b.size;
  if (out && s) {
    uint32_t c = b.size < kMaxBytes ? b.size : kMaxBytes;
    if (c > max_bytes) c = max_bytes;
    std::memcpy(out, b.data, c);
  }
  ReleaseSRWLockShared(&g_lock);
  return s;
}

}  // namespace engine_blocks
