#pragma once
#include <algorithm>
#include <cmath>

namespace nexuscap {

struct Rgb8 { int r, g, b; };

// BT.709 limited-range YCbCr -> RGB (studio swing).
inline Rgb8 ycbcr_bt709_limited_to_rgb(int y, int cb, int cr) {
  const double Y = (static_cast<double>(y) - 16.0) / 219.0;
  const double Cb = (static_cast<double>(cb) - 128.0) / 224.0;
  const double Cr = (static_cast<double>(cr) - 128.0) / 224.0;
  double R = Y + 1.5748 * Cr;
  double G = Y - 0.1873 * Cb - 0.4681 * Cr;
  double B = Y + 1.8556 * Cb;
  auto clamp8 = [](double v) {
    return static_cast<int>(std::clamp(std::lround(v * 255.0), 0L, 255L));
  };
  return {clamp8(R), clamp8(G), clamp8(B)};
}

}  // namespace nexuscap
