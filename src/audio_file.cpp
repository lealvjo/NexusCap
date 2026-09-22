#include "nexuscap/audio.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propidl.h>

#include <chrono>
#include <cstring>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace nexuscap {
namespace {

class ComInit {
public:
  ComInit() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
  ~ComInit() { CoUninitialize(); }
};

std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 0) return {};
  std::wstring w(static_cast<size_t>(n), 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  if (!w.empty() && w.back() == 0) w.pop_back();
  return w;
}

bool rewind_reader(IMFSourceReader* reader) {
  PROPVARIANT pos{};
  pos.vt = VT_I8;
  pos.hVal.QuadPart = 0;
  return SUCCEEDED(reader->SetCurrentPosition(GUID_NULL, pos));
}

bool append_sample_pcm(IMFSample* sample, std::vector<uint8_t>& q) {
  if (!sample) return false;
  IMFMediaBuffer* buf = nullptr;
  if (FAILED(sample->ConvertToContiguousBuffer(&buf)) || !buf) return false;
  BYTE* data = nullptr;
  DWORD max_len = 0, cur_len = 0;
  if (FAILED(buf->Lock(&data, &max_len, &cur_len)) || !data) {
    buf->Release();
    return false;
  }
  if (cur_len > 0) q.insert(q.end(), data, data + cur_len);
  buf->Unlock();
  buf->Release();
  return true;
}

bool open_audio_reader(const std::string& path, IMFSourceReader** out, std::string& why) {
  *out = nullptr;
  const std::wstring wpath = utf8_to_wide(path);
  if (wpath.empty()) {
    why = "path empty";
    return false;
  }
  IMFSourceReader* reader = nullptr;
  if (FAILED(MFCreateSourceReaderFromURL(wpath.c_str(), nullptr, &reader)) || !reader) {
    why = "MF open failed";
    return false;
  }
  reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  if (FAILED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE))) {
    reader->Release();
    why = "no audio stream";
    return false;
  }
  IMFMediaType* native = nullptr;
  const HRESULT hr = reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &native);
  if (FAILED(hr) || !native) {
    reader->Release();
    why = "no audio type";
    return false;
  }
  native->Release();
  *out = reader;
  return true;
}

}  // namespace

ClipAudioPlayer::~ClipAudioPlayer() { stop(); }

bool ClipAudioPlayer::start(const std::string& video_path, const std::string& sidecar_path, std::string& err) {
  if (running_) {
    err = "clip audio already running";
    return false;
  }
  playhead_on_ = false;
  seek0_ = false;
  ready_ = false;
  fail_.clear();
  running_ = true;
  th_ = std::thread([this, video_path, sidecar_path] { thread_main(video_path, sidecar_path); });
  for (int i = 0; i < 200 && running_ && !ready_ && fail_.empty(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!ready_) {
    err = fail_.empty() ? "clip audio init failed" : fail_;
    stop();
    return false;
  }
  return true;
}

void ClipAudioPlayer::stop() {
  running_ = false;
  if (th_.joinable()) th_.join();
  ready_ = false;
}

void ClipAudioPlayer::rebase_playhead() {
  seek0_.store(true, std::memory_order_relaxed);
  playhead_on_.store(true, std::memory_order_relaxed);
}

void ClipAudioPlayer::thread_main(std::string video_path, std::string sidecar_path) {
  ComInit com;
  if (FAILED(MFStartup(MF_VERSION))) {
    fail_ = "MFStartup failed";
    running_ = false;
    return;
  }

  IMFSourceReader* reader = nullptr;
  std::string why;
  if (!open_audio_reader(video_path, &reader, why) && !open_audio_reader(sidecar_path, &reader, why)) {
    fail_ = why.empty() ? "no clip audio" : why;
    MFShutdown();
    running_ = false;
    return;
  }

  IMMDeviceEnumerator* en = nullptr;
  IMMDevice* render_dev = nullptr;
  IAudioClient* client = nullptr;
  IAudioRenderClient* render = nullptr;
  WAVEFORMATEX* mix = nullptr;
  IMFMediaType* out_type = nullptr;
  bool ok = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en))) &&
            en && SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &render_dev)) && render_dev &&
            SUCCEEDED(render_dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(&client))) &&
            client && SUCCEEDED(client->GetMixFormat(&mix)) && mix;
  if (ok) {
    const UINT32 cb = sizeof(WAVEFORMATEX) + mix->cbSize;
    ok = SUCCEEDED(MFCreateMediaType(&out_type)) && out_type &&
         SUCCEEDED(MFInitMediaTypeFromWaveFormatEx(out_type, mix, cb)) &&
         SUCCEEDED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr,
                                               out_type));
  }
  if (out_type) out_type->Release();
  if (!ok) {
    fail_ = "WASAPI/MF format failed";
    if (mix) CoTaskMemFree(mix);
    if (client) client->Release();
    if (render_dev) render_dev->Release();
    if (en) en->Release();
    reader->Release();
    MFShutdown();
    running_ = false;
    return;
  }

  const REFERENCE_TIME buf100ns = 100000;
  if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, buf100ns, 0, mix, nullptr)) ||
      FAILED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render))) || !render) {
    fail_ = "WASAPI init failed";
    CoTaskMemFree(mix);
    client->Release();
    render_dev->Release();
    en->Release();
    reader->Release();
    MFShutdown();
    running_ = false;
    return;
  }

  UINT32 ren_buf = 0;
  client->GetBufferSize(&ren_buf);
  const UINT32 frame_bytes = mix->nBlockAlign ? mix->nBlockAlign : 4;
  client->Start();
  ready_ = true;

  std::vector<uint8_t> q;
  q.reserve(static_cast<size_t>(ren_buf) * frame_bytes * 4);

  while (running_) {
    if (seek0_.exchange(false, std::memory_order_relaxed)) {
      rewind_reader(reader);
      q.clear();
    }

    const bool play = playhead_on_.load(std::memory_order_relaxed);
    if (play) {
      while (q.size() < static_cast<size_t>(ren_buf) * frame_bytes) {
        DWORD flags = 0;
        IMFSample* sample = nullptr;
        HRESULT hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr,
                                        &flags, nullptr, &sample);
        if (FAILED(hr)) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
          rewind_reader(reader);
          if (sample) sample->Release();
          continue;
        }
        if (sample) {
          append_sample_pcm(sample, q);
          sample->Release();
        } else {
          break;
        }
      }
    }

    UINT32 pad = 0;
    if (FAILED(client->GetCurrentPadding(&pad))) break;
    const UINT32 avail = ren_buf > pad ? ren_buf - pad : 0;
    if (avail == 0) {
      Sleep(5);
      continue;
    }
    BYTE* out = nullptr;
    if (FAILED(render->GetBuffer(avail, &out)) || !out) {
      Sleep(5);
      continue;
    }
    const size_t need = static_cast<size_t>(avail) * frame_bytes;
    UINT32 gained = 0;
    if (!play || q.size() < need) {
      std::memset(out, 0, need);
      if (play && !q.empty()) {
        std::memcpy(out, q.data(), q.size());
        gained = frame_bytes > 0 ? static_cast<UINT32>(q.size() / frame_bytes) : 0;
        q.clear();
      }
    } else {
      std::memcpy(out, q.data(), need);
      q.erase(q.begin(), q.begin() + static_cast<std::ptrdiff_t>(need));
      gained = avail;
    }
    if (gained > 0) {
      apply_capture_volume_mix_pcm(out, gained, mix->nChannels, mix->wBitsPerSample,
                                  capture_volume_gain(volume_.load()));
    }
    render->ReleaseBuffer(avail, 0);
    Sleep(5);
  }

  client->Stop();
  render->Release();
  CoTaskMemFree(mix);
  client->Release();
  render_dev->Release();
  en->Release();
  reader->Release();
  MFShutdown();
  ready_ = false;
  running_ = false;
}

}  // namespace nexuscap
