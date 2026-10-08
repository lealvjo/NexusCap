#include "nexuscap/capture.hpp"
#include "nexuscap/settings_store.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <dispatcherqueue.h>

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

#undef GetCurrentTime

#include <unknwn.h>
#include <inspectable.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.System.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::SizeInt32;

namespace nexuscap {
namespace {

std::string wide_to_utf8(const wchar_t* w) {
  if (!w || !w[0]) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return {};
  std::string s(static_cast<size_t>(n - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
  return s;
}

std::string hresult_hex(HRESULT hr) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(static_cast<uint32_t>(hr)));
  return buf;
}

bool window_is_cloaked(HWND hwnd) {
  int cloaked = 0;
  return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

struct EnumCtx {
  std::vector<CapturableWindowInfo>* out = nullptr;
  uint64_t exclude = 0;
};

BOOL CALLBACK enum_capturable(HWND hwnd, LPARAM lp) {
  auto* ctx = reinterpret_cast<EnumCtx*>(lp);
  if (!ctx || !ctx->out) return TRUE;
  if (ctx->exclude != 0 && reinterpret_cast<uint64_t>(hwnd) == ctx->exclude) return TRUE;
  wchar_t title_w[512]{};
  wchar_t class_w[256]{};
  GetWindowTextW(hwnd, title_w, 512);
  GetClassNameW(hwnd, class_w, 256);
  const std::string title = wide_to_utf8(title_w);
  const std::string cls = wide_to_utf8(class_w);
  const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  const bool toolwindow = (ex & WS_EX_TOOLWINDOW) != 0;
  const bool owned = GetWindow(hwnd, GW_OWNER) != nullptr;
  const bool is_self = window_capture_is_self_class(cls.c_str()) || window_capture_is_self_title(title.c_str());
  if (!window_capture_list_candidate(IsWindowVisible(hwnd) != FALSE, IsIconic(hwnd) != FALSE, toolwindow, owned,
                                     window_is_cloaked(hwnd), is_self, title.c_str())) {
    return TRUE;
  }
  CapturableWindowInfo info;
  info.hwnd = reinterpret_cast<uint64_t>(hwnd);
  info.title = title;
  info.class_name = cls;
  ctx->out->push_back(std::move(info));
  return TRUE;
}

struct LargestCtx {
  DWORD pid = 0;
  HWND best = nullptr;
  long area = 0;
};

BOOL CALLBACK enum_largest_in_process(HWND hwnd, LPARAM lp) {
  auto* ctx = reinterpret_cast<LargestCtx*>(lp);
  if (!ctx || !ctx->pid) return TRUE;
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != ctx->pid) return TRUE;
  wchar_t class_w[256]{};
  GetClassNameW(hwnd, class_w, 256);
  const std::string cls = wide_to_utf8(class_w);
  const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  const bool toolwindow = (ex & WS_EX_TOOLWINDOW) != 0;
  const bool is_self = window_capture_is_self_class(cls.c_str());
  RECT rc{};
  GetClientRect(hwnd, &rc);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (!window_capture_process_view_candidate(IsWindowVisible(hwnd) != FALSE, IsIconic(hwnd) != FALSE, toolwindow,
                                             window_is_cloaked(hwnd), is_self, w, h)) {
    return TRUE;
  }
  const long area = static_cast<long>(w) * static_cast<long>(h);
  if (area > ctx->area) {
    ctx->area = area;
    ctx->best = hwnd;
  }
  return TRUE;
}

HWND largest_window_in_process(HWND seed) {
  if (!seed || !IsWindow(seed)) return nullptr;
  DWORD pid = 0;
  GetWindowThreadProcessId(seed, &pid);
  if (pid == 0) return seed;
  LargestCtx ctx;
  ctx.pid = pid;
  ctx.best = seed;
  RECT rc{};
  if (GetClientRect(seed, &rc)) {
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w >= 64 && h >= 64) ctx.area = static_cast<long>(w) * static_cast<long>(h);
  }
  EnumWindows(enum_largest_in_process, reinterpret_cast<LPARAM>(&ctx));
  return ctx.best ? ctx.best : seed;
}

void push_unique_hwnd(std::vector<HWND>& out, HWND hwnd) {
  if (!hwnd || !IsWindow(hwnd)) return;
  for (HWND have : out) {
    if (have == hwnd) return;
  }
  out.push_back(hwnd);
}

std::vector<HWND> collect_capture_hwnds(HWND seed) {
  std::vector<HWND> out;
  const HWND largest = largest_window_in_process(seed);
  push_unique_hwnd(out, largest);
  push_unique_hwnd(out, seed);
  if (seed) {
    push_unique_hwnd(out, GetAncestor(seed, GA_ROOT));
    push_unique_hwnd(out, GetWindow(seed, GW_OWNER));
  }
  return out;
}

void ensure_dispatcher_queue(winrt::Windows::System::DispatcherQueueController& controller) {
  if (winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) return;
  DispatcherQueueOptions options{sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
  HRESULT hr = CreateDispatcherQueueController(
      options, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(controller)));
  if (SUCCEEDED(hr) && controller) return;
  controller = nullptr;
  options.apartmentType = DQTAT_COM_STA;
  CreateDispatcherQueueController(
      options, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(controller)));
}

winrt::com_ptr<ID3D11Texture2D> texture_from_surface(
    const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface& surface) {
  winrt::com_ptr<ID3D11Texture2D> tex;
  auto access = surface.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
  winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(tex.put())));
  return tex;
}

IDirect3DDevice wrap_d3d_device(ID3D11Device* d3d) {
  winrt::com_ptr<IDXGIDevice> dxgi;
  winrt::check_hresult(d3d->QueryInterface(IID_PPV_ARGS(dxgi.put())));
  winrt::com_ptr<::IInspectable> inspectable;
  winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
  return inspectable.as<IDirect3DDevice>();
}

SizeInt32 hwnd_capture_size(HWND hwnd) {
  SizeInt32 size{2, 2};
  RECT rc{};
  if (hwnd && GetClientRect(hwnd, &rc)) {
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w >= 2) size.Width = w;
    if (h >= 2) size.Height = h;
  }
  return size;
}

void pump_ui_briefly() {
  MSG msg{};
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

GraphicsCaptureItem item_from_hwnd(HWND hwnd) {
  auto factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
  GraphicsCaptureItem item{nullptr};
  winrt::check_hresult(factory->CreateForWindow(
      hwnd, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), winrt::put_abi(item)));
  return item;
}

void copy_mapped_nv12(const D3D11_MAPPED_SUBRESOURCE& mapped, int w, int h, uint8_t* dst) {
  if (!mapped.pData || !dst) return;
  copy_nv12_from_pitch(static_cast<const uint8_t*>(mapped.pData), static_cast<int>(mapped.RowPitch), w, h, dst);
}

}  // namespace

void note_window_boot_line(const char* msg) {
  if (!msg || !msg[0]) return;
  std::printf("%s\n", msg);
  OutputDebugStringA(msg);
  OutputDebugStringA("\n");
  const std::string path = app_logs_dir() + "\\window_boot.txt";
  FILE* f = nullptr;
  if (fopen_s(&f, path.c_str(), "a") == 0 && f) {
    std::fputs(msg, f);
    std::fputc('\n', f);
    std::fclose(f);
  }
}

std::vector<CapturableWindowInfo> list_capturable_windows(uint64_t exclude_hwnd) {
  std::vector<CapturableWindowInfo> out;
  EnumCtx ctx{&out, exclude_hwnd};
  EnumWindows(enum_capturable, reinterpret_cast<LPARAM>(&ctx));
  return out;
}

uint64_t find_capturable_hwnd(const std::string& title, uint64_t exclude_hwnd) {
  if (title.empty()) return 0;
  const auto list = list_capturable_windows(exclude_hwnd);
  for (const auto& w : list) {
    if (w.title == title) return w.hwnd;
  }
  for (const auto& w : list) {
    if (window_title_matches_capture(w.title, title)) return w.hwnd;
  }
  return 0;
}

struct WindowCapture::Impl {
  FrameSlot* slot = nullptr;
  IClock* clock = nullptr;
  HWND hwnd = nullptr;
  winrt::com_ptr<ID3D11Device> d3d;
  winrt::com_ptr<ID3D11DeviceContext> ctx;
  winrt::com_ptr<ID3D11Texture2D> staging;
  UINT staging_w = 0;
  UINT staging_h = 0;
  // Duas cópias. O callback só escreve na livre. O loop lê a anterior, já pronta na GPU.
  struct HeldCopy {
    winrt::com_ptr<ID3D11Texture2D> tex;
    UINT tex_w = 0;
    UINT tex_h = 0;
    UINT src_w = 0;
    UINT src_h = 0;
    int w = 0;
    int h = 0;
    double arrived_ms = 0;
    uint64_t seq = 0;
    int state = 0;  // 0 livre, 1 cheia, 2 em conversão
  };
  HeldCopy copies_[2]{};
  uint64_t copy_seq_ = 0;
  std::mutex copy_mu;
  std::thread convert;
  std::atomic<bool> convert_stop{false};
  bool boot_log = false;
  int maps_logged = 0;
  bool copy_logged = false;
  winrt::com_ptr<ID3D11VideoDevice> video_dev;
  winrt::com_ptr<ID3D11VideoContext> video_ctx;
  winrt::com_ptr<ID3D11VideoProcessorEnumerator> video_enum;
  winrt::com_ptr<ID3D11VideoProcessor> video_proc;
  winrt::com_ptr<ID3D11Texture2D> bgra_gpu;
  winrt::com_ptr<ID3D11Texture2D> nv12_gpu;
  winrt::com_ptr<ID3D11Texture2D> nv12_staging;
  winrt::com_ptr<ID3D11VideoProcessorInputView> video_in;
  winrt::com_ptr<ID3D11VideoProcessorOutputView> video_out;
  UINT gpu_w = 0;
  UINT gpu_h = 0;
  bool video_ok = false;
  // Dois BGRA compartilhados. A Janela publica o handle. O Present abre no outro device.
  struct SharedBgra {
    winrt::com_ptr<ID3D11Texture2D> tex;
    winrt::com_ptr<IDXGIKeyedMutex> mtx;
    HANDLE handle = nullptr;
    UINT w = 0;
    UINT h = 0;
  };
  SharedBgra shared_bgra_[2]{};
  IDirect3DDevice winrt_d3d{nullptr};
  GraphicsCaptureItem item{nullptr};
  Direct3D11CaptureFramePool pool{nullptr};
  GraphicsCaptureSession session{nullptr};
  winrt::event_token arrived{};
  winrt::event_token closed{};
  std::mutex mu;
  std::vector<uint8_t> nv12;
  std::atomic<bool>* running = nullptr;
  std::atomic<uint64_t>* frames = nullptr;
  std::atomic<int>* out_w = nullptr;
  std::atomic<int>* out_h = nullptr;
  std::thread worker;
  DWORD worker_tid = 0;
  std::mutex start_mu;
  std::condition_variable start_cv;
  std::atomic<bool> start_signaled{false};
  std::string start_err;
  bool opening = false;
  double capture_fps = 60.0;

  void reset_video() {
    video_in = nullptr;
    video_out = nullptr;
    video_proc = nullptr;
    video_enum = nullptr;
    bgra_gpu = nullptr;
    nv12_gpu = nullptr;
    nv12_staging = nullptr;
    video_ctx = nullptr;
    video_dev = nullptr;
    gpu_w = 0;
    gpu_h = 0;
    video_ok = false;
  }

  void reset_pool() {
    std::lock_guard<std::mutex> lock(mu);
    if (pool && arrived) {
      try {
        pool.FrameArrived(arrived);
      } catch (...) {
      }
      arrived = {};
    }
    if (item && closed) {
      try {
        item.Closed(closed);
      } catch (...) {
      }
      closed = {};
    }
    if (session) {
      try {
        session.Close();
      } catch (...) {
      }
      session = nullptr;
    }
    if (pool) {
      try {
        pool.Close();
      } catch (...) {
      }
      pool = nullptr;
    }
    item = nullptr;
    winrt_d3d = nullptr;
    staging = nullptr;
    staging_w = 0;
    staging_h = 0;
    copies_[0] = {};
    copies_[1] = {};
    copy_seq_ = 0;
    reset_shared_bgra();
    maps_logged = 0;
    copy_logged = false;
    reset_video();
    ctx = nullptr;
    d3d = nullptr;
  }

  bool ensure_staging(UINT w, UINT h) {
    if (staging && staging_w == w && staging_h == h) return true;
    staging = nullptr;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(d3d->CreateTexture2D(&td, nullptr, staging.put()))) return false;
    staging_w = w;
    staging_h = h;
    return true;
  }

  bool init_video() {
    video_ok = false;
    if (!d3d || !ctx) return false;
    if (FAILED(d3d->QueryInterface(IID_PPV_ARGS(video_dev.put())))) return false;
    if (FAILED(ctx->QueryInterface(IID_PPV_ARGS(video_ctx.put())))) {
      video_dev = nullptr;
      return false;
    }
    video_ok = true;
    return true;
  }

  bool ensure_gpu_nv12(UINT w, UINT h) {
    if (!video_ok || !video_dev || !d3d) return false;
    if (nv12_gpu && gpu_w == w && gpu_h == h && video_proc && video_in && video_out) return true;
    video_in = nullptr;
    video_out = nullptr;
    video_proc = nullptr;
    video_enum = nullptr;
    bgra_gpu = nullptr;
    nv12_gpu = nullptr;
    nv12_staging = nullptr;
    gpu_w = 0;
    gpu_h = 0;

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = w;
    cd.InputHeight = h;
    cd.OutputWidth = w;
    cd.OutputHeight = h;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(video_dev->CreateVideoProcessorEnumerator(&cd, video_enum.put()))) return false;
    UINT in_flags = 0;
    UINT out_flags = 0;
    if (FAILED(video_enum->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &in_flags))) return false;
    if (FAILED(video_enum->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &out_flags))) return false;
    if ((in_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0) return false;
    if ((out_flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) return false;
    if (FAILED(video_dev->CreateVideoProcessor(video_enum.get(), 0, video_proc.put()))) return false;

    D3D11_TEXTURE2D_DESC bgra{};
    bgra.Width = w;
    bgra.Height = h;
    bgra.MipLevels = 1;
    bgra.ArraySize = 1;
    bgra.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bgra.SampleDesc.Count = 1;
    bgra.Usage = D3D11_USAGE_DEFAULT;
    bgra.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(d3d->CreateTexture2D(&bgra, nullptr, bgra_gpu.put()))) return false;

    D3D11_TEXTURE2D_DESC nv{};
    nv.Width = w;
    nv.Height = h;
    nv.MipLevels = 1;
    nv.ArraySize = 1;
    nv.Format = DXGI_FORMAT_NV12;
    nv.SampleDesc.Count = 1;
    nv.Usage = D3D11_USAGE_DEFAULT;
    nv.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(d3d->CreateTexture2D(&nv, nullptr, nv12_gpu.put()))) {
      nv.BindFlags = D3D11_BIND_RENDER_TARGET;
      if (FAILED(d3d->CreateTexture2D(&nv, nullptr, nv12_gpu.put()))) return false;
    }

    D3D11_TEXTURE2D_DESC st{};
    st.Width = w;
    st.Height = h;
    st.MipLevels = 1;
    st.ArraySize = 1;
    st.Format = DXGI_FORMAT_NV12;
    st.SampleDesc.Count = 1;
    st.Usage = D3D11_USAGE_STAGING;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(d3d->CreateTexture2D(&st, nullptr, nv12_staging.put()))) return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    if (FAILED(video_dev->CreateVideoProcessorInputView(bgra_gpu.get(), video_enum.get(), &ivd, video_in.put()))) {
      return false;
    }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (FAILED(video_dev->CreateVideoProcessorOutputView(nv12_gpu.get(), video_enum.get(), &ovd, video_out.put()))) {
      return false;
    }
    gpu_w = w;
    gpu_h = h;
    return true;
  }

  bool gpu_to_nv12(ID3D11Texture2D* bgra, UINT w, UINT h, uint8_t* dst) {
    if (!bgra || !dst || !video_ok || !video_ctx) return false;
    if (!ensure_gpu_nv12(w, h)) {
      video_ok = false;
      return false;
    }
    ctx->CopyResource(bgra_gpu.get(), bgra);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = video_in.get();
    if (FAILED(video_ctx->VideoProcessorBlt(video_proc.get(), video_out.get(), 0, 1, &stream))) {
      video_ok = false;
      return false;
    }
    ctx->CopyResource(nv12_staging.get(), nv12_gpu.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(nv12_staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    copy_mapped_nv12(mapped, static_cast<int>(w), static_cast<int>(h), dst);
    ctx->Unmap(nv12_staging.get(), 0);
    return true;
  }

  bool cpu_to_nv12(ID3D11Texture2D* bgra, UINT src_w, UINT src_h, int w, int h, uint8_t* dst) {
    if (!bgra || !dst) return false;
    if (!ensure_staging(src_w, src_h)) return false;
    ctx->CopyResource(staging.get(), bgra);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    bgra_to_nv12(static_cast<const uint8_t*>(mapped.pData), static_cast<int>(mapped.RowPitch), w, h, dst);
    ctx->Unmap(staging.get(), 0);
    return true;
  }

  void reset_shared_bgra() {
    for (SharedBgra& s : shared_bgra_) {
      s.mtx = nullptr;
      s.tex = nullptr;
      s.handle = nullptr;
      s.w = 0;
      s.h = 0;
    }
  }

  bool ensure_shared_bgra(SharedBgra& s, UINT w, UINT h) {
    if (s.tex && s.mtx && s.handle && s.w == w && s.h == h) return true;
    s.mtx = nullptr;
    s.tex = nullptr;
    s.handle = nullptr;
    s.w = 0;
    s.h = 0;
    if (!d3d || w < 2 || h < 2) return false;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    if (FAILED(d3d->CreateTexture2D(&td, nullptr, s.tex.put()))) return false;
    if (FAILED(s.tex->QueryInterface(IID_PPV_ARGS(s.mtx.put()))) || !s.mtx) {
      s.tex = nullptr;
      return false;
    }
    winrt::com_ptr<IDXGIResource> res;
    HANDLE handle = nullptr;
    if (FAILED(s.tex->QueryInterface(IID_PPV_ARGS(res.put()))) || !res ||
        FAILED(res->GetSharedHandle(&handle)) || !handle) {
      s.mtx = nullptr;
      s.tex = nullptr;
      return false;
    }
    s.handle = handle;
    s.w = w;
    s.h = h;
    return true;
  }

  void publish_bgra(HANDLE handle, int w, int h, double arrived_ms) {
    FramePayload p;
    p.bgra_shared = reinterpret_cast<std::uintptr_t>(handle);
    p.width = w;
    p.height = h;
    p.cap_qpc_ms = arrived_ms > 0.0 ? arrived_ms : (clock ? clock->now_ms() : 0);
    p.frame_index = frames->fetch_add(1, std::memory_order_relaxed);
    out_w->store(w, std::memory_order_relaxed);
    out_h->store(h, std::memory_order_relaxed);
    if (slot) slot->publish(std::move(p));
  }

  void publish_nv12(int w, int h, double arrived_ms) {
    FramePayload p;
    p.nv12.swap(nv12);
    p.width = w;
    p.height = h;
    p.cap_qpc_ms = arrived_ms > 0.0 ? arrived_ms : (clock ? clock->now_ms() : 0);
    p.frame_index = frames->fetch_add(1, std::memory_order_relaxed);
    out_w->store(w, std::memory_order_relaxed);
    out_h->store(h, std::memory_order_relaxed);
    if (slot) slot->publish(std::move(p));
  }

  void boot_line(const char* msg) const {
    if (boot_log) note_window_boot_line(msg);
  }

  bool ensure_copy_tex(HeldCopy& dst, UINT w, UINT h) {
    if (dst.tex && dst.tex_w == w && dst.tex_h == h) return true;
    dst.tex = nullptr;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (!d3d || FAILED(d3d->CreateTexture2D(&td, nullptr, dst.tex.put()))) return false;
    dst.tex_w = w;
    dst.tex_h = h;
    return true;
  }

  // Callback: só CopyResource na textura livre e solta o frame do WGC.
  void on_frame() {
    if (!running || !running->load(std::memory_order_relaxed) || !pool || !ctx) return;
    auto frame = pool.TryGetNextFrame();
    if (!frame) return;
    auto surface = frame.Surface();
    auto tex = texture_from_surface(surface);
    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);
    if (desc.Width < 2 || desc.Height < 2) return;
    int slot = -1;
    {
      std::lock_guard<std::mutex> lock(copy_mu);
      for (int i = 0; i < 2; ++i) {
        if (copies_[i].state == 0) {
          slot = i;
          copies_[i].state = 2;
          break;
        }
      }
    }
    if (slot < 0) return;
    HeldCopy& dst = copies_[slot];
    if (!ensure_copy_tex(dst, desc.Width, desc.Height)) {
      std::lock_guard<std::mutex> lock(copy_mu);
      dst.state = 0;
      return;
    }
    ctx->CopyResource(dst.tex.get(), tex.get());
    {
      std::lock_guard<std::mutex> lock(copy_mu);
      dst.src_w = desc.Width;
      dst.src_h = desc.Height;
      dst.w = even_capture_dim(static_cast<int>(desc.Width));
      dst.h = even_capture_dim(static_cast<int>(desc.Height));
      dst.arrived_ms = clock ? clock->now_ms() : 0;
      dst.seq = ++copy_seq_;
      dst.state = 1;
    }
    if (!copy_logged) {
      copy_logged = true;
      boot_line("primeira copia");
    }
  }

  void drain_copy() {
    int picked = -1;
    uint64_t best = 0;
    {
      std::lock_guard<std::mutex> lock(copy_mu);
      if (copy_seq_ < 2) return;
      for (int i = 0; i < 2; ++i) {
        if (copies_[i].state != 1 || !copies_[i].tex) continue;
        if (copies_[i].seq == copy_seq_) continue;
        if (picked < 0 || copies_[i].seq < best) {
          picked = i;
          best = copies_[i].seq;
        }
      }
      if (picked < 0) return;
      copies_[picked].state = 2;
    }
    HeldCopy& src = copies_[picked];
    const UINT src_w = src.src_w;
    const UINT src_h = src.src_h;
    const double arrived = src.arrived_ms;
    auto release_slot = [&]() {
      std::lock_guard<std::mutex> lock(copy_mu);
      src.state = 0;
    };
    if (src_w < 2 || src_h < 2 || !src.tex) {
      release_slot();
      return;
    }
    int buf = -1;
    for (int i = 0; i < 2; ++i) {
      if (!ensure_shared_bgra(shared_bgra_[i], src_w, src_h) || !shared_bgra_[i].mtx) continue;
      if (SUCCEEDED(shared_bgra_[i].mtx->AcquireSync(0, 0))) {
        buf = i;
        break;
      }
    }
    if (buf < 0) {
      for (int i = 0; i < 2; ++i) {
        if (!shared_bgra_[i].mtx || shared_bgra_[i].w != src_w || shared_bgra_[i].h != src_h) continue;
        if (SUCCEEDED(shared_bgra_[i].mtx->AcquireSync(1, 0))) {
          buf = i;
          break;
        }
      }
    }
    if (buf < 0) {
      release_slot();
      return;
    }
    SharedBgra& dst = shared_bgra_[buf];
    ctx->CopyResource(dst.tex.get(), src.tex.get());
    ctx->Flush();
    dst.mtx->ReleaseSync(1);
    const HANDLE handle = dst.handle;
    release_slot();
    publish_bgra(handle, static_cast<int>(src_w), static_cast<int>(src_h), arrived);
  }

  void signal_start(std::string err) {
    {
      std::lock_guard<std::mutex> lock(start_mu);
      start_err = std::move(err);
    }
    start_signaled.store(true, std::memory_order_release);
    start_cv.notify_one();
  }

  void run(HWND seed) {
    worker_tid = GetCurrentThreadId();
    winrt::Windows::System::DispatcherQueueController dq{nullptr};
    auto fail = [this](std::string e) {
      reset_pool();
      signal_start(std::move(e));
    };
    try {
      try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
      } catch (...) {
        try {
          winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (...) {
        }
      }
      ensure_dispatcher_queue(dq);

      UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
      HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                                     d3d.put(), nullptr, ctx.put());
      if (FAILED(hr)) {
        d3d = nullptr;
        ctx = nullptr;
        flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                               d3d.put(), nullptr, ctx.put());
      }
      if (FAILED(hr)) {
        fail("WGC: falha D3D11 (" + hresult_hex(hr) + ")");
        return;
      }
      {
        winrt::com_ptr<ID3D11Multithread> mt;
        if (SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(mt.put()))) && mt) mt->SetMultithreadProtected(TRUE);
      }
      init_video();
      winrt_d3d = wrap_d3d_device(d3d.get());

      HRESULT last_hr = E_FAIL;
      std::string last_msg;
      const auto candidates = collect_capture_hwnds(seed);
      for (HWND candidate : candidates) {
        try {
          item = item_from_hwnd(candidate);
          hwnd = candidate;
          last_hr = S_OK;
          break;
        } catch (const winrt::hresult_error& e) {
          last_hr = e.code();
          last_msg = wide_to_utf8(e.message().c_str());
        }
      }
      if (!item) {
        fail("WGC falhou (borderless / DRM): " + last_msg + " (" + hresult_hex(last_hr) + ")");
        return;
      }

      SizeInt32 size = item.Size();
      const SizeInt32 client = hwnd_capture_size(hwnd);
      if (size.Width < 2) size.Width = client.Width;
      if (size.Height < 2) size.Height = client.Height;
      if (size.Width < 2) size.Width = 2;
      if (size.Height < 2) size.Height = 2;
      pool = Direct3D11CaptureFramePool::Create(winrt_d3d, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
      session = pool.CreateCaptureSession(item);
      session.IsCursorCaptureEnabled(false);
      try {
        session.IsBorderRequired(false);
      } catch (...) {
      }
      try {
        session.MinUpdateInterval(
            winrt::Windows::Foundation::TimeSpan{wgc_min_update_ticks(capture_fps)});
      } catch (...) {
      }
      arrived = pool.FrameArrived([this](auto&&, auto&&) {
        try {
          on_frame();
        } catch (...) {
        }
      });
      closed = item.Closed([this](auto&&, auto&&) {
        if (running) running->store(false, std::memory_order_relaxed);
      });
      if (running) running->store(true, std::memory_order_relaxed);
      convert_stop.store(false, std::memory_order_relaxed);
      convert = std::thread([this]() {
        while (!convert_stop.load(std::memory_order_relaxed) && running &&
               running->load(std::memory_order_relaxed)) {
          drain_copy();
          Sleep(1);
        }
      });
      session.StartCapture();
      signal_start({});
      boot_line("StartCapture ok");

      MSG msg{};
      while (running && running->load(std::memory_order_relaxed)) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
          if (msg.message == WM_QUIT) {
            if (running) running->store(false, std::memory_order_relaxed);
            break;
          }
          TranslateMessage(&msg);
          DispatchMessageW(&msg);
        }
        if (!running || !running->load(std::memory_order_relaxed)) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 8, QS_ALLINPUT);
      }
    } catch (const winrt::hresult_error& e) {
      if (!start_signaled.load(std::memory_order_acquire)) {
        convert_stop.store(true, std::memory_order_relaxed);
        if (convert.joinable()) convert.join();
        fail("WGC falhou (borderless / DRM): " + wide_to_utf8(e.message().c_str()) + " (" + hresult_hex(e.code()) + ")");
        return;
      }
    } catch (...) {
      if (!start_signaled.load(std::memory_order_acquire)) {
        convert_stop.store(true, std::memory_order_relaxed);
        if (convert.joinable()) convert.join();
        fail("WGC falhou (borderless / DRM)");
        return;
      }
    }
    convert_stop.store(true, std::memory_order_relaxed);
    if (convert.joinable()) convert.join();
    reset_pool();
    if (running) running->store(false, std::memory_order_relaxed);
  }
};

WindowCapture::WindowCapture() : impl_(new Impl()) {}

WindowCapture::~WindowCapture() {
  stop();
  delete impl_;
  impl_ = nullptr;
}

bool WindowCapture::startup_pending() const {
  return impl_ && impl_->opening && !impl_->start_signaled.load(std::memory_order_acquire);
}

bool WindowCapture::take_startup_error(std::string& err) {
  if (!impl_ || !impl_->start_signaled.load(std::memory_order_acquire)) return false;
  std::lock_guard<std::mutex> lock(impl_->start_mu);
  if (impl_->start_err.empty()) return false;
  err = std::move(impl_->start_err);
  impl_->start_err.clear();
  impl_->opening = false;
  return true;
}

bool WindowCapture::start(const std::string& device_name, const CaptureStartOptions& opt, FrameSlot& slot,
                          IClock& clock, std::string& err) {
  if (running_) {
    err = "already running";
    return false;
  }
  if (device_name.empty() || device_name == "auto") {
    err = window_capture_pick_hint();
    return false;
  }
  HWND hwnd = nullptr;
  if (opt.hwnd != 0) {
    hwnd = reinterpret_cast<HWND>(opt.hwnd);
    if (!IsWindow(hwnd)) hwnd = nullptr;
  }
  if (!hwnd) {
    const uint64_t found = find_capturable_hwnd(device_name, 0);
    if (found == 0) {
      err = "janela nao encontrada: " + device_name;
      return false;
    }
    hwnd = reinterpret_cast<HWND>(found);
  }
  if (!impl_) {
    err = "WGC: interno";
    return false;
  }
  stop();
  impl_->slot = &slot;
  impl_->clock = &clock;
  impl_->hwnd = hwnd;
  impl_->running = &running_;
  impl_->frames = &frames_;
  impl_->out_w = &width_;
  impl_->out_h = &height_;
  impl_->start_signaled.store(false, std::memory_order_relaxed);
  impl_->start_err.clear();
  impl_->opening = opt.async_open;
  impl_->boot_log = opt.async_open;
  impl_->maps_logged = 0;
  impl_->copy_logged = false;
  impl_->capture_fps = opt.fps;
  frames_ = 0;
  width_ = 0;
  height_ = 0;
  running_ = false;
  try {
    impl_->worker = std::thread([this, hwnd]() { impl_->run(hwnd); });
  } catch (...) {
    impl_->opening = false;
    err = "WGC falhou: thread";
    return false;
  }
  if (opt.async_open) {
    opened_ = device_name;
    return true;
  }
  {
    // Sem bomba de mensagem a janela do NexusCap fica "não responde" e o cursor vira a rodinha.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!impl_->start_signaled.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= deadline) break;
      {
        std::unique_lock<std::mutex> lock(impl_->start_mu);
        impl_->start_cv.wait_for(lock, std::chrono::milliseconds(15), [this]() {
          return impl_->start_signaled.load(std::memory_order_acquire);
        });
      }
      pump_ui_briefly();
    }
    if (!impl_->start_signaled.load(std::memory_order_acquire)) {
      err = "WGC falhou: timeout ao abrir janela";
      running_ = false;
      if (impl_->worker_tid) PostThreadMessageW(impl_->worker_tid, WM_QUIT, 0, 0);
      if (impl_->worker.joinable()) impl_->worker.join();
      impl_->worker_tid = 0;
      return false;
    }
    if (!impl_->start_err.empty()) {
      err = impl_->start_err;
      running_ = false;
      if (impl_->worker.joinable()) impl_->worker.join();
      impl_->worker_tid = 0;
      return false;
    }
  }
  opened_ = device_name;
  if (opt.wait_first_frame) {
    const int wait_ms = (std::max)(1500, capture_first_frame_wait_ms(opt.width, opt.height));
    for (int i = 0; i < wait_ms && running_; i += 10) {
      pump_ui_briefly();
      if (frames_.load(std::memory_order_relaxed) > 0) break;
      Sleep(10);
    }
    if (frames_.load(std::memory_order_relaxed) == 0) {
      stop();
      err = "janela nao gerou frames (fullscreen exclusivo / DRM / minimizada)";
      return false;
    }
  }
  return true;
}

void WindowCapture::stop() {
  running_ = false;
  if (!impl_) return;
  if (impl_->worker_tid) PostThreadMessageW(impl_->worker_tid, WM_QUIT, 0, 0);
  if (impl_->worker.joinable()) impl_->worker.join();
  impl_->worker_tid = 0;
}

}  // namespace nexuscap
