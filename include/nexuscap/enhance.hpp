#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>

#include <memory>
#include <string>

#include "nexuscap/dlss_options.hpp"

namespace nexuscap {

struct EnhanceOptions {
  bool enabled = false;
  DlssMode mode = DlssMode::Dlaa;
  float sharpness = 0.f;
  DlssMvecSource mvec = DlssMvecSource::OpticalFlow;
};

struct EnhanceFrameIn {
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  ID3D11Texture2D* color_rgb = nullptr;  // B8G8R8A8 or R8G8B8A8 input
  int width = 0;
  int height = 0;
  uint64_t frame_index = 0;
};

struct EnhanceFrameOut {
  ID3D11Texture2D* color_out = nullptr;  // may alias input on passthrough
  double dlss_ms = 0;
  bool evaluated = false;
  std::string status;  // "passthrough" | "dlss" | "fallback:..."
};

class IFrameEnhance {
public:
  virtual ~IFrameEnhance() = default;
  virtual bool init(ID3D11Device* device, std::string& err) = 0;
  virtual void shutdown() = 0;
  virtual void set_options(const EnhanceOptions& opt) = 0;
  virtual EnhanceOptions options() const = 0;
  virtual bool feature_supported() const = 0;
  virtual bool enabled() const = 0;
  virtual const char* backend_name() const = 0;
  virtual bool evaluate(const EnhanceFrameIn& in, EnhanceFrameOut& out, std::string& err) = 0;
};

std::unique_ptr<IFrameEnhance> create_passthrough_enhance();
std::unique_ptr<IFrameEnhance> create_dlss_enhance();  // Streamline if built-in, else passthrough-compatible stub

}  // namespace nexuscap
