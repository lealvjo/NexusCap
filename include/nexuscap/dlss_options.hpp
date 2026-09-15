#pragma once
#include <algorithm>
#include <cctype>
#include <string>

namespace nexuscap {

enum class DlssMode {
  Off = 0,
  Dlaa,
  Quality,
  Balanced,
  Performance,
  UltraPerformance,
};

enum class DlssMvecSource {
  OpticalFlow = 0,
  Zero = 1,
};

inline std::string to_lower_copy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

inline bool parse_dlss_mode(const std::string& raw, DlssMode& out) {
  const std::string s = to_lower_copy(raw);
  if (s == "off" || s == "none") {
    out = DlssMode::Off;
    return true;
  }
  if (s == "dlaa" || s == "native" || s == "same") {
    out = DlssMode::Dlaa;
    return true;
  }
  if (s == "quality" || s == "q") {
    out = DlssMode::Quality;
    return true;
  }
  if (s == "balanced" || s == "b") {
    out = DlssMode::Balanced;
    return true;
  }
  if (s == "perf" || s == "performance" || s == "p") {
    out = DlssMode::Performance;
    return true;
  }
  if (s == "ultra" || s == "ultraperformance" || s == "ultra-performance" || s == "up") {
    out = DlssMode::UltraPerformance;
    return true;
  }
  return false;
}

inline const char* dlss_mode_name(DlssMode m) {
  switch (m) {
    case DlssMode::Off:
      return "off";
    case DlssMode::Dlaa:
      return "dlaa";
    case DlssMode::Quality:
      return "quality";
    case DlssMode::Balanced:
      return "balanced";
    case DlssMode::Performance:
      return "perf";
    case DlssMode::UltraPerformance:
      return "ultra";
  }
  return "off";
}

inline bool parse_dlss_mvec(const std::string& raw, DlssMvecSource& out) {
  const std::string s = to_lower_copy(raw);
  if (s == "of" || s == "optical" || s == "opticalflow" || s == "flow") {
    out = DlssMvecSource::OpticalFlow;
    return true;
  }
  if (s == "zero" || s == "0" || s == "none") {
    out = DlssMvecSource::Zero;
    return true;
  }
  return false;
}

inline const char* dlss_mvec_name(DlssMvecSource s) {
  return s == DlssMvecSource::Zero ? "zero" : "of";
}

inline float clamp01(float v) { return (std::max)(0.f, (std::min)(1.f, v)); }

// Same-res DLAA: input size must equal output size.
inline bool is_same_res_dlaa(int in_w, int in_h, int out_w, int out_h) {
  return in_w > 0 && in_h > 0 && in_w == out_w && in_h == out_h;
}

// Suggested render size for mode (same as output for DLAA; scaled for Quality+).
inline void dlss_suggested_render_size(DlssMode mode, int out_w, int out_h, int& render_w, int& render_h) {
  float scale = 1.f;
  switch (mode) {
    case DlssMode::Off:
    case DlssMode::Dlaa:
      scale = 1.f;
      break;
    case DlssMode::Quality:
      scale = 0.67f;
      break;
    case DlssMode::Balanced:
      scale = 0.58f;
      break;
    case DlssMode::Performance:
      scale = 0.50f;
      break;
    case DlssMode::UltraPerformance:
      scale = 0.33f;
      break;
  }
  render_w = (std::max)(1, static_cast<int>(out_w * scale + 0.5f));
  render_h = (std::max)(1, static_cast<int>(out_h * scale + 0.5f));
  if (mode == DlssMode::Dlaa || mode == DlssMode::Off) {
    render_w = out_w;
    render_h = out_h;
  }
}

// Routing: when disabled or Off, enhance must passthrough.
inline bool dlss_should_evaluate(bool enabled_flag, DlssMode mode, bool feature_supported) {
  return enabled_flag && mode != DlssMode::Off && feature_supported;
}

enum class EnhanceBackend {
  Off = 0,
  Streamline = 1,
  Renodx = 2,
};

inline bool parse_enhance_backend(const std::string& raw, EnhanceBackend& out) {
  const std::string s = to_lower_copy(raw);
  if (s == "off" || s == "none" || s == "passthrough") {
    out = EnhanceBackend::Off;
    return true;
  }
  if (s == "streamline" || s == "sl" || s == "native") {
    out = EnhanceBackend::Streamline;
    return true;
  }
  if (s == "renodx" || s == "reshade" || s == "x2") {
    out = EnhanceBackend::Renodx;
    return true;
  }
  return false;
}

inline const char* enhance_backend_name(EnhanceBackend b) {
  switch (b) {
    case EnhanceBackend::Streamline:
      return "streamline";
    case EnhanceBackend::Renodx:
      return "renodx";
    case EnhanceBackend::Off:
    default:
      return "off";
  }
}

// ReShade proxy present next to the exe (dxgi.dll + renodx addon).
inline bool renodx_files_present(const std::string& exe_dir) {
  const std::string sep = exe_dir.empty() || exe_dir.back() == '\\' || exe_dir.back() == '/' ? "" : "\\";
  const std::string base = exe_dir + sep;
  // Check via Win32 in callers; this helper is for path join in tests with fake paths.
  return !base.empty();
}

}  // namespace nexuscap
