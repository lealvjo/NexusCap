#pragma once
#include <atomic>
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

private:
  void thread_main(std::string prefer);

  std::atomic<bool> running_{false};
  std::atomic<bool> muted_{false};
  std::atomic<int> volume_{kDefaultCaptureVolumePercent};
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

private:
  void thread_main(std::string video_path, std::string sidecar_path);

  std::atomic<bool> running_{false};
  std::atomic<bool> ready_{false};
  std::atomic<bool> playhead_on_{false};
  std::atomic<bool> seek0_{false};
  std::atomic<int> volume_{kDefaultCaptureVolumePercent};
  std::string fail_;
  std::thread th_;
};

}  // namespace nexuscap
