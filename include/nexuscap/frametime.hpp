#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace nexuscap {

// Short rolling window for live frametime stability on the HUD.
class FrametimeWindow {
public:
  explicit FrametimeWindow(size_t capacity = 120) : cap_(capacity) { samples_.reserve(capacity); }

  void push(double interval_ms) {
    if (samples_.size() < cap_) {
      samples_.push_back(interval_ms);
    } else {
      samples_[idx_] = interval_ms;
      idx_ = (idx_ + 1) % cap_;
      filled_ = true;
    }
  }

  size_t size() const { return filled_ ? cap_ : samples_.size(); }

  double last() const {
    if (samples_.empty()) return 0;
    if (!filled_) return samples_.back();
    const size_t i = (idx_ + cap_ - 1) % cap_;
    return samples_[i];
  }

  double avg() const {
    if (samples_.empty()) return 0;
    double s = 0;
    for (double v : samples_) s += v;
    return s / static_cast<double>(samples_.size());
  }

  double max() const {
    if (samples_.empty()) return 0;
    return *std::max_element(samples_.begin(), samples_.end());
  }

  double p95() const {
    if (samples_.empty()) return 0;
    std::vector<double> v = samples_;
    std::sort(v.begin(), v.end());
    const double idx = 0.95 * static_cast<double>(v.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(idx));
    const size_t hi = static_cast<size_t>(std::ceil(idx));
    if (lo == hi) return v[lo];
    const double t = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - t) + v[hi] * t;
  }

  // Stable = p95 within +10% of target cadence and max under stall cap.
  // If max_stall_ms <= 0, stall cap defaults to 1.5× target (avoids 30fps UNSTABLE
  // when callers forget to pass a cadence-scaled hitch threshold).
  bool stable(double target_ms = 1000.0 / 60.0, double max_stall_ms = 0.0) const {
    if (size() < 30) return false;
    const double stall_cap = max_stall_ms > 0.0 ? max_stall_ms : target_ms * 1.5;
    return p95() <= target_ms * 1.10 && max() < stall_cap;
  }

  // Oldest→newest order for graph / sparkline.
  void chronological(std::vector<double>& out) const {
    out.clear();
    if (samples_.empty()) return;
    out.reserve(samples_.size());
    if (!filled_) {
      out = samples_;
      return;
    }
    for (size_t i = 0; i < cap_; ++i) out.push_back(samples_[(idx_ + i) % cap_]);
  }

  // Compact unicode spark (fallback / text HUD).
  std::wstring sparkline(size_t bars = 48, double target_ms = 1000.0 / 60.0) const {
    std::wstring out;
    std::vector<double> chron;
    chronological(chron);
    if (chron.empty() || bars == 0) return out;
    out.resize(bars, L' ');
    static const wchar_t* glyphs = L"▁▂▃▄▅▆▇█";
    for (size_t b = 0; b < bars; ++b) {
      const size_t src = (chron.size() * (b + 1)) / bars;
      const size_t i = src == 0 ? 0 : src - 1;
      const double v = chron[i];
      int level = 0;
      if (v > target_ms * 0.95) level = 1;
      if (v > target_ms * 1.05) level = 2;
      if (v > target_ms * 1.15) level = 3;
      if (v > target_ms * 1.30) level = 4;
      if (v > 25.0) level = 5;
      if (v > 40.0) level = 6;
      if (v > 60.0) level = 7;
      out[b] = glyphs[level];
    }
    return out;
  }

private:
  size_t cap_;
  size_t idx_ = 0;
  bool filled_ = false;
  std::vector<double> samples_;
};

}  // namespace nexuscap
