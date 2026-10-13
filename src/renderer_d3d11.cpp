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
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cstdint>
#include <string>
#include <vector>

#include <avrt.h>
#include <dwmapi.h>
#include <mmsystem.h>

#ifndef DWMWA_CLOAK
#define DWMWA_CLOAK 13
#endif

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

// Cap em baixo, FG em cima (FPS maior no topo).
int frametime_y(int gy, int gh, double ms, double ceil_ms) {
  return fg_frametime_plot_y(gy, gh, ms, 1, ceil_ms);
}

int frametime_y_trace(int gy, int gh, double cap_ms, int k, double ceil_ms) {
  const int y = fg_frametime_plot_y(gy, gh, cap_ms, k, ceil_ms);
  const int top = gy + 2;
  const int bot = gy + gh - 2;
  return y < top ? top : (y > bot ? bot : y);
}

struct CachedHudFont {
  HFONT handle = nullptr;
  int px = -1;
  int weight = 0;
  DWORD quality = 0;
  BYTE pitch = 0;
  wchar_t face[32]{};

  HFONT get(int font_px, int weight_, DWORD quality_, BYTE pitch_, const wchar_t* name) {
    if (!name) name = L"Segoe UI";
    if (handle && px == font_px && weight == weight_ && quality == quality_ && pitch == pitch_ &&
        wcscmp(face, name) == 0) {
      return handle;
    }
    if (handle) {
      DeleteObject(handle);
      handle = nullptr;
    }
    handle = CreateFontW(font_px, 0, 0, 0, weight_, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                         CLIP_DEFAULT_PRECIS, quality_, pitch_, name);
    px = font_px;
    weight = weight_;
    quality = quality_;
    pitch = pitch_;
    size_t i = 0;
    for (; i < 31 && name[i]; ++i) face[i] = name[i];
    face[i] = 0;
    return handle;
  }

  void clear() {
    if (handle) {
      DeleteObject(handle);
      handle = nullptr;
    }
    px = -1;
    face[0] = 0;
  }
};

struct HudFontBank {
  CachedHudFont live;
  CachedHudFont mini;
  CachedHudFont panel;
  CachedHudFont graph;
  CachedHudFont notice;
  CachedHudFont notice_live;
  CachedHudFont hold_big;
  CachedHudFont hold_small;
  void clear() {
    live.clear();
    mini.clear();
    panel.clear();
    graph.clear();
    notice.clear();
    notice_live.clear();
    hold_big.clear();
    hold_small.clear();
  }
};

HudFontBank g_hud_fonts;

void paint_frametime_traces(HDC hdc, int gx, int gy, int gw, int gh, const OverlayState& o, int pen_w,
                           int left_pad = 0) {
  if (!hdc || o.ft_hist.size() < 2 || gw < 8) return;
  const double target_ms = o.target_fps > 0 ? 1000.0 / o.target_fps : (1000.0 / 60.0);
  double ceil_ms = (std::max)(target_ms * 2.5, 40.0);
  for (double v : o.ft_hist) ceil_ms = (std::max)(ceil_ms, v * 1.05);
  ceil_ms = (std::min)(ceil_ms, 80.0);
  const size_t hn = o.ft_hist.size();
  const int traces = fg_frametime_trace_count(o.fg_on, o.fg_multiplier);
  const int base_w = (std::max)(1, pen_w);
  const int px = gx + (left_pad > 0 ? left_pad : 0);
  const int pw = gw - (left_pad > 0 ? left_pad : 0);
  if (pw < 8) return;
  for (int i = 1; i <= traces; ++i) {
    const int k = fg_frametime_trace_k(i, o.fg_multiplier);
    const FgFtRgb rgb = fg_frametime_trace_rgb(k, o.ft_stable);
    const int w = k >= 2 ? (std::max)(2, base_w + 1) : base_w;
    HPEN pen = CreatePen(PS_SOLID, w, RGB(rgb.r, rgb.g, rgb.b));
    HGDIOBJ old = SelectObject(hdc, pen);
    for (size_t s = 0; s < hn; ++s) {
      const int x = px + 2 + static_cast<int>((pw - 4) * static_cast<int>(s) / static_cast<int>(hn - 1));
      const int y = frametime_y_trace(gy, gh, o.ft_hist[s], k, ceil_ms);
      if (s == 0)
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
  const int gutter = fg_frametime_fps_gutter_px();
  HPEN target_pen = CreatePen(PS_DOT, 1, RGB(70, 160, 90));
  SelectObject(hdc, target_pen);
  const int y_tgt = y_for(target_ms);
  MoveToEx(hdc, gx + gutter, y_tgt, nullptr);
  LineTo(hdc, gx + gw - 1, y_tgt);
  DeleteObject(target_pen);

  paint_frametime_traces(hdc, gx, gy, gw, gh, o, pen_w, gutter);

  const int traces = fg_frametime_trace_count(o.fg_on, o.fg_multiplier);
  HFONT lf = g_hud_fonts.graph.get(16, FW_SEMIBOLD, NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS,
                                   L"Segoe UI");
  HGDIOBJ oldf = SelectObject(hdc, lf);
  SetBkMode(hdc, TRANSPARENT);
  const double last_ms = !o.ft_hist.empty() ? o.ft_hist.back() : o.frame_ms;
  for (int i = 1; i <= traces; ++i) {
    const int k = fg_frametime_trace_k(i, o.fg_multiplier);
    const FgFtRgb rgb = fg_frametime_trace_rgb(k, o.ft_stable);
    wchar_t fpsw[16]{};
    swprintf(fpsw, 16, L"%d", fg_frametime_fps(last_ms, k));
    const int ly = frametime_y_trace(gy, gh, last_ms, k, ceil_ms) - 8;
    SetTextColor(hdc, RGB(rgb.r, rgb.g, rgb.b));
    TextOutW(hdc, gx + 3, ly, fpsw, static_cast<int>(wcslen(fpsw)));
  }
  if (traces >= 2) {
    const wchar_t* names[4] = {L"cap", L"x2", L"x3", L"x4"};
    int x = gx + gutter + 6;
    const int ly = gy + 3;
    for (int i = 1; i <= traces && i <= 2; ++i) {
      const int k = fg_frametime_trace_k(i, o.fg_multiplier);
      const int name_i = k <= 1 ? 0 : (k > 4 ? 3 : k - 1);
      const FgFtRgb rgb = fg_frametime_trace_rgb(k, o.ft_stable);
      RECT sw{x, ly + 2, x + 9, ly + 12};
      HBRUSH br = CreateSolidBrush(RGB(rgb.r, rgb.g, rgb.b));
      FillRect(hdc, &sw, br);
      DeleteObject(br);
      SetTextColor(hdc, RGB(rgb.r, rgb.g, rgb.b));
      TextOutW(hdc, x + 11, ly, names[name_i], static_cast<int>(wcslen(names[name_i])));
      x += 36;
    }
  }
  SelectObject(hdc, oldf);

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
  int live_blit_x = 0;
  int live_blit_y = 0;
  int live_blit_w = 0;
  int live_blit_h = 0;
  ComPtr<ID3D11Texture2D> toastTex;
  ComPtr<ID3D11ShaderResourceView> toastSrv;
  int toast_w = 0;
  int toast_h = 0;
  DWORD toast_last_tick = 0;
  ComPtr<ID3D11Texture2D> noticeTex;
  ComPtr<ID3D11Texture2D> lastPresented;
  ComPtr<ID3D11ShaderResourceView> noticeSrv;
  int notice_w = 0;
  int notice_h = 0;
  DWORD notice_last_tick = 0;
  // Cache enhance output SRV to avoid creating it per frame
  ID3D11Texture2D* last_enhance_tex = nullptr;
  ComPtr<ID3D11ShaderResourceView> last_enhance_srv;
  int color_space = static_cast<int>(kDefaultColorSpace);
  int color_range = static_cast<int>(kDefaultColorRange);
  bool yuv_dirty = true;
};

RendererD3D11::~RendererD3D11() { shutdown(); }

void RendererD3D11::shutdown() {
  g_hud_fonts.clear();
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
  silence_reshade_home_splash();
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
  // Mesmo Present do app que segura o LSFG: flip waitable, sem tearing.
  sc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
  hr = factory->CreateSwapChainForHwnd(dev.Get(), hwnd, &sc, nullptr, nullptr, &impl_->swap);
  if (FAILED(hr)) {
    sc.BufferCount = 1;
    sc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  sc.Flags = 0;
  hr = factory->CreateSwapChainForHwnd(dev.Get(), hwnd, &sc, nullptr, nullptr, &impl_->swap);
  }
  if (FAILED(hr)) {
    err = "CreateSwapChainForHwnd failed";
    return false;
  }
  swap_flags_ = sc.Flags;
  swap_waitable_ = (swap_flags_ & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0;
  if (swap_waitable_) {
    ComPtr<IDXGISwapChain2> sc2;
    if (SUCCEEDED(impl_->swap.As(&sc2)) && sc2) sc2->SetMaximumFrameLatency(1);
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

HANDLE RendererD3D11::frame_latency_waitable_object() const {
  if (!impl_ || !impl_->swap || !swap_waitable_) return nullptr;
  ComPtr<IDXGISwapChain2> sc2;
  if (FAILED(impl_->swap.As(&sc2)) || !sc2) return nullptr;
  return sc2->GetFrameLatencyWaitableObject();
}

void RendererD3D11::resize(int width, int height) {
  if (!impl_ || !impl_->swap) return;
  back_w_ = width;
  back_h_ = height;
  impl_->ctx->OMSetRenderTargets(0, nullptr, nullptr);
  impl_->rtv.Reset();
  impl_->swap->ResizeBuffers(0, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, swap_flags_);
  ComPtr<ID3D11Texture2D> back;
  impl_->swap->GetBuffer(0, IID_PPV_ARGS(&back));
  impl_->device->CreateRenderTargetView(back.Get(), nullptr, &impl_->rtv);
}

bool RendererD3D11::recycle_swapchain() {
  if (!impl_ || !impl_->device || !impl_->ctx || !hwnd_ || back_w_ < 1 || back_h_ < 1) return false;
  impl_->ctx->OMSetRenderTargets(0, nullptr, nullptr);
  impl_->ctx->Flush();
  impl_->rtv.Reset();
  impl_->swap.Reset();

  ComPtr<IDXGIDevice> dxgi_dev;
  impl_->device.As(&dxgi_dev);
  if (!dxgi_dev) return false;
  ComPtr<IDXGIAdapter> adapter;
  dxgi_dev->GetAdapter(&adapter);
  ComPtr<IDXGIFactory2> factory;
  if (!adapter || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))) || !factory) return false;

  DXGI_SWAP_CHAIN_DESC1 sc{};
  sc.Width = static_cast<UINT>(back_w_);
  sc.Height = static_cast<UINT>(back_h_);
  sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sc.SampleDesc.Count = 1;
  sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sc.BufferCount = 2;
  sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sc.Scaling = DXGI_SCALING_STRETCH;
  sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  sc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
  HRESULT hr = factory->CreateSwapChainForHwnd(impl_->device.Get(), hwnd_, &sc, nullptr, nullptr, &impl_->swap);
  if (FAILED(hr)) {
    sc.BufferCount = 1;
    sc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sc.Flags = 0;
    hr = factory->CreateSwapChainForHwnd(impl_->device.Get(), hwnd_, &sc, nullptr, nullptr, &impl_->swap);
  }
  if (FAILED(hr) || !impl_->swap) return false;
  swap_flags_ = sc.Flags;
  swap_waitable_ = (swap_flags_ & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0;
  if (swap_waitable_) {
    ComPtr<IDXGISwapChain2> sc2;
    if (SUCCEEDED(impl_->swap.As(&sc2)) && sc2) sc2->SetMaximumFrameLatency(1);
  }
  factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
  ComPtr<ID3D11Texture2D> back;
  if (FAILED(impl_->swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
  if (FAILED(impl_->device->CreateRenderTargetView(back.Get(), nullptr, &impl_->rtv))) return false;
  return true;
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
    // Cache the enhance output SRV to avoid creating it per frame
    if (impl_->last_enhance_tex == show_tex && impl_->last_enhance_srv) {
      show_srv = impl_->last_enhance_srv;
    } else {
      impl_->device->CreateShaderResourceView(show_tex, nullptr, &show_srv);
      impl_->last_enhance_tex = show_tex;
      impl_->last_enhance_srv = show_srv;
    }
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

  // Bake HUD into the swapchain so OBS window/game capture of NexusCap sees it.
  if (overlay.visible) {
    if (overlay.hud_on && overlay.live_overlay) {
      burn_live_overlay(overlay);
    } else if (overlay.hud_on && overlay.mini_overlay) {
      burn_mini_overlay(overlay);
    } else if (overlay.hud_on) {
      burn_panel_overlay(overlay);
    }
    if (!overlay.session_notice.empty()) {
      burn_session_notice(overlay);
    }
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
  const bool want_hold = hold_arm_ || (hold_probe_ && hold_probe_(hold_probe_user_));
  hold_arm_ = false;
  const HoldFrameChoice choice = hold_frame_choice(want_hold, impl_->lastPresented != nullptr);
  if (choice == HoldFrameChoice::SnapshotLastSkip) {
    // Este backbuffer tem o menu novo e ainda não passou pelo ReShade.
    // A tela continua no frame anterior. Não apresentar, senão o scanout pisca.
    hold_snapshot_ = write_hold_bmp(impl_->lastPresented.Get());
    if (hold_snapshot_) {
      // O atalho não desenha este frame. O menu desenha o clique aqui.
      // Se o resize mostrar esse buffer, só o menu pisca. Volta o frame da tela.
      restore_backbuffer_from_last();
      present_ms = clk.now_ms() - t0;
      (void)err;
      return true;
    }
  }
  if (want_hold) {
    hold_snapshot_ = snapshot_hold_backbuffer();
  } else {
    // Clique que não reinicia não pode deixar a foto velha para o próximo restart.
    hold_snapshot_ = false;
  }

  // Ponteiro de antes do Present. Depois, GetBuffer(0) já é o outro buffer do flip.
  ComPtr<ID3D11Texture2D> presented;
  if (impl_->swap) (void)impl_->swap->GetBuffer(0, IID_PPV_ARGS(&presented));

  const UINT interval = (overlay.fg_on || vsync_) ? present_interval_ : 0u;
  silence_reshade_home_splash();
  impl_->swap->Present(interval, 0);
  // Only retain the presented frame if a hold may be needed (menu open or armed).
  // This avoids a full backbuffer copy every frame when the feature isn't being used.
  if (want_hold && presented) (void)retain_presented_frame(presented.Get());
  present_ms = clk.now_ms() - t0;
  (void)err;
  return true;
}

void RendererD3D11::restore_backbuffer_from_last() {
  if (!impl_ || !impl_->swap || !impl_->ctx || !impl_->lastPresented) return;
  ComPtr<ID3D11Texture2D> back;
  if (FAILED(impl_->swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return;
  impl_->ctx->OMSetRenderTargets(0, nullptr, nullptr);
  impl_->ctx->CopyResource(back.Get(), impl_->lastPresented.Get());
}

bool RendererD3D11::retain_presented_frame(ID3D11Texture2D* src) {
  if (!impl_ || !impl_->ctx || !impl_->device || !src) return false;
  D3D11_TEXTURE2D_DESC desc{};
  src->GetDesc(&desc);
  if (desc.Width < 2 || desc.Height < 2) return false;
  D3D11_TEXTURE2D_DESC have{};
  if (impl_->lastPresented) impl_->lastPresented->GetDesc(&have);
  if (!impl_->lastPresented || have.Width != desc.Width || have.Height != desc.Height ||
      have.Format != desc.Format) {
    impl_->lastPresented.Reset();
    D3D11_TEXTURE2D_DESC keep = desc;
    keep.MipLevels = 1;
    keep.ArraySize = 1;
    keep.SampleDesc.Count = 1;
    keep.SampleDesc.Quality = 0;
    keep.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    keep.MiscFlags = 0;
    keep.CPUAccessFlags = 0;
    keep.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(impl_->device->CreateTexture2D(&keep, nullptr, &impl_->lastPresented)) || !impl_->lastPresented) {
      return false;
    }
  }
  impl_->ctx->OMSetRenderTargets(0, nullptr, nullptr);
  impl_->ctx->CopyResource(impl_->lastPresented.Get(), src);
  return true;
}

bool RendererD3D11::snapshot_hold_from_last_presented() {
  if (!impl_ || !impl_->lastPresented) return false;
  hold_snapshot_ = write_hold_bmp(impl_->lastPresented.Get());
  return hold_snapshot_;
}

bool RendererD3D11::snapshot_hold_backbuffer() {
  if (!impl_ || !impl_->swap || !impl_->ctx || !impl_->device) return false;
  ComPtr<ID3D11Texture2D> back;
  if (FAILED(impl_->swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
  return write_hold_bmp(back.Get());
}

bool RendererD3D11::write_hold_bmp(ID3D11Texture2D* src) {
  if (!impl_ || !impl_->ctx || !impl_->device || !src) return false;
  D3D11_TEXTURE2D_DESC desc{};
  src->GetDesc(&desc);
  if (desc.Width < 2 || desc.Height < 2) return false;
  D3D11_TEXTURE2D_DESC st = desc;
  st.MipLevels = 1;
  st.ArraySize = 1;
  st.SampleDesc.Count = 1;
  st.SampleDesc.Quality = 0;
  st.BindFlags = 0;
  st.MiscFlags = 0;
  st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  st.Usage = D3D11_USAGE_STAGING;
  ComPtr<ID3D11Texture2D> stage;
  if (FAILED(impl_->device->CreateTexture2D(&st, nullptr, &stage)) || !stage) return false;
  impl_->ctx->OMSetRenderTargets(0, nullptr, nullptr);
  impl_->ctx->CopyResource(stage.Get(), src);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(impl_->ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) return false;
  const int w = static_cast<int>(desc.Width);
  const int h = static_cast<int>(desc.Height);
  const int pitch = static_cast<int>(mapped.RowPitch);
  const std::size_t stride = static_cast<std::size_t>(w) * 4u;
  if (pitch < static_cast<int>(stride)) {
    impl_->ctx->Unmap(stage.Get(), 0);
    return false;
  }
  BITMAPINFOHEADER bi{};
  bi.biSize = sizeof(bi);
  bi.biWidth = w;
  bi.biHeight = -h;
  bi.biPlanes = 1;
  bi.biBitCount = 32;
  bi.biCompression = BI_RGB;
  std::vector<std::uint8_t> bits(stride * static_cast<std::size_t>(h));
  const auto* rows = static_cast<const std::uint8_t*>(mapped.pData);
  for (int y = 0; y < h; ++y) {
    std::memcpy(bits.data() + static_cast<std::size_t>(y) * stride, rows + static_cast<std::size_t>(y) * pitch,
                stride);
  }
  impl_->ctx->Unmap(stage.Get(), 0);
  BITMAPFILEHEADER fh{};
  fh.bfType = 0x4D42;
  fh.bfOffBits = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER));
  fh.bfSize = fh.bfOffBits + static_cast<DWORD>(bits.size());
  const std::string utf8 = motor_hold_bmp_path();
  int nw = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
  if (nw <= 0) return false;
  std::wstring path(static_cast<std::size_t>(nw), 0);
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, path.data(), nw);
  if (!path.empty() && path.back() == 0) path.pop_back();
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
  fwrite(&fh, sizeof(fh), 1, f);
  fwrite(&bi, sizeof(bi), 1, f);
  fwrite(bits.data(), 1, bits.size(), f);
  fclose(f);
  return true;
}

void RendererD3D11::present_black() {
  if (!impl_ || !impl_->swap) return;
  silence_reshade_home_splash();
  const float clear[4] = {0, 0, 0, 1};
  impl_->ctx->OMSetRenderTargets(1, impl_->rtv.GetAddressOf(), nullptr);
  impl_->ctx->ClearRenderTargetView(impl_->rtv.Get(), clear);
  impl_->swap->Present(0, 0);
}

struct NcVblankCloak {
  IDXGIOutput* output;
  HANDLE done;
};

// O resize mede ~2 ms e, começando no vblank, acaba com o scan já no meio.
constexpr long long kFlickerResizeLeadUs = 2400;

long long flicker_us(const LARGE_INTEGER& a, const LARGE_INTEGER& b, const LARGE_INTEGER& freq) {
  if (freq.QuadPart <= 0) return 0;
  return (b.QuadPart - a.QuadPart) * 1000000ll / freq.QuadPart;
}

DWORD WINAPI nc_cloak_at_vblank(void* param) {
  auto* w = static_cast<NcVblankCloak*>(param);
  LARGE_INTEGER freq{}, t0{}, t1{}, now{};
  QueryPerformanceFrequency(&freq);
  if (w->output) w->output->WaitForVBlank();
  QueryPerformanceCounter(&t0);
  if (w->output) w->output->WaitForVBlank();
  QueryPerformanceCounter(&t1);
  long long period = flicker_us(t0, t1, freq);
  if (period < 4000 || period > 20000) period = 16667;
  const long long fire_at = period - kFlickerResizeLeadUs;
  timeBeginPeriod(1);
  for (;;) {
    QueryPerformanceCounter(&now);
    const long long elapsed = flicker_us(t1, now, freq);
    if (elapsed >= fire_at) break;
    if (fire_at - elapsed > 2000) Sleep(1);
  }
  timeEndPeriod(1);
  if (w->done) SetEvent(w->done);
  return 0;
}

void RendererD3D11::release_flip_for_cover(bool cover_latched) {
  if (!hwnd_) return;
  ComPtr<IDXGIOutput> output;
  if (impl_ && impl_->swap) (void)impl_->swap->GetContainingOutput(&output);
  // A capa já fez Present(1). Estamos no vblank. Não espera de novo.
  if (cover_latched) {
    if (impl_ && impl_->swap && impl_->ctx) {
      impl_->ctx->OMSetRenderTargets(0, nullptr, nullptr);
      impl_->rtv.Reset();
      impl_->swap->ResizeBuffers(0, 2, 2, DXGI_FORMAT_B8G8R8A8_UNORM, swap_flags_);
    }
    return;
  }
  HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  NcVblankCloak wait{output.Get(), done};
  HANDLE thread = nullptr;
  if (output && done) thread = CreateThread(nullptr, 0, nc_cloak_at_vblank, &wait, 0, nullptr);
  const DWORD t0 = GetTickCount();
  while (thread && done && WaitForSingleObject(done, 0) != WAIT_OBJECT_0 &&
         GetTickCount() - t0 < 80) {
    const DWORD wr = MsgWaitForMultipleObjects(1, &done, FALSE, 8, QS_ALLINPUT);
    if (wr == WAIT_OBJECT_0) break;
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        PostQuitMessage(static_cast<int>(msg.wParam));
        break;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  HRESULT hr = S_OK;
  const bool visible = IsWindowVisible(hwnd_) != FALSE;
  if (visible && impl_ && impl_->swap && impl_->ctx) {
    impl_->ctx->OMSetRenderTargets(0, nullptr, nullptr);
    impl_->rtv.Reset();
    hr = impl_->swap->ResizeBuffers(0, 2, 2, DXGI_FORMAT_B8G8R8A8_UNORM, swap_flags_);
    if (FAILED(hr)) {
      const BOOL cloak = TRUE;
      DwmSetWindowAttribute(hwnd_, DWMWA_CLOAK, &cloak, sizeof(cloak));
    }
  } else {
    const BOOL cloak = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_CLOAK, &cloak, sizeof(cloak));
  }
  if (thread) {
    WaitForSingleObject(thread, 80);
    CloseHandle(thread);
  }
  if (done) CloseHandle(done);
}

void RendererD3D11::restore_flip_after_cover() {
  if (!hwnd_) return;
  const BOOL cloak = FALSE;
  DwmSetWindowAttribute(hwnd_, DWMWA_CLOAK, &cloak, sizeof(cloak));
}

namespace {

constexpr wchar_t kHudClass[] = L"NexusCapHud";

std::wstring utf8_to_wide_hud(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 0) return L"";
  std::wstring w(static_cast<size_t>(n), 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  if (!w.empty() && w.back() == 0) w.pop_back();
  return w;
}

void paint_hud_fps_parts(HDC hdc, int x, int y, const HudFpsParts& parts) {
  for (int i = 0; i < parts.count; ++i) {
    const std::wstring w = utf8_to_wide_hud(parts.parts[i].text);
    if (w.empty()) continue;
    SetTextColor(hdc, RGB(parts.parts[i].r, parts.parts[i].g, parts.parts[i].b));
    TextOutW(hdc, x, y, w.c_str(), static_cast<int>(w.size()));
    SIZE sz{};
    GetTextExtentPoint32W(hdc, w.c_str(), static_cast<int>(w.size()), &sz);
    x += static_cast<int>(sz.cx);
  }
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

void paint_session_notice_band(HDC hdc, const RECT& rc, const OverlayState& o, bool plate, int scale = 1) {
  if (o.session_notice.empty()) return;
  if (scale < 1) scale = 1;
  std::string title, detail;
  session_notice_split(o.session_notice, title, detail);
  if (plate && overlay_draws_plate(o.overlay_no_bg)) {
    HBRUSH bg = CreateSolidBrush(RGB(12, 14, 18));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
  }
  SetBkMode(hdc, TRANSPARENT);
  const int font_px = 16 * scale;
  CachedHudFont& bank = scale > 1 ? g_hud_fonts.notice_live : g_hud_fonts.notice;
  HFONT notice_font = bank.get(font_px, FW_BOLD, NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  HFONT oldn = static_cast<HFONT>(SelectObject(hdc, notice_font));
  const int pad = 8 * scale;
  RECT tr{rc.left + pad, rc.top + 4 * scale, rc.right - pad, rc.bottom};
  SetTextColor(hdc, RGB(255, 200, 90));
  const std::wstring wt = utf8_to_wide_hud(title.empty() ? o.session_notice : title);
  DrawTextW(hdc, wt.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  if (!detail.empty()) {
    RECT dr{rc.left + pad, rc.top + 24 * scale, rc.right - pad, rc.bottom - 2};
    SetTextColor(hdc, RGB(255, 220, 140));
    const std::wstring wd = utf8_to_wide_hud(detail);
    DrawTextW(hdc, wd.c_str(), -1, &dr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
  }
  SelectObject(hdc, oldn);
}

int fill_mini_overlay_segs(const OverlayState& o, MiniOverlaySeg* segs) {
  char line0[768]{};
  char line1[768]{};
  char hw_compact[200]{};
  format_sys_metrics_compact(hw_compact, sizeof(hw_compact), metrics_from_overlay(o));
  const double show_fps = o.fps_smooth > 0 ? o.fps_smooth : o.fps;
  const bool dlss5_on = o.renodx_on;
  unsigned char cr = 0, cg = 55, cb = 145;
  live_console_rgb(clamp_live_console(o.live_console), cr, cg, cb);
  return format_mini_overlay_segments(
      segs, kMiniOverlayMaxSegs, line0, sizeof(line0), line1, sizeof(line1), show_fps, o.fps_fg_est, o.fg_on,
      o.fg_multiplier, dlss5_on, o.sync_image, o.frame_ms, o.ft_avg_ms, o.ft_p95_ms, o.ft_max_ms, o.ft_stable,
      o.e2e_ms, o.present_ms, o.stalls_window, o.clusters, o.dropped, o.capture_w, o.capture_h, o.work_w,
      o.work_h, o.output_w, o.output_h, o.scale_percent, o.has_stats600, o.s600_fps, o.s600_p95_e2e,
      o.s600_p95_present, o.s600_stalls, o.renodx_preset.c_str(), hw_compact[0] ? hw_compact : nullptr,
      o.dlss_ms, o.frames, o.s600_max_interval, o.fg_backend.c_str(), o.target_fps, o.nr_pass_count,
      overlay_console_label_or_null(o.overlay_console, clamp_live_console(o.live_console)), false, cr, cg, cb,
      o.fg_queue, o.fg_adaptive, o.fg_target_fps, o.fg_flow_scale,
      o.capture_fmt.empty() ? nullptr : o.capture_fmt.c_str());
}

int mini_overlay_text_avail(int bar_w) {
  const int pad = 8;
  const int graph_w = hud_mini_frametime_graph_w(bar_w);
  const int gx = (std::max)(pad, bar_w - pad - graph_w);
  const int text_max_x = gx - 8;
  return (std::max)(8, text_max_x - pad);
}

int mini_overlay_line_count(HDC hdc, const MiniOverlaySeg* segs, int nseg, int avail_px) {
  int widths[kMiniOverlayMaxSegs]{};
  const int n = nseg < kMiniOverlayMaxSegs ? nseg : kMiniOverlayMaxSegs;
  for (int i = 0; i < n; ++i) {
    const std::wstring w = utf8_to_wide_hud(segs[i].text);
    SIZE sz{};
    if (!w.empty()) GetTextExtentPoint32W(hdc, w.c_str(), static_cast<int>(w.size()), &sz);
    widths[i] = sz.cx > 0 ? static_cast<int>(sz.cx) : 1;
  }
  MiniOverlaySpan spans[8]{};
  int lines = mini_overlay_wrap_all(widths, segs, n, avail_px, spans, hud_mini_overlay_max_rows());
  if (lines < hud_mini_overlay_rows()) lines = hud_mini_overlay_rows();
  return lines;
}

int mini_overlay_bar_height(const OverlayState& o, int bar_w) {
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  const int font_px = hud_mini_overlay_font_px();
  HFONT font = g_hud_fonts.mini.get(font_px, FW_BOLD, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
  HFONT old = static_cast<HFONT>(SelectObject(mem, font));
  MiniOverlaySeg segs[kMiniOverlayMaxSegs]{};
  const int nseg = fill_mini_overlay_segs(o, segs);
  const int lines = mini_overlay_line_count(mem, segs, nseg, mini_overlay_text_avail(bar_w));
  SelectObject(mem, old);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return hud_mini_overlay_height_for_rows(lines);
}

void paint_mini_overlay_hdc(HDC hdc, const RECT& rc, const OverlayState& o) {
  if (overlay_draws_plate(o.overlay_no_bg)) {
    HBRUSH bg = CreateSolidBrush(RGB(12, 14, 18));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
  }
  SetBkMode(hdc, TRANSPARENT);

  const int font_px = hud_mini_overlay_font_px();
  HFONT mini_font = g_hud_fonts.mini.get(font_px, FW_BOLD, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN,
                                         L"Consolas");
  HFONT old = static_cast<HFONT>(SelectObject(hdc, mini_font));
  MiniOverlaySeg segs[kMiniOverlayMaxSegs]{};
  const int nseg = fill_mini_overlay_segs(o, segs);

  const int pad = 8;
  const int graph_w = hud_mini_frametime_graph_w(static_cast<int>(rc.right - rc.left));
  const int gap = 8;
  const int bar_w = static_cast<int>(rc.right - rc.left);
  const int bar_h = static_cast<int>(rc.bottom - rc.top);
  const int work_h = (std::max)(48, bar_h);
  const int gx = (std::max)(pad, bar_w - pad - graph_w);
  const int gy = 8;
  const int gh = (std::max)(51, work_h - 16);
  const int gw = (std::min)(graph_w, bar_w - pad * 2);
  const int text_max_x = gx - gap;
  const int avail = (std::max)(8, text_max_x - pad);
  const int line_gap = 2;

  int widths[kMiniOverlayMaxSegs]{};
  for (int i = 0; i < nseg; ++i) {
    const std::wstring w = utf8_to_wide_hud(segs[i].text);
    SIZE sz{};
    if (!w.empty()) GetTextExtentPoint32W(hdc, w.c_str(), static_cast<int>(w.size()), &sz);
    widths[i] = sz.cx > 0 ? static_cast<int>(sz.cx) : 1;
  }
  MiniOverlaySpan spans[8]{};
  int lines = mini_overlay_wrap_all(widths, segs, nseg, avail, spans, hud_mini_overlay_max_rows());
  if (lines < hud_mini_overlay_rows()) lines = hud_mini_overlay_rows();
  const int block_h = font_px * lines + line_gap * (lines - 1);
  int y = (std::max)(2, (work_h - block_h) / 2);

  auto skip_sep = [&](int i, int end) -> int {
    while (i < end && std::strcmp(segs[i].text, " · ") == 0) ++i;
    return i;
  };
  auto paint_span = [&](int begin, int end, int text_y) {
    int x = pad;
    begin = skip_sep(begin, end);
    for (int i = begin; i < end; ++i) {
      const std::wstring w = utf8_to_wide_hud(segs[i].text);
      if (w.empty()) continue;
      const int tw = widths[i];
      if (x > pad && x + tw > text_max_x) break;
      SetTextColor(hdc, RGB(segs[i].r, segs[i].g, segs[i].b));
      if (x + tw > text_max_x) {
        RECT clip{x, text_y - 1, text_max_x, text_y + font_px + 1};
        DrawTextW(hdc, w.c_str(), static_cast<int>(w.size()), &clip,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        break;
      }
      TextOutW(hdc, x, text_y, w.c_str(), static_cast<int>(w.size()));
      x += tw;
    }
  };
  for (int s = 0; s < lines; ++s) {
    if (s < 8 && spans[s].end > spans[s].begin) paint_span(spans[s].begin, spans[s].end, y);
    y += font_px + line_gap;
  }
  const int y0 = (std::max)(2, (work_h - block_h) / 2);

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

  HFONT big = g_hud_fonts.hold_big.get(32, FW_SEMIBOLD, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
                                       L"Segoe UI");
  HFONT old = static_cast<HFONT>(SelectObject(hdc, big));
  SetTextColor(hdc, RGB(255, 210, 110));
  RECT tr{rc.left + 24, rc.top + 18, rc.right - 24, rc.top + 62};
  DrawTextW(hdc, title.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
  SelectObject(hdc, old);

  HFONT small_font = g_hud_fonts.hold_small.get(20, FW_NORMAL, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS,
                                                L"Segoe UI");
  old = static_cast<HFONT>(SelectObject(hdc, small_font));
  SetTextColor(hdc, RGB(205, 225, 250));
  RECT dr{rc.left + 24, rc.top + 68, rc.right - 24, rc.bottom - 16};
  DrawTextW(hdc, detail.c_str(), -1, &dr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
  SelectObject(hdc, old);
}

}  // namespace

void paint_panel_overlay_hdc(HDC hdc, const RECT& rc, const OverlayState& o);

bool RendererD3D11::ensure_hud_tex(int w, int h) {
  if (!impl_ || !impl_->device || w < 8 || h < 8) return false;
  if (impl_->hudTex && impl_->hud_w == w && impl_->hud_h == h) return true;
  impl_->hudSrv.Reset();
  impl_->hudTex.Reset();
  D3D11_TEXTURE2D_DESC td{};
  td.Width = static_cast<UINT>(w);
  td.Height = static_cast<UINT>(h);
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DYNAMIC;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(impl_->device->CreateTexture2D(&td, nullptr, &impl_->hudTex))) return false;
  impl_->device->CreateShaderResourceView(impl_->hudTex.Get(), nullptr, &impl_->hudSrv);
  impl_->hud_w = w;
  impl_->hud_h = h;
  impl_->hud_last_tick = 0;
  return true;
}

bool RendererD3D11::ensure_toast_tex(int w, int h) {
  if (!impl_ || !impl_->device || w < 8 || h < 8) return false;
  if (impl_->toastTex && impl_->toast_w == w && impl_->toast_h == h) return true;
  impl_->toastSrv.Reset();
  impl_->toastTex.Reset();
  D3D11_TEXTURE2D_DESC td{};
  td.Width = static_cast<UINT>(w);
  td.Height = static_cast<UINT>(h);
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DYNAMIC;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(impl_->device->CreateTexture2D(&td, nullptr, &impl_->toastTex))) return false;
  impl_->device->CreateShaderResourceView(impl_->toastTex.Get(), nullptr, &impl_->toastSrv);
  impl_->toast_w = w;
  impl_->toast_h = h;
  impl_->toast_last_tick = 0;
  return true;
}

void RendererD3D11::upload_hud_bits(const void* bits, int w, int h) {
  if (!impl_ || !impl_->ctx || !impl_->hudTex || !bits || w < 1 || h < 1) return;
  D3D11_MAPPED_SUBRESOURCE map{};
  if (FAILED(impl_->ctx->Map(impl_->hudTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
  const auto* src = static_cast<const uint8_t*>(bits);
  auto* dst = static_cast<uint8_t*>(map.pData);
  const size_t row_bytes = static_cast<size_t>(w) * 4u;
  for (int y = 0; y < h; ++y) {
    std::memcpy(dst + static_cast<size_t>(y) * map.RowPitch, src + static_cast<size_t>(y) * row_bytes, row_bytes);
  }
  impl_->ctx->Unmap(impl_->hudTex.Get(), 0);
}

void RendererD3D11::upload_toast_bits(const void* bits, int w, int h) {
  if (!impl_ || !impl_->ctx || !impl_->toastTex || !bits || w < 1 || h < 1) return;
  D3D11_MAPPED_SUBRESOURCE map{};
  if (FAILED(impl_->ctx->Map(impl_->toastTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
  const auto* src = static_cast<const uint8_t*>(bits);
  auto* dst = static_cast<uint8_t*>(map.pData);
  const size_t row_bytes = static_cast<size_t>(w) * 4u;
  for (int y = 0; y < h; ++y) {
    std::memcpy(dst + static_cast<size_t>(y) * map.RowPitch, src + static_cast<size_t>(y) * row_bytes, row_bytes);
  }
  impl_->ctx->Unmap(impl_->toastTex.Get(), 0);
}

void RendererD3D11::blit_overlay_srv(ID3D11ShaderResourceView* srv, int x, int y, int w, int h) {
  if (!impl_ || !impl_->ctx || !impl_->rtv || !srv) return;
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
  vp.TopLeftX = static_cast<float>(x);
  vp.TopLeftY = static_cast<float>(y);
  vp.Width = static_cast<float>(w);
  vp.Height = static_cast<float>(h);
  impl_->ctx->OMSetRenderTargets(1, impl_->rtv.GetAddressOf(), nullptr);
  impl_->ctx->RSSetViewports(1, &vp);
  const float bf[4] = {0, 0, 0, 0};
  if (impl_->hudBlend) impl_->ctx->OMSetBlendState(impl_->hudBlend.Get(), bf, 0xffffffff);
  impl_->ctx->VSSetShader(impl_->blitVs.Get(), nullptr, 0);
  impl_->ctx->PSSetShader(impl_->blitPs.Get(), nullptr, 0);
  ID3D11ShaderResourceView* srvs[] = {srv};
  impl_->ctx->PSSetShaderResources(0, 1, srvs);
  ID3D11SamplerState* samp[] = {impl_->sampler.Get()};
  impl_->ctx->PSSetSamplers(0, 1, samp);
  impl_->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  impl_->ctx->Draw(3, 0);
  ID3D11ShaderResourceView* null_srv[] = {nullptr};
  impl_->ctx->PSSetShaderResources(0, 1, null_srv);
  impl_->ctx->OMSetBlendState(nullptr, bf, 0xffffffff);
}

void RendererD3D11::blit_hud(int x, int y, int w, int h) {
  if (!impl_ || !impl_->hudSrv) return;
  blit_overlay_srv(impl_->hudSrv.Get(), x, y, w, h);
}

void RendererD3D11::burn_mini_overlay(const OverlayState& overlay) {
  if (!impl_ || !impl_->ctx || !impl_->rtv || back_w_ < 64 || back_h_ < 32) return;

  const int gap = 8;
  int bar_w = (std::max)(200, back_w_ - gap * 2);
  int bar_h = mini_overlay_bar_height(overlay, bar_w);
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

void RendererD3D11::burn_panel_overlay(const OverlayState& overlay) {
  if (!impl_ || !impl_->ctx || !impl_->rtv || back_w_ < 64 || back_h_ < 32) return;
  const int margin = 24;
  int bar_w = (std::min)(hud_panel_width(overlay.advanced_metrics), (std::max)(160, back_w_ - margin * 2));
  int bar_h = (std::min)(hud_panel_height(overlay.advanced_metrics, overlay.hw_info),
                         (std::max)(80, back_h_ - margin * 2));
  if (!ensure_hud_tex(bar_w, bar_h)) return;

  const DWORD now = GetTickCount();
  const bool refresh = (impl_->hud_last_tick == 0) || (now - impl_->hud_last_tick) >= 33;
  if (refresh) {
    impl_->hud_last_tick = now;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bar_w;
    bmi.bmiHeader.biHeight = -bar_h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dib || !bits) {
      if (dib) DeleteObject(dib);
      if (mem) DeleteDC(mem);
      return;
    }
    HGDIOBJ old_bmp = SelectObject(mem, dib);
    if (overlay.overlay_no_bg && bits) {
      std::memset(bits, 0, static_cast<size_t>(bar_w) * static_cast<size_t>(bar_h) * 4u);
    }
    RECT rc{0, 0, bar_w, bar_h};
    paint_panel_overlay_hdc(mem, rc, overlay);
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
    upload_hud_bits(bits, bar_w, bar_h);
    SelectObject(mem, old_bmp);
    DeleteObject(dib);
    DeleteDC(mem);
  }
  blit_hud(margin, margin, bar_w, bar_h);
}

void RendererD3D11::burn_live_overlay(const OverlayState& overlay) {
  if (!impl_ || !impl_->ctx || !impl_->rtv || back_w_ < 64 || back_h_ < 32) return;

  const DWORD now = GetTickCount();
  const bool refresh =
      (impl_->hud_last_tick == 0) || (now - impl_->hud_last_tick) >= 33 || impl_->live_blit_w <= 0;
  if (refresh) {
    UINT dpi = 96;
    if (hwnd_) {
      using Fn = UINT(WINAPI*)(HWND);
      static Fn get_dpi =
          reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
      if (get_dpi) {
        const UINT d = get_dpi(hwnd_);
        if (d >= 96) dpi = d;
      }
    }
    auto md = [&](int v) { return MulDiv(v, static_cast<int>(dpi), 96); };
    const int font_px = md(16);
    const int hh = md(26);
    const int spark_w = md(104);
    const int gap = md(12);
    const int pad_x = md(8);
    const int margin = md(10);

    const LiveConsole cons = clamp_live_console(overlay.live_console);
    const double show_fps = overlay.fps_smooth > 0 ? overlay.fps_smooth : overlay.fps;
    LiveStripLines strip{};
    fill_live_strip_lines(strip, overlay.overlay_console, cons, overlay.renodx_on || overlay.dlss_on,
                          overlay.nr_pass_count, overlay.fg_on, overlay.fg_multiplier, show_fps,
                          overlay.fps_fg_est, overlay.ft_stable, overlay.fg_queue, overlay.frame_ms,
                          overlay.fg_adaptive, overlay.fg_target_fps, overlay.fg_flow_scale);
    const std::wstring cons_w = strip.has_console ? utf8_to_wide_hud(strip.console) : std::wstring{};
    const std::wstring dlss_w = utf8_to_wide_hud(strip.dlss);
    const std::wstring fg_w = utf8_to_wide_hud(strip.fg);
    const std::wstring fps_lab_w = utf8_to_wide_hud(strip.fps_lab.text);
    const std::wstring fps_cap_w = utf8_to_wide_hud(strip.fps_cap.text);
    const std::wstring fps_sep_w = strip.has_fg_fps ? utf8_to_wide_hud(strip.fps_sep.text) : std::wstring{};
    const std::wstring fps_fg_w = strip.has_fg_fps ? utf8_to_wide_hud(strip.fps_fg.text) : std::wstring{};
    const std::wstring ft_w = strip.ft[0] ? utf8_to_wide_hud(strip.ft) : std::wstring{};

    HDC screen = GetDC(nullptr);
    HDC measure = CreateCompatibleDC(screen);
    HFONT font = g_hud_fonts.live.get(font_px, FW_BOLD, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN,
                                      L"Consolas");
    HFONT old_m = static_cast<HFONT>(SelectObject(measure, font));
    auto measure_w = [&](const std::wstring& s) -> int {
      if (s.empty()) return 0;
      SIZE sz{};
      GetTextExtentPoint32W(measure, s.c_str(), static_cast<int>(s.size()), &sz);
      return sz.cx;
    };
    const int sz_cons = measure_w(cons_w);
    int ww = pad_x + sz_cons + (sz_cons > 0 ? gap : 0) + measure_w(dlss_w) + gap + measure_w(fg_w) + gap +
             measure_w(fps_lab_w) + measure_w(fps_cap_w) + measure_w(fps_sep_w) + measure_w(fps_fg_w) +
             (ft_w.empty() ? 0 : gap + measure_w(ft_w)) + gap + spark_w + pad_x;
    SelectObject(measure, old_m);
    DeleteDC(measure);
    if (ww < 80) ww = 80;
    const int bar_h = hh;
    const int bar_w = (std::min)(back_w_ - margin * 2, (std::max)(ww, (std::min)(960, back_w_ / 2)));
    const int content_w = (std::min)(ww, bar_w);
    if (!ensure_hud_tex(bar_w, bar_h)) {
      ReleaseDC(nullptr, screen);
      return;
    }

    impl_->hud_last_tick = now;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bar_w;
    bmi.bmiHeader.biHeight = -bar_h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
      if (dib) DeleteObject(dib);
      if (mem) DeleteDC(mem);
      ReleaseDC(nullptr, screen);
      return;
    }
    HGDIOBJ old_bmp = SelectObject(mem, dib);
    std::memset(bits, 0, static_cast<size_t>(bar_w) * static_cast<size_t>(bar_h) * 4u);
    const int x0 = bar_w - content_w;
    if (overlay_draws_plate(overlay.overlay_no_bg)) {
      RECT plate{x0, 0, bar_w, bar_h};
      HBRUSH bg = CreateSolidBrush(RGB(12, 14, 18));
      FillRect(mem, &plate, bg);
      DeleteObject(bg);
    }
    SetBkMode(mem, TRANSPARENT);
    HFONT old_font = static_cast<HFONT>(SelectObject(mem, font));
    const int text_y = (bar_h - font_px) / 2;
    int x = x0 + pad_x;
    auto draw_seg = [&](const std::wstring& text, COLORREF color) {
      if (text.empty()) return;
      SIZE sz{};
      GetTextExtentPoint32W(mem, text.c_str(), static_cast<int>(text.size()), &sz);
      SetTextColor(mem, color);
      TextOutW(mem, x, text_y, text.c_str(), static_cast<int>(text.size()));
      x += sz.cx + gap;
    };
    auto draw_join = [&](const std::wstring& text, COLORREF color) {
      if (text.empty()) return;
      SIZE sz{};
      GetTextExtentPoint32W(mem, text.c_str(), static_cast<int>(text.size()), &sz);
      SetTextColor(mem, color);
      TextOutW(mem, x, text_y, text.c_str(), static_cast<int>(text.size()));
      x += sz.cx;
    };
    if (!cons_w.empty()) draw_seg(cons_w, live_console_color(cons));
    draw_seg(dlss_w, RGB(240, 242, 248));
    unsigned char fgr = 255, fgg = 196, fgb = 72;
    live_fps_ft_rgb(overlay.fg_on && overlay.fg_multiplier >= 2 ? overlay.fg_multiplier : 1, overlay.ft_stable,
                    fgr, fgg, fgb);
    draw_seg(fg_w, RGB(fgr, fgg, fgb));
    draw_join(fps_lab_w, RGB(strip.fps_lab.r, strip.fps_lab.g, strip.fps_lab.b));
    draw_join(fps_cap_w, RGB(strip.fps_cap.r, strip.fps_cap.g, strip.fps_cap.b));
    draw_join(fps_sep_w, RGB(strip.fps_sep.r, strip.fps_sep.g, strip.fps_sep.b));
    draw_join(fps_fg_w, RGB(strip.fps_fg.r, strip.fps_fg.g, strip.fps_fg.b));
    x += gap;
    if (!ft_w.empty()) {
      const OverlayFtStatus ft_st =
          overlay_ft_status(overlay.ft_stable, overlay.fg_on, overlay.fg_multiplier, show_fps,
                            overlay.target_fps, overlay.fps_fg_est);
      const FgFtRgb ft_rgb = overlay_ft_status_rgb(ft_st);
      draw_seg(ft_w, RGB(ft_rgb.r, ft_rgb.g, ft_rgb.b));
    }
    {
      const int gx = x;
      const int gw = (std::min)(spark_w, bar_w - gx - pad_x);
      const int gy = md(4);
      const int gh = bar_h - gy * 2;
      if (gw >= 24) paint_frametime_traces(mem, gx, gy, gw, gh, overlay, (std::max)(1, md(1)));
    }
    SelectObject(mem, old_font);
    auto* px = static_cast<uint32_t*>(bits);
    if (overlay.overlay_no_bg) {
      const int n = bar_w * bar_h;
      for (int i = 0; i < n; ++i) {
        const uint32_t rgb = px[i] & 0x00FFFFFFu;
        px[i] = rgb ? (rgb | 0xFF000000u) : 0;
      }
    } else {
      constexpr uint32_t kA = 220u << 24;
      for (int y = 0; y < bar_h; ++y) {
        uint32_t* row = px + y * bar_w;
        for (int col = 0; col < bar_w; ++col) {
          row[col] = col >= x0 ? ((row[col] & 0x00FFFFFFu) | kA) : 0;
        }
      }
    }
    upload_hud_bits(bits, bar_w, bar_h);
    SelectObject(mem, old_bmp);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    impl_->live_blit_w = bar_w;
    impl_->live_blit_h = bar_h;
    impl_->live_blit_x = (std::max)(margin, back_w_ - margin - bar_w);
    impl_->live_blit_y = margin;
  }
  if (impl_->live_blit_w > 0) {
    blit_hud(impl_->live_blit_x, impl_->live_blit_y, impl_->live_blit_w, impl_->live_blit_h);
  }
}

void RendererD3D11::burn_session_notice(const OverlayState& overlay) {
  if (!impl_ || !impl_->ctx || !impl_->rtv || overlay.session_notice.empty()) return;
  if (back_w_ < 64 || back_h_ < 32) return;
  const int margin = 40;
  const int scale = session_notice_toast_scale();
  int bar_h = session_notice_band_px(overlay.session_notice, scale);
  int bar_w = (std::min)(960, (std::max)(560, back_w_ - margin * 2));
  if (bar_w > back_w_) bar_w = back_w_;
  if (bar_h > back_h_) bar_h = back_h_;
  if (bar_h < 8 || bar_w < 8) return;
  if (!ensure_toast_tex(bar_w, bar_h)) return;

  const DWORD now = GetTickCount();
  const bool refresh = (impl_->toast_last_tick == 0) || (now - impl_->toast_last_tick) >= 33;
  if (refresh) {
    impl_->toast_last_tick = now;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bar_w;
    bmi.bmiHeader.biHeight = -bar_h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dib || !bits) {
      if (dib) DeleteObject(dib);
      if (mem) DeleteDC(mem);
      return;
    }
    HGDIOBJ old_bmp = SelectObject(mem, dib);
    RECT rc{0, 0, bar_w, bar_h};
    if (overlay.overlay_no_bg && bits) {
      std::memset(bits, 0, static_cast<size_t>(bar_w) * static_cast<size_t>(bar_h) * 4u);
    }
    paint_session_notice_band(mem, rc, overlay, true, scale);
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
    upload_toast_bits(bits, bar_w, bar_h);
    SelectObject(mem, old_bmp);
    DeleteObject(dib);
    DeleteDC(mem);
  }
  int nx = 0;
  int ny = 0;
  overlay_notice_place(bar_w, bar_h, back_w_, back_h_, nx, ny);
  blit_overlay_srv(impl_->toastSrv.Get(), nx, ny, bar_w, bar_h);
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
  int hx = 0;
  int hy = 0;
  overlay_notice_place(nw, nh, back_w_, back_h_, hx, hy);
  vp.TopLeftX = static_cast<float>(hx);
  vp.TopLeftY = static_cast<float>(hy);
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
  if (hbmp_) {
    if (hdc_mem_ && hbmp_old_) SelectObject(hdc_mem_, hbmp_old_);
    DeleteObject(hbmp_);
    hbmp_ = nullptr;
    hbmp_old_ = nullptr;
  }
  if (hdc_mem_) {
    DeleteDC(hdc_mem_);
    hdc_mem_ = nullptr;
  }
  cache_w_ = cache_h_ = 0;
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
    int hh = mini_overlay_bar_height(state_, ww);
    SetWindowPos(hwnd_, HWND_TOPMOST, r.left + gap, r.top + gap, ww, hh,
                 SWP_NOACTIVATE | (visible_ ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    return;
  }
  const int ww = hud_panel_width(state_.advanced_metrics);
  int hh = hud_panel_height(state_.advanced_metrics, state_.hw_info);
  SetWindowPos(hwnd_, HWND_TOPMOST, r.left + 24, r.top + 24, ww, hh,
               SWP_NOACTIVATE | (visible_ ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

void MetricsHud::update(const OverlayState& state) {
  state_ = state;
  if (!hwnd_) return;
  // Compact / Mini / ADV / Live are burned into Present so OBS window capture sees them.
  set_visible(false);
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

  HFONT font = g_hud_fonts.panel.get(26, FW_SEMIBOLD, NONANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN,
                                     L"Consolas");
  HFONT old = static_cast<HFONT>(SelectObject(hdc, font));

  const wchar_t* mode = o.fake_capture ? L"FAKE" : L"LIVE";
  const bool exact = o.capture_w == o.output_w && o.capture_h == o.output_h && o.capture_w > 0;
  const double target_ms = o.target_fps > 0 ? 1000.0 / o.target_fps : (1000.0 / 60.0);

  // Compact HUD (Exibir metricas only): FPS + FG + res — no graph / last600.
  if (!o.advanced_metrics) {
    wchar_t lines[16][180];
    int n = 0;
    swprintf(lines[n++], 180, L"NexusCap  [%s]  ", mode);
    if (!o.capture_fmt.empty()) {
      swprintf(lines[n++], 180, L"%dx%d @ %.0f  %s", o.capture_w, o.capture_h, o.target_fps,
               utf8_to_wide_hud(o.capture_fmt).c_str());
    } else {
      swprintf(lines[n++], 180, L"%dx%d @ %.0f", o.capture_w, o.capture_h, o.target_fps);
    }
    const double show_fps = o.fps_smooth > 0 ? o.fps_smooth : o.fps;
    HudFpsParts fps_parts{};
    {
      fill_session_hud_fps_parts(fps_parts, show_fps, o.fps_fg_est, o.fg_on, o.fg_multiplier, true,
                                o.ft_stable, o.fg_adaptive, o.fg_target_fps, o.fg_flow_scale);
      char fps_line[128]{};
      join_hud_fps_parts(fps_line, sizeof(fps_line), fps_parts);
      swprintf(lines[n++], 180, L"%s", utf8_to_wide_hud(fps_line).c_str());
    }
    const OverlayFtStatus ft_st =
        overlay_ft_status(o.ft_stable, o.fg_on, o.fg_multiplier, show_fps, o.target_fps, o.fps_fg_est);
    int i_ft = -1;
    int i_lat = -1;
    int i_fg = -1;
    int i_sync = -1;
    {
      char ft_line[180]{};
      format_overlay_frametime_line(ft_line, sizeof(ft_line), o.frame_ms, o.ft_avg_ms, o.ft_p95_ms, o.ft_stable,
                                   o.fg_on, o.fg_multiplier, show_fps, o.target_fps, o.fps_fg_est);
      i_ft = n;
      swprintf(lines[n++], 180, L"%s", utf8_to_wide_hud(ft_line).c_str());
    }
    {
      const bool nr_on = o.renodx_on || o.dlss_backend.find("renodx") != std::string::npos;
      char lat_line[180]{};
      format_overlay_latency_line(lat_line, sizeof(lat_line), o.e2e_ms, o.present_ms, nr_on);
      i_lat = n;
      swprintf(lines[n++], 180, L"%s", utf8_to_wide_hud(lat_line).c_str());
    }
    {
      char stack[96]{};
      format_dlss5_stack_hud_line(stack, sizeof(stack), o.renodx_on, o.feed_on, o.nr_pass_count);
      swprintf(lines[n++], 180, L"%s", utf8_to_wide_hud(stack).c_str());
    }
    {
      char fg_line[160]{};
      format_session_hud_fg_status_line(fg_line, sizeof(fg_line), o.fg_on, o.fg_multiplier, o.fg_backend.c_str(),
                                        o.fg_flow_scale, o.fg_capture_api.c_str(), o.fg_status.c_str(),
                                        show_fps, o.fps_fg_est, o.fg_queue, o.fg_adaptive, o.fg_target_fps);
      i_fg = n;
      swprintf(lines[n++], 180, L"%s", utf8_to_wide_hud(fg_line).c_str());
    }
    i_sync = n;
    swprintf(lines[n++], 180, L"%s  Present(%u)", o.sync_image ? L"SYNC VSYNC" : L"SYNC TEAR",
             o.sync_image ? 1u : 0u);
    const int hw_start = n;
    if (o.hw_info) n = append_hw_metric_lines(lines, n, 15, o);
    const int hotkey_i = n;
    swprintf(lines[n++], 180, L"F3 HUD  PgDn FG  Delete NR  Esc");
    for (int i = 0; i < n; ++i) {
      COLORREF c = RGB(200, 200, 210);
      if (i == 0) c = RGB(255, 210, 70);
      else if (i == 1) c = RGB(120, 220, 255);
      else if (i == 2) c = RGB(120, 255, 160);
      else if (i == i_ft) {
        const FgFtRgb rgb = overlay_ft_status_rgb(ft_st);
        c = RGB(rgb.r, rgb.g, rgb.b);
      } else if (i == i_lat) c = RGB(120, 255, 160);
      else if (i == i_fg) c = o.fg_on ? RGB(120, 255, 200) : RGB(160, 170, 180);
      else if (i == i_sync) c = o.sync_image ? RGB(120, 220, 255) : RGB(255, 160, 90);
      else if (o.hw_info && i >= hw_start && i < hotkey_i) c = RGB(140, 255, 180);
      else if (i == hotkey_i) c = RGB(150, 150, 160);
      const int y = 10 + i * 28;
      if (i == 0) {
        hud_draw_title_with_console(hdc, 14, y, lines[i], o);
        continue;
      }
      if (i == 2) {
        paint_hud_fps_parts(hdc, 14, y, fps_parts);
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
  if (!o.capture_fmt.empty()) {
    wchar_t sig[180]{};
    swprintf(sig, 180, L"%s  %s", lines[2], utf8_to_wide_hud(o.capture_fmt).c_str());
    swprintf(lines[2], 180, L"%s", sig);
  }
  HudFpsParts fps_parts{};
  fill_session_hud_fps_parts(fps_parts, o.fps_smooth, o.fps_fg_est, o.fg_on, o.fg_multiplier, false,
                            o.ft_stable, o.fg_adaptive, o.fg_target_fps, o.fg_flow_scale);
  {
    char fps_line[128]{};
    join_hud_fps_parts(fps_line, sizeof(fps_line), fps_parts);
    swprintf(lines[3], 180, L"%s", utf8_to_wide_hud(fps_line).c_str());
  }
  const OverlayFtStatus ft_st = overlay_ft_status(
      o.ft_stable, o.fg_on, o.fg_multiplier, o.fps_smooth > 0 ? o.fps_smooth : o.fps, o.target_fps, o.fps_fg_est);
  {
    char ft_line[180]{};
    format_overlay_frametime_line(ft_line, sizeof(ft_line), o.frame_ms, o.ft_avg_ms, o.ft_p95_ms, o.ft_stable,
                                 o.fg_on, o.fg_multiplier, o.fps_smooth > 0 ? o.fps_smooth : o.fps, o.target_fps,
                                 o.fps_fg_est);
    swprintf(lines[4], 180, L"%s", utf8_to_wide_hud(ft_line).c_str());
  }
  {
    const bool nr_on = o.renodx_on || o.dlss_backend.find("renodx") != std::string::npos;
    char lat_line[180]{};
    format_overlay_latency_line(lat_line, sizeof(lat_line), o.e2e_ms, o.present_ms, nr_on,
                               o.sync_image ? "VSYNC" : "TEAR");
    swprintf(lines[5], 180, L"%s", utf8_to_wide_hud(lat_line).c_str());
  }
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
                                      o.fps_fg_est, o.fg_queue, o.fg_adaptive, o.fg_target_fps);
    swprintf(lines[7], 180, L"%s", utf8_to_wide_hud(fg_line).c_str());
  }

  int idx = 8;
  const int hw_start = idx;
  if (o.hw_info) idx = append_hw_metric_lines(lines, idx, 19, o);
  const int hw_end = idx;
  swprintf(lines[idx++], 180, L"stalls %llu   clusters %llu",
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
  const int nlines = idx;

  auto color_for_adv = [&](int i) -> COLORREF {
    if (i == 0) return RGB(255, 210, 70);
    if (i == 1) return RGB(200, 200, 210);
    if (i == 2) return RGB(120, 220, 255);
    if (i == 3) return RGB(120, 255, 160);
    if (i == 4) {
      const FgFtRgb c = overlay_ft_status_rgb(ft_st);
      return RGB(c.r, c.g, c.b);
    }
    if (i == 5) return RGB(120, 255, 160);
    if (i == 6) return RGB(180, 160, 255);
    if (i == 7) return o.fg_on ? RGB(120, 255, 200) : RGB(160, 170, 180);
    if (o.hw_info && i >= hw_start && i < hw_end) return RGB(140, 255, 180);
    if (i == nlines - 1) return RGB(150, 150, 160);
    if (i == nlines - 2) return RGB(255, 140, 140);
    return RGB(255, 180, 120);
  };

  for (int i = 0; i < 5; ++i) {
    const int y = 10 + i * 28;
    if (i == 0) {
      hud_draw_title_with_console(hdc, 14, y, lines[i], o);
      continue;
    }
    if (i == 3) {
      paint_hud_fps_parts(hdc, 14, y, fps_parts);
      continue;
    }
    SetTextColor(hdc, color_for_adv(i));
    TextOutW(hdc, 14, y, lines[i], static_cast<int>(wcslen(lines[i])));
  }

  // MSI-style frametime: cap real + FG do xN armado (Y real).
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
    
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    
    // Ensure cached DC and DIB exist with correct size
    if (!self->hdc_mem_ || self->cache_w_ != w || self->cache_h_ != h) {
      // Clean up old cache
      if (self->hbmp_) {
        if (self->hdc_mem_ && self->hbmp_old_) SelectObject(self->hdc_mem_, self->hbmp_old_);
        DeleteObject(self->hbmp_);
        self->hbmp_ = nullptr;
        self->hbmp_old_ = nullptr;
      }
      if (self->hdc_mem_) {
        DeleteDC(self->hdc_mem_);
        self->hdc_mem_ = nullptr;
      }
      
      // Create new cache
      if (w > 0 && h > 0) {
        self->hdc_mem_ = CreateCompatibleDC(hdc);
        self->hbmp_ = CreateCompatibleBitmap(hdc, w, h);
        self->hbmp_old_ = static_cast<HBITMAP>(SelectObject(self->hdc_mem_, self->hbmp_));
        self->cache_w_ = w;
        self->cache_h_ = h;
      }
    }
    
    // Paint to memory DC, then blit to screen
    if (self->hdc_mem_) {
      self->paint(self->hdc_mem_);
      BitBlt(hdc, 0, 0, w, h, self->hdc_mem_, 0, 0, SRCCOPY);
    } else {
      // Fallback if cache creation failed
      self->paint(hdc);
    }
    
    EndPaint(hwnd, &ps);
    return 0;
  }
  if (msg == WM_NCHITTEST) return HTTRANSPARENT;  // click-through
  if (msg == WM_ERASEBKGND) return 1;
  return DefWindowProcW(hwnd, msg, wp, lp);
}

namespace {
constexpr wchar_t kRestartNoticeClass[] = L"NexusCapRestartNotice";
constexpr int kRestartNoticeCardW = 900;
constexpr int kRestartNoticeCardH = 150;
#ifndef PW_RENDERFULLCONTENT
constexpr UINT kPrintFull = 0x00000002u;
#else
constexpr UINT kPrintFull = PW_RENDERFULLCONTENT;
#endif

std::wstring hold_path_w(const std::string& utf8) { return utf8_to_wide_hud(utf8); }

bool write_bmp32(const std::wstring& path, HBITMAP bmp) {
  BITMAP bm{};
  if (!GetObjectW(bmp, sizeof(bm), &bm) || bm.bmWidth < 1 || bm.bmHeight < 1) return false;
  BITMAPINFOHEADER bi{};
  bi.biSize = sizeof(bi);
  bi.biWidth = bm.bmWidth;
  bi.biHeight = -bm.bmHeight;
  bi.biPlanes = 1;
  bi.biBitCount = 32;
  bi.biCompression = BI_RGB;
  const std::size_t stride = static_cast<std::size_t>(bm.bmWidth) * 4u;
  std::vector<std::uint8_t> bits(stride * static_cast<std::size_t>(bm.bmHeight));
  HDC hdc = GetDC(nullptr);
  BITMAPINFO info{};
  info.bmiHeader = bi;
  const int got = GetDIBits(hdc, bmp, 0, static_cast<UINT>(bm.bmHeight), bits.data(), &info, DIB_RGB_COLORS);
  ReleaseDC(nullptr, hdc);
  if (got < 1) return false;
  BITMAPFILEHEADER fh{};
  fh.bfType = 0x4D42;
  fh.bfOffBits = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER));
  fh.bfSize = fh.bfOffBits + static_cast<DWORD>(bits.size());
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
  fwrite(&fh, sizeof(fh), 1, f);
  fwrite(&bi, sizeof(bi), 1, f);
  fwrite(bits.data(), 1, bits.size(), f);
  fclose(f);
  return true;
}
}  // namespace

bool save_motor_hold_from_hwnd(HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) return false;
  RECT wr{};
  if (!GetWindowRect(hwnd, &wr)) return false;
  const int w = wr.right - wr.left;
  const int h = wr.bottom - wr.top;
  if (w < 2 || h < 2) return false;
  HDC screen = GetDC(nullptr);
  if (!screen) return false;
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  HGDIOBJ old = SelectObject(mem, bmp);
  bool ok = BitBlt(mem, 0, 0, w, h, screen, wr.left, wr.top, SRCCOPY) != FALSE;
  if (!ok) ok = PrintWindow(hwnd, mem, kPrintFull) != FALSE;
  SelectObject(mem, old);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  if (!ok || !bmp) {
    if (bmp) DeleteObject(bmp);
    return false;
  }
  const bool saved = write_bmp32(hold_path_w(motor_hold_bmp_path()), bmp);
  DeleteObject(bmp);
  return saved;
}

void write_motor_hold_ready() {
  const std::wstring p = hold_path_w(motor_hold_ready_path());
  FILE* f = nullptr;
  if (_wfopen_s(&f, p.c_str(), L"wb") != 0 || !f) return;
  fputs("ready\n", f);
  fclose(f);
}

void reset_motor_hold_ready() {
  const std::wstring p = hold_path_w(motor_hold_ready_path());
  if (!p.empty()) DeleteFileW(p.c_str());
}

void clear_motor_hold_files() {
  const std::wstring bmp = hold_path_w(motor_hold_bmp_path());
  const std::wstring ready = hold_path_w(motor_hold_ready_path());
  if (!bmp.empty()) DeleteFileW(bmp.c_str());
  if (!ready.empty()) DeleteFileW(ready.c_str());
}

bool motor_hold_image_exists() {
  const std::wstring p = hold_path_w(motor_hold_bmp_path());
  if (p.empty()) return false;
  return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool wait_motor_hold_ready(int timeout_ms, RestartNotice* notice) {
  const std::wstring p = hold_path_w(motor_hold_ready_path());
  if (p.empty()) return false;
  const DWORD t0 = GetTickCount();
  const DWORD lim = timeout_ms < 1 ? 1u : static_cast<DWORD>(timeout_ms);
  while (GetTickCount() - t0 < lim) {
    if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    if (notice) notice->pump();
    Sleep(15);
  }
  return false;
}

void RestartNotice::release_hold_bmp() {
  if (hold_bmp_) {
    DeleteObject(hold_bmp_);
    hold_bmp_ = nullptr;
  }
  hold_w_ = hold_h_ = 0;
}

void RestartNotice::load_hold_bmp() {
  release_hold_bmp();
  const std::wstring p = hold_path_w(motor_hold_bmp_path());
  if (p.empty()) return;
  hold_bmp_ = static_cast<HBITMAP>(LoadImageW(nullptr, p.c_str(), IMAGE_BITMAP, 0, 0,
                                              LR_LOADFROMFILE | LR_CREATEDIBSECTION));
  if (!hold_bmp_) return;
  BITMAP bm{};
  if (GetObjectW(hold_bmp_, sizeof(bm), &bm)) {
    hold_w_ = bm.bmWidth;
    hold_h_ = bm.bmHeight;
  }
}

bool RestartNotice::show(HINSTANCE hi, const std::string& title, const std::string& detail, bool use_hold_image) {
  title_ = utf8_to_wide_hud(title);
  detail_ = utf8_to_wide_hud(detail);
  if (use_hold_image) load_hold_bmp();
  else release_hold_bmp();
  if (hwnd_) {
    InvalidateRect(hwnd_, nullptr, FALSE);
    pump();
    return true;
  }
  WNDCLASSW wc{};
  wc.lpfnWndProc = RestartNotice::WndProc;
  wc.hInstance = hi;
  wc.lpszClassName = kRestartNoticeClass;
  wc.hbrBackground = nullptr;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&wc);

  const int sw = GetSystemMetrics(SM_CXSCREEN);
  const int sh = GetSystemMetrics(SM_CYSCREEN);
  // GDI em ecrã inteiro: cobre a sessão a morrer. Sem D3D, sem segunda janela de motor.
  // Pinta o frame salvo antes de aparecer. O fundo preto da classe piscava a imagem.
  hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kRestartNoticeClass,
                          L"NexusCap", WS_POPUP, 0, 0, sw, sh, nullptr, nullptr, hi, this);
  if (!hwnd_) return false;
  if (HDC hdc = GetDC(hwnd_)) {
    paint(hdc);
    ReleaseDC(hwnd_, hdc);
    ValidateRect(hwnd_, nullptr);
  }
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  DwmFlush();
  pump();
  return true;
}

void RestartNotice::release_hold_flip() {
  if (flip_swap_) {
    flip_swap_->Release();
    flip_swap_ = nullptr;
  }
  if (flip_ctx_) {
    flip_ctx_->Release();
    flip_ctx_ = nullptr;
  }
  if (flip_device_) {
    flip_device_->Release();
    flip_device_ = nullptr;
  }
}

bool RestartNotice::present_hold_flip() {
  if (!hwnd_ || !hold_bmp_ || hold_w_ < 2 || hold_h_ < 2) return false;
  BITMAP bm{};
  if (!GetObject(hold_bmp_, sizeof(bm), &bm) || !bm.bmBits || bm.bmWidthBytes < hold_w_ * 4) return false;
  if (!flip_device_) {
    D3D_FEATURE_LEVEL fl{};
    const D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fls, 1, D3D11_SDK_VERSION,
                                 &flip_device_, &fl, &flip_ctx_)) ||
        !flip_device_ || !flip_ctx_) {
      release_hold_flip();
      return false;
    }
    ComPtr<IDXGIDevice> dxgi;
    flip_device_->QueryInterface(IID_PPV_ARGS(&dxgi));
    ComPtr<IDXGIAdapter> adapter;
    if (dxgi) dxgi->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    if (!adapter || FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))) || !factory) {
      release_hold_flip();
      return false;
    }
    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Width = static_cast<UINT>(hold_w_);
    sc.Height = static_cast<UINT>(hold_h_);
    sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 2;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc.Scaling = DXGI_SCALING_STRETCH;
    sc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    IDXGISwapChain1* created = nullptr;
    if (FAILED(factory->CreateSwapChainForHwnd(flip_device_, hwnd_, &sc, nullptr, nullptr, &created)) ||
        !created) {
      release_hold_flip();
      return false;
    }
    flip_swap_ = created;
  }
  ComPtr<ID3D11Texture2D> back;
  if (FAILED(flip_swap_->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
  flip_ctx_->UpdateSubresource(back.Get(), 0, nullptr, bm.bmBits, static_cast<UINT>(bm.bmWidthBytes), 0);
  return SUCCEEDED(flip_swap_->Present(1, 0));
}

void RestartNotice::destroy() {
  release_hold_flip();
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  release_hold_bmp();
}

void RestartNotice::pump() const {
  if (!hwnd_) return;
  SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
  MSG msg{};
  while (PeekMessageW(&msg, hwnd_, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

void RestartNotice::paint(HDC hdc) const {
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  if (hold_bmp_ && hold_w_ > 0 && hold_h_ > 0) {
    HDC mem = CreateCompatibleDC(hdc);
    HGDIOBJ old = SelectObject(mem, hold_bmp_);
    SetStretchBltMode(hdc, COLORONCOLOR);
    StretchBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, hold_w_, hold_h_, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    return;
  }
  HBRUSH bg = CreateSolidBrush(RGB(8, 10, 14));
  FillRect(hdc, &rc, bg);
  DeleteObject(bg);

  const int card_w = kRestartNoticeCardW;
  const int card_h = kRestartNoticeCardH;
  int x = 40;
  int y = static_cast<int>(rc.bottom) - card_h - 40;
  if (x + card_w > rc.right) x = static_cast<int>(rc.right) - card_w;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  RECT card{x, y, x + card_w, y + card_h};
  HBRUSH card_bg = CreateSolidBrush(RGB(14, 18, 26));
  FillRect(hdc, &card, card_bg);
  DeleteObject(card_bg);
  HPEN edge = CreatePen(PS_SOLID, 2, RGB(240, 180, 65));
  HGDIOBJ old_pen = SelectObject(hdc, edge);
  HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
  Rectangle(hdc, card.left, card.top, card.right, card.bottom);
  SelectObject(hdc, old_pen);
  SelectObject(hdc, old_brush);
  DeleteObject(edge);

  SetBkMode(hdc, TRANSPARENT);
  HFONT big = CreateFontW(34, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  HFONT old = static_cast<HFONT>(SelectObject(hdc, big));
  SetTextColor(hdc, RGB(255, 210, 110));
  TextOutW(hdc, x + 28, y + 30, title_.c_str(), static_cast<int>(title_.size()));
  SelectObject(hdc, old);
  DeleteObject(big);

  HFONT small_font = CreateFontW(22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  old = static_cast<HFONT>(SelectObject(hdc, small_font));
  SetTextColor(hdc, RGB(205, 225, 250));
  TextOutW(hdc, x + 28, y + 80, detail_.c_str(), static_cast<int>(detail_.size()));
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
  if (msg == WM_ERASEBKGND) return 1;
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
  if (hbmp_) {
    if (hdc_mem_ && hbmp_old_) SelectObject(hdc_mem_, hbmp_old_);
    DeleteObject(hbmp_);
    hbmp_ = nullptr;
    hbmp_old_ = nullptr;
  }
  if (hdc_mem_) {
    DeleteDC(hdc_mem_);
    hdc_mem_ = nullptr;
  }
  cache_w_ = cache_h_ = 0;
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
  // Live is burned into Present (same as Mini/Compact/ADV). Hide the GDI twin.
  set_visible(false);
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
  const double show_fps = state_.fps_smooth > 0 ? state_.fps_smooth : state_.fps;
  LiveStripLines strip{};
  fill_live_strip_lines(strip, state_.overlay_console, cons, state_.renodx_on || state_.dlss_on,
                        state_.nr_pass_count, state_.fg_on, state_.fg_multiplier, show_fps, state_.fps_fg_est,
                        state_.ft_stable, state_.fg_queue, state_.frame_ms, state_.fg_adaptive,
                        state_.fg_target_fps, state_.fg_flow_scale);
  const std::wstring cons_w = strip.has_console ? utf8_to_wide_hud(strip.console) : std::wstring{};
  const std::wstring dlss_w = utf8_to_wide_hud(strip.dlss);
  const std::wstring fg_w = utf8_to_wide_hud(strip.fg);
  const std::wstring fps_lab_w = utf8_to_wide_hud(strip.fps_lab.text);
  const std::wstring fps_cap_w = utf8_to_wide_hud(strip.fps_cap.text);
  const std::wstring fps_sep_w = strip.has_fg_fps ? utf8_to_wide_hud(strip.fps_sep.text) : std::wstring{};
  const std::wstring fps_fg_w = strip.has_fg_fps ? utf8_to_wide_hud(strip.fps_fg.text) : std::wstring{};
  const std::wstring ft_w = strip.ft[0] ? utf8_to_wide_hud(strip.ft) : std::wstring{};

  auto measure_w = [&](const std::wstring& s) -> int {
    if (s.empty()) return 0;
    SIZE sz{};
    GetTextExtentPoint32W(measure, s.c_str(), static_cast<int>(s.size()), &sz);
    return sz.cx;
  };
  const int sz_cons = measure_w(cons_w);
  const int sz_dlss = measure_w(dlss_w);
  const int sz_fg = measure_w(fg_w);
  const int sz_fps = measure_w(fps_lab_w) + measure_w(fps_cap_w) + measure_w(fps_sep_w) + measure_w(fps_fg_w);
  const int sz_ft = measure_w(ft_w);
  SelectObject(measure, old_m);
  DeleteDC(measure);

  const int ww = pad_x + sz_cons + (sz_cons > 0 ? gap : 0) + sz_dlss + gap + sz_fg + gap + sz_fps +
                 (sz_ft > 0 ? gap + sz_ft : 0) + gap + spark_w + pad_x;
  layout_w_ = ww;
  layout_h_ = hh;

  // Ensure cached DC and DIB exist with correct size
  HDC screen = GetDC(nullptr);
  void* bits = nullptr;
  
  if (!hdc_mem_ || cache_w_ != ww || cache_h_ != hh) {
    // Clean up old cache
    if (hbmp_) {
      if (hdc_mem_ && hbmp_old_) SelectObject(hdc_mem_, hbmp_old_);
      DeleteObject(hbmp_);
      hbmp_ = nullptr;
      hbmp_old_ = nullptr;
    }
    if (hdc_mem_) {
      DeleteDC(hdc_mem_);
      hdc_mem_ = nullptr;
    }
    
    // Create new cache
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = ww;
    bmi.bmiHeader.biHeight = -hh;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    
    hdc_mem_ = CreateCompatibleDC(screen);
    hbmp_ = CreateDIBSection(hdc_mem_, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbmp_ || !bits) {
      if (hbmp_) DeleteObject(hbmp_);
      if (hdc_mem_) DeleteDC(hdc_mem_);
      hdc_mem_ = nullptr;
      hbmp_ = nullptr;
      DeleteObject(font);
      ReleaseDC(nullptr, screen);
      return;
    }
    hbmp_old_ = static_cast<HBITMAP>(SelectObject(hdc_mem_, hbmp_));
    cache_w_ = ww;
    cache_h_ = hh;
  } else {
    // Reuse cached DIB, just get bits pointer
    DIBSECTION ds{};
    GetObjectW(hbmp_, sizeof(ds), &ds);
    bits = ds.dsBm.bmBits;
  }
  
  if (!hdc_mem_ || !bits) {
    DeleteObject(font);
    ReleaseDC(nullptr, screen);
    return;
  }
  
  HDC mem = hdc_mem_;
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
  unsigned char fgr = 255, fgg = 196, fgb = 72;
  live_fps_ft_rgb(state_.fg_on && state_.fg_multiplier >= 2 ? state_.fg_multiplier : 1, state_.ft_stable, fgr, fgg,
                  fgb);
  draw_seg(fg_w, RGB(fgr, fgg, fgb));
  auto draw_join = [&](const std::wstring& text, COLORREF color) {
    if (text.empty()) return;
    SIZE sz{};
    GetTextExtentPoint32W(mem, text.c_str(), static_cast<int>(text.size()), &sz);
    SetTextColor(mem, color);
    TextOutW(mem, x, text_y, text.c_str(), static_cast<int>(text.size()));
    x += sz.cx;
  };
  draw_join(fps_lab_w, RGB(strip.fps_lab.r, strip.fps_lab.g, strip.fps_lab.b));
  draw_join(fps_cap_w, RGB(strip.fps_cap.r, strip.fps_cap.g, strip.fps_cap.b));
  draw_join(fps_sep_w, RGB(strip.fps_sep.r, strip.fps_sep.g, strip.fps_sep.b));
  draw_join(fps_fg_w, RGB(strip.fps_fg.r, strip.fps_fg.g, strip.fps_fg.b));
  x += gap;
  if (!ft_w.empty()) {
    const OverlayFtStatus ft_st = overlay_ft_status(state_.ft_stable, state_.fg_on, state_.fg_multiplier,
                                                   show_fps, state_.target_fps, state_.fps_fg_est);
    const FgFtRgb ft_rgb = overlay_ft_status_rgb(ft_st);
    draw_seg(ft_w, RGB(ft_rgb.r, ft_rgb.g, ft_rgb.b));
  }

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

  // Don't delete mem or dib - they are cached
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
