#include "nexuscap/enhance.hpp"
#include "nexuscap/mvec_depth.hpp"
#include "nexuscap/clock.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dxgi.h>

#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(NEXUSCAP_WITH_STREAMLINE)
#include "sl.h"
#include "sl_dlss.h"
#include "sl_consts.h"
#pragma comment(lib, "sl.interposer.lib")
#endif

using Microsoft::WRL::ComPtr;

namespace nexuscap {
namespace {

#if defined(NEXUSCAP_WITH_STREAMLINE)

sl::DLSSMode to_sl_mode(DlssMode m) {
  switch (m) {
    case DlssMode::Dlaa:
      return sl::DLSSMode::eDLAA;
    case DlssMode::Quality:
      return sl::DLSSMode::eMaxQuality;
    case DlssMode::Balanced:
      return sl::DLSSMode::eBalanced;
    case DlssMode::Performance:
      return sl::DLSSMode::eMaxPerformance;
    case DlssMode::UltraPerformance:
      return sl::DLSSMode::eUltraPerformance;
    case DlssMode::Off:
    default:
      return sl::DLSSMode::eOff;
  }
}

class StreamlineDlssEnhance final : public IFrameEnhance {
public:
  ~StreamlineDlssEnhance() override { shutdown(); }

  bool init(ID3D11Device* device, std::string& err) override {
    shutdown();
    if (!device) {
      err = "null device";
      return false;
    }
    device_ = device;

    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    const auto slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash + 1);

    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    sl::Feature features[] = {sl::kFeatureDLSS};
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 1;
    pref.applicationId = 0x4e455843;  // 'NEXC'
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "NexusCap";
    pref.projectId = "NexusCap";
    pref.renderAPI = sl::RenderAPI::eD3D11;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eAllowOTA |
                 sl::PreferenceFlags::eLoadDownloadedPlugins | sl::PreferenceFlags::eUseFrameBasedResourceTagging |
                 sl::PreferenceFlags::eUseManualHooking;
    const wchar_t* plugin_paths[] = {dir.c_str()};
    pref.pathsToPlugins = plugin_paths;
    pref.numPathsToPlugins = 1;

    if (SL_FAILED(r_init, slInit(pref, sl::kSDKVersion))) {
      err = "slInit failed code=" + std::to_string(static_cast<int>(r_init));
      return false;
    }
    inited_sl_ = true;

    if (SL_FAILED(r_dev, slSetD3DDevice(device))) {
      err = "slSetD3DDevice failed code=" + std::to_string(static_cast<int>(r_dev));
      shutdown();
      return false;
    }

    sl::AdapterInfo adapter{};
    LUID luid{};
    ComPtr<IDXGIDevice> dxgi_dev;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi_dev))) && dxgi_dev) {
      ComPtr<IDXGIAdapter> dxgi_adapter;
      if (SUCCEEDED(dxgi_dev->GetAdapter(&dxgi_adapter)) && dxgi_adapter) {
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(dxgi_adapter->GetDesc(&desc))) {
          luid = desc.AdapterLuid;
          adapter.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
          adapter.deviceLUIDSizeInBytes = sizeof(luid);
        }
      }
    }
    std::string support_err;
    if (SL_FAILED(r_sup, slIsFeatureSupported(sl::kFeatureDLSS, adapter))) {
      support_err = "slIsFeatureSupported=" + std::to_string(static_cast<int>(r_sup));
      // Soft-fail: still allow evaluate attempts (X2-style capture may work anyway).
      supported_ = true;
    } else {
      supported_ = true;
    }

    if (!mvec_.init(device, err)) {
      supported_ = false;
    }

    ensure_output(device, 2560, 1440, err);
    if (!support_err.empty()) err = support_err;
    else err.clear();
    return true;
  }

  void shutdown() override {
    mvec_.shutdown();
    out_srv_.Reset();
    out_rtv_.Reset();
    out_color_.Reset();
    device_.Reset();
    if (inited_sl_) {
      slShutdown();
      inited_sl_ = false;
    }
    supported_ = false;
  }

  void set_options(const EnhanceOptions& opt) override {
    opt_ = opt;
    options_dirty_ = true;
  }
  EnhanceOptions options() const override { return opt_; }
  bool feature_supported() const override { return supported_; }
  bool enabled() const override { return dlss_should_evaluate(opt_.enabled, opt_.mode, supported_); }
  const char* backend_name() const override { return supported_ ? "streamline-dlss" : "streamline-unavailable"; }

  bool evaluate(const EnhanceFrameIn& in, EnhanceFrameOut& out, std::string& err) override {
    out.color_out = in.color_rgb;
    out.dlss_ms = 0;
    out.evaluated = false;
    out.status = "passthrough";

    if (!enabled() || !in.device || !in.ctx || !in.color_rgb) {
      return true;
    }

    QpcClock clk;
    const double t0 = clk.now_ms();

    if (!ensure_output(in.device, in.width, in.height, err)) {
      out.status = "fallback:output";
      return true;
    }
    if (!mvec_.ensure_size(in.device, in.width, in.height, err)) {
      out.status = "fallback:mvec-size";
      return true;
    }
    if (!mvec_.generate(in.ctx, in.color_rgb, opt_.mvec == DlssMvecSource::OpticalFlow, err)) {
      out.status = "fallback:mvec";
      return true;
    }

    if (options_dirty_ || last_w_ != in.width || last_h_ != in.height) {
      sl::DLSSOptions opts{};
      opts.mode = to_sl_mode(opt_.mode);
      opts.outputWidth = static_cast<uint32_t>(in.width);
      opts.outputHeight = static_cast<uint32_t>(in.height);
      opts.colorBuffersHDR = sl::Boolean::eFalse;
      opts.useAutoExposure = sl::Boolean::eTrue;
      opts.dlaaPreset = sl::DLSSPreset::ePresetK;
      opts.qualityPreset = sl::DLSSPreset::ePresetK;
      opts.balancedPreset = sl::DLSSPreset::ePresetK;
      opts.performancePreset = sl::DLSSPreset::ePresetM;
      opts.ultraPerformancePreset = sl::DLSSPreset::ePresetL;
      if (SL_FAILED(r_opt, slDLSSSetOptions(viewport_, opts))) {
        out.status = "fallback:set-options";
        err = "slDLSSSetOptions=" + std::to_string(static_cast<int>(r_opt));
        return true;
      }
      options_dirty_ = false;
      last_w_ = in.width;
      last_h_ = in.height;
    }

    sl::FrameToken* frame = nullptr;
    if (SL_FAILED(r_tok, slGetNewFrameToken(frame, nullptr)) || !frame) {
      out.status = "fallback:frame-token";
      return true;
    }

    sl::Constants constants{};
    constants.jitterOffset = {0.f, 0.f};
    constants.mvecScale = {1.f / static_cast<float>(in.width), 1.f / static_cast<float>(in.height)};
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.depthInverted = sl::Boolean::eFalse;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = (in.frame_index <= 1) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    if (SL_FAILED(r_c, slSetConstants(constants, *frame, viewport_))) {
      out.status = "fallback:constants";
      return true;
    }

    sl::Resource color_in{sl::ResourceType::eTex2d, in.color_rgb};
    sl::Resource color_out{sl::ResourceType::eTex2d, out_color_.Get()};
    sl::Resource depth{sl::ResourceType::eTex2d, mvec_.depth()};
    sl::Resource mvec_res{sl::ResourceType::eTex2d, mvec_.mvec()};

    sl::ResourceTag tags[] = {
        sl::ResourceTag{&color_in, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate},
        sl::ResourceTag{&color_out, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate},
        sl::ResourceTag{&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate},
        sl::ResourceTag{&mvec_res, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate},
    };

    if (SL_FAILED(r_tag, slSetTagForFrame(*frame, viewport_, tags, _countof(tags), in.ctx))) {
      out.status = "fallback:tags";
      return true;
    }

    const sl::BaseStructure* inputs[] = {&viewport_};
    if (SL_FAILED(r_ev, slEvaluateFeature(sl::kFeatureDLSS, *frame, inputs, 1, in.ctx))) {
      out.status = "fallback:evaluate";
      err = "slEvaluateFeature=" + std::to_string(static_cast<int>(r_ev));
      return true;
    }

    out.color_out = out_color_.Get();
    out.evaluated = true;
    out.status = "dlss";
    out.dlss_ms = clk.now_ms() - t0;
    return true;
  }

private:
  bool ensure_output(ID3D11Device* device, int w, int h, std::string& err) {
    if (out_color_ && out_w_ == w && out_h_ == h) return true;
    out_srv_.Reset();
    out_rtv_.Reset();
    out_color_.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &out_color_))) {
      err = "create dlss output failed";
      return false;
    }
    device->CreateRenderTargetView(out_color_.Get(), nullptr, &out_rtv_);
    device->CreateShaderResourceView(out_color_.Get(), nullptr, &out_srv_);
    out_w_ = w;
    out_h_ = h;
    return true;
  }

  EnhanceOptions opt_{};
  bool options_dirty_ = true;
  bool supported_ = false;
  bool inited_sl_ = false;
  int last_w_ = 0;
  int last_h_ = 0;
  int out_w_ = 0;
  int out_h_ = 0;
  sl::ViewportHandle viewport_{0};

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11Texture2D> out_color_;
  ComPtr<ID3D11RenderTargetView> out_rtv_;
  ComPtr<ID3D11ShaderResourceView> out_srv_;
  MvecDepthGenerator mvec_;
};

#endif  // NEXUSCAP_WITH_STREAMLINE

}  // namespace

std::unique_ptr<IFrameEnhance> create_dlss_enhance() {
#if defined(NEXUSCAP_WITH_STREAMLINE)
  return std::make_unique<StreamlineDlssEnhance>();
#else
  return create_passthrough_enhance();
#endif
}

}  // namespace nexuscap
