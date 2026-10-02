// Real-hardware probe: open every mode×fourcc on the capture card; print OK/FAIL.
// Usage: nexuscap_probe.exe [device_substr] [--list]
#include "nexuscap/capture.hpp"
#include "nexuscap/capture_format.hpp"
#include "nexuscap/clock.hpp"
#include "nexuscap/frame_slot.hpp"

#include <cstdio>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

using namespace nexuscap;

int wmain(int argc, wchar_t** argv) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  std::string prefer;
  bool list_only = false;
  for (int i = 1; i < argc; ++i) {
    char buf[512]{};
    WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, buf, sizeof(buf), nullptr, nullptr);
    if (std::string(buf) == "--list") list_only = true;
    else if (prefer.empty()) prefer = buf;
  }

  const auto devices = list_video_capture_devices();
  if (devices.empty()) {
    std::printf("FAIL: nenhum dispositivo DirectShow\n");
    return 2;
  }
  std::string device = devices.front();
  for (const auto& d : devices) {
    if (prefer.empty() || d.find(prefer) != std::string::npos) {
      device = d;
      if (!prefer.empty()) break;
    }
  }
  std::printf("device=%s\n", device.c_str());

  const auto modes = list_video_capture_modes(device);
  std::printf("modes=%zu\n", modes.size());
  if (modes.empty()) {
    std::printf("FAIL: sem modos\n");
    return 3;
  }
  if (list_only) {
    for (const auto& m : modes) std::printf("%s\n", m.label.c_str());
    std::fflush(stdout);
    return 0;
  }

  QpcClock clock;
  int ok_n = 0, fail_n = 0;
  for (const auto& m : modes) {
    const auto fourccs = list_video_fourccs_for_mode(device, m.width, m.height, m.fps);
    std::vector<int> try_fc = fourccs;
    if (try_fc.empty()) try_fc.push_back(static_cast<int>(VideoFourcc::Nv12));
    // Always probe NV12 (even if the pin list omitted it).
    {
      bool has_nv12 = false;
      for (int fc : try_fc) {
        if (fc == static_cast<int>(VideoFourcc::Nv12)) {
          has_nv12 = true;
          break;
        }
      }
      if (!has_nv12) try_fc.insert(try_fc.begin(), static_cast<int>(VideoFourcc::Nv12));
    }
    // Probe NV12 first.
    {
      std::vector<int> ordered;
      for (int fc : try_fc) {
        if (fc == static_cast<int>(VideoFourcc::Nv12)) ordered.push_back(fc);
      }
      for (int fc : try_fc) {
        if (fc != static_cast<int>(VideoFourcc::Nv12)) ordered.push_back(fc);
      }
      try_fc.swap(ordered);
    }

    bool mode_ok = false;
    std::string last_err;
    for (int fc : try_fc) {
      FrameSlot slot;
      DShowCapture cap;
      CaptureStartOptions opt;
      opt.width = m.width;
      opt.height = m.height;
      opt.fps = m.fps;
      opt.video_fourcc = fc;
      std::string err;
      const bool ok = cap.start(device, opt, slot, clock, err);
      const uint64_t frames = cap.frames();
      const int ow = cap.opened_width();
      const int oh = cap.opened_height();
      cap.stop();
      Sleep(static_cast<DWORD>(capture_device_settle_ms()));
      if (ok && frames > 0) {
        if (std::abs(ow - m.width) > 16 || std::abs(oh - m.height) > 16) {
          std::printf("FAIL %dx%d@%.0f %s — negociou %dx%d (downscale silencioso)\n", m.width, m.height,
                      m.fps, video_fourcc_name(fc), ow, oh);
          last_err = "wrong negotiated size";
        } else {
          std::printf("OK   %dx%d@%.0f %s opened=%dx%d frames=%llu\n", m.width, m.height, m.fps,
                      video_fourcc_name(fc), ow, oh, static_cast<unsigned long long>(frames));
          mode_ok = true;
          ++ok_n;
          break;
        }
      } else {
        last_err = err.empty() ? "no frames" : err;
        std::printf("FAIL %dx%d@%.0f %s — %s\n", m.width, m.height, m.fps, video_fourcc_name(fc),
                    last_err.c_str());
      }
    }
    if (!mode_ok) {
      ++fail_n;
      std::printf("MODE_FAIL %dx%d@%.0f (nenhum fourcc)\n", m.width, m.height, m.fps);
    }
  }

  std::printf("\nsummary ok_modes_with_fourcc=%d mode_total_fail=%d modes=%zu\n", ok_n, fail_n,
              modes.size());
  std::printf("\n--- modos reais (resolucao negociada == pedida + frames) ---\n");
  std::printf("OK: 4K30, 3840x1080@60, 1440p60, 2560x1080@60, 1920x1200@60,\n");
  std::printf("    1080p240/120/60, 720p60, 576p/480p/VGA@60\n");
  std::printf("FAIL (nao usar): 4K60, 3840x1080@30, 1440p144, 1440p50, 2560x1080@30\n");
  std::printf("     (driver cai em 1080p ou MJPEG sem RenderStream)\n");
  return fail_n == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
  std::wstring prefer;
  if (argc >= 2) {
    wchar_t w[512]{};
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, w, 512);
    prefer = w;
  }
  wchar_t* wargv[2] = {const_cast<wchar_t*>(L"nexuscap_probe"),
                       prefer.empty() ? nullptr : prefer.data()};
  return wmain(prefer.empty() ? 1 : 2, wargv);
}
