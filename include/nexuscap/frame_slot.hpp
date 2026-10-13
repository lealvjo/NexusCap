#pragma once
#include <cstdint>
#include <mutex>
#include <vector>

namespace nexuscap {

struct FramePayload {
  std::vector<uint8_t> nv12;
  // Janela: handle de textura BGRA compartilhada. 0 = quadro NV12 (HDMI).
  // O handle pertence à captura. Não fechar com CloseHandle.
  std::uintptr_t bgra_shared = 0;
  // Change 4: generation counter incremented when capture recreates shared BGRA textures
  std::uint32_t bgra_gen = 0;
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
      // Keep dropped buffer capacity for the capture thread (ping-pong).
      recycle_.nv12.swap(slot_.nv12);
      ++dropped_;
    }
    slot_ = std::move(frame);
    has_ = true;
    ++published_;
  }

  // Steal recycled NV12 capacity after a drop (or empty if none). Capture uses this to avoid realloc.
  void take_recycle_nv12(std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lock(mu_);
    out.swap(recycle_.nv12);
    recycle_.nv12.clear();
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

  // Drop pending sample (format change / reopen) so consumers never see stale WxH.
  void clear() {
    std::lock_guard<std::mutex> lock(mu_);
    if (has_) {
      recycle_.nv12.swap(slot_.nv12);
      slot_ = FramePayload{};
      has_ = false;
      ++dropped_;
    }
  }

private:
  mutable std::mutex mu_;
  FramePayload slot_;
  FramePayload recycle_;  // last overwritten buffer (capacity reuse)
  bool has_ = false;
  uint64_t dropped_ = 0;
  uint64_t published_ = 0;
  uint64_t consumed_ = 0;
};

}  // namespace nexuscap
