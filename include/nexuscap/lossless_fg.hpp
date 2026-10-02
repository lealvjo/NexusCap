#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "nexuscap/display_geom.hpp"
#include "nexuscap/settings_store.hpp"

namespace nexuscap {

enum class FgStatus {
  Off,      // x1 / disabled
  Missing,  // exe not found
  Ready,    // process available, FG not active on our window
  On,       // FG toggled on
  Error,
};

inline const char* fg_status_name(FgStatus s) {
  switch (s) {
    case FgStatus::Off:
      return "off";
    case FgStatus::Missing:
      return "missing";
    case FgStatus::Ready:
      return "ready";
    case FgStatus::On:
      return "on";
    case FgStatus::Error:
      return "error";
  }
  return "off";
}

// Clamp / validate CLI multiplier: only 1,2,3,4 accepted.
inline bool parse_fg_multiplier(const std::string& raw, int& out) {
  if (raw == "1") {
    out = 1;
    return true;
  }
  if (raw == "2") {
    out = 2;
    return true;
  }
  if (raw == "3") {
    out = 3;
    return true;
  }
  if (raw == "4") {
    out = 4;
    return true;
  }
  return false;
}

inline int clamp_fg_queue(int q) {
  if (q < 0) return 0;
  if (q > 2) return 2;
  return q;
}

inline const char* fg_queue_label(int q) {
  switch (clamp_fg_queue(q)) {
    case 1: return "1 -> Equilibrado";
    case 2: return "2 -> Maior estabilidade";
    default: return "0 -> Menor latencia";
  }
}

// Escala de fluxo: Padrao e o nosso valor (80). 70, 90 e 100 so se a pessoa escolher.
inline constexpr int kFgFlowScaleDefault = 80;
inline constexpr int kFgFlowScaleChoices[] = {80, 70, 90, 100};
inline constexpr int kFgFlowScaleChoiceCount = 4;

inline int clamp_fg_flow_scale(int v) {
  if (v == 70 || v == 90 || v == 100 || v == kFgFlowScaleDefault) return v;
  return kFgFlowScaleDefault;
}

inline const char* fg_flow_scale_label(int v) {
  switch (clamp_fg_flow_scale(v)) {
    case 70: return "70";
    case 90: return "90";
    case 100: return "100";
    default: return "Padrao";
  }
}

// Adaptive LSFG3Target: digitado. Teto = Hz do monitor. Piso = FPS da captura sem FG
// (se a captura vai a 40, o alvo nao pode ser 30). Sem captura conhecida, piso 30.
inline constexpr int kFgAdaptiveTargetMin = 30;
inline constexpr int kFgAdaptiveTargetDefault = 60;

inline int clamp_fg_adaptive_target_stored(int want) {
  if (want < kFgAdaptiveTargetMin) return kFgAdaptiveTargetMin;
  if (want > 360) return 360;
  return want;
}

inline int fg_adaptive_capture_floor(double capture_fps) {
  if (capture_fps < 1.0) return kFgAdaptiveTargetMin;
  int lo = static_cast<int>(capture_fps + 0.5);
  if (lo < 1) lo = 1;
  return lo;
}

inline int clamp_fg_adaptive_target(int want, int monitor_hz, double capture_fps = 0) {
  const int hi = clamp_display_refresh_hz(monitor_hz);
  int lo = fg_adaptive_capture_floor(capture_fps);
  if (lo > hi) lo = hi;
  int t = want;
  if (t < lo) t = lo;
  if (t > hi) t = hi;
  return t;
}

inline const char* lsfg3_mode1_xml(bool adaptive) { return adaptive ? "ADAPTIVE" : "FIXED"; }

inline const char* fg_mode_label(bool adaptive) {
  return adaptive ? "Adaptive (teto FPS)" : "Fixo (x2/x3/x4)";
}

inline bool fg_adaptive_apply_dirty(bool live_adaptive, int live_target, bool draft_adaptive, int draft_target) {
  return live_adaptive != draft_adaptive || live_target != draft_target;
}

// Aplicar so existe no Adaptive. Trocar para Fixo grava na hora (x2/x3/x4 sem leftover Adaptive).
inline bool fg_adaptive_apply_enabled(bool draft_adaptive, bool live_adaptive, int live_target,
                                     int draft_target) {
  return draft_adaptive && fg_adaptive_apply_dirty(live_adaptive, live_target, true, draft_target);
}

inline const char* fg_adaptive_ghost_hint() {
  return "Fixo multiplica a captura (x2, x3 ou x4). Adaptive gera frames ate o que voce definiu.";
}

inline bool parse_fg_adaptive_ini(const std::string& val) {
  return val == "1" || val == "true" || val == "TRUE" || val == "adaptive" || val == "ADAPTIVE";
}

// Hz do modo atual do ecrã onde está a janela (EnumDisplaySettings). Sem HWND = primário.
int display_refresh_hz(HWND hwnd = nullptr);

inline int clamp_fg_multiplier(int m) {
  if (m < 1) return 1;
  if (m > 4) return 4;
  return m;
}

// Overlay: "Fixo x2" vs "Adaptive 70". flow_scale >= 0 acrescenta Padrao/70/90/100.
inline void format_fg_mode_tag(char* out, size_t n, bool adaptive, int mult,
                               int target = kFgAdaptiveTargetDefault, int flow_scale = -1) {
  if (!out || n == 0) return;
  if (adaptive) {
    if (flow_scale >= 0) {
      std::snprintf(out, n, "Adaptive %d %s", clamp_fg_adaptive_target_stored(target),
                    fg_flow_scale_label(flow_scale));
    } else {
      std::snprintf(out, n, "Adaptive %d", clamp_fg_adaptive_target_stored(target));
    }
  } else if (flow_scale >= 0) {
    std::snprintf(out, n, "Fixo x%d %s", clamp_fg_multiplier(mult), fg_flow_scale_label(flow_scale));
  } else {
    std::snprintf(out, n, "Fixo x%d", clamp_fg_multiplier(mult));
  }
}

// Page Up cycle: 2 → 3 → 4 → 2 (x1 is off, not part of cycle).
inline int next_fg_multiplier(int m) {
  m = clamp_fg_multiplier(m);
  if (m <= 1) return 2;
  if (m == 2) return 3;
  if (m == 3) return 4;
  return 2;
}

inline bool fg_wants_process(int multiplier) { return clamp_fg_multiplier(multiplier) >= 2; }

// Graph traces: FG off / x1 → 1 linha (cap). FG on → cap + o xN armado (sem x2 fantasma no x3).
inline int fg_frametime_trace_count(bool fg_on, int fg_mult) {
  if (!fg_on) return 1;
  return clamp_fg_multiplier(fg_mult) < 2 ? 1 : 2;
}

// trace_i 1-based: 1 = cap, 2 = FG do multiplier armado.
inline int fg_frametime_trace_k(int trace_i, int fg_mult) {
  if (trace_i <= 1) return 1;
  const int m = clamp_fg_multiplier(fg_mult);
  return m < 2 ? 1 : m;
}

inline double fg_frametime_trace_ms(double capture_ms, int trace_k) {
  const int k = trace_k < 1 ? 1 : trace_k;
  return capture_ms / static_cast<double>(k);
}

// Y real: cap (mais ms / menos FPS) em baixo, FG (menos ms / mais FPS) em cima.
inline int fg_frametime_plot_y(int gy, int gh, double cap_ms, int trace_k, double ceil_ms) {
  const double ceil = ceil_ms < 1.0 ? 1.0 : ceil_ms;
  const double ms = fg_frametime_trace_ms(cap_ms, trace_k);
  const double t = ms < 0.0 ? 0.0 : (ms > ceil ? 1.0 : ms / ceil);
  const int span = gh > 4 ? gh - 4 : 0;
  return gy + 2 + static_cast<int>(t * static_cast<double>(span));
}

inline int fg_frametime_fps_gutter_px() { return 52; }

inline int fg_frametime_fps(double capture_ms, int trace_k) {
  const double ms = fg_frametime_trace_ms(capture_ms, trace_k);
  if (ms < 0.2) return 0;
  return static_cast<int>(1000.0 / ms + 0.5);
}

inline void format_fg_frametime_fps_label(char* out, size_t n, double capture_ms, int trace_k) {
  if (!out || n == 0) return;
  std::snprintf(out, n, "%d", fg_frametime_fps(capture_ms, trace_k));
}

struct FgFtRgb {
  unsigned char r = 90;
  unsigned char g = 220;
  unsigned char b = 255;
};

inline FgFtRgb fg_frametime_trace_rgb(int trace_k, bool stable) {
  switch (trace_k) {
    case 2:
      return {255, 196, 72};  // gold x2
    case 3:
      return {210, 120, 255};  // purple x3
    case 4:
      return {80, 255, 160};  // mint x4
    default:
      return stable ? FgFtRgb{90, 220, 255} : FgFtRgb{255, 140, 90};
  }
}

// HUD: STABLE / PLAYABLE FG / UNSTABLE. Mesma regra do grade PlayableFg (folga 0,5 + piso 20).
enum class OverlayFtStatus { Stable, PlayableFg, Unstable };

inline constexpr double kOverlayFtFpsSlop = 0.5;
inline constexpr double kOverlayFtFgMinCapFps = 20.0;

inline OverlayFtStatus overlay_ft_status(bool cap_stable, bool fg_on, int fg_mult, double fps_cap,
                                        double target_fps, double fps_fg_est = 0) {
  if (cap_stable) return OverlayFtStatus::Stable;
  const int m = clamp_fg_multiplier(fg_mult);
  if (!fg_on || m < 2) return OverlayFtStatus::Unstable;
  if (fps_cap + 1e-9 < kOverlayFtFgMinCapFps) return OverlayFtStatus::Unstable;
  const double tgt = target_fps > 1.0 ? target_fps : 60.0;
  const double floor_fps = tgt - kOverlayFtFpsSlop;
  const double fg = fps_fg_est > 0.0 ? fps_fg_est : (fps_cap * static_cast<double>(m));
  if (fg + 1e-6 >= floor_fps) return OverlayFtStatus::PlayableFg;
  return OverlayFtStatus::Unstable;
}

inline const char* overlay_ft_status_label(OverlayFtStatus s) {
  switch (s) {
    case OverlayFtStatus::Stable:
      return "STABLE";
    case OverlayFtStatus::PlayableFg:
      return "PLAYABLE FG";
    default:
      return "UNSTABLE";
  }
}

inline const char* overlay_ft_status_hint(OverlayFtStatus s) {
  return s == OverlayFtStatus::PlayableFg ? "FG cobre a meta" : "";
}

inline FgFtRgb overlay_ft_status_rgb(OverlayFtStatus s) {
  switch (s) {
    case OverlayFtStatus::Stable:
      return {120, 255, 160};
    case OverlayFtStatus::PlayableFg:
      return {255, 196, 72};
    default:
      return {255, 120, 100};
  }
}

// Compact / ADV / mini: same capture last/avg/p95. FG est = captura÷xN (not LS-measured).
inline void format_overlay_frametime_line(char* out, size_t n, double frame_ms, double ft_avg_ms,
                                         double ft_p95_ms, bool ft_stable, bool fg_on, int fg_mult,
                                         double fps_cap, double target_fps, double fps_fg_est = 0) {
  if (!out || n == 0) return;
  const OverlayFtStatus st =
      overlay_ft_status(ft_stable, fg_on, fg_mult, fps_cap, target_fps, fps_fg_est);
  const int m = clamp_fg_multiplier(fg_mult);
  if (fg_on && m >= 2) {
    std::snprintf(out, n, "ft %.2f  avg %.2f  p95 %.2f  FG est %.2f  %s", frame_ms, ft_avg_ms, ft_p95_ms,
                  fg_frametime_trace_ms(frame_ms, m), overlay_ft_status_label(st));
  } else {
    std::snprintf(out, n, "ft %.2f  avg %.2f  p95 %.2f  %s", frame_ms, ft_avg_ms, ft_p95_ms,
                  overlay_ft_status_label(st));
  }
}

inline void format_overlay_latency_line(char* out, size_t n, double e2e_ms, double present_ms, bool nr_on,
                                        const char* sync_label = nullptr) {
  if (!out || n == 0) return;
  if (nr_on && sync_label && sync_label[0]) {
    std::snprintf(out, n, "latencia  cap %.2f ms   present %.2f ms (+NR/DLSS5)   %s", e2e_ms, present_ms,
                  sync_label);
  } else if (nr_on) {
    std::snprintf(out, n, "latencia  cap %.2f ms   present %.2f ms (+NR/DLSS5)", e2e_ms, present_ms);
  } else if (sync_label && sync_label[0]) {
    std::snprintf(out, n, "latencia  cap %.2f ms   present %.2f ms   %s", e2e_ms, present_ms, sync_label);
  } else {
    std::snprintf(out, n, "latencia  cap %.2f ms   present %.2f ms", e2e_ms, present_ms);
  }
}

// Live strip: last + FG est only (no avg/p95).
inline void format_overlay_frametime_live(char* out, size_t n, double frame_ms, bool fg_on, int fg_mult) {
  if (!out || n == 0) return;
  const int m = clamp_fg_multiplier(fg_mult);
  if (fg_on && m >= 2) {
    std::snprintf(out, n, "%.2f · FG %.2f", frame_ms, fg_frametime_trace_ms(frame_ms, m));
  } else {
    std::snprintf(out, n, "%.2f", frame_ms);
  }
}

// Colored FPS chips — same RGB as the frametime sparkline (cap / x2 / x3 / x4).
struct HudFpsPart {
  char text[48]{};
  unsigned char r = 180;
  unsigned char g = 184;
  unsigned char b = 190;
};

struct HudFpsParts {
  HudFpsPart parts[8]{};
  int count = 0;
};

inline void hud_fps_push(HudFpsParts& o, const char* text, unsigned char r, unsigned char g, unsigned char b) {
  if (!text || !text[0] || o.count >= 8) return;
  std::snprintf(o.parts[o.count].text, sizeof(o.parts[o.count].text), "%s", text);
  o.parts[o.count].r = r;
  o.parts[o.count].g = g;
  o.parts[o.count].b = b;
  ++o.count;
}

inline void join_hud_fps_parts(char* out, size_t out_n, const HudFpsParts& p) {
  if (!out || out_n == 0) return;
  out[0] = '\0';
  size_t used = 0;
  for (int i = 0; i < p.count; ++i) {
    const size_t len = std::strlen(p.parts[i].text);
    if (used + len + 1 > out_n) break;
    std::memcpy(out + used, p.parts[i].text, len);
    used += len;
    out[used] = '\0';
  }
}

inline void fill_session_hud_fps_parts(HudFpsParts& o, double fps_cap, double fps_fg_est, bool fg_on,
                                       int fg_mult, bool compact, bool ft_stable = true,
                                       bool fg_adaptive = false, int fg_target = kFgAdaptiveTargetDefault,
                                       int flow_scale = -1) {
  o = HudFpsParts{};
  const int m = clamp_fg_multiplier(fg_mult);
  const FgFtRgb cap = fg_frametime_trace_rgb(1, ft_stable);
  hud_fps_push(o, compact ? "FPS  " : "FPS     ", 180, 184, 190);
  char num[24]{};
  std::snprintf(num, sizeof(num), "%.1f", fps_cap);
  hud_fps_push(o, num, cap.r, cap.g, cap.b);
  char tag[40]{};
  format_fg_mode_tag(tag, sizeof(tag), fg_adaptive, m, fg_target, flow_scale);
  if (fg_on && m >= 2) {
    const double fg = fps_fg_est > 0 ? fps_fg_est
                                    : (fg_adaptive ? static_cast<double>(clamp_fg_adaptive_target_stored(fg_target))
                                                   : (fps_cap * static_cast<double>(m)));
    hud_fps_push(o, " | ", 140, 144, 150);
    std::snprintf(num, sizeof(num), "%.1f", fg);
    const FgFtRgb fgc = fg_frametime_trace_rgb(m, ft_stable);
    hud_fps_push(o, num, fgc.r, fgc.g, fgc.b);
    char suf[64]{};
    if (compact) {
      std::snprintf(suf, sizeof(suf), "  cap|FG %s ON", tag);
    } else {
      std::snprintf(suf, sizeof(suf), "   cap|FG %s ON", tag);
    }
    hud_fps_push(o, suf, fgc.r, fgc.g, fgc.b);
  } else if (m >= 2) {
    char suf[64]{};
    if (compact) {
      std::snprintf(suf, sizeof(suf), "   FG %s off", tag);
    } else {
      std::snprintf(suf, sizeof(suf), "   FG %s off (PageDn)", tag);
    }
    hud_fps_push(o, suf, 160, 170, 180);
  }
}

inline void fill_lobby_hud_fps_parts(HudFpsParts& o, double fps, bool fg_enabled, int fg_mult,
                                     bool ft_stable = true, bool fg_adaptive = false,
                                     int fg_target = kFgAdaptiveTargetDefault, int flow_scale = -1) {
  o = HudFpsParts{};
  const int m = clamp_fg_multiplier(fg_mult);
  const FgFtRgb cap = fg_frametime_trace_rgb(1, ft_stable);
  hud_fps_push(o, "FPS  ", 180, 184, 190);
  char num[24]{};
  std::snprintf(num, sizeof(num), "%.0f", fps);
  hud_fps_push(o, num, cap.r, cap.g, cap.b);
  if (fg_enabled && m >= 2) {
    char tag[40]{};
    format_fg_mode_tag(tag, sizeof(tag), fg_adaptive, m, fg_target, flow_scale);
    char suf[64]{};
    std::snprintf(suf, sizeof(suf), "   FG %s off", tag);
    const FgFtRgb fgc = fg_frametime_trace_rgb(m, ft_stable);
    hud_fps_push(o, suf, fgc.r, fgc.g, fgc.b);
  }
}

// Lobby HUD: checkbox on = "off" until session PageDn (never imply running dual FPS).
inline void format_lobby_hud_fps_line(char* out, size_t out_n, double fps, bool fg_enabled, int fg_mult,
                                      bool fg_adaptive = false, int fg_target = kFgAdaptiveTargetDefault,
                                      int flow_scale = -1) {
  HudFpsParts parts{};
  fill_lobby_hud_fps_parts(parts, fps, fg_enabled, fg_mult, true, fg_adaptive, fg_target, flow_scale);
  join_hud_fps_parts(out, out_n, parts);
}

// Session stats when FG is ON: real capture FPS | estimated FG output FPS.
inline void format_session_hud_fps_line(char* out, size_t out_n, double fps_cap, double fps_fg_est, bool fg_on,
                                       int fg_mult, bool compact, bool fg_adaptive = false,
                                       int fg_target = kFgAdaptiveTargetDefault, int flow_scale = -1) {
  HudFpsParts parts{};
  fill_session_hud_fps_parts(parts, fps_cap, fps_fg_est, fg_on, fg_mult, compact, true, fg_adaptive,
                            fg_target, flow_scale);
  join_hud_fps_parts(out, out_n, parts);
}

// Colored segments for mini overlay paint (r,g,b 0–255). Sep = " · ".
struct MiniOverlaySeg {
  char text[160]{};
  unsigned char r = 210;
  unsigned char g = 230;
  unsigned char b = 255;
};

inline constexpr int kMiniOverlayMaxSegs = 32;

inline void mini_seg_set(MiniOverlaySeg& s, const char* text, unsigned char r, unsigned char g,
                         unsigned char b) {
  s.r = r;
  s.g = g;
  s.b = b;
  if (!text) {
    s.text[0] = '\0';
    return;
  }
  std::snprintf(s.text, sizeof(s.text), "%s", text);
}

inline void mini_seg_sep(MiniOverlaySeg& s) { mini_seg_set(s, " · ", 80, 95, 115); }

// 3-row mini bar: row0 = FPS…present, row1 = stalls+ft, row2 = L600+HW.
// Stalls used to sit at the end of row0 and were clipped by the sparkline.
inline bool mini_overlay_seg_is_stalls(const MiniOverlaySeg& s) {
  return std::strncmp(s.text, "stalls ", 7) == 0;
}
inline bool mini_overlay_seg_is_l600(const MiniOverlaySeg& s) {
  return std::strncmp(s.text, "L600", 4) == 0;
}
inline void mini_overlay_row_breaks(const MiniOverlaySeg* segs, int n, int* wrap_stalls, int* wrap_l600) {
  int ws = n;
  int wl = n;
  if (segs) {
    for (int i = 0; i < n; ++i) {
      if (ws == n && mini_overlay_seg_is_stalls(segs[i])) ws = i;
      if (wl == n && mini_overlay_seg_is_l600(segs[i])) wl = i;
    }
  }
  if (wrap_stalls) *wrap_stalls = ws;
  if (wrap_l600) *wrap_l600 = wl;
}

inline unsigned mini_hw_token_rgb(const char* tok, unsigned char& r, unsigned char& g, unsigned char& b) {
  if (!tok || !tok[0]) return 0;
  // Power
  if (std::strstr(tok, "W") && (tok[0] >= '0' && tok[0] <= '9')) {
    r = 140;
    g = 255;
    b = 220;
    return 1;
  }
  // VRAM / RAM compact
  if (tok[0] == 'V' || (tok[0] == 'R' && tok[1] >= '0' && tok[1] <= '9')) {
    r = 120;
    g = 255;
    b = 180;
    return 1;
  }
  // GPU SKU
  if (std::strstr(tok, "RTX") || std::strstr(tok, "GTX") || std::strstr(tok, "RX ") ||
      std::strstr(tok, "Arc") || std::strstr(tok, "GPU")) {
    r = 160;
    g = 240;
    b = 255;
    return 1;
  }
  // CPU SKU / fallback
  r = 255;
  g = 230;
  b = 90;
  return 1;
}

// Fills colored segments; returns count. Also builds joined line0/line1 like format_mini_overlay_lines.
inline int format_mini_overlay_segments(MiniOverlaySeg* segs, int max_segs, char* line0, size_t n0,
                                        char* line1, size_t n1, double fps_cap, double fps_fg_est, bool fg_on,
                                        int fg_mult, bool dlss5_on, bool sync_image, double frame_ms,
                                        double ft_avg_ms, double ft_p95_ms, double ft_max_ms, bool ft_stable,
                                        double e2e_ms, double present_ms, uint64_t stalls, uint64_t clusters,
                                        uint64_t dropped, int capture_w, int capture_h, int work_w = 0,
                                        int work_h = 0, int output_w = 0, int output_h = 0,
                                        int scale_percent = 100, bool has_stats600 = false,
                                        double s600_fps = 0, double s600_p95_e2e = 0,
                                        double s600_p95_present = 0, uint64_t s600_stalls = 0,
                                        const char* renodx_preset = nullptr, const char* hw_compact = nullptr,
                                        double dlss_ms = 0, uint64_t frames = 0, double s600_max_interval = 0,
                                        const char* fg_backend = nullptr, double target_fps = 0,
                                        int nr_pass_count = 1, const char* console_label = nullptr,
                                        bool console_flag = true, unsigned char cons_r = 0,
                                        unsigned char cons_g = 55, unsigned char cons_b = 145,
                                        int fg_queue = 0, bool fg_adaptive = false,
                                        int fg_target = kFgAdaptiveTargetDefault, int fg_flow_scale = -1,
                                        const char* capture_fmt = nullptr) {
  if (line0 && n0) line0[0] = '\0';
  if (line1 && n1) line1[0] = '\0';
  int n = 0;
  auto push = [&](const char* text, unsigned char r, unsigned char g, unsigned char b) {
    if (!segs || n >= max_segs || !text || !text[0]) return;
    mini_seg_set(segs[n++], text, r, g, b);
  };
  auto push_sep = [&]() {
    if (!segs || n >= max_segs) return;
    mini_seg_sep(segs[n++]);
  };

  if (console_label && console_label[0]) {
    push(console_label, cons_r, cons_g, cons_b);
    push_sep();
  }
  (void)console_flag;

  // Pass Count na frente. O "Fixo xN" do FG mora na linha de FPS; o X do motor
  // ficava depois da resolução e o gráfico cortava.
  {
    char nr_part[96]{};
    const char* preset = (renodx_preset && renodx_preset[0]) ? renodx_preset : "";
    const char* pass = nr_pass_count_label(nr_pass_count);
    const char* motor = nr_dlss5_motor_name(nr_pass_count);
    const char* state = dlss5_overlay_state_word(dlss5_on);
    if (dlss5_on && preset[0] && dlss_ms > 0.01) {
      std::snprintf(nr_part, sizeof(nr_part), "DLSS5 %s %s NR/%s %.1fms", state, pass, preset, dlss_ms);
    } else if (dlss5_on && preset[0]) {
      std::snprintf(nr_part, sizeof(nr_part), "DLSS5 %s %s Feed·%s NR/%s", state, pass, motor, preset);
    } else if (dlss5_on && dlss_ms > 0.01) {
      std::snprintf(nr_part, sizeof(nr_part), "DLSS5 %s %s NR %.1fms", state, pass, dlss_ms);
    } else {
      std::snprintf(nr_part, sizeof(nr_part), "DLSS5 %s %s Feed·%s", state, pass, motor);
    }
    push(nr_part, dlss5_on ? 180 : 140, dlss5_on ? 160 : 150, dlss5_on ? 255 : 170);
    push_sep();
  }

  HudFpsParts fps_parts{};
  fill_session_hud_fps_parts(fps_parts, fps_cap, fps_fg_est, fg_on, fg_mult, true, ft_stable, fg_adaptive,
                            fg_target, fg_flow_scale);
  for (int i = 0; i < fps_parts.count; ++i) {
    push(fps_parts.parts[i].text, fps_parts.parts[i].r, fps_parts.parts[i].g, fps_parts.parts[i].b);
  }

  if (fg_backend && fg_backend[0] && fg_on) {
    char fg_extra[40]{};
    std::snprintf(fg_extra, sizeof(fg_extra), " [%s]", fg_backend);
    push(fg_extra, 120, 255, 200);
  }
  {
    char fila[16]{};
    std::snprintf(fila, sizeof(fila), " fila%d", clamp_fg_queue(fg_queue));
    push(fila, 120, 255, 200);
  }

  // Resolução logo depois do FPS: antes ficava no fim e o gráfico cortava a escala.
  char res_part[96]{};
  if (capture_w > 0 && capture_h > 0) {
    const int ww = work_w > 0 ? work_w : capture_w;
    const int wh = work_h > 0 ? work_h : capture_h;
    const int ow = output_w > 0 ? output_w : capture_w;
    const int oh = output_h > 0 ? output_h : capture_h;
    const int sp = scale_percent > 0 ? scale_percent : 100;
    if (ow == capture_w && oh == capture_h) {
      std::snprintf(res_part, sizeof(res_part), "%dx%d>%dx%d@%d%%>out 1:1", capture_w, capture_h, ww, wh, sp);
    } else {
      std::snprintf(res_part, sizeof(res_part), "%dx%d>%dx%d@%d%%>%dx%d", capture_w, capture_h, ww, wh, sp, ow,
                    oh);
    }
  }
  if (capture_fmt && capture_fmt[0]) {
    char with_fmt[128]{};
    std::snprintf(with_fmt, sizeof(with_fmt), "%s %s", res_part[0] ? res_part : "-", capture_fmt);
    std::snprintf(res_part, sizeof(res_part), "%s", with_fmt);
  }
  push_sep();
  push(res_part[0] ? res_part : "-", 120, 220, 255);

  push_sep();
  if (sync_image) {
    push("VSYNC", 120, 220, 255);
  } else {
    push("TEAR", 255, 160, 90);
  }

  push_sep();
  char lat[80]{};
  // present = Present() wall time — includes Feed + RenoDX/NR when ON (~2ms tipico).
  // cap = captura pura no app (nao inclui o hook NR).
  if (dlss5_on) {
    std::snprintf(lat, sizeof(lat), "cap %.1f · present %.1fms+NR", e2e_ms, present_ms);
  } else {
    std::snprintf(lat, sizeof(lat), "cap %.1f · present %.1fms", e2e_ms, present_ms);
  }
  push(lat, 120, 255, 160);

  push_sep();
  char cnt[96]{};
  (void)dropped;
  (void)frames;
  std::snprintf(cnt, sizeof(cnt), "stalls %llu  clusters %llu",
                static_cast<unsigned long long>(stalls), static_cast<unsigned long long>(clusters));
  push(cnt, 255, 180, 120);

  push_sep();
  (void)ft_max_ms;
  const OverlayFtStatus ft_st =
      overlay_ft_status(ft_stable, fg_on, fg_mult, fps_cap, target_fps, fps_fg_est);
  const FgFtRgb ft_rgb = overlay_ft_status_rgb(ft_st);
  char ft_part[128]{};
  format_overlay_frametime_line(ft_part, sizeof(ft_part), frame_ms, ft_avg_ms, ft_p95_ms, ft_stable, fg_on,
                               fg_mult, fps_cap, target_fps, fps_fg_est);
  push(ft_part, ft_rgb.r, ft_rgb.g, ft_rgb.b);

  push_sep();
  char s600_part[120]{};
  if (has_stats600) {
    if (s600_max_interval > 0.01) {
      std::snprintf(s600_part, sizeof(s600_part),
                    "L600 fps %.1f p95e %.1f p95p %.1f stalls %llu max %.1f", s600_fps, s600_p95_e2e,
                    s600_p95_present, static_cast<unsigned long long>(s600_stalls), s600_max_interval);
    } else {
      std::snprintf(s600_part, sizeof(s600_part), "L600 fps %.1f p95e %.1f p95p %.1f stalls %llu", s600_fps,
                    s600_p95_e2e, s600_p95_present, static_cast<unsigned long long>(s600_stalls));
    }
  } else {
    std::snprintf(s600_part, sizeof(s600_part), "L600…");
  }
  push(s600_part, 255, 180, 120);

  if (hw_compact && hw_compact[0]) {
    // Split "CPU | GPU | V | R | W" into colored tokens.
    const char* p = hw_compact;
    while (*p && n < max_segs - 1) {
      while (*p == ' ') ++p;
      if (!*p) break;
      const char* start = p;
      while (*p && !(p[0] == ' ' && p[1] == '|' && p[2] == ' ')) ++p;
      size_t len = static_cast<size_t>(p - start);
      while (len > 0 && start[len - 1] == ' ') --len;
      char tok[96]{};
      if (len >= sizeof(tok)) len = sizeof(tok) - 1;
      if (len > 0) {
        std::memcpy(tok, start, len);
        tok[len] = '\0';
      }
      if (tok[0]) {
        push_sep();
        unsigned char r = 255, g = 230, b = 90;
        mini_hw_token_rgb(tok, r, g, b);
        push(tok, r, g, b);
      }
      if (p[0] == ' ' && p[1] == '|') {
        p += 3;
      } else if (*p == '|') {
        ++p;
      }
    }
  }

  if (line0 && n0 > 0) line0[0] = '\0';
  if (line1 && n1 > 0) line1[0] = '\0';
  if (segs && n > 0) {
    // Line0 = FPS…present (must stay short); stalls+ft+L600+HW on line1 for 2-string callers.
    int wrap_st = n;
    mini_overlay_row_breaks(segs, n, &wrap_st, nullptr);
    auto join_range = [](char* dst, size_t dst_n, const MiniOverlaySeg* s, int begin, int end) {
      if (!dst || dst_n == 0) return;
      size_t used = 0;
      dst[0] = '\0';
      for (int i = begin; i < end; ++i) {
        const size_t len = std::strlen(s[i].text);
        if (used + len + 1 > dst_n) break;
        std::memcpy(dst + used, s[i].text, len);
        used += len;
        dst[used] = '\0';
      }
    };
    join_range(line0, n0, segs, 0, wrap_st);
    join_range(line1, n1, segs, wrap_st, n);
  }
  return n;
}

// Mini overlay: row0 FPS…present; row1 stalls+ft (+ L600/HW in the 2-string API).
inline void format_mini_overlay_lines(char* line0, size_t n0, char* line1, size_t n1, double fps_cap,
                                      double fps_fg_est, bool fg_on, int fg_mult, bool dlss5_on,
                                      bool sync_image, double frame_ms, double ft_avg_ms, double ft_p95_ms,
                                      double ft_max_ms, bool ft_stable, double e2e_ms, double present_ms,
                                      uint64_t stalls, uint64_t clusters, uint64_t dropped, int capture_w,
                                      int capture_h, int work_w = 0, int work_h = 0, int output_w = 0,
                                      int output_h = 0, int scale_percent = 100, bool has_stats600 = false,
                                      double s600_fps = 0, double s600_p95_e2e = 0,
                                      double s600_p95_present = 0, uint64_t s600_stalls = 0,
                                      const char* renodx_preset = nullptr, const char* hw_compact = nullptr,
                                      double dlss_ms = 0, uint64_t frames = 0, double s600_max_interval = 0,
                                      const char* fg_backend = nullptr, double target_fps = 0,
                                      int nr_pass_count = 1, const char* console_label = nullptr,
                                      bool console_flag = true, unsigned char cons_r = 0,
                                      unsigned char cons_g = 55, unsigned char cons_b = 145,
                                      int fg_queue = 0, bool fg_adaptive = false,
                                      int fg_target = kFgAdaptiveTargetDefault, int fg_flow_scale = -1,
                                      const char* capture_fmt = nullptr) {
  MiniOverlaySeg segs[kMiniOverlayMaxSegs]{};
  format_mini_overlay_segments(segs, kMiniOverlayMaxSegs, line0, n0, line1, n1, fps_cap, fps_fg_est, fg_on,
                               fg_mult, dlss5_on, sync_image, frame_ms, ft_avg_ms, ft_p95_ms, ft_max_ms,
                               ft_stable, e2e_ms, present_ms, stalls, clusters, dropped, capture_w, capture_h,
                               work_w, work_h, output_w, output_h, scale_percent, has_stats600, s600_fps,
                               s600_p95_e2e, s600_p95_present, s600_stalls, renodx_preset, hw_compact, dlss_ms,
                               frames, s600_max_interval, fg_backend, target_fps, nr_pass_count, console_label,
                               console_flag, cons_r, cons_g, cons_b, fg_queue, fg_adaptive, fg_target,
                               fg_flow_scale, capture_fmt);
}

// Back-compat one-liner.
inline void format_mini_overlay_line(char* out, size_t out_n, double fps_cap, double fps_fg_est, bool fg_on,
                                    int fg_mult, bool dlss5_on, double frame_ms, double e2e_ms,
                                    uint64_t stalls, uint64_t clusters, int capture_w, int capture_h,
                                    int work_w = 0, int work_h = 0, int output_w = 0, int output_h = 0,
                                    int scale_percent = 100) {
  if (!out || out_n == 0) return;
  char a[512]{};
  char b[512]{};
  format_mini_overlay_lines(a, sizeof(a), b, sizeof(b), fps_cap, fps_fg_est, fg_on, fg_mult, dlss5_on,
                            /*sync_image=*/true, frame_ms, frame_ms, frame_ms, frame_ms, true, e2e_ms,
                            /*present=*/0, stalls, clusters, /*dropped=*/0, capture_w, capture_h, work_w,
                            work_h, output_w, output_h, scale_percent);
  std::snprintf(out, out_n, "%s%s", a, b);
}

inline void format_session_hud_fg_status_line(char* out, size_t out_n, bool fg_on, int fg_mult,
                                             const char* backend, int flow_scale, const char* capture_api,
                                             const char* status, double fps_cap = 0, double fps_fg_est = 0,
                                             int queue = 0, bool adaptive = false,
                                             int target = kFgAdaptiveTargetDefault) {
  if (!out || out_n == 0) return;
  const int m = clamp_fg_multiplier(fg_mult);
  const int q = clamp_fg_queue(queue);
  const char* be = backend && backend[0] ? backend : "lsfg3";
  const char* api = capture_api && capture_api[0] ? capture_api : "WGC";
  const char* st = status && status[0] ? status : (fg_on ? "On" : "Ready");
  char tag[40]{};
  format_fg_mode_tag(tag, sizeof(tag), adaptive, m, target, flow_scale);
  if (fg_on && m >= 2) {
    const double fg = fps_fg_est > 0
                          ? fps_fg_est
                          : (adaptive ? static_cast<double>(clamp_fg_adaptive_target_stored(target))
                                      : (fps_cap > 0 ? fps_cap * static_cast<double>(m) : 0));
    if (fps_cap > 0 && fg > 0) {
      std::snprintf(out, out_n, "FG      ON   %s  %s  %s  fila%d  |  %.0f->%.0f", tag, be, api, q, fps_cap, fg);
    } else {
      std::snprintf(out, out_n, "FG      ON   %s  %s  %s  fila%d", tag, be, api, q);
    }
  } else {
    std::snprintf(out, out_n, "FG      %s  %s  %s  %s  fila%d  [%s]", fg_on ? "ON" : "off", tag, be, api, q, st);
  }
}

// True when session FPS line shows real|FG (required while FG runtime is ON).
inline bool session_hud_fps_shows_cap_fg_split(const char* line) {
  if (!line) return false;
  return std::strstr(line, " | ") != nullptr && std::strstr(line, "cap|FG") != nullptr;
}

// Lobby must not imply active FG with a dual estimate.
inline bool lobby_fps_line_implies_active_fg(const char* line) {
  if (!line) return false;
  return std::strstr(line, " | ") != nullptr;
}

// Preferred FG multiplier stays armed (≥2) even when lobby checkbox is off so PageDn works.
inline int armed_fg_multiplier(bool /*fg_enabled*/, int fg_mult) {
  return clamp_fg_multiplier(fg_mult >= 2 ? fg_mult : 2);
}

// Lobby / pos-Benchmark: XML+processo so seguem o HDMI ligado. Off = x1, sem gerar.
inline int hdmi_lobby_fg_multiplier(bool fg_enabled, int fg_mult) {
  return fg_enabled ? armed_fg_multiplier(true, fg_mult) : 1;
}

// WM_CLOSE manda o LS para o tray (CloseToTray) — nao conta como morto.
// So zero processos no Toolhelp (snapshot ok) libera o lobby.
inline bool fg_leftover_cleared(unsigned running, bool snapshot_ok) { return snapshot_ok && running == 0; }
inline int fg_kill_wait_step_ms() { return 50; }
inline int fg_kill_wait_max_ms() { return 1500; }

// Pure helpers (unit-tested): mira-lock profile fragments.
std::string default_steam_lossless_exe();
std::string resolve_lossless_exe(const std::string& override_path);
bool lossless_exe_exists(const std::string& path);

// Lobby CFG: só liga FG se o exe existir (auto Steam ou Pasta).
inline bool lobby_fg_can_enable(const std::string& fg_path) { return !resolve_lossless_exe(fg_path).empty(); }

inline bool ls_xml_draw_fps_off(const std::string& xml) {
  return xml.find("<DrawFps>false</DrawFps>") != std::string::npos &&
         xml.find("<DrawFps>true</DrawFps>") == std::string::npos;
}

inline bool mira_lock_xml_ok(const std::string& xml) {
  return xml.find("LSFGFlowScale>80") != std::string::npos && xml.find("LSFGFlowScale>70") == std::string::npos &&
         xml.find("<QueueTarget>0</QueueTarget>") != std::string::npos &&
         xml.find("<CaptureApi>WGC</CaptureApi>") != std::string::npos && ls_xml_draw_fps_off(xml);
}

inline bool ls_ini_fps_counter_hidden(const std::string& ini) {
  const size_t sec = ini.find("[fps_counter]");
  if (sec == std::string::npos) return false;
  // "scale = 0" sozinho casa com 0.1 (chip minusculo ainda visivel). Exige fim de linha.
  const bool scale_off = ini.find("scale = 0\r", sec) != std::string::npos ||
                         ini.find("scale = 0\n", sec) != std::string::npos;
  return scale_off && ini.find("show_captured = 0", sec) != std::string::npos;
}

// Hide the LS settings UI and the small DrawFps chip. Never hide a large surface
// (LS output pode vir sem TOPMOST/LAYERED; esconder ela mata o FG na tela).
inline bool ls_should_hide_window(const wchar_t* title, const wchar_t* cls, unsigned ex_style, int w = 0,
                                  int h = 0) {
  if (!title) title = L"";
  if (!cls) cls = L"";
  const bool named = wcsstr(title, L"Lossless Scaling") != nullptr || wcsstr(cls, L"Lossless") != nullptr;
  if (!named) return false;
  // Classe da saída que cobre a janela. Esconder ela apaga o frame gen.
  if (_wcsicmp(cls, L"LosslessScaling") == 0) return false;
  if (wcsstr(cls, L"HwndWrapper") != nullptr) return true;
  if (w >= 640 && h >= 360) return false;
  if ((ex_style & WS_EX_TOPMOST) || (ex_style & WS_EX_LAYERED)) {
    return w > 0 && h > 0 && w <= 420 && h <= 220;
  }
  return true;
}

// Superficie grande do LS. Chip DrawFps e a janela WPF de settings nao contam.
inline bool ls_is_generating_output(const wchar_t* title, const wchar_t* cls, unsigned ex_style, int w,
                                    int h) {
  if (!title) title = L"";
  if (!cls) cls = L"";
  if (wcsstr(cls, L"HwndWrapper") != nullptr) return false;
  const bool named = wcsstr(title, L"Lossless Scaling") != nullptr || wcsstr(cls, L"Lossless") != nullptr ||
                     wcsstr(cls, L"LSFG") != nullptr || wcsstr(cls, L"Overlay") != nullptr;
  if (!named) return false;
  if (w < 640 || h < 360) return false;
  (void)ex_style;
  return true;
}

// Pelo menos 40% da janela alvo coberta pela saida do LS.
inline bool ls_output_covers_target(int ox, int oy, int ow, int oh, int tx, int ty, int tw, int th) {
  if (ow < 640 || oh < 360 || tw < 1 || th < 1) return false;
  const int x0 = ox > tx ? ox : tx;
  const int y0 = oy > ty ? oy : ty;
  const int x1 = (ox + ow) < (tx + tw) ? (ox + ow) : (tx + tw);
  const int y1 = (oy + oh) < (ty + th) ? (oy + oh) : (ty + th);
  if (x1 <= x0 || y1 <= y0) return false;
  const long long inter = static_cast<long long>(x1 - x0) * static_cast<long long>(y1 - y0);
  const long long area = static_cast<long long>(tw) * static_cast<long long>(th);
  return inter * 5 >= area * 2;
}
inline bool mira_lock_ini_ok(const std::string& ini) {
  return ini.find("flow_scale = 80") != std::string::npos && ini.find("ui_detection_rate = 2") != std::string::npos;
}

// Patch or inject mira-lock tags. Default flow is 80 (Padrao). 70/90/100 only when chosen.
std::string patch_mira_settings_xml(const std::string& xml, int multiplier, int queue = 0,
                                    bool adaptive = false, int target_fps = kFgAdaptiveTargetDefault,
                                    int monitor_hz = 60, int flow_scale = kFgFlowScaleDefault);

// Build a minimal valid Settings.xml with mira lock + LSFG3 FIXED (default) or Adaptive.
std::string build_mira_settings_xml(int multiplier, int queue = 0, bool adaptive = false,
                                    int target_fps = kFgAdaptiveTargetDefault, int monitor_hz = 60,
                                    int flow_scale = kFgFlowScaleDefault);
// Read LSFG3Multiplier from Settings.xml text; 0 = missing/invalid.
inline int parse_lsfg3_multiplier_from_xml(const std::string& xml) {
  const std::string open = "<LSFG3Multiplier>";
  const std::string close = "</LSFG3Multiplier>";
  const auto a = xml.find(open);
  if (a == std::string::npos) return 0;
  const auto b = xml.find(close, a + open.size());
  if (b == std::string::npos) return 0;
  const std::string raw = xml.substr(a + open.size(), b - (a + open.size()));
  int m = 0;
  if (!parse_fg_multiplier(raw, m)) return 0;
  return clamp_fg_multiplier(m);
}

inline bool parse_lsfg3_adaptive_from_xml(const std::string& xml) {
  return xml.find("<LSFG3Mode1>ADAPTIVE</LSFG3Mode1>") != std::string::npos;
}

inline bool parse_lsfg3_fixed_from_xml(const std::string& xml) {
  return xml.find("<LSFG3Mode1>FIXED</LSFG3Mode1>") != std::string::npos;
}

inline int parse_lsfg3_target_from_xml(const std::string& xml) {
  const std::string open = "<LSFG3Target>";
  const std::string close = "</LSFG3Target>";
  const auto a = xml.find(open);
  if (a == std::string::npos) return 0;
  const auto b = xml.find(close, a + open.size());
  if (b == std::string::npos) return 0;
  const int t = std::atoi(xml.substr(a + open.size(), b - (a + open.size())).c_str());
  return t > 0 ? t : 0;
}

inline bool fg_adaptive_confirmed_on_disk(bool want_adaptive, int want_target, const std::string& xml) {
  if (want_adaptive) {
    return parse_lsfg3_adaptive_from_xml(xml) && parse_lsfg3_target_from_xml(xml) == want_target;
  }
  return parse_lsfg3_fixed_from_xml(xml);
}

inline bool ls_exe_is_lossless(const wchar_t* exe) {
  if (!exe || !exe[0]) return false;
  if (_wcsicmp(exe, L"LosslessScaling.exe") == 0) return true;
  if (_wcsicmp(exe, L"Lossless Scaling.exe") == 0) return true;
  return wcsstr(exe, L"LosslessScaling") != nullptr || wcsstr(exe, L"Lossless Scaling") != nullptr;
}

// HUD ON: superficie grande do LS, ou F24 confirmado nesta sessao com o processo vivo.
// Superficie sozinha nao existia (LS gera sem janela nomeada) e o overlay mentia OFF.
inline bool fg_overlay_reads_on(bool state_active, int mult, bool owned_alive, unsigned running_ls,
                               bool confirmed = false, bool generating = false) {
  if (clamp_fg_multiplier(mult) < 2) return false;
  if (generating) return true;
  return confirmed && (state_active || owned_alive || running_ls > 0);
}

// Overlay xN only after disk confirms the same multiplier (no memory-only hope).
inline bool fg_multiplier_confirmed_on_disk(int want_mult, const std::string& settings_xml) {
  const int want = clamp_fg_multiplier(want_mult);
  if (want < 2) return parse_lsfg3_multiplier_from_xml(settings_xml) >= 2 || want <= 1;
  return parse_lsfg3_multiplier_from_xml(settings_xml) == want;
}

inline bool fg_ls_profile_confirmed_on_disk(int want_mult, bool adaptive, int target,
                                           const std::string& xml) {
  if (!fg_multiplier_confirmed_on_disk(want_mult, xml)) return false;
  return fg_adaptive_confirmed_on_disk(adaptive, target, xml);
}

// Patch [lsfg] section in config.ini text for mira lock. Default flow is 80.
std::string patch_lsfg_ini(const std::string& ini, int flow_scale = kFgFlowScaleDefault);

// Só o bloco [fps_counter]. Usado no re-hide depois do LS abrir: mexer em
// flow_scale / ui_detection_* com a sessao viva reabre a briga de perfil.
std::string patch_lsfg_ini_fps_counter_only(const std::string& ini);

struct FgState {
  FgStatus status = FgStatus::Off;
  int multiplier = 1;
  int queue_target = 0;  // 0 menor latência, 1 equilibrado, 2 maior estabilidade
  bool adaptive = false;  // LSFG3Mode1 ADAPTIVE; default FIXED
  int target_fps = kFgAdaptiveTargetDefault;
  int monitor_hz = 60;
  double capture_fps = 0;
  int flow_scale = 80;
  std::string capture_api = "WGC";
  std::string backend = "lsfg3";
  DWORD pid = 0;
  std::string exe_path;
  std::string last_error;
  // Contador do LS: vazio = escondido de verdade (gravado e conferido no disco).
  // Separado de last_error porque falhar o hide nao derruba o Frame Gen.
  std::string ini_hide_error;
  bool active = false;  // FG currently applied to our window
};

class LosslessFgController {
public:
  LosslessFgController() = default;
  ~LosslessFgController() { shutdown(); }

  LosslessFgController(const LosslessFgController&) = delete;
  LosslessFgController& operator=(const LosslessFgController&) = delete;

  // Configure before use. multiplier 1 = off semantics.
  void configure(int multiplier, const std::string& exe_override = {}, int queue = -1, int adaptive = -1,
                 int target_fps = -1, int monitor_hz = -1, double capture_fps = -1, int flow_scale = -1);

  const FgState& state() const { return state_; }
  bool busy() const { return pending_ != FgPending::None; }

  // Apply mira profile to %LOCALAPPDATA%\Lossless Scaling\Settings.xml (+ Steam config.ini).
  bool apply_profile(std::string& err);

  // Lobby / pos-Benchmark: mata LS leftover, grava mira HDMI, nao lanca processo.
  bool apply_hdmi_profile_without_launch(bool fg_enabled, int fg_mult, const std::string& path,
                                         std::string& err, int queue = 0, bool adaptive = false,
                                         int target_fps = kFgAdaptiveTargetDefault, int monitor_hz = 60,
                                         int flow_scale = kFgFlowScaleDefault);

  // Esc/fim do teste: F24 off se estiver gerando, TerminateProcess em todo LS, espera count=0.
  // Nao usa WM_CLOSE (tray). Chamar com a janela da sessao ainda viva.
  void pulse_fg_hotkey();
  bool kill_all_lossless_and_wait(DWORD timeout_ms = 0);

  // Start LS process under Job Object if multiplier >= 2 and exe exists.
  // allow_adopt=false forces a fresh process (required after x2↔x3↔x4 so LS reloads XML).
  // No Sleep — caller waits via tick() so Present keeps running.
  bool ensure_process(std::string& err, bool allow_adopt = true);

  // Async FG ops (no Present freeze). Call tick() every frame until !busy().
  bool request_toggle(HWND target, std::string& err);
  bool request_set_multiplier(int multiplier, HWND target, std::string& err);
  bool request_cycle_multiplier(HWND target, std::string& err);
  // Returns true when an async op just finished this frame (success or fail in last_error).
  bool tick(double now_ms);

  // Sync wrapper (selftests). Prefer request_* + tick in the session loop.
  bool toggle(HWND target, std::string& err);

  // Boot com FG ligado: adota o Lossless já aberto e segura o atalho até o 1º frame.
  // Sem processo vivo, arma o lançamento sem bloquear o primeiro frame.
  bool arm_autostart(HWND target, std::string& err);
  // O 1º frame HDMI já está na tela: agora o atalho pode ir.
  void release_autostart_hotkey();
  bool autostart_waits_first_frame() const { return defer_hotkey_; }

  // Restart do motor: tira o Lossless do Job deste processo sem matá-lo.
  // false: o Job ainda tinha a flag de matar e o processo pode ter caído.
  bool detach_for_restart();

  void shutdown(DWORD wait_ms = 800);

  // How many LosslessScaling.exe are running (0 = none). Used to avoid duplicate instances.
  // force=true ignora o cache de 200 ms (confirmacao ON/OFF / espera o processo morrer).
  // snapshot_ok=false: Toolhelp falhou — nao tratar o valor como zero real.
  static unsigned count_running_lossless_processes(bool force = false, bool* snapshot_ok = nullptr);

  bool owned_process_alive() const;
  bool output_generating_on(HWND target) const;
  bool overlay_reads_on(HWND target = nullptr) const;

  // If active but LS process gone → clear active (HUD never stays ON as false positive).
  void reconcile_process();

private:
  enum class FgPending {
    None,
    OnWaitDead,
    OnWaitLaunch,
    OnSendHotkey,
    OnConfirm,
    OffAfterHotkey,
    OffConfirmDead,
    MultWaitDead,
    MultWaitLaunch,
    MultAfterLaunch,
  };

  bool send_ls_hotkey();
  bool focus_window(HWND hwnd);
  void hide_ls_windows();
  bool write_settings_file(const std::string& xml, std::string& err);
  bool write_ini_alongside_exe(std::string& err);
  // Grava o config.ini e confere no disco. Nunca marca ReadOnly.
  bool write_ls_ini_verified(const std::string& ini_path, const std::string& data, std::string& err);
  // Re-hide do contador sem tocar em [lsfg] nem no XML (LS ja esta vivo).
  bool rehide_ls_fps_counter();
  bool adopt_existing_lossless(std::string& err);
  void terminate_extra_lossless_processes(DWORD keep_pid);
  bool run_sync_until_idle(std::string& err);

  FgState state_;
  HANDLE job_ = nullptr;
  HANDLE proc_ = nullptr;
  bool configured_ = false;

  FgPending pending_ = FgPending::None;
  double pending_due_ms_ = 0;
  HWND pending_hwnd_ = nullptr;
  int pending_prev_mult_ = 2;
  int pending_want_mult_ = 2;
  bool pending_was_active_ = false;
  int pending_retries_ = 0;
  bool pending_finished_ok_ = false;
  bool defer_hotkey_ = false;  // atalho do boot só depois do primeiro frame HDMI
  bool confirmed_on_ = false;  // F24 confirmou — HUD nao volta a off se o Toolhelp mentir
};

}  // namespace nexuscap
