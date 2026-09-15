#pragma once
#include <cstdio>
#include <cstring>
#include <string>

#include "nexuscap/dlss_options.hpp"
#include "nexuscap/lossless_fg.hpp"
#include "nexuscap/settings_store.hpp"
#include "nexuscap/capture_format.hpp"
#include "nexuscap/system_metrics.hpp"
#include "nexuscap/version.hpp"

namespace nexuscap {

struct AppConfig {
  std::string device_name = "auto";
  int width = 2560;
  int height = 1440;
  double fps = 60.0;
  double hitch_threshold_ms = 25.0;
  std::string log_dir = "logs";
  bool smoke = false;
  bool fake_capture = false;
  bool selftest_cap = false;
  bool selftest_dlss = false;
  int smoke_frames = 120;
  bool exclusive_fullscreen = true;
  bool show_overlay = true;
  bool frames_limited = false;  // true when --frames was passed (auto-quit)
  bool show_lobby = true;       // false with --no-lobby / --smoke / selftests
  bool advanced_metrics = false;
  bool hw_info = false;  // painel Infos GPU/CPU
  bool hide_panel_on_capture = true;
  bool sync_image = false;  // vsync Present
  int scale_percent = 100;  // 25–100; 100 = native
  int renodx_preset = 0;    // Default / Natural / Cinematic
  bool neural_rendering = true;  // NeuralUplift/feed; Delete toggles; persists
  int video_fourcc = 0;     // NV12 default
  int color_space = 0;      // Padrao → BT.709
  int color_range = 0;      // Limitado
  int capture_buffering = 0;  // Desativado

  // Enhance: renodx (ReShade proxy + presets) | streamline (native) | off
  EnhanceBackend enhance = EnhanceBackend::Off;
  bool dlss = false;  // legacy alias: enables Streamline when enhance==Off
  DlssMode dlss_mode = DlssMode::Dlaa;
  float dlss_sharpness = 0.f;
  DlssMvecSource dlss_mvec = DlssMvecSource::OpticalFlow;

  // Lossless Scaling FG (Steam install, never bundled): 1=off, 2/3/4=multiplier ready
  int fg_multiplier = 1;
  std::string fg_path;  // optional override of LosslessScaling.exe
  bool selftest_fg = false;

  HotkeyConfig hotkeys{};
};

AppConfig parse_args(int argc, char** argv);

// Lobby Sessão/CFG → AppConfig (tested).
void apply_persisted_to_config(const PersistedSettings& s, AppConfig& cfg);

// HUD starts visible when "Exibir FPS" is on. "Ocultar painel" does NOT hide the metrics HUD.
inline bool initial_hud_visible(const SessionUiFlags& ui) { return ui.show_fps; }
inline bool initial_hud_visible(bool show_fps, bool /*hide_panel_on_capture*/) { return show_fps; }

// hide_panel_on_capture=true → captura limpa; false → mostra faixa de controle na sessão.
inline bool session_shows_control_panel(bool hide_panel_on_capture) { return !hide_panel_on_capture; }
inline bool session_shows_control_panel(const SessionUiFlags& ui) {
  return session_shows_control_panel(ui.hide_panel_on_capture);
}

// sync_image → DXGI Present sync interval (1 = vsync, 0 = uncapped / tearing OK).
inline unsigned present_sync_interval(bool sync_image) { return sync_image ? 1u : 0u; }

// Compact vs advanced metrics panel size (MetricsHud).
// Advanced needs room for long signal/hotkey lines; HW adds 5 stacked rows.
inline int hud_panel_width(bool advanced_metrics) { return advanced_metrics ? 920 : 560; }
inline int hud_panel_height(bool advanced_metrics, bool hw_info = false) {
  int h = advanced_metrics ? 640 : 220;
  // 5 lines: CPU / GPU / RAM / VRAM / PWR
  if (hw_info) h += 5 * 28;
  return h;
}

// Separate floating strip for CPU/GPU/RAM/VRAM/PWR when "Infos GPU / CPU" is on.
inline int hw_info_panel_width() { return 360; }
inline int hw_info_panel_height() { return 12 + 5 * 28; }

// Brand logo next to the exe (copied at build) or under assets/.
inline std::string logo_png_path_join(const std::string& exe_dir, const char* rel) {
  if (exe_dir.empty()) return rel ? rel : "";
  const char sep = (exe_dir.back() == '\\' || exe_dir.back() == '/') ? '\0' : '\\';
  std::string out = exe_dir;
  if (sep) out.push_back(sep);
  if (rel) out += rel;
  return out;
}
inline std::string logo_png_candidate_assets(const std::string& exe_dir) {
  return logo_png_path_join(exe_dir, "assets\\logo.png");
}
inline std::string logo_png_candidate_root(const std::string& exe_dir) {
  return logo_png_path_join(exe_dir, "logo.png");
}

// Win32 ICON resource id embedded in NexusCap.exe (resources/nexuscap.rc).
inline constexpr int kAppIconResourceId = 101;

// Lines on lobby HUD preview (0 = HUD hidden). Compact also shows gear fields.
inline int lobby_hud_preview_lines(bool show_fps, bool advanced_metrics, bool hw_info = false) {
  if (!show_fps) return 0;
  int n = advanced_metrics ? 8 : 6;
  if (hw_info) n += kSysMetricsLineCount;
  return n;
}

// Pure HUD text for live preview — gear/Sessão toggles must change these strings.
struct LobbyHudPreviewLines {
  int count = 0;
  char line[16][128]{};
};

inline bool lobby_hud_contains(const LobbyHudPreviewLines& h, const char* needle) {
  if (!needle) return false;
  for (int i = 0; i < h.count; ++i) {
    if (std::strstr(h.line[i], needle) != nullptr) return true;
  }
  return false;
}

inline LobbyHudPreviewLines make_lobby_hud_preview_lines(const PersistedSettings& s, double fps_ema) {
  LobbyHudPreviewLines out;
  if (!s.ui.show_fps) return out;

  const float fps = fps_ema > 1.0 ? static_cast<float>(fps_ema) : 0.f;
  const float ft = fps > 1.f ? (1000.f / fps) : 0.f;
  const int fgx = s.fg_enabled ? (s.fg_multiplier >= 2 ? s.fg_multiplier : 2) : 1;
  const int scale = clamp_scale_percent(s.scale_percent);
  int n = 0;

  if (s.ui.advanced_metrics) {
    std::snprintf(out.line[n++], 128, "NexusCap  [LIVE]  ADV");
    std::snprintf(out.line[n++], 128, "%s", s.device_name.empty() ? "auto" : s.device_name.c_str());
    const int ww = scaled_extent(s.width, scale);
    const int wh = scaled_extent(s.height, scale);
    if (scale < 100) {
      std::snprintf(out.line[n++], 128, "signal %dx%d → work %dx%d (%d%%)", s.width, s.height, ww, wh, scale);
    } else {
      std::snprintf(out.line[n++], 128, "signal  %dx%d @ %.0f  work 100%%", s.width, s.height, s.fps);
    }
    if (s.fg_enabled) {
      std::snprintf(out.line[n++], 128, "FPS  %.0f | %.0f   FG x%d", fps, fps * static_cast<float>(fgx), fgx);
    } else {
      std::snprintf(out.line[n++], 128, "FPS  %.0f", fps);
    }
    std::snprintf(out.line[n++], 128, "frametime  %.1f ms", ft);
    std::snprintf(out.line[n++], 128, "FG  %s  x%d   scale %d%%", s.fg_enabled ? "ON" : "off", fgx, scale);
    std::snprintf(out.line[n++], 128, "RenoDX %s  %s  %s", s.enhance_renodx ? "ON" : "off",
                  renodx_visual_preset_name(s.renodx_preset), s.ui.fullscreen ? "fullscreen" : "windowed");
    std::snprintf(out.line[n++], 128, "%s   RGB work ~%d%%", s.ui.sync_image ? "SYNC VSYNC" : "SYNC TEAR",
                  static_cast<int>(scale_pixel_ratio(scale) * 100.0 + 0.5));
    if (s.ui.hw_info) {
      SystemMetricsSample blank;
      char hw[kSysMetricsLineCount][64]{};
      const int hn = format_sys_metrics_lines(hw, kSysMetricsLineCount, blank);
      for (int i = 0; i < hn && n < 16; ++i) {
        std::snprintf(out.line[n++], 128, "%s", hw[i]);
      }
    }
  } else {
    std::snprintf(out.line[n++], 128, "NexusCap  [LIVE]");
    std::snprintf(out.line[n++], 128, "%dx%d @ %.0f", s.width, s.height, s.fps);
    if (s.fg_enabled) {
      std::snprintf(out.line[n++], 128, "FPS  %.0f | %.0f  FG x%d", fps, fps * static_cast<float>(fgx), fgx);
    } else {
      std::snprintf(out.line[n++], 128, "FPS  %.0f", fps);
    }
    std::snprintf(out.line[n++], 128, "frametime  %.1f ms", ft);
    std::snprintf(out.line[n++], 128, "FG %s x%d  scale %d%%", s.fg_enabled ? "ON" : "off", fgx, scale);
    std::snprintf(out.line[n++], 128, "RenoDX %s %s  %s  %s", s.enhance_renodx ? "ON" : "off",
                  renodx_visual_preset_name(s.renodx_preset), s.ui.fullscreen ? "fullscreen" : "windowed",
                  s.ui.sync_image ? "SYNC VSYNC" : "SYNC TEAR");
    if (s.ui.hw_info) {
      SystemMetricsSample blank;
      char hw[kSysMetricsLineCount][64]{};
      const int hn = format_sys_metrics_lines(hw, kSysMetricsLineCount, blank);
      for (int i = 0; i < hn && n < 16; ++i) {
        std::snprintf(out.line[n++], 128, "%s", hw[i]);
      }
    }
  }
  out.count = n;
  return out;
}

// Prevents Esc-from-lobby / held Esc from instantly killing the capture session.
// Regression: Iniciar right after Esc→menu left KEYDOWN auto-repeat in the queue.
struct SessionQuitGate {
  bool armed = true;            // false while quit key still held from prior action
  double earliest_quit_ms = 0;  // ignore quit until this time (debounce)

  // quit_key_down: GetAsyncKeyState(quit_vk) & 0x8000 at session boot.
  void reset(double now_ms, bool quit_key_down, double debounce_ms = 750.0) {
    armed = !quit_key_down;
    earliest_quit_ms = now_ms + debounce_ms;
  }

  void on_keyup(int vk, int quit_vk) {
    if (vk == quit_vk) armed = true;
  }

  bool should_accept_quit_keydown(int vk, int quit_vk, double now_ms) const {
    if (vk != quit_vk) return false;
    if (!armed) return false;
    if (now_ms < earliest_quit_ms) return false;
    return true;
  }

  static bool is_stale_quit_message(unsigned msg, int vk, int quit_vk) {
    if (vk != quit_vk) return false;
    return msg == 0x0100 /*WM_KEYDOWN*/ || msg == 0x0101 /*WM_KEYUP*/ ||
           msg == 0x0104 /*WM_SYSKEYDOWN*/ || msg == 0x0105 /*WM_SYSKEYUP*/;
  }
};

// Lobby must NOT treat WM_QUIT as "cancel app". Esc→session→lobby often leaves a
// spurious WM_QUIT in the queue; honoring it closes NexusCap instead of showing the menu.
// Real lobby exit is only WM_CLOSE (window X) / explicit cancel.
inline bool lobby_message_exits_app(unsigned msg) {
  return msg == 0x0010;  // WM_CLOSE
}

inline bool lobby_message_is_spurious_quit(unsigned msg) {
  return msg == 0x0012;  // WM_QUIT
}

// Esc in capture: with lobby → back to menu code; without → process exit.
inline constexpr int kSessionExitBackToLobby = 100;
inline int session_exit_code_after_esc(bool session_from_lobby) {
  return session_from_lobby ? kSessionExitBackToLobby : 0;
}

// Esc in lobby chrome: never quits. Gear/hotkey capture may consume Esc locally.
enum class LobbyEscAction { None, CloseGear, CancelHotkeyCapture };
inline LobbyEscAction lobby_esc_action(bool gear_open, bool capturing_hotkey) {
  if (capturing_hotkey) return LobbyEscAction::CancelHotkeyCapture;
  if (gear_open) return LobbyEscAction::CloseGear;
  return LobbyEscAction::None;
}

// Soft on-screen tips while playing (FG / Neural Rendering hotkeys).
inline constexpr double kSessionNoticeTtlMs = 3200.0;

inline std::string session_notice_fg_on() {
  return "Frame Gen ON — latencia / artefatos podem subir";
}
inline std::string session_notice_fg_multiplier_up(int mult) {
  mult = clamp_fg_multiplier(mult);
  return "Frame Gen x" + std::to_string(mult) + " — mais FPS, mais carga GPU";
}
inline std::string session_notice_nr_on() {
  return "Neural Rendering ON — imagem mais bonita; mais GPU";
}
inline std::string session_notice_nr_off() {
  return "Neural Rendering OFF — imagem limpa; menos GPU";
}

// Split "titulo — detalhe" so HUD/toast never clip the second half.
inline void session_notice_split(const std::string& msg, std::string& title, std::string& detail) {
  title.clear();
  detail.clear();
  if (msg.empty()) return;
  const auto pos = msg.find(" — ");
  if (pos == std::string::npos) {
    title = msg;
    return;
  }
  title = msg.substr(0, pos);
  detail = msg.substr(pos + 3);
}

// Warn when enabling FG; when cycling, only if multiplier went up (2→3→4).
inline bool session_notice_for_fg_toggle(bool now_active) { return now_active; }
inline bool session_notice_for_fg_cycle(int prev_mult, int next_mult) {
  return clamp_fg_multiplier(next_mult) > clamp_fg_multiplier(prev_mult);
}

struct SessionNotice {
  std::string text;
  double until_ms = 0;
  void push(std::string msg, double now_ms, double ttl_ms = kSessionNoticeTtlMs) {
    if (msg.empty()) return;
    text = std::move(msg);
    until_ms = now_ms + ttl_ms;
  }
  bool active(double now_ms) const { return !text.empty() && now_ms < until_ms; }
  std::string visible(double now_ms) const { return active(now_ms) ? text : std::string{}; }
};

}  // namespace nexuscap
