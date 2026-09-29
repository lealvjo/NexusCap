#include "nexuscap/config.hpp"
#include "nexuscap/capture.hpp"
#include "nexuscap/live_mode.hpp"
#include "nexuscap/perf_test.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

namespace nexuscap {

AppConfig parse_args(int argc, char** argv) {
  AppConfig c;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](std::string& out) {
      if (i + 1 < argc) out = argv[++i];
    };
    if (a == "--smoke") {
      c.smoke = true;
      c.fake_capture = true;
      c.exclusive_fullscreen = false;
      c.dlss = false;
      c.show_lobby = false;
    } else if (a == "--fake-capture") {
      c.fake_capture = true;
    } else if (a == "--no-lobby") {
      c.show_lobby = false;
    } else if (a == "--autostart") {
      c.autostart = true;  // lobby settings still seed cfg; first lobby pass is skipped
    } else if (a == "--portable") {
      c.portable = true;  // same as portable.txt beside the exe
    } else if (a.rfind("--hud=", 0) == 0) {
      OverlayHudStep step = OverlayHudStep::Compact;
      if (parse_overlay_hud_step(a.c_str() + 6, step)) {
        c.overlay_hud_restore = static_cast<int>(step);
      }
    } else if (a == "--hud" && i + 1 < argc) {
      OverlayHudStep step = OverlayHudStep::Compact;
      if (parse_overlay_hud_step(argv[++i], step)) {
        c.overlay_hud_restore = static_cast<int>(step);
      }
    } else if (a == "--lobby-cfg") {
      c.open_lobby_cfg = true;  // CFG was open; reopen after lobby motor/Pass Count restart
    } else if (a == "--simulate-hdmi-busy") {
      c.simulate_hdmi_busy = true;
    } else if (a == "--selftest-cap" || a == "--selftest-vedo") {
      c.selftest_cap = true;
      c.exclusive_fullscreen = false;
      c.show_lobby = false;
    } else if (a == "--selftest-dlss") {
      c.selftest_dlss = true;
      c.dlss = true;
      c.dlss_mode = DlssMode::Dlaa;
      c.exclusive_fullscreen = false;
      c.show_lobby = false;
      if (!c.frames_limited && c.smoke_frames < 600) {
        c.smoke_frames = 1800;
        c.frames_limited = true;
      }
    } else if (a == "--selftest-fg") {
      c.selftest_fg = true;
      c.exclusive_fullscreen = false;
      c.show_lobby = false;
      if (!c.frames_limited && c.smoke_frames < 60) {
        c.smoke_frames = 90;
        c.frames_limited = true;
      }
      if (c.fg_multiplier < 2) c.fg_multiplier = 2;
    } else if (a == "--selftest-cfg") {
      c.selftest_cfg = true;
      c.fake_capture = true;
      c.exclusive_fullscreen = false;
      c.show_lobby = false;
      if (!c.frames_limited) {
        c.smoke_frames = 90;
        c.frames_limited = true;
      }
    } else if (a == "--frames" && i + 1 < argc) {
      c.smoke_frames = std::atoi(argv[++i]);
      c.frames_limited = true;
    } else if (a == "--scale" && i + 1 < argc) {
      c.scale_percent = clamp_scale_percent(std::atoi(argv[++i]));
      c.perf_cli_override = true;
    } else if (a == "--device" && i + 1 < argc) {
      next(c.device_name);
    } else if (a == "--source" && i + 1 < argc) {
      const std::string v = argv[++i];
      if (v == "window" || v == "janela") c.capture_source = 1;
      else c.capture_source = 0;
    } else if (a == "--window" && i + 1 < argc) {
      next(c.window_title);
      if (!c.window_title.empty()) c.capture_source = 1;
    } else if (a == "--log-dir" && i + 1 < argc) {
      next(c.log_dir);
    } else if (a == "--hitch-ms" && i + 1 < argc) {
      c.hitch_threshold_ms = std::atof(argv[++i]);
    } else if (a == "--windowed") {
      c.exclusive_fullscreen = false;
    } else if (a == "--dlss") {
      c.dlss = true;
      if (c.enhance == EnhanceBackend::Off) c.enhance = EnhanceBackend::Streamline;
    } else if (a == "--no-dlss") {
      c.dlss = false;
      if (c.enhance == EnhanceBackend::Streamline) c.enhance = EnhanceBackend::Off;
    } else if (a == "--renodx") {
      c.enhance = EnhanceBackend::Renodx;
      c.dlss = false;  // ReShade/RenoDX owns DLSS; avoid double Streamline
    } else if (a == "--enhance" && i + 1 < argc) {
      EnhanceBackend b = c.enhance;
      if (parse_enhance_backend(argv[++i], b)) {
        c.enhance = b;
        c.dlss = (b == EnhanceBackend::Streamline);
      }
    } else if (a == "--dlss-mode" && i + 1 < argc) {
      DlssMode m = c.dlss_mode;
      if (parse_dlss_mode(argv[++i], m)) c.dlss_mode = m;
    } else if (a == "--dlss-sharpness" && i + 1 < argc) {
      c.dlss_sharpness = clamp01(static_cast<float>(std::atof(argv[++i])));
    } else if (a == "--dlss-mvec" && i + 1 < argc) {
      DlssMvecSource s = c.dlss_mvec;
      if (parse_dlss_mvec(argv[++i], s)) c.dlss_mvec = s;
    } else if (a == "--fg-x" && i + 1 < argc) {
      int m = c.fg_multiplier;
      if (parse_fg_multiplier(argv[++i], m)) {
        c.fg_multiplier = m;
        c.perf_cli_override = true;
      }
    } else if (a == "--fg-adaptive" && i + 1 < argc) {
      const std::string v = argv[++i];
      c.fg_adaptive = (v == "1" || v == "true" || v == "ADAPTIVE" || v == "adaptive");
      c.perf_cli_override = true;
    } else if (a == "--fg-target" && i + 1 < argc) {
      c.fg_target_fps = clamp_fg_adaptive_target_stored(std::atoi(argv[++i]));
      c.perf_cli_override = true;
    } else if (a == "--fg-path" && i + 1 < argc) {
      next(c.fg_path);
    } else if (a == "--perf-test") {
      c.perf_test = true;
    } else if (a == "--show-perf-result") {
      c.popup_perf_result = true;
    } else if (a == "--perf-audio") {
      c.perf_test_audio = true;
    } else if (a == "--no-perf-audio") {
      c.perf_test_audio = false;
    } else if (a.rfind("--video=", 0) == 0) {
      c.video_path = a.substr(8);
    } else if (a == "--video" && i + 1 < argc) {
      next(c.video_path);
    } else if (a == "--width" && i + 1 < argc) {
      c.width = std::atoi(argv[++i]);
      c.perf_cli_override = true;
    } else if (a == "--height" && i + 1 < argc) {
      c.height = std::atoi(argv[++i]);
      c.perf_cli_override = true;
    } else if (a == "--fps" && i + 1 < argc) {
      c.fps = std::atof(argv[++i]);
      c.perf_cli_override = true;
    } else if (a == "--pass" && i + 1 < argc) {
      c.nr_pass_count = clamp_nr_pass_count(std::atoi(argv[++i]));
      c.perf_cli_override = true;
    } else if (a == "--preset" && i + 1 < argc) {
      c.renodx_preset = clamp_renodx_visual_preset(std::atoi(argv[++i]));
      c.perf_cli_override = true;
    } else if (a == "--help" || a == "-h") {
      // handled by main
    }
  }
  if (c.perf_test) {
    clamp_perf_test_signal(c.width, c.height);
    c.fps = clamp_perf_test_fps(c.fps);
    c.scale_percent = clamp_scale_percent(c.scale_percent);
    if (c.scale_percent < kMinFeedWorkResolution) c.scale_percent = kMinFeedWorkResolution;
    c.exclusive_fullscreen = false;
    c.sync_image = true;
    c.show_overlay = true;
    c.mini_overlay = true;
    c.advanced_metrics = false;
    c.live_overlay = false;
    c.enhance = EnhanceBackend::Renodx;
    c.dlss = false;
    c.neural_rendering = true;
    c.nr_runtime_on = true;
    if (c.fg_multiplier >= 2) c.fg_auto_start = true;
    else c.fg_auto_start = false;
    c.bug_report = true;
  }
  return c;
}

void apply_persisted_to_config(const PersistedSettings& s, AppConfig& cfg) {
  cfg.device_name = s.device_name;
  cfg.width = s.width;
  cfg.height = s.height;
  cfg.fps = s.fps;
  cfg.scale_percent = clamp_scale_percent(s.scale_percent);
  cfg.renodx_preset = clamp_renodx_visual_preset(s.renodx_preset);
  cfg.nr_pass_count = clamp_nr_pass_count(s.nr_pass_count);
  cfg.video_fourcc = clamp_video_fourcc(s.video_fourcc);
  cfg.color_space = clamp_color_space(s.color_space);
  cfg.color_range = clamp_color_range(s.color_range);
  cfg.capture_buffering = clamp_capture_buffering(s.capture_buffering);
  cfg.capture_volume = clamp_capture_volume_percent(s.capture_volume);
  cfg.capture_source = clamp_capture_source(s.capture_source);
  cfg.window_title = s.window_title;
  cfg.window_hwnd = s.window_hwnd;
  cfg.exclusive_fullscreen = s.ui.fullscreen;
  cfg.show_overlay = s.ui.show_fps;
  cfg.advanced_metrics = s.ui.advanced_metrics;
  cfg.mini_overlay = s.ui.mini_overlay;
  cfg.overlay_no_bg = s.ui.overlay_no_bg;
  cfg.hw_info = s.ui.hw_info;
  cfg.hide_panel_on_capture = true;  // captura sempre limpa (opcao do lobby saiu)
  cfg.sync_image = s.ui.sync_image;
  cfg.bug_report = s.ui.bug_report;
  cfg.perf_report = s.ui.perf_report;
  cfg.live_overlay = s.ui.live_overlay;
  cfg.overlay_console = s.ui.overlay_console;
  cfg.live_console = static_cast<int>(parse_live_console(s.ui.live_console));
  cfg.hotkeys = s.hotkeys;
  // Keep preferred x2/x3/x4 armed even when checkbox is off (PageDn can enable).
  cfg.fg_multiplier = armed_fg_multiplier(s.fg_enabled, s.fg_multiplier);
  cfg.fg_queue = clamp_fg_queue(s.fg_queue);
  cfg.fg_adaptive = s.fg_adaptive;
  cfg.fg_target_fps = clamp_fg_adaptive_target_stored(s.fg_target_fps);
  cfg.fg_auto_start = s.fg_enabled;
  cfg.fg_path = s.fg_path;
  if (s.enhance_renodx) {
    cfg.enhance = EnhanceBackend::Renodx;
    cfg.dlss = false;
    cfg.neural_rendering = s.neural_rendering;
  } else {
    cfg.enhance = EnhanceBackend::Off;
    cfg.neural_rendering = false;
  }
}

}  // namespace nexuscap
