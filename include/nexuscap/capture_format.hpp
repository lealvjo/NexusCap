#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace nexuscap {

// OBS-style video format (DirectShow subtypes we negotiate).
// Index 0 = NV12 stays the validated default (settings.ini compatible).
enum class VideoFourcc : int {
  Nv12 = 0,   // validated default (best HDMI path)
  I420 = 1,   // OBS I420 / IYUV
  Yuy2 = 2,
  Mjpeg = 3,  // OBS MJPEG
  Any = 4,    // OBS "Qualquer" → prefer NV12 then others
  Count = 5,
};

// OBS "Espaço de cores" — Padrao first (validated default).
enum class ColorSpace : int {
  Default = 0,     // OBS "Padrão" → BT.709 for HD (validated)
  Bt601 = 1,       // Rec. 601
  Bt709 = 2,       // Rec. 709
  Rec2100Pq = 3,   // Rec. 2100 (PQ) — UI parity; decode still BT.709 until HDR path
  Rec2100Hlg = 4,  // Rec. 2100 (HLG)
  Count = 5,
};

enum class ColorRange : int {
  Limited = 0,  // OBS "Limitado" — validated default
  Full = 1,     // OBS "Completo"
  Count = 2,
};

enum class CaptureBuffering : int {
  Disabled = 0,  // OBS "Desativado" — low latency (validated default)
  Enabled = 1,   // OBS "Ativado"
  Count = 2,
};

inline constexpr VideoFourcc kDefaultVideoFourcc = VideoFourcc::Nv12;
inline constexpr ColorSpace kDefaultColorSpace = ColorSpace::Default;
inline constexpr ColorRange kDefaultColorRange = ColorRange::Limited;
inline constexpr CaptureBuffering kDefaultCaptureBuffering = CaptureBuffering::Disabled;

inline int clamp_video_fourcc(int v) {
  if (v < 0 || v >= static_cast<int>(VideoFourcc::Count)) return static_cast<int>(kDefaultVideoFourcc);
  return v;
}
inline int clamp_color_space(int v) {
  if (v < 0 || v >= static_cast<int>(ColorSpace::Count)) return static_cast<int>(kDefaultColorSpace);
  return v;
}
inline int clamp_color_range(int v) {
  if (v < 0 || v >= static_cast<int>(ColorRange::Count)) return static_cast<int>(kDefaultColorRange);
  return v;
}
inline int clamp_capture_buffering(int v) {
  if (v < 0 || v >= static_cast<int>(CaptureBuffering::Count))
    return static_cast<int>(kDefaultCaptureBuffering);
  return v;
}

inline const char* video_fourcc_name(int v) {
  switch (clamp_video_fourcc(v)) {
    case static_cast<int>(VideoFourcc::I420):
      return "I420";
    case static_cast<int>(VideoFourcc::Yuy2):
      return "YUY2";
    case static_cast<int>(VideoFourcc::Mjpeg):
      return "MJPEG";
    case static_cast<int>(VideoFourcc::Any):
      return "Qualquer";
    default:
      return "NV12";
  }
}

// Janela publica NV12 no FrameSlot. HDMI mostra o fourcc negociado (0 = HDMI).
inline const char* hud_capture_format_name(int fourcc, int capture_source) {
  if (capture_source == 1) return video_fourcc_name(static_cast<int>(VideoFourcc::Nv12));
  return video_fourcc_name(fourcc);
}

// OBS dropdown order for Espaco de cores (values are ColorSpace enum ints).
inline constexpr int kObsColorSpaceUiOrder[] = {
    static_cast<int>(ColorSpace::Default),     // Padrao
    static_cast<int>(ColorSpace::Bt709),       // Rec. 709
    static_cast<int>(ColorSpace::Bt601),       // Rec. 601
    static_cast<int>(ColorSpace::Rec2100Pq),   // Rec. 2100 (PQ)
    static_cast<int>(ColorSpace::Rec2100Hlg),  // Rec. 2100 (HLG)
};
inline constexpr int kObsColorSpaceUiCount = 5;

inline const char* color_space_name(int v) {
  switch (clamp_color_space(v)) {
    case static_cast<int>(ColorSpace::Bt601):
      return "Rec. 601";
    case static_cast<int>(ColorSpace::Bt709):
      return "Rec. 709";
    case static_cast<int>(ColorSpace::Rec2100Pq):
      return "Rec. 2100 (PQ)";
    case static_cast<int>(ColorSpace::Rec2100Hlg):
      return "Rec. 2100 (HLG)";
    default:
      return "Padrao";
  }
}

inline const char* color_range_name(int v) {
  return clamp_color_range(v) == static_cast<int>(ColorRange::Full) ? "Completo" : "Limitado";
}

inline const char* capture_buffering_name(int v) {
  return clamp_capture_buffering(v) == static_cast<int>(CaptureBuffering::Enabled) ? "Ativado"
                                                                                  : "Desativado";
}

inline int capture_buffer_count(int buffering) {
  return clamp_capture_buffering(buffering) == static_cast<int>(CaptureBuffering::Enabled) ? 8 : 2;
}

// Effective matrix: Padrao / Rec.709 / Rec.2100* → BT.709 coeffs for SDR path today.
inline ColorSpace effective_color_space(int space) {
  const int s = clamp_color_space(space);
  if (s == static_cast<int>(ColorSpace::Default) || s == static_cast<int>(ColorSpace::Bt709) ||
      s == static_cast<int>(ColorSpace::Rec2100Pq) || s == static_cast<int>(ColorSpace::Rec2100Hlg)) {
    return ColorSpace::Bt709;
  }
  return ColorSpace::Bt601;
}

struct YuvRgbCoeffs {
  float y_scale = 255.f / 219.f;
  float y_offset = 16.f / 255.f;
  float c_scale = 255.f / 224.f;
  float c_offset = 128.f / 255.f;
  float r_cr = 1.5748f;
  float g_cb = -0.1873f;
  float g_cr = -0.4681f;
  float b_cb = 1.8556f;
};

inline YuvRgbCoeffs yuv_rgb_coeffs(int space, int range) {
  YuvRgbCoeffs c;
  const bool full = clamp_color_range(range) == static_cast<int>(ColorRange::Full);
  if (full) {
    c.y_scale = 1.f;
    c.y_offset = 0.f;
    c.c_scale = 1.f;
    c.c_offset = 128.f / 255.f;
  } else {
    c.y_scale = 255.f / 219.f;
    c.y_offset = 16.f / 255.f;
    c.c_scale = 255.f / 224.f;
    c.c_offset = 128.f / 255.f;
  }
  if (effective_color_space(space) == ColorSpace::Bt601) {
    c.r_cr = 1.402f;
    c.g_cb = -0.344136f;
    c.g_cr = -0.714136f;
    c.b_cb = 1.772f;
  } else {
    c.r_cr = 1.5748f;
    c.g_cb = -0.1873f;
    c.g_cr = -0.4681f;
    c.b_cb = 1.8556f;
  }
  return c;
}

// CPU NV12 sample → RGB8 using the same matrix as the GPU decode shader.
inline void yuv_to_rgb8(int y, int cb, int cr, const YuvRgbCoeffs& c, uint8_t* rgb) {
  const float Y = (static_cast<float>(y) / 255.f - c.y_offset) * c.y_scale;
  const float Cb = (static_cast<float>(cb) / 255.f - c.c_offset) * c.c_scale;
  const float Cr = (static_cast<float>(cr) / 255.f - c.c_offset) * c.c_scale;
  const float R = Y + c.r_cr * Cr;
  const float G = Y + c.g_cb * Cb + c.g_cr * Cr;
  const float B = Y + c.b_cb * Cb;
  auto clamp8 = [](float v) -> uint8_t {
    if (v < 0.f) return 0;
    if (v > 1.f) return 255;
    return static_cast<uint8_t>(v * 255.f + 0.5f);
  };
  rgb[0] = clamp8(R);
  rgb[1] = clamp8(G);
  rgb[2] = clamp8(B);
}

// True when two menu picks must produce different RGB for the same YUV sample.
inline bool color_decode_differs(int space_a, int range_a, int space_b, int range_b) {
  const auto ca = yuv_rgb_coeffs(space_a, range_a);
  const auto cb = yuv_rgb_coeffs(space_b, range_b);
  return std::abs(ca.y_scale - cb.y_scale) > 1e-4f || std::abs(ca.y_offset - cb.y_offset) > 1e-4f ||
         std::abs(ca.c_scale - cb.c_scale) > 1e-4f || std::abs(ca.r_cr - cb.r_cr) > 1e-4f ||
         std::abs(ca.g_cb - cb.g_cb) > 1e-4f || std::abs(ca.g_cr - cb.g_cr) > 1e-4f ||
         std::abs(ca.b_cb - cb.b_cb) > 1e-4f;
}

// Lobby preview band decode is still CPU NV12→RGBA; when RenoDX is ON the band also
// gets a CPU look-alike grade for Default/Natural/Cinematic (instant radio feedback).
inline constexpr bool kLobbyPreviewCpuBandUsesRenodxNeural = false;
inline constexpr bool kLobbyPreviewAppliesVisualPresetGrade = true;
// ReShade hooks lobby Present: when NR is ON, filters also apply to the whole window.
inline constexpr bool kLobbyPresentShowsRenodxWhenEnabled = true;
// Product contract: toggling RenoDX + presets must change what you see in the lobby preview.
inline constexpr bool kLobbyPreviewUsesRenodxNeural = kLobbyPresentShowsRenodxWhenEnabled;
// Session Present can also run RenoDX when enabled.
inline constexpr bool kSessionPresentCanUseRenodxNeural = true;

// Preference for "Qualquer" / pick_best: NV12 first (validated HDMI).
inline int pick_best_video_fourcc(const std::vector<int>& available) {
  if (available.empty()) return static_cast<int>(kDefaultVideoFourcc);
  const int prefer[] = {static_cast<int>(VideoFourcc::Nv12), static_cast<int>(VideoFourcc::I420),
                        static_cast<int>(VideoFourcc::Yuy2), static_cast<int>(VideoFourcc::Mjpeg)};
  for (int p : prefer) {
    for (int f : available) {
      if (clamp_video_fourcc(f) == p && f != static_cast<int>(VideoFourcc::Any)) return f;
    }
  }
  for (int f : available) {
    if (clamp_video_fourcc(f) != static_cast<int>(VideoFourcc::Any)) return clamp_video_fourcc(f);
  }
  return static_cast<int>(kDefaultVideoFourcc);
}

inline bool video_fourcc_in_list(int fourcc, const std::vector<int>& available) {
  const int f = clamp_video_fourcc(fourcc);
  if (f == static_cast<int>(VideoFourcc::Any)) return true;  // always selectable
  for (int a : available) {
    if (clamp_video_fourcc(a) == f) return true;
  }
  return false;
}

inline int resolve_video_fourcc_for_device(int saved, const std::vector<int>& available) {
  const int f = clamp_video_fourcc(saved);
  if (f == static_cast<int>(VideoFourcc::Any)) return f;
  if (video_fourcc_in_list(f, available)) return f;
  return pick_best_video_fourcc(available);
}

// Formato da combinacao escolhida. So entra o que essa res/Hz publicou.
// Sem NV12 na lista, NV12 nao aparece. Qualquer fica na frente se houver algum.
inline std::vector<int> capture_format_menu_for_mode(const std::vector<int>& accepted) {
  std::vector<int> out;
  for (int fc : accepted) {
    if (fc < 0) continue;
    const int f = clamp_video_fourcc(fc);
    if (f == static_cast<int>(VideoFourcc::Any)) continue;
    if (std::find(out.begin(), out.end(), f) == out.end()) out.push_back(f);
  }
  if (!out.empty()) out.insert(out.begin(), static_cast<int>(VideoFourcc::Any));
  return out;
}

// Concrete subtype to open when user picked Qualquer / a specific fourcc.
inline int concrete_video_fourcc_for_start(int saved, const std::vector<int>& available) {
  const int f = clamp_video_fourcc(saved);
  if (f == static_cast<int>(VideoFourcc::Any)) return pick_best_video_fourcc(available);
  if (video_fourcc_in_list(f, available)) return f;
  return pick_best_video_fourcc(available);
}

inline bool is_validated_default_capture_format(int fourcc, int space, int range, int buffering) {
  return clamp_video_fourcc(fourcc) == static_cast<int>(kDefaultVideoFourcc) &&
         clamp_color_space(space) == static_cast<int>(kDefaultColorSpace) &&
         clamp_color_range(range) == static_cast<int>(kDefaultColorRange) &&
         clamp_capture_buffering(buffering) == static_cast<int>(kDefaultCaptureBuffering);
}

inline void apply_validated_default_capture_format(int& fourcc, int& space, int& range, int& buffering) {
  fourcc = static_cast<int>(kDefaultVideoFourcc);
  space = static_cast<int>(kDefaultColorSpace);
  range = static_cast<int>(kDefaultColorRange);
  buffering = static_cast<int>(kDefaultCaptureBuffering);
}

}  // namespace nexuscap
