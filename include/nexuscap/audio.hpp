#pragma once
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>

namespace nexuscap {

inline constexpr int kMinCaptureVolumePercent = 0;
inline constexpr int kMaxCaptureVolumePercent = 100;
inline constexpr int kDefaultCaptureVolumePercent = 100;

inline int clamp_capture_volume_percent(int v) {
  if (v < kMinCaptureVolumePercent) return kMinCaptureVolumePercent;
  if (v > kMaxCaptureVolumePercent) return kMaxCaptureVolumePercent;
  return v;
}

inline float capture_volume_gain(int percent) {
  return static_cast<float>(clamp_capture_volume_percent(percent)) / 100.f;
}

inline void apply_capture_volume_f32(float* samples, size_t count, float gain) {
  if (!samples || count == 0) return;
  if (gain <= 0.f) {
    for (size_t i = 0; i < count; ++i) samples[i] = 0.f;
    return;
  }
  if (gain >= 1.f) return;
  for (size_t i = 0; i < count; ++i) samples[i] *= gain;
}

inline void apply_capture_volume_s16(std::int16_t* samples, size_t count, float gain) {
  if (!samples || count == 0) return;
  if (gain <= 0.f) {
    for (size_t i = 0; i < count; ++i) samples[i] = 0;
    return;
  }
  if (gain >= 1.f) return;
  for (size_t i = 0; i < count; ++i) {
    samples[i] = static_cast<std::int16_t>(static_cast<float>(samples[i]) * gain);
  }
}

inline void apply_capture_volume_mix_pcm(void* out, size_t frames, int channels, int bits, float gain) {
  if (!out || frames == 0 || gain >= 1.f) return;
  const int ch = channels < 1 ? 1 : channels;
  const size_t samples = frames * static_cast<size_t>(ch);
  if (bits == 32) {
    apply_capture_volume_f32(static_cast<float*>(out), samples, gain);
    return;
  }
  if (bits == 16) {
    apply_capture_volume_s16(static_cast<std::int16_t*>(out), samples, gain);
    return;
  }
  if (gain <= 0.f) {
    const int bytes = bits / 8;
    if (bytes > 0) std::memset(out, 0, frames * static_cast<size_t>(bytes * ch));
  }
}

// Modo de som do HDMI. Padrao nao filtra. Gamer sobe o agudo. Graves sobe o grave.
// O pico passa por um limite para o realce nao estourar o fone.
inline constexpr int kCaptureTonePadrao = 0;
inline constexpr int kCaptureToneGamer = 1;
inline constexpr int kCaptureToneGraves = 2;

inline int clamp_capture_tone(int mode) {
  if (mode < kCaptureTonePadrao) return kCaptureTonePadrao;
  if (mode > kCaptureToneGraves) return kCaptureToneGraves;
  return mode;
}

struct CaptureToneBiquad {
  float b0 = 1.f;
  float b1 = 0.f;
  float b2 = 0.f;
  float a1 = 0.f;
  float a2 = 0.f;
  float z1 = 0.f;
  float z2 = 0.f;
};

struct CaptureToneFilter {
  int mode = kCaptureTonePadrao;
  int rate = 0;
  int channels = 0;
  CaptureToneBiquad ch[8]{};
};

inline void capture_tone_design(CaptureToneBiquad& q, int mode, int rate) {
  const float fs = rate < 8000 ? 48000.f : static_cast<float>(rate);
  const bool graves = clamp_capture_tone(mode) == kCaptureToneGraves;
  const float db = graves ? 10.f : 8.f;
  const float freq = graves ? 110.f : 2200.f;
  const float A = std::pow(10.f, db / 40.f);
  const float w0 = 6.28318530718f * freq / fs;
  const float cosw = std::cos(w0);
  const float sinw = std::sin(w0);
  const float alpha = sinw * 0.5f * std::sqrt(2.f);
  const float shelf = 2.f * std::sqrt(A) * alpha;
  float b0 = 1.f, b1 = 0.f, b2 = 0.f, a0 = 1.f, a1 = 0.f, a2 = 0.f;
  if (graves) {
    b0 = A * ((A + 1.f) - (A - 1.f) * cosw + shelf);
    b1 = 2.f * A * ((A - 1.f) - (A + 1.f) * cosw);
    b2 = A * ((A + 1.f) - (A - 1.f) * cosw - shelf);
    a0 = (A + 1.f) + (A - 1.f) * cosw + shelf;
    a1 = -2.f * ((A - 1.f) + (A + 1.f) * cosw);
    a2 = (A + 1.f) + (A - 1.f) * cosw - shelf;
  } else {
    b0 = A * ((A + 1.f) + (A - 1.f) * cosw + shelf);
    b1 = -2.f * A * ((A - 1.f) + (A + 1.f) * cosw);
    b2 = A * ((A + 1.f) + (A - 1.f) * cosw - shelf);
    a0 = (A + 1.f) - (A - 1.f) * cosw + shelf;
    a1 = 2.f * ((A - 1.f) - (A + 1.f) * cosw);
    a2 = (A + 1.f) - (A - 1.f) * cosw - shelf;
  }
  if (a0 < 1.e-8f && a0 > -1.e-8f) a0 = 1.f;
  const float trim = std::pow(10.f, -1.f / 20.f);
  const float inv = trim / a0;
  q.b0 = b0 * inv;
  q.b1 = b1 * inv;
  q.b2 = b2 * inv;
  q.a1 = a1 / a0;
  q.a2 = a2 / a0;
  q.z1 = 0.f;
  q.z2 = 0.f;
}

inline float capture_tone_tick(CaptureToneBiquad& q, float x) {
  const float y = q.b0 * x + q.z1;
  q.z1 = q.b1 * x - q.a1 * y + q.z2;
  q.z2 = q.b2 * x - q.a2 * y;
  if (y > 1.f) return 1.f;
  if (y < -1.f) return -1.f;
  return y;
}

inline void capture_tone_prepare(CaptureToneFilter& f, int mode, int rate, int channels) {
  const int m = clamp_capture_tone(mode);
  const int ch = channels < 1 ? 1 : (channels > 8 ? 8 : channels);
  if (f.mode == m && f.rate == rate && f.channels == ch) return;
  f.mode = m;
  f.rate = rate;
  f.channels = ch;
  for (int c = 0; c < ch; ++c) capture_tone_design(f.ch[c], m, rate);
}

inline void apply_capture_tone_f32(float* samples, size_t frames, int channels, CaptureToneFilter& f, int mode,
                                   int rate) {
  const int m = clamp_capture_tone(mode);
  if (!samples || frames == 0 || m == kCaptureTonePadrao) return;
  const int ch = channels < 1 ? 1 : channels;
  if (ch > 8) return;
  capture_tone_prepare(f, m, rate, ch);
  for (size_t i = 0; i < frames; ++i) {
    for (int c = 0; c < ch; ++c) {
      float& s = samples[i * static_cast<size_t>(ch) + static_cast<size_t>(c)];
      s = capture_tone_tick(f.ch[c], s);
    }
  }
}

inline void apply_capture_tone_s16(std::int16_t* samples, size_t frames, int channels, CaptureToneFilter& f, int mode,
                                   int rate) {
  const int m = clamp_capture_tone(mode);
  if (!samples || frames == 0 || m == kCaptureTonePadrao) return;
  const int ch = channels < 1 ? 1 : channels;
  if (ch > 8) return;
  capture_tone_prepare(f, m, rate, ch);
  const size_t n = frames * static_cast<size_t>(ch);
  for (size_t i = 0; i < n; ++i) {
    const int c = static_cast<int>(i % static_cast<size_t>(ch));
    float x = static_cast<float>(samples[i]) / 32768.f;
    x = capture_tone_tick(f.ch[c], x);
    if (x > 1.f) x = 1.f;
    if (x < -1.f) x = -1.f;
    samples[i] = static_cast<std::int16_t>(std::lround(x * 32767.f));
  }
}

inline void apply_capture_tone_mix_pcm(void* out, size_t frames, int channels, int bits, int rate, int mode,
                                      CaptureToneFilter& f) {
  if (!out || frames == 0 || clamp_capture_tone(mode) == kCaptureTonePadrao) return;
  if (bits == 32) {
    apply_capture_tone_f32(static_cast<float*>(out), frames, channels, f, mode, rate);
    return;
  }
  if (bits == 16) apply_capture_tone_s16(static_cast<std::int16_t*>(out), frames, channels, f, mode, rate);
}

// Capture HDMI (or matching name) via WASAPI and play on default output.
class AudioMonitor {
public:
  ~AudioMonitor();
  bool start(const std::string& prefer_name_substr, std::string& err);
  void stop();
  bool running() const { return running_; }
  void set_muted(bool muted) { muted_ = muted; }
  bool muted() const { return muted_; }
  void set_volume(int percent) { volume_ = clamp_capture_volume_percent(percent); }
  int volume() const { return volume_.load(); }
  void set_tone(int mode) { tone_ = clamp_capture_tone(mode); }
  int tone() const { return tone_.load(); }

private:
  void thread_main(std::string prefer);

  std::atomic<bool> running_{false};
  std::atomic<bool> muted_{false};
  std::atomic<int> volume_{kDefaultCaptureVolumePercent};
  std::atomic<int> tone_{kCaptureTonePadrao};
  CaptureToneFilter tone_filter_{};
  std::thread th_;
};

// Benchmark: decode clip or sidecar audio and play on the default output.
class ClipAudioPlayer {
public:
  ~ClipAudioPlayer();
  bool start(const std::string& video_path, const std::string& sidecar_path, std::string& err);
  void stop();
  bool running() const { return running_; }
  bool ready() const { return ready_; }
  std::string last_error() const { return fail_; }
  void rebase_playhead();  // same clock gate as the virtual pin
  void set_volume(int percent) { volume_ = clamp_capture_volume_percent(percent); }
  int volume() const { return volume_.load(); }
  void set_tone(int mode) { tone_ = clamp_capture_tone(mode); }
  int tone() const { return tone_.load(); }

private:
  void thread_main(std::string video_path, std::string sidecar_path);

  std::atomic<bool> running_{false};
  std::atomic<bool> ready_{false};
  std::atomic<bool> playhead_on_{false};
  std::atomic<bool> seek0_{false};
  std::atomic<int> volume_{kDefaultCaptureVolumePercent};
  std::atomic<int> tone_{kCaptureTonePadrao};
  CaptureToneFilter tone_filter_{};
  std::string fail_;
  std::thread th_;
};

}  // namespace nexuscap
