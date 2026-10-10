#include "core/cb_hooks.hpp"

#include <Windows.h>
#include <intrin.h>

#include <cstring>
#include <cwchar>

#include <RED4ext/Api/v1/GameState.hpp>
#include <RED4ext/Api/v1/GameStates.hpp>
#include <RED4ext/GameStates.hpp>

#include "core/internal.hpp"
#include "core/prof.hpp"

namespace cb_hooks {
namespace {

// 2.31 RVAs (cp2077-symbols data/names_by_address.csv, ea - 0x140000000).
constexpr uintptr_t kUpdate3 = 0x1f088c;  // ?UpdateBuffer@GpuApi@@YAXUBufferRef@1@IPEBX@Z
constexpr uintptr_t kRawCS = 0x1ee3cc;    // ?SetComputeShaderConstsRaw@GpuApi@@YAXIPEBX@Z
// The callers whose data we keep: the return address right after their call into the hooked function.
constexpr uintptr_t kCb12WriterRet = 0x78673a; // CRenderInterface::CalculateSharedConstants+0x127a (UpdateBuffer, cb12)
constexpr uintptr_t kFogRawRet = 0x61afb0;     // CRenderVolumetricFogManager::Update+0x540 (the fog CS b6)
constexpr uintptr_t kCloudRawRet = 0x61c047;   // CRenderNode_RenderVolumetricCloudsLighting::Execute (53c455b6's b6)
constexpr uintptr_t kWorldRawRet = 0x61b89c;   // CRenderNode_RenderVolumetricClouds::Execute+0x2e8 (3dd31a42's b6)
constexpr uint32_t kBitUpdate3 = 4, kBitRawCS = 256;  // g_attached bits

// Every parameter is declared register-wide and forwarded untouched: BufferRef is 4 bytes and trivially copyable
// (passed in a register).
using Update3_t = void (*)(uint64_t buf, uint64_t a, const void* data);
using RawCS_t = void (*)(uint64_t size, const void* data);

Update3_t g_update3 = nullptr;
RawCS_t g_raw_cs = nullptr;
uint32_t g_attached = 0;
uintptr_t g_cb12_ret = 0, g_fog_ret = 0, g_cloud_ret = 0, g_world_ret = 0;  // absolute, set at attach

// The latest of each: cb12 and the volumetric passes' raw constants. One writer per block per frame.
constexpr uint32_t kCb12Bytes = 1584;    // cb12, CRenderInterface::CalculateSharedConstants
constexpr uint32_t kRawConstMax = 1024;  // the most of a raw b6 we keep
struct EngineConsts {
  uint64_t cb12_seq, fog_seq, cloud_seq, world_seq;   // writes seen so far (0 = none yet)
  uint32_t fog_size, cloud_size, world_size;
  float cb12[kCb12Bytes / 4];
  uint8_t fog_b6[kRawConstMax];    // CRenderVolumetricFogManager::Update (the fog CS b6)
  uint8_t cloud_b6[kRawConstMax];  // CRenderNode_RenderVolumetricCloudsLighting::Execute (53c455b6)
  uint8_t world_b6[kRawConstMax];  // CRenderNode_RenderVolumetricClouds::Execute (3dd31a42, VolumetricCludsState)
};
SRWLOCK g_consts_lock = SRWLOCK_INIT;
EngineConsts g_consts{};


void push_cb12(uint32_t id, const void* src, uint64_t size) {
  if (size != kCb12Bytes || !src) return;
  (void)id;
  AcquireSRWLockExclusive(&g_consts_lock);
  std::memcpy(g_consts.cb12, src, kCb12Bytes);
  ++g_consts.cb12_seq;
  ReleaseSRWLockExclusive(&g_consts_lock);
  core::bindings::on_engine_block(RG_ENGINE_CB12, src, kCb12Bytes);  // cbuffers fed from it (host API)
}

void d_update3(uint64_t buf, uint64_t a, const void* data) {
  core::prof::Scope prof_scope(core::prof::kEngineBlock);
  const auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
  if (ret == g_cb12_ret) push_cb12(static_cast<uint32_t>(buf), data, a);
  core::prof::Pause engine;
  g_update3(buf, a, data);
}

void d_raw_cs(uint64_t size, const void* data) {
  core::prof::Scope prof_scope(core::prof::kEngineBlock);
  const auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
  const auto n = static_cast<uint32_t>(size);
  const bool fog = ret == g_fog_ret, cloud = ret == g_cloud_ret, world = ret == g_world_ret;
  if ((fog || cloud || world) && data) {
    const uint32_t c = n < kRawConstMax ? n : kRawConstMax;  // the engine reads n bytes
    AcquireSRWLockExclusive(&g_consts_lock);
    if (fog) {
      std::memcpy(g_consts.fog_b6, data, c);
      g_consts.fog_size = n;
      ++g_consts.fog_seq;
    } else if (cloud) {
      std::memcpy(g_consts.cloud_b6, data, c);
      g_consts.cloud_size = n;
      ++g_consts.cloud_seq;
    } else {
      std::memcpy(g_consts.world_b6, data, c);
      g_consts.world_size = n;
      ++g_consts.world_seq;
    }
    ReleaseSRWLockExclusive(&g_consts_lock);
    core::bindings::on_engine_block(fog ? RG_ENGINE_FOG_B6 : cloud ? RG_ENGINE_CLOUD_BAKE_B6 : RG_ENGINE_CLOUD_WORLD_B6,
                                    data, c);  // cbuffers fed from it (host API)
  }
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

// The exe is 2.31 iff every return address the ship bodies compare against sits right after an E8 call whose
// target is exactly the hooked function. RED4ext already refuses another runtime; this is what keeps a wrong RVA (a
// hotfix with the same version number) from ever being hooked. Reads the exe's own mapped .text - always readable.
bool call_site_ok(uintptr_t base, uintptr_t ret_rva, uintptr_t target_rva) {
  const auto* p = reinterpret_cast<const uint8_t*>(base + ret_rva - 5);
  if (p[0] != 0xE8) return false;
  int32_t rel;
  std::memcpy(&rel, p + 1, 4);
  return base + ret_rva + static_cast<intptr_t>(rel) == base + target_rva;
}

// The hooks attach at BaseInitialization's OnEnter, not in Main(Load): by then every RED4ext plugin has loaded.
// FilmGrainFix byte-checks SetComputeShaderConstsRaw's prologue before hooking it and unloads itself on a mismatch -
// our jmp, written first (plugins load alphabetically), made it unload. Attaching last also keeps OUR detour the
// outermost one, which the _ReturnAddress() caller match in the bodies requires.
RED4ext::v1::PluginHandle g_handle = nullptr;
const RED4ext::v1::Sdk* g_sdk = nullptr;

void attach_now(RED4ext::v1::PluginHandle handle, const RED4ext::v1::Sdk* sdk);

bool on_base_init_enter(RED4ext::CGameApplication*) {
  if (g_sdk) attach_now(g_handle, g_sdk);
  return true;
}

}  // namespace

void attach(RED4ext::v1::PluginHandle handle, const RED4ext::v1::Sdk* sdk) {
  g_handle = handle;
  g_sdk = sdk;
  static RED4ext::v1::GameState state{&on_base_init_enter, nullptr, nullptr};
  if (!sdk->gameStates->Add(handle, RED4ext::EGameStateType::BaseInitialization, &state)) {
    sdk->logger->Warn(handle, "cb hooks: could not register the BaseInitialization state - attaching now");
    attach_now(handle, sdk);
  }
}

namespace {

void attach_now(RED4ext::v1::PluginHandle handle, const RED4ext::v1::Sdk* sdk) {
  const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  g_cb12_ret = base + kCb12WriterRet;
  g_fog_ret = base + kFogRawRet;
  g_cloud_ret = base + kCloudRawRet;
  g_world_ret = base + kWorldRawRet;
  if (flag_file(L"no_hooks")) {
    sdk->logger->Info(handle, "cb hooks: mode NONE (flag file no_hooks) - nothing attached");
    return;
  }
  const bool sites = call_site_ok(base, kCb12WriterRet, kUpdate3) && call_site_ok(base, kFogRawRet, kRawCS) &&
                     call_site_ok(base, kCloudRawRet, kRawCS) && call_site_ok(base, kWorldRawRet, kRawCS);
  if (!sites) {
    sdk->logger->Warn(handle, "cb hooks: the exe's call sites do not match game 2.31 - nothing attached, the "
                              "engine-fed features stay off");
    return;
  }
  // A jmp already in a prologue = another plugin hooked it before us (FilmGrainFix does SetComputeShaderConstsRaw).
  sdk->logger->InfoF(handle, "cb hooks: prologue byte UpdateBuffer 0x%02x, SetComputeShaderConstsRaw 0x%02x",
                     *reinterpret_cast<const uint8_t*>(base + kUpdate3), *reinterpret_cast<const uint8_t*>(base + kRawCS));
  if (sdk->hooking->Attach(handle, reinterpret_cast<void*>(base + kUpdate3), reinterpret_cast<void*>(&d_update3),
                           reinterpret_cast<void**>(&g_update3)))
    g_attached |= kBitUpdate3;
  else
    sdk->logger->Warn(handle, "cb hooks: UpdateBuffer did not attach");
  if (sdk->hooking->Attach(handle, reinterpret_cast<void*>(base + kRawCS), reinterpret_cast<void*>(&d_raw_cs),
                           reinterpret_cast<void**>(&g_raw_cs)))
    g_attached |= kBitRawCS;
  else
    sdk->logger->Warn(handle, "cb hooks: SetComputeShaderConstsRaw did not attach");
  sdk->logger->InfoF(handle, "cb hooks: mode SHIP, attached mask 0x%x", g_attached);
}

}  // namespace

void detach(RED4ext::v1::PluginHandle handle, const RED4ext::v1::Sdk* sdk) {
  const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  if (g_attached & kBitUpdate3) sdk->hooking->Detach(handle, reinterpret_cast<void*>(base + kUpdate3));
  if (g_attached & kBitRawCS) sdk->hooking->Detach(handle, reinterpret_cast<void*>(base + kRawCS));
  g_attached = 0;
}

uint64_t engine_raw_consts(uint32_t which, void* out, uint32_t max_bytes, uint32_t* size) {
  const uint64_t* seq;
  const void* src;
  uint32_t n;
  AcquireSRWLockShared(&g_consts_lock);
  switch (which) {
    case RG_ENGINE_FOG_B6: seq = &g_consts.fog_seq, src = g_consts.fog_b6, n = g_consts.fog_size; break;
    case RG_ENGINE_CLOUD_BAKE_B6: seq = &g_consts.cloud_seq, src = g_consts.cloud_b6, n = g_consts.cloud_size; break;
    case RG_ENGINE_CLOUD_WORLD_B6: seq = &g_consts.world_seq, src = g_consts.world_b6, n = g_consts.world_size; break;
    case RG_ENGINE_CB12: seq = &g_consts.cb12_seq, src = g_consts.cb12, n = kCb12Bytes; break;
    default: ReleaseSRWLockShared(&g_consts_lock); return 0;
  }
  const uint64_t s = *seq;
  if (size) *size = n;
  if (out && s) {
    uint32_t c = (which == RG_ENGINE_CB12 || n < kRawConstMax) ? n : kRawConstMax;
    if (c > max_bytes) c = max_bytes;
    std::memcpy(out, src, c);
  }
  ReleaseSRWLockShared(&g_consts_lock);
  return s;
}

}  // namespace cb_hooks
