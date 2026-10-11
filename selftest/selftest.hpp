#pragma once

// REDGraph's self-tests as a CLIENT plugin of their own (REDGraphSelfTest.dll) - installed only to test the host;
// REDGraph and the mods using it carry none of them. Its ini: REDGraphSelfTest.ini next to the DLL, section [Tests].

#include "rg_host.h"

namespace selftest {

namespace flow_probe {  // [Tests] FlowProbe: M5-M10 (t64 by name, our node, wrap / remove / replace, descriptors)
void register_with(const rg_host* host);
}  // namespace flow_probe

namespace rt_variant_test {  // [Tests] RtVariantTest 1 / 2: M12 (318cee44 + a "lit" raygen variant)
void register_with(const rg_host* host, int mode);
}  // namespace rt_variant_test

namespace rt_swap_test {  // [Tests] RtSwapTest 1: M3 swap (318cee44 whole, rgs_shadow_main "lit" in place)
void register_with(const rg_host* host, int mode);
}  // namespace rt_swap_test

namespace scope_test {  // [Tests] PatchScopeTest 1 / 2: M11 (the game's own 0c359133 swapped for itself, in a node)
void register_with(const rg_host* host, int mode);
}  // namespace scope_test

namespace lease_tex_test {  // [Tests] LeaseTexTest: M2 texture sources (a client texture, an engine one by name)
// runtime_hlsl: its companion compiled now from lease_tex.hlsl next to the DLL (HlslTest), not the checked-in blob
void register_with(const rg_host* host, bool on, bool runtime_hlsl);
}  // namespace lease_tex_test

namespace compose_test {  // [Tests] ComposeTest: M1 composition (a VS and a PS patch on one engine PSO)
void register_with(const rg_host* host, bool on);
}  // namespace compose_test

namespace state_test {  // [Tests] StateTest: M1 pipeline-state edits (state-only variants of the visible sky's PS)
void register_with(const rg_host* host, bool on);
}  // namespace state_test

namespace wrap_use_test {  // [Tests] WrapUseTest: M8 declarations (node_wrap_use keeps a transient alive into a wrap)
void register_with(const rg_host* host, bool on);
}  // namespace wrap_use_test

namespace draw_test {  // [Tests] DrawTest: the graphics wing of M4 (draw subscriptions, a draw companion, a pass after)
void register_with(const rg_host* host, bool on);
}  // namespace draw_test

namespace hlsl_test {  // [Tests] HlslTest: compile_hlsl (a good CS, a broken PS, an RT library refused)
bool available(const rg_host* host);  // the host has compile_hlsl
void register_with(const rg_host* host, bool on);
}  // namespace hlsl_test

}  // namespace selftest
