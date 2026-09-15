#include "nexuscap/mvec_depth.hpp"

#include <d3dcompiler.h>

#include <cstring>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace nexuscap {
namespace {

static const char* kVs = R"(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID) {
  VSOut o;
  float2 p = float2((id << 1) & 2, id & 2);
  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
  o.uv = p;
  return o;
}
)";

// Cheap dense optical-flow approximation: search small diamond for min SAD in luma.
static const char* kPsFlow = R"(
Texture2D currTex : register(t0);
Texture2D prevTex : register(t1);
SamplerState samp : register(s0);

float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
  float2 texel;
  currTex.GetDimensions(texel.x, texel.y);
  texel = 1.0 / texel;

  float lc = luma(currTex.SampleLevel(samp, uv, 0).rgb);
  float best = 1e9;
  float2 bestMv = 0;
  // radius 2 in pixels → 5x5 search (fast, good enough for temporal enhance)
  [unroll] for (int y = -2; y <= 2; ++y) {
    [unroll] for (int x = -2; x <= 2; ++x) {
      float2 d = float2(x, y);
      float lp = luma(prevTex.SampleLevel(samp, uv + d * texel, 0).rgb);
      float sad = abs(lc - lp);
      if (sad < best) { best = sad; bestMv = d; }
    }
  }
  // pixel-space motion (prev -> curr). DLSS expects mvec in pixel space with scale.
  return float4(bestMv, 0, 1);
}
)";

static const char* kPsZero = R"(
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
  return float4(0, 0, 0, 1);
}
)";

static const char* kPsDepth = R"(
float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_Target {
  // Synthetic far plane (1.0). Inverted depth flag will be set false.
  return float4(1, 0, 0, 1);
}
)";

bool compile_shader(const char* src, const char* entry, const char* target, ComPtr<ID3DBlob>& blob,
                    std::string& err) {
  ComPtr<ID3DBlob> err_blob;
  HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, entry, target, 0, 0, &blob,
                          &err_blob);
  if (FAILED(hr)) {
    err = err_blob ? static_cast<const char*>(err_blob->GetBufferPointer()) : "D3DCompile failed";
    return false;
  }
  return true;
}

}  // namespace

bool MvecDepthGenerator::init(ID3D11Device* device, std::string& err) {
  shutdown();
  if (!device) {
    err = "null device";
    return false;
  }
  device_ = device;
  return create_pipeline(device, err);
}

void MvecDepthGenerator::shutdown() {
  samp_.Reset();
  ps_depth_.Reset();
  ps_zero_.Reset();
  ps_flow_.Reset();
  vs_.Reset();
  prev_srv_.Reset();
  curr_srv_.Reset();
  depth_rtv_.Reset();
  mvec_rtv_.Reset();
  depth_.Reset();
  mvec_.Reset();
  prev_rgb_.Reset();
  device_.Reset();
  w_ = h_ = 0;
  have_prev_ = false;
}

bool MvecDepthGenerator::create_pipeline(ID3D11Device* device, std::string& err) {
  ComPtr<ID3DBlob> vsb, psb;
  if (!compile_shader(kVs, "main", "vs_5_0", vsb, err)) return false;
  if (FAILED(device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_))) {
    err = "CreateVertexShader failed";
    return false;
  }
  if (!compile_shader(kPsFlow, "main", "ps_5_0", psb, err)) return false;
  if (FAILED(device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_flow_))) {
    err = "CreatePixelShader flow failed";
    return false;
  }
  psb.Reset();
  if (!compile_shader(kPsZero, "main", "ps_5_0", psb, err)) return false;
  if (FAILED(device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_zero_))) {
    err = "CreatePixelShader zero failed";
    return false;
  }
  psb.Reset();
  if (!compile_shader(kPsDepth, "main", "ps_5_0", psb, err)) return false;
  if (FAILED(device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_depth_))) {
    err = "CreatePixelShader depth failed";
    return false;
  }
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  if (FAILED(device->CreateSamplerState(&sd, &samp_))) {
    err = "CreateSamplerState failed";
    return false;
  }
  return true;
}

bool MvecDepthGenerator::create_resources(ID3D11Device* device, int w, int h, std::string& err) {
  prev_srv_.Reset();
  curr_srv_.Reset();
  depth_rtv_.Reset();
  mvec_rtv_.Reset();
  depth_.Reset();
  mvec_.Reset();
  prev_rgb_.Reset();
  have_prev_ = false;

  D3D11_TEXTURE2D_DESC td{};
  td.Width = static_cast<UINT>(w);
  td.Height = static_cast<UINT>(h);
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  if (FAILED(device->CreateTexture2D(&td, nullptr, &prev_rgb_))) {
    err = "create prev_rgb failed";
    return false;
  }

  td.Format = DXGI_FORMAT_R16G16_FLOAT;
  if (FAILED(device->CreateTexture2D(&td, nullptr, &mvec_))) {
    err = "create mvec failed";
    return false;
  }
  if (FAILED(device->CreateRenderTargetView(mvec_.Get(), nullptr, &mvec_rtv_))) {
    err = "create mvec rtv failed";
    return false;
  }

  td.Format = DXGI_FORMAT_R32_FLOAT;
  if (FAILED(device->CreateTexture2D(&td, nullptr, &depth_))) {
    err = "create depth failed";
    return false;
  }
  if (FAILED(device->CreateRenderTargetView(depth_.Get(), nullptr, &depth_rtv_))) {
    err = "create depth rtv failed";
    return false;
  }

  w_ = w;
  h_ = h;
  return true;
}

bool MvecDepthGenerator::ensure_size(ID3D11Device* device, int w, int h, std::string& err) {
  if (w_ == w && h_ == h && mvec_ && depth_ && prev_rgb_) return true;
  return create_resources(device, w, h, err);
}

bool MvecDepthGenerator::generate(ID3D11DeviceContext* ctx, ID3D11Texture2D* curr_rgb, bool use_optical_flow,
                                  std::string& err) {
  if (!ctx || !curr_rgb || !mvec_ || !depth_) {
    err = "mvec generator not ready";
    return false;
  }

  // Depth always
  {
    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(w_);
    vp.Height = static_cast<float>(h_);
    vp.MaxDepth = 1.f;
    ctx->OMSetRenderTargets(1, depth_rtv_.GetAddressOf(), nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(ps_depth_.Get(), nullptr, 0);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->Draw(3, 0);
  }

  curr_srv_.Reset();
  prev_srv_.Reset();
  if (FAILED(device_->CreateShaderResourceView(curr_rgb, nullptr, &curr_srv_))) {
    err = "curr srv failed";
    return false;
  }

  const bool can_flow = use_optical_flow && have_prev_;
  if (can_flow) {
    if (FAILED(device_->CreateShaderResourceView(prev_rgb_.Get(), nullptr, &prev_srv_))) {
      err = "prev srv failed";
      return false;
    }
  }

  {
    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(w_);
    vp.Height = static_cast<float>(h_);
    vp.MaxDepth = 1.f;
    ctx->OMSetRenderTargets(1, mvec_rtv_.GetAddressOf(), nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(can_flow ? ps_flow_.Get() : ps_zero_.Get(), nullptr, 0);
    if (can_flow) {
      ID3D11ShaderResourceView* srvs[] = {curr_srv_.Get(), prev_srv_.Get()};
      ctx->PSSetShaderResources(0, 2, srvs);
      ctx->PSSetSamplers(0, 1, samp_.GetAddressOf());
    }
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, nulls);
  }

  // Save current as previous for next frame
  ctx->CopyResource(prev_rgb_.Get(), curr_rgb);
  have_prev_ = true;
  return true;
}

}  // namespace nexuscap
