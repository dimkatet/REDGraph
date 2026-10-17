// settings.hpp.

#include "settings.hpp"

#include <Windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>

#include "rg_host.h"
#include "core/gpu_timing.hpp"
#include "core/prof.hpp"
#include "ini_file.hpp"

namespace rg_settings {
namespace {

struct Def {
  const char* section;
  const char* key;
  bool live;
  const char* what;
};
const Def kDefs[] = {
    // The host's per-object detail in its log (every RT library the engine builds, every clone / bake).
    {"Diagnostics", "VerboseLog", true, "detailed log"},
    // The engine's render graph, read only: every distinct graph to render-graph/ next to the DLL (core/graph_probe).
    {"Diagnostics", "RenderGraphDump", false, "graph dump"},
    // The graph DEBUG window (CET, core/graph_debug): every node with its shaders, nodes removed and shaders skipped
    // live. Wraps every node class.
    {"Diagnostics", "RenderGraphDebug", false, "graph debug window"},
    // DRED (GPU page-fault address + auto-breadcrumbs) for the crash log - on BEFORE the engine creates its device.
    {"Diagnostics", "Dred", false, "DRED"},
    // GPU time of every companion / pass (timestamps on the engine's lists, core/gpu_timing) + their debug events.
    {"Diagnostics", "GpuTiming", true, "GPU timing"},
    // Raster draws of the VS clients watch (rg_host::census_watch) joined to the BLAS geometries the engine
    // builds - static / updatable / not in the BVH - and the deform pool's writers, every 10 s as [blas]
    // (core/blas_census). At the start only.
    {"Diagnostics", "BlasCensus", false, "BLAS census"},
    // The frame time every 10 s as [frame] (core/prof) - an on/off A/B of any feature against its own log's switches.
    {"Diagnostics", "FrameTimer", true, "frame timer"},
};
constexpr size_t kCount = sizeof(kDefs) / sizeof(kDefs[0]);

SRWLOCK g_lock = SRWLOCK_INIT;
float g_val[kCount];
shared::IniFile g_ini;
std::filesystem::path g_path;

void logf(uint32_t level, const char* fmt, const char* a, const char* b = "") {
  const rg_host* h = rg_host_get(RG_HOST_API_VERSION);
  if (!h) return;
  char line[512];
  std::snprintf(line, sizeof line, fmt, a, b);
  h->log(level, line);
}

size_t index_of(const char* name) {
  const char* dot = name ? std::strchr(name, '.') : nullptr;
  if (!dot) return kCount;
  const std::string section(name, dot);
  for (size_t i = 0; i < kCount; ++i)
    if (_stricmp(kDefs[i].section, section.c_str()) == 0 && _stricmp(kDefs[i].key, dot + 1) == 0) return i;
  return kCount;
}

void apply_live(size_t i) {
  const rg_host* h = rg_host_get(RG_HOST_API_VERSION);
  if (h && std::strcmp(kDefs[i].key, "VerboseLog") == 0) h->log_set_verbose(g_val[i] > 0.5f ? 1u : 0u);
  if (std::strcmp(kDefs[i].key, "GpuTiming") == 0) core::gpu_timing::set_enabled(g_val[i] > 0.5f);
  if (std::strcmp(kDefs[i].key, "FrameTimer") == 0) core::prof::set_frame_timer(g_val[i] > 0.5f);
}

bool parse_bool(const std::string& t, float* out) {
  const char* s = t.c_str();
  if (!_stricmp(s, "true") || !_stricmp(s, "1") || !_stricmp(s, "yes") || !_stricmp(s, "on")) return *out = 1, true;
  if (!_stricmp(s, "false") || !_stricmp(s, "0") || !_stricmp(s, "no") || !_stricmp(s, "off")) return *out = 0, true;
  return false;
}

}  // namespace

void load() {
  AcquireSRWLockExclusive(&g_lock);
  g_path = shared::IniFile::module_dir(&g_path) / "REDGraph.ini";
  const bool found = g_ini.read(g_path);
  std::string line;
  for (size_t i = 0; i < kCount; ++i) {
    g_val[i] = 0.0f;
    const std::string* s = g_ini.find(kDefs[i].section, kDefs[i].key);
    if (s && !parse_bool(*s, &g_val[i]))
      logf(RG_LOG_WARN, "settings: %s = %s is not a bool - off", kDefs[i].key, s->c_str());
    line += std::string(i ? ", " : "") + kDefs[i].key + (g_val[i] > 0.5f ? " true" : " false");
  }
  for (size_t i = 0; i < kCount; ++i)
    if (kDefs[i].live) apply_live(i);
  ReleaseSRWLockExclusive(&g_lock);
  logf(RG_LOG_INFO, found ? "settings: %s - %s" : "settings: no %s - defaults (%s)", g_path.string().c_str(),
       line.c_str());
}

bool get(const char* name, float* out) {
  const size_t i = index_of(name);
  if (i == kCount || !out) return false;
  AcquireSRWLockShared(&g_lock);
  *out = g_val[i];
  ReleaseSRWLockShared(&g_lock);
  return true;
}

bool set(const char* name, float value) {
  const size_t i = index_of(name);
  if (i == kCount || !std::isfinite(value)) return false;
  AcquireSRWLockExclusive(&g_lock);
  const float x = value > 0.5f ? 1.0f : 0.0f;
  if (x != g_val[i]) {
    g_val[i] = x;
    if (kDefs[i].live) apply_live(i);
    g_ini.put(kDefs[i].section, kDefs[i].key, x > 0.5f ? "true" : "false");
    if (!g_ini.write(g_path, "REDGraph - settings. Written whenever an option changes."))
      logf(RG_LOG_WARN, "settings: cannot write %s%s", g_path.string().c_str());
    logf(RG_LOG_INFO, "settings: %s = %s", kDefs[i].key, x > 0.5f ? "true" : "false");
  }
  ReleaseSRWLockExclusive(&g_lock);
  return true;
}

std::string path() {
  AcquireSRWLockShared(&g_lock);
  std::string p = g_path.string();
  ReleaseSRWLockShared(&g_lock);
  return p;
}

std::string report() {
  std::ostringstream o;
  AcquireSRWLockShared(&g_lock);
  for (size_t i = 0; i < kCount; ++i)
    o << kDefs[i].section << "." << kDefs[i].key << " = " << (g_val[i] > 0.5f ? "true" : "false")
      << (kDefs[i].live ? "" : " (from the next start)") << "\n";
  ReleaseSRWLockShared(&g_lock);
  return o.str();
}

}  // namespace rg_settings
