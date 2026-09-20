#include "nexuscap/lossless_fg.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

namespace nexuscap {
namespace {

std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 0) return L"";
  std::wstring w(static_cast<size_t>(n), 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  if (!w.empty() && w.back() == 0) w.pop_back();
  return w;
}

std::string wide_to_utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return {};
  std::string s(static_cast<size_t>(n), 0);
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
  if (!s.empty() && s.back() == 0) s.pop_back();
  return s;
}

std::string localappdata_settings_path() {
  wchar_t* buf = nullptr;
  size_t len = 0;
  if (_wdupenv_s(&buf, &len, L"LOCALAPPDATA") != 0 || !buf) return {};
  std::wstring p(buf);
  free(buf);
  p += L"\\Lossless Scaling\\Settings.xml";
  return wide_to_utf8(p);
}

std::string read_file_utf8(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool write_file_utf8_nobom(const std::string& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(out);
}

void replace_tag(std::string& xml, const std::string& tag, const std::string& value) {
  const std::string open = "<" + tag + ">";
  const std::string close = "</" + tag + ">";
  size_t pos = 0;
  while ((pos = xml.find(open, pos)) != std::string::npos) {
    const size_t start = pos + open.size();
    const size_t end = xml.find(close, start);
    if (end == std::string::npos) break;
    xml.replace(start, end - start, value);
    pos = start + value.size() + close.size();
  }
}

std::string steam_path_from_registry() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &key) != ERROR_SUCCESS) {
    return {};
  }
  wchar_t path[MAX_PATH]{};
  DWORD type = 0;
  DWORD size = sizeof(path);
  const LONG ok = RegQueryValueExW(key, L"SteamPath", nullptr, &type, reinterpret_cast<LPBYTE>(path), &size);
  RegCloseKey(key);
  if (ok != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
  std::wstring w(path);
  for (auto& c : w) {
    if (c == L'/') c = L'\\';
  }
  while (!w.empty() && (w.back() == L'\\' || w.back() == L'/')) w.pop_back();
  w += L"\\steamapps\\common\\Lossless Scaling\\LosslessScaling.exe";
  return wide_to_utf8(w);
}

BOOL CALLBACK enum_hide_ls(HWND hwnd, LPARAM) {
  wchar_t title[256]{};
  GetWindowTextW(hwnd, title, 256);
  wchar_t cls[128]{};
  GetClassNameW(hwnd, cls, 128);
  const unsigned ex = static_cast<unsigned>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
  RECT rc{};
  GetWindowRect(hwnd, &rc);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (ls_should_hide_window(title, cls, ex, w, h)) ShowWindow(hwnd, SW_HIDE);
  return TRUE;
}

}  // namespace

std::string default_steam_lossless_exe() {
  return "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Lossless Scaling\\LosslessScaling.exe";
}

bool lossless_exe_exists(const std::string& path) {
  if (path.empty()) return false;
  const DWORD attr = GetFileAttributesW(utf8_to_wide(path).c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string resolve_lossless_exe(const std::string& override_path) {
  if (!override_path.empty()) {
    if (lossless_exe_exists(override_path)) return override_path;
    return {};
  }
  const std::string def = default_steam_lossless_exe();
  if (lossless_exe_exists(def)) return def;
  const std::string reg = steam_path_from_registry();
  if (lossless_exe_exists(reg)) return reg;
  return {};
}

std::string build_mira_settings_xml(int multiplier) {
  multiplier = clamp_fg_multiplier(multiplier);
  if (multiplier < 2) multiplier = 2;  // profile template always stores a valid FG factor
  std::ostringstream o;
  o << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    << "<Settings xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
       "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\">\n"
    << "  <WindowMaximized>false</WindowMaximized>\n"
    << "  <Hotkey>F24</Hotkey>\n"
    << "  <HotkeyModifierKeys>None</HotkeyModifierKeys>\n"
    << "  <MinimizeToTray>true</MinimizeToTray>\n"
    << "  <CloseToTray>true</CloseToTray>\n"
    << "  <GameProfiles>\n"
    << "    <Profile>\n"
    << "      <Title>NexusCap</Title>\n"
    << "      <ScalingType>Off</ScalingType>\n"
    << "      <LSFG2Mode>X2</LSFG2Mode>\n"
    << "      <LSFG3Mode1>FIXED</LSFG3Mode1>\n"
    << "      <LSFG3Multiplier>" << multiplier << "</LSFG3Multiplier>\n"
    << "      <LSFG3Target>120</LSFG3Target>\n"
    << "      <LSFGFlowScale>100</LSFGFlowScale>\n"
    << "      <LSFGSize>BALANCED</LSFGSize>\n"
    << "      <FrameGeneration>LSFG3</FrameGeneration>\n"
    << "      <SyncMode>VSYNC1</SyncMode>\n"
    << "      <GsyncSupport>false</GsyncSupport>\n"
      << "      <DrawFps>false</DrawFps>\n"
    << "      <CaptureApi>WGC</CaptureApi>\n"
    << "      <QueueTarget>1</QueueTarget>\n"
    << "    </Profile>\n"
    << "  </GameProfiles>\n"
    << "</Settings>\n";
  return o.str();
}

std::string patch_mira_settings_xml(const std::string& xml_in, int multiplier) {
  multiplier = clamp_fg_multiplier(multiplier);
  if (multiplier < 2) multiplier = 2;
  std::string xml = xml_in.empty() ? build_mira_settings_xml(multiplier) : xml_in;

  // Kill legacy Flow 70/90 from X2 before writing 100.
  while (xml.find("<LSFGFlowScale>70</LSFGFlowScale>") != std::string::npos) {
    replace_tag(xml, "LSFGFlowScale", "100");
  }
  while (xml.find("<LSFGFlowScale>90</LSFGFlowScale>") != std::string::npos) {
    replace_tag(xml, "LSFGFlowScale", "100");
  }

  replace_tag(xml, "Hotkey", "F24");
  replace_tag(xml, "HotkeyModifierKeys", "None");
  replace_tag(xml, "LSFG3Mode1", "FIXED");
  replace_tag(xml, "LSFG3Multiplier", std::to_string(multiplier));
  replace_tag(xml, "LSFGFlowScale", "100");
  replace_tag(xml, "LSFGSize", "BALANCED");
  replace_tag(xml, "FrameGeneration", "LSFG3");
  replace_tag(xml, "SyncMode", "VSYNC1");
  replace_tag(xml, "GsyncSupport", "false");
  replace_tag(xml, "DrawFps", "false");
  if (xml.find("<DrawFps>") == std::string::npos) {
    const size_t at = xml.find("<CaptureApi>");
    if (at != std::string::npos) xml.insert(at, "<DrawFps>false</DrawFps>");
  }
  replace_tag(xml, "CaptureApi", "WGC");
  replace_tag(xml, "QueueTarget", "1");

  if (xml.find("<LSFGFlowScale>") == std::string::npos) {
    // Incomplete file — rebuild.
    return build_mira_settings_xml(multiplier);
  }
  if (xml.find(">70</LSFGFlowScale>") != std::string::npos) {
    replace_tag(xml, "LSFGFlowScale", "100");
  }
  return xml;
}

std::string patch_lsfg_ini(const std::string& ini_in) {
  const char* good = "[rendering]\r\n"
                     "flush = 1\r\n"
                     "\r\n"
                     "[capture]\r\n"
                     "frametime_buffer_size = 15\r\n"
                     "frametime_buffer_reset_multiplier = 6\r\n"
                     "queue_draining_momentum = 0.01\r\n"
                     "\r\n"
                     "[lsfg]\r\n"
                     "ui_detection_rate = 2\r\n"
                     "ui_detection_threshold = 0.30\r\n"
                     "flow_scale = 100\r\n"
                     "real_timestamp_tolerance = 0.05\r\n"
                     "base_framerate_threshold = 10\r\n"
                     "\r\n"
                     "[fps_counter]\r\n"
                     "scale = 0\r\n"
                     "color = 0xD3D3D3\r\n"
                     "font = Consolas\r\n"
                     "offset_x = 0\r\n"
                     "offset_y = 0\r\n"
                     "show_captured = 0\r\n";
  if (ini_in.empty()) return good;

  std::string out = ini_in;
  auto set_kv = [&](const char* key, const char* value) {
    const std::string k = std::string(key) + " = ";
    size_t pos = out.find(key);
    if (pos == std::string::npos) return;
    size_t line_end = out.find_first_of("\r\n", pos);
    if (line_end == std::string::npos) line_end = out.size();
    out.replace(pos, line_end - pos, k + value);
  };
  if (out.find("[lsfg]") == std::string::npos) return good;
  set_kv("ui_detection_rate", "2");
  set_kv("ui_detection_threshold", "0.30");
  set_kv("flow_scale", "100");
  // If keys missing, rewrite whole file.
  if (out.find("flow_scale = 100") == std::string::npos ||
      out.find("ui_detection_rate = 2") == std::string::npos) {
    return good;
  }
  // Medidor do LS: some o chip mesmo se o XML ainda tiver DrawFps true.
  const size_t fps_sec = out.find("[fps_counter]");
  const char* hidden = "[fps_counter]\r\n"
                       "scale = 0\r\n"
                       "color = 0xD3D3D3\r\n"
                       "font = Consolas\r\n"
                       "offset_x = 0\r\n"
                       "offset_y = 0\r\n"
                       "show_captured = 0\r\n";
  if (fps_sec == std::string::npos) {
    if (!out.empty() && out.back() != '\n') out += "\r\n";
    out += "\r\n";
    out += hidden;
  } else {
    size_t next = out.find("\n[", fps_sec + 1);
    if (next == std::string::npos) {
      out.replace(fps_sec, out.size() - fps_sec, hidden);
    } else {
      out.replace(fps_sec, next + 1 - fps_sec, std::string(hidden) + "\r\n");
    }
  }
  return out;
}

void LosslessFgController::configure(int multiplier, const std::string& exe_override) {
  state_.multiplier = clamp_fg_multiplier(multiplier);
  state_.flow_scale = 100;
  state_.capture_api = "WGC";
  state_.backend = "lsfg3";
  state_.exe_path = resolve_lossless_exe(exe_override);
  state_.last_error.clear();
  if (state_.multiplier <= 1) {
    state_.status = FgStatus::Off;
    state_.active = false;
    confirmed_on_ = false;
  } else if (state_.exe_path.empty()) {
    state_.status = FgStatus::Missing;
    state_.last_error = "LosslessScaling.exe not found (Steam path)";
  } else {
    state_.status = state_.active ? FgStatus::On : FgStatus::Ready;
  }
  configured_ = true;
}

bool LosslessFgController::write_settings_file(const std::string& xml, std::string& err) {
  const std::string path = localappdata_settings_path();
  if (path.empty()) {
    err = "LOCALAPPDATA missing";
    return false;
  }
  // Ensure directory exists.
  {
    const auto slash = path.find_last_of("\\/");
    if (slash != std::string::npos) {
      const std::wstring dir = utf8_to_wide(path.substr(0, slash));
      CreateDirectoryW(dir.c_str(), nullptr);
    }
  }
  const std::string bak = path + ".nexuscap.bak";
  if (!lossless_exe_exists(bak) && lossless_exe_exists(path)) {
    // Backup only once — use CopyFile for existing settings.
    CopyFileW(utf8_to_wide(path).c_str(), utf8_to_wide(bak).c_str(), TRUE);
  }
  SetFileAttributesW(utf8_to_wide(path).c_str(), FILE_ATTRIBUTE_NORMAL);
  if (!write_file_utf8_nobom(path, xml)) {
    err = "failed to write Settings.xml";
    return false;
  }
  // LS no save-on-exit regrava DrawFps=true. Trava o arquivo na nossa mira.
  SetFileAttributesW(utf8_to_wide(path).c_str(), FILE_ATTRIBUTE_READONLY);
  return true;
}

bool LosslessFgController::write_ini_alongside_exe(std::string& err) {
  if (state_.exe_path.empty()) return true;
  const auto slash = state_.exe_path.find_last_of("\\/");
  if (slash == std::string::npos) return true;
  const std::string ini_path = state_.exe_path.substr(0, slash + 1) + "config.ini";
  std::string cur = read_file_utf8(ini_path);
  const std::string patched = patch_lsfg_ini(cur);
  // May be read-only (keep-flow90); clear RO then write.
  SetFileAttributesW(utf8_to_wide(ini_path).c_str(), FILE_ATTRIBUTE_NORMAL);
  if (!write_file_utf8_nobom(ini_path, patched)) {
    err = "failed to write LS config.ini (continuing)";
    // Non-fatal — Settings.xml is enough for most fields.
    return true;
  }
  return true;
}

bool LosslessFgController::kill_all_lossless_and_wait(DWORD timeout_ms) {
  const DWORD cap = timeout_ms > 0 ? timeout_ms : static_cast<DWORD>(fg_kill_wait_max_ms());
  terminate_extra_lossless_processes(0);
  const DWORD t0 = GetTickCount();
  for (;;) {
    bool snap_ok = true;
    const unsigned n = count_running_lossless_processes(true, &snap_ok);
    if (fg_leftover_cleared(n, snap_ok)) {
      if (proc_) {
        CloseHandle(proc_);
        proc_ = nullptr;
      }
      if (job_) {
        CloseHandle(job_);
        job_ = nullptr;
      }
      state_.pid = 0;
      state_.active = false;
      confirmed_on_ = false;
      return true;
    }
    if (GetTickCount() - t0 >= cap) {
      terminate_extra_lossless_processes(0);
      bool ok2 = true;
      const unsigned left = count_running_lossless_processes(true, &ok2);
      return fg_leftover_cleared(left, ok2);
    }
    Sleep(static_cast<DWORD>(fg_kill_wait_step_ms()));
  }
}

void LosslessFgController::pulse_fg_hotkey() { (void)send_ls_hotkey(); }

bool LosslessFgController::apply_hdmi_profile_without_launch(bool fg_enabled, int fg_mult,
                                                             const std::string& path, std::string& err) {
  if (confirmed_on_ || state_.active) pulse_fg_hotkey();
  (void)kill_all_lossless_and_wait(static_cast<DWORD>(fg_kill_wait_max_ms()));
  configure(hdmi_lobby_fg_multiplier(fg_enabled, fg_mult), path);
  const bool ok = apply_profile(err);
  bool snap_ok = true;
  const unsigned left = count_running_lossless_processes(true, &snap_ok);
  if (!fg_leftover_cleared(left, snap_ok)) {
    if (!err.empty()) err += "; ";
    err += "Lossless Scaling ainda vivo apos terminate (" + std::to_string(left) + ")";
    return false;
  }
  return ok;
}

bool LosslessFgController::apply_profile(std::string& err) {
  if (!configured_) {
    err = "not configured";
    return false;
  }
  const int m = state_.multiplier <= 1 ? 2 : state_.multiplier;
  const std::string path = localappdata_settings_path();
  std::string cur = path.empty() ? std::string() : read_file_utf8(path);
  const std::string xml = patch_mira_settings_xml(cur, m);
  if (xml.find("LSFGFlowScale>100") == std::string::npos) {
    err = "mira profile missing FlowScale 100";
    state_.status = FgStatus::Error;
    return false;
  }
  if (xml.find("LSFGFlowScale>70") != std::string::npos) {
    err = "refusing FlowScale 70";
    state_.status = FgStatus::Error;
    return false;
  }
  if (!write_settings_file(xml, err)) {
    state_.status = FgStatus::Error;
    return false;
  }
  // Read-back: HUD xN must match disk (QA: no memory-only multiplier).
  const std::string on_disk = path.empty() ? std::string() : read_file_utf8(path);
  if (!fg_multiplier_confirmed_on_disk(m, on_disk)) {
    err = "Settings.xml LSFG3Multiplier != " + std::to_string(m) + " apos gravar";
    state_.status = FgStatus::Error;
    state_.last_error = err;
    return false;
  }
  if (!ls_xml_draw_fps_off(on_disk)) {
    err = "Settings.xml DrawFps ainda true apos gravar";
    state_.status = FgStatus::Error;
    state_.last_error = err;
    return false;
  }
  write_ini_alongside_exe(err);
  state_.flow_scale = 100;
  state_.capture_api = "WGC";
  if (state_.multiplier <= 1) {
    state_.status = FgStatus::Off;
  } else if (state_.exe_path.empty()) {
    state_.status = FgStatus::Missing;
  } else if (!state_.active) {
    state_.status = FgStatus::Ready;
  }
  return true;
}

bool LosslessFgController::ensure_process(std::string& err, bool allow_adopt) {
  if (!fg_wants_process(state_.multiplier)) {
    state_.status = FgStatus::Off;
    return true;
  }
  if (state_.exe_path.empty()) {
    state_.exe_path = resolve_lossless_exe({});
  }
  if (state_.exe_path.empty()) {
    err = "LosslessScaling.exe missing";
    state_.status = FgStatus::Missing;
    return false;
  }
  if (proc_) {
    DWORD code = 0;
    if (GetExitCodeProcess(proc_, &code) && code == STILL_ACTIVE) {
      // Drop any second LS that appeared (Steam auto-start, old session, etc.).
      terminate_extra_lossless_processes(state_.pid);
      state_.status = state_.active ? FgStatus::On : FgStatus::Ready;
      return true;
    }
    CloseHandle(proc_);
    proc_ = nullptr;
    state_.pid = 0;
  }

  if (!apply_profile(err)) return false;

  // Prefer a single already-running LS instead of launching a duplicate —
  // except after multiplier change (stale RAM profile vs new XML).
  if (allow_adopt && adopt_existing_lossless(err)) {
    terminate_extra_lossless_processes(state_.pid);
    hide_ls_windows();
    return true;
  }
  if (!allow_adopt) {
    terminate_extra_lossless_processes(0);  // kill every LS so next CreateProcess reloads XML
  }

  if (!job_) {
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (!job_) {
      err = "CreateJobObject failed";
      state_.status = FgStatus::Error;
      return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
      err = "SetInformationJobObject failed";
      state_.status = FgStatus::Error;
      return false;
    }
  }

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_SHOWMINNOACTIVE;
  PROCESS_INFORMATION pi{};
  std::wstring cmd = L"\"" + utf8_to_wide(state_.exe_path) + L"\"";
  std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
  mutable_cmd.push_back(0);

  const std::wstring workdir = utf8_to_wide(state_.exe_path.substr(0, state_.exe_path.find_last_of("\\/")));

  if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP,
                      nullptr, workdir.empty() ? nullptr : workdir.c_str(), &si, &pi)) {
    err = "CreateProcess LosslessScaling failed";
    state_.status = FgStatus::Error;
    return false;
  }
  if (!AssignProcessToJobObject(job_, pi.hProcess)) {
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    err = "AssignProcessToJobObject failed";
    state_.status = FgStatus::Error;
    return false;
  }
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  proc_ = pi.hProcess;
  state_.pid = pi.dwProcessId;
  state_.status = FgStatus::Ready;
  state_.active = false;
  // No Sleep here — session tick() waits so HDMI Present never freezes.
  hide_ls_windows();
  terminate_extra_lossless_processes(state_.pid);
  return true;
}

unsigned LosslessFgController::count_running_lossless_processes(bool force, bool* snapshot_ok) {
  // Throttle Toolhelp — overlay + reconcile called every Present otherwise.
  static unsigned cached = 0;
  static DWORD last_tick = 0;
  const DWORD now = GetTickCount();
  if (!force && last_tick != 0 && (now - last_tick) < 200) {
    if (snapshot_ok) *snapshot_ok = true;
    return cached;
  }
  last_tick = now;

  unsigned n = 0;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    if (snapshot_ok) *snapshot_ok = false;
    return cached;  // falha do snapshot nao mente FG off
  }
  if (snapshot_ok) *snapshot_ok = true;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      if (ls_exe_is_lossless(pe.szExeFile)) ++n;
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  cached = n;
  return n;
}

void LosslessFgController::terminate_extra_lossless_processes(DWORD keep_pid) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      if (!ls_exe_is_lossless(pe.szExeFile)) continue;
      if (keep_pid != 0 && pe.th32ProcessID == keep_pid) continue;
      HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
      if (h) {
        TerminateProcess(h, 0);
        CloseHandle(h);
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
}

bool LosslessFgController::adopt_existing_lossless(std::string& err) {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  DWORD found = 0;
  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  if (Process32FirstW(snap, &pe)) {
    do {
      if (ls_exe_is_lossless(pe.szExeFile)) {
        found = pe.th32ProcessID;
        break;
      }
    } while (Process32NextW(snap, &pe));
  }
  CloseHandle(snap);
  if (!found) return false;

  HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, found);
  if (!h) {
    err = "OpenProcess LosslessScaling failed";
    return false;
  }
  if (job_) {
    // Best-effort: foreign process may refuse job assignment.
    AssignProcessToJobObject(job_, h);
  }
  proc_ = h;
  state_.pid = found;
  state_.active = false;  // never assume FG already on our window
  state_.status = FgStatus::Ready;
  err.clear();
  return true;
}

bool LosslessFgController::request_toggle(HWND target, std::string& err) {
  if (!configured_) {
    err = "not configured";
    return false;
  }
  if (pending_ != FgPending::None) {
    err = "fg op already pending";
    return false;
  }
  if (state_.multiplier <= 1) state_.multiplier = 2;
  pending_hwnd_ = target;
  pending_finished_ok_ = false;
  if (!state_.active) {
    if (state_.exe_path.empty()) state_.exe_path = resolve_lossless_exe({});
    if (state_.exe_path.empty()) {
      err = "LosslessScaling.exe missing";
      state_.status = FgStatus::Missing;
      state_.last_error = err;
      return false;
    }
    // LS no exit grava DrawFps=true por cima do XML. Espera morrer, so depois escreve e lanca.
    pending_want_mult_ = state_.multiplier;
    if (proc_) shutdown(/*wait_ms=*/50);
    terminate_extra_lossless_processes(0);
    pending_ = FgPending::OnWaitDead;
    pending_due_ms_ = -80;
    pending_retries_ = 0;
    return true;
  }
  if (proc_) {
    focus_window(target);
    send_ls_hotkey();
  }
  pending_ = FgPending::OffAfterHotkey;
  pending_due_ms_ = -50;
  pending_retries_ = 0;
  return true;
}

bool LosslessFgController::request_set_multiplier(int multiplier, HWND target, std::string& err) {
  if (pending_ != FgPending::None) {
    err = "fg op already pending";
    return false;
  }
  const int m = clamp_fg_multiplier(multiplier);
  pending_prev_mult_ = state_.multiplier;
  pending_want_mult_ = m;
  pending_was_active_ = state_.active;
  pending_hwnd_ = target;
  pending_finished_ok_ = false;
  if (m <= 1) {
    state_.multiplier = 1;
    state_.active = false;
    state_.status = FgStatus::Off;
    return true;
  }
  if (state_.exe_path.empty()) state_.exe_path = resolve_lossless_exe({});
  if (state_.exe_path.empty()) {
    state_.status = FgStatus::Missing;
    err = "LosslessScaling.exe missing";
    return false;
  }
  // If FG on, release window first (hotkey) without blocking kill wait.
  if (pending_was_active_ && target) {
    focus_window(target);
    send_ls_hotkey();
    state_.active = false;
  }
  state_.multiplier = m;
  shutdown(/*wait_ms=*/50);
  terminate_extra_lossless_processes(0);
  pending_ = FgPending::MultWaitDead;
  pending_due_ms_ = -80;
  pending_retries_ = 0;
  return true;
}

bool LosslessFgController::request_cycle_multiplier(HWND target, std::string& err) {
  return request_set_multiplier(next_fg_multiplier(state_.multiplier), target, err);
}

bool LosslessFgController::tick(double now_ms) {
  if (pending_ == FgPending::None) return false;
  // Negative due = delay ms from first tick.
  if (pending_due_ms_ < 0) {
    pending_due_ms_ = now_ms + (-pending_due_ms_);
  }
  if (now_ms < pending_due_ms_) return false;

  auto fail = [&](const std::string& e) {
    state_.last_error = e;
    state_.status = FgStatus::Error;
    pending_ = FgPending::None;
    pending_finished_ok_ = false;
    return true;
  };
  auto ok_done = [&]() {
    pending_ = FgPending::None;
    pending_finished_ok_ = true;
    state_.last_error.clear();
    return true;
  };

  auto launch_fresh = [&]() -> bool {
    std::string e;
    if (pending_ == FgPending::MultWaitDead && pending_want_mult_ >= 2) {
      state_.multiplier = pending_want_mult_;
      configure(pending_want_mult_, state_.exe_path);
    }
    if (!apply_profile(e)) {
      fail(e);
      return false;
    }
    if (!ensure_process(e, /*allow_adopt=*/false)) {
      fail(e);
      return false;
    }
    return true;
  };

  switch (pending_) {
    case FgPending::OnWaitDead:
    case FgPending::MultWaitDead:
      if (count_running_lossless_processes(true) > 0) {
        terminate_extra_lossless_processes(0);
        if (pending_retries_ < 12) {
          ++pending_retries_;
          pending_due_ms_ = now_ms + 80.0;
          return false;
        }
      }
      if (!launch_fresh()) return true;
      pending_ = (pending_ == FgPending::OnWaitDead) ? FgPending::OnWaitLaunch : FgPending::MultWaitLaunch;
      pending_due_ms_ = now_ms + 350.0;  // LS ler o XML (DrawFps false) antes do F24
      return false;
    case FgPending::OnWaitLaunch: {
      std::string stamp;
      (void)apply_profile(stamp);  // tapa save-on-exit do LS velho
      hide_ls_windows();
      pending_ = FgPending::OnSendHotkey;
      pending_due_ms_ = now_ms;  // immediate next tick
      return false;
    }
    case FgPending::OnSendHotkey:
      if (!focus_window(pending_hwnd_)) return fail("failed to focus NexusCap window");
      hide_ls_windows();
      if (!send_ls_hotkey()) return fail("SendInput F24 failed");
      pending_ = FgPending::OnConfirm;
      pending_due_ms_ = now_ms + 120.0;
      return false;
    case FgPending::OnConfirm:
      if (count_running_lossless_processes(true) == 0) {
        return fail("LosslessScaling exited after hotkey (FG nao confirmado)");
      }
      state_.active = true;
      state_.status = FgStatus::On;
      confirmed_on_ = true;
      return ok_done();
    case FgPending::OffAfterHotkey:
      shutdown(/*wait_ms=*/50);  // don't block Present for 800ms
      pending_ = FgPending::OffConfirmDead;
      pending_due_ms_ = now_ms + 100.0;
      pending_retries_ = 0;
      return false;
    case FgPending::OffConfirmDead:
      if (count_running_lossless_processes(true) > 0) {
        terminate_extra_lossless_processes(0);
        if (pending_retries_ < 3) {
          ++pending_retries_;
          pending_due_ms_ = now_ms + 120.0;
          return false;
        }
        state_.active = true;
        return fail("LosslessScaling ainda rodando apos FG off");
      }
      state_.active = false;
      state_.status = FgStatus::Ready;
      confirmed_on_ = false;
      return ok_done();
    case FgPending::MultWaitLaunch:
      hide_ls_windows();
      pending_ = FgPending::MultAfterLaunch;
      pending_due_ms_ = now_ms;
      return false;
    case FgPending::MultAfterLaunch:
      if (pending_was_active_ && pending_hwnd_) {
        pending_ = FgPending::OnSendHotkey;
        pending_due_ms_ = now_ms;
        return false;
      }
      state_.status = FgStatus::Ready;
      return ok_done();
    default:
      pending_ = FgPending::None;
      return false;
  }
}

bool LosslessFgController::run_sync_until_idle(std::string& err) {
  // Selftests only — pump tick with Sleep(1), not used in session Present loop.
  LARGE_INTEGER freq{}, qpc{};
  QueryPerformanceFrequency(&freq);
  auto now_ms = [&]() {
    QueryPerformanceCounter(&qpc);
    return 1000.0 * static_cast<double>(qpc.QuadPart) / static_cast<double>(freq.QuadPart);
  };
  const double deadline = now_ms() + 5000.0;
  while (busy() && now_ms() < deadline) {
    tick(now_ms());
    Sleep(1);
  }
  if (busy()) {
    err = "fg async timeout";
    pending_ = FgPending::None;
    return false;
  }
  if (!pending_finished_ok_ && !state_.last_error.empty()) {
    err = state_.last_error;
    return false;
  }
  err.clear();
  return true;
}

bool LosslessFgController::toggle(HWND target, std::string& err) {
  if (!request_toggle(target, err)) return false;
  if (!busy()) return true;
  return run_sync_until_idle(err);
}

bool LosslessFgController::set_multiplier(int multiplier, HWND target, std::string& err) {
  if (!request_set_multiplier(multiplier, target, err)) return false;
  if (!busy()) return true;
  return run_sync_until_idle(err);
}

bool LosslessFgController::cycle_multiplier(HWND target, std::string& err) {
  return set_multiplier(next_fg_multiplier(state_.multiplier), target, err);
}

void LosslessFgController::hide_ls_windows() { EnumWindows(enum_hide_ls, 0); }

bool LosslessFgController::focus_window(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) return false;
  ShowWindow(hwnd, SW_SHOW);
  SetForegroundWindow(hwnd);
  BringWindowToTop(hwnd);
  return true;
}

bool LosslessFgController::send_ls_hotkey() {
  // F24 = VK 0x87 — configured in Settings.xml Hotkey.
  INPUT inputs[4]{};
  inputs[0].type = INPUT_KEYBOARD;
  inputs[0].ki.wVk = VK_F24;
  inputs[1].type = INPUT_KEYBOARD;
  inputs[1].ki.wVk = VK_F24;
  inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
  const UINT sent = SendInput(2, inputs, sizeof(INPUT));
  // Keyup extra — se o down ficou preso, a tecla nao trava o Benchmark.
  INPUT up{};
  up.type = INPUT_KEYBOARD;
  up.ki.wVk = VK_F24;
  up.ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(1, &up, sizeof(INPUT));
  return sent == 2;
}

void LosslessFgController::shutdown(DWORD wait_ms) {
  if (proc_) {
    // Prefer graceful close of main window, then terminate via job.
    DWORD pid = state_.pid;
    if (pid) {
      EnumWindows(
          [](HWND hwnd, LPARAM lp) -> BOOL {
            DWORD wpid = 0;
            GetWindowThreadProcessId(hwnd, &wpid);
            if (wpid == static_cast<DWORD>(lp)) PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return TRUE;
          },
          static_cast<LPARAM>(pid));
      if (wait_ms > 0) WaitForSingleObject(proc_, wait_ms);
    }
    CloseHandle(proc_);
    proc_ = nullptr;
  }
  if (job_) {
    CloseHandle(job_);  // KILL_ON_JOB_CLOSE
    job_ = nullptr;
  }
  state_.pid = 0;
  state_.active = false;
  confirmed_on_ = false;
  if (state_.status == FgStatus::On || state_.status == FgStatus::Ready) {
    state_.status = state_.multiplier <= 1 ? FgStatus::Off
                                           : (state_.exe_path.empty() ? FgStatus::Missing : FgStatus::Ready);
  }
}

bool LosslessFgController::owned_process_alive() const {
  if (!proc_) return false;
  DWORD code = 0;
  if (!GetExitCodeProcess(proc_, &code)) return false;
  return code == STILL_ACTIVE;
}

bool LosslessFgController::overlay_reads_on() const {
  return fg_overlay_reads_on(state_.active, state_.multiplier, owned_process_alive(),
                             count_running_lossless_processes(true), confirmed_on_);
}

void LosslessFgController::reconcile_process() {
  if (confirmed_on_) return;  // Toolhelp 0 depois do F24 mentia FG off no x2
  if (!state_.active) return;
  if (owned_process_alive()) return;
  bool snap_ok = true;
  const unsigned running = count_running_lossless_processes(true, &snap_ok);
  if (!snap_ok) return;
  if (running > 0) return;
  state_.active = false;
  state_.pid = 0;
  state_.status = state_.exe_path.empty() ? FgStatus::Missing : FgStatus::Ready;
  state_.last_error = "LosslessScaling saiu — FG desmarcado no overlay";
}

}  // namespace nexuscap
