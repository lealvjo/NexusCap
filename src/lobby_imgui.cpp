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
#include "nexuscap/settings_store.hpp"
#include "nexuscap/system_metrics.hpp"
#include "nexuscap/version.hpp"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

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
int* g_capture_vk_target = nullptr;
int g_lobby_gear_vk = 0x7B;  // default F12 — atualizado do settings a cada frame
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
  int started_w = 0;
  int started_h = 0;
  double started_fps = 0;
  int started_fourcc = -1;
  int started_buf = -1;
  int color_space = 0;
  int color_range = 0;
  int visual_preset = 0;
  bool enhance_preview = false;
  double fps_ema = 0;
  double last_frame_ms = 0;
  uint64_t frames_seen = 0;
  bool live = false;

  void apply_color(int space, int range) {
    color_space = clamp_color_space(space);
    color_range = clamp_color_range(range);
  }

  // Instant Default/Natural/Cinematic look on the preview band (not only disk/Present).
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
    slot.clear();
    release_tex();
    started_dev.clear();
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

  bool ensure(const std::string& device, int w, int h, double fps) {
    CaptureStartOptions opt;
    opt.width = w;
    opt.height = h;
    opt.fps = fps;
    return ensure(device, opt);
  }

  void tick() {
    FramePayload frame;
    if (!slot.consume(frame) || frame.width <= 0 || frame.height <= 0 || frame.nv12.empty()) return;

    ++frames_seen;
    const double now = clock.now_ms();
    if (last_frame_ms > 0) {
      const double inst = 1000.0 / (std::max)(0.001, now - last_frame_ms);
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
        const LobbyEscAction act = lobby_esc_action(g_open_gear, false);
        if (act == LobbyEscAction::CloseGear) g_open_gear = false;
        return 0;
      }
      if (lobby_gear_hotkey_toggles_cfg(static_cast<int>(wp), g_lobby_gear_vk, false)) {
        g_open_gear = !g_open_gear;
        return 0;
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

std::string resolve_logo_png_on_disk() {
  const std::string dir = exe_dir_utf8();
  for (const auto& p : {logo_png_candidate_assets(dir), logo_png_candidate_root(dir)}) {
    const DWORD attr = GetFileAttributesW(utf8_path_to_wide(p).c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) return p;
  }
  return {};
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

bool create_logo_srv(ID3D11Device* device, ID3D11ShaderResourceView** out_srv, int* out_w, int* out_h) {
  if (out_srv) *out_srv = nullptr;
  if (out_w) *out_w = 0;
  if (out_h) *out_h = 0;
  if (!device || !out_srv) return false;
  const std::string path = resolve_logo_png_on_disk();
  if (path.empty()) return false;
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

void apply_window_app_icon(HWND hwnd, HINSTANCE hi) {
  if (!hwnd) return;
  HICON icon = LoadIconW(hi, MAKEINTRESOURCEW(kAppIconResourceId));
  if (!icon) return;
  SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
  SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
}

// Mute/unmute WASAPI preview — bottom-right of letterboxed image.
void draw_preview_mute_button(PersistedSettings& settings, ImVec2 img_min, ImVec2 img_max) {
  const float pad = 10.f;
  const float sz = 28.f;
  ImGui::SetCursorScreenPos(ImVec2(img_max.x - pad - sz, img_max.y - pad - sz));
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.45f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.12f, 0.12f, 0.14f, 0.7f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.18f, 0.2f, 0.85f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.f);
  const char* label = settings.ui.preview_mute ? "M" : "S";
  if (ImGui::Button(label, ImVec2(sz, sz))) {
    settings.ui.preview_mute = !settings.ui.preview_mute;
    std::string serr;
    save_settings(settings, settings_default_path(), serr);
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetTooltip("%s", settings.ui.preview_mute ? "Som do preview (clique p/ ligar)"
                                                     : "Som do preview (clique p/ mutar)");
  }
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);
}

// Live HUD on left black bar of letterboxed preview — bright text readable on black.
void draw_hud_preview_overlay(const PersistedSettings& settings, const LobbyPreview& preview,
                              ImVec2 band_min, ImVec2 band_max, ImVec2 img_min, ImVec2 img_max) {
  if (!settings.ui.show_fps) return;
  // Live overlay owns session HUD — keep lobby preview clear of mini/ADV twin.
  if (settings.ui.live_overlay) return;

  LobbyHudPreviewLines hud = make_lobby_hud_preview_lines(settings, preview.fps_ema);
  if (hud.count <= 0) return;

  ImDrawList* dl = ImGui::GetWindowDrawList();

  // Mini overlay preview: 3-line bar matching session HUD (stalls on row 2).
  if (settings.ui.mini_overlay) {
    const float bar_h = static_cast<float>(hud_mini_overlay_height());
    ImVec2 p0(img_min.x, img_min.y);
    ImVec2 p1(img_max.x, img_min.y + bar_h);
    dl->AddRectFilled(p0, p1, IM_COL32(8, 10, 14, 210), 0.f);
    const float graph_w = 220.f;
    const float gx = (std::max)(p0.x + 8.f, p1.x - 8.f - graph_w);
    const float gy = p0.y + 9.f;
    const float gh = bar_h - 16.f;
    const float gw = (std::min)(graph_w, (std::max)(0.f, p1.x - p0.x - 16.f));

    const float fps = preview.fps_ema > 1.0 ? static_cast<float>(preview.fps_ema) : 0.f;
    const float ft = fps > 1.f ? (1000.f / fps) : 0.f;
    const int fgx = settings.fg_enabled ? (settings.fg_multiplier >= 2 ? settings.fg_multiplier : 2) : 1;
    const int scale = clamp_scale_percent(settings.scale_percent);
    const bool dlss5 = settings.enhance_renodx && settings.neural_rendering;
    SystemMetricsSample hw{};
    hw.cpu_name = "i5-11400F";
    hw.gpu_name = "RTX 5070";
    hw.cpu_ok = hw.gpu_ok = hw.cpu_temp_ok = hw.gpu_temp_ok = true;
    hw.vram_ok = hw.ram_ok = hw.power_ok = true;
    hw.cpu_percent = 35;
    hw.cpu_temp_c = 55;
    hw.gpu_percent = 40;
    hw.gpu_temp_c = 60;
    hw.vram_used_mb = 2048;
    hw.vram_budget_mb = 12288;
    hw.ram_used_mb = 16384;
    hw.ram_total_mb = 32768;
    hw.power_w = 90;
    char hw_compact[200]{};
    format_sys_metrics_compact(hw_compact, sizeof(hw_compact), hw);
    MiniOverlaySeg segs[kMiniOverlayMaxSegs]{};
    char line0[8]{}, line1[8]{};
    const int nseg = format_mini_overlay_segments(
        segs, kMiniOverlayMaxSegs, line0, sizeof(line0), line1, sizeof(line1), fps,
        fps * static_cast<double>((std::max)(fgx, 1)), false, fgx, dlss5, settings.ui.sync_image, ft, ft, ft, ft,
        true, ft, ft * 0.15f, 0, 0, 0, settings.width, settings.height, scaled_extent(settings.width, scale),
        scaled_extent(settings.height, scale), settings.width, settings.height, scale, false, 0, 0, 0, 0,
        renodx_visual_preset_name(settings.renodx_preset), hw_compact[0] ? hw_compact : nullptr, 0, 0, 0,
        nullptr, settings.fps, settings.nr_pass_count);

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
      dl->AddRectFilled(ImVec2(gx, gy), ImVec2(gx + gw, gy + gh), IM_COL32(20, 24, 30, 220), 2.f);
      const float tgt = settings.fps > 0 ? static_cast<float>(1000.0 / settings.fps) : 16.67f;
      const float t = ft > 0.f ? (std::min)(1.f, ft / (tgt * 2.5f)) : 0.2f;
      dl->AddRectFilled(ImVec2(gx + 2, gy + 2), ImVec2(gx + 2 + (gw - 4) * t, gy + gh - 2),
                        IM_COL32(100, 235, 255, 240), 2.f);
    }
    return;
  }

  // HW placeholders come from make_lobby_hud_preview_lines — strip them and paint a
  // dedicated multi-line block below (CPU / GPU / RAM / VRAM / PWR each on its own line).
  char hw_lines[kSysMetricsLineCount][64]{};
  int hw_n = 0;
  if (settings.ui.hw_info) {
    int keep = 0;
    for (int i = 0; i < hud.count; ++i) {
      const char* L = hud.line[i];
      if (std::strncmp(L, "CPU", 3) == 0 || std::strncmp(L, "GPU", 3) == 0 ||
          std::strncmp(L, "RAM", 3) == 0 || std::strncmp(L, "VRAM", 4) == 0 ||
          std::strncmp(L, "PWR", 3) == 0) {
        continue;
      }
      if (keep != i) std::snprintf(hud.line[keep], sizeof(hud.line[0]), "%s", L);
      ++keep;
    }
    hud.count = keep;

    g_lobby_hw.sample(g_dx.device, g_lobby_hw_sample);
    hw_n = format_sys_metrics_lines(hw_lines, kSysMetricsLineCount, g_lobby_hw_sample);
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
  const auto lay = lobby_hud_left_bar_layout(box, band_w, band_h, preferred_w, pad);
  const float box_w = (std::min)(preferred_w, lay.max_w);
  const float box_h = pad * 2.f + line_h * static_cast<float>(total_lines) +
                      (settings.ui.advanced_metrics ? 36.f : 0.f);
  ImVec2 p0(band_min.x + lay.x, band_min.y + lay.y);
  if (p0.y + box_h > band_max.y - 4.f) p0.y = (std::max)(band_min.y + 4.f, band_max.y - box_h - 4.f);
  ImVec2 p1(p0.x + box_w, p0.y + box_h);

  // No dark card on the black pillar — text sits directly on the bar.
  if (!lay.on_black_bar) {
    dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 160), 4.f);
  }

  // High-luminance colors so HUD stays readable on pure black side bars.
  const ImU32 cols[] = {
      IM_COL32(255, 230, 90, 255),  IM_COL32(160, 240, 255, 255), IM_COL32(120, 255, 180, 255),
      IM_COL32(255, 255, 255, 255), IM_COL32(140, 255, 220, 255), IM_COL32(200, 180, 255, 255),
      IM_COL32(255, 170, 120, 255), IM_COL32(160, 255, 200, 255), IM_COL32(255, 150, 150, 255),
  };
  int row = 0;
  for (int i = 0; i < hud.count; ++i, ++row) {
    dl->AddText(ImVec2(p0.x + 2.f, p0.y + pad + line_h * static_cast<float>(row)), cols[row % 9],
                hud.line[i]);
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
    dl->AddRectFilled(ImVec2(gx, gy), ImVec2(gx + gw, gy + gh), IM_COL32(20, 24, 30, 220), 3.f);
    const float tgt = settings.fps > 0 ? static_cast<float>(1000.0 / settings.fps) : 16.67f;
    const float t = ft > 0.f ? (std::min)(1.f, ft / (tgt * 2.5f)) : 0.2f;
    dl->AddRectFilled(ImVec2(gx + 2, gy + 2), ImVec2(gx + 2 + (gw - 4) * t, gy + gh - 2),
                      IM_COL32(100, 235, 255, 240), 2.f);
  }
}

}  // namespace

bool run_lobby(HINSTANCE hi, AppConfig& cfg, PersistedSettings& settings) {
  g_lobby_running = true;
  g_start_requested = false;
  g_open_gear = false;
  g_capture_vk_target = nullptr;

  // Scale is session-only. ReShade hooks lobby Present too — force feed 100% or the menu blurs.
  // Carry cfg.nr_runtime_on across Esc↔Iniciar (same RenoDX process — do not assume OFF).
  bool nr_runtime_on = cfg.nr_runtime_on;
  int nr_boot_retries = 0;
  bool renodx_dir_ok = true;
  {
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto exe_dir = std::filesystem::path(exe_path).parent_path();
    renodx_dir_ok = prepare_renodx_runtime_dirs(exe_dir.string());
    sync_feed_work_resolution_in_dir(exe_dir.string(), feed_work_resolution_for_lobby());
    // Idle lobby: drop Techniques when RenoDX off; keep full chain for live preview.
    sync_mgf_techniques_for_phase(exe_dir.string(), settings.enhance_renodx);
    const bool want_nr = want_neural_rendering(settings.enhance_renodx, settings.neural_rendering);
    sync_neural_rendering_enabled_files(exe_dir.string(), want_nr,
                                        feed_pipeline_on_for_renodx(settings.enhance_renodx));
    if (want_nr != nr_runtime_on) nr_boot_retries = 180;
  }

  WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hi, nullptr, nullptr, nullptr, nullptr, L"NexusCapLobby",
                 nullptr};
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

  LobbyPreview preview;
  AudioMonitor lobby_audio;
  bool lobby_audio_want = false;

  std::vector<std::string> devices = list_video_capture_devices();
  int device_idx = 0;
  for (size_t i = 0; i < devices.size(); ++i) {
    if (devices[i].find(settings.device_name) != std::string::npos || settings.device_name == devices[i]) {
      device_idx = static_cast<int>(i);
      break;
    }
  }

  auto current_device = [&]() -> std::string {
    if (!devices.empty() && device_idx >= 0 && device_idx < static_cast<int>(devices.size())) {
      return devices[static_cast<size_t>(device_idx)];
    }
    return settings.device_name.empty() ? std::string("auto") : settings.device_name;
  };

  auto refresh_modes = [&](std::vector<CaptureMode>& modes, int& mode_idx) {
    modes = list_video_capture_modes(current_device());
    // Keep only modes that advertise NV12 — MJPEG-only (4K60 etc.) can't feed the grabber.
    {
      std::vector<CaptureMode> usable;
      usable.reserve(modes.size());
      for (const auto& m : modes) {
        const auto fcs = list_video_fourccs_for_mode(current_device(), m.width, m.height, m.fps);
        bool has_nv12 = false;
        for (int fc : fcs) {
          if (fc == static_cast<int>(VideoFourcc::Nv12)) {
            has_nv12 = true;
            break;
          }
        }
        if (has_nv12) usable.push_back(m);
      }
      if (!usable.empty()) modes.swap(usable);
    }
    mode_idx = 0;
    for (size_t i = 0; i < modes.size(); ++i) {
      if (modes[i].width == settings.width && modes[i].height == settings.height &&
          std::abs(modes[i].fps - settings.fps) < 1.0) {
        mode_idx = static_cast<int>(i);
        break;
      }
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
    fourccs = list_video_fourccs_for_mode(current_device(), settings.width, settings.height, settings.fps);
    if (fourccs.empty()) fourccs.push_back(static_cast<int>(kDefaultVideoFourcc));
    if (std::find(fourccs.begin(), fourccs.end(), static_cast<int>(VideoFourcc::Any)) == fourccs.end()) {
      fourccs.insert(fourccs.begin(), static_cast<int>(VideoFourcc::Any));
    }
    settings.video_fourcc = resolve_video_fourcc_for_device(settings.video_fourcc, fourccs);
    fourccs_cache_key = key;
  };
  refresh_fourccs();

  auto restart_preview = [&]() -> bool {
    settings.device_name = current_device();
    CaptureStartOptions opt;
    opt.width = settings.width;
    opt.height = settings.height;
    opt.fps = settings.fps;
    opt.video_fourcc = settings.video_fourcc;
    opt.color_space = settings.color_space;
    opt.color_range = settings.color_range;
    opt.capture_buffering = settings.capture_buffering;
    return preview.ensure(settings.device_name, opt);
  };
  restart_preview();

  float scale_f = static_cast<float>(clamp_scale_percent(settings.scale_percent));
  // The lobby process always boots the Feed at 100% (menus would be downsampled too), so
  // any scale below that only takes effect in the relaunched session process.
  const int feed_loaded_scale = feed_work_resolution_for_lobby();
  bool gear_was_open = false;
  std::string lobby_toast;
  double lobby_toast_until = 0.0;
  bool motor_swap_pending = false;
  double motor_swap_until = 0.0;
  std::string motor_swap_title = "Trocando motor DLSS 5";
  std::string motor_swap_line;
  std::string motor_swap_args;  // "--autostart" resumes the session after the restart
  auto push_lobby_toast = [&](std::string msg) {
    if (msg.empty()) return;
    lobby_toast = std::move(msg);
    lobby_toast_until = ImGui::GetTime() + (kSessionNoticeTtlMs / 1000.0);
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

    preview.set_visual_preview(settings.renodx_preset,
                               want_neural_rendering(settings.enhance_renodx, settings.neural_rendering));
    preview.tick();

    // Preview audio: start/stop with live capture; mute zeros WASAPI frames (keeps path warm).
    {
      const bool want_audio = preview.live;
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
      if (lobby_audio.running()) lobby_audio.set_muted(settings.ui.preview_mute);
    }

    // Lobby Present loads Bridge asynchronously — pulse only when carried runtime ≠ want.
    if (nr_boot_retries > 0) {
      --nr_boot_retries;
      const bool want = want_neural_rendering(settings.enhance_renodx, settings.neural_rendering);
      if (want == nr_runtime_on) {
        nr_boot_retries = 0;
      } else if (dlss5_bridge_ready_for_nr()) {
        wchar_t exe_path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        if (apply_neural_rendering_state(std::filesystem::path(exe_path).parent_path().string(), want,
                                         &nr_runtime_on)) {
          nr_boot_retries = 0;
          cfg.nr_runtime_on = nr_runtime_on;
        }
      }
    }

    g_lobby_gear_vk = settings.hotkeys.vk_gear;
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
      if (logo_srv) {
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
      ImGui::SameLine(ImGui::GetWindowWidth() - 72);
      if (ImGui::Button("CFG", ImVec2(56, 40))) g_open_gear = true;
    }
    ImGui::EndChild();
    ImGui::Separator();

    // Pixel-tight vertical budget: body + ItemSpacing + footer == ContentRegionAvail
    // (ImGui inserts ItemSpacing between the body row and the footer child — do NOT add Dummy).
    const float item_sp_y = ImGui::GetStyle().ItemSpacing.y;
    const float footer_h = 72.f;
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
    ImGui::TextUnformatted("Fontes de video");
    ImGui::Spacing();

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

    ImGui::TextDisabled("MODO");
    ImGui::SetNextItemWidth(-1);
    if (modes.empty()) {
      ImGui::TextDisabled("(sem caps — %dx%d @ %.0f)", settings.width, settings.height, settings.fps);
    } else {
      std::vector<const char*> mlabels;
      mlabels.reserve(modes.size());
      for (auto& m : modes) mlabels.push_back(m.label.c_str());
      if (ImGui::Combo("##mode", &mode_idx, mlabels.data(), static_cast<int>(mlabels.size()))) {
        const auto& m = modes[static_cast<size_t>(mode_idx)];
        settings.width = m.width;
        settings.height = m.height;
        settings.fps = m.fps;
        fourccs_cache_key.clear();
        refresh_fourccs();
        // 4K+: force NV12 when available — MJPEG often opens then yields white/fake.
        if (capture_mode_prefers_nv12(settings.width, settings.height)) {
          for (int fc : fourccs) {
            if (fc == static_cast<int>(VideoFourcc::Nv12)) {
              settings.video_fourcc = fc;
              break;
            }
          }
        }
        restart_preview();
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
      if (ImGui::Combo("##fmt", &fmt_idx, fmt_labels.data(), static_cast<int>(fmt_labels.size()))) {
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
    if (!preview.err.empty()) {
      ImGui::TextColored(ImVec4(1.f, 0.55f, 0.2f, 1.f), "%s", preview.err.c_str());
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
        ImGui::TextDisabled("PRE-VISUALIZACAO  ·  %dx%d @ %.0f", settings.width, settings.height, settings.fps);
        ImGui::SameLine(ImGui::GetWindowWidth() - 160);
        if (preview.live && preview.frames_seen > 0) {
          ImGui::TextColored(ImVec4(0.3f, 0.95f, 0.45f, 1.f), "AO VIVO");
        } else if (preview.live) {
          ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.2f, 1.f), "AGUARDANDO");
        } else {
          ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.35f, 1.f), "OFF");
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
        if (preview.srv && preview.tex_w > 0 && preview.tex_h > 0 && avail.x > 1.f && avail.y > 1.f) {
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
            ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "Preview: %s", preview.err.c_str());
          } else if (preview.live) {
            ImGui::TextDisabled("Aguardando sinal da placa...");
          } else {
            ImGui::TextDisabled("Sem preview (escolha dispositivo / Recarregar)");
          }
          ImGui::Text("  %s", settings.device_name.empty() ? "auto" : settings.device_name.c_str());
          ImGui::TextDisabled("  %s · %s · %s", video_fourcc_name(settings.video_fourcc),
                              color_space_name(settings.color_space), color_range_name(settings.color_range));
        }
        if (hud_has_img) {
          draw_hud_preview_overlay(settings, preview, band_min, band_max, img_min, img_max);
          draw_preview_mute_button(settings, img_min, img_max);
        }
        if (!settings.ui.show_fps) {
          ImGui::SetCursorPos(ImVec2(12, 8));
          ImGui::TextColored(ImVec4(0.75f, 0.9f, 1.f, 1.f), "HUD off (marque Exibir FPS)");
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
    ImGui::TextDisabled("SESSAO");
    ImGui::TextUnformatted("Configuracoes");
    ImGui::Spacing();
    // Single column — two-up SameLine clipped labels on the right edge.
    ImGui::Checkbox("Exibir FPS (HUD)", &settings.ui.show_fps);
    if (ImGui::Checkbox("Mini overlay (barra topo)", &settings.ui.mini_overlay)) {
      if (settings.ui.mini_overlay) settings.ui.advanced_metrics = false;
    }
    ImGui::TextDisabled(settings.ui.mini_overlay ? "  -> 2 linhas · lat cap/present(+NR) · FT/L600/HW"
                                                 : "  -> HUD multi-linha · latencia cap + present");
    if (settings.ui.mini_overlay) {
      ImGui::TextDisabled("  present+NR ≈ processamento c/ DLSS5 (~2ms) · cap = captura pura");
    }

    ImGui::BeginDisabled(settings.ui.mini_overlay);
    ImGui::Checkbox("Metricas avancadas", &settings.ui.advanced_metrics);
    ImGui::Checkbox("Infos GPU / CPU", &settings.ui.hw_info);
    ImGui::EndDisabled();

    ImGui::Checkbox("Tela cheia", &settings.ui.fullscreen);
    ImGui::Checkbox("Ocultar lobby na captura", &settings.ui.hide_panel_on_capture);
    ImGui::TextDisabled(settings.ui.fullscreen ? "  -> Iniciar fullscreen" : "  -> Iniciar em janela");

    ImGui::Checkbox("Sincronizar imagem", &settings.ui.sync_image);
    ImGui::TextDisabled(settings.ui.sync_image ? "  -> SYNC VSYNC" : "  -> SYNC TEAR");
    ImGui::Checkbox("Overlay live", &settings.ui.live_overlay);
    if (settings.ui.live_overlay) {
      LiveConsole cur = parse_live_console(settings.ui.live_console);
      const char* preview = live_console_label(cur);
      auto console_imcol = [](LiveConsole c) -> ImVec4 {
        const COLORREF rgb = live_console_color(c);
        return ImVec4(GetRValue(rgb) / 255.f, GetGValue(rgb) / 255.f, GetBValue(rgb) / 255.f, 1.f);
      };
      ImGui::PushStyleColor(ImGuiCol_Text, console_imcol(cur));
      const bool open = ImGui::BeginCombo("Console##live", preview);
      ImGui::PopStyleColor();
      if (open) {
        const LiveConsole opts[] = {LiveConsole::Ps5, LiveConsole::XboxSs, LiveConsole::Switch2};
        for (LiveConsole o : opts) {
          const bool selected = (o == cur);
          ImGui::PushStyleColor(ImGuiCol_Text, console_imcol(o));
          if (ImGui::Selectable(live_console_label(o), selected)) {
            settings.ui.live_console = live_console_key(o);
          }
          ImGui::PopStyleColor();
          if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
    }
    ImGui::Checkbox("Reportar bug", &settings.ui.bug_report);
    ImGui::TextDisabled(settings.ui.bug_report ? "  -> modo diagnostico (report/*.txt)"
                                               : "  -> off (sem overhead)");
    ImGui::Checkbox("Reportar performance", &settings.ui.perf_report);
    ImGui::TextDisabled(settings.ui.perf_report ? "  -> snapshot p/ IA (performance/*.txt)"
                                                : "  -> off (sem overhead)");
    ImGui::EndChild();

    // Footer sits directly under body; natural ItemSpacing.y is the only gap (already reserved).
    ImGui::BeginChild("footer", ImVec2(0, footer_h), false, ImGuiWindowFlags_NoScrollbar);
    {
      const float row_h = 48.f;
      // Center the button row inside footer_h — slight air above without growing the layout.
      const float pad_y = (std::max)(0.f, (footer_h - row_h) * 0.5f - 2.f);
      if (pad_y > 0.5f) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + pad_y);

      if (preview.live && preview.frames_seen > 0) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.f), "* Preview ao vivo — Iniciar libera a placa");
      } else if (!preview.err.empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(1.f, 0.5f, 0.35f, 1.f), "! %s", preview.err.c_str());
      } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.f), "* Pronto — Esc na captura volta ao menu");
      }
      ImGui::SameLine(ImGui::GetWindowWidth() - 420);
      if (ImGui::Button("Sair", ImVec2(100, row_h))) {
        g_start_requested = false;
        g_lobby_running = false;
      }
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.95f, 1.f));
      if (ImGui::Button("Iniciar processamento", ImVec2(280, row_h))) {
        if (!devices.empty() && device_idx >= 0 && device_idx < static_cast<int>(devices.size())) {
          settings.device_name = devices[static_cast<size_t>(device_idx)];
        }
        if (!modes.empty() && mode_idx >= 0 && mode_idx < static_cast<int>(modes.size())) {
          settings.width = modes[static_cast<size_t>(mode_idx)].width;
          settings.height = modes[static_cast<size_t>(mode_idx)].height;
          settings.fps = modes[static_cast<size_t>(mode_idx)].fps;
        }
        settings.scale_percent = clamp_scale_percent(static_cast<int>(std::lround(scale_f)));
        const int session_scale = clamp_scale_percent(settings.scale_percent);
        {
          wchar_t exe_path[MAX_PATH]{};
          GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
          sync_feed_work_resolution_in_dir(std::filesystem::path(exe_path).parent_path().string(),
                                           feed_work_resolution_for_session(session_scale));
        }
        if (feed_scale_change_needs_relaunch(settings.enhance_renodx, feed_loaded_scale, session_scale)) {
          // Feed already snapshotted the old work_resolution — restart, then resume the session.
          std::string serr;
          save_settings(settings, settings_default_path(), serr);
          motor_swap_title = "Aplicando escala do Feed";
          motor_swap_line = std::to_string(effective_scale_percent(true, feed_loaded_scale)) + "%  \xE2\x86\x92  " +
                            std::to_string(effective_scale_percent(true, session_scale)) + "%";
          motor_swap_args = "--autostart";
          motor_swap_pending = true;
          motor_swap_until = ImGui::GetTime() + kDlss5MotorSwapHoldSec;
          push_lobby_toast(session_notice_feed_scale_apply(effective_scale_percent(true, feed_loaded_scale),
                                                          effective_scale_percent(true, session_scale)));
        } else {
          g_start_requested = true;
          g_lobby_running = false;
        }
      }
      ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    ImGui::End();

    if (g_open_gear && !gear_was_open) {
      scale_f = static_cast<float>(clamp_scale_percent(settings.scale_percent));
    }
    gear_was_open = g_open_gear;

    // Non-modal side panel — preview stays visible so you see adjustments live.
    if (g_open_gear) {
      const ImGuiViewport* mvp = ImGui::GetMainViewport();
      // Mesma regra do CFG da captura: largura acompanha a janela, senão as notas ficam cortadas.
      const float gear_w = (std::max)(420.f, (std::min)(mvp->WorkSize.x * 0.32f, 860.f));
      const float gear_h = (std::max)(240.f, mvp->WorkSize.y - 120.f);
      ImGui::SetNextWindowPos(ImVec2(mvp->WorkPos.x + mvp->WorkSize.x - gear_w - 40.f, mvp->WorkPos.y + 80.f),
                              ImGuiCond_Appearing);
      ImGui::SetNextWindowSize(ImVec2(gear_w, gear_h), ImGuiCond_Appearing);
      if (ImGui::Begin("Configuracoes (CFG)", &g_open_gear, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(nexuscap_accent_color(), "Ajuste e veja no preview ao vivo");
        ImGui::PushTextWrapPos(0.f);  // notas longas quebram linha em vez de sumir na borda
        ImGui::Separator();
        ImGui::TextUnformatted("Frame Gen (Lossless Scaling)");
        const bool fg_was = settings.fg_enabled;
        const int fg_mult_was = settings.fg_multiplier;
        ImGui::Checkbox("Habilitar Frame Gen", &settings.fg_enabled);
        ImGui::BeginDisabled(!settings.fg_enabled);
        ImGui::RadioButton("x2", &settings.fg_multiplier, 2);
        ImGui::SameLine();
        ImGui::RadioButton("x3", &settings.fg_multiplier, 3);
        ImGui::SameLine();
        ImGui::RadioButton("x4", &settings.fg_multiplier, 4);
        ImGui::EndDisabled();
        if (settings.fg_enabled && !fg_was) {
          const std::string resolved = resolve_lossless_exe(settings.fg_path);
          if (resolved.empty()) {
            settings.fg_enabled = false;
            push_lobby_toast(session_notice_fg_missing());
          } else {
            push_lobby_toast(session_notice_fg_on());
          }
        } else if (settings.fg_enabled && session_notice_for_fg_cycle(fg_mult_was, settings.fg_multiplier)) {
          push_lobby_toast(session_notice_fg_multiplier_up(settings.fg_multiplier));
        }
        {
          const std::string resolved = resolve_lossless_exe(settings.fg_path);
          ImGui::TextUnformatted("Exe do Lossless Scaling");
          if (ImGui::Button("Pasta##fg_browse", ImVec2(72, 0))) {
            std::string picked;
            if (browse_lossless_exe_path(g_hwnd, picked)) settings.fg_path = picked;
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

        ImGui::Separator();
        ImGui::TextUnformatted("Escala de resolucao (trabalho GPU)");
        int scale_i = clamp_scale_percent(settings.scale_percent);
        // Writing the cfg here would not reach this process' Feed (locked at 100%) and would
        // leave a scaled cfg behind if the app is killed — Iniciar writes it before relaunch.
        ImGui::SliderInt("##scale", &scale_i, kMinScalePercent, kMaxScalePercent, "%d%%");
        settings.scale_percent = clamp_scale_percent(scale_i);
        scale_f = static_cast<float>(settings.scale_percent);
        const int eff_scale = effective_scale_percent(settings.enhance_renodx, settings.scale_percent);
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
          if (feed_scale_change_needs_relaunch(true, feed_loaded_scale, settings.scale_percent)) {
            ImGui::TextColored(ImVec4(1.f, 0.82f, 0.35f, 1.f), "Feed ativo em %d%% — Iniciar aplica %d%% (reinicio)",
                               effective_scale_percent(true, feed_loaded_scale), eff_scale);
          }
        } else {
          ImGui::TextDisabled("Sinal %dx%d  ·  pixels RGB ~%d%%  ·  min %d%%", settings.width, settings.height,
                              pct_pix, kMinScalePercent);
        }
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
          sync_mgf_techniques_for_phase(exe_dir, settings.enhance_renodx);
          const bool want = want_neural_rendering(settings.enhance_renodx, settings.neural_rendering);
          if (!apply_neural_rendering_state(exe_dir, want, &nr_runtime_on,
                                            feed_pipeline_on_for_renodx(settings.enhance_renodx)) &&
              want && !dlss5_bridge_ready_for_nr()) {
            nr_boot_retries = 120;
          }
          push_lobby_toast(want ? session_notice_nr_on() : session_notice_nr_off());
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
        if (ImGui::RadioButton("Default", preset_i == 0)) preset_i = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Natural", preset_i == 1)) preset_i = 1;
        ImGui::SameLine();
        if (ImGui::RadioButton("Cinematic", preset_i == 2)) preset_i = 2;
        settings.renodx_preset = clamp_renodx_visual_preset(preset_i);
        if (settings.renodx_preset != preset_before) {
          wchar_t exe_path[MAX_PATH]{};
          GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
          const auto exe_dir = std::filesystem::path(exe_path).parent_path().string();
          const bool want = want_neural_rendering(settings.enhance_renodx, settings.neural_rendering);
          // Immediate look on the preview band + sync RenoDX files / Bridge for Present.
          preview.set_visual_preview(settings.renodx_preset, want);
          const bool writable = prepare_renodx_runtime_dirs(exe_dir);
          if (!writable) {
            push_lobby_toast(renodx_write_denied_notice());
          } else {
            apply_lobby_renodx_preset_live(exe_dir, settings.renodx_preset, want, &nr_runtime_on);
            cfg.nr_runtime_on = nr_runtime_on;
            push_lobby_toast(std::string("Preset: ") + renodx_visual_preset_name(settings.renodx_preset) +
                             " — olhe o preview");
            if (want && !nr_runtime_on && !dlss5_bridge_ready_for_nr()) {
              nr_boot_retries = 120;
            }
          }
        }
        ImGui::TextDisabled("NR=%d  model=%d  ·  olhe a imagem (nao so o texto)",
                            renodx_nr_preset_index(settings.renodx_preset),
                            renodx_feed_model_preset(settings.renodx_preset));
        ImGui::TextDisabled(nr_pass_count_uses_multipass_addon(settings.nr_pass_count)
                                ? "  no multipass o Iniciar aplica o preset (o addon so le no boot)"
                                : "  no 1x o preview muda na hora; na captura o preset reinicia");
        ImGui::TextDisabled("Mesmo look no Iniciar — janela ou tela cheia.");

        ImGui::Spacing();
        ImGui::TextUnformatted("Pass Count (NR multipass)");
        const int pass_before = clamp_nr_pass_count(settings.nr_pass_count);
        int pass_i = pass_before;
        if (ImGui::RadioButton("1x##nrpass", pass_i == 1)) pass_i = 1;
        ImGui::SameLine();
        if (ImGui::RadioButton("1,5x##nrpass", pass_i == 4)) pass_i = 4;
        ImGui::SameLine();
        if (ImGui::RadioButton("2x##nrpass", pass_i == 2)) pass_i = 2;
        ImGui::SameLine();
        if (ImGui::RadioButton("3x##nrpass", pass_i == 3)) pass_i = 3;
        settings.nr_pass_count = clamp_nr_pass_count(pass_i);
        if (settings.nr_pass_count != pass_before) {
          wchar_t exe_path[MAX_PATH]{};
          GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
          const auto exe_dir = std::filesystem::path(exe_path).parent_path().string();
          const bool want = want_neural_rendering(settings.enhance_renodx, settings.neural_rendering);
          const bool writable = prepare_renodx_runtime_dirs(exe_dir);
          // Multipass não relê o PassCount no processo vivo, então 1,5x↔2x↔3x também reinicia.
          const bool need_relaunch = !nr_pass_count_change_applies_live(pass_before, settings.nr_pass_count);
          if (!writable) {
            push_lobby_toast(renodx_write_denied_notice());
          } else if (need_relaunch) {
            // Krish ↔ multipass: show hold notice, then relaunch (avoids "bugou" flash).
            std::string serr;
            save_settings(settings, settings_default_path(), serr);
            (void)sync_nr_pass_count_files(exe_dir, settings.nr_pass_count, settings.renodx_preset);
            (void)sync_feed_work_resolution_in_dir(
                exe_dir, feed_work_resolution_for_session(settings.scale_percent));
            const bool motor_change =
                nr_pass_count_change_needs_relaunch(pass_before, settings.nr_pass_count);
            motor_swap_title = motor_change ? "Trocando motor DLSS 5" : "Aplicando Pass Count";
            motor_swap_line = std::string(nr_pass_count_label(pass_before)) + "  \xE2\x86\x92  " +
                              nr_pass_count_label(settings.nr_pass_count);
            motor_swap_args.clear();
            motor_swap_pending = true;
            motor_swap_until = ImGui::GetTime() + kDlss5MotorSwapHoldSec;
            push_lobby_toast(motor_change
                                 ? session_notice_dlss5_motor_swap(pass_before, settings.nr_pass_count)
                                 : session_notice_nr_pass_count_apply(pass_before, settings.nr_pass_count));
          } else {
            // 1,5x ↔ 2x ↔ 3x: same multipass addon — INI + NR reload only.
            apply_lobby_nr_pass_count_live(exe_dir, settings.nr_pass_count, want, &nr_runtime_on,
                                           settings.renodx_preset);
            cfg.nr_runtime_on = nr_runtime_on;
            push_lobby_toast(std::string("Pass Count: ") + nr_pass_count_label(settings.nr_pass_count));
            if (want && !nr_runtime_on && !dlss5_bridge_ready_for_nr()) {
              nr_boot_retries = 120;
            }
          }
        }
        ImGui::TextDisabled("1,5x: mesma res e ~60 fps que o 1x (recomendado).");
        ImGui::TextDisabled("2x/3x: mais pesados, pouco ganho, look mais processado.");
        ImGui::TextDisabled("Escala do Feed e manual se quiseres menos carga.");
        ImGui::TextDisabled("Delete ou Ctrl+M liga/desliga o NR na captura (reinicia e volta).");
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::TextUnformatted("Atalhos (estilo X2)");
        auto hotkey_row = [&](const char* label, int* vk) {
          ImGui::Text("%s", label);
          ImGui::SameLine(200);
          char buf[64];
          std::snprintf(buf, sizeof(buf), "%s##%s", vk_display_name(*vk), label);
          if (g_capture_vk_target == vk) {
            ImGui::Button("Pressione tecla...", ImVec2(160, 0));
          } else if (ImGui::Button(buf, ImVec2(160, 0))) {
            g_capture_vk_target = vk;
          }
        };
        hotkey_row("CFG no lobby", &settings.hotkeys.vk_gear);
        ImGui::TextDisabled("  default F12 — abre este painel sem clicar no botao CFG");
        hotkey_row("Frame Gen on/off", &settings.hotkeys.vk_fg_toggle);
        hotkey_row("Ciclo FG x2/x3/x4", &settings.hotkeys.vk_fg_cycle);
        {
          ImGui::AlignTextToFramePadding();
          ImGui::TextUnformatted("Overlay HUD");
          ImGui::SameLine(220);
          char buf[64];
          if (settings.hotkeys.hud_ctrl) {
            std::snprintf(buf, sizeof(buf), "Ctrl+%s##hud", vk_display_name(settings.hotkeys.vk_hud));
          } else {
            std::snprintf(buf, sizeof(buf), "%s##hud", vk_display_name(settings.hotkeys.vk_hud));
          }
          if (g_capture_vk_target == &settings.hotkeys.vk_hud) {
            ImGui::Button("... pressione tecla ...", ImVec2(200, 0));
          } else if (ImGui::Button(buf, ImVec2(200, 0))) {
            g_capture_vk_target = &settings.hotkeys.vk_hud;
          }
          ImGui::SameLine();
          ImGui::Checkbox("Ctrl##hud_ctrl", &settings.hotkeys.hud_ctrl);
        }
        {
          ImGui::AlignTextToFramePadding();
          ImGui::TextUnformatted("Menu na captura");
          ImGui::SameLine(220);
          char buf[64];
          if (settings.hotkeys.cfg_ctrl) {
            std::snprintf(buf, sizeof(buf), "Ctrl+%s##cfgkey", vk_display_name(settings.hotkeys.vk_cfg));
          } else {
            std::snprintf(buf, sizeof(buf), "%s##cfgkey", vk_display_name(settings.hotkeys.vk_cfg));
          }
          if (g_capture_vk_target == &settings.hotkeys.vk_cfg) {
            ImGui::Button("... pressione tecla ...", ImVec2(200, 0));
          } else if (ImGui::Button(buf, ImVec2(200, 0))) {
            g_capture_vk_target = &settings.hotkeys.vk_cfg;
          }
          ImGui::SameLine();
          ImGui::Checkbox("Ctrl##cfg_ctrl", &settings.hotkeys.cfg_ctrl);
          ImGui::TextDisabled("  abre este CFG por cima do jogo, sem sair da captura");
        }
        hotkey_row("Analyze", &settings.hotkeys.vk_analyze);
        hotkey_row("RenoDX overlay", &settings.hotkeys.vk_renodx);
        hotkey_row("Neural Rendering on/off", &settings.hotkeys.vk_nr_toggle);
        hotkey_row("Tela cheia", &settings.hotkeys.vk_fullscreen);

        ImGui::Spacing();
        auto persist_gear = [&]() {
          settings.scale_percent = clamp_scale_percent(static_cast<int>(std::lround(scale_f)));
          settings.renodx_preset = clamp_renodx_visual_preset(settings.renodx_preset);
          settings.nr_pass_count = clamp_nr_pass_count(settings.nr_pass_count);
          if (!settings.enhance_renodx) settings.neural_rendering = false;
          std::string serr;
          save_settings(settings, settings_default_path(), serr);
          wchar_t exe_path[MAX_PATH]{};
          GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
          const auto exe_dir = std::filesystem::path(exe_path).parent_path().string();
          sync_renodx_visual_preset_files(exe_dir, settings.renodx_preset);
          sync_nr_pass_count_files(exe_dir, settings.nr_pass_count, settings.renodx_preset);
          const bool want = want_neural_rendering(settings.enhance_renodx, settings.neural_rendering);
          apply_neural_rendering_state(exe_dir, want, &nr_runtime_on,
                                       feed_pipeline_on_for_renodx(settings.enhance_renodx));
        };
        if (ImGui::Button("Salvar", ImVec2(120, 0))) {
          persist_gear();
          g_open_gear = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Fechar", ImVec2(120, 0))) {
          persist_gear();
          g_open_gear = false;
        }
        ImGui::PopTextWrapPos();
      }
      ImGui::End();
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

    // Hold screen while swapping Krish ↔ multipass so restart does not look like a crash.
    if (motor_swap_pending) {
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
        ImGui::Spacing();
        ImGui::TextUnformatted(motor_swap_line.c_str());
        ImGui::TextDisabled("Feed + RenoDX reiniciam — aguarde…");
        const double left = (std::max)(0.0, motor_swap_until - ImGui::GetTime());
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.f, 1.f), "%.1fs", left);
      }
      ImGui::End();
      ImGui::PopStyleVar(2);
      ImGui::PopStyleColor(2);
    }

    ImGui::Render();
    const float clear[4] = {0.06f, 0.07f, 0.10f, 1.f};
    g_dx.ctx->OMSetRenderTargets(1, &g_dx.rtv, nullptr);
    g_dx.ctx->ClearRenderTargetView(g_dx.rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    // Uncapped Present — vsync Present(1) + full-res CPU preview decode locked ~30 fps.
    g_dx.swap->Present(0, 0);

    if (motor_swap_pending && ImGui::GetTime() >= motor_swap_until) {
      motor_swap_pending = false;
      // Release the capture device before the new process claims it (exclusive).
      preview.stop();
      lobby_audio.stop();
      if (relaunch_current_process(motor_swap_args.empty() ? nullptr : motor_swap_args.c_str())) {
        ExitProcess(0);
      }
      push_lobby_toast("Falha ao reiniciar — feche e abra o NexusCap");
    }
  }

  // Free capture device BEFORE session start (device exclusive).
  preview.stop();
  lobby_audio.stop();
  // Extra settle: Iniciar after mode churn — driver still releasing pin format.
  if (g_start_requested) Sleep(static_cast<DWORD>(capture_device_settle_ms()));
  if (logo_srv) {
    logo_srv->Release();
    logo_srv = nullptr;
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
  {
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const auto exe_dir = std::filesystem::path(exe_path).parent_path().string();
    // Session always needs Kernel→Feed→Sharpen on disk (ReShade may reload on next Present).
    sync_mgf_techniques_for_phase(exe_dir, /*session_or_live_preview=*/true);
  }
  apply_settings_to_cfg(settings, cfg);
  cfg.nr_runtime_on = nr_runtime_on;
  return true;
}

}  // namespace nexuscap
