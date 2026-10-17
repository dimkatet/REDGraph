#pragma once

// Fatal-exception + GPU-fault logger (docs/internals.md "Crash log"). Two kinds of death, two instruments, both into
// the plugin's own log:
//
//   CPU - a vectored exception handler runs first, on the faulting thread, before any engine handler can swallow it:
//   code, faulting RIP as module+RVA, the accessed address, an unwound call stack (20 frames). AV, illegal / privileged
//   instruction, stack overflow, stack-buffer overrun, in-page error only (not C++ exceptions, which engines throw on
//   purpose). At most 12 reports. OBSERVE ONLY - always EXCEPTION_CONTINUE_SEARCH; it never reads memory it was not
//   handed (a first-chance AV of OURS freezes the game in the engine's CrashReporter).
//
//   GPU - a shader page fault or a hang removes the DEVICE and the process dies later, somewhere unrelated, or quits.
//   So: GetDeviceRemovedReason inside the exception handler, every 500 ms on a watchdog thread (from set_device - a
//   game that loses the GPU often just closes itself, with no CPU fault) and at Unload; once removed, DRED - the faulting GPU
//   VA, the allocations around it, and the auto-breadcrumbs of every command list the GPU did not finish. DRED must be
//   on BEFORE the engine's D3D12CreateDevice: `[Diagnostics] Dred = true` in the plugin's ini, read at Load (costs a
//   little GPU time - off by default).

#include <RED4ext/Common.hpp>  // first: defines RED4EXT_HEADER_ONLY for Api/v1/SemVer.hpp (see internal.hpp)
#include <RED4ext/Api/v1/PluginHandle.hpp>
#include <RED4ext/Api/v1/Sdk.hpp>

namespace crash_log {

// At Load, after settings::load(): installs the handler; arms DRED when `dred`.
void open(RED4ext::v1::PluginHandle handle, const RED4ext::v1::Sdk* sdk, bool dred);
// At the host's attach (GpuApi::InitDevice, after every plugin's Load): hooks the engine's GPU-crash path
// (GpuApi::GpuCrashReport), whose fatal error ends the process before the watchdog's next poll.
void attach_engine();
// The engine's ID3D12Device*, once known (core's device_ready).
void set_device(void* device);
// At Unload: a game that quits on its own after losing the GPU never crashes - ask once more, then remove the handler.
void close();
// [Diagnostics] Dred was on at Load (rtlib's DispatchRays markers ride on the same switch).
bool dred_armed();
// Called after DRED in a device-removal report (rtlib: which DispatchRays the GPU was inside); `line` = one log line.
using ExtraDump = void (*)(void (*line)(const char*));
void set_extra_dump(ExtraDump fn);

}  // namespace crash_log
