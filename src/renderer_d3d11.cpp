#include "nexuscap/renderer.hpp"
#include "nexuscap/capture_format.hpp"
#include "nexuscap/clock.hpp"
#include "nexuscap/config.hpp"
#include "nexuscap/display_geom.hpp"
#include "nexuscap/dlss_options.hpp"
#include "nexuscap/live_mode.hpp"
#include "nexuscap/lossless_fg.hpp"
#include "nexuscap/settings_store.hpp"
#include "nexuscap/system_metrics.hpp"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <avrt.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "avrt.lib")

using Microsoft::WRL::ComPtr;

namespace nexuscap {
namespace {

// MSI: 0 ms no fundo, hitch sobe. FG empilha um pouco acima da linha base.
int frametime_y(int gy, int gh, double ms, double ceil_ms) {
  const double t = (std::max)(0.0, (std::min)(1.0, ms / (std::max)(ceil_ms, 1.0)));
  return gy + gh - 2 - static_cast<int>(t * (gh - 4));
}

int frametime_y_trace(int gy, int gh, double cap_ms, int k, double ceil_ms) {
  const int y_cap = frametime_y(gy, gh, cap_ms, ceil_ms);
  if (k <= 1) return y_cap;
  const int y_fg = frametime_y(gy, gh, fg_frametime_trace_ms(cap_ms, k), ceil_ms);
  const int y = y_cap - fg_frametime_stack_lift_px(y_fg - y_cap, k);
  const int top = gy + 2;
  const int bot = gy + gh - 2;
  return y < top ? top : (y > bot ? bot : y);
}

void paint_frametime_traces(HDC hdc, int gx, int gy, int gw, int gh, const OverlayState& o, int pen_w) {
  if (!hdc || o.ft_hist.size() < 2 || gw < 8) return;
  const double target_ms = o.target_fps > 0 ? 1000.0 / o.target_fps : (1000.0 / 60.0);
  double ceil_ms = (std::max)(target_ms * 2.5, 40.0);
  for (double v : o.ft_hist) ceil_ms = (std::max)(ceil_ms, v * 1.05);
  ceil_ms = (std::min)(ceil_ms, 80.0);
  const size_t hn = o.ft_hist.size();
  const int traces = fg_frametime_trace_count(o.fg_on, o.fg_multiplier);
  const int w = (std::max)(1, pen_w);
  for (int k = 1; k <= traces; ++k) {
    const FgFtRgb rgb = fg_frametime_trace_rgb(k, o.ft_stable);
    HPEN pen = CreatePen(PS_SOLID, w, RGB(rgb.r, rgb.g, rgb.b));
    HGDIOBJ old = SelectObject(hdc, pen);
    for (size_t i = 0; i < hn; ++i) {
      const int x = gx + 2 + static_cast<int>((gw - 4) * static_cast<int>(i) / static_cast<int>(hn - 1));
      const int y = frametime_y_trace(gy, gh, o.ft_hist[i], k, ceil_ms);
      if (i == 0)
        MoveToEx(hdc, x, y, nullptr);
      else
        LineTo(hdc, x, y);
    }
    SelectObject(hdc, old);
    DeleteObject(pen);
  }
}

void paint_frametime_graph(HDC hdc, int gx, int gy, int gw, int gh, const OverlayState& o, int pen_w) {
  if (!hdc || gw < 8 || gh < 16) return;
  RECT graph_rc{gx, gy, gx + gw, gy + gh};
  if (overlay_draws_plate(o.overlay_no_bg)) {
    HBRUSH gbg = CreateSolidBrush(RGB(8, 10, 14));
    FillRect(hdc, &graph_rc, gbg);
    DeleteObject(gbg);
  }
  HPEN border = CreatePen(PS_SOLID, 1, o.overlay_no_bg ? RGB(90, 96, 110) : RGB(50, 56, 68));
  HGDIOBJ old_pen = SelectObject(hdc, border);
  HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
  Rectangle(hdc, gx, gy, gx + gw, gy + gh);

  const double target_ms = o.target_fps > 0 ? 1000.0 / o.target_fps : (1000.0 / 60.0);
  double ceil_ms = (std::max)(target_ms * 2.5, 40.0);
  for (double v : o.ft_hist) ceil_ms = (std::max)(ceil_ms, v * 1.05);
  ceil_ms = (std::min)(ceil_ms, 80.0);
  auto y_for = [&](double ms) -> int { return frametime_y(gy, gh, ms, ceil_ms); };
  HPEN target_pen = CreatePen(PS_DOT, 1, RGB(70, 160, 90));
  SelectObject(hdc, target_pen);
  const int y_tgt = y_for(target_ms);
  MoveToEx(hdc, gx + 1, y_tgt, nullptr);
  LineTo(hdc, gx + gw - 1, y_tgt);
  DeleteObject(target_pen);

  paint_frametime_traces(hdc, gx, gy, gw, gh, o, pen_w);

  const int traces = fg_frametime_trace_count(o.fg_on, o.fg_multiplier);
  if (traces >= 2) {
    HFONT lf = CreateFontW(13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ oldf = SelectObject(hdc, lf);
    SetBkMode(hdc, TRANSPARENT);
    const wchar_t* names[4] = {L"cap", L"x2", L"x3", L"x4"};
    int x = gx + 6;
    const int ly = gy + 3;
    for (int k = 1; k <= traces && k <= 4; ++k) {
      const FgFtRgb rgb = fg_frametime_trace_rgb(k, o.ft_stable);
      RECT sw{x, ly + 2, x + 9, ly + 12};
      HBRUSH br = CreateSolidBrush(RGB(rgb.r, rgb.g, rgb.b));
      FillRect(hdc, &sw, br);
      DeleteObject(br);
      SetTextColor(hdc, RGB(rgb.r, rgb.g, rgb.b));
      TextOutW(hdc, x + 11, ly, names[k - 1], static_cast<int>(wcslen(names[k - 1])));
      x += 36;
    }
    SelectObject(hdc, oldf);
    DeleteObject(lf);
  }

  SelectObject(hdc, old_brush);
  SelectObject(hdc, old_pen);
  DeleteObject(border);
}

static const char* kShaderSrc = R"(
Texture2D texY : register(t0);
Texture2D texUV : register(t1);
SamplerState samp : register(s0);

cbuffer YuvDecode : register(b0) {
  float y_scale;
  float y_offset;
  float c_scale;
  float c_offset;
  float r_cr;
  float g_cb;
  float g_cr;
  float b_cb;
};

struct VSOut {
  float4 pos : SV_POSITION;
  float2 uv : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID) {
  VSOut o;
  float2 p = float2((id << 1) & 2, id & 2);
  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
  o.uv = p;
  return o;
}

float4 PSMain(VSOut i) : SV_Target {
  float y = texY.Sample(samp, i.uv).r;
  float2 uv = texUV.Sample(samp, i.uv).rg;
  float Y = (y - y_offset) * y_scale;
  float Cb = (uv.x - c_offset) * c_scale;
  float Cr = (uv.y - c_offset) * c_scale;
  float R = Y + r_cr * Cr;
  float G = Y + g_cb * Cb + g_cr * Cr;
  float B = Y + b_cb * Cb;
  return float4(saturate(R), saturate(G), saturate(B), 1);
}
)";

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_ERASEBKGND) {
    HDC hdc = reinterpret_cast<HDC>(wp);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    return 1;
  }
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

struct RendererD3D11::Impl {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> ctx;
  ComPtr<IDXGISwapChain1> swap;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  ComPtr<ID3D11SamplerState> sampler;       // point — 1:1 / decode
  ComPtr<ID3D11SamplerState> samplerLinear; // linear — upsample when scale < 100
  ComPtr<ID3D11Texture2D> texY;
  ComPtr<ID3D11Texture2D> texUV;
  ComPtr<ID3D11ShaderResourceView> srvY;
  ComPtr<ID3D11ShaderResourceView> srvUV;
  // Intermediate RGB at signal resolution (NV12 decode target + DLSS input)
  ComPtr<ID3D11Texture2D> colorRt;
  ComPtr<ID3D11RenderTargetView> colorRtv;
  ComPtr<ID3D11ShaderResourceView> colorSrv;
  int color_w = 0;
  int color_h = 0;
  // Blit RGB → swapchain
  ComPtr<ID3D11VertexShader> blitVs;
  ComPtr<ID3D11PixelShader> blitPs;
  ComPtr<ID3D11Buffer> yuvCb;
  // Mini overlay burned into Present (OBS Game Capture sees it in fullscreen too).
  ComPtr<ID3D11Texture2D> hudTex;
  ComPtr<ID3D11ShaderResourceView> hudSrv;
  ComPtr<ID3D11BlendState> hudBlend;
  int hud_w = 0;
  int hud_h = 0;
  DWORD hud_last_tick = 0;
  ComPtr<ID3D11Texture2D> noticeTex;
  ComPtr<ID3D11ShaderResourceView> noticeSrv;
  int notice_w = 0;
  int notice_h = 0;
  DWORD notice_last_tick = 0;
  int color_space = static_cast<int>(kDefaultColorSpace);
  int color_range = static_cast<int>(kDefaultColorRange);
  bool yuv_dirty = true;
};

RendererD3D11::~RendererD3D11() { shutdown(); }

void RendererD3D11::shutdown() {
  delete impl_;
  impl_ = nullptr;
  hwnd_ = nullptr;
  back_w_ = back_h_ = tex_w_ = tex_h_ = 0;
  vsync_ = false;
  scale_percent_ = 100;
  color_space_ = static_cast<int>(kDefaultColorSpace);
  color_range_ = static_cast<int>(kDefaultColorRange);
  enhance_ = nullptr;
}

void RendererD3D11::set_scale_percent(int percent) { scale_percent_ = clamp_scale_percent(percent); }

void RendererD3D11::set_color_format(int space, int range) {
  const int s = clamp_color_space(space);
  const int r = clamp_color_range(range);
  if (s == color_space_ && r == color_range_) return;
  color_space_ = s;
  color_range_ = r;
  if (impl_) impl_->yuv_dirty = true;
}

bool RendererD3D11::init(HWND hwnd, int width, int height, std::string& err) {
  hwnd_ = hwnd;
  back_w_ = width;
  back_h_ = height;
  impl_ = new Impl();

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
  flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
  D3D_FEATURE_LEVEL fl_out{};
  const D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_0};
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fls, 1,
                                 D3D11_SDK_VERSION, &dev, &fl_out, &ctx);
  if (FAILED(hr)) {
    err = "D3D11CreateDevice failed";
    return false;
  }
  impl_->device = dev;
  impl_->ctx = ctx;

  ComPtr<IDXGIDevice> dxgi_dev;
  dev.As(&dxgi_dev);
  ComPtr<IDXGIAdapter> adapter;
  dxgi_dev->GetAdapter(&adapter);
  ComPtr<IDXGIFactory2> factory;
  adapter->GetParent(IID_PPV_ARGS(&factory));

  DXGI_SWAP_CHAIN_DESC1 sc{};
  sc.Width = static_cast<UINT>(width);
  sc.Height = static_cast<UINT>(height);
  sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sc.SampleDesc.Count = 1;
  sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sc.BufferCount = 2;
  sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sc.Scaling = DXGI_SCALING_STRETCH;
  sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  // Explicitly NO FRAME_LATENCY_WAITABLE_OBJECT; ALLOW_TEARING when supported (uncapped Present).
  sc.Flags = 0;
  {
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5))) {
      BOOL allow = FALSE;
      if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))) &&
          allow) {
        sc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
      }
    }
  }
  swap_flags_ = sc.Flags;

  hr = factory->CreateSwapChainForHwnd(dev.Get(), hwnd, &sc, nullptr, nullptr, &impl_->swap);
  if (FAILED(hr)) {
    err = "CreateSwapChainForHwnd failed";
    return false;
  }
  factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

  ComPtr<ID3D11Texture2D> back;
  impl_->swap->GetBuffer(0, IID_PPV_ARGS(&back));
  hr = dev->CreateRenderTargetView(back.Get(), nullptr, &impl_->rtv);
  if (FAILED(hr)) {
    err = "CreateRenderTargetView failed";
    return false;
  }
  if (!create_pipeline(err)) return false;
  if (!create_blit_pipeline(err)) return false;
  return true;
}

ID3D11Device* RendererD3D11::device() const { return impl_ ? impl_->device.Get() : nullptr; }
ID3D11DeviceContext* RendererD3D11::context() const { return impl_ ? impl_->ctx.Get() : nullptr; }

void RendererD3D11::resize(int width, int height) {
  if (!impl_ || !impl_->swap) return;
  back_w_ = width;
  back_h_ = height;
  impl_->ctx->OMSetRenderTargets(0, nullptr, nullptr);
  impl_->rtv.Reset();
  impl_->swap->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
  ComPtr<ID3D11Texture2D> back;
  impl_->swap->GetBuffer(0, IID_PPV_ARGS(&back));
  impl_->device->CreateRenderTargetView(back.Get(), nullptr, &impl_->rtv);
}

bool RendererD3D11::create_pipeline(std::string& err) {
  ComPtr<ID3DBlob> vsb, psb, errb;
  HRESULT hr = D3DCompile(kShaderSrc, std::strlen(kShaderSrc), "nv12.hlsl", nullptr, nullptr, "VSMain",
                          "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &errb);
  if (FAILED(hr)) {
    err = "VS compile failed";
    return false;
  }
  hr = D3DCompile(kShaderSrc, std::strlen(kShaderSrc), "nv12.hlsl", nullptr, nullptr, "PSMain", "ps_5_0",
                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &errb);
  if (FAILED(hr)) {
    err = "PS compile failed";
    return false;
  }
  impl_->device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &impl_->vs);
  impl_->device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &impl_->ps);

  D3D11_BUFFER_DESC cbd{};
  cbd.ByteWidth = 32;  // 8 floats
  cbd.Usage = D3D11_USAGE_DYNAMIC;
  cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(impl_->device->CreateBuffer(&cbd, nullptr, &impl_->yuvCb))) {
    err = "create YUV CB failed";
    return false;
  }
  impl_->yuv_dirty = true;

  D3D11_SAMPLER_DESC sd{};
  // Point = nitidez 1:1 (linear amolecia chroma vs X2)
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  impl_->device->CreateSamplerState(&sd, &impl_->sampler);
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  impl_->device->CreateSamplerState(&sd, &impl_->samplerLinear);
  return true;
}

bool RendererD3D11::create_blit_pipeline(std::string& err) {
  static const char* kBlit = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
  VSOut o;
  float2 p = float2((id << 1) & 2, id & 2);
  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
  o.uv = p;
  return o;
}
float4 PSMain(VSOut i) : SV_Target { return tex.Sample(samp, i.uv); }
)";
  ComPtr<ID3DBlob> vsb, psb, errb;
  if (FAILED(D3DCompile(kBlit, std::strlen(kBlit), "blit.hlsl", nullptr, nullptr, "VSMain", "vs_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &errb))) {
    err = "blit VS compile failed";
    return false;
  }
  if (FAILED(D3DCompile(kBlit, std::strlen(kBlit), "blit.hlsl", nullptr, nullptr, "PSMain", "ps_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &errb))) {
    err = "blit PS compile failed";
    return false;
  }
  impl_->device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &impl_->blitVs);
  impl_->device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &impl_->blitPs);
  return true;
}

bool RendererD3D11::create_color_rt(int w, int h, std::string& err) {
  if (impl_->colorRt && impl_->color_w == w && impl_->color_h == h) return true;
  impl_->colorSrv.Reset();
  impl_->colorRtv.Reset();
  impl_->colorRt.Reset();
  D3D11_TEXTURE2D_DESC td{};
  td.Width = static_cast<UINT>(w);
  td.Height = static_cast<UINT>(h);
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(impl_->device->CreateTexture2D(&td, nullptr, &impl_->colorRt))) {
    err = "create color RT failed";
    return false;
  }
  impl_->device->CreateRenderTargetView(impl_->colorRt.Get(), nullptr, &impl_->colorRtv);
  impl_->device->CreateShaderResourceView(impl_->colorRt.Get(), nullptr, &impl_->colorSrv);
  impl_->color_w = w;
  impl_->color_h = h;
  return true;
}

bool RendererD3D11::create_nv12_textures(int w, int h, std::string& err) {
  if (w == tex_w_ && h == tex_h_ && impl_->texY) return true;
  impl_->texY.Reset();
  impl_->texUV.Reset();
  impl_->srvY.Reset();
  impl_->srvUV.Reset();
  tex_w_ = w;
  tex_h_ = h;

  D3D11_TEXTURE2D_DESC yd{};
  yd.Width = w;
  yd.Height = h;
  yd.MipLevels = 1;
  yd.ArraySize = 1;
  yd.Format = DXGI_FORMAT_R8_UNORM;
  yd.SampleDesc.Count = 1;
  yd.Usage = D3D11_USAGE_DYNAMIC;
  yd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  yd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  HRESULT hr = impl_->device->CreateTexture2D(&yd, nullptr, &impl_->texY);
  if (FAILED(hr)) {
    err = "CreateTexture2D Y failed";
    return false;
  }

  D3D11_TEXTURE2D_DESC uvd = yd;
  uvd.Width = w / 2;
  uvd.Height = h / 2;
  uvd.Format = DXGI_FORMAT_R8G8_UNORM;
  hr = impl_->device->CreateTexture2D(&uvd, nullptr, &impl_->texUV);
  if (FAILED(hr)) {
    err = "CreateTexture2D UV failed";
    return false;
  }
  impl_->device->CreateShaderResourceView(impl_->texY.Get(), nullptr, &impl_->srvY);
  impl_->device->CreateShaderResourceView(impl_->texUV.Get(), nullptr, &impl_->srvUV);
  return true;
}

bool RendererD3D11::present_frame(const FramePayload& frame, OverlayState& overlay,
                                  double& present_ms, std::string& err) {
  if (!impl_) {
    err = "renderer not init";
    return false;
  }
  QpcClock clk;
  const double t0 = clk.now_ms();
  if (!create_nv12_textures(frame.width, frame.height, err)) return false;

  // Work RT at scale% — 50% ≈ 1/4 RGB pixels (decode/NR cheaper); blit upscales soft.
  const int work_w = scaled_extent(frame.width, scale_percent_);
  const int work_h = scaled_extent(frame.height, scale_percent_);
  if (!create_color_rt(work_w, work_h, err)) return false;
  // HUD reports the DLSS5 work resolution (Feed) when it owns the scale, not this RT.
  const int hud_scale = hud_work_scale_percent_ > 0 ? clamp_scale_percent(hud_work_scale_percent_) : scale_percent_;
  overlay.scale_percent = hud_scale;
  overlay.work_w = scaled_extent(frame.width, hud_scale);
  overlay.work_h = scaled_extent(frame.height, hud_scale);

  const size_t y_size = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height);
  if (frame.nv12.size() < y_size + y_size / 2) {
    err = "nv12 buffer too small";
    return false;
  }

  D3D11_MAPPED_SUBRESOURCE map{};
  if (SUCCEEDED(impl_->ctx->Map(impl_->texY.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
    const uint8_t* src = frame.nv12.data();
    if (map.RowPitch == static_cast<UINT>(frame.width)) {
      std::memcpy(map.pData, src, y_size);
    } else {
      for (int y = 0; y < frame.height; ++y) {
        std::memcpy(static_cast<uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
                    src + static_cast<size_t>(y) * static_cast<size_t>(frame.width),
                    static_cast<size_t>(frame.width));
      }
    }
    impl_->ctx->Unmap(impl_->texY.Get(), 0);
  }
  if (SUCCEEDED(impl_->ctx->Map(impl_->texUV.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
    const uint8_t* src = frame.nv12.data() + y_size;
    const int uv_h = frame.height / 2;
    // UV is R8G8 packed: each row is `width` bytes for width/2 texels.
    if (map.RowPitch == static_cast<UINT>(frame.width)) {
      std::memcpy(map.pData, src, y_size / 2);
    } else {
      for (int y = 0; y < uv_h; ++y) {
        std::memcpy(static_cast<uint8_t*>(map.pData) + static_cast<size_t>(y) * map.RowPitch,
                    src + static_cast<size_t>(y) * static_cast<size_t>(frame.width),
                    static_cast<size_t>(frame.width));
      }
    }
    impl_->ctx->Unmap(impl_->texUV.Get(), 0);
  }

  // 1) Decode NV12 → work RGB (downsample via smaller RT when scale < 100)
  {
    if (impl_->yuvCb && (impl_->yuv_dirty || impl_->color_space != color_space_ ||
                         impl_->color_range != color_range_)) {
      const YuvRgbCoeffs c = yuv_rgb_coeffs(color_space_, color_range_);
      D3D11_MAPPED_SUBRESOURCE cmap{};
      if (SUCCEEDED(impl_->ctx->Map(impl_->yuvCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &cmap))) {
        float* f = static_cast<float*>(cmap.pData);
        f[0] = c.y_scale;
        f[1] = c.y_offset;
        f[2] = c.c_scale;
        f[3] = c.c_offset;
        f[4] = c.r_cr;
        f[5] = c.g_cb;
        f[6] = c.g_cr;
        f[7] = c.b_cb;
        impl_->ctx->Unmap(impl_->yuvCb.Get(), 0);
        impl_->color_space = color_space_;
        impl_->color_range = color_range_;
        impl_->yuv_dirty = false;
      }
    }

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(work_w);
    vp.Height = static_cast<float>(work_h);
    vp.MaxDepth = 1.f;
    const float clear[4] = {0, 0, 0, 1};
    impl_->ctx->OMSetRenderTargets(1, impl_->colorRtv.GetAddressOf(), nullptr);
    impl_->ctx->ClearRenderTargetView(impl_->colorRtv.Get(), clear);
    impl_->ctx->RSSetViewports(1, &vp);
    impl_->ctx->VSSetShader(impl_->vs.Get(), nullptr, 0);
    impl_->ctx->PSSetShader(impl_->ps.Get(), nullptr, 0);
    if (impl_->yuvCb) {
      ID3D11Buffer* cbs[] = {impl_->yuvCb.Get()};
      impl_->ctx->PSSetConstantBuffers(0, 1, cbs);
    }
    ID3D11ShaderResourceView* srvs[] = {impl_->srvY.Get(), impl_->srvUV.Get()};
    impl_->ctx->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* samp =
        (scale_percent_ < 100 && impl_->samplerLinear) ? impl_->samplerLinear.Get() : impl_->sampler.Get();
    impl_->ctx->PSSetSamplers(0, 1, &samp);
    impl_->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    impl_->ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[] = {nullptr, nullptr};
    impl_->ctx->PSSetShaderResources(0, 2, nulls);
  }

  // 2) Optional DLSS enhance at work resolution
  ID3D11Texture2D* show_tex = impl_->colorRt.Get();
  overlay.dlss_ms = 0;
  overlay.dlss_status = "passthrough";
  if (enhance_) {
    EnhanceFrameIn ein;
    ein.device = impl_->device.Get();
    ein.ctx = impl_->ctx.Get();
    ein.color_rgb = impl_->colorRt.Get();
    ein.width = work_w;
    ein.height = work_h;
    ein.frame_index = frame.frame_index;
    EnhanceFrameOut eout;
    std::string eerr;
    if (enhance_->evaluate(ein, eout, eerr) && eout.color_out) {
      show_tex = eout.color_out;
      overlay.dlss_ms = eout.dlss_ms;
      overlay.dlss_status = eout.status;
    } else if (!eerr.empty()) {
      overlay.dlss_status = "error:" + eerr;
    }
    overlay.dlss_on = enhance_->enabled();
    overlay.dlss_supported = enhance_->feature_supported();
    overlay.dlss_backend = enhance_->backend_name();
    overlay.dlss_mode = dlss_mode_name(enhance_->options().mode);
  }

  // 3) Blit work → swapchain. Linear upsample when scaled = visibly softer/blurrier.
  ComPtr<ID3D11ShaderResourceView> show_srv;
  if (show_tex == impl_->colorRt.Get()) {
    show_srv = impl_->colorSrv;
  } else {
    impl_->device->CreateShaderResourceView(show_tex, nullptr, &show_srv);
  }

  // Stretch fill — no letterbox bars in windowed or fullscreen (OBS-style).
  const auto box = fit_fill(work_w, work_h, back_w_, back_h_);
  D3D11_VIEWPORT vp{};
  vp.MinDepth = 0.f;
  vp.MaxDepth = 1.f;
  vp.TopLeftX = box.x;
  vp.TopLeftY = box.y;
  vp.Width = box.w;
  vp.Height = box.h;

  const float clear[4] = {0, 0, 0, 1};
  impl_->ctx->OMSetRenderTargets(1, impl_->rtv.GetAddressOf(), nullptr);
  impl_->ctx->ClearRenderTargetView(impl_->rtv.Get(), clear);
  impl_->ctx->RSSetViewports(1, &vp);
  impl_->ctx->VSSetShader(impl_->blitVs.Get(), nullptr, 0);
  impl_->ctx->PSSetShader(impl_->blitPs.Get(), nullptr, 0);
  ID3D11ShaderResourceView* blit_srv[] = {show_srv.Get()};
  impl_->ctx->PSSetShaderResources(0, 1, blit_srv);
  ID3D11SamplerState* blit_samp =
      (scale_percent_ < 100 && impl_->samplerLinear) ? impl_->samplerLinear.Get() : impl_->sampler.Get();
  impl_->ctx->PSSetSamplers(0, 1, &blit_samp);
  impl_->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  impl_->ctx->Draw(3, 0);
  ID3D11ShaderResourceView* null_srv[] = {nullptr};
  impl_->ctx->PSSetShaderResources(0, 1, null_srv);

  // Bake mini overlay into the swapchain so OBS Game Capture (fullscreen/window) gets it.
  // Live overlay uses its own HWND — never burn mini into the same space.
  if (overlay.mini_overlay && overlay.visible && !overlay.live_overlay) {
    burn_mini_overlay(overlay);
  }
  if (overlay.hdmi_hold_notice) {
    burn_hdmi_hold_notice(overlay);
  }

  // CFG da sessão por cima de tudo (mesmo backbuffer, mesmo Present).
  if (ui_draw_) {
    D3D11_VIEWPORT full{};
    full.Width = static_cast<float>(back_w_);
    full.Height = static_cast<float>(back_h_);
    full.MaxDepth = 1.f;
    impl_->ctx->OMSetRenderTargets(1, impl_->rtv.GetAddressOf(), nullptr);
    impl_->ctx->RSSetViewports(1, &full);
    ui_draw_(ui_draw_user_);
  }

  UINT present_flags = 0;
  if (!vsync_ && (swap_flags_ & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)) {
    present_flags = DXGI_PRESENT_ALLOW_TEARING;
  }
  impl_->swap->Present(vsync_ ? present_interval_ : 0u, present_flags);
  present_ms = clk.now_ms() - t0;
  (void)err;
  return true;
}

void RendererD3D11::present_black() {
  if (!impl_ || !impl_->swap) return;
  const float clear[4] = {0, 0, 0, 1};
  impl_->ctx->OMSetRenderTargets(1, impl_->rtv.GetAddressOf(), nullptr);
  impl_->ctx->ClearRenderTargetView(impl_->rtv.Get(), clear);
  impl_->swap->Present(0, 0);
}

namespace {

constexpr wchar_t kHudClass[] = L"NexusCapHud";

void apply_overlay_layered_attrs(HWND hwnd, bool overlay_no_bg) {
  if (!hwnd) return;
  if (overlay_draws_plate(overlay_no_bg)) {
    SetLayeredWindowAttributes(hwnd, 0, 220, LWA_ALPHA);
  } else {
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 255, LWA_COLORKEY);
  }
}

std::wstring utf8_to_wide_hud(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 0) return L"";
  std::wstring w(static_cast<size_t>(n), 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  if (!w.empty() && w.back() == 0) w.pop_back();
  return w;
}

void hud_draw_title_with_console(HDC hdc, int x, int y, const wchar_t* prefix, const OverlayState& o) {
  SetTextColor(hdc, RGB(255, 210, 70));
  const int plen = static_cast<int>(wcslen(prefix));
  TextOutW(hdc, x, y, prefix, plen);
  if (!o.overlay_console) return;
  SIZE sz{};
  GetTextExtentPoint32W(hdc, prefix, plen, &sz);
  char chip[48]{};
  format_overlay_console_chip(chip, sizeof(chip), clamp_live_console(o.live_console));
  const std::wstring w = utf8_to_wide_hud(chip);
  SetTextColor(hdc, live_console_color(clamp_live_console(o.live_console)));
  TextOutW(hdc, x + sz.cx, y, w.c_str(), static_cast<int>(w.size()));
}

SystemMetricsSample metrics_from_overlay(const OverlayState& o) {
  SystemMetricsSample s;
  s.cpu_ok = o.cpu_ok;
  s.cpu_percent = o.cpu_percent;
  s.cpu_temp_ok = o.cpu_temp_ok;
  s.cpu_temp_c = o.cpu_temp_c;
  s.gpu_ok = o.gpu_ok;
  s.gpu_percent = o.gpu_percent;
  s.gpu_temp_ok = o.gpu_temp_ok;
  s.gpu_temp_c = o.gpu_temp_c;
  s.vram_ok = o.vram_ok;
  s.vram_used_mb = o.vram_used_mb;
  s.vram_budget_mb = o.vram_budget_mb;
  s.ram_ok = o.ram_ok;
  s.ram_used_mb = o.ram_used_mb;
  s.ram_total_mb = o.ram_total_mb;
  s.power_ok = o.power_ok;
  s.power_w = o.power_w;
  s.cpu_name = o.cpu_name;
  s.gpu_name = o.gpu_name;
  return s;
}

// Copy UTF-8 metric lines into wide HUD rows (keeps wstring alive for swprintf).
int append_hw_metric_lines(wchar_t (*lines)[180], int n, int max_n, const OverlayState& o) {
  char hw[kSysMetricsLineCount][64]{};
  const int hn = format_sys_metrics_lines(hw, kSysMetricsLineCount, metrics_from_overlay(o));
  for (int i = 0; i < hn && n < max_n; ++i) {
    const std::wstring w = utf8_to_wide_hud(hw[i]);
    swprintf(lines[n++], 180, L"%s", w.c_str());
  }
  return n;
}

void paint_mini_overlay_hdc(HDC hdc, const RECT& rc, const OverlayState& o) {
  if (overlay_draws_plate(o.overlay_no_bg)) {
    HBRUSH bg = CreateSolidBrush(RGB(12, 14, 18));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
  }
  SetBkMode(hdc, TRANSPARENT);

  const int font_px = hud_mini_overlay_font_px();
  HFONT mini_font = CreateFontW(font_px, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                                FIXED_PITCH | FF_MODERN, L"Consolas");
  HFONT old = static_cast<HFONT>(SelectObject(hdc, mini_font));
  char line0[768]{};
  char line1[768]{};
  char hw_compact[200]{};
  format_sys_metrics_compact(hw_compact, sizeof(hw_compact), metrics_from_overlay(o));
  const double show_fps = o.fps_smooth > 0 ? o.fps_smooth : o.fps;
  const bool dlss5_on = o.renodx_on;
  MiniOverlaySeg segs[kMiniOverlayMaxSegs]{};
  unsigned char cr = 0, cg = 55, cb = 145;
  live_console_rgb(clamp_live_console(o.live_console), cr, cg, cb);
  const int nseg = format_mini_overlay_segments(
      segs, kMiniOverlayMaxSegs, line0, sizeof(line0), line1, sizeof(line1), show_fps, o.fps_fg_est, o.fg_on,
      o.fg_multiplier, dlss5_on, o.sync_image, o.frame_ms, o.ft_avg_ms, o.ft_p95_ms, o.ft_max_ms, o.ft_stable,
      o.e2e_ms, o.present_ms, o.stalls_window, o.clusters, o.dropped, o.capture_w, o.capture_h, o.work_w,
      o.work_h, o.output_w, o.output_h, o.scale_percent, o.has_stats600, o.s600_fps, o.s600_p95_e2e,
      o.s600_p95_present, o.s600_stalls, o.renodx_preset.c_str(), hw_compact[0] ? hw_compact : nullptr,
      o.dlss_ms, o.frames, o.s600_max_interval, o.fg_backend.c_str(), o.target_fps, o.nr_pass_count,
      overlay_console_label_or_null(o.overlay_console, clamp_live_console(o.live_console)), false, cr, cg, cb);
  (void)line0;
  (void)line1;

  const int pad = 8;
  const int graph_w = 220;
  const int gap = 8;
  const int bar_w = static_cast<int>(rc.right - rc.left);
  const int bar_h = static_cast<int>(rc.bottom - rc.top);
  const int gx = (std::max)(pad, bar_w - pad - graph_w);
  const int gy = 8;
  const int gh = (std::max)(51, bar_h - 16);
  const int gw = (std::min)(graph_w, bar_w - pad * 2);
  const int text_max_x = gx - gap;
  const int line_gap = 2;
  const int rows = hud_mini_overlay_rows();
  const int block_h = font_px * rows + line_gap * (rows - 1);
  const int y0 = (std::max)(2, (bar_h - block_h) / 2);

  int wrap_st = nseg;
  int wrap_l600 = nseg;
  mini_overlay_row_breaks(segs, nseg, &wrap_st, &wrap_l600);
  // Do not start a painted row on the " · " separator.
  auto skip_sep = [&](int i) -> int {
    while (i < nseg && std::strcmp(segs[i].text, " · ") == 0) ++i;
    return i;
  };
  wrap_st = skip_sep(wrap_st);
  wrap_l600 = skip_sep(wrap_l600);

  auto paint_row = [&](int begin, int end, int text_y) {
    int x = pad;
    begin = skip_sep(begin);
    for (int i = begin; i < end; ++i) {
      const std::wstring w = utf8_to_wide_hud(segs[i].text);
      if (w.empty()) continue;
      SIZE sz{};
      GetTextExtentPoint32W(hdc, w.c_str(), static_cast<int>(w.size()), &sz);
      if (x >= text_max_x) break;
      if (x + sz.cx > text_max_x) {
        RECT clip{x, text_y - 1, text_max_x, text_y + font_px + 1};
        SetTextColor(hdc, RGB(segs[i].r, segs[i].g, segs[i].b));
        DrawTextW(hdc, w.c_str(), static_cast<int>(w.size()), &clip,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        break;
      }
      SetTextColor(hdc, RGB(segs[i].r, segs[i].g, segs[i].b));
      TextOutW(hdc, x, text_y, w.c_str(), static_cast<int>(w.size()));
      x += sz.cx;
    }
  };
  paint_row(0, wrap_st, y0);
  paint_row(wrap_st, wrap_l600, y0 + font_px + line_gap);
  paint_row(wrap_l600, nseg, y0 + (font_px + line_gap) * 2);

  if (o.perf_remain_sec >= -1) {
    wchar_t tb[24]{};
    if (o.perf_remain_sec < 0) {
      swprintf(tb, 24, L"A carregar");
    } else {
      int s = o.perf_remain_sec;
      if (s > 40) s = 40;
      if (s < 0) s = 0;
      swprintf(tb, 24, L"0:%02d", s);
    }
    SIZE tsz{};
    GetTextExtentPoint32W(hdc, tb, static_cast<int>(wcslen(tb)), &tsz);
    const int tx = (std::max)(pad, text_max_x - static_cast<int>(tsz.cx));
    SetTextColor(hdc, o.perf_remain_sec < 0 ? RGB(255, 196, 72) : RGB(255, 220, 120));
    TextOutW(hdc, tx, y0, tb, static_cast<int>(wcslen(tb)));
  }

  if (gw >= 64) paint_frametime_graph(hdc, gx, gy, gw, gh, o, 2);

  SelectObject(hdc, old);
  DeleteObject(mini_font);

  if (!o.session_notice.empty()) {
    std::string title, detail;
    session_notice_split(o.session_notice, title, detail);
    HFONT notice_font = CreateFontW(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                                    DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT oldn = static_cast<HFONT>(SelectObject(hdc, notice_font));
    SetTextColor(hdc, RGB(255, 200, 90));
    const std::wstring wt = utf8_to_wide_hud(title);
    TextOutW(hdc, pad, bar_h > 40 ? bar_h - 22 : 36, wt.c_str(), static_cast<int>(wt.size()));
    SelectObject(hdc, oldn);
    DeleteObject(notice_font);
  }
}

void paint_hdmi_hold_card_hdc(HDC hdc, const RECT& rc, const OverlayState& overlay) {
  HBRUSH bg = CreateSolidBrush(RGB(14, 18, 26));
  FillRect(hdc, &rc, bg);
  DeleteObject(bg);
  HPEN edge = CreatePen(PS_SOLID, 2, RGB(240, 180, 65));
  HGDIOBJ old_pen = SelectObject(hdc, edge);
  HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
  Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
  SelectObject(hdc, old_pen);
  SelectObject(hdc, old_brush);
  DeleteObject(edge);

  SetBkMode(hdc, TRANSPARENT);
  const std::wstring title = utf8_to_wide_hud(
      overlay.hdmi_notice_title.empty() ? session_hdmi_notice_title() : overlay.hdmi_notice_title);
  const std::wstring detail = utf8_to_wide_hud(
      overlay.hdmi_notice_detail.empty() ? session_hdmi_notice_detail() : overlay.hdmi_notice_detail);

  HFONT big = CreateFontW(32, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  HFONT old = static_cast<HFONT>(SelectObject(hdc, big));
  SetTextColor(hdc, RGB(255, 210, 110));
  RECT tr{rc.left + 24, rc.top + 18, rc.right - 24, rc.top + 62};
  DrawTextW(hdc, title.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
  SelectObject(hdc, old);
  DeleteObject(big);

  HFONT small_font = CreateFontW(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  old = static_cast<HFONT>(SelectObject(hdc, small_font));
  SetTextColor(hdc, RGB(205, 225, 250));
  RECT dr{rc.left + 24, rc.top + 68, rc.right - 24, rc.bottom - 16};
  DrawTextW(hdc, detail.c_str(), -1, &dr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
  SelectObject(hdc, old);
  DeleteObject(small_font);
}

}  // namespace

void RendererD3D11::burn_mini_overlay(const OverlayState& overlay) {
  if (!impl_ || !impl_->ctx || !impl_->rtv || back_w_ < 64 || back_h_ < 32) return;

  const int gap = 8;
  int bar_w = (std::max)(200, back_w_ - gap * 2);
  int bar_h = hud_mini_overlay_height();
  if (!overlay.session_notice.empty()) {
    std::string t, d;
    session_notice_split(overlay.session_notice, t, d);
    bar_h += d.empty() ? 33 : 60;
  }
  if (bar_w > back_w_) bar_w = back_w_;
  if (bar_h > back_h_) bar_h = back_h_;

  // Rebuild GPU texture if size changed.
  if (!impl_->hudTex || impl_->hud_w != bar_w || impl_->hud_h != bar_h) {
    impl_->hudSrv.Reset();
    impl_->hudTex.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(bar_w);
    td.Height = static_cast<UINT>(bar_h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(impl_->device->CreateTexture2D(&td, nullptr, &impl_->hudTex))) return;
    impl_->device->CreateShaderResourceView(impl_->hudTex.Get(), nullptr, &impl_->hudSrv);
    impl_->hud_w = bar_w;
    impl_->hud_h = bar_h;
    impl_->hud_last_tick = 0;
  }

  // Throttle GDI raster ~30 Hz. Notice no longer forces every Present (isso travava o HUD no 2x).
  const DWORD now = GetTickCount();
  const bool refresh = (impl_->hud_last_tick == 0) || (now - impl_->hud_last_tick) >= 33;
  if (refresh) {
    impl_->hud_last_tick = now;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bar_w;
    bmi.bmiHeader.biHeight = -bar_h;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dib || !bits) {
      if (mem) DeleteDC(mem);
      return;
    }
    HGDIOBJ old_bmp = SelectObject(mem, dib);
    RECT rc{0, 0, bar_w, bar_h};
    if (overlay.overlay_no_bg && bits) {
      std::memset(bits, 0, static_cast<size_t>(bar_w) * static_cast<size_t>(bar_h) * 4u);
    }
    paint_mini_overlay_hdc(mem, rc, overlay);

    // GDI BI_RGB deixa alpha=0. Com placa: ~86% no quadro todo. Sem fundo: so a tinta.
    {
      auto* px = static_cast<uint32_t*>(bits);
      const int n = bar_w * bar_h;
      if (overlay.overlay_no_bg) {
        for (int i = 0; i < n; ++i) {
          const uint32_t rgb = px[i] & 0x00FFFFFFu;
          px[i] = rgb ? (rgb | 0xFF000000u) : 0;
        }
      } else {
        constexpr uint32_t kA = 220u << 24;
        for (int i = 0; i < n; ++i) px[i] = (px[i] & 0x00FFFFFFu) | kA;
      }
    }

    D3D11_MAPPED_SUBRESOURCE map{};
    if (SUCCEEDED(impl_->ctx->Map(impl_->hudTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
      const auto* src = static_cast<const uint8_t*>(bits);
      auto* dst = static_cast<uint8_t*>(map.pData);
      const size_t row_bytes = static_cast<size_t>(bar_w) * 4u;
      for (int y = 0; y < bar_h; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * map.RowPitch, src + static_cast<size_t>(y) * row_bytes,
                    row_bytes);
      }
      impl_->ctx->Unmap(impl_->hudTex.Get(), 0);
    }
    SelectObject(mem, old_bmp);
    DeleteObject(dib);
    DeleteDC(mem);
  }

  if (!impl_->hudSrv) return;
  if (!impl_->hudBlend) {
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    impl_->device->CreateBlendState(&bd, &impl_->hudBlend);
  }
  D3D11_VIEWPORT vp{};
  vp.MinDepth = 0.f;
  vp.MaxDepth = 1.f;
  vp.TopLeftX = static_cast<float>(gap);
  vp.TopLeftY = static_cast<float>(gap);
  vp.Width = static_cast<float>(bar_w);
  vp.Height = static_cast<float>(bar_h);
  impl_->ctx->OMSetRenderTargets(1, impl_->rtv.GetAddressOf(), nullptr);
  impl_->ctx->RSSetViewports(1, &vp);
  const float bf[4] = {0, 0, 0, 0};
  if (impl_->hudBlend) impl_->ctx->OMSetBlendState(impl_->hudBlend.Get(), bf, 0xffffffff);
  impl_->ctx->VSSetShader(impl_->blitVs.Get(), nullptr, 0);
  impl_->ctx->PSSetShader(impl_->blitPs.Get(), nullptr, 0);
  ID3D11ShaderResourceView* srv[] = {impl_->hudSrv.Get()};
  impl_->ctx->PSSetShaderResources(0, 1, srv);
  ID3D11SamplerState* samp[] = {impl_->sampler.Get()};
  impl_->ctx->PSSetSamplers(0, 1, samp);
  impl_->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  impl_->ctx->Draw(3, 0);
  ID3D11ShaderResourceView* null_srv[] = {nullptr};
  impl_->ctx->PSSetShaderResources(0, 1, null_srv);
  impl_->ctx->OMSetBlendState(nullptr, bf, 0xffffffff);
}

void RendererD3D11::burn_hdmi_hold_notice(const OverlayState& overlay) {
  if (!impl_ || !impl_->ctx || !impl_->rtv || back_w_ < 200 || back_h_ < 80) return;

  const int nw = (std::min)(900, (std::max)(320, back_w_ - 48));
  const int nh = (std::min)(168, (std::max)(120, back_h_ / 8));
  if (nw < 200 || nh < 80) return;

  if (!impl_->noticeTex || impl_->notice_w != nw || impl_->notice_h != nh) {
    impl_->noticeSrv.Reset();
    impl_->noticeTex.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(nw);
    td.Height = static_cast<UINT>(nh);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(impl_->device->CreateTexture2D(&td, nullptr, &impl_->noticeTex))) return;
    impl_->device->CreateShaderResourceView(impl_->noticeTex.Get(), nullptr, &impl_->noticeSrv);
    impl_->notice_w = nw;
    impl_->notice_h = nh;
    impl_->notice_last_tick = 0;
  }

  const DWORD now = GetTickCount();
  const bool refresh = (impl_->notice_last_tick == 0) || (now - impl_->notice_last_tick) >= 80;
  if (refresh) {
    impl_->notice_last_tick = now;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = nw;
    bmi.bmiHeader.biHeight = -nh;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dib || !bits) {
      if (mem) DeleteDC(mem);
      return;
    }
    HGDIOBJ old_bmp = SelectObject(mem, dib);
    RECT rc{0, 0, nw, nh};
    paint_hdmi_hold_card_hdc(mem, rc, overlay);
    auto* px = static_cast<uint32_t*>(bits);
    const int n = nw * nh;
    constexpr uint32_t kA = 230u << 24;
    for (int i = 0; i < n; ++i) px[i] = (px[i] & 0x00FFFFFFu) | kA;

    D3D11_MAPPED_SUBRESOURCE map{};
    if (SUCCEEDED(impl_->ctx->Map(impl_->noticeTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
      const auto* src = static_cast<const uint8_t*>(bits);
      auto* dst = static_cast<uint8_t*>(map.pData);
      const size_t row_bytes = static_cast<size_t>(nw) * 4u;
      for (int y = 0; y < nh; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * map.RowPitch, src + static_cast<size_t>(y) * row_bytes,
                    row_bytes);
      }
      impl_->ctx->Unmap(impl_->noticeTex.Get(), 0);
    }
    SelectObject(mem, old_bmp);
    DeleteObject(dib);
    DeleteDC(mem);
  }

  if (!impl_->noticeSrv) return;
  if (!impl_->hudBlend) {
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    impl_->device->CreateBlendState(&bd, &impl_->hudBlend);
  }
  D3D11_VIEWPORT vp{};
  vp.MinDepth = 0.f;
  vp.MaxDepth = 1.f;
  vp.TopLeftX = static_cast<float>((std::max)(0, (back_w_ - nw) / 2));
  vp.TopLeftY = static_cast<float>((std::max)(0, (back_h_ - nh) / 2));
  vp.Width = static_cast<float>(nw);
  vp.Height = static_cast<float>(nh);
  impl_->ctx->OMSetRenderTargets(1, impl_->rtv.GetAddressOf(), nullptr);
  impl_->ctx->RSSetViewports(1, &vp);
  const float bf[4] = {0, 0, 0, 0};
  if (impl_->hudBlend) impl_->ctx->OMSetBlendState(impl_->hudBlend.Get(), bf, 0xffffffff);
  impl_->ctx->VSSetShader(impl_->blitVs.Get(), nullptr, 0);
  impl_->ctx->PSSetShader(impl_->blitPs.Get(), nullptr, 0);
  ID3D11ShaderResourceView* srv[] = {impl_->noticeSrv.Get()};
  impl_->ctx->PSSetShaderResources(0, 1, srv);
  ID3D11SamplerState* samp[] = {impl_->sampler.Get()};
  impl_->ctx->PSSetSamplers(0, 1, samp);
  impl_->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  impl_->ctx->Draw(3, 0);
  ID3D11ShaderResourceView* null_srv[] = {nullptr};
  impl_->ctx->PSSetShaderResources(0, 1, null_srv);
  impl_->ctx->OMSetBlendState(nullptr, bf, 0xffffffff);
}

bool MetricsHud::create(HWND owner, HINSTANCE hi) {
  destroy();
  owner_ = owner;
  WNDCLASSW wc{};
  wc.lpfnWndProc = MetricsHud::WndProc;
  wc.hInstance = hi;
  wc.lpszClassName = kHudClass;
  wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&wc);

  // Semi-transparent TOPMOST popup (same feel as session/HW panels).
  hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kHudClass,
                          L"NexusCap metrics", WS_POPUP, 0, 0, 920, 780, owner, nullptr, hi, this);
  if (!hwnd_) return false;
  SetLayeredWindowAttributes(hwnd_, 0, 220, LWA_ALPHA);
  return true;
}

void MetricsHud::destroy() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  owner_ = nullptr;
}

void MetricsHud::set_visible(bool visible) {
  visible_ = visible;
  if (!hwnd_) return;
  ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void MetricsHud::reposition() {
  if (!hwnd_ || !owner_) return;
  RECT r{};
  GetWindowRect(owner_, &r);
  if (state_.mini_overlay) {
    const int gap = 8;
    const int owner_w = static_cast<int>(r.right - r.left);
    const int ww = (std::max)(200, owner_w - gap * 2);
    int hh = hud_mini_overlay_height();
    if (!state_.session_notice.empty()) {
      std::string t, d;
      session_notice_split(state_.session_notice, t, d);
      hh += d.empty() ? 33 : 60;
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, r.left + gap, r.top + gap, ww, hh,
                 SWP_NOACTIVATE | (visible_ ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    return;
  }
  const int ww = (std::max)(hud_panel_width(state_.advanced_metrics),
                            state_.session_notice.empty() ? 0 : 720);
  int hh = hud_panel_height(state_.advanced_metrics, state_.hw_info);
  if (!state_.session_notice.empty()) {
    std::string t, d;
    session_notice_split(state_.session_notice, t, d);
    hh += d.empty() ? 32 : 56;
  }
  SetWindowPos(hwnd_, HWND_TOPMOST, r.left + 24, r.top + 24, ww, hh,
               SWP_NOACTIVATE | (visible_ ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

void MetricsHud::update(const OverlayState& state) {
  const bool notice_changed = (state.session_notice != last_notice_);
  state_ = state;
  last_notice_ = state.session_notice;
  if (!hwnd_) return;
  // Live overlay owns the HUD surface — hide GDI twin / mini twin.
  if (state.live_overlay) {
    set_visible(false);
    return;
  }
  // Mini overlay is burned into Present. Hide GDI twin.
  if (state.mini_overlay) {
    set_visible(false);
    return;
  }
  set_visible(state.visible);
  if (!state.visible) return;
  apply_overlay_layered_attrs(hwnd_, state.overlay_no_bg);
  reposition();
  // Throttle GDI paint ~30 Hz — Invalidate+UpdateWindow every Present contended with D3D.
  const DWORD now = GetTickCount();
  if (!notice_changed && last_paint_tick_ != 0 && (now - last_paint_tick_) < 33) return;
  last_paint_tick_ = now;
  InvalidateRect(hwnd_, nullptr, FALSE);
  UpdateWindow(hwnd_);
}

void paint_panel_overlay_hdc(HDC hdc, const RECT& rc, const OverlayState& o);

void MetricsHud::paint(HDC hdc) const {
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  if (state_.mini_overlay) {
    if (overlay_draws_plate(state_.overlay_no_bg)) {
      HBRUSH bg = CreateSolidBrush(RGB(12, 14, 18));
      FillRect(hdc, &rc, bg);
      DeleteObject(bg);
    } else {
      FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    }
    SetBkMode(hdc, TRANSPARENT);
    paint_mini_overlay_hdc(hdc, rc, state_);
    return;
  }
  paint_panel_overlay_hdc(hdc, rc, state_);
}

void paint_panel_overlay_hdc(HDC hdc, const RECT& rc, const OverlayState& o) {
  if (overlay_draws_plate(o.overlay_no_bg)) {
    HBRUSH bg = CreateSolidBrush(RGB(12, 14, 18));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
  } else {
    FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
  }
  SetBkMode(hdc, TRANSPARENT);

  HFONT font = CreateFontW(26, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                           FIXED_PITCH | FF_MODERN, L"Consolas");
  HFONT old = static_cast<HFONT>(SelectObject(hdc, font));

  const wchar_t* mode = o.fake_capture ? L"FAKE" : L"LIVE";
  const bool exact = o.capture_w == o.output_w && o.capture_h == o.output_h && o.capture_w > 0;
  const double target_ms = o.target_fps > 0 ? 1000.0 / o.target_fps : (1000.0 / 60.0);

  // Compact HUD (Exibir metricas only): FPS + FG + res — no graph / last600.
  if (!o.advanced_metrics) {
    wchar_t lines[16][180];
    int n = 0;
    swprintf(lines[n++], 180, L"NexusCap  [%s]  ", mode);
    swprintf(lines[n++], 180, L"%dx%d @ %.0f", o.capture_w, o.capture_h, o.target_fps);
    {
      char fps_line[128]{};
      const double show_fps = o.fps_smooth > 0 ? o.fps_smooth : o.fps;
      format_session_hud_fps_line(fps_line, sizeof(fps_line), show_fps, o.fps_fg_est, o.fg_on, o.fg_multiplier,
                                  true);
      swprintf(lines[n++], 180, L"%s", utf8_to_wide_hud(fps_line).c_str());
    }
    if (fg_frametime_trace_count(o.fg_on, o.fg_multiplier) >= 2) {
      swprintf(lines[n++], 180, L"frametime  %.2f ms   FG %.2f   present %.2f", o.frame_ms,
               fg_frametime_trace_ms(o.frame_ms, o.fg_multiplier), o.present_ms);
    } else {
      swprintf(lines[n++], 180, L"frametime  %.2f ms   present %.2f", o.frame_ms, o.present_ms);
    }
    {
      char stack[96]{};
      format_dlss5_stack_hud_line(stack, sizeof(stack), o.renodx_on, o.feed_on, o.nr_pass_count);
      swprintf(lines[n++], 180, L"%s", utf8_to_wide_hud(stack).c_str());
    }
    swprintf(lines[n++], 180, L"%s  Present(%u)", o.sync_image ? L"SYNC VSYNC" : L"SYNC TEAR",
             o.sync_image ? 1u : 0u);
    const int hw_start = n;
    if (o.hw_info) n = append_hw_metric_lines(lines, n, 15, o);
    const int hotkey_i = n;
    swprintf(lines[n++], 180, L"F3 HUD  PgDn FG  Delete NR  Esc");
    int notice_i = -1;
    int notice_i2 = -1;
    if (!o.session_notice.empty() && n < 15) {
      std::string title, detail;
      session_notice_split(o.session_notice, title, detail);
      notice_i = n;
      swprintf(lines[n++], 180, L"%s", utf8_to_wide_hud(title.empty() ? o.session_notice : title).c_str());
      if (!detail.empty() && n < 16) {
        notice_i2 = n;
        swprintf(lines[n++], 180, L"  %s", utf8_to_wide_hud(detail).c_str());
      }
    }
    for (int i = 0; i < n; ++i) {
      COLORREF c = RGB(200, 200, 210);
      if (i == 0) c = RGB(255, 210, 70);
      else if (i == 1) c = RGB(120, 220, 255);
      else if (i == 2) c = RGB(120, 255, 160);
      else if (i == 4) c = o.sync_image ? RGB(120, 220, 255) : RGB(255, 160, 90);
      else if (o.hw_info && i >= hw_start && i < hotkey_i) c = RGB(140, 255, 180);
      else if (i == hotkey_i) c = RGB(150, 150, 160);
      else if (i == notice_i || i == notice_i2) c = RGB(255, 200, 120);
      const int y = 10 + i * 28;
      if (i == 0) {
        hud_draw_title_with_console(hdc, 14, y, lines[i], o);
        continue;
      }
      SetTextColor(hdc, c);
      TextOutW(hdc, 14, y, lines[i], static_cast<int>(wcslen(lines[i])));
    }
    const int gx = 14;
    const int gy = 10 + n * 28 + 6;
    const int gw = (std::max)(120, static_cast<int>(rc.right - rc.left) - 28);
    const int gh = (std::max)(72, static_cast<int>(rc.bottom - rc.top) - gy - 10);
    paint_frametime_graph(hdc, gx, gy, gw, gh, o, 2);
    SelectObject(hdc, old);
    DeleteObject(font);
    return;
  }

  // Advanced metrics: full panel + MSI-style frametime graph + last600.
  const wchar_t* an = o.analyze_on ? L"ON" : L"off";
  wchar_t lines[20][180];
  swprintf(lines[0], 180, L"NexusCap  [%s]  analyze=%s  ADV  ", mode, an);
  swprintf(lines[1], 180, L"device  %s", utf8_to_wide_hud(o.device).c_str());
  swprintf(lines[2], 180, L"signal  %dx%d @ %.0f Hz   output %dx%d%s", o.capture_w, o.capture_h,
           o.target_fps, o.output_w, o.output_h, exact ? L"  (1:1)" : L"  (scaled)");
  if (o.scale_percent > 0 && o.scale_percent < 100 && o.work_w > 0 && o.work_h > 0) {
    swprintf(lines[2], 180, L"signal %dx%d -> work %dx%d  (%d%%)   out %dx%d", o.capture_w, o.capture_h,
             o.work_w, o.work_h, o.scale_percent, o.output_w, o.output_h);
  } else if (o.work_w > 0 && o.work_h > 0) {
    swprintf(lines[2], 180, L"signal  %dx%d @ %.0f   work %dx%d (100%%)   out %dx%d", o.capture_w,
             o.capture_h, o.target_fps, o.work_w, o.work_h, o.output_w, o.output_h);
  }
  swprintf(lines[3], 180, L"FPS     %.1f  (smooth %.1f)", o.fps, o.fps_smooth);
  {
    char fps_line[128]{};
    format_session_hud_fps_line(fps_line, sizeof(fps_line), o.fps_smooth, o.fps_fg_est, o.fg_on, o.fg_multiplier,
                                false);
    swprintf(lines[3], 180, L"%s", utf8_to_wide_hud(fps_line).c_str());
  }
  swprintf(lines[4], 180, L"frametime  %.2f ms   avg %.2f  p95 %.2f  max %.2f  %s", o.frame_ms, o.ft_avg_ms,
           o.ft_p95_ms, o.ft_max_ms, o.ft_stable ? L"STABLE" : L"UNSTABLE");
  swprintf(lines[5], 180, L"latencia  cap %.2f ms   present %.2f ms%s   %s", o.e2e_ms, o.present_ms,
           (o.renodx_on || o.dlss_backend.find("renodx") != std::string::npos) ? L" (+NR/DLSS5)" : L"",
           o.sync_image ? L"VSYNC" : L"TEAR");
  if (o.renodx_on || o.dlss_backend.find("renodx") != std::string::npos) {
    char stack[96]{};
    format_dlss5_stack_hud_line(stack, sizeof(stack), o.renodx_on, o.feed_on, o.nr_pass_count);
    swprintf(lines[6], 180, L"%s  preset=%s  [%s]", utf8_to_wide_hud(stack).c_str(),
             utf8_to_wide_hud(o.renodx_preset).c_str(), utf8_to_wide_hud(o.dlss_status).c_str());
  } else {
    swprintf(lines[6], 180, L"DLSS    %s  mode=%s  %.2f ms  [%s] %s", o.dlss_on ? L"ON" : L"off",
             utf8_to_wide_hud(o.dlss_mode).c_str(), o.dlss_ms, utf8_to_wide_hud(o.dlss_backend).c_str(),
             utf8_to_wide_hud(o.dlss_status).c_str());
  }
  {
    char fg_line[160]{};
    format_session_hud_fg_status_line(fg_line, sizeof(fg_line), o.fg_on, o.fg_multiplier, o.fg_backend.c_str(),
                                      o.fg_flow_scale, o.fg_capture_api.c_str(), o.fg_status.c_str(), o.fps_smooth,
                                      o.fps_fg_est);
    swprintf(lines[7], 180, L"%s", utf8_to_wide_hud(fg_line).c_str());
  }

  int idx = 8;
  const int hw_start = idx;
  if (o.hw_info) idx = append_hw_metric_lines(lines, idx, 19, o);
  const int hw_end = idx;
  swprintf(lines[idx++], 180, L"frames  %llu   dropped %llu   stalls %llu   clusters %llu",
           static_cast<unsigned long long>(o.frames), static_cast<unsigned long long>(o.dropped),
           static_cast<unsigned long long>(o.stalls_window),
           static_cast<unsigned long long>(o.clusters));
  if (o.has_stats600) {
    swprintf(lines[idx++], 180, L"last600 fps %.2f  p95 e2e %.2f  p95 present %.2f  stalls %llu  max %.1f",
             o.s600_fps, o.s600_p95_e2e, o.s600_p95_present,
             static_cast<unsigned long long>(o.s600_stalls), o.s600_max_interval);
  } else {
    swprintf(lines[idx++], 180, L"last600 (aguardando ~10s de frames...)");
  }
  swprintf(lines[idx++], 180, L"F3 HUD  F4 analyze  PgDn FG  Delete NR  Home RenoDX  Esc");
  int notice_i = -1;
  int notice_i2 = -1;
  if (!o.session_notice.empty() && idx < 18) {
    std::string title, detail;
    session_notice_split(o.session_notice, title, detail);
    notice_i = idx;
    swprintf(lines[idx++], 180, L"%s", utf8_to_wide_hud(title.empty() ? o.session_notice : title).c_str());
    if (!detail.empty() && idx < 20) {
      notice_i2 = idx;
      swprintf(lines[idx++], 180, L"  %s", utf8_to_wide_hud(detail).c_str());
    }
  }
  const int nlines = idx;

  auto color_for_adv = [&](int i) -> COLORREF {
    if (i == 0) return RGB(255, 210, 70);
    if (i == 1) return RGB(200, 200, 210);
    if (i == 2) return RGB(120, 220, 255);
    if (i == 3) return RGB(120, 255, 160);
    if (i == 4) return o.ft_stable ? RGB(120, 255, 160) : RGB(255, 120, 100);
    if (i == 5) return RGB(120, 255, 160);
    if (i == 6) return RGB(180, 160, 255);
    if (i == 7) return o.fg_on ? RGB(120, 255, 200) : RGB(160, 170, 180);
    if (o.hw_info && i >= hw_start && i < hw_end) return RGB(140, 255, 180);
    if (i == notice_i || i == notice_i2) return RGB(255, 200, 120);
    if (notice_i >= 0 && i == notice_i - 1) return RGB(150, 150, 160);
    if (i == nlines - 1 && notice_i < 0) return RGB(150, 150, 160);
    if (i == nlines - 2 && notice_i < 0) return RGB(255, 140, 140);
    return RGB(255, 180, 120);
  };

  for (int i = 0; i < 5; ++i) {
    const int y = 10 + i * 28;
    if (i == 0) {
      hud_draw_title_with_console(hdc, 14, y, lines[i], o);
      continue;
    }
    SetTextColor(hdc, color_for_adv(i));
    TextOutW(hdc, 14, y, lines[i], static_cast<int>(wcslen(lines[i])));
  }

  // MSI-style frametime line graph (spikes = hitch) + uma linha FG por xN.
  const int gx = 14;
  const int gy = 10 + 5 * 28 + 6;
  const int gw = (std::max)(120, static_cast<int>(rc.right - rc.left) - 28);
  const int gh = 110;
  paint_frametime_graph(hdc, gx, gy, gw, gh, o, 2);

  double ceil_ms = (std::max)(target_ms * 2.5, 40.0);
  if (!o.ft_hist.empty()) {
    for (double v : o.ft_hist) ceil_ms = (std::max)(ceil_ms, v * 1.05);
  }
  ceil_ms = (std::min)(ceil_ms, 80.0);
  wchar_t scale[64];
  swprintf(scale, 64, L"0 - %.0f ms  (tgt %.2f)", ceil_ms, target_ms);
  SetTextColor(hdc, RGB(110, 120, 140));
  TextOutW(hdc, gx + 6, gy + gh - 18, scale, static_cast<int>(wcslen(scale)));

  const int text_y0 = gy + gh + 10;
  for (int i = 5; i < nlines; ++i) {
    SetTextColor(hdc, color_for_adv(i));
    TextOutW(hdc, 14, text_y0 + (i - 5) * 28, lines[i], static_cast<int>(wcslen(lines[i])));
  }

  SelectObject(hdc, old);
  DeleteObject(font);
}

LRESULT CALLBACK MetricsHud::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return TRUE;
  }
  auto* self = reinterpret_cast<MetricsHud*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_PAINT && self) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    self->paint(hdc);
    EndPaint(hwnd, &ps);
    return 0;
  }
  if (msg == WM_NCHITTEST) return HTTRANSPARENT;  // click-through
  if (msg == WM_ERASEBKGND) return 1;
  return DefWindowProcW(hwnd, msg, wp, lp);
}

namespace {
constexpr wchar_t kRestartNoticeClass[] = L"NexusCapRestartNotice";
constexpr int kRestartNoticeW = 900;
constexpr int kRestartNoticeH = 150;
}  // namespace

bool RestartNotice::show(HINSTANCE hi, const std::string& title, const std::string& detail) {
  title_ = utf8_to_wide_hud(title);
  detail_ = utf8_to_wide_hud(detail);
  if (hwnd_) {
    InvalidateRect(hwnd_, nullptr, FALSE);
    pump();
    return true;
  }
  WNDCLASSW wc{};
  wc.lpfnWndProc = RestartNotice::WndProc;
  wc.hInstance = hi;
  wc.lpszClassName = kRestartNoticeClass;
  wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&wc);

  const int sw = GetSystemMetrics(SM_CXSCREEN);
  const int sh = GetSystemMetrics(SM_CYSCREEN);
  const int x = (std::max)(0, (sw - kRestartNoticeW) / 2);
  const int y = (std::max)(0, (sh - kRestartNoticeH) / 2);
  // Sem owner: a janela de captura pode morrer que este aviso continua na tela.
  hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kRestartNoticeClass,
                          L"NexusCap", WS_POPUP, x, y, kRestartNoticeW, kRestartNoticeH, nullptr, nullptr, hi,
                          this);
  if (!hwnd_) return false;
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  pump();
  return true;
}

void RestartNotice::destroy() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void RestartNotice::pump() const {
  if (!hwnd_) return;
  SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  UpdateWindow(hwnd_);
  MSG msg{};
  while (PeekMessageW(&msg, hwnd_, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

void RestartNotice::paint(HDC hdc) const {
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  HBRUSH bg = CreateSolidBrush(RGB(14, 18, 26));
  FillRect(hdc, &rc, bg);
  DeleteObject(bg);
  HPEN edge = CreatePen(PS_SOLID, 2, RGB(240, 180, 65));
  HGDIOBJ old_pen = SelectObject(hdc, edge);
  HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
  Rectangle(hdc, 0, 0, rc.right, rc.bottom);
  SelectObject(hdc, old_pen);
  SelectObject(hdc, old_brush);
  DeleteObject(edge);

  SetBkMode(hdc, TRANSPARENT);
  HFONT big = CreateFontW(34, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  HFONT old = static_cast<HFONT>(SelectObject(hdc, big));
  SetTextColor(hdc, RGB(255, 210, 110));
  TextOutW(hdc, 28, 30, title_.c_str(), static_cast<int>(title_.size()));
  SelectObject(hdc, old);
  DeleteObject(big);

  HFONT small_font = CreateFontW(22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  old = static_cast<HFONT>(SelectObject(hdc, small_font));
  SetTextColor(hdc, RGB(205, 225, 250));
  TextOutW(hdc, 28, 80, detail_.c_str(), static_cast<int>(detail_.size()));
  SelectObject(hdc, old);
  DeleteObject(small_font);
}

LRESULT CALLBACK RestartNotice::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return TRUE;
  }
  auto* self = reinterpret_cast<RestartNotice*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_PAINT && self) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    self->paint(hdc);
    EndPaint(hwnd, &ps);
    return 0;
  }
  if (msg == WM_CLOSE) return 0;  // nunca fecha por clique: o relaunch decide
  return DefWindowProcW(hwnd, msg, wp, lp);
}

namespace {
constexpr wchar_t kHwInfoClass[] = L"NexusCapHwInfo";
}  // namespace

bool HwInfoPanel::create(HWND owner, HINSTANCE hi) {
  destroy();
  owner_ = owner;
  WNDCLASSW wc{};
  wc.lpfnWndProc = HwInfoPanel::WndProc;
  wc.hInstance = hi;
  wc.lpszClassName = kHwInfoClass;
  wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&wc);

  hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kHwInfoClass,
                          L"NexusCap HW info", WS_POPUP, 0, 0, hw_info_panel_width(), hw_info_panel_height(),
                          owner, nullptr, hi, this);
  if (!hwnd_) return false;
  SetLayeredWindowAttributes(hwnd_, 0, 220, LWA_ALPHA);
  visible_ = false;
  return true;
}

void HwInfoPanel::destroy() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  owner_ = nullptr;
  visible_ = false;
}

void HwInfoPanel::set_visible(bool visible) {
  visible_ = visible;
  if (!hwnd_) return;
  if (visible) reposition();
  ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void HwInfoPanel::reposition() {
  if (!hwnd_ || !owner_) return;
  RECT r{};
  GetWindowRect(owner_, &r);
  const int ww = hw_info_panel_width();
  const int hh = hw_info_panel_height();
  // Below the metrics HUD (top-left +24,+24,+h) → sit under it.
  SetWindowPos(hwnd_, HWND_TOPMOST, r.left + 24, r.top + 24 + hud_panel_height(state_.advanced_metrics, state_.hw_info) + 8,
               ww, hh, SWP_NOACTIVATE | (visible_ ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

void HwInfoPanel::update(const OverlayState& state) {
  state_ = state;
  if (!hwnd_) return;
  set_visible(state.hw_info);
  if (!state.hw_info) return;
  apply_overlay_layered_attrs(hwnd_, state.overlay_no_bg);
  reposition();
  InvalidateRect(hwnd_, nullptr, FALSE);
  UpdateWindow(hwnd_);
}

void HwInfoPanel::paint(HDC hdc) const {
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  if (overlay_draws_plate(state_.overlay_no_bg)) {
    HBRUSH bg = CreateSolidBrush(RGB(14, 18, 24));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
  } else {
    FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
  }
  HPEN edge = CreatePen(PS_SOLID, 1, RGB(80, 180, 120));
  HGDIOBJ old_pen = SelectObject(hdc, edge);
  Rectangle(hdc, 0, 0, rc.right, rc.bottom);
  SelectObject(hdc, old_pen);
  DeleteObject(edge);

  SetBkMode(hdc, TRANSPARENT);
  HFONT font = CreateFontW(20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                           FIXED_PITCH | FF_MODERN, L"Consolas");
  HFONT old = static_cast<HFONT>(SelectObject(hdc, font));

  const auto& o = state_;
  char hw[kSysMetricsLineCount][64]{};
  const int hn = format_sys_metrics_lines(hw, kSysMetricsLineCount, metrics_from_overlay(o));
  const COLORREF cols[5] = {RGB(255, 230, 90), RGB(160, 240, 255), RGB(120, 255, 180), RGB(255, 255, 255),
                            RGB(140, 255, 220)};
  for (int i = 0; i < hn; ++i) {
    SetTextColor(hdc, cols[i % 5]);
    const std::wstring w = utf8_to_wide_hud(hw[i]);
    TextOutW(hdc, 12, 10 + i * 28, w.c_str(), static_cast<int>(w.size()));
  }

  SelectObject(hdc, old);
  DeleteObject(font);
}

LRESULT CALLBACK HwInfoPanel::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return TRUE;
  }
  auto* self = reinterpret_cast<HwInfoPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_PAINT && self) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    self->paint(hdc);
    EndPaint(hwnd, &ps);
    return 0;
  }
  if (msg == WM_NCHITTEST) return HTTRANSPARENT;
  if (msg == WM_ERASEBKGND) return 1;
  return DefWindowProcW(hwnd, msg, wp, lp);
}

namespace {
DWORD session_window_style(bool fullscreen, bool locked) {
  if (fullscreen) return WS_POPUP;
  if (locked) return WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  return WS_OVERLAPPEDWINDOW;
}
}  // namespace

void show_session_window(HWND hwnd, bool maximize) {
  if (!hwnd || !IsWindow(hwnd)) return;
  ShowWindow(hwnd, maximize ? SW_SHOWMAXIMIZED : SW_SHOW);
  SetForegroundWindow(hwnd);
  BringWindowToTop(hwnd);
}

void hide_session_window(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) return;
  ShowWindow(hwnd, SW_HIDE);
}

HWND create_app_window(HINSTANCE hi, int w, int h, bool fullscreen, const wchar_t* title, bool start_hidden) {
  WNDCLASSW wc{};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hi;
  wc.lpszClassName = L"NexusCapWnd";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(kAppIconResourceId));
  if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  RegisterClassW(&wc);

  const int sw = GetSystemMetrics(SM_CXSCREEN);
  const int sh = GetSystemMetrics(SM_CYSCREEN);

  DWORD style = session_window_style(fullscreen, false);
  if (!fullscreen) style |= WS_MAXIMIZE;
  int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
  int ww = w, hh = h;
  if (fullscreen) {
    // Always cover the monitor. Sizing to signal (e.g. 1440p on 1080p) crops/zooms.
    const auto g = fullscreen_window_geom(sw, sh);
    x = g.x;
    y = g.y;
    ww = g.w;
    hh = g.h;
  } else {
    // Seed a reasonable size; ShowWindow(MAXIMIZED) covers the work area with chrome.
    const auto g = windowed_client_geom(w, h, sw, sh);
    RECT r{0, 0, g.w, g.h};
    AdjustWindowRect(&r, style, FALSE);
    ww = r.right - r.left;
    hh = r.bottom - r.top;
    x = (std::max)(0, (sw - ww) / 2);
    y = (std::max)(0, (sh - hh) / 2);
  }
  HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title, style | (start_hidden ? 0 : WS_VISIBLE), x, y, ww, hh,
                              nullptr, nullptr, hi, nullptr);
  if (hwnd) {
    HICON icon = LoadIconW(hi, MAKEINTRESOURCEW(kAppIconResourceId));
    if (icon) {
      SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
      SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    }
  }
  if (hwnd && !fullscreen) {
    if (start_hidden) {
      MONITORINFO mi{sizeof(mi)};
      HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
      if (GetMonitorInfoW(mon, &mi)) {
        SetWindowPos(hwnd, nullptr, mi.rcWork.left, mi.rcWork.top,
                     mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
      }
      ShowWindow(hwnd, SW_HIDE);
    } else {
      ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    }
  }
  return hwnd;
}

namespace {
constexpr wchar_t kLiveOverlayClass[] = L"NexusCapLiveOverlay";
// ~40% of the previous strip; width is measured from content (tight to top-right).
constexpr int kLiveH96 = 26;
constexpr int kLiveFont96 = 16;
constexpr int kLiveSpark96 = 104;
constexpr int kLiveGap96 = 12;
constexpr int kLivePad96 = 8;
constexpr int kLiveMargin96 = 10;

int live_mul_dpi(int v, UINT dpi) { return MulDiv(v, static_cast<int>(dpi), 96); }

void live_fix_dib_alpha(void* bits, int w, int h) {
  // GDI writes BGR with A=0; ULW_ALPHA needs A=255 on ink.
  // Empty pixels get A=1 so the whole strip receives mouse clicks (menu).
  auto* p = static_cast<unsigned char*>(bits);
  const int n = w * h;
  for (int i = 0; i < n; ++i) {
    const unsigned char b = p[0], g = p[1], r = p[2];
    if (b | g | r) {
      p[3] = 255;
    } else {
      p[0] = p[1] = p[2] = 1;
      p[3] = 1;
    }
    p += 4;
  }
}
}  // namespace

UINT LiveOverlay::dpi() const {
  HWND ref = hwnd_ ? hwnd_ : owner_;
  if (!ref) return 96;
  using Fn = UINT(WINAPI*)(HWND);
  static Fn get_dpi = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
  if (get_dpi) {
    const UINT d = get_dpi(ref);
    if (d >= 96) return d;
  }
  return 96;
}

bool LiveOverlay::create(HWND owner, HINSTANCE hi) {
  destroy();
  owner_ = owner;
  WNDCLASSW wc{};
  wc.lpfnWndProc = LiveOverlay::WndProc;
  wc.hInstance = hi;
  wc.lpszClassName = kLiveOverlayClass;
  wc.hbrBackground = nullptr;
  wc.hCursor = LoadCursor(nullptr, IDC_HAND);
  RegisterClassW(&wc);

  const UINT d = dpi();
  layout_w_ = live_mul_dpi(420, d);  // seed; present() tightens to content
  layout_h_ = live_mul_dpi(kLiveH96, d);

  hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, kLiveOverlayClass, L"NexusCap live",
                          WS_POPUP, 0, 0, layout_w_, layout_h_, owner, nullptr, hi, this);
  if (!hwnd_) return false;
  return true;
}

void LiveOverlay::destroy() {
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  owner_ = nullptr;
}

void LiveOverlay::set_visible(bool visible) {
  visible_ = visible;
  if (!hwnd_) return;
  ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void LiveOverlay::set_console(int live_console) {
  console_ = static_cast<int>(clamp_live_console(live_console));
}

bool LiveOverlay::take_console_changed() {
  const bool v = console_changed_;
  console_changed_ = false;
  return v;
}

void LiveOverlay::reposition() {
  if (!hwnd_ || !owner_ || !visible_) return;
  present();
}

void LiveOverlay::update(const OverlayState& state) {
  state_ = state;
  if (state.live_console != console_) {
    console_ = static_cast<int>(clamp_live_console(state.live_console));
  }
  if (!hwnd_) return;
  if (!state.live_overlay) {
    set_visible(false);
    return;
  }
  set_visible(state.visible);
  if (!state.visible) return;
  const DWORD now = GetTickCount();
  if (last_paint_tick_ != 0 && (now - last_paint_tick_) < 33) return;
  last_paint_tick_ = now;
  present();
}

void LiveOverlay::show_console_menu() {
  if (!hwnd_ || !state_.overlay_console) return;
  POINT pt{};
  GetCursorPos(&pt);
  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  for (LiveConsole o : kLiveConsoleOrder) {
    const int id = static_cast<int>(o) + 1;
    const UINT flags = MF_STRING | (console_ == static_cast<int>(o) ? MF_CHECKED : 0);
    const std::wstring label = utf8_to_wide_hud(live_console_label(o));
    AppendMenuW(menu, flags, static_cast<UINT_PTR>(id), label.c_str());
  }
  const int cmd =
      TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
  DestroyMenu(menu);
  if (cmd >= 1 && cmd <= kLiveConsoleCount) {
    const int next = static_cast<int>(clamp_live_console(cmd - 1));
    if (next != console_) {
      console_ = next;
      console_changed_ = true;
      present();
    }
  }
}

void LiveOverlay::present() const {
  if (!hwnd_ || !visible_ || !owner_) return;
  const UINT d = dpi();
  const int hh = live_mul_dpi(kLiveH96, d);
  const int font_px = live_mul_dpi(kLiveFont96, d);
  const int spark_w = live_mul_dpi(kLiveSpark96, d);
  const int gap = live_mul_dpi(kLiveGap96, d);
  const int pad_x = live_mul_dpi(kLivePad96, d);
  const int margin = live_mul_dpi(kLiveMargin96, d);

  HDC screen = GetDC(nullptr);
  HDC measure = CreateCompatibleDC(screen);
  HFONT font = CreateFontW(font_px, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                           CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
  HFONT old_m = static_cast<HFONT>(SelectObject(measure, font));

  const LiveConsole cons = clamp_live_console(console_);
  std::wstring cons_w;
  if (state_.overlay_console) {
    char chip[48]{};
    format_overlay_console_chip(chip, sizeof(chip), cons);
    cons_w = utf8_to_wide_hud(chip);
  }
  char dlss_buf[64]{};
  format_live_dlss5_line(dlss_buf, sizeof(dlss_buf), state_.renodx_on || state_.dlss_on, state_.nr_pass_count,
                         state_.feed_on);
  const std::wstring dlss_w = utf8_to_wide_hud(dlss_buf);
  const double show_fps = state_.fps_smooth > 0 ? state_.fps_smooth : state_.fps;
  char fps_part[64]{};
  format_live_fps_line(fps_part, sizeof(fps_part), show_fps, state_.fps_fg_est, state_.fg_on, state_.fg_multiplier);
  char fps_line[80]{};
  std::snprintf(fps_line, sizeof(fps_line), "FPS  %s", fps_part);
  const std::wstring fps_w = utf8_to_wide_hud(fps_line);

  SIZE sz_cons{}, sz_dlss{}, sz_fps{};
  if (!cons_w.empty()) {
    GetTextExtentPoint32W(measure, cons_w.c_str(), static_cast<int>(cons_w.size()), &sz_cons);
  }
  GetTextExtentPoint32W(measure, dlss_w.c_str(), static_cast<int>(dlss_w.size()), &sz_dlss);
  GetTextExtentPoint32W(measure, fps_w.c_str(), static_cast<int>(fps_w.size()), &sz_fps);
  SelectObject(measure, old_m);
  DeleteDC(measure);

  const int ww = pad_x + sz_cons.cx + (sz_cons.cx > 0 ? gap : 0) + sz_dlss.cx + gap + sz_fps.cx + gap + spark_w +
                 pad_x;
  layout_w_ = ww;
  layout_h_ = hh;

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = ww;
  bmi.bmiHeader.biHeight = -hh;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits) {
    if (dib) DeleteObject(dib);
    DeleteObject(font);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return;
  }
  HGDIOBJ old_bmp = SelectObject(mem, dib);
  std::memset(bits, 0, static_cast<size_t>(ww) * static_cast<size_t>(hh) * 4);
  if (overlay_draws_plate(state_.overlay_no_bg)) {
    RECT plate{0, 0, ww, hh};
    HBRUSH bg = CreateSolidBrush(RGB(12, 14, 18));
    FillRect(mem, &plate, bg);
    DeleteObject(bg);
  }

  SetBkMode(mem, TRANSPARENT);
  HFONT old_font = static_cast<HFONT>(SelectObject(mem, font));
  const int text_y = (hh - font_px) / 2;
  int x = pad_x;

  auto draw_seg = [&](const std::wstring& text, COLORREF color) {
    SIZE sz{};
    GetTextExtentPoint32W(mem, text.c_str(), static_cast<int>(text.size()), &sz);
    SetTextColor(mem, color);
    TextOutW(mem, x, text_y, text.c_str(), static_cast<int>(text.size()));
    x += sz.cx + gap;
  };

  if (!cons_w.empty()) draw_seg(cons_w, live_console_color(cons));
  draw_seg(dlss_w, RGB(240, 242, 248));
  draw_seg(fps_w, RGB(120, 220, 255));

  {
    const int gx = x;
    const int gw = spark_w;
    const int gy = live_mul_dpi(4, d);
    const int gh = hh - gy * 2;
    paint_frametime_traces(mem, gx, gy, gw, gh, state_, (std::max)(1, live_mul_dpi(1, d)));
  }

  SelectObject(mem, old_font);
  DeleteObject(font);
  live_fix_dib_alpha(bits, ww, hh);

  // Owner top-right — not previous hwnd rect (wide strip looked centered).
  RECT orc{};
  GetWindowRect(owner_, &orc);
  POINT pt{};
  pt.x = orc.right - margin - ww;
  pt.y = orc.top + margin;
  SIZE sz{ww, hh};
  POINT src{0, 0};
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  UpdateLayeredWindow(hwnd_, screen, &pt, &sz, mem, &src, 0, &blend, ULW_ALPHA);

  SelectObject(mem, old_bmp);
  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK LiveOverlay::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return TRUE;
  }
  auto* self = reinterpret_cast<LiveOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_PAINT && self) {
    PAINTSTRUCT ps;
    BeginPaint(hwnd, &ps);
    EndPaint(hwnd, &ps);
    self->present();
    return 0;
  }
  if (msg == WM_ERASEBKGND) return 1;
  if (msg == WM_DPICHANGED && self) {
    self->present();
    return 0;
  }
  if ((msg == WM_LBUTTONUP || msg == WM_RBUTTONUP) && self) {
    self->show_console_menu();
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

bool apply_display_mode(HWND hwnd, RendererD3D11& renderer, bool fullscreen, int signal_w, int signal_h,
                        int* out_client_w, int* out_client_h) {
  if (!hwnd) return false;
  const int sw = GetSystemMetrics(SM_CXSCREEN);
  const int sh = GetSystemMetrics(SM_CYSCREEN);
  (void)signal_w;
  (void)signal_h;

  LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
  if (fullscreen) {
    style &= ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW);
    style |= WS_POPUP;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    const auto g = fullscreen_window_geom(sw, sh);
    SetWindowPos(hwnd, HWND_TOP, g.x, g.y, g.w, g.h, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  } else {
    style &= ~static_cast<LONG_PTR>(WS_POPUP);
    style |= WS_OVERLAPPEDWINDOW;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
    ShowWindow(hwnd, SW_SHOWMAXIMIZED);  // windowed covering the desktop
  }

  RECT client{};
  GetClientRect(hwnd, &client);
  const int cw = (std::max)(1, static_cast<int>(client.right - client.left));
  const int ch = (std::max)(1, static_cast<int>(client.bottom - client.top));
  renderer.resize(cw, ch);
  if (out_client_w) *out_client_w = cw;
  if (out_client_h) *out_client_h = ch;
  return true;
}

void set_thread_priorities() {
  SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  DWORD idx = 0;
  HANDLE h = AvSetMmThreadCharacteristicsW(L"Games", &idx);
  if (h) AvSetMmThreadPriority(h, AVRT_PRIORITY_HIGH);
}

}  // namespace nexuscap
