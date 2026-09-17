#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace nexuscap {

// Stall/hitch threshold scales with capture cadence (session fps).
// `configured_ms` is the reference at 60fps (default 25). Examples with 25:
//   30fps → 50ms, 60 → 25, 120 → 12.5, 144 → ~10.4
// So normal cadence is never a stall; ~1.5× late still is.
inline double effective_hitch_threshold_ms(double configured_ms, double target_fps) {
  const double ref_60 = configured_ms > 1.0 ? configured_ms : 25.0;
  if (target_fps <= 1.0) return ref_60;
  return ref_60 * (60.0 / target_fps);
}

struct HitchEvent {
  double t_ms = 0;
  double interval_ms = 0;
  double cap_ms = 0;
  double upload_ms = 0;
  double present_ms = 0;
  double outside_ms = 0;
  uint64_t frame = 0;
};

struct ClusterEvent {
  double t_ms = 0;
  int n = 0;
  double span_ms = 0;
  double gap_since_prev_cluster_ms = 0;
  uint64_t frames_since_prev = 0;
  uint64_t first_frame = 0;
};

class HitchDetector {
public:
  explicit HitchDetector(double threshold_ms = 25.0, int max_log_per_window = 8)
      : threshold_ms_(threshold_ms), max_log_per_window_(max_log_per_window) {}

  void set_threshold(double ms) { threshold_ms_ = ms; }

  // Returns hitch if interval exceeds threshold (and under per-window log cap).
  std::optional<HitchEvent> observe(uint64_t frame, double t_ms, double interval_ms,
                                    double cap_ms, double upload_ms, double present_ms) {
    ++frames_seen_;
    if (interval_ms < threshold_ms_) return std::nullopt;
    ++stall_count_;
    if (logged_in_window_ >= max_log_per_window_) return std::nullopt;
    ++logged_in_window_;
    HitchEvent e;
    e.t_ms = t_ms;
    e.interval_ms = interval_ms;
    e.cap_ms = cap_ms;
    e.upload_ms = upload_ms;
    e.present_ms = present_ms;
    e.outside_ms = (std::max)(0.0, interval_ms - cap_ms - upload_ms - present_ms);
    e.frame = frame;
    recent_.push_back(e);
    // keep last ~64 for cluster scan
    if (recent_.size() > 64) recent_.erase(recent_.begin());
    return e;
  }

  void reset_window_log_cap() { logged_in_window_ = 0; }

  uint64_t stall_count() const { return stall_count_; }
  uint64_t frames_seen() const { return frames_seen_; }
  const std::vector<HitchEvent>& recent() const { return recent_; }

private:
  double threshold_ms_;
  int max_log_per_window_;
  int logged_in_window_ = 0;
  uint64_t stall_count_ = 0;
  uint64_t frames_seen_ = 0;
  std::vector<HitchEvent> recent_;
};

class ClusterDetector {
public:
  explicit ClusterDetector(double window_ms = 500.0, int min_count = 3)
      : window_ms_(window_ms), min_count_(min_count) {}

  std::optional<ClusterEvent> on_hitch(const HitchEvent& h) {
    // drop old
    while (!buf_.empty() && (h.t_ms - buf_.front().t_ms) > window_ms_) {
      buf_.erase(buf_.begin());
    }
    buf_.push_back(h);
    if (static_cast<int>(buf_.size()) < min_count_) return std::nullopt;

    // emit once per contiguous burst: when we just reached min_count or gap from last emit
    if (emitted_for_burst_ && (h.t_ms - last_emit_t_ms_) < window_ms_) {
      return std::nullopt;
    }

    ClusterEvent c;
    c.t_ms = h.t_ms;
    c.n = static_cast<int>(buf_.size());
    c.span_ms = buf_.back().t_ms - buf_.front().t_ms;
    c.first_frame = buf_.front().frame;
    if (have_prev_) {
      c.gap_since_prev_cluster_ms = buf_.front().t_ms - prev_cluster_start_ms_;
      c.frames_since_prev = buf_.front().frame - prev_cluster_first_frame_;
    } else {
      c.gap_since_prev_cluster_ms = 0;
      c.frames_since_prev = 0;
    }
    prev_cluster_start_ms_ = buf_.front().t_ms;
    prev_cluster_first_frame_ = buf_.front().frame;
    have_prev_ = true;
    emitted_for_burst_ = true;
    last_emit_t_ms_ = h.t_ms;
    return c;
  }

  void note_quiet(double t_ms) {
    if (emitted_for_burst_ && !buf_.empty() && (t_ms - buf_.back().t_ms) > window_ms_) {
      emitted_for_burst_ = false;
      buf_.clear();
    }
  }

private:
  double window_ms_;
  int min_count_;
  std::vector<HitchEvent> buf_;
  bool have_prev_ = false;
  bool emitted_for_burst_ = false;
  double prev_cluster_start_ms_ = 0;
  uint64_t prev_cluster_first_frame_ = 0;
  double last_emit_t_ms_ = 0;
};

}  // namespace nexuscap
