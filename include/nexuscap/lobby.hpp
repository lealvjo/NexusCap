#pragma once
#include "nexuscap/config.hpp"
#include "nexuscap/settings_store.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace nexuscap {

// Blocking lobby UI. Returns false if user cancelled / closed without starting.
// On success, cfg is updated from lobby choices (device, scale, FG, hotkeys, etc.).
bool run_lobby(HINSTANCE hi, AppConfig& cfg, PersistedSettings& settings);

}  // namespace nexuscap
