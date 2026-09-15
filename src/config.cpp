#include "nexuscap/config.hpp"

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
    } else if (a == "--frames" && i + 1 < argc) {
      c.smoke_frames = std::atoi(argv[++i]);
      c.frames_limited = true;
    } else if (a == "--scale" && i + 1 < argc) {
      c.scale_percent = clamp_scale_percent(std::atoi(argv[++i]));
    } else if (a == "--device" && i + 1 < argc) {
      next(c.device_name);
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
      if (parse_fg_multiplier(argv[++i], m)) c.fg_multiplier = m;
    } else if (a == "--fg-path" && i + 1 < argc) {
      next(c.fg_path);
    } else if (a == "--help" || a == "-h") {
      // handled by main
    }
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
  cfg.video_fourcc = clamp_video_fourcc(s.video_fourcc);
  cfg.color_space = clamp_color_space(s.color_space);
  cfg.color_range = clamp_color_range(s.color_range);
  cfg.capture_buffering = clamp_capture_buffering(s.capture_buffering);
  cfg.exclusive_fullscreen = s.ui.fullscreen;
  cfg.show_overlay = s.ui.show_fps;
  cfg.advanced_metrics = s.ui.advanced_metrics;
  cfg.hw_info = s.ui.hw_info;
  cfg.hide_panel_on_capture = s.ui.hide_panel_on_capture;
  cfg.sync_image = s.ui.sync_image;
  cfg.hotkeys = s.hotkeys;
  if (s.fg_enabled) {
    cfg.fg_multiplier = s.fg_multiplier >= 2 ? s.fg_multiplier : 2;
  } else {
    cfg.fg_multiplier = 1;
  }
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
