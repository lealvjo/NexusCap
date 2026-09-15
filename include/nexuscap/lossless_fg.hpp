#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>

namespace nexuscap {

enum class FgStatus {
  Off,      // x1 / disabled
  Missing,  // exe not found
  Ready,    // process available, FG not active on our window
  On,       // FG toggled on
  Error,
};

inline const char* fg_status_name(FgStatus s) {
  switch (s) {
    case FgStatus::Off:
      return "off";
    case FgStatus::Missing:
      return "missing";
    case FgStatus::Ready:
      return "ready";
    case FgStatus::On:
      return "on";
    case FgStatus::Error:
      return "error";
  }
  return "off";
}

// Clamp / validate CLI multiplier: only 1,2,3,4 accepted.
inline bool parse_fg_multiplier(const std::string& raw, int& out) {
  if (raw == "1") {
    out = 1;
    return true;
  }
  if (raw == "2") {
    out = 2;
    return true;
  }
  if (raw == "3") {
    out = 3;
    return true;
  }
  if (raw == "4") {
    out = 4;
    return true;
  }
  return false;
}

inline int clamp_fg_multiplier(int m) {
  if (m < 1) return 1;
  if (m > 4) return 4;
  return m;
}

// Page Up cycle: 2 → 3 → 4 → 2 (x1 is off, not part of cycle).
inline int next_fg_multiplier(int m) {
  m = clamp_fg_multiplier(m);
  if (m <= 1) return 2;
  if (m == 2) return 3;
  if (m == 3) return 4;
  return 2;
}

inline bool fg_wants_process(int multiplier) { return clamp_fg_multiplier(multiplier) >= 2; }

// Lobby HUD must NOT show "60 | 120" as if FG were already running — only "armado".
inline void format_lobby_hud_fps_line(char* out, size_t out_n, double fps, bool fg_enabled, int fg_mult) {
  if (!out || out_n == 0) return;
  const int m = clamp_fg_multiplier(fg_mult);
  if (fg_enabled && m >= 2) {
    std::snprintf(out, out_n, "FPS  %.0f   FG x%d armado", fps, m);
  } else {
    std::snprintf(out, out_n, "FPS  %.0f", fps);
  }
}

inline bool lobby_fps_line_implies_active_fg(const char* line) {
  if (!line) return false;
  // Old misleading "60 | 120" estimate, or explicit FG ON runtime.
  return std::strstr(line, " | ") != nullptr;
}

// Pure helpers (unit-tested): mira-lock profile fragments.
std::string default_steam_lossless_exe();
std::string resolve_lossless_exe(const std::string& override_path);
bool lossless_exe_exists(const std::string& path);

// Patch or inject mira-lock tags into existing Settings.xml text. Never writes Flow 70.
std::string patch_mira_settings_xml(const std::string& xml, int multiplier);

// Build a minimal valid Settings.xml with mira lock + LSFG3 FIXED.
std::string build_mira_settings_xml(int multiplier);

// Patch [lsfg] section in config.ini text for mira lock.
std::string patch_lsfg_ini(const std::string& ini);

struct FgState {
  FgStatus status = FgStatus::Off;
  int multiplier = 1;
  int flow_scale = 100;
  std::string capture_api = "WGC";
  std::string backend = "lsfg3";
  DWORD pid = 0;
  std::string exe_path;
  std::string last_error;
  bool active = false;  // FG currently applied to our window
};

class LosslessFgController {
public:
  LosslessFgController() = default;
  ~LosslessFgController() { shutdown(); }

  LosslessFgController(const LosslessFgController&) = delete;
  LosslessFgController& operator=(const LosslessFgController&) = delete;

  // Configure before use. multiplier 1 = off semantics.
  void configure(int multiplier, const std::string& exe_override = {});

  const FgState& state() const { return state_; }

  // Apply mira profile to %LOCALAPPDATA%\Lossless Scaling\Settings.xml (+ Steam config.ini).
  bool apply_profile(std::string& err);

  // Start LS process under Job Object if multiplier >= 2 and exe exists.
  bool ensure_process(std::string& err);

  // Toggle FG on target HWND via LS hotkey (F24). Starts process if needed.
  // Turning OFF kills the LS process so a leftover instance cannot keep injecting frames.
  bool toggle(HWND target, std::string& err);

  // Change multiplier (2/3/4). If FG was on, re-toggle to apply.
  bool set_multiplier(int multiplier, HWND target, std::string& err);

  // Cycle 2→3→4→2 (from 1 goes to 2).
  bool cycle_multiplier(HWND target, std::string& err);

  void shutdown();

  // How many LosslessScaling.exe are running (0 = none). Used to avoid duplicate instances.
  static unsigned count_running_lossless_processes();

private:
  bool send_ls_hotkey();
  bool focus_window(HWND hwnd);
  void hide_ls_windows();
  bool write_settings_file(const std::string& xml, std::string& err);
  bool write_ini_alongside_exe(std::string& err);
  bool adopt_existing_lossless(std::string& err);
  void terminate_extra_lossless_processes(DWORD keep_pid);

  FgState state_;
  HANDLE job_ = nullptr;
  HANDLE proc_ = nullptr;
  bool configured_ = false;
};

}  // namespace nexuscap
