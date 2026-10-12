#include "core/crash_log.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <intrin.h>

#include "core/engine_231.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace crash_log {
namespace {

constexpr int kMaxReports = 12;
constexpr int kMaxFrames = 20;
constexpr int kMaxCmdLists = 8;               // unfinished command lists printed from the breadcrumbs
constexpr int kOpsBefore = 3, kOpsAfter = 4;  // ops around the one each list stopped at

RED4ext::v1::PluginHandle g_handle = nullptr;
const RED4ext::v1::Sdk* g_sdk = nullptr;
PVOID g_veh = nullptr;
std::atomic<int> g_reports{0};
std::atomic<ID3D12Device*> g_device{nullptr};
std::atomic<bool> g_dred_armed{false};
std::atomic<bool> g_removed_reported{false};
std::atomic<ExtraDump> g_extra{nullptr};
// The watchdog: a game that loses the GPU often closes itself without any CPU fault (the 0.7.1 4080S reports,
// fog_crash.7z) - so the device is also polled, off the render threads, from set_device until Unload.
constexpr DWORD kWatchMs = 500;
HANDLE g_watch_stop = nullptr;
HANDLE g_watch_thread = nullptr;
// The engine's GPU-crash path (e231::kGpuCrashReport): its fatal error ends the process within the watchdog's period.
using GpuCrashReportFn = void (*)(bool);
GpuCrashReportFn g_orig_gpu_crash = nullptr;
bool g_gpu_crash_hooked = false;
std::atomic<bool> g_gpu_crash_seen{false};

void line(const char* text) {
  if (g_sdk) g_sdk->logger->Error(g_handle, text);
}

void linef(const char* fmt, ...) {
  char b[900];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(b, sizeof(b), fmt, args);
  va_end(args);
  line(b);
}

const char* op_name(D3D12_AUTO_BREADCRUMB_OP op) {
  switch (op) {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return "SetMarker";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return "BeginEvent";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return "EndEvent";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return "DrawInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED: return "DrawIndexedInstanced";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return "ExecuteIndirect";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return "Dispatch";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "CopyBufferRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "CopyTextureRegion";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return "CopyResource";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW: return "ClearUAV";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "ResourceBarrier";
    case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return "Present";
    case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE: return "BuildRaytracingAS";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRAYTRACINGACCELERATIONSTRUCTURE: return "CopyRaytracingAS";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return "DispatchRays";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEEXTENSIONCOMMAND: return "ExecuteExtensionCommand";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCHMESH: return "DispatchMesh";
    default: return nullptr;
  }
}

void report_dred(ID3D12Device* dev) {
  if (!g_dred_armed.load()) {
    line("[gpu] DRED was not armed ([Diagnostics] Dred = true in the plugin's ini, then restart) - no fault address");
    return;
  }
  ID3D12DeviceRemovedExtendedData* dred = nullptr;
  if (FAILED(dev->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData), reinterpret_cast<void**>(&dred))) ||
      dred == nullptr) {
    line("[gpu] DRED interface unavailable on this device");
    return;
  }
  D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
  const HRESULT pf_hr = dred->GetPageFaultAllocationOutput(&pf);
  if (SUCCEEDED(pf_hr)) {
    linef("[gpu] page fault VA 0x%llx", static_cast<unsigned long long>(pf.PageFaultVA));
    int k = 0;
    for (auto* a = pf.pHeadExistingAllocationNode; a != nullptr && k < 8; a = a->pNext, ++k) {
      linef("[gpu]   allocation at that VA: %ls (type %d)", a->ObjectNameW ? a->ObjectNameW : L"<unnamed>",
            static_cast<int>(a->AllocationType));
    }
    k = 0;
    for (auto* a = pf.pHeadRecentFreedAllocationNode; a != nullptr && k < 8; a = a->pNext, ++k) {
      linef("[gpu]   RECENTLY FREED at that VA: %ls (type %d)", a->ObjectNameW ? a->ObjectNameW : L"<unnamed>",
            static_cast<int>(a->AllocationType));
    }
  } else {
    linef("[gpu] no page-fault record, hr 0x%08lx (a hang/TDR rather than a bad address, or the driver did not report "
          "one)", static_cast<unsigned long>(pf_hr));
  }
  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
  const HRESULT bc_hr = dred->GetAutoBreadcrumbsOutput(&bc);
  if (FAILED(bc_hr)) {
    // 0x887a0022 DXGI_ERROR_NOT_CURRENTLY_AVAILABLE = breadcrumbs were off for this device (e.g. taken by Aftermath).
    linef("[gpu] no auto-breadcrumbs: GetAutoBreadcrumbsOutput failed, hr 0x%08lx", static_cast<unsigned long>(bc_hr));
  } else {
    int shown = 0, unfinished = 0;
    for (auto* nd = bc.pHeadAutoBreadcrumbNode; nd != nullptr; nd = nd->pNext) {
      if (nd->pLastBreadcrumbValue == nullptr || nd->pCommandHistory == nullptr) continue;
      const uint32_t done = *nd->pLastBreadcrumbValue, total = nd->BreadcrumbCount;
      if (done >= total) continue;
      ++unfinished;
      if (shown >= kMaxCmdLists) continue;
      ++shown;
      linef("[gpu] unfinished command list %ls on queue %ls: %u of %u ops done",
            nd->pCommandListDebugNameW ? nd->pCommandListDebugNameW : L"<unnamed>",
            nd->pCommandQueueDebugNameW ? nd->pCommandQueueDebugNameW : L"<unnamed>", done, total);
      const uint32_t lo = done > static_cast<uint32_t>(kOpsBefore) ? done - kOpsBefore : 0;
      const uint32_t hi = (std::min)(total, done + kOpsAfter);
      for (uint32_t i = lo; i < hi; ++i) {
        const char* nm = op_name(nd->pCommandHistory[i]);
        char b[64];
        if (nm == nullptr) std::snprintf(b, sizeof(b), "op %d", static_cast<int>(nd->pCommandHistory[i]));
        linef("[gpu]     %s #%u %s", i == done ? "->" : "  ", i, nm != nullptr ? nm : b);
      }
    }
    linef("[gpu] %d unfinished command list(s), %d shown", unfinished, shown);
  }
  dred->Release();
}

// Once per session: is the device gone, and if so, why and where.
void check_device(const char* when) {
  if (g_removed_reported.load()) return;
  ID3D12Device* dev = g_device.load();
  if (dev == nullptr) return;
  const HRESULT hr = dev->GetDeviceRemovedReason();
  if (hr == S_OK || g_removed_reported.exchange(true)) return;
  const char* what = hr == DXGI_ERROR_DEVICE_HUNG            ? "DEVICE_HUNG (a GPU hang / TDR)"
                     : hr == DXGI_ERROR_DEVICE_REMOVED        ? "DEVICE_REMOVED"
                     : hr == DXGI_ERROR_DEVICE_RESET          ? "DEVICE_RESET"
                     : hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR ? "DRIVER_INTERNAL_ERROR"
                     : hr == DXGI_ERROR_INVALID_CALL          ? "INVALID_CALL (the app did something illegal)"
                                                              : "?";
  linef("[gpu] D3D12 DEVICE REMOVED, seen at %s: reason 0x%08lx %s", when, static_cast<unsigned long>(hr), what);
  report_dred(dev);
  if (ExtraDump fn = g_extra.load()) fn(&line);
}

DWORD WINAPI watch(void*) {
  while (WaitForSingleObject(g_watch_stop, kWatchMs) == WAIT_TIMEOUT && !g_removed_reported.load())
    check_device("the device watchdog");
  return 0;
}

bool is_fatal(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_STACK_OVERFLOW:
    case STATUS_STACK_BUFFER_OVERRUN:
    case EXCEPTION_IN_PAGE_ERROR:
      return true;
    default:
      return false;
  }
}

// "module+0xRVA" for an address, or "<no module>+0xADDR" (heap: e.g. a hook trampoline).
void describe(uint64_t addr, char* out, size_t n) {
  HMODULE mod = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(addr), &mod) && mod) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(mod, path, MAX_PATH);
    const wchar_t* base = std::wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    std::snprintf(out, n, "%ls+0x%llx", base, static_cast<unsigned long long>(addr - reinterpret_cast<uint64_t>(mod)));
  } else {
    std::snprintf(out, n, "<no module>+0x%llx", static_cast<unsigned long long>(addr));
  }
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* ep) {
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (!is_fatal(code)) return EXCEPTION_CONTINUE_SEARCH;
  if (g_reports.fetch_add(1) >= kMaxReports) return EXCEPTION_CONTINUE_SEARCH;

  const CONTEXT& ctx = *ep->ContextRecord;
  char where[300];
  describe(ctx.Rip, where, sizeof(where));
  const auto& rec = *ep->ExceptionRecord;
  linef("[crash] thread %lu code=0x%08lx at %s | info0=0x%llx info1=0x%llx | rsp=0x%llx "
        "rcx=0x%llx rdx=0x%llx r8=0x%llx r9=0x%llx rax=0x%llx",
        GetCurrentThreadId(), code, where,
        static_cast<unsigned long long>(rec.NumberParameters > 0 ? rec.ExceptionInformation[0] : 0),
        static_cast<unsigned long long>(rec.NumberParameters > 1 ? rec.ExceptionInformation[1] : 0),
        static_cast<unsigned long long>(ctx.Rsp), static_cast<unsigned long long>(ctx.Rcx),
        static_cast<unsigned long long>(ctx.Rdx), static_cast<unsigned long long>(ctx.R8),
        static_cast<unsigned long long>(ctx.R9), static_cast<unsigned long long>(ctx.Rax));

  if (code != EXCEPTION_STACK_OVERFLOW) {  // no stack left to unwind on
    // The walk reads the stack, so it stays inside this thread's stack: a smashed RSP would fault in here.
    ULONG_PTR lo = 0, hi = 0;
    GetCurrentThreadStackLimits(&lo, &hi);
    CONTEXT c = ctx;
    for (int i = 0; i < kMaxFrames && c.Rip && c.Rsp >= lo && c.Rsp + 8 <= hi; ++i) {
      describe(c.Rip, where, sizeof(where));
      linef("[crash]   #%02d %s", i, where);
      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
      if (!fn) {  // leaf / no unwind info (e.g. a trampoline): take the return address at RSP
        c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
        c.Rsp += 8;
        continue;
      }
      PVOID handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fn, &c, &handler_data, &establisher, nullptr);
    }
  }
  // A CPU crash is often the aftermath of a GPU one (the engine dereferences what a removed device never wrote).
  check_device("the crash above");
  return EXCEPTION_CONTINUE_SEARCH;
}

// The engine found the device lost (a failed Present / HRESULT): report removal + DRED BEFORE its own crash report,
// whose fatal error ends the process. Observe only - the engine's path then runs unchanged.
void d_gpu_crash_report(bool arg) {
  if (!g_gpu_crash_seen.exchange(true)) {
    char from[300];
    describe(reinterpret_cast<uint64_t>(_ReturnAddress()), from, sizeof(from));
    linef("[gpu] the ENGINE reports a GPU crash (GpuApi::GpuCrashReport from %s)", from);
    check_device("the engine's GPU crash report");
    if (!g_removed_reported.load()) line("[gpu] ...but the device still answers S_OK - no removal reason, no DRED");
  }
  g_orig_gpu_crash(arg);
}

// DRED is a process-wide setting the D3D12 runtime reads at device creation - at Load the engine has not created its
// device yet (that is 9+ s later, the first PSO). Not a device call.
void arm_dred() {
  HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
  using GetDebugInterface = HRESULT(WINAPI*)(REFIID, void**);
  auto* get = d3d12 != nullptr ? reinterpret_cast<GetDebugInterface>(GetProcAddress(d3d12, "D3D12GetDebugInterface"))
                               : nullptr;
  ID3D12DeviceRemovedExtendedDataSettings* s = nullptr;
  if (get == nullptr || FAILED(get(__uuidof(ID3D12DeviceRemovedExtendedDataSettings), reinterpret_cast<void**>(&s))) ||
      s == nullptr) {
    g_sdk->logger->Warn(g_handle, "crash log: DRED settings unavailable - a GPU fault will be reported without its address");
    return;
  }
  s->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  s->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  s->Release();
  g_dred_armed.store(true);
}

}  // namespace

void open(RED4ext::v1::PluginHandle handle, const RED4ext::v1::Sdk* sdk, bool dred) {
  g_handle = handle;
  g_sdk = sdk;
  if (dred) arm_dred();
  g_veh = AddVectoredExceptionHandler(1, on_exception);
  sdk->logger->InfoF(handle, "crash log: %s, DRED %s", g_veh ? "ON (fatal exceptions + device removal)" : "FAILED to install",
                     g_dred_armed.load() ? "ARMED (page faults + auto-breadcrumbs, costs a little GPU time)"
                                         : "off ([Diagnostics] Dred = true, then restart)");
}

bool dred_armed() { return g_dred_armed.load(); }

void set_extra_dump(ExtraDump fn) { g_extra.store(fn); }

void set_device(void* device) {
  ID3D12Device* expected = nullptr;
  auto* dev = static_cast<ID3D12Device*>(device);
  if (dev == nullptr || !g_device.compare_exchange_strong(expected, dev)) return;
  dev->AddRef();  // Unload may outlive the engine's
  g_watch_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_watch_thread = g_watch_stop ? CreateThread(nullptr, 0, &watch, nullptr, 0, nullptr) : nullptr;
  if (g_sdk)
    g_sdk->logger->InfoF(g_handle, "crash log: device watchdog %s", g_watch_thread ? "ON (polls device removal every 500 ms)"
                                                                                  : "FAILED to start");
}

void attach_engine() {
  if (g_gpu_crash_hooked || g_sdk == nullptr) return;
  using core::e231::call_site_ok;
  const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  const bool sites = call_site_ok(base, core::e231::kPresentCrashReportRet, core::e231::kGpuCrashReport) &&
                     call_site_ok(base, core::e231::kCheckHrCrashReportRet, core::e231::kGpuCrashReport);
  g_gpu_crash_hooked =
      sites && g_sdk->hooking->Attach(g_handle, reinterpret_cast<void*>(base + core::e231::kGpuCrashReport),
                                      reinterpret_cast<void*>(&d_gpu_crash_report),
                                      reinterpret_cast<void**>(&g_orig_gpu_crash));
  g_sdk->logger->InfoF(g_handle, "crash log: the engine's GPU crash report %s",
                       g_gpu_crash_hooked ? "HOOKED (removal + DRED logged before it)"
                       : sites            ? "did NOT attach"
                                          : "- the call sites are not game 2.31's, NOT attached");
}

void close() {
  if (g_gpu_crash_hooked) {
    g_sdk->hooking->Detach(g_handle, reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) +
                                                            core::e231::kGpuCrashReport));
    g_gpu_crash_hooked = false;
  }
  bool watch_done = true;
  if (g_watch_thread) {
    SetEvent(g_watch_stop);
    // bounded: Unload may run where a thread cannot finish; then the thread and the device it reads are left alone
    watch_done = WaitForSingleObject(g_watch_thread, 1000) == WAIT_OBJECT_0;
    CloseHandle(g_watch_thread);
    g_watch_thread = nullptr;
  }
  if (watch_done && g_watch_stop) {
    CloseHandle(g_watch_stop);
    g_watch_stop = nullptr;
  }
  check_device("Unload");
  if (g_veh) RemoveVectoredExceptionHandler(g_veh);
  g_veh = nullptr;
  if (!watch_done) return;
  if (ID3D12Device* dev = g_device.exchange(nullptr)) dev->Release();
}

}  // namespace crash_log
