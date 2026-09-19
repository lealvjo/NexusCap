#include "nexuscap/capture.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propidl.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d3d11.lib")

namespace nexuscap {
namespace {


std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return L"";
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 0) return L"";
  std::wstring w(static_cast<size_t>(n), 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  if (!w.empty() && w.back() == 0) w.pop_back();
  return w;
}

std::string file_tail(const std::string& path) {
  const auto pos = path.find_last_of("\\/");
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

void scale_nv12_integer(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
  const int fx = dw / sw;
  const int fy = dh / sh;
  for (int y = 0; y < sh; ++y) {
    const uint8_t* row = src + static_cast<size_t>(y) * static_cast<size_t>(sw);
    for (int j = 0; j < fy; ++j) {
      uint8_t* out = dst + static_cast<size_t>(y * fy + j) * static_cast<size_t>(dw);
      if (fx == 2) {
        for (int x = 0; x < sw; ++x) {
          out[x * 2] = row[x];
          out[x * 2 + 1] = row[x];
        }
      } else {
        for (int x = 0; x < sw; ++x) {
          for (int i = 0; i < fx; ++i) out[x * fx + i] = row[x];
        }
      }
    }
  }
  const uint8_t* src_uv = src + static_cast<size_t>(sw) * static_cast<size_t>(sh);
  uint8_t* dst_uv = dst + static_cast<size_t>(dw) * static_cast<size_t>(dh);
  const int sw2 = sw / 2;
  const int sh2 = sh / 2;
  for (int y = 0; y < sh2; ++y) {
    const uint8_t* row = src_uv + static_cast<size_t>(y) * static_cast<size_t>(sw);
    for (int j = 0; j < fy; ++j) {
      uint8_t* out = dst_uv + static_cast<size_t>(y * fy + j) * static_cast<size_t>(dw);
      if (fx == 2) {
        for (int x = 0; x < sw2; ++x) {
          const int s = x * 2;
          const int d = x * 4;
          out[d] = row[s];
          out[d + 1] = row[s + 1];
          out[d + 2] = row[s];
          out[d + 3] = row[s + 1];
        }
      } else {
        for (int x = 0; x < sw2; ++x) {
          const uint8_t u = row[x * 2];
          const uint8_t v = row[x * 2 + 1];
          for (int i = 0; i < fx; ++i) {
            out[(x * fx + i) * 2] = u;
            out[(x * fx + i) * 2 + 1] = v;
          }
        }
      }
    }
  }
}

void scale_nv12_nearest(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
  if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
  if (sw == dw && sh == dh) {
    std::memcpy(dst, src, static_cast<size_t>(dw) * static_cast<size_t>(dh) * 3u / 2u);
    return;
  }
  if (file_cap_is_integer_scale(sw, sh, dw, dh)) {
    scale_nv12_integer(src, sw, sh, dst, dw, dh);
    return;
  }
  const size_t src_y = static_cast<size_t>(sw) * static_cast<size_t>(sh);
  const size_t dst_y = static_cast<size_t>(dw) * static_cast<size_t>(dh);
  for (int y = 0; y < dh; ++y) {
    const int sy = y * sh / dh;
    const uint8_t* row = src + static_cast<size_t>(sy) * static_cast<size_t>(sw);
    uint8_t* out = dst + static_cast<size_t>(y) * static_cast<size_t>(dw);
    for (int x = 0; x < dw; ++x) out[x] = row[x * sw / dw];
  }
  const int sw2 = sw / 2;
  const int sh2 = sh / 2;
  const int dw2 = dw / 2;
  const int dh2 = dh / 2;
  const uint8_t* src_uv = src + src_y;
  uint8_t* dst_uv = dst + dst_y;
  for (int y = 0; y < dh2; ++y) {
    const int sy = y * sh2 / dh2;
    const uint8_t* row = src_uv + static_cast<size_t>(sy) * static_cast<size_t>(sw);
    uint8_t* out = dst_uv + static_cast<size_t>(y) * static_cast<size_t>(dw);
    for (int x = 0; x < dw2; ++x) {
      const int sx = x * sw2 / dw2;
      out[x * 2] = row[sx * 2];
      out[x * 2 + 1] = row[sx * 2 + 1];
    }
  }
}

void rgb32_to_nv12(const uint8_t* bgra, LONG pitch, int w, int h, std::vector<uint8_t>& nv12) {
  nv12.resize(nv12_packed_bytes(w, h));
  uint8_t* y = nv12.data();
  uint8_t* uv = y + static_cast<size_t>(w) * static_cast<size_t>(h);
  for (int j = 0; j < h; ++j) {
    const uint8_t* row = bgra + static_cast<size_t>(j) * static_cast<size_t>(pitch);
    uint8_t* yrow = y + static_cast<size_t>(j) * static_cast<size_t>(w);
    for (int i = 0; i < w; ++i) {
      const int b = row[i * 4 + 0];
      const int g = row[i * 4 + 1];
      const int r = row[i * 4 + 2];
      const int yy = 16 + ((47 * r + 157 * g + 16 * b) >> 8);
      yrow[i] = static_cast<uint8_t>(std::clamp(yy, 16, 235));
    }
  }
  for (int j = 0; j < h; j += 2) {
    const uint8_t* row0 = bgra + static_cast<size_t>(j) * static_cast<size_t>(pitch);
    const uint8_t* row1 = bgra + static_cast<size_t>(j + 1) * static_cast<size_t>(pitch);
    uint8_t* uvrow = uv + static_cast<size_t>(j / 2) * static_cast<size_t>(w);
    for (int i = 0; i < w; i += 2) {
      const int b = (row0[i * 4] + row0[(i + 1) * 4] + row1[i * 4] + row1[(i + 1) * 4]) >> 2;
      const int g = (row0[i * 4 + 1] + row0[(i + 1) * 4 + 1] + row1[i * 4 + 1] + row1[(i + 1) * 4 + 1]) >> 2;
      const int r = (row0[i * 4 + 2] + row0[(i + 1) * 4 + 2] + row1[i * 4 + 2] + row1[(i + 1) * 4 + 2]) >> 2;
      const int cb = 128 + ((-26 * r - 87 * g + 112 * b) >> 8);
      const int cr = 128 + ((112 * r - 102 * g - 10 * b) >> 8);
      uvrow[i] = static_cast<uint8_t>(std::clamp(cb, 16, 240));
      uvrow[i + 1] = static_cast<uint8_t>(std::clamp(cr, 16, 240));
    }
  }
}

bool copy_rgb32(IMFSample* sample, int w, int h, std::vector<uint8_t>& packed) {
  IMFMediaBuffer* buf = nullptr;
  if (FAILED(sample->ConvertToContiguousBuffer(&buf)) || !buf) return false;
  const size_t need = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
  packed.resize(need);
  bool ok = false;
  IMF2DBuffer* buf2d = nullptr;
  if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&buf2d))) && buf2d) {
    BYTE* scan = nullptr;
    LONG pitch = 0;
    if (SUCCEEDED(buf2d->Lock2D(&scan, &pitch)) && scan) {
      const LONG abs_pitch = pitch < 0 ? -pitch : pitch;
      if (abs_pitch >= w * 4) {
        for (int y = 0; y < h; ++y) {
          std::memcpy(packed.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4u,
                      scan + static_cast<size_t>(y) * static_cast<size_t>(pitch),
                      static_cast<size_t>(w) * 4u);
        }
        ok = true;
      }
      buf2d->Unlock2D();
    }
    buf2d->Release();
  }
  if (!ok) {
    BYTE* data = nullptr;
    DWORD max_len = 0;
    DWORD cur = 0;
    if (SUCCEEDED(buf->Lock(&data, &max_len, &cur)) && data && cur >= need) {
      if (cur == need) {
        std::memcpy(packed.data(), data, need);
      } else {
        const LONG pitch = static_cast<LONG>(cur / static_cast<DWORD>(h));
        for (int y = 0; y < h; ++y) {
          std::memcpy(packed.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4u,
                      data + static_cast<size_t>(y) * static_cast<size_t>(pitch),
                      static_cast<size_t>(w) * 4u);
        }
      }
      buf->Unlock();
      ok = true;
    }
  }
  buf->Release();
  return ok;
}

void pack_nv12(const uint8_t* y, LONG y_pitch, const uint8_t* uv, LONG uv_pitch, int w, int h,
               std::vector<uint8_t>& out) {
  const size_t need = nv12_packed_bytes(w, h);
  out.resize(need);
  uint8_t* dst_y = out.data();
  for (int row = 0; row < h; ++row) {
    std::memcpy(dst_y + static_cast<size_t>(row) * static_cast<size_t>(w), y + static_cast<size_t>(row) * y_pitch,
                static_cast<size_t>(w));
  }
  uint8_t* dst_uv = dst_y + static_cast<size_t>(w) * static_cast<size_t>(h);
  const int h2 = h / 2;
  for (int row = 0; row < h2; ++row) {
    std::memcpy(dst_uv + static_cast<size_t>(row) * static_cast<size_t>(w), uv + static_cast<size_t>(row) * uv_pitch,
                static_cast<size_t>(w));
  }
}

bool copy_nv12(IMFSample* sample, int src_w, int src_h, std::vector<uint8_t>& packed) {
  IMFMediaBuffer* buf = nullptr;
  if (FAILED(sample->ConvertToContiguousBuffer(&buf)) || !buf) return false;
  IMF2DBuffer* buf2d = nullptr;
  bool ok = false;
  if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&buf2d))) && buf2d) {
    BYTE* scan = nullptr;
    LONG pitch = 0;
    if (SUCCEEDED(buf2d->Lock2D(&scan, &pitch)) && scan && src_w > 0 && src_h > 0 && pitch >= src_w) {
      pack_nv12(scan, pitch, scan + pitch * src_h, pitch, src_w, src_h, packed);
      buf2d->Unlock2D();
      ok = packed.size() >= nv12_packed_bytes(src_w, src_h);
    }
    buf2d->Release();
  }
  if (!ok) {
    BYTE* data = nullptr;
    DWORD max_len = 0;
    DWORD cur = 0;
    if (SUCCEEDED(buf->Lock(&data, &max_len, &cur)) && data && cur >= nv12_packed_bytes(src_w, src_h)) {
      packed.assign(data, data + nv12_packed_bytes(src_w, src_h));
      buf->Unlock();
      ok = true;
    }
  }
  buf->Release();
  return ok;
}

void wait_file_pts(IClock* clock, std::atomic<bool>& running, double due_ms) {
  if (!clock) return;
  while (running) {
    const double left = due_ms - clock->now_ms();
    if (left <= 0.0) break;
    const int nap = left > 8.0 ? 4 : 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(nap));
  }
}

double media_type_fps(IMFMediaType* type, double fallback) {
  if (!type) return fallback;
  UINT32 num = 0;
  UINT32 den = 0;
  if (SUCCEEDED(MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den)) && num > 0 && den > 0) {
    return static_cast<double>(num) / static_cast<double>(den);
  }
  return fallback;
}

IMFDXGIDeviceManager* create_mf_dxgi_manager() {
  ID3D11Device* dev = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                 nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, nullptr);
  if (FAILED(hr) || !dev) return nullptr;
  ID3D10Multithread* mt = nullptr;
  if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&mt))) && mt) {
    mt->SetMultithreadProtected(TRUE);
    mt->Release();
  }
  UINT token = 0;
  IMFDXGIDeviceManager* man = nullptr;
  hr = MFCreateDXGIDeviceManager(&token, &man);
  if (SUCCEEDED(hr) && man) hr = man->ResetDevice(dev, token);
  dev->Release();
  if (FAILED(hr) || !man) {
    if (man) man->Release();
    return nullptr;
  }
  return man;
}

bool rewind_reader(IMFSourceReader* reader) {
  PROPVARIANT pos{};
  pos.vt = VT_I8;
  pos.hVal.QuadPart = 0;
  return SUCCEEDED(reader->SetCurrentPosition(GUID_NULL, pos));
}

bool set_reader_type(IMFSourceReader* reader, const GUID& subtype, int w, int h) {
  IMFMediaType* out_type = nullptr;
  if (FAILED(MFCreateMediaType(&out_type)) || !out_type) return false;
  out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  out_type->SetGUID(MF_MT_SUBTYPE, subtype);
  if (w > 0 && h > 0) MFSetAttributeSize(out_type, MF_MT_FRAME_SIZE, static_cast<UINT32>(w), static_cast<UINT32>(h));
  MFSetAttributeRatio(out_type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  const HRESULT hr =
      reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, out_type);
  out_type->Release();
  return SUCCEEDED(hr);
}

}  // namespace

bool VirtualCapturePin::start(const std::string& device_name, const CaptureStartOptions& opt, FrameSlot& slot,
                              IClock& clock, std::string& err) {
  if (running_) {
    err = "already running";
    return false;
  }
  path_ = device_name;
  width_ = opt.width > 0 ? opt.width : 1920;
  height_ = opt.height > 0 ? opt.height : 1080;
  fps_ = virtual_pin_fps(opt.fps > 0 ? opt.fps : 60.0);
  slot_ = &slot;
  clock_ = &clock;
  thread_err_.clear();
  finished_ = false;
  have_frame_ = false;
  primed_ = false;
  playhead_on_ = false;
  frames_ = 0;
  clip_n_ = 0;
  decoded_next_ = 0;
  native_fps_ = 60.0;
  t0_ms_ = 0;
  last_nv12_ = FramePayload{};
  const int ring_n = virtual_pin_ring_frames_for(fps_, width_, height_);
  ring_.clear();
  ring_.resize(static_cast<size_t>(ring_n));
  if (path_.empty()) {
    err = "video vazio";
    return false;
  }
  std::error_code ec;
  if (!std::filesystem::exists(path_, ec)) {
    err = "video nao encontrado: " + file_tail(path_);
    return false;
  }
  opened_ = std::string("file:") + file_tail(path_);
  running_ = true;
  decode_th_ = std::thread([this] { decode_main(); });
  const auto wait0 = std::chrono::steady_clock::now();
  while (!primed_ && !finished_ && running_) {
    if (std::chrono::steady_clock::now() - wait0 > std::chrono::seconds(20)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!primed_) {
    running_ = false;
    if (decode_th_.joinable()) decode_th_.join();
    err = thread_err_.empty() ? "pino virtual sem frames (tela ficaria branca)" : thread_err_;
    finished_ = true;
    return false;
  }
  if (!opt.hold_playhead) {
    t0_ms_ = clock_ ? clock_->now_ms() : 0;
    playhead_on_ = true;
  }
  pub_th_ = std::thread([this] { publish_main(); });
  const auto pub0 = std::chrono::steady_clock::now();
  while (!have_frame_ && !finished_ && running_) {
    if (std::chrono::steady_clock::now() - pub0 > std::chrono::seconds(3)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!have_frame_) {
    running_ = false;
    if (pub_th_.joinable()) pub_th_.join();
    if (decode_th_.joinable()) decode_th_.join();
    err = thread_err_.empty() ? "pino virtual nao publicou" : thread_err_;
    finished_ = true;
    return false;
  }
  return true;
}

void VirtualCapturePin::stop() {
  running_ = false;
  if (pub_th_.joinable()) pub_th_.join();
  if (decode_th_.joinable()) decode_th_.join();
}

bool VirtualCapturePin::copy_ring_locked(uint64_t clip_i, FramePayload& out) {
  if (ring_.empty()) return false;
  const RingSlot& s = ring_[clip_i % ring_.size()];
  if (s.clip_index != clip_i || s.nv12.empty() || s.width <= 0) return false;
  out.nv12 = s.nv12;
  out.width = s.width;
  out.height = s.height;
  return true;
}

void VirtualCapturePin::decode_main() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool com_ok = SUCCEEDED(co) || co == RPC_E_CHANGED_MODE;
  if (!com_ok) {
    thread_err_ = "CoInitializeEx falhou";
    finished_ = true;
    return;
  }
  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
    thread_err_ = "MFStartup falhou";
    finished_ = true;
    if (SUCCEEDED(co)) CoUninitialize();
    return;
  }

  IMFAttributes* attrs = nullptr;
  IMFSourceReader* reader = nullptr;
  IMFDXGIDeviceManager* dxgi_man = create_mf_dxgi_manager();
  HRESULT hr = MFCreateAttributes(&attrs, 8);
  if (SUCCEEDED(hr) && attrs) {
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, FALSE);
    attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    if (dxgi_man) attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, dxgi_man);
    const std::wstring wpath = utf8_to_wide(path_);
    hr = MFCreateSourceReaderFromURL(wpath.c_str(), attrs, &reader);
    attrs->Release();
    attrs = nullptr;
  }
  if (FAILED(hr) || !reader) {
    thread_err_ = "Media Foundation nao abriu o video";
    finished_ = true;
    if (dxgi_man) dxgi_man->Release();
    MFShutdown();
    if (SUCCEEDED(co)) CoUninitialize();
    return;
  }

  reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);

  int native_w = 0;
  int native_h = 0;
  double native_fps = 30.0;
  IMFMediaType* native = nullptr;
  if (SUCCEEDED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &native)) &&
      native) {
    UINT32 nw = 0;
    UINT32 nh = 0;
    if (SUCCEEDED(MFGetAttributeSize(native, MF_MT_FRAME_SIZE, &nw, &nh)) && nw > 0 && nh > 0) {
      native_w = static_cast<int>(nw);
      native_h = static_cast<int>(nh);
    }
    native_fps = media_type_fps(native, native_fps);
    native->Release();
  }

  bool nv = set_reader_type(reader, MFVideoFormat_NV12, native_w, native_h) ||
            set_reader_type(reader, MFVideoFormat_NV12, 0, 0);
  bool rgb = false;
  if (!nv) {
    rgb = set_reader_type(reader, MFVideoFormat_RGB32, native_w, native_h) ||
          set_reader_type(reader, MFVideoFormat_RGB32, 0, 0);
  }
  if (!rgb && !nv) {
    thread_err_ = "sem conversao RGB32/NV12";
    reader->Release();
    finished_ = true;
    if (dxgi_man) dxgi_man->Release();
    MFShutdown();
    if (SUCCEEDED(co)) CoUninitialize();
    return;
  }

  GUID subtype = rgb ? MFVideoFormat_RGB32 : MFVideoFormat_NV12;
  IMFMediaType* cur = nullptr;
  if (SUCCEEDED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &cur)) && cur) {
    UINT32 nw = 0;
    UINT32 nh = 0;
    if (SUCCEEDED(MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &nw, &nh)) && nw > 0 && nh > 0) {
      native_w = static_cast<int>(nw);
      native_h = static_cast<int>(nh);
    }
    native_fps = media_type_fps(cur, native_fps);
    GUID got{};
    if (SUCCEEDED(cur->GetGUID(MF_MT_SUBTYPE, &got))) subtype = got;
    cur->Release();
  }
  if (native_w <= 0 || native_h <= 0) {
    thread_err_ = "video sem resolucao";
    reader->Release();
    finished_ = true;
    if (dxgi_man) dxgi_man->Release();
    MFShutdown();
    if (SUCCEEDED(co)) CoUninitialize();
    return;
  }
  rgb = (subtype == MFVideoFormat_RGB32);
  native_fps = file_cap_src_fps_or(native_fps);
  native_fps_ = native_fps;
  const int prime_n = virtual_pin_prime_frames(fps_);
  std::printf("virtual-pin: %s %dx%d -> %dx%d src@%.2f pin@%.0f ring=%zu (%s)%s\n", file_tail(path_).c_str(),
              native_w, native_h, width_, height_, native_fps, fps_, ring_.size(), rgb ? "rgb32" : "nv12",
              dxgi_man ? " d3d11" : "");

  std::vector<uint8_t> raw;
  std::vector<uint8_t> nv12_src;
  const size_t need = nv12_packed_bytes(width_, height_);
  uint64_t src_i = 0;

  while (running_ && !finished_) {
    const double t0_now = t0_ms_.load(std::memory_order_relaxed);
    if (primed_ && !playhead_on_.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }
    if (primed_ && playhead_on_.load(std::memory_order_relaxed) && !ring_.empty()) {
      const double now_dec = clock_ ? clock_->now_ms() : 0;
      const double clip_fps = native_fps_.load(std::memory_order_relaxed);
      const uint64_t play = virtual_pin_clip_index(now_dec, t0_now, clip_fps, clip_n_.load(std::memory_order_relaxed));
      if (virtual_pin_should_throttle_decode(src_i, play, clip_n_.load(std::memory_order_relaxed),
                                            static_cast<uint64_t>(ring_.size()))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
    }

    DWORD stream = 0;
    DWORD flags = 0;
    LONGLONG ts = 0;
    IMFSample* sample = nullptr;
    hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags, &ts,
                            &sample);
    if (FAILED(hr)) {
      if (thread_err_.empty()) thread_err_ = "ReadSample falhou";
      break;
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
      if (sample) sample->Release();
      if (src_i > 0) clip_n_ = src_i;
      int tries = 0;
      while (running_ && tries < 8 && !rewind_reader(reader)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        ++tries;
      }
      src_i = 0;
      continue;
    }
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
      IMFMediaType* changed = nullptr;
      if (SUCCEEDED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &changed)) &&
          changed) {
        UINT32 nw = 0;
        UINT32 nh = 0;
        if (SUCCEEDED(MFGetAttributeSize(changed, MF_MT_FRAME_SIZE, &nw, &nh)) && nw > 0 && nh > 0) {
          native_w = static_cast<int>(nw);
          native_h = static_cast<int>(nh);
        }
        GUID got{};
        if (SUCCEEDED(changed->GetGUID(MF_MT_SUBTYPE, &got))) {
          subtype = got;
          rgb = (subtype == MFVideoFormat_RGB32);
        }
        changed->Release();
      }
    }
    if (!sample) continue;

    bool got = false;
    if (rgb) {
      got = copy_rgb32(sample, native_w, native_h, raw);
      if (got) rgb32_to_nv12(raw.data(), native_w * 4, native_w, native_h, nv12_src);
    } else {
      got = copy_nv12(sample, native_w, native_h, nv12_src);
    }
    sample->Release();
    if (!got) continue;

    std::vector<uint8_t> dest(need);
    if (native_w != width_ || native_h != height_) {
      scale_nv12_nearest(nv12_src.data(), native_w, native_h, dest.data(), width_, height_);
    } else if (nv12_src.size() >= need) {
      std::memcpy(dest.data(), nv12_src.data(), need);
    } else {
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(ring_mu_);
      if (!ring_.empty()) {
        RingSlot& slot = ring_[src_i % ring_.size()];
        slot.clip_index = src_i;
        slot.width = width_;
        slot.height = height_;
        slot.nv12.swap(dest);
      }
      last_nv12_.nv12 = ring_.empty() ? dest : ring_[src_i % ring_.size()].nv12;
      last_nv12_.width = width_;
      last_nv12_.height = height_;
      last_nv12_.frame_index = src_i;
    }
    decoded_next_ = src_i + 1;
    if (!primed_ && static_cast<int>(src_i + 1) >= prime_n) primed_ = true;
    ++src_i;
  }

  reader->Release();
  if (dxgi_man) dxgi_man->Release();
  MFShutdown();
  if (SUCCEEDED(co)) CoUninitialize();
}

void VirtualCapturePin::publish_main() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  const double pin_fps = virtual_pin_fps(fps_);
  const double period = virtual_pin_period_ms(pin_fps);
  while (running_ && !finished_) {
    const bool playing = playhead_on_.load(std::memory_order_relaxed);
    const double t0 = t0_ms_.load(std::memory_order_relaxed);
    const double now = clock_ ? clock_->now_ms() : t0;
    const uint64_t nclip = clip_n_.load(std::memory_order_relaxed);
    const double clip_fps = native_fps_.load(std::memory_order_relaxed);
    const uint64_t pin_i = virtual_pin_held_src_index(playing, now, t0, pin_fps, 0);
    const uint64_t clip_i = virtual_pin_held_clip_index(playing, now, t0, clip_fps, nclip);

    FramePayload p;
    bool got = false;
    {
      std::lock_guard<std::mutex> lock(ring_mu_);
      got = copy_ring_locked(clip_i, p);
      if (!got) {
        const uint64_t take = virtual_pin_take_index(clip_i, decoded_next_.load(std::memory_order_relaxed));
        if (take != clip_i) got = copy_ring_locked(take, p);
      }
      if (!got && !last_nv12_.nv12.empty()) {
        p.nv12 = last_nv12_.nv12;
        p.width = last_nv12_.width;
        p.height = last_nv12_.height;
        got = p.width > 0 && !p.nv12.empty();
      }
    }
    if (got && slot_) {
      p.cap_qpc_ms = now;
      p.frame_index = pin_i;
      slot_->publish(std::move(p));
      have_frame_ = true;
      frames_ = pin_i + 1;
    }

    const double next = playing ? (t0 + static_cast<double>(pin_i + 1) * period) : (now + period);
    wait_file_pts(clock_, running_, next);
  }
}

}  // namespace nexuscap
