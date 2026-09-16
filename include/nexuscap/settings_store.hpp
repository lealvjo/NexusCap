#pragma once
#include <algorithm>
#include <cstdlib>
#include <string>

namespace nexuscap {

struct HotkeyConfig {
  int vk_quit = 0x1B;       // VK_ESCAPE
  int vk_hud = 0x72;        // VK_F3
  int vk_analyze = 0x73;    // VK_F4
  int vk_dlss = 0x74;       // VK_F5
  int vk_fg_toggle = 0x22;  // VK_NEXT (Page Down) — X2 framegen
  int vk_fg_cycle = 0x21;   // VK_PRIOR (Page Up)
  int vk_renodx = 0x24;     // VK_HOME — overlay presets
  int vk_nr_toggle = 0x2E;  // VK_DELETE — Neural Rendering on/off (FG independente)
  int vk_fullscreen = 0x7A; // VK_F11
  int vk_gear = 0x7B;       // VK_F12 — open gear in lobby
};

struct SessionUiFlags {
  bool show_fps = true;
  bool advanced_metrics = false;
  bool hw_info = false;  // painel CPU(+C) / GPU(+C) / RAM / VRAM / PWR (abaixo de métricas)
  bool fullscreen = true;
  bool hide_panel_on_capture = true;
  bool sync_image = false;
};

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
  int renodx_preset = 0;  // RenodxVisualPreset
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

inline const char* renodx_visual_preset_name(int v) {
  switch (clamp_renodx_visual_preset(v)) {
    case 1:
      return "Natural";
    case 2:
      return "Cinematic";
    default:
      return "Default";
  }
}

// RenoDX overlay "NR Preset": Natural=0, Cinematic=1.
inline int renodx_nr_preset_index(int visual) {
  return clamp_renodx_visual_preset(visual) == static_cast<int>(RenodxVisualPreset::Cinematic) ? 1 : 0;
}

// feed.cfg preset= maps overlay model: Default=0, Preset#1=1, Preset#2=2, Preset#3=3.
// Natural uses Preset#1 so Default/Natural/Cinematic each write a distinct combo (visible look).
inline int renodx_feed_model_preset(int visual) {
  switch (clamp_renodx_visual_preset(visual)) {
    case static_cast<int>(RenodxVisualPreset::Natural):
      return 1;
    case static_cast<int>(RenodxVisualPreset::Cinematic):
      return 0;  // model Default + NR Cinematic
    default:
      return 0;  // Default
  }
}

// True when two gear presets must produce different on-disk RenoDX config (preview/session).
inline bool renodx_visual_presets_differ_on_disk(int a, int b) {
  a = clamp_renodx_visual_preset(a);
  b = clamp_renodx_visual_preset(b);
  if (a == b) return false;
  return renodx_nr_preset_index(a) != renodx_nr_preset_index(b) ||
         renodx_feed_model_preset(a) != renodx_feed_model_preset(b);
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
inline std::string apply_keyed_int(std::string text, const std::string& key, int value) {
  const std::string line = key + std::to_string(value);
  const auto pos = text.find(key);
  if (pos != std::string::npos) {
    const auto end = text.find_first_of("\r\n", pos);
    text.replace(pos, (end == std::string::npos ? text.size() : end) - pos, line);
  } else {
    if (!text.empty() && text.back() != '\n') text.push_back('\n');
    text += line;
    text.push_back('\n');
  }
  return text;
}

inline std::string apply_keyed_value(std::string text, const std::string& key, const std::string& value) {
  const std::string line = key + value;
  const auto pos = text.find(key);
  if (pos != std::string::npos) {
    const auto end = text.find_first_of("\r\n", pos);
    text.replace(pos, (end == std::string::npos ? text.size() : end) - pos, line);
  } else {
    if (!text.empty() && text.back() != '\n') text.push_back('\n');
    text += line;
    text.push_back('\n');
  }
  return text;
}

inline int parse_keyed_int(const std::string& text, const std::string& key, int fallback) {
  const auto pos = text.find(key);
  if (pos == std::string::npos) return fallback;
  const auto start = pos + key.size();
  const auto end = text.find_first_of("\r\n", start);
  const std::string num = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
  return std::atoi(num.c_str());
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

// Approximate pixel ratio vs native (for perf messaging / tests). 50% → 0.25; 25% → ~0.0625.
inline double scale_pixel_ratio(int scale_percent) {
  const double s = clamp_scale_percent(scale_percent) / 100.0;
  return s * s;
}

// Keep RenoDX dlss5-feed.cfg work_resolution in sync with lobby scale (including 100%).
// Bug: only writing when scale<100 left work_resolution=50 stuck → blur at "100%".
inline std::string apply_feed_work_resolution(std::string text, int scale_percent) {
  return apply_keyed_int(std::move(text), "work_resolution=", clamp_scale_percent(scale_percent));
}

inline int parse_feed_work_resolution(const std::string& text, int fallback = 100) {
  return clamp_scale_percent(parse_keyed_int(text, "work_resolution=", fallback));
}

inline std::string apply_feed_model_preset(std::string text, int visual_preset) {
  return apply_keyed_int(std::move(text), "preset=", renodx_feed_model_preset(visual_preset));
}

inline int parse_feed_model_preset(const std::string& text, int fallback = 0) {
  return parse_keyed_int(text, "preset=", fallback);
}

// ReShade.ini NRPreset — Natural(0) / Cinematic(1).
inline std::string apply_reshade_nr_preset(std::string text, int visual_preset) {
  return apply_keyed_int(std::move(text), "NRPreset=", renodx_nr_preset_index(visual_preset));
}

inline int parse_reshade_nr_preset(const std::string& text, int fallback = 0) {
  return parse_keyed_int(text, "NRPreset=", fallback);
}

// After gear Salvar / session start: feed + ReShade must match settings.renodx_preset.
inline bool renodx_files_reflect_visual_preset(const std::string& feed_text, const std::string& reshade_text,
                                               int visual_preset) {
  const int v = clamp_renodx_visual_preset(visual_preset);
  if (parse_feed_model_preset(feed_text, -1) != renodx_feed_model_preset(v)) return false;
  if (parse_reshade_nr_preset(reshade_text, -1) != renodx_nr_preset_index(v)) return false;
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

inline std::string apply_renodx_visual_preset_to_feed(std::string feed, int visual_preset) {
  return apply_feed_model_preset(std::move(feed), visual_preset);
}

inline std::string apply_renodx_visual_preset_to_reshade(std::string reshade, int visual_preset) {
  return apply_reshade_nr_preset(std::move(reshade), visual_preset);
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

// dlss5-feed.cfg enabled= — feeder pipeline. Must stay ON for the whole Renodx session:
// feed does not hot-reload enabled=0→1 (Delete would only flip HUD / RenoDX flag).
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
// Session scale must NOT blur ImGui menu: lobby always forces feed to 100%.
inline constexpr int kLobbyFeedWorkResolution = 100;

inline int feed_work_resolution_for_session(int scale_percent) {
  return clamp_scale_percent(scale_percent);
}

inline int feed_work_resolution_for_lobby() { return kLobbyFeedWorkResolution; }

// Esc from a 50% session leaves feed@50 on disk — rewrite before lobby Present.
inline std::string apply_lobby_feed_work_resolution(std::string text) {
  return apply_feed_work_resolution(std::move(text), feed_work_resolution_for_lobby());
}

// True when feed still carries session scale that would soften lobby UI Present.
inline bool feed_leaks_session_scale_into_lobby(const std::string& feed_text, int session_scale_percent) {
  const int session = feed_work_resolution_for_session(session_scale_percent);
  if (session >= kLobbyFeedWorkResolution) return false;
  return parse_feed_work_resolution(feed_text, 100) == session;
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

// Write renodx_preset into dlss5-feed.cfg + ReShade.ini next to the exe.
bool sync_renodx_visual_preset_files(const std::string& exe_dir, int visual_preset);

// Persist Neural Rendering on/off (independent of Frame Gen). Updates feed+ReShade.
bool sync_neural_rendering_enabled_files(const std::string& exe_dir, bool nr_on, bool feed_on);

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
