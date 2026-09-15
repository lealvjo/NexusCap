#include "nexuscap/capture.hpp"

#include <chrono>
#include <cstring>
#include <vector>

namespace nexuscap {

bool FakeCapture::start(const std::string& device_name, const CaptureStartOptions& opt, FrameSlot& slot,
                        IClock& clock, std::string& err) {
  if (running_) {
    err = "already running";
    return false;
  }
  width_ = opt.width;
  height_ = opt.height;
  fps_ = opt.fps > 0 ? opt.fps : 60.0;
  slot_ = &slot;
  clock_ = &clock;
  opened_ = device_name.empty() || device_name == "auto" ? "fake" : device_name;
  running_ = true;
  frames_ = 0;
  th_ = std::thread([this] { thread_main(); });
  return true;
}

void FakeCapture::stop() {
  running_ = false;
  if (th_.joinable()) th_.join();
}

void FakeCapture::thread_main() {
  const size_t y_size = static_cast<size_t>(width_) * static_cast<size_t>(height_);
  const size_t nv12_size = y_size + y_size / 2;
  std::vector<uint8_t> buf(nv12_size);
  const double period_ms = 1000.0 / fps_;
  uint64_t i = 0;
  while (running_) {
    const uint8_t yv = static_cast<uint8_t>(16 + (i * 3) % 200);
    std::memset(buf.data(), yv, y_size);
    std::memset(buf.data() + y_size, 128, y_size / 2);
    // moving bar
    const int bar_x = static_cast<int>(i % static_cast<uint64_t>(width_));
    for (int y = 0; y < height_; ++y) {
      for (int x = 0; x < 8; ++x) {
        const int xx = (bar_x + x) % width_;
        buf[static_cast<size_t>(y) * static_cast<size_t>(width_) + static_cast<size_t>(xx)] = 235;
      }
    }
    FramePayload p;
    p.nv12 = buf;
    p.width = width_;
    p.height = height_;
    p.cap_qpc_ms = clock_->now_ms();
    p.frame_index = i++;
    slot_->publish(std::move(p));
    frames_ = i;
    std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(period_ms));
  }
}

}  // namespace nexuscap
