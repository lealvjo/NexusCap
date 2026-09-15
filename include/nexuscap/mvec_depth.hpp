#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <string>

namespace nexuscap {

// GPU motion vectors (RG16F) from consecutive RGB frames + synthetic depth (R32F).
class MvecDepthGenerator {
public:
  bool init(ID3D11Device* device, std::string& err);
  void shutdown();
  bool ensure_size(ID3D11Device* device, int w, int h, std::string& err);

  // Writes mvec_ (RG) and depth_ textures. prev/curr are B8G8R8A8_UNORM (or compatible).
  bool generate(ID3D11DeviceContext* ctx, ID3D11Texture2D* curr_rgb, bool use_optical_flow,
                std::string& err);

  ID3D11Texture2D* mvec() const { return mvec_.Get(); }
  ID3D11Texture2D* depth() const { return depth_.Get(); }
  ID3D11Texture2D* prev_rgb() const { return prev_rgb_.Get(); }

private:
  bool create_resources(ID3D11Device* device, int w, int h, std::string& err);
  bool create_pipeline(ID3D11Device* device, std::string& err);

  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> prev_rgb_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> mvec_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> depth_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> mvec_rtv_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> depth_rtv_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> curr_srv_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> prev_srv_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_flow_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_zero_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_depth_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> samp_;
  int w_ = 0;
  int h_ = 0;
  bool have_prev_ = false;
};

}  // namespace nexuscap
