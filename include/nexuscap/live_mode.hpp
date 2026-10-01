#pragma once
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "nexuscap/lossless_fg.hpp"
#include "nexuscap/settings_store.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace nexuscap {

// Live overlay — always available (no live.on gate). Console colors for stream branding.
// 0–2 stay PS5 / Xbox Series S / Switch 2 so persisted ints keep working.
enum class LiveConsole : int {
  Ps5 = 0,
  XboxSs = 1,
  Switch2 = 2,
  Ps4 = 3,
  Ps3 = 4,
  XboxSx = 5,
  XboxOne = 6,
  Switch = 7,
  Ps5Pro = 8,
};

inline constexpr int kLiveConsoleCount = 9;

// Lobby combo + overlay click menu (family order, not enum order).
inline constexpr LiveConsole kLiveConsoleOrder[kLiveConsoleCount] = {
    LiveConsole::Ps5,    LiveConsole::Ps5Pro, LiveConsole::Ps4,     LiveConsole::Ps3,
    LiveConsole::XboxSx, LiveConsole::XboxSs, LiveConsole::XboxOne, LiveConsole::Switch2,
    LiveConsole::Switch,
};

inline LiveConsole clamp_live_console(int v) {
  if (v < 0 || v >= kLiveConsoleCount) return LiveConsole::Ps5;
  return static_cast<LiveConsole>(v);
}

inline LiveConsole parse_live_console(const std::string& s) {
  if (s == "ps5_pro" || s == "ps5pro" || s == "ps5-pro") return LiveConsole::Ps5Pro;
  if (s == "ps4") return LiveConsole::Ps4;
  if (s == "ps3") return LiveConsole::Ps3;
  if (s == "xbox_sx" || s == "xsx" || s == "series_x") return LiveConsole::XboxSx;
  if (s == "xbox_ss" || s == "xbox" || s == "xss") return LiveConsole::XboxSs;
  if (s == "xbox_one" || s == "xboxone" || s == "xone") return LiveConsole::XboxOne;
  if (s == "switch2" || s == "nsw2") return LiveConsole::Switch2;
  if (s == "switch" || s == "nsw" || s == "switch1") return LiveConsole::Switch;
  return LiveConsole::Ps5;
}

inline const char* live_console_key(LiveConsole c) {
  switch (c) {
    case LiveConsole::Ps4:
      return "ps4";
    case LiveConsole::Ps3:
      return "ps3";
    case LiveConsole::XboxSx:
      return "xbox_sx";
    case LiveConsole::XboxSs:
      return "xbox_ss";
    case LiveConsole::XboxOne:
      return "xbox_one";
    case LiveConsole::Switch2:
      return "switch2";
    case LiveConsole::Switch:
      return "switch";
    case LiveConsole::Ps5Pro:
      return "ps5_pro";
    case LiveConsole::Ps5:
    default:
      return "ps5";
  }
}

inline const char* live_console_label(LiveConsole c) {
  switch (c) {
    case LiveConsole::Ps4:
      return "PS4";
    case LiveConsole::Ps3:
      return "PS3";
    case LiveConsole::XboxSx:
      return "Xbox Series X";
    case LiveConsole::XboxSs:
      return "Xbox Series S";
    case LiveConsole::XboxOne:
      return "Xbox One";
    case LiveConsole::Switch2:
      return "Switch 2";
    case LiveConsole::Switch:
      return "Switch";
    case LiveConsole::Ps5Pro:
      return "PS5 Pro";
    case LiveConsole::Ps5:
    default:
      return "PS5";
  }
}

// Fixed brand colors for live overlay console line.
inline COLORREF live_console_color(LiveConsole c) {
  switch (c) {
    case LiveConsole::Ps4:
      return RGB(0, 85, 185);  // PS4 brighter blue
    case LiveConsole::Ps3:
      return RGB(0, 32, 90);  // PS3 darker blue
    case LiveConsole::XboxSx:
    case LiveConsole::XboxSs:
    case LiveConsole::XboxOne:
      return RGB(16, 124, 16);  // Xbox green
    case LiveConsole::Switch2:
    case LiveConsole::Switch:
      return RGB(230, 0, 18);  // Switch red
    case LiveConsole::Ps5Pro:
      return RGB(0, 112, 210);  // PS5 Pro brighter blue
    case LiveConsole::Ps5:
    default:
      return RGB(0, 55, 145);  // PS5 dark blue
  }
}

inline void live_console_rgb(LiveConsole c, unsigned char& r, unsigned char& g, unsigned char& b) {
  const COLORREF rgb = live_console_color(c);
  r = GetRValue(rgb);
  g = GetGValue(rgb);
  b = GetBValue(rgb);
}

inline void format_overlay_console_chip(char* out, size_t n, LiveConsole c) {
  if (!out || n == 0) return;
  std::snprintf(out, n, "%s", live_console_label(c));
}

inline const char* overlay_console_label_or_null(bool show, LiveConsole c) {
  return show ? live_console_label(c) : nullptr;
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

// Live strip: same Pass tag as Mini/ADV (1x / 1.5x / 2x / 2x+ / 3x / 3x+).
inline void format_live_dlss5_line(char* out, size_t out_n, bool dlss5_on, int pass_count = 1,
                                  bool /*feed_on*/ = true) {
  if (!out || out_n == 0) return;
  if (!dlss5_on) {
    std::snprintf(out, out_n, "DLSS off");
    return;
  }
  std::snprintf(out, out_n, "DLSS %s", nr_pass_count_short(pass_count));
}

// Frame Gen on the live strip. FG is always uppercase: "FG x2" / "FG x3" / "FG x4" / "FG off".
inline void format_live_fg_line(char* out, size_t out_n, bool fg_on, int fg_mult, int queue = 0,
                               bool adaptive = false, int target = kFgAdaptiveTargetDefault,
                               int flow_scale = -1) {
  if (!out || out_n == 0) return;
  int m = fg_mult;
  if (m < 1) m = 1;
  if (m > 4) m = 4;
  const int q = queue < 0 ? 0 : (queue > 2 ? 2 : queue);
  if (fg_on && m >= 2) {
    char tag[40]{};
    format_fg_mode_tag(tag, sizeof(tag), adaptive, m, target, flow_scale);
    std::snprintf(out, out_n, "FG %s f%d", tag, q);
  } else {
    std::snprintf(out, out_n, "FG off f%d", q);
  }
}

// Same RGB as the frametime sparkline (cap / x2 / x3 / x4).
inline void live_fps_ft_rgb(int trace_k, bool stable, unsigned char& r, unsigned char& g, unsigned char& b) {
  switch (trace_k) {
    case 2:
      r = 255;
      g = 196;
      b = 72;
      return;
    case 3:
      r = 210;
      g = 120;
      b = 255;
      return;
    case 4:
      r = 80;
      g = 255;
      b = 160;
      return;
    default:
      if (stable) {
        r = 90;
        g = 220;
        b = 255;
      } else {
        r = 255;
        g = 140;
        b = 90;
      }
  }
}

struct LiveFpsPart {
  char text[24]{};
  unsigned char r = 180;
  unsigned char g = 184;
  unsigned char b = 190;
};

// Lobby preview + tests: same chips as the session live strip.
struct LiveStripLines {
  char console[48]{};
  char dlss[32]{};
  char fg[56]{};
  LiveFpsPart fps_lab;
  LiveFpsPart fps_cap;
  LiveFpsPart fps_sep;
  LiveFpsPart fps_fg;
  char ft[40]{};
  bool has_console = false;
  bool has_fg_fps = false;
};

inline void fill_live_strip_lines(LiveStripLines& o, bool show_console, LiveConsole cons, bool dlss_on,
                                 int pass_count, bool fg_on, int fg_mult, double fps_cap,
                                 double fps_fg_est = 0.0, bool ft_stable = true, int fg_queue = 0,
                                 double frame_ms = -1.0, bool fg_adaptive = false, int fg_target = 60,
                                 int flow_scale = -1) {
  o = LiveStripLines{};
  o.has_console = show_console;
  if (show_console) format_overlay_console_chip(o.console, sizeof(o.console), cons);
  format_live_dlss5_line(o.dlss, sizeof(o.dlss), dlss_on, pass_count, true);
  format_live_fg_line(o.fg, sizeof(o.fg), fg_on, fg_mult, fg_queue, fg_adaptive, fg_target, flow_scale);
  std::snprintf(o.fps_lab.text, sizeof(o.fps_lab.text), "FPS  ");
  o.fps_lab.r = 180;
  o.fps_lab.g = 184;
  o.fps_lab.b = 190;
  live_fps_ft_rgb(1, ft_stable, o.fps_cap.r, o.fps_cap.g, o.fps_cap.b);
  std::snprintf(o.fps_cap.text, sizeof(o.fps_cap.text), "%.1f", fps_cap);
  int m = fg_mult;
  if (m < 1) m = 1;
  if (m > 4) m = 4;
  if (fg_on && m >= 2) {
    o.has_fg_fps = true;
    std::snprintf(o.fps_sep.text, sizeof(o.fps_sep.text), " | ");
    o.fps_sep.r = 140;
    o.fps_sep.g = 144;
    o.fps_sep.b = 150;
    const double fg = fps_fg_est > 0 ? fps_fg_est
                                     : (fg_adaptive ? static_cast<double>(clamp_fg_adaptive_target_stored(fg_target))
                                                    : (fps_cap * static_cast<double>(m)));
    std::snprintf(o.fps_fg.text, sizeof(o.fps_fg.text), "%.1f", fg);
    live_fps_ft_rgb(m, ft_stable, o.fps_fg.r, o.fps_fg.g, o.fps_fg.b);
  }
  if (frame_ms >= 0.0) format_overlay_frametime_live(o.ft, sizeof(o.ft), frame_ms, fg_on, fg_mult);
}

}  // namespace nexuscap
