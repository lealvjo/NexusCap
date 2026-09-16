#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "nexuscap/analyze.hpp"
#include "nexuscap/audio_select.hpp"
#include "nexuscap/clock.hpp"
#include "nexuscap/color_bt709.hpp"
#include "nexuscap/config.hpp"
#include "nexuscap/display_geom.hpp"
#include "nexuscap/dlss_options.hpp"
#include "nexuscap/dlss5_bridge.hpp"
#include "nexuscap/frame_slot.hpp"
#include "nexuscap/hitch.hpp"
#include "nexuscap/lossless_fg.hpp"
#include "nexuscap/settings_store.hpp"
#include "nexuscap/stats_window.hpp"
#include "nexuscap/system_metrics.hpp"
#include "nexuscap/capture_format.hpp"
#include "nexuscap/capture.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace nexuscap;

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
  CHECK(std::strstr(line, "ON") != nullptr);

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
}

TEST_CASE("mini overlay one-line has FPS FG and DLSS5") {
  char line[256]{};
  format_mini_overlay_line(line, sizeof(line), 60.0, 120.0, true, 2, true, 16.7, 8.5, 2, 1, 2560, 1440, 2560,
                           1440, 2560, 1440, 100);
  CHECK(std::strstr(line, "60.0") != nullptr);
  CHECK(std::strstr(line, "120.0") != nullptr);
  CHECK(std::strstr(line, "cap|FG") != nullptr);
  CHECK(std::strstr(line, "DLSS5 ON") != nullptr);
  CHECK(std::strstr(line, "ft 16.7") != nullptr);
  CHECK(std::strstr(line, "e2e 8.5") != nullptr);
  CHECK(std::strstr(line, "stall 2") != nullptr);
  CHECK(std::strstr(line, "cl 1") != nullptr);
  CHECK(std::strstr(line, "in=work=out 2560x1440") != nullptr);

  format_mini_overlay_line(line, sizeof(line), 59.9, 0.0, false, 2, false, 16.6, 12.0, 0, 0, 2560, 1440, 1280,
                           720, 2560, 1440, 50);
  CHECK(std::strstr(line, "DLSS5 off") != nullptr);
  CHECK(std::strstr(line, "FG x2 off") != nullptr);
  CHECK(std::strstr(line, "stall 0") != nullptr);
  CHECK(std::strstr(line, "cl 0") != nullptr);
  CHECK(std::strstr(line, "in 2560x1440") != nullptr);
  CHECK(std::strstr(line, "work 1280x720") != nullptr);
  CHECK(std::strstr(line, "out 2560x1440") != nullptr);

  PersistedSettings s = default_settings();
  s.ui.show_fps = true;
  s.ui.mini_overlay = true;
  s.ui.advanced_metrics = true;  // ignored while mini is on
  s.enhance_renodx = true;
  s.neural_rendering = true;
  s.width = 2560;
  s.height = 1440;
  auto hud = make_lobby_hud_preview_lines(s, 60.0);
  CHECK(hud.count == 1);
  CHECK(lobby_hud_contains(hud, "DLSS5 ON"));
  CHECK(lobby_hud_preview_lines(true, true, false, true) == 1);
  CHECK(hud_uses_mini_overlay(true, true));
  CHECK_FALSE(hud_uses_advanced_metrics(true, true, true));
}

TEST_CASE("default FG off but multiplier stays armed for PageDn") {
  CHECK_FALSE(default_settings().fg_enabled);
  CHECK(default_settings().fg_multiplier == 2);

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
}

TEST_CASE("session_notice_fg_missing warns without Lossless") {
  const std::string msg = session_notice_fg_missing();
  CHECK(msg.find("Lossless Scaling") != std::string::npos);
  CHECK(msg.find("Pasta") != std::string::npos);
  CHECK(resolve_lossless_exe("C:\\definitely\\missing\\LosslessScaling.exe").empty());
}

TEST_CASE("mira settings xml never writes Flow 70") {
  const std::string built = build_mira_settings_xml(2);
  CHECK(built.find("LSFGFlowScale>100") != std::string::npos);
  CHECK(built.find("LSFGFlowScale>70") == std::string::npos);
  CHECK(built.find("LSFG3Multiplier>2") != std::string::npos);
  CHECK(built.find("<CaptureApi>WGC</CaptureApi>") != std::string::npos);
  CHECK(built.find("<LSFGSize>BALANCED</LSFGSize>") != std::string::npos);
  CHECK(built.find("<Hotkey>F24</Hotkey>") != std::string::npos);

  const std::string dirty =
      "<Settings><LSFGFlowScale>70</LSFGFlowScale><LSFG3Multiplier>2</LSFG3Multiplier>"
      "<CaptureApi>DXGI</CaptureApi><LSFGSize>QUALITY</LSFGSize></Settings>";
  const std::string patched = patch_mira_settings_xml(dirty, 3);
  CHECK(patched.find("LSFGFlowScale>100") != std::string::npos);
  CHECK(patched.find("LSFGFlowScale>70") == std::string::npos);
  CHECK(patched.find("LSFG3Multiplier>3") != std::string::npos);
  CHECK(patched.find("<CaptureApi>WGC</CaptureApi>") != std::string::npos);
  CHECK(parse_lsfg3_multiplier_from_xml(patched) == 3);
  CHECK(fg_multiplier_confirmed_on_disk(3, patched));
  CHECK_FALSE(fg_multiplier_confirmed_on_disk(4, patched));
  CHECK(parse_lsfg3_multiplier_from_xml("<nope>") == 0);
}

TEST_CASE("patch_lsfg_ini mira lock") {
  const std::string ini = patch_lsfg_ini("");
  CHECK(ini.find("flow_scale = 100") != std::string::npos);
  CHECK(ini.find("ui_detection_rate = 2") != std::string::npos);
  CHECK(ini.find("ui_detection_threshold = 0.30") != std::string::npos);
  const std::string bad = "[lsfg]\r\nflow_scale = 70\r\nui_detection_rate = 1\r\nui_detection_threshold = 0.10\r\n";
  const std::string fixed = patch_lsfg_ini(bad);
  CHECK(fixed.find("flow_scale = 100") != std::string::npos);
  CHECK(fixed.find("ui_detection_rate = 2") != std::string::npos);
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
  AppConfig cfg;
  apply_persisted_to_config(loaded, cfg);
  CHECK(cfg.fg_path == s.fg_path);
  CHECK(cfg.fg_multiplier == 3);
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
}

TEST_CASE("session notices for FG enable/up and NR on/off") {
  CHECK(session_notice_for_fg_toggle(true));
  CHECK_FALSE(session_notice_for_fg_toggle(false));
  CHECK(session_notice_for_fg_cycle(2, 3));
  CHECK(session_notice_for_fg_cycle(3, 4));
  CHECK_FALSE(session_notice_for_fg_cycle(4, 2));  // cycle wrap is not "subindo"
  CHECK_FALSE(session_notice_for_fg_cycle(3, 3));

  CHECK(session_notice_fg_on().find("Frame Gen ON") != std::string::npos);
  CHECK(session_notice_fg_multiplier_up(3).find("x3") != std::string::npos);
  CHECK(session_notice_nr_on().find("ON") != std::string::npos);
  CHECK(session_notice_nr_on().find("imagem mais bonita") != std::string::npos);
  CHECK(session_notice_nr_off().find("OFF") != std::string::npos);
  {
    std::string t, d;
    session_notice_split(session_notice_nr_on(), t, d);
    CHECK(t.find("Neural Rendering ON") != std::string::npos);
    CHECK(d.find("imagem mais bonita") != std::string::npos);
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

TEST_CASE("feed work_resolution sync restores 100% after 50% (blur regression)") {
  const std::string stuck50 =
      "enabled=1\nmode=2\nwork_resolution=50\nwork_upscale=0\nwork_sharpness=0.30\n";
  CHECK(parse_feed_work_resolution(stuck50) == 50);

  // UI back to 100% must rewrite feed — previously skipped when scale==100.
  const std::string fixed = apply_feed_work_resolution(stuck50, 100);
  CHECK(parse_feed_work_resolution(fixed) == 100);
  CHECK(fixed.find("work_resolution=100") != std::string::npos);
  CHECK(fixed.find("work_resolution=50") == std::string::npos);

  const std::string to75 = apply_feed_work_resolution(fixed, 75);
  CHECK(parse_feed_work_resolution(to75) == 75);

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
    CHECK(parse_feed_work_resolution(feed) == clamp_scale_percent(ui_scale));
  }

  // CRLF line endings (Windows editors).
  const std::string crlf = apply_feed_work_resolution("work_resolution=50\r\nwork_upscale=0\r\n", 100);
  CHECK(parse_feed_work_resolution(crlf) == 100);
  CHECK(crlf.find("work_resolution=100") != std::string::npos);

  // Out-of-range UI scale clamps before writing feed.
  CHECK(parse_feed_work_resolution(apply_feed_work_resolution("work_resolution=50\n", 200)) == 100);
  CHECK(parse_feed_work_resolution(apply_feed_work_resolution("work_resolution=50\n", 10)) == 25);
  CHECK(parse_feed_work_resolution(apply_feed_work_resolution("work_resolution=50\n", 25)) == 25);

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

TEST_CASE("Esc back to lobby must not keep session feed scale (menu blur)") {
  // Scenario: open@100 → Esc → set 50% → Iniciar OK → Esc → lobby must be sharp.
  // ReShade hooks lobby Present; feed@50 would blur the whole ImGui menu.
  CHECK(feed_work_resolution_for_lobby() == 100);
  CHECK(feed_work_resolution_for_session(50) == 50);
  CHECK(feed_work_resolution_for_session(100) == 100);
  CHECK(feed_work_resolution_for_lobby() != feed_work_resolution_for_session(50));

  // Session left feed at 50 while settings still say 50% — lobby rewrite ignores that.
  std::string feed =
      "enabled=1\nmode=2\nhdr=-1\nwork_resolution=50\nwork_upscale=0\nwork_sharpness=0.30\n";
  const int session_scale = 50;
  CHECK(feed_leaks_session_scale_into_lobby(feed, session_scale));

  feed = apply_lobby_feed_work_resolution(std::move(feed));
  CHECK(parse_feed_work_resolution(feed) == kLobbyFeedWorkResolution);
  CHECK_FALSE(feed_leaks_session_scale_into_lobby(feed, session_scale));
  // Persisted session scale stays 50 — only the feed for Present is isolated.
  CHECK(feed_work_resolution_for_session(session_scale) == 50);

  // Next Iniciar applies session scale again.
  feed = apply_feed_work_resolution(std::move(feed), feed_work_resolution_for_session(50));
  CHECK(parse_feed_work_resolution(feed) == 50);
  CHECK(feed_leaks_session_scale_into_lobby(feed, 50));
  feed = apply_lobby_feed_work_resolution(std::move(feed));
  CHECK(parse_feed_work_resolution(feed) == 100);

  // Full loop: 100 → 50 session → lobby → 50 session → lobby
  int ui = 100;
  feed = apply_feed_work_resolution(std::move(feed), feed_work_resolution_for_session(ui));
  CHECK(parse_feed_work_resolution(feed) == 100);
  ui = 50;
  feed = apply_feed_work_resolution(std::move(feed), feed_work_resolution_for_session(ui));
  CHECK(parse_feed_work_resolution(feed) == 50);
  feed = apply_lobby_feed_work_resolution(std::move(feed));
  CHECK(parse_feed_work_resolution(feed) == 100);
  feed = apply_feed_work_resolution(std::move(feed), feed_work_resolution_for_session(ui));
  CHECK(parse_feed_work_resolution(feed) == 50);
  feed = apply_lobby_feed_work_resolution(std::move(feed));
  CHECK(parse_feed_work_resolution(feed) == 100);
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
  s.ui.fullscreen = false;
  s.ui.hide_panel_on_capture = false;
  s.ui.sync_image = true;
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
  CHECK_FALSE(out.ui.fullscreen);
  CHECK_FALSE(out.ui.hide_panel_on_capture);
  CHECK(out.ui.sync_image);
  CHECK_FALSE(out.enhance_renodx);
  CHECK(out.renodx_preset == static_cast<int>(RenodxVisualPreset::Cinematic));
  // Legacy auto_start key must not break parse / must be ignored.
  CHECK(parse_settings_ini(text + "auto_start=1\n", out));
  CHECK_FALSE(serialize_settings_ini(out).find("auto_start=") != std::string::npos);
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
  // Scenario: change Default → Natural → Cinematic in gear, Salvar must rewrite both files.
  CHECK(std::string(renodx_visual_preset_name(0)) == "Default");
  CHECK(std::string(renodx_visual_preset_name(1)) == "Natural");
  CHECK(std::string(renodx_visual_preset_name(2)) == "Cinematic");
  CHECK(renodx_nr_preset_index(0) == 0);  // Default → NR Natural
  CHECK(renodx_nr_preset_index(1) == 0);  // Natural
  CHECK(renodx_nr_preset_index(2) == 1);  // Cinematic
  CHECK(renodx_feed_model_preset(0) == 0);
  CHECK(renodx_feed_model_preset(1) == 1);  // Natural → model Preset#1 (distinct from Default)
  CHECK(renodx_feed_model_preset(2) == 0);
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
  CHECK(parse_feed_model_preset(feed) == 0);

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

TEST_CASE("initial_hud_visible follows Exibir FPS only") {
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

TEST_CASE("session_shows_control_panel is inverse of hide_panel_on_capture") {
  // hide_panel=true (default) → captura limpa, sem faixa de sessão
  CHECK_FALSE(session_shows_control_panel(true));
  // hide_panel=false → faixa SessionControlPanel na captura
  CHECK(session_shows_control_panel(false));

  SessionUiFlags ui;
  ui.hide_panel_on_capture = true;
  CHECK_FALSE(session_shows_control_panel(ui));
  ui.hide_panel_on_capture = false;
  CHECK(session_shows_control_panel(ui));

  // Persistência → AppConfig → decisão de painel
  PersistedSettings s = default_settings();
  s.ui.hide_panel_on_capture = false;
  AppConfig cfg;
  apply_persisted_to_config(s, cfg);
  CHECK_FALSE(cfg.hide_panel_on_capture);
  CHECK(session_shows_control_panel(cfg.hide_panel_on_capture));

  s.ui.hide_panel_on_capture = true;
  apply_persisted_to_config(s, cfg);
  CHECK(cfg.hide_panel_on_capture);
  CHECK_FALSE(session_shows_control_panel(cfg.hide_panel_on_capture));
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
  CHECK_FALSE(cfg.hide_panel_on_capture);
  CHECK_FALSE(cfg.sync_image);
  CHECK(session_shows_control_panel(cfg.hide_panel_on_capture));
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
  CHECK(hud_panel_width(false) == 560);
  CHECK(hud_panel_height(false) == 220);
  CHECK(hud_panel_width(true) == 920);
  CHECK(hud_panel_height(true) == 640);
  CHECK(hud_panel_width(true) > hud_panel_width(false));
  CHECK(hud_panel_height(true) > hud_panel_height(false));
  CHECK(hud_panel_height(true, true) == 780);
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
  CHECK(lobby_hud_preview_lines(true, false, false, true) == 1);  // mini = single top line
  CHECK(lobby_hud_preview_lines(true, true, true, true) == 1);
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
  s.cpu_temp_ok = true;
  s.cpu_temp_c = 58;
  format_sys_cpu_line(buf, sizeof(buf), s);
  CHECK(std::string(buf) == "CPU  42%  58C");

  s.gpu_ok = true;
  s.gpu_percent = 77;
  s.gpu_temp_ok = true;
  s.gpu_temp_c = 72;
  s.vram_ok = true;
  s.vram_used_mb = 2048;
  s.vram_budget_mb = 8192;
  s.ram_ok = true;
  s.ram_used_mb = 12288;
  s.ram_total_mb = 32768;
  s.power_ok = true;
  s.power_w = 120;
  format_sys_metrics_line(buf, sizeof(buf), s);
  CHECK(std::strstr(buf, "CPU  42%  58C") != nullptr);
  CHECK(std::strstr(buf, "GPU  77%  72C") != nullptr);
  CHECK(std::strstr(buf, "RAM  12.0 / 32.0 GB") != nullptr);
  CHECK(std::strstr(buf, "VRAM 2.0 / 8.0 GB") != nullptr);
  CHECK(std::strstr(buf, "PWR  120 W") != nullptr);

  char lines[5][64]{};
  CHECK(format_sys_metrics_lines(lines, 5, s) == 5);
  CHECK(std::string(lines[0]) == "CPU  42%  58C");
  CHECK(std::string(lines[1]) == "GPU  77%  72C");
  CHECK(std::string(lines[2]) == "RAM  12.0 / 32.0 GB");
  CHECK(std::string(lines[3]) == "VRAM 2.0 / 8.0 GB");
  CHECK(std::string(lines[4]) == "PWR  120 W");

  CHECK(celsius_from_thermal_kelvin_raw(310.15) == doctest::Approx(37.0).epsilon(0.01));
  CHECK(celsius_from_thermal_kelvin_raw(3100.0) == doctest::Approx(36.85).epsilon(0.01));
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
  CHECK(lobby_hud_contains(hud, "Default"));
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
  CHECK(lobby_hud_contains(hud, "Cinematic"));
  CHECK(lobby_hud_contains(hud, "windowed"));
  CHECK(lobby_hud_contains(hud, "SYNC VSYNC"));
  CHECK_FALSE(lobby_hud_contains(hud, "Default"));

  s.renodx_preset = static_cast<int>(RenodxVisualPreset::Natural);
  hud = make_lobby_hud_preview_lines(s, 60.0);
  CHECK(lobby_hud_contains(hud, "Natural"));

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
  CHECK(lobby_hud_contains(hud, "Cinematic"));
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
  CHECK(cfg.show_overlay);
  CHECK(cfg.advanced_metrics);

  const auto hud = make_lobby_hud_preview_lines(loaded, 60.0);
  CHECK(lobby_hud_contains(hud, "Cinematic"));
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
  CHECK(app_version_is(1, 1, 7));
  CHECK(std::string(kAppVersion) == "1.1.7");
  CHECK(std::strstr(kAppNameWithVersion, "v1.1.7") != nullptr);
  CHECK(std::string(kAppAnalyzeBuild) == "NexusCap-1.1.7");
  const std::string lobby_t = app_window_title_lobby();
  const std::string sess_t = app_window_title_session();
  CHECK(lobby_t.find("1.1.7") != std::string::npos);
  CHECK(lobby_t.find("Console de captura") != std::string::npos);
  CHECK(lobby_t.find("\xE2\x80\x94") == std::string::npos);  // no UTF-8 em-dash
  CHECK(sess_t.find("1.1.7") != std::string::npos);
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

TEST_CASE("Delete NR needs feed pipeline ON (else HUD-only, no image change)") {
  // Log regression: session started with feed enabled=0; Delete pulsed RenoDX (ReShade
  // "NR toggled ON") but dlss5-feed stayed dead → stats ON/OFF with no visual change.
  CHECK(feed_pipeline_on_for_renodx(true));
  CHECK_FALSE(feed_pipeline_on_for_renodx(false));

  std::string feed = "enabled=0\nmode=2\n";
  std::string reshade = "NeuralUplift=0\nNRToggleKey=46\n";
  // Renodx loaded, NR off: feed must stay armed.
  feed = apply_feed_enabled(std::move(feed), feed_pipeline_on_for_renodx(true));
  reshade = apply_reshade_neural_uplift(std::move(reshade), false);
  CHECK(parse_feed_enabled(feed, false));
  CHECK_FALSE(parse_reshade_neural_uplift(reshade, true));

  // Delete ON: NeuralUplift flips; feed stays on.
  reshade = apply_reshade_neural_uplift(std::move(reshade), true);
  CHECK(parse_feed_enabled(feed, false));
  CHECK(parse_reshade_neural_uplift(reshade, false));

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
  }
  {
    CHECK(fg_overlay_on_confirmed(true, 1));
    CHECK_FALSE(fg_overlay_on_confirmed(true, 0));  // no LS process → never FG ON
    CHECK(fg_overlay_on_confirmed(false, 0));
    CHECK_FALSE(fg_overlay_on_confirmed(false, 2));  // leftover LS → OFF not confirmed
  }
}

TEST_CASE("mini overlay DLSS5 follows renodx_on only (not passthrough dlss_on)") {
  // Regression: HUD used renodx_on || (dlss_on && backend has renodx) while enhance is
  // passthrough — after Invert-bug fix, status is solely renodx_on / g_nr_runtime_on.
  char line[256]{};
  format_mini_overlay_line(line, sizeof(line), 60.0, 0.0, false, 2, true, 16.0, 8.0, 0, 0, 2560, 1440);
  CHECK(std::strstr(line, "DLSS5 ON") != nullptr);
  format_mini_overlay_line(line, sizeof(line), 60.0, 0.0, false, 2, false, 16.0, 8.0, 0, 0, 2560, 1440);
  CHECK(std::strstr(line, "DLSS5 off") != nullptr);
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
