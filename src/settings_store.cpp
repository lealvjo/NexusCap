#include "nexuscap/settings_store.hpp"
#include "nexuscap/pipeline.hpp"
#include "nexuscap/lossless_fg.hpp"
#include "nexuscap/audio.hpp"
#include "nexuscap/capture_format.hpp"
#include "nexuscap/dlss5_bridge.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

namespace nexuscap {
namespace {

std::string trim(std::string s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.erase(s.begin());
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
  return s;
}

}  // namespace

namespace {

bool patch_reshade_home_splash(uint8_t* base, size_t bytes) {
  if (reshade68_home_splash_gate_offset(base, bytes, 0xEB) != static_cast<size_t>(-1)) return true;
  const size_t gate = reshade68_home_splash_gate_offset(base, bytes, 0x74);
  if (gate == static_cast<size_t>(-1) || gate >= bytes) return false;
  auto* op = base + gate;
  DWORD old = 0;
  if (!VirtualProtect(op, 1, PAGE_EXECUTE_READWRITE, &old)) return false;
  op[0] = 0xEB;
  DWORD ignored = 0;
  VirtualProtect(op, 1, old, &ignored);
  FlushInstructionCache(GetCurrentProcess(), op, 1);
  return true;
}

}  // namespace

bool silence_reshade_home_splash() {
  static bool done = false;
  if (done) return true;
  const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
  if (snap == INVALID_HANDLE_VALUE) return false;
  MODULEENTRY32W me{};
  me.dwSize = sizeof(me);
  bool patched = false;
  if (Module32FirstW(snap, &me)) {
    do {
      if (_wcsicmp(me.szModule, L"dxgi.dll") != 0) continue;
      if (patch_reshade_home_splash(static_cast<uint8_t*>(me.modBaseAddr), me.modBaseSize)) {
        patched = true;
        break;
      }
    } while (Module32NextW(snap, &me));
  }
  CloseHandle(snap);
  if (!patched) return false;
  done = true;
  std::printf("renodx: aviso Home desligado\n");
  return true;
}

HotkeyConfig default_hotkeys() { return HotkeyConfig{}; }

PersistedSettings default_settings() {
  PersistedSettings s;
  fill_nr_advanced_defaults(s.nr_advanced);
  return s;
}

namespace {

std::string wide_to_utf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string out(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 1) return {};
  std::wstring out(static_cast<size_t>(n - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
  return out;
}

bool file_exists_utf8(const std::string& path) {
  const std::wstring w = utf8_to_wide(path);
  const DWORD a = GetFileAttributesW(w.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool write_text_if_changed(const std::string& path, const std::string& before, const std::string& after) {
  if (after == before) return true;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << after;
  return out.good();
}

bool files_have_same_contents(const std::string& a, const std::string& b) {
  std::ifstream left(a, std::ios::binary | std::ios::ate);
  std::ifstream right(b, std::ios::binary | std::ios::ate);
  if (!left || !right || left.tellg() != right.tellg()) return false;
  left.seekg(0);
  right.seekg(0);
  constexpr std::size_t kChunk = 4096;
  char lb[kChunk]{};
  char rb[kChunk]{};
  while (left && right) {
    left.read(lb, static_cast<std::streamsize>(kChunk));
    right.read(rb, static_cast<std::streamsize>(kChunk));
    const std::streamsize n = left.gcount();
    if (n != right.gcount() || std::memcmp(lb, rb, static_cast<std::size_t>(n)) != 0) return false;
  }
  return true;
}

std::string local_appdata_env() {
  wchar_t* buf = nullptr;
  size_t len = 0;
  if (_wdupenv_s(&buf, &len, L"LOCALAPPDATA") != 0 || !buf) return {};
  std::wstring dir(buf);
  free(buf);
  return wide_to_utf8(dir);
}

std::string module_dir() {
  wchar_t exe[MAX_PATH]{};
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH) || !exe[0]) return {};
  std::wstring p(exe);
  const auto slash = p.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return {};
  return wide_to_utf8(p.substr(0, slash));
}

bool g_portable_cli = false;
bool g_paths_ready = false;
bool g_portable = false;
std::string g_data_dir;

}  // namespace

void init_app_paths(bool portable_cli) {
  g_portable_cli = portable_cli;
  g_paths_ready = false;
  g_portable = false;
  g_data_dir.clear();
}

bool app_is_portable() {
  (void)app_data_dir();
  return g_portable;
}

std::string app_data_dir() {
  if (g_paths_ready) return g_data_dir;
  const std::string exe_dir = module_dir();
  const std::string sentinel =
      exe_dir.empty() ? std::string{} : exe_dir + "\\" + kPortableSentinelName;
  g_portable = want_portable_mode(!sentinel.empty() && file_exists_utf8(sentinel), g_portable_cli);
  g_data_dir = resolve_app_data_dir(exe_dir, local_appdata_env(), g_portable);
  if (!g_data_dir.empty()) CreateDirectoryW(utf8_to_wide(g_data_dir).c_str(), nullptr);
  g_paths_ready = true;
  return g_data_dir;
}

std::string settings_default_path() {
  const std::string base = app_data_dir();
  if (base.empty()) return "settings.ini";
  return base + "\\settings.ini";
}

std::string app_logs_dir() {
  const std::string base = app_data_dir();
  if (base.empty()) return "logs";
  const std::wstring w = utf8_to_wide(base + "\\logs");
  CreateDirectoryW(w.c_str(), nullptr);
  return base + "\\logs";
}

std::string app_reshade_cache_dir() {
  const std::string base = app_data_dir();
  if (base.empty()) return "reshade-cache";
  const std::wstring w = utf8_to_wide(base + "\\reshade-cache");
  CreateDirectoryW(w.c_str(), nullptr);
  return base + "\\reshade-cache";
}

std::string app_presets_dir() {
  const std::string base = app_data_dir();
  if (base.empty()) return "presets";
  const std::wstring w = utf8_to_wide(base + "\\presets");
  CreateDirectoryW(w.c_str(), nullptr);
  return base + "\\presets";
}

bool directory_is_writable(const std::string& dir) {
  if (dir.empty()) return false;
  const std::wstring wdir = utf8_to_wide(dir);
  CreateDirectoryW(wdir.c_str(), nullptr);
  const std::wstring probe = wdir + L"\\.nexuscap_write_probe";
  HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  CloseHandle(h);
  DeleteFileW(probe.c_str());
  return true;
}

namespace {
bool move_file_utf8(const std::string& from, const std::string& to) {
  if (from.empty() || to.empty()) return false;
  if (!file_exists_utf8(from)) return file_exists_utf8(to);
  const std::wstring wfrom = utf8_to_wide(from);
  const std::wstring wto = utf8_to_wide(to);
  if (file_exists_utf8(to)) DeleteFileW(wto.c_str());
  return MoveFileExW(wfrom.c_str(), wto.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
}  // namespace

bool park_reshade_dxgi(const std::string& exe_dir) {
  const std::string live = reshade_dxgi_path_in(exe_dir);
  const std::string parked = reshade_dxgi_parked_path_in(exe_dir);
  if (live.empty()) return false;
  if (!file_exists_utf8(live)) return true;
  return move_file_utf8(live, parked);
}

bool restore_reshade_dxgi(const std::string& exe_dir) {
  const std::string live = reshade_dxgi_path_in(exe_dir);
  const std::string parked = reshade_dxgi_parked_path_in(exe_dir);
  if (live.empty()) return false;
  if (file_exists_utf8(live)) return true;
  if (!file_exists_utf8(parked)) return false;
  return move_file_utf8(parked, live);
}

bool park_reshade_dxgi_wait(const std::string& exe_dir, int timeout_ms) {
  const std::string live = reshade_dxgi_path_in(exe_dir);
  if (live.empty()) return false;
  const DWORD t0 = GetTickCount();
  const DWORD lim = timeout_ms < 0 ? 0 : static_cast<DWORD>(timeout_ms);
  for (;;) {
    if (park_reshade_dxgi(exe_dir) && !file_exists_utf8(live)) return true;
    if (GetTickCount() - t0 >= lim) break;
    Sleep(40);
  }
  return !file_exists_utf8(live);
}

bool reshade_dxgi_present(const std::string& exe_dir) {
  return file_exists_utf8(reshade_dxgi_path_in(exe_dir)) ||
         file_exists_utf8(reshade_dxgi_parked_path_in(exe_dir));
}

RuntimeHealth check_runtime_health(const std::string& exe_dir) {
  RuntimeHealth h;
  h.writable = directory_is_writable(exe_dir);
  if (!exe_dir.empty()) {
    const std::string base = exe_dir.back() == '\\' || exe_dir.back() == '/' ? exe_dir : exe_dir + "\\";
    const std::string worker = pipeline_worker_dir(exe_dir) + "\\";
    h.has_dxgi = reshade_dxgi_present(exe_dir) || file_exists_utf8(worker + "dxgi.dll");
    h.has_renodx = file_exists_utf8(base + "addons-multipass\\renodx-dlss5.addon64") ||
                    file_exists_utf8(base + "renodx-dlss5.addon64") ||
                    file_exists_utf8(worker + "addons-multipass\\renodx-dlss5.addon64");
    h.has_feed = file_exists_utf8(base + "dlss5-feed.addon64") ||
                 file_exists_utf8(base + "addons-multipass\\dlss5-feed.addon64") ||
                 file_exists_utf8(worker + "addons-multipass\\dlss5-feed.addon64") ||
                 file_exists_utf8(worker + "dlss5-feed.addon64");
    // Krish / Feed resolve NGX "next to the addon or the game's executable".
    auto stack_or_exe = [&](const char* name) {
      return file_exists_utf8(base + "addons-multipass\\" + name) || file_exists_utf8(base + name);
    };
    h.has_nr_runtime = stack_or_exe("nvngx_dlssnr.dll");
    h.has_dlss_runtime = stack_or_exe("nvngx_dlss.dll");
    h.has_bridge = stack_or_exe("dlss5-bridge.addon64");
  }
  return h;
}

bool reshade_dxgi_loaded_in_process(const std::string& exe_dir) {
  HMODULE mod = GetModuleHandleW(L"dxgi.dll");
  if (!mod) return false;
  wchar_t path[MAX_PATH]{};
  const DWORD n = GetModuleFileNameW(mod, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return false;
  return module_path_is_reshade_dxgi(wide_to_utf8(path), exe_dir);
}

bool open_app_logs_folder() {
  const std::string logs = app_logs_dir();
  if (logs.empty()) return false;
  const std::wstring w = utf8_to_wide(logs);
  const HINSTANCE r = ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  return reinterpret_cast<intptr_t>(r) > 32;
}

bool prepare_renodx_runtime_dirs(const std::string& exe_dir) {
  if (exe_dir.empty()) return false;
  const std::string cache = app_reshade_cache_dir();
  const bool writable = directory_is_writable(exe_dir);
  // Always try to point cache at AppData so ReShade does not write under Program Files.
  const std::string reshade_path = exe_dir + "\\ReShade.ini";
  std::ifstream in(reshade_path);
  if (in) {
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    text = apply_keyed_value(std::move(text), "IntermediateCachePath=", cache);
    std::ofstream out(reshade_path, std::ios::trunc);
    if (out) out << text;
  }
  // Local reshade-cache folder next to exe (harmless if writable; ignored if we rewrote path).
  CreateDirectoryW(utf8_to_wide(exe_dir + "\\reshade-cache").c_str(), nullptr);
  return writable;
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
  o << "fg_queue=" << clamp_fg_queue(s.fg_queue) << "\n";
  o << "fg_flow_scale=" << clamp_fg_flow_scale(s.fg_flow_scale) << "\n";
  o << "fg_adaptive=" << (s.fg_adaptive ? 1 : 0) << "\n";
  o << "fg_target_fps=" << clamp_fg_adaptive_target_stored(s.fg_target_fps) << "\n";
  o << "fg_path=" << s.fg_path << "\n";
  o << "enhance_renodx=" << (s.enhance_renodx ? 1 : 0) << "\n";
  o << "neural_rendering=" << (s.neural_rendering ? 1 : 0) << "\n";
  o << "renodx_preset=" << clamp_renodx_visual_preset(s.renodx_preset) << "\n";
  o << "nr_pass_count=" << clamp_nr_pass_count(s.nr_pass_count) << "\n";
  write_nr_advanced_lines(o, s.nr_advanced);
  o << "video_fourcc=" << clamp_video_fourcc(s.video_fourcc) << "\n";
  o << "color_space=" << clamp_color_space(s.color_space) << "\n";
  o << "color_range=" << clamp_color_range(s.color_range) << "\n";
  o << "capture_buffering=" << clamp_capture_buffering(s.capture_buffering) << "\n";
  o << "capture_volume=" << clamp_capture_volume_percent(s.capture_volume) << "\n";
  o << "capture_tone=" << clamp_capture_tone(s.capture_tone) << "\n";
  o << "max_perf=" << (s.max_perf ? 1 : 0) << "\n";
  o << "capture_source=" << (s.capture_source == 1 ? 1 : 0) << "\n";
  o << "window_title=" << s.window_title << "\n";
  o << "window_hwnd=" << s.window_hwnd << "\n";
  o << "show_fps=" << (s.ui.show_fps ? 1 : 0) << "\n";
  o << "advanced_metrics=" << (s.ui.advanced_metrics ? 1 : 0) << "\n";
  o << "mini_overlay=" << (s.ui.mini_overlay ? 1 : 0) << "\n";
  o << "overlay_no_bg=" << (s.ui.overlay_no_bg ? 1 : 0) << "\n";
  o << "hw_info=" << (s.ui.hw_info ? 1 : 0) << "\n";
  o << "fullscreen=" << (s.ui.fullscreen ? 1 : 0) << "\n";
  o << "hide_panel_on_capture=1\n";  // captura sempre limpa; ini antigo com 0 e ignorado
  o << "sync_image=" << (s.ui.sync_image ? 1 : 0) << "\n";
  o << "preview_mute=" << (s.ui.preview_mute ? 1 : 0) << "\n";
  o << "preview_enabled=" << (s.ui.preview_enabled ? 1 : 0) << "\n";
  o << "preview_cfg_live=" << (s.ui.preview_cfg_live ? 1 : 0) << "\n";
  o << "bug_report=" << (s.ui.bug_report ? 1 : 0) << "\n";
  o << "perf_report=" << (s.ui.perf_report ? 1 : 0) << "\n";
  o << "live_overlay=" << (s.ui.live_overlay ? 1 : 0) << "\n";
  o << "overlay_console=" << (s.ui.overlay_console ? 1 : 0) << "\n";
  o << "live_console=" << (s.ui.live_console.empty() ? "ps5" : s.ui.live_console) << "\n";
  o << "\n[hotkeys]\n";
  o << "quit=" << s.hotkeys.vk_quit << "\n";
  o << "hud=" << s.hotkeys.vk_hud << "\n";
  o << "hud_ctrl=" << (s.hotkeys.hud_ctrl ? 1 : 0) << "\n";
  o << "analyze=" << s.hotkeys.vk_analyze << "\n";
  o << "dlss=" << s.hotkeys.vk_dlss << "\n";
  o << "fg_toggle=" << s.hotkeys.vk_fg_toggle << "\n";
  o << "fg_cycle=" << s.hotkeys.vk_fg_cycle << "\n";
  o << "renodx=" << s.hotkeys.vk_renodx << "\n";
  o << "nr_toggle=" << s.hotkeys.vk_nr_toggle << "\n";
  o << "fullscreen=" << s.hotkeys.vk_fullscreen << "\n";
  o << "gear=" << s.hotkeys.vk_gear << "\n";
  o << "gear_ctrl=" << (s.hotkeys.gear_ctrl ? 1 : 0) << "\n";
  o << "cfg_key=" << s.hotkeys.vk_cfg << "\n";
  o << "cfg_ctrl=" << (s.hotkeys.cfg_ctrl ? 1 : 0) << "\n";
  o << "pass_next=" << s.hotkeys.vk_pass_next << "\n";
  o << "pass_next_ctrl=" << (s.hotkeys.pass_next_ctrl ? 1 : 0) << "\n";
  o << "pass_prev=" << s.hotkeys.vk_pass_prev << "\n";
  o << "pass_prev_ctrl=" << (s.hotkeys.pass_prev_ctrl ? 1 : 0) << "\n";
  o << "look_next=" << s.hotkeys.vk_look_next << "\n";
  o << "look_next_shift=" << (s.hotkeys.look_next_shift ? 1 : 0) << "\n";
  o << "look_prev=" << s.hotkeys.vk_look_prev << "\n";
  o << "look_prev_shift=" << (s.hotkeys.look_prev_shift ? 1 : 0) << "\n";
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
      } else if (key == "fg_queue") {
        s.fg_queue = clamp_fg_queue(std::atoi(val.c_str()));
      } else if (key == "fg_flow_scale") {
        s.fg_flow_scale = clamp_fg_flow_scale(std::atoi(val.c_str()));
      } else if (key == "fg_adaptive") {
        s.fg_adaptive = parse_fg_adaptive_ini(val) || val == "1" || val == "true";
      } else if (key == "fg_target_fps") {
        s.fg_target_fps = clamp_fg_adaptive_target_stored(std::atoi(val.c_str()));
      } else if (key == "fg_path") {
        s.fg_path = val;
      } else if (key == "enhance_renodx") s.enhance_renodx = (val != "0" && val != "false");
      else if (key == "neural_rendering") s.neural_rendering = (val != "0" && val != "false");
      else if (key == "renodx_preset") s.renodx_preset = clamp_renodx_visual_preset(std::atoi(val.c_str()));
      else if (key == "nr_pass_count") s.nr_pass_count = clamp_nr_pass_count(std::atoi(val.c_str()));
      else if (parse_nr_advanced_setting_key(key, val, s.nr_advanced)) {
      }
      else if (key == "video_fourcc") s.video_fourcc = clamp_video_fourcc(std::atoi(val.c_str()));
      else if (key == "color_space") s.color_space = clamp_color_space(std::atoi(val.c_str()));
      else if (key == "color_range") s.color_range = clamp_color_range(std::atoi(val.c_str()));
      else if (key == "capture_buffering") s.capture_buffering = clamp_capture_buffering(std::atoi(val.c_str()));
      else if (key == "capture_volume") s.capture_volume = clamp_capture_volume_percent(std::atoi(val.c_str()));
      else if (key == "capture_tone") s.capture_tone = clamp_capture_tone(std::atoi(val.c_str()));
      else if (key == "max_perf") s.max_perf = (val == "1" || val == "true");
      else if (key == "capture_source") s.capture_source = (val == "1" || val == "window" || val == "janela") ? 1 : 0;
      else if (key == "window_title") s.window_title = val;
      else if (key == "window_hwnd") s.window_hwnd = static_cast<uint64_t>(std::strtoull(val.c_str(), nullptr, 10));
      else if (key == "show_fps") s.ui.show_fps = (val == "1" || val == "true");
      else if (key == "advanced_metrics") s.ui.advanced_metrics = (val == "1" || val == "true");
      else if (key == "mini_overlay") s.ui.mini_overlay = (val == "1" || val == "true");
      else if (key == "overlay_no_bg") s.ui.overlay_no_bg = (val == "1" || val == "true");
      else if (key == "hw_info") s.ui.hw_info = (val == "1" || val == "true");
      else if (key == "fullscreen") s.ui.fullscreen = (val == "1" || val == "true");
      else if (key == "hide_panel_on_capture") s.ui.hide_panel_on_capture = true;
      else if (key == "sync_image") s.ui.sync_image = (val == "1" || val == "true");
      else if (key == "preview_mute") s.ui.preview_mute = (val == "1" || val == "true");
      else if (key == "preview_enabled") s.ui.preview_enabled = (val == "1" || val == "true");
      else if (key == "preview_cfg_live") s.ui.preview_cfg_live = (val == "1" || val == "true");
      else if (key == "bug_report") s.ui.bug_report = (val == "1" || val == "true");
      else if (key == "perf_report") s.ui.perf_report = (val == "1" || val == "true");
      else if (key == "live_overlay") s.ui.live_overlay = (val == "1" || val == "true");
      else if (key == "overlay_console") s.ui.overlay_console = (val == "1" || val == "true");
      else if (key == "live_console") s.ui.live_console = val.empty() ? "ps5" : val;
      // legacy auto_start ignored — lobby always opens first (maximized window)
    } else if (section == "hotkeys") {
      int v = std::atoi(val.c_str());
      if (key == "quit") s.hotkeys.vk_quit = v;
      else if (key == "hud") s.hotkeys.vk_hud = v;
      else if (key == "hud_ctrl") s.hotkeys.hud_ctrl = (val == "1" || val == "true");
      else if (key == "analyze") s.hotkeys.vk_analyze = v;
      else if (key == "dlss") s.hotkeys.vk_dlss = v;
      else if (key == "fg_toggle") s.hotkeys.vk_fg_toggle = v;
      else if (key == "fg_cycle") s.hotkeys.vk_fg_cycle = v;
      else if (key == "renodx") s.hotkeys.vk_renodx = v;
      else if (key == "nr_toggle") s.hotkeys.vk_nr_toggle = v;
      else if (key == "fullscreen") s.hotkeys.vk_fullscreen = v;
      else if (key == "gear") s.hotkeys.vk_gear = v;
      else if (key == "gear_ctrl") s.hotkeys.gear_ctrl = (val == "1" || val == "true");
      else if (key == "cfg_key") s.hotkeys.vk_cfg = v;
      else if (key == "cfg_ctrl") s.hotkeys.cfg_ctrl = (val == "1" || val == "true");
      else if (key == "pass_next") s.hotkeys.vk_pass_next = v;
      else if (key == "pass_next_ctrl") s.hotkeys.pass_next_ctrl = (val == "1" || val == "true");
      else if (key == "pass_prev") s.hotkeys.vk_pass_prev = v;
      else if (key == "pass_prev_ctrl") s.hotkeys.pass_prev_ctrl = (val == "1" || val == "true");
      else if (key == "look_next") s.hotkeys.vk_look_next = v;
      else if (key == "look_next_shift") s.hotkeys.look_next_shift = (val == "1" || val == "true");
      else if (key == "look_prev") s.hotkeys.vk_look_prev = v;
      else if (key == "look_prev_shift") s.hotkeys.look_prev_shift = (val == "1" || val == "true");
    }
  }
  s.ui.hide_panel_on_capture = true;
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
    case 0x20:
      return "Espaco";
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
    case 0xBB:
      return "+";
    case 0xBD:
      return "-";
    case 0x6B:
      return "Num +";
    case 0x6D:
      return "Num -";
    default:
      break;
  }
  // Letras e números (ex.: Ctrl+M do CFG na captura) — uma string fixa por tecla.
  {
    static char alnum[36][2];
    static bool alnum_ready = false;
    if (!alnum_ready) {
      for (int i = 0; i < 10; ++i) alnum[i][0] = static_cast<char>('0' + i);
      for (int i = 0; i < 26; ++i) alnum[10 + i][0] = static_cast<char>('A' + i);
      alnum_ready = true;
    }
    if (vk >= '0' && vk <= '9') return alnum[vk - '0'];
    if (vk >= 'A' && vk <= 'Z') return alnum[10 + (vk - 'A')];
  }
  return "Key";
}

bool sync_feed_work_resolution_file(const std::string& path, int scale_percent, int pass_count) {
  std::ifstream in(path);
  if (!in) return false;
  const std::string before((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  const std::string after = apply_feed_work_resolution(before, scale_percent, pass_count);
  return write_text_if_changed(path, before, after);
}

namespace {

std::vector<std::string> feed_cfg_candidates(const std::string& exe_dir) {
  return {
      exe_dir + "\\dlss5-feed.cfg",
      exe_dir + "\\addons-multipass\\dlss5-feed.cfg",
  };
}

template <typename Fn>
bool for_each_feed_cfg(const std::string& exe_dir, Fn&& fn) {
  bool any = false;
  for (const auto& path : feed_cfg_candidates(exe_dir)) {
    if (fn(path)) any = true;
  }
  return any;
}

}  // namespace

bool sync_feed_work_resolution_in_dir(const std::string& exe_dir, int scale_percent, int pass_count) {
  return for_each_feed_cfg(exe_dir, [&](const std::string& path) {
    return sync_feed_work_resolution_file(path, scale_percent, pass_count);
  });
}

bool sync_mgf_techniques_for_phase(const std::string& exe_dir, bool session_or_live_preview) {
  const std::string path = exe_dir + "\\DLSS5MGF.ini";
  std::ifstream in(path);
  if (!in) return false;
  const std::string before((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  const std::string after = apply_mgf_techniques_for_phase(before, session_or_live_preview);
  return write_text_if_changed(path, before, after);
}

bool sync_renodx_visual_preset_files(const std::string& exe_dir, int visual_preset, int pass_count,
                                      const NrAdvancedLook* look) {
  const int v = clamp_renodx_visual_preset(visual_preset);
  const int p = clamp_nr_pass_count(pass_count);
  bool any = for_each_feed_cfg(exe_dir, [&](const std::string& feed_path) {
    std::ifstream in(feed_path);
    if (!in) return false;
    const std::string before((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const std::string after = apply_renodx_visual_preset_to_feed(before, v, p);
    return write_text_if_changed(feed_path, before, after);
  });
  const std::string reshade_path = exe_dir + "\\ReShade.ini";
  {
    std::ifstream in(reshade_path);
    if (in) {
      const std::string before((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      in.close();
      const std::string after = apply_reshade_look_for_current_stack(before, v, p, look);
      if (write_text_if_changed(reshade_path, before, after)) any = true;
    }
  }
  return any;
}

RenodxAddonSyncResult sync_renodx_addon_variant(const std::string& exe_dir, int pass_count) {
  const int p = clamp_nr_pass_count(pass_count);
  const std::string stack_dir = exe_dir + "\\addons-multipass";
  if (!file_exists_utf8(stack_dir + "\\renodx-dlss5.addon64")) {
    return RenodxAddonSyncResult::MissingVariant;
  }

  const std::string reshade_path = exe_dir + "\\ReShade.ini";
  std::ifstream in(reshade_path);
  if (!in) return RenodxAddonSyncResult::WriteFailed;
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();

  const std::string want_path = renodx_addon_stack_rel(p);
  const bool same = parse_reshade_addon_path(text) == want_path;
  const std::string after = apply_reshade_addon_path(text, p);
  if (!write_text_if_changed(reshade_path, text, after)) return RenodxAddonSyncResult::WriteFailed;
  return same ? RenodxAddonSyncResult::OkSame : RenodxAddonSyncResult::OkSwapped;
}

bool relaunch_current_process(const char* extra_args) {
  wchar_t exe[MAX_PATH]{};
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH) || !exe[0]) return false;
  std::wstring cmd = L"\"";
  cmd += exe;
  cmd += L"\"";
  const std::string extra = portable_relaunch_args(extra_args, app_is_portable());
  if (!extra.empty()) {
    cmd += L' ';
    for (const char* p = extra.c_str(); *p; ++p) cmd += static_cast<wchar_t>(*p);  // ASCII flags only
  }
  std::vector<wchar_t> buf(cmd.begin(), cmd.end());
  buf.push_back(L'\0');
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  // Sem isto o Windows troca o ponteiro para o cursor de "abrindo" ate o processo
  // novo entrar no loop de mensagens. O boot do motor leva ~0,5 s: o loading aparece
  // em toda troca que reinicia.
  si.dwFlags = STARTF_FORCEOFFFEEDBACK;
  PROCESS_INFORMATION pi{};
  const BOOL ok =
      CreateProcessW(exe, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
  if (!ok) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

bool sync_nr_pass_count_files(const std::string& exe_dir, int pass_count, int visual_preset,
                              const NrAdvancedLook* look) {
  const int p = clamp_nr_pass_count(pass_count);
  const int v = clamp_renodx_visual_preset(visual_preset);
  (void)sync_renodx_addon_variant(exe_dir, p);
  const std::string reshade_path = exe_dir + "\\ReShade.ini";
  std::ifstream in(reshade_path);
  if (!in) return false;
  const std::string before((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  std::string after = apply_reshade_addon_path(before, p);
  after = apply_reshade_multipass_look_defaults(std::move(after), p, v, look);
  if (!write_text_if_changed(reshade_path, before, after)) return false;
  {
    const std::string mgf_path = exe_dir + "\\DLSS5MGF.ini";
    std::ifstream min(mgf_path);
    if (min) {
      const std::string mgf_before((std::istreambuf_iterator<char>(min)), std::istreambuf_iterator<char>());
      min.close();
      const std::string mgf_after = apply_mgf_sharpen_for_pass(mgf_before, p);
      (void)write_text_if_changed(mgf_path, mgf_before, mgf_after);
    }
  }
  // ini antigo do Krish gravava o feed 1x (Cinematic=0); o multipass usa o slot 0/1/2.
  (void)for_each_feed_cfg(exe_dir, [&](const std::string& feed_path) {
    std::ifstream fin(feed_path);
    if (!fin) return false;
    const std::string feed_before((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
    fin.close();
    const std::string feed_after = apply_renodx_visual_preset_to_feed(feed_before, v, p);
    return write_text_if_changed(feed_path, feed_before, feed_after);
  });
  if (!pipeline_dir_is_worker(exe_dir)) {
    const std::string worker = pipeline_worker_dir(exe_dir);
    CreateDirectoryA(worker.c_str(), nullptr);
    const std::string src_ini = exe_dir + "\\ReShade.ini";
    const std::string dst_ini = worker + "\\ReShade.ini";
    if (file_exists_utf8(src_ini) && !file_exists_utf8(dst_ini)) {
      CopyFileA(src_ini.c_str(), dst_ini.c_str(), FALSE);
    }
    const std::string src_mgf = exe_dir + "\\DLSS5MGF.ini";
    const std::string dst_mgf = worker + "\\DLSS5MGF.ini";
    if (file_exists_utf8(src_mgf) && !files_have_same_contents(src_mgf, dst_mgf)) {
      CopyFileA(src_mgf.c_str(), dst_mgf.c_str(), FALSE);
    }
    if (file_exists_utf8(dst_ini)) {
      (void)sync_nr_pass_count_files(worker, p, v, look);
    }
  }
  return true;
}

bool sync_neural_rendering_enabled_files(const std::string& exe_dir, bool nr_on, bool feed_on) {
  bool any = for_each_feed_cfg(exe_dir, [&](const std::string& feed_path) {
    std::ifstream in(feed_path);
    if (!in) return false;
    const std::string before((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const std::string after = apply_feed_enabled(before, feed_on);
    return write_text_if_changed(feed_path, before, after);
  });
  const std::string reshade_path = exe_dir + "\\ReShade.ini";
  {
    std::ifstream in(reshade_path);
    if (in) {
      const std::string before((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      in.close();
      const std::string after = apply_reshade_neural_uplift(before, nr_on);
      if (write_text_if_changed(reshade_path, before, after)) any = true;
    }
  }
  if (any && !pipeline_dir_is_worker(exe_dir)) {
    const std::string worker = pipeline_worker_dir(exe_dir);
    if (file_exists_utf8(worker + "\\ReShade.ini") || file_exists_utf8(worker + "\\dlss5-feed.cfg") ||
        file_exists_utf8(worker + "\\addons-multipass\\dlss5-feed.cfg")) {
      (void)sync_neural_rendering_enabled_files(worker, nr_on, feed_on);
    }
  }
  return any;
}

namespace {

constexpr std::uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

inline std::uint32_t sha256_rotr(std::uint32_t x, std::uint32_t n) {
  return (x >> n) | (x << (32u - n));
}

void sha256_transform(std::uint32_t h[8], const std::uint8_t block[64]) {
  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 = sha256_rotr(w[i - 15], 7) ^ sha256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 = sha256_rotr(w[i - 2], 17) ^ sha256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; ++i) {
    const std::uint32_t S1 = sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ ((~e) & g);
    const std::uint32_t t1 = hh + S1 + ch + kSha256K[i] + w[i];
    const std::uint32_t S0 = sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t t2 = S0 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

}  // namespace

std::string sha256_hex(const void* data, size_t n) {
  std::uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  const auto* p = static_cast<const std::uint8_t*>(data);
  std::uint8_t block[64]{};
  const std::uint64_t bit_len = static_cast<std::uint64_t>(n) * 8ull;
  size_t off = 0;
  while (off + 64 <= n) {
    sha256_transform(h, p + off);
    off += 64;
  }
  const size_t rem = n - off;
  if (rem) std::memcpy(block, p + off, rem);
  block[rem] = 0x80;
  if (rem >= 56) {
    sha256_transform(h, block);
    std::memset(block, 0, 64);
  }
  for (int i = 0; i < 8; ++i) {
    block[63 - i] = static_cast<std::uint8_t>((bit_len >> (8 * i)) & 0xffu);
  }
  sha256_transform(h, block);
  char hex[65]{};
  for (int i = 0; i < 8; ++i) {
    std::snprintf(hex + i * 8, 9, "%08x", h[i]);
  }
  return std::string(hex, 64);
}

std::string sha256_file_hex(const std::string& path, std::uint64_t* size_out) {
  if (size_out) *size_out = 0;
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::string buf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (size_out) *size_out = static_cast<std::uint64_t>(buf.size());
  return sha256_hex(buf.data(), buf.size());
}

BundledMotorProbe probe_bundled_motors(const std::string& exe_dir) {
  BundledMotorProbe p;
  const std::string stock_path = exe_dir + "\\" + motors_stock_filename();
  std::ifstream in(stock_path, std::ios::binary);
  if (!in) return p;
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto entries = parse_motor_stock_manifest(text);
  if (entries.empty()) return p;
  p.stock_ok = true;
  for (const auto& e : entries) {
    std::string rel = e.rel;
    for (char& c : rel) {
      if (c == '/') c = '\\';
    }
    std::uint64_t got_size = 0;
    const std::string sha = sha256_file_hex(exe_dir + "\\" + rel, &got_size);
    apply_motor_file_compare( !sha.empty(), motor_file_matches_stock(e.size, e.sha256, got_size, sha),
                              motor_stock_rel_is_stable(e.rel), motor_stock_rel_is_multipass(e.rel),
                              p.compared_any, p.stable_dirty, p.multipass_dirty);
  }
  return p;
}

}  // namespace nexuscap
