#pragma once

// An ini file as REDGraph and its clients keep their settings: sections and keys in file order, unknown ones kept
// (a newer build's settings survive an older one), case-insensitive lookup, rewritten whole on a change.
// Header-only; no locking - the owner locks.

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace shared {

class IniFile {
 public:
  struct Section {
    std::string name;
    std::vector<std::pair<std::string, std::string>> kv;
  };

  // false = no such file (then empty).
  bool read(const std::filesystem::path& p) {
    sections_.clear();
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::string line;
    Section* cur = nullptr;
    while (std::getline(in, line)) {
      line = trim(line);
      if (line.empty() || line[0] == ';' || line[0] == '#') continue;
      if (line.front() == '[' && line.back() == ']') {
        sections_.push_back({trim(line.substr(1, line.size() - 2)), {}});
        cur = &sections_.back();
        continue;
      }
      const size_t eq = line.find('=');
      if (!cur || eq == std::string::npos) continue;
      cur->kv.emplace_back(trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
    }
    return true;
  }

  const std::string* find(const char* section, const char* key) const {
    for (const Section& s : sections_)
      if (_stricmp(s.name.c_str(), section) == 0)
        for (const auto& [k, v] : s.kv)
          if (_stricmp(k.c_str(), key) == 0) return &v;
    return nullptr;
  }

  void put(const char* section, const char* key, const std::string& value) {
    Section* sec = nullptr;
    for (Section& s : sections_)
      if (_stricmp(s.name.c_str(), section) == 0) sec = &s;
    if (!sec) {
      sections_.push_back({section, {}});
      sec = &sections_.back();
    }
    for (auto& [k, v] : sec->kv)
      if (_stricmp(k.c_str(), key) == 0) {
        v = value;
        return;
      }
    sec->kv.emplace_back(key, value);
  }

  // false = cannot write. `header` is the first comment line (without "; ").
  bool write(const std::filesystem::path& p, const char* header) const {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "; " << header << "\r\n";
    for (const Section& s : sections_) {
      out << "\r\n[" << s.name << "]\r\n";
      for (const auto& [k, v] : s.kv) out << k << " = " << v << "\r\n";
    }
    return true;
  }

  // The directory of the module holding `addr` (a plugin DLL: its ini lives next to it).
  static std::filesystem::path module_dir(const void* addr) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(addr), &m);
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(m, buf, static_cast<DWORD>(std::size(buf)));
    return std::filesystem::path(buf).parent_path();
  }

 private:
  static std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
  }

  std::vector<Section> sections_;
};

}  // namespace shared
