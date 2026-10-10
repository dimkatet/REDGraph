// The engine's render graph, READ ONLY ([Diagnostics] RenderGraphDump): every distinct CRenderNodeGraph that
// CRenderInterface::RenderFrame executes is written once to render-graph/graph-NNN.txt next to the DLL - its nodes
// (class name from the node's own GetName, vtable / Execute RVAs) and its dependency edges. Nothing is changed.
//
// Layout, 2.31 (static reading of AddNode / AddDependency / TRenderNodeGraphArray::Allocate / RemoveNode /
// RemoveDependency / BuildRenderFlowGroups, 2026-10-07; docs/internals.md):
//   CRenderNodeGraph: two sparse sets, element data + a dense list of live element indices (removal swaps with last)
//     nodes: +0x10 u32 live count, +0x18 SNodeData* data, +0x24 u32 data size, +0x28 u32* live list
//     deps:  +0x40 u32 live count, +0x48 SDepData* data, +0x54 u32 data size, +0x58 u32* live list
//   SNodeData (48 B): +0 dense pos (-1 free), +8 u32 type, +0xc u32 subtype, +0x10 CRenderNodeBase*, +0x18 u32,
//     +0x1c u16 flow group (BuildRenderFlowGroups), +0x20 in-list head[2], +0x28 out-list head[2] (per dep type)
//   SDepData (24 B): +0 dense pos, +4 type (0/1), +8 from handle, +0xc to handle, +0x10 next out of from,
//     +0x14 next in of to; handles: node | 1<<29, dep | 1<<30, 0x80000000 = none
//   CRenderNodeBase vtable: [1] Execute(ctx, job::Builder*), [2] GetName(CName* out) - checked on all 167 classes.
//
// The SHADER CENSUS: for kWindow frames after each new graph, every PSO bind (pso's bind funnel - state changes, not
// every draw) is paired with the node running on that thread (render_flow's CRenderNodeBase::Process hook, nested for
// group members) and written to graph-NNN-shaders.txt: node -> the shader hashes of the PSOs it binds, bind counts. The PSO's
// hashes come from pso's Create detours (every PSO, both kinds, while the census is on). Binds outside any Process
// (work a node handed to a job) are counted as "(no node)". RT: every DispatchRays (the global function all RT nodes
// call) is a row too - `rt=<library hash> rg:<raygen record> so:<state-object enum> site:<call site RVA> grid:<W x H
// range>`, the library from rtlib's creator detour by the state object's container entry; binds = dispatches. One row
// per CALL SITE in the node's Execute (a node has up to 12); the grid is a range (dynamic resolution moves it).
//
// The RESOURCE census, same window: every RenderFlow request (SRenderFlowInternalData::HandleRequest - the one funnel
// of RTAlloc / RTTempAlloc / the access declarations, 524 call sites) paired with the node, written to
// graph-NNN-resources.txt: node, request type, resource name hash, the request's u32 parameter (or the alloc's
// SRenderFlowTargetDesc), the flow phase, count. SRequest: +0 u8 type, +1 u8 (ctx+0x38), +4 u32 name hash, +8 u32
// param / 40 B desc (type 1).

#include <Windows.h>
#include <d3d12.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <RED4ext/CName.hpp>

#include "core/engine_231.hpp"
#include "core/internal.hpp"
#include "core/prof.hpp"
#include "dxil_hash.hpp"

namespace core::graph_probe {

namespace {

using namespace e231;

using ExecuteParallel_t = void (*)(const void* graph, const void* ctx, void* builder);
using GetName_t = RED4ext::CName* (*)(const void* node, RED4ext::CName* out);

ExecuteParallel_t g_orig = nullptr;
bool g_attached = false;
std::mutex g_mu;
uint64_t g_frame = 0;

constexpr uint32_t kMaxDumps = 64;
constexpr uint32_t kSeen = 64;
uint64_t g_seen[kSeen];
int32_t g_seen_dump[kSeen];  // its dump index, -1 = seen after the dump cap
uint32_t g_nseen = 0, g_ndumps = 0;

// ---- the shader census ----
std::atomic<bool> g_census{false};
std::atomic<bool> g_window{false};
uint64_t g_window_end = 0, g_window_start = 0;  // under g_mu
int32_t g_window_graph = -1;  // the dump index the open window belongs to, -1 = no files (graph not dumped)
uint64_t g_window_sig = 0;
constexpr uint64_t kWindow = 300;
std::atomic<bool> g_census_req{false};  // the debug module asked for a window on the executing graph
uint64_t g_cur_sig = 0;                 // the executing graph (under g_mu)
int32_t g_cur_dump = -1;                // its dump index, -1 = not dumped

struct Shaders {
  dxil_hash::Hash h[3];  // by rg_stage: VS, PS, CS
  uint8_t has = 0;       // bit s = h[s] valid
  uint8_t pipe = 0;      // 0 compute, 1 graphics
};
SRWLOCK g_pso_lock = SRWLOCK_INIT;
std::unordered_map<uintptr_t, Shaders> g_psos;

struct PairKey {
  uintptr_t node, pso;
  bool operator==(const PairKey& o) const { return node == o.node && pso == o.pso; }
};
struct PairHash {
  size_t operator()(const PairKey& k) const { return (k.node * 0x9E3779B97F4A7C15ull) ^ (k.pso >> 4); }
};
SRWLOCK g_pair_lock = SRWLOCK_INIT;
std::unordered_map<PairKey, uint64_t, PairHash> g_pairs;

// ---- binds outside any node: WHO records them (2026-10-08, render-graph.md "Next" 3) ----
// Every 4th such bind per thread captures its return-address chain; the frames inside the game's exe are kept (ours and
// the hook trampolines dropped). Written to graph-NNN-nonode.txt: shader set, samples, the chain - symbolise it offline
// (scripts/exe_re.py up / func). Census window only.
constexpr uint32_t kChain = 12, kChainSample = 4;
struct ChainKey {
  uintptr_t pso;
  uint32_t n;
  uint32_t rva[kChain];
  bool operator==(const ChainKey& o) const {
    return pso == o.pso && n == o.n && std::memcmp(rva, o.rva, n * sizeof rva[0]) == 0;
  }
};
struct ChainHash {
  size_t operator()(const ChainKey& k) const {
    uint64_t h = k.pso >> 4;
    for (uint32_t i = 0; i < k.n; ++i) h = (h ^ k.rva[i]) * 0x100000001B3ull;
    return h;
  }
};
SRWLOCK g_chain_lock = SRWLOCK_INIT;
std::unordered_map<ChainKey, uint64_t, ChainHash> g_chains;
uintptr_t g_exe_end = 0;
thread_local uint32_t t_chain_tick = 0;

void note_chain(uintptr_t pso) {
  if (t_chain_tick++ % kChainSample) return;
  void* frames[32];
  const USHORT got = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
  ChainKey k{pso, 0, {}};
  for (USHORT i = 0; i < got && k.n < kChain; ++i) {
    const auto a = reinterpret_cast<uintptr_t>(frames[i]);
    if (a >= g_base && a < g_exe_end) k.rva[k.n++] = static_cast<uint32_t>(a - g_base);
  }
  AcquireSRWLockExclusive(&g_chain_lock);
  ++g_chains[k];
  ReleaseSRWLockExclusive(&g_chain_lock);
}

// ---- RT: state object (container entry) -> library; dispatches per node ----
struct StateObject {
  dxil_hash::Hash hash;
  std::vector<std::string> exports;
};
SRWLOCK g_so_lock = SRWLOCK_INIT;
std::unordered_map<uintptr_t, StateObject> g_sos;
struct RtKey {
  uintptr_t node, entry;
  uint32_t so, rg, site;
  bool operator==(const RtKey& o) const {
    return node == o.node && entry == o.entry && so == o.so && rg == o.rg && site == o.site;
  }
};
struct RtKeyHash {
  size_t operator()(const RtKey& k) const {
    return (k.node * 0x9E3779B97F4A7C15ull) ^ (k.entry >> 3) ^ (uint64_t(k.so) << 40) ^ (uint64_t(k.rg) << 32) ^
           k.site;
  }
};
struct RtVal {
  uint64_t n = 0;
  uint32_t w0 = ~0u, w1 = 0, h0 = ~0u, h1 = 0;
};
SRWLOCK g_rt_lock = SRWLOCK_INIT;
std::unordered_map<RtKey, RtVal, RtKeyHash> g_rts;



struct ReqKey {
  uintptr_t node;
  uint32_t hash, param;
  uint8_t type, sub, phase;
  bool operator==(const ReqKey& o) const {
    return node == o.node && hash == o.hash && param == o.param && type == o.type && sub == o.sub && phase == o.phase;
  }
};
struct ReqKeyHash {
  size_t operator()(const ReqKey& k) const {
    return (k.node * 0x9E3779B97F4A7C15ull) ^ (uint64_t(k.hash) << 20) ^ (uint64_t(k.param) * 31) ^
           (uint64_t(k.type) << 8 | uint64_t(k.sub) << 16 | k.phase);
  }
};
struct ReqVal {
  uint64_t count = 0;
  uint8_t desc[40] = {};  // type 1: the SRenderFlowTargetDesc
};
SRWLOCK g_req_lock = SRWLOCK_INIT;
std::unordered_map<ReqKey, ReqVal, ReqKeyHash> g_reqs;

// The executing graph's node objects -> order + label + index in its debug tree (filled by walk, under g_mu).
struct Label {
  uint32_t order;
  std::string text;
  int32_t entry;
};
std::unordered_map<uintptr_t, Label> g_labels;

template <class T>
T rd(const uint8_t* p, uintptr_t off) {
  T v;
  std::memcpy(&v, p + off, sizeof v);
  return v;
}

struct View {
  const uint8_t* g;
  uint32_t n_live, n_size, d_live, d_size;
  const uint8_t *n_data, *d_data;
  const uint32_t *n_list, *d_list;
};

bool view_of(const void* graph, View* v) {
  const auto* g = static_cast<const uint8_t*>(graph);
  if (!g) return false;
  v->g = g;
  v->n_live = rd<uint32_t>(g, kGraphNodeLive);
  v->n_data = rd<const uint8_t*>(g, kGraphNodeData);
  v->n_size = rd<uint32_t>(g, kGraphNodeSize);
  v->n_list = rd<const uint32_t*>(g, kGraphNodeList);
  v->d_live = rd<uint32_t>(g, kGraphDepLive);
  v->d_data = rd<const uint8_t*>(g, kGraphDepData);
  v->d_size = rd<uint32_t>(g, kGraphDepSize);
  v->d_list = rd<const uint32_t*>(g, kGraphDepList);
  // Sanity, not a guard against bad memory (no SEH reads - the engine's crash handler freezes the game on any first-chance AV): counts that
  // contradict each other mean the layout is not what we read, and we stop before following a pointer.
  if (v->n_live > v->n_size || v->d_live > v->d_size || v->n_size > 65536 || v->d_size > 262144) return false;
  if (v->n_live && (!v->n_data || !v->n_list)) return false;
  if (v->d_live && (!v->d_data || !v->d_list)) return false;
  return true;
}

const uint8_t* node_at(const View& v, uint32_t elem) { return v.n_data + size_t(elem) * kNodeStride; }
const uint8_t* dep_at(const View& v, uint32_t elem) { return v.d_data + size_t(elem) * kDepStride; }

uint64_t fnv(uint64_t h, uint64_t x) {
  for (int i = 0; i < 8; ++i) h = (h ^ ((x >> (i * 8)) & 0xff)) * 0x100000001b3ull;
  return h;
}

// The graph's identity: its nodes (object, type, subtype) and edges. The engine caches graphs per camera setup
// (CRenderGraphCache), so a stable scene repeats one signature; a settings change builds a new one.
uint64_t signature(const View& v) {
  uint64_t h = fnv(0xcbf29ce484222325ull, (uint64_t(v.n_live) << 32) | v.d_live);
  for (uint32_t i = 0; i < v.n_live; ++i) {
    const uint32_t e = v.n_list[i];
    if (e >= v.n_size) return 0;
    const uint8_t* n = node_at(v, e);
    h = fnv(h, rd<uint64_t>(n, kNodeObject));
    h = fnv(h, (uint64_t(rd<uint32_t>(n, kNodeType)) << 32) | rd<uint32_t>(n, kNodeSubtype));
  }
  for (uint32_t i = 0; i < v.d_live; ++i) {
    const uint32_t e = v.d_list[i];
    if (e >= v.d_size) return 0;
    const uint8_t* d = dep_at(v, e);
    h = fnv(h, (uint64_t(rd<uint32_t>(d, kDepFrom)) << 32) | rd<uint32_t>(d, kDepTo));
    h = fnv(h, rd<uint32_t>(d, kDepType));
  }
  return h ? h : 1;
}

bool out_path(uint32_t index, const wchar_t* suffix, wchar_t* path) {
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&out_path), &self))
    return false;
  const DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
  if (!n || n >= MAX_PATH) return false;
  wchar_t* slash = wcsrchr(path, L'\\');
  if (!slash || (slash - path) + 40 >= MAX_PATH) return false;
  wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"render-graph");
  CreateDirectoryW(path, nullptr);
  const size_t len = wcslen(path);
  swprintf_s(path + len, MAX_PATH - len, L"\\graph-%03u%s.txt", index, suffix);
  return true;
}

uintptr_t rva(const void* p) {
  const auto a = reinterpret_cast<uintptr_t>(p);
  return a >= g_base ? a - g_base : 0;
}

const char* node_name(const uint8_t* obj, const void* const* vt) {
  RED4ext::CName cn;
  reinterpret_cast<GetName_t>(vt[kNodeVtGetName])(obj, &cn);
  const char* s = cn.ToString();
  return s && *s ? s : "?";
}

bool is_group(const void* const* vt) { return vt && rva(render_nodes::original_execute(vt)) == kGroupExecute; }

// A CRenderNodeCommandListGroup's members, in the order its Execute runs them; a member may be a group itself.
// The render mode as the engine's config vars say it NOW (PT implies RT on).
const char* cvar_mode(uint32_t* pt, uint32_t* rt) {
  *pt = *reinterpret_cast<const uint8_t*>(g_base + kCvPathTracingValue);
  *rt = *reinterpret_cast<const uint8_t*>(g_base + kCvRayTracingEnableValue);
  return *pt ? "PT" : *rt ? "RT" : "raster";
}

void dump_members(FILE* f, const uint8_t* group, int depth) {
  const auto* items = rd<const uint8_t*>(group, kGroupItems);
  const uint32_t n = rd<uint32_t>(group, kGroupItemCount);
  if (!items || n > 4096) {
    std::fprintf(f, "%*s(members unreadable: %u)\n", depth * 2, "", n);
    return;
  }
  for (uint32_t i = 0; i < n; ++i) {
    const auto* obj = rd<const uint8_t*>(items + size_t(i) * kGroupItemStride, 0);
    const void* const* vt = obj ? rd<const void* const*>(obj, 0) : nullptr;
    std::fprintf(f, "%*sm %u %llx %llx %016llx %s\n", depth * 2, "", i, static_cast<unsigned long long>(rva(vt)),
                 static_cast<unsigned long long>(vt ? rva(render_nodes::original_execute(vt)) : 0),
                 static_cast<unsigned long long>(rd<uint64_t>(items + size_t(i) * kGroupItemStride, 8)),
                 vt ? node_name(obj, vt) : "(null)");
    if (is_group(vt) && depth < 4) dump_members(f, obj, depth + 1);
  }
}

// ---- the walk: labels (census rows) + the debug module's tree, at every change of the executing graph ----

using Occ = std::unordered_map<std::string, uint32_t>;
void walk_members(const uint8_t* group, uint8_t depth, int32_t parent, const std::string& label, const std::string& key,
                  std::vector<DebugNode>* tree);

void walk_one(const uint8_t* obj, uint8_t depth, int32_t parent, const std::string& label_head,
              const std::string& parent_key, Occ& occ, std::vector<DebugNode>* tree) {
  const void* const* vt = rd<const void* const*>(obj, 0);
  const std::string name = vt ? node_name(obj, vt) : "(null)";
  const std::string label = label_head + name;
  const int32_t me = tree ? static_cast<int32_t>(tree->size()) : -1;
  g_labels.try_emplace(reinterpret_cast<uintptr_t>(obj), Label{uint32_t(g_labels.size()), label, me});
  const bool group = is_group(vt);
  const std::string key = parent_key + "/" + name + "#" + std::to_string(occ[name]++);
  if (tree) {
    using JobUsage_t = bool (*)(const void*);
    const bool job = !group && vt && reinterpret_cast<JobUsage_t>(vt[kNodeVtJobBuilderUsage])(obj);
    tree->push_back(DebugNode{key, name, obj, vt, parent, depth, group, job});
  }
  if (group && depth < 4) walk_members(obj, depth + 1, me, label, key, tree);
}

void walk_members(const uint8_t* group, uint8_t depth, int32_t parent, const std::string& label, const std::string& key,
                  std::vector<DebugNode>* tree) {
  const auto* items = rd<const uint8_t*>(group, kGroupItems);
  const uint32_t n = rd<uint32_t>(group, kGroupItemCount);
  if (!items || n > 4096) return;
  Occ occ;
  for (uint32_t i = 0; i < n; ++i) {
    const auto* obj = rd<const uint8_t*>(items + size_t(i) * kGroupItemStride, 0);
    if (obj) walk_one(obj, depth, parent, label + " / m" + std::to_string(i) + " ", key, occ, tree);
  }
}

// Under g_mu. tree = nullptr: the labels only.
void walk(const View& v, std::vector<DebugNode>* tree) {
  g_labels.clear();
  Occ occ;
  for (uint32_t i = 0; i < v.n_live; ++i) {
    const uint32_t e = v.n_list[i];
    const auto* obj = rd<const uint8_t*>(node_at(v, e), kNodeObject);
    if (obj) walk_one(obj, 0, -1, "e" + std::to_string(e) + " ", "", occ, tree);
  }
}

void dump(const View& v, uint64_t sig) {
  wchar_t path[MAX_PATH];
  if (!out_path(g_ndumps, L"", path)) return;
  FILE* f = nullptr;
  if (_wfopen_s(&f, path, L"w") || !f) return;
  std::fprintf(f, "# CRenderNodeGraph %p  frame %llu  sig %016llx  nodes %u (slots %u)  deps %u (slots %u)\n", v.g,
               static_cast<unsigned long long>(g_frame), static_cast<unsigned long long>(sig), v.n_live, v.n_size,
               v.d_live, v.d_size);
  uint32_t pt = 0, rt = 0;
  const char* mode = cvar_mode(&pt, &rt);
  std::fprintf(f, "# render mode (config vars at the dump): %s  (PathTracing %u, RayTracingEnable %u) - the census file "
               "has the mode by what ran\n", mode, pt, rt);
  std::fprintf(f, "# node <elem> <type> <subtype> <flowgroup> <f18> <obj+8> <vtable_rva> <execute_rva> <name>\n");
  std::fprintf(f, "#   m <i> <vtable_rva> <execute_rva> <item+8> <name>   (a group's members, indented by depth)\n");
  std::fprintf(f, "# dep <from_elem> <to_elem> <type>\n");
  for (uint32_t i = 0; i < v.n_live; ++i) {
    const uint32_t e = v.n_list[i];
    const uint8_t* n = node_at(v, e);
    const auto* obj = rd<const uint8_t*>(n, kNodeObject);
    const void* const* vt = obj ? rd<const void* const*>(obj, 0) : nullptr;
    std::fprintf(f, "node %u %u %u %u %08x %016llx %llx %llx %s\n", e, rd<uint32_t>(n, kNodeType),
                 rd<uint32_t>(n, kNodeSubtype), rd<uint16_t>(n, kNodeFlowGroup), rd<uint32_t>(n, kNodeF18),
                 obj ? static_cast<unsigned long long>(rd<uint64_t>(obj, 8)) : 0ull,
                 static_cast<unsigned long long>(rva(vt)),
                 static_cast<unsigned long long>(vt ? rva(render_nodes::original_execute(vt)) : 0),
                 vt ? node_name(obj, vt) : "(null)");
    if (is_group(vt)) dump_members(f, obj, 1);
  }
  for (uint32_t i = 0; i < v.d_live; ++i) {
    const uint8_t* d = dep_at(v, v.d_list[i]);
    std::fprintf(f, "dep %u %u %u\n", rd<uint32_t>(d, kDepFrom) & kHandleIndexMask,
                 rd<uint32_t>(d, kDepTo) & kHandleIndexMask, rd<uint32_t>(d, kDepType));
  }
  std::fclose(f);
  g_sdk->logger->InfoF(g_handle, "graph: #%u written - %u nodes, %u deps, sig %016llx, frame %llu (render-graph/)",
                       g_ndumps, v.n_live, v.d_live, static_cast<unsigned long long>(sig),
                       static_cast<unsigned long long>(g_frame));
  ++g_ndumps;
}

void hex(char* out, const dxil_hash::Hash& h) {
  for (int i = 0; i < 16; ++i) std::snprintf(out + 2 * i, 3, "%02x", h[i]);
}

void write_requests();

std::string label_of(uintptr_t node, uint32_t* order);

std::string shader_text(uintptr_t pso) {  // under g_pso_lock
  const auto it = g_psos.find(pso);
  if (it == g_psos.end()) return "(PSO not seen at creation)";
  static const char* const kStage[3] = {"vs", "ps", "cs"};
  std::string out;
  char b[40];
  for (int st = 0; st < 3; ++st)
    if (it->second.has & (1u << st)) {
      hex(b, it->second.h[st]);
      out += std::string(out.empty() ? "" : " ") + kStage[st] + "=" + b;
    }
  return out.empty() ? (it->second.pipe ? "gfx (no hashed stage)" : "cs (unhashed)") : out;
}

// The census window's binds outside any node, by call chain (graph-NNN-nonode.txt).
void write_chains(const std::unordered_map<ChainKey, uint64_t, ChainHash>& chains) {
  struct Out {
    std::string shaders, chain;
    uint64_t n;
  };
  std::unordered_map<std::string, Out> merged;  // PSOs of one shader set with one chain fold together
  uint64_t total = 0;
  AcquireSRWLockShared(&g_pso_lock);
  for (const auto& [k, n] : chains) {
    total += n;
    std::string chain;
    char b[24];
    for (uint32_t i = 0; i < k.n; ++i) {
      std::snprintf(b, sizeof b, "%s%llx", i ? " " : "", 0x140000000ull + k.rva[i]);
      chain += b;
    }
    const std::string sh = shader_text(k.pso);
    Out& o = merged.try_emplace(sh + "\t" + chain, Out{sh, chain, 0}).first->second;
    o.n += n;
  }
  ReleaseSRWLockShared(&g_pso_lock);
  std::vector<const Out*> sorted;
  for (const auto& [key, o] : merged) sorted.push_back(&o);
  std::sort(sorted.begin(), sorted.end(), [](const Out* a, const Out* b) { return a->n > b->n; });
  wchar_t path[MAX_PATH];
  FILE* f = nullptr;
  if (g_window_graph < 0 || !out_path(static_cast<uint32_t>(g_window_graph), L"-nonode", path) ||
      _wfopen_s(&f, path, L"w") || !f)
    return;
  std::fprintf(f, "# binds outside any node of graph-%03d, by call chain: every %u-th per thread sampled, %llu samples, "
               "%zu rows\n# <stage=hash ...>\t<samples>\t<return addresses in the exe, innermost first>\n",
               g_window_graph, kChainSample, static_cast<unsigned long long>(total), sorted.size());
  for (const Out* o : sorted)
    std::fprintf(f, "%s\t%llu\t%s\n", o->shaders.c_str(), static_cast<unsigned long long>(o->n), o->chain.c_str());
  std::fclose(f);
  g_sdk->logger->InfoF(g_handle, "graph: binds outside any node of #%d by call chain written - %zu rows, %llu samples",
                       g_window_graph, sorted.size(), static_cast<unsigned long long>(total));
}


// Under g_mu: the census window of graph g_window_graph ends - write what it saw.
void close_window() {
  g_window.store(false, std::memory_order_release);
  write_requests();
  std::unordered_map<PairKey, uint64_t, PairHash> pairs;
  AcquireSRWLockExclusive(&g_pair_lock);
  pairs.swap(g_pairs);
  ReleaseSRWLockExclusive(&g_pair_lock);
  std::unordered_map<ChainKey, uint64_t, ChainHash> chains;  // taken with the pairs: a window with no files drops both
  AcquireSRWLockExclusive(&g_chain_lock);
  chains.swap(g_chains);
  ReleaseSRWLockExclusive(&g_chain_lock);

  // node label + shader set -> binds, PSOs (one shader set often has several PSOs: blend / RT formats)
  struct Row {
    uint32_t order = 0;
    std::string node, shaders;
    uint64_t binds = 0;
    uint32_t psos = 0;
  };
  std::unordered_map<std::string, Row> rows;
  // The debug module's rows: (tree index, shader set) -> binds, PSOs.
  const bool debug = graph_debug::on();
  std::vector<DebugShaderRow> drows;
  std::unordered_map<std::string, size_t> dindex;
  auto drow = [&](int32_t entry, const std::string& what, uint64_t n) -> DebugShaderRow* {
    const auto [it, fresh] = dindex.try_emplace(std::to_string(entry) + "\t" + what, drows.size());
    if (fresh) drows.push_back(DebugShaderRow{entry, 0, 0, {}, {}, 0, 0});
    DebugShaderRow& d = drows[it->second];
    d.binds += n;
    d.psos += 1;
    return fresh ? &d : nullptr;
  };
  uint64_t total = 0, unattributed = 0, unknown_pso = 0, by_job = 0;
  AcquireSRWLockShared(&g_pso_lock);
  for (const auto& [kj, n] : pairs) {
    total += n;
    Row r;
    // bit 0 of the node = attributed through the command list it holds (a job), not a Process on the thread
    const bool job = kj.node & 1u;
    const PairKey k{kj.node & ~uintptr_t{1}, kj.pso};
    if (job) by_job += n;
    int32_t entry = -2;  // -1 outside any node, -2 a node not in this graph (no debug row)
    if (!k.node) {
      r.order = 0xfffffffe;
      r.node = "(no node)";
      unattributed += n;
      entry = -1;
    } else if (auto it = g_labels.find(k.node); it != g_labels.end()) {
      r.order = it->second.order;
      r.node = it->second.text;
      entry = it->second.entry;
    } else {
      r.order = 0xffffffff;
      char b[48];
      std::snprintf(b, sizeof b, "(node %p not in this graph)", reinterpret_cast<const void*>(k.node));
      r.node = b;
    }
    if (job) r.node += " [job]";
    if (auto it = g_psos.find(k.pso); it != g_psos.end()) {
      const Shaders& s = it->second;
      static const char* const kStage[3] = {"vs", "ps", "cs"};
      char b[40];
      for (int st = 0; st < 3; ++st)
        if (s.has & (1u << st)) {
          hex(b, s.h[st]);
          r.shaders += std::string(r.shaders.empty() ? "" : " ") + kStage[st] + "=" + b;
        }
      if (r.shaders.empty()) r.shaders = s.pipe ? "gfx (no hashed stage)" : "cs (unhashed)";
      if (debug && entry >= -1 && s.has)
        if (DebugShaderRow* d = drow(entry, r.shaders, n)) {
          d->has = s.has;
          d->pipe = s.pipe;
          for (int st = 0; st < 3; ++st) std::memcpy(d->h[st], s.h[st].data(), 16);
        }
    } else {
      r.shaders = "(PSO not seen at creation)";
      unknown_pso += n;
    }
    Row& dst = rows.try_emplace(r.node + "\t" + r.shaders, r).first->second;
    dst.binds += n;
    dst.psos += 1;
  }
  ReleaseSRWLockShared(&g_pso_lock);
  std::unordered_map<RtKey, RtVal, RtKeyHash> rts;
  AcquireSRWLockExclusive(&g_rt_lock);
  rts.swap(g_rts);
  ReleaseSRWLockExclusive(&g_rt_lock);
  uint64_t rays = 0, rays_unknown = 0;
  bool pt_nodes = false;  // RTXDI / ReSTIR GI dispatched: only path tracing runs them
  AcquireSRWLockShared(&g_so_lock);
  for (const auto& [k, v] : rts) {
    const uint64_t n = v.n;
    rays += n;
    Row r;
    r.node = label_of(k.node, &r.order);
    if (r.node.find("RTXDI") != std::string::npos || r.node.find("ReSTIRGI") != std::string::npos) pt_nodes = true;
    char b[200], grid[48];
    if (v.w0 == v.w1 && v.h0 == v.h1)
      std::snprintf(grid, sizeof grid, "%ux%u", v.w0, v.h0);
    else
      std::snprintf(grid, sizeof grid, "%u..%ux%u..%u", v.w0, v.w1, v.h0, v.h1);
    if (auto it = g_sos.find(k.entry); it != g_sos.end()) {
      char hx[40];
      hex(hx, it->second.hash);
      std::snprintf(b, sizeof b, "rt=%s rg:%u so:%u site:%x grid:%s", hx, k.rg, k.so, k.site, grid);
    } else {
      std::snprintf(b, sizeof b, "rt(state object not seen at creation, entry %p) rg:%u so:%u site:%x grid:%s",
                    reinterpret_cast<const void*>(k.entry), k.rg, k.so, k.site, grid);
      rays_unknown += n;
    }
    r.shaders = b;
    if (debug) {
      int32_t entry = -1;
      if (auto it = g_labels.find(k.node); it != g_labels.end()) entry = it->second.entry;
      if (k.node && entry < 0) entry = -2;
      if (entry >= -1)
        if (DebugShaderRow* d = drow(entry, r.shaders, n)) {
          d->pipe = 2;
          d->rt = b;
        }
    }
    Row& dst = rows.try_emplace(r.node + "\t" + r.shaders, r).first->second;
    dst.binds += n;
    dst.psos += 1;
  }
  ReleaseSRWLockShared(&g_so_lock);
  std::vector<const Row*> sorted;
  for (const auto& [key, r] : rows) sorted.push_back(&r);
  std::sort(sorted.begin(), sorted.end(), [](const Row* a, const Row* b) {
    return a->order != b->order ? a->order < b->order : a->binds > b->binds;
  });

  const uint64_t frames = g_frame - g_window_start;
  if (debug) graph_debug::on_census(g_window_sig, frames, std::move(drows));
  wchar_t path[MAX_PATH];
  FILE* f = nullptr;
  if (g_window_graph < 0 || !out_path(static_cast<uint32_t>(g_window_graph), L"-shaders", path) ||
      _wfopen_s(&f, path, L"w") || !f)
    return;
  uint64_t jw = 0, jr = 0, jf = 0;
  render_flow::job_stats(&jw, &jr, &jf);
  std::fprintf(f, "# shader census of graph-%03d: %llu frames, %llu PSO binds (%llu outside any node, %llu in a job a "
               "node submitted [job], %llu of PSOs not seen at creation), %zu rows; jobs given a node %llu, run %llu, "
               "pool full %llu (since start)\n", g_window_graph, static_cast<unsigned long long>(frames),
               static_cast<unsigned long long>(total), static_cast<unsigned long long>(unattributed),
               static_cast<unsigned long long>(by_job), static_cast<unsigned long long>(unknown_pso), sorted.size(),
               static_cast<unsigned long long>(jw), static_cast<unsigned long long>(jr),
               static_cast<unsigned long long>(jf));
  std::fprintf(f, "# DispatchRays: %llu (%llu of state objects not seen at creation); %zu state objects known\n",
               static_cast<unsigned long long>(rays), static_cast<unsigned long long>(rays_unknown), g_sos.size());
  uint32_t pt = 0, rt = 0;
  const char* cv = cvar_mode(&pt, &rt);
  const char* ran = !rays ? "raster (no DispatchRays)" : pt_nodes ? "PT (RTXDI / ReSTIR GI dispatched)" : "RT";
  std::fprintf(f, "# render mode by what ran: %s | config vars at the window's end: %s (PathTracing %u, "
               "RayTracingEnable %u)\n", ran, cv, pt, rt);
  std::fprintf(f, "# <node label>\t<stage=hash ...>\t<binds>\t<psos>\n");
  for (const Row* r : sorted)
    std::fprintf(f, "%s\t%s\t%llu\t%u\n", r->node.c_str(), r->shaders.c_str(),
                 static_cast<unsigned long long>(r->binds), r->psos);
  std::fclose(f);
  write_chains(chains);
  g_sdk->logger->InfoF(g_handle, "graph: census of #%d written - %zu rows, %llu binds (%llu outside any node, %llu by "
                       "jobs a node submitted), %llu DispatchRays, mode by what ran: %s (config vars: %s)",
                       g_window_graph, sorted.size(), static_cast<unsigned long long>(total),
                       static_cast<unsigned long long>(unattributed), static_cast<unsigned long long>(by_job),
                       static_cast<unsigned long long>(rays), ran, cv);
}

std::string label_of(uintptr_t node, uint32_t* order) {
  if (!node) {
    *order = 0xfffffffe;
    return "(no node)";
  }
  if (auto it = g_labels.find(node); it != g_labels.end()) {
    *order = it->second.order;
    return it->second.text;
  }
  *order = 0xffffffff;
  char b[48];
  std::snprintf(b, sizeof b, "(node %p not in this graph)", reinterpret_cast<const void*>(node));
  return b;
}

// Under g_mu, from close_window: the RenderFlow requests the window saw.
void write_requests() {
  std::unordered_map<ReqKey, ReqVal, ReqKeyHash> reqs;
  AcquireSRWLockExclusive(&g_req_lock);
  reqs.swap(g_reqs);
  ReleaseSRWLockExclusive(&g_req_lock);
  struct Row {
    uint32_t order;
    std::string node;
    const ReqKey* k;
    const ReqVal* v;
  };
  std::vector<Row> rows;
  uint64_t total = 0;
  for (const auto& [k, v] : reqs) {
    Row r{0, {}, &k, &v};
    r.node = label_of(k.node, &r.order);
    rows.push_back(std::move(r));
    total += v.count;
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.order != b.order) return a.order < b.order;
    if (a.k->phase != b.k->phase) return a.k->phase < b.k->phase;
    return a.k->hash < b.k->hash;
  });
  wchar_t path[MAX_PATH];
  FILE* f = nullptr;
  if (g_window_graph < 0 || !out_path(static_cast<uint32_t>(g_window_graph), L"-resources", path) ||
      _wfopen_s(&f, path, L"w") || !f)
    return;
  std::fprintf(f, "# RenderFlow requests of graph-%03d: %llu frames, %llu requests, %zu rows\n", g_window_graph,
               static_cast<unsigned long long>(g_frame - g_window_start), static_cast<unsigned long long>(total),
               rows.size());
  std::fprintf(f, "# <node label>\t<phase>\t<type>\t<sub>\t<name hash>\t<param>\t<count>\t<desc (type 1)>\n");
  for (const Row& r : rows) {
    char desc[81] = "";
    if (r.k->type == 1)
      for (int i = 0; i < 40; ++i) std::snprintf(desc + 2 * i, 3, "%02x", r.v->desc[i]);
    std::fprintf(f, "%s\t%u\t%u\t%u\t%08x\t%08x\t%llu\t%s\n", r.node.c_str(), r.k->phase, r.k->type, r.k->sub,
                 r.k->hash, r.k->param, static_cast<unsigned long long>(r.v->count), desc);
  }
  std::fclose(f);
  g_sdk->logger->InfoF(g_handle, "graph: resources of #%d written - %zu rows, %llu requests", g_window_graph,
                       rows.size(), static_cast<unsigned long long>(total));
}

// Under g_mu: a census window on the executing graph (g_labels are its labels).
void open_window(int32_t dump_index) {
  if (!g_census.load(std::memory_order_relaxed) || !render_flow::attached()) return;
  g_window_graph = dump_index;
  g_window_sig = g_cur_sig;
  g_window_start = g_frame;
  g_window_end = g_frame + kWindow;
  g_window.store(true, std::memory_order_release);
}

// Every frame, at ExecuteParallel's entry (one call per frame; the previous frame's graph jobs are done - RenderFrame
// waits on them): the signature of the executing graph; at a change, its labels + debug tree, the dump of a graph not
// seen before, a census window (new graphs; under the debug module every change, and on request).
void probe(const void* graph) {
  std::unique_lock<std::mutex> lock(g_mu, std::try_to_lock);
  if (!lock.owns_lock()) return;
  ++g_frame;
  if (g_window.load(std::memory_order_relaxed) && g_frame >= g_window_end) close_window();
  View v;
  if (!view_of(graph, &v)) {
    static bool said = false;
    if (!said) g_sdk->logger->Warn(g_handle, "graph: the graph's counts do not fit the 2.31 layout - not dumped");
    said = true;
    return;
  }
  const uint64_t sig = signature(v);
  if (!sig) return;
  const bool debug = graph_debug::on();
  if (sig == g_cur_sig) {
    if (g_census_req.load(std::memory_order_relaxed) && !g_window.load(std::memory_order_relaxed) &&
        g_census_req.exchange(false))
      open_window(g_cur_dump);
    return;
  }
  // The executing graph changed (a new build, or a cached one again).
  if (g_window.load(std::memory_order_relaxed)) close_window();  // its labels are about to be replaced
  g_cur_sig = sig;
  int32_t index = -1;
  bool fresh = true;
  for (uint32_t i = 0; i < g_nseen; ++i)
    if (g_seen[i] == sig) {
      fresh = false;
      index = g_seen_dump[i];
    }
  std::vector<DebugNode> tree;
  walk(v, debug ? &tree : nullptr);
  if (fresh && g_ndumps < kMaxDumps) {
    index = static_cast<int32_t>(g_ndumps);
    dump(v, sig);
    if (g_ndumps == static_cast<uint32_t>(index)) index = -1;  // not written
  }
  if (fresh && g_nseen < kSeen) {
    g_seen[g_nseen] = sig;
    g_seen_dump[g_nseen++] = index;
  }
  g_cur_dump = index;
  if (debug) {
    uint32_t pt = 0, rt = 0;
    graph_debug::on_tree(sig, cvar_mode(&pt, &rt), std::move(tree));
  }
  if ((fresh && index >= 0) || debug) open_window(fresh ? index : -1);
  g_census_req.store(false);
}

void request_census(const void* data, const uint8_t* req) {
  if (g_window.load(std::memory_order_relaxed) && req) {
    ReqKey k{};
    k.node = reinterpret_cast<uintptr_t>(render_flow::current_node());
    k.type = req[0];
    k.sub = req[1];
    k.phase = data ? static_cast<const uint8_t*>(data)[kFlowPhase] : 0xff;
    std::memcpy(&k.hash, req + 4, 4);
    if (k.type != 1) std::memcpy(&k.param, req + 8, 4);
    AcquireSRWLockExclusive(&g_req_lock);
    ReqVal& v = g_reqs[k];
    if (!v.count++ && k.type == 1) std::memcpy(v.desc, req + 8, sizeof v.desc);
    ReleaseSRWLockExclusive(&g_req_lock);
  }
}



void rays_census(uint32_t so, uint32_t a2, uint32_t a3, uint32_t a5, uint32_t site) {
  if (g_window.load(std::memory_order_relaxed)) {
    RtKey k{reinterpret_cast<uintptr_t>(render_flow::current_node()), 0, so, a5, site};
    const auto* tbl = *reinterpret_cast<const uint8_t* const*>(g_base + kRtSoTable);
    const auto* dev = *reinterpret_cast<const uint8_t* const*>(g_base + kDeviceData);
    if (tbl && dev) {
      const auto* arr = rd<const uint8_t*>(tbl, kRtSoTableArray);
      const uint32_t id = arr ? rd<uint32_t>(arr + size_t(so) * kRtSoEntry, 0) : 0;
      if (id) k.entry = reinterpret_cast<uintptr_t>(dev + kRtStateObjects + size_t(id - 1) * kRtStateObjectStride);
    }
    AcquireSRWLockExclusive(&g_rt_lock);
    RtVal& v = g_rts[k];
    ++v.n;
    v.w0 = std::min(v.w0, a2);
    v.w1 = std::max(v.w1, a2);
    v.h0 = std::min(v.h0, a3);
    v.h1 = std::max(v.h1, a3);
    ReleaseSRWLockExclusive(&g_rt_lock);
  }
}

void d_execute_parallel(const void* graph, const void* ctx, void* builder) {
  prof::on_graph_execute();
  probe(graph);
  g_orig(graph, ctx, builder);
}

}  // namespace

void enable() {
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
  g_exe_end = g_base + nt->OptionalHeader.SizeOfImage;
  g_census.store(true);
  render_flow::want();  // the node on each thread - CRenderNodeBase::Process
}
bool census() { return g_census.load(std::memory_order_relaxed); }

void on_request(const void* data, const uint8_t* req) { request_census(data, req); }

void on_pso_created(uint32_t pipe, const void* pso, const void* desc) {
  Shaders s;
  s.pipe = static_cast<uint8_t>(pipe);
  auto take = [&s](uint32_t stage, const D3D12_SHADER_BYTECODE& b) {
    if (!b.pShaderBytecode || !b.BytecodeLength) return;
    if (const auto h = dxil_hash::of(b.pShaderBytecode, b.BytecodeLength)) {
      s.h[stage] = *h;
      s.has |= 1u << stage;
    }
  };
  if (pipe == 0) {
    take(RG_STAGE_CS, static_cast<const D3D12_COMPUTE_PIPELINE_STATE_DESC*>(desc)->CS);
  } else {
    const auto* g = static_cast<const D3D12_GRAPHICS_PIPELINE_STATE_DESC*>(desc);
    take(RG_STAGE_VS, g->VS);
    take(RG_STAGE_PS, g->PS);
  }
  AcquireSRWLockExclusive(&g_pso_lock);
  g_psos[reinterpret_cast<uintptr_t>(pso)] = s;
  ReleaseSRWLockExclusive(&g_pso_lock);
}

void on_state_object(const void* entry, const uint8_t* hash16, const char* const* exports, uint32_t n) {
  StateObject s;
  std::memcpy(s.hash.data(), hash16, s.hash.size());
  for (uint32_t i = 0; i < n; ++i) s.exports.emplace_back(exports[i]);
  AcquireSRWLockExclusive(&g_so_lock);
  g_sos[reinterpret_cast<uintptr_t>(entry)] = std::move(s);
  ReleaseSRWLockExclusive(&g_so_lock);
}

void on_bind(uint32_t, const void* pso) {
  if (!g_window.load(std::memory_order_relaxed)) return;
  const auto node = reinterpret_cast<uintptr_t>(render_flow::current_node());
  const PairKey k{node | (node && render_flow::current_is_job() ? 1u : 0u), reinterpret_cast<uintptr_t>(pso)};
  AcquireSRWLockExclusive(&g_pair_lock);
  ++g_pairs[k];
  ReleaseSRWLockExclusive(&g_pair_lock);
  if (!k.node && g_exe_end) note_chain(k.pso);
}

bool pso_shaders(const void* pso, PsoShaders* out) {
  AcquireSRWLockShared(&g_pso_lock);
  const auto it = g_psos.find(reinterpret_cast<uintptr_t>(pso));
  const bool found = it != g_psos.end();
  if (found) {
    out->has = it->second.has;
    out->pipe = it->second.pipe;
    for (int st = 0; st < 3; ++st) std::memcpy(out->h[st], it->second.h[st].data(), 16);
  }
  ReleaseSRWLockShared(&g_pso_lock);
  return found;
}

void on_dispatch_rays(uint32_t so, uint32_t w, uint32_t h, uint32_t a5, uint32_t site) {
  rays_census(so, w, h, a5, site);
}
bool rays_wanted() { return g_attached && render_flow::attached(); }

void census_request() { g_census_req.store(true); }

bool census_running(uint64_t* frames_left) {
  if (!g_window.load(std::memory_order_acquire)) return false;
  // Unlocked reads of two u64 a render thread writes - a progress figure for the UI, not a measurement.
  const uint64_t end = g_window_end, now = g_frame;
  if (frames_left) *frames_left = end > now ? end - now : 0;
  return true;
}

void attach() {
  g_attached = attach_hook(kGraphExecuteParallel, kRenderFrameExecuteRet, reinterpret_cast<void*>(&d_execute_parallel),
                           reinterpret_cast<void**>(&g_orig), "CRenderNodeGraph::ExecuteParallel");
  const bool nodes = render_flow::attached();  // DispatchRays: rtlib owns the hook and reports here (rays_census)
  g_sdk->logger->InfoF(g_handle, "graph: render-graph dump %s, shader census %s ([Diagnostics] RenderGraphDump)",
                       g_attached ? "ATTACHED - every distinct graph goes to render-graph/" : "NOT attached",
                       nodes ? (render_flow::requests_attached() ? "ON (graph-NNN-shaders.txt, -resources.txt)"
                                                               : "ON (graph-NNN-shaders.txt; RenderFlow NOT attached)")
                                          : "off");
}

void detach() {
  if (g_attached) detach_hook(kGraphExecuteParallel);
  g_attached = false;
}

}  // namespace core::graph_probe
