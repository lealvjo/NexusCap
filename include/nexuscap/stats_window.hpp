#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace nexuscap {

struct Stats600 {
  uint64_t frames = 0;
  double fps = 0;
  double mean_interval_ms = 0;
  double p50_e2e_ms = 0;
  double p95_e2e_ms = 0;
  double p50_present_ms = 0;
  double p95_present_ms = 0;
  double max_interval_ms = 0;
  uint64_t stalls = 0;
};

class StatsWindow600 {
public:
  explicit StatsWindow600(uint64_t window = 600) : window_(window) {}

  std::optional<Stats600> push(double interval_ms, double e2e_ms, double present_ms, bool stall) {
    intervals_.push_back(interval_ms);
    e2e_.push_back(e2e_ms);
    present_.push_back(present_ms);
    if (stall) ++stalls_;
    max_interval_ = (std::max)(max_interval_, interval_ms);
    if (intervals_.size() < window_) return std::nullopt;

    Stats600 s;
    s.frames = intervals_.size();
    double sum = 0;
    for (double v : intervals_) sum += v;
    s.mean_interval_ms = sum / static_cast<double>(intervals_.size());
    s.fps = s.mean_interval_ms > 0 ? 1000.0 / s.mean_interval_ms : 0;
    s.p50_e2e_ms = percentile(e2e_, 0.50);
    s.p95_e2e_ms = percentile(e2e_, 0.95);
    s.p50_present_ms = percentile(present_, 0.50);
    s.p95_present_ms = percentile(present_, 0.95);
    s.max_interval_ms = max_interval_;
    s.stalls = stalls_;

    intervals_.clear();
    e2e_.clear();
    present_.clear();
    stalls_ = 0;
    max_interval_ = 0;
    return s;
  }

  void reset() {
    intervals_.clear();
    e2e_.clear();
    present_.clear();
    stalls_ = 0;
    max_interval_ = 0;
  }

private:
  static double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const double idx = p * static_cast<double>(v.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(idx));
    const size_t hi = static_cast<size_t>(std::ceil(idx));
    if (lo == hi) return v[lo];
    const double t = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - t) + v[hi] * t;
  }

  uint64_t window_;
  std::vector<double> intervals_;
  std::vector<double> e2e_;
  std::vector<double> present_;
  uint64_t stalls_ = 0;
  double max_interval_ = 0;
};

}  // namespace nexuscap
