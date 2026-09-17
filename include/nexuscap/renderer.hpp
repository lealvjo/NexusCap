#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <string>
#include <vector>

#include "nexuscap/enhance.hpp"
#include "nexuscap/frame_slot.hpp"

namespace nexuscap {

struct OverlayState {
  bool visible = true;
  bool advanced_metrics = false;  // lobby "Metricas avancadas" → full HUD
  bool mini_overlay = false;      // lobby "Mini overlay" → single top bar
  bool analyze_on = false;
  bool fake_capture = false;
  bool dlss_on = false;
  bool dlss_supported = false;
  std::string dlss_mode = "off";
  std::string dlss_status = "passthrough";
  std::string dlss_backend = "none";
  double dlss_ms = 0;
  std::string renodx_preset = "Default";  // Default / Natural / Cinematic (ADV HUD)
  bool renodx_on = false;
  bool sync_image = false;  // vsync Present(1) when true
  bool hw_info = false;     // painel Infos GPU / CPU
  double cpu_percent = 0;
  double cpu_temp_c = 0;
  double gpu_percent = 0;
  double gpu_temp_c = 0;
  double vram_used_mb = 0;
  double vram_budget_mb = 0;
  double ram_used_mb = 0;
  double ram_total_mb = 0;
  double power_w = 0;
  bool cpu_ok = false;
  bool cpu_temp_ok = false;
  bool gpu_ok = false;
  bool gpu_temp_ok = false;
  bool vram_ok = false;
  bool ram_ok = false;
  bool power_ok = false;
  std::string cpu_name;  // short HUD label, e.g. i5-11400F
  std::string gpu_name;  // short HUD label, e.g. RTX 5070
  std::string device;
  int capture_w = 0;
  int capture_h = 0;
  int output_w = 0;
  int output_h = 0;
  int scale_percent = 100;
  int work_w = 0;  // capture scaled by scale_percent
  int work_h = 0;
  double target_fps = 60.0;
  double fps = 0;
  double fps_smooth = 0;
  double fps_fg_est = 0;  // estimated output FPS when FG on (cap * multiplier)
  double frame_ms = 0;
  double ft_avg_ms = 0;
  double ft_p95_ms = 0;
  double ft_max_ms = 0;
  bool ft_stable = false;
  std::wstring ft_spark;
  std::vector<double> ft_hist;  // oldest→newest, for MSI-style line
  bool fg_on = false;
  int fg_multiplier = 1;
  std::string fg_status = "off";
  std::string fg_backend = "none";
  int fg_flow_scale = 100;
  std::string fg_capture_api = "WGC";
  std::string session_notice;  // soft tip (FG / NR); empty = none
  double e2e_ms = 0;
  double present_ms = 0;
  uint64_t frames = 0;
  uint64_t dropped = 0;
  uint64_t stalls_window = 0;
  uint64_t clusters = 0;
  bool has_stats600 = false;
  double s600_fps = 0;
  double s600_p95_e2e = 0;
  double s600_p95_present = 0;
  double s600_max_interval = 0;
  uint64_t s600_stalls = 0;
  // Secret live overlay (right edge). When active, mini/ADV HUD share is suppressed.
  bool live_overlay = false;
  int live_console = 0;  // LiveConsole
};

class MetricsHud {
public:
  ~MetricsHud() { destroy(); }
  bool create(HWND owner, HINSTANCE hi);
  void destroy();
  void update(const OverlayState& state);
  void set_visible(bool visible);

private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  void paint(HDC hdc) const;
  void reposition();

  HWND hwnd_ = nullptr;
  HWND owner_ = nullptr;
  OverlayState state_{};
  bool visible_ = true;
  DWORD last_paint_tick_ = 0;
  std::string last_notice_;
};

// Thin topmost strip when "Ocultar lobby na captura" is OFF — proves the toggle does something.
class SessionControlPanel {
public:
  ~SessionControlPanel() { destroy(); }
  bool create(HWND owner, HINSTANCE hi);
  void destroy();
  void set_visible(bool visible);
  void reposition();
  bool visible() const { return visible_; }
  HWND hwnd() const { return hwnd_; }

private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  void paint(HDC hdc) const;

  HWND hwnd_ = nullptr;
  HWND owner_ = nullptr;
  bool visible_ = false;
};

// Painel "Infos GPU / CPU" — aparece só quando o toggle do lobby está ligado.
class HwInfoPanel {
public:
  ~HwInfoPanel() { destroy(); }
  bool create(HWND owner, HINSTANCE hi);
  void destroy();
  void update(const OverlayState& state);
  void set_visible(bool visible);
  void reposition();
  bool visible() const { return visible_; }

private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  void paint(HDC hdc) const;

  HWND hwnd_ = nullptr;
  HWND owner_ = nullptr;
  OverlayState state_{};
  bool visible_ = false;
};

// Secret right-edge overlay: Console / DLSS5 / FPS (+ FG bar) / frametime sparkline.
// Per-pixel alpha (UpdateLayeredWindow) — crisp on high-DPI; click opens console picker.
class LiveOverlay {
public:
  ~LiveOverlay() { destroy(); }
  bool create(HWND owner, HINSTANCE hi);
  void destroy();
  void update(const OverlayState& state);
  void set_visible(bool visible);
  void set_console(int live_console);
  int console() const { return console_; }
  // True once after the user picks a console from the click menu.
  bool take_console_changed();

private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  void present() const;
  void reposition();
  void show_console_menu();
  UINT dpi() const;

  HWND hwnd_ = nullptr;
  HWND owner_ = nullptr;
  OverlayState state_{};
  int console_ = 0;
  bool console_changed_ = false;
  bool visible_ = false;
  DWORD last_paint_tick_ = 0;
  mutable int layout_w_ = 420;
  mutable int layout_h_ = 26;
};

class RendererD3D11 {
public:
  ~RendererD3D11();
  bool init(HWND hwnd, int width, int height, std::string& err);
  void shutdown();
  void resize(int width, int height);
  void set_enhance(IFrameEnhance* enhance) { enhance_ = enhance; }
  void set_vsync(bool enabled) { vsync_ = enabled; }
  bool vsync() const { return vsync_; }
  void set_scale_percent(int percent);  // 25–100; lower = softer image, less GPU work
  int scale_percent() const { return scale_percent_; }
  // Espaco/intervalo reais no decode NV12 (mesmo contrato do preview CPU).
  void set_color_format(int space, int range);
  int color_space() const { return color_space_; }
  int color_range() const { return color_range_; }
  ID3D11Device* device() const;
  ID3D11DeviceContext* context() const;
  bool present_frame(const FramePayload& frame, OverlayState& overlay, double& present_ms, std::string& err);
  void present_black();
  uint32_t swapchain_flags() const { return swap_flags_; }
  bool waitable() const { return false; }

private:
  bool create_nv12_textures(int w, int h, std::string& err);
  bool create_color_rt(int w, int h, std::string& err);
  bool create_pipeline(std::string& err);
  bool create_blit_pipeline(std::string& err);
  void burn_mini_overlay(const OverlayState& overlay);

  HWND hwnd_ = nullptr;
  int back_w_ = 0;
  int back_h_ = 0;
  int tex_w_ = 0;
  int tex_h_ = 0;
  uint32_t swap_flags_ = 0;
  bool vsync_ = false;
  int scale_percent_ = 100;
  int color_space_ = 0;
  int color_range_ = 0;
  IFrameEnhance* enhance_ = nullptr;
  struct Impl;
  Impl* impl_ = nullptr;
};

HWND create_app_window(HINSTANCE hi, int w, int h, bool fullscreen, const wchar_t* title);

// Switch borderless fullscreen ↔ overlapped window and resize the swapchain client.
// Returns client width/height written into out_client_w/h when non-null.
bool apply_display_mode(HWND hwnd, RendererD3D11& renderer, bool fullscreen, int signal_w, int signal_h,
                        int* out_client_w = nullptr, int* out_client_h = nullptr);

void set_thread_priorities();

}  // namespace nexuscap
