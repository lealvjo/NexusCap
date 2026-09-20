#pragma once
#include <atomic>
#include <string>
#include <thread>

namespace nexuscap {

// Capture HDMI (or matching name) via WASAPI and play on default output.
class AudioMonitor {
public:
  ~AudioMonitor();
  bool start(const std::string& prefer_name_substr, std::string& err);
  void stop();
  bool running() const { return running_; }
  void set_muted(bool muted) { muted_ = muted; }
  bool muted() const { return muted_; }

private:
  void thread_main(std::string prefer);

  std::atomic<bool> running_{false};
  std::atomic<bool> muted_{false};
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

private:
  void thread_main(std::string video_path, std::string sidecar_path);

  std::atomic<bool> running_{false};
  std::atomic<bool> ready_{false};
  std::atomic<bool> playhead_on_{false};
  std::atomic<bool> seek0_{false};
  std::string fail_;
  std::thread th_;
};

}  // namespace nexuscap
