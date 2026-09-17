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

}  // namespace nexuscap
