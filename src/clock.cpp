#include "nexuscap/clock.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace nexuscap {

QpcClock::QpcClock() {
  LARGE_INTEGER f{};
  QueryPerformanceFrequency(&f);
  inv_freq_ms_ = 1000.0 / static_cast<double>(f.QuadPart);
}

double QpcClock::now_ms() const {
  LARGE_INTEGER c{};
  QueryPerformanceCounter(&c);
  return static_cast<double>(c.QuadPart) * inv_freq_ms_;
}

}  // namespace nexuscap
