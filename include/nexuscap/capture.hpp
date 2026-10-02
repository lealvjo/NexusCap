#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nexuscap/capture_format.hpp"
#include "nexuscap/clock.hpp"
#include "nexuscap/frame_slot.hpp"

namespace nexuscap {

struct CaptureStartOptions {
  int width = 2560;
  int height = 1440;
  double fps = 60.0;
  int video_fourcc = static_cast<int>(kDefaultVideoFourcc);
  int color_space = static_cast<int>(kDefaultColorSpace);
  int color_range = static_cast<int>(kDefaultColorRange);
  int capture_buffering = static_cast<int>(kDefaultCaptureBuffering);
  // Recovery: keep the pin's negotiated WxH when HDMI comes back at another advertised mode.
  bool follow_hdmi_size = false;
  // Recovery already holds last_good on screen — don't block 0.9–3.5 s waiting for BufferCB.
  bool wait_first_frame = true;
  // Perf-test: prime the ring but keep clip 0 until rebase_playhead (NR/FG ready).
  bool hold_playhead = false;
  // Janela: HWND escolhida no picker. 0 = achar pelo titulo.
  uint64_t hwnd = 0;
  // Janela: a thread sobe e o Present segue preto. O StartCapture não trava a sessão.
  bool async_open = false;
  // USB E_FAIL: um recover interno (settle + NV12, buffering off). Evita recurse infinito.
  bool skip_dshow_recover = false;
};

// HDMI (placa) e o default. Janela (WGC) e opt-in — nao quebra o recover DShow.
enum class CaptureSourceKind : int { Hdmi = 0, Window = 1 };

inline int clamp_capture_source(int v) {
  return v == static_cast<int>(CaptureSourceKind::Window) ? static_cast<int>(CaptureSourceKind::Window)
                                                          : static_cast<int>(CaptureSourceKind::Hdmi);
}

inline bool capture_source_is_window(int v) {
  return clamp_capture_source(v) == static_cast<int>(CaptureSourceKind::Window);
}

inline int lobby_boot_capture_source() { return static_cast<int>(CaptureSourceKind::Hdmi); }

inline const char* capture_source_label(int v) {
  return capture_source_is_window(v) ? "Janela" : "Placa HDMI";
}

inline const char* capture_source_card_title(bool window) { return window ? "Janela" : "HDMI"; }

inline const char* capture_source_card_sub(bool window) {
  return window ? "qualquer janela" : "Placa de captura";
}

inline const char* window_capture_pick_hint() { return "Escolha uma janela"; }

inline const char* capture_factory_kind_name(bool fake, bool perf, bool window_source) {
  if (fake) return "fake";
  if (perf) return "file";
  if (window_source) return "window";
  return "dshow";
}

inline bool hdmi_recover_enabled(bool synthetic, bool app_relaunching, bool window_source) {
  return !synthetic && !app_relaunching && !window_source;
}

inline bool session_hdmi_audio_wanted(bool synthetic, bool smoke, bool window_source) {
  return !synthetic && !smoke && !window_source;
}

struct CapturableWindowInfo {
  uint64_t hwnd = 0;
  std::string title;
  std::string class_name;
};

inline bool window_capture_is_self_class(const char* cls) {
  return cls && std::strncmp(cls, "NexusCap", 8) == 0;
}

inline bool window_capture_is_self_title(const char* title) {
  return title && std::strncmp(title, "Nexus Cap", 9) == 0;
}

inline bool window_capture_list_candidate(bool visible, bool iconic, bool toolwindow, bool owned, bool cloaked,
                                          bool is_self, const char* title) {
  if (!visible || iconic || toolwindow || owned || cloaked || is_self) return false;
  return title && title[0] != '\0';
}

// Janela de jogo no mesmo processo (PCSX2 GS): pode ser owned e sem titulo.
inline bool window_capture_process_view_candidate(bool visible, bool iconic, bool toolwindow, bool cloaked,
                                                  bool is_self, int client_w, int client_h) {
  if (!visible || iconic || toolwindow || cloaked || is_self) return false;
  return client_w >= 64 && client_h >= 64;
}

inline bool window_title_matches_capture(const std::string& have, const std::string& want) {
  if (want.empty() || have.empty()) return false;
  if (have == want) return true;
  return have.find(want) != std::string::npos;
}

inline int even_capture_dim(int v) {
  if (v < 2) return 2;
  return v & ~1;
}

inline void copy_nv12_from_pitch(const uint8_t* src, int pitch, int width, int height, uint8_t* dst) {
  if (!src || !dst || pitch < width || width < 2 || height < 2) return;
  width = even_capture_dim(width);
  height = even_capture_dim(height);
  const size_t w = static_cast<size_t>(width);
  const size_t h = static_cast<size_t>(height);
  const size_t p = static_cast<size_t>(pitch);
  for (int y = 0; y < height; ++y) {
    std::memcpy(dst + static_cast<size_t>(y) * w, src + static_cast<size_t>(y) * p, w);
  }
  const uint8_t* uv = src + p * h;
  uint8_t* uv_dst = dst + w * h;
  for (int y = 0; y < height / 2; ++y) {
    std::memcpy(uv_dst + static_cast<size_t>(y) * w, uv + static_cast<size_t>(y) * p, w);
  }
}

inline void bgra_to_nv12(const uint8_t* bgra, int pitch, int width, int height, uint8_t* nv12) {
  if (!bgra || !nv12 || pitch < width * 4) return;
  width = even_capture_dim(width);
  height = even_capture_dim(height);
  const size_t y_stride = static_cast<size_t>(width);
  uint8_t* yplane = nv12;
  uint8_t* uvplane = nv12 + y_stride * static_cast<size_t>(height);
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = bgra + static_cast<size_t>(y) * static_cast<size_t>(pitch);
    for (int x = 0; x < width; ++x) {
      const int b = row[static_cast<size_t>(x) * 4];
      const int g = row[static_cast<size_t>(x) * 4 + 1];
      const int r = row[static_cast<size_t>(x) * 4 + 2];
      int Y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
      if (Y < 16) Y = 16;
      if (Y > 235) Y = 235;
      yplane[static_cast<size_t>(y) * y_stride + static_cast<size_t>(x)] = static_cast<uint8_t>(Y);
    }
  }
  for (int y = 0; y < height; y += 2) {
    const uint8_t* row0 = bgra + static_cast<size_t>(y) * static_cast<size_t>(pitch);
    const uint8_t* row1 = bgra + static_cast<size_t>(y + 1) * static_cast<size_t>(pitch);
    uint8_t* uv = uvplane + static_cast<size_t>(y / 2) * y_stride;
    for (int x = 0; x < width; x += 2) {
      const int b = (row0[x * 4] + row0[(x + 1) * 4] + row1[x * 4] + row1[(x + 1) * 4]) / 4;
      const int g = (row0[x * 4 + 1] + row0[(x + 1) * 4 + 1] + row1[x * 4 + 1] + row1[(x + 1) * 4 + 1]) / 4;
      const int r = (row0[x * 4 + 2] + row0[(x + 1) * 4 + 2] + row1[x * 4 + 2] + row1[(x + 1) * 4 + 2]) / 4;
      int U = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
      int V = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
      if (U < 16) U = 16;
      if (U > 240) U = 240;
      if (V < 16) V = 16;
      if (V > 240) V = 240;
      uv[x] = static_cast<uint8_t>(U);
      uv[x + 1] = static_cast<uint8_t>(V);
    }
  }
}

// Um quadro JPEG da placa vira o NV12 do tamanho pedido. O caminho NV12 nao usa isto.
bool mjpeg_jpeg_to_nv12(const uint8_t* jpeg, size_t jpeg_len, int width, int height,
                        std::vector<uint8_t>& bgra_scratch, uint8_t* nv12, size_t nv12_cap);

std::vector<CapturableWindowInfo> list_capturable_windows(uint64_t exclude_hwnd = 0);
uint64_t find_capturable_hwnd(const std::string& title, uint64_t exclude_hwnd = 0);

class CaptureBase {
public:
  virtual ~CaptureBase() = default;
  virtual bool start(const std::string& device_name, const CaptureStartOptions& opt, FrameSlot& slot,
                     IClock& clock, std::string& err) = 0;
  bool start(const std::string& device_name, int width, int height, double fps, FrameSlot& slot,
             IClock& clock, std::string& err) {
    CaptureStartOptions opt;
    opt.width = width;
    opt.height = height;
    opt.fps = fps;
    return start(device_name, opt, slot, clock, err);
  }
  virtual void stop() = 0;
  virtual bool running() const = 0;
  virtual uint64_t frames() const = 0;
  virtual std::string opened_device() const { return {}; }
  virtual int opened_width() const { return 0; }
  virtual int opened_height() const { return 0; }
  virtual int opened_fourcc() const { return static_cast<int>(kDefaultVideoFourcc); }
  virtual size_t last_sample_bytes() const { return 0; }
  virtual uint64_t rejected_samples() const { return 0; }
  virtual bool finished() const { return false; }
  // WGC async: a sessão pinta preto até o worker abrir. Erro chega depois, sem travar o Present.
  virtual bool startup_pending() const { return false; }
  virtual bool take_startup_error(std::string& /*err*/) { return false; }
  virtual void rebase_playhead(double /*t0_ms*/) {}
};

class FakeCapture final : public CaptureBase {
public:
  bool start(const std::string& device_name, const CaptureStartOptions& opt, FrameSlot& slot, IClock& clock,
             std::string& err) override;
  void stop() override;
  bool running() const override { return running_; }
  uint64_t frames() const override { return frames_; }
  std::string opened_device() const override { return opened_; }
  int opened_width() const override { return width_; }
  int opened_height() const override { return height_; }
  int opened_fourcc() const override { return static_cast<int>(VideoFourcc::Nv12); }

private:
  void thread_main();

  FrameSlot* slot_ = nullptr;
  IClock* clock_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  double fps_ = 60;
  std::string opened_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> frames_{0};
  std::thread th_;
};

inline double file_cap_src_fps_or(double native_fps) { return native_fps > 1.0 ? native_fps : 30.0; }

inline bool file_cap_is_integer_scale(int sw, int sh, int dw, int dh) {
  return sw > 0 && sh > 0 && dw >= sw && dh >= sh && (dw % sw) == 0 && (dh % sh) == 0 && (dw / sw) == (dh / sh);
}

// Keep the 30/60 grid. Catch-up bursts make the overlay spike red.
inline bool file_cap_repeat_ok(int w, int h, const std::vector<uint8_t>& nv12) {
  if (w <= 0 || h <= 0) return false;
  const size_t need = static_cast<size_t>(w) * static_cast<size_t>(h) * 3u / 2u;
  return nv12.size() >= need;
}

// Virtual capture pin: same 30|60 clamp as clamp_perf_test_fps (no extra tables).
inline double virtual_pin_fps(double plan_fps) { return plan_fps < 45.0 ? 30.0 : 60.0; }

inline double virtual_pin_period_ms(double plan_fps) { return 1000.0 / virtual_pin_fps(plan_fps); }

inline uint64_t virtual_pin_src_index(double now_ms, double t0_ms, double plan_fps, uint64_t n = 0) {
  const double fps = virtual_pin_fps(plan_fps);
  const double t = (now_ms - t0_ms) / 1000.0;
  if (t <= 0.0) return 0;
  uint64_t i = static_cast<uint64_t>(std::floor(t * fps + 1e-9));
  if (n > 0) i %= n;
  return i;
}

// Content time is wall-clock. A 60 fps clip at plan 30 still maps t→floor(t*clip_fps).
inline uint64_t virtual_pin_clip_index(double now_ms, double t0_ms, double clip_fps, uint64_t n = 0) {
  const double fps = clip_fps > 1.0 ? clip_fps : 60.0;
  const double t = (now_ms - t0_ms) / 1000.0;
  if (t <= 0.0) return 0;
  uint64_t i = static_cast<uint64_t>(std::floor(t * fps + 1e-9));
  if (n > 0) i %= n;
  return i;
}

// Playhead off (A carregar): wall clock must not advance clip or pin time.
inline uint64_t virtual_pin_held_src_index(bool playhead_on, double now_ms, double t0_ms, double plan_fps,
                                          uint64_t n = 0) {
  if (!playhead_on) return 0;
  return virtual_pin_src_index(now_ms, t0_ms, plan_fps, n);
}

inline uint64_t virtual_pin_held_clip_index(bool playhead_on, double now_ms, double t0_ms, double clip_fps,
                                           uint64_t n = 0) {
  if (!playhead_on) return 0;
  return virtual_pin_clip_index(now_ms, t0_ms, clip_fps, n);
}

inline int virtual_pin_ring_frames(double plan_fps, double hold_sec = 1.5) {
  const double fps = virtual_pin_fps(plan_fps);
  const double sec = hold_sec >= 0.5 ? hold_sec : 1.5;
  int n = static_cast<int>(std::ceil(sec * fps));
  if (n < 8) n = 8;
  return n;
}

inline int virtual_pin_ring_frames_for(double plan_fps, int width, int height, double hold_sec = 1.5) {
  int n = virtual_pin_ring_frames(plan_fps, hold_sec);
  if (width <= 0 || height <= 0) return n;
  const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 3u / 2u;
  if (bytes == 0) return n;
  const size_t cap = 768ull * 1024ull * 1024ull;
  int maxn = static_cast<int>(cap / bytes);
  if (maxn < 16) maxn = 16;
  if (n > maxn) n = maxn;
  return n;
}

inline int virtual_pin_prime_frames(double plan_fps, double prime_sec = 1.0) {
  const double fps = virtual_pin_fps(plan_fps);
  const double sec = prime_sec >= 0.25 ? prime_sec : 1.0;
  int n = static_cast<int>(std::ceil(sec * fps));
  if (n < 4) n = 4;
  return n;
}

// Decode lag / hitch: never block Present. Serve latest written.
inline uint64_t virtual_pin_take_index(uint64_t want, uint64_t decoded) {
  if (decoded == 0) return 0;
  if (want < decoded) return want;
  return decoded - 1;
}

inline uint64_t virtual_pin_ahead(uint64_t decoded_next, uint64_t play, uint64_t clip_n, uint64_t ring_n) {
  if (ring_n == 0) return 0;
  if (clip_n == 0) {
    if (decoded_next <= play) return 0;
    return decoded_next - play;
  }
  const uint64_t p = play % clip_n;
  const uint64_t d = decoded_next % clip_n;
  return (d + clip_n - p) % clip_n;
}

inline bool virtual_pin_should_throttle_decode(uint64_t decoded_next, uint64_t play, uint64_t clip_n,
                                              uint64_t ring_n) {
  if (ring_n <= 1) return false;
  return virtual_pin_ahead(decoded_next, play, clip_n, ring_n) + 1 >= ring_n;
}

class VirtualCapturePin final : public CaptureBase {
public:
  bool start(const std::string& device_name, const CaptureStartOptions& opt, FrameSlot& slot, IClock& clock,
             std::string& err) override;
  void stop() override;
  bool running() const override { return running_; }
  uint64_t frames() const override { return frames_; }
  std::string opened_device() const override { return opened_; }
  int opened_width() const override { return width_; }
  int opened_height() const override { return height_; }
  int opened_fourcc() const override { return static_cast<int>(VideoFourcc::Nv12); }
  bool finished() const override { return finished_; }
  void rebase_playhead(double t0_ms) override {
    t0_ms_.store(t0_ms, std::memory_order_relaxed);
    playhead_on_.store(true, std::memory_order_relaxed);
  }

private:
  struct RingSlot {
    uint64_t clip_index = ~uint64_t{0};
    std::vector<uint8_t> nv12;
    int width = 0;
    int height = 0;
  };

  void decode_main();
  void publish_main();
  bool copy_ring_locked(uint64_t clip_i, FramePayload& out);

  FrameSlot* slot_ = nullptr;
  IClock* clock_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  double fps_ = 60;
  std::string path_;
  std::string opened_;
  std::string thread_err_;
  std::atomic<bool> running_{false};
  std::atomic<bool> finished_{false};
  std::atomic<bool> have_frame_{false};
  std::atomic<bool> primed_{false};
  std::atomic<bool> playhead_on_{false};
  std::atomic<uint64_t> frames_{0};
  std::atomic<uint64_t> clip_n_{0};
  std::atomic<uint64_t> decoded_next_{0};
  std::atomic<double> native_fps_{60.0};
  std::atomic<double> t0_ms_{0};
  std::mutex ring_mu_;
  std::vector<RingSlot> ring_;
  FramePayload last_nv12_;
  std::thread decode_th_;
  std::thread pub_th_;
};

// Old PTS publisher. Perf-test uses the virtual pin.
using FileCapture = VirtualCapturePin;

class DShowCapture final : public CaptureBase {
public:
  DShowCapture();
  ~DShowCapture() override;
  bool start(const std::string& device_name, const CaptureStartOptions& opt, FrameSlot& slot, IClock& clock,
             std::string& err) override;
  void stop() override;
  bool running() const override { return running_; }
  uint64_t frames() const override { return frames_; }
  std::string opened_device() const override { return opened_; }
  int opened_width() const override;
  int opened_height() const override;
  int opened_fourcc() const override { return opened_fourcc_; }
  size_t last_sample_bytes() const override { return last_sample_bytes_.load(std::memory_order_relaxed); }
  uint64_t rejected_samples() const override { return rejected_samples_.load(std::memory_order_relaxed); }

  void push_buffer(const uint8_t* data, size_t len);

private:
  void push_mjpeg_buffer(const uint8_t* data, size_t len);
  struct Impl;
  Impl* impl_ = nullptr;
  FrameSlot* slot_ = nullptr;
  IClock* clock_ = nullptr;
  std::string opened_;
  int opened_fourcc_ = static_cast<int>(kDefaultVideoFourcc);
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> frames_{0};
  std::atomic<size_t> last_sample_bytes_{0};
  std::atomic<uint64_t> rejected_samples_{0};
  // Reused NV12 scratch — avoids per-frame heap churn when resolution is stable.
  std::vector<uint8_t> nv12_scratch_;
  std::vector<uint8_t> mjpeg_bgra_;
};

// WGC de uma HWND. Publica NV12 no mesmo FrameSlot do HDMI.
class WindowCapture final : public CaptureBase {
public:
  WindowCapture();
  ~WindowCapture() override;
  bool start(const std::string& device_name, const CaptureStartOptions& opt, FrameSlot& slot, IClock& clock,
             std::string& err) override;
  void stop() override;
  bool running() const override { return running_; }
  uint64_t frames() const override { return frames_.load(std::memory_order_relaxed); }
  std::string opened_device() const override { return opened_; }
  int opened_width() const override { return width_.load(std::memory_order_relaxed); }
  int opened_height() const override { return height_.load(std::memory_order_relaxed); }
  int opened_fourcc() const override { return static_cast<int>(VideoFourcc::Nv12); }
  bool startup_pending() const override;
  bool take_startup_error(std::string& err) override;

private:
  struct Impl;
  Impl* impl_ = nullptr;
  std::string opened_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> frames_{0};
  std::atomic<int> width_{0};
  std::atomic<int> height_{0};
};

// USB capture cards need a beat after Stop/SetFormat before reopen (white screen if skipped).
inline int capture_device_settle_ms() { return 180; }

inline std::string dshow_hresult_hex(unsigned long hr) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string s = "0x";
  s.resize(10, '0');
  s[0] = '0';
  s[1] = 'x';
  for (int i = 0; i < 8; ++i) {
    s[static_cast<size_t>(9 - i)] = kHex[(hr >> (i * 4)) & 0xFu];
  }
  return s;
}

inline const char* hdmi_open_busy_hint() {
  return "A placa esta em uso. Fecha o OBS ou outro app no USB e clica Recarregar entradas.";
}

// So compartilhamento real do USB. 0x80040217 (nao conectou) e recusa de modo.
inline bool hdmi_hresult_is_device_busy(unsigned long hr) {
  switch (hr) {
    case 0x80070020ul:  // ERROR_SHARING_VIOLATION
    case 0x800700AAul:  // ERROR_BUSY
    case 0x80070005ul:  // E_ACCESSDENIED
    case 0x800704CBul:  // ERROR_DEVICE_IN_USE
      return true;
    default:
      return false;
  }
}

inline const char* hdmi_mode_unsupported_text() {
  return "A placa nao entregou video nessa configuracao. Ela nao tem suporte.";
}

inline std::string hdmi_mode_refused_text(int width, int height, double fps) {
  char buf[160];
  std::snprintf(buf, sizeof(buf),
                "A placa nao entregou video em %dx%d @ %.0f. Essa configuracao nao tem suporte.",
                (std::max)(0, width), (std::max)(0, height), fps > 1.0 ? fps : 0.0);
  return buf;
}

inline bool hdmi_open_err_is_busy(const std::string& e) {
  if (e.empty()) return false;
  if (e.find("nao tem suporte") != std::string::npos || e.find("nao entregou video") != std::string::npos) {
    return false;
  }
  return e.find("placa ocupada") != std::string::npos || e.find("em uso") != std::string::npos ||
         e.find("0x80070020") != std::string::npos || e.find("0x800700AA") != std::string::npos ||
         e.find("0x80070005") != std::string::npos || e.find("0x800704CB") != std::string::npos;
}

inline std::string hdmi_open_error_user(const std::string& raw) {
  if (raw.empty()) return {};
  if (raw.find("reabriu NV12") != std::string::npos || raw.find("usando NV12") != std::string::npos) {
    return "Placa reabriu em NV12.";
  }
  if (hdmi_open_err_is_busy(raw)) return hdmi_open_busy_hint();
  const auto named = raw.find("nao tem suporte");
  if (named != std::string::npos) {
    size_t end = named + 15;
    if (end < raw.size() && raw[end] == '.') ++end;
    return raw.substr(0, end);
  }
  if (raw.find("RenderStream") != std::string::npos || raw.find("MediaControl") != std::string::npos ||
      raw.find("SetFormat") != std::string::npos || raw.find("0x") != std::string::npos ||
      raw.find("A placa nao abriu") != std::string::npos || raw.find("nao abriu") != std::string::npos) {
    return hdmi_mode_unsupported_text();
  }
  return raw;
}

// Iniciar com --simulate-hdmi-busy: nao abre DShow; o mesmo aviso da placa ocupada.
inline bool session_hdmi_open_blocked_by_busy_sim(bool simulate, bool from_lobby, bool synthetic,
                                                 bool window_source) {
  return simulate && from_lobby && !synthetic && !window_source;
}

// Iniciar no lobby relanca com --autostart e perde a flag: o aviso tem que nascer aqui.
inline bool lobby_iniciar_blocked_by_hdmi_busy_sim(bool simulate, bool window_source) {
  return session_hdmi_open_blocked_by_busy_sim(simulate, true, false, window_source);
}

inline const char* session_capture_fail_title() { return "A placa nao entregou video"; }

inline std::string session_capture_fail_detail(const std::string& raw) {
  const std::string u = hdmi_open_error_user(raw);
  return u.empty() ? std::string(hdmi_open_busy_hint()) : u;
}

inline std::string session_capture_fail_notice(const std::string& raw) {
  return std::string(session_capture_fail_title()) + " — " + session_capture_fail_detail(raw);
}

// WGC TimeSpan é 100 ns. 10000 (1 ms) pede ~1000 fps e trava o Present do jogo:
// o mouse fica na rodinha e o áudio corta. Um frame do fps da sessão, como a placa.
inline int64_t wgc_min_update_ticks(double fps) {
  if (fps < 1.0) fps = 60.0;
  if (fps > 240.0) fps = 240.0;
  int64_t n = static_cast<int64_t>(10000000.0 / fps + 0.5);
  if (n < 41667) n = 41667;
  if (n > 10000000) n = 10000000;
  return n;
}

// Spike da janela: tempos do aviso até o primeiro frame. O HDMI não escreve aqui.
void note_window_boot_line(const char* msg);

// HDMI só no reinício com hold. Janela abre a tela preta primeiro; o WGC entra depois.
inline bool session_overlaps_capture_with_renderer(bool window_source, bool hold_boot, bool synthetic,
                                                  bool smoke) {
  if (synthetic || smoke || window_source) return false;
  return hold_boot;
}

// Vão entre chegadas da captura. O Present (DLSS ligado ou não) não entra.
// O número da barra não é a média desses vãos: um tick pulado puxaria o fps para baixo.
// 0 = sem amostra nova (primeiro quadro, hold ou repeat).
inline double overlay_capture_interval_ms(double cap_t, double last_cap_t, bool holding, bool repeating) {
  if (holding || repeating) return 0;
  if (!(last_cap_t > 0.0) || !(cap_t > last_cap_t)) return 0;
  return cap_t - last_cap_t;
}

// After MediaControl::Run, wait for first BufferCB. 4K/MJPEG needs more than 1080p.
inline int capture_first_frame_wait_ms(int width = 1920, int height = 1080) {
  const long long px = static_cast<long long>((std::max)(1, width)) * static_cast<long long>((std::max)(1, height));
  // ~0.25 ms per 1k pixels, floor 900, ceiling 3500 (4K60 ≈ 2s+).
  int ms = static_cast<int>(px / 4000);
  if (ms < 900) ms = 900;
  if (ms > 3500) ms = 3500;
  return ms;
}

// Prefer NV12 at UHD+ — MJPEG often "opens" then delivers no convertible samples (white/fake).
inline bool capture_mode_prefers_nv12(int width, int height) {
  return static_cast<long long>(width) * height >= 3840LL * 2160LL;
}

// NV12 entra sempre que essa resolucao e esse Hz publicam NV12, salvo escolha explicita de MJPEG.
// Sem NV12 na lista, fica o formato que a placa aceita.
inline int capture_format_for_mode(int /*width*/, int /*height*/, int saved, const std::vector<int>& accepted) {
  if (accepted.empty()) return clamp_video_fourcc(saved);
  const int want = clamp_video_fourcc(saved);
  if (want == static_cast<int>(VideoFourcc::Mjpeg) && video_fourcc_in_list(want, accepted)) return want;
  if (video_fourcc_in_list(static_cast<int>(VideoFourcc::Nv12), accepted))
    return static_cast<int>(VideoFourcc::Nv12);
  return resolve_video_fourcc_for_device(saved, accepted);
}

// 59.94 vira 60. Uma taxa fora dessas faixas continua o valor arredondado (90 fica 90).
inline double round_capture_fps(double fps) {
  if (!(fps > 1.0) || fps > 1000.0) return 0;
  if (fps > 23.5 && fps < 24.5) return 24.0;
  if (fps > 24.5 && fps < 25.5) return 25.0;
  if (fps > 29.0 && fps < 31.0) return 30.0;
  if (fps > 49.0 && fps < 51.0) return 50.0;
  if (fps > 59.0 && fps < 61.0) return 60.0;
  if (fps > 119.0 && fps < 121.0) return 120.0;
  if (fps > 143.0 && fps < 145.0) return 144.0;
  if (fps > 239.0 && fps < 241.0) return 240.0;
  return std::round(fps);
}

inline void capture_push_fps(std::vector<double>& out, double fps) {
  fps = round_capture_fps(fps);
  if (!(fps >= 10.0) || fps > 480.0) return;
  for (double e : out) {
    if (std::abs(e - fps) < 0.5) return;
  }
  out.push_back(fps);
}

inline bool capture_fps_listed(const std::vector<double>& rates, double fps) {
  const double want = round_capture_fps(fps);
  for (double r : rates) {
    if (std::abs(r - want) < 1.0) return true;
  }
  return false;
}

// Igual a 6.5.5: a taxa nomeada, as pontas do intervalo e os Hz comuns que cabem nele.
// 1440p com faixa ate 60 inclui 30. Intervalo travado fica so na taxa nomeada.
inline void capture_fps_from_driver_interval(double nominal, double fps_lo, double fps_hi,
                                            std::vector<double>& out) {
  capture_push_fps(out, nominal);
  if (!(fps_lo > 1.0) || !(fps_hi > 1.0)) return;
  if (fps_lo > fps_hi) std::swap(fps_lo, fps_hi);
  if (fps_hi > 480.0 || fps_hi <= fps_lo + 0.5) return;
  capture_push_fps(out, fps_lo);
  capture_push_fps(out, fps_hi);
  static const double kInside[] = {24.0, 25.0, 30.0, 50.0, 60.0, 120.0, 144.0, 240.0};
  for (double step : kInside) {
    if (step + 0.51 < fps_lo || step - 0.51 > fps_hi) continue;
    capture_push_fps(out, step);
  }
}

inline bool capture_driver_interval_covers_fps(double nominal, double fps_lo, double fps_hi,
                                              double requested) {
  if (!(requested >= 1.0)) return false;
  std::vector<double> rates;
  capture_fps_from_driver_interval(nominal, fps_lo, fps_hi, rates);
  const double want = round_capture_fps(requested);
  for (double r : rates) {
    if (std::abs(r - want) < 1.0) return true;
  }
  return false;
}

// O fourcc acompanha a resolucao e o Hz que a placa nomeou nesse cap.
inline bool capture_driver_interval_accepts_fps(double nominal, double fps_lo, double fps_hi,
                                               double requested) {
  return capture_driver_interval_covers_fps(nominal, fps_lo, fps_hi, requested);
}

std::vector<std::string> list_video_capture_devices();

struct CaptureMode {
  int width = 0;
  int height = 0;
  double fps = 0;
  std::string label;
  // false = ponta de faixa ainda sem frame. A resolucao existe; o Hz nao entra no combo.
  bool hz_discrete = true;
};

// Resolucoes unicas, na ordem da lista (pixels desc). O Hz fica num segundo menu.
inline void capture_unique_resolutions(const std::vector<CaptureMode>& modes, std::vector<int>& widths,
                                       std::vector<int>& heights) {
  widths.clear();
  heights.clear();
  for (const auto& m : modes) {
    if (m.width <= 0 || m.height <= 0) continue;
    bool seen = false;
    for (size_t i = 0; i < widths.size(); ++i) {
      if (widths[i] == m.width && heights[i] == m.height) {
        seen = true;
        break;
      }
    }
    if (seen) continue;
    widths.push_back(m.width);
    heights.push_back(m.height);
  }
}

// Hz que essa resolucao publicou, do maior para o menor.
inline void capture_hz_for_resolution(const std::vector<CaptureMode>& modes, int width, int height,
                                     std::vector<double>& hz) {
  hz.clear();
  for (const auto& m : modes) {
    if (m.width != width || m.height != height || !(m.fps >= 1.0)) continue;
    bool seen = false;
    for (double e : hz) {
      if (std::abs(e - m.fps) < 0.5) {
        seen = true;
        break;
      }
    }
    if (!seen) hz.push_back(m.fps);
  }
  std::sort(hz.begin(), hz.end(), [](double a, double b) { return a > b; });
}

// Um cap publicado vira o Hz que a placa nomeou. A faixa nao vira outro Hz.
inline void capture_modes_from_driver_cap(int width, int height, double nominal, double fps_lo, double fps_hi,
                                         std::vector<CaptureMode>& out) {
  if (width < 160 || height < 120) return;
  std::vector<double> rates;
  capture_fps_from_driver_interval(nominal, fps_lo, fps_hi, rates);
  for (double fps : rates) {
    fps = round_capture_fps(fps);
    if (!(fps >= 10.0)) continue;
    bool seen = false;
    for (auto& e : out) {
      if (e.width == width && e.height == height && std::abs(e.fps - fps) < 0.5) {
        e.hz_discrete = true;
        seen = true;
        break;
      }
    }
    if (seen) continue;
    CaptureMode m;
    m.width = width;
    m.height = height;
    m.fps = fps;
    m.hz_discrete = true;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%dx%d @ %.0f", width, height, fps);
    m.label = buf;
    out.push_back(m);
  }
}

// Indice exato (meio Hz) ou o mais perto. Lista vazia devolve 0.
inline int capture_hz_index_for_fps(const std::vector<double>& hz, double want) {
  if (hz.empty()) return 0;
  int best = 0;
  double best_d = 1e9;
  for (int i = 0; i < static_cast<int>(hz.size()); ++i) {
    const double d = std::abs(hz[static_cast<size_t>(i)] - want);
    if (d < 0.5) return i;
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  return best;
}

// Modo que a placa publicou e depois nao entregou video. Fica no menu, sem clique.
struct CaptureModeKey {
  int width = 0;
  int height = 0;
  double fps = 0;
};

inline bool capture_mode_key_unsupported(const std::vector<CaptureModeKey>& blocked, int width, int height,
                                        double fps) {
  for (const CaptureModeKey& b : blocked) {
    if (b.width == width && b.height == height && std::abs(b.fps - fps) < 1.0) return true;
  }
  return false;
}

inline void capture_note_mode_unsupported(std::vector<CaptureModeKey>& blocked, int width, int height, double fps) {
  if (width <= 0 || height <= 0 || !(fps >= 1.0)) return;
  if (capture_mode_key_unsupported(blocked, width, height, fps)) return;
  blocked.push_back(CaptureModeKey{width, height, fps});
}

inline void capture_clear_mode_unsupported(std::vector<CaptureModeKey>& blocked, int width, int height, double fps) {
  blocked.erase(std::remove_if(blocked.begin(), blocked.end(),
                               [&](const CaptureModeKey& b) {
                                 return b.width == width && b.height == height && std::abs(b.fps - fps) < 1.0;
                               }),
                blocked.end());
}

// Mesmo vizinho de capture_hz_index_for_fps, pulando o que ja nao entregou video nessa resolucao.
inline int capture_hz_index_enabled(const std::vector<double>& hz, double want, int width, int height,
                                   const std::vector<CaptureModeKey>& blocked) {
  if (hz.empty()) return 0;
  int best = -1;
  double best_d = 1e9;
  for (int i = 0; i < static_cast<int>(hz.size()); ++i) {
    if (capture_mode_key_unsupported(blocked, width, height, hz[static_cast<size_t>(i)])) continue;
    const double d = std::abs(hz[static_cast<size_t>(i)] - want);
    if (d < 0.5) return i;
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  return best >= 0 ? best : 0;
}

// Iniciar so entra com video. Configuracao que a placa ja recusou fica sem clique.
inline bool lobby_iniciar_blocked_by_unsupported(bool window_source, int width, int height, double fps,
                                                const std::vector<CaptureModeKey>& blocked) {
  if (window_source) return false;
  return capture_mode_key_unsupported(blocked, width, height, fps);
}

// MJPEG que nao entregou video. O Hz em NV12 continua clicavel.
struct CaptureFormatKey {
  int width = 0;
  int height = 0;
  double fps = 0;
  int fourcc = 0;
};

inline bool capture_format_key_unsupported(const std::vector<CaptureFormatKey>& blocked, int width, int height,
                                          double fps, int fourcc) {
  for (const CaptureFormatKey& b : blocked) {
    if (b.width == width && b.height == height && b.fourcc == fourcc && std::abs(b.fps - fps) < 1.0) return true;
  }
  return false;
}

inline void capture_note_format_unsupported(std::vector<CaptureFormatKey>& blocked, int width, int height, double fps,
                                           int fourcc) {
  if (width <= 0 || height <= 0 || !(fps >= 1.0)) return;
  if (capture_format_key_unsupported(blocked, width, height, fps, fourcc)) return;
  blocked.push_back(CaptureFormatKey{width, height, fps, fourcc});
}

inline void capture_clear_format_unsupported(std::vector<CaptureFormatKey>& blocked, int width, int height, double fps,
                                            int fourcc) {
  blocked.erase(std::remove_if(blocked.begin(), blocked.end(),
                               [&](const CaptureFormatKey& b) {
                                 return b.width == width && b.height == height && b.fourcc == fourcc &&
                                        std::abs(b.fps - fps) < 1.0;
                               }),
                blocked.end());
}

inline bool lobby_iniciar_blocked_by_format(bool window_source, int width, int height, double fps, int fourcc,
                                           const std::vector<CaptureFormatKey>& blocked) {
  if (window_source) return false;
  return capture_format_key_unsupported(blocked, width, height, fps, fourcc);
}

// Hz gravado que essa resolucao publicou. Fora da lista, o mais perto. Lista vazia mantem o gravado.
inline double capture_fps_saved_on_card(const std::vector<CaptureMode>& modes, int width, int height,
                                       double saved) {
  std::vector<double> hz;
  capture_hz_for_resolution(modes, width, height, hz);
  if (hz.empty()) return saved;
  return hz[static_cast<size_t>(capture_hz_index_for_fps(hz, saved))];
}

// Indice do modo publicado mais perto do Hz gravado nessa resolucao.
inline int capture_select_mode_index(const std::vector<CaptureMode>& modes, int width, int height, double saved) {
  if (modes.empty()) return 0;
  int matched = -1;
  int closest = -1;
  double closest_d = 1e9;
  for (int i = 0; i < static_cast<int>(modes.size()); ++i) {
    const CaptureMode& m = modes[static_cast<size_t>(i)];
    if (m.width != width || m.height != height || !(m.fps >= 1.0)) continue;
    const double d = std::abs(m.fps - saved);
    if (d < 0.5 && matched < 0) matched = i;
    if (d < closest_d) {
      closest_d = d;
      closest = i;
    }
  }
  if (matched >= 0) return matched;
  if (closest >= 0) return closest;
  return 0;
}

std::vector<CaptureMode> list_video_capture_modes(const std::string& device_name);

// Fourccs (VideoFourcc ints) the device offers for a given mode — dynamic like OBS.
std::vector<int> list_video_fourccs_for_mode(const std::string& device_name, int width, int height,
                                             double fps);

// Packed NV12 size of the opened capture mode (not a hardcoded 2560×1440).
inline size_t nv12_packed_bytes(int width, int height) {
  if (width <= 0 || height <= 0) return 0;
  const size_t w = static_cast<size_t>(width);
  const size_t h = static_cast<size_t>(height);
  return w * h + w * h / 2;
}

// Accept exact packed size or a small stride pad. Reject 4K-into-1440p / 1080p-into-1440p junk.
inline bool nv12_buffer_matches_mode(size_t len, int width, int height) {
  const size_t need = nv12_packed_bytes(width, height);
  if (need == 0 || len < need) return false;
  const size_t stride = (static_cast<size_t>((std::max)(width, 0)) + 63u) & ~size_t{63};
  const size_t h = static_cast<size_t>((std::max)(height, 0));
  const size_t padded = stride * h + stride * h / 2;
  const size_t slack = static_cast<size_t>((std::max)(width, 32)) * 8;
  const size_t max_ok = (std::max)(padded, need + slack);
  return len <= max_ok;
}

// Pick an advertised mode whose NV12 size matches `len` (HDMI may come back at another res).
inline bool guess_nv12_wh_from_bytes(size_t len, const std::vector<CaptureMode>& modes, int* out_w, int* out_h) {
  int best_w = 0;
  int best_h = 0;
  size_t best_need = 0;
  for (const auto& m : modes) {
    if (!nv12_buffer_matches_mode(len, m.width, m.height)) continue;
    const size_t need = nv12_packed_bytes(m.width, m.height);
    if (need < best_need) continue;
    const long long px = static_cast<long long>(m.width) * m.height;
    const long long best_px = static_cast<long long>(best_w) * best_h;
    if (need == best_need && px < best_px) continue;
    best_need = need;
    best_w = m.width;
    best_h = m.height;
  }
  if (best_w <= 0 || best_h <= 0) return false;
  if (out_w) *out_w = best_w;
  if (out_h) *out_h = best_h;
  return true;
}

inline bool hdmi_pick_reopen_size(size_t last_bytes, int user_w, int user_h, const std::vector<CaptureMode>& modes,
                                  int* out_w, int* out_h) {
  int gw = 0;
  int gh = 0;
  if (last_bytes > 0 && guess_nv12_wh_from_bytes(last_bytes, modes, &gw, &gh)) {
    if (out_w) *out_w = gw;
    if (out_h) *out_h = gh;
    return true;
  }
  if (user_w > 0 && user_h > 0) {
    if (out_w) *out_w = user_w;
    if (out_h) *out_h = user_h;
    return true;
  }
  return false;
}

// Silent hold of last_good (game switch hitch). Pin stays open — no Stop/Run.
inline int hdmi_signal_lost_ms() { return 40; }
// Gold card only if the console really dropped HDMI this long (unplug / no-signal).
inline int hdmi_notice_lost_ms() { return 2500; }
inline int hdmi_handshake_discard_frames() { return 2; }
// Wrong-size only (real mode change). Pink handshake never reopens.
inline int hdmi_reopen_after_return_ms() { return 5000; }
inline int hdmi_reopen_cooldown_ms() { return 2500; }
inline int hdmi_hold_present_gap_ms() { return 14; }
inline int hdmi_notice_dwell_ms() { return 3000; }
inline int hdmi_settle_good_frames() { return 1; }

// Limited-range black (Y=16, UV=128) — better than HDMI "no signal" magenta.
inline void nv12_fill_limited_black(std::vector<uint8_t>& out, int width, int height) {
  const size_t need = nv12_packed_bytes(width, height);
  out.assign(need, 128);
  if (width > 0 && height > 0 && need > 0) {
    std::fill(out.begin(), out.begin() + static_cast<size_t>(width) * static_cast<size_t>(height),
              static_cast<uint8_t>(16));
  }
}

// HDMI no-signal is often magenta/pink (even with "NO SIGNAL" text / a bit of noise).
inline bool nv12_looks_like_hdmi_junk(const uint8_t* nv12, int width, int height) {
  if (!nv12 || width < 16 || height < 16) return false;
  const int w = width;
  const int h = height;
  const size_t ysz = static_cast<size_t>(w) * static_cast<size_t>(h);
  const uint8_t* y = nv12;
  const uint8_t* uv = nv12 + ysz;
  int ymin = 255, ymax = 0, y_n = 0;
  const int ystep = (std::max)(8, h / 20);
  const int xstep = (std::max)(8, w / 20);
  for (int row = 0; row < h; row += ystep) {
    const uint8_t* line = y + static_cast<size_t>(row) * static_cast<size_t>(w);
    for (int col = 0; col < w; col += xstep) {
      const int v = line[col];
      if (v < ymin) ymin = v;
      if (v > ymax) ymax = v;
      ++y_n;
    }
  }
  if (y_n <= 0) return false;
  int uv_n = 0, magenta = 0, u_sum = 0, v_sum = 0;
  const int uv_h = h / 2;
  const int uv_ystep = (std::max)(4, uv_h / 16);
  const int uv_xstep = (std::max)(8, w / 16);
  for (int row = 0; row < uv_h; row += uv_ystep) {
    const uint8_t* line = uv + static_cast<size_t>(row) * static_cast<size_t>(w);
    for (int col = 0; col + 1 < w; col += uv_xstep) {
      const int u = line[col];
      const int vv = line[col + 1];
      ++uv_n;
      u_sum += u;
      v_sum += vv;
      if (u >= 138 && vv >= 138) ++magenta;
    }
  }
  if (uv_n <= 0) return false;
  const int mean_u = u_sum / uv_n;
  const int mean_v = v_sum / uv_n;
  const bool mostly_magenta = magenta * 10 >= uv_n * 3;  // >= 30%
  const bool mean_pink = mean_u >= 138 && mean_v >= 138;
  const bool flat_y = (ymax - ymin) < 90;
  return mean_pink || (flat_y && mostly_magenta);
}

struct Nv12ChromaMean {
  int u = 128;
  int v = 128;
  int y_range = 0;
  bool ok = false;
};

inline Nv12ChromaMean nv12_chroma_mean(const uint8_t* nv12, int width, int height) {
  Nv12ChromaMean m;
  if (!nv12 || width < 16 || height < 16) return m;
  const int w = width;
  const int h = height;
  const size_t ysz = static_cast<size_t>(w) * static_cast<size_t>(h);
  const uint8_t* y = nv12;
  const uint8_t* uv = nv12 + ysz;
  int ymin = 255, ymax = 0, y_n = 0;
  const int ystep = (std::max)(8, h / 20);
  const int xstep = (std::max)(8, w / 20);
  for (int row = 0; row < h; row += ystep) {
    const uint8_t* line = y + static_cast<size_t>(row) * static_cast<size_t>(w);
    for (int col = 0; col < w; col += xstep) {
      const int v = line[col];
      if (v < ymin) ymin = v;
      if (v > ymax) ymax = v;
      ++y_n;
    }
  }
  int uv_n = 0, u_sum = 0, v_sum = 0;
  const int uv_h = h / 2;
  const int uv_ystep = (std::max)(4, uv_h / 16);
  const int uv_xstep = (std::max)(8, w / 16);
  for (int row = 0; row < uv_h; row += uv_ystep) {
    const uint8_t* line = uv + static_cast<size_t>(row) * static_cast<size_t>(w);
    for (int col = 0; col + 1 < w; col += uv_xstep) {
      u_sum += line[col];
      v_sum += line[col + 1];
      ++uv_n;
    }
  }
  if (uv_n <= 0 || y_n <= 0) return m;
  m.u = u_sum / uv_n;
  m.v = v_sum / uv_n;
  m.y_range = ymax - ymin;
  m.ok = true;
  return m;
}

inline Nv12ChromaMean nv12_chroma_mean(const std::vector<uint8_t>& nv12, int width, int height) {
  const size_t need = nv12_packed_bytes(width, height);
  if (need == 0 || nv12.size() < need) return {};
  return nv12_chroma_mean(nv12.data(), width, height);
}

// Whole-frame jump toward magenta vs last good — catches 1–3 handshake flashes junk misses.
inline bool nv12_chroma_jumped_to_pink(const Nv12ChromaMean& prev, const Nv12ChromaMean& now) {
  if (!prev.ok || !now.ok) return false;
  if (now.u < 140 || now.v < 140) return false;
  if ((now.u - prev.u) + (now.v - prev.v) < 40) return false;
  return now.y_range < 110;
}

inline bool nv12_looks_like_hdmi_junk(const std::vector<uint8_t>& nv12, int width, int height) {
  const size_t need = nv12_packed_bytes(width, height);
  if (need == 0 || nv12.size() < need) return true;
  return nv12_looks_like_hdmi_junk(nv12.data(), width, height);
}

enum class HdmiRecoverAction {
  None,
  Present,
  Hold,
  Discard,
  Reopen,
};

struct HdmiRecover {
  double last_good_ms = 0;
  double lost_at_ms = 0;
  double returned_at_ms = 0;
  double last_reopen_ms = 0;
  int discard_left = 0;
  int good_streak = 0;
  bool lost = false;
  bool reopen_armed = false;
};

inline bool hdmi_can_reopen(const HdmiRecover& s, double now_ms) {
  if (s.last_reopen_ms > 0 && (now_ms - s.last_reopen_ms) < static_cast<double>(hdmi_reopen_cooldown_ms())) {
    return false;
  }
  if (s.returned_at_ms <= 0) return false;
  return (now_ms - s.returned_at_ms) >= static_cast<double>(hdmi_reopen_after_return_ms());
}

inline bool hdmi_late_vs_last_good(const HdmiRecover& s, double now_ms) {
  return s.last_good_ms > 0 && (now_ms - s.last_good_ms) >= static_cast<double>(hdmi_signal_lost_ms());
}

inline HdmiRecoverAction hdmi_on_idle(HdmiRecover& s, double now_ms) {
  if (s.last_good_ms <= 0) return HdmiRecoverAction::None;
  if (!s.lost && hdmi_late_vs_last_good(s, now_ms)) {
    s.lost = true;
    s.lost_at_ms = now_ms;
    s.reopen_armed = false;
    s.returned_at_ms = 0;
    s.discard_left = 0;
    s.good_streak = 0;
  }
  return s.lost ? HdmiRecoverAction::Hold : HdmiRecoverAction::None;
}

// size_ok = buffer matches opened WxH. content_ok = not pink/no-signal junk.
// Stay on the pin: pink/handshake holds last_good and Present resumes on the first good frame.
// Reopen only if the pin stays the wrong size (real mode change), never for magenta junk.
inline HdmiRecoverAction hdmi_on_sample(HdmiRecover& s, double now_ms, bool size_ok, bool content_ok) {
  if (!size_ok || !content_ok) {
    if (!s.lost) {
      if (s.last_good_ms > 0) s.lost = true;
      s.lost_at_ms = now_ms;
    }
    s.good_streak = 0;
    s.discard_left = 0;
    if (size_ok && !content_ok) {
      s.reopen_armed = false;
      return s.last_good_ms > 0 ? HdmiRecoverAction::Hold : HdmiRecoverAction::Discard;
    }
    if (s.returned_at_ms <= 0) s.returned_at_ms = now_ms;
    s.reopen_armed = true;
    if (hdmi_can_reopen(s, now_ms)) return HdmiRecoverAction::Reopen;
    return s.last_good_ms > 0 ? HdmiRecoverAction::Hold : HdmiRecoverAction::Discard;
  }
  s.lost = false;
  s.reopen_armed = false;
  s.returned_at_ms = 0;
  s.lost_at_ms = 0;
  s.discard_left = 0;
  s.good_streak = 0;
  s.last_good_ms = now_ms;
  return HdmiRecoverAction::Present;
}

inline HdmiRecoverAction hdmi_on_sample(HdmiRecover& s, double now_ms, bool size_ok) {
  return hdmi_on_sample(s, now_ms, size_ok, size_ok);
}

// A late Present of a good live frame is NR/vsync hitch, not the console dropping HDMI.
// Pink/wrong-size after a gap still counts as handshake.
inline HdmiRecoverAction hdmi_on_arrived(HdmiRecover& s, double now_ms, bool size_ok, bool content_ok) {
  if (size_ok && content_ok) {
    return hdmi_on_sample(s, now_ms, true, true);
  }
  if (hdmi_late_vs_last_good(s, now_ms)) {
    (void)hdmi_on_idle(s, now_ms);
  }
  return hdmi_on_sample(s, now_ms, size_ok, content_ok);
}

inline void hdmi_note_reopened(HdmiRecover& s, double now_ms) {
  s.last_reopen_ms = now_ms;
  s.last_good_ms = now_ms;
  s.lost = false;
  s.reopen_armed = false;
  s.returned_at_ms = 0;
  s.lost_at_ms = 0;
  s.discard_left = 0;
  s.good_streak = 0;
}

inline bool hdmi_lost_long_enough(const HdmiRecover& s, double now_ms) {
  if (!s.lost) return false;
  const double t0 = s.lost_at_ms > 0 ? s.lost_at_ms : s.last_good_ms;
  if (t0 <= 0) return false;
  return (now_ms - t0) >= static_cast<double>(hdmi_notice_lost_ms());
}

inline bool hdmi_keep_last_good(HdmiRecoverAction act) {
  return act == HdmiRecoverAction::Hold || act == HdmiRecoverAction::Discard;
}

// Delete / NR / motor / Pass Count / preset / escala / --autostart: hitch nosso, nao do console.
inline bool hdmi_blocked_by_nexuscap_restart(bool restart_pending, bool restart_notice_visible,
                                             bool autostart_warmup) {
  return restart_pending || restart_notice_visible || autostart_warmup;
}

// Card only after a real dropout (hdmi_notice_lost_ms) or while that card is still dwelling.
inline bool hdmi_notice_visible(bool holding, double now_ms, double notice_until_ms) {
  (void)holding;
  return notice_until_ms > 0.0 && now_ms < notice_until_ms;
}

}  // namespace nexuscap
