#include "nexuscap/settings_store.hpp"
#include "nexuscap/capture_format.hpp"
#include "nexuscap/dlss5_bridge.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace nexuscap {
namespace {

std::string trim(std::string s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.erase(s.begin());
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
  return s;
}

}  // namespace

HotkeyConfig default_hotkeys() { return HotkeyConfig{}; }

PersistedSettings default_settings() { return PersistedSettings{}; }

std::string settings_default_path() {
  wchar_t* buf = nullptr;
  size_t len = 0;
  if (_wdupenv_s(&buf, &len, L"LOCALAPPDATA") != 0 || !buf) return "settings.ini";
  std::wstring dir(buf);
  free(buf);
  dir += L"\\NexusCap";
  CreateDirectoryW(dir.c_str(), nullptr);
  dir += L"\\settings.ini";
  char narrow[MAX_PATH];
  WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, narrow, MAX_PATH, nullptr, nullptr);
  return narrow;
}

std::string serialize_settings_ini(const PersistedSettings& s) {
  std::ostringstream o;
  o << "[session]\n";
  o << "device=" << s.device_name << "\n";
  o << "width=" << s.width << "\n";
  o << "height=" << s.height << "\n";
  o << "fps=" << s.fps << "\n";
  o << "scale_percent=" << clamp_scale_percent(s.scale_percent) << "\n";
  o << "fg_enabled=" << (s.fg_enabled ? 1 : 0) << "\n";
  o << "fg_multiplier=" << s.fg_multiplier << "\n";
  o << "fg_path=" << s.fg_path << "\n";
  o << "enhance_renodx=" << (s.enhance_renodx ? 1 : 0) << "\n";
  o << "neural_rendering=" << (s.neural_rendering ? 1 : 0) << "\n";
  o << "renodx_preset=" << clamp_renodx_visual_preset(s.renodx_preset) << "\n";
  o << "video_fourcc=" << clamp_video_fourcc(s.video_fourcc) << "\n";
  o << "color_space=" << clamp_color_space(s.color_space) << "\n";
  o << "color_range=" << clamp_color_range(s.color_range) << "\n";
  o << "capture_buffering=" << clamp_capture_buffering(s.capture_buffering) << "\n";
  o << "show_fps=" << (s.ui.show_fps ? 1 : 0) << "\n";
  o << "advanced_metrics=" << (s.ui.advanced_metrics ? 1 : 0) << "\n";
  o << "hw_info=" << (s.ui.hw_info ? 1 : 0) << "\n";
  o << "fullscreen=" << (s.ui.fullscreen ? 1 : 0) << "\n";
  o << "hide_panel_on_capture=" << (s.ui.hide_panel_on_capture ? 1 : 0) << "\n";
  o << "sync_image=" << (s.ui.sync_image ? 1 : 0) << "\n";
  o << "\n[hotkeys]\n";
  o << "quit=" << s.hotkeys.vk_quit << "\n";
  o << "hud=" << s.hotkeys.vk_hud << "\n";
  o << "analyze=" << s.hotkeys.vk_analyze << "\n";
  o << "dlss=" << s.hotkeys.vk_dlss << "\n";
  o << "fg_toggle=" << s.hotkeys.vk_fg_toggle << "\n";
  o << "fg_cycle=" << s.hotkeys.vk_fg_cycle << "\n";
  o << "renodx=" << s.hotkeys.vk_renodx << "\n";
  o << "nr_toggle=" << s.hotkeys.vk_nr_toggle << "\n";
  o << "fullscreen=" << s.hotkeys.vk_fullscreen << "\n";
  o << "gear=" << s.hotkeys.vk_gear << "\n";
  return o.str();
}

bool parse_settings_ini(const std::string& text, PersistedSettings& s) {
  s = default_settings();
  std::istringstream in(text);
  std::string line;
  std::string section;
  auto set_int = [](const std::string& v, int& out) {
    out = std::atoi(v.c_str());
  };
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line.front() == '[' && line.back() == ']') {
      section = line.substr(1, line.size() - 2);
      continue;
    }
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(line.substr(0, eq));
    const std::string val = trim(line.substr(eq + 1));
    if (section == "session" || section.empty()) {
      if (key == "device") s.device_name = val;
      else if (key == "width") set_int(val, s.width);
      else if (key == "height") set_int(val, s.height);
      else if (key == "fps") s.fps = std::atof(val.c_str());
      else if (key == "scale_percent") s.scale_percent = clamp_scale_percent(std::atoi(val.c_str()));
      else if (key == "fg_enabled") s.fg_enabled = (val == "1" || val == "true");
      else if (key == "fg_multiplier") {
        int m = std::atoi(val.c_str());
        if (m < 2) m = 2;
        if (m > 4) m = 4;
        s.fg_multiplier = m;
      } else if (key == "fg_path") {
        s.fg_path = val;
      } else if (key == "enhance_renodx") s.enhance_renodx = (val != "0" && val != "false");
      else if (key == "neural_rendering") s.neural_rendering = (val != "0" && val != "false");
      else if (key == "renodx_preset") s.renodx_preset = clamp_renodx_visual_preset(std::atoi(val.c_str()));
      else if (key == "video_fourcc") s.video_fourcc = clamp_video_fourcc(std::atoi(val.c_str()));
      else if (key == "color_space") s.color_space = clamp_color_space(std::atoi(val.c_str()));
      else if (key == "color_range") s.color_range = clamp_color_range(std::atoi(val.c_str()));
      else if (key == "capture_buffering") s.capture_buffering = clamp_capture_buffering(std::atoi(val.c_str()));
      else if (key == "show_fps") s.ui.show_fps = (val == "1" || val == "true");
      else if (key == "advanced_metrics") s.ui.advanced_metrics = (val == "1" || val == "true");
      else if (key == "hw_info") s.ui.hw_info = (val == "1" || val == "true");
      else if (key == "fullscreen") s.ui.fullscreen = (val == "1" || val == "true");
      else if (key == "hide_panel_on_capture") s.ui.hide_panel_on_capture = (val == "1" || val == "true");
      else if (key == "sync_image") s.ui.sync_image = (val == "1" || val == "true");
      // legacy auto_start ignored — lobby always opens first (maximized window)
    } else if (section == "hotkeys") {
      int v = std::atoi(val.c_str());
      if (key == "quit") s.hotkeys.vk_quit = v;
      else if (key == "hud") s.hotkeys.vk_hud = v;
      else if (key == "analyze") s.hotkeys.vk_analyze = v;
      else if (key == "dlss") s.hotkeys.vk_dlss = v;
      else if (key == "fg_toggle") s.hotkeys.vk_fg_toggle = v;
      else if (key == "fg_cycle") s.hotkeys.vk_fg_cycle = v;
      else if (key == "renodx") s.hotkeys.vk_renodx = v;
      else if (key == "nr_toggle") s.hotkeys.vk_nr_toggle = v;
      else if (key == "fullscreen") s.hotkeys.vk_fullscreen = v;
      else if (key == "gear") s.hotkeys.vk_gear = v;
    }
  }
  return true;
}

bool save_settings(const PersistedSettings& s, const std::string& path, std::string& err) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    err = "cannot write " + path;
    return false;
  }
  out << serialize_settings_ini(s);
  return true;
}

bool load_settings(PersistedSettings& s, const std::string& path, std::string& err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    err = "missing " + path;
    s = default_settings();
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return parse_settings_ini(ss.str(), s);
}

const char* vk_display_name(int vk) {
  switch (vk) {
    case 0x1B:
      return "Esc";
    case 0x70:
      return "F1";
    case 0x71:
      return "F2";
    case 0x72:
      return "F3";
    case 0x73:
      return "F4";
    case 0x74:
      return "F5";
    case 0x75:
      return "F6";
    case 0x7A:
      return "F11";
    case 0x7B:
      return "F12";
    case 0x21:
      return "Page Up";
    case 0x22:
      return "Page Down";
    case 0x24:
      return "Home";
    case 0x23:
      return "End";
    case 0x2D:
      return "Insert";
    case 0x2E:
      return "Delete";
    default:
      break;
  }
  return "Key";
}

bool sync_feed_work_resolution_file(const std::string& path, int scale_percent) {
  std::ifstream in(path);
  if (!in) return false;
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  text = apply_feed_work_resolution(std::move(text), scale_percent);
  std::ofstream out(path, std::ios::trunc);
  if (!out) return false;
  out << text;
  return true;
}

bool sync_renodx_visual_preset_files(const std::string& exe_dir, int visual_preset) {
  const int v = clamp_renodx_visual_preset(visual_preset);
  bool any = false;
  const std::string feed_path = exe_dir + "\\dlss5-feed.cfg";
  {
    std::ifstream in(feed_path);
    if (in) {
      std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      in.close();
      text = apply_renodx_visual_preset_to_feed(std::move(text), v);
      std::ofstream out(feed_path, std::ios::trunc);
      if (out) {
        out << text;
        any = true;
      }
    }
  }
  const std::string reshade_path = exe_dir + "\\ReShade.ini";
  {
    std::ifstream in(reshade_path);
    if (in) {
      std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      in.close();
      text = apply_renodx_visual_preset_to_reshade(std::move(text), v);
      // Bridge injects F13 into RenoDX; UI Delete calls Bridge (avoids double-toggle).
      text = apply_reshade_nr_toggle_key(std::move(text), kRenodxNrInjectVk);
      std::ofstream out(reshade_path, std::ios::trunc);
      if (out) {
        out << text;
        any = true;
      }
    }
  }
  return any;
}

bool sync_neural_rendering_enabled_files(const std::string& exe_dir, bool enabled) {
  bool any = false;
  const std::string feed_path = exe_dir + "\\dlss5-feed.cfg";
  {
    std::ifstream in(feed_path);
    if (in) {
      std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      in.close();
      text = apply_feed_enabled(std::move(text), enabled);
      std::ofstream out(feed_path, std::ios::trunc);
      if (out) {
        out << text;
        any = true;
      }
    }
  }
  const std::string reshade_path = exe_dir + "\\ReShade.ini";
  {
    std::ifstream in(reshade_path);
    if (in) {
      std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      in.close();
      text = apply_reshade_neural_uplift(std::move(text), enabled);
      text = apply_reshade_nr_toggle_key(std::move(text), kRenodxNrInjectVk);
      std::ofstream out(reshade_path, std::ios::trunc);
      if (out) {
        out << text;
        any = true;
      }
    }
  }
  return any;
}

}  // namespace nexuscap
