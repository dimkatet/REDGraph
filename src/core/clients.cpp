// clients.hpp. Report format (one record per line, tab-separated; cet/REDGraph/init.lua parses it):
//   C  index  module  api  how(direct|waited|skipped)  registrations  refused
//   E  client  kind  handle-or-result  name  target  state(ok|pending|REFUSED|ERROR <code>)  live  why
//   X  kind  target  clients(", ")          - one target registered by more than one client

#include "core/clients.hpp"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "core/internal.hpp"

namespace core::clients {
namespace {

constexpr uint32_t kMaxClients = 32;
constexpr uint32_t kMaxEntries = 1024;

const char* const kKindName[kKinds] = {
    "patch", "patch scope", "companion", "companion scope", "lease", "cbuffer", "cbuffer feed", "view", "RT library",
    "RT variant", "pass", "subscription", "device_ready", "flow", "node", "node use", "node alloc", "wrap", "remove"};

struct Client {
  HMODULE module;
  char name[64];
  uint32_t api;
  uint8_t how;  // 0 direct, 1 waited, 2 skipped
};

struct Entry {
  uint8_t client;
  Kind kind;
  int32_t result;
  rg_handle parent;
  char name[64];
  char target[80];
  char why[120];
};

std::mutex g_mu;
Client g_clients[kMaxClients];
uint32_t g_nclients = 0;
Entry g_entries[kMaxEntries];
uint32_t g_nentries = 0;
uint32_t g_dropped = 0;
thread_local const void* t_init = nullptr;
thread_local char t_why[120] = "";

// Ownership: the owner of every handle (client index + 1, 0 = none) and each client's module image, readable without
// the mutex (written under it during registration, published by the release store of g_nranges / the owner byte).
constexpr uint32_t kMaxHandle = 1024;
std::atomic<uint8_t> g_owner[kSpaces][kMaxHandle + 1];
struct Range {
  uintptr_t lo, hi;
};
Range g_range[kMaxClients];
std::atomic<uint32_t> g_nranges{0};
std::atomic<uint32_t> g_foreign_logged{0};

Range image_of(HMODULE m) {
  if (!m) return {0, 0};
  const auto* base = reinterpret_cast<const uint8_t*>(m);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  return {reinterpret_cast<uintptr_t>(base), reinterpret_cast<uintptr_t>(base) + nt->OptionalHeader.SizeOfImage};
}

HMODULE self_module() {
  static HMODULE self = [] {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&self_module), &m);
    return m;
  }();
  return self;
}

HMODULE module_of(const void* addr) {
  HMODULE m = nullptr;
  if (!addr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(addr), &m))
    return nullptr;
  return m;
}

void copy(char* dst, size_t cap, const char* src) {
  std::snprintf(dst, cap, "%s", src ? src : "");
  for (char* p = dst; *p; ++p)
    if (*p == '\t' || *p == '\n' || *p == '\r' || *p == '%') *p = ' ';  // the report's separators; CET's format strings
}

// Caller holds g_mu. The client of module m, added on first sight; -1 = the table is full.
int32_t client_of(HMODULE m) {
  for (uint32_t i = 0; i < g_nclients; ++i)
    if (g_clients[i].module == m) return static_cast<int32_t>(i);
  if (g_nclients >= kMaxClients) return -1;
  Client& c = g_clients[g_nclients];
  c = {};
  c.module = m;
  wchar_t path[MAX_PATH];
  const DWORD n = m ? GetModuleFileNameW(m, path, MAX_PATH) : 0;
  if (n && n < MAX_PATH) {
    const wchar_t* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    size_t k = 0;
    for (; base[k] && base[k] != L'.' && k + 1 < sizeof c.name; ++k)
      c.name[k] = base[k] < 0x80 ? static_cast<char>(base[k]) : '?';
    c.name[k] = 0;
  } else {
    copy(c.name, sizeof c.name, "(unknown module)");
  }
  g_range[g_nclients] = image_of(m);
  g_nranges.store(g_nclients + 1, std::memory_order_release);
  return static_cast<int32_t>(g_nclients++);
}

Space space_of(Kind k) {
  switch (k) {
    case kPatch: return sPatch;
    case kCompanion: return sCompanion;
    case kCbuffer: return sCbuffer;
    case kLease: return sLease;
    case kView: return sView;
    case kRtLib: return sRtLib;
    case kRtVariant: return sRtVariant;
    case kPass: return sPass;
    case kSubscribe: return sSubscribe;
    case kFlow: case kNodeAlloc: return sFlow;
    case kNode: return sNode;
    case kWrap: return sWrap;
    case kRemove: return sRemove;
    default: return kSpaces;
  }
}

// Caller holds g_mu. Which client a call returning to `ret` belongs to.
int32_t caller(const void* ret) {
  HMODULE m = module_of(ret);
  if (m == self_module() && t_init) m = module_of(t_init);  // a tail call out of a waiting client's init
  return client_of(m);
}

Entry* find(Kind kind, rg_handle h) {
  for (uint32_t i = 0; i < g_nentries; ++i)
    if (g_entries[i].kind == kind && g_entries[i].result == h && h > 0) return &g_entries[i];
  return nullptr;
}

const char* error_name(int32_t r) {
  switch (r) {
    case RG_E_ARG: return "ARG";
    case RG_E_TAKEN: return "TAKEN";
    case RG_E_FULL: return "FULL";
    case RG_E_UNSUPPORTED: return "UNSUPPORTED";
    case RG_E_LATE: return "LATE";
    case RG_E_BLOB: return "BLOB";
    case RG_E_FOREIGN: return "FOREIGN";
    default: return "?";
  }
}

// Registrations that return a handle (> 0); the others (scope, feed, use, device_ready) return 0 / an index on success.
bool returns_handle(Kind k) {
  return k != kPatchScope && k != kCompanionScope && k != kFeed && k != kNodeUse && k != kDeviceReady;
}

bool failed(const Entry& e) { return e.result < 0; }

const char* state_name(uint32_t s) {
  return s == RG_STATE_OK ? "ok" : s == RG_STATE_REFUSED ? "REFUSED" : "pending";
}

// The entry's state and live counters, read from the core now. Caller holds g_mu (the stats calls take no lock of ours).
void live(const Entry& e, std::string* state, std::string* counters) {
  char b[200] = "";
  if (failed(e)) {
    *state = std::string("ERROR ") + error_name(e.result);
    return;
  }
  *state = e.why[0] ? "REFUSED" : "ok";
  const rg_handle h = e.result;
  switch (e.kind) {
    case kPatch: {
      rg_patch_stats s{};
      pso::patch_stats(h, &s);
      *state = state_name(s.state);
      std::snprintf(b, sizeof b, "variant %d, engine PSOs %u, swaps %llu, out of scope %llu", s.variant, s.engine_psos,
                    static_cast<unsigned long long>(s.swaps), static_cast<unsigned long long>(s.out_of_scope));
      break;
    }
    case kCompanion: {
      rg_patch_stats s{};
      pso::companion_stats(h, &s);
      *state = state_name(s.state);
      std::snprintf(b, sizeof b, "engine PSOs %u, dispatches %llu, out of scope %llu", s.engine_psos,
                    static_cast<unsigned long long>(s.swaps), static_cast<unsigned long long>(s.out_of_scope));
      break;
    }
    case kLease: {
      rg_lease_stats s{};
      bindings::lease_stats(h, &s);
      *state = state_name(s.state);
      std::snprintf(b, sizeof b, "staged %llu, not staged %llu%s", static_cast<unsigned long long>(s.stages),
                    static_cast<unsigned long long>(s.not_staged), s.ready ? "" : ", NOT READY");
      break;
    }
    case kRtLib: {
      rg_rtlib_stats s{};
      rtlib::stats(h, &s);
      *state = state_name(s.state);
      std::snprintf(b, sizeof b, "swapped into %u, added to %u state object(s)%s", s.swapped, s.added,
                    s.failed ? ", creations FAILED with it" : "");
      break;
    }
    case kRtVariant: {
      rg_rt_variant_stats s{};
      rtlib::variant_stats(h, &s);
      std::snprintf(b, sizeof b, "%s, ran %llu, out of scope %llu, off %llu%s", s.on ? "ON" : "off",
                    static_cast<unsigned long long>(s.swapped), static_cast<unsigned long long>(s.out_of_scope),
                    static_cast<unsigned long long>(s.off), s.missing ? ", MISSING in its state object" : "");
      break;
    }
    case kNode: {
      rg_node_stats s{};
      render_nodes::node_stats(h, &s);
      std::snprintf(b, sizeof b, "inserted %u, runs %llu%s", s.inserted, static_cast<unsigned long long>(s.executes),
                    s.skipped_toplevel ? ", anchor is top-level: NOT inserted" : "");
      break;
    }
    case kWrap: {
      rg_wrap_stats s{};
      render_nodes::node_wrap_stats(h, &s);
      std::snprintf(b, sizeof b, "%s, runs %llu", s.wrapped ? "wrapped" : "target not seen yet",
                    static_cast<unsigned long long>(s.executes));
      break;
    }
    case kRemove: {
      rg_remove_stats s{};
      render_nodes::node_remove_stats(h, &s);
      std::snprintf(b, sizeof b, "%s, removed %llu, ran %llu%s", s.on ? "ON" : "off",
                    static_cast<unsigned long long>(s.removed), static_cast<unsigned long long>(s.ran),
                    s.refused ? ", REFUSED (a group / job-builder node)" : "");
      break;
    }
    case kFlow: {
      rg_flow_stats s{};
      render_flow::flow_stats(h, &s);
      std::snprintf(b, sizeof b, "hits %llu, misses %llu", static_cast<unsigned long long>(s.hits),
                    static_cast<unsigned long long>(s.misses));
      break;
    }
    default:
      break;
  }
  *counters = b;
}

// Targets several clients share: only what can collide (one engine shader / raygen / node, one register).
bool collides(Kind k) {
  return k == kPatch || k == kCompanion || k == kRtLib || k == kRtVariant || k == kSubscribe || k == kWrap ||
         k == kRemove || k == kNode || k == kLease;
}

// The family a target belongs to, so "a wrap and a remove on one node" counts as one target.
const char* family(Kind k) {
  switch (k) {
    case kWrap: case kRemove: case kNode: return "node";
    case kPatch: case kCompanion: case kSubscribe: return "shader";
    case kRtLib: case kRtVariant: return "RT";
    default: return kKindName[k];
  }
}

}  // namespace

void seen(const void* addr, uint32_t api_version, bool waited) {
  std::lock_guard<std::mutex> g(g_mu);
  const HMODULE m = module_of(addr);
  if (m == self_module()) return;  // our own CET natives read the table too
  const int32_t c = client_of(m);
  if (c < 0) return;
  g_clients[c].api = api_version;
  if (waited) g_clients[c].how = 1;
}

void skipped(const void* fn, uint32_t api_version) {
  std::lock_guard<std::mutex> g(g_mu);
  const int32_t c = client_of(module_of(fn));
  if (c < 0) return;
  g_clients[c].api = api_version;
  g_clients[c].how = 2;
}

void running_init(const void* fn) { t_init = fn; }

void note(const void* ret_addr, Kind kind, int32_t result, rg_handle parent, const char* name, const char* target) {
  std::lock_guard<std::mutex> g(g_mu);
  const int32_t c = caller(ret_addr);
  const Space sp = space_of(kind);
  if (c >= 0 && result > 0 && returns_handle(kind) && sp != kSpaces && static_cast<uint32_t>(result) <= kMaxHandle)
    g_owner[sp][result].store(static_cast<uint8_t>(c + 1), std::memory_order_release);  // before the table-full check
  if (c < 0 || g_nentries >= kMaxEntries) {
    t_why[0] = 0;
    ++g_dropped;
    return;
  }
  Entry& e = g_entries[g_nentries++];
  e = {};
  e.client = static_cast<uint8_t>(c);
  e.kind = kind;
  e.result = returns_handle(kind) || result < 0 ? result : 0;
  e.parent = parent;
  copy(e.name, sizeof e.name, name);
  copy(e.target, sizeof e.target, target);
  if (result < 0) copy(e.why, sizeof e.why, t_why);
  t_why[0] = 0;
}

void reason(const char* why) { copy(t_why, sizeof t_why, why); }

int32_t owner_check(const void* ret, Space s, rg_handle h, const char* call) {
  static const Range self = image_of(self_module());
  static const char* const kSpaceName[kSpaces] = {"patch", "companion", "cbuffer", "lease", "view", "RT library",
                                                   "RT variant", "pass", "subscription", "flow", "node", "wrap",
                                                   "remove"};
  const uint8_t owner = s < kSpaces && h > 0 && static_cast<uint32_t>(h) <= kMaxHandle
                            ? g_owner[s][h].load(std::memory_order_acquire)
                            : 0;
  uintptr_t a = reinterpret_cast<uintptr_t>(ret);
  if (a >= self.lo && a < self.hi && t_init) a = reinterpret_cast<uintptr_t>(t_init);  // a tail call out of an init
  int32_t c = -1;
  const uint32_t n = g_nranges.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n && c < 0; ++i)
    if (a >= g_range[i].lo && a < g_range[i].hi) c = static_cast<int32_t>(i);
  if (owner && c + 1 == owner) return 0;
  const int32_t r = owner ? RG_E_FOREIGN : RG_E_ARG;
  char why[200];
  if (owner)
    std::snprintf(why, sizeof why, "%s: %s #%d belongs to %s - a handle works only in the module that made it", call,
                  kSpaceName[s], h, g_clients[owner - 1].name);
  else
    std::snprintf(why, sizeof why, "%s: no %s #%d", call, s < kSpaces ? kSpaceName[s] : "?", h);
  copy(t_why, sizeof t_why, why);
  if (g_foreign_logged.fetch_add(1, std::memory_order_relaxed) < 16)
    g_sdk->logger->ErrorF(g_handle, "host: %s (called from %s)%s", why, c >= 0 ? g_clients[c].name : "an unknown module",
                          g_foreign_logged.load() == 16 ? " - further ones not logged" : "");
  return r;
}

void refused(Kind kind, rg_handle h, const char* why) {
  std::lock_guard<std::mutex> g(g_mu);
  Entry* e = find(kind, h);
  if (e && !e->why[0]) copy(e->why, sizeof e->why, why);
}

std::string name_of(Kind kind, rg_handle h) {
  std::lock_guard<std::mutex> g(g_mu);
  const Entry* e = find(kind, h);
  return e ? e->name : "";
}

std::string target_of(Kind kind, rg_handle h) {
  std::lock_guard<std::mutex> g(g_mu);
  const Entry* e = find(kind, h);
  return e ? e->target : "";
}

std::string client_of(Kind kind, rg_handle h) {
  std::lock_guard<std::mutex> g(g_mu);
  const Entry* e = find(kind, h);
  return e ? g_clients[e->client].name : "";
}

std::string report() {
  std::lock_guard<std::mutex> g(g_mu);
  std::string o;
  char line[640];
  for (uint32_t c = 0; c < g_nclients; ++c) {
    uint32_t regs = 0, bad = 0;
    for (uint32_t i = 0; i < g_nentries; ++i) {
      if (g_entries[i].client != c) continue;
      ++regs;
      if (failed(g_entries[i]) || g_entries[i].why[0]) ++bad;
    }
    static const char* const kHow[] = {"direct", "waited", "skipped"};
    std::snprintf(line, sizeof line, "C\t%u\t%s\t%u\t%s\t%u\t%u\n", c, g_clients[c].name, g_clients[c].api,
                  kHow[g_clients[c].how], regs, bad);
    o += line;
  }
  for (uint32_t i = 0; i < g_nentries; ++i) {
    const Entry& e = g_entries[i];
    std::string st, counters;
    live(e, &st, &counters);
    std::snprintf(line, sizeof line, "E\t%u\t%s\t%d\t%s\t%s\t%s\t%s\t%s\n", e.client, kKindName[e.kind], e.result,
                  e.name, e.target, st.c_str(), counters.c_str(), e.why);
    o += line;
  }
  // Shared targets: the first entry of each (family, target) with another client on it.
  for (uint32_t i = 0; i < g_nentries; ++i) {
    const Entry& a = g_entries[i];
    if (!collides(a.kind) || !a.target[0]) continue;
    bool first = true, other = false;
    for (uint32_t j = 0; j < g_nentries && first; ++j) {
      const Entry& b = g_entries[j];
      if (!collides(b.kind) || std::strcmp(family(a.kind), family(b.kind)) != 0 || std::strcmp(a.target, b.target) != 0)
        continue;
      if (j < i) first = false;
      if (b.client != a.client) other = true;
    }
    if (!first || !other) continue;
    std::string who;
    uint64_t listed = 0;
    for (uint32_t j = i; j < g_nentries; ++j) {
      const Entry& b = g_entries[j];
      if (!collides(b.kind) || std::strcmp(family(a.kind), family(b.kind)) != 0 || std::strcmp(a.target, b.target) != 0 ||
          (listed >> b.client & 1))
        continue;
      listed |= 1ull << b.client;
      if (!who.empty()) who += ", ";
      who += g_clients[b.client].name;
    }
    std::snprintf(line, sizeof line, "X\t%s\t%s\t%s\n", family(a.kind), a.target, who.c_str());
    o += line;
  }
  if (g_dropped) {
    std::snprintf(line, sizeof line, "D\t%u\n", g_dropped);
    o += line;
  }
  return o;
}

std::string summary() {
  std::lock_guard<std::mutex> g(g_mu);
  std::string o;
  char line[200];
  for (uint32_t c = 0; c < g_nclients; ++c) {
    uint32_t regs = 0, bad = 0;
    for (uint32_t i = 0; i < g_nentries; ++i) {
      if (g_entries[i].client != c) continue;
      ++regs;
      if (failed(g_entries[i]) || g_entries[i].why[0]) ++bad;
    }
    std::snprintf(line, sizeof line, "%s%s (API %u%s): %u registration(s), %u refused", o.empty() ? "" : "; ",
                  g_clients[c].name, g_clients[c].api,
                  g_clients[c].how == 1 ? ", waited" : g_clients[c].how == 2 ? ", SKIPPED - wants a newer host" : "",
                  regs, bad);
    o += line;
  }
  return o.empty() ? "no client" : o;
}

}  // namespace core::clients
