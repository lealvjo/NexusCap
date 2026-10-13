#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "nexuscap/pipeline.hpp"

#include <d3d11.h>
#include <dxgi.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace nexuscap {
namespace {

HANDLE g_job = nullptr;

bool ensure_job() {
  if (g_job) return true;
  g_job = CreateJobObjectW(nullptr, nullptr);
  if (!g_job) return false;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
  li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof(li));
  return true;
}

std::wstring utf16(const std::string& s) {
  if (s.empty()) return L"";
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 1) return L"";
  std::wstring w(static_cast<std::size_t>(n - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}

void kill_process(HANDLE proc) {
  if (!proc) return;
  TerminateProcess(proc, 0);
  WaitForSingleObject(proc, 2000);
  CloseHandle(proc);
}

double qpc_ms() {
  static LARGE_INTEGER freq{};
  if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  if (freq.QuadPart <= 0) return 0;
  return 1000.0 * static_cast<double>(now.QuadPart) / static_cast<double>(freq.QuadPart);
}

void release_com(void*& p) {
  if (!p) return;
  static_cast<IUnknown*>(p)->Release();
  p = nullptr;
}

bool shared_handle_of(ID3D11Texture2D* tex, std::uint64_t* out) {
  if (!tex || !out) return false;
  IDXGIResource* res = nullptr;
  if (FAILED(tex->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&res))) || !res) {
    return false;
  }
  HANDLE h = nullptr;
  const HRESULT hr = res->GetSharedHandle(&h);
  res->Release();
  if (FAILED(hr) || !h) return false;
  *out = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(h));
  return true;
}

void copy_to_shared(ID3D11DeviceContext* ctx, ID3D11Texture2D* shared, ID3D11Texture2D* src) {
  D3D11_TEXTURE2D_DESC sd{}, dd{};
  src->GetDesc(&sd);
  shared->GetDesc(&dd);
  if (sd.Width == dd.Width && sd.Height == dd.Height) {
    ctx->CopyResource(shared, src);
    return;
  }
  ctx->CopySubresourceRegion(shared, 0, 0, 0, 0, src, 0, nullptr);
}

void copy_from_shared(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, ID3D11Texture2D* shared, int w, int h) {
  D3D11_TEXTURE2D_DESC sd{}, dd{};
  dst->GetDesc(&dd);
  shared->GetDesc(&sd);
  if (sd.Width == dd.Width && sd.Height == dd.Height) {
    ctx->CopyResource(dst, shared);
    return;
  }
  D3D11_BOX box{};
  box.right = static_cast<UINT>(w);
  box.bottom = static_cast<UINT>(h);
  box.back = 1;
  ctx->CopySubresourceRegion(dst, 0, 0, 0, 0, shared, 0, &box);
}

bool acquire_km(void* km, DWORD ms) {
  if (!km) return true;
  return SUCCEEDED(static_cast<IDXGIKeyedMutex*>(km)->AcquireSync(0, ms));
}

void release_km(void* km) {
  if (!km) return;
  static_cast<IDXGIKeyedMutex*>(km)->ReleaseSync(0);
}

void copy_bgra(std::uint8_t* dst, int dst_pitch, const std::uint8_t* src, int src_pitch, int w, int h) {
  const int row = w * 4;
  for (int y = 0; y < h; ++y) {
    std::memcpy(dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(dst_pitch),
                src + static_cast<std::size_t>(y) * static_cast<std::size_t>(src_pitch),
                static_cast<std::size_t>(row));
  }
}

}  // namespace

bool PipelineHost::running() const {
  HANDLE proc = static_cast<HANDLE>(process_);
  if (!proc) return false;
  return WaitForSingleObject(proc, 0) == WAIT_TIMEOUT;
}

bool PipelineHost::ensure_maps() {
  if (in_ && out_) return true;
  const DWORD bytes = static_cast<DWORD>(pipeline_fb_bytes());
  if (!in_map_) {
    in_map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, bytes, kPipelineFbInName);
  }
  if (!out_map_) {
    out_map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, bytes, kPipelineFbOutName);
  }
  if (!in_map_ || !out_map_) return false;
  if (!in_) {
    in_ = static_cast<PipelineFbHeader*>(MapViewOfFile(in_map_, FILE_MAP_ALL_ACCESS, 0, 0, bytes));
  }
  if (!out_) {
    out_ = static_cast<PipelineFbHeader*>(MapViewOfFile(out_map_, FILE_MAP_ALL_ACCESS, 0, 0, bytes));
  }
  return in_ && out_;
}

bool PipelineHost::ensure_staging(void* device, int w, int h) {
  auto* dev = static_cast<ID3D11Device*>(device);
  if (!dev || w < 1 || h < 1) return false;
  if (staging_ && stage_w_ == w && stage_h_ == h) return true;
  if (staging_) {
    static_cast<ID3D11Texture2D*>(staging_)->Release();
    staging_ = nullptr;
  }
  D3D11_TEXTURE2D_DESC td{};
  td.Width = static_cast<UINT>(w);
  td.Height = static_cast<UINT>(h);
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
  ID3D11Texture2D* st = nullptr;
  if (FAILED(dev->CreateTexture2D(&td, nullptr, &st))) return false;
  staging_ = st;
  stage_w_ = w;
  stage_h_ = h;
  return true;
}

bool PipelineHost::spawn() {
  const std::string wdir = pipeline_worker_dir(exe_dir_);
  const std::string exe = wdir + "\\" + kPipelineWorkerExe;
  const std::wstring wexe = utf16(exe);
  const std::wstring wcwd = utf16(wdir);
  if (wexe.empty() || GetFileAttributesW(wexe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  std::wstring cmd = L"\"" + wexe + L"\"";
  if (!CreateProcessW(wexe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      wcwd.c_str(), &si, &pi)) {
    return false;
  }
  if (ensure_job()) AssignProcessToJobObject(g_job, pi.hProcess);
  CloseHandle(pi.hThread);
  process_ = pi.hProcess;
  pid_ = pi.dwProcessId;
  return true;
}

bool PipelineHost::start(const std::string& exe_dir) {
  exe_dir_ = exe_dir;
  (void)ensure_maps();
  if (running()) return true;
  stop();
  exe_dir_ = exe_dir;
  return spawn();
}

bool PipelineHost::recycle() {
  if (exe_dir_.empty()) return false;
  (void)ensure_maps();
  if (awaiting_live_ && old_process_) {
    kill_process(static_cast<HANDLE>(old_process_));
    old_process_ = nullptr;
    old_pid_ = 0;
  }
  HANDLE cur = static_cast<HANDLE>(process_);
  const unsigned long cur_pid = pid_;
  process_ = nullptr;
  pid_ = 0;
  if (!spawn()) {
    process_ = cur;
    pid_ = cur_pid;
    return false;
  }
  old_process_ = cur;
  old_pid_ = cur_pid;
  awaiting_live_ = true;
  hold_min_frame_ = (in_ && in_->frame > 0) ? in_->frame + 2 : 2;
  last_unique_t_ = 0;
  last_out_frame_ = 0;
  took_new_out_ = false;
  return true;
}

bool PipelineHost::consume_new_out() {
  const bool v = took_new_out_;
  took_new_out_ = false;
  return v;
}

void PipelineHost::finish_recycle_if_live() {
  if (!awaiting_live_) return;
  const bool live = out_ && out_->magic == kPipelineFbMagic && out_->pid == pid_ &&
                    out_->frame >= hold_min_frame_;
  if (!live) {
    if (!running() && !exe_dir_.empty()) (void)spawn();
    return;
  }
  kill_process(static_cast<HANDLE>(old_process_));
  old_process_ = nullptr;
  old_pid_ = 0;
  awaiting_live_ = false;
}

void PipelineHost::stop() {
  kill_process(static_cast<HANDLE>(old_process_));
  old_process_ = nullptr;
  old_pid_ = 0;
  awaiting_live_ = false;
  have_unique_ft_ = false;
  last_unique_t_ = 0;
  last_out_frame_ = 0;
  took_new_out_ = false;
  unique_interval_ms_ = 0;
  unique_fps_ = 0;
  kill_process(static_cast<HANDLE>(process_));
  process_ = nullptr;
  pid_ = 0;
  if (staging_) {
    static_cast<ID3D11Texture2D*>(staging_)->Release();
    staging_ = nullptr;
  }
  stage_w_ = stage_h_ = 0;
}

bool PipelineHost::process_color(void* device, void* ctx, void* tex, int w, int h, std::uint32_t frame) {
  auto* dev = static_cast<ID3D11Device*>(device);
  auto* context = static_cast<ID3D11DeviceContext*>(ctx);
  auto* rt = static_cast<ID3D11Texture2D*>(tex);
  if (!dev || !context || !rt) return false;
  if (w < 1 || h < 1 || w > kPipelineFbMaxW || h > kPipelineFbMaxH) return false;
  if (!ensure_maps() || !ensure_staging(device, w, h) || !in_) return false;

  if (!running() && awaiting_live_ && !exe_dir_.empty()) (void)spawn();

  auto* staging = static_cast<ID3D11Texture2D*>(staging_);
  D3D11_MAPPED_SUBRESOURCE map{};
  if (running()) {
    context->CopyResource(staging, rt);
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &map))) return have_last_;
    copy_bgra(reinterpret_cast<std::uint8_t*>(pipeline_fb_pixels(in_)), w * 4,
              static_cast<const std::uint8_t*>(map.pData), static_cast<int>(map.RowPitch), w, h);
    context->Unmap(staging, 0);
    in_->magic = kPipelineFbMagic;
    in_->width = static_cast<std::uint32_t>(w);
    in_->height = static_cast<std::uint32_t>(h);
    in_->pass = 0;
    in_->frame = frame;
    in_->pid = static_cast<std::uint32_t>(pid_);
  }

  const bool live = running() && out_ && out_->magic == kPipelineFbMagic && out_->pid == pid_ &&
                    out_->frame >= hold_min_frame_ && static_cast<int>(out_->width) == w &&
                    static_cast<int>(out_->height) == h;
  if (live) {
    if (out_->frame != last_out_frame_) {
      last_bgra_.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
      copy_bgra(last_bgra_.data(), w * 4, reinterpret_cast<const std::uint8_t*>(pipeline_fb_pixels(out_)),
                w * 4, w, h);
      last_w_ = w;
      last_h_ = h;
      have_last_ = true;
      const double t = qpc_ms();
      if (last_unique_t_ > 0.0 && t > last_unique_t_) {
        const double dt = t - last_unique_t_;
        if (dt > 0.5 && dt < 250.0) {
          unique_interval_ms_ = dt;
          unique_fps_ = 1000.0 / dt;
          have_unique_ft_ = true;
          took_new_out_ = true;
        }
      }
      last_unique_t_ = t;
      last_out_frame_ = out_->frame;
    }
    finish_recycle_if_live();
  } else if (awaiting_live_) {
    finish_recycle_if_live();
  }
  if (!have_last_ || last_w_ != w || last_h_ != h) return live;

  if (FAILED(context->Map(staging, 0, D3D11_MAP_WRITE, 0, &map))) return false;
  copy_bgra(static_cast<std::uint8_t*>(map.pData), static_cast<int>(map.RowPitch), last_bgra_.data(), w * 4, w,
            h);
  context->Unmap(staging, 0);
  context->CopyResource(rt, staging);
  return true;
}

PipelineHost& pipeline_global() {
  static PipelineHost host;
  return host;
}

bool recycle_pipeline_worker(const std::string& exe_dir) {
  PipelineHost& host = pipeline_global();
  if (!host.running()) return host.start(exe_dir);
  return host.recycle();
}

}  // namespace nexuscap
