// The render graph DEBUG module (graph_debug.hpp; docs/internals.md "Graph debug module"), [Diagnostics]
// RenderGraphDebug. Three parts, all on mechanisms already hardware-confirmed:
//   * the TREE - graph_probe walks the executing graph at each change of it (top-level nodes + group members, pre-order)
//     and hands it here with a key per node that survives a rebuild ("<parent key>/<name>#<n-th of that name among its
//     siblings>"); the CENSUS (graph_probe's 300-frame window: PSO binds and DispatchRays per node) gives each node
//     its shader sets. A census runs at every change of the graph and on request.
//   * NODE removal - M9 per node INSTANCE (render_nodes' debug slots: every class's slot [1] is wrapped at Register).
//     Wanted by key, so a removal outlives a graph rebuild and comes back with the graph. Refused: our own nodes,
//     job-builder nodes (their requests are not all issued on the node's thread), groups as such (a group = all its
//     members), structural nodes unless forced (render-target / constant binds, Synchronize, Present ...: their CPU
//     side effects are not RenderFlow requests, removing them can take the driver down).
//   * SHADER skip - a rule = a shader set (the stages a census row shows) + optionally a node: pso's bind funnel returns
//     NULL for a PSO carrying it (inside that node only, when scoped) - the engine's own "PSO still compiling" path,
//     which skips the draw / dispatch. Not for RT (a state object, not a PSO - render-graph.md "Next" 4). Work a node
//     hands to jobs is scoped to the node (render_flow wraps every job a node submits, 2026-10-08).

#include "core/graph_debug.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include "core/clients.hpp"
#include "core/internal.hpp"

namespace core::graph_debug {
namespace {

using graph_probe::DebugNode;
using graph_probe::DebugShaderRow;

std::atomic<bool> g_on{false};
std::mutex g_mu;  // everything below except the rules' render-side fields

uint64_t g_sig = 0;
std::string g_mode = "?";
std::vector<DebugNode> g_tree;
int32_t g_gen = 1;

struct Census {
  uint64_t frames = 0;
  std::vector<DebugShaderRow> rows;
};
std::unordered_map<uint64_t, Census> g_census;  // per graph signature
constexpr size_t kMaxCensus = 16;

std::set<std::string> g_removed;  // node keys

constexpr uint32_t kMaxRules = 32;
struct Rule {
  std::atomic<bool> live{false};
  uint8_t has = 0;  // stages that must match (rg_stage bits)
  uint8_t h[3][16] = {};
  std::atomic<const void*> node{nullptr};  // nullptr = everywhere; kAbsent = its node is not in the executing graph
  std::atomic<uint64_t> skipped{0};
  std::string node_key;  // under g_mu; "" = everywhere
};
Rule g_rules[kMaxRules];
std::atomic<uint32_t> g_nlive{0};
const void* const kAbsent = reinterpret_cast<const void*>(uintptr_t{1});

template <class... A>
void logf(const char* fmt, A... a) {
  g_sdk->logger->InfoF(g_handle, fmt, a...);
}

// Nodes whose job is state, not work: removing one leaves the engine's CPU-side state (bound targets, constants, the
// frame's bookkeeping) wrong for every node after it. Prefixes and whole names, as GetName returns them.
bool structural(const std::string& n) {
  static const char* const kPrefix[] = {"CRenderNode_Set", "CRenderNode_End", "CRenderNode_Bind", "CRenderNode_Unbind",
                                        "CRenderNode_AccelerationStructure", "CRenderNode_Init"};
  static const char* const kName[] = {
      "CRenderNode_Synchronize", "CRenderNode_Present", "CRenderNode_StartRender", "CRenderNode_DoCulling",
      "CRenderNode_DeclareCommonResourceAllocs", "CRenderNode_CameraResourceDependencyScope",
      "CRenderNode_PrepareSceneRendering", "CRenderNode_PrepareCollector", "CRenderNode_PrepareRenderElements",
      "CRenderNode_PrepareDynamicTextures", "CRenderNode_GlobalSystemsSetCommonUsage", "CRenderNode_PersistentCameraData",
      "CRenderNode_CleanupBatchDataAllocator", "CRenderNode_HairRenderTargets", "CRenderNode_PrepareDistantShadows",
      "CRenderNode_PrepareScreenSpaceWaterDepth"};
  for (const char* p : kPrefix)
    if (n.rfind(p, 0) == 0) return true;
  for (const char* x : kName)
    if (n == x) return true;
  return false;
}

// A node a client registered (node_register): the host inserted it, with the host's vtable - whatever its name.
rg_handle ours(const DebugNode& n) { return n.group ? 0 : render_nodes::debug_our_node(n.obj, n.vtable); }

std::string owner_of(rg_handle node) {
  const std::string c = node > 0 ? clients::client_of(clients::kNode, node) : "";
  return c.empty() ? "?" : c;
}

const Census* census_now() {
  const auto it = g_census.find(g_sig);
  return it == g_census.end() ? nullptr : &it->second;
}

// Under g_mu: the rule for (row's shader set, node key), or -1.
int32_t rule_of(const DebugShaderRow& r, const std::string& key) {
  for (uint32_t i = 0; i < kMaxRules; ++i) {
    const Rule& x = g_rules[i];
    if (!x.live.load() || x.has != r.has || x.node_key != key) continue;
    bool same = true;
    for (int st = 0; st < 3; ++st)
      if ((r.has & (1u << st)) && std::memcmp(x.h[st], r.h[st], 16) != 0) same = false;
    if (same) return static_cast<int32_t>(i);
  }
  return -1;
}

// Under g_mu: the node a rule's key names in the executing graph.
const void* resolve(const std::string& key) {
  if (key.empty()) return nullptr;
  for (const DebugNode& n : g_tree)
    if (n.key == key) return n.obj;
  return kAbsent;
}

// Under g_mu: one node's removal switch. "" = done.
std::string apply_node(const DebugNode& n, bool on, bool force) {
  if (n.group) return "a group (its members are switched one by one)";
  if (const rg_handle h = ours(n)) return "a node of a client (" + owner_of(h) + "), not the engine's";
  if (n.job) return "its work records in a job - M9 refuses it";
  if (!render_nodes::debug_removable(n.vtable)) return "its class is not wrapped";
  if (on && structural(n.name) && !force) return "structural node - tick 'allow structural' first";
  if (on) {
    if (!render_nodes::debug_remove(n.obj, true)) return "no free removal slot (32 in use)";
    g_removed.insert(n.key);
  } else {
    render_nodes::debug_remove(n.obj, false);
    g_removed.erase(n.key);
  }
  return "";
}

void hex32(char* out, const uint8_t* h, bool valid) {
  if (!valid) {
    out[0] = '-', out[1] = 0;
    return;
  }
  for (int i = 0; i < 16; ++i) std::snprintf(out + 2 * i, 3, "%02x", h[i]);
}

}  // namespace

void enable() {
  g_on.store(true);
  render_nodes::debug_enable();
}

bool on() { return g_on.load(std::memory_order_relaxed); }

// ---- from graph_probe (render thread, at ExecuteParallel's entry) ----

void on_tree(uint64_t sig, const char* mode, std::vector<DebugNode>&& nodes) {
  std::lock_guard lock(g_mu);
  g_tree = std::move(nodes);
  g_sig = sig;
  g_mode = mode ? mode : "?";
  ++g_gen;
  // The previous graph's objects may be freed and their memory reused by this one's: every slot goes, then the wanted
  // keys are mapped onto this graph's objects.
  render_nodes::debug_remove_clear();
  uint32_t mapped = 0, failed = 0;
  for (const DebugNode& n : g_tree)
    if (!n.group && g_removed.count(n.key)) (render_nodes::debug_remove(n.obj, true) ? mapped : failed) += 1;
  for (Rule& r : g_rules)
    if (r.live.load()) r.node.store(resolve(r.node_key), std::memory_order_release);
  static uint32_t logged = 0;  // a graph per frame (alternating builds) must not flood the log
  if (++logged <= 64)
    logf("graph debug: graph %016llx (%s) - %zu nodes; removals re-applied %u (failed %u, wanted %zu)%s",
         static_cast<unsigned long long>(sig), g_mode.c_str(), g_tree.size(), mapped, failed, g_removed.size(),
         logged == 64 ? " - the last such line" : "");
}

void on_census(uint64_t sig, uint64_t frames, std::vector<DebugShaderRow>&& rows) {
  std::lock_guard lock(g_mu);
  if (g_census.size() >= kMaxCensus && !g_census.count(sig)) g_census.erase(g_census.begin());
  Census& c = g_census[sig];
  c.frames = frames;
  c.rows = std::move(rows);
  ++g_gen;
}

// ---- from pso's bind funnel (render threads) ----

bool skip(uint32_t, const void* pso) {
  if (!g_nlive.load(std::memory_order_relaxed)) return false;
  graph_probe::PsoShaders s;
  if (!graph_probe::pso_shaders(pso, &s)) return false;
  const void* node = nullptr;
  bool node_read = false;
  for (Rule& r : g_rules) {
    if (!r.live.load(std::memory_order_acquire)) continue;
    bool match = r.has != 0;
    for (int st = 0; st < 3 && match; ++st)
      if ((r.has & (1u << st)) && (!(s.has & (1u << st)) || std::memcmp(r.h[st], s.h[st], 16) != 0)) match = false;
    if (!match) continue;
    if (const void* scope = r.node.load(std::memory_order_acquire)) {
      if (!node_read) node = render_flow::current_node(), node_read = true;
      if (scope != node) continue;
    }
    r.skipped.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
}

// ---- the UI ----

std::string report() {
  std::lock_guard lock(g_mu);
  std::string o;
  char b[512];
  uint64_t left = 0;
  const bool running = graph_probe::census_running(&left);
  const Census* c = census_now();
  char cs[64];
  if (running)
    std::snprintf(cs, sizeof cs, "running (%llu frames left)", static_cast<unsigned long long>(left));
  else if (c)
    std::snprintf(cs, sizeof cs, "%llu frames", static_cast<unsigned long long>(c->frames));
  else
    std::snprintf(cs, sizeof cs, "none");
  size_t here = 0;
  for (const DebugNode& n : g_tree)
    if (g_removed.count(n.key)) ++here;
  std::snprintf(b, sizeof b, "H\t%d\t%016llx\t%s\t%u\t%s\t%zu\t%zu\t%u\n", g_gen, static_cast<unsigned long long>(g_sig),
                g_mode.c_str(), render_nodes::debug_wrapped_classes(), cs, here, g_removed.size() - here,
                g_nlive.load());
  o += b;
  for (size_t i = 0; i < g_tree.size(); ++i) {
    const DebugNode& n = g_tree[i];
    char flags[8], *f = flags;
    if (n.group) *f++ = 'g';
    if (n.job) *f++ = 'j';
    if (structural(n.name)) *f++ = 's';
    const rg_handle own = ours(n);
    if (own) *f++ = 'o';
    if (!n.group && render_nodes::debug_removable(n.vtable)) *f++ = 'w';
    if (f == flags) *f++ = '-';
    *f = 0;
    rg_remove_stats st{};
    // removed: 1 = here (the window's), 2 = by a client's node_remove (the whole class; its counters are the class's)
    int removed = g_removed.count(n.key) ? 1 : 0;
    const rg_handle by = !removed && !n.group ? render_nodes::client_removal(n.name.c_str()) : 0;
    if (removed) render_nodes::debug_remove_stats(n.obj, &st);
    else if (by) removed = 2, render_nodes::node_remove_stats(by, &st);
    const std::string who = own ? owner_of(own) : by ? clients::client_of(clients::kRemove, by) : "-";
    std::snprintf(b, sizeof b, "N\t%zu\t%d\t%u\t%s\t%d\t%s\t%s\t%llu\t%llu\t%llu\t%s\n", i, n.parent, n.depth, flags,
                  removed, n.name.c_str(), n.key.c_str(), static_cast<unsigned long long>(st.removed),
                  static_cast<unsigned long long>(st.ran), static_cast<unsigned long long>(st.refused),
                  who.empty() ? "?" : who.c_str());
    o += b;
  }
  if (c)
    for (size_t i = 0; i < c->rows.size(); ++i) {
      const DebugShaderRow& r = c->rows[i];
      char hx[3][40];
      for (int st = 0; st < 3; ++st) hex32(hx[st], r.h[st], (r.has & (1u << st)) != 0);
      const std::string key = r.node >= 0 && static_cast<size_t>(r.node) < g_tree.size() ? g_tree[r.node].key : "";
      const int32_t rh = r.pipe != 2 && !key.empty() ? rule_of(r, key) : -1;
      const int32_t ra = r.pipe != 2 ? rule_of(r, "") : -1;
      std::snprintf(b, sizeof b, "S\t%zu\t%d\t%u\t%s\t%s\t%s\t%llu\t%u\t%d\t%d\t%llu\t%llu\t%s\n", i, r.node, r.pipe,
                    hx[0], hx[1], hx[2], static_cast<unsigned long long>(r.binds), r.psos, rh >= 0 ? 1 : 0,
                    ra >= 0 ? 1 : 0, rh >= 0 ? static_cast<unsigned long long>(g_rules[rh].skipped.load()) : 0ull,
                    ra >= 0 ? static_cast<unsigned long long>(g_rules[ra].skipped.load()) : 0ull, r.rt.c_str());
      o += b;
    }
  for (uint32_t i = 0; i < kMaxRules; ++i) {
    const Rule& r = g_rules[i];
    if (!r.live.load()) continue;
    char hx[3][40];
    for (int st = 0; st < 3; ++st) hex32(hx[st], r.h[st], (r.has & (1u << st)) != 0);
    std::snprintf(b, sizeof b, "R\t%u\t%s\t%s\t%s\t%s\t%llu\n", i, hx[0], hx[1], hx[2],
                  r.node_key.empty() ? "*" : r.node_key.c_str(), static_cast<unsigned long long>(r.skipped.load()));
    o += b;
  }
  return o;
}

std::string set_node(int32_t gen, int32_t index, bool on, bool force) {
  std::lock_guard lock(g_mu);
  if (gen != g_gen) return "stale - the graph or its census changed; refreshed";
  if (index < 0 || static_cast<size_t>(index) >= g_tree.size()) return "no such node";
  const DebugNode& n = g_tree[index];
  if (!n.group) {
    const std::string why = apply_node(n, on, force);
    logf("graph debug: node '%s' %s%s%s", n.key.c_str(), why.empty() ? (on ? "REMOVED" : "restored") : "NOT ",
         why.empty() ? "" : (on ? "removed - " : "restored - "), why.c_str());
    return why;
  }
  // A group: its members (pre-order: every following node deeper than the group).
  uint32_t done = 0, refused = 0;
  std::string first_why;
  for (size_t i = static_cast<size_t>(index) + 1; i < g_tree.size() && g_tree[i].depth > n.depth; ++i) {
    if (g_tree[i].group) continue;
    const std::string why = apply_node(g_tree[i], on, force);
    if (why.empty())
      ++done;
    else if (++refused == 1)
      first_why = g_tree[i].name + ": " + why;
  }
  logf("graph debug: group '%s' %s - %u members, %u refused%s%s", n.key.c_str(), on ? "REMOVED" : "restored", done,
       refused, refused ? " (first: " : "", refused ? (first_why + ")").c_str() : "");
  char b[256];
  if (!refused) return "";
  std::snprintf(b, sizeof b, "%u members switched, %u refused (first: %s)", done, refused, first_why.c_str());
  return b;
}

std::string set_shader(int32_t gen, int32_t row, int32_t where, bool on) {
  std::lock_guard lock(g_mu);
  if (gen != g_gen) return "stale - the graph or its census changed; refreshed";
  const Census* c = census_now();
  if (!c || row < 0 || static_cast<size_t>(row) >= c->rows.size()) return "no such shader row";
  const DebugShaderRow& r = c->rows[row];
  if (r.pipe == 2) return "a DispatchRays (an RT state object, not a PSO) - cannot be skipped here";
  if (!r.has) return "no hashed stage";
  std::string key;
  if (where == 1) {
    if (r.node < 0 || static_cast<size_t>(r.node) >= g_tree.size()) return "outside any node - only 'everywhere'";
    key = g_tree[r.node].key;
  } else if (where != 2) {
    return "where: 1 = in its node, 2 = everywhere";
  }
  const int32_t have = rule_of(r, key);
  if (!on) {
    if (have >= 0) {
      g_rules[have].live.store(false, std::memory_order_release);
      g_nlive.fetch_sub(1);
    }
    logf("graph debug: shader rule off (%s)", key.empty() ? "everywhere" : key.c_str());
    return "";
  }
  if (have >= 0) return "";
  for (uint32_t i = 0; i < kMaxRules; ++i) {
    Rule& x = g_rules[i];
    if (x.live.load()) continue;
    x.has = r.has;
    std::memcpy(x.h, r.h, sizeof x.h);
    x.node_key = key;
    x.skipped.store(0);
    x.node.store(resolve(key), std::memory_order_release);
    x.live.store(true, std::memory_order_release);
    g_nlive.fetch_add(1);
    char hx[3][40];
    for (int st = 0; st < 3; ++st) hex32(hx[st], r.h[st], (r.has & (1u << st)) != 0);
    logf("graph debug: SKIP vs %s ps %s cs %s - %s", hx[0], hx[1], hx[2], key.empty() ? "everywhere" : key.c_str());
    return "";
  }
  return "no free rule (32 in use)";
}

std::string rule_off(int32_t rule) {
  std::lock_guard lock(g_mu);
  if (rule < 0 || static_cast<uint32_t>(rule) >= kMaxRules || !g_rules[rule].live.load()) return "no such rule";
  g_rules[rule].live.store(false, std::memory_order_release);
  g_nlive.fetch_sub(1);
  logf("graph debug: shader rule %d off", rule);
  return "";
}

void census() { graph_probe::census_request(); }

void reset() {
  std::lock_guard lock(g_mu);
  for (const DebugNode& n : g_tree)
    if (!n.group && g_removed.count(n.key)) render_nodes::debug_remove(n.obj, false);
  g_removed.clear();
  for (Rule& r : g_rules)
    if (r.live.exchange(false)) g_nlive.fetch_sub(1);
  logf("graph debug: RESET - every removal and skip off");
}

}  // namespace core::graph_debug
