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

// Lobby HUD: checkbox on = "off" until session PageDn (never imply running dual FPS).
inline void format_lobby_hud_fps_line(char* out, size_t out_n, double fps, bool fg_enabled, int fg_mult) {
  if (!out || out_n == 0) return;
  const int m = clamp_fg_multiplier(fg_mult);
  if (fg_enabled && m >= 2) {
    std::snprintf(out, out_n, "FPS  %.0f   FG x%d off", fps, m);
  } else {
    std::snprintf(out, out_n, "FPS  %.0f", fps);
  }
}

// Session stats when FG is ON: real capture FPS | estimated FG output FPS.
inline void format_session_hud_fps_line(char* out, size_t out_n, double fps_cap, double fps_fg_est, bool fg_on,
                                       int fg_mult, bool compact) {
  if (!out || out_n == 0) return;
  const int m = clamp_fg_multiplier(fg_mult);
  if (fg_on && m >= 2) {
    const double fg = fps_fg_est > 0 ? fps_fg_est : (fps_cap * static_cast<double>(m));
    if (compact) {
      std::snprintf(out, out_n, "FPS  %.1f | %.1f  cap|FG x%d ON", fps_cap, fg, m);
    } else {
      std::snprintf(out, out_n, "FPS     %.1f | %.1f   cap|FG x%d ON", fps_cap, fg, m);
    }
  } else if (m >= 2) {
    // Prefer "off" over "pronto" — pronto looked like FG was already running.
    if (compact) {
      std::snprintf(out, out_n, "FPS  %.1f   FG x%d off", fps_cap, m);
    } else {
      std::snprintf(out, out_n, "FPS     %.1f   FG x%d off (PageDn)", fps_cap, m);
    }
  } else {
    if (compact) {
      std::snprintf(out, out_n, "FPS  %.1f", fps_cap);
    } else {
      std::snprintf(out, out_n, "FPS     %.1f", fps_cap);
    }
  }
}

// Colored segments for mini overlay paint (r,g,b 0–255). Sep = " · ".
struct MiniOverlaySeg {
  char text[160]{};
  unsigned char r = 210;
  unsigned char g = 230;
  unsigned char b = 255;
};

inline constexpr int kMiniOverlayMaxSegs = 28;

inline void mini_seg_set(MiniOverlaySeg& s, const char* text, unsigned char r, unsigned char g,
                         unsigned char b) {
  s.r = r;
  s.g = g;
  s.b = b;
  if (!text) {
    s.text[0] = '\0';
    return;
  }
  std::snprintf(s.text, sizeof(s.text), "%s", text);
}

inline void mini_seg_sep(MiniOverlaySeg& s) { mini_seg_set(s, " · ", 80, 95, 115); }

inline unsigned mini_hw_token_rgb(const char* tok, unsigned char& r, unsigned char& g, unsigned char& b) {
  if (!tok || !tok[0]) return 0;
  // Power
  if (std::strstr(tok, "W") && (tok[0] >= '0' && tok[0] <= '9')) {
    r = 140;
    g = 255;
    b = 220;
    return 1;
  }
  // VRAM / RAM compact
  if (tok[0] == 'V' || (tok[0] == 'R' && tok[1] >= '0' && tok[1] <= '9')) {
    r = 120;
    g = 255;
    b = 180;
    return 1;
  }
  // GPU SKU
  if (std::strstr(tok, "RTX") || std::strstr(tok, "GTX") || std::strstr(tok, "RX ") ||
      std::strstr(tok, "Arc") || std::strstr(tok, "GPU")) {
    r = 160;
    g = 240;
    b = 255;
    return 1;
  }
  // CPU SKU / fallback
  r = 255;
  g = 230;
  b = 90;
  return 1;
}

// Fills colored segments; returns count. Also builds joined line0/line1 like format_mini_overlay_lines.
inline int format_mini_overlay_segments(MiniOverlaySeg* segs, int max_segs, char* line0, size_t n0,
                                        char* line1, size_t n1, double fps_cap, double fps_fg_est, bool fg_on,
                                        int fg_mult, bool dlss5_on, bool sync_image, double frame_ms,
                                        double ft_avg_ms, double ft_p95_ms, double ft_max_ms, bool ft_stable,
                                        double e2e_ms, double present_ms, uint64_t stalls, uint64_t clusters,
                                        uint64_t dropped, int capture_w, int capture_h, int work_w = 0,
                                        int work_h = 0, int output_w = 0, int output_h = 0,
                                        int scale_percent = 100, bool has_stats600 = false,
                                        double s600_fps = 0, double s600_p95_e2e = 0,
                                        double s600_p95_present = 0, uint64_t s600_stalls = 0,
                                        const char* renodx_preset = nullptr, const char* hw_compact = nullptr,
                                        double dlss_ms = 0, uint64_t frames = 0, double s600_max_interval = 0,
                                        const char* fg_backend = nullptr, double target_fps = 0) {
  if (line0 && n0) line0[0] = '\0';
  if (line1 && n1) line1[0] = '\0';
  int n = 0;
  auto push = [&](const char* text, unsigned char r, unsigned char g, unsigned char b) {
    if (!segs || n >= max_segs || !text || !text[0]) return;
    mini_seg_set(segs[n++], text, r, g, b);
  };
  auto push_sep = [&]() {
    if (!segs || n >= max_segs) return;
    mini_seg_sep(segs[n++]);
  };

  char fps_part[96]{};
  format_session_hud_fps_line(fps_part, sizeof(fps_part), fps_cap, fps_fg_est, fg_on, fg_mult, true);
  push(fps_part, 120, 255, 160);

  if (fg_backend && fg_backend[0] && fg_on) {
    char fg_extra[40]{};
    std::snprintf(fg_extra, sizeof(fg_extra), " [%s]", fg_backend);
    push(fg_extra, 120, 255, 200);
  }

  push_sep();
  char nr_part[48]{};
  const char* preset = (renodx_preset && renodx_preset[0]) ? renodx_preset : "";
  if (dlss5_on && preset[0] && dlss_ms > 0.01) {
    std::snprintf(nr_part, sizeof(nr_part), "NR ON/%s %.1fms", preset, dlss_ms);
  } else if (dlss5_on && preset[0]) {
    std::snprintf(nr_part, sizeof(nr_part), "NR ON/%s", preset);
  } else if (dlss5_on && dlss_ms > 0.01) {
    std::snprintf(nr_part, sizeof(nr_part), "NR ON %.1fms", dlss_ms);
  } else {
    std::snprintf(nr_part, sizeof(nr_part), "NR %s", dlss5_on ? "ON" : "off");
  }
  push(nr_part, dlss5_on ? 180 : 140, dlss5_on ? 160 : 150, dlss5_on ? 255 : 170);

  push_sep();
  if (sync_image) {
    push("VSYNC", 120, 220, 255);
  } else {
    push("TEAR", 255, 160, 90);
  }

  push_sep();
  char lat[64]{};
  std::snprintf(lat, sizeof(lat), "p %.1f · e2e %.1f", present_ms, e2e_ms);
  push(lat, 120, 255, 160);

  push_sep();
  char cnt[72]{};
  std::snprintf(cnt, sizeof(cnt), "st%llu/cl%llu · frm %llu drop %llu",
                static_cast<unsigned long long>(stalls), static_cast<unsigned long long>(clusters),
                static_cast<unsigned long long>(frames), static_cast<unsigned long long>(dropped));
  push(cnt, 255, 180, 120);

  char res_part[80]{};
  if (capture_w > 0 && capture_h > 0) {
    const int ww = work_w > 0 ? work_w : capture_w;
    const int wh = work_h > 0 ? work_h : capture_h;
    const int ow = output_w > 0 ? output_w : capture_w;
    const int oh = output_h > 0 ? output_h : capture_h;
    if (ww != capture_w || wh != capture_h || ow != capture_w || oh != capture_h || scale_percent < 100) {
      std::snprintf(res_part, sizeof(res_part), "%dx%d>%dx%d>%dx%d@%d%%", capture_w, capture_h, ww, wh, ow,
                    oh, scale_percent > 0 ? scale_percent : 100);
    } else if (target_fps > 0.5) {
      std::snprintf(res_part, sizeof(res_part), "%dx%d@%.0f", capture_w, capture_h, target_fps);
    } else {
      std::snprintf(res_part, sizeof(res_part), "%dx%d", capture_w, capture_h);
    }
  }
  push_sep();
  push(res_part[0] ? res_part : "-", 120, 220, 255);

  push_sep();
  char ft_part[112]{};
  std::snprintf(ft_part, sizeof(ft_part), "ft %.1f avg %.1f p95 %.1f max %.1f %s", frame_ms, ft_avg_ms,
                ft_p95_ms, ft_max_ms, ft_stable ? "STABLE" : "UNSTABLE");
  if (ft_stable) {
    push(ft_part, 120, 255, 160);
  } else {
    push(ft_part, 255, 120, 100);
  }

  push_sep();
  char s600_part[120]{};
  if (has_stats600) {
    if (s600_max_interval > 0.01) {
      std::snprintf(s600_part, sizeof(s600_part),
                    "L600 fps %.1f p95e %.1f p95p %.1f st%llu max %.1f", s600_fps, s600_p95_e2e,
                    s600_p95_present, static_cast<unsigned long long>(s600_stalls), s600_max_interval);
    } else {
      std::snprintf(s600_part, sizeof(s600_part), "L600 fps %.1f p95e %.1f p95p %.1f st%llu", s600_fps,
                    s600_p95_e2e, s600_p95_present, static_cast<unsigned long long>(s600_stalls));
    }
  } else {
    std::snprintf(s600_part, sizeof(s600_part), "L600…");
  }
  push(s600_part, 255, 180, 120);

  if (hw_compact && hw_compact[0]) {
    // Split "CPU | GPU | V | R | W" into colored tokens.
    const char* p = hw_compact;
    while (*p && n < max_segs - 1) {
      while (*p == ' ') ++p;
      if (!*p) break;
      const char* start = p;
      while (*p && !(p[0] == ' ' && p[1] == '|' && p[2] == ' ')) ++p;
      size_t len = static_cast<size_t>(p - start);
      while (len > 0 && start[len - 1] == ' ') --len;
      char tok[96]{};
      if (len >= sizeof(tok)) len = sizeof(tok) - 1;
      if (len > 0) {
        std::memcpy(tok, start, len);
        tok[len] = '\0';
      }
      if (tok[0]) {
        push_sep();
        unsigned char r = 255, g = 230, b = 90;
        mini_hw_token_rgb(tok, r, g, b);
        push(tok, r, g, b);
      }
      if (p[0] == ' ' && p[1] == '|') {
        p += 3;
      } else if (*p == '|') {
        ++p;
      }
    }
  }

  if (line0 && n0 > 0 && segs && n > 0) {
    size_t used = 0;
    for (int i = 0; i < n; ++i) {
      const size_t len = std::strlen(segs[i].text);
      if (used + len + 1 > n0) break;
      std::memcpy(line0 + used, segs[i].text, len);
      used += len;
      line0[used] = '\0';
    }
  }
  return n;
}

// Mini overlay: ONE dense top-bar line with ADV+HW packed as complete as possible.
// line1 kept for API compat (always empty).
inline void format_mini_overlay_lines(char* line0, size_t n0, char* line1, size_t n1, double fps_cap,
                                      double fps_fg_est, bool fg_on, int fg_mult, bool dlss5_on,
                                      bool sync_image, double frame_ms, double ft_avg_ms, double ft_p95_ms,
                                      double ft_max_ms, bool ft_stable, double e2e_ms, double present_ms,
                                      uint64_t stalls, uint64_t clusters, uint64_t dropped, int capture_w,
                                      int capture_h, int work_w = 0, int work_h = 0, int output_w = 0,
                                      int output_h = 0, int scale_percent = 100, bool has_stats600 = false,
                                      double s600_fps = 0, double s600_p95_e2e = 0,
                                      double s600_p95_present = 0, uint64_t s600_stalls = 0,
                                      const char* renodx_preset = nullptr, const char* hw_compact = nullptr,
                                      double dlss_ms = 0, uint64_t frames = 0, double s600_max_interval = 0,
                                      const char* fg_backend = nullptr, double target_fps = 0) {
  MiniOverlaySeg segs[kMiniOverlayMaxSegs]{};
  format_mini_overlay_segments(segs, kMiniOverlayMaxSegs, line0, n0, line1, n1, fps_cap, fps_fg_est, fg_on,
                               fg_mult, dlss5_on, sync_image, frame_ms, ft_avg_ms, ft_p95_ms, ft_max_ms,
                               ft_stable, e2e_ms, present_ms, stalls, clusters, dropped, capture_w, capture_h,
                               work_w, work_h, output_w, output_h, scale_percent, has_stats600, s600_fps,
                               s600_p95_e2e, s600_p95_present, s600_stalls, renodx_preset, hw_compact, dlss_ms,
                               frames, s600_max_interval, fg_backend, target_fps);
}

// Back-compat one-liner.
inline void format_mini_overlay_line(char* out, size_t out_n, double fps_cap, double fps_fg_est, bool fg_on,
                                    int fg_mult, bool dlss5_on, double frame_ms, double e2e_ms,
                                    uint64_t stalls, uint64_t clusters, int capture_w, int capture_h,
                                    int work_w = 0, int work_h = 0, int output_w = 0, int output_h = 0,
                                    int scale_percent = 100) {
  if (!out || out_n == 0) return;
  char a[512]{};
  char b[8]{};
  format_mini_overlay_lines(a, sizeof(a), b, sizeof(b), fps_cap, fps_fg_est, fg_on, fg_mult, dlss5_on,
                            /*sync_image=*/true, frame_ms, frame_ms, frame_ms, frame_ms, true, e2e_ms,
                            /*present=*/0, stalls, clusters, /*dropped=*/0, capture_w, capture_h, work_w,
                            work_h, output_w, output_h, scale_percent);
  std::snprintf(out, out_n, "%s", a);
}

inline void format_session_hud_fg_status_line(char* out, size_t out_n, bool fg_on, int fg_mult,
                                             const char* backend, int flow_scale, const char* capture_api,
                                             const char* status, double fps_cap = 0, double fps_fg_est = 0) {
  if (!out || out_n == 0) return;
  const int m = clamp_fg_multiplier(fg_mult);
  const char* be = backend && backend[0] ? backend : "lsfg3";
  const char* api = capture_api && capture_api[0] ? capture_api : "WGC";
  const char* st = status && status[0] ? status : (fg_on ? "On" : "Ready");
  if (fg_on && m >= 2) {
    const double fg = fps_fg_est > 0 ? fps_fg_est : (fps_cap > 0 ? fps_cap * static_cast<double>(m) : 0);
    if (fps_cap > 0 && fg > 0) {
      std::snprintf(out, out_n, "FG      ON   x%d  %s  flow%d  %s  |  %.0f->%.0f", m, be, flow_scale, api,
                    fps_cap, fg);
    } else {
      std::snprintf(out, out_n, "FG      ON   x%d  %s  flow%d  %s", m, be, flow_scale, api);
    }
  } else {
    std::snprintf(out, out_n, "FG      %s  x%d  %s  flow%d  %s  [%s]", fg_on ? "ON" : "off", m, be,
                  flow_scale, api, st);
  }
}

// True when session FPS line shows real|FG (required while FG runtime is ON).
inline bool session_hud_fps_shows_cap_fg_split(const char* line) {
  if (!line) return false;
  return std::strstr(line, " | ") != nullptr && std::strstr(line, "cap|FG") != nullptr;
}

// Lobby must not imply active FG with a dual estimate.
inline bool lobby_fps_line_implies_active_fg(const char* line) {
  if (!line) return false;
  return std::strstr(line, " | ") != nullptr;
}

// Preferred FG multiplier stays armed (≥2) even when lobby checkbox is off so PageDn works.
inline int armed_fg_multiplier(bool /*fg_enabled*/, int fg_mult) {
  return clamp_fg_multiplier(fg_mult >= 2 ? fg_mult : 2);
}

// Pure helpers (unit-tested): mira-lock profile fragments.
std::string default_steam_lossless_exe();
std::string resolve_lossless_exe(const std::string& override_path);
bool lossless_exe_exists(const std::string& path);

// Patch or inject mira-lock tags into existing Settings.xml text. Never writes Flow 70.
std::string patch_mira_settings_xml(const std::string& xml, int multiplier);

// Build a minimal valid Settings.xml with mira lock + LSFG3 FIXED.
std::string build_mira_settings_xml(int multiplier);

// Read LSFG3Multiplier from Settings.xml text; 0 = missing/invalid.
inline int parse_lsfg3_multiplier_from_xml(const std::string& xml) {
  const std::string open = "<LSFG3Multiplier>";
  const std::string close = "</LSFG3Multiplier>";
  const auto a = xml.find(open);
  if (a == std::string::npos) return 0;
  const auto b = xml.find(close, a + open.size());
  if (b == std::string::npos) return 0;
  const std::string raw = xml.substr(a + open.size(), b - (a + open.size()));
  int m = 0;
  if (!parse_fg_multiplier(raw, m)) return 0;
  return clamp_fg_multiplier(m);
}

// Overlay xN only after disk confirms the same multiplier (no memory-only hope).
inline bool fg_multiplier_confirmed_on_disk(int want_mult, const std::string& settings_xml) {
  const int want = clamp_fg_multiplier(want_mult);
  if (want < 2) return parse_lsfg3_multiplier_from_xml(settings_xml) >= 2 || want <= 1;
  return parse_lsfg3_multiplier_from_xml(settings_xml) == want;
}

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
  bool busy() const { return pending_ != FgPending::None; }

  // Apply mira profile to %LOCALAPPDATA%\Lossless Scaling\Settings.xml (+ Steam config.ini).
  bool apply_profile(std::string& err);

  // Start LS process under Job Object if multiplier >= 2 and exe exists.
  // allow_adopt=false forces a fresh process (required after x2↔x3↔x4 so LS reloads XML).
  // No Sleep — caller waits via tick() so Present keeps running.
  bool ensure_process(std::string& err, bool allow_adopt = true);

  // Async FG ops (no Present freeze). Call tick() every frame until !busy().
  bool request_toggle(HWND target, std::string& err);
  bool request_set_multiplier(int multiplier, HWND target, std::string& err);
  bool request_cycle_multiplier(HWND target, std::string& err);
  // Returns true when an async op just finished this frame (success or fail in last_error).
  bool tick(double now_ms);

  // Sync wrappers (selftests). Prefer request_* + tick in the session loop.
  bool toggle(HWND target, std::string& err);
  bool set_multiplier(int multiplier, HWND target, std::string& err);
  bool cycle_multiplier(HWND target, std::string& err);

  void shutdown(DWORD wait_ms = 800);

  // How many LosslessScaling.exe are running (0 = none). Used to avoid duplicate instances.
  static unsigned count_running_lossless_processes();

  // If active but LS process gone → clear active (HUD never stays ON as false positive).
  void reconcile_process();

private:
  enum class FgPending {
    None,
    OnWaitLaunch,
    OnSendHotkey,
    OnConfirm,
    OffAfterHotkey,
    OffConfirmDead,
    MultWaitLaunch,
    MultAfterLaunch,
  };

  bool send_ls_hotkey();
  bool focus_window(HWND hwnd);
  void hide_ls_windows();
  bool write_settings_file(const std::string& xml, std::string& err);
  bool write_ini_alongside_exe(std::string& err);
  bool adopt_existing_lossless(std::string& err);
  void terminate_extra_lossless_processes(DWORD keep_pid);
  bool run_sync_until_idle(std::string& err);

  FgState state_;
  HANDLE job_ = nullptr;
  HANDLE proc_ = nullptr;
  bool configured_ = false;

  FgPending pending_ = FgPending::None;
  double pending_due_ms_ = 0;
  HWND pending_hwnd_ = nullptr;
  int pending_prev_mult_ = 2;
  int pending_want_mult_ = 2;
  bool pending_was_active_ = false;
  int pending_retries_ = 0;
  bool pending_finished_ok_ = false;
};

}  // namespace nexuscap
