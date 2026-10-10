#pragma once

// Who registered what (docs/internals.md "Clients tab"). The API carries no client identity, so the host
// attributes every registering call to the MODULE it was called from (its return address; the init a waiting client
// left in the rendezvous runs under that client's module). host.cpp wraps the registering entries of the table; the
// core's modules report what is refused later (a lease's vanilla check, a patch taken over by a higher priority).
// Cold path only: registration, refusals, the CET "Clients" tab and the status report.

#include <cstdint>
#include <string>

#include "rg_host.h"

namespace core::clients {

enum Kind : uint8_t {
  kPatch, kPatchScope, kCompanion, kCompanionScope, kLease, kCbuffer, kFeed, kView, kRtLib, kRtVariant, kPass,
  kSubscribe, kDeviceReady, kFlow, kNode, kNodeUse, kNodeAlloc, kWrap, kRemove, kKinds
};

// A client reached the table: rg_host_get from module of `addr`, or the host ran its waiting init `fn`.
void seen(const void* addr, uint32_t api_version, bool waited);
// A waiting client the host did NOT run (it wants a newer API).
void skipped(const void* fn, uint32_t api_version);
// The init of a waiting client is running on this thread (nullptr = done): calls from the host's own module made
// meanwhile (a tail call in the client's init) are the client's.
void running_init(const void* fn);

// One registering call, after it returned `result` (a handle, 0 = ok, or a RG_E_*). `parent` = the handle the call
// was about (scope: the patch; node_use: the node; rt_variant: the library), 0 = none. `target` = what it attaches
// to (a shader hash, a node name, a register) - also the key for "several clients on one target".
void note(const void* ret_addr, Kind kind, int32_t result, rg_handle parent, const char* name, const char* target);
// Why the registering call now running on this thread fails - the core says it before returning the RG_E_*; note()
// takes it.
void reason(const char* why);
// Refused after registration (the first reason sticks).
void refused(Kind kind, rg_handle h, const char* why);

// The name a registration got (for a scope / use line), "" = none.
std::string name_of(Kind kind, rg_handle h);
std::string target_of(Kind kind, rg_handle h);
// The client (its module's file name) that made that registration, "" = unknown.
std::string client_of(Kind kind, rg_handle h);

// ---- ownership (docs/internals.md "Core rules"): a handle works only in the module that made it ----
// One handle space per kind of object (node_alloc returns a flow handle: the flow space).
enum Space : uint8_t {
  sPatch, sCompanion, sCbuffer, sLease, sView, sRtLib, sRtVariant, sPass, sSubscribe, sFlow, sNode, sWrap, sRemove,
  kSpaces
};
// 0 = the call returning to `ret` comes from the client that made handle h; RG_E_FOREIGN = another client's;
// RG_E_ARG = no such handle. A failure is logged (the first few) with `call` and handed to reason(). Lock-free: cheap
// enough for the [hot] entries (a scan of the clients' module ranges).
int32_t owner_check(const void* ret, Space s, rg_handle h, const char* call);

// The CET tab (tab-separated lines, format in clients.cpp) and a summary for the log / status report.
std::string report();
std::string summary();

}  // namespace core::clients
