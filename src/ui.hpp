#pragma once

// REDGraph's channel to its CET window: native GLOBAL functions (RG_*) in the game's RTTI.

namespace rg_ui {

void attach();                     // at Load: our registration into the RTTI post-register callbacks
void log_status(const char* why);  // the status report into the host's log

}  // namespace rg_ui
