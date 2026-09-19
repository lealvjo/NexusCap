// Live HDMI recovery check on the capture card (VEDO / first device).
// Usage: nexuscap_hdmi_recover_probe.exe [device_substr]
#include "nexuscap/capture.hpp"
#include "nexuscap/clock.hpp"
#include "nexuscap/frame_slot.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

using namespace nexuscap;

static int consume_matching(DShowCapture& cap, FrameSlot& slot, int want, int timeout_ms, int* out_got) {
  QpcClock clock;
  const double t0 = clock.now_ms();
  int got = 0;
  int bad = 0;
  FramePayload frame;
  const int ow = cap.opened_width();
  const int oh = cap.opened_height();
  const size_t need = nv12_packed_bytes(ow, oh);
  while (got < want && (clock.now_ms() - t0) < timeout_ms) {
    if (!slot.consume(frame)) {
      Sleep(2);
      continue;
    }
    if (frame.width != ow || frame.height != oh || !nv12_buffer_matches_mode(frame.nv12.size(), ow, oh) ||
        frame.nv12.size() < need) {
      ++bad;
      continue;
    }
    ++got;
  }
  if (out_got) *out_got = got;
  return bad;
}

int main(int argc, char** argv) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  int fails = 0;
  auto fail = [&](const char* msg) {
    std::printf("FAIL  %s\n", msg);
    ++fails;
  };
  auto pass = [](const char* msg) { std::printf("PASS  %s\n", msg); };

  std::string prefer = argc >= 2 ? argv[1] : std::string{};
  const auto devices = list_video_capture_devices();
  if (devices.empty()) {
    std::printf("SKIP  nenhum dispositivo DirectShow\n");
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
    fail("lista de modos vazia");
    return 3;
  }

  CaptureMode pick = modes.front();
  int best_score = -1;
  for (const auto& m : modes) {
    int score = 0;
    if (m.width == 2560 && m.height == 1440) score += 100;
    else if (m.width == 1920 && m.height == 1080) score += 50;
    if (std::abs(m.fps - 60.0) < 1.5) score += 20;
    else if (std::abs(m.fps - 59.94) < 1.5) score += 15;
    if (score > best_score) {
      best_score = score;
      pick = m;
    }
  }
  std::printf("user_mode=%dx%d @ %.0f (prefer 1440p60, senao 1080p60)\n", pick.width, pick.height, pick.fps);

  std::printf("modos anunciados:");
  int shown = 0;
  for (const auto& m : modes) {
    if (shown < 8) std::printf(" %s", m.label.c_str());
    ++shown;
  }
  std::printf(" (%d)\n", shown);

  int gw = 0, gh = 0;
  if (!hdmi_pick_reopen_size(nv12_packed_bytes(pick.width, pick.height), 9999, 9999, modes, &gw, &gh) ||
      gw != pick.width || gh != pick.height) {
    fail("hdmi_pick_reopen_size nao devolveu o modo do utilizador");
  } else {
    pass("pick dinamico = modo aberto (nao 9999x9999)");
  }
  for (const auto& m : modes) {
    if (m.width == pick.width && m.height == pick.height) continue;
    int fw = 0, fh = 0;
    if (!hdmi_pick_reopen_size(nv12_packed_bytes(m.width, m.height), pick.width, pick.height, modes, &fw, &fh)) {
      fail("pick falhou para um modo anunciado");
      break;
    }
    if (fw != m.width || fh != m.height) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "pick %dx%d bytes devolveu %dx%d (user era %dx%d)", m.width, m.height, fw, fh,
                    pick.width, pick.height);
      fail(buf);
      break;
    }
  }
  if (fails == 0) pass("pick segue qualquer modo anunciado (1080/1440/4K/...)");

  QpcClock clock;
  FrameSlot slot;
  DShowCapture cap;
  CaptureStartOptions opt;
  opt.width = pick.width;
  opt.height = pick.height;
  opt.fps = pick.fps > 0 ? pick.fps : 60.0;
  opt.video_fourcc = static_cast<int>(VideoFourcc::Nv12);
  std::string err;
  if (!cap.start(device, opt, slot, clock, err)) {
    std::printf("note  start sem follow falhou (%s) — HDMI nao e o modo pedido; a seguir o pin\n", err.c_str());
    opt.follow_hdmi_size = true;
    err.clear();
    if (!cap.start(device, opt, slot, clock, err)) {
      std::printf("FAIL  start follow: %s\n", err.c_str());
      return 4;
    }
    pass("follow_hdmi_size abriu o modo que o HDMI esta a mandar");
  } else {
    pass("start no modo pedido");
  }
  std::printf("opened=%dx%d fourcc=%s\n", cap.opened_width(), cap.opened_height(),
              video_fourcc_name(cap.opened_fourcc()));
  if (!opt.follow_hdmi_size &&
      (std::abs(cap.opened_width() - pick.width) > 16 || std::abs(cap.opened_height() - pick.height) > 16)) {
    fail("aberto diferente do modo pedido (sem follow_hdmi_size)");
  }
  if (opt.follow_hdmi_size && cap.opened_width() > 0) {
    pass("sinal seguido (nao travado em 2560x1440)");
  }

  int got = 0;
  int bad = consume_matching(cap, slot, 30, 4000, &got);
  std::printf("live_frames=%d bad_size=%d rejected=%llu\n", got, bad,
              static_cast<unsigned long long>(cap.rejected_samples()));
  if (got < 20) fail("poucos frames no sinal vivo");
  else pass("sinal vivo no modo aberto");
  if (bad > 0) fail("BufferCB entregou tamanho != modo aberto");

  const uint64_t before_rej = cap.rejected_samples();
  const uint64_t before_pub = slot.published();
  const int ow = cap.opened_width();
  const int oh = cap.opened_height();
  std::vector<uint8_t> junk;
  auto inject = [&](int w, int h, const char* tag) {
    const size_t n = nv12_packed_bytes(w, h);
    junk.assign(n, 0x5A);
    cap.push_buffer(junk.data(), junk.size());
    if (nv12_buffer_matches_mode(n, ow, oh)) {
      std::printf("note  inject %s %dx%d e do mesmo tamanho packed — ignorado\n", tag, w, h);
      return;
    }
    if (cap.rejected_samples() <= before_rej && n != nv12_packed_bytes(ow, oh)) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "push_buffer nao rejeitou lixo %s %dx%d (%zu bytes)", tag, w, h, n);
      fail(buf);
    }
  };
  inject(1920, 1080, "1080p");
  inject(3840, 2160, "4K");
  inject(64, 64, "tiny");
  if (cap.rejected_samples() > before_rej) pass("lixo de tamanho errado deitado fora");
  else fail("nenhum reject apos inject 1080p/4K/tiny");

  slot.clear();
  int after_junk = 0;
  bad = consume_matching(cap, slot, 10, 2000, &after_junk);
  if (bad > 0) fail("depois do lixo ainda passou frame com WxH errado");
  else pass("depois do lixo o NR so veria o modo aberto");
  (void)before_pub;

  const size_t last_bytes = cap.last_sample_bytes();
  int try_w = opt.width, try_h = opt.height;
  (void)hdmi_pick_reopen_size(last_bytes, opt.width, opt.height, modes, &try_w, &try_h);
  std::printf("recover_pick=%dx%d last_bytes=%zu\n", try_w, try_h, last_bytes);

  HdmiRecover hdmi;
  hdmi_on_sample(hdmi, 1000.0, true);
  hdmi_on_idle(hdmi, 1000.0 + hdmi_signal_lost_ms());
  slot.clear();
  cap.stop();
  Sleep(static_cast<DWORD>(capture_device_settle_ms()));
  if (hdmi_on_idle(hdmi, 1000.0 + hdmi_signal_lost_ms()) != HdmiRecoverAction::Hold) {
    fail("maquina de estados nao pediu Hold apos perda");
  } else {
    pass("Hold no ultimo frame bom (sem Present de lixo)");
  }

  CaptureStartOptions rec = opt;
  rec.width = try_w;
  rec.height = try_h;
  rec.follow_hdmi_size = true;
  err.clear();
  DShowCapture cap2;
  if (!cap2.start(device, rec, slot, clock, err)) {
    std::printf("FAIL  recover start: %s\n", err.c_str());
    return 5;
  }
  hdmi_note_reopened(hdmi, clock.now_ms());
  std::printf("reopened=%dx%d follow=%d\n", cap2.opened_width(), cap2.opened_height(), rec.follow_hdmi_size ? 1 : 0);

  int handshake = 0;
  bad = consume_matching(cap2, slot, hdmi_handshake_discard_frames(), 4000, &handshake);
  for (int i = 0; i < handshake && i < hdmi_handshake_discard_frames(); ++i) {
    if (hdmi_on_sample(hdmi, 3000.0 + i, true) != HdmiRecoverAction::Discard) {
      fail("handshake nao foi Discard");
      break;
    }
  }
  if (handshake >= hdmi_handshake_discard_frames()) pass("primeiros frames do handshake descartaveis");
  else fail("poucos frames apos Stop/Run para descartar handshake");

  int live2 = 0;
  bad = consume_matching(cap2, slot, 30, 4000, &live2);
  std::printf("post_recover_frames=%d bad_size=%d\n", live2, bad);
  if (live2 < 20) fail("depois do recover o sinal nao voltou");
  else pass("Stop/Run + follow_hdmi_size voltou a entregar frames");
  if (bad > 0) fail("depois do recover ainda ha tamanho errado");
  if (hdmi_on_sample(hdmi, 4000.0, true) != HdmiRecoverAction::Present) fail("depois do handshake nao Present");
  else pass("depois do handshake o NR receberia frames bons");

  cap2.stop();
  std::printf("%s  fails=%d opened_was=%dx%d\n", fails == 0 ? "PASS" : "FAIL", fails, ow, oh);
  return fails == 0 ? 0 : 1;
}
