#pragma once
#include <string>

namespace nexuscap {

// Pure scoring for WASAPI capture device pick (testable without hardware).
// Higher is better. Negative = reject.
// If prefer_substr empty: any active capture is candidate (HDMI preferred, mic/line penalized).
inline int score_audio_capture_name(const std::wstring& friendly_name, const std::wstring& prefer_substr) {
  if (!prefer_substr.empty() && friendly_name.find(prefer_substr) == std::wstring::npos) return -1;
  int score = prefer_substr.empty() ? 10 : 100;
  if (friendly_name.find(L"HDMI") != std::wstring::npos) score += 50;
  if (friendly_name.find(L"Microfone") != std::wstring::npos ||
      friendly_name.find(L"Microphone") != std::wstring::npos ||
      friendly_name.find(L"Mic ") != std::wstring::npos) {
    score -= 40;
  }
  if (friendly_name.find(L"LINE IN") != std::wstring::npos ||
      friendly_name.find(L"Line In") != std::wstring::npos) {
    score -= 20;
  }
  return score;
}

inline bool is_exact_1to1(int back_w, int back_h, int frame_w, int frame_h) {
  return back_w == frame_w && back_h == frame_h;
}

inline bool is_auto_device(const std::string& name) {
  return name.empty() || name == "auto" || name == "AUTO" || name == "*";
}

}  // namespace nexuscap
