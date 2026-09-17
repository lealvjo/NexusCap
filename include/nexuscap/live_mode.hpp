#pragma once
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "nexuscap/settings_store.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace nexuscap {

// Live overlay — always available (no live.on gate). Console colors for stream branding.
enum class LiveConsole : int {
  Ps5 = 0,
  XboxSs = 1,
  Switch2 = 2,
};

inline LiveConsole clamp_live_console(int v) {
  if (v < 0) return LiveConsole::Ps5;
  if (v > 2) return LiveConsole::Ps5;
  return static_cast<LiveConsole>(v);
}

inline LiveConsole parse_live_console(const std::string& s) {
  if (s == "xbox_ss" || s == "xbox" || s == "xss") return LiveConsole::XboxSs;
  if (s == "switch2" || s == "switch" || s == "nsw2") return LiveConsole::Switch2;
  return LiveConsole::Ps5;
}

inline const char* live_console_key(LiveConsole c) {
  switch (c) {
    case LiveConsole::XboxSs:
      return "xbox_ss";
    case LiveConsole::Switch2:
      return "switch2";
    case LiveConsole::Ps5:
    default:
      return "ps5";
  }
}

inline const char* live_console_label(LiveConsole c) {
  switch (c) {
    case LiveConsole::XboxSs:
      return "Xbox Series S";
    case LiveConsole::Switch2:
      return "Switch 2";
    case LiveConsole::Ps5:
    default:
      return "PS5";
  }
}

// Fixed brand colors for live overlay console line.
inline COLORREF live_console_color(LiveConsole c) {
  switch (c) {
    case LiveConsole::XboxSs:
      return RGB(16, 124, 16);  // Xbox green
    case LiveConsole::Switch2:
      return RGB(230, 0, 18);  // Switch red
    case LiveConsole::Ps5:
    default:
      return RGB(0, 55, 145);  // PS5 dark blue
  }
}

inline std::string live_on_path_in(const std::string& app_data) {
  if (app_data.empty()) return {};
  return app_data + "\\live.on";
}

inline std::string live_on_path() { return live_on_path_in(app_data_dir()); }

inline bool live_mode_unlocked_at(const std::string& path) {
  if (path.empty()) return false;
  const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
  if (n <= 1) return false;
  std::wstring w(static_cast<size_t>(n - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
  const DWORD attr = GetFileAttributesW(w.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline bool live_mode_unlocked() { return live_mode_unlocked_at(live_on_path()); }

// Overlay live follows the checkbox; live.on is optional legacy unlock only.
inline bool live_overlay_active(bool /*unlocked*/, bool live_overlay_flag) {
  return live_overlay_flag;
}

// Cap FPS alone, or "cap | FG" when Frame Gen is on (same split idea as session HUD).
inline void format_live_fps_line(char* out, size_t out_n, double fps_cap, double fps_fg_est, bool fg_on,
                                 int fg_mult) {
  if (!out || out_n == 0) return;
  const int m = (std::max)(1, fg_mult);
  if (fg_on && m >= 2) {
    const double fg = fps_fg_est > 0 ? fps_fg_est : (fps_cap * static_cast<double>(m));
    std::snprintf(out, out_n, "%.1f | %.1f", fps_cap, fg);
  } else {
    std::snprintf(out, out_n, "%.1f", fps_cap);
  }
}

inline void format_live_dlss5_line(char* out, size_t out_n, bool dlss5_on, int pass_count = 1,
                                  bool feed_on = true) {
  if (!out || out_n == 0) return;
  format_dlss5_stack_hud_line(out, out_n, dlss5_on, feed_on, pass_count);
}

}  // namespace nexuscap
