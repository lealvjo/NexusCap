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
  bool hud_on = true;             // user HUD step; false = Off, toast still burns
  bool advanced_metrics = false;  // lobby "Metricas avancadas" → full HUD
  bool mini_overlay = false;      // lobby "Mini overlay" → single top bar
  bool overlay_no_bg = false;     // sem placa preta (compacto/mini/ADV/live)
  bool analyze_on = false;
  bool fake_capture = false;
  bool dlss_on = false;
  bool dlss_supported = false;
  std::string dlss_mode = "off";
  std::string dlss_status = "passthrough";
  std::string dlss_backend = "none";
  double dlss_ms = 0;
  std::string renodx_preset = "Default";  // 1x Default/Natural/Cinematic; 1.5x/2x/3x Model A/B/C
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
  int fg_flow_scale = 80;
  int fg_queue = 0;
  bool fg_adaptive = false;
  int fg_target_fps = 60;
  std::string fg_capture_api = "WGC";
  std::string session_notice;  // soft tip (FG / NR); empty = none
  bool hdmi_hold_notice = false;  // restart-style card burned into Present (exclusive FS)
  std::string hdmi_notice_title;
  std::string hdmi_notice_detail;
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
  // Live overlay (right edge). When active, mini/ADV HUD share is suppressed.
  bool live_overlay = false;
  bool overlay_console = false;  // optional console name on overlays
  int live_console = 0;  // LiveConsole
  int nr_pass_count = 1;
  bool feed_on = true;
  // Perf/benchmark HUD: -2 off, -1 loading, 0..40 remaining seconds.
  int perf_remain_sec = -2;
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
};

// "A aplicar o motor" — janela sem owner que segura o ultimo frame (GDI) enquanto
// o processo novo le o INI. Sem ela a tela apagava e parecia que o app tinha fechado.
class RestartNotice {
public:
  ~RestartNotice() { destroy(); }
  bool show(HINSTANCE hi, const std::string& title, const std::string& detail);
  void destroy();
  void pump() const;  // repinta enquanto a sessão está sendo desmontada
  bool visible() const { return hwnd_ != nullptr; }
  bool has_hold_image() const { return hold_bmp_ != nullptr; }

private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  void paint(HDC hdc) const;
  void load_hold_bmp();
  void release_hold_bmp();

  HWND hwnd_ = nullptr;
  std::wstring title_;
  std::wstring detail_;
  HBITMAP hold_bmp_ = nullptr;
  int hold_w_ = 0;
  int hold_h_ = 0;
};

bool save_motor_hold_from_hwnd(HWND hwnd);
void write_motor_hold_ready();
void reset_motor_hold_ready();
void clear_motor_hold_files();
bool motor_hold_image_exists();
bool wait_motor_hold_ready(int timeout_ms, RestartNotice* notice = nullptr);

// Right-edge live strip: console / DLSS 1x–3x+ / FG x2-x4|off / FPS / frametime sparkline.
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
  void set_present_interval(unsigned interval) { present_interval_ = interval > 0 ? interval : 1u; }
  unsigned present_interval() const { return present_interval_; }
  void set_scale_percent(int percent);  // 25–100; lower = softer image, less GPU work
  int scale_percent() const { return scale_percent_; }
  // Under RenoDX the app RT stays native and the scale lives in the Feed — report that
  // one in the HUD, otherwise the overlay shows 100% while DLSS5 works at 50%.
  void set_hud_work_scale_percent(int percent) { hud_work_scale_percent_ = percent; }
  // CFG da sessão (ImGui): desenha no backbuffer logo antes do Present, como o mini overlay.
  using UiDrawFn = void (*)(void* user);
  void set_ui_draw_callback(UiDrawFn fn, void* user) {
    ui_draw_ = fn;
    ui_draw_user_ = user;
  }
  // Espaco/intervalo reais no decode NV12 (mesmo contrato do preview CPU).
  void set_color_format(int space, int range);
  int color_space() const { return color_space_; }
  int color_range() const { return color_range_; }
  ID3D11Device* device() const;
  ID3D11DeviceContext* context() const;
  bool present_frame(const FramePayload& frame, OverlayState& overlay, double& present_ms, std::string& err);
  void present_black();
  // Same HWND: destroi e cria a swapchain para o ReShade reler o INI sem fechar a janela.
  bool recycle_swapchain();
  uint32_t swapchain_flags() const { return swap_flags_; }
  bool waitable() const { return swap_waitable_; }

private:
  bool create_nv12_textures(int w, int h, std::string& err);
  bool create_color_rt(int w, int h, std::string& err);
  bool create_pipeline(std::string& err);
  bool create_blit_pipeline(std::string& err);
  void burn_mini_overlay(const OverlayState& overlay);
  void burn_live_overlay(const OverlayState& overlay);
  void burn_panel_overlay(const OverlayState& overlay);
  void burn_session_notice(const OverlayState& overlay);
  void burn_hdmi_hold_notice(const OverlayState& overlay);
  bool ensure_hud_tex(int w, int h);
  bool ensure_toast_tex(int w, int h);
  void upload_hud_bits(const void* bits, int w, int h);
  void upload_toast_bits(const void* bits, int w, int h);
  void blit_hud(int x, int y, int w, int h);
  void blit_overlay_srv(ID3D11ShaderResourceView* srv, int x, int y, int w, int h);

  HWND hwnd_ = nullptr;
  int back_w_ = 0;
  int back_h_ = 0;
  int tex_w_ = 0;
  int tex_h_ = 0;
  uint32_t swap_flags_ = 0;
  bool swap_waitable_ = false;
  bool vsync_ = false;
  unsigned present_interval_ = 1;
  int scale_percent_ = 100;
  int hud_work_scale_percent_ = 0;  // 0 = HUD follows the app RT scale
  int color_space_ = 0;
  int color_range_ = 0;
  UiDrawFn ui_draw_ = nullptr;
  void* ui_draw_user_ = nullptr;
  IFrameEnhance* enhance_ = nullptr;
  struct Impl;
  Impl* impl_ = nullptr;
};

HWND create_app_window(HINSTANCE hi, int w, int h, bool fullscreen, const wchar_t* title,
                       bool start_hidden = false);

void show_session_window(HWND hwnd, bool maximize = false);
void hide_session_window(HWND hwnd);

// Switch borderless fullscreen ↔ overlapped window and resize the swapchain client.
// Returns client width/height written into out_client_w/h when non-null.
bool apply_display_mode(HWND hwnd, RendererD3D11& renderer, bool fullscreen, int signal_w, int signal_h,
                        int* out_client_w = nullptr, int* out_client_h = nullptr);

void set_thread_priorities();

}  // namespace nexuscap
