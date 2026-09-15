#pragma once
#include <cstdint>
#include <mutex>
#include <vector>

namespace nexuscap {

struct FramePayload {
  std::vector<uint8_t> nv12;
  int width = 0;
  int height = 0;
  double cap_qpc_ms = 0;
  uint64_t frame_index = 0;
};

// Single-slot latest-wins queue. New frames overwrite; never grows.
class FrameSlot {
public:
  void publish(FramePayload&& frame) {
    std::lock_guard<std::mutex> lock(mu_);
    if (has_) {
      ++dropped_;
    }
    slot_ = std::move(frame);
    has_ = true;
    ++published_;
  }

  bool consume(FramePayload& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!has_) return false;
    out = std::move(slot_);
    has_ = false;
    ++consumed_;
    return true;
  }

  bool peek_age_ms(double now_ms, double& age_ms) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!has_) return false;
    age_ms = now_ms - slot_.cap_qpc_ms;
    return true;
  }

  uint64_t dropped() const { return dropped_; }
  uint64_t published() const { return published_; }
  uint64_t consumed() const { return consumed_; }
  bool has_frame() const {
    std::lock_guard<std::mutex> lock(mu_);
    return has_;
  }

private:
  mutable std::mutex mu_;
  FramePayload slot_;
  bool has_ = false;
  uint64_t dropped_ = 0;
  uint64_t published_ = 0;
  uint64_t consumed_ = 0;
};

}  // namespace nexuscap
