#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
  int vk_cfg = 0x4D;        // 'M' — with cfg_ctrl → Ctrl+M abre o CFG na captura
  bool cfg_ctrl = true;     // require Ctrl for the session CFG menu
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
         "instalar, mas pode dar bugs e perdes 2x otimizado, 1,5x e 1x no Feed mais estavel.";
}

inline const char* lobby_motor_override_which(bool stable, bool multipass) {
  if (stable && multipass) return "1x (stable) e 1,5x/2x/3x (multipass)";
  if (stable) return "so o motor 1x (Feed estavel)";
  if (multipass) return "so o motor 1,5x/2x/2x otimizado/3x";
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

struct PersistedSettings {
  HotkeyConfig hotkeys{};
  SessionUiFlags ui{};
  int scale_percent = 100;     // 25–100; 100 = no scale
  bool fg_enabled = false;     // default OFF
  int fg_multiplier = 2;       // used when fg_enabled
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
  // Pass Count: 1=1x, 2=2x default, 3=3x, 4=1.5x, 5=2x otimizado (look exclusivo).
  int nr_pass_count = 1;
  // Origem (OBS-style). Defaults = validated HDMI path.
  int video_fourcc = 0;       // VideoFourcc::Nv12
  int color_space = 0;        // ColorSpace::Default (BT.709)
  int color_range = 0;        // ColorRange::Limited
  int capture_buffering = 0;  // CaptureBuffering::Disabled
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

// Pass Count UI: 1=Krish, 2=2x default, 3=3x, 4=1.5x, 5=2x otimizado (PassCount=2 + look).
inline constexpr int kMaxNrPassCount = 5;

inline int clamp_nr_pass_count(int v) {
  if (v < 1) return 1;
  if (v > kMaxNrPassCount) return kMaxNrPassCount;
  return v;
}

inline bool nr_pass_is_2x_optimized(int ui) { return clamp_nr_pass_count(ui) == 5; }

// What DirectNeuralRenderingPassCount we write (UI 4 → INI 1; UI 5 → INI 2).
inline int nr_reshade_pass_count(int ui) {
  switch (clamp_nr_pass_count(ui)) {
    case 2:
    case 5:
      return 2;
    case 3:
      return 3;
    case 4:
      return 1;
    default:
      return 1;
  }
}

inline const char* nr_pass_count_label(int v) {
  switch (clamp_nr_pass_count(v)) {
    case 2:
      return "2x default";
    case 3:
      return "3x multipass";
    case 4:
      return "1,5x";
    case 5:
      return "2x otimizado";
    default:
      return "1x estavel";
  }
}

// Hint under the Pass Count radios (lobby F12 e Ctrl+M).
inline const char* nr_pass_count_15x_hint() {
  return "1,5x exclusivo NexusCap: rede neural perto do 2x, sem perder desempenho.";
}
inline const char* nr_pass_count_2x_opt_hint() {
  return "2x otimizado exclusivo NexusCap: 2x de verdade, menos flicker e ghost, melhor qualidade de imagem.";
}
inline const char* nr_pass_count_23x_hint() {
  return "2x default / 3x: mais processados. O 2x otimizado e a opcao estavel.";
}

// Short HUD tag: "1x" / "1.5x" / "2x" / "2x+" / "3x".
inline const char* nr_pass_count_short(int v) {
  switch (clamp_nr_pass_count(v)) {
    case 2:
      return "2x";
    case 3:
      return "3x";
    case 4:
      return "1.5x";
    case 5:
      return "2x+";
    default:
      return "1x";
  }
}

// 1 → Krish. 2/3/4/5 → addons-multipass (4 = 1.5x; 5 = 2x otimizado).
inline bool nr_pass_count_uses_multipass_addon(int pass_count) {
  const int v = clamp_nr_pass_count(pass_count);
  return v >= 2;
}

inline const char* nr_dlss5_motor_name(int pass_count) {
  return nr_pass_count_uses_multipass_addon(pass_count) ? "multipass" : "stable";
}

// Compact overlay line: "DLSS5 1x Feed·stable NR ON"
inline void format_dlss5_stack_hud_line(char* out, size_t out_n, bool nr_on, bool feed_on, int pass_count) {
  if (!out || out_n == 0) return;
  std::snprintf(out, out_n, "DLSS5 %s Feed·%s NR %s", nr_pass_count_short(pass_count),
                nr_dlss5_motor_name(pass_count), nr_on ? "ON" : "off");
  (void)feed_on;
}

inline bool nr_pass_count_change_needs_relaunch(int before, int after) {
  return nr_pass_count_uses_multipass_addon(before) != nr_pass_count_uses_multipass_addon(after);
}

// O addon multipass lê DirectNeuralRenderingPassCount só no init e não aceita o ciclo OFF→ON
// do NR (ReShade.log: "nao importa USER32!GetAsyncKeyState ... alternancia do NR indisponivel").
// Medido: com o ini já em 3 o runtime seguia em "NR PASS TEST 1: passes=1". Logo 1,5x↔2x↔3x
// também precisa de processo novo — só escrever o ini não muda nada na imagem.
inline bool nr_pass_count_change_applies_live(int before, int after) {
  const int a = clamp_nr_pass_count(before);
  const int b = clamp_nr_pass_count(after);
  if (a == b) return true;
  return !nr_pass_count_uses_multipass_addon(a) && !nr_pass_count_uses_multipass_addon(b);
}

enum class RenodxAddonSyncResult {
  OkSame = 0,
  OkSwapped = 1,
  MissingVariant = 2,
  WriteFailed = 3,
};

// Point ReShade AddonPath at addons-stable (1x) or addons-multipass (2x/3x) before load.
// Stacks live in separate folders so only one renodx loads after restart.
RenodxAddonSyncResult sync_renodx_addon_variant(const std::string& exe_dir, int pass_count);

inline const char* renodx_addon_stack_rel(int pass_count) {
  return nr_pass_count_uses_multipass_addon(pass_count) ? ".\\addons-multipass" : ".\\addons-stable";
}

// Spawn a new NexusCap.exe then return true (caller should ExitProcess).
bool relaunch_current_process(const char* extra_args = nullptr);

// 1x (Krish / addons-stable): Default / Natural / Cinematic.
// 1.5x/2x/3x (addons-multipass): Model A / B / C — outro motor, outros knobs.
inline const char* renodx_visual_preset_name(int v, int pass_count = 1) {
  v = clamp_renodx_visual_preset(v);
  if (nr_pass_count_uses_multipass_addon(pass_count)) {
    switch (v) {
      case 1:
        return "Model B";
      case 2:
        return "Model C";
      default:
        return "Model A";
    }
  }
  switch (v) {
    case 1:
      return "Natural";
    case 2:
      return "Cinematic";
    default:
      return "Default";
  }
}

// RenoDX overlay "NR Preset" (Krish): Natural=0, Cinematic=1. Multipass keeps this dual-read.
inline int renodx_nr_preset_index(int visual) {
  return clamp_renodx_visual_preset(visual) == static_cast<int>(RenodxVisualPreset::Cinematic) ? 1 : 0;
}

// Multipass DirectNeuralRenderingStyle is the model slot (A=0, B=1, C=2).
// 1x has no Style knob — Krish uses NRPreset/NRStyle 0/1 instead.
inline int renodx_direct_style_index(int visual, int pass_count = 1) {
  visual = clamp_renodx_visual_preset(visual);
  if (nr_pass_count_uses_multipass_addon(pass_count)) return visual;
  return renodx_nr_preset_index(visual);
}

// feed.cfg preset=: 1x Default=0, Natural=Preset#1, Cinematic=0+NR cinema.
// Multipass writes the model slot 0/1/2 so A/B/C each hit a distinct Feed model.
inline int renodx_feed_model_preset(int visual, int pass_count = 1) {
  visual = clamp_renodx_visual_preset(visual);
  if (nr_pass_count_uses_multipass_addon(pass_count)) return visual;
  switch (visual) {
    case static_cast<int>(RenodxVisualPreset::Natural):
      return 1;
    case static_cast<int>(RenodxVisualPreset::Cinematic):
      return 0;  // model Default + NR Cinematic
    default:
      return 0;  // Default
  }
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

inline std::string apply_reshade_addon_path(std::string text, int pass_count) {
  return apply_keyed_value(std::move(text), "AddonPath=", renodx_addon_stack_rel(pass_count));
}

inline std::string parse_reshade_addon_path(const std::string& text, const char* fallback = ".\\addons-stable") {
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

// Keep RenoDX dlss5-feed.cfg work_resolution in sync with lobby scale (including 100%).
// Bug: only writing when scale<100 left work_resolution=50 stuck → blur at "100%".
// work_upscale=1 when below 100% so Feed actually downsamples then expands back.
// rebuild=1 forces the Feed feature to rebuild (it caches work_resolution otherwise).
inline std::string apply_feed_work_resolution(std::string text, int scale_percent) {
  int p = clamp_scale_percent(scale_percent);
  if (p < kMinFeedWorkResolution) p = kMinFeedWorkResolution;
  text = apply_keyed_int(std::move(text), "work_resolution=", p);
  text = apply_keyed_int(std::move(text), "work_upscale=", p < 100 ? 1 : 0);
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

// Preset visual: NRPreset/NRStyle in every section. Multipass also writes Style
// into the saved preset AND the live addon section (first-only left cinema stale).
// Style on that motor is Model A/B/C = 0/1/2, not the Krish NR 0/1 knob.
inline std::string apply_reshade_visual_look_keys(std::string text, int visual_preset) {
  const int nr = renodx_nr_preset_index(visual_preset);
  text = apply_reshade_nr_preset(std::move(text), visual_preset);
  text = apply_keyed_int_all(std::move(text), "NRStyle=", nr);
  text = upsert_keyed_int_in_section(std::move(text), "RenoDX.DLSS5", "NRStyle=", nr);
  if (reshade_addon_path_is_multipass(text)) {
    const int style = clamp_renodx_visual_preset(visual_preset);
    text = apply_keyed_int_all(std::move(text), "DirectNeuralRenderingStyle=", style);
    text = upsert_keyed_int_in_section(std::move(text), "RENODX-DLSS-preset1",
                                      "DirectNeuralRenderingStyle=", style);
    text = upsert_keyed_int_in_section(std::move(text), "RenoDX.DLSS5",
                                      "DirectNeuralRenderingStyle=", style);
  }
  return text;
}

inline int parse_reshade_nr_preset(const std::string& text, int fallback = 0) {
  return parse_keyed_int(text, "NRPreset=", fallback);
}

// Multipass stack — DirectNeuralRenderingPassCount (addons-multipass).
// Lives in both [RENODX-DLSS-preset*] and the live [RenoDX.DLSS5] — all must match.
inline std::string apply_reshade_nr_pass_count(std::string text, int passes) {
  return apply_keyed_int_all(std::move(text), "DirectNeuralRenderingPassCount=", nr_reshade_pass_count(passes));
}

inline int parse_reshade_nr_pass_count(const std::string& text, int fallback = 1) {
  const int raw = parse_keyed_int(text, "DirectNeuralRenderingPassCount=", fallback);
  if (raw < 1) return 1;
  if (raw > 3) return 3;  // INI never stores 4; 4 is UI-only (1.5x → INI 1)
  return raw;
}

// Multipass registers two Home overlays (RenoDX DLSS + Neural Resolution). Keep NR panel collapsed.
inline std::string collapse_reshade_imgui_window(std::string text, const char* title) {
  if (!title || !title[0]) return text;
  const std::string marker = std::string("[Window][") + title + "]";
  const auto pos = text.find(marker);
  if (pos == std::string::npos) return text;
  const auto collapsed = text.find("Collapsed=", pos);
  if (collapsed == std::string::npos) return text;
  const auto next_window = text.find("[Window][", pos + marker.size());
  if (next_window != std::string::npos && collapsed > next_window) return text;
  const auto val = collapsed + 10;  // after Collapsed=
  if (val < text.size() && (text[val] == '0' || text[val] == '1')) text[val] = '1';
  return text;
}

inline std::string sanitize_multipass_home_windows(std::string text) {
  text = collapse_reshade_imgui_window(std::move(text), "RenoDX-DLSSNR");
  text = collapse_reshade_imgui_window(std::move(text), "RenoDX Neural Resolution");
  const char* seed =
      "[Window][RenoDX DLSS],Pos=8,,8,Size=520,,900,Collapsed=0,"
      "[Window][RenoDX-DLSSNR],Pos=8,,8,Size=520,,900,Collapsed=1,"
      "[Window][RenoDX Neural Resolution],Pos=8,,8,Size=520,,900,Collapsed=1";
  if (text.find("[Window][RenoDX DLSS]") != std::string::npos ||
      text.find("[Window][RenoDX-DLSSNR]") != std::string::npos ||
      text.find("[Window][RenoDX Neural Resolution]") != std::string::npos) {
    return text;
  }
  const auto w = text.find("Window=");
  if (w == std::string::npos) {
    if (!text.empty() && text.back() != '\n') text.push_back('\n');
    text += "Window=";
    text += seed;
    text.push_back('\n');
    return text;
  }
  const auto end = text.find_first_of("\r\n", w);
  text.insert(end == std::string::npos ? text.size() : end, seed);
  return text;
}

// Align DirectNeuralRendering* keys (2/3/4=1.5x default look; 5 = 2x otimizado exclusivo).
inline std::string apply_reshade_multipass_look_defaults(std::string text, int pass_count,
                                                         int visual_preset = 0) {
  const bool opt = nr_pass_is_2x_optimized(pass_count);
  const int nits = opt ? 100 : 500;
  const int encoding = opt ? 2 : 0;
  const int hook = opt ? 2 : 1;
  const int ui_corr = opt ? 2 : 1;
  // [RENODX-DLSS] global
  text = apply_keyed_int(std::move(text), "DirectNeuralRenderingDiffuseWhiteNits=", nits);
  text = apply_keyed_int(std::move(text), "DirectNeuralRenderingDiffuseWhiteOverride=", 0);
  text = apply_keyed_int(std::move(text), "DirectNeuralRenderingEncoding=", encoding);
  text = apply_keyed_int(std::move(text), "DirectNeuralRenderingHookPoint=", hook);
  text = apply_keyed_int(std::move(text), "DirectNeuralRenderingRequireDlss=", 1);
  text = apply_keyed_int(std::move(text), "DirectNeuralRenderingUiCorrectionMode=", ui_corr);
  // [RENODX-DLSS-preset1] look knobs
  text = apply_keyed_int(std::move(text), "DirectNeuralRenderingAutoMask=", 1);
  text = apply_keyed_int(std::move(text), "DirectNeuralRenderingGlobalToneStrength=", 1);
  text = apply_keyed_int(std::move(text), "DirectNeuralRenderingIntensity=", 1);
  text = apply_keyed_int(std::move(text), "DirectNeuralRenderingLocalStructureStrength=", 1);
  text = apply_keyed_int(std::move(text), "DirectNeuralRenderingLocalToneStrength=", 1);
  text = apply_keyed_int(std::move(text), "DirectNeuralRenderingSkinStructureStrength=", 1);
  text = apply_keyed_int_all(std::move(text), "DirectNeuralRenderingStyle=",
                            clamp_renodx_visual_preset(visual_preset));
  text = apply_reshade_nr_pass_count(std::move(text), pass_count);
  // Keep legacy NR* block in sync for tools / dual-read / Krish.
  text = apply_keyed_int(std::move(text), "NRDiffuseWhiteNits=", nits);
  text = apply_keyed_int(std::move(text), "NRIntensity=", 1);
  text = apply_keyed_int(std::move(text), "NRGlobalTone=", 1);
  text = apply_keyed_int(std::move(text), "NRLocalTone=", 1);
  text = apply_keyed_int(std::move(text), "NRLocalStructure=", 1);
  text = apply_keyed_int(std::move(text), "NRAutoMask=", 1);
  text = apply_keyed_int(std::move(text), "NRSkinStructure=", -1);
  text = apply_keyed_int(std::move(text), "NRUICorrection=", ui_corr);
  text = apply_reshade_visual_look_keys(std::move(text), visual_preset);
  // Multipass: Delete is the real NRToggleKey (Bridge cannot inject GetAsyncKeyState).
  text = apply_keyed_int(std::move(text), "NRToggleKey=", 0x2E);
  text = sanitize_multipass_home_windows(std::move(text));
  return text;
}

// Pass Count 1 (Krish): only legacy NR* — avoids poking DirectNeural* on the stable addon.
inline std::string apply_reshade_stable_look_defaults(std::string text, int visual_preset = 0) {
  text = apply_reshade_nr_preset(std::move(text), visual_preset);
  text = apply_keyed_int(std::move(text), "NRDiffuseWhiteNits=", 500);
  text = apply_keyed_int(std::move(text), "NRIntensity=", 1);
  text = apply_keyed_int(std::move(text), "NRGlobalTone=", 1);
  text = apply_keyed_int(std::move(text), "NRLocalTone=", 1);
  text = apply_keyed_int(std::move(text), "NRLocalStructure=", 1);
  text = apply_keyed_int(std::move(text), "NRAutoMask=", 1);
  text = apply_keyed_int(std::move(text), "NRSkinStructure=", -1);
  text = apply_keyed_int(std::move(text), "NRUICorrection=", 1);
  text = apply_keyed_int(std::move(text), "NREnableUpscaling=", 1);
  text = apply_keyed_int(std::move(text), "NeuralUplift=", 1);
  text = apply_reshade_visual_look_keys(std::move(text), visual_preset);
  return text;
}

// UI 5 writes the same PassCount=2 as UI 2. Infer from exclusive look keys so a
// visual-preset-only write does not wipe 2x otimizado back to 2x default.
inline bool reshade_look_is_2x_optimized(const std::string& text) {
  return parse_keyed_int(text, "DirectNeuralRenderingEncoding=", 0) == 2 &&
         parse_keyed_int(text, "DirectNeuralRenderingHookPoint=", 1) == 2 &&
         parse_keyed_int(text, "DirectNeuralRenderingUiCorrectionMode=", 1) == 2;
}

inline int infer_multipass_ui_pass_count(const std::string& text) {
  const int raw = parse_reshade_nr_pass_count(text, 1);
  if (raw == 2 && reshade_look_is_2x_optimized(text)) return 5;
  return raw;
}

// Visual-preset writes must key off AddonPath, not PassCount: 1.5x is multipass + INI 1.
// pass_count >= 0: honor the UI slot (needed to keep 2x otimizado when INI is also 2).
inline std::string apply_reshade_look_for_current_stack(std::string text, int visual_preset,
                                                         int pass_count = -1) {
  if (reshade_addon_path_is_multipass(text)) {
    const int ui = pass_count >= 0 ? clamp_nr_pass_count(pass_count) : infer_multipass_ui_pass_count(text);
    return apply_reshade_multipass_look_defaults(std::move(text), ui, visual_preset);
  }
  text = apply_reshade_stable_look_defaults(std::move(text), visual_preset);
  return apply_keyed_int(std::move(text), "NRToggleKey=", 0x7C);  // VK_F13 / kRenodxNrInjectVk
}

// After gear Salvar / session start: feed + ReShade must match settings.renodx_preset.
// pass_count < 0: infer motor from AddonPath (tests without UI pass count).
inline bool renodx_files_reflect_visual_preset(const std::string& feed_text, const std::string& reshade_text,
                                               int visual_preset, int pass_count = -1) {
  const int v = clamp_renodx_visual_preset(visual_preset);
  const int nr = renodx_nr_preset_index(v);
  const bool mp = pass_count >= 0 ? nr_pass_count_uses_multipass_addon(pass_count)
                                  : reshade_addon_path_is_multipass(reshade_text);
  const int p = pass_count >= 0 ? clamp_nr_pass_count(pass_count) : (mp ? 2 : 1);
  if (parse_feed_model_preset(feed_text, -1) != renodx_feed_model_preset(v, p)) return false;
  if (!every_keyed_int_is(reshade_text, "NRPreset=", nr)) return false;
  if (!every_keyed_int_is(reshade_text, "NRStyle=", nr)) return false;
  if (mp && !every_keyed_int_is(reshade_text, "DirectNeuralRenderingStyle=",
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
  return apply_feed_model_preset(std::move(feed), visual_preset, pass_count);
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

// True when ReShade.ini AddonPath points at addons-multipass.
bool renodx_active_addon_is_multipass(const std::string& exe_dir);

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

inline int feed_work_resolution_for_session(int scale_percent) {
  const int p = clamp_scale_percent(scale_percent);
  return p < kMinFeedWorkResolution ? kMinFeedWorkResolution : p;
}

// What the pipeline really renders at: Feed floor under RenoDX, app RT scale otherwise.
inline int effective_scale_percent(bool renodx_stack, int ui_scale_percent) {
  return renodx_stack ? feed_work_resolution_for_session(ui_scale_percent)
                      : clamp_scale_percent(ui_scale_percent);
}

// True when the UI asks for less than the Feed can do (25–49% under RenoDX).
inline bool feed_scale_below_floor(bool renodx_stack, int ui_scale_percent) {
  return renodx_stack && clamp_scale_percent(ui_scale_percent) < kMinFeedWorkResolution;
}

// Feed snapshots work_resolution at addon load, so a scale picked in the lobby only lands
// after a relaunch that re-seeds the cfg first.
// Without RenoDX the renderer scales its own RT — no restart needed.
inline bool feed_scale_change_needs_relaunch(bool renodx_stack, int loaded_scale, int want_scale) {
  if (!renodx_stack) return false;
  // Compare what the Feed actually builds at — 40% and 33% both floor to 50%, no restart.
  return feed_work_resolution_for_session(loaded_scale) != feed_work_resolution_for_session(want_scale);
}

// Esc/X/timer do Benchmark: o processo ainda tem Pass/tom/NR/Feed/LS do teste na RAM.
// Gravar o HDMI no disco nao descarrega o addon — precisa processo novo.
inline bool perf_test_return_needs_fresh_process(bool was_perf_test) { return was_perf_test; }

// Lobby Present is the process's first swapchain — Feed/ReShade lock there. Iniciar in the
// same process cannot apply CFG (scale, Cinema, NR on/off). Ctrl+M works because it relaunches.
// Always --autostart so the capture window is the first Present with disk already matching CFG.
inline bool lobby_iniciar_needs_session_relaunch() { return true; }

// Menus must never inherit the session scale — the Feed would blur the whole lobby.
inline int feed_work_resolution_for_lobby() { return kLobbyFeedWorkResolution; }

// work_resolution the process should boot with: session scale only when skipping the lobby.
inline int feed_work_resolution_at_boot(bool lobby_first, int ui_scale_percent) {
  return lobby_first ? feed_work_resolution_for_lobby() : feed_work_resolution_for_session(ui_scale_percent);
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

// %LOCALAPPDATA%\NexusCap (created if missing). Empty if LOCALAPPDATA unavailable.
std::string app_data_dir();
std::string settings_default_path();  // app_data_dir()\settings.ini
std::string app_logs_dir();          // app_data_dir()\logs
std::string app_reshade_cache_dir(); // app_data_dir()\reshade-cache

// True when we can create/overwrite a probe file in dir (Program Files fails without admin).
bool directory_is_writable(const std::string& dir);

// Point ReShade IntermediateCachePath at a writable AppData cache; ensure folder exists.
// Returns false if exe_dir itself is not writable (presets/NR cannot be saved).
bool prepare_renodx_runtime_dirs(const std::string& exe_dir);

bool save_settings(const PersistedSettings& s, const std::string& path, std::string& err);
bool load_settings(PersistedSettings& s, const std::string& path, std::string& err);

// Rewrite dlss5-feed.cfg work_resolution on disk. false if missing/unreadable.
bool sync_feed_work_resolution_file(const std::string& path, int scale_percent);

// Feed lives under AddonPath (addons-stable / addons-multipass); keep root + both stacks in sync.
bool sync_feed_work_resolution_in_dir(const std::string& exe_dir, int scale_percent);

// DLSS5MGF.ini Techniques: full chain for session/live preview, empty for idle lobby.
bool sync_mgf_techniques_for_phase(const std::string& exe_dir, bool session_or_live_preview);

// Write renodx_preset into dlss5-feed.cfg + ReShade.ini next to the exe.
// pass_count picks 1x feed 0/1/0 vs multipass model 0/1/2.
bool sync_renodx_visual_preset_files(const std::string& exe_dir, int visual_preset, int pass_count = 1);

// Pass Count + look defaults. 1x keeps Krish NR*; 2x/3x writes DirectNeural* in ReShade.ini.
bool sync_nr_pass_count_files(const std::string& exe_dir, int pass_count, int visual_preset = 0);

// Persist Neural Rendering on/off (independent of Frame Gen). Updates feed+ReShade.
bool sync_neural_rendering_enabled_files(const std::string& exe_dir, bool nr_on, bool feed_on);

// CFG do lobby → disco do RenoDX/Feed, na ordem que o addon le no boot da sessao.
inline bool sync_persisted_session_files(const std::string& exe_dir, const PersistedSettings& s) {
  const bool want_nr = s.enhance_renodx && s.neural_rendering;
  (void)sync_nr_pass_count_files(exe_dir, s.nr_pass_count, s.renodx_preset);
  (void)sync_renodx_visual_preset_files(exe_dir, s.renodx_preset, s.nr_pass_count);
  (void)sync_neural_rendering_enabled_files(exe_dir, want_nr, want_nr);
  (void)sync_feed_work_resolution_in_dir(exe_dir, feed_work_resolution_for_session(s.scale_percent));
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

const char* vk_display_name(int vk);

// Toast when Program Files (or other) blocks RenoDX config writes.
inline const char* renodx_write_denied_notice() {
  return "Nao foi possivel gravar RenoDX nesta pasta.\n"
         "Reinstale o NexusCap na pasta do usuario (padrao do instalador) — "
         "Program Files bloqueia presets e pode fechar no Iniciar.";
}

}  // namespace nexuscap
