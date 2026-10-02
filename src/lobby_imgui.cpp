#include "nexuscap/live_mode.hpp"
#include "nexuscap/lobby.hpp"
#include "nexuscap/audio.hpp"
#include "nexuscap/audio_select.hpp"
#include "nexuscap/capture.hpp"
#include "nexuscap/capture_format.hpp"
#include "nexuscap/clock.hpp"
#include "nexuscap/config.hpp"
#include "nexuscap/display_geom.hpp"
#include "nexuscap/dlss_options.hpp"
#include "nexuscap/dlss5_bridge.hpp"
#include "nexuscap/imgui_theme.hpp"
#include "nexuscap/lossless_fg.hpp"
#include "nexuscap/nr_advanced_ui.hpp"
#include "nexuscap/perf_test.hpp"
#include "nexuscap/pipeline.hpp"
#include "nexuscap/session_cfg.hpp"
#include "nexuscap/settings_store.hpp"
#include "nexuscap/system_metrics.hpp"
#include "nexuscap/user_preset.hpp"
#include "nexuscap/version.hpp"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <atomic>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <commdlg.h>
#include <wincodec.h>
#include <tchar.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "windowscodecs.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace nexuscap {
namespace {

struct LobbyDx {
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  IDXGISwapChain* swap = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
};

LobbyDx g_dx;
HWND g_hwnd = nullptr;
bool g_lobby_running = true;
bool g_start_requested = false;
bool g_open_gear = false;
bool g_gear_close_request = false;
bool g_open_perf = false;
bool g_show_perf_result = false;
PerfTestPlan g_perf_plan{};
PerfTestResult g_perf_result{};
int* g_capture_vk_target = nullptr;
int g_lobby_gear_vk = 0x7B;  // default F12 — atualizado do settings a cada frame
std::atomic<int> g_lobby_pass_step{0};
bool g_lobby_gear_ctrl = false;
HotkeyConfig g_lobby_hotkeys = default_hotkeys();
SystemMetricsSampler g_lobby_hw;
SystemMetricsSample g_lobby_hw_sample{};

void create_rtv() {
  if (g_dx.rtv) {
    g_dx.rtv->Release();
    g_dx.rtv = nullptr;
  }
  ID3D11Texture2D* bb = nullptr;
  g_dx.swap->GetBuffer(0, IID_PPV_ARGS(&bb));
  if (bb) {
    g_dx.device->CreateRenderTargetView(bb, nullptr, &g_dx.rtv);
    bb->Release();
  }
}

bool create_device(HWND hwnd) {
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 2;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  UINT flags = 0;
  D3D_FEATURE_LEVEL fl;
  const D3D_FEATURE_LEVEL req[] = {D3D_FEATURE_LEVEL_11_0};
  if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, req, 1,
                                           D3D11_SDK_VERSION, &sd, &g_dx.swap, &g_dx.device, &fl, &g_dx.ctx))) {
    return false;
  }
  silence_reshade_home_splash();
  create_rtv();
  return true;
}

void cleanup_device() {
  if (g_dx.rtv) {
    g_dx.rtv->Release();
    g_dx.rtv = nullptr;
  }
  if (g_dx.swap) {
    g_dx.swap->Release();
    g_dx.swap = nullptr;
  }
  if (g_dx.ctx) {
    g_dx.ctx->Release();
    g_dx.ctx = nullptr;
  }
  if (g_dx.device) {
    g_dx.device->Release();
    g_dx.device = nullptr;
  }
}

// Live lobby preview: DShow NV12 → downscaled RGBA texture for ImGui::Image.
struct LobbyPreview {
  FrameSlot slot;
  QpcClock clock;
  std::unique_ptr<CaptureBase> capture;
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  int tex_w = 0;
  int tex_h = 0;
  std::vector<uint8_t> rgba_base;  // decode only
  std::vector<uint8_t> rgba;       // graded for upload
  std::string err;
  std::string started_dev;
  uint64_t started_hwnd = 0;
  int started_w = 0;
  int started_h = 0;
  double started_fps = 0;
  int started_fourcc = -1;
  int started_buf = -1;
  int color_space = 0;
  int color_range = 0;
  int visual_preset = 0;
  bool enhance_preview = false;
  bool file_mode = false;
  bool window_mode = false;
  double fps_ema = 0;
  double last_frame_ms = 0;
  uint64_t frames_seen = 0;
  bool live = false;

  void apply_color(int space, int range) {
    color_space = clamp_color_space(space);
    color_range = clamp_color_range(range);
  }

  // Instant Model A/B/C look on the preview band (not only disk/Present).
  void set_visual_preview(int preset, bool enhance) {
    const int p = clamp_renodx_visual_preset(preset);
    const bool e = enhance;
    if (p == visual_preset && e == enhance_preview) return;
    visual_preset = p;
    enhance_preview = e;
    regrade_and_upload();
  }

  void release_tex() {
    if (srv) {
      srv->Release();
      srv = nullptr;
    }
    if (tex) {
      tex->Release();
      tex = nullptr;
    }
    tex_w = tex_h = 0;
  }

  void upload_rgba(int ow, int oh) {
    if (!g_dx.device || !g_dx.ctx || rgba.empty() || ow <= 0 || oh <= 0) return;
    if (!tex || tex_w != ow || tex_h != oh) {
      release_tex();
      D3D11_TEXTURE2D_DESC td{};
      td.Width = static_cast<UINT>(ow);
      td.Height = static_cast<UINT>(oh);
      td.MipLevels = 1;
      td.ArraySize = 1;
      td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_DYNAMIC;
      td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      if (FAILED(g_dx.device->CreateTexture2D(&td, nullptr, &tex))) return;
      if (FAILED(g_dx.device->CreateShaderResourceView(tex, nullptr, &srv))) {
        release_tex();
        return;
      }
      tex_w = ow;
      tex_h = oh;
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(g_dx.ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      for (int y = 0; y < oh; ++y) {
        std::memcpy(static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                    rgba.data() + static_cast<size_t>(y) * ow * 4, static_cast<size_t>(ow) * 4);
      }
      g_dx.ctx->Unmap(tex, 0);
    }
  }

  void regrade_and_upload() {
    if (rgba_base.empty() || tex_w <= 0 || tex_h <= 0) return;
    rgba = rgba_base;
    const auto grade = lobby_preview_grade_for_preset(visual_preset, enhance_preview);
    apply_lobby_preview_grade_rgba(rgba.data(), tex_w * tex_h, grade);
    upload_rgba(tex_w, tex_h);
  }

  void stop() {
    if (capture) {
      capture->stop();
      capture.reset();
    }
    live = false;
    file_mode = false;
    window_mode = false;
    slot.clear();
    release_tex();
    started_dev.clear();
    started_hwnd = 0;
    started_w = started_h = 0;
    started_fps = 0;
    started_fourcc = started_buf = -1;
    fps_ema = 0;
    last_frame_ms = 0;
    // keep frames_seen / err for UI until next ensure
  }

  bool ensure(const std::string& device, const CaptureStartOptions& opt) {
    const int w = opt.width;
    const int h = opt.height;
    const double fps = opt.fps;
    if (w <= 0 || h <= 0 || fps <= 0) {
      err = "modo invalido";
      stop();
      return false;
    }
    if (capture && live && started_dev == device && started_w == w && started_h == h &&
        std::abs(started_fps - fps) < 0.5 && started_fourcc == opt.video_fourcc &&
        started_buf == opt.capture_buffering) {
      // Color does not need DShow restart — only CPU decode matrix.
      apply_color(opt.color_space, opt.color_range);
      return true;
    }
    stop();
    file_mode = false;
    window_mode = false;
    // USB capture cards often need a beat before reopen after format change.
    Sleep(static_cast<DWORD>(capture_device_settle_ms()));
    err.clear();
    frames_seen = 0;
    started_dev = device;
    started_w = w;
    started_h = h;
    started_fps = fps;
    started_fourcc = opt.video_fourcc;
    started_buf = opt.capture_buffering;
    apply_color(opt.color_space, opt.color_range);
    capture = std::make_unique<DShowCapture>();
    const std::string dev = device.empty() ? "auto" : device;
    if (!capture->start(dev, opt, slot, clock, err)) {
      // Formato pedido (MJPEG/YUY2/…) falhou — nao deixar preview morto: cai pra NV12.
      if (clamp_video_fourcc(opt.video_fourcc) != static_cast<int>(VideoFourcc::Nv12)) {
        CaptureStartOptions fb = opt;
        fb.video_fourcc = static_cast<int>(VideoFourcc::Nv12);
        std::string err2;
        if (capture->start(dev, fb, slot, clock, err2)) {
          started_fourcc = static_cast<int>(VideoFourcc::Nv12);
          err = std::string("formato ") + video_fourcc_name(opt.video_fourcc) +
                " falhou; preview em NV12";
          live = true;
          return true;
        }
        err = err + " | fallback NV12: " + err2;
      }
      capture.reset();
      live = false;
      return false;
    }
    live = true;
    return true;
  }

  bool ensure_window(const std::string& title, const CaptureStartOptions& opt) {
    if (title.empty()) {
      err = "escolha uma janela";
      stop();
      return false;
    }
    if (capture && live && window_mode && started_dev == title && started_hwnd == opt.hwnd) {
      apply_color(opt.color_space, opt.color_range);
      return true;
    }
    stop();
    err.clear();
    frames_seen = 0;
    file_mode = false;
    window_mode = true;
    started_dev = title;
    started_hwnd = opt.hwnd;
    started_w = opt.width;
    started_h = opt.height;
    started_fps = opt.fps;
    started_fourcc = static_cast<int>(VideoFourcc::Nv12);
    started_buf = opt.capture_buffering;
    apply_color(opt.color_space, opt.color_range);
    capture = std::make_unique<WindowCapture>();
    CaptureStartOptions wopt = opt;
    wopt.wait_first_frame = false;
    if (!capture->start(title, wopt, slot, clock, err)) {
      capture.reset();
      live = false;
      window_mode = false;
      return false;
    }
    started_w = capture->opened_width() > 0 ? capture->opened_width() : opt.width;
    started_h = capture->opened_height() > 0 ? capture->opened_height() : opt.height;
    live = true;
    return true;
  }

  bool ensure(const std::string& device, int w, int h, double fps) {
    CaptureStartOptions opt;
    opt.width = w;
    opt.height = h;
    opt.fps = fps;
    return ensure(device, opt);
  }

  bool ensure_file(const std::string& path, const CaptureStartOptions& opt) {
    const int w = opt.width;
    const int h = opt.height;
    const double fps = opt.fps;
    if (path.empty() || w <= 0 || h <= 0 || fps <= 0) {
      err = "video do teste invalido";
      stop();
      return false;
    }
    if (capture && live && file_mode && started_dev == path && started_w == w && started_h == h &&
        std::abs(started_fps - fps) < 0.5) {
      apply_color(opt.color_space, opt.color_range);
      return true;
    }
    stop();
    err.clear();
    frames_seen = 0;
    file_mode = true;
    started_dev = path;
    started_w = w;
    started_h = h;
    started_fps = fps;
    started_fourcc = static_cast<int>(VideoFourcc::Nv12);
    started_buf = opt.capture_buffering;
    apply_color(opt.color_space, opt.color_range);
    capture = std::make_unique<FileCapture>();
    if (!capture->start(path, opt, slot, clock, err)) {
      capture.reset();
      live = false;
      file_mode = false;
      return false;
    }
    live = true;
    return true;
  }

  void tick() {
    FramePayload frame;
    if (!slot.consume(frame) || frame.width <= 0 || frame.height <= 0 || frame.nv12.empty()) return;

    ++frames_seen;
    const double now = clock.now_ms();
    if (last_frame_ms > 0) {
      const double dt = now - last_frame_ms;
      const double inst = 1000.0 / (std::max)(0.001, dt);
      fps_ema = fps_ema > 1.0 ? fps_ema * 0.90 + inst * 0.10 : inst;
    }
    last_frame_ms = now;

    const int max_w = kLobbyPreviewMaxWidth;
    const int step = lobby_preview_downsample_step(frame.width, max_w);
    const int ow = (std::max)(1, frame.width / step);
    const int oh = (std::max)(1, frame.height / step);
    rgba_base.resize(static_cast<size_t>(ow) * static_cast<size_t>(oh) * 4);

    const size_t need = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height) * 3 / 2;
    if (frame.nv12.size() < need) return;
    const uint8_t* yplane = frame.nv12.data();
    const uint8_t* uvplane = frame.nv12.data() + static_cast<size_t>(frame.width) * frame.height;

    const YuvRgbCoeffs coeffs = yuv_rgb_coeffs(color_space, color_range);
    for (int y = 0; y < oh; ++y) {
      for (int x = 0; x < ow; ++x) {
        const int sx = (x * step) & ~1;
        const int sy = (y * step) & ~1;
        const int Y = yplane[static_cast<size_t>(sy) * frame.width + sx];
        const uint8_t* uv = uvplane + static_cast<size_t>(sy / 2) * frame.width + (sx & ~1);
        const int U = uv[0];
        const int V = uv[1];
        uint8_t* dst = rgba_base.data() + (static_cast<size_t>(y) * ow + x) * 4;
        yuv_to_rgb8(Y, U, V, coeffs, dst);
        dst[3] = 255;
      }
    }

    rgba = rgba_base;
    const auto grade = lobby_preview_grade_for_preset(visual_preset, enhance_preview);
    apply_lobby_preview_grade_rgba(rgba.data(), ow * oh, grade);
    upload_rgba(ow, oh);
  }
};

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
  switch (msg) {
    case WM_ERASEBKGND: {
      HDC hdc = reinterpret_cast<HDC>(wp);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      return 1;
    }
    case WM_SIZE:
      if (g_dx.device && wp != SIZE_MINIMIZED) {
        if (g_dx.rtv) {
          g_dx.rtv->Release();
          g_dx.rtv = nullptr;
        }
        g_dx.swap->ResizeBuffers(0, (UINT)LOWORD(lp), (UINT)HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
        create_rtv();
      }
      return 0;
    case WM_KEYDOWN:
      if (g_capture_vk_target) {
        if (wp == VK_ESCAPE) {
          g_capture_vk_target = nullptr;
          return 0;
        }
        if (wp == VK_SHIFT || wp == VK_CONTROL || wp == VK_MENU) return 0;
        *g_capture_vk_target = static_cast<int>(wp);
        g_capture_vk_target = nullptr;
        return 0;
      }
      if (wp == VK_ESCAPE) {
        // Esc never closes the lobby / app — only gear or hotkey capture (see lobby_esc_action).
        const LobbyEscAction act = lobby_esc_action(g_open_gear || g_open_perf || g_show_perf_result, false);
        if (act == LobbyEscAction::CloseGear) {
          if (g_open_gear) g_gear_close_request = true;
          g_open_perf = false;
          g_show_perf_result = false;
        }
        return 0;
      }
      if (lobby_gear_hotkey_toggles_cfg(static_cast<int>(wp), g_lobby_gear_vk, false,
                                       (GetKeyState(VK_CONTROL) & 0x8000) != 0, g_lobby_gear_ctrl)) {
        if (g_open_gear) g_gear_close_request = true;
        else g_open_gear = true;
        return 0;
      }
      if ((lp & (1u << 30)) == 0) {
        const int pass_step =
            pass_cycle_hotkey_step(static_cast<int>(wp), (GetKeyState(VK_CONTROL) & 0x8000) != 0, g_lobby_hotkeys);
        if (pass_step != 0) {
          g_lobby_pass_step.store(pass_step);
          return 0;
        }
      }
      return 0;
    case WM_CLOSE:
      // X = cancel lobby, never start session.
      g_start_requested = false;
      g_lobby_running = false;
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      // Do NOT PostQuitMessage — leftover WM_QUIT kills run_app after Iniciar.
      g_lobby_running = false;
      if (g_hwnd == hwnd) g_hwnd = nullptr;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

void apply_lobby_theme() {
  apply_nexuscap_imgui_theme();
  // Só o FramePadding é do lobby: a janela é grande e os controles ficam clicáveis à distância.
  ImGui::GetStyle().FramePadding = ImVec2(14, 10);
}

void apply_settings_to_cfg(const PersistedSettings& s, AppConfig& cfg) {
  apply_persisted_to_config(s, cfg);
}

// Pick LosslessScaling.exe via native file dialog (manual override when Steam auto-detect fails).
bool browse_lossless_exe_path(HWND owner, std::string& out_path) {
  wchar_t file[MAX_PATH]{};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = L"Lossless Scaling\0LosslessScaling.exe\0Executaveis\0*.exe\0Todos\0*.*\0";
  ofn.nFilterIndex = 1;
  ofn.lpstrTitle = L"Selecionar LosslessScaling.exe";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetOpenFileNameW(&ofn)) return false;
  char narrow[MAX_PATH * 4]{};
  WideCharToMultiByte(CP_UTF8, 0, file, -1, narrow, static_cast<int>(sizeof(narrow)), nullptr, nullptr);
  out_path = narrow;
  return !out_path.empty();
}

std::string fg_path_display_tail(const std::string& path, size_t max_chars = 48) {
  if (path.size() <= max_chars) return path;
  return "..." + path.substr(path.size() - (max_chars - 3));
}

std::wstring utf8_path_to_wide(const std::string& s) {
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 0) return L"";
  std::wstring w(static_cast<size_t>(n), 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  if (!w.empty() && w.back() == 0) w.pop_back();
  return w;
}

std::string exe_dir_utf8() {
  wchar_t exe_path[MAX_PATH]{};
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  return std::filesystem::path(exe_path).parent_path().string();
}

std::string resolve_brand_png_on_disk(const std::string& assets_path, const std::string& root_path) {
  for (const auto& p : {assets_path, root_path}) {
    const DWORD attr = GetFileAttributesW(utf8_path_to_wide(p).c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) return p;
  }
  return {};
}

std::string resolve_logo_png_on_disk() {
  const std::string dir = exe_dir_utf8();
  return resolve_brand_png_on_disk(logo_png_candidate_assets(dir), logo_png_candidate_root(dir));
}

std::string resolve_watermark_png_on_disk() {
  const std::string dir = exe_dir_utf8();
  return resolve_brand_png_on_disk(watermark_png_candidate_assets(dir), watermark_png_candidate_root(dir));
}

bool load_png_bgra_file(const std::wstring& path, int& out_w, int& out_h, std::vector<uint8_t>& out_bgra) {
  out_w = out_h = 0;
  out_bgra.clear();
  IWICImagingFactory* factory = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
    return false;
  }
  IWICBitmapDecoder* decoder = nullptr;
  HRESULT hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                    WICDecodeMetadataCacheOnLoad, &decoder);
  if (FAILED(hr) || !decoder) {
    factory->Release();
    return false;
  }
  IWICBitmapFrameDecode* frame = nullptr;
  hr = decoder->GetFrame(0, &frame);
  if (FAILED(hr) || !frame) {
    decoder->Release();
    factory->Release();
    return false;
  }
  IWICFormatConverter* conv = nullptr;
  hr = factory->CreateFormatConverter(&conv);
  if (FAILED(hr) || !conv ||
      FAILED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom))) {
    if (conv) conv->Release();
    frame->Release();
    decoder->Release();
    factory->Release();
    return false;
  }
  UINT w = 0, h = 0;
  conv->GetSize(&w, &h);
  if (w == 0 || h == 0) {
    conv->Release();
    frame->Release();
    decoder->Release();
    factory->Release();
    return false;
  }
  out_bgra.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
  hr = conv->CopyPixels(nullptr, w * 4u, static_cast<UINT>(out_bgra.size()), out_bgra.data());
  conv->Release();
  frame->Release();
  decoder->Release();
  factory->Release();
  if (FAILED(hr)) {
    out_bgra.clear();
    return false;
  }
  out_w = static_cast<int>(w);
  out_h = static_cast<int>(h);
  return true;
}

bool create_png_srv(ID3D11Device* device, const std::string& path, ID3D11ShaderResourceView** out_srv, int* out_w,
                    int* out_h) {
  if (out_srv) *out_srv = nullptr;
  if (out_w) *out_w = 0;
  if (out_h) *out_h = 0;
  if (!device || !out_srv || path.empty()) return false;
  int w = 0, h = 0;
  std::vector<uint8_t> bgra;
  if (!load_png_bgra_file(utf8_path_to_wide(path), w, h, bgra)) return false;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = static_cast<UINT>(w);
  td.Height = static_cast<UINT>(h);
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_IMMUTABLE;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA sd{};
  sd.pSysMem = bgra.data();
  sd.SysMemPitch = static_cast<UINT>(w * 4);
  ID3D11Texture2D* tex = nullptr;
  if (FAILED(device->CreateTexture2D(&td, &sd, &tex)) || !tex) return false;
  ID3D11ShaderResourceView* srv = nullptr;
  const HRESULT hr = device->CreateShaderResourceView(tex, nullptr, &srv);
  tex->Release();
  if (FAILED(hr) || !srv) return false;
  *out_srv = srv;
  if (out_w) *out_w = w;
  if (out_h) *out_h = h;
  return true;
}

bool create_logo_srv(ID3D11Device* device, ID3D11ShaderResourceView** out_srv, int* out_w, int* out_h) {
  return create_png_srv(device, resolve_logo_png_on_disk(), out_srv, out_w, out_h);
}

bool create_watermark_srv(ID3D11Device* device, ID3D11ShaderResourceView** out_srv, int* out_w, int* out_h) {
  return create_png_srv(device, resolve_watermark_png_on_disk(), out_srv, out_w, out_h);
}

bool lobby_draw_origem_card(const char* id, bool window_kind, bool selected) {
  ImGui::PushID(id);
  const float w = ImGui::GetContentRegionAvail().x;
  const float h = 58.f;
  const bool clicked = ImGui::InvisibleButton("##hit", ImVec2(w, h));
  const bool hovered = ImGui::IsItemHovered();
  const ImVec2 p0 = ImGui::GetItemRectMin();
  const ImVec2 p1 = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 bg = selected ? IM_COL32(22, 58, 96, 255)
                            : (hovered ? IM_COL32(28, 38, 54, 255) : IM_COL32(16, 22, 32, 255));
  const ImU32 bd = selected ? IM_COL32(90, 185, 255, 230) : IM_COL32(48, 68, 92, 170);
  dl->AddRectFilled(p0, p1, bg, 8.f);
  dl->AddRect(p0, p1, bd, 8.f, 0, selected ? 1.6f : 1.f);
  if (selected) {
    dl->AddRectFilled(ImVec2(p0.x, p0.y + 8.f), ImVec2(p0.x + 3.f, p1.y - 8.f), IM_COL32(115, 217, 255, 255), 2.f);
  }
  const ImU32 icon = selected ? IM_COL32(160, 230, 255, 255) : IM_COL32(130, 150, 170, 255);
  const ImVec2 ic(p0.x + 14.f, p0.y + 18.f);
  if (window_kind) {
    dl->AddRect(ImVec2(ic.x + 2.f, ic.y - 2.f), ImVec2(ic.x + 16.f, ic.y + 10.f), icon, 2.f, 0, 1.5f);
    dl->AddRect(ImVec2(ic.x + 6.f, ic.y + 4.f), ImVec2(ic.x + 20.f, ic.y + 16.f), icon, 2.f, 0, 1.5f);
  } else {
    dl->AddRectFilled(ImVec2(ic.x + 1.f, ic.y + 2.f), ImVec2(ic.x + 21.f, ic.y + 13.f), icon, 2.f);
    dl->AddTriangleFilled(ImVec2(ic.x + 5.f, ic.y + 13.f), ImVec2(ic.x + 17.f, ic.y + 13.f),
                          ImVec2(ic.x + 11.f, ic.y + 18.f), icon);
  }
  const char* title = capture_source_card_title(window_kind);
  const char* sub = capture_source_card_sub(window_kind);
  const ImU32 col_title = selected ? IM_COL32(236, 246, 255, 255) : IM_COL32(210, 220, 232, 255);
  const ImU32 col_sub = IM_COL32(138, 154, 170, 255);
  ImGui::PushClipRect(ImVec2(p0.x + 42.f, p0.y), ImVec2(p1.x - 10.f, p1.y), true);
  dl->AddText(ImVec2(p0.x + 42.f, p0.y + 8.f), col_title, title);
  dl->AddText(ImVec2(p0.x + 42.f, p0.y + 30.f), col_sub, sub);
  ImGui::PopClipRect();
  ImGui::PopID();
  return clicked;
}

void apply_window_app_icon(HWND hwnd, HINSTANCE hi) {
  if (!hwnd) return;
  HICON icon = LoadIconW(hi, MAKEINTRESOURCEW(kAppIconResourceId));
  if (!icon) return;
  SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
  SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
}

// Live HUD on left black bar of letterboxed preview — bright text readable on black.
void draw_live_preview_strip(const PersistedSettings& settings, const LobbyPreview& preview, ImVec2 img_min,
                             ImVec2 img_max) {
  const LiveConsole cons = parse_live_console(settings.ui.live_console);
  const bool dlss5 = settings.enhance_renodx && settings.neural_rendering;
  const int fgx = settings.fg_enabled ? (settings.fg_multiplier >= 2 ? settings.fg_multiplier : 2) : 1;
  const double fps = preview.fps_ema > 1.0 ? preview.fps_ema : settings.fps;
  const double ft = fps > 1.0 ? 1000.0 / fps : 0.0;
  LiveStripLines strip{};
  fill_live_strip_lines(strip, settings.ui.overlay_console, cons, dlss5, settings.nr_pass_count,
                        settings.fg_enabled, fgx, fps, 0.0, true, settings.fg_queue, ft, settings.fg_adaptive,
                        settings.fg_target_fps, settings.fg_flow_scale);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float font_sz = 18.f;
  const float gap = 10.f;
  const float pad_x = 10.f;
  const float pad_y = 6.f;
  const float bar_h = font_sz + pad_y * 2.f;
  auto text_w = [&](const char* t) {
    if (!t || !t[0]) return 0.f;
    return ImGui::CalcTextSize(t).x * (font_sz / (std::max)(1.f, ImGui::GetFontSize()));
  };
  float content_w = text_w(strip.dlss) + gap + text_w(strip.fg) + gap + text_w(strip.fps_lab.text) +
                    text_w(strip.fps_cap.text);
  if (strip.has_fg_fps) content_w += text_w(strip.fps_sep.text) + text_w(strip.fps_fg.text);
  if (strip.ft[0]) content_w += gap + text_w(strip.ft);
  if (strip.has_console) content_w += text_w(strip.console) + gap;
  const float bar_w = content_w + pad_x * 2.f;
  const float x1 = img_max.x - 8.f;
  const float y0 = img_min.y + 8.f;
  ImVec2 p0(x1 - bar_w, y0);
  ImVec2 p1(x1, y0 + bar_h);
  if (p0.x < img_min.x + 4.f) p0.x = img_min.x + 4.f;
  if (overlay_draws_plate(settings.ui.overlay_no_bg)) {
    dl->AddRectFilled(p0, p1, IM_COL32(12, 14, 18, 220), 4.f);
  }
  float x = p0.x + pad_x;
  const float y = p0.y + pad_y;
  auto paint = [&](const char* t, ImU32 col) {
    if (!t || !t[0]) return;
    dl->AddText(nullptr, font_sz, ImVec2(x, y), col, t);
    x += text_w(t) + gap;
  };
  if (strip.has_console) {
    unsigned char cr = 0, cg = 55, cb = 145;
    live_console_rgb(cons, cr, cg, cb);
    paint(strip.console, IM_COL32(cr, cg, cb, 255));
  }
  paint(strip.dlss, IM_COL32(240, 242, 248, 255));
  {
    unsigned char fr = 255, fg = 196, fb = 72;
    live_fps_ft_rgb(settings.fg_enabled && fgx >= 2 ? fgx : 1, true, fr, fg, fb);
    paint(strip.fg, IM_COL32(fr, fg, fb, 255));
  }
  paint(strip.fps_lab.text, IM_COL32(strip.fps_lab.r, strip.fps_lab.g, strip.fps_lab.b, 255));
  x -= gap;
  paint(strip.fps_cap.text, IM_COL32(strip.fps_cap.r, strip.fps_cap.g, strip.fps_cap.b, 255));
  if (strip.has_fg_fps) {
    x -= gap;
    paint(strip.fps_sep.text, IM_COL32(strip.fps_sep.r, strip.fps_sep.g, strip.fps_sep.b, 255));
    x -= gap;
    paint(strip.fps_fg.text, IM_COL32(strip.fps_fg.r, strip.fps_fg.g, strip.fps_fg.b, 255));
  }
  if (strip.ft[0]) {
    const OverlayFtStatus st = overlay_ft_status(true, settings.fg_enabled, fgx, fps, settings.fps, 0);
    const FgFtRgb rgb = overlay_ft_status_rgb(st);
    paint(strip.ft, IM_COL32(rgb.r, rgb.g, rgb.b, 255));
  }
}

void draw_hud_preview_overlay(const PersistedSettings& settings, const LobbyPreview& preview,
                              ImVec2 band_min, ImVec2 band_max, ImVec2 img_min, ImVec2 img_max) {
  if (settings.ui.live_overlay) {
    draw_live_preview_strip(settings, preview, img_min, img_max);
    return;
  }
  if (!settings.ui.show_fps) return;

  if (settings.ui.mini_overlay || settings.ui.hw_info) {
    g_lobby_hw.sample(g_dx.device, g_lobby_hw_sample);
  }
  LobbyHudPreviewLines hud = make_lobby_hud_preview_lines(settings, preview.fps_ema, &g_lobby_hw_sample);
  if (hud.count <= 0) return;

  ImDrawList* dl = ImGui::GetWindowDrawList();

  // Mini overlay preview: 3-line bar matching session HUD (stalls on row 2).
  if (settings.ui.mini_overlay) {
    const float bar_h = static_cast<float>(hud_mini_overlay_height());
    ImVec2 p0(img_min.x, img_min.y);
    ImVec2 p1(img_max.x, img_min.y + bar_h);
    if (overlay_draws_plate(settings.ui.overlay_no_bg)) {
      dl->AddRectFilled(p0, p1, IM_COL32(8, 10, 14, 210), 0.f);
    }
    const float graph_w = static_cast<float>(hud_mini_frametime_graph_w(static_cast<int>(p1.x - p0.x)));
    const float gx = (std::max)(p0.x + 8.f, p1.x - 8.f - graph_w);
    const float gy = p0.y + 9.f;
    const float gh = bar_h - 16.f;
    const float gw = (std::min)(graph_w, (std::max)(0.f, p1.x - p0.x - 16.f));

    const float fps = preview.fps_ema > 1.0 ? static_cast<float>(preview.fps_ema) : 0.f;
    const float ft = fps > 1.f ? (1000.f / fps) : 0.f;
    const int fgx = settings.fg_enabled ? (settings.fg_multiplier >= 2 ? settings.fg_multiplier : 2) : 1;
    const int scale = clamp_scale_percent(settings.scale_percent);
    const bool dlss5 = settings.enhance_renodx && settings.neural_rendering;
    char hw_compact[200]{};
    format_sys_metrics_compact(hw_compact, sizeof(hw_compact), g_lobby_hw_sample);
    MiniOverlaySeg segs[kMiniOverlayMaxSegs]{};
    char line0[8]{}, line1[8]{};
    const LiveConsole cons = parse_live_console(settings.ui.live_console);
    unsigned char cr = 0, cg = 55, cb = 145;
    live_console_rgb(cons, cr, cg, cb);
    const int nseg = format_mini_overlay_segments(
        segs, kMiniOverlayMaxSegs, line0, sizeof(line0), line1, sizeof(line1), fps,
        fps * static_cast<double>((std::max)(fgx, 1)), false, fgx, dlss5, settings.ui.sync_image, ft, ft, ft, ft,
        true, ft, ft * 0.15f, 0, 0, 0, settings.width, settings.height, scaled_extent(settings.width, scale),
        scaled_extent(settings.height, scale), settings.width, settings.height, scale, false, 0, 0, 0, 0,
        renodx_visual_preset_name(settings.renodx_preset, settings.nr_pass_count), hw_compact[0] ? hw_compact : nullptr, 0, 0, 0,
        nullptr, settings.fps, settings.nr_pass_count,
        overlay_console_label_or_null(settings.ui.overlay_console, cons), false, cr, cg, cb, settings.fg_queue,
        settings.fg_adaptive, settings.fg_target_fps);

    const float font_sz = static_cast<float>(hud_mini_overlay_font_px());
    const float line_gap = 2.f;
    const int rows = hud_mini_overlay_rows();
    const float block_h = font_sz * static_cast<float>(rows) + line_gap * static_cast<float>(rows - 1);
    const float y0 = p0.y + (bar_h - block_h) * 0.5f;
    const float text_max_x = gx - 8.f;
    int wrap_st = nseg;
    int wrap_l600 = nseg;
    mini_overlay_row_breaks(segs, nseg, &wrap_st, &wrap_l600);
    auto skip_sep = [&](int i) -> int {
      while (i < nseg && std::strcmp(segs[i].text, " · ") == 0) ++i;
      return i;
    };
    wrap_st = skip_sep(wrap_st);
    wrap_l600 = skip_sep(wrap_l600);
    auto paint_row = [&](int begin, int end, float y) {
      float x = p0.x + 8.f;
      begin = skip_sep(begin);
      for (int i = begin; i < end; ++i) {
        if (!segs[i].text[0]) continue;
        const ImU32 col = IM_COL32(segs[i].r, segs[i].g, segs[i].b, 255);
        const ImVec2 sz = ImGui::CalcTextSize(segs[i].text);
        const float w = sz.x * (font_sz / ImGui::GetFontSize());
        if (x >= text_max_x) break;
        if (x + w > text_max_x) {
          dl->PushClipRect(ImVec2(x, p0.y), ImVec2(text_max_x, p1.y), true);
          dl->AddText(nullptr, font_sz, ImVec2(x, y), col, segs[i].text);
          dl->PopClipRect();
          break;
        }
        dl->AddText(nullptr, font_sz, ImVec2(x, y), col, segs[i].text);
        x += w;
      }
    };
    paint_row(0, wrap_st, y0);
    paint_row(wrap_st, wrap_l600, y0 + font_sz + line_gap);
    paint_row(wrap_l600, nseg, y0 + (font_sz + line_gap) * 2.f);

    if (gw >= 48.f) {
      if (overlay_draws_plate(settings.ui.overlay_no_bg)) {
        dl->AddRectFilled(ImVec2(gx, gy), ImVec2(gx + gw, gy + gh), IM_COL32(20, 24, 30, 220), 2.f);
      }
      const float tgt = settings.fps > 0 ? static_cast<float>(1000.0 / settings.fps) : 16.67f;
      const float t = ft > 0.f ? (std::min)(1.f, ft / (tgt * 2.5f)) : 0.2f;
      dl->AddRectFilled(ImVec2(gx + 2, gy + 2), ImVec2(gx + 2 + (gw - 4) * t, gy + gh - 2),
                        IM_COL32(100, 235, 255, 240), 2.f);
    }
    return;
  }

  // HW lines are always appended last — drop them here and paint the dedicated block.
  char hw_lines[kSysMetricsLineCount][64]{};
  int hw_n = 0;
  if (settings.ui.hw_info) {
    hw_n = format_sys_metrics_lines(hw_lines, kSysMetricsLineCount, g_lobby_hw_sample);
    if (hud.count >= hw_n) hud.count -= hw_n;
  }

  const float pad = 8.f;
  const float line_h = settings.ui.advanced_metrics ? 16.f : 15.f;
  const int total_lines = hud.count + hw_n;
  const float preferred_w = settings.ui.hw_info ? 280.f : (settings.ui.advanced_metrics ? 360.f : 280.f);
  const float band_w = band_max.x - band_min.x;
  const float band_h = band_max.y - band_min.y;
  ViewportGeom box{};
  box.x = img_min.x - band_min.x;
  box.y = img_min.y - band_min.y;
  box.w = img_max.x - img_min.x;
  box.h = img_max.y - img_min.y;
  const auto lay = lobby_preview_hud_layout(box, band_w, band_h, preferred_w, pad);
  const float box_w = (std::min)(preferred_w, lay.max_w);
  const float box_h = pad * 2.f + line_h * static_cast<float>(total_lines) +
                      (settings.ui.advanced_metrics ? 36.f : 0.f);
  ImVec2 p0(band_min.x + lay.x, band_min.y + lay.y);
  if (p0.y + box_h > band_max.y - 4.f) p0.y = (std::max)(band_min.y + 4.f, band_max.y - box_h - 4.f);
  ImVec2 p1(p0.x + box_w, p0.y + box_h);

  // No dark card on the black pillar — text sits directly on the bar.
  if (!lay.on_black_bar && overlay_draws_plate(settings.ui.overlay_no_bg)) {
    dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 160), 4.f);
  }

  // High-luminance colors so HUD stays readable on pure black side bars.
  const ImU32 cols[] = {
      IM_COL32(255, 230, 90, 255),  IM_COL32(160, 240, 255, 255), IM_COL32(120, 255, 180, 255),
      IM_COL32(255, 255, 255, 255), IM_COL32(140, 255, 220, 255), IM_COL32(200, 180, 255, 255),
      IM_COL32(255, 170, 120, 255), IM_COL32(160, 255, 200, 255), IM_COL32(255, 150, 150, 255),
  };
  const float prev_fps = preview.fps_ema > 1.0 ? static_cast<float>(preview.fps_ema) : 0.f;
  const int prev_fgx = settings.fg_enabled ? (settings.fg_multiplier >= 2 ? settings.fg_multiplier : 2) : 1;
  int row = 0;
  for (int i = 0; i < hud.count; ++i, ++row) {
    const float y = p0.y + pad + line_h * static_cast<float>(row);
    if (std::strncmp(hud.line[i], "FPS", 3) == 0) {
      HudFpsParts parts{};
      fill_lobby_hud_fps_parts(parts, prev_fps, settings.fg_enabled, prev_fgx, true, settings.fg_adaptive,
                              settings.fg_target_fps, settings.fg_flow_scale);
      float x = p0.x + 2.f;
      for (int p = 0; p < parts.count; ++p) {
        if (!parts.parts[p].text[0]) continue;
        dl->AddText(ImVec2(x, y), IM_COL32(parts.parts[p].r, parts.parts[p].g, parts.parts[p].b, 255),
                    parts.parts[p].text);
        x += ImGui::CalcTextSize(parts.parts[p].text).x;
      }
      continue;
    }
    dl->AddText(ImVec2(p0.x + 2.f, y), cols[row % 9], hud.line[i]);
  }
  // Infos GPU/CPU — one metric per line (never concatenated).
  const ImU32 hw_cols[] = {
      IM_COL32(255, 230, 90, 255),  IM_COL32(160, 240, 255, 255), IM_COL32(120, 255, 180, 255),
      IM_COL32(255, 255, 255, 255), IM_COL32(140, 255, 220, 255),
  };
  for (int i = 0; i < hw_n; ++i, ++row) {
    dl->AddText(ImVec2(p0.x + 2.f, p0.y + pad + line_h * static_cast<float>(row)), hw_cols[i % 5],
                hw_lines[i]);
  }

  if (settings.ui.advanced_metrics) {
    const float fps = preview.fps_ema > 1.0 ? static_cast<float>(preview.fps_ema) : 0.f;
    const float ft = fps > 1.f ? (1000.f / fps) : 0.f;
    const float gx = p0.x + 2.f;
    const float gy = p1.y - 28.f;
    const float gw = (std::max)(24.f, box_w - 4.f);
    const float gh = 18.f;
    if (overlay_draws_plate(settings.ui.overlay_no_bg)) {
      dl->AddRectFilled(ImVec2(gx, gy), ImVec2(gx + gw, gy + gh), IM_COL32(20, 24, 30, 220), 3.f);
    }
    const float tgt = settings.fps > 0 ? static_cast<float>(1000.0 / settings.fps) : 16.67f;
    const float t = ft > 0.f ? (std::min)(1.f, ft / (tgt * 2.5f)) : 0.2f;
    dl->AddRectFilled(ImVec2(gx + 2, gy + 2), ImVec2(gx + 2 + (gw - 4) * t, gy + gh - 2),
                      IM_COL32(100, 235, 255, 240), 2.f);
  }
}

}  // namespace

// Sobe o processo do Lossless no lobby, sem o atalho. O Iniciar só cola na janela nova.
// relaunch=true mata e abre de novo (x2/x3/fila/Adaptive só entram num processo novo).
void lobby_park_lossless(const PersistedSettings& settings, bool relaunch) {
  LosslessFgController fg;
  std::string ferr;
  const bool want = settings.fg_enabled && lobby_fg_can_enable(settings.fg_path);
  if (!want) {
    (void)fg.apply_hdmi_profile_without_launch(false, settings.fg_multiplier, settings.fg_path, ferr,
                                               settings.fg_queue, settings.fg_adaptive, settings.fg_target_fps,
                                               display_refresh_hz(nullptr), settings.fg_flow_scale);
    return;
  }
  const int hz = display_refresh_hz(nullptr);
  fg.configure(hdmi_lobby_fg_multiplier(true, settings.fg_multiplier), settings.fg_path, settings.fg_queue,
               settings.fg_adaptive ? 1 : 0, settings.fg_target_fps, hz, -1.0, settings.fg_flow_scale);
  if (!fg.apply_profile(ferr)) return;
  const unsigned running = LosslessFgController::count_running_lossless_processes(true);
  if (running == 0 || relaunch) {
    if (!fg.ensure_process(ferr, /*allow_adopt=*/!relaunch)) return;
  }
  (void)fg.detach_for_restart();
}

struct LobbyLsGuard {
  bool keep = false;
  ~LobbyLsGuard() {
    if (keep) return;
    LosslessFgController fg;
    (void)fg.kill_all_lossless_and_wait(400);
  }
};

bool run_lobby(HINSTANCE hi, AppConfig& cfg, PersistedSettings& settings) {
  g_lobby_running = true;
  g_start_requested = false;
  settings.capture_source = lobby_boot_capture_source();
  g_open_gear = lobby_consume_open_cfg(cfg.open_lobby_cfg);
  bool force_nr_advanced = cfg.open_nr_advanced;
  cfg.open_nr_advanced = false;
  g_open_perf = false;
  const std::string last_path = default_perf_test_result_path(app_data_dir());
  const bool have_last = read_perf_test_result_file(last_path, g_perf_result) && g_perf_result.valid;
  g_show_perf_result = perf_test_should_popup_result(cfg.popup_perf_result, have_last, g_perf_result.popup);
  cfg.popup_perf_result = false;
  if (g_show_perf_result && g_perf_result.popup) {
    g_perf_result.popup = false;
    (void)write_perf_test_result_file(last_path, g_perf_result);
  }
  if (!read_perf_test_plan_file(default_perf_test_plan_path(app_data_dir()), g_perf_plan)) {
    g_perf_plan = PerfTestPlan{};
  }
  g_capture_vk_target = nullptr;

  // Scale is session-only. ReShade hooks lobby Present too — force feed 100% or the menu blurs.
  // Carry cfg.nr_runtime_on across Esc↔Iniciar (same RenoDX process — do not assume OFF).
  bool nr_runtime_on = cfg.nr_runtime_on;
  int nr_boot_retries = 0;
  bool renodx_dir_ok = true;
  BundledMotorProbe motor_probe{};
  {
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto exe_dir = std::filesystem::path(exe_path).parent_path();
    motor_probe = probe_bundled_motors(exe_dir.string());
    renodx_dir_ok = prepare_renodx_runtime_dirs(exe_dir.string());
    sync_feed_work_resolution_in_dir(
        exe_dir.string(), feed_work_resolution_at_boot(true, settings.scale_percent, settings.nr_pass_count));
    // Idle lobby: NR/Feed no disco só se CFG live. Iniciar grava o look da sessão no clique.
    const bool cfg_live =
        lobby_preview_cfg_wanted(settings.ui.preview_enabled, settings.ui.preview_cfg_live);
    const bool session_want = want_neural_rendering(settings.enhance_renodx, settings.neural_rendering);
    const bool strip_same_process = !cfg_live && nr_runtime_on;
    sync_lobby_preview_pipeline_files(exe_dir.string(), settings);
    if (cfg_live && session_want) {
      // Fresh attach lê NeuralUplift=1. Não pulsar — o enable-pulse inverte o runtime.
      nr_runtime_on = true;
      cfg.nr_runtime_on = true;
    } else {
      nr_runtime_on = false;
      cfg.nr_runtime_on = false;
    }
    if (strip_same_process &&
        relaunch_current_process(lobby_return_relaunch_args(false, g_show_perf_result))) {
      ExitProcess(0);
    }
  }

  // O processo do Lossless sobe com o app. O atalho só vai na captura, depois do 1º frame.
  LobbyLsGuard ls_guard;
  lobby_park_lossless(settings, /*relaunch=*/false);

  WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hi, nullptr, nullptr,
                 static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)), nullptr, L"NexusCapLobby", nullptr};
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(kAppIconResourceId));
  if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wc.hIconSm = wc.hIcon;
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }
  const std::wstring lobby_title = utf8_path_to_wide(app_window_title_lobby());
  g_hwnd = CreateWindowW(wc.lpszClassName, lobby_title.c_str(), WS_OVERLAPPEDWINDOW, 0, 0, 1280, 720, nullptr,
                         nullptr, wc.hInstance, nullptr);
  if (!g_hwnd || !create_device(g_hwnd)) {
    cleanup_device();
    return false;
  }
  apply_window_app_icon(g_hwnd, hi);
  // Windowed, covering the monitor work area (not exclusive fullscreen, not a tiny centered box).
  ShowWindow(g_hwnd, SW_SHOWMAXIMIZED);
  UpdateWindow(g_hwnd);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;
  io.FontGlobalScale = 1.45f;
  apply_lobby_theme();
  ImGui_ImplWin32_Init(g_hwnd);
  ImGui_ImplDX11_Init(g_dx.device, g_dx.ctx);

  ID3D11ShaderResourceView* logo_srv = nullptr;
  int logo_w = 0, logo_h = 0;
  create_logo_srv(g_dx.device, &logo_srv, &logo_w, &logo_h);
  ID3D11ShaderResourceView* watermark_srv = nullptr;
  int watermark_w = 0, watermark_h = 0;
  create_watermark_srv(g_dx.device, &watermark_srv, &watermark_w, &watermark_h);

  pipeline_global().stop();
  LobbyPreview preview;
  AudioMonitor lobby_audio;
  bool lobby_audio_want = false;
  bool fg_adp_committed = settings.fg_adaptive;
  int fg_tgt_committed = settings.fg_target_fps;
  bool fg_adp_draft = fg_adp_committed;
  int fg_tgt_draft = fg_tgt_committed;

  std::vector<std::string> devices = list_video_capture_devices();
  int device_idx = 0;
  for (size_t i = 0; i < devices.size(); ++i) {
    if (devices[i].find(settings.device_name) != std::string::npos || settings.device_name == devices[i]) {
      device_idx = static_cast<int>(i);
      break;
    }
  }

  std::vector<CapturableWindowInfo> windows;
  int window_idx = 0;
  auto refresh_windows = [&]() {
    windows = list_capturable_windows(reinterpret_cast<uint64_t>(g_hwnd));
    window_idx = 0;
    for (size_t i = 0; i < windows.size(); ++i) {
      if ((settings.window_hwnd != 0 && windows[i].hwnd == settings.window_hwnd) ||
          windows[i].title == settings.window_title) {
        window_idx = static_cast<int>(i);
        break;
      }
    }
    if (!windows.empty() && window_idx >= 0 && window_idx < static_cast<int>(windows.size())) {
      settings.window_title = windows[static_cast<size_t>(window_idx)].title;
      settings.window_hwnd = windows[static_cast<size_t>(window_idx)].hwnd;
    }
  };
  refresh_windows();

  auto current_device = [&]() -> std::string {
    if (!devices.empty() && device_idx >= 0 && device_idx < static_cast<int>(devices.size())) {
      return devices[static_cast<size_t>(device_idx)];
    }
    return settings.device_name.empty() ? std::string("auto") : settings.device_name;
  };

  auto refresh_modes = [&](std::vector<CaptureMode>& modes, int& mode_idx) {
    modes = list_video_capture_modes(current_device());
    mode_idx = 0;
    bool found_mode = false;
    for (size_t i = 0; i < modes.size(); ++i) {
      if (modes[i].width == settings.width && modes[i].height == settings.height &&
          std::abs(modes[i].fps - settings.fps) < 1.0) {
        mode_idx = static_cast<int>(i);
        found_mode = true;
        break;
      }
    }
    if (!found_mode) {
      int same = -1;
      double best_d = 1e9;
      for (size_t i = 0; i < modes.size(); ++i) {
        if (modes[i].width != settings.width || modes[i].height != settings.height) continue;
        const double d = std::abs(modes[i].fps - settings.fps);
        if (d < best_d) {
          best_d = d;
          same = static_cast<int>(i);
        }
      }
      if (same >= 0) mode_idx = same;
    }
    if (!modes.empty()) {
      settings.width = modes[static_cast<size_t>(mode_idx)].width;
      settings.height = modes[static_cast<size_t>(mode_idx)].height;
      settings.fps = modes[static_cast<size_t>(mode_idx)].fps;
    }
  };
  std::vector<CaptureMode> modes;
  int mode_idx = 0;
  refresh_modes(modes, mode_idx);

  // NEVER list fourccs every frame — reopens the card while preview holds it → freeze.
  std::vector<int> fourccs;
  std::string fourccs_cache_key;
  auto refresh_fourccs = [&]() {
    char key[320];
    std::snprintf(key, sizeof(key), "%s|%d|%d|%.3f", current_device().c_str(), settings.width,
                  settings.height, settings.fps);
    if (fourccs_cache_key == key && !fourccs.empty()) return;
    const auto accepted =
        list_video_fourccs_for_mode(current_device(), settings.width, settings.height, settings.fps);
    fourccs = capture_format_menu_for_mode(accepted);
    if (!accepted.empty()) {
      settings.video_fourcc =
          capture_format_for_mode(settings.width, settings.height, settings.video_fourcc, accepted);
    }
    fourccs_cache_key = key;
  };
  refresh_fourccs();

  auto restart_preview = [&]() -> bool {
    if (cfg.simulate_hdmi_busy) {
      preview.stop();
      preview.err = hdmi_open_busy_hint();
      return false;
    }
    if (!lobby_preview_capture_wanted(settings.ui.preview_enabled)) {
      preview.stop();
      return false;
    }
    CaptureStartOptions opt;
    opt.width = settings.width;
    opt.height = settings.height;
    opt.fps = settings.fps;
    opt.video_fourcc = settings.video_fourcc;
    opt.color_space = settings.color_space;
    opt.color_range = settings.color_range;
    opt.capture_buffering = settings.capture_buffering;
    if (capture_source_is_window(settings.capture_source)) {
      CaptureStartOptions wopt = opt;
      wopt.hwnd = settings.window_hwnd;
      wopt.wait_first_frame = false;
      return preview.ensure_window(settings.window_title, wopt);
    }
    settings.device_name = current_device();
    return preview.ensure(settings.device_name, opt);
  };
  if (!restart_preview()) {
    Sleep(static_cast<DWORD>(capture_device_settle_ms()));
    (void)restart_preview();
  }

  float scale_f = static_cast<float>(clamp_scale_percent(settings.scale_percent));
  // The lobby process always boots the Feed at 100% (menus would be downsampled too), so
  // any scale below that only takes effect in the relaunched session process.
  const int feed_loaded_scale = feed_work_resolution_for_lobby();
  bool gear_was_open = false;
  PersistedSettings gear_snapshot = settings;
  bool cfg_unsaved_ask = false;
  std::string lobby_toast;
  double lobby_toast_until = 0.0;
  bool hdmi_iniciar_fail_open = false;
  bool motor_swap_pending = false;
  bool motor_swap_silent = false;
  bool motor_swap_enter_session = false;
  bool motor_swap_sync_session_files = false;
  double motor_swap_until = 0.0;
  std::string motor_swap_title = lobby_pipeline_hold_title();
  std::string motor_swap_line;
  std::string motor_swap_args;  // "--autostart" resumes the session after the restart
  bool motor_swap_refresh_lobby = false;
  auto push_lobby_toast = [&](std::string msg) {
    if (msg.empty()) return;
    lobby_toast = std::move(msg);
    lobby_toast_until = ImGui::GetTime() + (kSessionNoticeTtlMs / 1000.0);
  };

  std::vector<std::string> lobby_preset_names;
  std::vector<std::string> lobby_preset_summaries;
  int lobby_preset_sel = -1;
  char lobby_preset_save_name[kUserPresetNameBuf]{};
  bool lobby_preset_save_open = false;
  bool lobby_preset_rename_open = false;
  bool lobby_preset_ow = false;
  bool lobby_preset_rename_ow = false;
  bool lobby_preset_del = false;
  bool lobby_preset_imp_ow = false;
  std::string lobby_preset_imp_text;
  auto reload_lobby_presets = [&]() {
    std::string keep;
    if (lobby_preset_sel >= 0 && lobby_preset_sel < static_cast<int>(lobby_preset_names.size())) {
      keep = lobby_preset_names[static_cast<size_t>(lobby_preset_sel)];
    }
    lobby_preset_names.clear();
    lobby_preset_summaries.clear();
    std::vector<UserPreset> have;
    list_user_presets_in(default_user_presets_path(), have);
    for (const auto& p : have) {
      lobby_preset_names.push_back(p.name);
      lobby_preset_summaries.push_back(user_preset_list_summary(p));
    }
    lobby_preset_sel = keep.empty() ? -1 : find_user_preset_index(have, keep);
  };
  reload_lobby_presets();
  auto adv_now = [&]() -> const NrAdvancedLook* {
    return &settings.nr_advanced[clamp_nr_pass_count(settings.nr_pass_count)];
  };
  NrAdvancedLook adv_draft = settings.nr_advanced[clamp_nr_pass_count(settings.nr_pass_count)];
  int adv_draft_pass = clamp_nr_pass_count(settings.nr_pass_count);
  auto apply_lobby_named_preset = [&](const UserPreset& p) {
    const bool fg_ok = lobby_fg_can_enable(settings.fg_path);
    if (user_preset_already_active(user_preset_from_persisted({}, settings), p, fg_ok)) {
      push_lobby_toast(user_preset_already_active_toast(p.name));
      return;
    }
    const LobbyUserPresetPlan plan = plan_lobby_user_preset_apply(settings, p, fg_ok);
    apply_user_preset_to_persisted(settings, p, fg_ok);
    scale_f = static_cast<float>(clamp_scale_percent(settings.scale_percent));
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto exe_dir = std::filesystem::path(exe_path).parent_path().string();
    std::string serr;
    save_settings(settings, settings_default_path(), serr);
    gear_snapshot = settings;
    fg_adp_committed = settings.fg_adaptive;
    fg_tgt_committed = settings.fg_target_fps;
    fg_adp_draft = fg_adp_committed;
    fg_tgt_draft = fg_tgt_committed;
    const bool live = lobby_preview_cfg_wanted(settings.ui.preview_enabled, settings.ui.preview_cfg_live);
    const bool want = want_neural_rendering(settings.enhance_renodx, settings.neural_rendering);
    if ((plan.pass_relaunch || plan.advanced_changed) && live) {
      (void)sync_nr_pass_count_files(exe_dir, settings.nr_pass_count, settings.renodx_preset, adv_now());
      (void)sync_renodx_visual_preset_files(exe_dir, settings.renodx_preset, settings.nr_pass_count, adv_now());
      (void)sync_feed_work_resolution_in_dir(
          exe_dir, feed_work_resolution_for_session(settings.scale_percent, settings.nr_pass_count,
                                                   settings.width, settings.height),
          settings.nr_pass_count);
      motor_swap_title = lobby_pipeline_hold_title();
      motor_swap_line = std::string(nr_pass_count_label(plan.pass_before)) + "  \xE2\x86\x92  " +
                        nr_pass_count_label(plan.pass_after);
      motor_swap_args = g_open_gear ? lobby_cfg_relaunch_args() : "";
      motor_swap_pending = true;
      motor_swap_silent = false;
      motor_swap_enter_session = false;
      motor_swap_until = ImGui::GetTime() + kDlss5MotorSwapHoldSec;
      write_restart_toast(session_notice_named_preset_apply(p.name));
    } else if (live) {
      preview.set_visual_preview(settings.renodx_preset, want);
      if (plan.look_changed) {
        apply_lobby_renodx_preset_live(exe_dir, settings.renodx_preset, want, &nr_runtime_on,
                                       settings.nr_pass_count, adv_now());
      } else {
        (void)sync_nr_pass_count_files(exe_dir, settings.nr_pass_count, settings.renodx_preset, adv_now());
        (void)sync_renodx_visual_preset_files(exe_dir, settings.renodx_preset, settings.nr_pass_count,
                                              adv_now());
      }
      sync_lobby_preview_pipeline_files(exe_dir, settings);
      if (!apply_neural_rendering_state(exe_dir, want, &nr_runtime_on,
                                        feed_pipeline_on_for_renodx(settings.enhance_renodx)) &&
          want && !dlss5_bridge_ready_for_nr()) {
        nr_boot_retries = 120;
      }
      cfg.nr_runtime_on = nr_runtime_on;
    } else {
      sync_lobby_preview_pipeline_files(exe_dir, settings);
      (void)sync_nr_pass_count_files(exe_dir, settings.nr_pass_count, settings.renodx_preset, adv_now());
      (void)sync_renodx_visual_preset_files(exe_dir, settings.renodx_preset, settings.nr_pass_count, adv_now());
    }
    if (plan.fg_missing) {
      push_lobby_toast(session_notice_fg_missing());
    } else if (!((plan.pass_relaunch || plan.advanced_changed) && live)) {
      push_lobby_toast(session_notice_named_preset_apply(p.name));
    }
    adv_draft = settings.nr_advanced[clamp_nr_pass_count(settings.nr_pass_count)];
    adv_draft_pass = clamp_nr_pass_count(settings.nr_pass_count);
  };
  if (!renodx_dir_ok) {
    push_lobby_toast(renodx_write_denied_notice());
  } else {
    wchar_t exe_path_h[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path_h, MAX_PATH);
    const auto health = check_runtime_health(std::filesystem::path(exe_path_h).parent_path().string());
    if (!health.writable) {
      push_lobby_toast(renodx_write_denied_notice());
    } else if (!health.renodx_complete()) {
      push_lobby_toast("RenoDX incompleto (dxgi/addon/feed). Reinstale o Nexus Cap ou use Abrir logs.");
    }
  }
  if (auto t = take_restart_toast(); !t.empty()) {
    push_lobby_toast(std::move(t));
  } else if (!cfg.lobby_boot_toast.empty()) {
    push_lobby_toast(std::move(cfg.lobby_boot_toast));
    cfg.lobby_boot_toast.clear();
  }

  while (g_lobby_running) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      // Spurious WM_QUIT after Esc→session→lobby must NOT cancel the menu (old bug: app closed).
      if (lobby_message_is_spurious_quit(msg.message)) {
        continue;
      }
      if (lobby_message_exits_app(msg.message)) {
        g_start_requested = false;
        g_lobby_running = false;
      }
    }
    if (!g_lobby_running) break;

    preview.set_visual_preview(
        settings.renodx_preset,
        lobby_preview_cfg_wanted(settings.ui.preview_enabled, settings.ui.preview_cfg_live) &&
            want_neural_rendering(settings.enhance_renodx, settings.neural_rendering));
    if (lobby_preview_capture_wanted(settings.ui.preview_enabled)) {
      preview.tick();
    } else if (preview.live) {
      preview.stop();
    }

    // Preview audio: start/stop with live capture; mute zeros WASAPI frames (keeps path warm).
    {
      const bool want_audio = lobby_preview_audio_wanted(settings.ui.preview_enabled, preview.live) &&
                              !capture_source_is_window(settings.capture_source);
      if (want_audio != lobby_audio_want) {
        lobby_audio_want = want_audio;
        if (want_audio) {
          std::string aerr;
          const std::string prefer =
              is_auto_device(settings.device_name) ? std::string{} : settings.device_name;
          if (!lobby_audio.start(prefer, aerr)) {
            lobby_audio_want = false;
          }
        } else {
          lobby_audio.stop();
        }
      }
      if (lobby_audio.running()) {
        lobby_audio.set_muted(settings.ui.preview_mute);
        lobby_audio.set_volume(settings.capture_volume);
      }
    }

    // Lobby Present loads Bridge asynchronously — pulse only when carried runtime ≠ want.
    if (nr_boot_retries > 0) {
      --nr_boot_retries;
      const bool want =
          lobby_preview_cfg_wanted(settings.ui.preview_enabled, settings.ui.preview_cfg_live) &&
          want_neural_rendering(settings.enhance_renodx, settings.neural_rendering);
      if (want == nr_runtime_on) {
        nr_boot_retries = 0;
      } else if (dlss5_bridge_ready_for_nr()) {
        wchar_t exe_path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        {
          const auto exe_dir = std::filesystem::path(exe_path).parent_path().string();
          if (apply_neural_rendering_state(exe_dir, want, &nr_runtime_on)) {
            nr_boot_retries = 0;
            cfg.nr_runtime_on = nr_runtime_on;
            if (!want) {
              sync_neural_rendering_enabled_files(
                  exe_dir, want_neural_rendering(settings.enhance_renodx, settings.neural_rendering),
                  feed_pipeline_on_for_renodx(settings.enhance_renodx));
            }
          }
        }
      }
    }

    g_lobby_gear_vk = settings.hotkeys.vk_gear;
    g_lobby_gear_ctrl = settings.hotkeys.gear_ctrl;
    g_lobby_hotkeys = settings.hotkeys;
    if (const int step = g_lobby_pass_step.exchange(0)) {
      if (!motor_swap_pending && settings.enhance_renodx) {
        const int before = clamp_nr_pass_count(settings.nr_pass_count);
        const int after = step_nr_pass_count(before, step);
        settings.nr_pass_count = after;
        settings.scale_percent = nr_pass_count_on_select_scale(before, after, settings.scale_percent);
        scale_f = static_cast<float>(settings.scale_percent);
        wchar_t exe_path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        const auto exe_dir = std::filesystem::path(exe_path).parent_path().string();
        const int pass = clamp_nr_pass_count(after);
        const NrAdvancedLook* look = &settings.nr_advanced[pass];
        if (!prepare_renodx_runtime_dirs(exe_dir)) {
          push_lobby_toast(renodx_write_denied_notice());
        } else if (lobby_preview_cfg_wanted(settings.ui.preview_enabled, settings.ui.preview_cfg_live)) {
          std::string serr;
          save_settings(settings, settings_default_path(), serr);
          gear_snapshot = settings;
          (void)sync_nr_pass_count_files(exe_dir, pass, settings.renodx_preset, look);
          (void)sync_feed_work_resolution_in_dir(
              exe_dir, feed_work_resolution_for_session(settings.scale_percent, pass, settings.width, settings.height),
              pass);
          motor_swap_title = lobby_pipeline_hold_title();
          motor_swap_line = std::string(nr_pass_count_label(before)) + "  \xE2\x86\x92  " +
                            nr_pass_count_label(after);
          motor_swap_args = lobby_cfg_relaunch_args();
          motor_swap_pending = true;
          motor_swap_silent = false;
          motor_swap_enter_session = false;
          motor_swap_until = ImGui::GetTime() + kDlss5MotorSwapHoldSec;
          write_restart_toast(session_notice_nr_pass_count_apply(before, after));
        } else {
          std::string serr;
          save_settings(settings, settings_default_path(), serr);
          gear_snapshot = settings;
          (void)sync_nr_pass_count_files(exe_dir, pass, settings.renodx_preset, look);
          push_lobby_toast(std::string("Pass Count: ") + nr_pass_count_label(after));
        }
      }
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##lobby_root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::BeginChild("header", ImVec2(0, 72), false, ImGuiWindowFlags_NoScrollbar);
    {
      const float header_y = ImGui::GetCursorPosY();
      if (watermark_srv) {
        ImGui::SetCursorPosY(header_y);
        ImGui::Image(reinterpret_cast<ImTextureID>(watermark_srv), ImVec2(60, 64));
        ImGui::SameLine();
      }
      if (logo_srv) {
        ImGui::SetCursorPosY(header_y + (watermark_srv ? 8.f : 0.f));
        ImGui::Image(reinterpret_cast<ImTextureID>(logo_srv), ImVec2(48, 48));
      } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.55f, 0.95f, 1.f));
        ImGui::Button("N", ImVec2(48, 48));
        ImGui::PopStyleColor();
      }
      ImGui::SameLine();
      ImGui::BeginGroup();
      ImGui::SetWindowFontScale(1.15f);
      ImGui::TextUnformatted(kAppNameWithVersion);
      ImGui::SetWindowFontScale(1.0f);
      ImGui::TextDisabled("Console de captura");
      ImGui::EndGroup();
      ImGui::SameLine(ImGui::GetWindowWidth() - 220);
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.95f, 1.f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.62f, 1.f, 1.f));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.45f, 0.85f, 1.f));
      if (ImGui::Button(perf_test_lobby_button(), ImVec2(120, 40))) {
        g_open_perf = true;
        g_open_gear = false;
        (void)resolve_perf_test_video(exe_dir_utf8(), g_perf_plan.width, g_perf_plan.height, g_perf_plan.video);
      }
      ImGui::SameLine();
      if (ImGui::Button("CFG", ImVec2(72, 40))) g_open_gear = true;
      ImGui::PopStyleColor(3);
    }
    ImGui::EndChild();
    ImGui::Separator();

    // Pixel-tight vertical budget: body + ItemSpacing + footer == ContentRegionAvail
    // (ImGui inserts ItemSpacing between the body row and the footer child — do NOT add Dummy).
    const float item_sp_y = ImGui::GetStyle().ItemSpacing.y;
    const bool motor_warn = motor_probe.wanted();
    const float footer_h = motor_warn ? 118.f : 72.f;
    const float avail_y = ImGui::GetContentRegionAvail().y;
    const float body_h = (std::max)(200.f, avail_y - footer_h - item_sp_y);
    const float body_w = ImGui::GetContentRegionAvail().x;
    const float side_gap = ImGui::GetStyle().ItemSpacing.x;
    // Leave room for two SameLine gaps so the right panel is not clipped.
    const float usable_w = (std::max)(480.f, body_w - side_gap * 2.f);
    const int sig_w = preview.tex_w > 0 ? preview.tex_w : settings.width;
    const int sig_h = preview.tex_h > 0 ? preview.tex_h : settings.height;
    const auto sides = lobby_side_panel_layout(usable_w, body_h, sig_w, sig_h, 375.f, 525.f, 280.f);
    const float left_w = sides.left_w;
    const float right_w = sides.right_w;
    const float mid_w = sides.mid_w;

    // Side menus aligned to letterbox black-bar widths; preview fills the center column.
    ImGui::BeginChild("origem", ImVec2(left_w, body_h), true);
    ImGui::TextDisabled("ORIGEM");
    ImGui::TextUnformatted("De onde vem a imagem");
    ImGui::Spacing();
    {
      const int src_hdmi = static_cast<int>(CaptureSourceKind::Hdmi);
      const int src_win = static_cast<int>(CaptureSourceKind::Window);
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 6.f));
      if (lobby_draw_origem_card("hdmi", false, settings.capture_source == src_hdmi)) {
        settings.capture_source = src_hdmi;
        restart_preview();
      }
      if (lobby_draw_origem_card("win", true, settings.capture_source == src_win)) {
        settings.capture_source = src_win;
        refresh_windows();
        restart_preview();
      }
      ImGui::PopStyleVar();
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (capture_source_is_window(settings.capture_source)) {
      ImGui::TextDisabled("ESCOLHER JANELA");
      std::vector<std::string> wlabel_store;
      std::vector<const char*> wlabels;
      wlabel_store.reserve(windows.size());
      wlabels.reserve(windows.size());
      for (auto& w : windows) {
        std::string lab = w.title;
        for (size_t p = 0; p + 1 < lab.size(); ++p) {
          if (lab[p] == '#' && lab[p + 1] == '#') lab[p + 1] = ' ';
        }
        wlabel_store.push_back(std::move(lab));
      }
      for (auto& s : wlabel_store) wlabels.push_back(s.c_str());
      if (wlabels.empty()) {
        ImGui::TextDisabled("(nenhuma janela visivel)");
      } else {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##win", &window_idx, wlabels.data(), static_cast<int>(wlabels.size()))) {
          if (window_idx >= 0 && window_idx < static_cast<int>(windows.size())) {
            settings.window_title = windows[static_cast<size_t>(window_idx)].title;
            settings.window_hwnd = windows[static_cast<size_t>(window_idx)].hwnd;
            restart_preview();
          }
        }
      }
      if (ImGui::Button("Atualizar lista", ImVec2(-1, 0))) {
        refresh_windows();
        restart_preview();
      }
      ImGui::Spacing();
      ImGui::PushTextWrapPos(0.f);
      ImGui::TextDisabled("O som fica no app de origem. Fullscreen exclusivo e DRM nao aparecem.");
      ImGui::PopTextWrapPos();
    } else {
    ImGui::TextDisabled("DISPOSITIVO");
    if (devices.empty()) {
      ImGui::TextDisabled("(nenhum — Recarregar)");
    } else {
      std::vector<const char*> labels;
      labels.reserve(devices.size());
      for (auto& d : devices) labels.push_back(d.c_str());
      ImGui::SetNextItemWidth(-1);
      if (ImGui::Combo("##dev", &device_idx, labels.data(), static_cast<int>(labels.size()))) {
        settings.device_name = devices[static_cast<size_t>(device_idx)];
        fourccs_cache_key.clear();
        refresh_modes(modes, mode_idx);
        refresh_fourccs();
        restart_preview();
      }
    }

    if (modes.empty()) {
      ImGui::TextDisabled("MODO");
      ImGui::TextDisabled("(sem caps — %dx%d @ %.0f)", settings.width, settings.height, settings.fps);
    } else {
      std::vector<int> res_w;
      std::vector<int> res_h;
      capture_unique_resolutions(modes, res_w, res_h);
      int res_idx = 0;
      for (size_t i = 0; i < res_w.size(); ++i) {
        if (res_w[i] == settings.width && res_h[i] == settings.height) {
          res_idx = static_cast<int>(i);
          break;
        }
      }
      std::vector<double> hz;
      if (!res_w.empty()) capture_hz_for_resolution(modes, res_w[static_cast<size_t>(res_idx)],
                                                   res_h[static_cast<size_t>(res_idx)], hz);
      int hz_idx = capture_hz_index_for_fps(hz, settings.fps);

      auto sync_mode_idx = [&]() {
        mode_idx = 0;
        for (size_t i = 0; i < modes.size(); ++i) {
          if (modes[i].width == settings.width && modes[i].height == settings.height &&
              std::abs(modes[i].fps - settings.fps) < 1.0) {
            mode_idx = static_cast<int>(i);
            break;
          }
        }
      };
      auto apply_capture_mode = [&]() {
        fourccs_cache_key.clear();
        refresh_fourccs();
        restart_preview();
      };

      std::vector<std::string> res_store;
      std::vector<const char*> res_labels;
      res_store.reserve(res_w.size());
      for (size_t i = 0; i < res_w.size(); ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%dx%d", res_w[i], res_h[i]);
        res_store.emplace_back(buf);
      }
      for (auto& s : res_store) res_labels.push_back(s.c_str());
      ImGui::TextDisabled("RESOLUCAO");
      ImGui::SetNextItemWidth(-1);
      if (!res_labels.empty() &&
          ImGui::Combo("##res", &res_idx, res_labels.data(), static_cast<int>(res_labels.size()))) {
        settings.width = res_w[static_cast<size_t>(res_idx)];
        settings.height = res_h[static_cast<size_t>(res_idx)];
        capture_hz_for_resolution(modes, settings.width, settings.height, hz);
        hz_idx = capture_hz_index_for_fps(hz, settings.fps);
        if (!hz.empty()) settings.fps = hz[static_cast<size_t>(hz_idx)];
        sync_mode_idx();
        apply_capture_mode();
      }

      std::vector<std::string> hz_store;
      std::vector<const char*> hz_labels;
      hz_store.reserve(hz.size());
      for (double f : hz) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f Hz", f);
        hz_store.emplace_back(buf);
      }
      for (auto& s : hz_store) hz_labels.push_back(s.c_str());
      ImGui::TextDisabled("HZ");
      ImGui::SetNextItemWidth(-1);
      if (hz_labels.empty()) {
        ImGui::TextDisabled("(sem Hz)");
      } else if (ImGui::Combo("##hz", &hz_idx, hz_labels.data(), static_cast<int>(hz_labels.size()))) {
        settings.fps = hz[static_cast<size_t>(hz_idx)];
        sync_mode_idx();
        apply_capture_mode();
      }
    }

    ImGui::Spacing();
    ImGui::TextDisabled("FORMATO / COR");
    refresh_fourccs();
    const float col_w = (std::max)(110.f, (ImGui::GetContentRegionAvail().x - 8.f) * 0.5f);
    {
      int fmt_idx = 0;
      std::vector<std::string> fmt_store;
      std::vector<const char*> fmt_labels;
      fmt_store.reserve(fourccs.size());
      for (size_t i = 0; i < fourccs.size(); ++i) {
        fmt_store.emplace_back(video_fourcc_name(fourccs[i]));
        if (fourccs[i] == settings.video_fourcc) fmt_idx = static_cast<int>(i);
      }
      for (auto& s : fmt_store) fmt_labels.push_back(s.c_str());
      ImGui::BeginGroup();
      ImGui::TextUnformatted("Formato");
      ImGui::SetNextItemWidth(col_w);
      if (fmt_labels.empty()) {
        ImGui::TextDisabled("(sem formato)");
      } else if (ImGui::Combo("##fmt", &fmt_idx, fmt_labels.data(), static_cast<int>(fmt_labels.size()))) {
        const int prev = settings.video_fourcc;
        settings.video_fourcc = fourccs[static_cast<size_t>(fmt_idx)];
        if (!restart_preview()) {
          settings.video_fourcc = prev;
          restart_preview();
        } else if (!preview.err.empty() && preview.err.find("falhou") != std::string::npos) {
          settings.video_fourcc = static_cast<int>(kDefaultVideoFourcc);
        }
      }
      ImGui::EndGroup();
    }
    ImGui::SameLine(0.f, 8.f);
    {
      const char* space_labels[kObsColorSpaceUiCount];
      int ui_idx = 0;
      for (int i = 0; i < kObsColorSpaceUiCount; ++i) {
        space_labels[i] = color_space_name(kObsColorSpaceUiOrder[i]);
        if (kObsColorSpaceUiOrder[i] == clamp_color_space(settings.color_space)) ui_idx = i;
      }
      ImGui::BeginGroup();
      ImGui::TextUnformatted("Espaco");
      ImGui::SetNextItemWidth(col_w);
      if (ImGui::Combo("##cspace", &ui_idx, space_labels, kObsColorSpaceUiCount)) {
        settings.color_space = kObsColorSpaceUiOrder[ui_idx];
        preview.apply_color(settings.color_space, settings.color_range);
      }
      ImGui::EndGroup();
    }
    {
      const char* range_labels[] = {"Limitado", "Completo"};
      int rg = clamp_color_range(settings.color_range);
      ImGui::BeginGroup();
      ImGui::TextUnformatted("Intervalo");
      ImGui::SetNextItemWidth(col_w);
      if (ImGui::Combo("##crange", &rg, range_labels, 2)) {
        settings.color_range = rg;
        preview.apply_color(settings.color_space, settings.color_range);
      }
      ImGui::EndGroup();
    }
    ImGui::SameLine(0.f, 8.f);
    {
      const char* buf_labels[] = {"Desativado", "Ativado"};
      int bf = clamp_capture_buffering(settings.capture_buffering);
      ImGui::BeginGroup();
      ImGui::TextUnformatted("Buffering");
      ImGui::SameLine();
      ImGui::TextDisabled("(?)");
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Desativado = baixa latencia (padrao validado).\nAtivado = mais buffers na placa.");
      }
      ImGui::SetNextItemWidth(col_w);
      if (ImGui::Combo("##cbuf", &bf, buf_labels, 2)) {
        settings.capture_buffering = bf;
        restart_preview();
      }
      ImGui::EndGroup();
    }
    if (is_validated_default_capture_format(settings.video_fourcc, settings.color_space, settings.color_range,
                                           settings.capture_buffering)) {
      ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.5f, 1.f), "Padrao validado");
    }
    if (ImGui::Button("Recarregar entradas", ImVec2(-1, 0))) {
      devices = list_video_capture_devices();
      device_idx = 0;
      if (!devices.empty()) settings.device_name = devices[0];
      fourccs_cache_key.clear();
      refresh_modes(modes, mode_idx);
      refresh_fourccs();
      restart_preview();
    }
    }  // HDMI origem
    if (!preview.err.empty()) {
      ImGui::PushTextWrapPos(0.f);
      ImGui::TextColored(ImVec4(1.f, 0.55f, 0.2f, 1.f), "%s", hdmi_open_error_user(preview.err).c_str());
      ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("centro", ImVec2(mid_w, body_h), false, ImGuiWindowFlags_NoScrollbar);
    {
      // Preview takes the full center column (no metrics strip under it).
      const float preview_h = (std::max)(120.f, ImGui::GetContentRegionAvail().y);
      ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.04f, 0.05f, 0.07f, 1.f));
      ImGui::BeginChild("preview_band", ImVec2(0, preview_h), true,
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
      {
        if (capture_source_is_window(settings.capture_source)) {
          const int pw = preview.tex_w > 0 ? preview.tex_w : 0;
          const int ph = preview.tex_h > 0 ? preview.tex_h : 0;
          if (pw >= 2 && ph >= 2) {
            ImGui::TextDisabled("PRE-VISUALIZACAO  ·  JANELA  ·  %dx%d", pw, ph);
          } else {
            ImGui::TextDisabled("PRE-VISUALIZACAO  ·  JANELA");
          }
        } else {
          ImGui::TextDisabled("PRE-VISUALIZACAO  ·  HDMI  ·  %dx%d @ %.0f", settings.width, settings.height,
                              settings.fps);
        }
        {
          const char* pv = lobby_preview_toggle_label(settings.ui.preview_enabled);
          const char* cfgp = lobby_preview_cfg_toggle_label(settings.ui.preview_cfg_live);
          const char* sm = lobby_audio_toggle_label(settings.ui.preview_mute);
          const float pad_x = ImGui::GetStyle().FramePadding.x * 2.f;
          const float gap = ImGui::GetStyle().ItemSpacing.x;
          const float bw1 = ImGui::CalcTextSize(pv).x + pad_x;
          const float bw_cfg = ImGui::CalcTextSize(cfgp).x + pad_x;
          const float bw2 = ImGui::CalcTextSize(sm).x + pad_x;
          const float status_w = 88.f;
          const float total = bw1 + gap + bw_cfg + gap + bw2 + gap + status_w + 12.f;
          ImGui::SameLine(ImGui::GetWindowWidth() - total);
          auto queue_preview_pipeline_relaunch = [&]() {
            wchar_t exe_path[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
            const auto exe_dir = std::filesystem::path(exe_path).parent_path().string();
            sync_lobby_preview_pipeline_files(exe_dir, settings);
            const bool live =
                lobby_preview_cfg_wanted(settings.ui.preview_enabled, settings.ui.preview_cfg_live);
            const bool session_want =
                want_neural_rendering(settings.enhance_renodx, settings.neural_rendering);
            preview.set_visual_preview(settings.renodx_preset, live && session_want);
            nr_runtime_on = live && session_want;
            cfg.nr_runtime_on = nr_runtime_on;
            motor_swap_title = lobby_pipeline_hold_title();
            motor_swap_line = lobby_preview_cfg_pipeline_hold_line(live);
            motor_swap_args = g_open_gear ? lobby_cfg_relaunch_args() : "";
            motor_swap_refresh_lobby = true;
            motor_swap_pending = true;
            motor_swap_silent = false;
            motor_swap_enter_session = false;
            motor_swap_until = ImGui::GetTime() + kDlss5MotorSwapHoldSec;
            write_restart_toast(lobby_preview_cfg_pipeline_hold_line(live));
          };
          if (ImGui::SmallButton(pv)) {
            const bool pipeline_was =
                lobby_preview_cfg_wanted(settings.ui.preview_enabled, settings.ui.preview_cfg_live);
            settings.ui.preview_enabled = !settings.ui.preview_enabled;
            std::string serr;
            save_settings(settings, settings_default_path(), serr);
            if (settings.ui.preview_enabled) {
              restart_preview();
            } else {
              preview.stop();
              lobby_audio.stop();
              lobby_audio_want = false;
            }
            const bool pipeline_now =
                lobby_preview_cfg_wanted(settings.ui.preview_enabled, settings.ui.preview_cfg_live);
            if (pipeline_was != pipeline_now) queue_preview_pipeline_relaunch();
          }
          ImGui::SameLine();
          ImGui::BeginDisabled(!settings.ui.preview_enabled);
          if (ImGui::SmallButton(cfgp)) {
            settings.ui.preview_cfg_live = !settings.ui.preview_cfg_live;
            std::string serr;
            save_settings(settings, settings_default_path(), serr);
            queue_preview_pipeline_relaunch();
          }
          ImGui::EndDisabled();
          ImGui::SameLine();
          if (ImGui::SmallButton(sm)) {
            settings.ui.preview_mute = !settings.ui.preview_mute;
            std::string serr;
            save_settings(settings, settings_default_path(), serr);
            if (lobby_audio.running()) {
              lobby_audio.set_muted(settings.ui.preview_mute);
              lobby_audio.set_volume(settings.capture_volume);
            }
          }
          ImGui::SameLine();
          if (!settings.ui.preview_enabled) {
            ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.35f, 1.f), "OFF");
          } else if (preview.live && preview.frames_seen > 0) {
            ImGui::TextColored(ImVec4(0.3f, 0.95f, 0.45f, 1.f), "AO VIVO");
          } else if (preview.live) {
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.2f, 1.f), "AGUARDANDO");
          } else {
            ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.35f, 1.f), "OFF");
          }
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 1.f));
        const float fill_h = (std::max)(40.f, ImGui::GetContentRegionAvail().y);
        ImGui::BeginChild("preview_fill", ImVec2(0, fill_h), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImVec2 band_min{}, band_max{}, img_min{}, img_max{};
        bool hud_has_img = false;
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        band_min = origin;
        band_max = ImVec2(origin.x + avail.x, origin.y + avail.y);
        if (!settings.ui.preview_enabled) {
          img_min = band_min;
          img_max = band_max;
          hud_has_img = false;
          ImGui::SetCursorPos(ImVec2(16.f, avail.y * 0.35f));
          ImGui::TextDisabled("Preview desligado");
          ImGui::TextDisabled("Ative o preview para ver o sinal HDMI (o Iniciar nao muda).");
        } else if (preview.srv && preview.tex_w > 0 && preview.tex_h > 0 && avail.x > 1.f && avail.y > 1.f) {
          // Fill center column — no letterbox black bars (side menus already own that space).
          const auto box =
              fit_fill(preview.tex_w, preview.tex_h, static_cast<int>(avail.x), static_cast<int>(avail.y));
          img_min = ImVec2(origin.x + box.x, origin.y + box.y);
          img_max = ImVec2(img_min.x + box.w, img_min.y + box.h);
          ImDrawList* dl = ImGui::GetWindowDrawList();
          dl->AddImage(reinterpret_cast<ImTextureID>(preview.srv), img_min, img_max);
          ImGui::Dummy(avail);
          hud_has_img = true;
        } else {
          img_min = band_min;
          img_max = band_max;
          hud_has_img = true;
          ImGui::SetCursorPos(ImVec2(16.f, avail.y * 0.35f));
          if (!preview.err.empty()) {
            ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "Preview: %s",
                               hdmi_open_error_user(preview.err).c_str());
          } else if (preview.live) {
            ImGui::TextDisabled("Aguardando sinal da placa...");
          } else {
            ImGui::TextDisabled("Sem preview (escolha dispositivo / Recarregar)");
          }
          ImGui::Text("  %s", settings.device_name.empty() ? "auto" : settings.device_name.c_str());
          ImGui::TextDisabled("  %s · %s · %s", video_fourcc_name(settings.video_fourcc),
                              color_space_name(settings.color_space), color_range_name(settings.color_range));
        }
        if (hud_has_img && settings.ui.preview_enabled) {
          draw_hud_preview_overlay(settings, preview, band_min, band_max, img_min, img_max);
        }
        if (!settings.ui.show_fps) {
          ImGui::SetCursorPos(ImVec2(12, 8));
          ImGui::TextColored(ImVec4(0.75f, 0.9f, 1.f, 1.f), "HUD off (marque Exibir metricas)");
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
      }
      ImGui::EndChild();
      ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("sessao", ImVec2(right_w, body_h), true);
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("SESSAO");
    auto session_tip = [](const char* tip) {
      if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tip);
    };
    ImGui::Spacing();
    ImGui::TextDisabled("OVERLAY");
    ImGui::Checkbox("Exibir metricas", &settings.ui.show_fps);
    session_tip("FPS, latencia e graficos na imagem");
    if (ImGui::Checkbox("Mini overlay", &settings.ui.mini_overlay)) {
      if (settings.ui.mini_overlay) settings.ui.advanced_metrics = false;
    }
    session_tip("Barra curta no topo, com o FPS da captura e do present");
    ImGui::Checkbox("Sem fundo", &settings.ui.overlay_no_bg);
    session_tip(settings.ui.overlay_no_bg ? "So o texto, sem a placa preta" : "Placa preta atras do texto");

    ImGui::BeginDisabled(settings.ui.mini_overlay);
    ImGui::Checkbox("Metricas avancadas", &settings.ui.advanced_metrics);
    session_tip(settings.ui.mini_overlay ? "Indisponivel com o mini overlay" : "Grafico e janela de 600 frames");
    ImGui::Checkbox("GPU e CPU", &settings.ui.hw_info);
    session_tip("Uso de GPU, CPU e memoria");
    ImGui::EndDisabled();

    if (ImGui::Checkbox("Overlay live", &settings.ui.live_overlay)) {
      if (settings.ui.live_overlay) settings.ui.overlay_console = true;
    }
    session_tip("Overlay por cima de tudo, fora da janela do jogo");
    ImGui::Checkbox("Console", &settings.ui.overlay_console);
    session_tip("Nome do console no overlay");
    if (settings.ui.overlay_console) {
      LiveConsole cur = parse_live_console(settings.ui.live_console);
      auto console_imcol = [](LiveConsole c) -> ImVec4 {
        const COLORREF rgb = live_console_color(c);
        return ImVec4(GetRValue(rgb) / 255.f, GetGValue(rgb) / 255.f, GetBValue(rgb) / 255.f, 1.f);
      };
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Text, console_imcol(cur));
      char console_btn[48]{};
      std::snprintf(console_btn, sizeof(console_btn), "%s##console_pick", live_console_label(cur));
      if (ImGui::Button(console_btn)) ImGui::OpenPopup("##console_menu");
      ImGui::PopStyleColor();
      if (ImGui::BeginPopup("##console_menu")) {
        for (LiveConsole o : kLiveConsoleOrder) {
          const bool selected = (o == cur);
          ImGui::PushStyleColor(ImGuiCol_Text, console_imcol(o));
          if (ImGui::MenuItem(live_console_label(o), nullptr, selected)) {
            settings.ui.live_console = live_console_key(o);
          }
          ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
      }
    }
    ImGui::Spacing();
    ImGui::TextDisabled("IMAGEM");
    ImGui::Checkbox("Tela cheia", &settings.ui.fullscreen);
    settings.ui.hide_panel_on_capture = true;
    session_tip(settings.ui.fullscreen ? "A sessao abre em tela cheia" : "A sessao abre em janela");
    ImGui::Checkbox("Sincronizar imagem", &settings.ui.sync_image);
    session_tip(settings.ui.sync_image ? "VSync ligado" : "VSync desligado");

    ImGui::Spacing();
    ImGui::TextDisabled("RELATORIO");
    ImGui::Checkbox("Reportar bug", &settings.ui.bug_report);
    session_tip(settings.ui.bug_report ? "Grava diagnostico em report/" : "Desligado");
    if (ImGui::Checkbox("Reportar performance", &settings.ui.perf_report)) {
      std::string serr;
      save_settings(settings, settings_default_path(), serr);
    }
    session_tip(settings.ui.perf_report ? "No fim da captura abre a analise" : "Desligado");
    ImGui::PopTextWrapPos();
    ImGui::EndChild();

    // Footer sits directly under body; natural ItemSpacing.y is the only gap (already reserved).
    ImGui::BeginChild("footer", ImVec2(0, footer_h), false, ImGuiWindowFlags_NoScrollbar);
    {
      const float row_h = 48.f;
      // Center the button row inside footer_h — slight air above without growing the layout.
      if (!motor_warn) {
        const float pad_y = (std::max)(0.f, (footer_h - row_h) * 0.5f - 2.f);
        if (pad_y > 0.5f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + pad_y);
      }

      const float footer_y0 = ImGui::GetCursorPosY();
      if (preview.live && preview.frames_seen > 0) {
        ImGui::AlignTextToFramePadding();
        if (capture_source_is_window(settings.capture_source)) {
          ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.f), "* Preview da janela — audio no app de origem");
        } else {
          ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.f), "* Preview ao vivo — Iniciar libera a placa");
        }
      } else if (!preview.err.empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(1.f, 0.5f, 0.35f, 1.f), "! %s", hdmi_open_error_user(preview.err).c_str());
      } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.f), "* Pronto — Esc na captura volta ao menu");
      }
      if (motor_warn) {
        ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 430.f);
        ImGui::TextColored(nexuscap_warn_color(), "%s", lobby_motor_override_warn());
        ImGui::TextColored(nexuscap_warn_color(), "  -> %s",
                           lobby_motor_override_which(motor_probe.stable_dirty, motor_probe.multipass_dirty));
        ImGui::PopTextWrapPos();
      }
      ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 420.f, footer_y0));
      if (ImGui::Button("Sair", ImVec2(100, row_h))) {
        g_start_requested = false;
        g_lobby_running = false;
      }
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.95f, 1.f));
      if (ImGui::Button("Iniciar processamento", ImVec2(280, row_h))) {
        if (lobby_iniciar_blocked_by_hdmi_busy_sim(cfg.simulate_hdmi_busy,
                                                   capture_source_is_window(settings.capture_source))) {
          hdmi_iniciar_fail_open = true;
          push_lobby_toast(session_capture_fail_notice(hdmi_open_busy_hint()));
        } else if (capture_source_is_window(settings.capture_source)) {
          if (settings.window_title.empty()) {
            push_lobby_toast(window_capture_pick_hint());
          } else {
            settings.scale_percent = clamp_scale_percent(static_cast<int>(std::lround(scale_f)));
            settings.fg_adaptive = fg_adp_committed;
            settings.fg_target_fps = fg_tgt_committed;
            if (!settings.enhance_renodx) settings.neural_rendering = false;
            {
              std::string serr;
              save_settings(settings, settings_default_path(), serr);
            }
            motor_swap_title = lobby_iniciar_hold_title();
            motor_swap_line.clear();
            motor_swap_pending = true;
            motor_swap_silent = !lobby_iniciar_shows_pipeline_card(true);
            motor_swap_sync_session_files = true;
            motor_swap_until = ImGui::GetTime();
            if (lobby_iniciar_needs_session_relaunch(settings.ui.preview_enabled, settings.ui.preview_cfg_live,
                                                     settings.enhance_renodx)) {
              motor_swap_args = "--autostart";
              motor_swap_enter_session = false;
            } else {
              motor_swap_args.clear();
              motor_swap_enter_session = true;
            }
          }
        } else {
        if (!devices.empty() && device_idx >= 0 && device_idx < static_cast<int>(devices.size())) {
          settings.device_name = devices[static_cast<size_t>(device_idx)];
        }
        if (!modes.empty() && mode_idx >= 0 && mode_idx < static_cast<int>(modes.size())) {
          settings.width = modes[static_cast<size_t>(mode_idx)].width;
          settings.height = modes[static_cast<size_t>(mode_idx)].height;
          settings.fps = modes[static_cast<size_t>(mode_idx)].fps;
        }
        settings.scale_percent = clamp_scale_percent(static_cast<int>(std::lround(scale_f)));
        settings.fg_adaptive = fg_adp_committed;
        settings.fg_target_fps = fg_tgt_committed;
        if (!settings.enhance_renodx) settings.neural_rendering = false;
        {
          std::string serr;
          save_settings(settings, settings_default_path(), serr);
        }
        motor_swap_title = lobby_iniciar_hold_title();
        motor_swap_line.clear();
        motor_swap_pending = true;
        motor_swap_silent = !lobby_iniciar_shows_pipeline_card(false);
        motor_swap_sync_session_files = true;
        motor_swap_until = ImGui::GetTime();
        if (lobby_iniciar_needs_session_relaunch(settings.ui.preview_enabled, settings.ui.preview_cfg_live,
                                                 settings.enhance_renodx)) {
          motor_swap_args = "--autostart";
          motor_swap_enter_session = false;
        } else {
          motor_swap_args.clear();
          motor_swap_enter_session = true;
        }
        }
      }
      ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    ImGui::End();

    if (g_open_gear && !gear_was_open) {
      scale_f = static_cast<float>(clamp_scale_percent(settings.scale_percent));
      reload_lobby_presets();
      gear_snapshot = settings;
      cfg_unsaved_ask = false;
      adv_draft = settings.nr_advanced[clamp_nr_pass_count(settings.nr_pass_count)];
      adv_draft_pass = clamp_nr_pass_count(settings.nr_pass_count);
    }
    gear_was_open = g_open_gear;

    auto persist_gear = [&]() {
      settings.scale_percent = clamp_scale_percent(static_cast<int>(std::lround(scale_f)));
      settings.renodx_preset = clamp_renodx_visual_preset(settings.renodx_preset);
      settings.nr_pass_count = clamp_nr_pass_count(settings.nr_pass_count);
      settings.capture_volume = clamp_capture_volume_percent(settings.capture_volume);
      if (!settings.enhance_renodx) settings.neural_rendering = false;
      std::string serr;
      save_settings(settings, settings_default_path(), serr);
      wchar_t exe_path[MAX_PATH]{};
      GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
      const auto exe_dir = std::filesystem::path(exe_path).parent_path().string();
      sync_nr_pass_count_files(exe_dir, settings.nr_pass_count, settings.renodx_preset, adv_now());
      sync_renodx_visual_preset_files(exe_dir, settings.renodx_preset, settings.nr_pass_count, adv_now());
      const bool want = want_neural_rendering(settings.enhance_renodx, settings.neural_rendering);
      if (lobby_preview_cfg_wanted(settings.ui.preview_enabled, settings.ui.preview_cfg_live)) {
        sync_neural_rendering_enabled_files(exe_dir, want, feed_pipeline_on_for_renodx(settings.enhance_renodx));
        apply_neural_rendering_state(exe_dir, want, &nr_runtime_on,
                                     feed_pipeline_on_for_renodx(settings.enhance_renodx));
      } else {
        sync_lobby_preview_pipeline_files(exe_dir, settings);
      }
      gear_snapshot = settings;
    };
    auto revert_gear = [&]() {
      settings = gear_snapshot;
      scale_f = static_cast<float>(clamp_scale_percent(settings.scale_percent));
      fg_adp_committed = settings.fg_adaptive;
      fg_tgt_committed = settings.fg_target_fps;
      fg_adp_draft = fg_adp_committed;
      fg_tgt_draft = fg_tgt_committed;
      persist_gear();
    };
    auto adv_slot = [&]() {
      const int slot = adv_draft_pass >= 1 ? adv_draft_pass : clamp_nr_pass_count(settings.nr_pass_count);
      return clamp_nr_pass_count(slot);
    };
    auto adv_unapplied = [&]() {
      return nr_advanced_draft_unapplied(adv_draft, settings.nr_advanced[adv_slot()]);
    };
    auto commit_advanced = [&](bool as_reset) -> bool {
      const int shown = adv_slot();
      settings.nr_advanced[shown] = clamp_nr_advanced(adv_draft);
      adv_draft = settings.nr_advanced[shown];
      adv_draft_pass = shown;
      wchar_t exe_path[MAX_PATH]{};
      GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
      const auto exe_dir = std::filesystem::path(exe_path).parent_path().string();
      const bool live =
          lobby_preview_cfg_wanted(settings.ui.preview_enabled, settings.ui.preview_cfg_live);
      if (!prepare_renodx_runtime_dirs(exe_dir)) {
        push_lobby_toast(renodx_write_denied_notice());
        return false;
      }
      std::string serr;
      save_settings(settings, settings_default_path(), serr);
      gear_snapshot = settings;
      (void)sync_nr_pass_count_files(exe_dir, settings.nr_pass_count, settings.renodx_preset, adv_now());
      (void)sync_renodx_visual_preset_files(exe_dir, settings.renodx_preset, settings.nr_pass_count, adv_now());
      if (live) {
        motor_swap_title = lobby_pipeline_hold_title();
        motor_swap_line = std::string(nr_pass_count_label(shown)) +
                          (as_reset ? "  default" : "  configs avancadas");
        motor_swap_args = lobby_cfg_relaunch_args_with_advanced();
        motor_swap_refresh_lobby = true;
        motor_swap_pending = true;
        motor_swap_silent = false;
        motor_swap_enter_session = false;
        motor_swap_until = ImGui::GetTime() + kDlss5MotorSwapHoldSec;
        write_restart_toast(std::string(as_reset ? "Default do " : "Configs avancadas: ") +
                            nr_pass_count_label(shown));
      } else {
        push_lobby_toast(std::string(as_reset ? "Default do " : "Configs avancadas: ") +
                         nr_pass_count_label(shown) + " — no Iniciar");
      }
      return true;
    };
    auto cfg_dirty = [&]() {
      return lobby_cfg_has_unsaved(settings, gear_snapshot, fg_adp_draft, fg_adp_committed, fg_tgt_draft,
                                   fg_tgt_committed) ||
             adv_unapplied();
    };
    auto request_close_gear = [&]() {
      if (cfg_unsaved_ask) return;
      if (lobby_cfg_close_needs_prompt(cfg_dirty())) cfg_unsaved_ask = true;
      else g_open_gear = false;
    };
    if (g_gear_close_request) {
      g_gear_close_request = false;
      if (g_open_gear) request_close_gear();
    }

    // Non-modal side panel — preview stays visible so you see adjustments live.
    if (g_open_gear) {
      const ImGuiViewport* mvp = ImGui::GetMainViewport();
      // Largura minima cabe label + tecla + checkbox Ctrl.
      const float gear_w = (std::max)(hotkey_table_min_width(), (std::min)(mvp->WorkSize.x * 0.40f, 980.f));
      const float gear_h = (std::max)(240.f, mvp->WorkSize.y - 120.f);
      ImGui::SetNextWindowSizeConstraints(ImVec2(hotkey_table_min_width(), 240.f), ImVec2(FLT_MAX, FLT_MAX));
      ImGui::SetNextWindowPos(ImVec2(mvp->WorkPos.x + mvp->WorkSize.x - gear_w - 40.f, mvp->WorkPos.y + 80.f),
                              ImGuiCond_Appearing);
      ImGui::SetNextWindowSize(ImVec2(gear_w, gear_h), ImGuiCond_Always);
      bool gear_p_open = true;
      if (ImGui::Begin("Configuracoes (CFG)", &gear_p_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(nexuscap_accent_color(),
                           lobby_preview_cfg_wanted(settings.ui.preview_enabled, settings.ui.preview_cfg_live)
                               ? "Ajuste e veja no preview ao vivo"
                               : "Ajuste grava para o Iniciar (preview sem NR)");
        ImGui::PushTextWrapPos(0.f);  // notas longas quebram linha em vez de sumir na borda
        ImGui::Separator();
        auto lobby_cfg_volume = [&]() {
        ImGui::TextUnformatted("Volume da captura");
        if (ImGui::SliderInt("##capture_vol", &settings.capture_volume, kMinCaptureVolumePercent,
                             kMaxCaptureVolumePercent, "%d%%")) {
          settings.capture_volume = clamp_capture_volume_percent(settings.capture_volume);
          if (lobby_audio.running()) lobby_audio.set_volume(settings.capture_volume);
          std::string serr;
          save_settings(settings, settings_default_path(), serr);
          gear_snapshot = settings;
        }
        ImGui::TextDisabled("HDMI no fone / caixa  |  0-100  |  nao vai para o preset");
        };
        auto lobby_cfg_fg = [&]() {
        ImGui::TextUnformatted("Frame Gen (Lossless Scaling)");
        const bool fg_was = settings.fg_enabled;
        const int fg_mult_was = settings.fg_multiplier;
        const int fg_queue_was = settings.fg_queue;
        const int fg_flow_was = settings.fg_flow_scale;
        const bool fg_adp_was = settings.fg_adaptive;
        const int fg_tgt_was = settings.fg_target_fps;
        const std::string fg_path_was = settings.fg_path;
        ImGui::Checkbox("Habilitar Frame Gen", &settings.fg_enabled);
        ImGui::BeginDisabled(!settings.fg_enabled || fg_adp_draft);
        ImGui::RadioButton("x2", &settings.fg_multiplier, 2);
        ImGui::SameLine();
        ImGui::RadioButton("x3", &settings.fg_multiplier, 3);
        ImGui::SameLine();
        ImGui::RadioButton("x4", &settings.fg_multiplier, 4);
        ImGui::EndDisabled();
        const int lobby_hz = display_refresh_hz(g_hwnd);
        const double lobby_cap = preview.fps_ema > 1.0 ? preview.fps_ema : settings.fps;
        auto commit_lobby_fg_mode = [&](bool adaptive, int target) {
          settings.fg_adaptive = adaptive;
          settings.fg_target_fps = target;
          fg_adp_committed = adaptive;
          fg_tgt_committed = target;
          fg_adp_draft = adaptive;
          fg_tgt_draft = target;
          std::string serr;
          save_settings(settings, settings_default_path(), serr);
          gear_snapshot = settings;
          push_lobby_toast(session_notice_fg_adaptive_apply(settings.fg_adaptive, settings.fg_target_fps, lobby_hz,
                                                           false, lobby_cap));
        };
        ImGui::TextUnformatted("Modo FG");
        if (ImGui::BeginCombo("##fg_mode", fg_mode_label(fg_adp_draft))) {
          if (ImGui::Selectable(fg_mode_label(false), !fg_adp_draft)) {
            fg_adp_draft = false;
            if (fg_adp_committed) commit_lobby_fg_mode(false, fg_tgt_committed);
          }
          if (!fg_adp_draft) ImGui::SetItemDefaultFocus();
          if (ImGui::Selectable(fg_mode_label(true), fg_adp_draft)) fg_adp_draft = true;
          ImGui::EndCombo();
        }
        ImGui::BeginDisabled(!settings.fg_enabled || !fg_adp_draft);
        int lobby_tgt = fg_tgt_draft;
        if (ImGui::InputInt("Alvo FPS##fg_tgt", &lobby_tgt, 0, 0)) fg_tgt_draft = lobby_tgt;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          fg_tgt_draft = clamp_fg_adaptive_target(fg_tgt_draft, lobby_hz, lobby_cap);
        }
        if (ImGui::Button("Hz do monitor##fg_hz")) {
          fg_tgt_draft = clamp_fg_adaptive_target(lobby_hz, lobby_hz, lobby_cap);
        }
        ImGui::EndDisabled();
        {
          const int draft_clamped = clamp_fg_adaptive_target(fg_tgt_draft, lobby_hz, lobby_cap);
          const int lo = clamp_fg_adaptive_target(1, lobby_hz, lobby_cap);
          const bool apply_on =
              fg_adaptive_apply_enabled(fg_adp_draft, fg_adp_committed, fg_tgt_committed, draft_clamped);
          ImGui::BeginDisabled(!settings.fg_enabled || !apply_on);
          if (ImGui::Button("Aplicar Adaptive##fg_apply")) {
            fg_tgt_draft = draft_clamped;
            commit_lobby_fg_mode(true, draft_clamped);
          }
          ImGui::EndDisabled();
          char hz_hint[176]{};
          std::snprintf(hz_hint, sizeof(hz_hint),
                        "No Adaptive, escreve o alvo (%d a %d) e carrega Aplicar. Fixo vale na hora.", lo, lobby_hz);
          ImGui::TextDisabled("%s", hz_hint);
          ImGui::TextDisabled("%s", fg_adaptive_ghost_hint());
        }
        ImGui::TextUnformatted("Perfil FG");
        settings.fg_queue = clamp_fg_queue(settings.fg_queue);
        if (ImGui::BeginCombo("##fg_queue", fg_queue_label(settings.fg_queue))) {
          for (int q = 0; q < 3; ++q) {
            const bool selected = settings.fg_queue == q;
            if (ImGui::Selectable(fg_queue_label(q), selected)) settings.fg_queue = q;
            if (selected) ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }
        ImGui::TextDisabled("0 menor latencia (validado). Vale na proxima abertura do frame gen.");
        ImGui::TextUnformatted("Escala de fluxo");
        settings.fg_flow_scale = clamp_fg_flow_scale(settings.fg_flow_scale);
        if (ImGui::BeginCombo("##fg_flow", fg_flow_scale_label(settings.fg_flow_scale))) {
          for (int i = 0; i < kFgFlowScaleChoiceCount; ++i) {
            const int v = kFgFlowScaleChoices[i];
            const bool selected = settings.fg_flow_scale == v;
            if (ImGui::Selectable(fg_flow_scale_label(v), selected)) settings.fg_flow_scale = v;
            if (selected) ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }
        ImGui::TextDisabled("Padrao e o nosso valor. 70, 90 e 100 ficam a sua escolha.");
        if (settings.fg_enabled && !fg_was) {
          if (!lobby_fg_can_enable(settings.fg_path)) {
            settings.fg_enabled = false;
            push_lobby_toast(session_notice_fg_missing());
          } else {
            push_lobby_toast(session_notice_fg_on());
          }
        } else if (settings.fg_enabled && session_notice_for_fg_cycle(fg_mult_was, settings.fg_multiplier)) {
          push_lobby_toast(session_notice_fg_multiplier_up(settings.fg_multiplier));
        }
        if (settings.fg_flow_scale != fg_flow_was) {
          push_lobby_toast(std::string("Escala de fluxo ") + fg_flow_scale_label(settings.fg_flow_scale));
        }
        if (settings.fg_enabled != fg_was || settings.fg_multiplier != fg_mult_was ||
            settings.fg_queue != fg_queue_was || settings.fg_flow_scale != fg_flow_was ||
            settings.fg_adaptive != fg_adp_was ||
            settings.fg_target_fps != fg_tgt_was || settings.fg_path != fg_path_was) {
          lobby_park_lossless(settings, /*relaunch=*/true);
        }
        {
          const std::string resolved = resolve_lossless_exe(settings.fg_path);
          ImGui::TextUnformatted("Exe do Lossless Scaling");
          if (ImGui::Button("Pasta##fg_browse", ImVec2(72, 0))) {
            std::string picked;
            if (browse_lossless_exe_path(g_hwnd, picked)) {
              settings.fg_path = picked;
              if (lobby_fg_can_enable(settings.fg_path)) {
                push_lobby_toast("Lossless Scaling — caminho manual OK");
              } else {
                push_lobby_toast(session_notice_fg_missing());
              }
            }
          }
          ImGui::SameLine();
          if (ImGui::Button("Auto##fg_auto", ImVec2(56, 0))) settings.fg_path.clear();
          if (!settings.fg_path.empty()) {
            ImGui::TextWrapped("%s", fg_path_display_tail(settings.fg_path).c_str());
            ImGui::TextDisabled(resolved.empty() ? "caminho manual invalido" : "caminho manual OK");
          } else if (!resolved.empty()) {
            ImGui::TextDisabled("auto: %s", fg_path_display_tail(resolved).c_str());
          } else {
            ImGui::TextColored(ImVec4(1.f, 0.55f, 0.35f, 1.f),
                               "nao encontrado — clique Pasta e selecione LosslessScaling.exe");
          }
        }
        ImGui::TextDisabled("HUD do preview muda na hora.");
        };
        auto lobby_cfg_image = [&]() {
        ImGui::TextUnformatted("Escala de resolucao (trabalho GPU)");
        int scale_i = clamp_scale_percent(settings.scale_percent);
        // Writing the cfg here would not reach this process' Feed (locked at 100%) and would
        // leave a scaled cfg behind if the app is killed — Iniciar writes it before relaunch.
        ImGui::SliderInt("##scale", &scale_i, kMinScalePercent, kMaxScalePercent, "%d%%");
        settings.scale_percent = clamp_scale_percent(scale_i);
        scale_f = static_cast<float>(settings.scale_percent);
        const int eff_scale = effective_scale_percent(settings.enhance_renodx, settings.scale_percent,
                                                      settings.nr_pass_count, settings.width, settings.height);
        const int work_w = scaled_extent(settings.width, eff_scale);
        const int work_h = scaled_extent(settings.height, eff_scale);
        const int pct_pix = static_cast<int>(scale_pixel_ratio(eff_scale) * 100.0 + 0.5);
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 1.f, 1.f), "Trabalho Feed/NR: %dx%d (%d%%)", work_w, work_h,
                           eff_scale);
        if (settings.enhance_renodx) {
          ImGui::TextDisabled("Escala entra no DLSS 5 (Feed); captura e saida ficam nativas.");
          ImGui::TextDisabled("Menus ficam sempre em 100% — a escala vale na captura.");
          if (feed_scale_below_floor(true, settings.scale_percent)) {
            ImGui::TextColored(ImVec4(1.f, 0.82f, 0.35f, 1.f), "Feed nao aceita abaixo de %d%% — usando %d%%",
                               kMinFeedWorkResolution, eff_scale);
          }
          if (feed_scale_change_needs_relaunch(true, feed_loaded_scale, settings.scale_percent,
                                               settings.nr_pass_count, settings.width, settings.height)) {
            ImGui::TextColored(ImVec4(1.f, 0.82f, 0.35f, 1.f), "Feed ativo em %d%%: Iniciar aplica %d%%",
                               effective_scale_percent(true, feed_loaded_scale), eff_scale);
          }
        } else {
          ImGui::TextDisabled("Sinal %dx%d  ·  pixels RGB ~%d%%  ·  min %d%%", settings.width, settings.height,
                              pct_pix, kMinScalePercent);
        }
        if (app_is_portable()) ImGui::TextDisabled("%s", portable_data_dir_hint());
        if (ImGui::Button("Abrir logs")) {
          if (!open_app_logs_folder()) push_lobby_toast("Nao foi possivel abrir a pasta de logs.");
        }

        ImGui::Separator();
        if (ImGui::Checkbox("RenoDX / Neural Rendering (preview ao vivo)", &settings.enhance_renodx)) {
          // Stack on → arm NR for preview; stack off → clear NR.
          settings.neural_rendering = settings.enhance_renodx;
          wchar_t exe_path[MAX_PATH]{};
          GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
          const auto exe_dir = std::filesystem::path(exe_path).parent_path().string();
          const bool live =
              lobby_preview_cfg_wanted(settings.ui.preview_enabled, settings.ui.preview_cfg_live);
          const bool want = want_neural_rendering(settings.enhance_renodx, settings.neural_rendering);
          sync_lobby_preview_pipeline_files(exe_dir, settings);
          if (live) {
            if (!apply_neural_rendering_state(exe_dir, want, &nr_runtime_on,
                                              feed_pipeline_on_for_renodx(settings.enhance_renodx)) &&
                want && !dlss5_bridge_ready_for_nr()) {
              nr_boot_retries = 120;
            }
          }
        }
        ImGui::TextDisabled(settings.enhance_renodx
                                ? (settings.neural_rendering ? "  → NR ON (Delete liga/desliga na captura)"
                                                             : "  → RenoDX armado · NR off (Delete na captura)")
                                : "  → preview limpo — sem RenoDX (Delete nao funciona ate marcar)");
        if (settings.enhance_renodx) {
          ImGui::TextDisabled(nr_runtime_on ? "  runtime NR: Confirmado ON (filtros no preview)"
                                            : "  runtime NR: Aplicando / aguardando Bridge");
        }
        ImGui::BeginDisabled(!settings.enhance_renodx);
        ImGui::TextUnformatted("Preset visual (ao vivo no preview)");
        const int preset_before = clamp_renodx_visual_preset(settings.renodx_preset);
        int preset_i = preset_before;
        if (ImGui::RadioButton(renodx_visual_preset_name(0, settings.nr_pass_count), preset_i == 0))
          preset_i = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton(renodx_visual_preset_name(1, settings.nr_pass_count), preset_i == 1))
          preset_i = 1;
        ImGui::SameLine();
        if (ImGui::RadioButton(renodx_visual_preset_name(2, settings.nr_pass_count), preset_i == 2))
          preset_i = 2;
        settings.renodx_preset = clamp_renodx_visual_preset(preset_i);
        if (settings.renodx_preset != preset_before) {
          wchar_t exe_path[MAX_PATH]{};
          GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
          const auto exe_dir = std::filesystem::path(exe_path).parent_path().string();
          const bool want = want_neural_rendering(settings.enhance_renodx, settings.neural_rendering);
          const bool live =
              lobby_preview_cfg_wanted(settings.ui.preview_enabled, settings.ui.preview_cfg_live);
          preview.set_visual_preview(settings.renodx_preset, live && want);
          const bool writable = prepare_renodx_runtime_dirs(exe_dir);
          if (!writable) {
            push_lobby_toast(renodx_write_denied_notice());
          } else if (live) {
            apply_lobby_renodx_preset_live(exe_dir, settings.renodx_preset, want, &nr_runtime_on,
                                           settings.nr_pass_count, adv_now());
            cfg.nr_runtime_on = nr_runtime_on;
            push_lobby_toast(std::string("Preset: ") +
                             renodx_visual_preset_name(settings.renodx_preset, settings.nr_pass_count) +
                             " — olhe o preview");
            if (want && !nr_runtime_on && !dlss5_bridge_ready_for_nr()) {
              nr_boot_retries = 120;
            }
          } else {
            (void)sync_renodx_visual_preset_files(exe_dir, settings.renodx_preset, settings.nr_pass_count,
                                                  adv_now());
            push_lobby_toast(std::string("Preset: ") +
                             renodx_visual_preset_name(settings.renodx_preset, settings.nr_pass_count) +
                             " — no Iniciar");
          }
        }
        ImGui::TextDisabled("NR=%d  model=%d  ·  olhe a imagem (nao so o texto)",
                            renodx_nr_preset_index(settings.renodx_preset),
                            renodx_feed_model_preset(settings.renodx_preset, settings.nr_pass_count));
        ImGui::TextDisabled("  Default / Natural / Cinematic (o addon so le no boot)");
        ImGui::TextDisabled("Mesmo look no Iniciar — janela ou tela cheia.");

        ImGui::Spacing();
        {
          const int shown = clamp_nr_pass_count(settings.nr_pass_count);
          if (shown != adv_draft_pass) {
            adv_draft = settings.nr_advanced[shown];
            adv_draft_pass = shown;
          }
          bool adv_apply = false;
          bool adv_reset = false;
          const bool force_adv = force_nr_advanced;
          force_nr_advanced = false;
          draw_nr_advanced_panel(adv_draft, shown, adv_apply, adv_reset, force_adv);
          if (adv_apply || adv_reset) (void)commit_advanced(adv_reset);
        }
        ImGui::TextUnformatted("Pass Count (NR multipass)");
        const int pass_before = clamp_nr_pass_count(settings.nr_pass_count);
        int pass_i = pass_before;
        if (ImGui::RadioButton("1x##nrpass", pass_i == 1)) pass_i = 1;
        ImGui::SameLine();
        if (ImGui::RadioButton("1,5x##nrpass", pass_i == 4)) pass_i = 4;
        if (ImGui::RadioButton("2x otimizado##nrpass", pass_i == 5)) pass_i = 5;
        ImGui::SameLine();
        if (ImGui::RadioButton("3x otimizado##nrpass", pass_i == 6)) pass_i = 6;
        settings.nr_pass_count = clamp_nr_pass_count(pass_i);
        if (settings.nr_pass_count != pass_before) {
          settings.scale_percent =
              nr_pass_count_on_select_scale(pass_before, settings.nr_pass_count, settings.scale_percent);
          scale_f = static_cast<float>(settings.scale_percent);
          wchar_t exe_path[MAX_PATH]{};
          GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
          const auto exe_dir = std::filesystem::path(exe_path).parent_path().string();
          const bool want = want_neural_rendering(settings.enhance_renodx, settings.neural_rendering);
          const bool writable = prepare_renodx_runtime_dirs(exe_dir);
          // Multipass não relê o PassCount no processo vivo, então 1,5x↔2x↔3x também reinicia.
          const bool need_relaunch = !nr_pass_count_change_applies_live(pass_before, settings.nr_pass_count);
          if (!writable) {
            push_lobby_toast(renodx_write_denied_notice());
          } else if (need_relaunch &&
                     lobby_preview_cfg_wanted(settings.ui.preview_enabled, settings.ui.preview_cfg_live)) {
            // Show hold notice, then relaunch (avoids "bugou" flash).
            std::string serr;
            save_settings(settings, settings_default_path(), serr);
            gear_snapshot = settings;
            (void)sync_nr_pass_count_files(exe_dir, settings.nr_pass_count, settings.renodx_preset, adv_now());
            (void)sync_feed_work_resolution_in_dir(
                exe_dir, feed_work_resolution_for_session(settings.scale_percent, settings.nr_pass_count,
                                                         settings.width, settings.height),
                settings.nr_pass_count);
            const bool motor_change =
                nr_pass_count_change_needs_relaunch(pass_before, settings.nr_pass_count);
            motor_swap_title = lobby_pipeline_hold_title();
            motor_swap_line = std::string(nr_pass_count_label(pass_before)) + "  \xE2\x86\x92  " +
                              nr_pass_count_label(settings.nr_pass_count);
            motor_swap_args = lobby_cfg_relaunch_args();
            motor_swap_pending = true;
            motor_swap_silent = false;
            motor_swap_enter_session = false;
            motor_swap_until = ImGui::GetTime() + kDlss5MotorSwapHoldSec;
            write_restart_toast(motor_change
                                 ? session_notice_dlss5_motor_swap(pass_before, settings.nr_pass_count)
                                 : session_notice_nr_pass_count_apply(pass_before, settings.nr_pass_count));
          } else if (lobby_preview_cfg_wanted(settings.ui.preview_enabled, settings.ui.preview_cfg_live)) {
            // 1,5x ↔ 2x ↔ 3x: same multipass addon — INI + NR reload only.
            apply_lobby_nr_pass_count_live(exe_dir, settings.nr_pass_count, want, &nr_runtime_on,
                                           settings.renodx_preset, adv_now());
            cfg.nr_runtime_on = nr_runtime_on;
            push_lobby_toast(std::string("Pass Count: ") + nr_pass_count_label(settings.nr_pass_count));
            if (want && !nr_runtime_on && !dlss5_bridge_ready_for_nr()) {
              nr_boot_retries = 120;
            }
          } else {
            std::string serr;
            save_settings(settings, settings_default_path(), serr);
            gear_snapshot = settings;
            (void)sync_nr_pass_count_files(exe_dir, settings.nr_pass_count, settings.renodx_preset, adv_now());
            push_lobby_toast(std::string("Pass Count: ") + nr_pass_count_label(settings.nr_pass_count) +
                             " — no Iniciar");
          }
        }
        ImGui::TextDisabled("%s", nr_pass_count_selected_hint(settings.nr_pass_count));
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::TextUnformatted("Presets");
        if (ImGui::BeginChild("##cfg_preset_list", ImVec2(0.f, 168.f), true)) {
          if (lobby_preset_names.empty()) {
            ImGui::TextDisabled("Nenhum preset. Salve o look deste jogo.");
          }
          for (int i = 0; i < static_cast<int>(lobby_preset_names.size()); ++i) {
            if (draw_user_preset_list_row(i, lobby_preset_names[static_cast<size_t>(i)].c_str(),
                                          lobby_preset_summaries[static_cast<size_t>(i)].c_str(),
                                          lobby_preset_sel == i))
              lobby_preset_sel = i;
          }
        }
        ImGui::EndChild();
        const bool cfg_have_preset =
            lobby_preset_sel >= 0 && lobby_preset_sel < static_cast<int>(lobby_preset_names.size());
        if (ImGui::Button("Salvar preset")) {
          lobby_preset_rename_open = false;
          lobby_preset_save_open = true;
          lobby_preset_save_name[0] = 0;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!cfg_have_preset);
        if (ImGui::Button("Aplicar") && cfg_have_preset) {
          std::vector<UserPreset> have;
          list_user_presets_in(default_user_presets_path(), have);
          const int idx = find_user_preset_index(have, lobby_preset_names[static_cast<size_t>(lobby_preset_sel)]);
          if (idx >= 0) apply_lobby_named_preset(have[static_cast<size_t>(idx)]);
        }
        ImGui::SameLine();
        if (ImGui::Button("Apagar")) lobby_preset_del = true;
        ImGui::SameLine();
        if (ImGui::Button("Renomear") && cfg_have_preset) {
          lobby_preset_save_open = false;
          lobby_preset_rename_open = true;
          fill_user_preset_name_buf(lobby_preset_save_name, sizeof(lobby_preset_save_name),
                                    lobby_preset_names[static_cast<size_t>(lobby_preset_sel)]);
        }
        ImGui::SameLine();
        if (ImGui::Button("Compartilhar") && cfg_have_preset) {
          std::vector<UserPreset> have;
          list_user_presets_in(default_user_presets_path(), have);
          const int idx = find_user_preset_index(have, lobby_preset_names[static_cast<size_t>(lobby_preset_sel)]);
          if (idx >= 0) {
            std::string path;
            if (browse_share_user_preset(g_hwnd, have[static_cast<size_t>(idx)].name,
                                         serialize_user_preset(have[static_cast<size_t>(idx)]), path)) {
              push_lobby_toast("Arquivo pronto: " + std::filesystem::u8path(path).filename().string());
            }
          }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Importar")) {
          std::string text;
          if (browse_import_user_preset(g_hwnd, text)) {
            UserPreset p;
            if (!parse_user_preset(text, p)) {
              push_lobby_toast("Arquivo invalido");
            } else {
              std::vector<UserPreset> have;
              list_user_presets_in(default_user_presets_path(), have);
              if (find_user_preset_index(have, p.name) >= 0) {
                lobby_preset_imp_text = text;
                lobby_preset_imp_ow = true;
              } else {
                bool replaced = false;
                std::string err;
                if (import_user_preset_in(default_user_presets_path(), text, p, replaced, err)) {
                  reload_lobby_presets();
                  std::vector<UserPreset> now;
                  list_user_presets_in(default_user_presets_path(), now);
                  lobby_preset_sel = find_user_preset_index(now, p.name);
                  push_lobby_toast("Preset importado: " + p.name);
                } else {
                  push_lobby_toast(err.empty() ? "Import falhou" : err);
                }
              }
            }
          }
        }
        ImGui::TextDisabled("Renomear troca so o nome. Mesma lista do Ctrl+M. Aplicar grava para o Iniciar.");

        if (lobby_preset_save_open) {
          ImGui::SetNextItemWidth(-1.f);
          ImGui::InputText("##lobby_preset_name", lobby_preset_save_name, sizeof(lobby_preset_save_name),
                           ImGuiInputTextFlags_CallbackCharFilter, [](ImGuiInputTextCallbackData* data) -> int {
                             return user_preset_name_filter_event(data->EventChar, data->BufTextLen);
                           });
          draw_user_preset_name_field_hint(lobby_preset_save_name);
          if (ImGui::Button("Salvar##lobbygo")) {
            std::string name;
            const UserPresetNameStatus st = normalize_user_preset_name(lobby_preset_save_name, name);
            if (st != UserPresetNameStatus::Ok) {
              push_lobby_toast(user_preset_name_status_toast(st));
            } else {
              std::vector<UserPreset> have;
              list_user_presets_in(default_user_presets_path(), have);
              if (find_user_preset_index(have, name) >= 0) {
                lobby_preset_ow = true;
              } else {
                settings.scale_percent = clamp_scale_percent(static_cast<int>(std::lround(scale_f)));
                UserPreset snap = user_preset_from_persisted(name, settings);
                bool replaced = false;
                std::string err;
                if (upsert_user_preset_in(default_user_presets_path(), snap, replaced, err)) {
                  lobby_preset_save_open = false;
                  reload_lobby_presets();
                  std::vector<UserPreset> now;
                  list_user_presets_in(default_user_presets_path(), now);
                  lobby_preset_sel = find_user_preset_index(now, name);
                  push_lobby_toast(replaced ? ("Preset substituido: " + name) : ("Preset salvo: " + name));
                } else {
                  push_lobby_toast(err.empty() ? "Nao deu para salvar o preset" : err);
                }
              }
            }
          }
          ImGui::SameLine();
          if (ImGui::Button("Cancelar##lobbysave")) lobby_preset_save_open = false;
        }
        if (lobby_preset_rename_open) {
          ImGui::SetNextItemWidth(-1.f);
          ImGui::InputText("##lobby_preset_rename", lobby_preset_save_name, sizeof(lobby_preset_save_name),
                           ImGuiInputTextFlags_CallbackCharFilter, [](ImGuiInputTextCallbackData* data) -> int {
                             return user_preset_name_filter_event(data->EventChar, data->BufTextLen);
                           });
          draw_user_preset_name_field_hint(lobby_preset_save_name);
          if (ImGui::Button("Renomear##lobbygo")) {
            std::string name;
            const UserPresetNameStatus st = normalize_user_preset_name(lobby_preset_save_name, name);
            if (st != UserPresetNameStatus::Ok) {
              push_lobby_toast(user_preset_name_status_toast(st));
            } else if (cfg_have_preset) {
              bool replaced = false;
              std::string err;
              const std::string old = lobby_preset_names[static_cast<size_t>(lobby_preset_sel)];
              if (!rename_user_preset_in(default_user_presets_path(), old, name, false, replaced, err)) {
                if (err == "existe") {
                  lobby_preset_rename_ow = true;
                } else {
                  push_lobby_toast(err.empty() ? "Nao deu para renomear" : err);
                }
              } else {
                lobby_preset_rename_open = false;
                reload_lobby_presets();
                std::vector<UserPreset> now;
                list_user_presets_in(default_user_presets_path(), now);
                lobby_preset_sel = find_user_preset_index(now, name);
                push_lobby_toast(replaced ? ("Preset substituido: " + name) : ("Preset renomeado: " + name));
              }
            }
          }
          ImGui::SameLine();
          if (ImGui::Button("Cancelar##lobbyrename")) lobby_preset_rename_open = false;
        }
        if (lobby_preset_ow) {
          ImGui::OpenPopup("##lobby_ow_preset");
          lobby_preset_ow = false;
        }
        if (ImGui::BeginPopupModal("##lobby_ow_preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
          std::string name;
          normalize_user_preset_name(lobby_preset_save_name, name);
          ImGui::Text("Substituir %s?", name.empty() ? lobby_preset_save_name : name.c_str());
          if (ImGui::Button("Substituir")) {
            settings.scale_percent = clamp_scale_percent(static_cast<int>(std::lround(scale_f)));
            UserPreset snap = user_preset_from_persisted(name.empty() ? lobby_preset_save_name : name, settings);
            bool replaced = false;
            std::string err;
            if (upsert_user_preset_in(default_user_presets_path(), snap, replaced, err)) {
              lobby_preset_save_open = false;
              reload_lobby_presets();
              std::vector<UserPreset> now;
              list_user_presets_in(default_user_presets_path(), now);
              lobby_preset_sel = find_user_preset_index(now, snap.name);
              push_lobby_toast("Preset substituido: " + snap.name);
            } else {
              push_lobby_toast(err.empty() ? "Nao deu para salvar o preset" : err);
            }
            ImGui::CloseCurrentPopup();
          }
          ImGui::SameLine();
          if (ImGui::Button("Cancelar##lobbyow")) ImGui::CloseCurrentPopup();
          ImGui::EndPopup();
        }
        if (lobby_preset_rename_ow) {
          ImGui::OpenPopup("##lobby_rn_ow_preset");
          lobby_preset_rename_ow = false;
        }
        if (ImGui::BeginPopupModal("##lobby_rn_ow_preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
          std::string name;
          normalize_user_preset_name(lobby_preset_save_name, name);
          ImGui::Text("Substituir %s?", name.empty() ? lobby_preset_save_name : name.c_str());
          if (ImGui::Button("Substituir##lobbyrn")) {
            if (cfg_have_preset) {
              bool replaced = false;
              std::string err;
              const std::string old = lobby_preset_names[static_cast<size_t>(lobby_preset_sel)];
              const std::string dest = name.empty() ? lobby_preset_save_name : name;
              if (rename_user_preset_in(default_user_presets_path(), old, dest, true, replaced, err)) {
                lobby_preset_rename_open = false;
                reload_lobby_presets();
                std::vector<UserPreset> now;
                list_user_presets_in(default_user_presets_path(), now);
                lobby_preset_sel = find_user_preset_index(now, dest);
                push_lobby_toast("Preset substituido: " + dest);
              } else {
                push_lobby_toast(err.empty() ? "Nao deu para renomear" : err);
              }
            }
            ImGui::CloseCurrentPopup();
          }
          ImGui::SameLine();
          if (ImGui::Button("Cancelar##lobbyrnow")) ImGui::CloseCurrentPopup();
          ImGui::EndPopup();
        }
        if (lobby_preset_del) {
          ImGui::OpenPopup("##lobby_del_preset");
          lobby_preset_del = false;
        }
        if (ImGui::BeginPopupModal("##lobby_del_preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
          const char* n = cfg_have_preset ? lobby_preset_names[static_cast<size_t>(lobby_preset_sel)].c_str() : "";
          ImGui::Text("Apagar %s?", n);
          if (ImGui::Button("Apagar##lobbygo")) {
            std::string err;
            if (cfg_have_preset &&
                delete_user_preset_in(default_user_presets_path(),
                                      lobby_preset_names[static_cast<size_t>(lobby_preset_sel)], err)) {
              push_lobby_toast(std::string("Preset apagado: ") + n);
              lobby_preset_sel = -1;
              reload_lobby_presets();
            } else {
              push_lobby_toast(err.empty() ? "Nao deu para apagar" : err);
            }
            ImGui::CloseCurrentPopup();
          }
          ImGui::SameLine();
          if (ImGui::Button("Cancelar##lobbydel")) ImGui::CloseCurrentPopup();
          ImGui::EndPopup();
        }
        if (lobby_preset_imp_ow) {
          ImGui::OpenPopup("##lobby_imp_ow");
          lobby_preset_imp_ow = false;
        }
        if (ImGui::BeginPopupModal("##lobby_imp_ow", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
          UserPreset p;
          parse_user_preset(lobby_preset_imp_text, p);
          ImGui::Text("Substituir %s?", p.name.c_str());
          if (ImGui::Button("Substituir##lobbyimp")) {
            bool replaced = false;
            std::string err;
            if (import_user_preset_in(default_user_presets_path(), lobby_preset_imp_text, p, replaced, err)) {
              reload_lobby_presets();
              std::vector<UserPreset> now;
              list_user_presets_in(default_user_presets_path(), now);
              lobby_preset_sel = find_user_preset_index(now, p.name);
              push_lobby_toast("Preset importado: " + p.name);
            } else {
              push_lobby_toast(err.empty() ? "Import falhou" : err);
            }
            lobby_preset_imp_text.clear();
            ImGui::CloseCurrentPopup();
          }
          ImGui::SameLine();
          if (ImGui::Button("Cancelar##lobbyimp")) {
            lobby_preset_imp_text.clear();
            ImGui::CloseCurrentPopup();
          }
          ImGui::EndPopup();
        }

        };
        auto lobby_cfg_keys = [&]() { draw_hotkey_config_table(settings.hotkeys, false, g_capture_vk_target, nullptr); };

        if (ImGui::BeginTabBar("##lobby_cfg_tabs")) {
          if (ImGui::BeginTabItem(cfg_tab_image())) {
            lobby_cfg_image();
            ImGui::EndTabItem();
          }
          if (ImGui::BeginTabItem(cfg_tab_sound())) {
            lobby_cfg_volume();
            ImGui::EndTabItem();
          }
          if (ImGui::BeginTabItem(cfg_tab_fg())) {
            lobby_cfg_fg();
            ImGui::EndTabItem();
          }
          if (ImGui::BeginTabItem(cfg_tab_keys())) {
            lobby_cfg_keys();
            ImGui::EndTabItem();
          }
          ImGui::EndTabBar();
        }

        ImGui::Spacing();
        if (ImGui::Button("Salvar", ImVec2(120, 0))) {
          persist_gear();
          cfg_unsaved_ask = false;
          g_open_gear = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Fechar", ImVec2(120, 0))) {
          request_close_gear();
        }
        ImGui::PopTextWrapPos();
      }
      ImGui::End();
      if (!gear_p_open) request_close_gear();
    }

    if (cfg_unsaved_ask) {
      ImGui::OpenPopup("##lobby_cfg_unsaved");
      const ImGuiViewport* uv = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(ImVec2(uv->WorkPos.x + uv->WorkSize.x * 0.5f, uv->WorkPos.y + uv->WorkSize.y * 0.5f),
                              ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
      ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.10f, 0.14f, 0.20f, 0.98f));
      ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.20f, 0.65f, 0.95f, 0.8f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.f, 16.f));
      if (ImGui::BeginPopupModal("##lobby_cfg_unsaved", nullptr,
                                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                     ImGuiWindowFlags_NoMove)) {
        ImGui::PushTextWrapPos(360.f);
        ImGui::TextColored(ImVec4(1.f, 0.82f, 0.45f, 1.f), "%s", lobby_cfg_unsaved_title());
        ImGui::TextUnformatted(lobby_cfg_unsaved_question());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.95f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.62f, 1.f, 1.f));
        if (ImGui::Button("Sim", ImVec2(120, 36))) {
          bool ok = true;
          if (adv_unapplied()) ok = commit_advanced(false);
          if (ok) {
            persist_gear();
            cfg_unsaved_ask = false;
            g_open_gear = false;
          } else {
            cfg_unsaved_ask = false;
          }
          ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (ImGui::Button("Nao", ImVec2(120, 36))) {
          revert_gear();
          adv_draft = settings.nr_advanced[clamp_nr_pass_count(settings.nr_pass_count)];
          adv_draft_pass = clamp_nr_pass_count(settings.nr_pass_count);
          cfg_unsaved_ask = false;
          g_open_gear = false;
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
      }
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor(2);
    }

    if (g_open_perf) {
      const ImGuiViewport* mvp = ImGui::GetMainViewport();
      const float gear_w = (std::max)(420.f, (std::min)(mvp->WorkSize.x * 0.32f, 860.f));
      const float gear_h = (std::max)(240.f, mvp->WorkSize.y - 120.f);
      ImGui::SetNextWindowPos(ImVec2(mvp->WorkPos.x + mvp->WorkSize.x - gear_w - 40.f, mvp->WorkPos.y + 80.f),
                              ImGuiCond_Appearing);
      ImGui::SetNextWindowSize(ImVec2(gear_w, gear_h), ImGuiCond_Appearing);
      if (ImGui::Begin(perf_test_lobby_title(), &g_open_perf, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(nexuscap_accent_color(), "Simula a carga sem placa de captura");
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("%s", perf_test_lobby_blurb());
        ImGui::Separator();
        ImGui::TextUnformatted("Volume da captura");
        if (ImGui::SliderInt("##perf_capture_vol", &settings.capture_volume, kMinCaptureVolumePercent,
                             kMaxCaptureVolumePercent, "%d%%")) {
          settings.capture_volume = clamp_capture_volume_percent(settings.capture_volume);
          if (lobby_audio.running()) lobby_audio.set_volume(settings.capture_volume);
          std::string serr;
          save_settings(settings, settings_default_path(), serr);
        }
        ImGui::TextDisabled("abre com o volume do CFG  |  0-100  |  nao vai para o preset");
        ImGui::Separator();

        ImGui::TextUnformatted("Pass Count");
        int pass_i = clamp_nr_pass_count(g_perf_plan.nr_pass_count);
        if (ImGui::RadioButton("1x##perfpass", pass_i == 1)) pass_i = 1;
        ImGui::SameLine();
        if (ImGui::RadioButton("1,5x##perfpass", pass_i == 4)) pass_i = 4;
        if (ImGui::RadioButton("2x otimizado##perfpass", pass_i == 5)) pass_i = 5;
        ImGui::SameLine();
        if (ImGui::RadioButton("3x otimizado##perfpass", pass_i == 6)) pass_i = 6;
        g_perf_plan.nr_pass_count = clamp_nr_pass_count(pass_i);
        ImGui::TextDisabled("%s", nr_pass_count_2x_opt_hint());

        ImGui::Separator();
        ImGui::TextDisabled("RESOLUCAO");
        ImGui::SetNextItemWidth(-1);
        {
          const char* res_labels[] = {"1920x1080", "2560x1440", "3840x2160"};
          int sig = perf_test_resolution_index(g_perf_plan.width, g_perf_plan.height);
          if (ImGui::Combo("##perfsig", &sig, res_labels, 3)) {
            perf_test_resolution_at(sig, g_perf_plan.width, g_perf_plan.height);
          }
        }

        ImGui::TextDisabled("HZ");
        ImGui::SetNextItemWidth(-1);
        {
          const char* hz_labels[] = {"30 Hz", "60 Hz"};
          int hz_i = perf_test_hz_index(g_perf_plan.fps);
          if (ImGui::Combo("##perffps", &hz_i, hz_labels, 2)) g_perf_plan.fps = perf_test_hz_value(hz_i);
        }
        ImGui::TextDisabled("%s", perf_test_hz_blurb());

        ImGui::Separator();
        bool audio_on = g_perf_plan.audio;
        if (ImGui::Checkbox(perf_test_audio_toggle(), &audio_on)) g_perf_plan.audio = audio_on;
        ImGui::TextDisabled("%s", perf_test_audio_hint());

        ImGui::Separator();
        ImGui::TextUnformatted("Escala do Feed");
        int scale_i = clamp_scale_percent(g_perf_plan.scale_percent);
        if (scale_i < kMinFeedWorkResolution) scale_i = kMinFeedWorkResolution;
        ImGui::SliderInt("##perfscale", &scale_i, kMinFeedWorkResolution, kMaxScalePercent, "%d%%");
        g_perf_plan.scale_percent = clamp_scale_percent(scale_i);

        ImGui::Separator();
        ImGui::TextUnformatted("Tom da cena");
        int preset_i = clamp_renodx_visual_preset(g_perf_plan.renodx_preset);
        char p0[64], p1[64], p2[64];
        std::snprintf(p0, sizeof(p0), "%s##perfpreset0", renodx_visual_preset_name(0, g_perf_plan.nr_pass_count));
        std::snprintf(p1, sizeof(p1), "%s##perfpreset1", renodx_visual_preset_name(1, g_perf_plan.nr_pass_count));
        std::snprintf(p2, sizeof(p2), "%s##perfpreset2", renodx_visual_preset_name(2, g_perf_plan.nr_pass_count));
        if (ImGui::RadioButton(p0, preset_i == 0)) preset_i = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton(p1, preset_i == 1)) preset_i = 1;
        ImGui::SameLine();
        if (ImGui::RadioButton(p2, preset_i == 2)) preset_i = 2;
        g_perf_plan.renodx_preset = clamp_renodx_visual_preset(preset_i);

        ImGui::Separator();
        ImGui::TextUnformatted("Frame Gen");
        int fg_i = clamp_fg_multiplier(g_perf_plan.fg_multiplier);
        if (ImGui::RadioButton("off##perffg", fg_i < 2)) fg_i = 1;
        ImGui::BeginDisabled(g_perf_plan.fg_adaptive);
        ImGui::SameLine();
        if (ImGui::RadioButton("x2##perffg", fg_i == 2)) fg_i = 2;
        ImGui::SameLine();
        if (ImGui::RadioButton("x3##perffg", fg_i == 3)) fg_i = 3;
        ImGui::SameLine();
        if (ImGui::RadioButton("x4##perffg", fg_i == 4)) fg_i = 4;
        ImGui::EndDisabled();
        g_perf_plan.fg_multiplier = clamp_fg_multiplier(fg_i);
        g_perf_plan.fg_queue = clamp_fg_queue(g_perf_plan.fg_queue);
        const int perf_hz = display_refresh_hz(g_hwnd);
        ImGui::TextUnformatted("Modo FG");
        if (ImGui::BeginCombo("##perf_fg_mode", fg_mode_label(g_perf_plan.fg_adaptive))) {
          if (ImGui::Selectable(fg_mode_label(false), !g_perf_plan.fg_adaptive)) g_perf_plan.fg_adaptive = false;
          if (!g_perf_plan.fg_adaptive) ImGui::SetItemDefaultFocus();
          if (ImGui::Selectable(fg_mode_label(true), g_perf_plan.fg_adaptive)) g_perf_plan.fg_adaptive = true;
          ImGui::EndCombo();
        }
        ImGui::BeginDisabled(g_perf_plan.fg_multiplier < 2 || !g_perf_plan.fg_adaptive);
        int perf_tgt = g_perf_plan.fg_target_fps;
        if (ImGui::InputInt("Alvo FPS##perf_fg_tgt", &perf_tgt, 0, 0)) g_perf_plan.fg_target_fps = perf_tgt;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
          g_perf_plan.fg_target_fps = clamp_fg_adaptive_target(g_perf_plan.fg_target_fps, perf_hz, g_perf_plan.fps);
        }
        if (ImGui::Button("Hz do monitor##perf_fg_hz")) {
          g_perf_plan.fg_target_fps = clamp_fg_adaptive_target(perf_hz, perf_hz, g_perf_plan.fps);
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("Iniciar ja carrega modo e alvo. Sem Aplicar. Padrao Fixo.");
        ImGui::TextDisabled("%s", fg_adaptive_ghost_hint());
        ImGui::TextUnformatted("Perfil FG");
        if (ImGui::BeginCombo("##perf_fg_queue", fg_queue_label(g_perf_plan.fg_queue))) {
          for (int q = 0; q < 3; ++q) {
            const bool selected = g_perf_plan.fg_queue == q;
            if (ImGui::Selectable(fg_queue_label(q), selected)) g_perf_plan.fg_queue = q;
            if (selected) ImGui::SetItemDefaultFocus();
          }
          ImGui::EndCombo();
        }
        ImGui::TextDisabled("Padrao 0. So neste plano; o CFG HDMI nao muda.");
        if (g_perf_plan.fg_multiplier >= 2 && !lobby_fg_can_enable(settings.fg_path)) {
          ImGui::TextColored(ImVec4(1.f, 0.55f, 0.35f, 1.f), "Lossless Scaling nao encontrado — FG fica off");
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Video do app");
        (void)resolve_perf_test_video(exe_dir_utf8(), g_perf_plan.width, g_perf_plan.height, g_perf_plan.video);
        if (g_perf_plan.video.empty()) {
          ImGui::TextColored(ImVec4(1.f, 0.55f, 0.35f, 1.f), "falta o clipe 1080/1440/4K em assets");
        } else {
          {
            std::string name = g_perf_plan.video;
            const auto slash = name.find_last_of("\\/");
            if (slash != std::string::npos) name = name.substr(slash + 1);
            ImGui::TextDisabled("O clipe segue a resolucao do plano: %s. O ficheiro e 60 fps; o pino simula 30 ou 60 Hz.",
                                name.c_str());
          }
        }
        ImGui::TextDisabled("Mesmo Present da sessao HDMI. O clipe 1440 e o da captura, nao um 22 Mbps mole.");
        ImGui::TextDisabled("Se so o Frame Gen chega a meta: Jogavel com FG (fps gerado, latencia maior).");
        ImGui::TextDisabled("Se o cap ja e a meta e o FG pesou: Apertado (desliga o FG e ficas nativo).");
        ImGui::TextDisabled("Se nao for Bom, a escala sai de (fps/meta)^(2/3). Se 50% ainda nao chega, Pass 1x.");
        ImGui::TextDisabled("Roda no Present+NR+FG em janela tela cheia.");
        ImGui::TextDisabled("So grava o plano do teste. Pass, tom e escala do CFG HDMI nao mudam.");

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.95f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.62f, 1.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.45f, 0.85f, 1.f));
        if (ImGui::Button(perf_test_lobby_start(), ImVec2(240, 44))) {
          g_perf_plan = clamp_perf_test_plan(g_perf_plan);
          (void)resolve_perf_test_video(exe_dir_utf8(), g_perf_plan.width, g_perf_plan.height, g_perf_plan.video);
          if (g_perf_plan.video.empty()) {
            push_lobby_toast("Falta o video do plano em assets (1080/1440/4K)");
          } else if (!write_perf_test_plan_file(default_perf_test_plan_path(app_data_dir()), g_perf_plan)) {
            push_lobby_toast("Nao gravou o plano do teste");
          } else {
            {
              std::string serr;
              settings.capture_volume = clamp_capture_volume_percent(settings.capture_volume);
              save_settings(settings, settings_default_path(), serr);
            }
            (void)sync_perf_test_pipeline_files(exe_dir_utf8(), g_perf_plan);
            motor_swap_title = perf_test_pipeline_notice_title();
            motor_swap_line = perf_test_pipeline_notice_detail();
            motor_swap_args = perf_test_cli_args(g_perf_plan);
            motor_swap_pending = true;
            motor_swap_silent = false;
            motor_swap_enter_session = false;
            motor_swap_until = ImGui::GetTime() + kDlss5MotorSwapHoldSec;
            g_open_perf = false;
          }
        }
        ImGui::PopStyleColor(3);
        ImGui::PopTextWrapPos();
      }
      ImGui::End();
    }

    if (g_show_perf_result && g_perf_result.valid) {
      const ImGuiViewport* tv = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(ImVec2(tv->WorkPos.x + tv->WorkSize.x * 0.5f, tv->WorkPos.y + tv->WorkSize.y * 0.42f),
                              ImGuiCond_Always, ImVec2(0.5f, 0.5f));
      ImGui::SetNextWindowBgAlpha(0.97f);
      ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.10f, 0.16f, 0.98f));
      const ImVec4 grade_col = g_perf_result.grade == PerfTestGrade::Good
                                   ? ImVec4(0.35f, 0.90f, 0.50f, 1.f)
                                   : (g_perf_result.grade == PerfTestGrade::Tight
                                          ? ImVec4(1.f, 0.82f, 0.35f, 1.f)
                                          : (g_perf_result.grade == PerfTestGrade::PlayableFg
                                                 ? ImVec4(0.95f, 0.78f, 0.35f, 1.f)
                                                 : ImVec4(1.f, 0.45f, 0.35f, 1.f)));
      ImGui::PushStyleColor(ImGuiCol_Border, grade_col);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.f, 20.f));
      if (ImGui::Begin("##perf_result", &g_show_perf_result,
                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextColored(grade_col, "%s",
                           perf_result_popup_title(g_perf_result.hdmi_session, g_perf_result.grade));
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.f);
        ImGui::TextUnformatted(g_perf_result.summary.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(120, 36))) g_show_perf_result = false;
      }
      ImGui::End();
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor(2);
    }

    if (hdmi_iniciar_fail_open) {
      const ImGuiViewport* tv = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(ImVec2(tv->WorkPos.x + tv->WorkSize.x * 0.5f, tv->WorkPos.y + tv->WorkSize.y * 0.45f),
                              ImGuiCond_Always, ImVec2(0.5f, 0.5f));
      ImGui::SetNextWindowBgAlpha(0.97f);
      ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.10f, 0.16f, 0.98f));
      ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.f, 0.55f, 0.25f, 0.95f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.f, 20.f));
      if (ImGui::Begin("##hdmi_iniciar_fail", nullptr,
                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextColored(ImVec4(1.f, 0.72f, 0.35f, 1.f), "%s", session_capture_fail_title());
        ImGui::Spacing();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.f);
        ImGui::TextUnformatted(session_capture_fail_detail(hdmi_open_busy_hint()).c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("OK", ImVec2(120, 36))) hdmi_iniciar_fail_open = false;
      }
      ImGui::End();
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor(2);
    }

    // Soft tip: FG on / FG up / NR on-off — mirrors session HUD notices.
    if (!lobby_toast.empty() && ImGui::GetTime() < lobby_toast_until) {
      const ImGuiViewport* tv = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(ImVec2(tv->WorkPos.x + tv->WorkSize.x * 0.5f, tv->WorkPos.y + tv->WorkSize.y - 56.f),
                              ImGuiCond_Always, ImVec2(0.5f, 1.f));
      ImGui::SetNextWindowBgAlpha(0.92f);
      ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.14f, 0.20f, 0.95f));
      ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.20f, 0.65f, 0.95f, 0.8f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.f, 10.f));
      if (ImGui::Begin("##lobby_toast", nullptr,
                       ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                           ImGuiWindowFlags_NoNav)) {
        std::string title, detail;
        session_notice_split(lobby_toast, title, detail);
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 42.f);
        ImGui::TextColored(ImVec4(1.f, 0.82f, 0.45f, 1.f), "%s", title.empty() ? lobby_toast.c_str() : title.c_str());
        if (!detail.empty()) {
          ImGui::TextColored(ImVec4(0.95f, 0.88f, 0.65f, 1.f), "%s", detail.c_str());
        }
        ImGui::PopTextWrapPos();
      }
      ImGui::End();
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor(2);
    } else if (!lobby_toast.empty() && ImGui::GetTime() >= lobby_toast_until) {
      lobby_toast.clear();
    }

    // Hold screen while the Pass Count relaunch runs so restart does not look like a crash.
    if (motor_swap_pending && !motor_swap_silent) {
      const ImGuiViewport* tv = ImGui::GetMainViewport();
      ImGui::SetNextWindowPos(ImVec2(tv->WorkPos.x + tv->WorkSize.x * 0.5f, tv->WorkPos.y + tv->WorkSize.y * 0.45f),
                              ImGuiCond_Always, ImVec2(0.5f, 0.5f));
      ImGui::SetNextWindowBgAlpha(0.96f);
      ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.10f, 0.16f, 0.98f));
      ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.95f, 0.70f, 0.25f, 0.95f));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.f, 20.f));
      if (ImGui::Begin("##motor_swap", nullptr,
                       ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav)) {
        ImGui::TextColored(ImVec4(1.f, 0.82f, 0.35f, 1.f), "%s", motor_swap_title.c_str());
        if (!motor_swap_line.empty()) {
          ImGui::Spacing();
          ImGui::TextUnformatted(motor_swap_line.c_str());
        }
      }
      ImGui::End();
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor(2);
    }

    ImGui::Render();
    const float clear[4] = {0.f, 0.f, 0.f, 1.f};
    g_dx.ctx->OMSetRenderTargets(1, &g_dx.rtv, nullptr);
    g_dx.ctx->ClearRenderTargetView(g_dx.rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    // Sem preview ao vivo o dxgi está parked: Present(0) disparava a GPU a 80% só no menu.
    const UINT lobby_sync =
        lobby_loads_reshade_dxgi(settings.ui.preview_enabled, settings.ui.preview_cfg_live,
                                 settings.enhance_renodx)
            ? 0u
            : 1u;
    silence_reshade_home_splash();
    g_dx.swap->Present(lobby_sync, 0);

    if (motor_swap_pending && ImGui::GetTime() >= motor_swap_until) {
      motor_swap_pending = false;
      if (motor_swap_sync_session_files) {
        wchar_t exe_path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        const auto exe_dir = std::filesystem::path(exe_path).parent_path().string();
        (void)sync_persisted_session_files(exe_dir, settings);
        motor_swap_sync_session_files = false;
      }
      if (motor_swap_enter_session) {
        motor_swap_enter_session = false;
        g_start_requested = true;
        g_lobby_running = false;
      } else if (lobby_hold_relaunch_starts_session(motor_swap_args.c_str())) {
        if (motor_swap_args.find("perf-test") != std::string::npos &&
            !perf_test_reuses_warm_lossless(settings.fg_enabled, settings.fg_multiplier, settings.fg_queue,
                                           settings.fg_adaptive, settings.fg_target_fps, g_perf_plan.fg_multiplier,
                                           g_perf_plan.fg_queue, g_perf_plan.fg_adaptive,
                                           g_perf_plan.fg_target_fps)) {
          LosslessFgController fg;
          (void)fg.kill_all_lossless_and_wait(400);
        }
        preview.stop();
        lobby_audio.stop();
        pipeline_global().stop();
        {
          wchar_t exe_path[MAX_PATH]{};
          GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
          (void)restore_reshade_dxgi(std::filesystem::path(exe_path).parent_path().string());
        }
        // Janela: solta o device do lobby antes do filho criar o dele. O HDMI segue com o device vivo.
        if (lobby_relaunch_releases_device_before_spawn(capture_source_is_window(settings.capture_source))) {
          if (logo_srv) {
            logo_srv->Release();
            logo_srv = nullptr;
          }
          if (watermark_srv) {
            watermark_srv->Release();
            watermark_srv = nullptr;
          }
          if (g_dx.ctx) g_dx.ctx->Flush();
          ImGui_ImplDX11_Shutdown();
          ImGui_ImplWin32_Shutdown();
          ImGui::DestroyContext();
          cleanup_device();
          if (g_hwnd) {
            DestroyWindow(g_hwnd);
            g_hwnd = nullptr;
          }
        }
        if (relaunch_current_process(motor_swap_args.c_str())) {
          ExitProcess(0);
        }
        if (lobby_relaunch_releases_device_before_spawn(capture_source_is_window(settings.capture_source))) {
          ExitProcess(1);
        }
        push_lobby_toast("Falha ao iniciar: feche e abra o NexusCap");
      } else if (motor_swap_refresh_lobby) {
        preview.stop();
        lobby_audio.stop();
        pipeline_global().stop();
        {
          wchar_t exe_path[MAX_PATH]{};
          GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
          const auto exe_dir = std::filesystem::path(exe_path).parent_path().string();
          if (lobby_loads_reshade_dxgi(settings.ui.preview_enabled, settings.ui.preview_cfg_live,
                                       settings.enhance_renodx)) {
            (void)restore_reshade_dxgi(exe_dir);
          }
        }
        motor_swap_refresh_lobby = false;
        if (relaunch_current_process(motor_swap_args.empty() ? nullptr : motor_swap_args.c_str())) {
          ExitProcess(0);
        }
        push_lobby_toast("Falha ao aplicar o preview");
      } else {
        // CFG/X no lobby so grava INI. O motor (dxgi) entra no Iniciar.
        pipeline_global().stop();
        push_lobby_toast("Guardado — o motor aplica no Iniciar");
      }
    }
  }

  // Free capture device BEFORE session start (device exclusive).
  preview.stop();
  lobby_audio.stop();
  // A sessão tenta abrir imediatamente e só espera/reabre se o dispositivo ainda estiver ocupado.
  if (logo_srv) {
    logo_srv->Release();
    logo_srv = nullptr;
  }
  if (watermark_srv) {
    watermark_srv->Release();
    watermark_srv = nullptr;
  }

  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  cleanup_device();
  if (g_hwnd) {
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
  }
  UnregisterClassW(wc.lpszClassName, wc.hInstance);

  if (!g_start_requested) return false;
  ls_guard.keep = true;
  {
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto exe_dir = std::filesystem::path(exe_path).parent_path().string();
    // Session always needs Kernel→Feed→Sharpen on disk (ReShade may reload on next Present).
    sync_mgf_techniques_for_phase(exe_dir, /*session_or_live_preview=*/true);
    (void)restore_reshade_dxgi(exe_dir);
  }
  apply_settings_to_cfg(settings, cfg);
  cfg.nr_runtime_on = nr_runtime_on;
  return true;
}

}  // namespace nexuscap
