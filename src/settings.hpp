#pragma once

// REDGraph's own settings: `REDGraph.ini` next to REDGraph.dll, section [Diagnostics]. A setting is named
// "Section.Key"; bools are 0 / 1. VerboseLog applies live, the rest at the next start (they shape what attaches).

#include <string>

namespace rg_settings {

void load();  // at Load, first: resolves the path, reads the file, logs the values
bool get(const char* name, float* out);  // false = no such setting
bool set(const char* name, float value);  // saved; VerboseLog applied live; false = no such setting
std::string path();
std::string report();  // one line per setting: name = value

}  // namespace rg_settings
