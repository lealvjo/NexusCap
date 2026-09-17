#include "nexuscap/capture.hpp"
#include "nexuscap/audio_select.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <dshow.h>
#include <dvdmedia.h>

#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

// Sample Grabber interfaces (qedit.h removed from modern SDKs).
struct __declspec(uuid("0579154A-2B53-4994-B0D0-E773148EFF85")) ISampleGrabberCB : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SampleCB(double SampleTime, IMediaSample* pSample) = 0;
  virtual HRESULT STDMETHODCALLTYPE BufferCB(double SampleTime, BYTE* pBuffer, long BufferLen) = 0;
};

struct __declspec(uuid("6B652FFF-11FE-4fce-92AD-0266B5D7C78F")) ISampleGrabber : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL OneShot) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* pType) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* pType) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL BufferThem) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* pBufferSize, long* pBuffer) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** ppSample) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* pCallback, long WhichMethodToCallback) = 0;
};

static const CLSID CLSID_SampleGrabber = {0xC1F400A0, 0x3F08, 0x11d3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
static const CLSID CLSID_NullRenderer = {0xC1F400A4, 0x3F08, 0x11d3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
// Some cards report 'I420' instead of MEDIASUBTYPE_IYUV (same planar layout).
static const GUID kMediasubtypeI420 = {0x30323449, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

namespace nexuscap {
namespace {

int fourcc_from_subtype(const GUID& g) {
  if (g == MEDIASUBTYPE_NV12) return static_cast<int>(VideoFourcc::Nv12);
  if (g == MEDIASUBTYPE_IYUV || g == kMediasubtypeI420) return static_cast<int>(VideoFourcc::I420);
  if (g == MEDIASUBTYPE_YUY2) return static_cast<int>(VideoFourcc::Yuy2);
  if (g == MEDIASUBTYPE_MJPG) return static_cast<int>(VideoFourcc::Mjpeg);
  return -1;
}

bool subtype_matches_fourcc(const GUID& g, int fourcc) {
  const int f = clamp_video_fourcc(fourcc);
  if (f == static_cast<int>(VideoFourcc::Nv12)) return g == MEDIASUBTYPE_NV12;
  if (f == static_cast<int>(VideoFourcc::I420)) return g == MEDIASUBTYPE_IYUV || g == kMediasubtypeI420;
  if (f == static_cast<int>(VideoFourcc::Yuy2)) return g == MEDIASUBTYPE_YUY2;
  if (f == static_cast<int>(VideoFourcc::Mjpeg)) return g == MEDIASUBTYPE_MJPG;
  return false;
}

GUID primary_subtype_for(int fourcc) {
  switch (clamp_video_fourcc(fourcc)) {
    case static_cast<int>(VideoFourcc::I420):
      return MEDIASUBTYPE_IYUV;
    case static_cast<int>(VideoFourcc::Yuy2):
      return MEDIASUBTYPE_YUY2;
    case static_cast<int>(VideoFourcc::Mjpeg):
      return MEDIASUBTYPE_MJPG;
    default:
      return MEDIASUBTYPE_NV12;
  }
}

void free_media_type(AM_MEDIA_TYPE& mt) {
  if (mt.cbFormat) CoTaskMemFree(mt.pbFormat);
  if (mt.pUnk) mt.pUnk->Release();
  ZeroMemory(&mt, sizeof(mt));
}

bool name_contains(IMoniker* mon, const std::string& needle) {
  IPropertyBag* prop = nullptr;
  if (FAILED(mon->BindToStorage(0, 0, IID_IPropertyBag, reinterpret_cast<void**>(&prop))) || !prop) {
    return false;
  }
  VARIANT var;
  VariantInit(&var);
  bool ok = false;
  if (SUCCEEDED(prop->Read(L"FriendlyName", &var, 0)) && var.vt == VT_BSTR && var.bstrVal) {
    char narrow[512];
    WideCharToMultiByte(CP_UTF8, 0, var.bstrVal, -1, narrow, sizeof(narrow), nullptr, nullptr);
    ok = std::string(narrow).find(needle) != std::string::npos;
  }
  VariantClear(&var);
  prop->Release();
  return ok;
}

std::string moniker_friendly_name(IMoniker* mon) {
  IPropertyBag* prop = nullptr;
  if (FAILED(mon->BindToStorage(0, 0, IID_IPropertyBag, reinterpret_cast<void**>(&prop))) || !prop) {
    return {};
  }
  VARIANT var;
  VariantInit(&var);
  std::string name;
  if (SUCCEEDED(prop->Read(L"FriendlyName", &var, 0)) && var.vt == VT_BSTR && var.bstrVal) {
    char narrow[512];
    WideCharToMultiByte(CP_UTF8, 0, var.bstrVal, -1, narrow, sizeof(narrow), nullptr, nullptr);
    name = narrow;
  }
  VariantClear(&var);
  prop->Release();
  return name;
}

class GrabCB final : public ISampleGrabberCB {
public:
  explicit GrabCB(DShowCapture* owner) : owner_(owner) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (riid == IID_IUnknown || riid == __uuidof(ISampleGrabberCB)) {
      *ppv = static_cast<ISampleGrabberCB*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = --ref_;
    if (!r) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE BufferCB(double, BYTE* pBuffer, long BufferLen) override {
    if (!owner_ || !pBuffer || BufferLen <= 0) return S_OK;
    owner_->push_buffer(pBuffer, static_cast<size_t>(BufferLen));
    return S_OK;
  }

private:
  DShowCapture* owner_;
  std::atomic<ULONG> ref_{1};
};

}  // namespace

struct DShowCapture::Impl {
  IGraphBuilder* graph = nullptr;
  ICaptureGraphBuilder2* builder = nullptr;
  IMediaControl* control = nullptr;
  IBaseFilter* source = nullptr;
  IBaseFilter* grabber_f = nullptr;
  IBaseFilter* null_f = nullptr;
  ISampleGrabber* grabber = nullptr;
  GrabCB* cb = nullptr;
  int width = 0;
  int height = 0;
};

void DShowCapture::push_buffer(const uint8_t* data, size_t len) {
  if (!running_ || !slot_ || !clock_ || !impl_) return;
  const int w = impl_->width;
  const int h = impl_->height;
  const size_t need = static_cast<size_t>(w) * static_cast<size_t>(h) * 3 / 2;
  if (len < need) return;
  if (nv12_scratch_.size() != need) {
    slot_->take_recycle_nv12(nv12_scratch_);
    if (nv12_scratch_.size() != need) nv12_scratch_.resize(need);
  }
  std::memcpy(nv12_scratch_.data(), data, need);
  FramePayload p;
  p.width = w;
  p.height = h;
  p.cap_qpc_ms = clock_->now_ms();
  p.frame_index = frames_.fetch_add(1);
  p.nv12.swap(nv12_scratch_);  // publish owns buffer; scratch empty until recycle/resize
  slot_->publish(std::move(p));
}

DShowCapture::DShowCapture() : impl_(new Impl) {}
DShowCapture::~DShowCapture() {
  stop();
  delete impl_;
  impl_ = nullptr;
}

bool DShowCapture::start(const std::string& device_name, const CaptureStartOptions& opt, FrameSlot& slot,
                         IClock& clock, std::string& err) {
  stop();
  slot_ = &slot;
  clock_ = &clock;
  const int width = opt.width;
  const int height = opt.height;
  const double fps = opt.fps;
  const int want_saved = clamp_video_fourcc(opt.video_fourcc);
  // Always try NV12 first — this card's MJPEG/YUY2/I420 often SetFormat OK then RenderStream→NV12 grabber fails.
  std::vector<int> try_fourccs;
  try_fourccs.push_back(static_cast<int>(VideoFourcc::Nv12));
  if (want_saved != static_cast<int>(VideoFourcc::Nv12) &&
      want_saved != static_cast<int>(VideoFourcc::Any)) {
    try_fourccs.push_back(want_saved);
  }
  if (want_saved == static_cast<int>(VideoFourcc::Any)) {
    for (int fc : {static_cast<int>(VideoFourcc::I420), static_cast<int>(VideoFourcc::Yuy2),
                   static_cast<int>(VideoFourcc::Mjpeg)}) {
      try_fourccs.push_back(fc);
    }
  }
  opened_fourcc_ = try_fourccs.front();
  impl_->width = width;
  impl_->height = height;

  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool com_init = SUCCEEDED(hr) || hr == S_FALSE || hr == RPC_E_CHANGED_MODE;

  hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_IGraphBuilder,
                        reinterpret_cast<void**>(&impl_->graph));
  if (FAILED(hr)) {
    err = "CoCreate FilterGraph failed";
    return false;
  }
  hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_ICaptureGraphBuilder2,
                        reinterpret_cast<void**>(&impl_->builder));
  if (FAILED(hr)) {
    err = "CoCreate CaptureGraphBuilder2 failed";
    return false;
  }
  impl_->builder->SetFiltergraph(impl_->graph);

  ICreateDevEnum* dev_enum = nullptr;
  hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum,
                        reinterpret_cast<void**>(&dev_enum));
  if (FAILED(hr)) {
    err = "SystemDeviceEnum failed";
    return false;
  }
  IEnumMoniker* enum_mon = nullptr;
  hr = dev_enum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enum_mon, 0);
  dev_enum->Release();
  if (hr != S_OK || !enum_mon) {
    err = "no video capture devices";
    return false;
  }

  IMoniker* mon = nullptr;
  IMoniker* chosen = nullptr;
  const bool auto_pick = is_auto_device(device_name);
  while (enum_mon->Next(1, &mon, nullptr) == S_OK) {
    if (auto_pick || name_contains(mon, device_name)) {
      chosen = mon;
      break;
    }
    mon->Release();
  }
  enum_mon->Release();
  if (!chosen) {
    err = auto_pick ? "no video capture devices" : ("device not found: " + device_name);
    return false;
  }
  opened_ = moniker_friendly_name(chosen);
  if (opened_.empty()) opened_ = auto_pick ? "auto" : device_name;
  hr = chosen->BindToObject(0, 0, IID_IBaseFilter, reinterpret_cast<void**>(&impl_->source));
  chosen->Release();
  if (FAILED(hr)) {
    err = "BindToObject source failed";
    return false;
  }
  impl_->graph->AddFilter(impl_->source, L"Source");

  hr = CoCreateInstance(CLSID_SampleGrabber, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter,
                        reinterpret_cast<void**>(&impl_->grabber_f));
  if (FAILED(hr)) {
    err = "SampleGrabber create failed (qedit/grabber missing?)";
    return false;
  }
  impl_->graph->AddFilter(impl_->grabber_f, L"Grabber");
  impl_->grabber_f->QueryInterface(__uuidof(ISampleGrabber), reinterpret_cast<void**>(&impl_->grabber));

  AM_MEDIA_TYPE mt{};
  mt.majortype = MEDIATYPE_Video;
  // Decoder path is NV12; DShow inserts a converter when the pin is YUY2.
  mt.subtype = MEDIASUBTYPE_NV12;
  mt.formattype = FORMAT_VideoInfo;
  impl_->grabber->SetMediaType(&mt);
  impl_->grabber->SetBufferSamples(FALSE);
  impl_->grabber->SetOneShot(FALSE);
  impl_->cb = new GrabCB(this);
  impl_->grabber->SetCallback(impl_->cb, 1);  // BufferCB

  hr = CoCreateInstance(CLSID_NullRenderer, nullptr, CLSCTX_INPROC_SERVER, IID_IBaseFilter,
                        reinterpret_cast<void**>(&impl_->null_f));
  if (FAILED(hr)) {
    err = "NullRenderer create failed";
    return false;
  }
  impl_->graph->AddFilter(impl_->null_f, L"Null");

  // Prefer exact format via IAMStreamConfig on capture pin (OBS-style negotiation).
  IAMStreamConfig* cfg = nullptr;
  hr = impl_->builder->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, impl_->source,
                                     IID_IAMStreamConfig, reinterpret_cast<void**>(&cfg));
  if (SUCCEEDED(hr) && cfg) {
    int count = 0, size = 0;
    cfg->GetNumberOfCapabilities(&count, &size);
    std::vector<BYTE> caps(static_cast<size_t>(size));
    bool set_ok = false;
    auto try_match_fourcc = [&](int fourcc) -> bool {
      for (int i = 0; i < count; ++i) {
        AM_MEDIA_TYPE* pmt = nullptr;
        if (FAILED(cfg->GetStreamCaps(i, &pmt, caps.data())) || !pmt) continue;
        bool match = false;
        if (pmt->formattype == FORMAT_VideoInfo && pmt->pbFormat) {
          auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(pmt->pbFormat);
          const int w = vih->bmiHeader.biWidth;
          const int h = std::abs(vih->bmiHeader.biHeight);
          const double f = vih->AvgTimePerFrame > 0 ? 10000000.0 / static_cast<double>(vih->AvgTimePerFrame) : 0;
          if (subtype_matches_fourcc(pmt->subtype, fourcc) && w == width && h == height &&
              std::abs(f - fps) < 1.0)
            match = true;
        } else if (pmt->formattype == FORMAT_VideoInfo2 && pmt->pbFormat) {
          auto* vih = reinterpret_cast<VIDEOINFOHEADER2*>(pmt->pbFormat);
          const int w = vih->bmiHeader.biWidth;
          const int h = std::abs(vih->bmiHeader.biHeight);
          const double f = vih->AvgTimePerFrame > 0 ? 10000000.0 / static_cast<double>(vih->AvgTimePerFrame) : 0;
          if (subtype_matches_fourcc(pmt->subtype, fourcc) && w == width && h == height &&
              std::abs(f - fps) < 1.0)
            match = true;
        }
        bool ok = false;
        if (match) ok = SUCCEEDED(cfg->SetFormat(pmt));
        free_media_type(*pmt);
        CoTaskMemFree(pmt);
        if (ok) return true;
      }
      // I420: also try alternate GUID if primary pin scan used IYUV-only path above already covers both.
      (void)primary_subtype_for(fourcc);
      return false;
    };
    for (int fc : try_fourccs) {
      if (!try_match_fourcc(fc)) continue;
      set_ok = true;
      opened_fourcc_ = fc;
      if (want_saved != static_cast<int>(VideoFourcc::Any) && fc != want_saved) {
        err = "formato pedido indisponivel; usando ";
        err += video_fourcc_name(fc);
      }
      break;
    }

    IAMBufferNegotiation* nego = nullptr;
    if (SUCCEEDED(impl_->builder->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, impl_->source,
                                                IID_IAMBufferNegotiation, reinterpret_cast<void**>(&nego))) &&
        nego) {
      ALLOCATOR_PROPERTIES props{};
      props.cBuffers = capture_buffer_count(opt.capture_buffering);
      props.cbBuffer = 0;
      props.cbAlign = 0;
      props.cbPrefix = 0;
      nego->SuggestAllocatorProperties(&props);
      nego->Release();
    }
    cfg->Release();
    if (!set_ok) {
      // Still RenderStream — default pin caps may match request (e.g. 1080p60).
      // Wrong size is rejected after GetConnectedMediaType below.
      err = "SetFormat falhou; tentando negociacao padrao";
    }
  }

  hr = impl_->builder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, impl_->source, impl_->grabber_f,
                                    impl_->null_f);
  if (FAILED(hr)) {
    const unsigned long hrx = static_cast<unsigned long>(hr);
    const int was_fc = opened_fourcc_;
    stop();
    if (was_fc != static_cast<int>(VideoFourcc::Nv12)) {
      CaptureStartOptions nv = opt;
      nv.video_fourcc = static_cast<int>(VideoFourcc::Nv12);
      std::string err2;
      if (start(device_name, nv, slot, clock, err2)) {
        err = "RenderStream falhou p/ " + std::string(video_fourcc_name(was_fc)) + "; usando NV12";
        return true;
      }
      err = "RenderStream failed hr=0x" + std::to_string(hrx) + " | NV12: " + err2;
      return false;
    }
    err = "RenderStream failed hr=0x" + std::to_string(hrx);
    return false;
  }

  // Confirm connected type (even width/height for NV12 UV).
  AM_MEDIA_TYPE connected{};
  if (SUCCEEDED(impl_->grabber->GetConnectedMediaType(&connected))) {
    if (connected.formattype == FORMAT_VideoInfo && connected.pbFormat) {
      auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(connected.pbFormat);
      int cw = vih->bmiHeader.biWidth;
      int ch = std::abs(vih->bmiHeader.biHeight);
      if (cw > 0) cw &= ~1;
      if (ch > 0) ch &= ~1;
      if (cw > 0 && ch > 0) {
        impl_->width = cw;
        impl_->height = ch;
      }
    }
    free_media_type(connected);
  }

  // Reject silent downscale (4K60 NV12 → 1080p) — that looked like "OK" then white/wrong session.
  if (std::abs(impl_->width - width) > 16 || std::abs(impl_->height - height) > 16) {
    err = "negociou " + std::to_string(impl_->width) + "x" + std::to_string(impl_->height) +
          " em vez de " + std::to_string(width) + "x" + std::to_string(height);
    stop();
    return false;
  }

  impl_->graph->QueryInterface(IID_IMediaControl, reinterpret_cast<void**>(&impl_->control));
  hr = impl_->control->Run();
  if (FAILED(hr)) {
    err = "MediaControl::Run failed";
    stop();
    return false;
  }
  running_ = true;
  frames_ = 0;
  // White-screen guard: mode churn / 4K can Run() with delayed or zero NV12 samples.
  {
    const int wait_ms = capture_first_frame_wait_ms(impl_->width, impl_->height);
    const DWORD t0 = GetTickCount();
    while (frames_.load(std::memory_order_relaxed) == 0 &&
           (GetTickCount() - t0) < static_cast<DWORD>(wait_ms)) {
      Sleep(10);
    }
    if (frames_.load(std::memory_order_relaxed) == 0) {
      const std::string detail =
          "nenhum frame apos Run (troca de modo / placa ocupada / formato sem NV12)";
      stop();
      // MJPEG@4K often negotiates then never produces NV12 — retry NV12 once.
      if (want_saved != static_cast<int>(VideoFourcc::Nv12)) {
        CaptureStartOptions nv = opt;
        nv.video_fourcc = static_cast<int>(VideoFourcc::Nv12);
        std::string err2;
        if (start(device_name, nv, slot, clock, err2)) {
          err = "formato " + std::string(video_fourcc_name(want_saved)) +
                " sem frames; usando NV12";
          return true;
        }
        err = detail + " | NV12: " + err2;
        return false;
      }
      err = detail;
      return false;
    }
  }
  err.clear();
  (void)com_init;
  (void)fps;
  return true;
}

int DShowCapture::opened_width() const { return impl_ ? impl_->width : 0; }
int DShowCapture::opened_height() const { return impl_ ? impl_->height : 0; }

void DShowCapture::stop() {
  running_ = false;
  if (!impl_) return;
  if (impl_->control) {
    impl_->control->Stop();
    // Let the driver finish tearing down the previous pin format before Release.
    Sleep(static_cast<DWORD>(capture_device_settle_ms()));
    impl_->control->Release();
    impl_->control = nullptr;
  }
  if (impl_->grabber) {
    impl_->grabber->SetCallback(nullptr, 0);
    impl_->grabber->Release();
    impl_->grabber = nullptr;
  }
  if (impl_->cb) {
    impl_->cb->Release();
    impl_->cb = nullptr;
  }
  auto rel = [](IBaseFilter*& f) {
    if (f) {
      f->Release();
      f = nullptr;
    }
  };
  rel(impl_->null_f);
  rel(impl_->grabber_f);
  rel(impl_->source);
  if (impl_->builder) {
    impl_->builder->Release();
    impl_->builder = nullptr;
  }
  if (impl_->graph) {
    impl_->graph->Release();
    impl_->graph = nullptr;
  }
  impl_->width = 0;
  impl_->height = 0;
  nv12_scratch_.clear();
}

std::vector<std::string> list_video_capture_devices() {
  std::vector<std::string> out;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ICreateDevEnum* de = nullptr;
  if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum,
                              reinterpret_cast<void**>(&de))) ||
      !de) {
    return out;
  }
  IEnumMoniker* en = nullptr;
  if (FAILED(de->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &en, 0)) || !en) {
    de->Release();
    return out;
  }
  IMoniker* mon = nullptr;
  while (en->Next(1, &mon, nullptr) == S_OK) {
    const std::string name = moniker_friendly_name(mon);
    if (!name.empty()) out.push_back(name);
    mon->Release();
  }
  en->Release();
  de->Release();
  return out;
}

namespace {

IMoniker* find_video_moniker(const std::string& device_name) {
  ICreateDevEnum* de = nullptr;
  if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum,
                              reinterpret_cast<void**>(&de))) ||
      !de) {
    return nullptr;
  }
  IEnumMoniker* en = nullptr;
  if (FAILED(de->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &en, 0)) || !en) {
    de->Release();
    return nullptr;
  }
  const bool auto_pick = is_auto_device(device_name);
  IMoniker* found = nullptr;
  IMoniker* mon = nullptr;
  while (en->Next(1, &mon, nullptr) == S_OK) {
    if (auto_pick || name_contains(mon, device_name) || moniker_friendly_name(mon) == device_name) {
      found = mon;  // take ownership of this ref
      break;
    }
    mon->Release();
  }
  en->Release();
  de->Release();
  return found;
}

double fps_from_avg_time(REFERENCE_TIME t) {
  if (t <= 0) return 0;
  return 10000000.0 / static_cast<double>(t);
}

}  // namespace

std::vector<CaptureMode> list_video_capture_modes(const std::string& device_name) {
  std::vector<CaptureMode> out;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMoniker* mon = find_video_moniker(device_name.empty() ? "auto" : device_name);
  if (!mon) return out;

  IBaseFilter* filter = nullptr;
  if (FAILED(mon->BindToObject(0, 0, IID_IBaseFilter, reinterpret_cast<void**>(&filter))) || !filter) {
    mon->Release();
    return out;
  }
  mon->Release();

  IEnumPins* pins = nullptr;
  if (FAILED(filter->EnumPins(&pins)) || !pins) {
    filter->Release();
    return out;
  }

  IPin* pin = nullptr;
  while (pins->Next(1, &pin, nullptr) == S_OK) {
    PIN_DIRECTION dir = PINDIR_INPUT;
    pin->QueryDirection(&dir);
    if (dir != PINDIR_OUTPUT) {
      pin->Release();
      continue;
    }
    IAMStreamConfig* cfg = nullptr;
    if (FAILED(pin->QueryInterface(IID_IAMStreamConfig, reinterpret_cast<void**>(&cfg))) || !cfg) {
      pin->Release();
      continue;
    }
    int count = 0, size = 0;
    if (SUCCEEDED(cfg->GetNumberOfCapabilities(&count, &size)) && count > 0 && size > 0) {
      std::vector<BYTE> caps(static_cast<size_t>(size));
      for (int i = 0; i < count; ++i) {
        AM_MEDIA_TYPE* mt = nullptr;
        if (FAILED(cfg->GetStreamCaps(i, &mt, caps.data())) || !mt) continue;
        int w = 0, h = 0;
        double fps = 0;
        if (mt->formattype == FORMAT_VideoInfo && mt->cbFormat >= sizeof(VIDEOINFOHEADER) && mt->pbFormat) {
          auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(mt->pbFormat);
          w = vih->bmiHeader.biWidth;
          h = std::abs(vih->bmiHeader.biHeight);
          fps = fps_from_avg_time(vih->AvgTimePerFrame);
        } else if (mt->formattype == FORMAT_VideoInfo2 && mt->cbFormat >= sizeof(VIDEOINFOHEADER2) &&
                   mt->pbFormat) {
          auto* vih = reinterpret_cast<VIDEOINFOHEADER2*>(mt->pbFormat);
          w = vih->bmiHeader.biWidth;
          h = std::abs(vih->bmiHeader.biHeight);
          fps = fps_from_avg_time(vih->AvgTimePerFrame);
        }
        free_media_type(*mt);
        CoTaskMemFree(mt);
        if (w < 160 || h < 120) continue;
        if (fps < 1.0) fps = 60.0;
        // Round common rates
        if (fps > 59.0 && fps < 61.0) fps = 60.0;
        else if (fps > 29.0 && fps < 31.0) fps = 30.0;
        else if (fps > 119.0 && fps < 121.0) fps = 120.0;
        else if (fps > 49.0 && fps < 51.0) fps = 50.0;
        else fps = std::round(fps);

        CaptureMode m;
        m.width = w;
        m.height = h;
        m.fps = fps;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%dx%d @ %.0f", w, h, fps);
        m.label = buf;
        out.push_back(m);
      }
    }
    cfg->Release();
    pin->Release();
    break;  // first output pin with StreamConfig is enough
  }
  pins->Release();
  filter->Release();

  // Unique by w/h/fps, sort descending by pixels then fps.
  std::sort(out.begin(), out.end(), [](const CaptureMode& a, const CaptureMode& b) {
    const long pa = static_cast<long>(a.width) * a.height;
    const long pb = static_cast<long>(b.width) * b.height;
    if (pa != pb) return pa > pb;
    if (a.fps != b.fps) return a.fps > b.fps;
    return a.label < b.label;
  });
  out.erase(std::unique(out.begin(), out.end(),
                        [](const CaptureMode& a, const CaptureMode& b) {
                          return a.width == b.width && a.height == b.height &&
                                 std::abs(a.fps - b.fps) < 0.5;
                        }),
            out.end());
  return out;
}

std::vector<int> list_video_fourccs_for_mode(const std::string& device_name, int width, int height,
                                             double fps) {
  std::vector<int> out;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMoniker* mon = find_video_moniker(device_name.empty() ? "auto" : device_name);
  if (!mon) return out;

  IBaseFilter* filter = nullptr;
  if (FAILED(mon->BindToObject(0, 0, IID_IBaseFilter, reinterpret_cast<void**>(&filter))) || !filter) {
    mon->Release();
    return out;
  }
  mon->Release();

  IEnumPins* pins = nullptr;
  if (FAILED(filter->EnumPins(&pins)) || !pins) {
    filter->Release();
    return out;
  }

  IPin* pin = nullptr;
  while (pins->Next(1, &pin, nullptr) == S_OK) {
    PIN_DIRECTION dir = PINDIR_INPUT;
    pin->QueryDirection(&dir);
    if (dir != PINDIR_OUTPUT) {
      pin->Release();
      continue;
    }
    IAMStreamConfig* cfg = nullptr;
    if (FAILED(pin->QueryInterface(IID_IAMStreamConfig, reinterpret_cast<void**>(&cfg))) || !cfg) {
      pin->Release();
      continue;
    }
    int count = 0, size = 0;
    if (SUCCEEDED(cfg->GetNumberOfCapabilities(&count, &size)) && count > 0 && size > 0) {
      std::vector<BYTE> caps(static_cast<size_t>(size));
      for (int i = 0; i < count; ++i) {
        AM_MEDIA_TYPE* mt = nullptr;
        if (FAILED(cfg->GetStreamCaps(i, &mt, caps.data())) || !mt) continue;
        int w = 0, h = 0;
        double f = 0;
        if (mt->formattype == FORMAT_VideoInfo && mt->cbFormat >= sizeof(VIDEOINFOHEADER) && mt->pbFormat) {
          auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(mt->pbFormat);
          w = vih->bmiHeader.biWidth;
          h = std::abs(vih->bmiHeader.biHeight);
          f = fps_from_avg_time(vih->AvgTimePerFrame);
        } else if (mt->formattype == FORMAT_VideoInfo2 && mt->cbFormat >= sizeof(VIDEOINFOHEADER2) &&
                   mt->pbFormat) {
          auto* vih = reinterpret_cast<VIDEOINFOHEADER2*>(mt->pbFormat);
          w = vih->bmiHeader.biWidth;
          h = std::abs(vih->bmiHeader.biHeight);
          f = fps_from_avg_time(vih->AvgTimePerFrame);
        }
        const int fc = fourcc_from_subtype(mt->subtype);
        free_media_type(*mt);
        CoTaskMemFree(mt);
        if (fc < 0 || w != width || h != height) continue;
        if (std::abs(f - fps) > 1.5 && !(fps > 59 && fps < 61 && f > 59 && f < 61)) continue;
        if (std::find(out.begin(), out.end(), fc) == out.end()) out.push_back(fc);
      }
    }
    cfg->Release();
    pin->Release();
    break;
  }
  pins->Release();
  filter->Release();

  // OBS-ish order: I420, NV12, YUY2, MJPEG (Qualquer is prepended in the lobby UI).
  const int obs_order[] = {static_cast<int>(VideoFourcc::I420), static_cast<int>(VideoFourcc::Nv12),
                           static_cast<int>(VideoFourcc::Yuy2), static_cast<int>(VideoFourcc::Mjpeg)};
  std::vector<int> ordered;
  for (int p : obs_order) {
    if (std::find(out.begin(), out.end(), p) != out.end()) ordered.push_back(p);
  }
  for (int fc : out) {
    if (std::find(ordered.begin(), ordered.end(), fc) == ordered.end()) ordered.push_back(fc);
  }
  return ordered;
}

}  // namespace nexuscap
