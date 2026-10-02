#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace nexuscap {

// Período da fonte, em ms, e o fps correspondente (1000 / período).
// A média de todos os vãos lê um tick pulado (2T) como se o jogo tivesse caído.
// O período é a média aparada do grupo em volta da mediana: o tick que o jogo de fato usa.
struct OverlaySourceRate {
  double period_ms = 0;
  double fps = 0;
  double p95_ms = 0;
  double max_ms = 0;
};

inline double overlay_sorted_percentile(const std::vector<double>& v, double p) {
  if (v.empty()) return 0;
  if (v.size() == 1) return v.front();
  const double idx = p * static_cast<double>(v.size() - 1);
  const size_t lo = static_cast<size_t>(std::floor(idx));
  const size_t hi = static_cast<size_t>(std::ceil(idx));
  if (lo == hi) return v[lo];
  const double t = idx - static_cast<double>(lo);
  return v[lo] * (1.0 - t) + v[hi] * t;
}

inline void overlay_collect_period_band(const std::vector<double>& sorted, double center,
                                        std::vector<double>& out) {
  out.clear();
  if (!(center > 0.0)) return;
  const double lo = center * 0.82;
  const double hi = center * 1.18;
  for (double x : sorted) {
    if (x >= lo && x <= hi) out.push_back(x);
  }
}

// 10% de cada ponta fora, para um vão na borda da faixa não puxar o centro.
inline double overlay_trimmed_mean(const std::vector<double>& sorted_band) {
  if (sorted_band.empty()) return 0;
  size_t lo = 0;
  size_t hi = sorted_band.size();
  if (sorted_band.size() >= 10) {
    const size_t cut = sorted_band.size() / 10;
    lo = cut;
    hi = sorted_band.size() - cut;
  }
  double sum = 0;
  for (size_t i = lo; i < hi; ++i) sum += sorted_band[i];
  const size_t n = hi - lo;
  return n ? sum / static_cast<double>(n) : sorted_band[sorted_band.size() / 2];
}

inline OverlaySourceRate overlay_source_rate(const double* samples, size_t n) {
  OverlaySourceRate r;
  if (!samples || n == 0) return r;
  std::vector<double> v;
  v.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    const double x = samples[i];
    if (x > 1.0 && x < 250.0) v.push_back(x);
  }
  if (v.empty()) return r;
  std::sort(v.begin(), v.end());
  const double med = overlay_sorted_percentile(v, 0.5);
  std::vector<double> band;
  overlay_collect_period_band(v, med, band);
  double period = med;
  if (band.size() >= 3) {
    std::sort(band.begin(), band.end());
    period = overlay_trimmed_mean(band);
    std::vector<double> refined;
    overlay_collect_period_band(v, period, refined);
    if (refined.size() >= 3) {
      std::sort(refined.begin(), refined.end());
      period = overlay_trimmed_mean(refined);
      band.swap(refined);
    }
  }
  if (!(period > 0.0)) return r;
  r.period_ms = period;
  r.fps = 1000.0 / period;
  const std::vector<double>& spread = band.size() >= 3 ? band : v;
  r.p95_ms = overlay_sorted_percentile(spread, 0.95);
  r.max_ms = spread.back();
  return r;
}

// Vão de k ticks (quadro pulado na cópia) volta ao período. Um vão que não cai em k*T fica como está.
inline double overlay_source_gap_ms(double gap_ms, double period_ms) {
  if (!(period_ms > 1.0) || !(gap_ms > 1.0)) return gap_ms;
  const double k = std::round(gap_ms / period_ms);
  if (k < 1.0) return gap_ms;
  const double harm = k * period_ms;
  if (std::fabs(gap_ms - harm) <= period_ms * 0.20) return period_ms;
  return gap_ms;
}

// Short rolling window for live frametime stability on the HUD.
class FrametimeWindow {
public:
  explicit FrametimeWindow(size_t capacity = 120) : cap_(capacity) { samples_.reserve(capacity); }

  void reset() {
    samples_.clear();
    idx_ = 0;
    filled_ = false;
  }

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

  // Same rule as stable(), with p95 already computed (avoids a second copy+sort per frame).
  bool stable_with_p95(double p95_ms, double target_ms = 1000.0 / 60.0, double max_stall_ms = 0.0) const {
    if (size() < 30) return false;
    const double stall_cap = max_stall_ms > 0.0 ? max_stall_ms : target_ms * 1.5;
    return p95_ms <= target_ms * 1.10 && max() < stall_cap;
  }

  // Estabilidade do período da fonte. O max cru da janela inclui tick pulado e não entra.
  bool stable_for_source(double p95_ms, double source_max_ms, double target_ms, double max_stall_ms) const {
    if (size() < 30) return false;
    const double stall_cap = max_stall_ms > 0.0 ? max_stall_ms : target_ms * 1.5;
    return p95_ms <= target_ms * 1.10 && source_max_ms < stall_cap;
  }

  // Stable = p95 within +10% of target cadence and max under stall cap.
  // If max_stall_ms <= 0, stall cap defaults to 1.5× target (avoids 30fps UNSTABLE
  // when callers forget to pass a cadence-scaled hitch threshold).
  bool stable(double target_ms = 1000.0 / 60.0, double max_stall_ms = 0.0) const {
    return stable_with_p95(p95(), target_ms, max_stall_ms);
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

  // Últimos 2 s em 60 fps. A mediana acha o tick; a média aparada desse grupo é o período.
  OverlaySourceRate source_rate() const {
    std::vector<double> chron;
    chronological(chron);
    if (chron.size() > 120) {
      return overlay_source_rate(chron.data() + (chron.size() - 120), 120);
    }
    return overlay_source_rate(chron.data(), chron.size());
  }

  // Compact unicode spark (fallback / text HUD).
  // source_period_ms > 0 desenha o tick da fonte: um vão de 2T não vira pico.
  std::wstring sparkline(size_t bars = 48, double target_ms = 1000.0 / 60.0,
                         double source_period_ms = 0) const {
    std::wstring out;
    std::vector<double> chron;
    chronological(chron);
    if (chron.empty() || bars == 0) return out;
    out.resize(bars, L' ');
    static const wchar_t* glyphs = L"▁▂▃▄▅▆▇█";
    for (size_t b = 0; b < bars; ++b) {
      const size_t src = (chron.size() * (b + 1)) / bars;
      const size_t i = src == 0 ? 0 : src - 1;
      const double raw = chron[i];
      const double v = source_period_ms > 0.0 ? overlay_source_gap_ms(raw, source_period_ms) : raw;
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
