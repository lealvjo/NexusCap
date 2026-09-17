#include "nexuscap/audio.hpp"
#include "nexuscap/audio_select.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cstring>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

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
  std::wstring w(static_cast<size_t>(n), 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  if (!w.empty() && w.back() == 0) w.pop_back();
  return w;
}

IMMDevice* find_capture(IMMDeviceEnumerator* en, const std::wstring& prefer) {
  IMMDeviceCollection* col = nullptr;
  if (FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col)) || !col) return nullptr;
  UINT count = 0;
  col->GetCount(&count);
  IMMDevice* best = nullptr;
  int best_score = -1;
  for (UINT i = 0; i < count; ++i) {
    IMMDevice* d = nullptr;
    if (FAILED(col->Item(i, &d)) || !d) continue;
    IPropertyStore* store = nullptr;
    std::wstring name;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &store)) && store) {
      PROPVARIANT var;
      PropVariantInit(&var);
      if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &var)) && var.vt == VT_LPWSTR && var.pwszVal) {
        name = var.pwszVal;
      }
      PropVariantClear(&var);
      store->Release();
    }
    const int score = score_audio_capture_name(name, prefer);
    if (score > best_score) {
      if (best) best->Release();
      best = d;
      best_score = score;
    } else {
      d->Release();
    }
  }
  col->Release();
  return best;
}

}  // namespace

AudioMonitor::~AudioMonitor() { stop(); }

bool AudioMonitor::start(const std::string& prefer_name_substr, std::string& err) {
  if (running_) {
    err = "audio already running";
    return false;
  }
  running_ = true;
  th_ = std::thread([this, prefer_name_substr] { thread_main(prefer_name_substr); });
  (void)err;
  return true;
}

void AudioMonitor::stop() {
  running_ = false;
  if (th_.joinable()) th_.join();
}

void AudioMonitor::thread_main(std::string prefer) {
  ComInit com;
  IMMDeviceEnumerator* en = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&en))) ||
      !en) {
    running_ = false;
    return;
  }

  const std::wstring pref = utf8_to_wide(prefer);  // empty = any capture (HDMI preferred)
  IMMDevice* cap_dev = find_capture(en, pref);
  if (!cap_dev) {
    en->Release();
    running_ = false;
    return;
  }

  IMMDevice* render_dev = nullptr;
  en->GetDefaultAudioEndpoint(eRender, eConsole, &render_dev);
  en->Release();
  if (!render_dev) {
    cap_dev->Release();
    running_ = false;
    return;
  }

  IAudioClient* cap_client = nullptr;
  IAudioClient* ren_client = nullptr;
  if (FAILED(cap_dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&cap_client))) ||
      FAILED(render_dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(&ren_client)))) {
    if (cap_client) cap_client->Release();
    if (ren_client) ren_client->Release();
    cap_dev->Release();
    render_dev->Release();
    running_ = false;
    return;
  }

  WAVEFORMATEX* mix = nullptr;
  if (FAILED(cap_client->GetMixFormat(&mix)) || !mix) {
    cap_client->Release();
    ren_client->Release();
    cap_dev->Release();
    render_dev->Release();
    running_ = false;
    return;
  }

  const REFERENCE_TIME buf100ns = 100000;  // 10ms
  HRESULT hr_c = cap_client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, buf100ns, 0, mix, nullptr);
  HRESULT hr_r = ren_client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, buf100ns, 0, mix, nullptr);
  if (FAILED(hr_c) || FAILED(hr_r)) {
    CoTaskMemFree(mix);
    cap_client->Release();
    ren_client->Release();
    cap_dev->Release();
    render_dev->Release();
    running_ = false;
    return;
  }

  IAudioCaptureClient* acc = nullptr;
  IAudioRenderClient* arc = nullptr;
  cap_client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&acc));
  ren_client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&arc));
  UINT32 ren_buf = 0;
  ren_client->GetBufferSize(&ren_buf);

  cap_client->Start();
  ren_client->Start();

  const UINT32 frame_bytes = mix->nBlockAlign;
  while (running_) {
    Sleep(5);
    UINT32 packet = 0;
    if (FAILED(acc->GetNextPacketSize(&packet))) break;
    while (packet > 0) {
      BYTE* data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      if (FAILED(acc->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

      UINT32 pad = 0;
      ren_client->GetCurrentPadding(&pad);
      const UINT32 avail = ren_buf - pad;
      const UINT32 to_write = frames < avail ? frames : avail;
      if (to_write > 0) {
        BYTE* out = nullptr;
        if (SUCCEEDED(arc->GetBuffer(to_write, &out)) && out) {
          if (muted_ || (flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
            std::memset(out, 0, static_cast<size_t>(to_write) * frame_bytes);
          } else {
            std::memcpy(out, data, static_cast<size_t>(to_write) * frame_bytes);
          }
          arc->ReleaseBuffer(to_write, 0);
        }
      }
      acc->ReleaseBuffer(frames);
      if (FAILED(acc->GetNextPacketSize(&packet))) break;
    }
  }

  cap_client->Stop();
  ren_client->Stop();
  if (acc) acc->Release();
  if (arc) arc->Release();
  CoTaskMemFree(mix);
  cap_client->Release();
  ren_client->Release();
  cap_dev->Release();
  render_dev->Release();
  running_ = false;
}

}  // namespace nexuscap
