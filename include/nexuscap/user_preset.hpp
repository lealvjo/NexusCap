#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "nexuscap/live_mode.hpp"
#include "nexuscap/lossless_fg.hpp"
#include "nexuscap/session_cfg.hpp"
#include "nexuscap/settings_store.hpp"

namespace nexuscap {

inline constexpr int kUserPresetNameMax = 50;
inline constexpr int kUserPresetNameBuf = kUserPresetNameMax + 1;
inline constexpr int kUserPresetMaxCount = 32;
inline constexpr int kUserPresetFormat = 1;
inline constexpr const char* kUserPresetMagicKey = "nexuscap-preset";
inline constexpr const char* kUserPresetExtShare = ".nexuscap-preset";
inline constexpr const char* kUserPresetExtStore = ".ini";

struct UserPreset {
  std::string name;
  bool hud_visible = true;
  bool overlay_no_bg = false;
  bool overlay_console = false;
  int live_console = 0;
  bool neural_rendering = true;
  int renodx_preset = 0;
  int nr_pass_count = 1;
  bool fg_enabled = false;
  int fg_multiplier = 2;
  int scale_percent = 100;
};

inline bool user_preset_io_allowed(bool perf_test) { return !perf_test; }

inline std::string trim_user_preset_name(std::string s) {
  size_t a = 0;
  while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
  size_t b = s.size();
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
  return s.substr(a, b - a);
}

inline bool user_preset_char_forbidden(unsigned char c) {
  return c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
         c == '|' || c == '\n' || c == '\r';
}

inline std::string sanitize_user_preset_chars(std::string s) {
  for (char& c : s) {
    if (user_preset_char_forbidden(static_cast<unsigned char>(c))) c = '-';
  }
  return s;
}

inline bool user_preset_names_equal(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const int ca = std::tolower(static_cast<unsigned char>(a[i]));
    const int cb = std::tolower(static_cast<unsigned char>(b[i]));
    if (ca != cb) return false;
  }
  return true;
}

enum class UserPresetNameStatus { Ok, Empty, TooLong, Forbidden };

inline bool user_preset_name_has_forbidden(const std::string& s) {
  for (unsigned char c : s) {
    if (user_preset_char_forbidden(c)) return true;
  }
  return false;
}

inline UserPresetNameStatus normalize_user_preset_name(const std::string& raw, std::string& out) {
  out = trim_user_preset_name(raw);
  if (out.empty()) return UserPresetNameStatus::Empty;
  if (user_preset_name_has_forbidden(out)) {
    out.clear();
    return UserPresetNameStatus::Forbidden;
  }
  if (static_cast<int>(out.size()) > kUserPresetNameMax) {
    out.clear();
    return UserPresetNameStatus::TooLong;
  }
  return UserPresetNameStatus::Ok;
}

inline const char* user_preset_name_status_toast(UserPresetNameStatus s) {
  switch (s) {
    case UserPresetNameStatus::Empty: {
      static char buf[72];
      std::snprintf(buf, sizeof(buf), "Nome invalido (1 a %d caracteres)", kUserPresetNameMax);
      return buf;
    }
    case UserPresetNameStatus::TooLong: {
      static char buf[72];
      std::snprintf(buf, sizeof(buf), "Nome invalido (no maximo %d caracteres)", kUserPresetNameMax);
      return buf;
    }
    case UserPresetNameStatus::Forbidden:
      return "Nome invalido (sem \\ / : * ? \" < > |)";
    default:
      return "";
  }
}

inline const char* user_preset_name_field_hint() {
  static char buf[80];
  std::snprintf(buf, sizeof(buf), "1 a %d caracteres. Sem \\ / : * ? \" < > |", kUserPresetNameMax);
  return buf;
}

inline int user_preset_name_typed_len(const char* buf) {
  return buf ? static_cast<int>(std::strlen(buf)) : 0;
}

inline int user_preset_name_filter_event(unsigned int event_char, int buf_text_len) {
  if (event_char <= 127 && user_preset_char_forbidden(static_cast<unsigned char>(event_char))) return 1;
  if (buf_text_len >= kUserPresetNameMax) return 1;
  return 0;
}

inline bool user_preset_name_at_limit(const char* buf) {
  return user_preset_name_typed_len(buf) >= kUserPresetNameMax;
}

inline std::string user_preset_name_limit_hint(const char* buf) {
  const int n = user_preset_name_typed_len(buf);
  char line[96]{};
  if (n >= kUserPresetNameMax) {
    std::snprintf(line, sizeof(line), "%d/%d  limite de caracteres", kUserPresetNameMax, kUserPresetNameMax);
  } else {
    std::snprintf(line, sizeof(line), "%d/%d caracteres. Sem \\ / : * ? \" < > |", n, kUserPresetNameMax);
  }
  return line;
}

inline void clamp_user_preset(UserPreset& p) {
  std::string n;
  if (normalize_user_preset_name(p.name, n) == UserPresetNameStatus::Ok) p.name = n;
  p.live_console = static_cast<int>(clamp_live_console(p.live_console));
  p.renodx_preset = clamp_renodx_visual_preset(p.renodx_preset);
  p.nr_pass_count = clamp_nr_pass_count(p.nr_pass_count);
  p.fg_multiplier = clamp_fg_multiplier(p.fg_multiplier >= 2 ? p.fg_multiplier : 2);
  p.scale_percent = clamp_scale_percent(p.scale_percent);
}

inline std::string user_preset_list_summary(const UserPreset& p) {
  char b[72]{};
  const char* look = renodx_visual_preset_name(p.renodx_preset, p.nr_pass_count);
  if (p.fg_enabled) {
    std::snprintf(b, sizeof(b), "%s  %s  FG x%d  %d%%", nr_pass_count_short(p.nr_pass_count), look,
                  clamp_fg_multiplier(p.fg_multiplier), clamp_scale_percent(p.scale_percent));
  } else {
    std::snprintf(b, sizeof(b), "%s  %s  FG off  %d%%", nr_pass_count_short(p.nr_pass_count), look,
                  clamp_scale_percent(p.scale_percent));
  }
  return b;
}

inline constexpr float kUserPresetMarqueeGapPx = 36.f;
inline constexpr float kUserPresetMarqueePxPerSec = 42.f;

// 0 = nome cabe, sem rolagem. Senao offset em pixels para o loop.
inline float user_preset_marquee_offset(float text_w, float avail, double t_sec) {
  if (text_w <= avail + 0.5f) return 0.f;
  const float loop = text_w + kUserPresetMarqueeGapPx;
  if (loop < 1.f) return 0.f;
  float x = std::fmod(static_cast<float>(t_sec) * kUserPresetMarqueePxPerSec, loop);
  if (x < 0.f) x += loop;
  return x;
}

inline UserPreset user_preset_from_persisted(const std::string& name, const PersistedSettings& s) {
  UserPreset p;
  p.name = name;
  p.hud_visible = s.ui.show_fps;
  p.overlay_no_bg = s.ui.overlay_no_bg;
  p.overlay_console = s.ui.overlay_console;
  p.live_console = static_cast<int>(parse_live_console(s.ui.live_console));
  p.neural_rendering = s.neural_rendering;
  p.renodx_preset = s.renodx_preset;
  p.nr_pass_count = s.nr_pass_count;
  p.fg_enabled = s.fg_enabled;
  p.fg_multiplier = s.fg_multiplier;
  p.scale_percent = s.scale_percent;
  clamp_user_preset(p);
  return p;
}

inline UserPreset user_preset_from_session(const std::string& name, const SessionCfgState& s) {
  UserPreset p;
  p.name = name;
  p.hud_visible = s.hud_visible;
  p.overlay_no_bg = s.overlay_no_bg;
  p.overlay_console = s.overlay_console;
  p.live_console = s.live_console;
  p.neural_rendering = s.nr_on;
  p.renodx_preset = s.renodx_preset;
  p.nr_pass_count = s.nr_pass_count;
  p.fg_enabled = s.fg_on;
  p.fg_multiplier = s.fg_multiplier;
  p.scale_percent = s.scale_percent;
  clamp_user_preset(p);
  return p;
}

inline std::string serialize_user_preset(const UserPreset& raw) {
  UserPreset p = raw;
  clamp_user_preset(p);
  std::ostringstream o;
  o << kUserPresetMagicKey << "=" << kUserPresetFormat << "\n";
  o << "name=" << p.name << "\n";
  o << "hud_visible=" << (p.hud_visible ? 1 : 0) << "\n";
  o << "overlay_no_bg=" << (p.overlay_no_bg ? 1 : 0) << "\n";
  o << "overlay_console=" << (p.overlay_console ? 1 : 0) << "\n";
  o << "live_console=" << p.live_console << "\n";
  o << "neural_rendering=" << (p.neural_rendering ? 1 : 0) << "\n";
  o << "renodx_preset=" << p.renodx_preset << "\n";
  o << "nr_pass_count=" << p.nr_pass_count << "\n";
  o << "fg_enabled=" << (p.fg_enabled ? 1 : 0) << "\n";
  o << "fg_multiplier=" << p.fg_multiplier << "\n";
  o << "scale_percent=" << p.scale_percent << "\n";
  return o.str();
}

inline bool parse_bool_01(const std::string& v) {
  return v == "1" || v == "true" || v == "TRUE" || v == "yes";
}

inline bool parse_user_preset(const std::string& text, UserPreset& out) {
  UserPreset p;
  bool saw_magic = false;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq);
    const std::string v = line.substr(eq + 1);
    if (k == kUserPresetMagicKey) {
      saw_magic = true;
      continue;
    }
    if (k == "name") p.name = v;
    else if (k == "hud_visible") p.hud_visible = parse_bool_01(v);
    else if (k == "overlay_no_bg") p.overlay_no_bg = parse_bool_01(v);
    else if (k == "overlay_console") p.overlay_console = parse_bool_01(v);
    else if (k == "live_console") {
      if (!v.empty() && (std::isalpha(static_cast<unsigned char>(v[0])) || v.find('_') != std::string::npos))
        p.live_console = static_cast<int>(parse_live_console(v));
      else
        p.live_console = std::atoi(v.c_str());
    } else if (k == "neural_rendering") p.neural_rendering = parse_bool_01(v);
    else if (k == "renodx_preset") p.renodx_preset = std::atoi(v.c_str());
    else if (k == "nr_pass_count") p.nr_pass_count = std::atoi(v.c_str());
    else if (k == "fg_enabled") p.fg_enabled = parse_bool_01(v);
    else if (k == "fg_multiplier") p.fg_multiplier = std::atoi(v.c_str());
    else if (k == "scale_percent") p.scale_percent = std::atoi(v.c_str());
  }
  if (!saw_magic) return false;
  std::string n;
  if (normalize_user_preset_name(p.name, n) != UserPresetNameStatus::Ok) return false;
  p.name = n;
  clamp_user_preset(p);
  out = p;
  return true;
}

inline SessionCfgActions plan_user_preset_apply(const SessionCfgState& cur, const UserPreset& raw,
                                                bool fg_can_enable) {
  UserPreset p = raw;
  clamp_user_preset(p);
  const bool want_fg = p.fg_enabled && fg_can_enable;
  SessionCfgActions a;
  if (cur.hud_visible != p.hud_visible) a.hud_changed = true;
  if (cur.overlay_no_bg != p.overlay_no_bg) a.overlay_no_bg_changed = true;
  if (cur.overlay_console != p.overlay_console ||
      clamp_live_console(cur.live_console) != clamp_live_console(p.live_console)) {
    a.console_changed = true;
  }
  if (cur.nr_on != p.neural_rendering) a.toggle_nr = true;
  if (clamp_renodx_visual_preset(cur.renodx_preset) != p.renodx_preset) a.preset_changed = true;
  if (clamp_nr_pass_count(cur.nr_pass_count) != p.nr_pass_count) {
    a.pass_count_changed = true;
    a.pass_count_before = clamp_nr_pass_count(cur.nr_pass_count);
  }
  if (cur.fg_on != want_fg) a.toggle_fg = true;
  if (want_fg && clamp_fg_multiplier(cur.fg_multiplier) != p.fg_multiplier) a.fg_multiplier_changed = true;
  if (clamp_scale_percent(cur.scale_percent) != p.scale_percent) {
    a.scale_apply = true;
    a.scale_before = clamp_scale_percent(cur.scale_percent);
  }
  a.apply_user_preset = true;
  a.apply_hud = p.hud_visible;
  a.apply_overlay_no_bg = p.overlay_no_bg;
  a.apply_overlay_console = p.overlay_console;
  a.apply_live_console = p.live_console;
  a.apply_nr_on = p.neural_rendering;
  a.apply_renodx_preset = p.renodx_preset;
  a.apply_nr_pass_count = p.nr_pass_count;
  a.apply_fg_on = want_fg;
  a.apply_fg_multiplier = p.fg_multiplier;
  a.apply_scale_percent = p.scale_percent;
  return a;
}

inline void stamp_session_from_user_preset(SessionCfgState& cur, const UserPreset& raw, bool fg_can_enable) {
  UserPreset p = raw;
  clamp_user_preset(p);
  cur.hud_visible = p.hud_visible;
  cur.overlay_no_bg = p.overlay_no_bg;
  cur.overlay_console = p.overlay_console;
  cur.live_console = p.live_console;
  cur.nr_on = p.neural_rendering;
  cur.renodx_preset = p.renodx_preset;
  cur.nr_pass_count = p.nr_pass_count;
  cur.fg_on = p.fg_enabled && fg_can_enable;
  cur.fg_multiplier = p.fg_multiplier;
  cur.scale_percent = p.scale_percent;
}

inline void apply_user_preset_to_persisted(PersistedSettings& s, const UserPreset& raw, bool fg_can_enable) {
  UserPreset p = raw;
  clamp_user_preset(p);
  s.ui.show_fps = p.hud_visible;
  s.ui.overlay_no_bg = p.overlay_no_bg;
  s.ui.overlay_console = p.overlay_console;
  s.ui.live_console = live_console_key(clamp_live_console(p.live_console));
  s.neural_rendering = p.neural_rendering;
  if (p.neural_rendering) s.enhance_renodx = true;
  s.renodx_preset = p.renodx_preset;
  s.nr_pass_count = p.nr_pass_count;
  s.fg_enabled = p.fg_enabled && fg_can_enable;
  s.fg_multiplier = p.fg_multiplier;
  s.scale_percent = p.scale_percent;
}

struct LobbyUserPresetPlan {
  bool fg_missing = false;
  bool pass_relaunch = false;
  bool motor_relaunch = false;
  bool look_changed = false;
  int pass_before = 1;
  int pass_after = 1;
};

inline LobbyUserPresetPlan plan_lobby_user_preset_apply(const PersistedSettings& cur, const UserPreset& raw,
                                                       bool fg_can_enable) {
  UserPreset p = raw;
  clamp_user_preset(p);
  LobbyUserPresetPlan r;
  r.pass_before = clamp_nr_pass_count(cur.nr_pass_count);
  r.pass_after = p.nr_pass_count;
  r.pass_relaunch = !nr_pass_count_change_applies_live(r.pass_before, r.pass_after);
  r.motor_relaunch = nr_pass_count_change_needs_relaunch(r.pass_before, r.pass_after);
  r.look_changed = clamp_renodx_visual_preset(cur.renodx_preset) != p.renodx_preset;
  r.fg_missing = p.fg_enabled && !fg_can_enable;
  return r;
}

bool browse_share_user_preset(HWND owner, const std::string& name, const std::string& text, std::string& out_path);
bool browse_import_user_preset(HWND owner, std::string& text);

inline bool persisted_untouched_by_user_preset(const PersistedSettings& before, const PersistedSettings& after) {
  return before.device_name == after.device_name && before.hotkeys.vk_quit == after.hotkeys.vk_quit &&
         before.fg_path == after.fg_path && before.width == after.width && before.height == after.height;
}

inline std::string user_presets_dir_in(const std::string& app_data) {
  if (app_data.empty()) return {};
  return app_data + "\\presets";
}

inline std::filesystem::path default_user_presets_path() {
  return std::filesystem::u8path(app_presets_dir());
}

inline std::filesystem::path user_preset_store_path(const std::filesystem::path& dir, const std::string& name) {
  std::string n;
  if (normalize_user_preset_name(name, n) != UserPresetNameStatus::Ok) return {};
  return dir / std::filesystem::u8path(n + kUserPresetExtStore);
}

inline bool user_preset_ext_ok(const std::filesystem::path& p) {
  const auto ext = p.extension().string();
  return ext == kUserPresetExtStore || ext == kUserPresetExtShare || ext == ".INI";
}

inline bool read_user_preset_file(const std::filesystem::path& path, UserPreset& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  return parse_user_preset(ss.str(), out);
}

inline bool write_user_preset_file(const std::filesystem::path& path, const UserPreset& p) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << serialize_user_preset(p);
  return static_cast<bool>(out);
}

inline bool list_user_presets_in(const std::filesystem::path& dir, std::vector<UserPreset>& out) {
  out.clear();
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) return true;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (ec || !e.is_regular_file()) continue;
    if (!user_preset_ext_ok(e.path())) continue;
    UserPreset p;
    if (!read_user_preset_file(e.path(), p)) continue;
    bool dup = false;
    for (const auto& have : out) {
      if (user_preset_names_equal(have.name, p.name)) {
        dup = true;
        break;
      }
    }
    if (!dup) out.push_back(p);
  }
  std::sort(out.begin(), out.end(), [](const UserPreset& a, const UserPreset& b) {
    std::string la = a.name;
    std::string lb = b.name;
    for (char& c : la) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : lb) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return la < lb;
  });
  return true;
}

inline int find_user_preset_index(const std::vector<UserPreset>& list, const std::string& name) {
  for (int i = 0; i < static_cast<int>(list.size()); ++i) {
    if (user_preset_names_equal(list[static_cast<size_t>(i)].name, name)) return i;
  }
  return -1;
}

inline bool upsert_user_preset_in(const std::filesystem::path& dir, UserPreset p, bool& replaced,
                                  std::string& err) {
  replaced = false;
  clamp_user_preset(p);
  std::string n;
  if (normalize_user_preset_name(p.name, n) != UserPresetNameStatus::Ok) {
    err = "nome invalido";
    return false;
  }
  p.name = n;
  std::vector<UserPreset> have;
  list_user_presets_in(dir, have);
  const int idx = find_user_preset_index(have, p.name);
  if (idx < 0 && static_cast<int>(have.size()) >= kUserPresetMaxCount) {
    err = "lista cheia";
    return false;
  }
  std::error_code ec;
  if (idx >= 0) {
    replaced = true;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
      if (ec || !e.is_regular_file()) continue;
      UserPreset old;
      if (read_user_preset_file(e.path(), old) && user_preset_names_equal(old.name, p.name)) {
        std::filesystem::remove(e.path(), ec);
      }
    }
  }
  const auto dest = user_preset_store_path(dir, p.name);
  if (dest.empty() || !write_user_preset_file(dest, p)) {
    err = "falha ao gravar";
    return false;
  }
  return true;
}

inline bool delete_user_preset_in(const std::filesystem::path& dir, const std::string& name, std::string& err);

inline void fill_user_preset_name_buf(char* buf, size_t n, const std::string& name) {
  if (!buf || n == 0) return;
  std::snprintf(buf, n, "%s", name.c_str());
}

// Troca so o nome. Nao puxa o look atual da sessao. overwrite=false recusa se o destino
// ja existir (err="existe"); overwrite=true substitui o destino e apaga o antigo.
inline bool rename_user_preset_in(const std::filesystem::path& dir, const std::string& old_raw,
                                  const std::string& new_raw, bool overwrite, bool& replaced, std::string& err) {
  replaced = false;
  std::string old_n;
  std::string new_n;
  if (normalize_user_preset_name(old_raw, old_n) != UserPresetNameStatus::Ok) {
    err = "nome invalido";
    return false;
  }
  const UserPresetNameStatus st = normalize_user_preset_name(new_raw, new_n);
  if (st != UserPresetNameStatus::Ok) {
    err = "nome invalido";
    return false;
  }
  std::vector<UserPreset> have;
  list_user_presets_in(dir, have);
  const int src = find_user_preset_index(have, old_n);
  if (src < 0) {
    err = "nao encontrado";
    return false;
  }
  UserPreset p = have[static_cast<size_t>(src)];
  if (p.name == new_n) return true;
  const int dst = find_user_preset_index(have, new_n);
  if (dst >= 0 && !user_preset_names_equal(have[static_cast<size_t>(dst)].name, old_n)) {
    if (!overwrite) {
      err = "existe";
      return false;
    }
    replaced = true;
  }
  if (!delete_user_preset_in(dir, old_n, err)) return false;
  p.name = new_n;
  bool dummy = false;
  if (!upsert_user_preset_in(dir, p, dummy, err)) return false;
  return true;
}

inline bool delete_user_preset_in(const std::filesystem::path& dir, const std::string& name, std::string& err) {
  std::string n;
  if (normalize_user_preset_name(name, n) != UserPresetNameStatus::Ok) {
    err = "nome invalido";
    return false;
  }
  std::error_code ec;
  bool any = false;
  if (!std::filesystem::exists(dir, ec)) {
    err = "nao encontrado";
    return false;
  }
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (ec || !e.is_regular_file()) continue;
    UserPreset old;
    if (read_user_preset_file(e.path(), old) && user_preset_names_equal(old.name, n)) {
      std::filesystem::remove(e.path(), ec);
      any = !ec;
    }
  }
  if (!any) {
    err = "nao encontrado";
    return false;
  }
  return true;
}

inline bool import_user_preset_in(const std::filesystem::path& dir, const std::string& text, UserPreset& out,
                                  bool& replaced, std::string& err) {
  if (!parse_user_preset(text, out)) {
    err = "arquivo invalido";
    return false;
  }
  return upsert_user_preset_in(dir, out, replaced, err);
}

}  // namespace nexuscap
