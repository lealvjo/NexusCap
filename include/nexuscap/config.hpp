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
  bool mini_overlay = false;    // Steam Deck-style top bar
  bool hw_info = false;  // painel Infos GPU/CPU
  bool hide_panel_on_capture = true;
  bool sync_image = true;  // vsync Present — default after 2.0.1 A/B (TEAR felt shakier)
  int scale_percent = 100;  // 25–100; 100 = native
  int renodx_preset = 0;    // Default / Natural / Cinematic
  bool neural_rendering = true;  // NeuralUplift/feed; Delete toggles; persists
  // Live RenoDX NR (Bridge). Carried lobby↔session — NOT the same as neural_rendering on disk.
  // Resetting this to false while RenoDX is still ON causes a boot pulse that inverts HUD vs image.
  bool nr_runtime_on = false;
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
  // When true, session auto-toggles FG on open. False = armed for PageDn only (default).
  bool fg_auto_start = false;
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
// Mini overlay: thin Steam Deck-style top bar (height only; width follows owner).
inline int hud_panel_width(bool advanced_metrics) { return advanced_metrics ? 920 : 560; }
inline int hud_panel_height(bool advanced_metrics, bool hw_info = false) {
  int h = advanced_metrics ? 640 : 220;
  // 5 lines: CPU / GPU / RAM / VRAM / PWR
  if (hw_info) h += 5 * 28;
  return h;
}
inline int hud_mini_overlay_height() { return 34; }  // one dense metric row + frametime sparkline

// When mini overlay is on, multi-line compact/advanced HUD is suppressed.
inline bool hud_uses_mini_overlay(bool show_fps, bool mini_overlay) { return show_fps && mini_overlay; }
inline bool hud_uses_advanced_metrics(bool show_fps, bool mini_overlay, bool advanced_metrics) {
  return show_fps && !mini_overlay && advanced_metrics;
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
inline int lobby_hud_preview_lines(bool show_fps, bool advanced_metrics, bool hw_info = false,
                                   bool mini_overlay = false) {
  if (!show_fps) return 0;
  if (mini_overlay) return 1;  // one dense full-width bar
  int n = advanced_metrics ? 8 : 6;
  if (hw_info) n += kSysMetricsLineCount;
  return n;
}

// Pure HUD text for live preview — gear/Sessão toggles must change these strings.
struct LobbyHudPreviewLines {
  int count = 0;
  char line[16][384]{};
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

  if (s.ui.mini_overlay) {
    const bool dlss5 = s.enhance_renodx && s.neural_rendering;
    SystemMetricsSample hw{};
    // Mini always reserves HW strip in preview (live session fills real SKU/%).
    hw.cpu_name = "i5-11400F";
    hw.gpu_name = "RTX 5070";
    hw.cpu_ok = hw.gpu_ok = hw.cpu_temp_ok = hw.gpu_temp_ok = true;
    hw.vram_ok = hw.ram_ok = hw.power_ok = true;
    hw.cpu_percent = 35;
    hw.cpu_temp_c = 55;
    hw.gpu_percent = 40;
    hw.gpu_temp_c = 60;
    hw.vram_used_mb = 2048;
    hw.vram_budget_mb = 12288;
    hw.ram_used_mb = 16384;
    hw.ram_total_mb = 32768;
    hw.power_w = 90;
    char hw_compact[200]{};
    format_sys_metrics_compact(hw_compact, sizeof(hw_compact), hw);
    format_mini_overlay_lines(
        out.line[0], 384, out.line[1], 384, fps, fps * static_cast<double>((std::max)(fgx, 1)), false, fgx,
        dlss5, s.ui.sync_image, ft, ft, ft, ft, true, ft, /*present=*/ft * 0.15f, 0, 0, 0, s.width, s.height,
        scaled_extent(s.width, scale), scaled_extent(s.height, scale), s.width, s.height, scale, false, 0, 0,
        0, 0, renodx_visual_preset_name(s.renodx_preset), hw_compact, /*dlss_ms=*/0, /*frames=*/0,
        /*s600_max=*/0, /*fg_backend=*/nullptr, s.fps);
    out.count = 1;
    return out;
  }

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
      format_lobby_hud_fps_line(out.line[n++], 128, fps, true, fgx);
    } else {
      format_lobby_hud_fps_line(out.line[n++], 128, fps, false, fgx);
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
      format_lobby_hud_fps_line(out.line[n++], 128, fps, true, fgx);
    } else {
      format_lobby_hud_fps_line(out.line[n++], 128, fps, false, fgx);
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
inline std::string session_notice_fg_missing() {
  return "Frame Gen — Lossless Scaling nao encontrado (CFG → Pasta)";
}
inline std::string session_notice_nr_on() {
  return "Neural Rendering ON — Confirmado; imagem mais bonita; mais GPU";
}
inline std::string session_notice_nr_off() {
  return "Neural Rendering OFF — Confirmado; imagem limpa; menos GPU";
}
inline std::string session_notice_nr_applying() {
  return "Neural Rendering — Aplicando…";
}
inline std::string session_notice_fg_applying() {
  return "Frame Gen — Aplicando…";
}
inline std::string session_notice_fg_confirmed_on(int mult) {
  return std::string("Frame Gen ON — Confirmado x") + std::to_string(clamp_fg_multiplier(mult));
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

// Esc → lobby: CFG checkbox must mirror PageDn / PageUp actions from the session.
inline void apply_session_fg_to_persisted(bool fg_active, int fg_mult, PersistedSettings& s) {
  s.fg_enabled = fg_active;
  if (fg_mult >= 2) s.fg_multiplier = clamp_fg_multiplier(fg_mult);
}

inline void apply_session_fg_to_config(bool fg_active, int fg_mult, AppConfig& cfg) {
  cfg.fg_auto_start = fg_active;
  if (fg_mult >= 2) cfg.fg_multiplier = clamp_fg_multiplier(fg_mult);
}

// Esc → lobby: mirror Delete into neural_rendering. Always keep enhance_renodx if the
// session had RenoDX loaded — otherwise next Iniciar has no stack and Delete is dead
// (log: "nr toggle blocked: RenoDX not loaded this session").
inline void apply_session_nr_to_persisted(bool nr_on, bool session_had_renodx, PersistedSettings& s) {
  s.neural_rendering = nr_on;
  if (session_had_renodx) {
    s.enhance_renodx = true;
  } else if (nr_on) {
    s.enhance_renodx = true;
  }
  // else: leave enhance_renodx as the user left it in the lobby (do not force clear).
}

// Mid-session Delete: only the NR flag. Never clear EnhanceBackend::Renodx.
inline void apply_session_nr_runtime_to_config(bool nr_on, AppConfig& cfg) {
  cfg.neural_rendering = nr_on;
}

inline bool session_nr_hotkey_allowed(EnhanceBackend enhance) {
  return enhance == EnhanceBackend::Renodx;
}

// Lobby checkbox = RenoDX stack (not NR runtime). NR on/off is Delete / status line.
inline bool lobby_renodx_checkbox_checked(bool enhance_renodx) { return enhance_renodx; }

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
