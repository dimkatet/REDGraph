// REDGraph's native global functions for its CET window (cet/REDGraph/init.lua), called as Game.<name>():
//   RG_Ping() -> String                     the host is there (version, API, registration state)
//   RG_Get(String name) -> Float            a host setting, "Diagnostics.Key" (settings.hpp); bools 0 / 1
//   RG_Set(String name, Float v) -> Bool    saved (VerboseLog live, the rest from the next start)
//   RG_Status() -> String                   the host's counters + its settings (also into its log)
//   RG_Graph*                               the render graph debug module ([Diagnostics] RenderGraphDebug,
//                                           core/graph_debug.hpp): RG_Graph() -> the report ("" = module off),
//                                           RG_GraphNode(gen, index, removed, force), RG_GraphShader(gen, row,
//                                           where, skip), RG_GraphRuleOff(rule) -> "" or why not;
//                                           RG_GraphAction(0 census / 1 reset) -> Bool.
//   RG_Profile(Float delay, Float seconds) -> String   the host's own CPU, measured `seconds` after `delay`
//                                           (core/prof.hpp); "" = armed, else why not
//   RG_ProfileState() -> String             "idle" / "starts in" / "measuring", then the last report
//   RG_Clients() -> String                  the host's clients and what each registered (core/clients.cpp has
//                                           the format)

#include "ui.hpp"

#include <cstdio>
#include <string>

#include <RED4ext/RED4ext.hpp>

#include "rg_host.h"
#include "core/clients.hpp"
#include "core/graph_debug.hpp"
#include "core/prof.hpp"
#include "settings.hpp"

#ifndef RG_VERSION_STRING
#define RG_VERSION_STRING "dev"
#endif

namespace rg_ui {
namespace {

const rg_host* host() { return rg_host_get(RG_HOST_API_VERSION); }

void say(uint32_t level, const std::string& s) {
  if (const rg_host* h = host()) h->log(level, s.c_str());
}

void Ping(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, RED4ext::CString* out, int64_t) {
  frame->code++;  // ParamEnd
  if (out) {
    char s[160];
    std::snprintf(s, sizeof(s), "REDGraph %s - host API %u", RG_VERSION_STRING, static_cast<unsigned>(RG_HOST_API_VERSION));
    *out = RED4ext::CString(s);
  }
}

void Get(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, float* out, int64_t) {
  RED4ext::CString name;
  RED4ext::GetParameter(frame, &name);
  frame->code++;  // ParamEnd
  float v = 0.0f;
  if (!rg_settings::get(name.c_str(), &v)) say(RG_LOG_WARN, std::string("ui: RG_Get('") + name.c_str() + "') - no such setting");
  if (out) *out = v;
}

void Set(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, bool* out, int64_t) {
  RED4ext::CString name;
  float v = 0.0f;
  RED4ext::GetParameter(frame, &name);
  RED4ext::GetParameter(frame, &v);
  frame->code++;  // ParamEnd
  const bool ok = rg_settings::set(name.c_str(), v);
  if (!ok) say(RG_LOG_WARN, std::string("ui: RG_Set('") + name.c_str() + "') - no such setting");
  if (out) *out = ok;
}

std::string status() {
  char line[512];
  std::string o;
  std::snprintf(line, sizeof line, "REDGraph %s, host API %u\n", RG_VERSION_STRING, static_cast<unsigned>(RG_HOST_API_VERSION));
  o += line;
  rg_host_stats hs{};
  if (const rg_host* h = host()) h->host_stats(&hs);
  std::snprintf(line, sizeof line, "PSO hooks %s; engine PSOs compute %llu, graphics %llu (no hash %llu); mapped %u\n",
                hs.pso_hooks ? "attached" : "NOT ATTACHED", static_cast<unsigned long long>(hs.compute_creates),
                static_cast<unsigned long long>(hs.graphics_creates),
                static_cast<unsigned long long>(hs.creates_unhashed), hs.mapped);
  o += line;
  o += "Clients: " + core::clients::summary() + "\n";
  const std::string prof = core::prof::report();
  o += prof.empty() ? "host profile: not run (Host tab, Profile)\n" : prof;
  o += "Settings (" + rg_settings::path() + "):\n" + rg_settings::report();
  return o;
}

void log_report(const char* why, const std::string& s) {
  say(RG_LOG_INFO, std::string("ui: status report (") + why + ") follows");
  size_t a = 0;
  while (a < s.size()) {
    const size_t b = s.find('\n', a);
    say(RG_LOG_INFO, "status: " + s.substr(a, b == std::string::npos ? std::string::npos : b - a));
    if (b == std::string::npos) break;
    a = b + 1;
  }
}

void Status(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, RED4ext::CString* out, int64_t) {
  frame->code++;  // ParamEnd
  const std::string s = status();
  log_report("RG_Status()", s);
  if (out) *out = RED4ext::CString(s.c_str());
}

void Clients(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, RED4ext::CString* out, int64_t) {
  frame->code++;  // ParamEnd
  if (out) *out = RED4ext::CString(core::clients::report().c_str());
}

void Profile(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, RED4ext::CString* out, int64_t) {
  float delay = 0.0f, seconds = 0.0f;
  RED4ext::GetParameter(frame, &delay);
  RED4ext::GetParameter(frame, &seconds);
  frame->code++;  // ParamEnd
  if (out) *out = RED4ext::CString(core::prof::arm(delay, seconds).c_str());
}

void ProfileState(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, RED4ext::CString* out, int64_t) {
  frame->code++;  // ParamEnd
  if (out) *out = RED4ext::CString((core::prof::state() + "\n" + core::prof::report()).c_str());
}

// ---- the render graph debug module (core/graph_debug.hpp has the report format) ----

void Graph(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, RED4ext::CString* out, int64_t) {
  frame->code++;  // ParamEnd
  if (out) *out = RED4ext::CString(core::graph_debug::on() ? core::graph_debug::report().c_str() : "");
}

void GraphNode(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, RED4ext::CString* out, int64_t) {
  int32_t gen = 0, index = 0;
  bool on = false, force = false;
  RED4ext::GetParameter(frame, &gen);
  RED4ext::GetParameter(frame, &index);
  RED4ext::GetParameter(frame, &on);
  RED4ext::GetParameter(frame, &force);
  frame->code++;  // ParamEnd
  const std::string r = core::graph_debug::on() ? core::graph_debug::set_node(gen, index, on, force) : "debug off";
  if (out) *out = RED4ext::CString(r.c_str());
}

void GraphShader(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, RED4ext::CString* out, int64_t) {
  int32_t gen = 0, row = 0, where = 0;
  bool on = false;
  RED4ext::GetParameter(frame, &gen);
  RED4ext::GetParameter(frame, &row);
  RED4ext::GetParameter(frame, &where);
  RED4ext::GetParameter(frame, &on);
  frame->code++;  // ParamEnd
  const std::string r = core::graph_debug::on() ? core::graph_debug::set_shader(gen, row, where, on) : "debug off";
  if (out) *out = RED4ext::CString(r.c_str());
}

void GraphRuleOff(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, RED4ext::CString* out, int64_t) {
  int32_t rule = 0;
  RED4ext::GetParameter(frame, &rule);
  frame->code++;  // ParamEnd
  const std::string r = core::graph_debug::on() ? core::graph_debug::rule_off(rule) : "debug off";
  if (out) *out = RED4ext::CString(r.c_str());
}

// action: 0 = a new census on the executing graph, 1 = every removal and skip off.
void GraphAction(RED4ext::IScriptable*, RED4ext::CStackFrame* frame, bool* out, int64_t) {
  int32_t action = 0;
  RED4ext::GetParameter(frame, &action);
  frame->code++;  // ParamEnd
  const bool ok = core::graph_debug::on() && (action == 0 || action == 1);
  if (ok && action == 0) core::graph_debug::census();
  if (ok && action == 1) core::graph_debug::reset();
  if (out) *out = ok;
}

template <typename Fn>
RED4ext::CGlobalFunction* make(const char* name, Fn fn, const char* ret) {
  auto* f = RED4ext::CGlobalFunction::Create(name, name, fn);
  f->flags = {.isNative = true, .isStatic = true};
  f->SetReturnType(ret);
  return f;
}

void post_register() {
  auto* rtti = RED4ext::CRTTISystem::Get();
  rtti->RegisterFunction(make("RG_Ping", &Ping, "String"));
  {
    auto* f = make("RG_Get", &Get, "Float");
    f->AddParam("String", "name");
    rtti->RegisterFunction(f);
  }
  {
    auto* f = make("RG_Set", &Set, "Bool");
    f->AddParam("String", "name");
    f->AddParam("Float", "value");
    rtti->RegisterFunction(f);
  }
  rtti->RegisterFunction(make("RG_Status", &Status, "String"));
  rtti->RegisterFunction(make("RG_Clients", &Clients, "String"));
  {
    auto* f = make("RG_Profile", &Profile, "String");
    f->AddParam("Float", "delay");
    f->AddParam("Float", "seconds");
    rtti->RegisterFunction(f);
  }
  rtti->RegisterFunction(make("RG_ProfileState", &ProfileState, "String"));
  rtti->RegisterFunction(make("RG_Graph", &Graph, "String"));
  {
    auto* f = make("RG_GraphNode", &GraphNode, "String");
    f->AddParam("Int32", "gen");
    f->AddParam("Int32", "index");
    f->AddParam("Bool", "removed");
    f->AddParam("Bool", "force");
    rtti->RegisterFunction(f);
  }
  {
    auto* f = make("RG_GraphShader", &GraphShader, "String");
    f->AddParam("Int32", "gen");
    f->AddParam("Int32", "row");
    f->AddParam("Int32", "where");
    f->AddParam("Bool", "skip");
    rtti->RegisterFunction(f);
  }
  {
    auto* f = make("RG_GraphRuleOff", &GraphRuleOff, "String");
    f->AddParam("Int32", "rule");
    rtti->RegisterFunction(f);
  }
  {
    auto* f = make("RG_GraphAction", &GraphAction, "Bool");
    f->AddParam("Int32", "action");
    rtti->RegisterFunction(f);
  }
  say(RG_LOG_INFO, "ui: native functions registered - RG_Ping, RG_Get, RG_Set, RG_Status, RG_Clients, RG_Profile*, RG_Graph*");
}

void on_register() {}

}  // namespace

void attach() {
  auto* rtti = RED4ext::CRTTISystem::Get();
  rtti->AddRegisterCallback(on_register);
  rtti->AddPostRegisterCallback(post_register);
}

void log_status(const char* why) { log_report(why, status()); }

}  // namespace rg_ui
