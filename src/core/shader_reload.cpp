// [Dev] ShaderHotReload (docs/internals.md "Shader hot reload (dev)"): new bytes for a registered patch / companion /
// draw companion while the game runs. The folder ([Dev] ShaderReloadDir, else `shader_reload/` next to REDGraph.dll -
// virtual under MO2, see enable) is polled from the frame hook every kPollFrames; a file whose time or size changed is
// applied:
//   <target>[.v<N>].dxbc  a signed DXBC container
//   <target>[.v<N>].ll    LLVM IR text (an edited disassembly) - assembled and signed here (dxcompiler.dll + dxil.dll
//                         next to REDGraph.dll, as for compile_hlsl)
// <target> = the client's name made file-safe (pso::dev_file_stem) or the engine shader's hash in hex; N = the variant
// (none = 0). `_targets.txt` in the folder lists every target with both names, written at the first frame. Files
// already there at the start are applied too - the folder IS the set of overrides; empty it when done.
// Off (the default): nothing here runs, pso keeps no descs - the host is exactly the release one.

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/internal.hpp"

namespace core::shader_reload {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kPollFrames = 30;
bool g_on = false;
uint32_t g_frame = 0;
bool g_listed = false;
fs::path g_dir;
struct Seen {
  fs::file_time_type time;
  uintmax_t size;
};
std::map<std::wstring, Seen> g_seen;
std::vector<std::unique_ptr<std::vector<uint8_t>>> g_keep;  // never freed: clones and future PSOs point into them

fs::path our_dir() {
  HMODULE m = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&our_dir), &m);
  wchar_t buf[MAX_PATH];
  const DWORD n = m ? GetModuleFileNameW(m, buf, MAX_PATH) : 0;
  return n ? fs::path(buf).parent_path() : fs::path();
}

uint32_t u32_at(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

// The rg_stage of a DXBC container's DXIL program (its version word's kind: 0 PS, 1 VS, 2 GS, 5 CS), -1 = unknown.
int32_t stage_of(const uint8_t* b, size_t size) {
  if (size < 32) return -1;
  const uint32_t n = u32_at(b + 28);
  for (uint32_t i = 0; i < n && 32 + 4 * (i + 1) <= size; ++i) {
    const uint32_t at = u32_at(b + 32 + 4 * i);
    if (at + 12 > size || std::memcmp(b + at, "DXIL", 4) != 0) continue;
    switch (u32_at(b + at + 8) >> 16) {
      case 0: return RG_STAGE_PS;
      case 1: return RG_STAGE_VS;
      case 2: return RG_STAGE_GS;
      case 5: return RG_STAGE_CS;
      default: return -1;
    }
  }
  return -1;
}

const char* why(int32_t rc) {
  switch (rc) {
    case RG_E_ARG: return "no such target (see _targets.txt)";
    case RG_E_TAKEN: return "two targets answer to this name - use the other one (_targets.txt)";
    case RG_E_FULL: return "the target has no such variant";
    case RG_E_BLOB: return "a shader of another stage than the target's";
    case RG_E_COMPILE: return "the driver refused a clone - every clone stays as it was";
    default: return "?";
  }
}

// false = not complete yet (a writer still busy): try again at the next poll without marking it seen.
bool apply(const fs::path& file) {
  const std::string fname = file.filename().string();
  std::string stem = file.stem().string();
  const bool ir = _wcsicmp(file.extension().c_str(), L".ll") == 0;
  int32_t variant = 0;
  if (const size_t dot = stem.rfind(".v"); dot != std::string::npos && dot + 2 < stem.size() &&
                                           stem.find_first_not_of("0123456789", dot + 2) == std::string::npos) {
    variant = std::atoi(stem.c_str() + dot + 2);
    stem.resize(dot);
  }
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  auto bytes = std::make_unique<std::vector<uint8_t>>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  in.close();
  rg_blob blob{};
  if (ir) {
    std::string err;
    const std::string text(bytes->begin(), bytes->end());
    if (const int32_t rc = hlsl::assemble(text, &blob, &err)) {
      g_sdk->logger->ErrorF(g_handle, "DEV reload: %s NOT applied - %.600s", fname.c_str(), err.c_str());
      return true;  // seen: the next save retries
    }
  } else {
    const size_t n = bytes->size();
    if (n < 32 || std::memcmp(bytes->data(), "DXBC", 4) != 0 || u32_at(bytes->data() + 24) != n) return false;
    bool signed_ = false;
    for (int i = 4; i < 20; ++i) signed_ |= (*bytes)[i] != 0;
    if (!signed_) {
      g_sdk->logger->ErrorF(g_handle, "DEV reload: %s NOT applied - the container is unsigned (validate it, or drop "
                            "the .ll here instead)", fname.c_str());
      return true;
    }
    blob = {bytes->data(), n};
    g_keep.push_back(std::move(bytes));
  }
  const auto* b = static_cast<const uint8_t*>(blob.bytes);
  pso::DevReload r;
  const int32_t rc = pso::dev_reload(stem.c_str(), variant, blob, stage_of(b, blob.size), &r);
  if (rc) {
    if (rc == RG_E_COMPILE)
      g_sdk->logger->ErrorF(g_handle, "DEV reload: %s NOT applied to %s '%s' variant %d - %u of %u engine PSO(s) "
                            "refused it, hr 0x%08x; the previous shader stays", fname.c_str(), r.kind, r.name, variant,
                            r.failed, r.mapped, r.last_hr);
    else
      g_sdk->logger->ErrorF(g_handle, "DEV reload: %s NOT applied - %s", fname.c_str(), why(rc));
    return true;
  }
  g_sdk->logger->InfoF(g_handle, "DEV reload: %s -> %s '%s' variant %d: %u clone(s) rebuilt on %u engine PSO(s)%s "
                       "(%zu bytes)", fname.c_str(), r.kind, r.name, variant, r.rebuilt, r.mapped,
                       r.mapped ? "" : " - none made yet, the engine's next ones take it", blob.size);
  if (r.no_desc)
    g_sdk->logger->WarnF(g_handle, "DEV reload: %u engine PSO(s) of '%s' have no kept desc - not rebuilt", r.no_desc,
                         r.name);
  return true;
}

void list_targets() {
  const std::string t = pso::dev_targets();
  std::ofstream out(g_dir / "_targets.txt", std::ios::trunc);
  out << "# REDGraph [Dev] ShaderHotReload - drop <file> here, the game picks it up. Rewritten at every start.\n" << t;
  g_sdk->logger->InfoF(g_handle, "DEV reload: %zu target(s) listed in %s", static_cast<size_t>(std::count(
                       t.begin(), t.end(), '\n')), (g_dir / "_targets.txt").string().c_str());
}

void poll() {
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(g_dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    const fs::path& p = e.path();
    const std::wstring ext = p.extension().wstring();
    if (_wcsicmp(ext.c_str(), L".dxbc") != 0 && _wcsicmp(ext.c_str(), L".ll") != 0) continue;
    const Seen now{e.last_write_time(ec), e.file_size(ec)};
    if (ec) continue;
    auto it = g_seen.find(p.filename().wstring());
    if (it != g_seen.end() && it->second.time == now.time && it->second.size == now.size) continue;
    if (apply(p)) g_seen[p.filename().wstring()] = now;
  }
}

}  // namespace

void enable(const std::string& dir) {
  g_on = true;
  pso::dev_enable();
  g_dir = dir.empty() ? our_dir() / "shader_reload" : fs::u8path(dir);
  std::error_code ec;
  fs::create_directories(g_dir, ec);
  g_sdk->logger->WarnF(g_handle, "DEV: shader hot reload ON ([Dev] ShaderHotReload) - watching %s; every patched PSO "
                       "keeps a copy of its desc. Not for players.", g_dir.string().c_str());
  if (dir.empty())
    g_sdk->logger->WarnF(g_handle, "DEV: under MO2 that folder is VIRTUAL - a file created in it from outside the game "
                         "under a new name is never seen; set [Dev] ShaderReloadDir = <a folder outside the game>");
}

void frame_tick() {
  if (!g_on || ++g_frame % kPollFrames) return;
  if (!g_listed) {  // registration closed long ago: every target is known
    g_listed = true;
    list_targets();
  }
  poll();
}

}  // namespace core::shader_reload
