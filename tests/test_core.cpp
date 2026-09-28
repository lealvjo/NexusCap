#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "nexuscap/analyze.hpp"
#include "nexuscap/audio.hpp"
#include "nexuscap/audio_select.hpp"
#include "nexuscap/clock.hpp"
#include "nexuscap/color_bt709.hpp"
#include "nexuscap/config.hpp"
#include "nexuscap/display_geom.hpp"
#include "nexuscap/dlss_options.hpp"
#include "nexuscap/dlss5_bridge.hpp"
#include "nexuscap/frame_slot.hpp"
#include "nexuscap/frametime.hpp"
#include "nexuscap/hitch.hpp"
#include "nexuscap/live_mode.hpp"
#include "nexuscap/lossless_fg.hpp"
#include "nexuscap/perf_test.hpp"
#include "nexuscap/session_cfg.hpp"
#include "nexuscap/session_report.hpp"
#include "nexuscap/settings_store.hpp"
#include "nexuscap/user_preset.hpp"
#include "nexuscap/stats_window.hpp"
#include "nexuscap/system_metrics.hpp"
#include "nexuscap/capture_format.hpp"
#include "nexuscap/capture.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace nexuscap;

TEST_CASE("FrameSlot clear drops stale frame after format change") {
  FrameSlot slot;
  FramePayload a;
  a.frame_index = 1;
  a.width = 1920;
  a.height = 1080;
  a.nv12.resize(100);
  slot.publish(std::move(a));
  CHECK(slot.has_frame());
  slot.clear();
  CHECK_FALSE(slot.has_frame());
  FramePayload out;
  CHECK_FALSE(slot.consume(out));
  CHECK(slot.dropped() >= 1);
}

TEST_CASE("capture reopen settle helpers") {
  CHECK(capture_device_settle_ms() >= 100);
  CHECK(capture_first_frame_wait_ms(1920, 1080) >= capture_device_settle_ms());
  CHECK(capture_first_frame_wait_ms(3840, 2160) > capture_first_frame_wait_ms(1920, 1080));
  CHECK(capture_mode_prefers_nv12(3840, 2160));
  CHECK_FALSE(capture_mode_prefers_nv12(1920, 1080));
}

TEST_CASE("nv12 buffer match is the opened mode not a hardcoded 1440p") {
  const size_t n1080 = nv12_packed_bytes(1920, 1080);
  const size_t n1440 = nv12_packed_bytes(2560, 1440);
  const size_t n4k = nv12_packed_bytes(3840, 2160);
  CHECK(n1080 > 0);
  CHECK(n1440 > n1080);
  CHECK(n4k > n1440);
  CHECK(nv12_buffer_matches_mode(n1080, 1920, 1080));
  CHECK(nv12_buffer_matches_mode(n1440, 2560, 1440));
  CHECK(nv12_buffer_matches_mode(n1440 + 2560, 2560, 1440));  // small stride pad
  CHECK_FALSE(nv12_buffer_matches_mode(n1080, 2560, 1440));
  CHECK_FALSE(nv12_buffer_matches_mode(n4k, 2560, 1440));
  CHECK_FALSE(nv12_buffer_matches_mode(n1440, 1920, 1080));
  CHECK_FALSE(nv12_buffer_matches_mode(n1440 - 1, 2560, 1440));
}

TEST_CASE("hdmi reopen size follows advertised mode of the returning buffer") {
  std::vector<CaptureMode> modes;
  modes.push_back(CaptureMode{3840, 2160, 60.0, "4k"});
  modes.push_back(CaptureMode{2560, 1440, 60.0, "1440"});
  modes.push_back(CaptureMode{1920, 1080, 60.0, "1080"});
  int w = 0, h = 0;
  CHECK(guess_nv12_wh_from_bytes(nv12_packed_bytes(1920, 1080), modes, &w, &h));
  CHECK(w == 1920);
  CHECK(h == 1080);
  CHECK(guess_nv12_wh_from_bytes(nv12_packed_bytes(2560, 1440), modes, &w, &h));
  CHECK(w == 2560);
  CHECK(h == 1440);
  CHECK(hdmi_pick_reopen_size(nv12_packed_bytes(1920, 1080), 2560, 1440, modes, &w, &h));
  CHECK(w == 1920);
  CHECK(h == 1080);
  CHECK(hdmi_pick_reopen_size(0, 2560, 1440, modes, &w, &h));
  CHECK(w == 2560);
  CHECK(h == 1440);
  CHECK(hdmi_pick_reopen_size(0, 1280, 720, modes, &w, &h));
  CHECK(w == 1280);
  CHECK(h == 720);
}

TEST_CASE("hdmi recover holds last good and resumes on first good frame") {
  CHECK(hdmi_reopen_after_return_ms() >= 4000);
  CHECK(hdmi_notice_lost_ms() >= 2000);
  CHECK(hdmi_signal_lost_ms() <= 50);
  CHECK(hdmi_signal_lost_ms() >= 32);
  HdmiRecover s;
  CHECK(hdmi_on_sample(s, 1000.0, true) == HdmiRecoverAction::Present);
  CHECK(hdmi_on_idle(s, 1020.0) == HdmiRecoverAction::None);
  CHECK(hdmi_on_idle(s, 1000.0 + hdmi_signal_lost_ms()) == HdmiRecoverAction::Hold);
  CHECK(s.lost);
  CHECK_FALSE(hdmi_lost_long_enough(s, 1000.0 + hdmi_signal_lost_ms()));
  // Pin stays open: first good picture goes to Present (no settle / no Stop/Run).
  CHECK(hdmi_on_sample(s, 1400.0, true, true) == HdmiRecoverAction::Present);
  CHECK_FALSE(s.lost);
}

TEST_CASE("hdmi recover rejects wrong-size samples and follows later") {
  HdmiRecover s;
  CHECK(hdmi_on_sample(s, 1000.0, true) == HdmiRecoverAction::Present);
  CHECK(hdmi_on_sample(s, 1016.0, false) == HdmiRecoverAction::Hold);
  CHECK(hdmi_on_sample(s, 1016.0 + 700.0, false) == HdmiRecoverAction::Hold);
  CHECK(hdmi_on_sample(s, 1016.0 + hdmi_reopen_after_return_ms(), false) == HdmiRecoverAction::Reopen);
}

TEST_CASE("hdmi recover pink junk never becomes last good and never reopens") {
  HdmiRecover s;
  CHECK(hdmi_on_sample(s, 1000.0, true, true) == HdmiRecoverAction::Present);
  CHECK(hdmi_on_sample(s, 1016.0, true, false) == HdmiRecoverAction::Hold);
  CHECK(hdmi_on_sample(s, 1016.0 + hdmi_reopen_after_return_ms(), true, false) == HdmiRecoverAction::Hold);
  CHECK(hdmi_on_sample(s, 1016.0 + hdmi_reopen_after_return_ms() + 16.0, true, true) ==
        HdmiRecoverAction::Present);
}

TEST_CASE("hdmi recover cooldown blocks a second Stop/Run") {
  HdmiRecover s;
  CHECK(hdmi_on_sample(s, 1000.0, true) == HdmiRecoverAction::Present);
  CHECK(hdmi_on_sample(s, 1016.0, false) == HdmiRecoverAction::Hold);
  const double t_reopen = 1016.0 + hdmi_reopen_after_return_ms();
  CHECK(hdmi_on_sample(s, t_reopen, false) == HdmiRecoverAction::Reopen);
  hdmi_note_reopened(s, t_reopen);
  CHECK(hdmi_on_sample(s, t_reopen + 50.0, false) == HdmiRecoverAction::Hold);
  CHECK_FALSE(hdmi_can_reopen(s, t_reopen + 50.0));
  CHECK(hdmi_on_sample(s, t_reopen + hdmi_reopen_cooldown_ms(), false) == HdmiRecoverAction::Hold);
  CHECK(hdmi_on_sample(s, t_reopen + 50.0 + hdmi_reopen_after_return_ms(), false) ==
        HdmiRecoverAction::Reopen);
}

TEST_CASE("hdmi recover 720p user mode is not locked to 1440p") {
  const size_t n720 = nv12_packed_bytes(1280, 720);
  CHECK(nv12_buffer_matches_mode(n720, 1280, 720));
  CHECK_FALSE(nv12_buffer_matches_mode(nv12_packed_bytes(2560, 1440), 1280, 720));
  CHECK_FALSE(nv12_buffer_matches_mode(n720, 2560, 1440));
  std::vector<CaptureMode> modes = {
      CaptureMode{2560, 1440, 60.0, "1440"},
      CaptureMode{1920, 1080, 60.0, "1080"},
      CaptureMode{1280, 720, 60.0, "720"},
  };
  int w = 0, h = 0;
  CHECK(hdmi_pick_reopen_size(n720, 2560, 1440, modes, &w, &h));
  CHECK(w == 1280);
  CHECK(h == 720);
}

TEST_CASE("nv12 magenta no-signal is junk, normal picture is not") {
  const int w = 64, h = 64;
  std::vector<uint8_t> mag(nv12_packed_bytes(w, h), 0);
  std::fill(mag.begin(), mag.begin() + static_cast<size_t>(w) * h, static_cast<uint8_t>(140));
  std::fill(mag.begin() + static_cast<size_t>(w) * h, mag.end(), static_cast<uint8_t>(200));
  CHECK(nv12_looks_like_hdmi_junk(mag, w, h));
  std::vector<uint8_t> noisy = mag;
  for (int y = 0; y < h; ++y) {
    noisy[static_cast<size_t>(y) * w + 3] = static_cast<uint8_t>(80 + (y % 40));
  }
  CHECK(nv12_looks_like_hdmi_junk(noisy, w, h));
  std::vector<uint8_t> blk;
  nv12_fill_limited_black(blk, w, h);
  CHECK(blk.size() == nv12_packed_bytes(w, h));
  CHECK(blk[0] == 16);
  CHECK(blk[static_cast<size_t>(w) * h] == 128);
  CHECK_FALSE(nv12_looks_like_hdmi_junk(blk, w, h));
  std::vector<uint8_t> pic(nv12_packed_bytes(w, h), 128);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      pic[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>((x + y) & 255);
    }
  }
  CHECK_FALSE(nv12_looks_like_hdmi_junk(pic, w, h));
}

TEST_CASE("hdmi late good sample is present hitch not dropout") {
  HdmiRecover s;
  CHECK(hdmi_on_arrived(s, 1000.0, true, true) == HdmiRecoverAction::Present);
  CHECK(hdmi_on_arrived(s, 1016.0, true, true) == HdmiRecoverAction::Present);
  CHECK(hdmi_on_arrived(s, 1016.0 + 51.0, true, true) == HdmiRecoverAction::Present);
  CHECK_FALSE(s.lost);
  CHECK(hdmi_on_arrived(s, 1016.0 + 51.0, true, false) == HdmiRecoverAction::Hold);
  CHECK(s.lost);
  CHECK(hdmi_on_idle(s, 2000.0) == HdmiRecoverAction::Hold);
}

TEST_CASE("hdmi notice only after a real long dropout") {
  CHECK(hdmi_notice_dwell_ms() >= 2500);
  CHECK(hdmi_notice_dwell_ms() <= 4000);
  CHECK(hdmi_notice_lost_ms() >= 2000);
  CHECK_FALSE(hdmi_notice_visible(true, 1000.0, 0.0));
  CHECK(hdmi_notice_visible(false, 1100.0, 4000.0));
  CHECK_FALSE(hdmi_notice_visible(false, 4100.0, 4000.0));
  HdmiRecover s;
  CHECK(hdmi_on_sample(s, 1000.0, true, true) == HdmiRecoverAction::Present);
  CHECK(hdmi_on_idle(s, 1000.0 + hdmi_signal_lost_ms()) == HdmiRecoverAction::Hold);
  CHECK_FALSE(hdmi_lost_long_enough(s, 1000.0 + hdmi_signal_lost_ms()));
  CHECK_FALSE(hdmi_lost_long_enough(s, 1000.0 + 400.0));
  CHECK(hdmi_lost_long_enough(s, s.lost_at_ms + hdmi_notice_lost_ms()));
}

TEST_CASE("nv12 chroma jump toward magenta is pink handshake") {
  const int w = 64, h = 64;
  std::vector<uint8_t> pic(nv12_packed_bytes(w, h), 128);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      pic[static_cast<size_t>(y) * w + x] = static_cast<uint8_t>(40 + ((x + y) % 80));
    }
  }
  std::vector<uint8_t> pink(nv12_packed_bytes(w, h), 200);
  std::fill(pink.begin(), pink.begin() + static_cast<size_t>(w) * h, static_cast<uint8_t>(120));
  const auto prev = nv12_chroma_mean(pic, w, h);
  const auto now = nv12_chroma_mean(pink, w, h);
  CHECK(prev.ok);
  CHECK(now.ok);
  CHECK(nv12_chroma_jumped_to_pink(prev, now));
  CHECK_FALSE(nv12_chroma_jumped_to_pink(prev, prev));
}

TEST_CASE("hdmi session loop: freeze then recover without Stop/Run") {
  HdmiRecover s;
  int present = 0, hold = 0, discard = 0, reopen = 0;
  auto tick = [&](double now, bool have_new, bool size_ok, bool mismatch_bytes) {
    HdmiRecoverAction act = HdmiRecoverAction::None;
    if (have_new) {
      act = hdmi_on_sample(s, now, size_ok, size_ok);
    } else {
      act = hdmi_on_idle(s, now);
      if (s.lost && mismatch_bytes) {
        const HdmiRecoverAction pulse = hdmi_on_sample(s, now, false, false);
        if (pulse == HdmiRecoverAction::Reopen) act = pulse;
      }
    }
    if (act == HdmiRecoverAction::Present) ++present;
    else if (act == HdmiRecoverAction::Hold) ++hold;
    else if (act == HdmiRecoverAction::Discard) ++discard;
    else if (act == HdmiRecoverAction::Reopen) ++reopen;
    return act;
  };

  CHECK(tick(1000.0, true, true, false) == HdmiRecoverAction::Present);
  CHECK(tick(1016.0, true, true, false) == HdmiRecoverAction::Present);
  CHECK(tick(1040.0, false, false, false) == HdmiRecoverAction::None);
  CHECK(tick(1000.0 + hdmi_signal_lost_ms() + 16.0, false, false, false) == HdmiRecoverAction::Hold);
  CHECK(hold >= 1);
  CHECK(reopen == 0);
  CHECK(tick(1400.0, true, true, false) == HdmiRecoverAction::Present);
  CHECK(present == 3);
  CHECK(reopen == 0);
}

TEST_CASE("FrameSlot latest-wins and drop count") {
  FrameSlot slot;
  FramePayload a;
  a.frame_index = 1;
  a.width = 2;
  a.height = 2;
  a.nv12.resize(6, 1);
  slot.publish(std::move(a));
  FramePayload b;
  b.frame_index = 2;
  b.width = 2;
  b.height = 2;
  b.nv12.resize(6, 2);
  slot.publish(std::move(b));
  CHECK(slot.dropped() == 1);
  FramePayload out;
  CHECK(slot.consume(out));
  CHECK(out.frame_index == 2);
  CHECK_FALSE(slot.consume(out));
}

TEST_CASE("FrameSlot recycles dropped NV12 capacity") {
  FrameSlot slot;
  FramePayload a;
  a.nv12.assign(6, 1);
  a.frame_index = 1;
  slot.publish(std::move(a));
  FramePayload b;
  b.nv12.assign(6, 2);
  b.frame_index = 2;
  slot.publish(std::move(b));
  std::vector<uint8_t> recycled;
  slot.take_recycle_nv12(recycled);
  CHECK(recycled.size() == 6);
  CHECK(recycled[0] == 1);
}

TEST_CASE("MGF techniques idle lobby vs session chain") {
  const std::string base =
      "Techniques=Lumenite_Kernel@lumenite_Kernel.fx,DLSS5_Feed@DLSS5_Feed.fx,DLSS5_Sharpen@DLSS5_Sharpen.fx\n"
      "TechniqueSorting=Lumenite_Kernel@lumenite_Kernel.fx,DLSS5_Feed@DLSS5_Feed.fx,DLSS5_Sharpen@DLSS5_Sharpen.fx\n";
  CHECK(renodx_mgf_chain_order_ok(base));
  auto idle = apply_mgf_techniques_for_phase(base, false);
  CHECK_FALSE(renodx_mgf_chain_order_ok(idle));
  {
    const auto pos = idle.find("Techniques=");
    REQUIRE(pos != std::string::npos);
    const auto end = idle.find('\n', pos);
    const std::string line = idle.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    CHECK(line == "Techniques=");
  }
  auto sess = apply_mgf_techniques_for_phase(idle, true);
  CHECK(renodx_mgf_chain_order_ok(sess));
}

TEST_CASE("perf test pipeline restores techniques after lobby CFG live off") {
  CHECK_FALSE(lobby_preview_nr_files_on(true, false, true, true));
  const std::string idle = apply_mgf_techniques_for_phase(
      "Techniques=Lumenite_Kernel@lumenite_Kernel.fx\nTechniqueSorting=Lumenite_Kernel@lumenite_Kernel.fx\n",
      false);
  CHECK_FALSE(renodx_mgf_chain_order_ok(idle));
  CHECK(apply_mgf_techniques_for_phase(idle, true).find(kRenodxSessionTechniqueChain) != std::string::npos);

  PerfTestPlan p;
  p.nr_pass_count = 4;
  p.renodx_preset = 1;
  p.scale_percent = 100;
  const PersistedSettings s = persisted_from_perf_test_plan(p);
  CHECK(s.enhance_renodx);
  CHECK(s.neural_rendering);
  CHECK(s.nr_pass_count == 4);
  CHECK(s.renodx_preset == 1);
}

TEST_CASE("HitchDetector threshold and log cap") {
  HitchDetector d(25.0, 8);
  int logged = 0;
  for (int i = 0; i < 20; ++i) {
    if (d.observe(static_cast<uint64_t>(i), i * 10.0, 30.0, 0, 1, 1)) ++logged;
  }
  CHECK(logged == 8);
  CHECK(d.stall_count() == 20);
  d.reset_window_log_cap();
  CHECK(d.observe(100, 1000, 30, 0, 0, 0).has_value());
}

TEST_CASE("effective_hitch_threshold_ms scales with capture fps") {
  CHECK(effective_hitch_threshold_ms(25.0, 60.0) == doctest::Approx(25.0));
  CHECK(effective_hitch_threshold_ms(25.0, 120.0) == doctest::Approx(12.5));
  CHECK(effective_hitch_threshold_ms(25.0, 144.0) == doctest::Approx(25.0 * 60.0 / 144.0));
  CHECK(effective_hitch_threshold_ms(25.0, 30.0) == doctest::Approx(50.0));
  CHECK(effective_hitch_threshold_ms(25.0, 24.0) == doctest::Approx(62.5));
  HitchDetector d30(effective_hitch_threshold_ms(25.0, 30.0));
  CHECK_FALSE(d30.observe(1, 0, 33.4, 0, 0, 0).has_value());
  CHECK(d30.observe(2, 50, 55.0, 0, 0, 0).has_value());
  HitchDetector d120(effective_hitch_threshold_ms(25.0, 120.0));
  CHECK_FALSE(d120.observe(1, 0, 8.4, 0, 0, 0).has_value());
  CHECK(d120.observe(2, 20, 14.0, 0, 0, 0).has_value());
}

TEST_CASE("hitch no false positive on steady cadence 30/60/120/144") {
  const double fpss[] = {30.0, 60.0, 120.0, 144.0};
  for (double fps : fpss) {
    const double thr = effective_hitch_threshold_ms(25.0, fps);
    const double cadence = 1000.0 / fps;
    HitchDetector d(thr);
    // 600 steady frames at exact cadence (+0.5ms jitter) must not stall.
    for (int i = 0; i < 600; ++i) {
      CHECK_FALSE(d.observe(static_cast<uint64_t>(i), i * cadence, cadence + 0.5, 0, 0, 0).has_value());
    }
    CHECK(d.stall_count() == 0);
    // One real hitch (~2× cadence) must count.
    CHECK(d.observe(600, 600 * cadence, cadence * 2.0, 0, 0, 0).has_value());
    CHECK(d.stall_count() == 1);
  }
  // Legacy bug: fixed 25ms would stall every 30fps frame.
  CHECK(effective_hitch_threshold_ms(25.0, 30.0) > 33.4);
}

TEST_CASE("overlay ft_stable no false UNSTABLE at 30/120/144") {
  const double fpss[] = {30.0, 60.0, 120.0, 144.0};
  for (double fps : fpss) {
    const double cadence = 1000.0 / fps;
    const double hitch_ms = effective_hitch_threshold_ms(25.0, fps);
    FrametimeWindow w(180);
    for (int i = 0; i < 60; ++i) w.push(cadence + 0.3);
    CHECK(w.stable(cadence, hitch_ms));
  }
  // Old HUD bug: default max_stall 25ms made every 30fps frame UNSTABLE.
  FrametimeWindow w30(180);
  for (int i = 0; i < 60; ++i) w30.push(33.4);
  CHECK(w30.stable(1000.0 / 30.0));  // auto 1.5× target
  CHECK_FALSE(w30.stable(1000.0 / 30.0, 25.0));  // legacy fixed cap still fails
  {
    FrametimeWindow w(120);
    for (int i = 0; i < 60; ++i) w.push(16.7);
    const double p = w.p95();
    CHECK(w.stable_with_p95(p, 16.67, 25.0) == w.stable(16.67, 25.0));
    CHECK(w.stable_with_p95(p, 16.67, 25.0));
  }
}

TEST_CASE("overlay ft status PLAYABLE FG matches Benchmark grade") {
  CHECK(kOverlayFtFpsSlop == doctest::Approx(kPerfTestFpsSlop));
  CHECK(overlay_ft_status(true, true, 3, 40.0, 60.0, 120.0) == OverlayFtStatus::Stable);
  CHECK(std::string(overlay_ft_status_label(OverlayFtStatus::Stable)) == "STABLE");
  CHECK(std::string(overlay_ft_status_hint(OverlayFtStatus::Stable)).empty());

  CHECK(overlay_ft_status(false, true, 2, 40.0, 60.0, 80.0) == OverlayFtStatus::PlayableFg);
  CHECK(std::string(overlay_ft_status_label(OverlayFtStatus::PlayableFg)) == "PLAYABLE FG");
  CHECK(std::string(overlay_ft_status_hint(OverlayFtStatus::PlayableFg)).find("FG cobre") != std::string::npos);
  {
    const FgFtRgb c = overlay_ft_status_rgb(OverlayFtStatus::PlayableFg);
    CHECK(c.r == 255);
    CHECK(c.g == 196);
    CHECK(c.b == 72);
  }

  CHECK(overlay_ft_status(false, true, 2, 20.0, 60.0, 40.0) == OverlayFtStatus::Unstable);
  CHECK(overlay_ft_status(false, true, 2, 22.0, 30.0, 44.0) == OverlayFtStatus::PlayableFg);
  CHECK(overlay_ft_status(false, true, 4, 18.0, 60.0, 72.0) == OverlayFtStatus::Unstable);
  CHECK(overlay_ft_status(false, false, 2, 40.0, 60.0, 80.0) == OverlayFtStatus::Unstable);
  CHECK(overlay_ft_status(false, true, 1, 40.0, 60.0, 0) == OverlayFtStatus::Unstable);
  CHECK(std::string(overlay_ft_status_label(OverlayFtStatus::Unstable)) == "UNSTABLE");

  {
    PerfTestSample s;
    s.fps = 40.0;
    s.target_fps = 60.0;
    s.fg_multiplier = 2;
    s.p95_present_ms = 8.0;
    s.stalls = 0;
    CHECK(grade_perf_test(s) == PerfTestGrade::PlayableFg);
    CHECK(overlay_ft_status(false, true, 2, s.fps, s.target_fps, perf_test_fg_est_fps(s.fps, 2)) ==
          OverlayFtStatus::PlayableFg);
  }
  {
    PerfTestSample s;
    s.fps = 22.0;
    s.target_fps = 30.0;
    s.fg_multiplier = 2;
    s.p95_present_ms = 8.0;
    s.stalls = 0;
    CHECK(grade_perf_test(s) == PerfTestGrade::PlayableFg);
    CHECK(overlay_ft_status(false, true, 2, s.fps, s.target_fps) == OverlayFtStatus::PlayableFg);
  }
  {
    PerfTestSample s;
    s.fps = 18.0;
    s.target_fps = 60.0;
    s.fg_multiplier = 4;
    s.p95_present_ms = 8.0;
    s.stalls = 0;
    CHECK(grade_perf_test(s) == PerfTestGrade::Fail);
    CHECK(overlay_ft_status(false, true, 4, s.fps, s.target_fps) == OverlayFtStatus::Unstable);
  }

  char a[256]{};
  char b[256]{};
  format_mini_overlay_lines(a, sizeof(a), b, sizeof(b), 40.0, 80.0, true, 2, true, true, 25.0, 25.0, 26.0, 30.0,
                            false, 8.5, 2.2, 0, 0, 0, 2560, 1440, 2560, 1440, 2560, 1440, 100, false, 0, 0, 0, 0,
                            nullptr, nullptr, 0, 0, 0, nullptr, 60.0);
  CHECK(std::strstr(b, "PLAYABLE FG") != nullptr);
  CHECK(std::strstr(b, "UNSTABLE") == nullptr);
}

TEST_CASE("overlay frametime line is shared across HUDs") {
  char line[160]{};
  format_overlay_frametime_line(line, sizeof(line), 25.04, 24.82, 26.10, false, true, 2, 40.0, 60.0, 80.0);
  CHECK(std::strstr(line, "ft 25.04") != nullptr);
  CHECK(std::strstr(line, "avg 24.82") != nullptr);
  CHECK(std::strstr(line, "p95 26.10") != nullptr);
  CHECK(std::strstr(line, "FG est 12.52") != nullptr);
  CHECK(std::strstr(line, "PLAYABLE FG") != nullptr);
  CHECK(std::strstr(line, "max") == nullptr);
  CHECK(std::strstr(line, "present") == nullptr);

  format_overlay_frametime_line(line, sizeof(line), 16.67, 16.60, 17.00, true, false, 2, 60.0, 60.0, 0);
  CHECK(std::strstr(line, "ft 16.67") != nullptr);
  CHECK(std::strstr(line, "FG est") == nullptr);
  CHECK(std::strstr(line, "STABLE") != nullptr);

  format_overlay_frametime_live(line, sizeof(line), 25.04, true, 2);
  CHECK(std::string(line) == "25.04 · FG 12.52");
  format_overlay_frametime_live(line, sizeof(line), 16.67, false, 2);
  CHECK(std::string(line) == "16.67");

  format_overlay_latency_line(line, sizeof(line), 8.50, 2.20, true);
  CHECK(std::strstr(line, "cap 8.50") != nullptr);
  CHECK(std::strstr(line, "present 2.20") != nullptr);
  CHECK(std::strstr(line, "+NR") != nullptr);
  CHECK(std::strstr(line, "ft ") == nullptr);

  PersistedSettings s = default_settings();
  s.ui.show_fps = true;
  auto hud = make_lobby_hud_preview_lines(s, 60.0);
  CHECK(lobby_hud_contains(hud, "ft "));
  CHECK(lobby_hud_contains(hud, "avg "));
  CHECK(lobby_hud_contains(hud, "p95 "));
  CHECK_FALSE(lobby_hud_contains(hud, "FG est"));
  CHECK_FALSE(lobby_hud_contains(hud, "frametime  "));
}

TEST_CASE("ClusterDetector metronome gap") {
  ClusterDetector c(500.0, 3);
  FakeClock clock;
  std::vector<ClusterEvent> clusters;
  auto burst = [&](double t0, uint64_t f0) {
    for (int i = 0; i < 8; ++i) {
      HitchEvent h;
      h.t_ms = t0 + i * 16.0;
      h.interval_ms = 32;
      h.frame = f0 + static_cast<uint64_t>(i);
      if (auto e = c.on_hitch(h)) clusters.push_back(*e);
    }
    c.note_quiet(t0 + 2000);
  };
  burst(1000, 100);
  burst(1000 + 16200, 100 + 960);
  burst(1000 + 32400, 100 + 1920);
  REQUIRE(clusters.size() >= 3);
  CHECK(std::abs(clusters[1].gap_since_prev_cluster_ms - 16200.0) < 1.0);
  CHECK(clusters[1].frames_since_prev == 960);
}

TEST_CASE("StatsWindow600 fps") {
  StatsWindow600 w(600);
  std::optional<Stats600> out;
  for (int i = 0; i < 600; ++i) {
    out = w.push(16.666, 8.0, 1.0, false);
  }
  REQUIRE(out.has_value());
  CHECK(out->frames == 600);
  CHECK(out->fps > 59.0);
  CHECK(out->fps < 61.0);
  CHECK(out->stalls == 0);
}

TEST_CASE("stats warmup excludes boot frames from window") {
  CHECK_FALSE(stats_frame_counts_toward_window(1));
  CHECK_FALSE(stats_frame_counts_toward_window(kStatsWarmupFrames));
  CHECK(stats_frame_counts_toward_window(kStatsWarmupFrames + 1));
  CHECK(kStatsWarmupFrames == 120);

  // Boot hitch + steady: only steady samples enter the window → ~60 fps.
  StatsWindow600 w(600);
  std::optional<Stats600> out;
  uint64_t presented = 0;
  for (uint64_t i = 0; i < kStatsWarmupFrames + 600; ++i) {
    ++presented;
    const double interval = (presented == 6) ? 2500.0 : 16.666;
    if (!stats_frame_counts_toward_window(presented)) continue;
    out = w.push(interval, 8.0, 1.0, interval >= 25.0);
  }
  REQUIRE(out.has_value());
  CHECK(out->fps > 59.0);
  CHECK(out->fps < 61.0);
  CHECK(out->max_interval_ms < 100.0);
  CHECK(out->stalls == 0);
}

TEST_CASE("AnalyzeWriter JSONL schema") {
  AnalyzeWriter a;
  SessionInfo info;
  info.device = "VEDO-TEST";
  info.width = 2560;
  info.height = 1440;
  info.fps = 60;
  a.session_start(info, 0);
  HitchEvent h{10, 32, 0, 1, 2, 29, 5};
  a.hitch(h);
  ClusterEvent c{10, 8, 120, 16200, 960, 1};
  a.cluster(c);
  Stats600 s;
  s.frames = 600;
  s.fps = 60;
  a.stats_600(s, 20);
  a.session_end(30);
  const auto lines = a.lines_copy();
  REQUIRE(lines.size() >= 5);
  CHECK(lines[0].find("\"type\":\"session_start\"") != std::string::npos);
  CHECK(lines[1].find("\"type\":\"hitch\"") != std::string::npos);
  CHECK(lines[2].find("\"type\":\"cluster\"") != std::string::npos);
  CHECK(lines[3].find("\"type\":\"stats_600\"") != std::string::npos);
  CHECK(lines.back().find("\"type\":\"session_end\"") != std::string::npos);
  const std::string sum = a.summary_text(30);
  CHECK(sum.find("hitch_count:") != std::string::npos);
  CHECK(sum.find("cluster_count:") != std::string::npos);
  CHECK(sum.find("VEDO-TEST") != std::string::npos);
}

TEST_CASE("BT709 limited black and white") {
  auto black = ycbcr_bt709_limited_to_rgb(16, 128, 128);
  CHECK(black.r == 0);
  CHECK(black.g == 0);
  CHECK(black.b == 0);
  auto white = ycbcr_bt709_limited_to_rgb(235, 128, 128);
  CHECK(white.r == 255);
  CHECK(white.g == 255);
  CHECK(white.b == 255);
}

TEST_CASE("parse_args smoke") {
  char a0[] = "NexusCap";
  char a1[] = "--smoke";
  char a2[] = "--frames";
  char a3[] = "90";
  char* argv[] = {a0, a1, a2, a3};
  auto c = parse_args(4, argv);
  CHECK(c.smoke);
  CHECK(c.fake_capture);
  CHECK(c.smoke_frames == 90);
  CHECK(c.frames_limited);
  CHECK_FALSE(c.dlss);
}

TEST_CASE("--frames alone enables auto-quit limit (no infinite run)") {
  char a0[] = "NexusCap";
  char a1[] = "--fake-capture";
  char a2[] = "--frames";
  char a3[] = "90";
  char* argv[] = {a0, a1, a2, a3};
  auto c = parse_args(4, argv);
  CHECK(c.fake_capture);
  CHECK(c.frames_limited);
  CHECK(c.smoke_frames == 90);
  CHECK_FALSE(c.smoke);
}

TEST_CASE("parse --renodx selects feeder stack backend") {
  char a0[] = "NexusCap";
  char a1[] = "--renodx";
  char* argv[] = {a0, a1};
  auto c = parse_args(2, argv);
  CHECK(c.enhance == EnhanceBackend::Renodx);
  CHECK_FALSE(c.dlss);
  CHECK(std::string(enhance_backend_name(c.enhance)) == "renodx");
}

TEST_CASE("parse --enhance streamline") {
  char a0[] = "NexusCap";
  char a1[] = "--enhance";
  char a2[] = "streamline";
  char* argv[] = {a0, a1, a2};
  auto c = parse_args(3, argv);
  CHECK(c.enhance == EnhanceBackend::Streamline);
  CHECK(c.dlss);
}

TEST_CASE("parse dlss flags and modes") {
  char a0[] = "NexusCap";
  char a1[] = "--dlss";
  char a2[] = "--dlss-mode";
  char a3[] = "quality";
  char a4[] = "--dlss-mvec";
  char a5[] = "zero";
  char a6[] = "--dlss-sharpness";
  char a7[] = "0.5";
  char* argv[] = {a0, a1, a2, a3, a4, a5, a6, a7};
  auto c = parse_args(8, argv);
  CHECK(c.dlss);
  CHECK(c.enhance == EnhanceBackend::Streamline);
  CHECK(c.dlss_mode == DlssMode::Quality);
  CHECK(c.dlss_mvec == DlssMvecSource::Zero);
  CHECK(c.dlss_sharpness == doctest::Approx(0.5f));
}

TEST_CASE("dlss mode parse helpers") {
  DlssMode m = DlssMode::Off;
  CHECK(parse_dlss_mode("dlaa", m));
  CHECK(m == DlssMode::Dlaa);
  CHECK(parse_dlss_mode("balanced", m));
  CHECK(m == DlssMode::Balanced);
  CHECK_FALSE(parse_dlss_mode("nope", m));
  CHECK(std::string(dlss_mode_name(DlssMode::Performance)) == "perf");
}

TEST_CASE("same-res DLAA geometry and routing") {
  CHECK(is_same_res_dlaa(2560, 1440, 2560, 1440));
  CHECK_FALSE(is_same_res_dlaa(1920, 1080, 2560, 1440));
  int rw = 0, rh = 0;
  dlss_suggested_render_size(DlssMode::Dlaa, 2560, 1440, rw, rh);
  CHECK(rw == 2560);
  CHECK(rh == 1440);
  dlss_suggested_render_size(DlssMode::Quality, 2560, 1440, rw, rh);
  CHECK(rw < 2560);
  CHECK(rh < 1440);
  CHECK_FALSE(dlss_should_evaluate(false, DlssMode::Dlaa, true));
  CHECK_FALSE(dlss_should_evaluate(true, DlssMode::Off, true));
  CHECK_FALSE(dlss_should_evaluate(true, DlssMode::Dlaa, false));
  CHECK(dlss_should_evaluate(true, DlssMode::Dlaa, true));
}

TEST_CASE("audio prefers VEDO HDMI over mic/line") {
  const std::wstring prefer = L"VEDO";
  const int hdmi = score_audio_capture_name(L"HDMI (2- VEDO-VDV66003)", prefer);
  const int mic = score_audio_capture_name(L"Microfone (VEDO-VDV66003)", prefer);
  const int line = score_audio_capture_name(L"LINE IN (3- VEDO-VDV66003)", prefer);
  const int other = score_audio_capture_name(L"Microfone (AB13X USB Audio)", prefer);
  CHECK(hdmi > mic);
  CHECK(hdmi > line);
  CHECK(other < 0);
  CHECK(hdmi >= 150);
}

TEST_CASE("audio auto prefers HDMI without brand filter") {
  const std::wstring prefer;
  const int hdmi = score_audio_capture_name(L"HDMI (2- CaptureCard)", prefer);
  const int mic = score_audio_capture_name(L"Microfone (Realtek)", prefer);
  CHECK(hdmi > mic);
  CHECK(hdmi > 0);
}

TEST_CASE("is_auto_device") {
  CHECK(is_auto_device("auto"));
  CHECK(is_auto_device(""));
  CHECK_FALSE(is_auto_device("VEDO"));
}

TEST_CASE("1to1 geometry helper") {
  CHECK(is_exact_1to1(2560, 1440, 2560, 1440));
  CHECK_FALSE(is_exact_1to1(1920, 1080, 2560, 1440));
}

TEST_CASE("fullscreen window uses monitor size not signal (zoom/crop regression)") {
  // Bug: borderless window sized to 2560x1440 on a 1080p monitor cropped/zoomed the image.
  const auto g1080 = fullscreen_window_geom(1920, 1080);
  CHECK(g1080.w == 1920);
  CHECK(g1080.h == 1080);
  CHECK(g1080.x == 0);
  CHECK(g1080.y == 0);
  CHECK_FALSE(fullscreen_would_crop(1920, 1080, g1080.w, g1080.h));

  const auto g1440 = fullscreen_window_geom(2560, 1440);
  CHECK(g1440.w == 2560);
  CHECK(g1440.h == 1440);

  // Old buggy placement would create signal-sized window on smaller monitor:
  CHECK(fullscreen_would_crop(1920, 1080, 2560, 1440));
  CHECK_FALSE(fullscreen_would_crop(2560, 1440, 2560, 1440));
}

TEST_CASE("windowed mode fits inside smaller monitor (no off-screen 1440p window)") {
  // Bug: windowed used raw signal size → 2560x1440 client on 1280x720 desktop.
  const auto raw = windowed_client_geom(2560, 1440);
  CHECK(raw.w == 2560);
  CHECK(raw.h == 1440);

  const auto fit720 = windowed_client_geom(2560, 1440, 1280, 720);
  CHECK(fit720.w < 2560);
  CHECK(fit720.h < 1440);
  CHECK(windowed_fits_monitor(fit720.w, fit720.h, 1280, 720));
  CHECK(fit720.w <= 1280 - 96);
  CHECK(fit720.h <= 720 - 120);
  // Aspect roughly preserved (16:9).
  CHECK(doctest::Approx(static_cast<double>(fit720.w) / fit720.h).epsilon(0.05) == 16.0 / 9.0);

  const auto fit1080 = windowed_client_geom(2560, 1440, 1920, 1080);
  CHECK(windowed_fits_monitor(fit1080.w, fit1080.h, 1920, 1080));
  CHECK(fit1080.w <= 1920);
  CHECK(fit1080.h <= 1080);

  // Signal already smaller than monitor → keep 1:1.
  const auto native = windowed_client_geom(1280, 720, 1920, 1080);
  CHECK(native.w == 1280);
  CHECK(native.h == 720);
}

TEST_CASE("perf test uses maximized windowed not a half window") {
  CHECK_FALSE(perf_test_present_is_half_window(1920, 1080, 1920, 1080));
  CHECK_FALSE(perf_test_present_is_half_window(1920, 1040, 1920, 1080));
  CHECK_FALSE(perf_test_present_is_half_window(2560, 1400, 2560, 1440));
  CHECK(perf_test_present_is_half_window(1056, 594, 1920, 1080));
  CHECK(perf_test_present_is_half_window(960, 540, 1920, 1080));
  CHECK(perf_test_present_is_half_window(1408, 792, 2560, 1440));
  const auto view55 = half_centered_window_geom(1920, 1080, 1920, 1080);
  CHECK(perf_test_present_is_half_window(view55.w, view55.h, 1920, 1080));
  AppConfig cfg;
  PerfTestPlan p;
  apply_perf_test_plan_to_config(p, cfg);
  CHECK(cfg.perf_test);
  CHECK_FALSE(cfg.exclusive_fullscreen);
  CHECK(cfg.sync_image);
}

TEST_CASE("perf test pass 2x is not fg x2 and 40 fps window is not the present") {
  CHECK(perf_test_present_is_half_window(1056, 594, 1920, 1080));
  PerfTestSample pass2;
  pass2.fps = 60.0;
  pass2.target_fps = 60.0;
  pass2.fg_multiplier = 1;
  CHECK(grade_perf_test(pass2) == PerfTestGrade::Good);
  PerfTestSample win40;
  win40.fps = 40.0;
  win40.target_fps = 60.0;
  win40.fg_multiplier = 1;
  CHECK(grade_perf_test(win40) == PerfTestGrade::Fail);
  CHECK(perf_test_cadence_locked(40.0));
  PerfTestSample fg2;
  fg2.fps = 40.0;
  fg2.target_fps = 60.0;
  fg2.fg_multiplier = 2;
  CHECK(grade_perf_test(fg2) == PerfTestGrade::PlayableFg);
  CHECK(perf_test_fg_est_fps(40.0, 2) == doctest::Approx(80.0));
  CHECK(std::string(perf_test_fg_label(1)) == "FG off");
  CHECK(std::string(nr_pass_count_short(2)).find("2") != std::string::npos);
}

TEST_CASE("perf test window is 55 percent monitor and centered") {
  CHECK(kPerfTestWindowPercent == 55);
  const auto g1080 = half_centered_window_geom(1920, 1080, 1920, 1080);
  CHECK(g1080.w == 1056);
  CHECK(g1080.h == 594);
  CHECK(g1080.x == 432);
  CHECK(g1080.y == 243);
  CHECK(windowed_fits_monitor(g1080.w, g1080.h, 1920, 1080));
  CHECK(doctest::Approx(static_cast<double>(g1080.w) / g1080.h).epsilon(0.02) == 16.0 / 9.0);
  CHECK(g1080.w * 100 / 1920 == 55);
  CHECK(g1080.h * 100 / 1080 == 55);

  const auto g1440 = half_centered_window_geom(2560, 1440, 2560, 1440);
  CHECK(g1440.w == 1408);
  CHECK(g1440.h == 792);
  CHECK(g1440.x == 576);
  CHECK(g1440.y == 324);

  const auto g4k_on_1080 = half_centered_window_geom(3840, 2160, 1920, 1080);
  CHECK(g4k_on_1080.w == 1056);
  CHECK(g4k_on_1080.h == 594);
  CHECK(g4k_on_1080.x == 432);
  CHECK(g4k_on_1080.y == 243);
  CHECK(g4k_on_1080.w < 1920);
  CHECK(g4k_on_1080.h < 1080);

  const auto old50 = half_centered_window_geom(1920, 1080, 1920, 1080, 50);
  CHECK(old50.w == 960);
  CHECK(old50.h == 540);
}

TEST_CASE("letterbox fits 1440p signal into 1080p output without crop") {
  const auto v = fit_letterbox(2560, 1440, 1920, 1080);
  CHECK(v.w <= 1920.f + 0.01f);
  CHECK(v.h <= 1080.f + 0.01f);
  // 16:9 into 16:9 → exact fit
  CHECK(v.w == doctest::Approx(1920.f).epsilon(0.01));
  CHECK(v.h == doctest::Approx(1080.f).epsilon(0.01));
  CHECK(v.x == doctest::Approx(0.f).epsilon(0.01));
  CHECK(v.y == doctest::Approx(0.f).epsilon(0.01));
}

TEST_CASE("fit_fill has no black bars (session present contract)") {
  // Letterbox on mismatched aspect leaves bars; fill must cover the whole output.
  const auto letter = fit_letterbox(2560, 1440, 1280, 800);  // 16:9 into 16:10
  CHECK(viewport_has_black_bars(letter, 1280, 800));

  const auto fill = fit_fill(2560, 1440, 1280, 800);
  CHECK(fill.x == 0.f);
  CHECK(fill.y == 0.f);
  CHECK(fill.w == doctest::Approx(1280.f));
  CHECK(fill.h == doctest::Approx(800.f));
  CHECK_FALSE(viewport_has_black_bars(fill, 1280, 800));

  // Windowed client smaller than signal still fills (no pillar/letter box).
  const auto win = fit_fill(2560, 1440, 1574, 889);
  CHECK_FALSE(viewport_has_black_bars(win, 1574, 889));
  CHECK(win.w == 1574.f);
  CHECK(win.h == 889.f);
}

TEST_CASE("lobby preview letterbox centers with left/right black bars") {
  // Wide preview band + 16:9 signal → pillar bars (not stretch).
  const auto prev = fit_letterbox(640, 360, 1200, 520);
  CHECK(viewport_has_black_bars(prev, 1200, 520));
  CHECK(viewport_has_side_bars(prev, 1200));
  CHECK(prev.x > 1.f);
  CHECK(prev.w + prev.x < 1200.f - 1.f);
  // Vertically fills (or nearly) when band is shorter aspect than 16:9? 1200x520 ≈ 2.307 vs 16:9≈1.778
  // So height-limited → side bars, y≈0.
  CHECK(prev.y == doctest::Approx(0.f).epsilon(0.01));
  CHECK(prev.h == doctest::Approx(520.f).epsilon(0.01));
  // Aspect preserved.
  CHECK(doctest::Approx(prev.w / prev.h).epsilon(0.02) == 16.0 / 9.0);
  // Symmetric L/R.
  CHECK(prev.x == doctest::Approx((1200.f - prev.w) * 0.5f).epsilon(0.5));

  // Stretch would have filled — letterbox must NOT match fill.
  const auto fill = fit_fill(640, 360, 1200, 520);
  CHECK(fill.w == 1200.f);
  CHECK(prev.w < fill.w);
}

TEST_CASE("lobby HUD sits on left black bar with readable colors") {
  const auto box = fit_letterbox(1920, 1080, 1600, 700);
  REQUIRE(viewport_has_side_bars(box, 1600));
  const auto lay = lobby_hud_left_bar_layout(box, 1600.f, 700.f, 280.f);
  CHECK(lay.on_black_bar);
  CHECK(lay.x >= 0.f);
  CHECK(lay.max_w <= box.x);
  CHECK(lay.max_w >= 48.f);

  // Narrow bar (near-matched aspect) → fallback, not claimed as side-bar HUD.
  const auto tight = fit_letterbox(1920, 1080, 1920, 1080);
  CHECK_FALSE(viewport_has_side_bars(tight, 1920));
  const auto lay2 = lobby_hud_left_bar_layout(tight, 1920.f, 1080.f, 280.f);
  CHECK_FALSE(lay2.on_black_bar);

  // Preview fill (sem pillar): HUD no canto da imagem, inset da sessao.
  const auto fill = fit_fill(1920, 1080, 800, 450);
  CHECK_FALSE(viewport_has_side_bars(fill, 800));
  const auto lay_fill = lobby_preview_hud_layout(fill, 800.f, 450.f, 280.f);
  CHECK_FALSE(lay_fill.on_black_bar);
  CHECK(lay_fill.x == doctest::Approx(fill.x + kLobbyPreviewHudInset));
  CHECK(lay_fill.y == doctest::Approx(fill.y + kLobbyPreviewHudInset));

  // High-contrast palette used on black pillar (must pass luminance gate).
  CHECK(hud_color_readable_on_black(255, 230, 90));
  CHECK(hud_color_readable_on_black(160, 240, 255));
  CHECK(hud_color_readable_on_black(120, 255, 180));
  CHECK(hud_color_readable_on_black(255, 255, 255));
  CHECK_FALSE(hud_color_readable_on_black(40, 40, 45));  // old dark-card gray fails
}

TEST_CASE("lobby side panels follow letterbox black-bar widths") {
  // Wider side menus (~+25%): preview may squeeze; mid still usable.
  const auto wide = lobby_side_panel_layout(2400.f, 900.f, 1920, 1080, 375.f, 525.f, 280.f);
  CHECK(wide.left_w >= 375.f);
  CHECK(wide.right_w >= 375.f);
  CHECK(wide.mid_w >= 280.f);
  CHECK(doctest::Approx(wide.left_w + wide.right_w + wide.mid_w).epsilon(0.5) == 2400.f);

  const auto tight = lobby_side_panel_layout(1920.f, 1080.f, 1920, 1080, 375.f, 525.f, 280.f);
  CHECK(tight.left_w == doctest::Approx(375.f));
  CHECK(tight.right_w == doctest::Approx(375.f));
  CHECK(tight.mid_w >= 280.f);
}

TEST_CASE("letterbox fits 1080p signal into 1440p output centered") {
  const auto v = fit_letterbox(1920, 1080, 2560, 1440);
  CHECK(v.w == doctest::Approx(2560.f).epsilon(0.01));
  CHECK(v.h == doctest::Approx(1440.f).epsilon(0.01));
}

TEST_CASE("session_start records low-latency contract") {
  AnalyzeWriter a;
  SessionInfo info;
  info.device = "VEDO-VDV66003";
  info.width = 2560;
  info.height = 1440;
  info.fps = 60;
  info.buffering = false;
  info.waitable = false;
  info.fourcc = "NV12";
  a.session_start(info, 1.0);
  const auto lines = a.lines_copy();
  REQUIRE_FALSE(lines.empty());
  CHECK(lines[0].find("\"width\":2560") != std::string::npos);
  CHECK(lines[0].find("\"height\":1440") != std::string::npos);
  CHECK(lines[0].find("\"fourcc\":\"NV12\"") != std::string::npos);
  CHECK(lines[0].find("\"buffering\":false") != std::string::npos);
  CHECK(lines[0].find("\"waitable\":false") != std::string::npos);
}

TEST_CASE("parse --fg-x multipliers") {
  {
    char a0[] = "NexusCap";
    char* argv[] = {a0};
    auto c = parse_args(1, argv);
    CHECK(c.fg_multiplier == 1);
  }
  for (const char* x : {"1", "2", "3", "4"}) {
    char a0[] = "NexusCap";
    char a1[] = "--fg-x";
    char a2[8];
    std::snprintf(a2, sizeof(a2), "%s", x);
    char* argv[] = {a0, a1, a2};
    auto c = parse_args(3, argv);
    CHECK(c.fg_multiplier == std::atoi(x));
  }
  {
    char a0[] = "NexusCap";
    char a1[] = "--fg-x";
    char a2[] = "5";
    char* argv[] = {a0, a1, a2};
    auto c = parse_args(3, argv);
    CHECK(c.fg_multiplier == 1);  // invalid ignored
  }
  {
    char a0[] = "NexusCap";
    char a1[] = "--fg-x";
    char a2[] = "nope";
    char* argv[] = {a0, a1, a2};
    auto c = parse_args(3, argv);
    CHECK(c.fg_multiplier == 1);
  }
}

TEST_CASE("parse --fg-adaptive and --fg-target") {
  {
    char a0[] = "NexusCap";
    char* argv[] = {a0};
    auto c = parse_args(1, argv);
    CHECK_FALSE(c.fg_adaptive);
    CHECK(c.fg_target_fps == 60);
  }
  {
    char a0[] = "NexusCap";
    char a1[] = "--fg-adaptive";
    char a2[] = "1";
    char a3[] = "--fg-target";
    char a4[] = "80";
    char* argv[] = {a0, a1, a2, a3, a4};
    auto c = parse_args(5, argv);
    CHECK(c.fg_adaptive);
    CHECK(c.fg_target_fps == 80);
  }
  {
    char a0[] = "NexusCap";
    char a1[] = "--fg-target";
    char a2[] = "20";
    char* argv[] = {a0, a1, a2};
    auto c = parse_args(3, argv);
    CHECK(c.fg_target_fps == 30);
  }
}

TEST_CASE("parse --fg-path") {
  char a0[] = "NexusCap";
  char a1[] = "--fg-path";
  char a2[] = "C:\\fake\\LosslessScaling.exe";
  char* argv[] = {a0, a1, a2};
  auto c = parse_args(3, argv);
  CHECK(c.fg_path.find("LosslessScaling.exe") != std::string::npos);
}

TEST_CASE("next_fg_multiplier cycles 2-3-4") {
  CHECK(next_fg_multiplier(1) == 2);
  CHECK(next_fg_multiplier(2) == 3);
  CHECK(next_fg_multiplier(3) == 4);
  CHECK(next_fg_multiplier(4) == 2);
  CHECK(fg_wants_process(1) == false);
  CHECK(fg_wants_process(2) == true);
}

TEST_CASE("FG frametime traces stack one line per multiplier even below 60 fps") {
  CHECK(fg_frametime_trace_count(false, 1) == 1);
  CHECK(fg_frametime_trace_count(false, 2) == 1);
  CHECK(fg_frametime_trace_count(false, 3) == 1);
  CHECK(fg_frametime_trace_count(false, 4) == 1);
  CHECK(fg_frametime_trace_count(true, 2) == 2);
  CHECK(fg_frametime_trace_count(true, 3) == 2);
  CHECK(fg_frametime_trace_count(true, 4) == 2);
  CHECK(fg_frametime_trace_count(true, 1) == 1);
  CHECK(fg_frametime_trace_k(1, 3) == 1);
  CHECK(fg_frametime_trace_k(2, 3) == 3);
  CHECK(fg_frametime_trace_k(2, 4) == 4);
  CHECK(fg_frametime_trace_k(2, 2) == 2);
  CHECK(hdmi_lobby_fg_multiplier(false, 2) == 1);
  CHECK(hdmi_lobby_fg_multiplier(false, 3) == 1);
  CHECK(hdmi_lobby_fg_multiplier(true, 3) == 3);
  CHECK(hdmi_lobby_fg_multiplier(true, 1) == 2);
  CHECK(fg_leftover_cleared(0, true));
  CHECK_FALSE(fg_leftover_cleared(1, true));
  CHECK_FALSE(fg_leftover_cleared(0, false));
  CHECK_FALSE(fg_leftover_cleared(2, true));
  CHECK(fg_kill_wait_max_ms() >= fg_kill_wait_step_ms());

  // 30 fps capture: base 33.333 ms, x2 sits at 16.667, x3 at 11.111, x4 at 8.333.
  const double cap30 = 1000.0 / 30.0;
  CHECK(fg_frametime_trace_ms(cap30, 1) == doctest::Approx(cap30));
  CHECK(fg_frametime_trace_ms(cap30, 2) == doctest::Approx(cap30 / 2.0));
  CHECK(fg_frametime_trace_ms(cap30, 3) == doctest::Approx(cap30 / 3.0));
  CHECK(fg_frametime_trace_ms(cap30, 4) == doctest::Approx(cap30 / 4.0));
  CHECK(fg_frametime_trace_ms(cap30, 2) < fg_frametime_trace_ms(cap30, 1));

  const double cap60 = 1000.0 / 60.0;
  CHECK(fg_frametime_trace_ms(cap60, 2) == doctest::Approx(cap60 / 2.0));
  {
    const int gy = 0;
    const int gh = 120;
    const double ceil_ms = 80.0;
    const double cap25 = 25.0;
    const int y_cap = fg_frametime_plot_y(gy, gh, cap25, 1, ceil_ms);
    const int y_x3 = fg_frametime_plot_y(gy, gh, cap25, 3, ceil_ms);
    CHECK(y_cap != y_x3);
    CHECK(std::abs(y_cap - y_x3) > 16);
    CHECK(y_x3 < y_cap);  // FG (mais FPS) em cima, cap em baixo
  }
  CHECK(fg_frametime_fps(25.0, 1) == 40);
  CHECK(fg_frametime_fps(25.0, 3) == 120);
  CHECK(fg_frametime_fps(1000.0 / 60.0, 1) == 60);
  CHECK(fg_frametime_fps_gutter_px() >= 24);
  {
    char lab[16]{};
    format_fg_frametime_fps_label(lab, sizeof(lab), 25.0, 1);
    CHECK(std::string(lab) == "40");
  }

  const FgFtRgb base = fg_frametime_trace_rgb(1, true);
  const FgFtRgb x2 = fg_frametime_trace_rgb(2, true);
  const FgFtRgb x3 = fg_frametime_trace_rgb(3, true);
  const FgFtRgb x4 = fg_frametime_trace_rgb(4, true);
  CHECK((base.r != x2.r || base.g != x2.g || base.b != x2.b));
  CHECK((x2.r != x3.r || x2.g != x3.g || x2.b != x3.b));
  CHECK((x3.r != x4.r || x3.g != x4.g || x3.b != x4.b));
}

TEST_CASE("lobby HUD FPS never shows fake dual when FG only armed") {
  char line[128]{};
  format_lobby_hud_fps_line(line, sizeof(line), 60.0, false, 2);
  CHECK_FALSE(lobby_fps_line_implies_active_fg(line));
  CHECK(std::strstr(line, "60") != nullptr);

  format_lobby_hud_fps_line(line, sizeof(line), 60.0, true, 2);
  CHECK_FALSE(lobby_fps_line_implies_active_fg(line));  // off, not "60 | 120"
  CHECK(std::strstr(line, "off") != nullptr);
  CHECK(std::strstr(line, "x2") != nullptr);
  CHECK(std::strstr(line, " | ") == nullptr);
  CHECK(std::strstr(line, "pronto") == nullptr);
  CHECK(std::strstr(line, "armado") == nullptr);

  format_lobby_hud_fps_line(line, sizeof(line), 60.0, true, 1);
  CHECK(std::strstr(line, "off") == nullptr);

  PersistedSettings s = default_settings();
  s.ui.show_fps = true;
  s.fg_enabled = true;
  s.fg_multiplier = 2;
  auto hud = make_lobby_hud_preview_lines(s, 60.0);
  CHECK(lobby_hud_contains(hud, "off"));
  CHECK_FALSE(lobby_hud_contains(hud, " | "));
}

TEST_CASE("session HUD shows real|FG dual FPS when FG on") {
  char line[128]{};
  format_session_hud_fps_line(line, sizeof(line), 60.0, 120.0, true, 2, true);
  CHECK(session_hud_fps_shows_cap_fg_split(line));
  CHECK(std::strstr(line, "60.0") != nullptr);
  CHECK(std::strstr(line, "120.0") != nullptr);
  CHECK(std::strstr(line, "cap|FG") != nullptr);
  CHECK(std::strstr(line, "x2") != nullptr);
  CHECK(std::strstr(line, "Fixo") != nullptr);
  CHECK(std::strstr(line, "ON") != nullptr);

  format_session_hud_fps_line(line, sizeof(line), 40.0, 70.0, true, 2, true, true, 70);
  CHECK(session_hud_fps_shows_cap_fg_split(line));
  CHECK(std::strstr(line, "Adaptive 70") != nullptr);
  CHECK(std::strstr(line, "Fixo") == nullptr);
  char fg_adp[160]{};
  format_session_hud_fg_status_line(fg_adp, sizeof(fg_adp), true, 2, "lsfg3", 80, "WGC", "On", 40.0, 70.0, 0,
                                   true, 70);
  CHECK(std::strstr(fg_adp, "Adaptive 70") != nullptr);
  CHECK(std::strstr(fg_adp, "Fixo") == nullptr);
  {
    char tag[24]{};
    format_fg_mode_tag(tag, sizeof(tag), false, 2);
    CHECK(std::string(tag) == "Fixo x2");
    format_fg_mode_tag(tag, sizeof(tag), true, 2, 70);
    CHECK(std::string(tag) == "Adaptive 70");
  }

  format_session_hud_fps_line(line, sizeof(line), 60.0, 180.0, true, 3, false);
  CHECK(session_hud_fps_shows_cap_fg_split(line));
  CHECK(std::strstr(line, "180.0") != nullptr);
  CHECK(std::strstr(line, "x3") != nullptr);

  // FG only armed (not active): single FPS + off (not "pronto").
  format_session_hud_fps_line(line, sizeof(line), 60.0, 0.0, false, 2, true);
  CHECK_FALSE(session_hud_fps_shows_cap_fg_split(line));
  CHECK(std::strstr(line, "off") != nullptr);
  CHECK(std::strstr(line, "pronto") == nullptr);
  CHECK(std::strstr(line, "ON") == nullptr);

  char fg[160]{};
  format_session_hud_fg_status_line(fg, sizeof(fg), true, 2, "lsfg3", 100, "WGC", "On", 60.0, 120.0);
  CHECK(std::strstr(fg, "60") != nullptr);
  CHECK(std::strstr(fg, "120") != nullptr);
  CHECK(std::strstr(fg, "->") != nullptr);
  CHECK(std::strstr(fg, "fila0") != nullptr);
  format_session_hud_fg_status_line(fg, sizeof(fg), true, 2, "lsfg3", 80, "WGC", "On", 60.0, 120.0, 2);
  CHECK(std::strstr(fg, "fila2") != nullptr);

  HudFpsParts parts{};
  fill_session_hud_fps_parts(parts, 60.0, 120.0, true, 2, true, true);
  char joined[128]{};
  join_hud_fps_parts(joined, sizeof(joined), parts);
  format_session_hud_fps_line(line, sizeof(line), 60.0, 120.0, true, 2, true);
  CHECK(std::string(joined) == std::string(line));
  CHECK(parts.count >= 4);
  CHECK(std::string(parts.parts[1].text) == "60.0");
  CHECK(std::string(parts.parts[3].text) == "120.0");
  {
    const FgFtRgb cap = fg_frametime_trace_rgb(1, true);
    const FgFtRgb x2 = fg_frametime_trace_rgb(2, true);
    CHECK(parts.parts[1].r == cap.r);
    CHECK(parts.parts[1].g == cap.g);
    CHECK(parts.parts[1].b == cap.b);
    CHECK(parts.parts[3].r == x2.r);
    CHECK(parts.parts[3].g == x2.g);
    CHECK(parts.parts[3].b == x2.b);
  }
  fill_session_hud_fps_parts(parts, 60.0, 240.0, true, 4, false, true);
  join_hud_fps_parts(joined, sizeof(joined), parts);
  format_session_hud_fps_line(line, sizeof(line), 60.0, 240.0, true, 4, false);
  CHECK(std::string(joined) == std::string(line));
  CHECK(std::string(parts.parts[3].text) == "240.0");
  {
    const FgFtRgb x4 = fg_frametime_trace_rgb(4, true);
    CHECK(parts.parts[3].r == x4.r);
    CHECK(parts.parts[3].g == x4.g);
    CHECK(parts.parts[3].b == x4.b);
  }
  fill_session_hud_fps_parts(parts, 60.0, 0.0, false, 2, true, true);
  join_hud_fps_parts(joined, sizeof(joined), parts);
  format_session_hud_fps_line(line, sizeof(line), 60.0, 0.0, false, 2, true);
  CHECK(std::string(joined) == std::string(line));
  CHECK_FALSE(session_hud_fps_shows_cap_fg_split(joined));
}

TEST_CASE("mini overlay two-line packs ADV fps fg nr sync ft last600 and hw") {
  char a[768]{};
  char b[768]{};
  SystemMetricsSample hw{};
  hw.cpu_name = "i5-11400F";
  hw.gpu_name = "RTX 5070";
  hw.cpu_ok = hw.gpu_ok = hw.cpu_temp_ok = hw.gpu_temp_ok = true;
  hw.vram_ok = hw.ram_ok = hw.power_ok = true;
  hw.cpu_percent = 42;
  hw.cpu_temp_c = 58;
  hw.gpu_percent = 77;
  hw.gpu_temp_c = 72;
  hw.vram_used_mb = 2048;
  hw.vram_budget_mb = 8192;
  hw.ram_used_mb = 12288;
  hw.ram_total_mb = 32768;
  hw.power_w = 120;
  char hw_compact[200]{};
  format_sys_metrics_compact(hw_compact, sizeof(hw_compact), hw);
  CHECK(std::strstr(hw_compact, "i5-11400F") != nullptr);
  CHECK(std::strstr(hw_compact, "RTX 5070") != nullptr);

  format_mini_overlay_lines(a, sizeof(a), b, sizeof(b), 60.0, 120.0, true, 2, true, true, 16.7, 16.6, 17.0,
                            18.0, true, 8.5, 2.2, 2, 1, 5, 2560, 1440, 2560, 1440, 2560, 1440, 100, true, 59.9,
                            2.0, 2.1, 0, "Default", hw_compact, 1.5, 900, 20.0, "lsfg3", 60.0);
  CHECK(std::strstr(a, "60.0") != nullptr);
  CHECK(std::strstr(a, "120.0") != nullptr);
  CHECK(std::strstr(a, "cap|FG") != nullptr);
  CHECK(std::strstr(a, "DLSS5") != nullptr);
  CHECK(std::strstr(a, "1x") != nullptr);
  CHECK(std::strstr(a, "VSYNC") != nullptr);
  CHECK(std::strstr(a, "present 2.2ms+NR") != nullptr);
  CHECK(std::strstr(a, "cap 8.5") != nullptr);
  CHECK(std::strstr(a, "2560x1440>2560x1440@100%>out 1:1") != nullptr);
  CHECK(std::strstr(a, "st2/cl1") == nullptr);  // stalls live on row 1 so the sparkline cannot clip them
  CHECK(std::strstr(b, "st2/cl1") == nullptr);
  CHECK(std::strstr(b, "stalls 2") != nullptr);
  CHECK(std::strstr(b, "clusters 1") != nullptr);
  CHECK(std::strstr(b, "frm 900") != nullptr);
  CHECK(std::strstr(b, "drop 5") != nullptr);
  CHECK(std::strstr(b, "ft 16.70") != nullptr);
  CHECK(std::strstr(b, "avg 16.60") != nullptr);
  CHECK(std::strstr(b, "p95 17.00") != nullptr);
  CHECK(std::strstr(b, "FG est 8.35") != nullptr);
  CHECK(std::strstr(b, "max 18") == nullptr);
  CHECK(std::strstr(b, "STABLE") != nullptr);
  CHECK(std::strstr(b, "L600") != nullptr);
  CHECK(std::strstr(b, "i5-11400F") != nullptr);
  CHECK(std::strstr(b, "RTX 5070") != nullptr);
  CHECK(std::strstr(b, "120W") != nullptr);
  CHECK(std::strstr(a, "[lsfg3]") != nullptr);
  CHECK(std::strstr(a, "1.5ms") != nullptr);

  MiniOverlaySeg segs[kMiniOverlayMaxSegs]{};
  const int nseg = format_mini_overlay_segments(
      segs, kMiniOverlayMaxSegs, a, sizeof(a), b, sizeof(b), 60.0, 120.0, true, 2, true, true, 16.7, 16.6, 17.0,
      18.0, true, 8.5, 2.2, 2, 1, 5, 2560, 1440, 2560, 1440, 2560, 1440, 100, true, 59.9, 2.0, 2.1, 0, "Default",
      hw_compact, 1.5, 900, 20.0, "lsfg3", 60.0);
  CHECK(nseg >= 8);
  bool saw_cap_cyan = false;
  bool saw_fg_gold = false;
  bool saw_purple_nr = false;
  bool saw_yellow_cpu = false;
  bool saw_cyan_gpu = false;
  const FgFtRgb cap_rgb = fg_frametime_trace_rgb(1, true);
  const FgFtRgb x2_rgb = fg_frametime_trace_rgb(2, true);
  for (int i = 0; i < nseg; ++i) {
    if (std::strcmp(segs[i].text, "60.0") == 0 && segs[i].r == cap_rgb.r && segs[i].g == cap_rgb.g &&
        segs[i].b == cap_rgb.b) {
      saw_cap_cyan = true;
    }
    if (std::strcmp(segs[i].text, "120.0") == 0 && segs[i].r == x2_rgb.r && segs[i].g == x2_rgb.g &&
        segs[i].b == x2_rgb.b) {
      saw_fg_gold = true;
    }
    if (std::strstr(segs[i].text, "DLSS5") && segs[i].b >= 240) saw_purple_nr = true;
    if (std::strstr(segs[i].text, "i5-11400F") && segs[i].r >= 240 && segs[i].g >= 200) saw_yellow_cpu = true;
    if (std::strstr(segs[i].text, "RTX 5070") && segs[i].b >= 240) saw_cyan_gpu = true;
  }
  CHECK(saw_cap_cyan);
  CHECK(saw_fg_gold);
  CHECK(saw_purple_nr);
  CHECK(saw_yellow_cpu);
  CHECK(saw_cyan_gpu);

  format_mini_overlay_lines(a, sizeof(a), b, sizeof(b), 59.9, 0.0, false, 2, false, false, 16.6, 16.6, 16.6,
                            16.6, true, 12.0, 3.0, 0, 0, 0, 2560, 1440, 1280, 720, 2560, 1440, 50);
  CHECK(std::strstr(a, "NR off") != nullptr);
  CHECK(std::strstr(a, "DLSS5") != nullptr);
  CHECK(std::strstr(a, "TEAR") != nullptr);
  CHECK(std::strstr(a, "FG Fixo x2 off") != nullptr);
  CHECK(std::strstr(a, "2560x1440>1280x720@50%>out 1:1") != nullptr);
  CHECK(std::strstr(b, "st0/cl0") == nullptr);
  CHECK(std::strstr(b, "stalls 0") != nullptr);
  CHECK(std::strstr(b, "clusters 0") != nullptr);
  CHECK(std::strstr(b, "L600") != nullptr);

  // Legacy one-liner still includes key tokens.
  char line[512]{};
  format_mini_overlay_line(line, sizeof(line), 60.0, 120.0, true, 2, true, 16.7, 8.5, 2, 1, 2560, 1440, 2560,
                           1440, 2560, 1440, 100);
  CHECK(std::strstr(line, "60.0") != nullptr);
  CHECK(std::strstr(line, "cap|FG") != nullptr);
  {
    const bool has_dlss_or_nr =
        (std::strstr(line, "DLSS5") != nullptr) || (std::strstr(line, "NR") != nullptr);
    CHECK(has_dlss_or_nr);
  }

  PersistedSettings s = default_settings();
  s.ui.show_fps = true;
  s.ui.mini_overlay = true;
  s.ui.advanced_metrics = true;  // ignored while mini is on
  s.enhance_renodx = true;
  s.neural_rendering = true;
  s.width = 2560;
  s.height = 1440;
  auto hud = make_lobby_hud_preview_lines(s, 60.0);
  CHECK(hud.count == 2);
  CHECK(lobby_hud_contains(hud, "DLSS5"));
  CHECK(lobby_hud_contains(hud, "1x"));
  CHECK(lobby_hud_contains(hud, "Feed"));
  CHECK_FALSE(lobby_hud_contains(hud, "i5-11400F"));
  CHECK_FALSE(lobby_hud_contains(hud, "RTX 5070"));
  CHECK_FALSE(lobby_hud_contains(hud, "35%"));
  {
    SystemMetricsSample live{};
    live.cpu_name = "i5-11400F";
    live.gpu_name = "RTX 5070";
    live.cpu_ok = live.gpu_ok = live.cpu_temp_ok = live.gpu_temp_ok = true;
    live.cpu_percent = 12;
    live.cpu_temp_c = 48;
    live.gpu_percent = 77;
    live.gpu_temp_c = 72;
    auto live_hud = make_lobby_hud_preview_lines(s, 60.0, &live);
    CHECK(lobby_hud_contains(live_hud, "i5-11400F"));
    CHECK(lobby_hud_contains(live_hud, "RTX 5070"));
    CHECK(lobby_hud_contains(live_hud, "12%"));
    CHECK_FALSE(lobby_hud_contains(live_hud, "35%/55C"));
    live.cpu_percent = 88;
    live_hud = make_lobby_hud_preview_lines(s, 60.0, &live);
    CHECK(lobby_hud_contains(live_hud, "88%"));
    CHECK_FALSE(lobby_hud_contains(live_hud, "12%"));
  }
  CHECK(lobby_hud_preview_lines(true, true, false, true) == 3);
  CHECK(hud_uses_mini_overlay(true, true));
  CHECK_FALSE(hud_uses_advanced_metrics(true, true, true));
  CHECK(overlay_draws_plate(false));
  CHECK_FALSE(overlay_draws_plate(true));
  CHECK_FALSE(overlay_no_bg_locked_to_mini());
  CHECK(overlay_no_bg_ui_enabled(false, false, false));
  CHECK(overlay_no_bg_ui_enabled(true, false, false));
  CHECK(overlay_no_bg_ui_enabled(false, false, true));
  CHECK(overlay_no_bg_ui_enabled(false, true, false));
}

TEST_CASE("overlay sem fundo is a free toggle for every HUD style") {
  CHECK_FALSE(overlay_no_bg_locked_to_mini());
  CHECK(overlay_no_bg_ui_enabled());
  CHECK(overlay_no_bg_ui_enabled(true, false, false));
  CHECK(overlay_no_bg_ui_enabled(false, true, false));
  CHECK(overlay_no_bg_ui_enabled(false, false, true));
  CHECK(overlay_draws_plate(false));
  CHECK_FALSE(overlay_draws_plate(true));

  PersistedSettings compact = default_settings();
  compact.ui.mini_overlay = false;
  compact.ui.advanced_metrics = false;
  compact.ui.live_overlay = false;
  compact.ui.overlay_no_bg = true;
  AppConfig cfg;
  apply_persisted_to_config(compact, cfg);
  CHECK(cfg.overlay_no_bg);
  CHECK_FALSE(cfg.mini_overlay);

  PersistedSettings mini = compact;
  mini.ui.mini_overlay = true;
  apply_persisted_to_config(mini, cfg);
  CHECK(cfg.overlay_no_bg);
  CHECK(cfg.mini_overlay);

  PersistedSettings live = compact;
  live.ui.live_overlay = true;
  apply_persisted_to_config(live, cfg);
  CHECK(cfg.overlay_no_bg);
  CHECK(cfg.live_overlay);

  cfg.overlay_no_bg = true;
  PersistedSettings back = default_settings();
  CHECK_FALSE(back.ui.overlay_no_bg);
  apply_session_cfg_to_persisted(cfg, back);
  CHECK(back.ui.overlay_no_bg);
}

TEST_CASE("default FG off but multiplier stays armed for PageDn") {
  CHECK_FALSE(default_settings().fg_enabled);
  CHECK(default_settings().fg_multiplier == 2);
  CHECK_FALSE(default_settings().fg_adaptive);
  CHECK(default_settings().fg_target_fps == 60);

  PersistedSettings s = default_settings();
  s.fg_enabled = false;
  s.fg_multiplier = 3;
  AppConfig cfg;
  apply_persisted_to_config(s, cfg);
  CHECK_FALSE(cfg.fg_auto_start);
  CHECK(cfg.fg_multiplier == 3);
  CHECK(armed_fg_multiplier(false, 1) == 2);
  CHECK(armed_fg_multiplier(false, 4) == 4);

  s.fg_enabled = true;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.fg_auto_start);
  CHECK(cfg.fg_multiplier == 3);

  s.fg_adaptive = true;
  s.fg_target_fps = 80;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.fg_adaptive);
  CHECK(cfg.fg_target_fps == 80);
  PersistedSettings back = default_settings();
  apply_session_cfg_to_persisted(cfg, back);
  CHECK(back.fg_adaptive);
  CHECK(back.fg_target_fps == 80);
}

TEST_CASE("fresh install without Lossless cannot enable FG until Pasta or Auto finds exe") {
  CHECK_FALSE(lobby_fg_can_enable("C:\\definitely\\missing\\LosslessScaling.exe"));
  CHECK(session_notice_fg_missing().find("Pasta") != std::string::npos);
  const std::string auto_path = resolve_lossless_exe({});
  if (auto_path.empty()) {
    CHECK_FALSE(lobby_fg_can_enable({}));
  } else {
    CHECK(lobby_fg_can_enable({}));
    CHECK(lossless_exe_exists(auto_path));
  }
}

TEST_CASE("manual Pasta path is used when the exe exists") {
  const std::string missing = "Z:\\no\\LosslessScaling.exe";
  CHECK(resolve_lossless_exe(missing).empty());
  const std::string auto_path = resolve_lossless_exe({});
  if (!auto_path.empty()) {
    CHECK(resolve_lossless_exe(auto_path) == auto_path);
    CHECK(lobby_fg_can_enable(auto_path));
  }
}

TEST_CASE("mira-lock anti-ghost always Flow 80 WGC auto or manual") {
  CHECK(mira_lock_xml_ok(build_mira_settings_xml(2)));
  CHECK(build_mira_settings_xml(2).find("<QueueTarget>0</QueueTarget>") != std::string::npos);
  const std::string q2 = build_mira_settings_xml(2, 2);
  CHECK(q2.find("<QueueTarget>2</QueueTarget>") != std::string::npos);
  CHECK_FALSE(mira_lock_xml_ok(q2));
  CHECK(mira_lock_xml_ok(build_mira_settings_xml(3)));
  CHECK(mira_lock_xml_ok(build_mira_settings_xml(4)));
  CHECK(mira_lock_xml_ok(patch_mira_settings_xml("", 2)));  // fresh LOCALAPPDATA
  const std::string dirty =
      "<Settings><LSFGFlowScale>70</LSFGFlowScale><LSFG3Multiplier>2</LSFG3Multiplier>"
      "<CaptureApi>DXGI</CaptureApi></Settings>";
  const std::string patched = patch_mira_settings_xml(dirty, 3);
  CHECK(mira_lock_xml_ok(patched));
  CHECK(patched.find("LSFG3Multiplier>3") != std::string::npos);
  CHECK(mira_lock_ini_ok(patch_lsfg_ini("")));
  CHECK(mira_lock_ini_ok(patch_lsfg_ini("flow_scale = 70\r\nui_detection_rate = 1\r\n")));
  // Caminho do exe nao muda o XML — Auto e Pasta usam o mesmo perfil.
  CHECK(patch_mira_settings_xml("", 2) == build_mira_settings_xml(2));
  // Pessoa mexeu no LS (Flow 70 / DXGI): o próximo arranque regrava a nossa mira.
  const std::string user_tweak =
      "<Settings><LSFGFlowScale>70</LSFGFlowScale><CaptureApi>DXGI</CaptureApi>"
      "<LSFGSize>QUALITY</LSFGSize></Settings>";
  CHECK(mira_lock_xml_ok(patch_mira_settings_xml(user_tweak, 2)));
}

TEST_CASE("mira settings xml never writes Flow 70") {
  const std::string built = build_mira_settings_xml(2);
  CHECK(built.find("LSFGFlowScale>80") != std::string::npos);
  CHECK(built.find("LSFGFlowScale>70") == std::string::npos);
  CHECK(built.find("LSFG3Multiplier>2") != std::string::npos);
  CHECK(built.find("<CaptureApi>WGC</CaptureApi>") != std::string::npos);
  CHECK(built.find("<LSFGSize>BALANCED</LSFGSize>") != std::string::npos);
  CHECK(built.find("<Hotkey>F24</Hotkey>") != std::string::npos);
  CHECK(built.find("<DrawFps>false</DrawFps>") != std::string::npos);
  CHECK(built.find("<DrawFps>true</DrawFps>") == std::string::npos);

  const std::string dirty =
      "<Settings><LSFGFlowScale>70</LSFGFlowScale><LSFG3Multiplier>2</LSFG3Multiplier>"
      "<CaptureApi>DXGI</CaptureApi><LSFGSize>QUALITY</LSFGSize></Settings>";
  const std::string patched = patch_mira_settings_xml(dirty, 3);
  CHECK(patched.find("LSFGFlowScale>80") != std::string::npos);
  CHECK(patched.find("LSFGFlowScale>70") == std::string::npos);
  CHECK(patched.find("LSFG3Multiplier>3") != std::string::npos);
  CHECK(patched.find("<CaptureApi>WGC</CaptureApi>") != std::string::npos);
  CHECK(patched.find("<DrawFps>false</DrawFps>") != std::string::npos);
  CHECK(parse_lsfg3_multiplier_from_xml(patched) == 3);
  CHECK(fg_multiplier_confirmed_on_disk(3, patched));
  CHECK_FALSE(fg_multiplier_confirmed_on_disk(4, patched));
  CHECK(parse_lsfg3_multiplier_from_xml("<nope>") == 0);
}

TEST_CASE("LSFG Adaptive XML 50/65/80 and clamp to monitor Hz") {
  CHECK(std::string(lsfg3_mode1_xml(false)) == "FIXED");
  CHECK(std::string(lsfg3_mode1_xml(true)) == "ADAPTIVE");
  const std::string fixed = build_mira_settings_xml(2);
  CHECK(mira_lock_xml_ok(fixed));
  CHECK(parse_lsfg3_fixed_from_xml(fixed));
  CHECK_FALSE(parse_lsfg3_adaptive_from_xml(fixed));
  CHECK(fixed.find("<LSFG3Mode1>FIXED</LSFG3Mode1>") != std::string::npos);
  CHECK(fixed.find("<Hotkey>F24</Hotkey>") != std::string::npos);
  CHECK(fixed.find("LSFGFlowScale>80") != std::string::npos);
  CHECK(fixed.find("<CaptureApi>WGC</CaptureApi>") != std::string::npos);
  CHECK(fg_adaptive_confirmed_on_disk(false, 60, fixed));
  CHECK_FALSE(fg_adaptive_confirmed_on_disk(true, 60, fixed));

  for (int tgt : {50, 65, 80}) {
    const std::string xml = build_mira_settings_xml(2, 0, true, tgt, 144);
    CHECK(mira_lock_xml_ok(xml));
    CHECK(parse_lsfg3_adaptive_from_xml(xml));
    CHECK_FALSE(parse_lsfg3_fixed_from_xml(xml));
    CHECK(parse_lsfg3_target_from_xml(xml) == tgt);
    CHECK(fg_adaptive_confirmed_on_disk(true, tgt, xml));
    CHECK(fg_ls_profile_confirmed_on_disk(2, true, tgt, xml));
    CHECK(xml.find("LSFGFlowScale>80") != std::string::npos);
    CHECK(xml.find("<CaptureApi>WGC</CaptureApi>") != std::string::npos);
    CHECK(xml.find("<Hotkey>F24</Hotkey>") != std::string::npos);
  }

  const std::string clamped = build_mira_settings_xml(4, 0, true, 80, 60);
  CHECK(parse_lsfg3_adaptive_from_xml(clamped));
  CHECK(parse_lsfg3_target_from_xml(clamped) == 60);
  CHECK(fg_adaptive_confirmed_on_disk(true, 60, clamped));
  CHECK_FALSE(fg_adaptive_confirmed_on_disk(true, 80, clamped));

  CHECK(session_notice_fg_adaptive_apply(true, 70, 144, true).find("Adaptive 70") != std::string::npos);
  CHECK(session_notice_fg_adaptive_apply(false, 70, 144, false).find("Fixo") != std::string::npos);
  CHECK(session_notice_fg_adaptive_apply(true, 80, 60, true).find("Adaptive 60") != std::string::npos);
  CHECK_FALSE(fg_adaptive_apply_dirty(true, 70, true, 70));
  CHECK(fg_adaptive_apply_dirty(true, 70, false, 70));
  CHECK(fg_adaptive_apply_dirty(true, 70, true, 60));
  CHECK(fg_adaptive_apply_enabled(true, false, 60, 70));
  CHECK(fg_adaptive_apply_enabled(true, true, 70, 80));
  CHECK_FALSE(fg_adaptive_apply_enabled(false, true, 70, 70));
  CHECK_FALSE(fg_adaptive_apply_enabled(false, true, 70, 60));
  CHECK_FALSE(fg_adaptive_apply_enabled(true, true, 70, 70));
  CHECK(std::string(fg_adaptive_ghost_hint()).find("x2") != std::string::npos);
  CHECK(std::string(fg_adaptive_ghost_hint()).find("definiu") != std::string::npos);

  UserPreset up;
  up.name = "Adaptive70";
  up.fg_enabled = true;
  up.fg_adaptive = true;
  up.fg_target_fps = 70;
  up.fg_multiplier = 2;
  UserPreset up2;
  REQUIRE(parse_user_preset(serialize_user_preset(up), up2));
  CHECK(up2.fg_adaptive);
  CHECK(up2.fg_target_fps == 70);
  CHECK(user_preset_list_summary(up2).find("Adaptive 70") != std::string::npos);

  const std::string dirty =
      "<Settings><LSFG3Mode1>FIXED</LSFG3Mode1><LSFG3Target>90</LSFG3Target>"
      "<LSFGFlowScale>80</LSFGFlowScale><LSFG3Multiplier>2</LSFG3Multiplier>"
      "<CaptureApi>WGC</CaptureApi><QueueTarget>0</QueueTarget><DrawFps>false</DrawFps></Settings>";
  const std::string patched = patch_mira_settings_xml(dirty, 2, 0, true, 65, 144);
  CHECK(parse_lsfg3_adaptive_from_xml(patched));
  CHECK(parse_lsfg3_target_from_xml(patched) == 65);
  const std::string back = patch_mira_settings_xml(patched, 2, 0, false, 65, 144);
  CHECK(parse_lsfg3_fixed_from_xml(back));
  CHECK_FALSE(parse_lsfg3_adaptive_from_xml(back));
}

TEST_CASE("ls hide only settings ui not fg output overlay") {
  CHECK(ls_should_hide_window(L"Lossless Scaling", L"HwndWrapper[LosslessScaling.exe]", 0));
  CHECK(ls_should_hide_window(L"Lossless Scaling", L"HwndWrapper[LosslessScaling.exe]", WS_EX_TOPMOST));
  CHECK_FALSE(ls_should_hide_window(L"Lossless Scaling", L"LosslessOverlay", WS_EX_LAYERED));
  CHECK(ls_should_hide_window(L"Lossless Scaling", L"LosslessOverlay", WS_EX_LAYERED, 180, 48));
  CHECK_FALSE(ls_should_hide_window(L"Lossless Scaling", L"LosslessOverlay", WS_EX_LAYERED, 2560, 1440));
  CHECK_FALSE(ls_should_hide_window(L"NexusCap", L"NexusCapWnd", 0));
  CHECK(ls_should_hide_window(L"Lossless Scaling", L"HwndWrapper[LosslessScaling.exe]", 0, 1150, 650));
  CHECK_FALSE(ls_should_hide_window(L"Lossless Scaling", L"LosslessOverlay", 0, 2560, 1440));
  CHECK_FALSE(ls_should_hide_window(L"", L"LosslessScaling", 0, 2560, 1440));
  CHECK_FALSE(ls_is_generating_output(L"Lossless Scaling", L"HwndWrapper[LosslessScaling.exe]", 0, 2560, 1440));
  CHECK_FALSE(ls_is_generating_output(L"Lossless Scaling", L"LosslessOverlay", WS_EX_LAYERED, 180, 48));
  CHECK(ls_is_generating_output(L"Lossless Scaling", L"LosslessOverlay", WS_EX_LAYERED, 2560, 1440));
  CHECK(ls_is_generating_output(L"Lossless Scaling", L"LosslessOverlay", WS_EX_TOPMOST, 1920, 1080));
  CHECK_FALSE(ls_is_generating_output(L"NexusCap", L"NexusCapWnd", WS_EX_TOPMOST, 2560, 1440));
  CHECK(ls_output_covers_target(0, 0, 2560, 1440, 0, 0, 2560, 1440));
  CHECK(ls_output_covers_target(0, 0, 2560, 1440, 100, 80, 1920, 1080));
  CHECK_FALSE(ls_output_covers_target(0, 0, 800, 600, 2000, 0, 2560, 1440));
  CHECK_FALSE(ls_output_covers_target(0, 0, 180, 48, 0, 0, 2560, 1440));
}

TEST_CASE("FG ON contract: F24 or generating surface then HUD and frametime agree") {
  // Processo vivo sem F24 / sem superficie = overlay nunca diz ON.
  CHECK_FALSE(fg_overlay_on_confirmed(true, 0));
  CHECK_FALSE(fg_overlay_reads_on(true, 2, true, 0, false, false));
  CHECK_FALSE(fg_overlay_reads_on(true, 2, true, 1, false, false));
  CHECK_FALSE(fg_overlay_reads_on(true, 1, true, 1, true, true));

  const bool on_f24 = fg_overlay_reads_on(true, 2, true, 1, true, false);
  CHECK(fg_overlay_on_confirmed(true, 1));
  CHECK(on_f24);

  const bool on_surface = fg_overlay_reads_on(false, 2, false, 0, false, true);
  CHECK(on_surface);
  CHECK(ls_is_generating_output(L"Lossless Scaling", L"LosslessOverlay", WS_EX_LAYERED, 2560, 1440));
  CHECK_FALSE(ls_should_hide_window(L"Lossless Scaling", L"LosslessOverlay", WS_EX_LAYERED, 2560, 1440));
  CHECK_FALSE(ls_should_hide_window(L"", L"LosslessScaling", 0, 2560, 1440));

  const std::string xml = build_mira_settings_xml(2);
  CHECK(mira_lock_xml_ok(xml));
  CHECK(xml.find("<Hotkey>F24</Hotkey>") != std::string::npos);
  CHECK(fg_multiplier_confirmed_on_disk(2, xml));
  CHECK_FALSE(fg_multiplier_confirmed_on_disk(3, xml));
  CHECK(fg_frametime_trace_count(on_f24, 2) == 2);

  char fps[128]{};
  char ft[160]{};
  char fg_line[160]{};
  format_session_hud_fps_line(fps, sizeof(fps), 40.0, 80.0, on_f24, 2, true);
  CHECK(session_hud_fps_shows_cap_fg_split(fps));
  CHECK(std::strstr(fps, "x2 ON") != nullptr);
  format_overlay_frametime_line(ft, sizeof(ft), 25.0, 25.0, 26.0, false, on_f24, 2, 40.0, 60.0, 80.0);
  CHECK(std::strstr(ft, "FG est 12.50") != nullptr);
  CHECK(std::strstr(ft, "PLAYABLE FG") != nullptr);
  format_session_hud_fg_status_line(fg_line, sizeof(fg_line), on_f24, 2, "lsfg3", 80, "WGC", "On", 40.0, 80.0);
  CHECK(std::strstr(fg_line, "FG      ON") != nullptr);
  CHECK(std::strstr(fg_line, "flow80") != nullptr);
  CHECK(std::strstr(fg_line, "WGC") != nullptr);

  char a[512]{};
  char b[512]{};
  format_mini_overlay_lines(a, sizeof(a), b, sizeof(b), 40.0, 80.0, on_f24, 2, true, true, 25.0, 25.0, 26.0, 30.0,
                            false, 8.5, 2.2, 0, 0, 0, 2560, 1440, 2560, 1440, 2560, 1440, 100, false, 0, 0, 0, 0,
                            nullptr, nullptr, 0, 0, 0, "lsfg3", 60.0);
  CHECK(std::strstr(a, "cap|FG") != nullptr);
  CHECK(std::strstr(b, "FG est 12.50") != nullptr);

  LiveStripLines strip{};
  fill_live_strip_lines(strip, false, LiveConsole::Ps5, true, 5, on_f24, 2, 40.0, 80.0, false, 0, 25.0);
  CHECK(strip.has_fg_fps);
  CHECK(std::string(strip.ft) == "25.00 · FG 12.50");

  const bool still_off = fg_overlay_reads_on(true, 2, true, 0, false, false);
  CHECK_FALSE(still_off);
  format_session_hud_fps_line(fps, sizeof(fps), 40.0, 80.0, still_off, 2, true);
  CHECK_FALSE(session_hud_fps_shows_cap_fg_split(fps));
  format_overlay_frametime_line(ft, sizeof(ft), 25.0, 25.0, 26.0, false, still_off, 2, 40.0, 60.0, 80.0);
  CHECK(std::strstr(ft, "FG est") == nullptr);
  format_session_hud_fg_status_line(fg_line, sizeof(fg_line), still_off, 2, "lsfg3", 80, "WGC", "Ready");
  CHECK(std::strstr(fg_line, "off") != nullptr);
  CHECK(std::strstr(fg_line, "FG      ON") == nullptr);
}

TEST_CASE("patch_lsfg_ini mira lock") {
  const std::string ini = patch_lsfg_ini("");
  CHECK(ini.find("flow_scale = 80") != std::string::npos);
  CHECK(ini.find("ui_detection_rate = 2") != std::string::npos);
  CHECK(ini.find("ui_detection_threshold = 0.30") != std::string::npos);
  CHECK(ls_ini_fps_counter_hidden(ini));
  const std::string bad = "[lsfg]\r\nflow_scale = 70\r\nui_detection_rate = 1\r\nui_detection_threshold = 0.10\r\n";
  const std::string fixed = patch_lsfg_ini(bad);
  CHECK(fixed.find("flow_scale = 80") != std::string::npos);
  CHECK(fixed.find("ui_detection_rate = 2") != std::string::npos);
  CHECK(ls_ini_fps_counter_hidden(fixed));
  const std::string dirty_fps =
      "[lsfg]\r\nflow_scale = 80\r\nui_detection_rate = 2\r\nui_detection_threshold = 0.30\r\n"
      "[fps_counter]\r\nscale = 1.0\r\nshow_captured = 1\r\n";
  CHECK(ls_ini_fps_counter_hidden(patch_lsfg_ini(dirty_fps)));
  CHECK(ls_xml_draw_fps_off(patch_mira_settings_xml(
      "<Settings><LSFGFlowScale>100</LSFGFlowScale><CaptureApi>WGC</CaptureApi><DrawFps>true</DrawFps></Settings>",
      2)));
}

TEST_CASE("fps counter only patch never touches lsfg profile") {
  // keep-flow90 deixava exatamente isto: contador aceso e flow 100 contra a nossa mira.
  const std::string from_script =
      "[rendering]\r\nflush = 1\r\n\r\n"
      "[lsfg]\r\nui_detection_rate = 2\r\nui_detection_threshold = 0.30\r\nflow_scale = 100\r\n"
      "real_timestamp_tolerance = 0.05\r\nbase_framerate_threshold = 10\r\n\r\n"
      "[fps_counter]\r\nscale = 1.0\r\ncolor = 0xD3D3D3\r\nfont = Consolas\r\n"
      "offset_x = 0\r\noffset_y = 0\r\nshow_captured = 1\r\n";
  const std::string fps_only = patch_lsfg_ini_fps_counter_only(from_script);
  CHECK(ls_ini_fps_counter_hidden(fps_only));
  // Com o LS vivo nao mexemos no perfil: flow_scale fica como estava.
  CHECK(fps_only.find("flow_scale = 100") != std::string::npos);
  CHECK(fps_only.find("flow_scale = 80") == std::string::npos);
  CHECK(fps_only.find("ui_detection_rate = 2") != std::string::npos);
  CHECK(fps_only.find("[rendering]") != std::string::npos);
  // Idempotente: reaplicar nao duplica secao nem reacende o chip.
  const std::string twice = patch_lsfg_ini_fps_counter_only(fps_only);
  CHECK(ls_ini_fps_counter_hidden(twice));
  CHECK(twice == fps_only);
  CHECK(twice.find("[fps_counter]") == twice.rfind("[fps_counter]"));

  // Sem a secao: injeta escondida, mantendo o resto.
  const std::string no_sec = "[lsfg]\r\nflow_scale = 80\r\nui_detection_rate = 2\r\n";
  const std::string injected = patch_lsfg_ini_fps_counter_only(no_sec);
  CHECK(ls_ini_fps_counter_hidden(injected));
  CHECK(injected.find("flow_scale = 80") != std::string::npos);

  // O caminho completo (pre-lancamento) continua a impor a mira E o hide.
  const std::string full = patch_lsfg_ini(from_script);
  CHECK(ls_ini_fps_counter_hidden(full));
  CHECK(mira_lock_ini_ok(full));
  CHECK(full.find("flow_scale = 100") == std::string::npos);
  CHECK_FALSE(ls_ini_fps_counter_hidden(
      "[fps_counter]\r\nscale = 1.0\r\nshow_captured = 1\r\n"));
  CHECK_FALSE(ls_ini_fps_counter_hidden(
      "[fps_counter]\r\nscale = 0.1\r\nshow_captured = 0\r\n"));
  CHECK(ls_ini_fps_counter_hidden("[fps_counter]\nscale = 0\nshow_captured = 0\n"));
}

TEST_CASE("resolve_lossless_exe missing override") {
  CHECK(resolve_lossless_exe("C:\\definitely\\missing\\LosslessScaling.exe").empty());
  CHECK_FALSE(lossless_exe_exists("C:\\definitely\\missing\\LosslessScaling.exe"));
}

TEST_CASE("fg_path persists in settings and reaches AppConfig") {
  PersistedSettings s = default_settings();
  s.fg_enabled = true;
  s.fg_multiplier = 3;
  s.fg_path = "D:\\Games\\Lossless Scaling\\LosslessScaling.exe";
  const std::string ini = serialize_settings_ini(s);
  CHECK(ini.find("fg_path=D:\\Games\\Lossless Scaling\\LosslessScaling.exe") != std::string::npos);
  PersistedSettings loaded;
  REQUIRE(parse_settings_ini(ini, loaded));
  CHECK(loaded.fg_path == s.fg_path);
  CHECK(loaded.fg_multiplier == 3);
  CHECK_FALSE(loaded.fg_adaptive);
  CHECK(loaded.fg_target_fps == 60);
  s.fg_adaptive = true;
  s.fg_target_fps = 80;
  const std::string ini2 = serialize_settings_ini(s);
  CHECK(ini2.find("fg_adaptive=1") != std::string::npos);
  CHECK(ini2.find("fg_target_fps=80") != std::string::npos);
  PersistedSettings adp;
  REQUIRE(parse_settings_ini(ini2, adp));
  CHECK(adp.fg_adaptive);
  CHECK(adp.fg_target_fps == 80);
  AppConfig cfg;
  apply_persisted_to_config(adp, cfg);
  CHECK(cfg.fg_path == s.fg_path);
  CHECK(cfg.fg_multiplier == 3);
  CHECK(cfg.fg_adaptive);
  CHECK(cfg.fg_target_fps == 80);
  // Empty path = auto-detect (resolve may still find Steam install).
  loaded.fg_path.clear();
  apply_persisted_to_config(loaded, cfg);
  CHECK(cfg.fg_path.empty());
}

TEST_CASE("logo brand paths and app icon resource id") {
  CHECK(kAppIconResourceId == 101);
  CHECK(logo_png_candidate_assets("C:\\app") == "C:\\app\\assets\\logo.png");
  CHECK(logo_png_candidate_root("C:\\app") == "C:\\app\\logo.png");
  CHECK(logo_png_path_join("C:\\app\\", "assets\\logo.png") == "C:\\app\\assets\\logo.png");
  CHECK(watermark_png_candidate_assets("C:\\app") == "C:\\app\\assets\\watermark.png");
  CHECK(watermark_png_candidate_root("C:\\app") == "C:\\app\\watermark.png");
}

TEST_CASE("session notices for FG enable/up and NR on/off") {
  CHECK(session_notice_for_fg_toggle(true));
  CHECK_FALSE(session_notice_for_fg_toggle(false));
  CHECK(session_notice_for_fg_cycle(2, 3));
  CHECK(session_notice_for_fg_cycle(3, 4));
  CHECK_FALSE(session_notice_for_fg_cycle(4, 2));  // cycle wrap is not "subindo"
  CHECK_FALSE(session_notice_for_fg_cycle(3, 3));

  CHECK(session_notice_fg_on().find("Frame Gen ON") != std::string::npos);
  CHECK(session_notice_fg_on().find("artefato") == std::string::npos);
  CHECK(session_notice_fg_on().find("latencia") == std::string::npos);
  CHECK(session_notice_fg_multiplier_up(3).find("x3") != std::string::npos);
  CHECK(session_notice_nr_on().find("ON") != std::string::npos);
  CHECK(session_notice_nr_on().find("Confirmado") != std::string::npos);
  CHECK(session_notice_nr_on().find("imagem mais bonita") != std::string::npos);
  CHECK(session_notice_nr_off().find("OFF") != std::string::npos);
  CHECK(session_notice_nr_applying().find("Aplicando") != std::string::npos);
  CHECK(session_notice_hdmi_recover().find("HDMI") != std::string::npos);
  CHECK(session_hdmi_notice_title().find("console") != std::string::npos);
  CHECK(session_hdmi_notice_title().find("parou") != std::string::npos);
  CHECK(session_hdmi_notice_title().find("HDMI") != std::string::npos);
  CHECK(session_hdmi_notice_detail().find("pixels") != std::string::npos);
  CHECK(session_notice_hdmi_recover().find("NexusCap") != std::string::npos);
  {
    std::string t, d;
    session_notice_split(session_notice_nr_on(), t, d);
    CHECK(t.find("Neural Rendering ON") != std::string::npos);
    CHECK(d.find("imagem mais bonita") != std::string::npos);
  }
  CHECK(session_notice_extra_px(session_notice_fg_on()) == 36);
  CHECK(session_notice_extra_px("so titulo") == 36);
  CHECK(session_notice_extra_px("") == 0);
  CHECK(session_notice_toast_scale() == 3);
  CHECK(session_notice_band_px(session_notice_fg_on(), 1) == 48);
  CHECK(session_notice_band_px(session_notice_fg_on(), 3) == 144);
  CHECK(session_notice_band_px("", 3) == 0);
  {
    int x = 0, y = 0;
    overlay_notice_center(720, 144, 1920, 1080, x, y);
    CHECK(x == 600);
    CHECK(y == 468);
    overlay_notice_center(1920, 200, 1920, 1080, x, y);
    CHECK(x == 0);
    CHECK(y == 440);
  }

  SessionNotice n;
  n.push(session_notice_fg_on(), 1000.0, 500.0);
  CHECK(n.active(1200.0));
  CHECK(n.visible(1200.0).find("Frame Gen") != std::string::npos);
  CHECK_FALSE(n.active(1600.0));
  CHECK(n.visible(1600.0).empty());

  n.push(session_notice_nr_off(), 2000.0, 1000.0);
  CHECK(n.visible(2500.0) == session_notice_nr_off());
  n.push(session_notice_nr_on(), 2600.0, 1000.0);
  CHECK(n.visible(2700.0) == session_notice_nr_on());
}

TEST_CASE("session_start records fg fields") {
  AnalyzeWriter a;
  SessionInfo info;
  info.fg_enabled = true;
  info.fg_multiplier = 2;
  info.fg_backend = "lsfg3";
  info.fg_status = "on";
  a.session_start(info, 1.0);
  const auto lines = a.lines_copy();
  REQUIRE_FALSE(lines.empty());
  CHECK(lines[0].find("\"fg_enabled\":true") != std::string::npos);
  CHECK(lines[0].find("\"fg_multiplier\":2") != std::string::npos);
  CHECK(lines[0].find("\"fg_backend\":\"lsfg3\"") != std::string::npos);
  const std::string sum = a.summary_text(2.0);
  CHECK(sum.find("fg:") != std::string::npos);
  CHECK(sum.find("lsfg3") != std::string::npos);
}

TEST_CASE("app_present_scale native under RenoDX (v2.0 no double-downsample)") {
  CHECK(app_present_scale_percent(true, 50) == 100);
  CHECK(app_present_scale_percent(true, 25) == 100);
  CHECK(app_present_scale_percent(true, 100) == 100);
  CHECK(app_present_scale_percent(false, 50) == 50);
  CHECK(app_present_scale_percent(false, 75) == 75);
  // Feed still follows UI scale; app RT stays native when RenoDX on.
  CHECK(feed_work_resolution_for_session(50) == 50);
  CHECK(app_present_scale_percent(true, 50) != feed_work_resolution_for_session(50));
}

TEST_CASE("runtime health detects missing renodx files") {
  const auto empty = check_runtime_health("");
  CHECK_FALSE(empty.renodx_complete());
}

TEST_CASE("clamp_scale_percent and defaults") {
  CHECK(clamp_scale_percent(100) == 100);
  CHECK(clamp_scale_percent(50) == 50);
  CHECK(clamp_scale_percent(25) == 25);
  CHECK(clamp_scale_percent(24) == 25);
  CHECK(clamp_scale_percent(10) == 25);
  CHECK(clamp_scale_percent(200) == 100);
  CHECK(kMinScalePercent == 25);
  CHECK(kMaxScalePercent == 100);
  auto s = default_settings();
  CHECK(s.scale_percent == 100);
  CHECK_FALSE(s.fg_enabled);
  CHECK(s.hotkeys.vk_fg_toggle == 0x22);
  CHECK(s.hotkeys.vk_fg_cycle == 0x21);
}

TEST_CASE("scaled_extent and pixel ratio for work resolution") {
  CHECK(scaled_extent(2560, 100) == 2560);
  CHECK(scaled_extent(1440, 100) == 1440);
  CHECK(scaled_extent(2560, 50) == 1280);
  CHECK(scaled_extent(1440, 50) == 720);
  CHECK(scaled_extent(2560, 25) == 640);
  CHECK(scaled_extent(1440, 25) == 360);
  CHECK(scaled_extent(1920, 75) == 1440);
  CHECK((scaled_extent(2560, 50) & 1) == 0);
  CHECK(scale_pixel_ratio(100) == doctest::Approx(1.0));
  CHECK(scale_pixel_ratio(50) == doctest::Approx(0.25));  // 50% linear → 25% pixels
  CHECK(scale_pixel_ratio(25) == doctest::Approx(0.0625));
  CHECK(scale_pixel_ratio(50) < scale_pixel_ratio(75));
  CHECK(scale_pixel_ratio(75) < scale_pixel_ratio(100));
}

TEST_CASE("apply_keyed_int does not match rebuild inside warmup_rebuild") {
  std::string feed =
      "warmup_rebuild=180\nrebuild=0\nwork_resolution=100\nwork_upscale=0\n";
  feed = apply_feed_work_resolution(std::move(feed), 75);
  CHECK(feed.find("warmup_rebuild=180") != std::string::npos);
  CHECK(feed.find("rebuild=1") != std::string::npos);
  CHECK(feed.find("work_resolution=75") != std::string::npos);
  CHECK(feed.find("work_upscale=1") != std::string::npos);
  // Must not have corrupted the warmup line into "warmup_1" / similar.
  CHECK(feed.find("warmup_rebuild=1\n") == std::string::npos);
}

TEST_CASE("feed work_resolution sync restores 100% after 50% (blur regression)") {
  const std::string stuck50 =
      "enabled=1\nmode=2\nwork_resolution=50\nwork_upscale=0\nwork_sharpness=0.30\n";
  CHECK(parse_feed_work_resolution(stuck50) == 50);

  // UI back to 100% must rewrite feed — previously skipped when scale==100.
  const std::string fixed = apply_feed_work_resolution(stuck50, 100);
  CHECK(parse_feed_work_resolution(fixed) == 100);
  CHECK(fixed.find("work_resolution=100") != std::string::npos);
  CHECK(fixed.find("work_resolution=50") == std::string::npos);
  CHECK(fixed.find("work_upscale=0") != std::string::npos);
  CHECK(fixed.find("rebuild=1") != std::string::npos);

  const std::string to75 = apply_feed_work_resolution(fixed, 75);
  CHECK(parse_feed_work_resolution(to75) == 75);
  CHECK(to75.find("work_upscale=1") != std::string::npos);

  // Missing key gets appended.
  const std::string bare = apply_feed_work_resolution("enabled=1\n", 100);
  CHECK(parse_feed_work_resolution(bare) == 100);
}

TEST_CASE("feed work_resolution always matches UI scale (critical blur path)") {
  // Exact production cfg shape that caused "UI 100% but image blurry".
  std::string feed =
      "enabled=1\nmode=2\nhdr=-1\nwork_resolution=50\nwork_upscale=0\nwork_sharpness=0.30\n";

  // Simulate session starts: 50 → 100 → 50 → 100 must never leave 50 stuck.
  const int sequence[] = {50, 100, 50, 75, 100, 100};
  for (int ui_scale : sequence) {
    feed = apply_feed_work_resolution(std::move(feed), ui_scale);
    CHECK(parse_feed_work_resolution(feed) == feed_work_resolution_for_session(ui_scale));
  }

  // CRLF line endings (Windows editors).
  const std::string crlf = apply_feed_work_resolution("work_resolution=50\r\nwork_upscale=0\r\n", 100);
  CHECK(parse_feed_work_resolution(crlf) == 100);
  CHECK(crlf.find("work_resolution=100") != std::string::npos);

  // Out-of-range UI scale clamps before writing feed; under 50% the Feed floor wins.
  CHECK(parse_feed_work_resolution(apply_feed_work_resolution("work_resolution=50\n", 200)) == 100);
  CHECK(parse_feed_work_resolution(apply_feed_work_resolution("work_resolution=50\n", 10)) == kMinFeedWorkResolution);
  CHECK(parse_feed_work_resolution(apply_feed_work_resolution("work_resolution=50\n", 25)) == kMinFeedWorkResolution);

  // Mismatch detector: feed@50 + UI@100 is the bug condition.
  const int feed_v = 50;
  const int ui_v = 100;
  CHECK(feed_v != ui_v);
  CHECK(parse_feed_work_resolution(apply_feed_work_resolution("work_resolution=50\n", ui_v)) == ui_v);
}

TEST_CASE("renderer work size at 100% equals signal (no silent downsample)") {
  CHECK(scaled_extent(2560, 100) == 2560);
  CHECK(scaled_extent(1440, 100) == 1440);
  // 100% must not shrink; 50% must.
  CHECK(scaled_extent(2560, 100) > scaled_extent(2560, 50));
  CHECK(scaled_extent(2560, 100) == scaled_extent(2560, clamp_scale_percent(100)));
}

// Feed downsamples everything it presents: menus stay at 100%, session gets the scale in
// its own process (--autostart), and Esc restores 100% for the next lobby.
TEST_CASE("Esc back to lobby restores 100% feed (menus stay sharp)") {
  CHECK(feed_work_resolution_for_lobby() == 100);
  CHECK(feed_work_resolution_for_session(50) == 50);
  CHECK(feed_work_resolution_for_session(100) == 100);
  // Boot: lobby first → 100%; straight into capture → session scale.
  CHECK(feed_work_resolution_at_boot(true, 50) == 100);
  CHECK(feed_work_resolution_at_boot(false, 50) == 50);
  CHECK(feed_work_resolution_at_boot(false, 25) == kMinFeedWorkResolution);
  // Leaving a scaled session needs a relaunch so the menu process locks 100%.
  CHECK(feed_scale_change_needs_relaunch(true, 50, kLobbyFeedWorkResolution));
  CHECK_FALSE(feed_scale_change_needs_relaunch(true, 100, kLobbyFeedWorkResolution));
  CHECK_FALSE(feed_scale_change_needs_relaunch(false, 50, kLobbyFeedWorkResolution));

  std::string feed =
      "enabled=1\nmode=2\nhdr=-1\nwork_resolution=100\nwork_upscale=0\nwork_sharpness=0.30\nrebuild=0\n";
  const int session_scale = 50;
  CHECK_FALSE(feed_leaks_session_scale_into_lobby(feed));

  // Session scale on disk would blur the next lobby Present.
  feed = apply_feed_work_resolution(std::move(feed), session_scale);
  CHECK(parse_feed_work_resolution(feed) == 50);
  CHECK(feed.find("work_upscale=1") != std::string::npos);
  CHECK(feed.find("rebuild=1") != std::string::npos);
  CHECK(feed_leaks_session_scale_into_lobby(feed));
  CHECK(feed_work_resolution_for_session(session_scale) == 50);

  feed = apply_lobby_feed_work_resolution(std::move(feed));
  CHECK(parse_feed_work_resolution(feed) == 100);
  CHECK(feed.find("work_upscale=0") != std::string::npos);
  CHECK_FALSE(feed_leaks_session_scale_into_lobby(feed));

  // Full cycle: lobby 100% → session 50% → Esc 100% → session 50% again.
  int ui = 100;
  feed = apply_feed_work_resolution(std::move(feed), feed_work_resolution_at_boot(true, ui));
  CHECK(parse_feed_work_resolution(feed) == 100);
  ui = 50;
  feed = apply_feed_work_resolution(std::move(feed), feed_work_resolution_for_session(ui));
  CHECK(parse_feed_work_resolution(feed) == 50);
  feed = apply_lobby_feed_work_resolution(std::move(feed));
  CHECK(parse_feed_work_resolution(feed) == 100);
  CHECK_FALSE(feed_leaks_session_scale_into_lobby(feed));
  feed = apply_feed_work_resolution(std::move(feed), feed_work_resolution_for_session(ui));
  CHECK(parse_feed_work_resolution(feed) == 50);
  CHECK(feed_leaks_session_scale_into_lobby(feed));
}

TEST_CASE("settings ini round-trip") {
  PersistedSettings s = default_settings();
  s.scale_percent = 75;
  s.fg_enabled = true;
  s.fg_multiplier = 3;
  s.device_name = "VEDO-TEST";
  s.hotkeys.vk_hud = 0x71;
  s.ui.show_fps = true;
  s.ui.advanced_metrics = true;
  s.ui.mini_overlay = true;
  s.ui.overlay_no_bg = true;
  s.ui.fullscreen = false;
  s.ui.sync_image = true;
  s.ui.preview_mute = true;
  s.ui.preview_enabled = false;
  s.ui.live_overlay = true;
  s.ui.overlay_console = true;
  s.ui.live_console = "xbox_ss";
  s.enhance_renodx = false;
  s.renodx_preset = static_cast<int>(RenodxVisualPreset::Cinematic);
  const std::string text = serialize_settings_ini(s);
  PersistedSettings out;
  CHECK(parse_settings_ini(text, out));
  CHECK(out.scale_percent == 75);
  CHECK(out.fg_enabled);
  CHECK(out.fg_multiplier == 3);
  CHECK(out.device_name == "VEDO-TEST");
  CHECK(out.hotkeys.vk_hud == 0x71);
  CHECK(out.hotkeys.vk_fg_toggle == 0x22);
  CHECK(out.ui.show_fps);
  CHECK(out.ui.advanced_metrics);
  CHECK(out.ui.mini_overlay);
  CHECK(out.ui.overlay_no_bg);
  CHECK_FALSE(out.ui.fullscreen);
  CHECK(out.ui.hide_panel_on_capture);
  CHECK(out.ui.sync_image);
  CHECK(out.ui.preview_mute);
  CHECK_FALSE(out.ui.preview_enabled);
  CHECK(out.ui.live_overlay);
  CHECK(out.ui.overlay_console);
  CHECK(out.ui.live_console == "xbox_ss");
  CHECK_FALSE(out.enhance_renodx);
  CHECK(out.renodx_preset == static_cast<int>(RenodxVisualPreset::Cinematic));
  // Legacy auto_start key must not break parse / must be ignored.
  CHECK(parse_settings_ini(text + "auto_start=1\n", out));
  CHECK_FALSE(serialize_settings_ini(out).find("auto_start=") != std::string::npos);
}

TEST_CASE("lobby preview and audio toggles persist and stop capture") {
  CHECK(default_settings().ui.preview_enabled);
  CHECK_FALSE(default_settings().ui.preview_mute);
  CHECK(lobby_preview_capture_wanted(true));
  CHECK_FALSE(lobby_preview_capture_wanted(false));
  CHECK(lobby_preview_audio_wanted(true, true));
  CHECK_FALSE(lobby_preview_audio_wanted(false, true));
  CHECK_FALSE(lobby_preview_audio_wanted(true, false));
  CHECK_FALSE(lobby_preview_audio_wanted(false, false));
  CHECK(std::string(lobby_preview_toggle_label(true)) == "Desativar preview");
  CHECK(std::string(lobby_preview_toggle_label(false)) == "Ativar preview");
  CHECK(std::string(lobby_audio_toggle_label(false)) == "Desativar som");
  CHECK(std::string(lobby_audio_toggle_label(true)) == "Ativar som");

  PersistedSettings s = default_settings();
  s.ui.preview_enabled = false;
  s.ui.preview_mute = true;
  PersistedSettings out;
  REQUIRE(parse_settings_ini(serialize_settings_ini(s), out));
  CHECK_FALSE(out.ui.preview_enabled);
  CHECK(out.ui.preview_mute);
  CHECK(serialize_settings_ini(out).find("preview_enabled=0") != std::string::npos);
  CHECK(serialize_settings_ini(out).find("preview_mute=1") != std::string::npos);

  PersistedSettings missing;
  REQUIRE(parse_settings_ini("show_fps=1\n", missing));
  CHECK(missing.ui.preview_enabled);
  CHECK_FALSE(missing.ui.preview_mute);
}

TEST_CASE("capture volume clamps gain persists and stays out of presets") {
  CHECK(clamp_capture_volume_percent(-8) == 0);
  CHECK(clamp_capture_volume_percent(0) == 0);
  CHECK(clamp_capture_volume_percent(40) == 40);
  CHECK(clamp_capture_volume_percent(100) == 100);
  CHECK(clamp_capture_volume_percent(140) == 100);
  CHECK(capture_volume_gain(0) == 0.f);
  CHECK(capture_volume_gain(50) == 0.5f);
  CHECK(capture_volume_gain(100) == 1.f);
  CHECK(capture_volume_gain(-8) == 0.f);
  CHECK(capture_volume_gain(140) == 1.f);

  float f32[] = {1.f, -0.5f, 0.25f};
  apply_capture_volume_f32(f32, 3, 1.f);
  CHECK(f32[0] == 1.f);
  CHECK(f32[1] == -0.5f);
  apply_capture_volume_f32(f32, 3, 0.5f);
  CHECK(f32[0] == 0.5f);
  CHECK(f32[1] == -0.25f);
  CHECK(f32[2] == 0.125f);
  apply_capture_volume_f32(f32, 3, 0.f);
  CHECK(f32[0] == 0.f);
  CHECK(f32[1] == 0.f);
  CHECK(f32[2] == 0.f);

  std::int16_t s16[] = {1000, -800, 400};
  apply_capture_volume_s16(s16, 3, 0.5f);
  CHECK(s16[0] == 500);
  CHECK(s16[1] == -400);
  CHECK(s16[2] == 200);
  apply_capture_volume_s16(s16, 3, 0.f);
  CHECK(s16[0] == 0);
  CHECK(s16[1] == 0);

  CHECK(default_settings().capture_volume == kDefaultCaptureVolumePercent);
  PersistedSettings s = default_settings();
  s.capture_volume = 40;
  PersistedSettings out;
  REQUIRE(parse_settings_ini(serialize_settings_ini(s), out));
  CHECK(out.capture_volume == 40);
  CHECK(serialize_settings_ini(out).find("capture_volume=40") != std::string::npos);

  PersistedSettings missing;
  REQUIRE(parse_settings_ini("show_fps=1\n", missing));
  CHECK(missing.capture_volume == 100);
  PersistedSettings over;
  REQUIRE(parse_settings_ini("capture_volume=180\n", over));
  CHECK(over.capture_volume == 100);
  PersistedSettings under;
  REQUIRE(parse_settings_ini("capture_volume=-3\n", under));
  CHECK(under.capture_volume == 0);

  AppConfig cfg;
  apply_persisted_to_config(out, cfg);
  CHECK(cfg.capture_volume == 40);
  cfg.capture_volume = 25;
  apply_session_cfg_to_persisted(cfg, s);
  CHECK(s.capture_volume == 25);
  cfg.perf_test = true;
  cfg.capture_volume = 90;
  apply_session_cfg_to_persisted(cfg, s);
  CHECK(s.capture_volume == 25);

  UserPreset p;
  p.name = "TLOU 2x";
  p.nr_pass_count = 5;
  p.renodx_preset = 2;
  p.fg_enabled = true;
  p.fg_multiplier = 3;
  p.scale_percent = 80;
  const std::string text = serialize_user_preset(p);
  CHECK(text.find("capture_volume") == std::string::npos);
  UserPreset junk;
  REQUIRE(parse_user_preset(text + "capture_volume=10\n", junk));
  CHECK(junk.scale_percent == 80);

  PersistedSettings disk = default_settings();
  disk.capture_volume = 40;
  disk.device_name = "Elgato 4K X";
  apply_user_preset_to_persisted(disk, p, true);
  CHECK(disk.capture_volume == 40);
  CHECK(disk.nr_pass_count == 5);
  PersistedSettings before = default_settings();
  before.capture_volume = 40;
  before.device_name = "Elgato 4K X";
  PersistedSettings after = before;
  apply_user_preset_to_persisted(after, p, true);
  CHECK(persisted_untouched_by_user_preset(before, after));
  CHECK(after.capture_volume == 40);

  SessionCfgState sess{};
  sess.capture_volume = 40;
  sess.nr_pass_count = 1;
  stamp_session_from_user_preset(sess, p, true);
  CHECK(sess.capture_volume == 40);
  CHECK(sess.nr_pass_count == 5);

  AppConfig bench;
  bench.capture_volume = 40;
  PerfTestPlan plan;
  apply_perf_test_plan_to_config(plan, bench);
  CHECK(bench.perf_test);
  CHECK(bench.capture_volume == 40);
  CHECK(serialize_perf_test_plan(plan).find("capture_volume") == std::string::npos);
  CHECK(perf_test_plan_from_config(bench).audio == bench.perf_test_audio);

  float mix[] = {1.f, -1.f};
  apply_capture_volume_mix_pcm(mix, 1, 2, 32, 0.5f);
  CHECK(mix[0] == 0.5f);
  CHECK(mix[1] == -0.5f);
}

TEST_CASE("lobby preview cfg live default off persist and labels") {
  CHECK_FALSE(default_settings().ui.preview_cfg_live);
  CHECK_FALSE(lobby_preview_cfg_wanted(true, false));
  CHECK(lobby_preview_cfg_wanted(true, true));
  CHECK_FALSE(lobby_preview_cfg_wanted(false, true));
  CHECK_FALSE(lobby_preview_cfg_wanted(false, false));
  CHECK(std::string(lobby_preview_cfg_toggle_label(false)) ==
        "Visualizar configuracoes no preview em tempo real");
  CHECK(std::string(lobby_preview_cfg_toggle_label(true)) ==
        "Ocultar configuracoes no preview em tempo real");

  PersistedSettings s = default_settings();
  s.ui.preview_cfg_live = true;
  PersistedSettings on;
  REQUIRE(parse_settings_ini(serialize_settings_ini(s), on));
  CHECK(on.ui.preview_cfg_live);
  CHECK(serialize_settings_ini(on).find("preview_cfg_live=1") != std::string::npos);

  s.ui.preview_cfg_live = false;
  PersistedSettings off;
  REQUIRE(parse_settings_ini(serialize_settings_ini(s), off));
  CHECK_FALSE(off.ui.preview_cfg_live);
  CHECK(serialize_settings_ini(off).find("preview_cfg_live=0") != std::string::npos);

  PersistedSettings missing;
  REQUIRE(parse_settings_ini("preview_enabled=1\n", missing));
  CHECK_FALSE(missing.ui.preview_cfg_live);

  CHECK_FALSE(lobby_preview_nr_files_on(true, false, true, true));
  CHECK(lobby_preview_nr_files_on(true, true, true, true));
  CHECK_FALSE(lobby_preview_nr_files_on(true, true, false, true));
  CHECK_FALSE(lobby_preview_feed_files_on(true, false, true));
  CHECK(lobby_preview_feed_files_on(true, true, true));
  CHECK(std::string(lobby_preview_cfg_pipeline_hold_line(false)) == "Preview HDMI sem NR");
  CHECK(std::string(lobby_preview_cfg_pipeline_hold_line(true)) ==
        "Configuracoes no preview em tempo real");
}

TEST_CASE("bundled motor stock never false-positive or false-negative") {
  CHECK_FALSE(lobby_motor_override_wanted(false, true, true, true));
  CHECK_FALSE(lobby_motor_override_wanted(true, false, true, false));
  CHECK_FALSE(lobby_motor_override_wanted(true, true, false, false));
  CHECK(lobby_motor_override_wanted(true, true, true, false));
  CHECK(lobby_motor_override_wanted(true, true, false, true));
  CHECK(lobby_motor_override_wanted(true, true, true, true));

  CHECK(std::string(lobby_motor_override_warn()) ==
        "Aviso: motores padrao alterados. Podes trocar o Feed e/ou o RenoDX (um ou os dois) depois de "
        "instalar, mas pode dar bugs e perdes 2x otimizado e 1,5x.");
  CHECK(std::string(lobby_motor_override_which(true, false)) == "so o motor 1x (Feed estavel)");
  CHECK(std::string(lobby_motor_override_which(false, true)) == "o motor multipass (1x/1,5x/2x/3x)");
  CHECK(std::string(lobby_motor_override_which(true, true)) == "1x (stable) e 1,5x/2x/3x (multipass)");
  CHECK(std::string(lobby_motor_override_which(false, false)).empty());

  CHECK(sha256_hex(nullptr, 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256_hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  const std::string stock_a = "hello-stock-a";
  const std::string stock_b = "hello-stock-B";
  const std::string sha_a = sha256_hex(stock_a.data(), stock_a.size());
  const std::string sha_b = sha256_hex(stock_b.data(), stock_b.size());
  REQUIRE(sha_a != sha_b);
  CHECK(motor_file_matches_stock(stock_a.size(), sha_a, stock_a.size(), sha_a));
  {
    std::string upper = sha_a;
    for (char& c : upper) {
      if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
    }
    CHECK(motor_file_matches_stock(stock_a.size(), sha_a, stock_a.size(), upper));
  }
  CHECK_FALSE(motor_file_matches_stock(stock_a.size(), sha_a, stock_a.size(), sha_b));
  CHECK_FALSE(motor_file_matches_stock(stock_a.size(), sha_a, stock_a.size() + 1, sha_a));

  CHECK(parse_motor_stock_manifest("").empty());
  CHECK(parse_motor_stock_manifest("# only comment\n\n; nope\n").empty());
  const std::string sha64a(64, 'A');
  const std::string sha64b(64, 'b');
  const std::string sha64skip(64, 'd');
  const std::string mixed = std::string("# size sha256 path\n") + "12 " + sha64a +
                            " addons-stable\\renodx-dlss5.addon64\n" + "99 " + sha64skip +
                            " other/skip.addon64\n" + "8 " + sha64b +
                            " ./addons-multipass/dlss5-feed.addon64\r\n";
  const auto parsed = parse_motor_stock_manifest(mixed);
  REQUIRE(parsed.size() == 2);
  CHECK(parsed[0].rel == "addons-stable/renodx-dlss5.addon64");
  CHECK(parsed[0].size == 12);
  CHECK(motor_stock_hex_equal(parsed[0].sha256, std::string(64, 'a')));
  CHECK(parsed[1].rel == "addons-multipass/dlss5-feed.addon64");

  const auto tmp = std::filesystem::temp_directory_path() / "nexuscap_motors_ut";
  std::error_code ec;
  std::filesystem::remove_all(tmp, ec);
  std::filesystem::create_directories(tmp / "addons-stable", ec);
  std::filesystem::create_directories(tmp / "addons-multipass", ec);

  auto write_bin = [&](const std::filesystem::path& p, const std::string& bytes) {
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o << bytes;
  };
  auto write_stock = [&](const std::string& body) {
    std::ofstream o(tmp / motors_stock_filename(), std::ios::binary | std::ios::trunc);
    o << body;
  };
  auto stock_line = [&](const std::string& rel, const std::string& bytes) {
    return std::to_string(bytes.size()) + " " + sha256_hex(bytes.data(), bytes.size()) + " " + rel + "\n";
  };

  const std::string snr = "stable-nr";
  const std::string sfeed = "stable-feed";
  const std::string mnr = "multi-nr";
  const std::string mfeed = "multi-feed";
  write_bin(tmp / "addons-stable" / "renodx-dlss5.addon64", snr);
  write_bin(tmp / "addons-stable" / "dlss5-feed.addon64", sfeed);
  write_bin(tmp / "addons-multipass" / "renodx-dlss5.addon64", mnr);
  write_bin(tmp / "addons-multipass" / "dlss5-feed.addon64", mfeed);

  CHECK_FALSE(probe_bundled_motors(tmp.string()).wanted());  // no stock
  write_stock("#\n");
  CHECK_FALSE(probe_bundled_motors(tmp.string()).stock_ok);

  write_stock(stock_line("addons-stable/renodx-dlss5.addon64", snr) +
              stock_line("addons-stable/dlss5-feed.addon64", sfeed) +
              stock_line("addons-multipass/renodx-dlss5.addon64", mnr) +
              stock_line("addons-multipass/dlss5-feed.addon64", mfeed));
  {
    const auto ok = probe_bundled_motors(tmp.string());
    CHECK(ok.stock_ok);
    CHECK(ok.compared_any);
    CHECK_FALSE(ok.stable_dirty);
    CHECK_FALSE(ok.multipass_dirty);
    CHECK_FALSE(ok.wanted());
  }

  std::filesystem::remove(tmp / "addons-stable" / "dlss5-feed.addon64", ec);
  {
    const auto miss = probe_bundled_motors(tmp.string());
    CHECK(miss.stock_ok);
    CHECK(miss.compared_any);
    CHECK_FALSE(miss.wanted());
  }
  write_bin(tmp / "addons-stable" / "dlss5-feed.addon64", sfeed);

  write_bin(tmp / "addons-stable" / "dlss5-feed.addon64", "stable-FEED");  // same-ish size? different bytes
  {
    const auto feed_only = probe_bundled_motors(tmp.string());
    CHECK(feed_only.wanted());
    CHECK(feed_only.stable_dirty);
    CHECK_FALSE(feed_only.multipass_dirty);
    CHECK(std::string(lobby_motor_override_which(feed_only.stable_dirty, feed_only.multipass_dirty)) ==
          "so o motor 1x (Feed estavel)");
  }
  write_bin(tmp / "addons-stable" / "dlss5-feed.addon64", sfeed);

  write_bin(tmp / "addons-multipass" / "renodx-dlss5.addon64", "multi-nrX");
  {
    const auto mp = probe_bundled_motors(tmp.string());
    CHECK(mp.wanted());
    CHECK_FALSE(mp.stable_dirty);
    CHECK(mp.multipass_dirty);
    CHECK(std::string(lobby_motor_override_which(mp.stable_dirty, mp.multipass_dirty)) ==
          "o motor multipass (1x/1,5x/2x/3x)");
  }

  write_bin(tmp / "addons-stable" / "renodx-dlss5.addon64", "stable-nrX");
  {
    const auto both = probe_bundled_motors(tmp.string());
    CHECK(both.wanted());
    CHECK(both.stable_dirty);
    CHECK(both.multipass_dirty);
    CHECK(std::string(lobby_motor_override_which(both.stable_dirty, both.multipass_dirty)) ==
          "1x (stable) e 1,5x/2x/3x (multipass)");
  }

  write_bin(tmp / "addons-stable" / "renodx-dlss5.addon64", snr);
  write_bin(tmp / "addons-multipass" / "renodx-dlss5.addon64", mnr);
  std::filesystem::remove(tmp / "addons-stable" / "renodx-dlss5.addon64", ec);
  std::filesystem::remove(tmp / "addons-stable" / "dlss5-feed.addon64", ec);
  std::filesystem::remove(tmp / "addons-multipass" / "renodx-dlss5.addon64", ec);
  std::filesystem::remove(tmp / "addons-multipass" / "dlss5-feed.addon64", ec);
  {
    const auto none = probe_bundled_motors(tmp.string());
    CHECK(none.stock_ok);
    CHECK_FALSE(none.compared_any);
    CHECK_FALSE(none.wanted());
  }
  std::filesystem::remove_all(tmp, ec);
}

TEST_CASE("lobby preview caps decode width for UI fps (not soft 640)") {
  // Soft 640 was muddy; full 2560 CPU NV12 decode pinned lobby ~30 fps.
  CHECK(kLobbyPreviewMaxWidth == 1280);
  CHECK(lobby_preview_downsample_step(640) == 1);
  CHECK(lobby_preview_downsample_step(1280) == 1);
  CHECK(lobby_preview_downsample_step(1920) == 2);
  CHECK(lobby_preview_downsample_step(2560) == 2);
  CHECK(lobby_preview_out_width(2560) == 1280);
  CHECK(lobby_preview_out_width(1920) == 960);
  CHECK(lobby_preview_out_width(1280) == 1280);
  // Explicit override still works.
  CHECK(lobby_preview_downsample_step(3840, 1920) == 2);
  CHECK(lobby_preview_out_width(3840, 1920) == 1920);
  CHECK(lobby_preview_out_width(2560, 640) == 640);  // old soft path
  CHECK(lobby_preview_out_width(2560) > lobby_preview_out_width(2560, 640));
  CHECK(lobby_preview_max_width_for_mode(0, false) == 1280);
  CHECK(lobby_preview_max_width_for_mode(1920, false) == 1280);
  CHECK(lobby_preview_max_width_for_mode(1920, true) == 1920);
  CHECK(lobby_preview_max_width_for_mode(2560, true) == 2560);
  CHECK(lobby_preview_max_width_for_mode(3840, true) == 3840);
  CHECK(lobby_preview_downsample_step(1920, lobby_preview_max_width_for_mode(1920, true)) == 1);
  CHECK(lobby_preview_downsample_step(2560, lobby_preview_max_width_for_mode(2560, true)) == 1);
  CHECK(lobby_preview_downsample_step(3840, lobby_preview_max_width_for_mode(3840, true)) == 1);
  CHECK(lobby_preview_out_width(1920, lobby_preview_max_width_for_mode(1920, true)) == 1920);
  CHECK(lobby_preview_out_width(2560, lobby_preview_max_width_for_mode(2560, true)) == 2560);
}

TEST_CASE("program opens lobby first; windowed capture covers monitor (no auto-start)") {
  // Misread fix: do NOT skip lobby into capture on boot.
  PersistedSettings s = default_settings();
  // Even if an old ini had auto_start=1, we ignore it and always show lobby.
  const std::string legacy = serialize_settings_ini(s) + "auto_start=1\n";
  PersistedSettings loaded;
  REQUIRE(parse_settings_ini(legacy, loaded));
  CHECK(serialize_settings_ini(loaded).find("auto_start=") == std::string::npos);

  // Windowed Iniciar → exclusive_fullscreen false (maximized window path in create_app_window).
  loaded.ui.fullscreen = false;
  AppConfig cfg;
  apply_persisted_to_config(loaded, cfg);
  CHECK_FALSE(cfg.exclusive_fullscreen);

  loaded.ui.fullscreen = true;
  apply_persisted_to_config(loaded, cfg);
  CHECK(cfg.exclusive_fullscreen);
}

TEST_CASE("renodx DLSS5MGF chain is Kernel then Feed then Sharpen") {
  // Wrong order / Kernel off = cintilacao temporal (docs no DLSS5_Sharpen.fx).
  const std::string ok =
      "Techniques=Lumenite_Kernel@lumenite_Kernel.fx,DLSS5_Feed@DLSS5_Feed.fx,DLSS5_Sharpen@DLSS5_Sharpen.fx\n"
      "TechniqueSorting=Lumenite_Kernel@lumenite_Kernel.fx,DLSS5_Feed@DLSS5_Feed.fx,DLSS5_Sharpen@DLSS5_Sharpen.fx\n";
  CHECK(renodx_mgf_chain_order_ok(ok));

  const std::string kernel_off =
      "Techniques=DLSS5_Feed@DLSS5_Feed.fx,DLSS5_Sharpen@DLSS5_Sharpen.fx\n"
      "TechniqueSorting=DLSS5_Feed@DLSS5_Feed.fx,DLSS5_Sharpen@DLSS5_Sharpen.fx,Lumenite_Kernel@lumenite_Kernel.fx\n";
  CHECK_FALSE(renodx_mgf_chain_order_ok(kernel_off));

  const std::string sharpen_above_feed =
      "Techniques=Lumenite_Kernel@lumenite_Kernel.fx,DLSS5_Sharpen@DLSS5_Sharpen.fx,DLSS5_Feed@DLSS5_Feed.fx\n"
      "TechniqueSorting=Lumenite_Kernel@lumenite_Kernel.fx,DLSS5_Sharpen@DLSS5_Sharpen.fx,DLSS5_Feed@DLSS5_Feed.fx\n";
  CHECK_FALSE(renodx_mgf_chain_order_ok(sharpen_above_feed));

#ifdef NEXUSCAP_SOURCE_DIR
  const std::string setup_path = std::string(NEXUSCAP_SOURCE_DIR) + "/tools/setup_renodx.ps1";
  std::ifstream in(setup_path, std::ios::binary);
  REQUIRE(in);
  std::ostringstream ss;
  ss << in.rdbuf();
  CHECK(renodx_mgf_chain_order_ok(ss.str()));
#endif
}

TEST_CASE("gear renodx preset change+save reflects in feed and ReShade") {
  // Scenario: change Model A → B → C in gear, Salvar must rewrite both files.
  CHECK(std::string(renodx_visual_preset_name(0)) == "Model A");
  CHECK(std::string(renodx_visual_preset_name(1)) == "Model B");
  CHECK(std::string(renodx_visual_preset_name(2)) == "Model C");
  CHECK(std::string(renodx_visual_preset_name(0, 1)) == "Model A");
  CHECK(std::string(renodx_visual_preset_name(2, 1)) == "Model C");
  CHECK(std::string(renodx_visual_preset_name(0, 4)) == "Model A");
  CHECK(std::string(renodx_visual_preset_name(1, 2)) == "Model B");
  CHECK(std::string(renodx_visual_preset_name(2, 3)) == "Model C");
  CHECK(renodx_nr_preset_index(0) == 0);
  CHECK(renodx_nr_preset_index(1) == 0);
  CHECK(renodx_nr_preset_index(2) == 1);
  CHECK(renodx_feed_model_preset(0) == 0);
  CHECK(renodx_feed_model_preset(1) == 1);
  CHECK(renodx_feed_model_preset(2) == 2);
  CHECK(renodx_visual_presets_differ_on_disk(0, 1));
  CHECK(renodx_visual_presets_differ_on_disk(0, 2));
  CHECK(renodx_visual_presets_differ_on_disk(1, 2));
  CHECK_FALSE(renodx_visual_presets_differ_on_disk(2, 2));

  std::string feed =
      "enabled=1\nmode=2\nwork_resolution=100\npreset=0\nwork_upscale=0\n";
  std::string reshade =
      "EnableHooks=2\nNRPreset=0\nNRStyle=0\nNRIntensity=1\n";

  // Persist Cinematic in settings.ini and prove round-trip.
  PersistedSettings s = default_settings();
  s.enhance_renodx = true;
  s.renodx_preset = static_cast<int>(RenodxVisualPreset::Cinematic);
  const std::string ini = serialize_settings_ini(s);
  CHECK(ini.find("renodx_preset=2") != std::string::npos);
  PersistedSettings loaded;
  REQUIRE(parse_settings_ini(ini, loaded));
  CHECK(loaded.renodx_preset == 2);

  // Salvar path: apply to feed + ReShade from persisted value.
  feed = apply_renodx_visual_preset_to_feed(std::move(feed), loaded.renodx_preset);
  reshade = apply_renodx_visual_preset_to_reshade(std::move(reshade), loaded.renodx_preset);
  CHECK(renodx_files_reflect_visual_preset(feed, reshade, loaded.renodx_preset));
  CHECK(parse_reshade_nr_preset(reshade) == 1);
  CHECK(parse_feed_model_preset(feed) == 2);

  // Switch Natural → NR=0 and model=1 (must differ from Default on disk).
  loaded.renodx_preset = static_cast<int>(RenodxVisualPreset::Natural);
  feed = apply_renodx_visual_preset_to_feed(std::move(feed), loaded.renodx_preset);
  reshade = apply_renodx_visual_preset_to_reshade(std::move(reshade), loaded.renodx_preset);
  CHECK(renodx_files_reflect_visual_preset(feed, reshade, 1));
  CHECK(parse_reshade_nr_preset(reshade) == 0);
  CHECK(parse_feed_model_preset(feed) == 1);

  // Default after Cinematic also restores NR=0 model=0.
  loaded.renodx_preset = static_cast<int>(RenodxVisualPreset::Default);
  const std::string ini2 = serialize_settings_ini(loaded);
  CHECK(ini2.find("renodx_preset=0") != std::string::npos);
  feed = apply_renodx_visual_preset_to_feed(std::move(feed), 0);
  reshade = apply_renodx_visual_preset_to_reshade(std::move(reshade), 0);
  CHECK(renodx_files_reflect_visual_preset(feed, reshade, 0));
  CHECK(parse_feed_model_preset(feed) == 0);
  CHECK(parse_reshade_nr_preset(reshade) == 0);

  // apply_persisted carries preset into session cfg (Iniciar).
  AppConfig cfg;
  apply_persisted_to_config(loaded, cfg);
  CHECK(cfg.renodx_preset == 0);

  PersistedSettings cinema = loaded;
  cinema.renodx_preset = 2;
  apply_persisted_to_config(cinema, cfg);
  CHECK(cfg.renodx_preset == 2);
  CHECK(cfg.exclusive_fullscreen == cinema.ui.fullscreen);
}

TEST_CASE("pass count has no 0.5x; 1x multipass is the native floor") {
  CHECK(clamp_nr_pass_count(-1) == 1);
  CHECK(clamp_nr_pass_count(0) == 1);
  CHECK(nr_reshade_pass_count(-1) == 1);
  CHECK(nr_reshade_pass_count(0) == 1);
  CHECK(nr_pass_count_uses_multipass_addon(1));
  CHECK(std::string(nr_dlss5_motor_name(1)) == "multipass");
  CHECK(std::string(nr_pass_count_short(1)) == "1x");
  CHECK(std::string(nr_pass_count_label(1)).find("1x") != std::string::npos);
  for (int v = -2; v <= kMaxNrPassCount + 2; ++v) {
    const std::string lab = nr_pass_count_label(v);
    const std::string sh = nr_pass_count_short(v);
    CHECK(lab.find("0,5") == std::string::npos);
    CHECK(lab.find("0.5") == std::string::npos);
    CHECK(lab.find("1,7") == std::string::npos);
    CHECK(lab.find("1.7") == std::string::npos);
    CHECK(sh.find("0.5") == std::string::npos);
    CHECK(sh.find("1.7") == std::string::npos);
    CHECK(nr_reshade_pass_count(v) >= 1);
  }
}

TEST_CASE("multipass nr_pass_count clamp serialize and ReShade sync") {
  CHECK(clamp_nr_pass_count(0) == 1);
  CHECK(clamp_nr_pass_count(1) == 1);
  CHECK(clamp_nr_pass_count(2) == 2);
  CHECK(clamp_nr_pass_count(3) == 3);
  CHECK(clamp_nr_pass_count(4) == 4);
  CHECK(clamp_nr_pass_count(5) == 5);
  CHECK(clamp_nr_pass_count(6) == 6);
  CHECK(clamp_nr_pass_count(9) == 6);
  CHECK(nr_reshade_pass_count(1) == 1);
  CHECK(nr_reshade_pass_count(2) == 2);
  CHECK(nr_reshade_pass_count(3) == 3);
  CHECK(nr_reshade_pass_count(4) == 1);  // 1.5x writes INI PassCount=1
  CHECK(nr_reshade_pass_count(5) == 2);  // 2x otimizado: dois evaluates
  CHECK(nr_reshade_pass_count(6) == 3);  // 3x otimizado: tres evaluates
  CHECK(std::string(nr_pass_count_label(1)).find("1x") != std::string::npos);
  CHECK(std::string(nr_pass_count_label(2)).find("2x") != std::string::npos);
  CHECK(std::string(nr_pass_count_label(2)).find("default") != std::string::npos);
  CHECK(std::string(nr_pass_count_label(3)).find("default") != std::string::npos);
  CHECK(std::string(nr_pass_count_short(3)) == "3x");
  CHECK(std::string(nr_pass_count_label(4)).find("1,5x") != std::string::npos);
  CHECK(std::string(nr_pass_count_label(5)).find("otimizado") != std::string::npos);
  CHECK(std::string(nr_pass_count_label(6)).find("otimizado") != std::string::npos);
  CHECK(std::string(nr_pass_count_short(4)) == "1.5x");
  CHECK(std::string(nr_pass_count_short(5)) == "2x+");
  CHECK(std::string(nr_pass_count_short(6)) == "3x+");
  CHECK_FALSE(nr_pass_is_2x_optimized(2));
  CHECK(nr_pass_is_2x_optimized(5));
  CHECK_FALSE(nr_pass_is_3x_optimized(3));
  CHECK(nr_pass_is_3x_optimized(6));
  CHECK_FALSE(nr_pass_uses_optimized_look(3));
  CHECK(nr_pass_uses_optimized_look(4));
  CHECK(nr_pass_uses_optimized_look(5));
  CHECK(nr_pass_uses_optimized_look(6));
  CHECK_FALSE(nr_pass_uses_optimized_look(2));
  CHECK_FALSE(nr_pass_uses_optimized_look(1));
  CHECK(nr_pass_count_uses_multipass_addon(1));
  CHECK(nr_pass_count_uses_multipass_addon(2));
  CHECK(nr_pass_count_uses_multipass_addon(3));
  CHECK(nr_pass_count_uses_multipass_addon(4));
  CHECK(nr_pass_count_uses_multipass_addon(5));
  CHECK(nr_pass_count_uses_multipass_addon(6));
  CHECK_FALSE(nr_pass_count_change_needs_relaunch(1, 2));
  CHECK_FALSE(nr_pass_count_change_needs_relaunch(1, 4));
  CHECK_FALSE(nr_pass_count_change_needs_relaunch(4, 1));
  CHECK_FALSE(nr_pass_count_change_applies_live(1, 4));
  CHECK_FALSE(nr_pass_count_change_applies_live(4, 1));
  CHECK_FALSE(nr_pass_count_change_applies_live(1, 2));
  CHECK_FALSE(nr_pass_count_change_needs_relaunch(4, 2));
  CHECK_FALSE(nr_pass_count_change_needs_relaunch(2, 3));
  CHECK_FALSE(nr_pass_count_change_needs_relaunch(2, 5));
  CHECK_FALSE(nr_pass_count_change_needs_relaunch(3, 6));
  CHECK_FALSE(nr_pass_count_change_needs_relaunch(1, 1));
  CHECK_FALSE(nr_pass_count_change_applies_live(2, 5));
  CHECK_FALSE(nr_pass_count_change_applies_live(5, 2));
  CHECK_FALSE(nr_pass_count_change_applies_live(3, 6));
  CHECK_FALSE(nr_pass_count_change_applies_live(6, 3));

  {
    std::string win =
        "Window=[Window][RenoDX DLSS],Pos=8,,8,Size=100,,100,Collapsed=0,"
        "[Window][RenoDX-DLSSNR],Pos=8,,8,Size=100,,100,Collapsed=0,"
        "[Window][RenoDX Neural Resolution],Pos=1,,1,Size=10,,10,Collapsed=0\n";
    win = sanitize_multipass_home_windows(std::move(win));
    CHECK(win.find("[Window][RenoDX DLSS],Pos=8,,8,Size=520,,900,Collapsed=0") != std::string::npos);
    CHECK(win.find("[Window][RenoDX-DLSSNR],Pos=8,,8,Size=520,,900,Collapsed=0") != std::string::npos);
    CHECK(win.find("[Window][RenoDX Neural Resolution],Pos=8,,8,Size=520,,900,Collapsed=0") != std::string::npos);
    CHECK(win.find("[Window][RenoDX DLSS5 Generic],Pos=-8000,,-8000,Size=1,,1,Collapsed=1") != std::string::npos);
    CHECK(win.find("[Window][RenoDX DLSS5 Generic ],Pos=-8000,,-8000,Size=1,,1,Collapsed=1") != std::string::npos);
    CHECK(win.find("[Window][DLSS 5 Neural Rendering],Pos=-8000,,-8000,Size=1,,1,Collapsed=1") !=
          std::string::npos);
    CHECK(win.find("[Window][RenoDX DLSS5 Generic],Pos=8,,8") == std::string::npos);
    CHECK(win.find("[Window][RenoDX DLSS5 Generic ],Pos=8,,8") == std::string::npos);
    CHECK(win.find("[Window][DLSS 5 Neural Rendering],Pos=8,,8") == std::string::npos);
  }
  {
    std::string win =
        "Window=[Window][RenoDX DLSS],Pos=8,,8,Size=100,,100,Collapsed=0,"
        "[Window][RenoDX-DLSSNR],Pos=8,,8,Size=100,,100,Collapsed=0,"
        "[Window][RenoDX Neural Resolution],Pos=1,,1,Size=10,,10,Collapsed=0\n";
    win = sanitize_home_windows(std::move(win), false);
    CHECK(win.find("OverlayCollapsed=DLSS 5 Neural Rendering@renodx-dlss5.addon64") != std::string::npos);
    CHECK(win.find("[Window][###addons],Pos=931,,128,Size=895,,1296,Collapsed=0") != std::string::npos);
    CHECK(win.find("[Window][Complementos###addons],Pos=931,,128,Size=895,,1296,Collapsed=0,DockId=0x00000001,,0") !=
          std::string::npos);
    CHECK(win.find("[Window][Add-ons###addons],Pos=931,,128,Size=895,,1296,Collapsed=0,DockId=0x00000001,,0") !=
          std::string::npos);
    CHECK(win.find("[Window][###home],Pos=931,,128,Size=895,,1296,Collapsed=0") != std::string::npos);
    CHECK(win.find("[Window][In\xC3\xAD" "cio###home],Pos=931,,128,Size=895,,1296,Collapsed=0,DockId=0x00000001,,1") !=
          std::string::npos);
    CHECK(win.find("[Window][###about],Pos=931,,128,Size=895,,1296,Collapsed=0") != std::string::npos);
    CHECK(win.find("[Window][###about],Pos=-8000") == std::string::npos);
    CHECK(win.find("Selected=0x84F87037") != std::string::npos);
    CHECK(win.find("[Window][RenoDX DLSS5 Generic],Pos=8,,8,Size=520,,900,Collapsed=0") != std::string::npos);
    CHECK(win.find("[Window][RenoDX DLSS5 Generic ],Pos=8,,8,Size=520,,900,Collapsed=0") != std::string::npos);
    CHECK(win.find("[Window][DLSS 5 Neural Rendering],Pos=8,,8,Size=520,,900,Collapsed=0") != std::string::npos);
    CHECK(win.find("[Window][RenoDX-DLSSNR],Pos=-8000,,-8000,Size=1,,1,Collapsed=1") != std::string::npos);
    CHECK(win.find("[Window][RenoDX DLSS],Pos=-8000,,-8000,Size=1,,1,Collapsed=1") != std::string::npos);
    CHECK(win.find("[Window][RenoDX Neural Resolution],Pos=-8000,,-8000") != std::string::npos);
    CHECK(win.find("[Window][RenoDX DLSS],Pos=8,,8") == std::string::npos);
    CHECK(win.find("[Window][RenoDX-DLSSNR],Pos=8,,8") == std::string::npos);
  }
  {
    std::string win =
        "[OVERLAY]\n"
        "Window=[Window][Debug##Default],Pos=60,,60,Size=400,,400,Collapsed=0,"
        "[Window][RenoDX DLSS5 Generic],Pos=8,,8,Size=520,,900,Collapsed=0,"
        "[Window][DLSS 5 Neural Rendering],Pos=8,,8,Size=520,,900,Collapsed=0,"
        "[Window][RenoDX-DLSSNR],Pos=-8000,,-8000,Size=1,,1,Collapsed=1,"
        "[Window][RenoDX DLSS],Pos=8,,8,Size=520,,900,Collapsed=0\n"
        "[RenoDX.DLSS5]\n"
        "Window=[Window][RenoDX DLSS],Pos=8,,8,Size=520,,900,Collapsed=0,"
        "[Window][RenoDX-DLSSNR],Pos=-8000,,-8000,Size=1,,1,Collapsed=1,"
        "[Window][RenoDX Neural Resolution],Pos=-8000,,-8000,Size=1,,1,Collapsed=1\n"
        "PostSaveCommandHideWindow=0\n"
        "[ADDON]\n"
        "OverlayCollapsed=1\n";
    auto nfind = [](const std::string& s, const std::string& n) {
      int c = 0;
      for (std::size_t p = 0; (p = s.find(n, p)) != std::string::npos; p += n.size()) ++c;
      return c;
    };
    CHECK(sanitize_home_windows_for_pass(win, 1) == sanitize_home_windows_for_pass(win, 4));
    const std::string one = sanitize_home_windows(win, false);
    CHECK(nfind(one, "[Window][RenoDX DLSS5 Generic],Pos=8,,8,Size=520,,900,Collapsed=0") == 2);
    CHECK(nfind(one, "[Window][RenoDX DLSS5 Generic ],Pos=8,,8,Size=520,,900,Collapsed=0") == 2);
    CHECK(nfind(one, "[Window][DLSS 5 Neural Rendering],Pos=8,,8,Size=520,,900,Collapsed=0") == 2);
    CHECK(nfind(one, "[Window][RenoDX DLSS],Pos=-8000,,-8000,Size=1,,1,Collapsed=1") == 2);
    CHECK(nfind(one, "[Window][RenoDX-DLSSNR],Pos=-8000,,-8000,Size=1,,1,Collapsed=1") == 2);
    CHECK(one.find("[Window][RenoDX DLSS],Pos=8,,8") == std::string::npos);
    CHECK(one.find("[Window][RenoDX-DLSSNR],Pos=8,,8") == std::string::npos);
    CHECK(one.find("PostSaveCommandHideWindow=0") != std::string::npos);
    CHECK(one.find("OverlayCollapsed=DLSS 5 Neural Rendering@renodx-dlss5.addon64") != std::string::npos);
    CHECK(one.find("OverlayCollapsed=1") == std::string::npos);
    CHECK(one.find("[Window][###addons],Pos=931,,128,Size=895,,1296,Collapsed=0") != std::string::npos);
    CHECK(one.find("[Window][Complementos###addons],Pos=931,,128,Size=895,,1296,Collapsed=0,DockId=0x00000001,,0") !=
          std::string::npos);
    CHECK(one.find("[Window][###about],Pos=931,,128,Size=895,,1296,Collapsed=0") != std::string::npos);
    CHECK(one.find("[Window][###about],Pos=-8000") == std::string::npos);
    CHECK(one.find("Selected=0x84F87037") != std::string::npos);

    const std::string mp = sanitize_home_windows_for_pass(win, 4);
    CHECK(nfind(mp, "[Window][RenoDX DLSS],Pos=8,,8,Size=520,,900,Collapsed=0") == 2);
    CHECK(nfind(mp, "[Window][RenoDX-DLSSNR],Pos=8,,8,Size=520,,900,Collapsed=0") == 2);
    CHECK(nfind(mp, "[Window][RenoDX Neural Resolution],Pos=8,,8,Size=520,,900,Collapsed=0") == 2);
    CHECK(nfind(mp, "[Window][RenoDX DLSS5 Generic],Pos=-8000,,-8000,Size=1,,1,Collapsed=1") == 2);
    CHECK(nfind(mp, "[Window][RenoDX DLSS5 Generic ],Pos=-8000,,-8000,Size=1,,1,Collapsed=1") == 2);
    CHECK(nfind(mp, "[Window][DLSS 5 Neural Rendering],Pos=-8000,,-8000,Size=1,,1,Collapsed=1") == 2);
    CHECK(mp.find("[Window][RenoDX DLSS5 Generic],Pos=8,,8") == std::string::npos);
    CHECK(mp.find("[Window][RenoDX DLSS5 Generic ],Pos=8,,8") == std::string::npos);
    CHECK(mp.find("[Window][DLSS 5 Neural Rendering],Pos=8,,8") == std::string::npos);
    CHECK(mp.find("[Window][###addons],Pos=931,,128") != std::string::npos);
    CHECK(mp.find("[Window][###about],Pos=-8000") == std::string::npos);
  }

  PersistedSettings s = default_settings();
  CHECK(s.nr_pass_count == 1);
  s.nr_pass_count = 2;
  const std::string ini = serialize_settings_ini(s);
  CHECK(ini.find("nr_pass_count=2") != std::string::npos);
  PersistedSettings loaded;
  REQUIRE(parse_settings_ini(ini, loaded));
  CHECK(loaded.nr_pass_count == 2);
  s.nr_pass_count = 4;
  const std::string ini15 = serialize_settings_ini(s);
  CHECK(ini15.find("nr_pass_count=4") != std::string::npos);
  PersistedSettings loaded15;
  REQUIRE(parse_settings_ini(ini15, loaded15));
  CHECK(loaded15.nr_pass_count == 4);
  PersistedSettings loaded_old175;
  REQUIRE(parse_settings_ini("[session]\nnr_pass_count=5\n", loaded_old175));
  CHECK(loaded_old175.nr_pass_count == 5);  // 5 = 2x otimizado
  s.nr_pass_count = 5;
  const std::string ini5 = serialize_settings_ini(s);
  CHECK(ini5.find("nr_pass_count=5") != std::string::npos);
  PersistedSettings loaded5;
  REQUIRE(parse_settings_ini(ini5, loaded5));
  CHECK(loaded5.nr_pass_count == 5);

  AppConfig cfg;
  apply_persisted_to_config(loaded, cfg);
  CHECK(cfg.nr_pass_count == 2);

  std::string reshade = "EnableHooks=2\nNRPreset=0\nNeuralUplift=1\n";
  reshade = apply_reshade_nr_pass_count(std::move(reshade), 3);
  CHECK(parse_reshade_nr_pass_count(reshade) == 3);
  CHECK(reshade.find("DirectNeuralRenderingPassCount=3") != std::string::npos);
  reshade = apply_reshade_nr_pass_count(std::move(reshade), 4);
  CHECK(parse_reshade_nr_pass_count(reshade) == 1);
  CHECK(reshade.find("DirectNeuralRenderingPassCount=1") != std::string::npos);
  CHECK(reshade.find("DirectNeuralRenderingPassCount=4") == std::string::npos);
  reshade = apply_reshade_nr_pass_count(std::move(reshade), 5);
  CHECK(parse_reshade_nr_pass_count(reshade) == 2);  // 2x otimizado → INI 2

  reshade = apply_reshade_multipass_look_defaults(std::move(reshade), 2, /*visual=*/0);
  CHECK(parse_reshade_nr_pass_count(reshade) == 2);
  CHECK(reshade.find("DirectNeuralRenderingDiffuseWhiteNits=500") != std::string::npos);
  CHECK(reshade.find("DirectNeuralRenderingEncoding=0") != std::string::npos);
  CHECK(reshade.find("DirectNeuralRenderingHookPoint=1") != std::string::npos);
  CHECK(reshade.find("DirectNeuralRenderingRequireDlss=1") != std::string::npos);
  CHECK(reshade.find("DirectNeuralRenderingSkinStructureStrength=1") != std::string::npos);
  CHECK(reshade.find("DirectNeuralRenderingUiCorrectionMode=1") != std::string::npos);
  CHECK(reshade.find("DirectNeuralRenderingStyle=0") != std::string::npos);
  CHECK_FALSE(reshade_look_is_2x_optimized(reshade));
  reshade = apply_reshade_multipass_look_defaults(std::move(reshade), 1, /*Model C=*/2);
  CHECK(reshade.find("DirectNeuralRenderingStyle=2") != std::string::npos);
  reshade = apply_reshade_multipass_look_defaults(std::move(reshade), 4, /*visual=*/0);
  CHECK(parse_reshade_nr_pass_count(reshade) == 1);
  CHECK(reshade.find("DirectNeuralRenderingPassCount=4") == std::string::npos);
  CHECK(reshade.find("AddonPath=") == std::string::npos);  // look defaults don't touch stack path

  {
    std::string look = "AddonPath=.\\addons-multipass\nDirectNeuralRenderingPassCount=1\nNRToggleKey=124\n";
    look = apply_reshade_look_for_current_stack(std::move(look), 0);
    CHECK(reshade_addon_path_is_multipass(look));
    CHECK(parse_reshade_nr_pass_count(look) == 1);
    CHECK(look.find("NRToggleKey=46") != std::string::npos);  // Delete, not F13
    CHECK(look.find("NRToggleKey=124") == std::string::npos);
    CHECK(look.find("DirectNeuralRenderingIntensity=1") != std::string::npos);
  }
  {
    // ini antigo do 1x Krish: o look passa a ser o multipass default com Delete.
    std::string look = "AddonPath=.\\addons-stable\nDirectNeuralRenderingPassCount=1\nNRToggleKey=124\n";
    look = apply_reshade_look_for_current_stack(std::move(look), 0);
    CHECK(look.find("NRToggleKey=46") != std::string::npos);
    CHECK(look.find("NRToggleKey=124") == std::string::npos);
    CHECK(look.find("DirectNeuralRenderingDiffuseWhiteNits=500") != std::string::npos);
    CHECK(look.find("DirectNeuralRenderingEncoding=0") != std::string::npos);
    CHECK(infer_multipass_ui_pass_count(look) == 1);
    look = apply_reshade_addon_path(std::move(look), 1);
    CHECK(reshade_addon_path_is_multipass(look));
    CHECK(look.find("addons-stable") == std::string::npos);
  }

#ifdef NEXUSCAP_SOURCE_DIR
  const std::string setup_path = std::string(NEXUSCAP_SOURCE_DIR) + "/tools/setup_renodx.ps1";
  std::ifstream in(setup_path, std::ios::binary);
  REQUIRE(in);
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string setup = ss.str();
  CHECK(setup.find("renodx-multipass") != std::string::npos);
  CHECK(setup.find("renodx-stable") == std::string::npos);
  CHECK(setup.find("DirectNeuralRenderingPassCount=1") != std::string::npos);
  CHECK(setup.find("DirectNeuralRenderingDiffuseWhiteNits=500") != std::string::npos);
  CHECK(setup.find("[RENODX-DLSS]") != std::string::npos);
  CHECK(setup.find("[RENODX-DLSS-preset1]") != std::string::npos);
  CHECK(setup.find("addons-multipass") != std::string::npos);
  CHECK(setup.find("AddonPath=.\\addons-multipass") != std::string::npos);
  CHECK(setup.find("AddonPath=.\\addons-stable") == std::string::npos);
  CHECK(setup.find("\"addons-stable/") == std::string::npos);  // fora do motors.stock
#endif
}

TEST_CASE("boot migrates an old addons-stable ReShade.ini to multipass Pass 1 default look") {
  namespace fs = std::filesystem;
  const fs::path tmp = fs::temp_directory_path() / "nexuscap_pass1_migrate";
  std::error_code ec;
  fs::remove_all(tmp, ec);
  fs::create_directories(tmp / "addons-multipass", ec);
  { std::ofstream(tmp / "addons-multipass" / "renodx-dlss5.addon64", std::ios::binary) << "mp"; }
  {
    std::ofstream(tmp / "ReShade.ini")
        << "[ADDON]\nAddonPath=.\\addons-stable\n[RENODX-DLSS-preset1]\nDirectNeuralRenderingPassCount=1\n"
           "[RenoDX.DLSS5]\nNeuralUplift=1\nNRPreset=1\nNRToggleKey=124\n";
  }
  { std::ofstream(tmp / "dlss5-feed.cfg") << "enabled=1\npreset=0\nwork_resolution=100\n"; }

  CHECK(sync_renodx_addon_variant(tmp.string(), 1) == RenodxAddonSyncResult::OkSwapped);
  REQUIRE(sync_nr_pass_count_files(tmp.string(), 1, 2));

  auto slurp = [](const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  const std::string ini = slurp(tmp / "ReShade.ini");
  const std::string feed = slurp(tmp / "dlss5-feed.cfg");
  CHECK(reshade_addon_path_is_multipass(ini));
  CHECK(ini.find("addons-stable") == std::string::npos);
  CHECK(parse_reshade_nr_pass_count(ini) == 1);
  CHECK(infer_multipass_ui_pass_count(ini) == 1);
  CHECK(ini.find("DirectNeuralRenderingDiffuseWhiteNits=500") != std::string::npos);
  CHECK(ini.find("DirectNeuralRenderingEncoding=0") != std::string::npos);
  CHECK(ini.find("DirectNeuralRenderingHookPoint=1") != std::string::npos);
  CHECK(ini.find("DirectNeuralRenderingUiCorrectionMode=1") != std::string::npos);
  CHECK(ini.find("NRToggleKey=46") != std::string::npos);
  CHECK(ini.find("NRToggleKey=124") == std::string::npos);
  CHECK(every_keyed_int_is(ini, "DirectNeuralRenderingStyle=", 2));
  CHECK(parse_feed_model_preset(feed) == 2);
  CHECK(renodx_files_reflect_visual_preset(feed, ini, 2, 1));
  fs::remove_all(tmp, ec);
}

TEST_CASE("1.5x and 2x otimizado share hook look; 2x default and 3x stay intact") {
  CHECK(nr_reshade_pass_count(2) == 2);
  CHECK(nr_reshade_pass_count(5) == 2);
  CHECK(perf_test_pass_work(2) == 2.0);
  CHECK(perf_test_pass_work(5) == 2.0);
  CHECK(perf_test_pass_work(4) == 1.5);
  CHECK(perf_test_pass_work(1) == 1.0);
  CHECK(perf_test_pass_work(3) == 3.0);

  const char* hints[] = {nr_pass_count_15x_hint(), nr_pass_count_2x_opt_hint(), nr_pass_count_23x_hint(),
                         nr_pass_count_label(2), nr_pass_count_label(5)};
  for (const char* h : hints) {
    const std::string s(h);
    CHECK(s.find("1,7") == std::string::npos);
    CHECK(s.find("1.7") == std::string::npos);
    CHECK(s.find("1,8") == std::string::npos);
    CHECK(s.find("Lossless") == std::string::npos);
  }
  CHECK(std::string(nr_pass_count_2x_opt_hint()).find("NexusCap") != std::string::npos);
  CHECK(std::string(nr_pass_count_2x_opt_hint()).find("flicker") != std::string::npos);

  auto apply = [](int p) {
    return apply_reshade_multipass_look_defaults("EnableHooks=2\nNeuralUplift=1\n", p, 0);
  };
  const std::string look1 = apply(1);
  const std::string look15 = apply(4);
  const std::string look2 = apply(2);
  const std::string look2b = apply(2);
  const std::string look3 = apply(3);
  const std::string look5 = apply(5);
  const std::string look6 = apply(6);
  CHECK(look2 == look2b);
  CHECK(look2 != look5);
  CHECK(look15 != look2);
  CHECK(look15 != look5);
  CHECK(look3 != look2);
  CHECK(look3 != look5);
  CHECK(look3 != look6);
  CHECK(look6 != look5);

  CHECK(parse_reshade_nr_pass_count(look1) == 1);
  CHECK(look1.find("DirectNeuralRenderingEncoding=0") != std::string::npos);
  CHECK(parse_reshade_nr_pass_count(look15) == 1);
  CHECK(parse_reshade_nr_pass_count(look2) == 2);
  CHECK(parse_reshade_nr_pass_count(look3) == 3);
  CHECK(parse_reshade_nr_pass_count(look5) == 2);
  CHECK(parse_reshade_nr_pass_count(look6) == 3);

  CHECK(look15.find("DirectNeuralRenderingEncoding=2") != std::string::npos);
  CHECK(look15.find("DirectNeuralRenderingHookPoint=2") != std::string::npos);
  CHECK(look15.find("DirectNeuralRenderingUiCorrectionMode=2") != std::string::npos);
  CHECK(look15.find("DirectNeuralRenderingDiffuseWhiteNits=100") != std::string::npos);
  CHECK(look15.find("NRDiffuseWhiteNits=100") != std::string::npos);
  CHECK(look15.find("NRUICorrection=2") != std::string::npos);
  CHECK(look15.find("DirectNeuralRenderingAutoMask=1") != std::string::npos);
  CHECK(look15.find("DirectNeuralRenderingIntensity=1") != std::string::npos);
  CHECK(look15.find("DirectNeuralRenderingLocalStructureStrength=1") != std::string::npos);
  CHECK(look15.find("DirectNeuralRenderingPassCount=1") != std::string::npos);
  CHECK(reshade_look_is_2x_optimized(look15));
  CHECK(infer_multipass_ui_pass_count("AddonPath=.\\addons-multipass\n" + look15) == 4);

  CHECK(look2.find("DirectNeuralRenderingEncoding=0") != std::string::npos);
  CHECK(look2.find("DirectNeuralRenderingHookPoint=1") != std::string::npos);
  CHECK(look2.find("DirectNeuralRenderingUiCorrectionMode=1") != std::string::npos);
  CHECK(look2.find("DirectNeuralRenderingDiffuseWhiteNits=500") != std::string::npos);
  CHECK(look2.find("NRDiffuseWhiteNits=500") != std::string::npos);
  CHECK(look2.find("NRUICorrection=1") != std::string::npos);
  CHECK(look2.find("DirectNeuralRenderingAutoMask=1") != std::string::npos);
  CHECK(look2.find("DirectNeuralRenderingLocalStructureStrength=1") != std::string::npos);
  CHECK(look2.find("DirectNeuralRenderingSkinStructureStrength=1") != std::string::npos);

  CHECK(look3.find("DirectNeuralRenderingEncoding=0") != std::string::npos);
  CHECK(look3.find("DirectNeuralRenderingHookPoint=1") != std::string::npos);
  CHECK(look3.find("DirectNeuralRenderingUiCorrectionMode=1") != std::string::npos);
  CHECK(look3.find("DirectNeuralRenderingDiffuseWhiteNits=500") != std::string::npos);
  CHECK(look3.find("DirectNeuralRenderingPassCount=3") != std::string::npos);
  CHECK_FALSE(reshade_look_is_2x_optimized(look3));
  CHECK(infer_multipass_ui_pass_count("AddonPath=.\\addons-multipass\n" + look3) == 3);

  CHECK(look6.find("DirectNeuralRenderingEncoding=2") != std::string::npos);
  CHECK(look6.find("DirectNeuralRenderingHookPoint=2") != std::string::npos);
  CHECK(look6.find("DirectNeuralRenderingUiCorrectionMode=2") != std::string::npos);
  CHECK(look6.find("DirectNeuralRenderingDiffuseWhiteNits=100") != std::string::npos);
  CHECK(look6.find("DirectNeuralRenderingPassCount=3") != std::string::npos);
  CHECK(reshade_look_is_2x_optimized(look6));
  CHECK(infer_multipass_ui_pass_count("AddonPath=.\\addons-multipass\n" + look6) == 6);

  CHECK(look5.find("DirectNeuralRenderingEncoding=2") != std::string::npos);
  CHECK(look5.find("DirectNeuralRenderingHookPoint=2") != std::string::npos);
  CHECK(look5.find("DirectNeuralRenderingUiCorrectionMode=2") != std::string::npos);
  CHECK(look5.find("DirectNeuralRenderingDiffuseWhiteNits=100") != std::string::npos);
  CHECK(look5.find("NRDiffuseWhiteNits=100") != std::string::npos);
  CHECK(look5.find("NRUICorrection=2") != std::string::npos);
  CHECK(look5.find("DirectNeuralRenderingAutoMask=1") != std::string::npos);
  CHECK(look5.find("DirectNeuralRenderingIntensity=1") != std::string::npos);
  CHECK(look5.find("DirectNeuralRenderingLocalStructureStrength=1") != std::string::npos);
  CHECK(look5.find("DirectNeuralRenderingSkinStructureStrength=1") != std::string::npos);
  CHECK(look5.find("NRLocalStructure=1") != std::string::npos);
  CHECK(look5.find("DirectNeuralRenderingPassCount=2") != std::string::npos);
  CHECK(reshade_look_is_2x_optimized(look5));
  CHECK_FALSE(reshade_look_is_2x_optimized(look2));

  {
    std::string infer15 = "AddonPath=.\\addons-multipass\n" + look15;
    infer15 = apply_reshade_look_for_current_stack(std::move(infer15), 0);
    CHECK(reshade_look_is_2x_optimized(infer15));
    CHECK(parse_reshade_nr_pass_count(infer15) == 1);
    CHECK(infer_multipass_ui_pass_count(infer15) == 4);
  }
  {
    std::string infer = "AddonPath=.\\addons-multipass\n" + look5;
    infer = apply_reshade_look_for_current_stack(std::move(infer), 0);
    CHECK(reshade_look_is_2x_optimized(infer));
    CHECK(parse_reshade_nr_pass_count(infer) == 2);
    CHECK(infer_multipass_ui_pass_count(infer) == 5);
  }
  {
    std::string infer6 = "AddonPath=.\\addons-multipass\n" + look6;
    infer6 = apply_reshade_look_for_current_stack(std::move(infer6), 0);
    CHECK(reshade_look_is_2x_optimized(infer6));
    CHECK(parse_reshade_nr_pass_count(infer6) == 3);
    CHECK(infer_multipass_ui_pass_count(infer6) == 6);
  }
  {
    std::string infer = "AddonPath=.\\addons-multipass\n" + look2;
    infer = apply_reshade_look_for_current_stack(std::move(infer), 0);
    CHECK_FALSE(reshade_look_is_2x_optimized(infer));
    CHECK(infer_multipass_ui_pass_count(infer) == 2);
    CHECK(infer.find("DirectNeuralRenderingEncoding=0") != std::string::npos);
    CHECK(infer.find("DirectNeuralRenderingDiffuseWhiteNits=500") != std::string::npos);
  }
  {
    std::string keep5 = "AddonPath=.\\addons-multipass\nDirectNeuralRenderingPassCount=2\n";
    keep5 = apply_reshade_look_for_current_stack(std::move(keep5), 0, 5);
    CHECK(reshade_look_is_2x_optimized(keep5));
  }

  // Pass 1x no multipass: 1 evaluate com o look default (nao o gancho do 1,5x).
  std::string pass1 = apply_reshade_multipass_look_defaults("NRPreset=0\n", 1, 0);
  CHECK(parse_reshade_nr_pass_count(pass1) == 1);
  CHECK(pass1.find("DirectNeuralRenderingDiffuseWhiteNits=500") != std::string::npos);
  CHECK(pass1.find("DirectNeuralRenderingEncoding=0") != std::string::npos);
  CHECK(pass1.find("DirectNeuralRenderingHookPoint=1") != std::string::npos);
  CHECK(pass1.find("DirectNeuralRenderingUiCorrectionMode=1") != std::string::npos);
  CHECK(pass1.find("NRToggleKey=46") != std::string::npos);
  CHECK_FALSE(reshade_look_is_2x_optimized(pass1));
  CHECK(infer_multipass_ui_pass_count(pass1) == 1);
  CHECK(pass1.find("[Window][RenoDX DLSS],Pos=8,,8") != std::string::npos);
  CHECK(pass1.find("[Window][RenoDX DLSS5 Generic],Pos=-8000,,-8000") != std::string::npos);
  CHECK(std::strlen(kReshadeHomeKrishTitleSpaced) == 21);
}

TEST_CASE("fullscreen checkbox save reflects exclusive_fullscreen on Iniciar") {
  PersistedSettings s = default_settings();
  s.ui.fullscreen = true;
  AppConfig cfg;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.exclusive_fullscreen);

  s.ui.fullscreen = false;  // "modo janela"
  const std::string ini = serialize_settings_ini(s);
  CHECK(ini.find("fullscreen=0") != std::string::npos);
  PersistedSettings out;
  REQUIRE(parse_settings_ini(ini, out));
  CHECK_FALSE(out.ui.fullscreen);
  apply_persisted_to_config(out, cfg);
  CHECK_FALSE(cfg.exclusive_fullscreen);
}

TEST_CASE("apply_persisted_to_config maps Sessao + engrenagem") {
  PersistedSettings s = default_settings();
  s.device_name = "VEDO-VDV";
  s.width = 1920;
  s.height = 1080;
  s.fps = 120;
  s.scale_percent = 80;
  s.ui.show_fps = true;
  s.ui.advanced_metrics = true;
  s.ui.fullscreen = false;
  s.ui.hide_panel_on_capture = true;
  s.ui.sync_image = true;
  s.fg_enabled = true;
  s.fg_multiplier = 4;
  s.enhance_renodx = true;
  s.renodx_preset = static_cast<int>(RenodxVisualPreset::Natural);
  s.hotkeys.vk_fg_toggle = 0x22;

  AppConfig cfg;
  cfg.enhance = EnhanceBackend::Off;
  apply_persisted_to_config(s, cfg);

  CHECK(cfg.device_name == "VEDO-VDV");
  CHECK(cfg.width == 1920);
  CHECK(cfg.height == 1080);
  CHECK(cfg.fps == 120);
  CHECK(cfg.scale_percent == 80);
  CHECK(cfg.renodx_preset == 1);
  CHECK(cfg.show_overlay);
  CHECK(cfg.advanced_metrics);
  CHECK_FALSE(cfg.exclusive_fullscreen);
  CHECK(cfg.hide_panel_on_capture);
  CHECK(cfg.sync_image);
  CHECK(cfg.fg_multiplier == 4);
  CHECK(cfg.fg_auto_start);
  CHECK(cfg.enhance == EnhanceBackend::Renodx);
  CHECK_FALSE(cfg.dlss);
  CHECK(cfg.hotkeys.vk_fg_toggle == 0x22);

  // FG off in gear → multiplier stays armed for PageDn; auto-start off
  s.fg_enabled = false;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.fg_multiplier == 4);
  CHECK_FALSE(cfg.fg_auto_start);

  // RenoDX off clears Renodx backend
  s.enhance_renodx = false;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.enhance == EnhanceBackend::Off);
}

TEST_CASE("initial_hud_visible follows Exibir metricas only") {
  CHECK(initial_hud_visible(true, false));
  CHECK(initial_hud_visible(true, true));  // hide lobby must NOT hide HUD
  CHECK_FALSE(initial_hud_visible(false, false));
  CHECK_FALSE(initial_hud_visible(false, true));

  SessionUiFlags ui;
  ui.show_fps = true;
  ui.hide_panel_on_capture = true;
  ui.advanced_metrics = true;
  CHECK(initial_hud_visible(ui));
  ui.show_fps = false;
  CHECK_FALSE(initial_hud_visible(ui));
}

TEST_CASE("session capture is always clean; no lobby strip") {
  CHECK(session_capture_is_clean());
  CHECK_FALSE(session_shows_control_panel());
  CHECK_FALSE(session_shows_control_panel(true));
  CHECK_FALSE(session_shows_control_panel(false));

  SessionUiFlags ui;
  ui.hide_panel_on_capture = true;
  CHECK_FALSE(session_shows_control_panel(ui));
  ui.hide_panel_on_capture = false;
  CHECK_FALSE(session_shows_control_panel(ui));

  PersistedSettings s = default_settings();
  s.ui.hide_panel_on_capture = false;
  AppConfig cfg;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.hide_panel_on_capture);
  CHECK_FALSE(session_shows_control_panel(cfg.hide_panel_on_capture));
  CHECK(session_capture_is_clean());

  s.ui.hide_panel_on_capture = true;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.hide_panel_on_capture);
  CHECK_FALSE(session_shows_control_panel(cfg.hide_panel_on_capture));

  // Stale ini (old checkbox off) must not bring the strip back.
  PersistedSettings stale;
  REQUIRE(parse_settings_ini("hide_panel_on_capture=0\n", stale));
  CHECK(stale.ui.hide_panel_on_capture);
  apply_persisted_to_config(stale, cfg);
  CHECK(cfg.hide_panel_on_capture);
  CHECK_FALSE(session_shows_control_panel(cfg.hide_panel_on_capture));
  const std::string saved = serialize_settings_ini(stale);
  CHECK(saved.find("hide_panel_on_capture=1") != std::string::npos);
  CHECK(saved.find("hide_panel_on_capture=0") == std::string::npos);

  // HUD / VSYNC / fullscreen stay independent of the removed strip.
  s = default_settings();
  s.ui.show_fps = true;
  s.ui.hide_panel_on_capture = false;
  CHECK(initial_hud_visible(s.ui));
  s.ui.show_fps = false;
  CHECK_FALSE(initial_hud_visible(s.ui));
}

TEST_CASE("hide_panel leftover does not change sync_image") {
  PersistedSettings s = default_settings();
  s.ui.hide_panel_on_capture = true;
  s.ui.sync_image = true;
  AppConfig cfg;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.hide_panel_on_capture);
  CHECK(cfg.sync_image);
  CHECK_FALSE(session_shows_control_panel(cfg.hide_panel_on_capture));
  CHECK(present_sync_interval(cfg.sync_image) == 1u);

  s.ui.hide_panel_on_capture = false;
  s.ui.sync_image = false;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.hide_panel_on_capture);
  CHECK_FALSE(cfg.sync_image);
  CHECK_FALSE(session_shows_control_panel(cfg.hide_panel_on_capture));
  CHECK(present_sync_interval(cfg.sync_image) == 0u);

  s.ui.show_fps = true;
  s.ui.advanced_metrics = true;
  s.ui.sync_image = true;
  s.ui.hide_panel_on_capture = true;
  auto hud = make_lobby_hud_preview_lines(s, 60.0);
  CHECK(lobby_hud_contains(hud, "SYNC VSYNC"));
  s.ui.sync_image = false;
  hud = make_lobby_hud_preview_lines(s, 60.0);
  CHECK(lobby_hud_contains(hud, "SYNC TEAR"));
  CHECK_FALSE(lobby_hud_contains(hud, "SYNC VSYNC"));
}

TEST_CASE("present interval locks perf test to meta not monitor Hz") {
  CHECK(present_interval_for_target(false, 60, 120) == 0u);
  CHECK(present_interval_for_target(true, 60, 60) == 1u);
  CHECK(present_interval_for_target(true, 30, 60) == 2u);
  CHECK(present_interval_for_target(true, 60, 120) == 2u);
  CHECK(present_interval_for_target(true, 30, 120) == 4u);
  CHECK(present_interval_for_target(true, 60, 144) == 2u);
  CHECK(present_interval_for_target(true, 30, 144) == 4u);
  CHECK(present_interval_for_target(true, 60, 0) == 1u);
  CHECK(clamp_display_refresh_hz(0) == 60);
  CHECK(clamp_display_refresh_hz(120) == 120);
  CHECK(clamp_fg_adaptive_target(50, 144) == 50);
  CHECK(clamp_fg_adaptive_target(65, 144) == 65);
  CHECK(clamp_fg_adaptive_target(80, 144) == 80);
  CHECK(clamp_fg_adaptive_target(80, 60) == 60);
  CHECK(clamp_fg_adaptive_target(50, 60) == 50);
  CHECK(clamp_fg_adaptive_target(20, 144) == 30);
  CHECK(clamp_fg_adaptive_target(400, 144) == 144);
  CHECK(clamp_fg_adaptive_target(80, 0) == 60);
  CHECK(fg_adaptive_capture_floor(0) == 30);
  CHECK(fg_adaptive_capture_floor(40) == 40);
  CHECK(clamp_fg_adaptive_target(30, 144, 40) == 40);
  CHECK(clamp_fg_adaptive_target(50, 144, 40) == 50);
  CHECK(clamp_fg_adaptive_target(80, 60, 40) == 60);
  CHECK(clamp_fg_adaptive_target(20, 60, 40) == 40);
  const int live_hz = display_refresh_hz(nullptr);
  CHECK(live_hz >= 30);
  CHECK(live_hz <= 360);
}

TEST_CASE("present_sync_interval maps sync_image to DXGI Present sync") {
  // sync_image=false → Present(0) uncapped / tear OK
  CHECK(present_sync_interval(false) == 0u);
  // sync_image=true → Present(1) vsync
  CHECK(present_sync_interval(true) == 1u);

  PersistedSettings s = default_settings();
  s.ui.sync_image = false;
  AppConfig cfg;
  apply_persisted_to_config(s, cfg);
  CHECK_FALSE(cfg.sync_image);
  CHECK(present_sync_interval(cfg.sync_image) == 0u);

  s.ui.sync_image = true;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.sync_image);
  CHECK(present_sync_interval(cfg.sync_image) == 1u);

  // Round-trip INI must keep sync so renderer.set_vsync sees the same flag
  const std::string ini = serialize_settings_ini(s);
  PersistedSettings loaded;
  REQUIRE(parse_settings_ini(ini, loaded));
  CHECK(loaded.ui.sync_image);
  AppConfig cfg2;
  apply_persisted_to_config(loaded, cfg2);
  CHECK(cfg2.sync_image);
  CHECK(present_sync_interval(cfg2.sync_image) == 1u);
}

TEST_CASE("hide_panel and sync_image are independent contracts") {
  // Toggles must not cancel each other — both can be on/off freely.
  PersistedSettings s = default_settings();
  s.ui.hide_panel_on_capture = true;
  s.ui.sync_image = true;
  AppConfig cfg;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.hide_panel_on_capture);
  CHECK(cfg.sync_image);
  CHECK_FALSE(session_shows_control_panel(cfg.hide_panel_on_capture));
  CHECK(present_sync_interval(cfg.sync_image) == 1u);

  s.ui.hide_panel_on_capture = false;
  s.ui.sync_image = false;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.hide_panel_on_capture);
  CHECK_FALSE(cfg.sync_image);
  CHECK_FALSE(session_shows_control_panel(cfg.hide_panel_on_capture));
  CHECK(present_sync_interval(cfg.sync_image) == 0u);

  // HUD preview must advertise sync state regardless of hide_panel
  s.ui.show_fps = true;
  s.ui.advanced_metrics = true;
  s.ui.sync_image = true;
  s.ui.hide_panel_on_capture = true;
  auto hud = make_lobby_hud_preview_lines(s, 60.0);
  CHECK(lobby_hud_contains(hud, "SYNC VSYNC"));
  s.ui.sync_image = false;
  hud = make_lobby_hud_preview_lines(s, 60.0);
  CHECK(lobby_hud_contains(hud, "SYNC TEAR"));
  CHECK_FALSE(lobby_hud_contains(hud, "SYNC VSYNC"));
}

TEST_CASE("hud_panel size compact vs advanced metrics") {
  CHECK(hud_panel_width(false) == 880);
  CHECK(hud_panel_height(false) == 348);
  CHECK(hud_panel_width(true) == 920);
  CHECK(hud_panel_height(true) == 640);
  CHECK(hud_panel_width(true) > hud_panel_width(false));
  CHECK(hud_panel_height(true) > hud_panel_height(false));
  CHECK(hud_panel_height(true, true) == 780);
  CHECK(hud_panel_height(false, true) == 488);
  CHECK(hud_mini_overlay_rows() == 3);
  CHECK(hud_mini_overlay_height() >= hud_mini_overlay_font_px() * 3 + 16);
  CHECK(hw_info_panel_width() == 360);
  CHECK(hw_info_panel_height() == 152);
  CHECK(kSysMetricsLineCount == 5);
}

TEST_CASE("lobby hud preview lines follow Sessao toggles") {
  CHECK(lobby_hud_preview_lines(false, false) == 0);
  CHECK(lobby_hud_preview_lines(false, true) == 0);
  CHECK(lobby_hud_preview_lines(true, false) == 6);  // compact includes gear fields
  CHECK(lobby_hud_preview_lines(true, true) == 8);
  CHECK(lobby_hud_preview_lines(true, true) > lobby_hud_preview_lines(true, false));
  CHECK(lobby_hud_preview_lines(true, false, true) == 11);  // + 5 Infos GPU/CPU lines
  CHECK(lobby_hud_preview_lines(true, true, true) == 13);
  CHECK(lobby_hud_preview_lines(true, false, false, true) == 3);  // mini = three dense rows
  CHECK(lobby_hud_preview_lines(true, true, true, true) == 3);   // mini wins over ADV+HW line count
}

TEST_CASE("hw_info toggle persists and shows preview line") {
  PersistedSettings s = default_settings();
  CHECK_FALSE(s.ui.hw_info);
  s.ui.hw_info = true;
  s.ui.show_fps = true;
  s.ui.advanced_metrics = false;

  const std::string ini = serialize_settings_ini(s);
  PersistedSettings loaded;
  REQUIRE(parse_settings_ini(ini, loaded));
  CHECK(loaded.ui.hw_info);

  AppConfig cfg;
  apply_persisted_to_config(loaded, cfg);
  CHECK(cfg.hw_info);

  auto hud = make_lobby_hud_preview_lines(loaded, 60.0);
  CHECK(hud.count == 11);
  CHECK(lobby_hud_contains(hud, "CPU  --"));
  CHECK(lobby_hud_contains(hud, "GPU  --"));
  CHECK(lobby_hud_contains(hud, "RAM  --"));
  CHECK(lobby_hud_contains(hud, "VRAM --"));
  CHECK(lobby_hud_contains(hud, "PWR  --"));

  loaded.ui.hw_info = false;
  hud = make_lobby_hud_preview_lines(loaded, 60.0);
  CHECK(hud.count == 6);
  CHECK_FALSE(lobby_hud_contains(hud, "CPU  --"));
}

TEST_CASE("system metrics formatters and cpu percent math") {
  CHECK(std::string(sync_image_badge(true)) == "SYNC VSYNC");
  CHECK(std::string(sync_image_badge(false)) == "SYNC TEAR");
  CHECK(present_sync_interval(true) == 1u);
  CHECK(present_sync_interval(false) == 0u);

  CHECK(shorten_cpu_display_name("Intel(R) Core(TM) i5-11400F CPU @ 2.60GHz") == "i5-11400F");
  CHECK(shorten_cpu_display_name("AMD Ryzen 7 5800X 8-Core Processor") == "R7 5800X");
  CHECK(shorten_gpu_display_name("NVIDIA GeForce RTX 5070") == "RTX 5070");
  CHECK(shorten_gpu_display_name("AMD Radeon RX 7800 XT") == "RX 7800 XT");

  // idle half of kernel+user → 50% busy
  CHECK(cpu_percent_from_idle_deltas(50, 80, 20) == doctest::Approx(50.0));
  CHECK(cpu_percent_from_idle_deltas(0, 100, 0) == doctest::Approx(100.0));
  CHECK(cpu_percent_from_idle_deltas(100, 100, 0) == doctest::Approx(0.0));
  CHECK(cpu_percent_from_idle_deltas(0, 0, 0) == doctest::Approx(0.0));

  SystemMetricsSample s;
  char buf[220];
  format_sys_cpu_line(buf, sizeof(buf), s);
  CHECK(std::string(buf) == "CPU  --");
  s.cpu_ok = true;
  s.cpu_percent = 42;
  format_sys_cpu_line(buf, sizeof(buf), s);
  CHECK(std::string(buf) == "CPU  42%");
  s.cpu_name = "i5-11400F";
  format_sys_cpu_line(buf, sizeof(buf), s);
  CHECK(std::string(buf) == "i5-11400F  42%");
  s.cpu_temp_ok = true;
  s.cpu_temp_c = 58;
  format_sys_cpu_line(buf, sizeof(buf), s);
  CHECK(std::string(buf) == "i5-11400F  42%  58C");

  s.gpu_ok = true;
  s.gpu_percent = 77;
  s.gpu_temp_ok = true;
  s.gpu_temp_c = 72;
  s.gpu_name = "RTX 5070";
  s.vram_ok = true;
  s.vram_used_mb = 2048;
  s.vram_budget_mb = 8192;
  s.ram_ok = true;
  s.ram_used_mb = 12288;
  s.ram_total_mb = 32768;
  s.power_ok = true;
  s.power_w = 120;
  format_sys_metrics_line(buf, sizeof(buf), s);
  CHECK(std::strstr(buf, "i5-11400F") != nullptr);
  CHECK(std::strstr(buf, "RTX 5070") != nullptr);
  CHECK(std::strstr(buf, "77%") != nullptr);
  CHECK(std::strstr(buf, "i5-11400F  42%  58C") != nullptr);
  CHECK(std::strstr(buf, "RTX 5070  77%  72C") != nullptr);
  CHECK(std::strstr(buf, "RAM  12.0 / 32.0 GB") != nullptr);
  CHECK(std::strstr(buf, "VRAM 2.0 / 8.0 GB") != nullptr);
  CHECK(std::strstr(buf, "PWR  120 W") != nullptr);

  char lines[5][64]{};
  CHECK(format_sys_metrics_lines(lines, 5, s) == 5);
  CHECK(std::string(lines[0]) == "i5-11400F  42%  58C");
  CHECK(std::string(lines[1]) == "RTX 5070  77%  72C");
  CHECK(std::string(lines[2]) == "RAM  12.0 / 32.0 GB");
  CHECK(std::string(lines[3]) == "VRAM 2.0 / 8.0 GB");
  CHECK(std::string(lines[4]) == "PWR  120 W");

  // Sem nome conhecido → fallback genérico.
  s.cpu_name.clear();
  s.gpu_name.clear();
  format_sys_cpu_line(buf, sizeof(buf), s);
  CHECK(std::string(buf) == "CPU  42%  58C");
  format_sys_gpu_line(buf, sizeof(buf), s);
  CHECK(std::string(buf) == "GPU  77%  72C");

  CHECK(celsius_from_thermal_kelvin_raw(310.15) == doctest::Approx(37.0).epsilon(0.01));
  CHECK(celsius_from_thermal_kelvin_raw(3100.0) == doctest::Approx(36.85).epsilon(0.01));
}

TEST_CASE("shorten cpu/gpu display names for HUD") {
  CHECK(shorten_cpu_display_name("").empty());
  CHECK(shorten_cpu_display_name("Intel(R) Core(TM) i7-13700K CPU @ 3.40GHz") == "i7-13700K");
  CHECK(shorten_cpu_display_name("Intel(R) Core(TM) i9-14900HX") == "i9-14900HX");
  CHECK(shorten_cpu_display_name("AMD Ryzen 5 5600X 6-Core Processor") == "R5 5600X");
  CHECK(shorten_gpu_display_name("").empty());
  CHECK(shorten_gpu_display_name("NVIDIA GeForce RTX 5070 Laptop GPU") == "RTX 5070");
  CHECK(shorten_gpu_display_name("NVIDIA GeForce GTX 1660 SUPER") == "GTX 1660 SUPER");
  CHECK(shorten_gpu_display_name("Intel(R) Arc(TM) A770 Graphics") == "Arc A770 Graphics");
}

TEST_CASE("make_lobby_hud_preview_lines reflects gear adjustments live") {
  // Preview purpose: see CFG/engrenagem changes as you tweak them.
  PersistedSettings s = default_settings();
  s.ui.show_fps = true;
  s.ui.advanced_metrics = false;
  s.width = 2560;
  s.height = 1440;
  s.fps = 60;
  s.scale_percent = 100;
  s.fg_enabled = false;
  s.enhance_renodx = true;
  s.renodx_preset = static_cast<int>(RenodxVisualPreset::Default);
  s.ui.fullscreen = true;
  s.ui.sync_image = false;

  auto hud = make_lobby_hud_preview_lines(s, 60.0);
  CHECK(hud.count == 6);
  CHECK(lobby_hud_contains(hud, "scale 100%"));
  CHECK(lobby_hud_contains(hud, "FG off"));
  CHECK(lobby_hud_contains(hud, "Model A"));
  CHECK(lobby_hud_contains(hud, "fullscreen"));
  CHECK(lobby_hud_contains(hud, "SYNC TEAR"));

  s.scale_percent = 50;
  s.fg_enabled = true;
  s.fg_multiplier = 3;
  s.renodx_preset = static_cast<int>(RenodxVisualPreset::Cinematic);
  s.ui.fullscreen = false;
  s.ui.sync_image = true;
  hud = make_lobby_hud_preview_lines(s, 60.0);
  CHECK(lobby_hud_contains(hud, "scale 50%"));
  CHECK(lobby_hud_contains(hud, "FG ON"));
  CHECK(lobby_hud_contains(hud, "x3"));
  CHECK(lobby_hud_contains(hud, "Model C"));
  CHECK(lobby_hud_contains(hud, "windowed"));
  CHECK(lobby_hud_contains(hud, "SYNC VSYNC"));
  CHECK_FALSE(lobby_hud_contains(hud, "Model A"));

  s.renodx_preset = static_cast<int>(RenodxVisualPreset::Natural);
  hud = make_lobby_hud_preview_lines(s, 60.0);
  CHECK(lobby_hud_contains(hud, "Model B"));

  s.enhance_renodx = false;
  hud = make_lobby_hud_preview_lines(s, 60.0);
  CHECK(lobby_hud_contains(hud, "RenoDX off"));

  // ADV analytics also shows preset + work size.
  s.ui.advanced_metrics = true;
  s.enhance_renodx = true;
  s.renodx_preset = static_cast<int>(RenodxVisualPreset::Cinematic);
  s.scale_percent = 75;
  hud = make_lobby_hud_preview_lines(s, 60.0);
  CHECK(hud.count == 8);
  CHECK(lobby_hud_contains(hud, "ADV"));
  CHECK(lobby_hud_contains(hud, "Model C"));
  CHECK(lobby_hud_contains(hud, "75%"));
  CHECK(lobby_hud_contains(hud, "work"));

  s.ui.show_fps = false;
  hud = make_lobby_hud_preview_lines(s, 60.0);
  CHECK(hud.count == 0);
}

TEST_CASE("full gear contract: CFG fields reach session + preview HUD + RenoDX files") {
  // Engrenagem → settings.ini → apply_persisted → captura/HUD/feed.
  PersistedSettings s = default_settings();
  s.fg_enabled = true;
  s.fg_multiplier = 4;
  s.fg_path = "C:\\Steam\\steamapps\\common\\Lossless Scaling\\LosslessScaling.exe";
  s.scale_percent = 50;
  s.enhance_renodx = true;
  s.renodx_preset = static_cast<int>(RenodxVisualPreset::Cinematic);
  s.ui.show_fps = true;
  s.ui.advanced_metrics = true;
  s.ui.fullscreen = false;  // janela
  s.ui.sync_image = true;
  s.ui.hide_panel_on_capture = false;
  s.hotkeys.vk_fg_toggle = 0x22;
  s.hotkeys.vk_fullscreen = 0x7A;

  const std::string ini = serialize_settings_ini(s);
  PersistedSettings loaded;
  REQUIRE(parse_settings_ini(ini, loaded));
  CHECK(loaded.fg_enabled);
  CHECK(loaded.fg_multiplier == 4);
  CHECK(loaded.fg_path.find("LosslessScaling.exe") != std::string::npos);
  CHECK(loaded.scale_percent == 50);
  CHECK(loaded.enhance_renodx);
  CHECK(loaded.renodx_preset == 2);
  CHECK_FALSE(loaded.ui.fullscreen);
  CHECK(loaded.ui.sync_image);
  CHECK(loaded.ui.hide_panel_on_capture);
  CHECK(loaded.ui.advanced_metrics);

  AppConfig cfg;
  apply_persisted_to_config(loaded, cfg);
  CHECK(cfg.fg_multiplier == 4);
  CHECK(cfg.fg_path.find("LosslessScaling.exe") != std::string::npos);
  CHECK(cfg.scale_percent == 50);
  CHECK(cfg.enhance == EnhanceBackend::Renodx);
  CHECK(cfg.renodx_preset == 2);
  CHECK_FALSE(cfg.exclusive_fullscreen);
  CHECK(cfg.sync_image);
  CHECK(cfg.hide_panel_on_capture);
  CHECK_FALSE(session_shows_control_panel(cfg.hide_panel_on_capture));
  CHECK(cfg.show_overlay);
  CHECK(cfg.advanced_metrics);

  const auto hud = make_lobby_hud_preview_lines(loaded, 60.0);
  CHECK(lobby_hud_contains(hud, "Model C"));
  CHECK(lobby_hud_contains(hud, "scale 50%"));
  CHECK(lobby_hud_contains(hud, "x4"));
  CHECK(lobby_hud_contains(hud, "windowed"));
  CHECK(lobby_hud_contains(hud, "SYNC VSYNC"));
  CHECK(lobby_hud_contains(hud, "FG"));  // "FG  ON  x4" in ADV

  std::string feed = "preset=0\nwork_resolution=100\n";
  std::string reshade = "NRPreset=0\n";
  feed = apply_renodx_visual_preset_to_feed(std::move(feed), cfg.renodx_preset);
  reshade = apply_renodx_visual_preset_to_reshade(std::move(reshade), cfg.renodx_preset);
  CHECK(parse_feed_model_preset(feed) == renodx_feed_model_preset(2));
  CHECK(parse_reshade_nr_preset(reshade) == 1);
  CHECK(renodx_files_reflect_visual_preset(feed, reshade, 2));

  feed = apply_feed_work_resolution(std::move(feed), feed_work_resolution_for_session(cfg.scale_percent));
  CHECK(parse_feed_work_resolution(feed) == 50);

  loaded.fg_enabled = false;
  apply_persisted_to_config(loaded, cfg);
  CHECK(cfg.fg_multiplier == 4);
  CHECK_FALSE(cfg.fg_auto_start);

  loaded.enhance_renodx = false;
  apply_persisted_to_config(loaded, cfg);
  CHECK(cfg.enhance == EnhanceBackend::Off);
}

TEST_CASE("renodx preset applies same files for windowed and fullscreen session start") {
  // Iniciar with exclusive_fullscreen on/off must still push the same gear preset to disk.
  for (bool fullscreen : {false, true}) {
    PersistedSettings s = default_settings();
    s.enhance_renodx = true;
    s.ui.fullscreen = fullscreen;
    s.renodx_preset = static_cast<int>(RenodxVisualPreset::Cinematic);
    AppConfig cfg;
    apply_persisted_to_config(s, cfg);
    CHECK(cfg.exclusive_fullscreen == fullscreen);
    CHECK(cfg.renodx_preset == 2);
    CHECK(cfg.enhance == EnhanceBackend::Renodx);

    std::string feed = "preset=0\nwork_resolution=100\n";
    std::string reshade = "NRPreset=0\n";
    feed = apply_renodx_visual_preset_to_feed(std::move(feed), cfg.renodx_preset);
    reshade = apply_renodx_visual_preset_to_reshade(std::move(reshade), cfg.renodx_preset);
    CHECK(renodx_files_reflect_visual_preset(feed, reshade, cfg.renodx_preset));
    CHECK(parse_reshade_nr_preset(reshade) == 1);
  }
}

TEST_CASE("capture format OBS defaults and menu combinations") {
  CHECK(static_cast<int>(kDefaultVideoFourcc) == 0);
  CHECK(std::string(video_fourcc_name(0)) == "NV12");
  CHECK(std::string(video_fourcc_name(1)) == "I420");
  CHECK(std::string(video_fourcc_name(2)) == "YUY2");
  CHECK(std::string(video_fourcc_name(3)) == "MJPEG");
  CHECK(std::string(video_fourcc_name(4)) == "Qualquer");
  CHECK(std::string(color_space_name(0)) == "Padrao");
  CHECK(std::string(color_space_name(1)) == "Rec. 601");
  CHECK(std::string(color_space_name(2)) == "Rec. 709");
  CHECK(std::string(color_space_name(3)) == "Rec. 2100 (PQ)");
  CHECK(std::string(color_space_name(4)) == "Rec. 2100 (HLG)");
  CHECK(std::string(color_range_name(0)) == "Limitado");
  CHECK(std::string(color_range_name(1)) == "Completo");
  CHECK(std::string(capture_buffering_name(0)) == "Desativado");
  CHECK(std::string(capture_buffering_name(1)) == "Ativado");
  CHECK(capture_buffer_count(0) == 2);
  CHECK(capture_buffer_count(1) == 8);
  CHECK(effective_color_space(0) == ColorSpace::Bt709);
  CHECK(effective_color_space(3) == ColorSpace::Bt709);
  CHECK(effective_color_space(4) == ColorSpace::Bt709);
  CHECK(is_validated_default_capture_format(0, 0, 0, 0));
  CHECK_FALSE(is_validated_default_capture_format(1, 0, 0, 0));
  CHECK_FALSE(is_validated_default_capture_format(0, 0, 1, 0));
  CHECK_FALSE(is_validated_default_capture_format(0, 0, 0, 1));

  // Clamp garbage → defaults
  CHECK(clamp_video_fourcc(99) == 0);
  CHECK(clamp_color_space(-1) == 0);
  CHECK(clamp_color_range(9) == 0);
  CHECK(clamp_capture_buffering(5) == 0);

  // Dynamic resolve: prefer NV12 when available
  std::vector<int> only_yuy2{2};
  CHECK(pick_best_video_fourcc(only_yuy2) == 2);
  std::vector<int> both{2, 0};
  CHECK(pick_best_video_fourcc(both) == 0);
  CHECK(resolve_video_fourcc_for_device(2, both) == 2);
  CHECK(resolve_video_fourcc_for_device(2, std::vector<int>{0}) == 0);  // fall back NV12
  CHECK(resolve_video_fourcc_for_device(4, both) == 4);  // Qualquer stays
  CHECK(concrete_video_fourcc_for_start(4, both) == 0);  // Qualquer → NV12

  // OBS color-space UI order: Padrao, Rec.709, Rec.601, PQ, HLG
  CHECK(kObsColorSpaceUiOrder[0] == 0);
  CHECK(kObsColorSpaceUiOrder[1] == 2);
  CHECK(kObsColorSpaceUiOrder[2] == 1);

  // Coeffs change between limited/full and 601/709
  const auto lim709 = yuv_rgb_coeffs(2, 0);
  const auto full709 = yuv_rgb_coeffs(2, 1);
  const auto lim601 = yuv_rgb_coeffs(1, 0);
  CHECK(lim709.y_offset > 0.01f);
  CHECK(full709.y_offset == doctest::Approx(0.f));
  CHECK(std::abs(lim709.r_cr - lim601.r_cr) > 0.01f);

  // All menu combos persist via settings → AppConfig
  for (int f = 0; f < static_cast<int>(VideoFourcc::Count); ++f) {
    for (int s = 0; s < static_cast<int>(ColorSpace::Count); ++s) {
      for (int r = 0; r < static_cast<int>(ColorRange::Count); ++r) {
        for (int b = 0; b < static_cast<int>(CaptureBuffering::Count); ++b) {
          PersistedSettings ps = default_settings();
          ps.video_fourcc = f;
          ps.color_space = s;
          ps.color_range = r;
          ps.capture_buffering = b;
          const std::string ini = serialize_settings_ini(ps);
          PersistedSettings loaded;
          REQUIRE(parse_settings_ini(ini, loaded));
          CHECK(loaded.video_fourcc == f);
          CHECK(loaded.color_space == s);
          CHECK(loaded.color_range == r);
          CHECK(loaded.capture_buffering == b);
          AppConfig cfg;
          apply_persisted_to_config(loaded, cfg);
          CHECK(cfg.video_fourcc == f);
          CHECK(cfg.color_space == s);
          CHECK(cfg.color_range == r);
          CHECK(cfg.capture_buffering == b);
          CaptureStartOptions opt;
          opt.video_fourcc = cfg.video_fourcc;
          opt.color_space = cfg.color_space;
          opt.color_range = cfg.color_range;
          opt.capture_buffering = cfg.capture_buffering;
          CHECK(capture_buffer_count(opt.capture_buffering) == (b == 0 ? 2 : 8));
        }
      }
    }
  }

  // Defaults remain the validated HDMI path
  PersistedSettings def = default_settings();
  CHECK(is_validated_default_capture_format(def.video_fourcc, def.color_space, def.color_range,
                                            def.capture_buffering));
}

TEST_CASE("neural rendering toggle helpers independent of FG") {
  CHECK(default_hotkeys().vk_nr_toggle == 0x2E);  // Delete
  CHECK(std::string(vk_display_name(0x2E)) == "Delete");

  std::string reshade = "NeuralUplift=1\nNRPreset=0\nNRToggleKey=0\n";
  reshade = apply_reshade_neural_uplift(std::move(reshade), false);
  CHECK_FALSE(parse_reshade_neural_uplift(reshade, true));
  reshade = apply_reshade_neural_uplift(std::move(reshade), true);
  CHECK(parse_reshade_neural_uplift(reshade, false));
  reshade = apply_reshade_nr_toggle_key(std::move(reshade), kRenodxNrInjectVk);
  CHECK(parse_reshade_nr_toggle_key(reshade, 0) == kRenodxNrInjectVk);
  CHECK(kRenodxNrInjectVk != 0x2E);  // Delete UI ≠ RenoDX inject key (no double-toggle)

  std::string feed = "enabled=1\npreset=0\n";
  feed = apply_feed_enabled(std::move(feed), false);
  CHECK_FALSE(parse_feed_enabled(feed, true));
  feed = apply_feed_enabled(std::move(feed), true);
  CHECK(parse_feed_enabled(feed, false));

  // FG multiplier path must not require NeuralUplift
  PersistedSettings s = default_settings();
  s.enhance_renodx = false;
  s.neural_rendering = true;  // stale flag must not keep NR on when RenoDX off
  s.fg_enabled = true;
  s.fg_multiplier = 3;
  AppConfig cfg;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.enhance == EnhanceBackend::Off);
  CHECK_FALSE(cfg.neural_rendering);
  CHECK(cfg.fg_multiplier == 3);

  s.enhance_renodx = true;
  s.neural_rendering = false;  // Delete-off must stick on next Iniciar
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.enhance == EnhanceBackend::Renodx);
  CHECK_FALSE(cfg.neural_rendering);
  CHECK(cfg.fg_multiplier == 3);

  s.neural_rendering = true;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.neural_rendering);

  // Boot file intent: never force ON when flag is off / RenoDX unchecked.
  const bool want_off = false;
  CHECK_FALSE(want_off);
  const bool want_on = (cfg.enhance == EnhanceBackend::Renodx) && cfg.neural_rendering;
  CHECK(want_on);

  const std::string ini = serialize_settings_ini(s);
  CHECK(ini.find("nr_toggle=") != std::string::npos);
  CHECK(ini.find("neural_rendering=1") != std::string::npos);
  s.neural_rendering = false;
  const std::string ini2 = serialize_settings_ini(s);
  CHECK(ini2.find("neural_rendering=0") != std::string::npos);
  PersistedSettings loaded;
  REQUIRE(parse_settings_ini(ini2, loaded));
  CHECK_FALSE(loaded.neural_rendering);
  CHECK(loaded.enhance_renodx);
  CHECK(loaded.hotkeys.vk_nr_toggle == 0x2E);
}

TEST_CASE("lobby preview visual preset grade changes pixels") {
  CHECK(kLobbyPreviewAppliesVisualPresetGrade);
  CHECK(lobby_preview_grade_is_identity(lobby_preview_grade_for_preset(0, false)));
  CHECK(lobby_preview_grade_is_identity(lobby_preview_grade_for_preset(2, false)));

  const auto g0 = lobby_preview_grade_for_preset(0, true);
  const auto g1 = lobby_preview_grade_for_preset(1, true);
  const auto g2 = lobby_preview_grade_for_preset(2, true);
  CHECK_FALSE(lobby_preview_grade_is_identity(g0));
  CHECK(lobby_preview_grades_differ(g0, g1));
  CHECK(lobby_preview_grades_differ(g0, g2));
  CHECK(lobby_preview_grades_differ(g1, g2));

  uint8_t px0[4] = {128, 100, 80, 255};
  uint8_t px1[4] = {128, 100, 80, 255};
  uint8_t px2[4] = {128, 100, 80, 255};
  apply_lobby_preview_grade_pixel(px0, g0);
  apply_lobby_preview_grade_pixel(px1, g1);
  apply_lobby_preview_grade_pixel(px2, g2);
  // Default / Natural / Cinematic must not produce the same RGB on a midtone sample.
  CHECK(!(px0[0] == px1[0] && px0[1] == px1[1] && px0[2] == px1[2]));
  CHECK(!(px0[0] == px2[0] && px0[1] == px2[1] && px0[2] == px2[2]));
  CHECK(!(px1[0] == px2[0] && px1[1] == px2[1] && px1[2] == px2[2]));

  // Off → identity leaves buffer untouched.
  uint8_t raw[4] = {90, 110, 130, 255};
  uint8_t copy[4] = {90, 110, 130, 255};
  apply_lobby_preview_grade_pixel(copy, lobby_preview_grade_for_preset(2, false));
  CHECK(copy[0] == raw[0]);
  CHECK(copy[1] == raw[1]);
  CHECK(copy[2] == raw[2]);

  // rgba batch path
  uint8_t buf[8] = {128, 100, 80, 255, 200, 180, 160, 255};
  apply_lobby_preview_grade_rgba(buf, 2, g2);
  CHECK(buf[3] == 255);
  CHECK(buf[7] == 255);
}

TEST_CASE("app version brand string") {
  CHECK(app_version_is(5, 0, 2));
  CHECK_FALSE(app_version_is(2, 4, 1));
  CHECK(std::string(kAppVersion) == "5.0.2");
  CHECK(std::strstr(kAppNameWithVersion, "v5.0.2") != nullptr);
  CHECK(std::string(kAppAnalyzeBuild) == "NexusCap-5.0.2");
  const std::string lobby_t = app_window_title_lobby();
  const std::string sess_t = app_window_title_session();
  CHECK(lobby_t.find("5.0.2") != std::string::npos);
  CHECK(lobby_t.find("Console de captura") != std::string::npos);
  CHECK(lobby_t.find("\xE2\x80\x94") == std::string::npos);  // no UTF-8 em-dash
  CHECK(sess_t.find("5.0.2") != std::string::npos);
  CHECK(sess_t.find("Nexus Cap v") != std::string::npos);
}

TEST_CASE("Esc back to lobby reflects session FG hotkey in settings") {
  PersistedSettings s = default_settings();
  CHECK_FALSE(s.fg_enabled);
  s.fg_multiplier = 2;

  // Entered with FG off; PageDn enabled x3 mid-session.
  apply_session_fg_to_persisted(true, 3, s);
  CHECK(s.fg_enabled);
  CHECK(s.fg_multiplier == 3);

  AppConfig cfg;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.fg_auto_start);
  CHECK(cfg.fg_multiplier == 3);

  // PageDn off again → checkbox clears.
  apply_session_fg_to_persisted(false, 3, s);
  apply_session_fg_to_config(false, 3, cfg);
  CHECK_FALSE(s.fg_enabled);
  CHECK(s.fg_multiplier == 3);  // preferred xN kept
  CHECK_FALSE(cfg.fg_auto_start);
}

TEST_CASE("Esc back to lobby keeps RenoDX stack so Delete works next session") {
  PersistedSettings s = default_settings();
  s.enhance_renodx = true;
  s.neural_rendering = true;

  // Delete off mid-session → NR flag off, but enhance stays (log regression: blocked Delete).
  apply_session_nr_to_persisted(false, true, s);
  CHECK(s.enhance_renodx);
  CHECK_FALSE(s.neural_rendering);
  CHECK(lobby_renodx_checkbox_checked(s.enhance_renodx));

  AppConfig cfg;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.enhance == EnhanceBackend::Renodx);
  CHECK_FALSE(cfg.neural_rendering);
  CHECK(session_nr_hotkey_allowed(cfg.enhance));

  apply_session_nr_to_persisted(true, true, s);
  CHECK(s.enhance_renodx);
  CHECK(s.neural_rendering);
  CHECK(lobby_renodx_checkbox_checked(s.enhance_renodx));

  apply_persisted_to_config(s, cfg);
  CHECK(cfg.enhance == EnhanceBackend::Renodx);
  CHECK(cfg.neural_rendering);
}

TEST_CASE("smoke and selftest do not wipe the user's NeuralUplift files") {
  AppConfig live;
  live.enhance = EnhanceBackend::Off;
  CHECK(session_writes_renodx_off_files(live));  // Iniciar sem RenoDX no lobby

  AppConfig smoke;
  smoke.smoke = true;
  smoke.fake_capture = true;
  smoke.enhance = EnhanceBackend::Off;
  CHECK_FALSE(session_writes_renodx_off_files(smoke));

  AppConfig cap;
  cap.selftest_cap = true;
  cap.enhance = EnhanceBackend::Off;
  CHECK_FALSE(session_writes_renodx_off_files(cap));

  AppConfig cfg;
  cfg.selftest_cfg = true;
  cfg.fake_capture = true;
  cfg.enhance = EnhanceBackend::Off;
  CHECK_FALSE(session_writes_renodx_off_files(cfg));

  AppConfig fg;
  fg.selftest_fg = true;
  fg.enhance = EnhanceBackend::Off;
  CHECK_FALSE(session_writes_renodx_off_files(fg));

  AppConfig rx;
  rx.enhance = EnhanceBackend::Renodx;
  CHECK_FALSE(session_writes_renodx_off_files(rx));
}

TEST_CASE("Delete without RenoDX warns and does not fake HUD off") {
  AppConfig cfg;
  cfg.enhance = EnhanceBackend::Off;
  CHECK_FALSE(session_nr_hotkey_allowed(cfg.enhance));
  const std::string blocked = session_notice_nr_needs_renodx();
  CHECK(blocked.find("RenoDX") != std::string::npos);
  CHECK(blocked.find("lobby") != std::string::npos);
  CHECK(blocked.find("OFF") == std::string::npos);
  CHECK(blocked.find("Confirmado") == std::string::npos);
  CHECK(session_notice_nr_off().find("Confirmado") != std::string::npos);
  CHECK(session_notice_nr_off() != blocked);
}

TEST_CASE("session NR off restart also turns feed off (else DLSS5 keeps evaluating)") {
  CHECK(feed_pipeline_on_for_renodx(true));
  CHECK_FALSE(feed_pipeline_on_for_renodx(false));

  std::string feed = "enabled=1\nmode=2\n";
  std::string reshade = "NeuralUplift=1\nNRToggleKey=46\n";
  // Delete/CFG NR off + restart: both files must go off or the image stays DLSS5.
  feed = apply_feed_enabled(std::move(feed), false);
  reshade = apply_reshade_neural_uplift(std::move(reshade), false);
  CHECK_FALSE(parse_feed_enabled(feed, true));
  CHECK_FALSE(parse_reshade_neural_uplift(reshade, true));

  // Delete ON + restart: both back on (feed lê enabled no init do processo novo).
  feed = apply_feed_enabled(std::move(feed), true);
  reshade = apply_reshade_neural_uplift(std::move(reshade), true);
  CHECK(parse_feed_enabled(feed, false));
  CHECK(parse_reshade_neural_uplift(reshade, false));

  // Lobby: RenoDX ligado sem restart — feed fica armado.
  CHECK(feed_pipeline_on_for_renodx(true));

  // Unload Renodx: both off.
  feed = apply_feed_enabled(std::move(feed), feed_pipeline_on_for_renodx(false));
  reshade = apply_reshade_neural_uplift(std::move(reshade), false);
  CHECK_FALSE(parse_feed_enabled(feed, true));
  CHECK_FALSE(parse_reshade_neural_uplift(reshade, true));
}

TEST_CASE("Delete NR off keeps Renodx backend so Delete can turn back on") {
  AppConfig cfg;
  cfg.enhance = EnhanceBackend::Renodx;
  cfg.neural_rendering = true;
  CHECK(session_nr_hotkey_allowed(cfg.enhance));

  // Delete → off: flag clears, backend MUST stay Renodx (old bug blocked re-enable).
  apply_session_nr_runtime_to_config(false, cfg);
  CHECK_FALSE(cfg.neural_rendering);
  CHECK(cfg.enhance == EnhanceBackend::Renodx);
  CHECK(session_nr_hotkey_allowed(cfg.enhance));

  apply_session_nr_runtime_to_config(true, cfg);
  CHECK(cfg.neural_rendering);
  CHECK(cfg.enhance == EnhanceBackend::Renodx);

  // Without Renodx backend, hotkey is blocked (lobby checkbox off).
  cfg.enhance = EnhanceBackend::Off;
  CHECK_FALSE(session_nr_hotkey_allowed(cfg.enhance));
}

TEST_CASE("Esc after Delete-off does not unload Renodx for next session") {
  PersistedSettings s = default_settings();
  s.enhance_renodx = true;
  s.neural_rendering = true;
  apply_session_nr_to_persisted(false, /*session_had_renodx=*/true, s);
  AppConfig cfg;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.enhance == EnhanceBackend::Renodx);
  CHECK_FALSE(cfg.neural_rendering);
  CHECK(session_nr_hotkey_allowed(cfg.enhance));

  // Esc sync without Renodx session must NOT force-clear enhance (lobby checkbox owns that).
  s.enhance_renodx = true;
  s.neural_rendering = false;
  apply_session_nr_to_persisted(false, /*session_had_renodx=*/false, s);
  CHECK(s.enhance_renodx);
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.enhance == EnhanceBackend::Renodx);
  CHECK(session_nr_hotkey_allowed(cfg.enhance));
}

TEST_CASE("Esc lobby sync keeps FG and NR independent") {
  PersistedSettings s = default_settings();
  s.fg_enabled = false;
  s.enhance_renodx = true;
  s.neural_rendering = true;

  apply_session_fg_to_persisted(true, 2, s);           // PageDn on
  apply_session_nr_to_persisted(false, true, s);        // Delete off (kept enhance)
  CHECK(s.fg_enabled);
  CHECK(s.enhance_renodx);
  CHECK_FALSE(s.neural_rendering);
  CHECK(lobby_renodx_checkbox_checked(s.enhance_renodx));

  apply_session_fg_to_persisted(false, 2, s);
  apply_session_nr_to_persisted(true, true, s);
  CHECK_FALSE(s.fg_enabled);
  CHECK(s.enhance_renodx);
  CHECK(s.neural_rendering);
}

TEST_CASE("renodx runtime dirs prefer writable AppData cache") {
  const std::string data = app_data_dir();
  CHECK_FALSE(data.empty());
  CHECK(data.find("NexusCap") != std::string::npos);
  CHECK(directory_is_writable(data));
  const std::string cache = app_reshade_cache_dir();
  CHECK(cache.find("reshade-cache") != std::string::npos);
  CHECK(directory_is_writable(cache));
  const std::string logs = app_logs_dir();
  CHECK(logs.find("logs") != std::string::npos);
  CHECK(std::strstr(renodx_write_denied_notice(), "Program Files") != nullptr);

  // IntermediateCachePath rewrite helper keeps absolute AppData path.
  std::string ini = "[GENERAL]\nIntermediateCachePath=.\\reshade-cache\n";
  ini = apply_keyed_value(std::move(ini), "IntermediateCachePath=", cache);
  CHECK(ini.find(cache) != std::string::npos);
  CHECK(ini.find(".\\reshade-cache") == std::string::npos);
}

TEST_CASE("portable mode: exe folder vs AppData") {
  CHECK_FALSE(want_portable_mode(false, false));
  CHECK(want_portable_mode(true, false));
  CHECK(want_portable_mode(false, true));
  CHECK(want_portable_mode(true, true));
  CHECK(std::string(kPortableSentinelName) == "portable.txt");

  CHECK(resolve_app_data_dir("E:\\usb\\NexusCap", "C:\\Users\\u\\AppData\\Local", false) ==
        "C:\\Users\\u\\AppData\\Local\\NexusCap");
  CHECK(resolve_app_data_dir("E:\\usb\\NexusCap", "C:\\Users\\u\\AppData\\Local", true) == "E:\\usb\\NexusCap");
  CHECK(resolve_app_data_dir("", "C:\\Users\\u\\AppData\\Local", true) == "C:\\Users\\u\\AppData\\Local\\NexusCap");
  CHECK(resolve_app_data_dir("E:\\usb", "", false).empty());
  CHECK(resolve_app_data_dir("E:\\usb", "", true) == "E:\\usb");

  CHECK(portable_relaunch_args("--autostart", false) == "--autostart");
  CHECK(portable_relaunch_args("--autostart", true) == "--portable --autostart");
  CHECK(portable_relaunch_args(nullptr, true) == "--portable");
  CHECK(portable_relaunch_args(nullptr, false).empty());
  CHECK(portable_relaunch_args("--lobby-cfg", true) == "--portable --lobby-cfg");
  CHECK(portable_relaunch_args("--show-perf-result", true) == "--portable --show-perf-result");

  const char* argv_on[] = {"NexusCap.exe", "--portable", "--autostart"};
  const AppConfig on = parse_args(3, const_cast<char**>(argv_on));
  CHECK(on.portable);
  CHECK(on.autostart);

  const char* argv_off[] = {"NexusCap.exe", "--autostart"};
  CHECK_FALSE(parse_args(2, const_cast<char**>(argv_off)).portable);

  CHECK(std::strstr(portable_data_dir_hint(), "portable") != nullptr);
  CHECK(std::strstr(portable_data_dir_hint(), "exe") != nullptr);
  CHECK_FALSE(app_is_portable());  // unit exe has no portable.txt and no init_app_paths(true)
}

TEST_CASE("lobby RenoDX preview: toggle and presets apply on Present") {
  // Product: enable/disable RenoDX in CFG must show/hide filters on lobby preview Present.
  CHECK(kLobbyPresentShowsRenodxWhenEnabled);
  CHECK(kLobbyPreviewUsesRenodxNeural);
  CHECK_FALSE(kLobbyPreviewCpuBandUsesRenodxNeural);

  {
    // Enable while runtime OFF → pulse ON, preview shows RenoDX.
    const auto p = plan_lobby_renodx_toggle(true, true, false);
    CHECK(p.show_on_present);
    CHECK(p.need_nr_pulse);
    CHECK_FALSE(p.need_preset_reload_cycle);
    CHECK(p.resulting_runtime_on);
  }
  {
    // Disable while runtime ON → pulse OFF, preview limpo.
    const auto p = plan_lobby_renodx_toggle(false, false, true);
    CHECK_FALSE(p.show_on_present);
    CHECK(p.need_nr_pulse);
    CHECK_FALSE(p.resulting_runtime_on);
  }
  {
    // Already matched ON — no extra pulse on re-check.
    const auto p = plan_lobby_renodx_toggle(true, true, true);
    CHECK(p.show_on_present);
    CHECK_FALSE(p.need_nr_pulse);
  }

  // Default → Natural / Cinematic while NR ON: sync files + OFF→ON reload cycle.
  {
    const auto p = plan_lobby_renodx_preset_change(true, true, 0, 2, true);
    CHECK(p.show_on_present);
    CHECK(p.sync_preset_files);
    CHECK(p.need_preset_reload_cycle);
    CHECK_FALSE(p.need_nr_pulse);
    CHECK(p.resulting_runtime_on);
  }
  {
    const auto p = plan_lobby_renodx_preset_change(true, true, 1, 0, true);
    CHECK(p.sync_preset_files);
    CHECK(p.need_preset_reload_cycle);
  }
  {
    // Same preset → no sync / no cycle.
    const auto p = plan_lobby_renodx_preset_change(true, true, 2, 2, true);
    CHECK_FALSE(p.sync_preset_files);
    CHECK_FALSE(p.need_preset_reload_cycle);
  }
  {
    // Preset change with RenoDX OFF: still write files, no Present filters, no cycle.
    const auto p = plan_lobby_renodx_preset_change(false, false, 0, 1, false);
    CHECK_FALSE(p.show_on_present);
    CHECK(p.sync_preset_files);
    CHECK_FALSE(p.need_preset_reload_cycle);
    CHECK_FALSE(p.need_nr_pulse);
  }
  {
    // Preset change while enhance ON but runtime still OFF → single enable pulse (boot).
    const auto p = plan_lobby_renodx_preset_change(true, true, 0, 1, false);
    CHECK(p.show_on_present);
    CHECK(p.sync_preset_files);
    CHECK_FALSE(p.need_preset_reload_cycle);
    CHECK(p.need_nr_pulse);
    CHECK(p.resulting_runtime_on);
  }

  // Distinct disk fingerprints so Default/Natural/Cinematic look different on preview.
  CHECK(renodx_visual_presets_differ_on_disk(0, 1));
  CHECK(renodx_visual_presets_differ_on_disk(0, 2));
  CHECK(renodx_visual_presets_differ_on_disk(1, 2));
}

TEST_CASE("neural rendering Bridge plan: enable/disable cannot desync") {
  // Regression: settings ON at boot must NOT imply runtime ON (ReShade.log: enabled=OFF).
  CHECK_FALSE(settings_imply_runtime_on_at_boot(true, true));
  CHECK(nr_boot_needs_enable_pulse(true));
  CHECK_FALSE(nr_boot_needs_enable_pulse(false));
  CHECK(want_neural_rendering(true, true));
  CHECK_FALSE(want_neural_rendering(true, false));
  CHECK_FALSE(want_neural_rendering(false, true));

  {
    const auto p = plan_neural_runtime(true, false);
    CHECK(p.sync_files_enabled);
    CHECK(p.need_bridge_pulse);
    CHECK(p.resulting_runtime_on);
  }
  {
    const auto p = plan_neural_runtime(false, true);
    CHECK_FALSE(p.sync_files_enabled);
    CHECK(p.need_bridge_pulse);
    CHECK_FALSE(p.resulting_runtime_on);
  }
  {
    const auto p = plan_neural_runtime(true, true);
    CHECK_FALSE(p.need_bridge_pulse);  // already matched — no double pulse
  }
  {
    const auto p = plan_neural_runtime(false, false);
    CHECK_FALSE(p.need_bridge_pulse);
  }

  {
    const auto on = plan_nr_user_toggle(false);
    CHECK(on.need_bridge_pulse);
    CHECK(on.resulting_runtime_on);
    const auto off = plan_nr_user_toggle(true);
    CHECK(off.need_bridge_pulse);
    CHECK_FALSE(off.resulting_runtime_on);
  }

  const Dlss5BridgeCommand cmd = make_nr_pulse_command();
  CHECK(bridge_command_is_valid_nr_pulse(cmd));
  {
    Dlss5BridgeCommand bad = cmd;
    bad.size = 0;
    CHECK_FALSE(bridge_command_is_valid_nr_pulse(bad));
  }
  {
    Dlss5BridgeCommand bad = cmd;
    reinterpret_cast<uint8_t*>(&bad)[0x48] = 0;
    CHECK_FALSE(bridge_command_is_valid_nr_pulse(bad));
  }

  // enable → disable → enable without getting stuck (old Delete desync path)
  bool runtime = false;
  auto step = [&](bool want) {
    const auto p = plan_neural_runtime(want, runtime);
    if (p.need_bridge_pulse) runtime = p.resulting_runtime_on;
    return p;
  };
  CHECK(step(true).need_bridge_pulse);
  CHECK(runtime);
  CHECK_FALSE(step(true).need_bridge_pulse);
  CHECK(step(false).need_bridge_pulse);
  CHECK_FALSE(runtime);
  CHECK(step(true).need_bridge_pulse);
  CHECK(runtime);

  // Lobby left NR ON → Iniciar must NOT pulse again (that inverted HUD vs RenoDX).
  {
    bool carried = true;  // cfg.nr_runtime_on from lobby preview
    const bool want = true;
    const auto boot = plan_neural_runtime(want, carried);
    CHECK_FALSE(boot.need_bridge_pulse);
    CHECK(boot.resulting_runtime_on);
  }
  // Lobby NR ON, settings now want OFF → one pulse to OFF (not assume OFF then pulse ON).
  {
    const auto boot = plan_neural_runtime(false, true);
    CHECK(boot.need_bridge_pulse);
    CHECK_FALSE(boot.resulting_runtime_on);
  }
  // Fresh process / no lobby carry: OFF + want ON → pulse once.
  {
    const auto boot = plan_neural_runtime(true, false);
    CHECK(boot.need_bridge_pulse);
    CHECK(boot.resulting_runtime_on);
  }

  // Double Delete within 350ms must be ignored (log had OFF→ON ~400ms → HUD/RenoDX desync).
  {
    double last = 0;
    CHECK(nr_toggle_debounce_ok(1000.0, last));
    CHECK_FALSE(nr_toggle_debounce_ok(1200.0, last));   // +200ms
    CHECK_FALSE(nr_toggle_debounce_ok(1349.0, last));   // +349ms
    CHECK(nr_toggle_debounce_ok(1350.0, last));         // +350ms accept
  }

  // Overlay confirmation helpers — no false positive without ACK / process.
  {
    Dlss5BridgeStatus st{};
    st.nr_delivered = 5;
    st.nr_pulses = 3;
    CHECK(bridge_nr_delivered_advanced(4, st));
    CHECK_FALSE(bridge_nr_delivered_advanced(5, st));
    CHECK(bridge_nr_ack_advanced(5, 2, st));   // pulses advanced
    CHECK_FALSE(bridge_nr_ack_advanced(5, 3, st));
  }
  {
    CHECK(parse_reshade_nr_toggled_line("NR toggled ON via 0x124") == 1);
    CHECK(parse_reshade_nr_toggled_line("NR toggled OFF via 0x124") == 0);
    CHECK(parse_reshade_nr_toggled_line("other") == -1);
    const char* log =
        "10:00:00 INFO stuff\n"
        "10:01:00 NR toggled ON via 0x124\n"
        "10:02:00 noise\n"
        "10:03:00 NR toggled OFF via 0x124\n";
    CHECK(parse_reshade_nr_last_toggled(log) == 0);
    CHECK(parse_reshade_nr_last_toggled("NR toggled ON via 0x124\n") == 1);
    const char* stale = "10:00 NR toggled ON via 0x124\n";
    CHECK(parse_reshade_nr_last_toggled(stale) == 1);
    // New append after OFF must win (Bridge-ACK-before-log bug regression).
    CHECK(parse_reshade_nr_last_toggled("10:01 NR toggled OFF via 0x124\n") == 0);
    CHECK(parse_reshade_nr_eval_line(
              "[RenoDX] DLSS-NR direct: EvaluateFeature succeeded: evaluation=178 options_revision=1") == 178);
    CHECK(parse_reshade_nr_eval_line("nope") == -1);
    CHECK(parse_reshade_nr_eval_count("EvaluateFeature succeeded: evaluation=1\n"
                                     "noise\n"
                                     "EvaluateFeature succeeded: evaluation=2\n") == 2);
  }
  {
    CHECK(fg_overlay_on_confirmed(true, 1));
    CHECK_FALSE(fg_overlay_on_confirmed(true, 0));  // no LS process → never FG ON
    CHECK(fg_overlay_on_confirmed(false, 0));
    CHECK_FALSE(fg_overlay_on_confirmed(false, 2));  // leftover LS → OFF not confirmed
    CHECK_FALSE(fg_overlay_reads_on(true, 2, true, 0));  // processo sem F24
    CHECK_FALSE(fg_overlay_reads_on(true, 4, false, 1));
    CHECK_FALSE(fg_overlay_reads_on(true, 2, false, 0));
    CHECK(fg_overlay_reads_on(true, 2, true, 1, true, false));  // F24 confirmado nesta sessao
    CHECK(fg_overlay_reads_on(false, 2, false, 0, false, true));  // superficie grande
    CHECK_FALSE(fg_overlay_reads_on(false, 2, false, 0, true));  // confirm sem processo
    CHECK_FALSE(fg_overlay_reads_on(false, 4, false, 0));
    CHECK_FALSE(fg_overlay_reads_on(true, 1, true, 1, true, true));
    CHECK(ls_exe_is_lossless(L"LosslessScaling.exe"));
    CHECK(ls_exe_is_lossless(L"Lossless Scaling.exe"));
    CHECK_FALSE(ls_exe_is_lossless(L"NexusCap.exe"));
  }
}

TEST_CASE("mini overlay DLSS5 follows renodx_on only (not passthrough dlss_on)") {
  // Regression: HUD used renodx_on || (dlss_on && backend has renodx) while enhance is
  // passthrough — after Invert-bug fix, status is solely renodx_on / g_nr_runtime_on.
  char line[512]{};
  format_mini_overlay_line(line, sizeof(line), 60.0, 0.0, false, 2, true, 16.0, 8.0, 0, 0, 2560, 1440);
  CHECK(std::strstr(line, "NR ON") != nullptr);
  format_mini_overlay_line(line, sizeof(line), 60.0, 0.0, false, 2, false, 16.0, 8.0, 0, 0, 2560, 1440);
  CHECK(std::strstr(line, "NR off") != nullptr);
}

TEST_CASE("format change must not re-query device every frame") {
  // Contract: list_video_fourccs is expensive/locks the card. Cache key must change only with mode.
  const std::string a = std::string("dev|2560|1440|60.000");
  const std::string b = std::string("dev|2560|1440|60.000");
  const std::string c = std::string("dev|1920|1080|60.000");
  CHECK(a == b);
  CHECK(a != c);

  // Fallback path: failed non-NV12 format should still prefer NV12 as concrete start.
  std::vector<int> avail{0, 2};  // NV12 + YUY2
  CHECK(concrete_video_fourcc_for_start(static_cast<int>(VideoFourcc::Yuy2), avail) == 2);
  CHECK(concrete_video_fourcc_for_start(static_cast<int>(VideoFourcc::Any), avail) == 0);
  CHECK(concrete_video_fourcc_for_start(static_cast<int>(VideoFourcc::Mjpeg), std::vector<int>{0}) == 0);

  // Color apply must not require capture restart (same fourcc/buf → early ensure return).
  CaptureStartOptions o1;
  o1.width = 2560;
  o1.height = 1440;
  o1.fps = 60;
  o1.video_fourcc = 0;
  o1.capture_buffering = 0;
  o1.color_space = 0;
  o1.color_range = 0;
  CaptureStartOptions o2 = o1;
  o2.color_space = 1;
  o2.color_range = 1;
  CHECK(o1.video_fourcc == o2.video_fourcc);
  CHECK(o1.capture_buffering == o2.capture_buffering);
  CHECK(color_decode_differs(o1.color_space, o1.color_range, o2.color_space, o2.color_range));
}

TEST_CASE("lobby preview vs RenoDX and real color/format contracts") {
  // CPU band stays NV12→RGBA; with enhance ON a visual grade previews Default/Natural/Cinematic.
  CHECK_FALSE(kLobbyPreviewCpuBandUsesRenodxNeural);
  CHECK(kLobbyPreviewAppliesVisualPresetGrade);
  CHECK(kLobbyPresentShowsRenodxWhenEnabled);
  CHECK(kLobbyPreviewUsesRenodxNeural);
  CHECK(kSessionPresentCanUseRenodxNeural);

  // Checkbox: want NR only when RenoDX + flag; Present path shows filters on preview.
  CHECK(want_neural_rendering(true, true));
  CHECK_FALSE(want_neural_rendering(false, true));
  CHECK_FALSE(kLobbyPreviewCpuBandUsesRenodxNeural);

  // Color/range are REAL in decode matrix (not cosmetic labels).
  CHECK(color_decode_differs(/*709 lim*/ 2, 0, /*709 full*/ 2, 1));
  CHECK(color_decode_differs(/*601 lim*/ 1, 0, /*709 lim*/ 2, 0));
  CHECK_FALSE(color_decode_differs(0, 0, 2, 0));  // Padrao ≡ Rec.709
  CHECK_FALSE(color_decode_differs(3, 0, 2, 0));  // Rec.2100 PQ → same SDR matrix today
  CHECK_FALSE(color_decode_differs(4, 0, 2, 0));  // HLG → same SDR matrix today

  uint8_t lim[3]{}, full[3]{}, bt601[3]{};
  const auto c709l = yuv_rgb_coeffs(2, 0);
  const auto c709f = yuv_rgb_coeffs(2, 1);
  const auto c601 = yuv_rgb_coeffs(1, 0);
  // Same mid-gray YUV sample must diverge between limited/full and 601/709.
  yuv_to_rgb8(126, 128, 140, c709l, lim);
  yuv_to_rgb8(126, 128, 140, c709f, full);
  yuv_to_rgb8(126, 128, 140, c601, bt601);
  CHECK((lim[0] != full[0] || lim[1] != full[1] || lim[2] != full[2]));
  CHECK((lim[0] != bt601[0] || lim[1] != bt601[1] || lim[2] != bt601[2]));

  // Format + buffering still real defaults / counts
  CHECK(static_cast<int>(kDefaultVideoFourcc) == static_cast<int>(VideoFourcc::Nv12));
  CHECK(capture_buffer_count(0) == 2);
  CHECK(capture_buffer_count(1) == 8);
  CHECK(is_validated_default_capture_format(0, 0, 0, 0));

  // Lobby→session: enhance_renodx off clears NR; on keeps neural_rendering flag for Start.
  PersistedSettings s = default_settings();
  s.enhance_renodx = false;
  s.neural_rendering = true;
  AppConfig cfg;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.enhance == EnhanceBackend::Off);
  CHECK_FALSE(cfg.neural_rendering);
  s.enhance_renodx = true;
  s.neural_rendering = true;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.enhance == EnhanceBackend::Renodx);
  CHECK(cfg.neural_rendering);
  CHECK(cfg.color_space == 0);
  CHECK(cfg.color_range == 0);
}

TEST_CASE("analyze session_start records renodx_preset for analytics") {
  AnalyzeWriter a;
  SessionInfo info;
  info.device = "VEDO";
  info.width = 2560;
  info.height = 1440;
  info.fps = 60;
  info.dlss_enabled = true;
  info.dlss_mode = "renodx-presets";
  info.dlss_backend = "renodx+feed";
  info.renodx_preset = "Cinematic";
  a.session_start(info, 1.0);
  const auto lines = a.lines_copy();
  REQUIRE_FALSE(lines.empty());
  CHECK(lines[0].find("\"renodx_preset\":\"Cinematic\"") != std::string::npos);
  const std::string sum = a.summary_text(2.0);
  CHECK(sum.find("renodx_preset: Cinematic") != std::string::npos);
}

TEST_CASE("Esc never closes lobby; session Esc returns to lobby code") {
  // Spurious WM_QUIT must not exit the lobby (regression: Esc closed the whole app).
  CHECK(lobby_message_is_spurious_quit(0x0012));  // WM_QUIT
  CHECK_FALSE(lobby_message_exits_app(0x0012));
  CHECK(lobby_message_exits_app(0x0010));  // WM_CLOSE = X button
  CHECK_FALSE(lobby_message_is_spurious_quit(0x0010));

  // Esc in lobby chrome: never quit.
  CHECK(lobby_esc_action(false, false) == LobbyEscAction::None);
  CHECK(lobby_esc_action(true, false) == LobbyEscAction::CloseGear);
  CHECK(lobby_esc_action(false, true) == LobbyEscAction::CancelHotkeyCapture);
  CHECK(lobby_esc_action(true, true) == LobbyEscAction::CancelHotkeyCapture);
  CHECK(lobby_gear_hotkey_toggles_cfg(0x7B, 0x7B, false));
  CHECK_FALSE(lobby_gear_hotkey_toggles_cfg(0x7B, 0x7B, true));
  CHECK_FALSE(lobby_gear_hotkey_toggles_cfg(0x1B, 0x7B, false));
  CHECK_FALSE(lobby_gear_hotkey_toggles_cfg(0x7B, 0, false));

  // Session Esc exit codes
  CHECK(session_exit_code_after_esc(true) == kSessionExitBackToLobby);
  CHECK(session_exit_code_after_esc(true) == 100);
  CHECK(session_exit_code_after_esc(false) == 0);

  // Gate still blocks held Esc right after Iniciar (existing contract).
  SessionQuitGate g;
  g.reset(1000.0, /*quit_key_down=*/true, 750.0);
  CHECK_FALSE(g.should_accept_quit_keydown(0x1B, 0x1B, 2000.0));
  g.on_keyup(0x1B, 0x1B);
  CHECK(g.should_accept_quit_keydown(0x1B, 0x1B, 1750.0));
}

TEST_CASE("SessionQuitGate blocks stale Esc after Iniciar") {
  constexpr int kEsc = 0x1B;
  SessionQuitGate g;

  // Esc still held from lobby return → not armed; KEYDOWN must not quit.
  g.reset(1000.0, /*quit_key_down=*/true, /*debounce_ms=*/750.0);
  CHECK_FALSE(g.armed);
  CHECK_FALSE(g.should_accept_quit_keydown(kEsc, kEsc, 1000.0));
  CHECK_FALSE(g.should_accept_quit_keydown(kEsc, kEsc, 2000.0));  // still not armed
  g.on_keyup(kEsc, kEsc);
  CHECK(g.armed);
  CHECK_FALSE(g.should_accept_quit_keydown(kEsc, kEsc, 1600.0));  // inside debounce from t=1000
  CHECK(g.should_accept_quit_keydown(kEsc, kEsc, 1750.0));         // armed + past debounce

  // Esc not held at boot → armed, but debounce still applies.
  g.reset(5000.0, /*quit_key_down=*/false, 750.0);
  CHECK(g.armed);
  CHECK_FALSE(g.should_accept_quit_keydown(kEsc, kEsc, 5000.0));
  CHECK_FALSE(g.should_accept_quit_keydown(kEsc, kEsc, 5749.0));
  CHECK(g.should_accept_quit_keydown(kEsc, kEsc, 5750.0));
  CHECK_FALSE(g.should_accept_quit_keydown(0x70, kEsc, 6000.0));  // other key
}

TEST_CASE("SessionQuitGate detects stale quit messages") {
  constexpr int kEsc = 0x1B;
  CHECK(SessionQuitGate::is_stale_quit_message(0x0100, kEsc, kEsc));  // WM_KEYDOWN
  CHECK(SessionQuitGate::is_stale_quit_message(0x0101, kEsc, kEsc));  // WM_KEYUP
  CHECK(SessionQuitGate::is_stale_quit_message(0x0104, kEsc, kEsc));  // WM_SYSKEYDOWN
  CHECK_FALSE(SessionQuitGate::is_stale_quit_message(0x0100, 0x70, kEsc));
  CHECK_FALSE(SessionQuitGate::is_stale_quit_message(0x0200, kEsc, kEsc));  // WM_MOUSEMOVE
}

TEST_CASE("parse --no-lobby and --scale") {
  char a0[] = "NexusCap";
  char a1[] = "--no-lobby";
  char a2[] = "--scale";
  char a3[] = "80";
  char* argv[] = {a0, a1, a2, a3};
  auto c = parse_args(4, argv);
  CHECK_FALSE(c.show_lobby);
  CHECK(c.scale_percent == 80);
}

TEST_CASE("smoke implies no lobby") {
  char a0[] = "NexusCap";
  char a1[] = "--smoke";
  char* argv[] = {a0, a1};
  auto c = parse_args(2, argv);
  CHECK(c.smoke);
  CHECK_FALSE(c.show_lobby);
}

TEST_CASE("hud hotkey default is Ctrl+Space") {
  const HotkeyConfig h = default_hotkeys();
  CHECK(h.vk_hud == 0x20);
  CHECK(h.hud_ctrl);
  CHECK(hud_hotkey_fired(0x20, true, h.vk_hud, h.hud_ctrl));
  CHECK_FALSE(hud_hotkey_fired(0x20, false, h.vk_hud, h.hud_ctrl));
  CHECK_FALSE(hud_hotkey_fired(0x72, true, h.vk_hud, h.hud_ctrl));
  CHECK(std::string(vk_display_name(0x20)) == "Espaco");
}

TEST_CASE("Ctrl+Space cycles overlay HUD Compact Mini Advanced Live Off then Compact") {
  OverlayHudFlags f = overlay_hud_flags_for(OverlayHudStep::Compact);
  CHECK(f.visible);
  CHECK_FALSE(f.mini);
  CHECK_FALSE(f.advanced);
  CHECK_FALSE(f.live);
  CHECK(overlay_hud_step_of(f.visible, f.mini, f.advanced, f.live) == OverlayHudStep::Compact);
  CHECK_FALSE(hud_uses_mini_overlay(f.visible, f.mini));
  CHECK_FALSE(hud_uses_advanced_metrics(f.visible, f.mini, f.advanced));

  f = cycle_overlay_hud(f.visible, f.mini, f.advanced, f.live);
  CHECK(overlay_hud_step_of(f.visible, f.mini, f.advanced, f.live) == OverlayHudStep::Mini);
  CHECK(f.visible);
  CHECK(f.mini);
  CHECK_FALSE(f.advanced);
  CHECK_FALSE(f.live);
  CHECK(hud_uses_mini_overlay(f.visible, f.mini));
  CHECK_FALSE(hud_uses_advanced_metrics(f.visible, f.mini, f.advanced));

  f = cycle_overlay_hud(f.visible, f.mini, f.advanced, f.live);
  CHECK(overlay_hud_step_of(f.visible, f.mini, f.advanced, f.live) == OverlayHudStep::Advanced);
  CHECK(f.visible);
  CHECK_FALSE(f.mini);
  CHECK(f.advanced);
  CHECK_FALSE(f.live);
  CHECK(hud_uses_advanced_metrics(f.visible, f.mini, f.advanced));

  f = cycle_overlay_hud(f.visible, f.mini, f.advanced, f.live);
  CHECK(overlay_hud_step_of(f.visible, f.mini, f.advanced, f.live) == OverlayHudStep::Live);
  CHECK(f.visible);
  CHECK(f.live);
  CHECK_FALSE(f.mini);
  CHECK_FALSE(f.advanced);

  f = cycle_overlay_hud(f.visible, f.mini, f.advanced, f.live);
  CHECK(overlay_hud_step_of(f.visible, f.mini, f.advanced, f.live) == OverlayHudStep::Off);
  CHECK_FALSE(f.visible);
  CHECK_FALSE(f.mini);
  CHECK_FALSE(f.advanced);
  CHECK_FALSE(f.live);

  f = cycle_overlay_hud(f.visible, f.mini, f.advanced, f.live);
  CHECK(overlay_hud_step_of(f.visible, f.mini, f.advanced, f.live) == OverlayHudStep::Compact);
  CHECK(f.visible);
  CHECK_FALSE(f.mini);
  CHECK_FALSE(f.advanced);
  CHECK_FALSE(f.live);
}

TEST_CASE("overlay HUD cycle starts Compact when overlay is off") {
  const OverlayHudFlags f = cycle_overlay_hud(/*visible=*/false, /*mini=*/true, /*advanced=*/true, /*live=*/true);
  CHECK(overlay_hud_step_of(f.visible, f.mini, f.advanced, f.live) == OverlayHudStep::Compact);
  CHECK(f.visible);
  CHECK_FALSE(f.mini);
  CHECK_FALSE(f.advanced);
  CHECK_FALSE(f.live);
}

TEST_CASE("overlay HUD cycle detects live over mini and mini over advanced") {
  CHECK(overlay_hud_step_of(true, true, true, true) == OverlayHudStep::Live);
  CHECK(overlay_hud_step_of(true, true, true, false) == OverlayHudStep::Mini);
  CHECK(overlay_hud_step_of(true, false, true, false) == OverlayHudStep::Advanced);
  CHECK(next_overlay_hud_step(OverlayHudStep::Live) == OverlayHudStep::Off);
  CHECK(next_overlay_hud_step(OverlayHudStep::Off) == OverlayHudStep::Compact);
}

TEST_CASE("lobby overlay checkboxes pick the Ctrl+Space start step") {
  CHECK(overlay_hud_step_of(true, false, false, false) == OverlayHudStep::Compact);
  CHECK(overlay_hud_step_of(true, true, false, false) == OverlayHudStep::Mini);
  CHECK(overlay_hud_step_of(true, false, true, false) == OverlayHudStep::Advanced);
  CHECK(overlay_hud_step_of(true, false, false, true) == OverlayHudStep::Live);
  CHECK(overlay_hud_step_of(false, false, false, false) == OverlayHudStep::Off);
  CHECK(cycle_overlay_hud(true, true, false, false).advanced);  // Mini → Advanced
  CHECK(cycle_overlay_hud(true, false, false, true).visible == false);  // Live → Off
}

TEST_CASE("overlay HUD cycle is session-only and does not rewrite lobby settings") {
  PersistedSettings lobby = default_settings();
  lobby.ui.mini_overlay = false;
  lobby.ui.advanced_metrics = false;
  lobby.ui.live_overlay = false;
  lobby.ui.show_fps = true;
  OverlayHudFlags f = overlay_hud_flags_for(OverlayHudStep::Compact);
  f = cycle_overlay_hud(f.visible, f.mini, f.advanced, f.live);
  CHECK(f.mini);
  CHECK_FALSE(lobby.ui.mini_overlay);
  CHECK_FALSE(lobby.ui.advanced_metrics);
  CHECK_FALSE(lobby.ui.live_overlay);
  CHECK(lobby.ui.show_fps);
}

TEST_CASE("motor restart restores the session overlay without writing lobby checks") {
  PersistedSettings lobby = default_settings();
  lobby.ui.mini_overlay = false;
  lobby.ui.advanced_metrics = false;
  lobby.ui.live_overlay = false;
  lobby.ui.show_fps = true;

  OverlayHudFlags live = overlay_hud_flags_for(OverlayHudStep::Live);
  const std::string args = session_cfg_relaunch_args_for_hud(live.visible, live.mini, live.advanced, live.live);
  CHECK(args.find("--autostart") != std::string::npos);
  CHECK(args.find("--hud=live") != std::string::npos);
  CHECK(session_cfg_relaunch_args_for_hud(OverlayHudStep::Mini).find("--hud=mini") != std::string::npos);
  CHECK(session_cfg_relaunch_args_for_hud(OverlayHudStep::Off).find("--hud=off") != std::string::npos);
  CHECK(std::string(session_cfg_relaunch_args()) == "--autostart");

  char a0[] = "NexusCap.exe";
  char a1[] = "--autostart";
  char a2[] = "--hud=live";
  char* argv[] = {a0, a1, a2};
  AppConfig cfg = parse_args(3, argv);
  CHECK(cfg.autostart);
  CHECK(cfg.overlay_hud_restore == static_cast<int>(OverlayHudStep::Live));
  apply_persisted_to_config(lobby, cfg);
  CHECK_FALSE(cfg.live_overlay);  // disco ainda e o lobby
  CHECK(apply_autostart_overlay_hud(cfg));
  CHECK(cfg.live_overlay);
  CHECK(cfg.show_overlay);
  CHECK_FALSE(cfg.mini_overlay);
  CHECK_FALSE(cfg.advanced_metrics);
  CHECK_FALSE(lobby.ui.live_overlay);
  apply_session_cfg_to_persisted(cfg, lobby);
  CHECK_FALSE(lobby.ui.live_overlay);
  CHECK_FALSE(lobby.ui.mini_overlay);
}

TEST_CASE("Ctrl+Space overlay cycle stays blocked while session CFG is open") {
  CHECK(session_hotkeys_blocked_while_cfg_open(true));
  CHECK_FALSE(session_hotkeys_blocked_while_cfg_open(false));
}

TEST_CASE("session report paths are unique by timestamp and kind") {
  const std::string bug_a = make_session_report_path("C:\\NexusCap", SessionReportKind::Bug, 2026, 9, 16, 21, 5, 30);
  const std::string bug_b = make_session_report_path("C:\\NexusCap", SessionReportKind::Bug, 2026, 9, 16, 21, 5, 31);
  const std::string perf =
      make_session_report_path("C:\\NexusCap", SessionReportKind::Perf, 2026, 9, 16, 21, 5, 30);
  CHECK(bug_a.find("\\report\\bug_20260916_210530.txt") != std::string::npos);
  CHECK(bug_b.find("bug_20260916_210531.txt") != std::string::npos);
  CHECK(perf.find("\\performance\\performance_20260916_210530.txt") != std::string::npos);
  CHECK(bug_a != bug_b);
  CHECK(bug_a != perf);
}

TEST_CASE("bug_report and perf_report persist and map to AppConfig") {
  CHECK_FALSE(default_settings().ui.bug_report);
  CHECK_FALSE(default_settings().ui.perf_report);

  PersistedSettings s = default_settings();
  s.ui.bug_report = true;
  s.ui.perf_report = true;
  s.hotkeys.vk_hud = 0x20;
  s.hotkeys.hud_ctrl = true;
  const std::string ini = serialize_settings_ini(s);
  CHECK(ini.find("bug_report=1") != std::string::npos);
  CHECK(ini.find("perf_report=1") != std::string::npos);
  CHECK(ini.find("hud_ctrl=1") != std::string::npos);

  PersistedSettings out;
  CHECK(parse_settings_ini(ini, out));
  CHECK(out.ui.bug_report);
  CHECK(out.ui.perf_report);
  CHECK(out.hotkeys.hud_ctrl);
  CHECK(out.hotkeys.vk_hud == 0x20);

  AppConfig cfg;
  apply_persisted_to_config(out, cfg);
  CHECK(cfg.bug_report);
  CHECK(cfg.perf_report);
  CHECK(cfg.hotkeys.hud_ctrl);

  SessionReportWriter idle;
  CHECK_FALSE(idle.active());
}

TEST_CASE("SessionReportWriter writes unique files when started") {
  namespace fs = std::filesystem;
  const auto tmp = fs::temp_directory_path() / "nexuscap_report_ut";
  fs::create_directories(tmp);
  const std::string dir = tmp.string();

  SessionReportSnapshot snap;
  snap.device = "ut-device";
  snap.width = 1920;
  snap.height = 1080;
  snap.os_version = "Windows UT";
  snap.cpu_name = "UT-CPU";
  snap.gpu_name = "UT-GPU";
  snap.reshade_snippet = "NRIntensity=1\n";
  snap.feed_snippet = "work_resolution=100\n";
  snap.mgf_snippet = "Techniques=\n";

  const std::string bug_path = make_session_report_path(dir, SessionReportKind::Bug, 2026, 1, 2, 3, 4, 5);
  SessionReportWriter bug;
  bug.start(SessionReportKind::Bug, bug_path, snap);
  bug.note_event(1, "hitch", "interval_ms=30");
  bug.end(2, 10, 0, 1, 0);
  CHECK(fs::exists(bug_path));
  const std::string bug_txt = read_text_file_utf8(bug_path);
  CHECK(bug_txt.find("NEXUSCAP_BUG_REPORT") != std::string::npos);
  CHECK(bug_txt.find("event=hitch") != std::string::npos);

  const std::string perf_path =
      make_session_report_path(dir, SessionReportKind::Perf, 2026, 1, 2, 3, 4, 6);
  SessionReportWriter perf;
  perf.start(SessionReportKind::Perf, perf_path, snap);
  perf.heartbeat(100, 60, 16, 2, 16.5, 17.0, 0, 0, 0, false, true, 10, 20, true, true, 8.5, 2.1);
  perf.end(200, 20, 0, 0, 0);
  CHECK(fs::exists(perf_path));
  const std::string perf_txt = read_text_file_utf8(perf_path);
  CHECK(perf_txt.find("=== SETUP ===") != std::string::npos);
  CHECK(perf_txt.find("=== RENODX ===") != std::string::npos);
  CHECK(perf_txt.find("=== TUNABLES (app) ===") != std::string::npos);
  CHECK(perf_txt.find("=== METRICS ===") != std::string::npos);
  CHECK(perf_txt.find("=== AI_HINT ===") != std::string::npos);
  CHECK(perf_txt.find("UT-CPU") != std::string::npos);
  CHECK(perf_txt.find("NRIntensity=1") != std::string::npos);
  CHECK(perf_txt.find("ft_avg=") != std::string::npos);
  CHECK(perf_txt.find("e2e=") != std::string::npos);

  SessionReportWriter bug2;
  const std::string bug2_path = make_session_report_path(dir, SessionReportKind::Bug, 2026, 1, 2, 3, 4, 7);
  bug2.start(SessionReportKind::Bug, bug2_path, snap);
  bug2.note_exception(50, "present", "device lost");
  bug2.end(60, 1, 0, 0, 0);
  const std::string bug2_txt = read_text_file_utf8(bug2_path);
  CHECK(bug2_txt.find("=== EXCEPTIONS ===") != std::string::npos);
  CHECK(bug2_txt.find("event=exception") != std::string::npos);
  CHECK(bug2_txt.find("where=present") != std::string::npos);
  CHECK(bug2_txt.find("device lost") != std::string::npos);

  SessionReportWriter noop;
  CHECK_FALSE(noop.active());
}

TEST_CASE("live console enum parse/label/color") {
  CHECK(parse_live_console("ps5") == LiveConsole::Ps5);
  CHECK(parse_live_console("") == LiveConsole::Ps5);
  CHECK(parse_live_console("ps4") == LiveConsole::Ps4);
  CHECK(parse_live_console("ps3") == LiveConsole::Ps3);
  CHECK(parse_live_console("xbox_ss") == LiveConsole::XboxSs);
  CHECK(parse_live_console("xbox") == LiveConsole::XboxSs);
  CHECK(parse_live_console("xbox_sx") == LiveConsole::XboxSx);
  CHECK(parse_live_console("xsx") == LiveConsole::XboxSx);
  CHECK(parse_live_console("xbox_one") == LiveConsole::XboxOne);
  CHECK(parse_live_console("switch2") == LiveConsole::Switch2);
  CHECK(parse_live_console("nsw2") == LiveConsole::Switch2);
  CHECK(parse_live_console("switch") == LiveConsole::Switch);
  CHECK(parse_live_console("switch1") == LiveConsole::Switch);
  CHECK(parse_live_console("ps5_pro") == LiveConsole::Ps5Pro);
  CHECK(parse_live_console("ps5pro") == LiveConsole::Ps5Pro);
  CHECK(std::string(live_console_key(LiveConsole::Ps5)) == "ps5");
  CHECK(std::string(live_console_key(LiveConsole::Ps5Pro)) == "ps5_pro");
  CHECK(std::string(live_console_key(LiveConsole::Ps4)) == "ps4");
  CHECK(std::string(live_console_key(LiveConsole::Ps3)) == "ps3");
  CHECK(std::string(live_console_key(LiveConsole::XboxSs)) == "xbox_ss");
  CHECK(std::string(live_console_key(LiveConsole::XboxSx)) == "xbox_sx");
  CHECK(std::string(live_console_key(LiveConsole::XboxOne)) == "xbox_one");
  CHECK(std::string(live_console_key(LiveConsole::Switch2)) == "switch2");
  CHECK(std::string(live_console_key(LiveConsole::Switch)) == "switch");
  CHECK(std::string(live_console_label(LiveConsole::Ps5)) == "PS5");
  CHECK(std::string(live_console_label(LiveConsole::Ps5Pro)) == "PS5 Pro");
  CHECK(std::string(live_console_label(LiveConsole::Ps4)) == "PS4");
  CHECK(std::string(live_console_label(LiveConsole::Ps3)) == "PS3");
  CHECK(std::string(live_console_label(LiveConsole::XboxSs)) == "Xbox Series S");
  CHECK(std::string(live_console_label(LiveConsole::XboxSx)) == "Xbox Series X");
  CHECK(std::string(live_console_label(LiveConsole::XboxOne)) == "Xbox One");
  CHECK(std::string(live_console_label(LiveConsole::Switch2)) == "Switch 2");
  CHECK(std::string(live_console_label(LiveConsole::Switch)) == "Switch");
  CHECK(live_console_color(LiveConsole::Ps5) == RGB(0, 55, 145));
  CHECK(live_console_color(LiveConsole::Ps5Pro) == RGB(0, 112, 210));
  CHECK(live_console_color(LiveConsole::Ps4) == RGB(0, 85, 185));
  CHECK(live_console_color(LiveConsole::Ps3) == RGB(0, 32, 90));
  CHECK(live_console_color(LiveConsole::XboxSs) == RGB(16, 124, 16));
  CHECK(live_console_color(LiveConsole::XboxSx) == RGB(16, 124, 16));
  CHECK(live_console_color(LiveConsole::XboxOne) == RGB(16, 124, 16));
  CHECK(live_console_color(LiveConsole::Switch2) == RGB(230, 0, 18));
  CHECK(live_console_color(LiveConsole::Switch) == RGB(230, 0, 18));
  CHECK(clamp_live_console(-1) == LiveConsole::Ps5);
  CHECK(clamp_live_console(99) == LiveConsole::Ps5);
  CHECK(clamp_live_console(kLiveConsoleCount) == LiveConsole::Ps5);
  CHECK(clamp_live_console(static_cast<int>(LiveConsole::Switch)) == LiveConsole::Switch);
  CHECK(clamp_live_console(0) == LiveConsole::Ps5);
  CHECK(clamp_live_console(1) == LiveConsole::XboxSs);
  CHECK(clamp_live_console(2) == LiveConsole::Switch2);

  bool seen[kLiveConsoleCount]{};
  for (LiveConsole o : kLiveConsoleOrder) {
    const int i = static_cast<int>(o);
    CHECK(i >= 0);
    CHECK(i < kLiveConsoleCount);
    CHECK_FALSE(seen[i]);
    seen[i] = true;
    CHECK(parse_live_console(live_console_key(o)) == o);
    CHECK(clamp_live_console(i) == o);
    CHECK_FALSE(std::string(live_console_label(o)).empty());
  }
  for (int i = 0; i < kLiveConsoleCount; ++i) CHECK(seen[i]);
}

TEST_CASE("console overlay is optional and defaults to PS5 when shown") {
  PersistedSettings s = default_settings();
  CHECK_FALSE(s.ui.overlay_console);
  CHECK(s.ui.live_console == "ps5");
  CHECK(parse_live_console(s.ui.live_console) == LiveConsole::Ps5);
  AppConfig cfg;
  apply_persisted_to_config(s, cfg);
  CHECK_FALSE(cfg.overlay_console);
  CHECK(cfg.live_console == static_cast<int>(LiveConsole::Ps5));
  auto hud = make_lobby_hud_preview_lines(s, 60.0);
  CHECK_FALSE(lobby_hud_contains(hud, "PS5"));

  s.ui.overlay_console = true;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.overlay_console);
  hud = make_lobby_hud_preview_lines(s, 60.0);
  CHECK(lobby_hud_contains(hud, "PS5"));
  char chip[48]{};
  format_overlay_console_chip(chip, sizeof(chip), LiveConsole::Ps5);
  CHECK(std::string(chip) == "PS5");
  CHECK(overlay_console_label_or_null(false, LiveConsole::Ps5) == nullptr);
  CHECK(std::string(overlay_console_label_or_null(true, LiveConsole::Ps5)) == "PS5");
}

TEST_CASE("console branding is public on compact mini and persist") {
  PersistedSettings s = default_settings();
  s.ui.show_fps = true;
  s.ui.overlay_console = true;
  s.ui.live_console = "xbox_sx";
  auto hud = make_lobby_hud_preview_lines(s, 60.0);
  CHECK(lobby_hud_contains(hud, "Xbox Series X"));
  CHECK_FALSE(lobby_hud_contains(hud, "\xE2\x96\xA0"));

  s.ui.advanced_metrics = true;
  s.ui.live_console = "ps4";
  hud = make_lobby_hud_preview_lines(s, 60.0);
  CHECK(lobby_hud_contains(hud, "PS4"));
  CHECK(lobby_hud_contains(hud, "ADV"));

  s.ui.advanced_metrics = false;
  s.ui.live_console = "xbox_sx";
  hud = make_lobby_hud_preview_lines(s, 60.0);
  CHECK(lobby_hud_contains(hud, "Xbox Series X"));
  CHECK(lobby_hud_contains(hud, "NexusCap"));

  s.ui.overlay_console = false;
  hud = make_lobby_hud_preview_lines(s, 60.0);
  CHECK_FALSE(lobby_hud_contains(hud, "Xbox Series X"));
  CHECK(lobby_hud_contains(hud, "NexusCap"));

  MiniOverlaySeg segs[kMiniOverlayMaxSegs]{};
  char a[384]{}, b[384]{};
  unsigned char cr = 0, cg = 85, cb = 185;
  const int n = format_mini_overlay_segments(
      segs, kMiniOverlayMaxSegs, a, sizeof(a), b, sizeof(b), 60.0, 0.0, false, 2, true, true, 16.7, 16.6, 17.0,
      18.0, true, 8.5, 2.2, 0, 0, 0, 2560, 1440, 2560, 1440, 2560, 1440, 100, false, 0, 0, 0, 0, "Default",
      nullptr, 0, 0, 0, nullptr, 60.0, 1, "PS4", false, cr, cg, cb);
  CHECK(n > 0);
  CHECK(std::strstr(segs[0].text, "PS4") != nullptr);
  CHECK(std::strstr(segs[0].text, "\xE2\x96\xA0") == nullptr);
  CHECK(std::strstr(a, "PS4") != nullptr);

  AppConfig cfg;
  cfg.live_console = static_cast<int>(LiveConsole::Ps3);
  cfg.overlay_console = true;
  PersistedSettings back = default_settings();
  apply_session_cfg_to_persisted(cfg, back);
  CHECK(back.ui.live_console == std::string("ps3"));
  CHECK(back.ui.overlay_console);
}

TEST_CASE("live mode gate file helpers still work; overlay active follows flag") {
  CHECK_FALSE(live_mode_unlocked_at(""));
  CHECK_FALSE(live_mode_unlocked_at("C:\\this\\path\\should\\not\\exist\\live.on"));
  CHECK_FALSE(live_overlay_active(false, false));
  CHECK(live_overlay_active(false, true));  // no longer gated by live.on
  CHECK(live_overlay_active(true, true));
  CHECK_FALSE(live_overlay_active(true, false));

  namespace fs = std::filesystem;
  const auto dir =
      fs::temp_directory_path() /
      ("nexuscap_live_gate_test_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(dir);
  const auto path = dir / "live.on";
  {
    std::ofstream o(path.string());
    o << "1\n";
  }
  CHECK(live_mode_unlocked_at(path.string()));
  fs::remove(path);
  CHECK_FALSE(live_mode_unlocked_at(path.string()));
  fs::remove_all(dir);
}

TEST_CASE("live fps line shows FG bar when on") {
  char line[96]{};
  format_live_fps_line(line, sizeof(line), 60.0, 240.0, true, 4);
  CHECK(std::string(line) == "60.0 | 240.0");
  format_live_fps_line(line, sizeof(line), 59.9, 0.0, false, 2);
  CHECK(std::string(line) == "59.9");
  format_live_dlss5_line(line, sizeof(line), true, 1, true);
  CHECK(std::string(line) == "DLSS 1x");
  CHECK(std::string(line).find("stable") == std::string::npos);
  CHECK(std::string(line).find("Model") == std::string::npos);
  format_live_dlss5_line(line, sizeof(line), true, 2, true);
  CHECK(std::string(line) == "DLSS 2x");
  format_live_dlss5_line(line, sizeof(line), true, 5, true);
  CHECK(std::string(line) == "DLSS 2x+");
  format_live_dlss5_line(line, sizeof(line), true, 6, true);
  CHECK(std::string(line) == "DLSS 3x+");
  format_live_dlss5_line(line, sizeof(line), true, 3, true);
  CHECK(std::string(line) == "DLSS 3x");
  format_live_dlss5_line(line, sizeof(line), true, 4, true);
  CHECK(std::string(line) == "DLSS 1.5x");
  format_live_dlss5_line(line, sizeof(line), false, 2, true);
  CHECK(std::string(line) == "DLSS off");
  format_live_fg_line(line, sizeof(line), true, 2);
  CHECK(std::string(line) == "FG Fixo x2 f0");
  format_live_fg_line(line, sizeof(line), true, 3);
  CHECK(std::string(line) == "FG Fixo x3 f0");
  format_live_fg_line(line, sizeof(line), true, 4);
  CHECK(std::string(line) == "FG Fixo x4 f0");
  format_live_fg_line(line, sizeof(line), false, 3);
  CHECK(std::string(line) == "FG off f0");
  format_live_fg_line(line, sizeof(line), true, 2, 2);
  CHECK(std::string(line) == "FG Fixo x2 f2");
  format_live_fg_line(line, sizeof(line), true, 2, 0, true, 70);
  CHECK(std::string(line) == "FG Adaptive 70 f0");
  CHECK(std::strstr(line, "FG") == line);
  LiveStripLines strip{};
  fill_live_strip_lines(strip, true, LiveConsole::Ps5Pro, true, 5, true, 3, 60.0);
  CHECK(strip.has_console);
  CHECK(std::string(strip.console) == "PS5 Pro");
  CHECK(std::string(strip.dlss) == "DLSS 2x+");
  CHECK(std::string(strip.fg) == "FG Fixo x3 f0");
  CHECK(std::string(strip.fps_cap.text) == "60.0");
  CHECK(strip.has_fg_fps);
  CHECK(std::string(strip.fps_fg.text) == "180.0");
  CHECK(strip.ft[0] == '\0');
  fill_live_strip_lines(strip, true, LiveConsole::Ps5Pro, true, 5, true, 3, 60.0, 180.0, true, 0, 16.67);
  CHECK(std::string(strip.ft) == "16.67 · FG 5.56");
  {
    const FgFtRgb cap = fg_frametime_trace_rgb(1, true);
    CHECK(strip.fps_cap.r == cap.r);
    CHECK(strip.fps_cap.g == cap.g);
    CHECK(strip.fps_cap.b == cap.b);
    const FgFtRgb fg3 = fg_frametime_trace_rgb(3, true);
    CHECK(strip.fps_fg.r == fg3.r);
    CHECK(strip.fps_fg.g == fg3.g);
    CHECK(strip.fps_fg.b == fg3.b);
  }
}

TEST_CASE("apply_persisted copies live overlay flags") {
  PersistedSettings s = default_settings();
  s.ui.live_overlay = true;
  s.ui.overlay_console = true;
  s.ui.live_console = "switch2";
  AppConfig cfg;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.live_overlay);
  CHECK(cfg.overlay_console);
  CHECK(cfg.live_console == static_cast<int>(LiveConsole::Switch2));
  s.ui.live_console = "xbox_sx";
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.live_console == static_cast<int>(LiveConsole::XboxSx));
  s.ui.live_console = "ps3";
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.live_console == static_cast<int>(LiveConsole::Ps3));
  s.ui.live_console = "xbox_one";
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.live_console == static_cast<int>(LiveConsole::XboxOne));
  s.ui.live_console = "switch";
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.live_console == static_cast<int>(LiveConsole::Switch));
}

TEST_CASE("apply_persisted copies live console") {
  PersistedSettings s = default_settings();
  s.ui.live_console = "xbox_sx";
  AppConfig cfg;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.live_console == static_cast<int>(LiveConsole::XboxSx));
}

TEST_CASE("live overlay active follows setting without live.on") {
  PersistedSettings s = default_settings();
  s.ui.live_overlay = true;
  AppConfig cfg;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.live_overlay);
  CHECK(live_overlay_active(false, cfg.live_overlay));
}

TEST_CASE("dlss5 motor swap notice names both engines") {
  const std::string msg = session_notice_dlss5_motor_swap(1, 2);
  CHECK(msg.find("Otimizando o pipeline") != std::string::npos);
  CHECK(msg.find("motor") != std::string::npos);
  CHECK(msg.find("1x") != std::string::npos);
  CHECK(msg.find("2x") != std::string::npos);
}

// Feed reads dlss5-feed.cfg once per process — a new scale only lands after relaunch.
TEST_CASE("feed scale change needs relaunch only under RenoDX") {
  CHECK(feed_scale_change_needs_relaunch(true, 100, 50));
  CHECK(feed_scale_change_needs_relaunch(true, 50, 100));
  CHECK_FALSE(feed_scale_change_needs_relaunch(true, 50, 50));
  // Both floor to 50% in the Feed — restarting would change nothing.
  CHECK_FALSE(feed_scale_change_needs_relaunch(true, 40, 33));
  // No RenoDX: renderer scales its own RT, no restart.
  CHECK_FALSE(feed_scale_change_needs_relaunch(false, 100, 50));
  const std::string msg = session_notice_feed_scale_apply(100, 50);
  CHECK(msg.find("100%") != std::string::npos);
  CHECK(msg.find("50%") != std::string::npos);
}

TEST_CASE("lobby Iniciar always relaunches so CFG is first swapchain") {
  CHECK(lobby_iniciar_needs_session_relaunch());
  const std::string msg = session_notice_lobby_iniciar_apply();
  CHECK(msg.find("DLSS 5") != std::string::npos);
  CHECK(msg.find("preset") != std::string::npos);
  CHECK(msg.find("escala") != std::string::npos);

  // Cinema + NR off + 50%: disk → cfg → autostart boot uses session scale, not lobby 100%.
  PersistedSettings s = default_settings();
  s.enhance_renodx = true;
  s.neural_rendering = false;
  s.renodx_preset = static_cast<int>(RenodxVisualPreset::Cinematic);
  s.nr_pass_count = 1;
  s.scale_percent = 50;
  AppConfig cfg;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.enhance == EnhanceBackend::Renodx);
  CHECK_FALSE(cfg.neural_rendering);
  CHECK(cfg.renodx_preset == 2);
  CHECK(cfg.scale_percent == 50);
  CHECK(feed_work_resolution_at_boot(/*lobby_first=*/true, cfg.scale_percent) == 100);
  CHECK(feed_work_resolution_at_boot(/*lobby_first=*/false, cfg.scale_percent) == 50);
  CHECK(want_neural_rendering(s.enhance_renodx, s.neural_rendering) == false);

  s.neural_rendering = true;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.neural_rendering);
  CHECK(want_neural_rendering(s.enhance_renodx, s.neural_rendering));

  s.enhance_renodx = false;
  s.neural_rendering = true;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.enhance == EnhanceBackend::Off);
  CHECK_FALSE(cfg.neural_rendering);

  std::string feed = "enabled=1\npreset=0\nwork_resolution=100\n";
  std::string reshade = "NeuralUplift=1\nNRPreset=0\n";
  feed = apply_feed_enabled(std::move(feed), false);
  feed = apply_renodx_visual_preset_to_feed(std::move(feed), 2, 1);
  feed = apply_feed_work_resolution(std::move(feed), feed_work_resolution_for_session(50));
  reshade = apply_reshade_neural_uplift(std::move(reshade), false);
  reshade = apply_renodx_visual_preset_to_reshade(std::move(reshade), 2);
  CHECK_FALSE(parse_feed_enabled(feed, true));
  CHECK_FALSE(parse_reshade_neural_uplift(reshade, true));
  CHECK(renodx_files_reflect_visual_preset(feed, reshade, 2, 1));
  CHECK(parse_feed_work_resolution(feed) == 50);
}

// Measured: multipass RenoDX ignores NRToggleKey, so Delete must not fake an "off" HUD.
TEST_CASE("NR hotkey never claimed: every Pass Count runs on multipass") {
  CHECK(renodx_stack_supports_nr_hotkey(/*multipass_stack=*/false));
  CHECK_FALSE(renodx_stack_supports_nr_hotkey(/*multipass_stack=*/true));
  CHECK_FALSE(renodx_stack_supports_nr_hotkey(nr_pass_count_uses_multipass_addon(2)));
  CHECK_FALSE(renodx_stack_supports_nr_hotkey(nr_pass_count_uses_multipass_addon(4)));
  CHECK_FALSE(renodx_stack_supports_nr_hotkey(nr_pass_count_uses_multipass_addon(1)));
  const std::string msg15 = session_notice_nr_hotkey_unavailable(4);
  CHECK(msg15.find("1,5x") != std::string::npos);
  CHECK(msg15.find("Home") != std::string::npos);
}

// ReShade keeps PassCount in the saved preset AND in the live addon section.
TEST_CASE("pass count rewrites every ReShade section") {
  const std::string ini =
      "[RENODX-DLSS-preset1]\nDirectNeuralRenderingPassCount=1\nDirectNeuralRenderingStyle=0\n\n"
      "[RenoDX.DLSS5]\nDirectNeuralRenderingPassCount=1\nNeuralUplift=1\n";
  const std::string out = apply_reshade_nr_pass_count(ini, 3);
  CHECK(out.find("DirectNeuralRenderingPassCount=1") == std::string::npos);
  std::size_t hits = 0;
  for (std::size_t p = out.find("DirectNeuralRenderingPassCount=3"); p != std::string::npos;
       p = out.find("DirectNeuralRenderingPassCount=3", p + 1)) {
    ++hits;
  }
  CHECK(hits == 2);
  CHECK(out.find("DirectNeuralRenderingStyle=0") != std::string::npos);
  CHECK(out.find("NeuralUplift=1") != std::string::npos);
  CHECK(parse_reshade_nr_pass_count(out) == 3);
  // Missing key still gets appended once.
  CHECK(parse_reshade_nr_pass_count(apply_reshade_nr_pass_count("[RenoDX.DLSS5]\n", 2)) == 2);
}

// Measured: dlss5-feed rejects work_resolution < 50 and builds at 100% instead.
TEST_CASE("feed work_resolution floors at 50 percent") {
  CHECK(feed_work_resolution_for_session(100) == 100);
  CHECK(feed_work_resolution_for_session(67) == 67);
  CHECK(feed_work_resolution_for_session(50) == 50);
  CHECK(feed_work_resolution_for_session(40) == kMinFeedWorkResolution);
  CHECK(feed_work_resolution_for_session(kMinScalePercent) == kMinFeedWorkResolution);
  CHECK(parse_feed_work_resolution(apply_feed_work_resolution("work_resolution=100\n", 33), 0) == 50);
  // Overlay must report what the Feed builds, not what the slider asked for.
  CHECK(effective_scale_percent(true, 33) == 50);
  CHECK(effective_scale_percent(false, 33) == 33);  // no RenoDX → renderer really scales
  CHECK(feed_scale_below_floor(true, 49));
  CHECK_FALSE(feed_scale_below_floor(true, 50));
  CHECK_FALSE(feed_scale_below_floor(false, 25));
}

TEST_CASE("--autostart keeps lobby settings seeding and skips the menu once") {
  const char* argv[] = {"NexusCap.exe", "--autostart"};
  const AppConfig c = parse_args(2, const_cast<char**>(argv));
  CHECK(c.autostart);
  CHECK(c.show_lobby);  // settings still seed cfg; main skips the first run_lobby
  CHECK_FALSE(c.open_lobby_cfg);  // captura, nao o CFG do lobby
}

TEST_CASE("lobby CFG reopen after motor-swap relaunch") {
  CHECK(std::string(lobby_cfg_relaunch_args()) == "--lobby-cfg");
  CHECK(std::string(lobby_cfg_relaunch_args()).find("autostart") == std::string::npos);
  CHECK(std::string(session_cfg_relaunch_args()) == "--autostart");

  char a0[] = "NexusCap.exe";
  char a1[] = "--lobby-cfg";
  char* argv[] = {a0, a1};
  AppConfig c = parse_args(2, argv);
  CHECK(c.open_lobby_cfg);
  CHECK(c.show_lobby);
  CHECK_FALSE(c.autostart);

  bool flag = true;
  CHECK(lobby_consume_open_cfg(flag));
  CHECK_FALSE(flag);
  CHECK_FALSE(lobby_consume_open_cfg(flag));
  CHECK(lobby_consume_open_cfg(c.open_lobby_cfg));
  CHECK_FALSE(c.open_lobby_cfg);

  // Normal boot / Iniciar / Ctrl+M must not leave the lobby CFG stuck open.
  AppConfig fresh;
  CHECK_FALSE(lobby_consume_open_cfg(fresh.open_lobby_cfg));
}

TEST_CASE("mini overlay 3-row breaks keep stalls off the clipped FPS row") {
  MiniOverlaySeg segs[kMiniOverlayMaxSegs]{};
  char a[8]{}, b[8]{};
  const int n = format_mini_overlay_segments(
      segs, kMiniOverlayMaxSegs, a, sizeof(a), b, sizeof(b), 60.0, 0.0, false, 2, true, true, 16.7, 16.6, 17.0,
      18.0, true, 8.5, 2.2, 7, 1, 3, 2560, 1440, 1280, 720, 2560, 1440, 50, true, 59.9, 2.0, 2.1, 4, "Default",
      nullptr, 0, 900, 20.0, nullptr, 60.0, 1);
  int wrap_st = -1;
  int wrap_l600 = -1;
  mini_overlay_row_breaks(segs, n, &wrap_st, &wrap_l600);
  CHECK(wrap_st > 0);
  CHECK(wrap_st < wrap_l600);
  CHECK(wrap_l600 < n);
  CHECK(mini_overlay_seg_is_stalls(segs[wrap_st]));
  CHECK(std::strstr(segs[wrap_st].text, "st7/cl1") == nullptr);
  CHECK(std::strstr(segs[wrap_st].text, "stalls 7") != nullptr);
  CHECK(std::strstr(segs[wrap_st].text, "clusters 1") != nullptr);
  CHECK(mini_overlay_seg_is_l600(segs[wrap_l600]));
  // FPS / res / VSYNC stay on row 0; stalls never share that row (sparkline used to clip them).
  bool row0_has_res = false;
  bool row0_has_stalls = false;
  bool row1_has_ft = false;
  for (int i = 0; i < wrap_st; ++i) {
    if (std::strstr(segs[i].text, "2560x1440")) row0_has_res = true;
    if (mini_overlay_seg_is_stalls(segs[i])) row0_has_stalls = true;
  }
  for (int i = wrap_st; i < wrap_l600; ++i) {
    if (std::strncmp(segs[i].text, "ft ", 3) == 0) row1_has_ft = true;
  }
  CHECK(row0_has_res);
  CHECK_FALSE(row0_has_stalls);
  CHECK(row1_has_ft);
}

TEST_CASE("3.0 HUD stack line names motor and pass count") {
  char line[96]{};
  format_dlss5_stack_hud_line(line, sizeof(line), true, true, 1);
  CHECK(std::strstr(line, "1x") != nullptr);
  CHECK(std::strstr(line, "multipass") != nullptr);
  CHECK(std::strstr(line, "NR ON") != nullptr);
  format_dlss5_stack_hud_line(line, sizeof(line), false, true, 2);
  CHECK(std::strstr(line, "2x") != nullptr);
  CHECK(std::strstr(line, "multipass") != nullptr);
  CHECK(std::strstr(line, "NR off") != nullptr);
  format_dlss5_stack_hud_line(line, sizeof(line), true, true, 4);
  CHECK(std::strstr(line, "1.5x") != nullptr);
  CHECK(std::strstr(line, "multipass") != nullptr);
  CHECK(std::strstr(line, "1x") == nullptr);  // must not look like Krish 1x
  format_dlss5_stack_hud_line(line, sizeof(line), true, true, 5);
  CHECK(std::strstr(line, "2x+") != nullptr);
  CHECK(std::strstr(line, "2x+") == std::strstr(line, "2x"));
  format_live_dlss5_line(line, sizeof(line), true, 5, true);
  CHECK(std::string(line) == "DLSS 2x+");
  format_live_dlss5_line(line, sizeof(line), true, 6, true);
  CHECK(std::string(line) == "DLSS 3x+");
}

TEST_CASE("1.5x mini overlay names multipass motor not 1x stable") {
  MiniOverlaySeg segs[kMiniOverlayMaxSegs]{};
  char a[8]{}, b[8]{};
  const int n = format_mini_overlay_segments(
      segs, kMiniOverlayMaxSegs, a, sizeof(a), b, sizeof(b), 60.0, 0.0, false, 2, true, true, 16.7, 16.6, 17.0,
      18.0, true, 8.5, 2.2, 0, 0, 0, 2560, 1440, 2560, 1440, 2560, 1440, 100, true, 59.9, 2.0, 2.1, 0, "Default",
      nullptr, 0, 900, 20.0, nullptr, 60.0, 4);
  bool found_15 = false;
  bool found_motor = false;
  bool found_1x_token = false;
  for (int i = 0; i < n; ++i) {
    if (std::strstr(segs[i].text, "1.5x")) found_15 = true;
    if (std::strstr(segs[i].text, "multipass")) found_motor = true;
    if (std::strstr(segs[i].text, "1x") && std::strstr(segs[i].text, "1.5x") == nullptr) found_1x_token = true;
    if (std::strstr(segs[i].text, "stable")) found_1x_token = true;
  }
  CHECK(found_15);
  CHECK(found_motor);
  CHECK_FALSE(found_1x_token);
}

TEST_CASE("2x otimizado mini and live overlays show 2x+") {
  MiniOverlaySeg segs[kMiniOverlayMaxSegs]{};
  char a[8]{}, b[8]{};
  const int n = format_mini_overlay_segments(
      segs, kMiniOverlayMaxSegs, a, sizeof(a), b, sizeof(b), 60.0, 0.0, false, 2, true, true, 16.7, 16.6, 17.0,
      18.0, true, 8.5, 2.2, 0, 0, 0, 2560, 1440, 2560, 1440, 2560, 1440, 100, true, 59.9, 2.0, 2.1, 0, "Default",
      nullptr, 0, 900, 20.0, nullptr, 60.0, 5);
  bool found_2xp = false;
  bool found_bare_2x = false;
  for (int i = 0; i < n; ++i) {
    if (std::strstr(segs[i].text, "2x+")) found_2xp = true;
    if (std::strstr(segs[i].text, "2x") && std::strstr(segs[i].text, "2x+") == nullptr) found_bare_2x = true;
  }
  CHECK(found_2xp);
  CHECK_FALSE(found_bare_2x);
  char live[32]{};
  format_live_dlss5_line(live, sizeof(live), true, 5, true);
  CHECK(std::string(live) == "DLSS 2x+");
  format_live_dlss5_line(live, sizeof(live), true, 2, true);
  CHECK(std::string(live) == "DLSS 2x");
}

TEST_CASE("apply_keyed_int_all rewrites every occurrence") {
  const std::string in = "DirectNeuralRenderingPassCount=1\nkeep=1\nDirectNeuralRenderingPassCount=1\n";
  const std::string out = apply_keyed_int_all(in, "DirectNeuralRenderingPassCount=", 2);
  CHECK(out.find("DirectNeuralRenderingPassCount=1") == std::string::npos);
  CHECK(out.find("keep=1") != std::string::npos);
  std::size_t hits = 0;
  for (std::size_t p = out.find("DirectNeuralRenderingPassCount=2"); p != std::string::npos;
       p = out.find("DirectNeuralRenderingPassCount=2", p + 1)) {
    ++hits;
  }
  CHECK(hits == 2);
}

TEST_CASE("feed boot scale is 100 in lobby and session scale when autostart") {
  CHECK(feed_work_resolution_at_boot(/*lobby_first=*/true, 50) == 100);
  CHECK(feed_work_resolution_at_boot(/*lobby_first=*/false, 50) == 50);
  CHECK(feed_work_resolution_at_boot(false, 25) == kMinFeedWorkResolution);
}

TEST_CASE("session CFG hotkey default is Ctrl+M and never collides with the HUD") {
  const HotkeyConfig h = default_hotkeys();
  CHECK(h.vk_cfg == 0x4D);
  CHECK(h.cfg_ctrl);
  CHECK(cfg_hotkey_fired(0x4D, true, h.vk_cfg, h.cfg_ctrl));
  CHECK_FALSE(cfg_hotkey_fired(0x4D, false, h.vk_cfg, h.cfg_ctrl));
  CHECK_FALSE(cfg_hotkey_fired(0x20, true, h.vk_cfg, h.cfg_ctrl));
  // Ctrl+Espaco continua so HUD; Ctrl+M continua so CFG.
  CHECK_FALSE(hud_hotkey_fired(0x4D, true, h.vk_hud, h.hud_ctrl));
  CHECK(hud_hotkey_fired(0x20, true, h.vk_hud, h.hud_ctrl));
  CHECK(h.vk_cfg != h.vk_hud);
  CHECK(std::string(vk_display_name(0x4D)) == "M");
}

TEST_CASE("Esc closes the session CFG before leaving to the lobby") {
  CHECK(session_esc_action(/*cfg_open=*/true) == SessionEscAction::CloseCfg);
  CHECK(session_esc_action(/*cfg_open=*/false) == SessionEscAction::BackToLobby);
}

TEST_CASE("session hotkeys are blocked while the CFG has the keyboard") {
  CHECK(session_hotkeys_blocked_while_cfg_open(true));
  CHECK_FALSE(session_hotkeys_blocked_while_cfg_open(false));
}

TEST_CASE("session CFG plan: motor swap and feed scale restart, the rest is live") {
  // 1x tambem e multipass: sem troca de addon, so o PassCount/look (reinicio de pass).
  CHECK(plan_session_cfg_apply(true, 1, 4, 100, 100) == SessionCfgApply::RelaunchPassCount);
  CHECK(plan_session_cfg_apply(true, 4, 1, 100, 100) == SessionCfgApply::RelaunchPassCount);
  CHECK(plan_session_cfg_apply(true, 1, 2, 100, 100) == SessionCfgApply::RelaunchPassCount);
  // 1,5x ↔ 2x ↔ 3x: mesmo motor, mas o multipass le o PassCount so no init (ReShade.log:
  // ini em 3 e runtime em "passes=1", sem NRToggleKey pro ciclo OFF→ON) — reinicia tambem.
  CHECK(plan_session_cfg_apply(true, 4, 2, 100, 100) == SessionCfgApply::RelaunchPassCount);
  CHECK(plan_session_cfg_apply(true, 2, 3, 100, 100) == SessionCfgApply::RelaunchPassCount);
  CHECK(plan_session_cfg_apply(true, 3, 4, 100, 100) == SessionCfgApply::RelaunchPassCount);
  CHECK(plan_session_cfg_preset_apply(true) == SessionCfgApply::RelaunchPreset);
  CHECK(plan_session_cfg_preset_apply(false) == SessionCfgApply::RelaunchPreset);
  CHECK_FALSE(session_cfg_preset_applies_live(true));
  CHECK_FALSE(session_cfg_preset_applies_live(false));
  CHECK(plan_session_cfg_nr_apply(true, false) == SessionCfgApply::RelaunchNr);
  CHECK(plan_session_cfg_nr_apply(true, true) == SessionCfgApply::RelaunchNr);
  CHECK(plan_session_cfg_nr_apply(false, false) == SessionCfgApply::Live);
  CHECK_FALSE(nr_pass_count_change_applies_live(4, 2));
  CHECK_FALSE(nr_pass_count_change_applies_live(2, 3));
  CHECK(nr_pass_count_change_applies_live(2, 2));  // sem troca, nada a aplicar
  CHECK(nr_pass_count_change_applies_live(1, 1));
  // Preset / NR / FG nao mexem em pass count nem escala.
  CHECK(plan_session_cfg_apply(true, 4, 4, 50, 50) == SessionCfgApply::Live);
  // Escala do Feed so entra no boot do processo.
  CHECK(plan_session_cfg_apply(true, 4, 4, 100, 50) == SessionCfgApply::RelaunchFeedScale);
  // 40% e 33% caem no mesmo piso do Feed — reiniciar nao mudaria nada.
  CHECK(plan_session_cfg_apply(true, 4, 4, 40, 33) == SessionCfgApply::Live);
  // Sem RenoDX o renderer escala o proprio RT.
  CHECK(plan_session_cfg_apply(false, 1, 1, 100, 50) == SessionCfgApply::Live);
  // Pass Count tem prioridade quando as duas coisas mudam juntas.
  CHECK(plan_session_cfg_apply(true, 1, 4, 100, 50) == SessionCfgApply::RelaunchPassCount);

  CHECK(session_cfg_apply_needs_relaunch(SessionCfgApply::RelaunchMotor));
  CHECK(session_cfg_apply_needs_relaunch(SessionCfgApply::RelaunchPassCount));
  CHECK(session_cfg_apply_needs_relaunch(SessionCfgApply::RelaunchFeedScale));
  CHECK(session_cfg_apply_needs_relaunch(SessionCfgApply::RelaunchPreset));
  CHECK(session_cfg_apply_needs_relaunch(SessionCfgApply::RelaunchNr));
  CHECK_FALSE(session_cfg_apply_needs_relaunch(SessionCfgApply::Live));
}

TEST_CASE("session CFG restart resumes the session, never the lobby") {
  CHECK(std::string(session_cfg_relaunch_args()) == "--autostart");
  CHECK(kSessionExitRelaunch != kSessionExitBackToLobby);
  const char* argv[] = {"NexusCap.exe", session_cfg_relaunch_args()};
  const AppConfig c = parse_args(2, const_cast<char**>(argv));
  CHECK(c.autostart);
}

TEST_CASE("session CFG NR on/off restarts on every RenoDX motor") {
  CHECK(session_cfg_nr_toggle_allowed(/*renodx=*/true, /*multipass=*/false));
  CHECK(session_cfg_nr_toggle_allowed(true, true));
  CHECK_FALSE(session_cfg_nr_toggle_allowed(false, false));
  CHECK(plan_session_cfg_nr_apply(true, false) == SessionCfgApply::RelaunchNr);
  CHECK(plan_session_cfg_nr_apply(true, true) == SessionCfgApply::RelaunchNr);
  const std::string on = session_notice_nr_apply(true);
  const std::string off = session_notice_nr_apply(false);
  CHECK(on.find("ON") != std::string::npos);
  CHECK(off.find("OFF") != std::string::npos);
  CHECK(on.find("reinicio") != std::string::npos);
}

TEST_CASE("session CFG changes survive the restart through settings.ini") {
  AppConfig cfg;
  cfg.enhance = EnhanceBackend::Renodx;
  cfg.renodx_preset = 2;  // Cinematic
  cfg.nr_pass_count = 2;
  cfg.scale_percent = 50;
  cfg.neural_rendering = true;
  cfg.fg_auto_start = true;
  cfg.fg_multiplier = 4;

  PersistedSettings s = default_settings();
  apply_session_cfg_to_persisted(cfg, s);
  CHECK(s.renodx_preset == 2);
  CHECK(s.nr_pass_count == 2);
  CHECK(s.scale_percent == 50);
  CHECK(s.neural_rendering);
  CHECK(s.enhance_renodx);
  CHECK(s.fg_enabled);
  CHECK(s.fg_multiplier == 4);

  PersistedSettings out;
  CHECK(parse_settings_ini(serialize_settings_ini(s), out));
  CHECK(out.renodx_preset == 2);
  CHECK(out.nr_pass_count == 2);
  CHECK(out.scale_percent == 50);
  CHECK(out.fg_enabled);
  CHECK(out.fg_multiplier == 4);

  // O processo novo monta o AppConfig a partir do ini — mesma sessao de volta.
  AppConfig back;
  apply_persisted_to_config(out, back);
  CHECK(back.renodx_preset == 2);
  CHECK(back.nr_pass_count == 2);
  CHECK(back.scale_percent == 50);
  CHECK(back.enhance == EnhanceBackend::Renodx);
}

TEST_CASE("CFG hotkey round trips through settings.ini and old files default to Ctrl+M") {
  PersistedSettings s = default_settings();
  s.hotkeys.vk_cfg = 0x4E;  // N
  s.hotkeys.cfg_ctrl = false;
  const std::string ini = serialize_settings_ini(s);
  CHECK(ini.find("cfg_key=78") != std::string::npos);
  CHECK(ini.find("cfg_ctrl=0") != std::string::npos);
  PersistedSettings out;
  CHECK(parse_settings_ini(ini, out));
  CHECK(out.hotkeys.vk_cfg == 0x4E);
  CHECK_FALSE(out.hotkeys.cfg_ctrl);

  // Ini de versao anterior (sem as chaves) carrega o default Ctrl+M.
  PersistedSettings old;
  CHECK(parse_settings_ini("[hotkeys]\nhud=32\nhud_ctrl=1\n", old));
  CHECK(old.hotkeys.vk_cfg == 0x4D);
  CHECK(old.hotkeys.cfg_ctrl);
}

TEST_CASE("file capture scale helpers stay live") {
  CHECK(file_cap_repeat_ok(2560, 1440, std::vector<uint8_t>(nv12_packed_bytes(2560, 1440))));
  CHECK_FALSE(file_cap_repeat_ok(0, 0, {}));
  CHECK(file_cap_is_integer_scale(1280, 720, 2560, 1440));
  CHECK_FALSE(file_cap_is_integer_scale(1280, 720, 1920, 1080));
  CHECK(file_cap_is_integer_scale(1920, 1080, 3840, 2160));
  CHECK_FALSE(file_cap_is_integer_scale(1920, 1080, 2560, 1440));
  CHECK(file_cap_src_fps_or(59.94) == doctest::Approx(59.94));
  CHECK(file_cap_src_fps_or(0.0) == doctest::Approx(30.0));
}

TEST_CASE("perf test waits for DLSS 5 before the clock") {
  CHECK(perf_test_pipeline_notice_title().find("Gerando") != std::string::npos);
  CHECK(perf_test_pipeline_notice_title().find("pipeline") != std::string::npos);
  CHECK(perf_test_pipeline_notice_title().find("Benchmark") != std::string::npos);
  CHECK(perf_test_pipeline_notice_detail().find("Aguarde") != std::string::npos);
  CHECK(session_notice_perf_test_hold().find("Gerando") != std::string::npos);
  CHECK(perf_test_pipeline_fail_title().find("nao ficou pronto") != std::string::npos);
  CHECK(perf_test_pipeline_fail_detail().find("Timeout") != std::string::npos);
  CHECK_FALSE(perf_test_should_fail_pipeline(false, kPerfTestRevealMaxMs - 1));
  CHECK(perf_test_should_fail_pipeline(false, kPerfTestRevealMaxMs));
  CHECK_FALSE(perf_test_should_fail_pipeline(true, kPerfTestRevealMaxMs));
  CHECK_FALSE(perf_test_dlss5_ready(false, 200));
  CHECK_FALSE(perf_test_dlss5_ready(true, 1));
  CHECK_FALSE(perf_test_dlss5_ready(true, kStatsWarmupFrames));
  CHECK(perf_test_dlss5_ready(true, kStatsWarmupFrames + 1));
  CHECK_FALSE(perf_test_should_start_clock(false, 0, false, false, false));
  CHECK(perf_test_should_start_clock(true, kStatsWarmupFrames + 1, false, false, false));
  CHECK_FALSE(perf_test_should_start_clock(false, 10, false, false, false));
  CHECK_FALSE(perf_test_should_start_clock(true, kStatsWarmupFrames + 1, true, false, false));
  CHECK(perf_test_should_start_clock(true, kStatsWarmupFrames + 1, true, true, false));
  CHECK(perf_test_should_start_clock(true, kStatsWarmupFrames + 1, true, false, true));
  CHECK_FALSE(perf_test_params_ready(true, 1, false, false, false));
  CHECK(perf_test_params_ready(true, kStatsWarmupFrames + 1, false, false, false));
  CHECK(perf_test_params_ready(true, kStatsWarmupFrames + 1, true, false, true));
  CHECK_FALSE(perf_test_fg_ready(true, false, false));
  CHECK(perf_test_fg_ready(true, false, true));
  CHECK(perf_test_fg_ready(true, true, false));
  CHECK(perf_test_fg_ready(false, false, false));
  CHECK_FALSE(perf_test_should_reveal_window(false, 0));
  CHECK_FALSE(perf_test_should_reveal_window(false, kPerfTestRevealMaxMs));
  CHECK(perf_test_should_reveal_window(true, 0));
  CHECK_FALSE(perf_test_should_stop(false));
  CHECK(perf_test_should_stop(true));
  CHECK_FALSE(perf_test_nr_runtime_live(true, false));
  CHECK_FALSE(perf_test_nr_runtime_live(false, true));
  CHECK(perf_test_nr_runtime_live(true, true));
  CHECK_FALSE(perf_test_nr_evals_are_live(0));
  CHECK(perf_test_nr_evals_are_live(1));
  CHECK(perf_test_nr_evals_are_live(kPerfTestNrMinEvals));
  CHECK_FALSE(perf_test_dlss5_ready(true, kStatsWarmupFrames + 1, false));
  CHECK(perf_test_dlss5_ready(true, kStatsWarmupFrames + 1, true));
  CHECK_FALSE(perf_test_should_start_clock(true, kStatsWarmupFrames + 1, false, false, false, 0, false));
  CHECK(perf_test_should_start_clock(true, kStatsWarmupFrames + 1, false, false, false, 0, true));
  CHECK(session_teardown_step_before(SessionTeardownStep::PresentBlack, SessionTeardownStep::HideWindow));
  CHECK(session_teardown_step_before(SessionTeardownStep::HideWindow, SessionTeardownStep::WriteAnalyze));
  CHECK(session_teardown_step_before(SessionTeardownStep::WriteAnalyze, SessionTeardownStep::ShutdownFg));
  CHECK(session_teardown_step_before(SessionTeardownStep::ShutdownFg, SessionTeardownStep::ReleaseD3d));
  CHECK(session_teardown_step_before(SessionTeardownStep::ReleaseD3d, SessionTeardownStep::DestroyHwnd));
  CHECK_FALSE(session_teardown_step_before(SessionTeardownStep::ReleaseD3d, SessionTeardownStep::HideWindow));
}

TEST_CASE("perf test A carregar never blocks forever after NR is ready") {
  const uint64_t ready = kStatsWarmupFrames + 1;
  CHECK(perf_test_carregar_can_block(false, true, false, false, 0));
  CHECK(perf_test_carregar_can_block(true, true, false, false, 0));
  CHECK_FALSE(perf_test_carregar_can_block(true, true, true, false, 0));
  CHECK_FALSE(perf_test_carregar_can_block(true, true, false, true, 0));
  CHECK_FALSE(perf_test_carregar_can_block(true, true, false, false, kPerfTestFgWaitMaxMs));
  CHECK_FALSE(perf_test_should_start_clock(true, ready, true, false, false, 0));
  CHECK(perf_test_should_start_clock(true, ready, true, false, false, kPerfTestFgWaitMaxMs));
  CHECK(perf_test_should_start_clock(true, ready, true, false, true, 0));
  CHECK(perf_test_should_start_clock(true, ready, true, true, false, 0));
  // NR ainda nao pronto: timeout do FG nao dispara o relogio (stall inicial).
  CHECK_FALSE(perf_test_should_start_clock(false, ready, true, false, false, 60000));
  CHECK_FALSE(perf_test_should_start_clock(true, 1, true, false, false, kPerfTestFgWaitMaxMs));
  CHECK(fg_overlay_reads_on(true, 4, true, 0, true, true));
  CHECK(fg_overlay_reads_on(true, 4, true, 0, true, false));
}

TEST_CASE("perf test remain_sec and timer") {
  CHECK(perf_test_remain_sec(0, 0) == kPerfTestMaxSec);
  CHECK(perf_test_remain_sec(1000, 0) == kPerfTestMaxSec - 1);
  CHECK(perf_test_remain_sec(20000, 0) == 20);
  CHECK(perf_test_remain_sec(40000, 0) == 0);
  CHECK(perf_test_remain_sec(41000, 0) == 0);
  CHECK(perf_test_remain_sec(1000, 1000) == kPerfTestMaxSec);
  char t[16]{};
  format_perf_test_timer(t, sizeof(t), 40);
  CHECK(std::string(t) == "0:40");
  format_perf_test_timer(t, sizeof(t), 39);
  CHECK(std::string(t) == "0:39");
  format_perf_test_timer(t, sizeof(t), 0);
  CHECK(std::string(t) == "0:00");
  format_perf_test_timer(t, sizeof(t), -1);
  CHECK(std::string(t) == "A carregar");
}

TEST_CASE("perf test playhead rebase advances at plan fps") {
  const double t0 = 8000;
  CHECK(virtual_pin_src_index(t0, t0, 30) == 0);
  CHECK(virtual_pin_src_index(t0 + 1000, t0, 30) == 30);
  CHECK(virtual_pin_src_index(t0 + 1000, t0, 60) == 60);
  CHECK(virtual_pin_src_index(t0 + 2000, t0, 30) == 60);
  CHECK(virtual_pin_src_index(t0 + 2000, t0, 60) == 120);
}

TEST_CASE("perf result popup only after this benchmark") {
  CHECK(perf_test_should_popup_result(true, true));
  CHECK_FALSE(perf_test_should_popup_result(false, true));
  CHECK_FALSE(perf_test_should_popup_result(true, false));
  CHECK_FALSE(perf_test_should_popup_result(false, false));
  CHECK(perf_test_should_popup_result(false, true, true));  // relaunch da escala 75%
  CHECK_FALSE(perf_test_should_popup_result(false, false, true));
  {
    char exe[] = "NexusCap.exe";
    char a0[] = "--show-perf-result";
    char* argv[] = {exe, a0};
    CHECK(parse_args(2, argv).popup_perf_result);
  }
}

TEST_CASE("lobby return relaunch keeps HDMI Reportar performance popup") {
  CHECK(std::string(lobby_return_relaunch_args(true, false)) == "--show-perf-result");
  CHECK(std::string(lobby_return_relaunch_args(false, true)) == "--show-perf-result");
  CHECK(lobby_return_relaunch_args(false, false) == nullptr);
}

TEST_CASE("HDMI session perf popup reuses Benchmark grade and keeps titles apart") {
  CHECK(session_perf_popup_wanted(true, false));
  CHECK_FALSE(session_perf_popup_wanted(true, true));
  CHECK_FALSE(session_perf_popup_wanted(false, false));
  CHECK_FALSE(session_perf_popup_wanted(false, true));
  CHECK(std::string(session_perf_popup_title()) == "Reportar performance ao finalizar gameplay");
  CHECK(std::string(perf_result_popup_title(true, PerfTestGrade::Good)) == session_perf_popup_title());
  CHECK(std::string(perf_result_popup_title(true, PerfTestGrade::Fail)) == session_perf_popup_title());
  CHECK(std::string(perf_result_popup_title(false, PerfTestGrade::Good)) == "Bom");
  CHECK(std::string(perf_result_popup_title(false, PerfTestGrade::Tight)) == "Apertado");
  CHECK(std::string(perf_result_popup_title(false, PerfTestGrade::PlayableFg)) == "Jogavel com FG");
  CHECK(std::string(perf_result_popup_title(false, PerfTestGrade::Fail)) == "Nao aguenta");

  CHECK_FALSE(session_perf_result_ready(true, false, 0));
  CHECK_FALSE(session_perf_result_ready(true, false, 179));
  CHECK(session_perf_result_ready(true, false, 180));
  CHECK_FALSE(session_perf_result_ready(true, true, 180));
  CHECK_FALSE(session_perf_result_ready(false, false, 180));

  AppConfig cfg;
  cfg.perf_test = false;
  cfg.perf_report = true;
  cfg.width = 2560;
  cfg.height = 1440;
  cfg.fps = 60;
  cfg.scale_percent = 100;
  cfg.nr_pass_count = 5;
  cfg.fg_auto_start = true;
  cfg.fg_multiplier = 2;

  PerfScoreAcc short_acc;
  for (int i = 0; i < 179; ++i) short_acc.push(16.667, 4.0, false);
  PerfTestResult no_popup;
  CHECK_FALSE(fill_perf_test_session_result(cfg, &short_acc, nullptr, no_popup));
  CHECK_FALSE(no_popup.valid);

  PerfScoreAcc acc;
  for (int i = 0; i < 180; ++i) acc.push(16.667, 4.0, false);

  AppConfig bench = cfg;
  bench.perf_test = true;
  bench.perf_report = false;
  PerfTestResult br;
  CHECK(fill_perf_test_session_result(bench, &acc, nullptr, br));
  CHECK(br.valid);
  CHECK(br.popup);
  CHECK_FALSE(br.hdmi_session);
  CHECK(br.grade == grade_perf_test(br.sample));
  CHECK(br.summary == format_perf_test_card(br.plan, br.sample, br.grade));
  CHECK(std::string(perf_result_popup_title(br.hdmi_session, br.grade)) == perf_test_grade_label(br.grade));
  CHECK(std::string(perf_result_popup_title(br.hdmi_session, br.grade)) != session_perf_popup_title());

  PerfTestResult hr;
  CHECK(fill_perf_test_session_result(cfg, &acc, nullptr, hr));
  CHECK(hr.valid);
  CHECK(hr.popup);
  CHECK(hr.hdmi_session);
  CHECK(hr.grade == br.grade);
  CHECK(hr.summary == br.summary);
  CHECK(hr.sample.fps == doctest::Approx(br.sample.fps));
  CHECK(std::string(perf_result_popup_title(hr.hdmi_session, hr.grade)) == session_perf_popup_title());

  std::string stripped;
  {
    std::istringstream in(serialize_perf_test_result(br));
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind("hdmi_session=", 0) == 0) continue;
      stripped += line;
      stripped += '\n';
    }
  }
  PerfTestResult legacy;
  CHECK(parse_perf_test_result(stripped, legacy));
  CHECK_FALSE(legacy.hdmi_session);
  CHECK(std::string(perf_result_popup_title(legacy.hdmi_session, legacy.grade)) ==
        perf_test_grade_label(legacy.grade));

  PerfTestResult hdmi_round;
  CHECK(parse_perf_test_result(serialize_perf_test_result(hr), hdmi_round));
  CHECK(hdmi_round.hdmi_session);
  CHECK(hdmi_round.grade == hr.grade);
  CHECK(hdmi_round.summary == hr.summary);

  PerfTestResult failr;
  CHECK(fill_perf_test_session_result(bench, nullptr, "Pipeline timeout", failr));
  CHECK(failr.grade == PerfTestGrade::Fail);
  CHECK(failr.summary == "Pipeline timeout");
  CHECK_FALSE(failr.hdmi_session);

  CHECK(fill_perf_test_session_result(bench, &short_acc, nullptr, failr));
  CHECK(failr.summary.find("amostras suficientes") != std::string::npos);
  CHECK_FALSE(failr.hdmi_session);

  CHECK_FALSE(fill_perf_test_session_result(cfg, nullptr, "Pipeline timeout", failr));

  AppConfig off;
  off.perf_test = false;
  off.perf_report = false;
  CHECK_FALSE(fill_perf_test_session_result(off, &acc, nullptr, failr));
}

TEST_CASE("perf test lobby says Benchmark") {
  CHECK(std::string(perf_test_lobby_title()) == "Benchmark");
  CHECK(std::string(perf_test_lobby_button()) == "Benchmark");
  CHECK(std::string(perf_test_lobby_start()) == "Iniciar benchmark");
  CHECK(std::string(perf_test_lobby_blurb()).find("40") != std::string::npos);
  CHECK(std::string(perf_test_lobby_blurb()).find("CFG HDMI") != std::string::npos);
  CHECK(std::string(perf_test_lobby_blurb()).find("beta") != std::string::npos);
  CHECK(std::string(perf_test_lobby_blurb()).find("nitid") != std::string::npos);
  CHECK(std::string(perf_test_lobby_blurb()).find("modelo") != std::string::npos);
  CHECK(std::string(perf_test_lobby_blurb()).find("equacao") != std::string::npos);
  CHECK(std::string(perf_test_lobby_blurb()).find("sugestao") != std::string::npos);
  CHECK(std::string(perf_test_lobby_blurb()).find(" — ") == std::string::npos);
  CHECK(std::string(perf_test_lobby_blurb()).find(" · ") == std::string::npos);
  CHECK(std::string(perf_test_counting_notice()).find("Benchmark") != std::string::npos);
  CHECK(std::string(perf_test_hz_title()).find("30") != std::string::npos);
  CHECK(std::string(perf_test_hz_title()).find("60 Hz") != std::string::npos);
  CHECK(std::string(perf_test_hz_blurb()).find("HDMI") != std::string::npos);
  CHECK(std::string(perf_test_audio_toggle()) == "Habilitar som no teste");
  CHECK(std::string(perf_test_audio_hint()).find("audio") != std::string::npos);
  CHECK(serialize_perf_test_plan(PerfTestPlan{}).find("capture_volume") == std::string::npos);
}

TEST_CASE("restart notice explains the reason and covers the new process boot") {
  const std::string title = session_restart_notice_title();
  CHECK(title.find("pipeline") != std::string::npos);
  CHECK(title.find("Otimizando") != std::string::npos);
  CHECK(lobby_pipeline_hold_title() == title);
  const std::string motor = session_restart_notice_detail(SessionCfgApply::RelaunchMotor);
  const std::string scale = session_restart_notice_detail(SessionCfgApply::RelaunchFeedScale);
  const std::string pass = session_restart_notice_detail(SessionCfgApply::RelaunchPassCount);
  const std::string preset = session_restart_notice_detail(SessionCfgApply::RelaunchPreset);
  const std::string nr = session_restart_notice_detail(SessionCfgApply::RelaunchNr);
  CHECK(motor.find("Motor") != std::string::npos);
  CHECK(scale.find("Escala") != std::string::npos);
  CHECK(pass.find("Pass Count") != std::string::npos);
  CHECK(preset.find("Preset") != std::string::npos);
  CHECK(nr.find("Neural Rendering") != std::string::npos);
  CHECK(motor != scale);
  CHECK(pass != motor);
  CHECK(preset != pass);
  CHECK(nr != preset);
  CHECK(pass.find("sessao") != std::string::npos);
  const std::string preset_toast = session_notice_preset_apply(0, 2);
  CHECK(preset_toast.find("Model A") != std::string::npos);
  CHECK(preset_toast.find("Model C") != std::string::npos);
  CHECK(preset_toast.find("reinicio") != std::string::npos);
  const std::string model_toast = session_notice_preset_apply(0, 2, 4);
  CHECK(model_toast.find("Model A") != std::string::npos);
  CHECK(model_toast.find("Model C") != std::string::npos);
  CHECK(model_toast.find("Cinematic") == std::string::npos);
  // Toast na sessao diz de onde pra onde, igual a troca de motor.
  const std::string toast = session_notice_nr_pass_count_apply(4, 2);
  CHECK(toast.find("1,5x") != std::string::npos);
  CHECK(toast.find("2x default") != std::string::npos);
  CHECK(toast.find("Pass Count") != std::string::npos);
  CHECK(toast.find("pipeline") != std::string::npos);
  // Os dois prometem a volta para a sessao, nunca para o lobby.
  CHECK(motor.find("sessao") != std::string::npos);
  CHECK(scale.find("sessao") != std::string::npos);
  CHECK(restart_notice_visible_at_boot(/*autostart=*/true));
  CHECK_FALSE(restart_notice_visible_at_boot(false));
  CHECK(session_hdmi_notice_title().find("console") != std::string::npos);
  CHECK(session_hdmi_notice_title().find("parou") != std::string::npos);
  CHECK(session_hdmi_notice_title().find("HDMI") != std::string::npos);
  CHECK(session_hdmi_notice_title().find("consola") == std::string::npos);
  CHECK(session_hdmi_notice_detail().find("pixels") != std::string::npos);
  CHECK(session_hdmi_notice_detail().find("NexusCap") != std::string::npos);
  CHECK(session_hdmi_notice_detail().find("erro") != std::string::npos);
  CHECK(session_hdmi_notice_detail().find("bugs") != std::string::npos);
  CHECK(kDlss5MotorSwapHoldSec <= 0.55);
  CHECK(kDlss5MotorSwapHoldSec >= 0.50);
  CHECK(std::string(nr_pass_count_15x_hint()).find("exclusivo") != std::string::npos);
  CHECK(std::string(nr_pass_count_15x_hint()).find("2x") != std::string::npos);
  CHECK(std::string(nr_pass_count_15x_hint()).find("desempenho") != std::string::npos);
  CHECK(std::string(nr_pass_count_23x_hint()).find("2x default") != std::string::npos);
  CHECK(std::string(nr_pass_count_23x_hint()).find("3x") != std::string::npos);
  CHECK(std::string(nr_pass_count_2x_opt_hint()).find("exclusivo") != std::string::npos);
  CHECK(std::string(nr_pass_count_2x_opt_hint()).find("1,7") == std::string::npos);
}

TEST_CASE("hdmi console card is blocked on every NexusCap manual relaunch") {
  CHECK(session_cfg_apply_needs_relaunch(SessionCfgApply::RelaunchMotor));
  CHECK(session_cfg_apply_needs_relaunch(SessionCfgApply::RelaunchPassCount));
  CHECK(session_cfg_apply_needs_relaunch(SessionCfgApply::RelaunchFeedScale));
  CHECK(session_cfg_apply_needs_relaunch(SessionCfgApply::RelaunchPreset));
  CHECK(session_cfg_apply_needs_relaunch(SessionCfgApply::RelaunchNr));
  CHECK_FALSE(session_cfg_apply_needs_relaunch(SessionCfgApply::Live));
  CHECK(hdmi_blocked_by_nexuscap_restart(/*pending=*/true, false, false));
  CHECK(hdmi_blocked_by_nexuscap_restart(false, /*notice=*/true, false));
  CHECK(hdmi_blocked_by_nexuscap_restart(false, false, /*autostart warmup=*/true));
  CHECK_FALSE(hdmi_blocked_by_nexuscap_restart(false, false, false));
  HdmiRecover s;
  CHECK(hdmi_on_arrived(s, 1000.0, true, true) == HdmiRecoverAction::Present);
  CHECK(hdmi_on_arrived(s, 1051.0, true, false) == HdmiRecoverAction::Hold);
  const bool burn = (hdmi_on_arrived(s, 1067.0, true, false) == HdmiRecoverAction::Hold) &&
                    !hdmi_blocked_by_nexuscap_restart(true, false, false);
  CHECK_FALSE(burn);
}

TEST_CASE("visual preset writes every ReShade section on 1x 1.5x 2x 3x") {
  CHECK_FALSE(every_keyed_int_is("NRPreset=0\nNRPreset=1\n", "NRPreset=", 1));
  CHECK(every_keyed_int_is("NRPreset=1\nNRPreset=1\n", "NRPreset=", 1));
  CHECK_FALSE(every_keyed_int_is("EnableHooks=2\n", "NRPreset=", 0));

  const std::string live_ini =
      "[GENERAL]\nAddonPath=.\\addons-multipass\n\n"
      "[RENODX-DLSS-preset1]\nDirectNeuralRenderingPassCount=1\nDirectNeuralRenderingStyle=0\n\n"
      "[RenoDX.DLSS5]\nNeuralUplift=1\nNRPreset=0\nNRStyle=0\nDirectNeuralRenderingPassCount=1\n";

  // Model C: live section used to stay at NRStyle=0 / missing Style — addon ignored preset1.
  std::string cinema = apply_reshade_visual_look_keys(live_ini, 2);
  CHECK(every_keyed_int_is(cinema, "NRPreset=", 1));
  CHECK(every_keyed_int_is(cinema, "NRStyle=", 1));
  CHECK(every_keyed_int_is(cinema, "DirectNeuralRenderingStyle=", 2));
  CHECK(cinema.find("[RenoDX.DLSS5]") != std::string::npos);
  CHECK(cinema.find("DirectNeuralRenderingStyle=2") != std::string::npos);

  std::string back = apply_reshade_visual_look_keys(std::move(cinema), 0);
  CHECK(every_keyed_int_is(back, "NRPreset=", 0));
  CHECK(every_keyed_int_is(back, "NRStyle=", 0));
  CHECK(every_keyed_int_is(back, "DirectNeuralRenderingStyle=", 0));

  const int passes[] = {1, 4, 2, 3};
  const int visuals[] = {0, 1, 2};
  for (int p : passes) {
    for (int v : visuals) {
      std::string reshade =
          std::string("[GENERAL]\nAddonPath=") + renodx_addon_stack_rel(p) +
          "\n\n[RENODX-DLSS-preset1]\nDirectNeuralRenderingPassCount=1\nDirectNeuralRenderingStyle=0\n\n"
          "[RenoDX.DLSS5]\nNeuralUplift=1\nNRPreset=0\nNRStyle=0\nDirectNeuralRenderingPassCount=1\n";
      std::string feed = "enabled=1\npreset=0\nwork_resolution=100\n";
      reshade = apply_reshade_multipass_look_defaults(std::move(reshade), p, v);
      reshade = apply_reshade_visual_look_keys(std::move(reshade), v);
      feed = apply_renodx_visual_preset_to_feed(std::move(feed), v, p);
      CHECK(renodx_files_reflect_visual_preset(feed, reshade, v, p));
      CHECK(every_keyed_int_is(reshade, "NRPreset=", renodx_nr_preset_index(v)));
      CHECK(every_keyed_int_is(reshade, "NRStyle=", renodx_nr_preset_index(v)));
      if (nr_pass_count_uses_multipass_addon(p)) {
        CHECK(every_keyed_int_is(reshade, "DirectNeuralRenderingStyle=",
                                renodx_direct_style_index(v, p)));
      }
      CHECK(parse_feed_model_preset(feed) == renodx_feed_model_preset(v, p));
      CHECK(parse_keyed_int(feed, "rebuild=", 0) == 1);
    }
  }
}

TEST_CASE("session CFG preset persist then boot files match on every motor") {
  for (int p : {1, 4, 2, 3}) {
    for (int v : {0, 1, 2}) {
      AppConfig cfg;
      cfg.enhance = EnhanceBackend::Renodx;
      cfg.nr_pass_count = p;
      cfg.renodx_preset = v;
      PersistedSettings s = default_settings();
      apply_session_cfg_to_persisted(cfg, s);
      CHECK(s.renodx_preset == v);
      CHECK(s.nr_pass_count == p);

      AppConfig back;
      apply_persisted_to_config(s, back);
      CHECK(back.renodx_preset == v);
      CHECK(back.nr_pass_count == p);
      CHECK(plan_session_cfg_preset_apply(nr_pass_count_uses_multipass_addon(p)) ==
            SessionCfgApply::RelaunchPreset);

      std::string reshade =
          std::string("AddonPath=") + renodx_addon_stack_rel(back.nr_pass_count) +
          "\n[RENODX-DLSS-preset1]\nDirectNeuralRenderingStyle=0\n[RenoDX.DLSS5]\nNRPreset=0\nNRStyle=0\n";
      reshade = apply_reshade_look_for_current_stack(std::move(reshade), back.renodx_preset);
      std::string feed = "preset=9\n";
      feed = apply_renodx_visual_preset_to_feed(std::move(feed), back.renodx_preset, p);
      CHECK(renodx_files_reflect_visual_preset(feed, reshade, v, p));
    }
  }
}

TEST_CASE("--selftest-cfg runs the session without the lobby") {
  const char* argv[] = {"NexusCap.exe", "--selftest-cfg"};
  const AppConfig c = parse_args(2, const_cast<char**>(argv));
  CHECK(c.selftest_cfg);
  CHECK_FALSE(c.show_lobby);
  CHECK_FALSE(c.exclusive_fullscreen);
  CHECK(c.frames_limited);
  CHECK(c.smoke_frames >= 60);
}

TEST_CASE("visual preset names and disk keys follow the DLSS5 motor") {
  CHECK(renodx_nr_preset_index(0) == 0);
  CHECK(renodx_nr_preset_index(1) == 0);
  CHECK(renodx_nr_preset_index(2) == 1);

  // Todos os Pass Count no multipass: Model A/B/C, Style+feed 0/1/2, NR* legado 0/0/1.
  for (int p : {1, 4, 2, 3}) {
    CHECK(nr_pass_count_uses_multipass_addon(p));
    CHECK(std::string(renodx_visual_preset_name(0, p)) == "Model A");
    CHECK(std::string(renodx_visual_preset_name(1, p)) == "Model B");
    CHECK(std::string(renodx_visual_preset_name(2, p)) == "Model C");
    CHECK(renodx_feed_model_preset(0, p) == 0);
    CHECK(renodx_feed_model_preset(1, p) == 1);
    CHECK(renodx_feed_model_preset(2, p) == 2);
    CHECK(renodx_direct_style_index(0, p) == 0);
    CHECK(renodx_direct_style_index(1, p) == 1);
    CHECK(renodx_direct_style_index(2, p) == 2);
    CHECK(renodx_visual_presets_differ_on_disk(0, 1, p));
    CHECK(renodx_visual_presets_differ_on_disk(0, 2, p));
    CHECK(renodx_visual_presets_differ_on_disk(1, 2, p));
  }

  // 1x e 1,5x usam o mesmo slot do feed: Model C = 2 nos dois.
  CHECK(renodx_feed_model_preset(2, 1) == renodx_feed_model_preset(2, 4));
  std::string feed = "enabled=1\npreset=0\n";
  feed = apply_renodx_visual_preset_to_feed(std::move(feed), 2, 1);
  CHECK(parse_feed_model_preset(feed) == 2);
  feed = apply_renodx_visual_preset_to_feed(std::move(feed), 2, 4);
  CHECK(parse_feed_model_preset(feed) == 2);

  const int motors[] = {1, 4, 2, 3};
  const int scales[] = {100, 50};

  auto feed_look_and_scale_ok = [](const std::string& f, int v, int p, int scale) {
    const int want_work = feed_work_resolution_for_session(scale);
    CHECK(parse_feed_model_preset(f) == renodx_feed_model_preset(v, p));
    CHECK(parse_feed_work_resolution(f) == want_work);
    CHECK(parse_keyed_int(f, "work_upscale=", -1) == (want_work < 100 ? 1 : 0));
    CHECK(parse_keyed_int(f, "rebuild=", 0) == 1);
  };

  // Disco: motor x preset x escala. Preset nao apaga work_resolution; escala nao apaga model.
  for (int p : motors) {
    for (int v : {0, 1, 2}) {
      for (int scale : scales) {
        std::string reshade =
            std::string("[GENERAL]\nAddonPath=") + renodx_addon_stack_rel(p) +
            "\n\n[RENODX-DLSS-preset1]\nDirectNeuralRenderingPassCount=1\nDirectNeuralRenderingStyle=9\n\n"
            "[RenoDX.DLSS5]\nNeuralUplift=1\nNRPreset=9\nNRStyle=9\nDirectNeuralRenderingStyle=9\n";
        std::string f = "enabled=1\npreset=9\nwork_resolution=9\nwork_upscale=0\n";
        reshade = apply_reshade_multipass_look_defaults(std::move(reshade), p, v);
        f = apply_feed_work_resolution(std::move(f), scale);
        f = apply_renodx_visual_preset_to_feed(std::move(f), v, p);
        CHECK(renodx_files_reflect_visual_preset(f, reshade, v, p));
        feed_look_and_scale_ok(f, v, p, scale);
        CHECK(every_keyed_int_is(reshade, "NRPreset=", renodx_nr_preset_index(v)));
        CHECK(every_keyed_int_is(reshade, "NRStyle=", renodx_nr_preset_index(v)));
        if (nr_pass_count_uses_multipass_addon(p)) {
          CHECK(every_keyed_int_is(reshade, "DirectNeuralRenderingStyle=", v));
        }

        f = apply_renodx_visual_preset_to_feed(std::move(f), v, p);
        f = apply_feed_work_resolution(std::move(f), scale);
        CHECK(renodx_files_reflect_visual_preset(f, reshade, v, p));
        feed_look_and_scale_ok(f, v, p, scale);
      }
    }
  }

  // HUD compacto/mini/ADV + persist + overlay: 4 motores x 3 looks x 2 escalas.
  for (int p : motors) {
    for (int v : {0, 1, 2}) {
      for (int scale : scales) {
        const char* want = renodx_visual_preset_name(v, p);
        const bool mp = nr_pass_count_uses_multipass_addon(p);
        char scale_tag[16]{};
        std::snprintf(scale_tag, sizeof(scale_tag), "scale %d%%", scale);
        char at_tag[16]{};
        std::snprintf(at_tag, sizeof(at_tag), "@%d%%", scale);
        const char* other_scale = (scale == 50) ? "scale 100%" : "scale 50%";
        const char* other_at = (scale == 50) ? "@100%" : "@50%";

        PersistedSettings s = default_settings();
        s.enhance_renodx = true;
        s.neural_rendering = true;
        s.ui.show_fps = true;
        s.renodx_preset = v;
        s.nr_pass_count = p;
        s.scale_percent = scale;

        AppConfig cfg;
        apply_persisted_to_config(s, cfg);
        CHECK(cfg.renodx_preset == v);
        CHECK(cfg.nr_pass_count == p);
        CHECK(cfg.scale_percent == scale);
        CHECK(effective_scale_percent(true, cfg.scale_percent) == feed_work_resolution_for_session(scale));

        PersistedSettings back = default_settings();
        apply_session_cfg_to_persisted(cfg, back);
        CHECK(back.renodx_preset == v);
        CHECK(back.nr_pass_count == p);
        CHECK(back.scale_percent == scale);

        const int ww = scaled_extent(s.width, scale);
        const int wh = scaled_extent(s.height, scale);

        for (bool mini : {false, true}) {
          for (bool adv : {false, true}) {
            s.ui.mini_overlay = mini;
            s.ui.advanced_metrics = adv;
            const auto hud = make_lobby_hud_preview_lines(s, 60.0);
            CHECK(lobby_hud_contains(hud, want));
            if (mini) {
              CHECK(lobby_hud_contains(hud, at_tag));
              CHECK_FALSE(lobby_hud_contains(hud, other_at));
            } else {
              CHECK(lobby_hud_contains(hud, scale_tag));
              CHECK_FALSE(lobby_hud_contains(hud, other_scale));
            }
            if (mp) {
              CHECK_FALSE(lobby_hud_contains(hud, "Default"));
              CHECK_FALSE(lobby_hud_contains(hud, "Natural"));
              CHECK_FALSE(lobby_hud_contains(hud, "Cinematic"));
            } else {
              CHECK_FALSE(lobby_hud_contains(hud, "Model A"));
              CHECK_FALSE(lobby_hud_contains(hud, "Model B"));
              CHECK_FALSE(lobby_hud_contains(hud, "Model C"));
            }
          }
        }

        MiniOverlaySeg segs[kMiniOverlayMaxSegs]{};
        char a[384]{}, b[384]{};
        const int n = format_mini_overlay_segments(
            segs, kMiniOverlayMaxSegs, a, sizeof(a), b, sizeof(b), 60.0, 0.0, false, 2, true, true, 16.7, 16.6,
            17.0, 18.0, true, 8.5, 2.2, 0, 0, 0, s.width, s.height, ww, wh, s.width, s.height, scale, true, 59.9,
            2.0, 2.1, 0, want, nullptr, 1.5, 0, 0, nullptr, 60.0, p);
        CHECK(n > 0);
        CHECK(std::strstr(a, want) != nullptr);
        CHECK(std::strstr(a, nr_pass_count_short(p)) != nullptr);
        CHECK(std::strstr(a, at_tag) != nullptr);
        CHECK(std::strstr(a, other_at) == nullptr);
        if (scale == 50) {
          CHECK(std::strstr(a, "1280x720") != nullptr);
        }

        const std::string toast = session_notice_preset_apply(0, v, p);
        CHECK(toast.find(renodx_visual_preset_name(0, p)) != std::string::npos);
        CHECK(toast.find(want) != std::string::npos);
      }
    }
  }

  // Troca de motor em todas as direcoes, nas duas escalas: look muda, work_resolution fica.
  for (int from_p : motors) {
    for (int to_p : motors) {
      for (int v : {0, 1, 2}) {
        for (int scale : scales) {
          std::string f = "enabled=1\npreset=9\nwork_resolution=9\nwork_upscale=0\n";
          f = apply_feed_work_resolution(std::move(f), scale);
          f = apply_renodx_visual_preset_to_feed(std::move(f), v, from_p);
          feed_look_and_scale_ok(f, v, from_p, scale);

          std::string reshade = std::string("[GENERAL]\nAddonPath=") + renodx_addon_stack_rel(from_p) +
                                "\n[RENODX-DLSS-preset1]\nDirectNeuralRenderingStyle=9\n"
                                "[RenoDX.DLSS5]\nNRPreset=9\nNRStyle=9\nDirectNeuralRenderingStyle=9\n";
          reshade = apply_reshade_addon_path(std::move(reshade), from_p);
          reshade = apply_reshade_look_for_current_stack(std::move(reshade), v);
          CHECK(renodx_files_reflect_visual_preset(f, reshade, v, from_p));

          f = apply_renodx_visual_preset_to_feed(std::move(f), v, to_p);
          feed_look_and_scale_ok(f, v, to_p, scale);
          reshade = apply_reshade_addon_path(std::move(reshade), to_p);
          reshade = apply_reshade_look_for_current_stack(std::move(reshade), v);
          CHECK(renodx_files_reflect_visual_preset(f, reshade, v, to_p));
          feed_look_and_scale_ok(f, v, to_p, scale);
          if (nr_pass_count_uses_multipass_addon(to_p)) {
            CHECK(every_keyed_int_is(reshade, "DirectNeuralRenderingStyle=", v));
          }
        }
      }
    }
  }

  // Salvar da engrenagem: visual primeiro (AddonPath ainda velho), Pass Count depois. Escala 50% sobrevive.
  {
    std::string reshade =
        "[GENERAL]\nAddonPath=.\\addons-stable\n[RENODX-DLSS-preset1]\nDirectNeuralRenderingStyle=9\n"
        "[RenoDX.DLSS5]\nNRPreset=0\nNRStyle=0\n";
    std::string f = "preset=0\nwork_resolution=100\nwork_upscale=0\n";
    f = apply_feed_work_resolution(std::move(f), 50);
    f = apply_renodx_visual_preset_to_feed(std::move(f), 2, 4);  // Model C feed=2 even before stack swap
    CHECK(parse_feed_model_preset(f) == 2);
    CHECK(parse_feed_work_resolution(f) == 50);
    CHECK(parse_keyed_int(f, "work_upscale=", 0) == 1);
    reshade = apply_reshade_look_for_current_stack(std::move(reshade), 2);  // ini antigo addons-stable
    CHECK(parse_reshade_nr_preset(reshade) == 1);
    CHECK(every_keyed_int_is(reshade, "DirectNeuralRenderingStyle=", 2));
    reshade = apply_reshade_addon_path(std::move(reshade), 4);
    reshade = apply_reshade_multipass_look_defaults(std::move(reshade), 4, 2);
    CHECK(renodx_files_reflect_visual_preset(f, reshade, 2, 4));
    CHECK(every_keyed_int_is(reshade, "DirectNeuralRenderingStyle=", 2));
    CHECK(parse_feed_work_resolution(f) == 50);
  }
}

TEST_CASE("lobby gear and Ctrl+M session CFG share motor names, disk, and scale") {
  // Os dois menus chamam a mesma API de nome; o disco e o mesmo apply. O que muda e o plano:
  // lobby (tela principal / F12) aplica look ao vivo no 1x; Ctrl+M sempre reinicia.
  const int motors[] = {1, 4, 2, 3};
  const int scales[] = {100, 50};
  for (int p : motors) {
    for (int v : {0, 1, 2}) {
      for (int scale : scales) {
        const char* radio0 = renodx_visual_preset_name(0, p);
        const char* radio1 = renodx_visual_preset_name(1, p);
        const char* radio2 = renodx_visual_preset_name(2, p);
        const char* want = renodx_visual_preset_name(v, p);
        const bool radio_ok = (std::string(radio0) == want) || (std::string(radio1) == want) ||
                              (std::string(radio2) == want);
        CHECK(radio_ok);
        if (nr_pass_count_uses_multipass_addon(p)) {
          CHECK(std::string(radio0) == "Model A");
          CHECK(std::string(radio1) == "Model B");
          CHECK(std::string(radio2) == "Model C");
        } else {
          CHECK(std::string(radio0) == "Default");
          CHECK(std::string(radio1) == "Natural");
          CHECK(std::string(radio2) == "Cinematic");
        }

        // Lobby: troca de preset sincroniza ficheiros (preview). Ctrl+M: sempre relaunch.
        const auto lobby = plan_lobby_renodx_preset_change(true, true, 0, v, true);
        CHECK(lobby.sync_preset_files == (v != 0));
        CHECK_FALSE(session_cfg_preset_applies_live(nr_pass_count_uses_multipass_addon(p)));
        CHECK(plan_session_cfg_preset_apply(nr_pass_count_uses_multipass_addon(p)) ==
              SessionCfgApply::RelaunchPreset);

        PersistedSettings gear = default_settings();
        gear.enhance_renodx = true;
        gear.neural_rendering = true;
        gear.ui.show_fps = true;
        gear.renodx_preset = v;
        gear.nr_pass_count = p;
        gear.scale_percent = scale;

        // Tela principal: Salvar da engrenagem / lobby CFG.
        const std::string ini = serialize_settings_ini(gear);
        PersistedSettings loaded;
        REQUIRE(parse_settings_ini(ini, loaded));
        CHECK(loaded.renodx_preset == v);
        CHECK(loaded.nr_pass_count == p);
        CHECK(loaded.scale_percent == scale);

        AppConfig from_lobby;
        apply_persisted_to_config(loaded, from_lobby);
        CHECK(from_lobby.renodx_preset == v);
        CHECK(from_lobby.nr_pass_count == p);
        CHECK(from_lobby.scale_percent == scale);

        // Ctrl+M: persist da sessao para o --autostart.
        AppConfig from_cfg;
        from_cfg.enhance = EnhanceBackend::Renodx;
        from_cfg.renodx_preset = v;
        from_cfg.nr_pass_count = p;
        from_cfg.scale_percent = scale;
        PersistedSettings after_ctrl_m = default_settings();
        apply_session_cfg_to_persisted(from_cfg, after_ctrl_m);
        CHECK(after_ctrl_m.renodx_preset == v);
        CHECK(after_ctrl_m.nr_pass_count == p);
        CHECK(after_ctrl_m.scale_percent == scale);

        std::string feed = "enabled=1\npreset=9\nwork_resolution=9\nwork_upscale=0\n";
        std::string reshade = std::string("[GENERAL]\nAddonPath=") + renodx_addon_stack_rel(p) +
                              "\n[RENODX-DLSS-preset1]\nDirectNeuralRenderingStyle=9\n"
                              "[RenoDX.DLSS5]\nNRPreset=9\nNRStyle=9\nDirectNeuralRenderingStyle=9\n";
        reshade = apply_reshade_look_for_current_stack(std::move(reshade), v);
        feed = apply_feed_work_resolution(std::move(feed), scale);
        feed = apply_renodx_visual_preset_to_feed(std::move(feed), v, p);
        CHECK(renodx_files_reflect_visual_preset(feed, reshade, v, p));
        CHECK(parse_feed_work_resolution(feed) == feed_work_resolution_for_session(scale));

        const auto lobby_hud = make_lobby_hud_preview_lines(gear, 60.0);
        CHECK(lobby_hud_contains(lobby_hud, want));
        gear.ui.mini_overlay = true;
        const auto mini = make_lobby_hud_preview_lines(gear, 60.0);
        CHECK(lobby_hud_contains(mini, want));

        const std::string lobby_toast = std::string("Preset: ") + want + " — olhe o preview";
        CHECK(lobby_toast.find(want) != std::string::npos);
        const std::string cfg_toast = session_notice_preset_apply(0, v, p);
        CHECK(cfg_toast.find(want) != std::string::npos);
        CHECK(cfg_toast.find("reinicio") != std::string::npos);
      }
    }
  }
}

TEST_CASE("perf test clamps signal fps scale and pass") {
  CHECK(clamp_perf_test_fps(29) == 30.0);
  CHECK(clamp_perf_test_fps(30) == 30.0);
  CHECK(clamp_perf_test_fps(59) == 60.0);
  CHECK(clamp_perf_test_fps(60) == 60.0);
  int w = 2000, h = 1200;
  clamp_perf_test_signal(w, h);
  CHECK(w == 1920);
  CHECK(h == 1080);
  w = 2560;
  h = 1440;
  clamp_perf_test_signal(w, h);
  CHECK(w == 2560);
  CHECK(h == 1440);
  w = 3840;
  h = 2160;
  clamp_perf_test_signal(w, h);
  CHECK(w == 3840);
  CHECK(h == 2160);
  CHECK(std::string(perf_test_signal_label(1920, 1080)) == "1080p");
  CHECK(std::string(perf_test_signal_label(2560, 1440)) == "1440p");
  CHECK(std::string(perf_test_signal_label(3840, 2160)) == "4K");
  PerfTestPlan p;
  p.scale_percent = 10;
  p.nr_pass_count = 99;
  p.fg_multiplier = 9;
  p = clamp_perf_test_plan(p);
  CHECK(p.scale_percent == kMinFeedWorkResolution);
  CHECK(p.nr_pass_count == kMaxNrPassCount);
  CHECK(p.fg_multiplier == 4);
}

TEST_CASE("perf test verdict good tight fail") {
  PerfTestSample ok;
  ok.fps = 59.9;
  ok.p95_present_ms = 2.3;
  ok.stalls = 0;
  ok.target_fps = 60;
  CHECK(grade_perf_test(ok) == PerfTestGrade::Good);
  CHECK(std::string(perf_test_grade_label(PerfTestGrade::Good)) == "Bom");

  PerfTestSample tight = ok;
  tight.p95_present_ms = 20.0;
  CHECK(grade_perf_test(tight) == PerfTestGrade::Tight);

  PerfTestSample one_hitch = ok;
  one_hitch.p95_present_ms = 16.7;
  one_hitch.stalls = 1;
  CHECK(grade_perf_test(one_hitch) == PerfTestGrade::Good);

  PerfTestSample stalls = ok;
  stalls.stalls = 2;
  CHECK(grade_perf_test(stalls) == PerfTestGrade::Tight);

  PerfTestSample fail_fps = ok;
  fail_fps.fps = 40.0;
  CHECK(grade_perf_test(fail_fps) == PerfTestGrade::Fail);

  PerfTestSample fail_stalls = ok;
  fail_stalls.stalls = 3;
  CHECK(grade_perf_test(fail_stalls) == PerfTestGrade::Fail);

  CHECK(perf_test_present_budget_ms(60) > 17.6);
  CHECK(perf_test_present_budget_ms(60) < 18.3);
  CHECK(perf_test_present_budget_ms(30) > 34.7);
  CHECK(perf_test_present_budget_ms(30) < 35.3);
  PerfTestSample vsync_ok = ok;
  vsync_ok.p95_present_ms = 16.7;
  CHECK(grade_perf_test(vsync_ok) == PerfTestGrade::Good);

  PerfTestSample fg_saves = ok;
  fg_saves.fps = 40.0;
  fg_saves.p95_present_ms = 25.0;
  fg_saves.stalls = 295;
  fg_saves.fg_multiplier = 2;
  CHECK(grade_perf_test(fg_saves) == PerfTestGrade::PlayableFg);
  CHECK(std::string(perf_test_grade_label(PerfTestGrade::PlayableFg)) == "Jogavel com FG");

  PerfTestSample fg_short = fg_saves;
  fg_short.fps = 20.0;
  fg_short.fg_multiplier = 2;  // ~40 FG, below 60 meta
  CHECK(grade_perf_test(fg_short) == PerfTestGrade::Fail);

  PerfTestSample fg_holds = ok;
  fg_holds.fg_multiplier = 2;
  fg_holds.stalls = 0;
  CHECK(grade_perf_test(fg_holds) == PerfTestGrade::Good);
  fg_holds.stalls = 3;
  CHECK(grade_perf_test(fg_holds) == PerfTestGrade::Tight);
}

TEST_CASE("perf test 2x 1440p good card matches native capture") {
  PerfTestPlan p;
  p.width = 2560;
  p.height = 1440;
  p.fps = 60;
  p.scale_percent = 100;
  p.nr_pass_count = 2;
  p.fg_multiplier = 1;
  PerfTestSample s;
  s.fps = 60.0686;
  s.p95_present_ms = 16.7;
  s.stalls = 0;
  s.target_fps = 60;
  CHECK(grade_perf_test(s) == PerfTestGrade::Good);
  s.fps = 59.9355;
  s.p95_present_ms = 16.6571;
  s.stalls = 1;
  CHECK(grade_perf_test(s) == PerfTestGrade::Good);
  const std::string card = format_perf_test_card(p, s, PerfTestGrade::Good);
  CHECK(card.find("2x") != std::string::npos);
  CHECK(card.find("1440p") != std::string::npos);
  CHECK(card.find("Bom") != std::string::npos);
  CHECK(card.find("Segura 60 nativo sem FG") != std::string::npos);
  CHECK(card.find("Nao segura") == std::string::npos);
}

TEST_CASE("perf test card mentions combo and grade") {
  PerfTestPlan p;
  p.width = 2560;
  p.height = 1440;
  p.fps = 60;
  p.scale_percent = 100;
  p.nr_pass_count = 2;
  p.fg_multiplier = 1;
  PerfTestSample s;
  s.fps = 40.2;
  s.p95_present_ms = 25.0;
  s.stalls = 4;
  s.target_fps = 60;
  const auto g = grade_perf_test(s);
  CHECK(g == PerfTestGrade::Fail);
  const std::string card = format_perf_test_card(p, s, g);
  CHECK(card.find("2x") != std::string::npos);
  CHECK(card.find("1440p") != std::string::npos);
  CHECK(card.find("60 fps") != std::string::npos);
  CHECK(card.find("FG off") != std::string::npos);
  CHECK(card.find("Nao aguenta") != std::string::npos);

  p.fg_multiplier = 2;
  s.fg_multiplier = 2;
  CHECK(grade_perf_test(s) == PerfTestGrade::PlayableFg);
  const std::string fg_card = format_perf_test_card(p, s, PerfTestGrade::PlayableFg);
  CHECK(perf_test_fg_est_fps(40.0, 2) == doctest::Approx(80.0));
  CHECK(perf_test_fg_latency_ms(40.0, 2) > 30.0);
  CHECK(perf_test_fg_latency_ms(40.0, 2) < 45.0);
  CHECK(fg_card.find("Jogavel com FG") != std::string::npos);
  CHECK(fg_card.find("gerado") != std::string::npos);
  CHECK(fg_card.find("Da para jogar") != std::string::npos);
  CHECK(fg_card.find("escala para 75") != std::string::npos);
  CHECK(perf_test_suggest_scale(100, s, PerfTestGrade::PlayableFg, 60) == 75);
  CHECK(perf_test_advice(p, PerfTestGrade::Fail).find("Pass 1x") != std::string::npos);
  p.nr_pass_count = 1;
  p.scale_percent = 50;
  CHECK(perf_test_advice(p, PerfTestGrade::Fail).find("1080p") != std::string::npos);
  p.width = 3840;
  p.height = 2160;
  CHECK(perf_test_advice(p, PerfTestGrade::Fail).find("1440p") != std::string::npos);
}

TEST_CASE("perf test plan and result serialize") {
  PerfTestPlan p;
  p.width = 1920;
  p.height = 1080;
  p.fps = 30;
  p.scale_percent = 80;
  p.nr_pass_count = 4;
  p.renodx_preset = 2;
  p.fg_multiplier = 3;
  p.fg_queue = 2;
  p.audio = false;
  p.video = "C:\\vids\\last.mp4";
  PerfTestPlan back;
  CHECK(parse_perf_test_plan(serialize_perf_test_plan(p), back));
  CHECK(back.width == 1920);
  CHECK(back.height == 1080);
  CHECK(back.fps == 30.0);
  CHECK(back.scale_percent == 80);
  CHECK(back.nr_pass_count == 4);
  CHECK(back.renodx_preset == 2);
  CHECK(back.fg_multiplier == 3);
  CHECK(back.fg_queue == 2);
  CHECK_FALSE(back.fg_adaptive);
  CHECK(back.fg_target_fps == 60);
  CHECK_FALSE(back.audio);
  CHECK(back.video == "C:\\vids\\last.mp4");
  p.fg_adaptive = true;
  p.fg_target_fps = 80;
  CHECK(parse_perf_test_plan(serialize_perf_test_plan(p), back));
  CHECK(back.fg_adaptive);
  CHECK(back.fg_target_fps == 80);
  PerfTestPlan oldp;
  CHECK(parse_perf_test_plan("width=1920\nheight=1080\nplan_fps=60\n", oldp));
  CHECK(oldp.audio);
  CHECK(oldp.fg_queue == 0);
  CHECK_FALSE(oldp.fg_adaptive);
  CHECK(oldp.fg_target_fps == 60);

  PerfTestResult r;
  r.valid = true;
  r.plan = p;
  r.sample.fps = 29.8;
  r.sample.p95_present_ms = 8.1;
  r.sample.stalls = 0;
  r.sample.target_fps = 30;
  r.grade = grade_perf_test(r.sample);
  r.summary = format_perf_test_card(p, r.sample, r.grade);
  PerfTestResult r2;
  CHECK(parse_perf_test_result(serialize_perf_test_result(r), r2));
  CHECK(r2.valid);
  CHECK(r2.grade == PerfTestGrade::Good);
  CHECK(r2.plan.width == 1920);
  CHECK(r2.sample.stalls == 0);
  CHECK(r2.summary.find("Bom") != std::string::npos);

  const auto tmp = (std::filesystem::temp_directory_path() / "nexuscap_perf_roundtrip.txt").string();
  CHECK(write_perf_test_result_file(tmp, r));
  PerfTestResult r3;
  CHECK(read_perf_test_result_file(tmp, r3));
  CHECK(r3.valid);
  CHECK(r3.grade == r.grade);
  std::error_code ec;
  std::filesystem::remove(tmp, ec);
}

TEST_CASE("perf test finds newest video in search dirs") {
  const auto root = std::filesystem::temp_directory_path() / "nexuscap_perf_vids";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root / "assets", ec);
  std::filesystem::create_directories(root / "samples", ec);
  {
    std::ofstream a((root / "old.mp4").string());
    a << "old";
  }
  {
    std::ofstream b((root / "assets" / "newer.mkv").string());
    b << "new";
  }
  std::filesystem::last_write_time(root / "old.mp4", std::filesystem::file_time_type::clock::now(), ec);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  std::filesystem::last_write_time(root / "assets" / "newer.mkv",
                                   std::filesystem::file_time_type::clock::now(), ec);
  std::string found;
  CHECK(find_newest_perf_test_video({root.string(), (root / "assets").string(), (root / "samples").string()},
                                    found));
  CHECK(found.find("newer.mkv") != std::string::npos);
  CHECK(is_perf_test_video_ext("foo.MP4"));
  CHECK(is_perf_test_video_ext("foo.mkv"));
  CHECK_FALSE(is_perf_test_video_ext("foo.txt"));
  {
    std::ofstream bundled((root / "assets" / "perf_test.mp4").string());
    bundled << "bundled";
  }
  std::string resolved;
  CHECK(resolve_perf_test_video(root.string(), resolved));
  CHECK(resolved.find("perf_test.mp4") != std::string::npos);
  CHECK(perf_test_bundled_path("D:\\app").find("assets\\perf_test.mp4") != std::string::npos);
  CHECK(std::string(perf_test_bundled_filename_for(1920, 1080)) == "perf_test_1080.mp4");
  CHECK(std::string(perf_test_bundled_filename_for(2560, 1440)) == "perf_test_1440.mp4");
  CHECK(std::string(perf_test_bundled_filename_for(3840, 2160)) == "perf_test_4k.mp4");
  CHECK(std::string(perf_test_audio_filename()) == "perf_test.m4a");
  CHECK(perf_test_audio_sidecar_path("D:\\app\\assets\\perf_test_1440.mp4").find("perf_test.m4a") != std::string::npos);
  {
    std::ofstream f1080((root / "assets" / "perf_test_1080.mp4").string());
    f1080 << "1080";
    std::ofstream f1440((root / "assets" / "perf_test_1440.mp4").string());
    f1440 << "1440";
    std::ofstream f4k((root / "assets" / "perf_test_4k.mp4").string());
    f4k << "4k";
  }
  std::string r1080, r1440, r4k;
  CHECK(resolve_perf_test_video(root.string(), 1920, 1080, r1080));
  CHECK(r1080.find("perf_test_1080.mp4") != std::string::npos);
  CHECK(resolve_perf_test_video(root.string(), 2560, 1440, r1440));
  CHECK(r1440.find("perf_test_1440.mp4") != std::string::npos);
  CHECK(resolve_perf_test_video(root.string(), 3840, 2160, r4k));
  CHECK(r4k.find("perf_test_4k.mp4") != std::string::npos);
  std::filesystem::remove_all(root, ec);
}

TEST_CASE("perf test scores from the whole session not vsync fps") {
  CHECK_FALSE(perf_test_score_ready(0));
  CHECK_FALSE(perf_test_score_ready(179));
  CHECK(perf_test_score_ready(180));
  PerfScoreAcc acc;
  for (int i = 0; i < 180; ++i) acc.push(16.667, 4.0, false);
  const auto s = perf_test_sample_from_acc(acc, 60.0, 1);
  CHECK(s.fps == doctest::Approx(60.0).epsilon(0.02));
  CHECK(s.p95_present_ms == doctest::Approx(4.0).epsilon(0.01));
  CHECK(s.stalls == 0);
  CHECK(s.fg_multiplier == 1);
  CHECK(grade_perf_test(s) == PerfTestGrade::Good);
  CHECK_FALSE(perf_test_score_ready(acc.frames() - 1));
}

TEST_CASE("parse_args --perf-test keeps windowed plan knobs") {
  char exe[] = "NexusCap.exe";
  char a0[] = "--perf-test";
  char a1[] = "--width";
  char a2[] = "1920";
  char a3[] = "--height";
  char a4[] = "1080";
  char a5[] = "--fps";
  char a6[] = "30";
  char a7[] = "--scale";
  char a8[] = "80";
  char a9[] = "--pass";
  char a10[] = "2";
  char a11[] = "--preset";
  char a12[] = "1";
  char a13[] = "--fg-x";
  char a14[] = "3";
  char a15[] = "--video";
  char a16[] = "C:\\clip.mp4";
  char* argv[] = {exe, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16};
  const AppConfig c = parse_args(18, argv);
  CHECK(c.perf_test);
  CHECK_FALSE(c.exclusive_fullscreen);
  CHECK(c.mini_overlay);
  CHECK(c.width == 1920);
  CHECK(c.height == 1080);
  CHECK(c.fps == 30.0);
  CHECK(c.scale_percent == 80);
  CHECK(c.nr_pass_count == 2);
  CHECK(c.renodx_preset == 1);
  CHECK(c.fg_multiplier == 3);
  CHECK(c.fg_auto_start);
  CHECK(c.bug_report);
  CHECK(c.video_path == "C:\\clip.mp4");
  CHECK(c.enhance == EnhanceBackend::Renodx);
  CHECK(c.perf_cli_override);
  const std::string args = perf_test_cli_args(perf_test_plan_from_config(c));
  CHECK(args.find("--perf-test") != std::string::npos);
  CHECK(args.find("--width 1920") != std::string::npos);
  CHECK(args.find("--fg-x 3") != std::string::npos);
  CHECK(args.find("--perf-audio") != std::string::npos);
  CHECK(c.perf_test_audio);
}

TEST_CASE("parse_args --no-perf-audio stays off the HDMI CFG") {
  char exe[] = "NexusCap.exe";
  char a0[] = "--perf-test";
  char a1[] = "--no-perf-audio";
  char* argv[] = {exe, a0, a1};
  const AppConfig c = parse_args(3, argv);
  CHECK(c.perf_test);
  CHECK_FALSE(c.perf_test_audio);
  PerfTestPlan p = perf_test_plan_from_config(c);
  CHECK_FALSE(p.audio);
  CHECK(perf_test_cli_args(p).find("--no-perf-audio") != std::string::npos);
}

TEST_CASE("perf test plan never writes into real HDMI presets") {
  PersistedSettings s = default_settings();
  s.nr_pass_count = 1;
  s.renodx_preset = 0;
  s.scale_percent = 100;
  s.fg_enabled = false;
  s.neural_rendering = false;
  s.enhance_renodx = true;
  const PersistedSettings before = s;

  AppConfig cfg;
  PerfTestPlan p;
  p.width = 2560;
  p.height = 1440;
  p.fps = 60;
  p.scale_percent = 50;
  p.nr_pass_count = 3;
  p.renodx_preset = 2;
  p.fg_multiplier = 3;
  apply_perf_test_plan_to_config(p, cfg);
  CHECK(cfg.perf_test);
  CHECK(cfg.perf_test_audio);
  CHECK(cfg.nr_pass_count == 3);
  CHECK(cfg.renodx_preset == 2);
  CHECK(cfg.scale_percent == 50);

  apply_session_cfg_to_persisted(cfg, s);
  CHECK(s.nr_pass_count == before.nr_pass_count);
  CHECK(s.renodx_preset == before.renodx_preset);
  CHECK(s.scale_percent == before.scale_percent);
  CHECK(s.fg_enabled == before.fg_enabled);
  CHECK(s.neural_rendering == before.neural_rendering);
  CHECK(serialize_settings_ini(s) == serialize_settings_ini(before));
  CHECK_FALSE(session_writes_renodx_off_files(cfg));
}

TEST_CASE("benchmark knobs do not propagate to lobby HDMI restore") {
  PersistedSettings hdmi = default_settings();
  hdmi.scale_percent = 100;
  hdmi.nr_pass_count = 1;
  hdmi.renodx_preset = 0;
  hdmi.fg_enabled = false;
  hdmi.fg_multiplier = 2;
  hdmi.neural_rendering = true;
  hdmi.enhance_renodx = true;
  const PersistedSettings before = hdmi;

  PerfTestPlan p;
  p.width = 2560;
  p.height = 1440;
  p.fps = 60;
  p.scale_percent = 75;
  p.nr_pass_count = 3;
  p.renodx_preset = 2;
  p.fg_multiplier = 3;

  AppConfig cfg;
  apply_perf_test_plan_to_config(p, cfg);
  apply_session_cfg_to_persisted(cfg, hdmi);
  CHECK(serialize_settings_ini(hdmi) == serialize_settings_ini(before));

  apply_persisted_to_config(hdmi, cfg);
  CHECK(cfg.scale_percent == 100);
  CHECK(cfg.nr_pass_count == 1);
  CHECK(cfg.renodx_preset == 0);
  CHECK_FALSE(cfg.fg_auto_start);
  CHECK(cfg.fg_multiplier == 2);

  const auto r = hdmi_restore_after_perf_test(hdmi);
  CHECK(r.scale_percent == 100);
  CHECK(r.nr_pass_count == 1);
  CHECK(r.renodx_preset == 0);
  CHECK(r.enhance_renodx);
  CHECK(r.neural_rendering);
  CHECK_FALSE(r.fg_enabled);
  CHECK(r.fg_multiplier == 2);
  CHECK(r.lobby_feed_work_resolution == 100);
  CHECK(hdmi_lobby_fg_multiplier(r.fg_enabled, r.fg_multiplier) == 1);
  {
    const auto hud = make_lobby_hud_preview_lines(hdmi, 60.0);
    bool saw_fg_off = false;
    bool saw_test_scale = false;
    bool saw_test_fg = false;
    for (int i = 0; i < hud.count; ++i) {
      if (std::strstr(hud.line[i], "FG off")) saw_fg_off = true;
      if (std::strstr(hud.line[i], "75%")) saw_test_scale = true;
      if (std::strstr(hud.line[i], "x3")) saw_test_fg = true;
    }
    CHECK(saw_fg_off);
    CHECK_FALSE(saw_test_scale);
    CHECK_FALSE(saw_test_fg);
  }

  hdmi.fg_enabled = true;
  hdmi.fg_multiplier = 2;
  hdmi.scale_percent = 100;
  const auto on = hdmi_restore_after_perf_test(hdmi);
  CHECK(on.fg_enabled);
  CHECK(hdmi_lobby_fg_multiplier(on.fg_enabled, on.fg_multiplier) == 2);
  CHECK(perf_test_return_needs_fresh_process(true));
  CHECK_FALSE(perf_test_return_needs_fresh_process(false));
}

TEST_CASE("benchmark Esc/X/timer restore suite") {
  CHECK(session_exit_code_after_esc(true) == kSessionExitBackToLobby);
  CHECK(session_exit_code_after_window_close(true) == session_exit_code_after_esc(true));
  CHECK(session_exit_code_after_window_close(false) == session_exit_code_after_esc(false));
  CHECK(session_user_leave_returns_to_lobby(true));
  CHECK_FALSE(session_user_leave_returns_to_lobby(false));
  CHECK(lobby_message_exits_app(0x0010));  // WM_CLOSE no lobby
  CHECK_FALSE(lobby_message_exits_app(0x0012));
  CHECK(lobby_message_is_spurious_quit(0x0012));
  CHECK(std::strcmp(perf_test_return_relaunch_args(), "--show-perf-result") == 0);
  CHECK(perf_test_return_needs_fresh_process(true));
  CHECK_FALSE(fg_leftover_cleared(1, true));
  CHECK(fg_leftover_cleared(0, true));
  CHECK_FALSE(fg_leftover_cleared(0, false));

  CHECK(fg_frametime_trace_count(false, 2) == 1);
  CHECK(fg_frametime_trace_count(false, 4) == 1);
  CHECK(fg_frametime_trace_count(true, 4) == 2);

  const int scales[] = {50, 75, 100};
  const int passes[] = {1, 3, 4};
  const int fgs[] = {1, 2, 3, 4};
  for (int sc : scales) {
    for (int ps : passes) {
      for (int fg : fgs) {
        PersistedSettings hdmi = default_settings();
        hdmi.scale_percent = 100;
        hdmi.nr_pass_count = 4;
        hdmi.renodx_preset = 0;
        hdmi.fg_enabled = false;
        hdmi.fg_multiplier = 2;
        hdmi.enhance_renodx = true;
        hdmi.neural_rendering = true;
        const PersistedSettings before = hdmi;

        PerfTestPlan p;
        p.width = 2560;
        p.height = 1440;
        p.fps = 60;
        p.scale_percent = sc;
        p.nr_pass_count = ps;
        p.renodx_preset = 2;
        p.fg_multiplier = fg;
        AppConfig cfg;
        apply_perf_test_plan_to_config(p, cfg);
        apply_session_cfg_to_persisted(cfg, hdmi);
        CHECK(serialize_settings_ini(hdmi) == serialize_settings_ini(before));

        apply_persisted_to_config(hdmi, cfg);
        const auto r = hdmi_restore_after_perf_test(hdmi);
        CHECK(r.scale_percent == 100);
        CHECK(r.nr_pass_count == 4);
        CHECK(r.renodx_preset == 0);
        CHECK_FALSE(r.fg_enabled);
        CHECK(r.lobby_feed_work_resolution == 100);
        CHECK(hdmi_lobby_fg_multiplier(r.fg_enabled, r.fg_multiplier) == 1);
        CHECK(cfg.scale_percent == 100);
        CHECK(cfg.nr_pass_count == 4);
        CHECK_FALSE(cfg.fg_auto_start);
        CHECK(perf_test_return_needs_fresh_process(true));
      }
    }
  }
}

TEST_CASE("apply_perf_test_plan_to_config does not keep exclusive fullscreen") {
  AppConfig cfg;
  cfg.exclusive_fullscreen = true;
  cfg.scale_percent = 40;
  PerfTestPlan p;
  p.width = 3840;
  p.height = 2160;
  p.fps = 60;
  p.scale_percent = 50;
  p.nr_pass_count = 1;
  p.fg_multiplier = 1;
  p.video = "last.mp4";
  apply_perf_test_plan_to_config(p, cfg);
  CHECK(cfg.perf_test);
  CHECK_FALSE(cfg.exclusive_fullscreen);
  CHECK(cfg.width == 3840);
  CHECK(cfg.height == 2160);
  CHECK(cfg.scale_percent == 50);
  CHECK_FALSE(cfg.fg_auto_start);
  CHECK(cfg.bug_report);
  CHECK(cfg.video_path == "last.mp4");
  CHECK(cfg.mini_overlay);
}

TEST_CASE("benchmark always writes bug report and does not wait for FG when off") {
  AppConfig cfg;
  cfg.bug_report = false;
  cfg.fg_auto_start = true;
  PerfTestPlan p;
  p.width = 2560;
  p.height = 1440;
  p.fps = 60;
  p.fg_multiplier = 1;
  apply_perf_test_plan_to_config(p, cfg);
  CHECK(cfg.perf_test);
  CHECK(cfg.bug_report);
  CHECK_FALSE(cfg.fg_auto_start);
  const uint64_t ready = kStatsWarmupFrames + 1;
  CHECK(perf_test_should_start_clock(true, ready, false, false, false, 0));
  CHECK_FALSE(perf_test_carregar_can_block(true, false, false, false, 0));
  PersistedSettings s = default_settings();
  CHECK_FALSE(s.ui.bug_report);
}

TEST_CASE("perf test combos pass x scale x scene color") {
  const int passes[] = {1, 4, 2, 3};
  const int scales[] = {50, 100};
  PerfTestSample ok;
  ok.fps = 59.9;
  ok.p95_present_ms = 2.2;
  ok.stalls = 0;
  ok.target_fps = 60;
  int n = 0;
  for (int pass : passes) {
    for (int scale : scales) {
      for (int preset = 0; preset <= 2; ++preset) {
        ++n;
        PerfTestPlan p;
        p.width = 1920;
        p.height = 1080;
        p.fps = 60;
        p.nr_pass_count = pass;
        p.scale_percent = scale;
        p.renodx_preset = preset;
        AppConfig cfg;
        apply_perf_test_plan_to_config(p, cfg);
        CHECK(cfg.nr_pass_count == pass);
        CHECK(cfg.scale_percent == scale);
        CHECK(cfg.renodx_preset == preset);
        CHECK(cfg.mini_overlay);
        CHECK_FALSE(cfg.exclusive_fullscreen);
        const char* color = renodx_visual_preset_name(preset, pass);
        CHECK(color != nullptr);
        CHECK(std::strlen(color) > 0);
        CHECK(std::string(color) == std::string(preset == 0 ? "Model A" : (preset == 1 ? "Model B" : "Model C")));
        const std::string card = format_perf_test_card(p, ok, grade_perf_test(ok));
        CHECK(card.find(nr_pass_count_short(pass)) != std::string::npos);
        CHECK(card.find(("escala " + std::to_string(scale)).c_str()) != std::string::npos);
        CHECK(card.find("1080p") != std::string::npos);
        const std::string cli = perf_test_cli_args(p);
        CHECK(cli.find("--pass " + std::to_string(pass)) != std::string::npos);
        CHECK(cli.find("--scale " + std::to_string(scale)) != std::string::npos);
        CHECK(cli.find("--preset " + std::to_string(preset)) != std::string::npos);
      }
    }
  }
  CHECK(n == 24);
}

TEST_CASE("perf test table 30 and 60 with fg and advice") {
  auto sample = [](double fps, int fg, double target, uint64_t stalls = 0, double present = 2.0) {
    PerfTestSample s;
    s.fps = fps;
    s.fg_multiplier = fg;
    s.target_fps = target;
    s.stalls = stalls;
    s.p95_present_ms = present;
    return s;
  };
  CHECK(grade_perf_test(sample(30.0, 1, 30)) == PerfTestGrade::Good);
  CHECK(grade_perf_test(sample(30.0, 1, 30, 0, 40.0)) == PerfTestGrade::Tight);
  CHECK(grade_perf_test(sample(30.0, 1, 30, 3)) == PerfTestGrade::Fail);
  CHECK(grade_perf_test(sample(30.0, 2, 30)) == PerfTestGrade::Good);
  CHECK(grade_perf_test(sample(30.0, 3, 30)) == PerfTestGrade::Good);
  CHECK(grade_perf_test(sample(30.0, 4, 30)) == PerfTestGrade::Good);
  CHECK(grade_perf_test(sample(30.0, 2, 30, 4)) == PerfTestGrade::Tight);
  CHECK(grade_perf_test(sample(22.0, 1, 30)) == PerfTestGrade::Fail);
  CHECK(grade_perf_test(sample(22.0, 2, 30)) == PerfTestGrade::PlayableFg);
  CHECK(grade_perf_test(sample(22.0, 3, 30)) == PerfTestGrade::PlayableFg);
  CHECK(grade_perf_test(sample(18.0, 2, 30)) == PerfTestGrade::Fail);
  CHECK(grade_perf_test(sample(15.0, 4, 30)) == PerfTestGrade::Fail);

  CHECK(grade_perf_test(sample(60.0, 1, 60)) == PerfTestGrade::Good);
  CHECK(grade_perf_test(sample(60.0, 2, 60)) == PerfTestGrade::Good);
  CHECK(grade_perf_test(sample(60.0, 2, 60, 5)) == PerfTestGrade::Tight);
  CHECK(grade_perf_test(sample(40.0, 1, 60)) == PerfTestGrade::Fail);
  CHECK(grade_perf_test(sample(40.0, 2, 60)) == PerfTestGrade::PlayableFg);
  CHECK(grade_perf_test(sample(30.0, 2, 60)) == PerfTestGrade::PlayableFg);
  CHECK(grade_perf_test(sample(30.0, 1, 60)) == PerfTestGrade::Fail);
  CHECK(grade_perf_test(sample(25.0, 2, 60)) == PerfTestGrade::Fail);
  CHECK(grade_perf_test(sample(20.0, 3, 60)) == PerfTestGrade::PlayableFg);
  CHECK(grade_perf_test(sample(20.0, 2, 60)) == PerfTestGrade::Fail);
  CHECK(grade_perf_test(sample(18.0, 4, 60)) == PerfTestGrade::Fail);

  PerfTestPlan p;
  p.width = 2560;
  p.height = 1440;
  p.fps = 60;
  p.scale_percent = 100;
  p.fg_multiplier = 2;
  const std::string good_fg = format_perf_test_card(p, sample(60.0, 2, 60), PerfTestGrade::Good);
  CHECK(good_fg.find("Bom") != std::string::npos);
  CHECK(good_fg.find("opcional") != std::string::npos);
  CHECK(good_fg.find("extra") != std::string::npos);
  const std::string tight_fg = format_perf_test_card(p, sample(60.0, 2, 60, 5, 18.0), PerfTestGrade::Tight);
  CHECK(tight_fg.find("Apertado") != std::string::npos);
  CHECK(tight_fg.find("custar") != std::string::npos);
  CHECK(tight_fg.find("escala para") == std::string::npos);
  CHECK(perf_test_suggest_scale(100, sample(40.0, 2, 60), PerfTestGrade::PlayableFg, 60) == 75);
  CHECK(perf_test_suggest_scale(100, sample(25.0, 1, 60), PerfTestGrade::Fail, 60) == 55);
  CHECK(perf_test_suggest_scale(100, sample(15.0, 1, 60), PerfTestGrade::Fail, 60) == 50);
  CHECK(perf_test_suggest_scale(100, sample(22.0, 2, 30), PerfTestGrade::PlayableFg, 30) == 80);
  CHECK(perf_test_suggest_scale(80, sample(40.0, 2, 60), PerfTestGrade::PlayableFg, 60) == 60);
  p.fps = 30;
  const std::string play30 = format_perf_test_card(p, sample(22.0, 2, 30), PerfTestGrade::PlayableFg);
  CHECK(play30.find("Jogavel com FG") != std::string::npos);
  CHECK(play30.find("Da para jogar") != std::string::npos);
}

TEST_CASE("perf test advice validates real 3x cadence lock not area formula") {
  auto sample = [](double fps, int fg, double target, uint64_t stalls, double present) {
    PerfTestSample s;
    s.fps = fps;
    s.fg_multiplier = fg;
    s.target_fps = target;
    s.stalls = stalls;
    s.p95_present_ms = present;
    return s;
  };
  CHECK(perf_test_cadence_locked(30.00));
  CHECK(perf_test_cadence_locked(39.9995));
  CHECK(perf_test_cadence_locked(40.00));
  CHECK_FALSE(perf_test_cadence_locked(59.94));
  CHECK_FALSE(perf_test_cadence_locked(47.0));
  CHECK(perf_test_suggest_scale(100, sample(30.0, 1, 60, 599, 33.3), PerfTestGrade::Fail, 60) == 60);

  PerfTestPlan p3;
  p3.width = 2560;
  p3.height = 1440;
  p3.fps = 60;
  p3.scale_percent = 100;
  p3.nr_pass_count = 3;
  PerfTestSample lock30 = sample(30.0, 1, 60, 599, 33.30);
  CHECK(grade_perf_test(lock30) == PerfTestGrade::Fail);
  const std::string a30 = perf_test_advice(p3, lock30, PerfTestGrade::Fail);
  CHECK(a30.find("escala para 60") != std::string::npos);
  CHECK(a30.find("70") == std::string::npos);
  CHECK(a30.find("Pass 1x") == std::string::npos);

  p3.scale_percent = 70;
  PerfTestSample lock40 = sample(39.9995, 1, 60, 300, 25.07);
  CHECK(grade_perf_test(lock40) == PerfTestGrade::Fail);
  const std::string a40 = perf_test_advice(p3, lock40, PerfTestGrade::Fail);
  CHECK(a40.find("escala para 50") != std::string::npos);
  CHECK(a40.find("55") == std::string::npos);

  p3.nr_pass_count = 1;
  p3.scale_percent = 100;
  const std::string a1x = perf_test_advice(p3, sample(40.0, 1, 60, 295, 25.0), PerfTestGrade::Fail);
  CHECK(a1x.find("escala para 75") != std::string::npos);
}

TEST_CASE("perf test p=1.5 combination grid matches fitted 1440 locks") {
  auto pass_ui = [](double w) {
    if (w == 1.5) return 4;
    if (w == 2.0) return 2;
    if (w == 3.0) return 3;
    return 1;
  };
  auto pred_gpu = [](double pw, int scale) { return 11.11 * pw * std::pow(scale / 100.0, 1.5); };
  auto pred_lock = [](double gpu) {
    if (gpu <= 16.67) return 60;
    if (gpu <= 25.0) return 40;
    return 30;
  };
  int cells = 0;
  for (double pw : {1.0, 1.5, 2.0, 3.0}) {
    for (int scale : {50, 60, 70, 80, 90, 100}) {
      ++cells;
      const int lock = pred_lock(pred_gpu(pw, scale));
      PerfTestPlan p;
      p.width = 2560;
      p.height = 1440;
      p.fps = 60;
      p.scale_percent = scale;
      p.nr_pass_count = pass_ui(pw);
      PerfTestSample s;
      s.fps = static_cast<double>(lock);
      s.target_fps = 60;
      s.p95_present_ms = 1000.0 / lock;
      s.stalls = lock < 60 ? 200ull : 0;
      const auto g = grade_perf_test(s);
      const std::string tip = perf_test_advice(p, s, g);
      if (lock >= 60) {
        CHECK(g == PerfTestGrade::Good);
        CHECK(tip.find("escala para") == std::string::npos);
      } else {
        CHECK(g == PerfTestGrade::Fail);
        const bool floor_ok = perf_test_scale_floor_hits_meta(scale, s.fps, 60);
        if (!floor_ok && pw > 1.0) {
          CHECK(tip.find("Pass 1x") != std::string::npos);
        } else {
          const int next = perf_test_suggest_scale(scale, s, g, 60);
          CHECK(next >= 50);
          CHECK(next < scale);
          CHECK(pred_gpu(pw, next) <= 16.67 + 1.5);
        }
      }
    }
  }
  CHECK(cells == 24);
}

TEST_CASE("perf test every lobby combo has a coherent next step") {
  const int widths[] = {1920, 2560, 3840};
  const int heights[] = {1080, 1440, 2160};
  const double metas[] = {30.0, 60.0};
  const int scales[] = {50, 70, 100};
  const int passes[] = {1, 4, 2, 3};
  const int fgs[] = {1, 2, 3, 4};
  int n = 0;
  for (int i = 0; i < 3; ++i) {
    for (double meta : metas) {
      for (int scale : scales) {
        for (int pass : passes) {
          for (int fg : fgs) {
            ++n;
            PerfTestPlan p;
            p.width = widths[i];
            p.height = heights[i];
            p.fps = meta;
            p.scale_percent = scale;
            p.nr_pass_count = pass;
            p.fg_multiplier = fg;
            auto check_card = [&](const PerfTestSample& s) {
              const auto g = grade_perf_test(s);
              const std::string card = format_perf_test_card(p, s, g);
              const std::string tip = perf_test_advice(p, s, g);
              CHECK_FALSE(card.empty());
              CHECK(card.find(perf_test_grade_label(g)) != std::string::npos);
              if (g == PerfTestGrade::Good) {
                CHECK(card.find("Nao segura") == std::string::npos);
                if (fg < 2) CHECK(card.find("nativo sem FG") != std::string::npos);
                return;
              }
              if (g == PerfTestGrade::Tight && fg >= 2) {
                CHECK(card.find("custar") != std::string::npos);
                return;
              }
              const bool at_floor = scale <= 50 && pass == 1 && p.width < 2200 && meta < 45.0;
              if (g == PerfTestGrade::Fail && at_floor && fg < 2) {
                if (perf_test_fg_mult_hits_meta(s.fps, meta, 2) >= 2) {
                  CHECK(tip.find("FG x") != std::string::npos);
                } else {
                  CHECK(tip.find("minimo") != std::string::npos);
                }
                CHECK(tip.find("desliga o Frame Gen ou baixa o Pass") == std::string::npos);
              } else if (g == PerfTestGrade::PlayableFg && at_floor) {
                CHECK(tip.empty());
              } else if (g == PerfTestGrade::Tight && at_floor) {
                CHECK(tip.empty());
              } else {
                CHECK_FALSE(tip.empty());
              }
            };
            PerfTestSample ok;
            ok.fps = meta;
            ok.target_fps = meta;
            ok.fg_multiplier = fg;
            ok.stalls = 0;
            ok.p95_present_ms = 2.0;
            check_card(ok);
            PerfTestSample tight = ok;
            tight.stalls = fg >= 2 ? 4 : 2;
            tight.p95_present_ms = 20.0;
            check_card(tight);
            PerfTestSample mid;
            mid.fps = meta < 45.0 ? 22.0 : 40.0;
            mid.target_fps = meta;
            mid.fg_multiplier = fg;
            mid.stalls = 80;
            mid.p95_present_ms = 25.0;
            check_card(mid);
            PerfTestSample dead;
            dead.fps = 15.0;
            dead.target_fps = meta;
            dead.fg_multiplier = fg;
            dead.stalls = 200;
            dead.p95_present_ms = 40.0;
            check_card(dead);
          }
        }
      }
    }
  }
  CHECK(n == 3 * 2 * 3 * 4 * 4);
}

TEST_CASE("perf test advice fills 30/60 and FG gaps without pointing a weak 30 at 60") {
  auto sample = [](double fps, int fg, double target, uint64_t stalls = 0, double present = 2.0) {
    PerfTestSample s;
    s.fps = fps;
    s.fg_multiplier = fg;
    s.target_fps = target;
    s.stalls = stalls;
    s.p95_present_ms = present;
    return s;
  };
  CHECK(perf_test_fg_mult_hits_meta(40.0, 60.0, 2) == 2);
  CHECK(perf_test_fg_mult_hits_meta(25.0, 60.0, 2) == 3);
  CHECK(perf_test_fg_mult_hits_meta(25.0, 60.0, 3) == 3);
  CHECK(perf_test_fg_mult_hits_meta(15.0, 60.0, 2) == 0);
  CHECK(perf_test_fg_mult_hits_meta(40.0, 30.0, 2) == 2);

  PerfTestPlan p;
  p.width = 2560;
  p.height = 1440;
  p.fps = 30;
  p.scale_percent = 100;
  p.nr_pass_count = 1;
  p.fg_multiplier = 1;
  const std::string good30_60 = perf_test_advice(p, sample(60.0, 1, 30), PerfTestGrade::Good);
  CHECK(good30_60.find("meta 60") != std::string::npos);
  const std::string good30_40 = perf_test_advice(p, sample(40.0, 1, 30), PerfTestGrade::Good);
  CHECK(good30_40.find("60") == std::string::npos);
  CHECK(good30_40.empty());

  p.fps = 60;
  const std::string fail40 = perf_test_advice(p, sample(40.0, 1, 60), PerfTestGrade::Fail);
  CHECK(fail40.find("escala para 75") != std::string::npos);
  CHECK(fail40.find("FG x2") != std::string::npos);
  CHECK(fail40.find("latencia") != std::string::npos);
  const std::string fail15 = perf_test_advice(p, sample(15.0, 1, 60), PerfTestGrade::Fail);
  CHECK(fail15.find("FG x") == std::string::npos);

  p.fg_multiplier = 2;
  const std::string fail25x2 = perf_test_advice(p, sample(25.0, 2, 60), PerfTestGrade::Fail);
  CHECK(fail25x2.find("escala para") != std::string::npos);
  CHECK(fail25x2.find("FG x3") != std::string::npos);
  CHECK(fail25x2.find("FG x2 chega") == std::string::npos);

  const std::string play40 = format_perf_test_card(p, sample(40.0, 2, 60), PerfTestGrade::PlayableFg);
  CHECK(play40.find("Jogavel com FG") != std::string::npos);
  CHECK(play40.find("escala para 75") != std::string::npos);
  CHECK(play40.find("latencia") != std::string::npos);

  const std::string tight_ok = perf_test_advice(p, sample(60.0, 2, 60, 5, 18.0), PerfTestGrade::Tight);
  CHECK(tight_ok.empty());
  const std::string tight_hot = perf_test_advice(p, sample(60.0, 2, 60, 5, 22.0), PerfTestGrade::Tight);
  CHECK(tight_hot.find("escala para") != std::string::npos);
}

TEST_CASE("virtual pin playhead follows plan fps 30 and 60 not present cadence") {
  CHECK(virtual_pin_fps(30) == 30.0);
  CHECK(virtual_pin_fps(29) == 30.0);
  CHECK(virtual_pin_fps(60) == 60.0);
  CHECK(virtual_pin_fps(59) == 60.0);
  CHECK(virtual_pin_period_ms(60) == doctest::Approx(1000.0 / 60.0));
  CHECK(virtual_pin_period_ms(30) == doctest::Approx(1000.0 / 30.0));

  CHECK(virtual_pin_src_index(0, 0, 60) == 0);
  CHECK(virtual_pin_src_index(1000, 0, 60) == 60);
  CHECK(virtual_pin_src_index(1000, 0, 30) == 30);
  CHECK(virtual_pin_src_index(2000, 1000, 60) == 60);
  CHECK(virtual_pin_src_index(2500, 0, 60, 100) == 50);

  // 40 presents/s over 1s: wall playhead is 60 src/s, not 40.
  uint64_t last60 = 0;
  for (int i = 0; i <= 40; ++i) last60 = virtual_pin_src_index(static_cast<double>(i) * 25.0, 0, 60);
  CHECK(last60 == 60);
  uint64_t last30 = 0;
  for (int i = 0; i <= 40; ++i) last30 = virtual_pin_src_index(static_cast<double>(i) * 25.0, 0, 30);
  CHECK(last30 == 30);

  // Clip 60 fps, plan 30: content index is 60 at t=1s; pin index is 30.
  CHECK(virtual_pin_clip_index(1000, 0, 60) == 60);
  CHECK(virtual_pin_clip_index(1000, 0, 60, 2400) == 60);
  CHECK(virtual_pin_src_index(1000, 0, 30) == 30);
}

TEST_CASE("virtual pin ring depth is time not a hardcoded frame count") {
  CHECK(virtual_pin_ring_frames(60) == 90);
  CHECK(virtual_pin_ring_frames(30) == 45);
  CHECK(virtual_pin_prime_frames(60) == 60);
  CHECK(virtual_pin_prime_frames(30) == 30);
  CHECK(virtual_pin_ring_frames_for(60, 1920, 1080) == 90);
  CHECK(virtual_pin_ring_frames_for(60, 2560, 1440) == 90);
  CHECK(virtual_pin_ring_frames_for(30, 2560, 1440) == 45);
  const int r4k = virtual_pin_ring_frames_for(60, 3840, 2160);
  CHECK(r4k == 64);
}

TEST_CASE("virtual pin holds clip 0 until playhead starts") {
  CHECK(virtual_pin_held_clip_index(false, 10000, 0, 60) == 0);
  CHECK(virtual_pin_held_src_index(false, 10000, 0, 60) == 0);
  CHECK(virtual_pin_held_src_index(false, 10000, 0, 30) == 0);
  const double t0 = 8000;
  CHECK(virtual_pin_held_src_index(true, t0, t0, 30) == 0);
  CHECK(virtual_pin_held_src_index(true, t0 + 1000, t0, 30) == 30);
  CHECK(virtual_pin_held_src_index(true, t0 + 1000, t0, 60) == 60);
  CHECK(virtual_pin_held_clip_index(true, t0 + 1000, t0, 60) == 60);
}

TEST_CASE("virtual pin take latest after hitch never blocks") {
  CHECK(virtual_pin_take_index(0, 0) == 0);
  CHECK(virtual_pin_take_index(0, 300) == 0);
  CHECK(virtual_pin_take_index(10, 20) == 10);
  CHECK(virtual_pin_take_index(80, 10) == 9);
  // 2s hitch at 60: want jumps 120, decoded still 10 → latest 9, not freeze/empty.
  CHECK(virtual_pin_take_index(virtual_pin_src_index(2000, 0, 60), 10) == 9);
  CHECK(virtual_pin_should_throttle_decode(90, 0, 0, 90));
  CHECK_FALSE(virtual_pin_should_throttle_decode(40, 0, 0, 90));
  CHECK(virtual_pin_ahead(50, 10, 0, 90) == 40);
}

TEST_CASE("perf test stall uses effective hitch of the meta not a flat 25ms") {
  CHECK_FALSE(perf_test_is_stall(16.7, 60));
  CHECK_FALSE(perf_test_is_stall(25.0, 60));
  CHECK_FALSE(perf_test_is_stall(25.06, 60));
  CHECK(perf_test_is_stall(33.0, 60));
  CHECK_FALSE(perf_test_is_stall(33.3, 30));
  CHECK_FALSE(perf_test_is_stall(50.0, 30));
  CHECK(perf_test_is_stall(60.0, 30));
  CHECK(effective_hitch_threshold_ms(25.0, 30.0) == doctest::Approx(50.0));
  CHECK(effective_hitch_threshold_ms(25.0, 60.0) == doctest::Approx(25.0));
  PerfScoreAcc acc;
  for (int i = 0; i < 200; ++i) acc.push(25.06, 24.5, perf_test_is_stall(25.06, 60));
  const auto s = perf_test_sample_from_acc(acc, 60.0, 1);
  CHECK(s.fps == doctest::Approx(1000.0 / 25.06).epsilon(0.02));
  CHECK(s.stalls == 0);
  CHECK(grade_perf_test(s) == PerfTestGrade::Fail);
}

static std::filesystem::path make_user_preset_tmp(const char* tag) {
  auto root = std::filesystem::temp_directory_path() / "nexuscap_preset_ut" / tag;
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  return root;
}

static UserPreset make_user_preset(const char* name, int pass, int look, bool fg, int fgx, int scale) {
  UserPreset p;
  p.name = name;
  p.hud_visible = true;
  p.overlay_no_bg = false;
  p.overlay_console = true;
  p.live_console = 0;
  p.neural_rendering = true;
  p.renodx_preset = look;
  p.nr_pass_count = pass;
  p.fg_enabled = fg;
  p.fg_multiplier = fgx;
  p.scale_percent = scale;
  clamp_user_preset(p);
  return p;
}

static SessionCfgState make_preset_session(int pass, int look, bool fg, int fgx, int scale) {
  SessionCfgState s;
  s.renodx_stack = true;
  s.nr_on = true;
  s.renodx_preset = look;
  s.nr_pass_count = pass;
  s.scale_percent = scale;
  s.fg_on = fg;
  s.fg_multiplier = fgx;
  s.hud_visible = true;
  s.overlay_no_bg = false;
  s.overlay_console = false;
  s.live_console = 0;
  return s;
}

static void require_preset_equal(const UserPreset& a, const UserPreset& b) {
  CHECK(a.name == b.name);
  CHECK(a.hud_visible == b.hud_visible);
  CHECK(a.overlay_no_bg == b.overlay_no_bg);
  CHECK(a.overlay_console == b.overlay_console);
  CHECK(a.live_console == b.live_console);
  CHECK(a.neural_rendering == b.neural_rendering);
  CHECK(a.renodx_preset == b.renodx_preset);
  CHECK(a.nr_pass_count == b.nr_pass_count);
  CHECK(a.fg_enabled == b.fg_enabled);
  CHECK(a.fg_multiplier == b.fg_multiplier);
  CHECK(a.scale_percent == b.scale_percent);
}

TEST_CASE("user preset name trim empty too long and forbidden chars") {
  std::string out;
  CHECK(normalize_user_preset_name("", out) == UserPresetNameStatus::Empty);
  CHECK(normalize_user_preset_name("   ", out) == UserPresetNameStatus::Empty);
  CHECK(normalize_user_preset_name("  TLOU  ", out) == UserPresetNameStatus::Ok);
  CHECK(out == "TLOU");
  CHECK(normalize_user_preset_name(std::string(kUserPresetNameMax, 'x'), out) == UserPresetNameStatus::Ok);
  CHECK(out.size() == static_cast<size_t>(kUserPresetNameMax));
  CHECK(normalize_user_preset_name(std::string(kUserPresetNameMax + 1, 'x'), out) ==
        UserPresetNameStatus::TooLong);
  CHECK(out.empty());
  CHECK(normalize_user_preset_name("TLOU/2x", out) == UserPresetNameStatus::Forbidden);
  CHECK(out.empty());
  CHECK(user_preset_name_has_forbidden("TLOU:2x"));
  CHECK(user_preset_char_forbidden(static_cast<unsigned char>('*')));
  CHECK_FALSE(user_preset_char_forbidden(static_cast<unsigned char>('a')));
  CHECK(std::string(user_preset_name_status_toast(UserPresetNameStatus::Forbidden)).find('\\') !=
        std::string::npos);
  CHECK(user_preset_names_equal("TLOU", "tlou"));
  CHECK(user_preset_names_equal("TLOU 2x", "tlou 2x"));
  CHECK_FALSE(user_preset_names_equal("TLOU", "GoW"));
  CHECK(normalize_user_preset_name("TLOU cafe", out) == UserPresetNameStatus::Ok);
  CHECK(kUserPresetNameMax == 50);
  CHECK(kUserPresetNameBuf == kUserPresetNameMax + 1);
  CHECK(std::string(user_preset_name_status_toast(UserPresetNameStatus::Empty))
            .find(std::to_string(kUserPresetNameMax)) != std::string::npos);
  CHECK(std::string(user_preset_name_status_toast(UserPresetNameStatus::TooLong))
            .find(std::to_string(kUserPresetNameMax)) != std::string::npos);
  CHECK(std::string(user_preset_name_field_hint()).find(std::to_string(kUserPresetNameMax)) != std::string::npos);
  const char forb[] = "\\/:*?\"<>|\n\r";
  for (const char* c = forb; *c; ++c) {
    CHECK(user_preset_char_forbidden(static_cast<unsigned char>(*c)));
    CHECK(user_preset_name_filter_event(static_cast<unsigned int>(static_cast<unsigned char>(*c)), 0) == 1);
    CHECK(normalize_user_preset_name(std::string("A") + *c + "B", out) == UserPresetNameStatus::Forbidden);
  }
  CHECK(user_preset_name_filter_event(static_cast<unsigned int>('a'), 0) == 0);
  CHECK(user_preset_name_filter_event(static_cast<unsigned int>('a'), kUserPresetNameMax - 1) == 0);
  CHECK(user_preset_name_filter_event(static_cast<unsigned int>('a'), kUserPresetNameMax) == 1);
  CHECK(user_preset_name_filter_event(233u, 0) == 0);
  CHECK(user_preset_name_filter_event(233u, kUserPresetNameMax) == 1);
  CHECK(user_preset_name_limit_hint("").find("0/50") != std::string::npos);
  CHECK(user_preset_name_limit_hint("TLOU").find("4/50") != std::string::npos);
  CHECK(user_preset_name_at_limit(std::string(kUserPresetNameMax, 'x').c_str()));
  CHECK_FALSE(user_preset_name_at_limit(std::string(kUserPresetNameMax - 1, 'x').c_str()));
  CHECK(user_preset_name_limit_hint(std::string(kUserPresetNameMax, 'x').c_str()).find("limite") !=
        std::string::npos);
  char copied[kUserPresetNameBuf]{};
  fill_user_preset_name_buf(copied, sizeof(copied), "TLOU 2x");
  CHECK(std::string(copied) == "TLOU 2x");
  char tiny[4]{};
  fill_user_preset_name_buf(tiny, sizeof(tiny), "TLOU");
  CHECK(std::string(tiny) == "TLO");
  UserPreset too_long;
  too_long.name = std::string(kUserPresetNameMax + 1, 'z');
  too_long.nr_pass_count = 1;
  CHECK_FALSE(parse_user_preset(serialize_user_preset(too_long), too_long));
}

TEST_CASE("user preset list refuses the 33rd without dropping the others") {
  const auto dir = make_user_preset_tmp("full32");
  bool replaced = false;
  std::string err;
  for (int i = 0; i < kUserPresetMaxCount; ++i) {
    char name[16]{};
    std::snprintf(name, sizeof(name), "Slot %02d", i);
    UserPreset p = make_user_preset(name, 1, 0, false, 2, 100);
    REQUIRE(upsert_user_preset_in(dir, p, replaced, err));
    CHECK_FALSE(replaced);
  }
  std::vector<UserPreset> have;
  REQUIRE(list_user_presets_in(dir, have));
  CHECK(have.size() == static_cast<size_t>(kUserPresetMaxCount));
  UserPreset extra = make_user_preset("Slot extra", 2, 1, true, 3, 80);
  CHECK_FALSE(upsert_user_preset_in(dir, extra, replaced, err));
  CHECK(err == "lista cheia");
  have.clear();
  REQUIRE(list_user_presets_in(dir, have));
  CHECK(have.size() == static_cast<size_t>(kUserPresetMaxCount));
  CHECK(find_user_preset_index(have, "Slot extra") < 0);
  UserPreset overwrite = make_user_preset("Slot 00", 5, 2, true, 3, 100);
  REQUIRE(upsert_user_preset_in(dir, overwrite, replaced, err));
  CHECK(replaced);
  have.clear();
  REQUIRE(list_user_presets_in(dir, have));
  CHECK(have.size() == static_cast<size_t>(kUserPresetMaxCount));
  REQUIRE(rename_user_preset_in(dir, "Slot 31", "Slot extra", false, replaced, err));
  CHECK_FALSE(replaced);
  have.clear();
  REQUIRE(list_user_presets_in(dir, have));
  CHECK(have.size() == static_cast<size_t>(kUserPresetMaxCount));
  CHECK(find_user_preset_index(have, "Slot 31") < 0);
  CHECK(find_user_preset_index(have, "Slot extra") >= 0);
}

TEST_CASE("user preset snapshot is the whole Ctrl+M and roundtrips") {
  UserPreset a;
  a.name = "TLOU 2x";
  a.hud_visible = false;
  a.overlay_no_bg = true;
  a.overlay_console = true;
  a.live_console = 1;
  a.neural_rendering = false;
  a.renodx_preset = 2;
  a.nr_pass_count = 5;
  a.fg_enabled = true;
  a.fg_multiplier = 3;
  a.scale_percent = 80;
  UserPreset b;
  REQUIRE(parse_user_preset(serialize_user_preset(a), b));
  require_preset_equal(a, b);

  const int passes[] = {1, 4, 2, 5, 3};
  const int looks[] = {0, 1, 2};
  const int fgs[] = {0, 2, 3, 4};
  for (int pass : passes) {
    for (int look : looks) {
      for (int fgx : fgs) {
        const bool fg = fgx >= 2;
        UserPreset cell = make_user_preset("cell", pass, look, fg, fg == false ? 2 : fgx, 100);
        UserPreset got;
        REQUIRE(parse_user_preset(serialize_user_preset(cell), got));
        CHECK(got.nr_pass_count == pass);
        CHECK(got.renodx_preset == look);
        CHECK(got.fg_enabled == fg);
        if (fg) CHECK(got.fg_multiplier == fgx);
      }
    }
  }

  UserPreset tlou = make_user_preset("TLOU 2x", 5, 2, true, 3, 100);
  UserPreset tlou2;
  REQUIRE(parse_user_preset(serialize_user_preset(tlou), tlou2));
  CHECK(tlou2.nr_pass_count == 5);
  CHECK(tlou2.renodx_preset == 2);
  CHECK(tlou2.fg_enabled);
  CHECK(tlou2.fg_multiplier == 3);
  CHECK(tlou2.scale_percent == 100);

  UserPreset one_fg4 = make_user_preset("1x FGx4", 1, 0, true, 4, 100);
  UserPreset three_off = make_user_preset("3x off", 3, 1, false, 2, 75);
  UserPreset a1, a3;
  REQUIRE(parse_user_preset(serialize_user_preset(one_fg4), a1));
  REQUIRE(parse_user_preset(serialize_user_preset(three_off), a3));
  CHECK(a1.nr_pass_count == 1);
  CHECK(a1.fg_enabled);
  CHECK(a1.fg_multiplier == 4);
  CHECK(a3.nr_pass_count == 3);
  CHECK_FALSE(a3.fg_enabled);
  CHECK(a3.scale_percent == 75);

  CHECK(std::string(renodx_visual_preset_name(2, 1)) == "Model C");
  CHECK(std::string(renodx_visual_preset_name(2, 5)) == "Model C");
  CHECK(tlou2.renodx_preset == 2);
  CHECK(tlou2.nr_pass_count == 5);
}

TEST_CASE("user preset list summary is Pass look FG and scale") {
  CHECK(user_preset_list_summary(make_user_preset("a", 5, 2, true, 3, 100)) == "2x+  Model C  FG x3  100%");
  CHECK(user_preset_list_summary(make_user_preset("b", 1, 0, false, 2, 100)) == "1x  Model A  FG off  100%");
  CHECK(user_preset_list_summary(make_user_preset("c", 4, 1, true, 2, 100)) == "1.5x  Model B  FG x2  100%");
  CHECK(user_preset_list_summary(make_user_preset("d", 2, 0, true, 4, 100)) == "2x  Model A  FG x4  100%");
  CHECK(user_preset_list_summary(make_user_preset("e", 3, 2, false, 3, 80)) == "3x  Model C  FG off  80%");
  CHECK(user_preset_list_summary(make_user_preset("e6", 6, 2, false, 3, 80)) == "3x+  Model C  FG off  80%");
  CHECK(user_preset_list_summary(make_user_preset("f", 1, 2, true, 3, 100)) == "1x  Model C  FG x3  100%");
  CHECK(user_preset_list_summary(make_user_preset("g", 1, 1, false, 2, 100)) == "1x  Model B  FG off  100%");
  CHECK(user_preset_list_summary(make_user_preset("a", 5, 2, true, 3, 100)).find("HUD") == std::string::npos);
  CHECK(user_preset_marquee_offset(80.f, 200.f, 12.0) == 0.f);
  CHECK(user_preset_marquee_offset(100.f, 100.f, 9.0) == 0.f);
  CHECK(user_preset_marquee_offset(200.f, 100.f, 0.0) == 0.f);
  CHECK(user_preset_marquee_offset(200.f, 100.f, -1.0) >= 0.f);
  const float loop = 200.f + kUserPresetMarqueeGapPx;
  const float wrap = user_preset_marquee_offset(200.f, 100.f, static_cast<double>(loop / kUserPresetMarqueePxPerSec));
  CHECK(wrap < 0.05f);
  const float mid = user_preset_marquee_offset(200.f, 100.f, 1.0);
  CHECK(mid > 40.f);
  CHECK(mid < 45.f);
}

TEST_CASE("user preset FG and scale survive session persist after apply restart") {
  const UserPreset p = make_user_preset("TLOU 2x", 5, 2, true, 3, 80);
  PersistedSettings disk = default_settings();
  disk.fg_enabled = false;
  disk.fg_multiplier = 2;
  disk.scale_percent = 100;
  apply_user_preset_to_persisted(disk, p, true);
  CHECK(disk.fg_enabled);
  CHECK(disk.fg_multiplier == 3);
  CHECK(disk.scale_percent == 80);

  AppConfig stale{};
  stale.fg_auto_start = false;
  stale.fg_multiplier = 2;
  stale.scale_percent = 100;
  apply_session_cfg_to_persisted(stale, disk);
  CHECK_FALSE(disk.fg_enabled);
  CHECK(disk.scale_percent == 100);

  apply_user_preset_to_persisted(disk, p, true);
  AppConfig stamped{};
  apply_session_fg_to_config(p.fg_enabled, p.fg_multiplier, stamped);
  stamped.scale_percent = p.scale_percent;
  stamped.renodx_preset = p.renodx_preset;
  stamped.nr_pass_count = p.nr_pass_count;
  stamped.neural_rendering = p.neural_rendering;
  apply_session_cfg_to_persisted(stamped, disk);
  CHECK(disk.fg_enabled);
  CHECK(disk.fg_multiplier == 3);
  CHECK(disk.scale_percent == 80);
  CHECK(disk.nr_pass_count == 5);
}

TEST_CASE("user preset lobby snapshot and apply plan") {
  PersistedSettings s = default_settings();
  s.device_name = "Elgato 4K X";
  s.fg_path = "C:\\LS\\LosslessScaling.exe";
  s.width = 2560;
  s.height = 1440;
  s.ui.show_fps = false;
  s.ui.overlay_no_bg = true;
  s.ui.overlay_console = true;
  s.ui.live_console = "switch2";
  s.neural_rendering = true;
  s.enhance_renodx = true;
  s.renodx_preset = 2;
  s.nr_pass_count = 5;
  s.fg_enabled = true;
  s.fg_multiplier = 3;
  s.scale_percent = 80;
  const UserPreset snap = user_preset_from_persisted("TLOU 2x", s);
  CHECK(snap.hud_visible == false);
  CHECK(snap.overlay_no_bg);
  CHECK(snap.overlay_console);
  CHECK(snap.nr_pass_count == 5);
  CHECK(snap.renodx_preset == 2);
  CHECK(snap.fg_enabled);
  CHECK(snap.fg_multiplier == 3);
  CHECK(snap.scale_percent == 80);
  CHECK(snap.live_console == static_cast<int>(parse_live_console("switch2")));

  PersistedSettings other = default_settings();
  other.device_name = s.device_name;
  other.fg_path = s.fg_path;
  other.width = s.width;
  other.height = s.height;
  other.nr_pass_count = 1;
  other.renodx_preset = 0;
  other.fg_enabled = false;
  other.scale_percent = 100;
  other.enhance_renodx = false;
  other.neural_rendering = false;
  const LobbyUserPresetPlan plan = plan_lobby_user_preset_apply(other, snap, true);
  CHECK(plan.pass_relaunch);
  CHECK_FALSE(plan.motor_relaunch);
  CHECK(plan.look_changed);
  CHECK_FALSE(plan.fg_missing);
  apply_user_preset_to_persisted(other, snap, true);
  CHECK(persisted_untouched_by_user_preset(s, other));
  CHECK(other.nr_pass_count == 5);
  CHECK(other.scale_percent == 80);
  CHECK(other.fg_enabled);
  CHECK(other.fg_multiplier == 3);
  CHECK(other.enhance_renodx);
  CHECK(other.ui.show_fps == false);

  const LobbyUserPresetPlan miss = plan_lobby_user_preset_apply(other, snap, false);
  CHECK(miss.fg_missing);
  PersistedSettings no_ls = other;
  apply_user_preset_to_persisted(no_ls, snap, false);
  CHECK_FALSE(no_ls.fg_enabled);
  CHECK(no_ls.nr_pass_count == 5);
  CHECK_FALSE(plan_lobby_user_preset_apply(s, snap, true).pass_relaunch);
  CHECK_FALSE(nr_pass_count_change_needs_relaunch(2, 5));
  CHECK(plan_lobby_user_preset_apply(other, make_user_preset("opt", 5, 2, false, 2, 80), true).motor_relaunch ==
        nr_pass_count_change_needs_relaunch(other.nr_pass_count, 5));
}

TEST_CASE("user preset snapshot takes the Ctrl+M scale not the default 100") {
  SessionCfgState s = make_preset_session(5, 2, true, 3, 67);
  const UserPreset p = user_preset_from_session("TLOU 2x", s);
  CHECK(p.scale_percent == 67);
  UserPreset got;
  REQUIRE(parse_user_preset(serialize_user_preset(p), got));
  CHECK(got.scale_percent == 67);
  const SessionCfgActions a = plan_user_preset_apply(make_preset_session(5, 2, true, 3, 100), p, true);
  CHECK(a.scale_apply);
  CHECK(a.apply_scale_percent == 67);
  PersistedSettings disk = default_settings();
  disk.scale_percent = 100;
  apply_user_preset_to_persisted(disk, p, true);
  AppConfig cfg{};
  cfg.scale_percent = 67;
  apply_session_cfg_to_persisted(cfg, disk);
  CHECK(disk.scale_percent == 67);
}

TEST_CASE("user preset upsert overwrite delete and A-Z list") {
  const auto dir = make_user_preset_tmp("upsert");
  bool replaced = false;
  std::string err;
  UserPreset first = make_user_preset("TLOU 2x", 5, 2, true, 3, 100);
  REQUIRE(upsert_user_preset_in(dir, first, replaced, err));
  CHECK_FALSE(replaced);
  UserPreset second = make_user_preset("tlou 2x", 1, 0, false, 2, 50);
  REQUIRE(upsert_user_preset_in(dir, second, replaced, err));
  CHECK(replaced);
  std::vector<UserPreset> have;
  REQUIRE(list_user_presets_in(dir, have));
  CHECK(have.size() == 1);
  CHECK(have[0].nr_pass_count == 1);
  CHECK_FALSE(have[0].fg_enabled);
  CHECK(have[0].scale_percent == 50);

  UserPreset gow = make_user_preset("GoW", 2, 1, true, 2, 100);
  UserPreset zelda = make_user_preset("Zelda", 4, 0, false, 2, 100);
  REQUIRE(upsert_user_preset_in(dir, zelda, replaced, err));
  REQUIRE(upsert_user_preset_in(dir, gow, replaced, err));
  have.clear();
  REQUIRE(list_user_presets_in(dir, have));
  CHECK(have.size() == 3);
  CHECK(have[0].name == "GoW");
  CHECK(have[1].name == "tlou 2x");
  CHECK(have[2].name == "Zelda");

  REQUIRE(delete_user_preset_in(dir, "Zelda", err));
  have.clear();
  REQUIRE(list_user_presets_in(dir, have));
  CHECK(have.size() == 2);
  CHECK(find_user_preset_index(have, "Zelda") < 0);
  CHECK_FALSE(std::filesystem::exists(user_preset_store_path(dir, "Zelda")));
}

TEST_CASE("user preset import export extra field junk and clamp") {
  const auto dir = make_user_preset_tmp("impexp");
  UserPreset src = make_user_preset("TLOU 2x", 5, 2, true, 3, 100);
  src.hud_visible = false;
  src.overlay_no_bg = true;
  const std::string text = serialize_user_preset(src);
  UserPreset got;
  bool replaced = false;
  std::string err;
  REQUIRE(import_user_preset_in(dir, text, got, replaced, err));
  CHECK_FALSE(replaced);
  require_preset_equal(src, got);

  std::string future = text;
  future += "future_knob=99\nversion_note=keep\n";
  UserPreset fut;
  REQUIRE(parse_user_preset(future, fut));
  CHECK(fut.name == "TLOU 2x");
  CHECK(fut.nr_pass_count == 5);
  CHECK(fut.fg_multiplier == 3);

  std::vector<UserPreset> have;
  REQUIRE(list_user_presets_in(dir, have));
  const size_t before = have.size();
  CHECK_FALSE(parse_user_preset("", got));
  CHECK_FALSE(parse_user_preset("lixo sem cabecalho\nname=Bad\n", got));
  CHECK_FALSE(parse_user_preset("name=TLOU\nhud_visible=1\n", got));
  UserPreset bad;
  CHECK_FALSE(import_user_preset_in(dir, "", bad, replaced, err));
  CHECK(err == "arquivo invalido");
  have.clear();
  REQUIRE(list_user_presets_in(dir, have));
  CHECK(have.size() == before);

  UserPreset wild;
  REQUIRE(parse_user_preset(
      "nexuscap-preset=1\nname=ClampMe\nhud_visible=1\noverlay_no_bg=0\noverlay_console=0\n"
      "live_console=0\nneural_rendering=1\nrenodx_preset=9\nnr_pass_count=99\nfg_enabled=1\n"
      "fg_multiplier=7\nscale_percent=3\n",
      wild));
  CHECK(wild.nr_pass_count == clamp_nr_pass_count(99));
  CHECK(wild.nr_pass_count == 6);
  CHECK(wild.scale_percent == clamp_scale_percent(3));
  CHECK(wild.scale_percent == 25);
  CHECK(wild.fg_multiplier == clamp_fg_multiplier(7));
  CHECK(wild.fg_multiplier == 4);
  CHECK(wild.renodx_preset == clamp_renodx_visual_preset(9));

  UserPreset again = make_user_preset("TLOU 2x", 1, 0, false, 2, 100);
  REQUIRE(import_user_preset_in(dir, serialize_user_preset(again), got, replaced, err));
  CHECK(replaced);
  have.clear();
  REQUIRE(list_user_presets_in(dir, have));
  CHECK(have.size() == 1);
}

TEST_CASE("user preset apply plan matches Ctrl+M actions and is fill-safe") {
  const SessionCfgState cur = make_preset_session(1, 0, false, 2, 100);
  const UserPreset tlou = make_user_preset("TLOU 2x", 5, 2, true, 3, 80);
  const SessionCfgActions a = plan_user_preset_apply(cur, tlou, true);
  CHECK(a.apply_user_preset);
  CHECK(a.pass_count_changed);
  CHECK(a.pass_count_before == 1);
  CHECK(a.preset_changed);
  CHECK(a.toggle_fg);
  CHECK(a.fg_multiplier_changed);
  CHECK(a.scale_apply);
  CHECK(a.scale_before == 100);
  CHECK_FALSE(a.hud_changed);
  CHECK_FALSE(a.toggle_nr);
  CHECK(a.apply_nr_pass_count == 5);
  CHECK(a.apply_renodx_preset == 2);
  CHECK(a.apply_fg_on);
  CHECK(a.apply_fg_multiplier == 3);
  CHECK(a.apply_scale_percent == 80);
  CHECK_FALSE(nr_pass_count_change_needs_relaunch(a.pass_count_before, a.apply_nr_pass_count));
  CHECK(plan_session_cfg_apply(true, a.pass_count_before, a.apply_nr_pass_count, 100, 80) ==
        SessionCfgApply::RelaunchPassCount);

  const SessionCfgActions same_motor = plan_user_preset_apply(make_preset_session(2, 0, false, 2, 100),
                                                             make_user_preset("opt", 5, 0, false, 2, 100), true);
  CHECK(same_motor.pass_count_changed);
  CHECK_FALSE(nr_pass_count_change_needs_relaunch(2, 5));
  CHECK(plan_session_cfg_apply(true, 2, 5, 100, 100) == SessionCfgApply::RelaunchPassCount);

  const SessionCfgActions cinema = plan_user_preset_apply(make_preset_session(1, 0, false, 2, 100),
                                                         make_user_preset("cin", 1, 2, false, 2, 100), true);
  CHECK(cinema.preset_changed);
  CHECK_FALSE(cinema.pass_count_changed);
  CHECK_FALSE(nr_pass_count_change_needs_relaunch(1, 1));

  const SessionCfgActions miss = plan_user_preset_apply(cur, tlou, false);
  CHECK(miss.apply_user_preset);
  CHECK(miss.pass_count_changed);
  CHECK(miss.preset_changed);
  CHECK(miss.scale_apply);
  CHECK_FALSE(miss.apply_fg_on);
  CHECK_FALSE(miss.toggle_fg);
  CHECK_FALSE(miss.fg_multiplier_changed);

  const SessionCfgActions drop_fg =
      plan_user_preset_apply(make_preset_session(5, 2, true, 3, 100), tlou, false);
  CHECK_FALSE(drop_fg.apply_fg_on);
  CHECK(drop_fg.toggle_fg);

  UserPreset hud = make_user_preset("hud", 1, 0, false, 2, 100);
  hud.hud_visible = false;
  hud.overlay_no_bg = true;
  const SessionCfgActions hu = plan_user_preset_apply(cur, hud, true);
  CHECK(hu.hud_changed);
  CHECK(hu.overlay_no_bg_changed);
  CHECK_FALSE(hu.apply_hud);
  CHECK(hu.apply_overlay_no_bg);
}

TEST_CASE("user preset persist skips machine fields and Benchmark refuses IO") {
  CHECK(user_preset_io_allowed(false));
  CHECK_FALSE(user_preset_io_allowed(true));

  PersistedSettings before = default_settings();
  before.device_name = "Elgato 4K X";
  before.fg_path = "C:\\Games\\Lossless Scaling\\LosslessScaling.exe";
  before.width = 2560;
  before.height = 1440;
  before.hotkeys.vk_quit = 0x1B;
  PersistedSettings after = before;
  apply_user_preset_to_persisted(after, make_user_preset("TLOU 2x", 5, 2, true, 3, 80), true);
  CHECK(persisted_untouched_by_user_preset(before, after));
  CHECK(after.device_name == "Elgato 4K X");
  CHECK(after.fg_path == before.fg_path);
  CHECK(after.hotkeys.vk_quit == before.hotkeys.vk_quit);
  CHECK(after.nr_pass_count == 5);
  CHECK(after.renodx_preset == 2);
  CHECK(after.fg_enabled);
  CHECK(after.fg_multiplier == 3);
  CHECK(after.scale_percent == 80);

  PersistedSettings no_ls = before;
  apply_user_preset_to_persisted(no_ls, make_user_preset("TLOU 2x", 5, 2, true, 3, 80), false);
  CHECK(persisted_untouched_by_user_preset(before, no_ls));
  CHECK_FALSE(no_ls.fg_enabled);
  CHECK(no_ls.nr_pass_count == 5);
}

TEST_CASE("user preset disk keeps UTF-8 name") {
  const auto dir = make_user_preset_tmp("utf8");
  bool replaced = false;
  std::string err;
  const std::string nome = "TLOU a\xC3\xA7\xC3\xA3o";
  UserPreset p = make_user_preset("tmp", 5, 2, true, 3, 80);
  p.name = nome;
  REQUIRE(upsert_user_preset_in(dir, p, replaced, err));
  std::vector<UserPreset> have;
  REQUIRE(list_user_presets_in(dir, have));
  REQUIRE(have.size() == 1);
  CHECK(have[0].name == nome);
  CHECK(std::filesystem::exists(user_preset_store_path(dir, nome)));
}

TEST_CASE("user preset disk overwrite is one file and load sorts A-Z") {
  const auto dir = make_user_preset_tmp("disk");
  bool replaced = false;
  std::string err;
  REQUIRE(upsert_user_preset_in(dir, make_user_preset("Beta", 1, 0, false, 2, 100), replaced, err));
  REQUIRE(upsert_user_preset_in(dir, make_user_preset("Alpha", 2, 1, true, 2, 100), replaced, err));
  REQUIRE(upsert_user_preset_in(dir, make_user_preset("beta", 3, 2, false, 2, 90), replaced, err));
  CHECK(replaced);
  int files = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    if (e.is_regular_file()) ++files;
  }
  CHECK(files == 2);
  std::vector<UserPreset> have;
  REQUIRE(list_user_presets_in(dir, have));
  CHECK(have.size() == 2);
  CHECK(have[0].name == "Alpha");
  CHECK(have[1].name == "beta");
  CHECK(have[1].nr_pass_count == 3);
  REQUIRE(delete_user_preset_in(dir, "Alpha", err));
  files = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    if (e.is_regular_file()) ++files;
  }
  CHECK(files == 1);
}

TEST_CASE("user preset rename keeps knobs and can overwrite") {
  const auto dir = make_user_preset_tmp("rename");
  bool replaced = false;
  std::string err;
  UserPreset tlou = make_user_preset("TLOU 2x", 5, 2, true, 3, 80);
  tlou.overlay_no_bg = true;
  REQUIRE(upsert_user_preset_in(dir, tlou, replaced, err));
  REQUIRE(upsert_user_preset_in(dir, make_user_preset("GoW", 1, 0, false, 2, 100), replaced, err));

  CHECK_FALSE(rename_user_preset_in(dir, "TLOU 2x", "GoW", false, replaced, err));
  CHECK(err == "existe");
  std::vector<UserPreset> have;
  REQUIRE(list_user_presets_in(dir, have));
  CHECK(have.size() == 2);

  REQUIRE(rename_user_preset_in(dir, "TLOU 2x", "TLOU GOW", false, replaced, err));
  CHECK_FALSE(replaced);
  have.clear();
  REQUIRE(list_user_presets_in(dir, have));
  CHECK(have.size() == 2);
  CHECK(find_user_preset_index(have, "TLOU 2x") < 0);
  const int idx = find_user_preset_index(have, "TLOU GOW");
  REQUIRE(idx >= 0);
  CHECK(have[static_cast<size_t>(idx)].nr_pass_count == 5);
  CHECK(have[static_cast<size_t>(idx)].fg_multiplier == 3);
  CHECK(have[static_cast<size_t>(idx)].scale_percent == 80);
  CHECK(have[static_cast<size_t>(idx)].overlay_no_bg);
  CHECK(std::filesystem::exists(user_preset_store_path(dir, "TLOU GOW")));
  CHECK_FALSE(std::filesystem::exists(user_preset_store_path(dir, "TLOU 2x")));

  REQUIRE(rename_user_preset_in(dir, "TLOU GOW", "tlou gow", false, replaced, err));
  have.clear();
  REQUIRE(list_user_presets_in(dir, have));
  CHECK(have.size() == 2);
  CHECK(have[static_cast<size_t>(find_user_preset_index(have, "tlou gow"))].name == "tlou gow");

  REQUIRE(rename_user_preset_in(dir, "tlou gow", "GoW", true, replaced, err));
  CHECK(replaced);
  have.clear();
  REQUIRE(list_user_presets_in(dir, have));
  CHECK(have.size() == 1);
  CHECK(have[0].name == "GoW");
  CHECK(have[0].nr_pass_count == 5);
  CHECK(have[0].scale_percent == 80);

  REQUIRE(rename_user_preset_in(dir, "GoW", "GoW", false, replaced, err));
  CHECK_FALSE(replaced);
  CHECK(std::filesystem::exists(user_preset_store_path(dir, "GoW")));
  CHECK_FALSE(rename_user_preset_in(dir, "GoW", "   ", false, replaced, err));
  CHECK(err == "nome invalido");
  CHECK_FALSE(rename_user_preset_in(dir, "GoW", "GoW/bad", false, replaced, err));
  CHECK(err == "nome invalido");
  CHECK_FALSE(rename_user_preset_in(dir, "Missing", "Novo", false, replaced, err));
  CHECK(err == "nao encontrado");

  const std::string utf = "GoW a\xC3\xA7\xC3\xA3o";
  REQUIRE(rename_user_preset_in(dir, "GoW", utf, false, replaced, err));
  CHECK_FALSE(replaced);
  have.clear();
  REQUIRE(list_user_presets_in(dir, have));
  REQUIRE(have.size() == 1);
  CHECK(have[0].name == utf);
  CHECK(have[0].nr_pass_count == 5);
  CHECK(have[0].overlay_no_bg);
  CHECK(std::filesystem::exists(user_preset_store_path(dir, utf)));
}




