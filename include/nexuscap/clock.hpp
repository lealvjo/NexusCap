#pragma once
#include <cstdint>

namespace nexuscap {

struct IClock {
  virtual ~IClock() = default;
  virtual double now_ms() const = 0;
};

struct QpcClock final : IClock {
  QpcClock();
  double now_ms() const override;
private:
  double inv_freq_ms_;
};

struct FakeClock final : IClock {
  explicit FakeClock(double start_ms = 0.0) : t_(start_ms) {}
  double now_ms() const override { return t_; }
  void set(double ms) { t_ = ms; }
  void advance(double ms) { t_ += ms; }
private:
  double t_;
};

}  // namespace nexuscap
