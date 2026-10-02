#pragma once
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include "nexuscap/dlss5_bridge.hpp"
#include "nexuscap/dlss_options.hpp"
#include "nexuscap/lossless_fg.hpp"
#include "nexuscap/settings_store.hpp"
#include "nexuscap/live_mode.hpp"
#include "nexuscap/capture_format.hpp"
#include "nexuscap/system_metrics.hpp"
#include "nexuscap/version.hpp"
#include "nexuscap/audio.hpp"

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
  bool autostart = false;       // --autostart: seed from settings, skip lobby once (feed scale relaunch)
  bool portable = false;        // --portable: settings/presets/logs beside the exe (zip / USB)
  bool open_lobby_cfg = false;  // --lobby-cfg: reopen lobby CFG after a motor/Pass Count restart
  bool open_session_cfg = false;  // --session-cfg: CFG estava aberto; volta aberto na sessão
  bool open_nr_advanced = false;  // --nr-adv-open: seção avançada estava aberta no aplicar
  bool simulate_hdmi_busy = false;  // preview + Iniciar: placa em uso (sem abrir DShow)
  std::string lobby_boot_toast;     // one-shot toast ao voltar ao lobby (captura falhou)
  int overlay_hud_restore = -1; // --hud=: session overlay after OUR restart (not lobby checks)
  bool advanced_metrics = false;
  bool mini_overlay = false;    // Steam Deck-style top bar
  bool overlay_no_bg = false;   // todos os overlays: sem placa preta
  bool hw_info = false;  // painel Infos GPU/CPU
  bool hide_panel_on_capture = true;
  bool sync_image = true;  // vsync Present — default after 2.0.1 A/B (TEAR felt shakier)
  bool bug_report = false;
  bool perf_report = false;
  bool live_overlay = false;  // right-side live overlay (DLSS5 + FPS strip)
  bool overlay_console = false;  // optional console name on overlays
  int live_console = 0;       // LiveConsole: which console is being captured
  int scale_percent = 100;  // 25–100; 100 = native
  int renodx_preset = 0;    // 1x Default/Natural/Cinematic; 1.5x/2x/3x Model A/B/C
  int nr_pass_count = 1;    // 1=1x, 4=1.5x, 5=2x+, 6=3x+
  NrAdvancedLook nr_advanced[7]{};
  bool neural_rendering = true;  // NeuralUplift/feed; Delete toggles; persists
  // Live RenoDX NR (Bridge). Carried lobby↔session — NOT the same as neural_rendering on disk.
  // Resetting this to false while RenoDX is still ON causes a boot pulse that inverts HUD vs image.
  bool nr_runtime_on = false;
  int video_fourcc = 0;     // NV12 default
  int color_space = 0;      // Padrao → BT.709
  int color_range = 0;      // Limitado
  int capture_buffering = 0;  // Desativado
  int capture_volume = 100;  // HDMI WASAPI 0–100; settings.ini, not presets
  int capture_tone = 0;      // 0 padrao, 1 gamer, 2 graves; settings.ini, not presets
  int capture_source = 0;    // 0 HDMI, 1 Janela
  std::string window_title;
  uint64_t window_hwnd = 0;

  // Enhance: renodx (ReShade proxy + presets) | streamline (native) | off
  EnhanceBackend enhance = EnhanceBackend::Off;
  bool dlss = false;  // legacy alias: enables Streamline when enhance==Off
  DlssMode dlss_mode = DlssMode::Dlaa;
  float dlss_sharpness = 0.f;
  DlssMvecSource dlss_mvec = DlssMvecSource::OpticalFlow;

  // Lossless Scaling FG (Steam install, never bundled): 1=off, 2/3/4=multiplier ready
  int fg_multiplier = 1;
  int fg_queue = 0;  // Lossless QueueTarget: 0 menor latência (validado)
  int fg_flow_scale = 80;  // Padrao = 80; 70/90/100 a escolha da pessoa
  bool fg_adaptive = false;  // default FIXED (mira-lock validado)
  int fg_target_fps = 60;    // Adaptive LSFG3Target; teto = Hz do monitor
  // When true, session auto-toggles FG on open. False = armed for PageDn only (default).
  bool fg_auto_start = false;
  std::string fg_path;  // optional override of LosslessScaling.exe
  bool selftest_fg = false;
  bool selftest_cfg = false;  // --selftest-cfg: desenha o CFG da sessão e valida o draw data
  bool perf_test = false;     // --perf-test: FileCapture em janela, sem gravar no CFG HDMI
  bool popup_perf_result = false;  // so no regresso deste benchmark; nao reabrir no HDMI/preview
  bool perf_cli_override = false;  // --width/--pass/--scale/--preset no CLI; nao sobrescrever com o plano
  bool perf_test_audio = true;     // som do clipe no Benchmark; so plano/CLI, nao CFG HDMI
  std::string video_path;     // --video=: mp4/mkv do teste de performance

  HotkeyConfig hotkeys{};
};

AppConfig parse_args(int argc, char** argv);

// Lobby Sessão/CFG → AppConfig (tested).
void apply_persisted_to_config(const PersistedSettings& s, AppConfig& cfg);

// HUD starts visible when "Exibir metricas" is on. The old lobby strip is gone.
inline bool initial_hud_visible(const SessionUiFlags& ui) { return ui.show_fps; }
inline bool initial_hud_visible(bool show_fps, bool /*hide_panel_on_capture*/) { return show_fps; }

// Captura sempre limpa: a faixa SessionControlPanel saiu (nao tinha funcao real).
inline bool session_shows_control_panel(bool /*hide_panel_on_capture*/ = false) { return false; }
inline bool session_shows_control_panel(const SessionUiFlags& /*ui*/) { return false; }
inline bool session_capture_is_clean() { return true; }

// sync_image → DXGI Present sync interval (1 = vsync, 0 = uncapped / tearing OK).
inline unsigned present_sync_interval(bool sync_image) { return sync_image ? 1u : 0u; }

// Windowed vsync follows the monitor Hz. On 120 Hz, Present(1) = 120 fps and the
// 30 fps clip looks slow (NR runs 4×). Interval = Hz / meta, clamped 1..4.
inline unsigned present_interval_for_target(bool sync_image, double target_fps, int monitor_hz) {
  if (!sync_image) return 0u;
  const double fps = target_fps >= 45.0 ? 60.0 : 30.0;
  int hz = monitor_hz;
  if (hz < 30) hz = 60;
  if (hz > 360) hz = 360;
  int iv = static_cast<int>((static_cast<double>(hz) + fps * 0.5) / fps);
  if (iv < 1) iv = 1;
  if (iv > 4) iv = 4;
  return static_cast<unsigned>(iv);
}

// Compact vs advanced metrics panel size (MetricsHud).
// Advanced needs room for long signal/hotkey lines; HW adds 5 stacked rows.
// Mini overlay: thin Steam Deck-style top bar (height only; width follows owner).
inline int hud_panel_width(bool advanced_metrics) { return advanced_metrics ? 920 : 880; }
inline int hud_panel_height(bool advanced_metrics, bool hw_info = false) {
  int h = advanced_metrics ? 640 : 348;  // compact: FT partilhado + latencia + grafico
  // 5 lines: CPU / GPU / RAM / VRAM / PWR
  if (hw_info) h += 5 * 28;
  return h;
}
inline int hud_mini_overlay_height() { return 168; }  // 3 text rows + grafico FT com linhas FG
inline int hud_mini_overlay_font_px() { return 27; }  // +50% vs 18
inline int hud_mini_overlay_rows() { return 3; }

// O mini ocupa a largura da tela. O grafico de FT fica no canto e usa a sobra
// ao lado do texto, em vez de um bloco fixo de 220 px.
inline int hud_mini_frametime_graph_w(int bar_w) {
  if (bar_w < 64) return 220;
  int w = bar_w * 2 / 5;
  if (w < 280) w = 280;
  if (w > 720) w = 720;
  const int room = bar_w - 520;
  if (room >= 220 && w > room) w = room;
  if (w > bar_w - 32) w = bar_w - 32;
  if (w < 160) w = 160;
  return w;
}

// When mini overlay is on, multi-line compact/advanced HUD is suppressed.
inline bool hud_uses_mini_overlay(bool show_fps, bool mini_overlay) { return show_fps && mini_overlay; }
inline bool overlay_draws_plate(bool overlay_no_bg) { return !overlay_no_bg; }
// Overlay sem fundo vale em compacto, mini, advanced e live — o check nunca fica preso ao mini.
inline bool overlay_no_bg_locked_to_mini() { return false; }
inline bool overlay_no_bg_ui_enabled(bool /*mini_overlay*/ = false, bool /*live_overlay*/ = false,
                                    bool /*advanced_metrics*/ = false) {
  return !overlay_no_bg_locked_to_mini();
}
inline bool hud_uses_advanced_metrics(bool show_fps, bool mini_overlay, bool advanced_metrics) {
  return show_fps && !mini_overlay && advanced_metrics;
}

// Ctrl+Espaço na sessão: Compact → Mini → Advanced → Live → Off → Compact.
// Só a sessão muda; os checks do lobby ficam como estavam.
enum class OverlayHudStep : int { Compact = 0, Mini = 1, Advanced = 2, Live = 3, Off = 4 };
inline constexpr int kOverlayHudStepCount = 5;

struct OverlayHudFlags {
  bool visible = true;
  bool mini = false;
  bool advanced = false;
  bool live = false;
};

inline OverlayHudFlags overlay_hud_flags_for(OverlayHudStep step) {
  OverlayHudFlags f{};
  switch (step) {
    case OverlayHudStep::Mini:
      f.visible = true;
      f.mini = true;
      return f;
    case OverlayHudStep::Advanced:
      f.visible = true;
      f.advanced = true;
      return f;
    case OverlayHudStep::Live:
      f.visible = true;
      f.live = true;
      return f;
    case OverlayHudStep::Off:
      f.visible = false;
      f.mini = false;
      f.advanced = false;
      f.live = false;
      return f;
    case OverlayHudStep::Compact:
    default:
      f.visible = true;
      return f;
  }
}

inline OverlayHudStep overlay_hud_step_of(bool visible, bool mini, bool advanced, bool live) {
  if (!visible) return OverlayHudStep::Off;
  if (live) return OverlayHudStep::Live;
  if (mini) return OverlayHudStep::Mini;
  if (advanced) return OverlayHudStep::Advanced;
  return OverlayHudStep::Compact;
}

inline OverlayHudStep next_overlay_hud_step(OverlayHudStep cur) {
  const int i = static_cast<int>(cur);
  const int n = (i < 0) ? 0 : i;
  return static_cast<OverlayHudStep>((n + 1) % kOverlayHudStepCount);
}

inline OverlayHudFlags cycle_overlay_hud(bool visible, bool mini, bool advanced, bool live) {
  return overlay_hud_flags_for(next_overlay_hud_step(overlay_hud_step_of(visible, mini, advanced, live)));
}

inline const char* overlay_hud_step_cli(OverlayHudStep s) {
  switch (s) {
    case OverlayHudStep::Mini:
      return "mini";
    case OverlayHudStep::Advanced:
      return "advanced";
    case OverlayHudStep::Live:
      return "live";
    case OverlayHudStep::Off:
      return "off";
    case OverlayHudStep::Compact:
    default:
      return "compact";
  }
}

inline bool parse_overlay_hud_step(const char* raw, OverlayHudStep& out) {
  if (!raw || !raw[0]) return false;
  if (std::strcmp(raw, "compact") == 0) {
    out = OverlayHudStep::Compact;
    return true;
  }
  if (std::strcmp(raw, "mini") == 0) {
    out = OverlayHudStep::Mini;
    return true;
  }
  if (std::strcmp(raw, "advanced") == 0 || std::strcmp(raw, "adv") == 0) {
    out = OverlayHudStep::Advanced;
    return true;
  }
  if (std::strcmp(raw, "live") == 0) {
    out = OverlayHudStep::Live;
    return true;
  }
  if (std::strcmp(raw, "off") == 0) {
    out = OverlayHudStep::Off;
    return true;
  }
  return false;
}

inline void apply_overlay_hud_flags(const OverlayHudFlags& f, AppConfig& cfg) {
  cfg.show_overlay = f.visible;
  cfg.mini_overlay = f.mini;
  cfg.advanced_metrics = f.advanced;
  cfg.live_overlay = f.live;
}

// --autostart --hud=Y: volta o overlay da sessão. Não grava os checks do lobby.
inline bool apply_autostart_overlay_hud(AppConfig& cfg) {
  if (cfg.overlay_hud_restore < 0 || cfg.overlay_hud_restore >= kOverlayHudStepCount) return false;
  apply_overlay_hud_flags(overlay_hud_flags_for(static_cast<OverlayHudStep>(cfg.overlay_hud_restore)), cfg);
  return true;
}

inline const char* session_cfg_reopen_arg() { return "--session-cfg"; }
inline const char* nr_advanced_reopen_arg() { return "--nr-adv-open"; }

inline std::string session_cfg_relaunch_args_for_hud(OverlayHudStep step, bool reopen_cfg = false,
                                                     bool reopen_advanced = false) {
  std::string a = std::string("--autostart --hud=") + overlay_hud_step_cli(step);
  if (reopen_cfg) {
    a += " ";
    a += session_cfg_reopen_arg();
  }
  if (reopen_advanced) {
    a += " ";
    a += nr_advanced_reopen_arg();
  }
  return a;
}

inline std::string session_cfg_relaunch_args_for_hud(bool visible, bool mini, bool advanced, bool live,
                                                     bool reopen_cfg = false, bool reopen_advanced = false) {
  return session_cfg_relaunch_args_for_hud(overlay_hud_step_of(visible, mini, advanced, live), reopen_cfg,
                                           reopen_advanced);
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
inline std::string watermark_png_candidate_assets(const std::string& exe_dir) {
  return logo_png_path_join(exe_dir, "assets\\watermark.png");
}
inline std::string watermark_png_candidate_root(const std::string& exe_dir) {
  return logo_png_path_join(exe_dir, "watermark.png");
}

// Win32 ICON resource id embedded in NexusCap.exe (resources/nexuscap.rc).
inline constexpr int kAppIconResourceId = 101;

// Lines on lobby HUD preview (0 = HUD hidden). Compact also shows gear fields.
inline int lobby_hud_preview_lines(bool show_fps, bool advanced_metrics, bool hw_info = false,
                                   bool mini_overlay = false) {
  if (!show_fps) return 0;
  if (mini_overlay) return hud_mini_overlay_rows();  // FPS+res / stalls+ft / L600+HW
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

inline LobbyHudPreviewLines make_lobby_hud_preview_lines(const PersistedSettings& s, double fps_ema,
                                                        const SystemMetricsSample* hw_live = nullptr) {
  LobbyHudPreviewLines out;
  if (!s.ui.show_fps) return out;

  const float fps = fps_ema > 1.0 ? static_cast<float>(fps_ema) : 0.f;
  const float ft = fps > 1.f ? (1000.f / fps) : 0.f;
  const int fgx = s.fg_enabled ? (s.fg_multiplier >= 2 ? s.fg_multiplier : 2) : 1;
  const int scale = clamp_scale_percent(s.scale_percent);
  int n = 0;

  if (s.ui.mini_overlay) {
    const bool dlss5 = s.enhance_renodx && s.neural_rendering;
    // Same sampler as the session HUD — never a fake SKU / % / temp.
    const SystemMetricsSample hw = hw_live ? *hw_live : SystemMetricsSample{};
    char hw_compact[200]{};
    format_sys_metrics_compact(hw_compact, sizeof(hw_compact), hw);
    const LiveConsole cons = parse_live_console(s.ui.live_console);
    unsigned char cr = 0, cg = 55, cb = 145;
    live_console_rgb(cons, cr, cg, cb);
    const char* cap_fmt = hud_capture_format_name(s.video_fourcc, s.capture_source);
    format_mini_overlay_lines(
        out.line[0], 384, out.line[1], 384, fps, fps * static_cast<double>((std::max)(fgx, 1)), false, fgx,
        dlss5, s.ui.sync_image, ft, ft, ft, ft, true, ft, /*present=*/ft * 0.15f, 0, 0, 0, s.width, s.height,
        scaled_extent(s.width, scale), scaled_extent(s.height, scale), s.width, s.height, scale, false, 0, 0,
        0, 0, renodx_visual_preset_name(s.renodx_preset, s.nr_pass_count), hw_compact, /*dlss_ms=*/0, /*frames=*/0,
        /*s600_max=*/0, /*fg_backend=*/nullptr, s.fps, s.nr_pass_count,
        overlay_console_label_or_null(s.ui.overlay_console, cons), false, cr, cg, cb, s.fg_queue,
        s.fg_adaptive, s.fg_target_fps, -1, cap_fmt);
    out.count = out.line[1][0] ? 2 : 1;
    return out;
  }

  if (s.ui.advanced_metrics) {
    if (s.ui.overlay_console) {
      char chip[48]{};
      format_overlay_console_chip(chip, sizeof(chip), parse_live_console(s.ui.live_console));
      std::snprintf(out.line[n++], 128, "NexusCap  [LIVE]  ADV  %s", chip);
    } else {
      std::snprintf(out.line[n++], 128, "NexusCap  [LIVE]  ADV");
    }
    std::snprintf(out.line[n++], 128, "%s", s.device_name.empty() ? "auto" : s.device_name.c_str());
    const int ww = scaled_extent(s.width, scale);
    const int wh = scaled_extent(s.height, scale);
    const char* cap_fmt = hud_capture_format_name(s.video_fourcc, s.capture_source);
    if (scale < 100) {
      std::snprintf(out.line[n++], 128, "signal %dx%d → work %dx%d (%d%%)  %s", s.width, s.height, ww, wh,
                    scale, cap_fmt);
    } else {
      std::snprintf(out.line[n++], 128, "signal  %dx%d @ %.0f  work 100%%  %s", s.width, s.height, s.fps,
                    cap_fmt);
    }
    if (s.fg_enabled) {
      format_lobby_hud_fps_line(out.line[n++], 128, fps, true, fgx, s.fg_adaptive, s.fg_target_fps,
                                s.fg_flow_scale);
    } else {
      format_lobby_hud_fps_line(out.line[n++], 128, fps, false, fgx, s.fg_adaptive, s.fg_target_fps,
                                s.fg_flow_scale);
    }
    format_overlay_frametime_line(out.line[n++], 128, ft, ft, ft, true, false, fgx, fps, s.fps, 0);
    {
      char tag[40]{};
      format_fg_mode_tag(tag, sizeof(tag), s.fg_adaptive, fgx, s.fg_target_fps, s.fg_flow_scale);
      std::snprintf(out.line[n++], 128, "FG  %s  %s  fila%d   scale %d%%", s.fg_enabled ? "ON" : "off", tag,
                    clamp_fg_queue(s.fg_queue), scale);
    }
    {
      char stack[96]{};
      format_dlss5_stack_hud_line(stack, sizeof(stack), s.enhance_renodx && s.neural_rendering, s.enhance_renodx,
                                  s.nr_pass_count);
      if (!s.enhance_renodx) {
        std::snprintf(out.line[n++], 128, "RenoDX off  %s  %s",
                      renodx_visual_preset_name(s.renodx_preset, s.nr_pass_count),
                      s.ui.fullscreen ? "fullscreen" : "windowed");
      } else {
        std::snprintf(out.line[n++], 128, "%s  %s  %s", stack,
                      renodx_visual_preset_name(s.renodx_preset, s.nr_pass_count),
                      s.ui.fullscreen ? "fullscreen" : "windowed");
      }
    }
    std::snprintf(out.line[n++], 128, "%s   RGB work ~%d%%", s.ui.sync_image ? "SYNC VSYNC" : "SYNC TEAR",
                  static_cast<int>(scale_pixel_ratio(scale) * 100.0 + 0.5));
    if (s.ui.hw_info) {
      const SystemMetricsSample hw = hw_live ? *hw_live : SystemMetricsSample{};
      char hw_lines[kSysMetricsLineCount][64]{};
      const int hn = format_sys_metrics_lines(hw_lines, kSysMetricsLineCount, hw);
      for (int i = 0; i < hn && n < 16; ++i) {
        std::snprintf(out.line[n++], 128, "%s", hw_lines[i]);
      }
    }
  } else {
    if (s.ui.overlay_console) {
      char chip[48]{};
      format_overlay_console_chip(chip, sizeof(chip), parse_live_console(s.ui.live_console));
      std::snprintf(out.line[n++], 128, "NexusCap  [LIVE]  %s", chip);
    } else {
      std::snprintf(out.line[n++], 128, "NexusCap  [LIVE]");
    }
    std::snprintf(out.line[n++], 128, "%dx%d @ %.0f  %s", s.width, s.height, s.fps,
                  hud_capture_format_name(s.video_fourcc, s.capture_source));
    if (s.fg_enabled) {
      format_lobby_hud_fps_line(out.line[n++], 128, fps, true, fgx, s.fg_adaptive, s.fg_target_fps,
                                s.fg_flow_scale);
    } else {
      format_lobby_hud_fps_line(out.line[n++], 128, fps, false, fgx, s.fg_adaptive, s.fg_target_fps,
                                s.fg_flow_scale);
    }
    format_overlay_frametime_line(out.line[n++], 128, ft, ft, ft, true, false, fgx, fps, s.fps, 0);
    {
      char tag[40]{};
      format_fg_mode_tag(tag, sizeof(tag), s.fg_adaptive, fgx, s.fg_target_fps, s.fg_flow_scale);
      std::snprintf(out.line[n++], 128, "FG %s %s  fila%d  scale %d%%", s.fg_enabled ? "ON" : "off", tag,
                    clamp_fg_queue(s.fg_queue), scale);
    }
    {
      char stack[96]{};
      format_dlss5_stack_hud_line(stack, sizeof(stack), s.enhance_renodx && s.neural_rendering, s.enhance_renodx,
                                  s.nr_pass_count);
      if (!s.enhance_renodx) {
        std::snprintf(out.line[n++], 128, "RenoDX off %s  %s  %s",
                      renodx_visual_preset_name(s.renodx_preset, s.nr_pass_count),
                      s.ui.fullscreen ? "fullscreen" : "windowed",
                      s.ui.sync_image ? "SYNC VSYNC" : "SYNC TEAR");
      } else {
        std::snprintf(out.line[n++], 128, "%s  %s  %s  %s", stack,
                      renodx_visual_preset_name(s.renodx_preset, s.nr_pass_count),
                      s.ui.fullscreen ? "fullscreen" : "windowed",
                      s.ui.sync_image ? "SYNC VSYNC" : "SYNC TEAR");
      }
    }
    if (s.ui.hw_info) {
      const SystemMetricsSample hw = hw_live ? *hw_live : SystemMetricsSample{};
      char hw_lines[kSysMetricsLineCount][64]{};
      const int hn = format_sys_metrics_lines(hw_lines, kSysMetricsLineCount, hw);
      for (int i = 0; i < hn && n < 16; ++i) {
        std::snprintf(out.line[n++], 128, "%s", hw_lines[i]);
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

// Esc e o X da janela: mesmo destino (teardown + lobby). Nao destroi a HWND no clique.
inline int session_exit_code_after_window_close(bool session_from_lobby) {
  return session_exit_code_after_esc(session_from_lobby);
}
inline bool session_user_leave_returns_to_lobby(bool session_from_lobby) { return session_from_lobby; }

// CFG na captura (Ctrl+M) pediu reinício: sai da sessão, salva e volta com --autostart.
inline constexpr int kSessionExitRelaunch = 101;

// Esc com o CFG aberto fecha só o menu; sem menu, volta ao lobby.
enum class SessionEscAction { CloseCfg, BackToLobby };
inline SessionEscAction session_esc_action(bool cfg_open) {
  return cfg_open ? SessionEscAction::CloseCfg : SessionEscAction::BackToLobby;
}

// Com o menu aberto o ImGui tem o teclado — A / PgDn / Delete / F11 não podem disparar.
inline bool session_hotkeys_blocked_while_cfg_open(bool cfg_open) { return cfg_open; }

// CFG 6.0: mesmas abas no lobby (F12) e na sessão (Ctrl+M). Imagem primeiro.
inline const char* cfg_tab_image() { return "Imagem"; }
inline const char* cfg_tab_hud() { return "HUD"; }
inline const char* cfg_tab_sound() { return "Som"; }
inline const char* cfg_tab_fg() { return "Frame Gen"; }
inline const char* cfg_tab_presets() { return "Presets"; }
inline const char* cfg_tab_keys() { return "Atalhos"; }

// O que a mudança feita no CFG da sessão custa.
// 6.0: o 8.5 só lê o X no init — o Present da janela visível é o motor.
// Troca de X relança o NexusCap (um GPU). O aviso GDI segura a tela; sem segunda janela.
enum class SessionCfgApply {
  Live,
  RecycleWorker,
  RelaunchMotor,
  RelaunchPassCount,
  RelaunchFeedScale,
  RelaunchPreset,
  RelaunchNr,
};

// Preset e NR no 1x tambem precisam do filho novo: o pulso OFF→ON invertia a imagem vs a UI.
inline bool session_cfg_preset_applies_live(bool /*multipass_stack*/) { return false; }

inline SessionCfgApply plan_session_cfg_preset_apply(bool /*multipass_stack*/) {
  return SessionCfgApply::RecycleWorker;
}

inline SessionCfgApply plan_session_cfg_apply(bool renodx_stack, int pass_before, int pass_after,
                                              int scale_loaded, int scale_after) {
  if (nr_pass_count_change_needs_relaunch(pass_before, pass_after)) return SessionCfgApply::RecycleWorker;
  // Mesmo motor, mas o multipass não relê o PassCount sem processo novo do filho.
  if (!nr_pass_count_change_applies_live(pass_before, pass_after)) {
    return SessionCfgApply::RecycleWorker;
  }
  if (feed_scale_change_needs_relaunch(renodx_stack, scale_loaded, scale_after, pass_after)) {
    return SessionCfgApply::RecycleWorker;
  }
  return SessionCfgApply::Live;
}

inline bool session_cfg_apply_needs_relaunch(SessionCfgApply a) {
  return a != SessionCfgApply::Live;
}

// Reinício da sessão sempre volta para a captura, nunca para o lobby.
inline const char* session_cfg_relaunch_args() { return "--autostart"; }

// Pass Count / motor no CFG do lobby: reabre o lobby com o CFG ainda aberto.
inline const char* lobby_cfg_relaunch_args() { return "--lobby-cfg"; }

// Aplicar avançado no lobby: volta com o CFG e a seção ainda abertos.
inline std::string lobby_cfg_relaunch_args_with_advanced() {
  return std::string(lobby_cfg_relaunch_args()) + " " + nr_advanced_reopen_arg();
}

// Hold do lobby: so Iniciar / Benchmark matam o processo. Troca de X no lobby so grava.
inline bool lobby_hold_relaunch_starts_session(const char* args) {
  if (!args || !args[0]) return false;
  const std::string a(args);
  return a.find("--autostart") != std::string::npos || a.find("--perf-test") != std::string::npos;
}

// One-shot: consume the boot flag so Esc→lobby later does not keep reopening CFG.
inline bool lobby_consume_open_cfg(bool& open_lobby_cfg) {
  const bool v = open_lobby_cfg;
  open_lobby_cfg = false;
  return v;
}

// Delete / Ctrl+M ligam e desligam o NR em qualquer Pass Count. Sem pulso: grava e reinicia.
inline bool session_cfg_nr_toggle_allowed(bool renodx_stack, bool /*multipass_stack*/) {
  return renodx_stack;
}

inline SessionCfgApply plan_session_cfg_nr_apply(bool renodx_stack, bool multipass_stack) {
  if (!session_cfg_nr_toggle_allowed(renodx_stack, multipass_stack)) return SessionCfgApply::Live;
  return SessionCfgApply::RecycleWorker;
}

// Aviso na troca de X: a imagem fica; o motor novo le o INI.
inline std::string session_restart_notice_title() { return "A aplicar o motor"; }
inline std::string lobby_pipeline_hold_title() { return session_restart_notice_title(); }
inline std::string lobby_iniciar_hold_title() { return "Otimizando pipeline"; }

// Janela: o Iniciar solta o device do lobby antes do filho criar o dele. HDMI deixa o device vivo.
inline bool lobby_relaunch_releases_device_before_spawn(bool window_source) { return window_source; }

// Cartão "Otimizando pipeline" no Iniciar. A janela entra direto; o HDMI do lobby ainda mostra.
inline bool lobby_iniciar_shows_pipeline_card(bool window_source) { return !window_source; }

// Capa do boot da janela. Primeiro Iniciar não pinta cartão. Troca de X/preset/NR segura o frame salvo.
enum class WindowBootCover { None, HoldFrame };
inline WindowBootCover window_boot_cover(bool hold_boot) {
  return hold_boot ? WindowBootCover::HoldFrame : WindowBootCover::None;
}
// O primeiro frame ao vivo nasce com FPS 0 e o menu do Ctrl+M ainda sem as abas.
// A capa fica esses frames; soltar antes disso e o corte aparece na imagem.
inline constexpr int kMotorHoldSettleFrames = 8;
inline bool motor_hold_ready_to_show(bool hold_boot, int live_frames) {
  return !hold_boot || live_frames >= kMotorHoldSettleFrames;
}

// Clique do menu no meio do Present. Com frame já na tela, a foto é esse frame
// e este Present não sai: o backbuffer do clique ainda não passou pelo ReShade.
// Sem frame anterior, um Present do backbuffer atual. Frame normal apresenta
// e só depois copia o buffer que entrou no Present.
enum class HoldFrameChoice { SnapshotLastSkip, SnapshotCurrentPresent, PresentThenCopy };
inline HoldFrameChoice hold_frame_choice(bool want_hold, bool has_last_presented) {
  if (!want_hold) return HoldFrameChoice::PresentThenCopy;
  if (has_last_presented) return HoldFrameChoice::SnapshotLastSkip;
  return HoldFrameChoice::SnapshotCurrentPresent;
}
inline std::string session_restart_notice_detail(SessionCfgApply a) {
  switch (a) {
    case SessionCfgApply::RecycleWorker:
      return "A tela fica.";
    case SessionCfgApply::RelaunchMotor:
      return "Motor: a tela fica.";
    case SessionCfgApply::RelaunchPassCount:
      return "Pass Count: a tela fica.";
    case SessionCfgApply::RelaunchFeedScale:
      return "Escala do Feed: a tela fica.";
    case SessionCfgApply::RelaunchPreset:
      return "Preset visual: a tela fica.";
    case SessionCfgApply::RelaunchNr:
      return "Neural Rendering: a tela fica.";
    default:
      return "A tela fica.";
  }
}
inline bool restart_notice_visible_at_boot(bool autostart, bool have_hold_image = false) {
  return autostart && have_hold_image;
}

// Esc in lobby chrome: never quits. Gear/hotkey capture may consume Esc locally.
enum class LobbyEscAction { None, CloseGear, CancelHotkeyCapture };
inline LobbyEscAction lobby_esc_action(bool gear_open, bool capturing_hotkey) {
  if (capturing_hotkey) return LobbyEscAction::CancelHotkeyCapture;
  if (gear_open) return LobbyEscAction::CloseGear;
  return LobbyEscAction::None;
}

// F12 (ou o rebind vk_gear) abre/fecha o CFG do lobby. Esc e captura de tecla ganham.
inline bool lobby_gear_hotkey_toggles_cfg(int vk, int gear_vk, bool capturing_hotkey, bool ctrl_down = false,
                                         bool gear_ctrl = false) {
  if (capturing_hotkey || gear_vk <= 0) return false;
  if (vk != gear_vk) return false;
  return !gear_ctrl || ctrl_down;
}

inline const char* lobby_cfg_unsaved_title() { return "Alteracoes foram feitas e nao foram salvas."; }
inline const char* lobby_cfg_unsaved_question() { return "Deseja aplica-las?"; }

// Soft on-screen tips while playing (FG / Neural Rendering hotkeys).
inline constexpr double kSessionNoticeTtlMs = 3200.0;
// Um Present do aviso e aplica. O 0,55s do timer antigo só atrasava o X.
inline constexpr double kDlss5MotorSwapHoldSec = 0.0;

inline std::string session_hdmi_notice_title() { return "O console parou de mandar o sinal HDMI"; }
inline std::string session_hdmi_notice_detail() {
  return "Nao e um erro do NexusCap. Limpamos os pixels para quando o sinal voltar ficar limpo, sem bugs.";
}
inline std::string session_notice_hdmi_recover() {
  return session_hdmi_notice_title() + " — " + session_hdmi_notice_detail();
}
inline std::string session_notice_fg_on() {
  return "Frame Gen ON";
}
inline std::string session_notice_fg_multiplier_up(int mult) {
  mult = clamp_fg_multiplier(mult);
  return "Frame Gen x" + std::to_string(mult) + " — mais FPS, mais carga GPU";
}
inline std::string session_notice_fg_queue_apply(int queue, bool restarting) {
  const int q = clamp_fg_queue(queue);
  if (restarting) return "Perfil FG fila" + std::to_string(q) + " — reabrindo o Lossless";
  return "Perfil FG fila" + std::to_string(q) + " — na proxima abertura";
}
inline std::string session_notice_fg_flow_apply(int flow, bool restarting) {
  const char* label = fg_flow_scale_label(flow);
  if (restarting) return std::string("Escala de fluxo ") + label + " — reabrindo o Lossless";
  return std::string("Escala de fluxo ") + label + " — na proxima abertura";
}
inline std::string session_notice_fg_adaptive_apply(bool adaptive, int target, int monitor_hz, bool restarting,
                                                    double capture_fps = 0) {
  if (!adaptive) {
    return restarting ? "Frame Gen Fixo — reabrindo o Lossless" : "Frame Gen Fixo — na proxima abertura";
  }
  const int t = clamp_fg_adaptive_target(target, monitor_hz, capture_fps);
  if (restarting) return "Frame Gen Adaptive " + std::to_string(t) + " — reabrindo o Lossless";
  return "Frame Gen Adaptive " + std::to_string(t) + " — na proxima abertura";
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
// Delete sem stack RenoDX: avisa o lobby. Nunca reutilizar session_notice_nr_off()
// (HUD fingia OFF com a imagem ainda limpa / sem filtros).
inline std::string session_notice_nr_needs_renodx() {
  return "Neural Rendering — marque RenoDX no lobby";
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
inline std::string session_notice_dlss5_motor_swap(int from_pass, int to_pass) {
  return std::string("A aplicar o motor: ") + nr_pass_count_label(from_pass) + " → " +
         nr_pass_count_label(to_pass);
}
inline std::string session_notice_nr_pass_count_apply(int from_pass, int to_pass) {
  return std::string("A aplicar Pass Count: ") + nr_pass_count_label(from_pass) + " → " +
         nr_pass_count_label(to_pass);
}
inline std::string session_notice_named_preset_apply(const std::string& name) {
  return std::string("Aplicando preset \xE2\x86\x92 ") + name;
}
inline std::string session_notice_preset_apply(int /*from_preset*/, int to_preset, int pass_count = 1) {
  return session_notice_named_preset_apply(renodx_visual_preset_name(to_preset, pass_count));
}
inline std::string session_notice_nr_apply(bool want_on) {
  return std::string("Aplicando DLSS 5: ") + (want_on ? "ON" : "OFF");
}
inline std::string session_notice_nr_hotkey_unavailable(int pass_count) {
  return std::string("Neural Rendering sempre ON em ") + nr_pass_count_label(pass_count) +
         " — on/off pelo Home do RenoDX";
}
inline std::string session_notice_cfg_open(int vk_cfg, bool cfg_ctrl) {
  std::string key = cfg_ctrl ? std::string("Ctrl+") : std::string();
  key += vk_display_name(vk_cfg);
  return "CFG aberto — " + key + " ou Esc fecha";
}
inline std::string session_notice_cfg_closed() {
  return "CFG fechado — ajustes mantidos";
}
// Lobby: a pessoa ainda não está na captura. O atalho do CFG só vale depois do Iniciar.
inline std::string lobby_notice_cfg_after_start(int vk_cfg, bool cfg_ctrl) {
  std::string key = cfg_ctrl ? std::string("Ctrl+") : std::string();
  key += vk_display_name(vk_cfg);
  return "Apos Iniciar, " + key + " abre o menu de configuracoes";
}
inline std::string session_notice_feed_scale_apply(int from_pct, int to_pct) {
  return std::string("Aplicando escala do Feed: ") + std::to_string(clamp_scale_percent(from_pct)) + "% → " +
         std::to_string(clamp_scale_percent(to_pct)) + "%";
}
inline std::string session_notice_lobby_iniciar_apply() { return lobby_iniciar_hold_title(); }

// Toast de Pass/preset/escala/Iniciar: o processo morre no restart. Grava e mostra na volta.
inline std::string restart_toast_path() {
  const std::string base = app_data_dir();
  return base.empty() ? std::string("boot_toast.txt") : base + "\\boot_toast.txt";
}

inline bool write_restart_toast_in(const std::string& path, const std::string& msg) {
  if (path.empty()) return false;
  const auto p = std::filesystem::u8path(path);
  if (msg.empty()) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
    return true;
  }
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << msg;
  return static_cast<bool>(out);
}

inline std::string take_restart_toast_in(const std::string& path) {
  if (path.empty()) return {};
  const auto p = std::filesystem::u8path(path);
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  in.close();
  std::error_code ec;
  std::filesystem::remove(p, ec);
  std::string s = ss.str();
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

inline bool write_restart_toast(const std::string& msg) { return write_restart_toast_in(restart_toast_path(), msg); }

inline std::string take_restart_toast() { return take_restart_toast_in(restart_toast_path()); }

inline std::string perf_test_pipeline_notice_title() { return "Gerando o pipeline do Benchmark"; }
inline std::string perf_test_pipeline_notice_detail() { return "Aguarde: a janela abre quando estiver pronto"; }
inline std::string perf_test_pipeline_fail_title() { return "Pipeline do Benchmark nao ficou pronto"; }
inline std::string perf_test_pipeline_fail_detail() { return "Timeout. Volta ao lobby: tenta de novo."; }
inline std::string session_notice_perf_test_hold() {
  return perf_test_pipeline_notice_title() + " — " + perf_test_pipeline_notice_detail();
}

// Split "titulo — detalhe" so HUD/toast never clip the second half.
inline void session_notice_split(const std::string& msg, std::string& title, std::string& detail) {
  title.clear();
  detail.clear();
  if (msg.empty()) return;
  static const std::string kSep = " — ";
  const auto pos = msg.find(kSep);
  if (pos == std::string::npos) {
    title = msg;
    return;
  }
  title = msg.substr(0, pos);
  detail = msg.substr(pos + kSep.size());
}

inline int session_notice_extra_px(const std::string& msg) {
  if (msg.empty()) return 0;
  std::string t, d;
  session_notice_split(msg, t, d);
  return d.empty() ? 36 : 60;
}

inline int session_notice_toast_scale() { return 3; }

inline int session_notice_band_px(const std::string& msg, int scale) {
  if (scale < 1) scale = 1;
  const int extra = session_notice_extra_px(msg);
  if (extra <= 0) return 0;
  return extra * scale + 12 * scale;
}

// Toast no canto inferior esquerdo, fora do overlay (live/mini/compacto/ADV).
inline void overlay_notice_place(int bar_w, int bar_h, int back_w, int back_h, int& x, int& y) {
  constexpr int kMargin = 40;
  x = kMargin;
  y = back_h - bar_h - kMargin;
  if (back_w > 0 && x + bar_w > back_w) x = back_w - bar_w;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
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

// Smoke / selftest / fake não podem apagar NeuralUplift do utilizador. O lobby
// é que grava OFF quando o RenoDX é desmarcado de propósito.
inline bool session_writes_renodx_off_files(const AppConfig& cfg) {
  if (cfg.enhance == EnhanceBackend::Renodx) return false;
  if (cfg.perf_test) return false;
  if (cfg.smoke || cfg.fake_capture || cfg.selftest_cap || cfg.selftest_cfg || cfg.selftest_fg ||
      cfg.selftest_dlss) {
    return false;
  }
  return true;
}

// --autostart e --no-lobby pulam o lobby. O cfg nasce com NrAdvancedLook vazio
// (UI Ligada, aparência desligada). Sem esta cópia a sessão grava esse vazio
// por cima da fábrica do 1,5x.
inline void seed_cfg_nr_advanced_from_settings(AppConfig& cfg, const PersistedSettings& s) {
  copy_nr_advanced(cfg.nr_advanced, s.nr_advanced);
}

// Perf-test fica na fábrica. Ponteiro para o struct vazio não é a fábrica.
inline const NrAdvancedLook* session_nr_look_for_sync(const AppConfig& cfg) {
  if (cfg.perf_test) return nullptr;
  return &cfg.nr_advanced[clamp_nr_pass_count(cfg.nr_pass_count)];
}

// CFG da sessão → disco. O processo novo (--autostart) lê isto para voltar igual.
// Teste de performance nunca toca nos presets HDMI (Pass / tom / escala / FG / NR).
inline void apply_session_cfg_to_persisted(const AppConfig& cfg, PersistedSettings& s) {
  if (cfg.perf_test) return;
  s.renodx_preset = clamp_renodx_visual_preset(cfg.renodx_preset);
  s.nr_pass_count = clamp_nr_pass_count(cfg.nr_pass_count);
  copy_nr_advanced(s.nr_advanced, cfg.nr_advanced);
  s.scale_percent = clamp_scale_percent(cfg.scale_percent);
  s.capture_volume = clamp_capture_volume_percent(cfg.capture_volume);
  s.capture_tone = clamp_capture_tone(cfg.capture_tone);
  s.ui.live_console = live_console_key(clamp_live_console(cfg.live_console));
  s.ui.overlay_console = cfg.overlay_console;
  s.ui.overlay_no_bg = cfg.overlay_no_bg;
  apply_session_fg_to_persisted(cfg.fg_auto_start, cfg.fg_multiplier, s);
  s.fg_queue = clamp_fg_queue(cfg.fg_queue);
  s.fg_flow_scale = clamp_fg_flow_scale(cfg.fg_flow_scale);
  s.fg_adaptive = cfg.fg_adaptive;
  s.fg_target_fps = clamp_fg_adaptive_target_stored(cfg.fg_target_fps);
  apply_session_nr_to_persisted(cfg.neural_rendering, cfg.enhance == EnhanceBackend::Renodx, s);
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
