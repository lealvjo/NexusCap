#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "nexuscap/pipeline.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")

namespace {

constexpr int kFbW = nexuscap::kPipelineFbMaxW;
constexpr int kFbH = nexuscap::kPipelineFbMaxH;
int g_in_w = 1920;
int g_in_h = 1080;
std::uint32_t g_first_in = 0;

const char* kVs = R"(
void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
  float2 p = float2((id << 1) & 2, id & 2);
  uv = float2(p.x, p.y);
  pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
)";

const char* kPs = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  return tex.Sample(samp, uv);
}
)";

HWND g_hwnd = nullptr;
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11Texture2D* g_tex = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
ID3D11SamplerState* g_samp = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11Texture2D* g_staging = nullptr;
ID3D11Texture2D* g_in_gpu = nullptr;   // Shared input texture from host
ID3D11Texture2D* g_out_gpu = nullptr;  // Shared output texture to host
IDXGIKeyedMutex* g_km_in = nullptr;
IDXGIKeyedMutex* g_km_out = nullptr;
bool g_gpu_ok = false;
HANDLE g_in_map = nullptr;
HANDLE g_out_map = nullptr;
nexuscap::PipelineFbHeader* g_in = nullptr;
nexuscap::PipelineFbHeader* g_out = nullptr;
int g_ini_pass = 1;

std::wstring exe_dir() {
  wchar_t buf[MAX_PATH]{};
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::wstring p(buf);
  const auto slash = p.find_last_of(L"\\/");
  if (slash != std::wstring::npos) p.resize(slash);
  return p;
}

void write_utf8_file(const std::wstring& path, const std::string& body) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return;
  fwrite(body.data(), 1, body.size(), f);
  fclose(f);
}

int read_ini_pass() {
  std::ifstream in(exe_dir() + L"\\ReShade.ini", std::ios::binary);
  if (!in) return 1;
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string t = ss.str();
  const auto pos = t.find("NRPasses=");
  if (pos == std::string::npos) return 1;
  const int n = std::atoi(t.c_str() + pos + 9);
  return n < 1 ? 1 : n;
}

std::uint32_t* fb_pixels(nexuscap::PipelineFbHeader* h) {
  return reinterpret_cast<std::uint32_t*>(h + 1);
}

void fb_touch(nexuscap::PipelineFbHeader* h, int pass, std::uint32_t pid, std::uint32_t frame) {
  if (!h) return;
  h->magic = nexuscap::kPipelineFbMagic;
  h->width = static_cast<std::uint32_t>(g_in_w);
  h->height = static_cast<std::uint32_t>(g_in_h);
  h->pass = static_cast<std::uint32_t>(pass);
  h->frame = frame;
  h->pid = pid;
}

void write_ready(DWORD pid) {
  char line[64];
  std::snprintf(line, sizeof(line), "READY %lu\n", static_cast<unsigned long>(pid));
  write_utf8_file(exe_dir() + L"\\worker.ready", line);
}

bool open_maps() {
  const DWORD bytes = static_cast<DWORD>(nexuscap::pipeline_fb_bytes());
  g_in_map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, nexuscap::kPipelineFbInName);
  g_out_map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, nexuscap::kPipelineFbOutName);
  if (!g_in_map) {
    g_in_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, bytes,
                                  nexuscap::kPipelineFbInName);
  }
  if (!g_out_map) {
    g_out_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, bytes,
                                   nexuscap::kPipelineFbOutName);
  }
  if (!g_in_map || !g_out_map) return false;
  g_in = static_cast<nexuscap::PipelineFbHeader*>(
      MapViewOfFile(g_in_map, FILE_MAP_ALL_ACCESS, 0, 0, bytes));
  g_out = static_cast<nexuscap::PipelineFbHeader*>(
      MapViewOfFile(g_out_map, FILE_MAP_ALL_ACCESS, 0, 0, bytes));
  return g_in && g_out;
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  if (msg == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, w, l);
}

bool create_window(HINSTANCE inst) {
  WNDCLASSW wc{};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = inst;
  wc.lpszClassName = L"NexusCapPipelineWorker";
  RegisterClassW(&wc);
  g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"NexusCap Pipeline", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                           CW_USEDEFAULT, kFbW, kFbH, nullptr, nullptr, inst, nullptr);
  if (!g_hwnd) return false;
  ShowWindow(g_hwnd, SW_HIDE);
  return true;
}

bool compile_vs_ps() {
  ID3DBlob* vsb = nullptr;
  ID3DBlob* err = nullptr;
  if (FAILED(D3DCompile(kVs, strlen(kVs), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsb, &err))) {
    if (err) err->Release();
    return false;
  }
  if (err) err->Release();
  ID3DBlob* psb = nullptr;
  if (FAILED(D3DCompile(kPs, strlen(kPs), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psb, &err))) {
    vsb->Release();
    if (err) err->Release();
    return false;
  }
  if (err) err->Release();
  const bool ok =
      SUCCEEDED(g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs)) &&
      SUCCEEDED(g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps));
  vsb->Release();
  psb->Release();
  return ok;
}

bool create_d3d() {
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 2;
  sd.BufferDesc.Width = kFbW;
  sd.BufferDesc.Height = kFbH;
  sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = g_hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                                           D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, nullptr, &g_ctx))) {
    return false;
  }
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return false;
  const HRESULT rtv = g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
  bb->Release();
  if (FAILED(rtv)) return false;

  D3D11_TEXTURE2D_DESC td{};
  td.Width = kFbW;
  td.Height = kFbH;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DYNAMIC;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_tex))) return false;
  if (FAILED(g_dev->CreateShaderResourceView(g_tex, nullptr, &g_srv))) return false;

  D3D11_SAMPLER_DESC sm{};
  sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  if (FAILED(g_dev->CreateSamplerState(&sm, &g_samp))) return false;

  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_staging))) return false;
  return compile_vs_ps();
}

void upload_in() {
  if (!g_in || g_in->magic != nexuscap::kPipelineFbMagic) return;
  if (g_in->width >= 16 && g_in->height >= 16) {
    g_in_w = static_cast<int>((std::min)(g_in->width, static_cast<std::uint32_t>(kFbW)));
    g_in_h = static_cast<int>((std::min)(g_in->height, static_cast<std::uint32_t>(kFbH)));
  }
  if (!g_first_in) g_first_in = g_in->frame ? g_in->frame : 1;
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(g_ctx->Map(g_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
  const auto* src = fb_pixels(g_in);
  auto* dst = static_cast<std::uint8_t*>(m.pData);
  for (int y = 0; y < g_in_h; ++y) {
    memcpy(dst + static_cast<std::size_t>(y) * m.RowPitch, src + static_cast<std::size_t>(y) * g_in_w,
           static_cast<std::size_t>(g_in_w) * 4u);
  }
  g_ctx->Unmap(g_tex, 0);
}

bool upload_in_gpu() {
  if (!g_in || g_in->magic != nexuscap::kPipelineFbMagic) return false;
  if (!g_gpu_ok || !g_in_gpu || !g_km_in) return false;
  
  if (g_in->width >= 16 && g_in->height >= 16) {
    g_in_w = static_cast<int>((std::min)(g_in->width, static_cast<std::uint32_t>(kFbW)));
    g_in_h = static_cast<int>((std::min)(g_in->height, static_cast<std::uint32_t>(kFbH)));
  }
  if (!g_first_in) g_first_in = g_in->frame ? g_in->frame : 1;
  
  // Acquire input mutex briefly - host should have released it
  if (FAILED(g_km_in->AcquireSync(0, 5))) return false;
  
  // Copy shared input texture to render texture
  g_ctx->CopyResource(g_tex, g_in_gpu);
  
  // Release immediately so host can write next frame
  g_km_in->ReleaseSync(0);
  return true;
}

void publish_out() {
  if (!g_out || !g_swap) return;
  if (!g_in || g_in->magic != nexuscap::kPipelineFbMagic) return;
  if (!g_first_in || g_in->frame < g_first_in + 2) return;
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb) return;
  g_ctx->CopyResource(g_staging, bb);
  bb->Release();
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(g_ctx->Map(g_staging, 0, D3D11_MAP_READ, 0, &m))) return;
  auto* dst = fb_pixels(g_out);
  const auto* src = static_cast<const std::uint8_t*>(m.pData);
  for (int y = 0; y < g_in_h; ++y) {
    memcpy(dst + static_cast<std::size_t>(y) * g_in_w, src + static_cast<std::size_t>(y) * m.RowPitch,
           static_cast<std::size_t>(g_in_w) * 4u);
  }
  g_ctx->Unmap(g_staging, 0);
  const std::uint32_t frame = g_in->frame;
  fb_touch(g_out, g_ini_pass, GetCurrentProcessId(), frame);
  g_out->gpu = 0;  // CPU path output
}

bool publish_out_gpu() {
  if (!g_out || !g_swap) return false;
  if (!g_in || g_in->magic != nexuscap::kPipelineFbMagic) return false;
  if (!g_first_in || g_in->frame < g_first_in + 2) return false;
  if (!g_gpu_ok || !g_out_gpu || !g_km_out) return false;
  
  // Acquire output mutex briefly
  if (FAILED(g_km_out->AcquireSync(0, 5))) return false;
  
  // Copy backbuffer to shared output texture
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb) {
    g_km_out->ReleaseSync(0);
    return false;
  }
  g_ctx->CopyResource(g_out_gpu, bb);
  bb->Release();
  
  const std::uint32_t frame = g_in->frame;
  fb_touch(g_out, g_ini_pass, GetCurrentProcessId(), frame);
  g_out->gpu = 1;  // GPU path output
  
  // Release so host can read result
  g_km_out->ReleaseSync(0);
  return true;
}

void teardown() {
  if (g_in) UnmapViewOfFile(g_in);
  if (g_out) UnmapViewOfFile(g_out);
  if (g_in_map) CloseHandle(g_in_map);
  if (g_out_map) CloseHandle(g_out_map);
  if (g_km_out) g_km_out->Release();
  if (g_km_in) g_km_in->Release();
  if (g_out_gpu) g_out_gpu->Release();
  if (g_in_gpu) g_in_gpu->Release();
  if (g_staging) g_staging->Release();
  if (g_samp) g_samp->Release();
  if (g_srv) g_srv->Release();
  if (g_tex) g_tex->Release();
  if (g_ps) g_ps->Release();
  if (g_vs) g_vs->Release();
  if (g_rtv) g_rtv->Release();
  if (g_swap) g_swap->Release();
  if (g_ctx) g_ctx->Release();
  if (g_dev) g_dev->Release();
}

bool open_shared_textures() {
  if (!g_in || !g_dev) return false;
  if (g_in->gpu == 0 || g_in->in_shared == 0 || g_in->out_shared == 0) return false;
  
  // Release old shared resources if any
  if (g_km_out) {
    g_km_out->Release();
    g_km_out = nullptr;
  }
  if (g_km_in) {
    g_km_in->Release();
    g_km_in = nullptr;
  }
  if (g_out_gpu) {
    g_out_gpu->Release();
    g_out_gpu = nullptr;
  }
  if (g_in_gpu) {
    g_in_gpu->Release();
    g_in_gpu = nullptr;
  }
  g_gpu_ok = false;
  
  // Open shared input texture
  HANDLE in_h = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(g_in->in_shared));
  ID3D11Device1* dev1 = nullptr;
  if (FAILED(g_dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&dev1))) || !dev1) {
    return false;
  }
  
  ID3D11Texture2D* in_tex = nullptr;
  HRESULT hr = dev1->OpenSharedResource1(in_h, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&in_tex));
  dev1->Release();
  
  if (FAILED(hr) || !in_tex) return false;
  
  // Get keyed mutex for input
  IDXGIKeyedMutex* km_in = nullptr;
  if (FAILED(in_tex->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&km_in))) || !km_in) {
    in_tex->Release();
    return false;
  }
  
  // Open shared output texture
  HANDLE out_h = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(g_in->out_shared));
  if (FAILED(g_dev->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&dev1))) || !dev1) {
    km_in->Release();
    in_tex->Release();
    return false;
  }
  
  ID3D11Texture2D* out_tex = nullptr;
  hr = dev1->OpenSharedResource1(out_h, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&out_tex));
  dev1->Release();
  
  if (FAILED(hr) || !out_tex) {
    km_in->Release();
    in_tex->Release();
    return false;
  }
  
  // Get keyed mutex for output
  IDXGIKeyedMutex* km_out = nullptr;
  if (FAILED(out_tex->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&km_out))) || !km_out) {
    out_tex->Release();
    km_in->Release();
    in_tex->Release();
    return false;
  }
  
  g_in_gpu = in_tex;
  g_out_gpu = out_tex;
  g_km_in = km_in;
  g_km_out = km_out;
  g_gpu_ok = true;
  return true;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
  g_ini_pass = read_ini_pass();
  open_maps();
  if (!create_window(inst) || !create_d3d()) return 1;

  D3D11_VIEWPORT vp{};
  vp.Width = static_cast<float>(kFbW);
  vp.Height = static_cast<float>(kFbH);
  vp.MaxDepth = 1;
  bool ready = false;
  std::uint32_t last_done = 0;
  bool have_done = false;
  MSG msg{};
  for (;;) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        teardown();
        return 0;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    // So o filho atual processa. O velho no recycle nao corre o mesmo quadro.
    if (!g_in || g_in->magic != nexuscap::kPipelineFbMagic ||
        (g_in->pid != 0 && g_in->pid != GetCurrentProcessId()) ||
        (have_done && g_in->frame == last_done)) {
      Sleep(1);
      continue;
    }
    
    // Try to open shared textures if GPU path is signaled
    if (g_in->gpu == 1 && !g_gpu_ok) {
      open_shared_textures();
    }
    
    // Use GPU path if available, fallback to CPU
    bool uploaded = false;
    if (g_gpu_ok && g_in->gpu == 1) {
      uploaded = upload_in_gpu();
    }
    if (!uploaded) {
      upload_in();
    }
    
    vp.Width = static_cast<float>(kFbW);
    vp.Height = static_cast<float>(kFbH);
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &g_srv);
    g_ctx->PSSetSamplers(0, 1, &g_samp);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->Draw(3, 0);
    g_swap->Present(0, 0);
    
    // Publish output via GPU or CPU
    bool published = false;
    if (g_gpu_ok && g_in->gpu == 1) {
      published = publish_out_gpu();
    }
    if (!published) {
      publish_out();
    }
    
    last_done = g_in->frame;
    have_done = true;
    if (!ready && g_out && g_out->pid == GetCurrentProcessId() && g_first_in &&
        g_out->frame >= g_first_in + 2) {
      write_ready(GetCurrentProcessId());
      ready = true;
    }
  }
}
