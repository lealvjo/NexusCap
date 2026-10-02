#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ostream>
#include <string>
#include <vector>

namespace nexuscap {

struct HotkeyConfig {
  int vk_quit = 0x1B;       // VK_ESCAPE
  int vk_hud = 0x20;        // VK_SPACE — with hud_ctrl → Ctrl+Espaço
  bool hud_ctrl = true;     // require Ctrl for overlay HUD cycle
  int vk_analyze = 0x73;    // VK_F4
  int vk_dlss = 0x74;       // VK_F5
  int vk_fg_toggle = 0x22;  // VK_NEXT (Page Down) — X2 framegen
  int vk_fg_cycle = 0x21;   // VK_PRIOR (Page Up)
  int vk_renodx = 0x24;     // VK_HOME — overlay presets
  int vk_nr_toggle = 0x2E;  // VK_DELETE — Neural Rendering on/off (FG independente)
  int vk_fullscreen = 0x7A; // VK_F11
  int vk_gear = 0x7B;       // VK_F12 — open gear in lobby
  bool gear_ctrl = false;   // optional Ctrl for the lobby CFG
  int vk_cfg = 0x4D;        // 'M' — with cfg_ctrl → Ctrl+M abre o CFG na captura
  bool cfg_ctrl = true;     // require Ctrl for the session CFG menu
  int vk_pass_next = 0xBB;  // VK_OEM_PLUS — with pass_next_ctrl → Ctrl++
  bool pass_next_ctrl = true;
  int vk_pass_prev = 0xBD;  // VK_OEM_MINUS — with pass_prev_ctrl → Ctrl+-
  bool pass_prev_ctrl = true;
};

struct SessionUiFlags {
  bool show_fps = true;
  bool advanced_metrics = false;
  // Steam Deck-style: one horizontal top bar; turns off compact/advanced multi-line HUD.
  bool mini_overlay = false;
  bool overlay_no_bg = false;  // todos os overlays: sem placa preta (só texto)
  bool hw_info = false;  // painel CPU(+C) / GPU(+C) / RAM / VRAM / PWR (abaixo de métricas)
  bool fullscreen = true;
  bool hide_panel_on_capture = true;  // leftover ini key; always forced on (captura limpa)
  bool sync_image = true;  // VSYNC default — TEAR+ALLOW_TEARING felt shakier in A/B vs 2.0.1
  bool preview_mute = false;  // lobby preview audio mute (botao Desativar som)
  bool preview_enabled = true;  // lobby HDMI preview capture; off stops DShow + WASAPI
  bool preview_cfg_live = false;  // NR/RenoDX no preview do lobby; default off (GPU)
  bool bug_report = false;    // session → report/bug_TIMESTAMP.txt
  bool perf_report = false;   // session → performance/performance_TIMESTAMP.txt
  // Live overlay strip. Console branding is public (all overlays).
  bool live_overlay = false;
  bool overlay_console = false;  // optional: show console name on overlays
  std::string live_console = "ps5";
};

// Lobby: preview HDMI and WASAPI. Mute zeros samples; preview off stops capture+audio.
inline bool lobby_preview_capture_wanted(bool preview_enabled) { return preview_enabled; }
inline bool lobby_preview_audio_wanted(bool preview_enabled, bool preview_live) {
  return preview_enabled && preview_live;
}
inline const char* lobby_preview_toggle_label(bool preview_enabled) {
  return preview_enabled ? "Desativar preview" : "Ativar preview";
}
inline const char* lobby_audio_toggle_label(bool muted) {
  return muted ? "Ativar som" : "Desativar som";
}
inline bool lobby_preview_cfg_wanted(bool preview_enabled, bool preview_cfg_live) {
  return preview_enabled && preview_cfg_live;
}
inline const char* lobby_preview_cfg_toggle_label(bool preview_cfg_live) {
  return preview_cfg_live ? "Ocultar configuracoes no preview em tempo real"
                          : "Visualizar configuracoes no preview em tempo real";
}

// Disco do Present do lobby. Iniciar usa sync_persisted_session_files, nao estes helpers.
inline bool lobby_preview_nr_files_on(bool preview_enabled, bool preview_cfg_live, bool enhance_renodx,
                                      bool neural_rendering) {
  return lobby_preview_cfg_wanted(preview_enabled, preview_cfg_live) && enhance_renodx &&
         neural_rendering;
}
inline bool lobby_preview_feed_files_on(bool preview_enabled, bool preview_cfg_live, bool enhance_renodx) {
  return lobby_preview_cfg_wanted(preview_enabled, preview_cfg_live) && enhance_renodx;
}

// Lobby só carrega o dxgi/ReShade quando o preview ao vivo está ligado.
inline bool lobby_loads_reshade_dxgi(bool preview_enabled, bool preview_cfg_live, bool enhance_renodx) {
  return lobby_preview_cfg_wanted(preview_enabled, preview_cfg_live) && enhance_renodx;
}

inline constexpr const char kReshadeDxgiFile[] = "dxgi.dll";
inline constexpr const char kReshadeDxgiParkedFile[] = "dxgi.session.dll";

inline std::string reshade_dxgi_path_in(const std::string& exe_dir) {
  if (exe_dir.empty()) return {};
  const char c = exe_dir.back();
  if (c == '\\' || c == '/') return exe_dir + kReshadeDxgiFile;
  return exe_dir + "\\" + kReshadeDxgiFile;
}
inline std::string reshade_dxgi_parked_path_in(const std::string& exe_dir) {
  if (exe_dir.empty()) return {};
  const char c = exe_dir.back();
  if (c == '\\' || c == '/') return exe_dir + kReshadeDxgiParkedFile;
  return exe_dir + "\\" + kReshadeDxgiParkedFile;
}

bool park_reshade_dxgi(const std::string& exe_dir);
bool restore_reshade_dxgi(const std::string& exe_dir);
bool park_reshade_dxgi_wait(const std::string& exe_dir, int timeout_ms);
bool reshade_dxgi_present(const std::string& exe_dir);
inline const char* lobby_preview_cfg_pipeline_hold_line(bool cfg_wanted) {
  return cfg_wanted ? "Configuracoes no preview em tempo real" : "Preview HDMI sem NR";
}

inline const char* motors_stock_filename() { return "motors.stock"; }

struct MotorStockEntry {
  std::uint64_t size = 0;
  std::string sha256;
  std::string rel;
};

inline std::string normalize_motor_stock_rel(std::string rel) {
  while (!rel.empty() && (rel.front() == ' ' || rel.front() == '\t')) rel.erase(rel.begin());
  while (!rel.empty() && (rel.back() == ' ' || rel.back() == '\t' || rel.back() == '\r')) rel.pop_back();
  if (rel.size() >= 2 && rel[0] == '.' && (rel[1] == '/' || rel[1] == '\\')) rel.erase(0, 2);
  for (char& c : rel) {
    if (c == '\\') c = '/';
  }
  return rel;
}

inline bool motor_stock_hex_equal(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'F') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'F') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

inline bool motor_stock_rel_is_tracked(const std::string& rel) {
  return rel == "addons-stable/renodx-dlss5.addon64" || rel == "addons-stable/dlss5-feed.addon64" ||
         rel == "addons-multipass/renodx-dlss5.addon64" || rel == "addons-multipass/dlss5-feed.addon64";
}

inline bool motor_stock_rel_is_stable(const std::string& rel) {
  return rel.find("addons-stable/") == 0;
}

inline bool motor_stock_rel_is_multipass(const std::string& rel) {
  return rel.find("addons-multipass/") == 0;
}

inline bool motor_file_matches_stock(std::uint64_t want_size, const std::string& want_sha,
                                    std::uint64_t got_size, const std::string& got_sha) {
  return want_size == got_size && motor_stock_hex_equal(want_sha, got_sha);
}

inline bool lobby_motor_override_wanted(bool stock_ok, bool compared_any, bool stable_dirty,
                                        bool multipass_dirty) {
  return stock_ok && compared_any && (stable_dirty || multipass_dirty);
}

inline const char* lobby_motor_override_warn() {
  return "Aviso: motores padrao alterados. Podes trocar o Feed e/ou o RenoDX (um ou os dois) depois de "
         "instalar, mas pode dar bugs e perdes 2x otimizado e 1,5x.";
}

inline const char* lobby_motor_override_which(bool stable, bool multipass) {
  if (stable && multipass) return "1x (stable) e 1,5x/2x/3x (multipass)";
  if (stable) return "so o motor 1x (Feed estavel)";
  if (multipass) return "o motor multipass (1x/1,5x/2x/3x)";
  return "";
}

inline std::vector<MotorStockEntry> parse_motor_stock_manifest(const std::string& text) {
  std::vector<MotorStockEntry> out;
  std::string line;
  for (size_t i = 0; i <= text.size(); ++i) {
    if (i < text.size() && text[i] != '\n') {
      line.push_back(text[i]);
      continue;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t a = 0;
    while (a < line.size() && (line[a] == ' ' || line[a] == '\t')) ++a;
    if (a < line.size() && line[a] != '#' && line[a] != ';') {
      const auto sp1 = line.find_first_of(" \t", a);
      if (sp1 != std::string::npos) {
        size_t b = sp1;
        while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
        const auto sp2 = line.find_first_of(" \t", b);
        if (sp2 != std::string::npos) {
          MotorStockEntry e;
          e.size = static_cast<std::uint64_t>(std::strtoull(line.c_str() + a, nullptr, 10));
          e.sha256 = line.substr(b, sp2 - b);
          e.rel = normalize_motor_stock_rel(line.substr(sp2 + 1));
          if (e.size > 0 && e.sha256.size() == 64 && motor_stock_rel_is_tracked(e.rel)) out.push_back(e);
        }
      }
    }
    line.clear();
  }
  return out;
}

struct BundledMotorProbe {
  bool stock_ok = false;
  bool compared_any = false;
  bool stable_dirty = false;
  bool multipass_dirty = false;
  bool wanted() const {
    return lobby_motor_override_wanted(stock_ok, compared_any, stable_dirty, multipass_dirty);
  }
};

inline void apply_motor_file_compare(bool compared, bool matches, bool is_stable, bool is_multipass,
                                     bool& compared_any, bool& stable_dirty, bool& multipass_dirty) {
  if (!compared) return;
  compared_any = true;
  if (matches) return;
  if (is_stable) stable_dirty = true;
  if (is_multipass) multipass_dirty = true;
}

std::string sha256_hex(const void* data, size_t n);
std::string sha256_file_hex(const std::string& path, std::uint64_t* size_out);
BundledMotorProbe probe_bundled_motors(const std::string& exe_dir);

// Painel "Configuracoes avancadas". Um bloco por X. -1 em skin = 1.00 (padrao) do mod.
struct NrAdvancedLook {
  double local_tone = 1;
  double local_structure = 1;
  double skin = -1;
  int auto_mask = 1;
  int skin_independent = 1;
  int ui_correction = 1;
  int look_mode = 0;
  double look_strength = 1;
  double look_brighten = 1;
  double look_darken = 1;
  double look_colour = 1;
  double look_hue = 1;
  double look_shadows = 1;
  double look_midtones = 1;
  double look_highlights = 1;
  int detail_stability = 0;  // 0 Auto, 1 Desligado, 2 Sempre
};

struct PersistedSettings {
  HotkeyConfig hotkeys{};
  SessionUiFlags ui{};
  int scale_percent = 100;     // 25–100; 100 = no scale
  bool fg_enabled = false;     // default OFF
  int fg_multiplier = 2;       // used when fg_enabled
  int fg_queue = 0;            // 0 menor latência, 1 equilibrado, 2 maior estabilidade
  int fg_flow_scale = 80;      // Padrao = 80; 70/90/100 so se a pessoa escolher
  bool fg_adaptive = false;    // LSFG3Mode1 Adaptive; default FIXED
  int fg_target_fps = 60;      // LSFG3Target; clamp ao Hz do monitor na hora de gravar
  // Optional override of LosslessScaling.exe; empty = auto (Steam path / registry).
  std::string fg_path;
  std::string device_name = "auto";
  int width = 2560;
  int height = 1440;
  double fps = 60.0;
  bool enhance_renodx = true;
  // Neural Rendering on/off (Delete na captura). Independente do FG; persiste entre sessões.
  bool neural_rendering = true;
  // Gear: Default / Natural / Cinematic (RenoDX Home overlay names).
  int renodx_preset = 0;  // 1x Default/Natural/Cinematic; 1.5x/2x/3x Model A/B/C
  // Pass Count: 1=1x, 4=1.5x, 5=2x otimizado, 6=3x otimizado. 2 e 3 (default) aposentados.
  int nr_pass_count = 1;
  // Receita avançada de cada X (índice = Pass Count 1..6). Ausente no ini = fábrica.
  NrAdvancedLook nr_advanced[7]{};
  // Origem (OBS-style). Defaults = validated HDMI path.
  int video_fourcc = 0;       // VideoFourcc::Nv12
  int color_space = 0;        // ColorSpace::Default (BT.709)
  int color_range = 0;        // ColorRange::Limited
  int capture_buffering = 0;  // CaptureBuffering::Disabled
  // HDMI WASAPI monitor gain (CFG). App setting — never a user preset field.
  int capture_volume = 100;
  // 0=HDMI (default no boot). 1=Janela. Lobby sempre abre em HDMI; Iniciar Janela grava 1 para --autostart.
  int capture_source = 0;
  std::string window_title;  // ultimo titulo do picker Janela
  uint64_t window_hwnd = 0;
};

enum class RenodxVisualPreset : int {
  Default = 0,
  Natural = 1,
  Cinematic = 2,
};

inline int clamp_renodx_visual_preset(int v) {
  if (v < 0) return 0;
  if (v > 2) return 2;
  return v;
}

// Pass Count UI: 1=1x, 4=1.5x, 5=2x otimizado, 6=3x otimizado.
// 2 e 3 (default, gancho antigo e mais pesado) aposentados: caem no otimizado.
// 7 (1.7x) aposentado: clamp para 4 (1.5x herda a receita).
inline constexpr int kMaxNrPassCount = 6;
inline constexpr int kNr15xPlusUi = 4;
inline constexpr const char* kDefaultMgfSharpness = "0.450000";

inline int clamp_nr_pass_count(int v) {
  if (v == 7) return 4;
  if (v == 2) return 5;
  if (v == 3) return 6;
  if (v < 1) return 1;
  if (v > kMaxNrPassCount) return kMaxNrPassCount;
  return v;
}

inline bool nr_pass_is_2x_optimized(int ui) { return clamp_nr_pass_count(ui) == 5; }
inline bool nr_pass_is_3x_optimized(int ui) { return clamp_nr_pass_count(ui) == 6; }
inline bool nr_pass_is_15x_plus(int ui) { return clamp_nr_pass_count(ui) == kNr15xPlusUi; }

// 1x / 1,5x / 2x otimizado / 3x otimizado: gancho barato no HDMI 8.5.
inline bool nr_pass_uses_optimized_look(int ui) {
  const int v = clamp_nr_pass_count(ui);
  return v == 1 || v == 4 || v == 5 || v == 6;
}

inline double clamp_nr_knob(double v, double lo, double hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

inline int clamp_nr_choice(int v, int hi) {
  if (v < 0) return 0;
  if (v > hi) return hi;
  return v;
}

inline NrAdvancedLook clamp_nr_advanced(NrAdvancedLook a) {
  a.local_tone = clamp_nr_knob(a.local_tone, 0, 3);
  a.local_structure = clamp_nr_knob(a.local_structure, 0, 3);
  a.skin = clamp_nr_knob(a.skin, -1, 3);
  a.auto_mask = a.auto_mask ? 1 : 0;
  a.skin_independent = a.skin_independent ? 1 : 0;
  a.ui_correction = clamp_nr_choice(a.ui_correction, 2);
  a.look_mode = a.look_mode ? 1 : 0;
  a.look_strength = clamp_nr_knob(a.look_strength, 0, 3);
  a.look_brighten = clamp_nr_knob(a.look_brighten, 0, 3);
  a.look_darken = clamp_nr_knob(a.look_darken, 0, 3);
  a.look_colour = clamp_nr_knob(a.look_colour, 0, 3);
  a.look_hue = clamp_nr_knob(a.look_hue, 0, 3);
  a.look_shadows = clamp_nr_knob(a.look_shadows, 0, 3);
  a.look_midtones = clamp_nr_knob(a.look_midtones, 0, 3);
  a.look_highlights = clamp_nr_knob(a.look_highlights, 0, 3);
  a.detail_stability = clamp_nr_choice(a.detail_stability, 2);
  return a;
}

// Fábrica de cada X: o que apply_nr_edit_recipe e o gancho já gravavam.
inline NrAdvancedLook nr_advanced_default(int pass) {
  NrAdvancedLook a;
  const int p = clamp_nr_pass_count(pass);
  a.ui_correction = nr_pass_uses_optimized_look(p) ? 2 : 1;
  if (nr_pass_is_15x_plus(p)) {
    a.look_mode = 1;
    a.look_strength = 1.75;
    a.look_brighten = 0.50;
    a.look_darken = 0.86;
    a.look_colour = 1.12;
    a.look_hue = 0.62;
    a.look_shadows = 0.90;
    a.look_midtones = 1.00;
    a.look_highlights = 0.50;
  }
  return a;
}

inline void fill_nr_advanced_defaults(NrAdvancedLook* slots) {
  if (!slots) return;
  for (int i = 0; i <= 6; ++i) slots[i] = nr_advanced_default(i == 0 ? 1 : i);
}

inline bool nr_advanced_near(double a, double b) {
  const double d = a - b;
  return d < 0.0005 && d > -0.0005;
}

inline bool nr_advanced_equal(const NrAdvancedLook& a, const NrAdvancedLook& b) {
  const NrAdvancedLook x = clamp_nr_advanced(a);
  const NrAdvancedLook y = clamp_nr_advanced(b);
  return nr_advanced_near(x.local_tone, y.local_tone) && nr_advanced_near(x.local_structure, y.local_structure) &&
         nr_advanced_near(x.skin, y.skin) && x.auto_mask == y.auto_mask &&
         x.skin_independent == y.skin_independent && x.ui_correction == y.ui_correction &&
         x.look_mode == y.look_mode && nr_advanced_near(x.look_strength, y.look_strength) &&
         nr_advanced_near(x.look_brighten, y.look_brighten) && nr_advanced_near(x.look_darken, y.look_darken) &&
         nr_advanced_near(x.look_colour, y.look_colour) && nr_advanced_near(x.look_hue, y.look_hue) &&
         nr_advanced_near(x.look_shadows, y.look_shadows) && nr_advanced_near(x.look_midtones, y.look_midtones) &&
         nr_advanced_near(x.look_highlights, y.look_highlights) && x.detail_stability == y.detail_stability;
}

inline bool nr_advanced_slots_equal(const NrAdvancedLook* a, const NrAdvancedLook* b) {
  if (!a || !b) return a == b;
  for (int i = 1; i <= 6; ++i) {
    if (!nr_advanced_equal(a[i], b[i])) return false;
  }
  return true;
}

inline bool nr_advanced_draft_unapplied(const NrAdvancedLook& draft, const NrAdvancedLook& saved) {
  return !nr_advanced_equal(draft, saved);
}

inline void copy_nr_advanced(NrAdvancedLook* dst, const NrAdvancedLook* src) {
  if (!dst || !src) return;
  for (int i = 0; i <= 6; ++i) dst[i] = src[i];
}

inline std::string format_nr_decimal(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", v);
  return buf;
}

inline std::string serialize_nr_advanced_csv(const NrAdvancedLook& raw) {
  const NrAdvancedLook a = clamp_nr_advanced(raw);
  char buf[320];
  std::snprintf(buf, sizeof(buf),
                "%.4f,%.4f,%.4f,%d,%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d", a.local_tone,
                a.local_structure, a.skin, a.auto_mask, a.skin_independent, a.ui_correction, a.look_mode,
                a.look_strength, a.look_brighten, a.look_darken, a.look_colour, a.look_hue, a.look_shadows,
                a.look_midtones, a.look_highlights, a.detail_stability);
  return buf;
}

inline bool parse_nr_advanced_csv(const std::string& val, NrAdvancedLook& out) {
  NrAdvancedLook a;
  int mask = 0, skin_i = 0, ui = 0, mode = 0, stab = 0;
  const int n = std::sscanf(val.c_str(), "%lf,%lf,%lf,%d,%d,%d,%d,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%d", &a.local_tone,
                             &a.local_structure, &a.skin, &mask, &skin_i, &ui, &mode, &a.look_strength,
                             &a.look_brighten, &a.look_darken, &a.look_colour, &a.look_hue, &a.look_shadows,
                             &a.look_midtones, &a.look_highlights, &stab);
  if (n != 16) return false;
  a.auto_mask = mask;
  a.skin_independent = skin_i;
  a.ui_correction = ui;
  a.look_mode = mode;
  a.detail_stability = stab;
  out = clamp_nr_advanced(a);
  return true;
}

inline void write_nr_advanced_lines(std::ostream& o, const NrAdvancedLook* slots) {
  if (!slots) return;
  for (int p = 1; p <= 6; ++p) {
    if (nr_advanced_equal(slots[p], nr_advanced_default(p))) continue;
    o << "nr_adv_" << p << "=" << serialize_nr_advanced_csv(slots[p]) << "\n";
  }
}

// true se a chave era nr_adv_N (consumida, mesmo se o índice for ignorado).
inline bool parse_nr_advanced_setting_key(const std::string& key, const std::string& val, NrAdvancedLook* slots) {
  if (key.compare(0, 7, "nr_adv_") != 0) return false;
  const int p = std::atoi(key.c_str() + 7);
  if (!slots || p < 1 || p > 6) return true;
  NrAdvancedLook look;
  if (parse_nr_advanced_csv(val, look)) slots[p] = look;
  return true;
}

// What DirectNeuralRenderingPassCount we write (UI 4 → INI 1; UI 5 → INI 2; UI 6 → INI 3).
inline int nr_reshade_pass_count(int ui) {
  switch (clamp_nr_pass_count(ui)) {
    case 2:
    case 5:
      return 2;
    case 3:
    case 6:
      return 3;
    default:
      return 1;
  }
}

inline const char* nr_pass_count_label(int v) {
  switch (clamp_nr_pass_count(v)) {
    case 4:
      return "1,5x";
    case 5:
      return "2x otimizado";
    case 6:
      return "3x otimizado";
    default:
      return "1x";
  }
}

// Ordem dos radios: 1x, 1,5x, 2x otimizado, 3x otimizado.
inline constexpr int kNrPassCycleOrder[] = {1, 4, 5, 6};
inline constexpr int kNrPassCycleCount = 4;

inline int nr_pass_cycle_index(int pass) {
  const int c = clamp_nr_pass_count(pass);
  for (int i = 0; i < kNrPassCycleCount; ++i) {
    if (kNrPassCycleOrder[i] == c) return i;
  }
  return 0;
}

// delta +1 avanca, -1 volta. No ultimo volta ao 1x; no 1x o menos cai no 3x otimizado.
inline int step_nr_pass_count(int pass, int delta) {
  if (delta == 0) return clamp_nr_pass_count(pass);
  const int n = kNrPassCycleCount;
  int step = delta % n;
  if (step < 0) step += n;
  return kNrPassCycleOrder[(nr_pass_cycle_index(pass) + step) % n];
}

// + do teclado e do numpad sao o mesmo atalho, e o mesmo para o -.
inline bool pass_hotkey_vk_match(int vk, int bound) {
  if (bound == 0 || vk == 0) return false;
  if (vk == bound) return true;
  if ((bound == 0xBB && vk == 0x6B) || (bound == 0x6B && vk == 0xBB)) return true;
  if ((bound == 0xBD && vk == 0x6D) || (bound == 0x6D && vk == 0xBD)) return true;
  return false;
}

// Atalho configuravel do ciclo de Pass Count. +1 avanca, -1 volta.
inline int pass_cycle_hotkey_step(int vk, bool ctrl_down, const HotkeyConfig& hk) {
  auto fires = [&](int bound, bool need_ctrl) {
    if (!pass_hotkey_vk_match(vk, bound)) return false;
    if (need_ctrl && !ctrl_down) return false;
    return true;
  };
  if (fires(hk.vk_pass_next, hk.pass_next_ctrl)) return 1;
  if (fires(hk.vk_pass_prev, hk.pass_prev_ctrl)) return -1;
  return 0;
}

// Hint under the Pass Count radios (lobby F12 e Ctrl+M).
inline const char* nr_pass_count_1x_hint() {
  return "1x exclusivo NexusCap: gancho do 2x otimizado — menos flicker, melhor imagem, sem perder desempenho.";
}
inline const char* nr_pass_count_15x_hint() {
  return "1,5x exclusivo NexusCap: evolucao do 1x — ainda mais proximo do 2x.";
}
inline const char* nr_pass_count_15x_plus_hint() { return nr_pass_count_15x_hint(); }
inline const char* nr_pass_count_2x_opt_hint() {
  return "2x otimizado exclusivo NexusCap: 2x de verdade, menos flicker e ghost, melhor qualidade de imagem.";
}
// Um hint — o do X escolhido. Os quatro de uma vez deixavam o CFG uma lista.
inline const char* nr_pass_count_selected_hint(int ui) {
  switch (clamp_nr_pass_count(ui)) {
    case 1:
      return nr_pass_count_1x_hint();
    case 4:
      return nr_pass_count_15x_hint();
    case 6:
      return nr_pass_count_2x_opt_hint();
    default:
      return nr_pass_count_2x_opt_hint();
  }
}

// Short HUD tag: "1x" / "1.5x" / "2x+" / "3x+".
inline const char* nr_pass_count_short(int v) {
  switch (clamp_nr_pass_count(v)) {
    case 4:
      return "1.5x";
    case 5:
      return "2x+";
    case 6:
      return "3x+";
    default:
      return "1x";
  }
}

// Todo Pass Count roda no addons-multipass (1 = 1x default; 4 = 1.5x; 5 = 2x otimizado; 6 = 3x otimizado).
inline bool nr_pass_count_uses_multipass_addon(int /*pass_count*/) { return true; }

inline const char* nr_dlss5_motor_name(int /*pass_count*/) { return "multipass"; }

inline const char* dlss5_overlay_state_word(bool on) { return on ? "ON" : "off"; }

// Compact overlay line: "DLSS5 ON 2x otimizado Feed·multipass"
inline void format_dlss5_stack_hud_line(char* out, size_t out_n, bool nr_on, bool feed_on, int pass_count) {
  if (!out || out_n == 0) return;
  std::snprintf(out, out_n, "DLSS5 %s %s Feed·%s", dlss5_overlay_state_word(nr_on),
                nr_pass_count_label(pass_count), nr_dlss5_motor_name(pass_count));
  (void)feed_on;
}

// Troca de motor (addon diferente). Com um motor só, nunca acontece.
inline bool nr_pass_count_change_needs_relaunch(int /*before*/, int /*after*/) { return false; }

// O addon multipass lê DirectNeuralRenderingPassCount só no init e não aceita o ciclo OFF→ON
// do NR (ReShade.log: "nao importa USER32!GetAsyncKeyState ... alternancia do NR indisponivel").
// Medido: com o ini já em 3 o runtime seguia em "NR PASS TEST 1: passes=1". Logo 1,5x↔2x↔3x
// também precisa de processo novo — só escrever o ini não muda nada na imagem.
inline bool nr_pass_count_change_applies_live(int before, int after) {
  return clamp_nr_pass_count(before) == clamp_nr_pass_count(after);
}

enum class RenodxAddonSyncResult {
  OkSame = 0,
  OkSwapped = 1,
  MissingVariant = 2,
  WriteFailed = 3,
};

// Point ReShade AddonPath at addons-multipass before load. Corrige ini antigo em addons-stable.
RenodxAddonSyncResult sync_renodx_addon_variant(const std::string& exe_dir, int pass_count);

inline const char* renodx_addon_stack_rel(int /*pass_count*/) { return ".\\addons-multipass"; }

// Spawn a new NexusCap.exe then return true (caller should ExitProcess).
bool relaunch_current_process(const char* extra_args = nullptr);

// Mesmo nome do mod (NR Style): Default / Natural / Cinematic.
inline const char* renodx_visual_preset_name(int v, int /*pass_count*/ = 1) {
  switch (clamp_renodx_visual_preset(v)) {
    case 1:
      return "Natural";
    case 2:
      return "Cinematic";
    default:
      return "Default";
  }
}

// RenoDX overlay "NR Preset" (Krish): Natural=0, Cinematic=1. Não é o NR Style.
inline int renodx_nr_preset_index(int visual) {
  return clamp_renodx_visual_preset(visual) == static_cast<int>(RenodxVisualPreset::Cinematic) ? 1 : 0;
}

// Krish 8.5 lê NRStyle nesta ordem: Default=0, Natural=1, Cinematic=2.
inline int renodx_nr_style_index(int visual) {
  return clamp_renodx_visual_preset(visual);
}

// Multipass DirectNeuralRenderingStyle is the model slot (A=0, B=1, C=2).
inline int renodx_direct_style_index(int visual, int /*pass_count*/ = 1) {
  return clamp_renodx_visual_preset(visual);
}

// feed.cfg preset=: Model A/B/C = 0/1/2. 1x e 1,5x seguem a cena.
inline int renodx_feed_model_preset(int visual, int /*pass_count*/ = 1) {
  return clamp_renodx_visual_preset(visual);
}

// True when two gear presets must produce different on-disk RenoDX config (preview/session).
inline bool renodx_visual_presets_differ_on_disk(int a, int b, int pass_count = 1) {
  a = clamp_renodx_visual_preset(a);
  b = clamp_renodx_visual_preset(b);
  if (a == b) return false;
  return renodx_nr_preset_index(a) != renodx_nr_preset_index(b) ||
         renodx_feed_model_preset(a, pass_count) != renodx_feed_model_preset(b, pass_count) ||
         renodx_direct_style_index(a, pass_count) != renodx_direct_style_index(b, pass_count);
}

// CPU look-alike for Default/Natural/Cinematic on the lobby preview band (ImGui texture).
// RenoDX still runs on Present; this grade makes radio changes visible immediately.
struct LobbyPreviewGrade {
  float contrast = 1.f;
  float saturation = 1.f;
  float brightness = 0.f;
  float warm = 0.f;  // +R / -B
};

inline bool lobby_preview_grade_is_identity(const LobbyPreviewGrade& g) {
  return g.contrast == 1.f && g.saturation == 1.f && g.brightness == 0.f && g.warm == 0.f;
}

inline LobbyPreviewGrade lobby_preview_grade_for_preset(int visual_preset, bool enhance_on) {
  LobbyPreviewGrade g;
  if (!enhance_on) return g;
  switch (clamp_renodx_visual_preset(visual_preset)) {
    case static_cast<int>(RenodxVisualPreset::Natural):
      g.contrast = 1.06f;
      g.saturation = 1.10f;
      g.brightness = 0.025f;
      g.warm = 0.01f;
      break;
    case static_cast<int>(RenodxVisualPreset::Cinematic):
      g.contrast = 1.22f;
      g.saturation = 0.82f;
      g.brightness = -0.04f;
      g.warm = 0.06f;
      break;
    default:  // Default — subtle clarity
      g.contrast = 1.10f;
      g.saturation = 1.03f;
      g.brightness = 0.f;
      g.warm = 0.f;
      break;
  }
  return g;
}

inline bool lobby_preview_grades_differ(const LobbyPreviewGrade& a, const LobbyPreviewGrade& b) {
  return a.contrast != b.contrast || a.saturation != b.saturation || a.brightness != b.brightness ||
         a.warm != b.warm;
}

inline uint8_t lobby_preview_grade_clamp_u8(float v) {
  if (v < 0.f) return 0;
  if (v > 255.f) return 255;
  return static_cast<uint8_t>(v + 0.5f);
}

inline void apply_lobby_preview_grade_pixel(uint8_t* rgb, const LobbyPreviewGrade& g) {
  if (!rgb || lobby_preview_grade_is_identity(g)) return;
  float r = rgb[0] / 255.f;
  float gg = rgb[1] / 255.f;
  float b = rgb[2] / 255.f;
  r = (r - 0.5f) * g.contrast + 0.5f + g.brightness;
  gg = (gg - 0.5f) * g.contrast + 0.5f + g.brightness;
  b = (b - 0.5f) * g.contrast + 0.5f + g.brightness;
  const float y = 0.2126f * r + 0.7152f * gg + 0.0722f * b;
  r = y + (r - y) * g.saturation;
  gg = y + (gg - y) * g.saturation;
  b = y + (b - y) * g.saturation;
  r += g.warm;
  b -= g.warm;
  rgb[0] = lobby_preview_grade_clamp_u8(r * 255.f);
  rgb[1] = lobby_preview_grade_clamp_u8(gg * 255.f);
  rgb[2] = lobby_preview_grade_clamp_u8(b * 255.f);
}

inline void apply_lobby_preview_grade_rgba(uint8_t* rgba, int pixel_count, const LobbyPreviewGrade& g) {
  if (!rgba || pixel_count <= 0 || lobby_preview_grade_is_identity(g)) return;
  for (int i = 0; i < pixel_count; ++i) {
    apply_lobby_preview_grade_pixel(rgba + static_cast<size_t>(i) * 4, g);
  }
}

inline constexpr int kMinScalePercent = 25;
inline constexpr int kMaxScalePercent = 100;

inline int clamp_scale_percent(int p) {
  if (p < kMinScalePercent) return kMinScalePercent;
  if (p > kMaxScalePercent) return kMaxScalePercent;
  return p;
}

// Trocar o modo X não reescreve a escala da UI.
inline int nr_pass_count_on_select_scale(int /*pass_before*/, int /*pass_after*/, int scale_percent) {
  return clamp_scale_percent(scale_percent);
}

// Generic key=value int rewriter (feed.cfg / ReShade.ini style).
// Key must start at beginning of line (avoids "rebuild=" matching inside "warmup_rebuild=").
inline std::string apply_keyed_int(std::string text, const std::string& key, int value) {
  const std::string line = key + std::to_string(value);
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const auto found = text.find(key, pos);
    if (found == std::string::npos) break;
    const bool at_line_start = (found == 0) || text[found - 1] == '\n' || text[found - 1] == '\r';
    if (at_line_start) {
      const auto end = text.find_first_of("\r\n", found);
      text.replace(found, (end == std::string::npos ? text.size() : end) - found, line);
      return text;
    }
    pos = found + key.size();
  }
  if (!text.empty() && text.back() != '\n') text.push_back('\n');
  text += line;
  text.push_back('\n');
  return text;
}

// ReShade keeps the same key in several sections (saved preset + live addon section).
// Rewriting only the first match left the live section stale (e.g. PassCount=3 in the
// preset while [RenoDX.DLSS5] still ran 1 pass), so rewrite every occurrence.
inline std::string apply_keyed_int_all(std::string text, const std::string& key, int value) {
  const std::string line = key + std::to_string(value);
  std::size_t pos = 0;
  bool hit = false;
  while (pos <= text.size()) {
    const auto found = text.find(key, pos);
    if (found == std::string::npos) break;
    const bool at_line_start = (found == 0) || text[found - 1] == '\n' || text[found - 1] == '\r';
    if (!at_line_start) {
      pos = found + key.size();
      continue;
    }
    const auto end = text.find_first_of("\r\n", found);
    text.replace(found, (end == std::string::npos ? text.size() : end) - found, line);
    hit = true;
    pos = found + line.size();
  }
  return hit ? text : apply_keyed_int(std::move(text), key, value);
}

// True when every line-start occurrence of key equals want. Missing key → false.
inline bool every_keyed_int_is(const std::string& text, const std::string& key, int want) {
  std::size_t pos = 0;
  bool any = false;
  while (pos <= text.size()) {
    const auto found = text.find(key, pos);
    if (found == std::string::npos) break;
    const bool at_line_start = (found == 0) || text[found - 1] == '\n' || text[found - 1] == '\r';
    if (!at_line_start) {
      pos = found + key.size();
      continue;
    }
    const auto start = found + key.size();
    const auto end = text.find_first_of("\r\n", start);
    const std::string num = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (std::atoi(num.c_str()) != want) return false;
    any = true;
    pos = found + key.size();
  }
  return any;
}

inline std::size_t find_ini_section_pos(const std::string& text, const char* section) {
  const std::string hdr = std::string("[") + section + "]";
  std::size_t pos = 0;
  while (pos < text.size()) {
    const auto found = text.find(hdr, pos);
    if (found == std::string::npos) return std::string::npos;
    const bool at_line = (found == 0) || text[found - 1] == '\n' || text[found - 1] == '\r';
    if (at_line) return found;
    pos = found + 1;
  }
  return std::string::npos;
}

inline std::size_t ini_section_body_end(const std::string& text, std::size_t section_start) {
  const auto nl = text.find('\n', section_start);
  if (nl == std::string::npos) return text.size();
  std::size_t i = nl + 1;
  while (i < text.size()) {
    if (text[i] == '[') return i;
    const auto e = text.find('\n', i);
    if (e == std::string::npos) return text.size();
    i = e + 1;
  }
  return text.size();
}

// ReShade live section [RenoDX.DLSS5] can omit a key that only exists in the saved
// preset. apply_keyed_int_all then never creates it there, and the addon keeps the default.
inline std::string upsert_keyed_int_in_section(std::string text, const char* section, const std::string& key,
                                               int value) {
  const auto start = find_ini_section_pos(text, section);
  if (start == std::string::npos) return apply_keyed_int(std::move(text), key, value);
  const auto hdr_nl = text.find('\n', start);
  const auto body_start = (hdr_nl == std::string::npos) ? text.size() : hdr_nl + 1;
  const auto body_end = ini_section_body_end(text, start);
  const std::string line = key + std::to_string(value);
  std::size_t pos = body_start;
  while (pos < body_end) {
    const auto found = text.find(key, pos);
    if (found == std::string::npos || found >= body_end) break;
    const bool at_line_start = (found == 0) || text[found - 1] == '\n' || text[found - 1] == '\r';
    if (!at_line_start) {
      pos = found + key.size();
      continue;
    }
    const auto end = text.find_first_of("\r\n", found);
    text.replace(found, (end == std::string::npos ? text.size() : end) - found, line);
    return text;
  }
  std::string insert = line + "\n";
  text.insert(body_end, insert);
  return text;
}

inline std::string apply_keyed_value(std::string text, const std::string& key, const std::string& value) {
  const std::string line = key + value;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const auto found = text.find(key, pos);
    if (found == std::string::npos) break;
    const bool at_line_start = (found == 0) || text[found - 1] == '\n' || text[found - 1] == '\r';
    if (at_line_start) {
      const auto end = text.find_first_of("\r\n", found);
      text.replace(found, (end == std::string::npos ? text.size() : end) - found, line);
      return text;
    }
    pos = found + key.size();
  }
  if (!text.empty() && text.back() != '\n') text.push_back('\n');
  text += line;
  text.push_back('\n');
  return text;
}

inline std::string apply_keyed_value_all(std::string text, const std::string& key, const std::string& value) {
  const std::string line = key + value;
  std::size_t pos = 0;
  bool hit = false;
  while (pos <= text.size()) {
    const auto found = text.find(key, pos);
    if (found == std::string::npos) break;
    const bool at_line_start = (found == 0) || text[found - 1] == '\n' || text[found - 1] == '\r';
    if (!at_line_start) {
      pos = found + key.size();
      continue;
    }
    const auto end = text.find_first_of("\r\n", found);
    text.replace(found, (end == std::string::npos ? text.size() : end) - found, line);
    hit = true;
    pos = found + line.size();
  }
  return hit ? text : apply_keyed_value(std::move(text), key, value);
}

inline std::string upsert_keyed_value_in_section(std::string text, const char* section, const std::string& key,
                                                 const std::string& value) {
  const auto start = find_ini_section_pos(text, section);
  if (start == std::string::npos) return apply_keyed_value(std::move(text), key, value);
  const auto hdr_nl = text.find('\n', start);
  const auto body_start = (hdr_nl == std::string::npos) ? text.size() : hdr_nl + 1;
  const auto body_end = ini_section_body_end(text, start);
  const std::string line = key + value;
  std::size_t pos = body_start;
  while (pos < body_end) {
    const auto found = text.find(key, pos);
    if (found == std::string::npos || found >= body_end) break;
    const bool at_line_start = (found == 0) || text[found - 1] == '\n' || text[found - 1] == '\r';
    if (!at_line_start) {
      pos = found + key.size();
      continue;
    }
    const auto end = text.find_first_of("\r\n", found);
    text.replace(found, (end == std::string::npos ? text.size() : end) - found, line);
    return text;
  }
  text.insert(body_end, line + "\n");
  return text;
}

// Rewrite every existing key=; if none exist, create it in the live addon section.
inline std::string apply_keyed_value_all_live(std::string text, const std::string& key, const std::string& value,
                                             const char* section = "RenoDX.DLSS5") {
  const std::string line = key + value;
  std::size_t pos = 0;
  bool hit = false;
  while (pos <= text.size()) {
    const auto found = text.find(key, pos);
    if (found == std::string::npos) break;
    const bool at_line_start = (found == 0) || text[found - 1] == '\n' || text[found - 1] == '\r';
    if (!at_line_start) {
      pos = found + key.size();
      continue;
    }
    const auto end = text.find_first_of("\r\n", found);
    text.replace(found, (end == std::string::npos ? text.size() : end) - found, line);
    hit = true;
    pos = found + line.size();
  }
  if (hit) return text;
  return upsert_keyed_value_in_section(std::move(text), section, key, value);
}

inline std::string apply_reshade_addon_path(std::string text, int pass_count) {
  return apply_keyed_value(std::move(text), "AddonPath=", renodx_addon_stack_rel(pass_count));
}

inline std::string parse_reshade_addon_path(const std::string& text, const char* fallback = ".\\addons-multipass") {
  const std::string key = "AddonPath=";
  const auto pos = text.find(key);
  if (pos == std::string::npos) return fallback ? fallback : "";
  const auto start = pos + key.size();
  const auto end = text.find_first_of("\r\n", start);
  return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

inline bool reshade_addon_path_is_multipass(const std::string& text) {
  return parse_reshade_addon_path(text).find("multipass") != std::string::npos;
}

inline int parse_keyed_int(const std::string& text, const std::string& key, int fallback) {
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const auto found = text.find(key, pos);
    if (found == std::string::npos) return fallback;
    const bool at_line_start = (found == 0) || text[found - 1] == '\n' || text[found - 1] == '\r';
    if (at_line_start) {
      const auto start = found + key.size();
      const auto end = text.find_first_of("\r\n", start);
      const std::string num = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
      return std::atoi(num.c_str());
    }
    pos = found + key.size();
  }
  return fallback;
}

inline double parse_keyed_number(const std::string& text, const std::string& key, double fallback) {
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const auto found = text.find(key, pos);
    if (found == std::string::npos) return fallback;
    const bool at_line_start = (found == 0) || text[found - 1] == '\n' || text[found - 1] == '\r';
    if (at_line_start) {
      const auto start = found + key.size();
      const auto end = text.find_first_of("\r\n", start);
      const std::string num = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
      char* stop = nullptr;
      const double value = std::strtod(num.c_str(), &stop);
      if (stop == num.c_str()) return fallback;
      return value;
    }
    pos = found + key.size();
  }
  return fallback;
}

// Grava a receita do painel nas chaves que o Krish 8.5 lê. NRStyle e Pass Count ficam de fora.
inline std::string apply_nr_advanced_look(std::string text, const NrAdvancedLook& raw) {
  const NrAdvancedLook a = clamp_nr_advanced(raw);
  const std::string skin_nr = a.skin < 0 ? "-1" : format_nr_decimal(a.skin);
  const std::string skin_direct = a.skin < 0 ? "1" : format_nr_decimal(a.skin);
  auto live = [&](const char* key, const std::string& value) {
    text = apply_keyed_value_all_live(std::move(text), key, value);
  };
  auto all = [&](const char* key, const std::string& value) {
    text = apply_keyed_value_all(std::move(text), key, value);
  };
  live("NRLookMode=", std::to_string(a.look_mode));
  live("NRLookStrength=", format_nr_decimal(a.look_strength));
  live("NRLookBrighten=", format_nr_decimal(a.look_brighten));
  live("NRLookDarken=", format_nr_decimal(a.look_darken));
  live("NRLookColour=", format_nr_decimal(a.look_colour));
  live("NRLookHue=", format_nr_decimal(a.look_hue));
  live("NRLookShadows=", format_nr_decimal(a.look_shadows));
  live("NRLookMidtones=", format_nr_decimal(a.look_midtones));
  live("NRLookHighlights=", format_nr_decimal(a.look_highlights));
  live("NRSkinIndependent=", std::to_string(a.skin_independent));
  live("NRDetailStability=", std::to_string(a.detail_stability));
  all("DirectNeuralRenderingIntensity=", "1");
  all("NRIntensity=", "1");
  all("DirectNeuralRenderingGlobalToneStrength=", "1");
  all("NRGlobalTone=", "1");
  all("DirectNeuralRenderingLocalToneStrength=", format_nr_decimal(a.local_tone));
  all("NRLocalTone=", format_nr_decimal(a.local_tone));
  all("DirectNeuralRenderingLocalStructureStrength=", format_nr_decimal(a.local_structure));
  all("NRLocalStructure=", format_nr_decimal(a.local_structure));
  all("DirectNeuralRenderingSkinStructureStrength=", skin_direct);
  all("NRSkinStructure=", skin_nr);
  all("DirectNeuralRenderingAutoMask=", std::to_string(a.auto_mask));
  all("NRAutoMask=", std::to_string(a.auto_mask));
  all("NRColorStrength=", "1.00");
  all("NRTransferStrength=", "1.00");
  return text;
}

// Receita do 1,5x no Krish 8.5 (NRLook*). Fora dele o look fica neutro e a
// intensidade do passe fica 1 — esse e o 1x.
inline std::string apply_nr_edit_recipe(std::string text, bool plus) {
  return apply_nr_advanced_look(std::move(text), nr_advanced_default(plus ? kNr15xPlusUi : 1));
}

// Capture signal → work resolution after lobby scale slider (25–100%).
// Even dimensions keep chroma-friendly sizes when downsampling.
inline int scaled_extent(int dim, int scale_percent) {
  if (dim <= 0) return 0;
  const int p = clamp_scale_percent(scale_percent);
  int out = (dim * p + 50) / 100;
  if (out < 2) out = 2;
  out &= ~1;  // even
  return out;
}

// v2.0: with RenoDX stack, the app presents native capture; UI scale drives ONLY
// Feed work_resolution. Applying scale in both app RT and Feed double-downsamples
// (soft image) without saving NV12 upload cost.
inline int app_present_scale_percent(bool renodx_stack, int ui_scale_percent) {
  if (renodx_stack) return 100;
  return clamp_scale_percent(ui_scale_percent);
}

// Approximate pixel ratio vs native (for perf messaging / tests). 50% → 0.25; 25% → ~0.0625.
inline double scale_pixel_ratio(int scale_percent) {
  const double s = clamp_scale_percent(scale_percent) / 100.0;
  return s * s;
}

// Measured on dlss5-feed: work_resolution below 50 is rejected and the feature silently
// builds at 100% (overlay then "lies" about the scale). Never write below this floor.
inline constexpr int kMinFeedWorkResolution = 50;

inline int feed_work_upscale_flag(int work_percent, int /*pass_count*/ = 1) {
  return clamp_scale_percent(work_percent) < 100 ? 1 : 0;
}

// Keep RenoDX dlss5-feed.cfg work_resolution in sync with lobby scale (including 100%).
// Bug: only writing when scale<100 left work_resolution=50 stuck → blur at "100%".
// work_upscale=1 when below 100% so Feed actually downsamples then expands back.
// rebuild=1 força o Feed a reler.
inline std::string apply_feed_work_resolution(std::string text, int scale_percent, int pass_count = 1) {
  int p = clamp_scale_percent(scale_percent);
  if (p < kMinFeedWorkResolution) p = kMinFeedWorkResolution;
  text = apply_keyed_int(std::move(text), "work_resolution=", p);
  text = apply_keyed_int(std::move(text), "work_upscale=", feed_work_upscale_flag(p, pass_count));
  text = apply_keyed_int(std::move(text), "rebuild=", 1);
  return text;
}

inline int parse_feed_work_resolution(const std::string& text, int fallback = 100) {
  return clamp_scale_percent(parse_keyed_int(text, "work_resolution=", fallback));
}

inline std::string apply_feed_model_preset(std::string text, int visual_preset, int pass_count = 1) {
  text = apply_keyed_int(std::move(text), "preset=", renodx_feed_model_preset(visual_preset, pass_count));
  // Feed caches preset= like work_resolution — force rebuild so the look entra.
  return apply_keyed_int(std::move(text), "rebuild=", 1);
}

inline int parse_feed_model_preset(const std::string& text, int fallback = 0) {
  return parse_keyed_int(text, "preset=", fallback);
}

// ReShade.ini NRPreset — Natural(0) / Cinematic(1). All sections + live [RenoDX.DLSS5].
inline std::string apply_reshade_nr_preset(std::string text, int visual_preset) {
  const int nr = renodx_nr_preset_index(visual_preset);
  text = apply_keyed_int_all(std::move(text), "NRPreset=", nr);
  return upsert_keyed_int_in_section(std::move(text), "RenoDX.DLSS5", "NRPreset=", nr);
}

// NRPreset fica no knob 0/1. NRStyle é o combo do mod: Default/Natural/Cinematic = 0/1/2.
inline std::string apply_reshade_visual_look_keys(std::string text, int visual_preset) {
  text = apply_reshade_nr_preset(std::move(text), visual_preset);
  const int style = renodx_nr_style_index(visual_preset);
  text = apply_keyed_int_all(std::move(text), "NRStyle=", style);
  text = upsert_keyed_int_in_section(std::move(text), "RenoDX.DLSS5", "NRStyle=", style);
  text = apply_keyed_int_all(std::move(text), "DirectNeuralRenderingStyle=", style);
  text = upsert_keyed_int_in_section(std::move(text), "RENODX-DLSS-preset1",
                                    "DirectNeuralRenderingStyle=", style);
  text = upsert_keyed_int_in_section(std::move(text), "RenoDX.DLSS5",
                                    "DirectNeuralRenderingStyle=", style);
  return text;
}

inline int parse_reshade_nr_preset(const std::string& text, int fallback = 0) {
  return parse_keyed_int(text, "NRPreset=", fallback);
}

// Multipass stack — DirectNeuralRenderingPassCount (addons-multipass).
// Lives in both [RENODX-DLSS-preset*] and the live [RenoDX.DLSS5] — all must match.
inline std::string apply_reshade_nr_pass_count(std::string text, int passes) {
  const int n = nr_reshade_pass_count(passes);
  text = apply_keyed_int_all(std::move(text), "DirectNeuralRenderingPassCount=", n);
  text = upsert_keyed_int_in_section(std::move(text), "RenoDX.DLSS5", "NRPasses=", n);
  return apply_keyed_int_all(std::move(text), "NRPasses=", n);
}

inline int parse_reshade_nr_pass_count(const std::string& text, int fallback = 1) {
  int raw = parse_keyed_int(text, "DirectNeuralRenderingPassCount=", -1);
  if (raw < 0) raw = parse_keyed_int(text, "NRPasses=", fallback);
  if (raw < 1) return 1;
  if (raw > 3) return 3;  // INI never stores 4; 4 is UI-only (1.5x → INI 1)
  return raw;
}

// Home overlay layout. Extra RenoDX panels stay off-screen — Collapsed=1 still shows a title bar.
inline constexpr const char* kReshadeVisibleImguiGeom = "Pos=8,,8,Size=895,,1296,Collapsed=0";
inline constexpr const char* kReshadeHiddenImguiGeom = "Pos=-8000,,-8000,Size=1,,1,Collapsed=1";
inline constexpr const char* kReshadeHomeKrishTitle = "RenoDX DLSS5 Generic";
// Krish v4.7 register_overlay title is the C-string with a trailing space (21 chars).
inline constexpr const char* kReshadeHomeKrishTitleSpaced = "RenoDX DLSS5 Generic ";
inline constexpr const char* kReshadeHomeMultipassTitle = "RenoDX DLSS";

inline constexpr const char* kReshadeKrishSettingsOverlay =
    "DLSS 5 Neural Rendering@renodx-dlss5.addon64";
inline constexpr const char* kReshadeChromeDocked0 =
    "Pos=931,,128,Size=895,,1296,Collapsed=0,DockId=0x00000001,,0";
inline constexpr const char* kReshadeChromeDocked1 =
    "Pos=931,,128,Size=895,,1296,Collapsed=0,DockId=0x00000001,,1";
inline constexpr const char* kReshadeChromeDocked2 =
    "Pos=931,,128,Size=895,,1296,Collapsed=0,DockId=0x00000001,,2";
inline constexpr const char* kReshadeChromeDocked3 =
    "Pos=931,,128,Size=895,,1296,Collapsed=0,DockId=0x00000001,,3";
inline constexpr const char* kReshadeChromeDocked4 =
    "Pos=931,,128,Size=895,,1296,Collapsed=0,DockId=0x00000001,,4";
inline constexpr const char* kReshadeChromeDocked5 =
    "Pos=931,,128,Size=895,,1296,Collapsed=0,DockId=0x00000001,,5";

inline std::string reshade_imgui_window_token(const char* title, const char* geom) {
  std::string t = "[Window][";
  t += title ? title : "";
  t += "],";
  t += geom ? geom : kReshadeHiddenImguiGeom;
  return t;
}

inline std::string reshade_imgui_window_token(const char* title, bool visible) {
  return reshade_imgui_window_token(title, visible ? kReshadeVisibleImguiGeom : kReshadeHiddenImguiGeom);
}

inline std::string upsert_reshade_imgui_window(std::string text, const char* title, const char* geom) {
  if (!title || !title[0]) return text;
  const std::string token = reshade_imgui_window_token(title, geom);
  const std::string marker = std::string("[Window][") + title + "]";

  std::size_t pos = 0;
  while ((pos = text.find(marker, pos)) != std::string::npos) {
    const auto next = text.find("[Window][", pos + marker.size());
    const auto nl = text.find_first_of("\r\n", pos);
    std::size_t end = text.size();
    if (next != std::string::npos && (nl == std::string::npos || next < nl))
      end = next;
    else if (nl != std::string::npos)
      end = nl;
    text.replace(pos, end - pos, token);
    const auto after = pos + token.size();
    if (after < text.size() && text.compare(after, 8, "[Window]") == 0) text.insert(after, ",");
    pos = after;
  }

  pos = 0;
  bool saw_window = false;
  while ((pos = text.find("Window=", pos)) != std::string::npos) {
    const bool at_line = (pos == 0) || text[pos - 1] == '\n' || text[pos - 1] == '\r';
    if (!at_line) {
      pos += 7;
      continue;
    }
    saw_window = true;
    const auto line_end = text.find_first_of("\r\n", pos);
    const std::size_t end = line_end == std::string::npos ? text.size() : line_end;
    const std::string line = text.substr(pos, end - pos);
    if (line.find(marker) == std::string::npos) {
      std::size_t insert_at = end;
      if (insert_at > pos + 7) {
        const char prev = text[insert_at - 1];
        if (prev != '=' && prev != ',') {
          text.insert(insert_at, ",");
          ++insert_at;
        }
      }
      text.insert(insert_at, token);
      pos = insert_at + token.size();
    } else {
      pos = end == std::string::npos ? text.size() : end + 1;
    }
  }

  if (!saw_window) {
    if (!text.empty() && text.back() != '\n') text.push_back('\n');
    text += "Window=";
    text += token;
    text.push_back('\n');
  }
  return text;
}

inline std::string upsert_reshade_imgui_window(std::string text, const char* title, bool visible) {
  return upsert_reshade_imgui_window(std::move(text), title,
                                    visible ? kReshadeVisibleImguiGeom : kReshadeHiddenImguiGeom);
}

inline std::string collapse_reshade_imgui_window(std::string text, const char* title) {
  return upsert_reshade_imgui_window(std::move(text), title, /*visible=*/false);
}

// ReShade 6.6 Begin() titles are "Label###id" (locale + id). Bare "###home" is a ghost
// key: DockId/Selected attach to windows that never exist, so Home falls back to Início.
inline constexpr const char kReshadeWinHomePt[] = "In\xC3\xAD" "cio###home";
inline constexpr const char kReshadeWinHomeEn[] = "Home###home";
inline constexpr const char kReshadeWinAddonsPt[] = "Complementos###addons";
inline constexpr const char kReshadeWinAddonsEn[] = "Add-ons###addons";
inline constexpr const char kReshadeWinSettingsPt[] = "Configura\xC3\xA7\xC3\xB5" "es###settings";
inline constexpr const char kReshadeWinSettingsEn[] = "Settings###settings";
inline constexpr const char kReshadeWinStatsPt[] = "Estat\xC3\xAD" "sticas###statistics";
inline constexpr const char kReshadeWinStatsEn[] = "Statistics###statistics";
inline constexpr const char kReshadeWinAboutPt[] = "Sobre###about";
inline constexpr const char kReshadeWinAboutEn[] = "About###about";

inline std::string upsert_reshade_overlay_chrome(std::string text, bool addons_first = false) {
  const char* g_home = addons_first ? kReshadeChromeDocked1 : kReshadeChromeDocked0;
  const char* g_addons = addons_first ? kReshadeChromeDocked0 : kReshadeChromeDocked1;
  const char* home_names[] = {"###home", kReshadeWinHomeEn, kReshadeWinHomePt};
  const char* addons_names[] = {"###addons", kReshadeWinAddonsEn, kReshadeWinAddonsPt};
  const char* settings_names[] = {"###settings", kReshadeWinSettingsEn, kReshadeWinSettingsPt};
  const char* stats_names[] = {"###statistics", kReshadeWinStatsEn, kReshadeWinStatsPt};
  const char* log_names[] = {"###log", "Log###log"};
  const char* about_names[] = {"###about", kReshadeWinAboutEn, kReshadeWinAboutPt};
  for (const char* n : home_names) text = upsert_reshade_imgui_window(std::move(text), n, g_home);
  for (const char* n : addons_names) text = upsert_reshade_imgui_window(std::move(text), n, g_addons);
  for (const char* n : settings_names)
    text = upsert_reshade_imgui_window(std::move(text), n, kReshadeChromeDocked2);
  for (const char* n : stats_names)
    text = upsert_reshade_imgui_window(std::move(text), n, kReshadeChromeDocked3);
  for (const char* n : log_names) text = upsert_reshade_imgui_window(std::move(text), n, kReshadeChromeDocked4);
  for (const char* n : about_names)
    text = upsert_reshade_imgui_window(std::move(text), n, kReshadeChromeDocked5);
  return text;
}

// ImHashStr("addons") with ReShade 6.6 CRC32C — Complementos tab.
inline constexpr const char* kReshadeDockingSelectAddons =
    "[Docking][Data],DockNode  ID=0x00000001 Pos=931,,128 Size=895,,1296 Selected=0x84F87037,"
    "DockSpace ID=0xB0DF600F Window=0xCC18005E Pos=8,,8 Size=2544,,1424 CentralNode=1";

// Sem o toast do ReShade ("Press Home to open the overlay" / "aperte Home").
// 6.8 le TutorialProgress em [OVERLAY]. ShowStartupMessage nao existe nesse binario.
inline std::string apply_reshade_quiet_overlay(std::string text) {
  text = upsert_keyed_value_in_section(std::move(text), "OVERLAY", "ShowStartupMessage=", "0");
  text = upsert_keyed_value_in_section(std::move(text), "OVERLAY", "ShowOSD=", "0");
  text = upsert_keyed_value_in_section(std::move(text), "OVERLAY", "ShowPresetTransitionMessage=", "0");
  text = upsert_keyed_value_in_section(std::move(text), "OVERLAY", "ShowScreenshotMessage=", "0");
  text = upsert_keyed_value_in_section(std::move(text), "OVERLAY", "ShowFPS=", "0");
  text = upsert_keyed_value_in_section(std::move(text), "OVERLAY", "ShowClock=", "0");
  text = upsert_keyed_value_in_section(std::move(text), "OVERLAY", "ShowFrameTime=", "0");
  text = upsert_keyed_value_in_section(std::move(text), "OVERLAY", "ShowPresetName=", "0");
  text = upsert_keyed_value_in_section(std::move(text), "OVERLAY", "TutorialProgress=", "4");
  text = upsert_keyed_value_in_section(std::move(text), "GENERAL", "TutorialProgress=", "4");
  return text;
}

// ReShade 6.8: o aviso "Pressione Home..." e o splash. O byte apontado e o
// salto que esconde a faixa (74 = so quando o flag esta off, EB = sempre escondido).
inline size_t reshade68_home_splash_gate_offset(const uint8_t* data, size_t n, uint8_t gate = 0x74) {
  static constexpr uint8_t kBefore[] = {
      0x48, 0xBA, 0x00, 0xF2, 0x05, 0x2A, 0x01, 0x00, 0x00, 0x00,
      0x44, 0x38, 0xAE, 0xF0, 0x04, 0x00, 0x00,
  };
  static constexpr uint8_t kMid[] = {0x50, 0x48, 0x8B, 0xCE, 0xE8};
  static constexpr uint8_t kTail[] = {
      0x84, 0xC0, 0x75, 0x34, 0x83, 0xBE, 0x08, 0x05, 0x00, 0x00, 0x01, 0x77, 0x13,
      0x48, 0x8B, 0x86, 0xC8, 0x00, 0x00, 0x00, 0x48, 0x2B, 0x86, 0x50, 0x02, 0x00, 0x00, 0x48, 0x3B, 0xC2,
  };
  constexpr size_t kRel = 4;
  const size_t need = sizeof(kBefore) + 1 + sizeof(kMid) + kRel + sizeof(kTail) + 2;
  if (!data || n < need) return static_cast<size_t>(-1);
  for (size_t i = 0; i + need <= n; ++i) {
    if (std::memcmp(data + i, kBefore, sizeof(kBefore)) != 0) continue;
    if (data[i + sizeof(kBefore)] != gate) continue;
    if (std::memcmp(data + i + sizeof(kBefore) + 1, kMid, sizeof(kMid)) != 0) continue;
    const size_t tail = i + sizeof(kBefore) + 1 + sizeof(kMid) + kRel;
    if (std::memcmp(data + tail, kTail, sizeof(kTail)) != 0) continue;
    const uint8_t b0 = data[tail + sizeof(kTail)];
    const uint8_t b1 = data[tail + sizeof(kTail) + 1];
    const bool open = (b0 == 0x7C && b1 == 0x18) || (b0 == 0x90 && b1 == 0x90);
    if (!open) continue;
    return i + sizeof(kBefore);
  }
  return static_cast<size_t>(-1);
}

// Desliga o aviso de Home no dxgi do ReShade ja carregado. Nao altera o arquivo.
bool silence_reshade_home_splash();

// Complete addon overlay of the active motor (cena / Pass Count / knobs).
// Krish 8.5 regista "DLSS 5 Neural Rendering". Títulos V6.6/ShortFuse ficam fora do ecrã.
inline std::string sanitize_home_windows(std::string text, bool /*multipass*/) {
  text = upsert_reshade_imgui_window(std::move(text), kReshadeHomeKrishTitle, false);
  text = upsert_reshade_imgui_window(std::move(text), kReshadeHomeKrishTitleSpaced, false);
  text = upsert_reshade_imgui_window(std::move(text), "DLSS 5 Neural Rendering", true);
  text = upsert_reshade_imgui_window(std::move(text), kReshadeHomeMultipassTitle, false);
  text = upsert_reshade_imgui_window(std::move(text), "RenoDX-DLSSNR", false);
  text = upsert_reshade_imgui_window(std::move(text), "RenoDX Neural Resolution", false);
  text = upsert_reshade_overlay_chrome(std::move(text), /*addons_first=*/true);
  text = apply_keyed_value(std::move(text), "OverlayCollapsed=", "");
  text = apply_keyed_value(std::move(text), "Docking=", kReshadeDockingSelectAddons);
  text = apply_reshade_quiet_overlay(std::move(text));
  return text;
}

inline std::string sanitize_multipass_home_windows(std::string text) {
  return sanitize_home_windows(std::move(text), true);
}

inline std::string sanitize_home_windows_for_pass(std::string text, int pass_count) {
  return sanitize_home_windows(std::move(text), nr_pass_count_uses_multipass_addon(pass_count));
}

// Align DirectNeuralRendering* keys. Todo X vivo usa o gancho otimizado.
inline std::string apply_reshade_multipass_look_defaults(std::string text, int pass_count,
                                                         int visual_preset = 0,
                                                         const NrAdvancedLook* custom = nullptr) {
  const bool opt = nr_pass_uses_optimized_look(pass_count);
  const int nits = opt ? 100 : 500;
  const int encoding = opt ? 2 : 0;
  const int hook = opt ? 2 : 1;
  const NrAdvancedLook look = custom ? clamp_nr_advanced(*custom) : nr_advanced_default(pass_count);
  const int ui_corr = look.ui_correction;
  // [RENODX-DLSS] global + live [RenoDX.DLSS5]. O 8.5 lê NR*; Direct* fica alinhado em todas as secções.
  text = apply_keyed_int_all(std::move(text), "DirectNeuralRenderingDiffuseWhiteNits=", nits);
  text = apply_keyed_int_all(std::move(text), "DirectNeuralRenderingDiffuseWhiteOverride=", 0);
  text = apply_keyed_int_all(std::move(text), "DirectNeuralRenderingEncoding=", encoding);
  text = apply_keyed_int_all(std::move(text), "DirectNeuralRenderingHookPoint=", hook);
  text = apply_keyed_int_all(std::move(text), "DirectNeuralRenderingHookStage=", 0);
  text = apply_keyed_int_all(std::move(text), "DirectNeuralRenderingRequireDlss=", 1);
  text = apply_keyed_int_all(std::move(text), "DirectNeuralRenderingUiCorrectionMode=", ui_corr);
  text = upsert_keyed_int_in_section(std::move(text), "RenoDX.DLSS5", "NRSourceEncoding=", encoding);
  text = upsert_keyed_int_in_section(std::move(text), "RenoDX.DLSS5", "NRHookPoint=", hook);
  text = upsert_keyed_int_in_section(std::move(text), "RenoDX.DLSS5", "NRDiffuseWhiteNits=", nits);
  text = upsert_keyed_int_in_section(std::move(text), "RenoDX.DLSS5", "NRUICorrection=", ui_corr);
  text = upsert_keyed_int_in_section(std::move(text), "RenoDX.DLSS5", "ConfigVersion=", 8);
  text = apply_keyed_int_all(std::move(text), "NRSourceEncoding=", encoding);
  text = apply_keyed_int_all(std::move(text), "NRHookPoint=", hook);
  text = apply_keyed_int_all(std::move(text), "NRDiffuseWhiteNits=", nits);
  text = apply_keyed_int_all(std::move(text), "NRUICorrection=", ui_corr);
  // [RENODX-DLSS-preset1] look knobs. A receita salva do X; sem ponteiro, a fábrica.
  text = apply_keyed_int(std::move(text), "DirectNeuralRenderingAutoMask=", look.auto_mask);
  text = apply_nr_advanced_look(std::move(text), look);
  text = apply_reshade_nr_pass_count(std::move(text), pass_count);
  // Keep legacy NR* block in sync for tools / dual-read / Krish.
  text = apply_keyed_int(std::move(text), "NRDiffuseWhiteNits=", nits);
  text = apply_keyed_int(std::move(text), "NRAutoMask=", look.auto_mask);
  text = apply_keyed_int(std::move(text), "NRUICorrection=", ui_corr);
  text = apply_reshade_visual_look_keys(std::move(text), visual_preset);
  {
    const int style = renodx_direct_style_index(visual_preset, pass_count);
    text = apply_keyed_int_all(std::move(text), "DirectNeuralRenderingStyle=", style);
    text = upsert_keyed_int_in_section(std::move(text), "RENODX-DLSS-preset1",
                                      "DirectNeuralRenderingStyle=", style);
    text = upsert_keyed_int_in_section(std::move(text), "RenoDX.DLSS5",
                                      "DirectNeuralRenderingStyle=", style);
  }
  // Multipass: Delete is the real NRToggleKey (Bridge cannot inject GetAsyncKeyState).
  text = apply_keyed_int(std::move(text), "NRToggleKey=", 0x2E);
  text = sanitize_multipass_home_windows(std::move(text));
  return text;
}

// UI 5 writes the same PassCount=2 as UI 2. Infer from exclusive look keys so a
// visual-preset-only write does not wipe 2x otimizado back to 2x default.
inline bool reshade_look_is_2x_optimized(const std::string& text) {
  const int encoding = parse_keyed_int(text, "DirectNeuralRenderingEncoding=",
                                      parse_keyed_int(text, "NRSourceEncoding=", 0));
  const int hook = parse_keyed_int(text, "DirectNeuralRenderingHookPoint=",
                                  parse_keyed_int(text, "NRHookPoint=", 1));
  const int ui = parse_keyed_int(text, "DirectNeuralRenderingUiCorrectionMode=",
                                parse_keyed_int(text, "NRUICorrection=", 1));
  return encoding == 2 && hook == 2 && ui == 2;
}

inline int infer_multipass_ui_pass_count(const std::string& text) {
  const int raw = parse_reshade_nr_pass_count(text, 1);
  if (reshade_look_is_2x_optimized(text)) {
    const bool plus = parse_keyed_number(text, "NRLookStrength=", 1.0) > 1.01 ||
                      parse_keyed_number(text, "DirectNeuralRenderingIntensity=", 1.0) > 1.01;
    if (raw == 2) return 5;
    if (raw == 3) return 6;
    if (raw == 1) return plus ? 4 : 1;
  }
  return raw;
}

// INI 1 is both 1x and 1,5x: infer the UI slot from the look keys.
// pass_count >= 0: honor the UI slot (needed to keep 2x otimizado when INI is also 2).
inline std::string apply_reshade_look_for_current_stack(std::string text, int visual_preset,
                                                         int pass_count = -1,
                                                         const NrAdvancedLook* custom = nullptr) {
  const int ui = pass_count >= 0 ? clamp_nr_pass_count(pass_count) : infer_multipass_ui_pass_count(text);
  const NrAdvancedLook look = custom ? clamp_nr_advanced(*custom) : nr_advanced_default(ui);
  return apply_reshade_multipass_look_defaults(std::move(text), ui, visual_preset, &look);
}

// After gear Salvar / session start: feed + ReShade must match settings.renodx_preset.
// pass_count < 0: infer the UI slot from the ReShade look keys.
inline bool renodx_files_reflect_visual_preset(const std::string& feed_text, const std::string& reshade_text,
                                               int visual_preset, int pass_count = -1) {
  const int v = clamp_renodx_visual_preset(visual_preset);
  const int nr = renodx_nr_preset_index(v);
  const int p = pass_count >= 0 ? clamp_nr_pass_count(pass_count) : infer_multipass_ui_pass_count(reshade_text);
  if (parse_feed_model_preset(feed_text, -1) != renodx_feed_model_preset(v, p)) return false;
  if (!every_keyed_int_is(reshade_text, "NRPreset=", nr)) return false;
  if (!every_keyed_int_is(reshade_text, "NRStyle=", renodx_nr_style_index(v))) return false;
  if (!every_keyed_int_is(reshade_text, "DirectNeuralRenderingStyle=",
                               renodx_direct_style_index(v, p))) {
    return false;
  }
  return true;
}

// DLSS5MGF.ini: Kernel (MV) -> Feed (DLSS) -> Sharpen. Sharpen/Feed before Kernel = cintilacao.
inline bool renodx_mgf_technique_chain_ok(const std::string& chain) {
  const auto k = chain.find("Lumenite_Kernel@lumenite_Kernel.fx");
  const auto f = chain.find("DLSS5_Feed@DLSS5_Feed.fx");
  const auto s = chain.find("DLSS5_Sharpen@DLSS5_Sharpen.fx");
  if (k == std::string::npos || f == std::string::npos || s == std::string::npos) return false;
  return k < f && f < s;
}

inline constexpr const char* kRenodxSessionTechniqueChain =
    "Lumenite_Kernel@lumenite_Kernel.fx,DLSS5_Feed@DLSS5_Feed.fx,DLSS5_Sharpen@DLSS5_Sharpen.fx";

// Lobby with RenoDX off: empty Techniques so Present stays light until Iniciar / preview ON.
inline constexpr const char* kRenodxLobbyIdleTechniqueChain = "";

inline std::string apply_mgf_techniques(std::string text, const char* chain) {
  const char* c = chain ? chain : kRenodxSessionTechniqueChain;
  text = apply_keyed_value(std::move(text), "Techniques=", c);
  text = apply_keyed_value(std::move(text), "TechniqueSorting=", c);
  return text;
}

inline std::string apply_mgf_techniques_for_phase(std::string text, bool session_or_live_preview) {
  return apply_mgf_techniques(std::move(text),
                              session_or_live_preview ? kRenodxSessionTechniqueChain
                                                      : kRenodxLobbyIdleTechniqueChain);
}

inline std::string apply_mgf_sharpen_for_pass(std::string text, int /*pass_count*/) {
  return apply_keyed_value(std::move(text), "Sharpness=", kDefaultMgfSharpness);
}

inline bool renodx_mgf_chain_order_ok(const std::string& mgf_ini_or_script) {
  auto value_after = [&](const char* key) -> std::string {
    const std::string k(key);
    std::size_t pos = 0;
    while (pos < mgf_ini_or_script.size()) {
      const auto line_end = mgf_ini_or_script.find('\n', pos);
      const auto end = line_end == std::string::npos ? mgf_ini_or_script.size() : line_end;
      std::string line = mgf_ini_or_script.substr(pos, end - pos);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.size() >= k.size() && line.compare(0, k.size(), k) == 0) {
        return line.substr(k.size());
      }
      pos = end == mgf_ini_or_script.size() ? end : end + 1;
    }
    return {};
  };
  const std::string techniques = value_after("Techniques=");
  const std::string sorting = value_after("TechniqueSorting=");
  if (techniques.empty() || sorting.empty()) return false;
  return renodx_mgf_technique_chain_ok(techniques) && renodx_mgf_technique_chain_ok(sorting);
}

inline std::string apply_renodx_visual_preset_to_feed(std::string feed, int visual_preset,
                                                    int pass_count = 1) {
  feed = apply_feed_model_preset(std::move(feed), visual_preset, pass_count);
  return apply_keyed_value(std::move(feed), "work_sharpness=", "0.30");
}

inline std::string apply_renodx_visual_preset_to_reshade(std::string reshade, int visual_preset) {
  return apply_reshade_visual_look_keys(std::move(reshade), visual_preset);
}

// NeuralUplift in ReShade.ini [RenoDX.DLSS5] — 1=NR on, 0=off (Frame Gen is independent).
inline std::string apply_reshade_neural_uplift(std::string text, bool enabled) {
  return apply_keyed_int(std::move(text), "NeuralUplift=", enabled ? 1 : 0);
}

inline bool parse_reshade_neural_uplift(const std::string& text, bool fallback = true) {
  return parse_keyed_int(text, "NeuralUplift=", fallback ? 1 : 0) != 0;
}

// Addon NR toggle key (ReShade keycode). Bridge injects F13; Delete calls Bridge.
inline std::string apply_reshade_nr_toggle_key(std::string text, int vk) {
  return apply_keyed_int(std::move(text), "NRToggleKey=", vk);
}

inline int parse_reshade_nr_toggle_key(const std::string& text, int fallback = 0x2E) {
  return parse_keyed_int(text, "NRToggleKey=", fallback);
}

// dlss5-feed.cfg enabled=. No lobby o feed fica armado com RenoDX (não há restart).
// Na sessão o Delete reinicia: feed segue o NeuralUplift — senão o DLSS5 continua a avaliar.
inline std::string apply_feed_enabled(std::string text, bool enabled) {
  return apply_keyed_int(std::move(text), "enabled=", enabled ? 1 : 0);
}

inline bool parse_feed_enabled(const std::string& text, bool fallback = true) {
  return parse_keyed_int(text, "enabled=", fallback ? 1 : 0) != 0;
}

// Pure: when Renodx stack is loaded, feed stays on even if NR is off.
inline bool feed_pipeline_on_for_renodx(bool renodx_loaded) { return renodx_loaded; }

// Lobby live preview: old max_w=640 looked soft; 2560 forced full 1440p CPU
// NV12→RGBA every frame and pinned the lobby ~30 fps (Present vsync + decode).
// 1280 keeps the preview band sharp enough while the UI can hold ~60.
inline constexpr int kLobbyPreviewMaxWidth = 1280;

// HDMI lobby stays at 1280. Perf preview keeps the plan width so 1440/4K
// do not collapse to a 1080-looking band (1920 from 4K step-2, or 1280 from 1440).
inline int lobby_preview_max_width_for_mode(int plan_w, bool perf_preview) {
  if (!perf_preview) return kLobbyPreviewMaxWidth;
  return plan_w > 0 ? plan_w : kLobbyPreviewMaxWidth;
}

inline int lobby_preview_downsample_step(int frame_w, int max_w = kLobbyPreviewMaxWidth) {
  if (frame_w <= 0) return 1;
  if (frame_w <= max_w) return 1;
  int step = (frame_w + max_w - 1) / max_w;
  if (step < 1) step = 1;
  if (step & 1) ++step;  // even for NV12 chroma
  return step;
}

inline int lobby_preview_out_width(int frame_w, int max_w = kLobbyPreviewMaxWidth) {
  return (std::max)(1, frame_w / lobby_preview_downsample_step(frame_w, max_w));
}

// ReShade dxgi.dll + dlss5-feed hook Present for the whole process (lobby + session).
// The Feed locks work_resolution when the first swapchain appears (the lobby's) and keeps
// it for the whole process, and it downsamples everything it presents — ImGui menus and
// HUD included. So the lobby process stays at 100% (sharp menus) and a scaled session runs
// in a relaunched process (--autostart), whose first swapchain is the capture window.
inline constexpr int kLobbyFeedWorkResolution = 100;

inline int feed_work_resolution_for_session(int scale_percent, int /*pass_count*/ = 1, int /*capture_w*/ = 0,
                                           int /*capture_h*/ = 0) {
  const int p = clamp_scale_percent(scale_percent);
  return p < kMinFeedWorkResolution ? kMinFeedWorkResolution : p;
}

// What the pipeline really renders at: Feed floor under RenoDX, app RT scale otherwise.
inline int effective_scale_percent(bool renodx_stack, int ui_scale_percent, int pass_count = 1,
                                   int capture_w = 0, int capture_h = 0) {
  return renodx_stack ? feed_work_resolution_for_session(ui_scale_percent, pass_count, capture_w, capture_h)
                      : clamp_scale_percent(ui_scale_percent);
}

// True when the UI asks for less than the Feed can do (25–49% under RenoDX).
inline bool feed_scale_below_floor(bool renodx_stack, int ui_scale_percent) {
  return renodx_stack && clamp_scale_percent(ui_scale_percent) < kMinFeedWorkResolution;
}

// Feed snapshots work_resolution at addon load, so a scale picked in the lobby only lands
// after a relaunch that re-seeds the cfg first.
// Without RenoDX the renderer scales its own RT — no restart needed.
inline bool feed_scale_change_needs_relaunch(bool renodx_stack, int loaded_scale, int want_scale,
                                             int pass_count = 1, int capture_w = 0, int capture_h = 0) {
  if (!renodx_stack) return false;
  // Compare what the Feed actually builds at — 40% and 33% both floor to 50%, no restart.
  return feed_work_resolution_for_session(loaded_scale, pass_count, capture_w, capture_h) !=
         feed_work_resolution_for_session(want_scale, pass_count, capture_w, capture_h);
}

// Esc/X/timer do Benchmark: o processo ainda tem Pass/tom/NR/Feed/LS do teste na RAM.
// Gravar o HDMI no disco nao descarrega o addon — precisa processo novo.
inline bool perf_test_return_needs_fresh_process(bool was_perf_test) { return was_perf_test; }

// O Feed e o RenoDX só leem NeuralUplift, enabled e a cadeia MGF no nascimento do processo.
// O lobby grava isso desligado. Iniciar no mesmo processo deixa o CFG e o overlay em ON
// com a imagem ainda OFF — o Delete que religa só funciona porque reinicia.
inline bool lobby_iniciar_needs_session_relaunch(bool preview_enabled, bool preview_cfg_live,
                                                bool enhance_renodx) {
  (void)preview_enabled;
  (void)preview_cfg_live;
  return enhance_renodx;
}

// Menus must never inherit the session scale — the Feed would blur the whole lobby.
inline int feed_work_resolution_for_lobby() { return kLobbyFeedWorkResolution; }

// work_resolution the process should boot with: session scale only when skipping the lobby.
inline int feed_work_resolution_at_boot(bool lobby_first, int ui_scale_percent, int pass_count = 1,
                                        int capture_w = 0, int capture_h = 0) {
  if (lobby_first) return feed_work_resolution_for_lobby();
  return feed_work_resolution_for_session(ui_scale_percent, pass_count, capture_w, capture_h);
}

// Quick health for lobby first-run / Iniciar (exe_dir = folder next to NexusCap.exe).
struct RuntimeHealth {
  bool writable = false;
  bool has_dxgi = false;
  bool has_renodx = false;
  bool has_feed = false;
  bool renodx_complete() const { return has_dxgi && has_renodx && has_feed; }
};

RuntimeHealth check_runtime_health(const std::string& exe_dir);
bool open_app_logs_folder();  // ShellExecute logs dir

// Back to 100% so the next lobby process presents menus at full resolution.
inline std::string apply_lobby_feed_work_resolution(std::string text) {
  return apply_feed_work_resolution(std::move(text), feed_work_resolution_for_lobby());
}

// True when the cfg still carries a session scale that would soften the lobby Present.
inline bool feed_leaks_session_scale_into_lobby(const std::string& feed_text) {
  return parse_feed_work_resolution(feed_text, kLobbyFeedWorkResolution) != kLobbyFeedWorkResolution;
}

HotkeyConfig default_hotkeys();
PersistedSettings default_settings();

inline constexpr const char kPortableSentinelName[] = "portable.txt";

inline bool want_portable_mode(bool sentinel_present, bool portable_cli) {
  return sentinel_present || portable_cli;
}

// Installed: localappdata\NexusCap. Portable: exe_dir (empty exe_dir falls back).
inline std::string resolve_app_data_dir(const std::string& exe_dir, const std::string& localappdata,
                                        bool portable) {
  if (portable && !exe_dir.empty()) return exe_dir;
  if (localappdata.empty()) return {};
  return localappdata + "\\NexusCap";
}

// Relaunch must keep --portable when the zip/CLI opted in (sentinel file is enough on its own).
inline std::string portable_relaunch_args(const char* extra, bool portable) {
  const char* e = extra ? extra : "";
  if (!portable) return std::string(e);
  if (!e[0]) return std::string("--portable");
  return std::string("--portable ") + e;
}

inline const char* portable_data_dir_hint() {
  return "Modo portable: settings, presets e logs nesta pasta (ao lado do exe).";
}

// Call once after parse_args so --portable wins before the first settings/log write.
void init_app_paths(bool portable_cli);
bool app_is_portable();

// Installed: %LOCALAPPDATA%\NexusCap. Portable: pasta do exe. Created if missing.
std::string app_data_dir();
std::string settings_default_path();  // app_data_dir()\settings.ini
std::string app_logs_dir();          // app_data_dir()\logs
std::string app_reshade_cache_dir(); // app_data_dir()\reshade-cache
std::string app_presets_dir();       // app_data_dir()\presets

// Último frame da sessão, para o aviso GDI não ir a preto no restart do motor.
inline std::string motor_hold_bmp_path_in(const std::string& data_dir) {
  if (data_dir.empty()) return {};
  return data_dir + "\\motor_hold.bmp";
}
inline std::string motor_hold_ready_path_in(const std::string& data_dir) {
  if (data_dir.empty()) return {};
  return data_dir + "\\motor_hold.ready";
}
inline std::string motor_hold_bmp_path() { return motor_hold_bmp_path_in(app_data_dir()); }
inline std::string motor_hold_ready_path() { return motor_hold_ready_path_in(app_data_dir()); }

// True when we can create/overwrite a probe file in dir (Program Files fails without admin).
bool directory_is_writable(const std::string& dir);

// Point ReShade IntermediateCachePath at a writable AppData cache; ensure folder exists.
// Returns false if exe_dir itself is not writable (presets/NR cannot be saved).
bool prepare_renodx_runtime_dirs(const std::string& exe_dir);

bool save_settings(const PersistedSettings& s, const std::string& path, std::string& err);
bool load_settings(PersistedSettings& s, const std::string& path, std::string& err);

// Rewrite dlss5-feed.cfg work_resolution on disk. false if missing/unreadable.
bool sync_feed_work_resolution_file(const std::string& path, int scale_percent, int pass_count = 1);

// Feed lives under AddonPath (addons-multipass); keep root + stack in sync.
bool sync_feed_work_resolution_in_dir(const std::string& exe_dir, int scale_percent, int pass_count = 1);

// DLSS5MGF.ini Techniques: full chain for session/live preview, empty for idle lobby.
bool sync_mgf_techniques_for_phase(const std::string& exe_dir, bool session_or_live_preview);

// Write renodx_preset into dlss5-feed.cfg + ReShade.ini next to the exe.
// Feed model = visual index 0/1/2 (Model A/B/C) on every Pass Count.
bool sync_renodx_visual_preset_files(const std::string& exe_dir, int visual_preset, int pass_count = 1,
                                      const NrAdvancedLook* look = nullptr);

// Pass Count + multipass look defaults (DirectNeural* + legacy NR*) in ReShade.ini.
// look == nullptr grava a fábrica daquele X.
bool sync_nr_pass_count_files(const std::string& exe_dir, int pass_count, int visual_preset = 0,
                              const NrAdvancedLook* look = nullptr);

// Persist Neural Rendering on/off (independent of Frame Gen). Updates feed+ReShade.
bool sync_neural_rendering_enabled_files(const std::string& exe_dir, bool nr_on, bool feed_on);

// CFG do lobby → disco do RenoDX/Feed, na ordem que o addon le no boot da sessao.
inline bool sync_persisted_session_files(const std::string& exe_dir, const PersistedSettings& s) {
  const bool want_nr = s.enhance_renodx && s.neural_rendering;
  (void)sync_nr_pass_count_files(exe_dir, s.nr_pass_count, s.renodx_preset,
                                 &s.nr_advanced[clamp_nr_pass_count(s.nr_pass_count)]);
  (void)sync_renodx_visual_preset_files(exe_dir, s.renodx_preset, s.nr_pass_count,
                                        &s.nr_advanced[clamp_nr_pass_count(s.nr_pass_count)]);
  (void)sync_neural_rendering_enabled_files(exe_dir, want_nr, want_nr);
  (void)sync_feed_work_resolution_in_dir(
      exe_dir, feed_work_resolution_at_boot(false, s.scale_percent, s.nr_pass_count, s.width, s.height),
      s.nr_pass_count);
  (void)sync_mgf_techniques_for_phase(exe_dir, s.enhance_renodx);
  return true;
}

// Lobby Present: NR/Feed/grade só com preview + CFG live. Sem reescrever o look do Iniciar.
inline bool sync_lobby_preview_pipeline_files(const std::string& exe_dir, const PersistedSettings& s) {
  const bool live = lobby_preview_cfg_wanted(s.ui.preview_enabled, s.ui.preview_cfg_live);
  (void)sync_mgf_techniques_for_phase(exe_dir, live && s.enhance_renodx);
  return sync_neural_rendering_enabled_files(
      exe_dir,
      lobby_preview_nr_files_on(s.ui.preview_enabled, s.ui.preview_cfg_live, s.enhance_renodx,
                                s.neural_rendering),
      lobby_preview_feed_files_on(s.ui.preview_enabled, s.ui.preview_cfg_live, s.enhance_renodx));
}

// Alvo do lobby apos Benchmark: so persist HDMI. O plano do teste nao entra.
struct HdmiRestoreAfterPerfTest {
  int scale_percent = 100;
  int nr_pass_count = 1;
  int renodx_preset = 0;
  bool enhance_renodx = false;
  bool neural_rendering = false;
  bool fg_enabled = false;
  int fg_multiplier = 2;
  int lobby_feed_work_resolution = 100;
};

inline HdmiRestoreAfterPerfTest hdmi_restore_after_perf_test(const PersistedSettings& s) {
  HdmiRestoreAfterPerfTest t;
  t.scale_percent = clamp_scale_percent(s.scale_percent);
  t.nr_pass_count = clamp_nr_pass_count(s.nr_pass_count);
  t.renodx_preset = clamp_renodx_visual_preset(s.renodx_preset);
  t.enhance_renodx = s.enhance_renodx;
  t.neural_rendering = s.enhance_renodx && s.neural_rendering;
  t.fg_enabled = s.fg_enabled;
  t.fg_multiplier = s.fg_multiplier >= 2 ? s.fg_multiplier : 2;
  t.lobby_feed_work_resolution = kLobbyFeedWorkResolution;
  return t;
}

// Depois do teste: devolve escala/Pass/tom/NR/Feed ao CFG HDMI. O plano fica so em perf_test_plan.txt.
inline bool restore_hdmi_pipeline_after_perf_test(const std::string& exe_dir, const PersistedSettings& s) {
  const HdmiRestoreAfterPerfTest t = hdmi_restore_after_perf_test(s);
  PersistedSettings hdmi = s;
  hdmi.scale_percent = t.scale_percent;
  hdmi.nr_pass_count = t.nr_pass_count;
  hdmi.renodx_preset = t.renodx_preset;
  hdmi.enhance_renodx = t.enhance_renodx;
  hdmi.neural_rendering = t.neural_rendering;
  hdmi.fg_enabled = t.fg_enabled;
  hdmi.fg_multiplier = t.fg_multiplier;
  (void)sync_persisted_session_files(exe_dir, hdmi);
  (void)sync_feed_work_resolution_in_dir(exe_dir, t.lobby_feed_work_resolution);
  return true;
}

// Pure helpers for unit tests (serialize to string).
std::string serialize_settings_ini(const PersistedSettings& s);
bool parse_settings_ini(const std::string& text, PersistedSettings& s);

inline bool lobby_cfg_settings_dirty(const PersistedSettings& live, const PersistedSettings& snap) {
  return serialize_settings_ini(live) != serialize_settings_ini(snap);
}

inline bool lobby_cfg_fg_draft_dirty(bool adp_draft, bool adp_committed, int tgt_draft, int tgt_committed) {
  if (adp_draft != adp_committed) return true;
  return adp_draft && tgt_draft != tgt_committed;
}

inline bool lobby_cfg_has_unsaved(const PersistedSettings& live, const PersistedSettings& snap, bool adp_draft,
                                  bool adp_committed, int tgt_draft, int tgt_committed) {
  return lobby_cfg_settings_dirty(live, snap) ||
         lobby_cfg_fg_draft_dirty(adp_draft, adp_committed, tgt_draft, tgt_committed);
}

inline bool lobby_cfg_close_needs_prompt(bool has_unsaved) { return has_unsaved; }

const char* vk_display_name(int vk);

// Toast when Program Files (or other) blocks RenoDX config writes.
inline const char* renodx_write_denied_notice() {
  return "Nao foi possivel gravar RenoDX nesta pasta.\n"
         "Reinstale o NexusCap na pasta do usuario (padrao do instalador) — "
         "Program Files bloqueia presets e pode fechar no Iniciar.";
}

}  // namespace nexuscap
