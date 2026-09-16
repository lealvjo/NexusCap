#pragma once
#include <algorithm>
#include <cmath>

namespace nexuscap {

struct WindowGeom {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

struct ViewportGeom {
  float x = 0;
  float y = 0;
  float w = 0;
  float h = 0;
  float scale = 1.f;
};

// Fullscreen must cover the monitor — never the capture/signal size.
// Using signal size when monitor is smaller causes a cropped/"zoomed" look.
inline WindowGeom fullscreen_window_geom(int monitor_w, int monitor_h) {
  WindowGeom g;
  g.x = 0;
  g.y = 0;
  g.w = monitor_w > 0 ? monitor_w : 1;
  g.h = monitor_h > 0 ? monitor_h : 1;
  return g;
}

// Windowed: prefer 1:1 client = signal, but never larger than the monitor.
// Bug: 1440p signal on 720p/1080p monitor created an off-screen / unusable window.
inline WindowGeom windowed_client_geom(int signal_w, int signal_h, int monitor_w = 0, int monitor_h = 0) {
  WindowGeom g;
  g.x = 0;
  g.y = 0;
  g.w = signal_w > 0 ? signal_w : 1;
  g.h = signal_h > 0 ? signal_h : 1;
  if (monitor_w > 0 && monitor_h > 0) {
    // Leave room for title bar / borders after AdjustWindowRect.
    const int avail_w = (std::max)(320, monitor_w - 96);
    const int avail_h = (std::max)(240, monitor_h - 120);
    if (g.w > avail_w || g.h > avail_h) {
      const float sx = static_cast<float>(avail_w) / static_cast<float>(g.w);
      const float sy = static_cast<float>(avail_h) / static_cast<float>(g.h);
      const float s = (std::min)(sx, sy);
      g.w = (std::max)(2, static_cast<int>(g.w * s) & ~1);
      g.h = (std::max)(2, static_cast<int>(g.h * s) & ~1);
    }
  }
  return g;
}

inline bool windowed_fits_monitor(int client_w, int client_h, int monitor_w, int monitor_h) {
  if (monitor_w <= 0 || monitor_h <= 0) return true;
  return client_w <= monitor_w && client_h <= monitor_h;
}

inline bool fullscreen_would_crop(int monitor_w, int monitor_h, int window_w, int window_h) {
  return window_w > monitor_w || window_h > monitor_h;
}

// Fit signal into output with letterbox (no crop / no stretch aspect change).
inline ViewportGeom fit_letterbox(int signal_w, int signal_h, int out_w, int out_h) {
  ViewportGeom v;
  if (signal_w <= 0 || signal_h <= 0 || out_w <= 0 || out_h <= 0) {
    v.w = static_cast<float>(out_w > 0 ? out_w : 1);
    v.h = static_cast<float>(out_h > 0 ? out_h : 1);
    return v;
  }
  const float sx = static_cast<float>(out_w) / static_cast<float>(signal_w);
  const float sy = static_cast<float>(out_h) / static_cast<float>(signal_h);
  const float s = (std::min)(sx, sy);
  v.scale = s;
  v.w = static_cast<float>(signal_w) * s;
  v.h = static_cast<float>(signal_h) * s;
  v.x = (static_cast<float>(out_w) - v.w) * 0.5f;
  v.y = (static_cast<float>(out_h) - v.h) * 0.5f;
  return v;
}

// Stretch to fill the entire output — no black bars (OBS-style "preencher janela").
// Used for session windowed/fullscreen present (lobby preview uses fit_letterbox).
inline ViewportGeom fit_fill(int /*signal_w*/, int /*signal_h*/, int out_w, int out_h) {
  ViewportGeom v;
  v.x = 0.f;
  v.y = 0.f;
  v.w = static_cast<float>(out_w > 0 ? out_w : 1);
  v.h = static_cast<float>(out_h > 0 ? out_h : 1);
  v.scale = 1.f;
  return v;
}

// True when viewport leaves empty strips (letterbox/pillarbox) on the output.
inline bool viewport_has_black_bars(const ViewportGeom& v, int out_w, int out_h, float eps = 0.75f) {
  if (out_w <= 0 || out_h <= 0) return false;
  if (v.x > eps || v.y > eps) return true;
  if (v.w + v.x < static_cast<float>(out_w) - eps) return true;
  if (v.h + v.y < static_cast<float>(out_h) - eps) return true;
  return false;
}

// Left/right pillar bars only (preview centered in a wider band).
inline bool viewport_has_side_bars(const ViewportGeom& v, int out_w, float eps = 0.75f) {
  if (out_w <= 0) return false;
  if (v.x > eps) return true;
  if (v.w + v.x < static_cast<float>(out_w) - eps) return true;
  return false;
}

// Where lobby HUD sits: left black bar when wide enough, else left edge of the band.
struct LobbyHudLeftBarLayout {
  float x = 0.f;
  float y = 0.f;
  float max_w = 0.f;
  bool on_black_bar = false;
};

inline LobbyHudLeftBarLayout lobby_hud_left_bar_layout(const ViewportGeom& box, float band_w,
                                                      float band_h, float preferred_hud_w,
                                                      float pad = 8.f) {
  LobbyHudLeftBarLayout L;
  L.y = pad;
  const float left_bar = box.x;
  if (left_bar >= 72.f) {
    L.on_black_bar = true;
    L.x = pad;
    L.max_w = (std::max)(48.f, left_bar - pad * 2.f);
  } else {
    L.on_black_bar = false;
    L.x = pad;
    L.max_w = (std::max)(48.f, (std::min)(preferred_hud_w, band_w - pad * 2.f));
  }
  (void)band_h;
  return L;
}

// Lobby side panels sized like letterbox black bars (pillarbox), with usable min/max.
struct LobbySidePanelLayout {
  float left_w = 300.f;
  float right_w = 300.f;
  float mid_w = 600.f;
};

inline LobbySidePanelLayout lobby_side_panel_layout(float total_w, float total_h, int signal_w, int signal_h,
                                                   float min_side = 375.f, float max_side = 525.f,
                                                   float min_mid = 280.f) {
  LobbySidePanelLayout out;
  if (total_w < 1.f) total_w = 1.f;
  if (total_h < 1.f) total_h = 1.f;
  min_side = (std::max)(160.f, min_side);
  max_side = (std::max)(min_side, max_side);
  min_mid = (std::max)(160.f, min_mid);

  const auto box = fit_letterbox(signal_w > 0 ? signal_w : 16, signal_h > 0 ? signal_h : 9,
                                 static_cast<int>(total_w), static_cast<int>(total_h));
  float left = box.x;
  float right = total_w - (box.x + box.w);
  // No/thin bars → still keep usable side menus; clamp to [min,max].
  left = (std::max)(min_side, (std::min)(max_side, (std::max)(left, min_side)));
  right = (std::max)(min_side, (std::min)(max_side, (std::max)(right, min_side)));

  float mid = total_w - left - right;
  if (mid < min_mid) {
    const float need = min_mid - mid;
    const float cut = need * 0.5f;
    left = (std::max)(min_side, left - cut);
    right = (std::max)(min_side, right - cut);
    mid = total_w - left - right;
    if (mid < min_mid) {
      // Last resort: shrink sides evenly to free mid.
      const float excess = left + right - (total_w - min_mid);
      if (excess > 0.f) {
        left = (std::max)(120.f, left - excess * 0.5f);
        right = (std::max)(120.f, right - excess * 0.5f);
        mid = total_w - left - right;
      }
    }
  }
  out.left_w = left;
  out.right_w = right;
  out.mid_w = (std::max)(1.f, mid);
  return out;
}

// Relative luminance of an opaque RGB color (0..1). HUD-on-black needs high contrast.
inline float rgb_relative_luminance(int r, int g, int b) {
  const float R = static_cast<float>(r) / 255.f;
  const float G = static_cast<float>(g) / 255.f;
  const float B = static_cast<float>(b) / 255.f;
  return 0.2126f * R + 0.7152f * G + 0.0722f * B;
}

inline bool hud_color_readable_on_black(int r, int g, int b, float min_luma = 0.45f) {
  return rgb_relative_luminance(r, g, b) >= min_luma;
}

}  // namespace nexuscap
