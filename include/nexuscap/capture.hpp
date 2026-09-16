#pragma once
#include <atomic>
#include <functional>
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
};

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

  void push_buffer(const uint8_t* data, size_t len);

private:
  struct Impl;
  Impl* impl_ = nullptr;
  FrameSlot* slot_ = nullptr;
  IClock* clock_ = nullptr;
  std::string opened_;
  int opened_fourcc_ = static_cast<int>(kDefaultVideoFourcc);
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> frames_{0};
  // Reused NV12 scratch — avoids per-frame heap churn when resolution is stable.
  std::vector<uint8_t> nv12_scratch_;
};

std::vector<std::string> list_video_capture_devices();

struct CaptureMode {
  int width = 0;
  int height = 0;
  double fps = 0;
  std::string label;
};

std::vector<CaptureMode> list_video_capture_modes(const std::string& device_name);

// Fourccs (VideoFourcc ints) the device offers for a given mode — dynamic like OBS.
std::vector<int> list_video_fourccs_for_mode(const std::string& device_name, int width, int height,
                                             double fps);

}  // namespace nexuscap
