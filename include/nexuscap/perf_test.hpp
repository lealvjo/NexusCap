#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "nexuscap/config.hpp"
#include "nexuscap/hitch.hpp"
#include "nexuscap/lossless_fg.hpp"
#include "nexuscap/settings_store.hpp"
#include "nexuscap/stats_window.hpp"

namespace nexuscap {

inline constexpr int kPerfTestMaxSec = 40;
inline constexpr double kPerfTestFpsSlop = 0.5;
inline constexpr double kPerfTestHoldMaxMs = 8000.0;
// Se o pipeline nao ficar pronto, falha e volta ao lobby — o usuario nao espera para sempre.
inline constexpr double kPerfTestRevealMaxMs = 20000.0;
// Depois do NR pronto: nunca ficar em "A carregar" para sempre se o overlay mentir o FG.
inline constexpr double kPerfTestFgWaitMaxMs = 8000.0;
// Playhead preso no frame 0 so gera 1-3 evals. 1 EvaluateFeature ja prova que o NR ligou.
inline constexpr int kPerfTestNrMinEvals = 1;

inline bool perf_test_nr_runtime_live(bool nr_flag, bool nr_live) { return nr_flag && nr_live; }

inline bool perf_test_nr_evals_are_live(int evals, int min_evals = kPerfTestNrMinEvals) {
  return evals >= min_evals;
}

inline bool perf_test_dlss5_ready(bool nr_runtime_on, uint64_t presented, bool nr_live = true) {
  return perf_test_nr_runtime_live(nr_runtime_on, nr_live) && stats_frame_counts_toward_window(presented);
}

inline bool perf_test_fg_ready(bool want_fg, bool fg_on, bool fg_unavailable, double fg_wait_ms = 0) {
  if (!want_fg) return true;
  if (fg_on || fg_unavailable) return true;
  return fg_wait_ms + 1e-9 >= kPerfTestFgWaitMaxMs;
}

inline bool perf_test_params_ready(bool nr_runtime_on, uint64_t presented, bool want_fg, bool fg_on,
                                  bool fg_unavailable, double fg_wait_ms = 0, bool nr_live = true) {
  return perf_test_dlss5_ready(nr_runtime_on, presented, nr_live) &&
         perf_test_fg_ready(want_fg, fg_on, fg_unavailable, fg_wait_ms);
}

// Relógio com NR no ar + FG real. Se o FG nao confirmar, timeout — senão "A carregar" trava a tecla.
inline bool perf_test_should_start_clock(bool nr_runtime_on, uint64_t presented, bool want_fg, bool fg_on,
                                        bool fg_unavailable, double fg_wait_ms = 0, bool nr_live = true) {
  return perf_test_params_ready(nr_runtime_on, presented, want_fg, fg_on, fg_unavailable, fg_wait_ms, nr_live);
}

// "A carregar" so enquanto falta NR ou o FG ainda esta dentro da janela de espera.
inline bool perf_test_carregar_can_block(bool nr_ready, bool want_fg, bool fg_on, bool fg_unavailable,
                                        double fg_wait_ms) {
  if (!nr_ready) return true;
  return !perf_test_fg_ready(want_fg, fg_on, fg_unavailable, fg_wait_ms);
}

inline bool perf_test_should_reveal_window(bool clock_ready, double wait_ms) {
  // Sem timeout de revelar HUD: se o relogio nao comeca, falha (nao abre "A carregar").
  (void)wait_ms;
  return clock_ready;
}

// Nao deixar o cartao/janela de espera para sempre — volta ao lobby.
inline bool perf_test_should_fail_pipeline(bool clock_ready, double wait_ms) {
  return !clock_ready && wait_ms >= kPerfTestRevealMaxMs;
}

// Ordem do teardown: preto → hide → analyze → FG → D3D → DestroyWindow.
enum class SessionTeardownStep { PresentBlack = 0, HideWindow = 1, WriteAnalyze = 2, ShutdownFg = 3,
                                 ReleaseD3d = 4, DestroyHwnd = 5 };
inline bool session_teardown_step_before(SessionTeardownStep earlier, SessionTeardownStep later) {
  return static_cast<int>(earlier) < static_cast<int>(later);
}

// Clipe de 40s. O relogio conta 40s; rewind no EOS nao encerra o teste.
inline bool perf_test_should_stop(bool time_up) { return time_up; }

inline int perf_test_remain_sec(double now_ms, double t0_ms, int max_sec = kPerfTestMaxSec) {
  const int cap = max_sec > 0 ? max_sec : kPerfTestMaxSec;
  const double left = static_cast<double>(cap) - (now_ms - t0_ms) / 1000.0;
  if (left <= 0.0) return 0;
  int s = static_cast<int>(std::floor(left + 1e-9));
  if (s > cap) s = cap;
  return s;
}

inline void format_perf_test_timer(char* out, size_t n, int remain_sec) {
  if (!out || n == 0) return;
  if (remain_sec < 0) {
    std::snprintf(out, n, "A carregar");
    return;
  }
  int s = remain_sec;
  if (s > kPerfTestMaxSec) s = kPerfTestMaxSec;
  std::snprintf(out, n, "0:%02d", s);
}

inline const char* perf_test_lobby_title() { return "Benchmark"; }
inline const char* perf_test_lobby_button() { return "Benchmark"; }
inline const char* perf_test_lobby_start() { return "Iniciar benchmark"; }
inline const char* perf_test_lobby_blurb() {
  return "Funcao beta. A nota nao e um chute: e um modelo nosso, uma equacao com fps, present, stalls, escala e Frame Gen. Em cima disso o NexusCap monta uma sugestao inteligente (que escala, se o FG ajuda, qual Pass). O video e so para validar a carga, uns 40 segundos de gameplay. As imagens podem parecer um pouco menos nitidas, o resultado e real. O relogio so conta quando NR e FG ja tiverem ligado. A nota e do teu PC. O CFG HDMI nao muda.";
}
inline const char* perf_test_hz_title() { return "Simular captura em 30 ou 60 Hz"; }
inline const char* perf_test_hz_blurb() {
  return "O pino entrega o clipe como uma placa HDMI a 30 ou 60 Hz. A nota usa esta meta, nao e o Hz do monitor.";
}
inline const char* perf_test_audio_toggle() { return "Habilitar som no teste"; }
inline const char* perf_test_audio_hint() {
  return "Toca o audio original da captura de jogo na saida padrao. Desliga se nao quiseres barulho.";
}
inline const char* perf_test_counting_notice() { return "Benchmark a contar. Parametros carregados."; }

// Resultado no lobby: uma vez, so depois deste benchmark. Ficheiro last.txt nao reabre o cartao.
inline bool perf_test_should_popup_result(bool just_finished_benchmark, bool result_valid,
                                         bool file_popup = false) {
  return result_valid && (just_finished_benchmark || file_popup);
}

// HDMI: checkbox SESSÃO "Reportar performance". Nunca no caminho do Benchmark.
inline bool session_perf_popup_wanted(bool perf_report, bool perf_test) {
  return perf_report && !perf_test;
}

inline const char* session_perf_popup_title() {
  return "Reportar performance ao finalizar gameplay";
}

struct PerfTestPlan {
  int width = 1920;
  int height = 1080;
  double fps = 60.0;
  int scale_percent = 100;
  int nr_pass_count = 1;
  int renodx_preset = 0;
  int fg_multiplier = 1;  // 1 = off
  bool audio = true;      // som do clipe; so no plano do teste, nao no CFG HDMI
  std::string video;
};

enum class PerfTestGrade { Good = 0, Tight = 1, Fail = 2, PlayableFg = 3 };

struct PerfTestSample {
  double fps = 0;
  double p95_present_ms = 0;
  uint64_t stalls = 0;
  double target_fps = 60;
  int fg_multiplier = 1;
};

// Nao gradear pelo fps_smooth do vsync (60 falso). Precisa da janela do teste.
inline constexpr uint64_t kPerfTestMinScoreFrames = 180;

struct PerfScoreAcc {
  std::vector<double> intervals;
  std::vector<double> presents;
  uint64_t stalls = 0;
  void reset() {
    intervals.clear();
    presents.clear();
    stalls = 0;
  }
  void push(double interval_ms, double present_ms, bool stall) {
    intervals.push_back(interval_ms);
    presents.push_back(present_ms);
    if (stall) ++stalls;
  }
  uint64_t frames() const { return static_cast<uint64_t>(intervals.size()); }
};

inline bool perf_test_score_ready(uint64_t frames, uint64_t min_n = kPerfTestMinScoreFrames) {
  return frames >= min_n;
}

inline bool session_perf_result_ready(bool perf_report, bool perf_test, uint64_t frames) {
  return session_perf_popup_wanted(perf_report, perf_test) && perf_test_score_ready(frames);
}

inline double perf_test_p95(std::vector<double> v) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const double idx = 0.95 * static_cast<double>(v.size() - 1);
  const size_t lo = static_cast<size_t>(std::floor(idx));
  const size_t hi = static_cast<size_t>(std::ceil(idx));
  if (lo == hi) return v[lo];
  const double t = idx - static_cast<double>(lo);
  return v[lo] * (1.0 - t) + v[hi] * t;
}

inline PerfTestSample perf_test_sample_from_acc(const PerfScoreAcc& a, double target, int fg) {
  PerfTestSample s;
  s.target_fps = target > 1.0 ? target : 60.0;
  s.fg_multiplier = fg;
  if (a.intervals.empty()) return s;
  double sum = 0;
  for (double v : a.intervals) sum += v;
  s.fps = sum > 0.0 ? 1000.0 * static_cast<double>(a.intervals.size()) / sum : 0;
  s.p95_present_ms = perf_test_p95(a.presents);
  s.stalls = a.stalls;
  return s;
}

struct PerfTestResult {
  PerfTestPlan plan;
  PerfTestSample sample;
  PerfTestGrade grade = PerfTestGrade::Fail;
  std::string summary;
  bool valid = false;
  bool popup = false;  // 1 = mostrar no lobby; sobrevive ao relaunch da escala
  bool hdmi_session = false;  // titulo HDMI; o Benchmark continua com a nota
};

inline double clamp_perf_test_fps(double fps) { return fps < 45.0 ? 30.0 : 60.0; }

inline void clamp_perf_test_signal(int& w, int& h) {
  if (w >= 3200 || h >= 1800) {
    w = 3840;
    h = 2160;
    return;
  }
  if (w >= 2200 || h >= 1300) {
    w = 2560;
    h = 1440;
    return;
  }
  w = 1920;
  h = 1080;
}

inline const char* perf_test_signal_label(int w, int h) {
  if (w >= 3200 || h >= 1800) return "4K";
  if (w >= 2200 || h >= 1300) return "1440p";
  return "1080p";
}

inline PerfTestPlan clamp_perf_test_plan(PerfTestPlan p) {
  clamp_perf_test_signal(p.width, p.height);
  p.fps = clamp_perf_test_fps(p.fps);
  p.scale_percent = clamp_scale_percent(p.scale_percent);
  if (p.scale_percent < kMinFeedWorkResolution) p.scale_percent = kMinFeedWorkResolution;
  p.nr_pass_count = clamp_nr_pass_count(p.nr_pass_count);
  p.renodx_preset = clamp_renodx_visual_preset(p.renodx_preset);
  p.fg_multiplier = clamp_fg_multiplier(p.fg_multiplier);
  return p;
}

inline bool is_perf_test_video_ext(const std::string& path) {
  auto dot = path.find_last_of('.');
  if (dot == std::string::npos) return false;
  std::string e = path.substr(dot);
  for (char& c : e) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return e == ".mp4" || e == ".mkv" || e == ".mov" || e == ".webm";
}

inline std::string video_file_display_tail(const std::string& path, size_t max_chars = 52) {
  if (path.size() <= max_chars) return path;
  return "..." + path.substr(path.size() - (max_chars - 3));
}

inline bool find_newest_perf_test_video(const std::vector<std::string>& dirs, std::string& out) {
  out.clear();
  namespace fs = std::filesystem;
  fs::file_time_type best{};
  bool any = false;
  for (const auto& dir : dirs) {
    std::error_code ec;
    if (dir.empty() || !fs::exists(dir, ec)) continue;
    for (const auto& ent : fs::directory_iterator(dir, ec)) {
      if (ec) break;
      if (!ent.is_regular_file(ec)) continue;
      const std::string p = ent.path().string();
      if (!is_perf_test_video_ext(p)) continue;
      const auto t = ent.last_write_time(ec);
      if (ec) continue;
      if (!any || t > best) {
        best = t;
        out = p;
        any = true;
      }
    }
  }
  return any;
}

inline const char* perf_test_audio_filename() { return "perf_test.m4a"; }

inline std::string perf_test_audio_sidecar_path(const std::string& video_path) {
  if (video_path.empty()) return {};
  return (std::filesystem::path(video_path).parent_path() / perf_test_audio_filename()).string();
}

inline const char* perf_test_bundled_filename() { return "perf_test.mp4"; }

inline const char* perf_test_bundled_filename_for(int w, int h) {
  if (w >= 3200 || h >= 1800) return "perf_test_4k.mp4";
  if (w >= 2200 || h >= 1300) return "perf_test_1440.mp4";
  return "perf_test_1080.mp4";
}

inline std::string perf_test_bundled_path(const std::string& exe_dir) {
  if (exe_dir.empty()) return {};
  return exe_dir + "\\assets\\" + perf_test_bundled_filename();
}

inline std::string perf_test_bundled_path_for(const std::string& exe_dir, int w, int h) {
  if (exe_dir.empty()) return {};
  return exe_dir + "\\assets\\" + perf_test_bundled_filename_for(w, h);
}

inline std::vector<std::string> perf_test_search_dirs(const std::string& exe_dir) {
  std::vector<std::string> d;
  if (!exe_dir.empty()) {
    d.push_back(exe_dir + "\\assets");
    d.push_back(exe_dir);
    d.push_back(exe_dir + "\\samples");
  }
  return d;
}

inline bool resolve_perf_test_video(const std::string& exe_dir, int width, int height, std::string& out) {
  out.clear();
  namespace fs = std::filesystem;
  std::error_code ec;
  int w = width, h = height;
  clamp_perf_test_signal(w, h);
  const char* names[6];
  int n = 0;
  auto add = [&](const char* s) {
    if (!s) return;
    for (int i = 0; i < n; ++i)
      if (std::strcmp(names[i], s) == 0) return;
    if (n < 6) names[n++] = s;
  };
  add(perf_test_bundled_filename_for(w, h));
  if (w >= 3200) add("perf_test_1440.mp4");
  add("perf_test_1440.mp4");
  add("perf_test_1080.mp4");
  add(perf_test_bundled_filename());
  for (int i = 0; i < n; ++i) {
    const std::string in_assets = exe_dir.empty() ? std::string{} : exe_dir + "\\assets\\" + names[i];
    if (!in_assets.empty() && fs::exists(in_assets, ec)) {
      out = in_assets;
      return true;
    }
    const std::string beside = exe_dir.empty() ? std::string{} : exe_dir + "\\" + names[i];
    if (!beside.empty() && fs::exists(beside, ec)) {
      out = beside;
      return true;
    }
  }
  return find_newest_perf_test_video(perf_test_search_dirs(exe_dir), out);
}

inline bool resolve_perf_test_video(const std::string& exe_dir, std::string& out) {
  return resolve_perf_test_video(exe_dir, 1920, 1080, out);
}

inline double perf_test_present_budget_ms(double target_fps) {
  const double fps = target_fps > 1.0 ? target_fps : 60.0;
  // VSYNC Present(1) reports ~one frame. +1.5 ms so 16,7 ms @ 60 is Bom, not Apertado.
  return 1000.0 / fps + 1.5;
}

// Acc hitch: limiar efectivo da meta (30|60), nao 25 ms chumbado.
// Slack = o mesmo +1.5 do budget, para um lock a 40 Hz (25.0 ms) nao virar 800 stalls.
inline bool perf_test_is_stall(double interval_ms, double target_fps, double configured_ms = 25.0) {
  const double fps = target_fps > 1.0 ? target_fps : 60.0;
  const double hitch = effective_hitch_threshold_ms(configured_ms, fps);
  const double slack = perf_test_present_budget_ms(fps) - (1000.0 / fps);
  return interval_ms > hitch + slack;
}

inline const char* perf_test_grade_label(PerfTestGrade g) {
  switch (g) {
    case PerfTestGrade::Good:
      return "Bom";
    case PerfTestGrade::Tight:
      return "Apertado";
    case PerfTestGrade::PlayableFg:
      return "Jogavel com FG";
    default:
      return "Nao aguenta";
  }
}

inline const char* perf_result_popup_title(bool hdmi_session, PerfTestGrade grade) {
  return hdmi_session ? session_perf_popup_title() : perf_test_grade_label(grade);
}

inline const char* perf_test_fg_label(int fg_mult) {
  const int m = clamp_fg_multiplier(fg_mult);
  if (m < 2) return "FG off";
  if (m == 2) return "FG x2";
  if (m == 3) return "FG x3";
  return "FG x4";
}

inline double perf_test_fg_est_fps(double cap_fps, int fg_mult) {
  const int m = clamp_fg_multiplier(fg_mult);
  if (m < 2 || cap_fps <= 0) return 0;
  return cap_fps * static_cast<double>(m);
}

// Real frame + one FG frame: ~38 ms at 40 cap / x2, in the 30–40 ms band.
inline double perf_test_fg_latency_ms(double cap_fps, int fg_mult) {
  const double cap = cap_fps > 1.0 ? cap_fps : 30.0;
  const double fg = perf_test_fg_est_fps(cap, fg_mult);
  return (1000.0 / cap) + (fg > 1.0 ? 1000.0 / fg : 0.0);
}

inline PerfTestGrade grade_perf_test(const PerfTestSample& s) {
  const double floor_fps = s.target_fps - kPerfTestFpsSlop;
  const double budget = perf_test_present_budget_ms(s.target_fps);
  const bool cap_ok = s.fps + 1e-6 >= floor_fps;
  const bool fg_on = s.fg_multiplier >= 2;
  const double fg_fps = perf_test_fg_est_fps(s.fps, s.fg_multiplier);
  const bool fg_meta_ok = fg_on && fg_fps + 1e-6 >= floor_fps;
  if (cap_ok) {
    // Cap already holds the meta. Stalls with FG on = FG costing, not a Fail.
    // 1 hitch (reveal/rewind do clipe) com 60 travado nao e Apertado.
    if (s.stalls >= 3) return fg_on ? PerfTestGrade::Tight : PerfTestGrade::Fail;
    if (s.stalls > 1 || s.p95_present_ms > budget) return PerfTestGrade::Tight;
    return PerfTestGrade::Good;
  }
  if (fg_meta_ok && s.fps >= 20.0) return PerfTestGrade::PlayableFg;
  return PerfTestGrade::Fail;
}

// Fit on this box: gpu_ms ≈ 11.11·pass_work·(scale/100)^1.5. Online invert drops pass_work:
// next_scale = scale × (fps/meta)^(1/1.5) = scale × (fps/meta)^(2/3). Floor to 5 so 63→60, not 65.
inline constexpr double kPerfTestScalePow = 1.5;

inline double perf_test_pass_work(int nr_pass_count) {
  switch (clamp_nr_pass_count(nr_pass_count)) {
    case 4:
      return 1.5;
    case 2:
    case 5:
      return 2.0;
    case 3:
      return 3.0;
    default:
      return 1.0;
  }
}

inline int perf_test_floor_scale(double raw) {
  int v = static_cast<int>(std::floor(raw / 5.0 + 1e-9)) * 5;
  if (v < kMinFeedWorkResolution) return kMinFeedWorkResolution;
  if (v > kMaxScalePercent) return kMaxScalePercent;
  return v;
}

inline int perf_test_snap_scale(int v) { return perf_test_floor_scale(static_cast<double>(v)); }

inline bool perf_test_cadence_locked(double fps) {
  return std::fabs(fps - 40.0) < 0.35 || std::fabs(fps - 30.0) < 0.35 || std::fabs(fps - 20.0) < 0.35;
}

inline bool perf_test_scale_floor_hits_meta(int cur, double fps, double target) {
  if (fps <= 1.0 || target <= 1.0 || cur < kMinFeedWorkResolution) return false;
  const double gpu = 1000.0 / fps;
  const double gpu50 = gpu * std::pow(50.0 / static_cast<double>(cur), kPerfTestScalePow);
  return gpu50 <= (1000.0 / target) + 0.75;
}

inline int perf_test_suggest_scale(int cur, const PerfTestSample& s, PerfTestGrade g, double target) {
  if (cur < kMinFeedWorkResolution) cur = kMinFeedWorkResolution;
  if (g == PerfTestGrade::Good) return 0;
  if (g == PerfTestGrade::Tight) {
    if (cur <= kMinFeedWorkResolution) return 0;
    const double budget = perf_test_present_budget_ms(target > 1.0 ? target : 60.0);
    double load = 1.0;
    if (budget > 1.0 && s.p95_present_ms > 0) load = s.p95_present_ms / budget;
    if (s.stalls >= 3) load = (std::max)(load, 1.12);
    else if (s.stalls > 0) load = (std::max)(load, 1.06);
    if (load <= 1.0) return 0;
    const int want = perf_test_floor_scale(static_cast<double>(cur) * std::pow(1.0 / load, 1.0 / kPerfTestScalePow));
    if (want >= cur) return 0;
    return want;
  }
  if (s.fps <= 1.0 || target <= 1.0 || cur <= kMinFeedWorkResolution) return 0;
  if (s.fps + 1e-6 >= target - kPerfTestFpsSlop) return 0;
  const int want =
      perf_test_floor_scale(static_cast<double>(cur) * std::pow(s.fps / target, 1.0 / kPerfTestScalePow));
  if (want >= cur) return 0;
  return want;
}

inline std::string perf_test_next_lever(const PerfTestPlan& p, const char* why) {
  char b[220];
  if (p.nr_pass_count > 1) {
    std::snprintf(b, sizeof(b), "Para melhores resultados, testa Pass 1x%s.", why);
    return b;
  }
  if (p.width >= 3200) {
    std::snprintf(b, sizeof(b), "Para melhores resultados, testa em 1440p%s.", why);
    return b;
  }
  if (p.width >= 2200) {
    std::snprintf(b, sizeof(b), "Para melhores resultados, testa em 1080p%s.", why);
    return b;
  }
  if (p.fps >= 45.0) {
    std::snprintf(b, sizeof(b), "Para melhores resultados, testa a meta 30 fps%s.", why);
    return b;
  }
  return {};
}

// Smallest FG xN (from `from_mult`) whose cap×N still hits the meta. Cap < 20 stays Fail.
inline int perf_test_fg_mult_hits_meta(double cap_fps, double target, int from_mult = 2) {
  if (cap_fps < 20.0 || target <= 1.0) return 0;
  const double floor_fps = target - kPerfTestFpsSlop;
  int m = from_mult < 2 ? 2 : from_mult;
  if (m > 4) return 0;
  for (; m <= 4; ++m) {
    if (perf_test_fg_est_fps(cap_fps, m) + 1e-6 >= floor_fps) return m;
  }
  return 0;
}

inline std::string perf_test_fg_alt_advice(double cap_fps, double target, int current_fg) {
  const int start = current_fg >= 2 ? current_fg + 1 : 2;
  const int m = perf_test_fg_mult_hits_meta(cap_fps, target, start);
  if (m < 2) return {};
  char b[160];
  if (current_fg < 2) {
    std::snprintf(b, sizeof(b), "ou FG x%d chega a meta gerada (latencia maior).", m);
  } else {
    std::snprintf(b, sizeof(b), "ou testa FG x%d.", m);
  }
  return b;
}

inline std::string perf_test_join_advice(const std::string& native, const std::string& fg) {
  if (native.empty()) return fg;
  if (fg.empty()) return native;
  return native + " " + fg;
}

inline std::string perf_test_scale_tip(int next, int cur, double fps, double target, bool tight) {
  char b[220];
  if (tight) {
    std::snprintf(b, sizeof(b),
                  "Para melhores resultados, reduz a escala para %d%% (present apertado aos %d%%).", next, cur);
  } else {
    std::snprintf(b, sizeof(b),
                  "Para melhores resultados, reduz a escala para %d%% (%.0f fps nesta escala, meta %.0f).", next,
                  fps, target);
  }
  return b;
}

inline std::string perf_test_advice(const PerfTestPlan& raw, const PerfTestSample& s, PerfTestGrade g) {
  const PerfTestPlan p = clamp_perf_test_plan(raw);
  const double target = s.target_fps > 1.0 ? s.target_fps : p.fps;
  const std::string fg_alt =
      (g == PerfTestGrade::Fail) ? perf_test_fg_alt_advice(s.fps, target, p.fg_multiplier) : std::string{};
  if (g == PerfTestGrade::Good) {
    if (p.fps < 45.0 && s.fps + 1e-6 >= 60.0 - kPerfTestFpsSlop) {
      return "O cap ja segura 60. Podes testar a meta 60.";
    }
    return {};
  }
  if (g == PerfTestGrade::Tight && p.fg_multiplier >= 2) {
    const double budget = perf_test_present_budget_ms(target);
    if (s.p95_present_ms > budget) {
      const int next = perf_test_suggest_scale(p.scale_percent, s, g, target);
      if (next > 0) return perf_test_scale_tip(next, p.scale_percent, s.fps, target, true);
    }
    return {};
  }
  if (g != PerfTestGrade::Tight && p.nr_pass_count > 1 && s.fps > 1.0 &&
      !perf_test_scale_floor_hits_meta(p.scale_percent, s.fps, target)) {
    return perf_test_join_advice(
        "Para melhores resultados, testa Pass 1x (nesta amostra o Pass limita; cortar escala nao chega).", fg_alt);
  }
  const int next = perf_test_suggest_scale(p.scale_percent, s, g, target);
  if (next > 0) {
    return perf_test_join_advice(perf_test_scale_tip(next, p.scale_percent, s.fps, target, g == PerfTestGrade::Tight),
                                fg_alt);
  }
  if (g == PerfTestGrade::Tight) {
    const std::string more = perf_test_next_lever(p, " (mais folga)");
    return more.empty() ? std::string{} : more;
  }
  if (g == PerfTestGrade::PlayableFg) {
    const std::string native = perf_test_next_lever(p, " para tentar nativo");
    return native.empty() ? std::string{} : native;
  }
  const std::string fail = perf_test_next_lever(p, "");
  if (!fail.empty()) return perf_test_join_advice(fail, fg_alt);
  if (!fg_alt.empty()) {
    const char* rest = fg_alt.compare(0, 3, "ou ") == 0 ? fg_alt.c_str() + 3 : fg_alt.c_str();
    return std::string("Para melhores resultados, ") + rest;
  }
  if (p.fg_multiplier >= 2)
    return "Para melhores resultados, desliga o Frame Gen. Neste minimo o FG nao salva o cap.";
  return "Neste minimo (1080p, 50%, Pass 1x, 30 fps) a maquina nao segura este plano.";
}

inline std::string perf_test_advice(const PerfTestPlan& raw, PerfTestGrade g) {
  return perf_test_advice(raw, PerfTestSample{}, g);
}

inline void fmt_br_1(char* out, size_t n, double v) {
  std::snprintf(out, n, "%.1f", v);
  for (char* p = out; *p; ++p) {
    if (*p == '.') *p = ',';
  }
}

inline std::string format_perf_test_card(const PerfTestPlan& raw, const PerfTestSample& s, PerfTestGrade g) {
  const PerfTestPlan p = clamp_perf_test_plan(raw);
  const double target = s.target_fps > 1.0 ? s.target_fps : p.fps;
  const bool cap_ok = s.fps + 1e-6 >= (target - kPerfTestFpsSlop);
  const bool fg_on = p.fg_multiplier >= 2;
  char fpsb[32];
  char presb[32];
  char fgb[32];
  char latb[32];
  fmt_br_1(fpsb, sizeof(fpsb), s.fps);
  fmt_br_1(presb, sizeof(presb), s.p95_present_ms);
  fmt_br_1(fgb, sizeof(fgb), perf_test_fg_est_fps(s.fps, p.fg_multiplier));
  fmt_br_1(latb, sizeof(latb), perf_test_fg_latency_ms(s.fps, p.fg_multiplier));
  char buf[880];
  if (g == PerfTestGrade::PlayableFg && fg_on) {
    std::snprintf(buf, sizeof(buf),
                  "Com %s, %s, %.0f fps, escala %d%%, %s, o resultado foi %s (%s fps no cap / ~%s FG, latencia ~%s ms). O fps extra e gerado. Da para jogar, mas nao e %.0f nativo.",
                  nr_pass_count_short(p.nr_pass_count), perf_test_signal_label(p.width, p.height), p.fps,
                  p.scale_percent, perf_test_fg_label(p.fg_multiplier), perf_test_grade_label(g), fpsb, fgb, latb,
                  p.fps);
  } else if (g == PerfTestGrade::Good && fg_on) {
    std::snprintf(buf, sizeof(buf),
                  "Com %s, %s, %.0f fps, escala %d%%, %s, o resultado foi %s (%s fps nativo). FG da ~%s extra; latencia ~%s ms. FG opcional.",
                  nr_pass_count_short(p.nr_pass_count), perf_test_signal_label(p.width, p.height), p.fps,
                  p.scale_percent, perf_test_fg_label(p.fg_multiplier), perf_test_grade_label(g), fpsb, fgb, latb);
  } else if (g == PerfTestGrade::Tight && fg_on && s.stalls > 0) {
    std::snprintf(buf, sizeof(buf),
                  "Com %s, %s, %.0f fps, escala %d%%, %s, o resultado foi %s (%s fps no cap / ~%s FG, present %s ms, %llu stalls). O cap segura %.0f. O FG esta a custar. Desliga o FG e ficas nativo.",
                  nr_pass_count_short(p.nr_pass_count), perf_test_signal_label(p.width, p.height), p.fps,
                  p.scale_percent, perf_test_fg_label(p.fg_multiplier), perf_test_grade_label(g), fpsb, fgb, presb,
                  static_cast<unsigned long long>(s.stalls), p.fps);
  } else if (g == PerfTestGrade::Tight) {
    if (fg_on) {
      std::snprintf(buf, sizeof(buf),
                    "Com %s, %s, %.0f fps, escala %d%%, %s, o resultado foi %s (%s fps no cap / ~%s FG, present %s ms, %llu stalls). Chega aos %.0f, mas apertado.",
                    nr_pass_count_short(p.nr_pass_count), perf_test_signal_label(p.width, p.height), p.fps,
                    p.scale_percent, perf_test_fg_label(p.fg_multiplier), perf_test_grade_label(g), fpsb, fgb, presb,
                    static_cast<unsigned long long>(s.stalls), p.fps);
    } else {
      std::snprintf(buf, sizeof(buf),
                    "Com %s, %s, %.0f fps, escala %d%%, %s, o resultado foi %s (%s fps, present %s ms, %llu stalls). Chega aos %.0f, mas apertado.",
                    nr_pass_count_short(p.nr_pass_count), perf_test_signal_label(p.width, p.height), p.fps,
                    p.scale_percent, perf_test_fg_label(p.fg_multiplier), perf_test_grade_label(g), fpsb, presb,
                    static_cast<unsigned long long>(s.stalls), p.fps);
    }
  } else if (fg_on) {
    if (s.fps < 20.0) {
      std::snprintf(buf, sizeof(buf),
                    "Com %s, %s, %.0f fps, escala %d%%, %s, o resultado foi %s (%s fps no cap / ~%s FG, present %s ms, %llu stalls). FG passa o numero no ecra; o cap e baixo demais.",
                    nr_pass_count_short(p.nr_pass_count), perf_test_signal_label(p.width, p.height), p.fps,
                    p.scale_percent, perf_test_fg_label(p.fg_multiplier), perf_test_grade_label(g), fpsb, fgb, presb,
                    static_cast<unsigned long long>(s.stalls));
    } else {
      std::snprintf(buf, sizeof(buf),
                    "Com %s, %s, %.0f fps, escala %d%%, %s, o resultado foi %s (%s fps no cap / ~%s FG, present %s ms, %llu stalls). Nem o FG chega a meta.",
                    nr_pass_count_short(p.nr_pass_count), perf_test_signal_label(p.width, p.height), p.fps,
                    p.scale_percent, perf_test_fg_label(p.fg_multiplier), perf_test_grade_label(g), fpsb, fgb, presb,
                    static_cast<unsigned long long>(s.stalls));
    }
  } else if (g == PerfTestGrade::Good || cap_ok) {
    std::snprintf(buf, sizeof(buf),
                  "Com %s, %s, %.0f fps, escala %d%%, %s, o resultado foi %s (%s fps, present %s ms, %llu stalls). Segura %.0f nativo sem FG.",
                  nr_pass_count_short(p.nr_pass_count), perf_test_signal_label(p.width, p.height), p.fps,
                  p.scale_percent, perf_test_fg_label(p.fg_multiplier), perf_test_grade_label(g), fpsb, presb,
                  static_cast<unsigned long long>(s.stalls), p.fps);
  } else {
    std::snprintf(buf, sizeof(buf),
                  "Com %s, %s, %.0f fps, escala %d%%, %s, o resultado foi %s (%s fps, present %s ms, %llu stalls). Sem FG nao chega aos %.0f.",
                  nr_pass_count_short(p.nr_pass_count), perf_test_signal_label(p.width, p.height), p.fps,
                  p.scale_percent, perf_test_fg_label(p.fg_multiplier), perf_test_grade_label(g), fpsb, presb,
                  static_cast<unsigned long long>(s.stalls), p.fps);
  }
  const std::string tip = perf_test_advice(p, s, g);
  if (tip.empty()) return buf;
  return std::string(buf) + " " + tip;
}

inline std::string serialize_perf_test_plan(const PerfTestPlan& raw) {
  const PerfTestPlan p = clamp_perf_test_plan(raw);
  std::ostringstream o;
  o << "width=" << p.width << "\n";
  o << "height=" << p.height << "\n";
  o << "plan_fps=" << p.fps << "\n";
  o << "scale_percent=" << p.scale_percent << "\n";
  o << "nr_pass_count=" << p.nr_pass_count << "\n";
  o << "renodx_preset=" << p.renodx_preset << "\n";
  o << "fg_multiplier=" << p.fg_multiplier << "\n";
  o << "audio=" << (p.audio ? 1 : 0) << "\n";
  o << "video=" << p.video << "\n";
  return o.str();
}

inline bool parse_perf_test_plan(const std::string& text, PerfTestPlan& out) {
  PerfTestPlan p;
  std::istringstream in(text);
  std::string line;
  bool any = false;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq);
    const std::string v = line.substr(eq + 1);
    any = true;
    if (k == "width") p.width = std::atoi(v.c_str());
    else if (k == "height") p.height = std::atoi(v.c_str());
    else if (k == "plan_fps" || k == "fps") p.fps = std::atof(v.c_str());
    else if (k == "scale_percent") p.scale_percent = std::atoi(v.c_str());
    else if (k == "nr_pass_count") p.nr_pass_count = std::atoi(v.c_str());
    else if (k == "renodx_preset") p.renodx_preset = std::atoi(v.c_str());
    else if (k == "fg_multiplier") p.fg_multiplier = std::atoi(v.c_str());
    else if (k == "audio") p.audio = (v == "1" || v == "true");
    else if (k == "video") p.video = v;
  }
  out = clamp_perf_test_plan(p);
  return any;
}

inline std::string default_perf_test_plan_path(const std::string& data_dir) {
  if (data_dir.empty()) return {};
  return data_dir + "\\perf_test_plan.txt";
}

inline bool write_perf_test_plan_file(const std::string& path, const PerfTestPlan& p) {
  if (path.empty()) return false;
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  std::ofstream o(path, std::ios::trunc);
  if (!o) return false;
  o << serialize_perf_test_plan(p);
  return static_cast<bool>(o);
}

inline bool read_perf_test_plan_file(const std::string& path, PerfTestPlan& out) {
  out = PerfTestPlan{};
  std::ifstream in(path);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  return parse_perf_test_plan(ss.str(), out);
}

inline std::string perf_test_cli_args(const PerfTestPlan& raw) {
  const PerfTestPlan p = clamp_perf_test_plan(raw);
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "--perf-test --windowed --width %d --height %d --fps %.0f --scale %d --pass %d --preset %d --fg-x %d %s",
                p.width, p.height, p.fps, p.scale_percent, p.nr_pass_count, p.renodx_preset, p.fg_multiplier,
                p.audio ? "--perf-audio" : "--no-perf-audio");
  return buf;
}

inline std::string serialize_perf_test_result(const PerfTestResult& r) {
  const PerfTestPlan p = clamp_perf_test_plan(r.plan);
  std::ostringstream o;
  o << "valid=" << (r.valid ? 1 : 0) << "\n";
  o << "grade=" << static_cast<int>(r.grade) << "\n";
  o << "fps=" << r.sample.fps << "\n";
  o << "p95_present=" << r.sample.p95_present_ms << "\n";
  o << "stalls=" << r.sample.stalls << "\n";
  o << "target_fps=" << r.sample.target_fps << "\n";
  o << "width=" << p.width << "\n";
  o << "height=" << p.height << "\n";
  o << "plan_fps=" << p.fps << "\n";
  o << "scale_percent=" << p.scale_percent << "\n";
  o << "nr_pass_count=" << p.nr_pass_count << "\n";
  o << "renodx_preset=" << p.renodx_preset << "\n";
  o << "fg_multiplier=" << p.fg_multiplier << "\n";
  o << "video=" << p.video << "\n";
  o << "popup=" << (r.popup ? 1 : 0) << "\n";
  o << "hdmi_session=" << (r.hdmi_session ? 1 : 0) << "\n";
  o << "summary=" << r.summary << "\n";
  return o.str();
}

inline bool parse_perf_test_result(const std::string& text, PerfTestResult& out) {
  out = PerfTestResult{};
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq);
    const std::string v = line.substr(eq + 1);
    if (k == "valid") out.valid = (v == "1" || v == "true");
    else if (k == "grade") out.grade = static_cast<PerfTestGrade>(std::atoi(v.c_str()));
    else if (k == "fps") out.sample.fps = std::atof(v.c_str());
    else if (k == "p95_present") out.sample.p95_present_ms = std::atof(v.c_str());
    else if (k == "stalls") out.sample.stalls = static_cast<uint64_t>(std::strtoull(v.c_str(), nullptr, 10));
    else if (k == "target_fps") out.sample.target_fps = std::atof(v.c_str());
    else if (k == "width") out.plan.width = std::atoi(v.c_str());
    else if (k == "height") out.plan.height = std::atoi(v.c_str());
    else if (k == "plan_fps") out.plan.fps = std::atof(v.c_str());
    else if (k == "scale_percent") out.plan.scale_percent = std::atoi(v.c_str());
    else if (k == "nr_pass_count") out.plan.nr_pass_count = std::atoi(v.c_str());
    else if (k == "renodx_preset") out.plan.renodx_preset = std::atoi(v.c_str());
    else if (k == "fg_multiplier") out.plan.fg_multiplier = std::atoi(v.c_str());
    else if (k == "video") out.plan.video = v;
    else if (k == "popup") out.popup = (v == "1" || v == "true");
    else if (k == "hdmi_session") out.hdmi_session = (v == "1" || v == "true");
    else if (k == "summary") out.summary = v;
  }
  out.plan = clamp_perf_test_plan(out.plan);
  if (out.sample.target_fps <= 0) out.sample.target_fps = out.plan.fps;
  if (out.summary.empty() && out.valid) {
    out.summary = format_perf_test_card(out.plan, out.sample, out.grade);
  }
  return out.valid;
}

inline bool write_perf_test_result_file(const std::string& path, const PerfTestResult& r) {
  if (path.empty()) return false;
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  std::ofstream o(path, std::ios::trunc);
  if (!o) return false;
  o << serialize_perf_test_result(r);
  return static_cast<bool>(o);
}

inline bool read_perf_test_result_file(const std::string& path, PerfTestResult& out) {
  out = PerfTestResult{};
  std::ifstream in(path);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  return parse_perf_test_result(ss.str(), out);
}

inline std::string default_perf_test_result_path(const std::string& data_dir) {
  if (data_dir.empty()) return {};
  return data_dir + "\\perf_test_last.txt";
}

// Processo novo depois do Benchmark (Esc / X / timer). O lobby nao herda Pass/tom/NR/Feed/LS.
inline const char* perf_test_return_relaunch_args() { return "--show-perf-result"; }

// Esc da sessao HDMI com escala/NR pode relancar o lobby. Sem esta flag o popup
// do Reportar performance e consumido e o processo novo abre sem a analise.
inline const char* lobby_return_relaunch_args(bool was_perf_test, bool hdmi_perf_popup_pending) {
  if (was_perf_test || hdmi_perf_popup_pending) return perf_test_return_relaunch_args();
  return nullptr;
}

// Lobby CFG live OFF esvazia Techniques=. O Iniciar regrava antes do relaunch; o Benchmark
// tem de fazer o mesmo — senão o RenoDX arranca sem cadeia e o relógio morre em evals=0.
inline PersistedSettings persisted_from_perf_test_plan(const PerfTestPlan& raw) {
  const PerfTestPlan p = clamp_perf_test_plan(raw);
  PersistedSettings s = default_settings();
  s.enhance_renodx = true;
  s.neural_rendering = true;
  s.nr_pass_count = p.nr_pass_count;
  s.renodx_preset = p.renodx_preset;
  s.scale_percent = p.scale_percent;
  return s;
}

inline bool sync_perf_test_pipeline_files(const std::string& exe_dir, const PerfTestPlan& raw) {
  return sync_persisted_session_files(exe_dir, persisted_from_perf_test_plan(raw));
}

inline void apply_perf_test_plan_to_config(const PerfTestPlan& raw, AppConfig& cfg) {
  const PerfTestPlan p = clamp_perf_test_plan(raw);
  cfg.perf_test = true;
  cfg.video_path = p.video;
  cfg.width = p.width;
  cfg.height = p.height;
  cfg.fps = p.fps;
  cfg.scale_percent = p.scale_percent;
  cfg.nr_pass_count = p.nr_pass_count;
  cfg.renodx_preset = p.renodx_preset;
  cfg.exclusive_fullscreen = false;
  cfg.sync_image = true;
  cfg.show_overlay = true;
  cfg.mini_overlay = true;
  cfg.advanced_metrics = false;
  cfg.live_overlay = false;
  cfg.enhance = EnhanceBackend::Renodx;
  cfg.dlss = false;
  cfg.neural_rendering = true;
  cfg.nr_runtime_on = true;
  cfg.perf_test_audio = p.audio;
  // Volume is settings.ini, not the Benchmark plan. apply_perf_test_plan_to_config must not touch it.
  // Feature nova: todo Benchmark gera report de bug (nao grava no CFG HDMI).
  cfg.bug_report = true;
  if (p.fg_multiplier >= 2) {
    cfg.fg_multiplier = p.fg_multiplier;
    cfg.fg_auto_start = true;
  } else {
    cfg.fg_auto_start = false;
    cfg.fg_multiplier = armed_fg_multiplier(false, 2);
  }
}

inline PerfTestPlan perf_test_plan_from_config(const AppConfig& cfg) {
  PerfTestPlan p;
  p.width = cfg.width;
  p.height = cfg.height;
  p.fps = cfg.fps;
  p.scale_percent = cfg.scale_percent;
  p.nr_pass_count = cfg.nr_pass_count;
  p.renodx_preset = cfg.renodx_preset;
  p.fg_multiplier = cfg.fg_auto_start ? cfg.fg_multiplier : 1;
  p.audio = cfg.perf_test_audio;
  p.video = cfg.video_path;
  return clamp_perf_test_plan(p);
}

// Benchmark: sempre escreve (inclui falha / poucas amostras). HDMI: so com ≥180 frames.
inline bool fill_perf_test_session_result(const AppConfig& cfg, const PerfScoreAcc* acc, const char* fail_reason,
                                         PerfTestResult& out) {
  const bool hdmi = session_perf_popup_wanted(cfg.perf_report, cfg.perf_test);
  if (!cfg.perf_test && !hdmi) return false;
  if (hdmi && (!acc || !perf_test_score_ready(acc->frames()))) return false;

  out = PerfTestResult{};
  out.valid = true;
  out.hdmi_session = hdmi;
  out.plan = perf_test_plan_from_config(cfg);
  out.sample.target_fps = out.plan.fps;
  out.sample.fg_multiplier = out.plan.fg_multiplier;
  if (!hdmi && fail_reason && *fail_reason) {
    out.grade = PerfTestGrade::Fail;
    out.summary = fail_reason;
  } else if (acc && perf_test_score_ready(acc->frames())) {
    out.sample = perf_test_sample_from_acc(*acc, out.plan.fps, out.plan.fg_multiplier);
    out.grade = grade_perf_test(out.sample);
    out.summary = format_perf_test_card(out.plan, out.sample, out.grade);
  } else {
    out.grade = PerfTestGrade::Fail;
    out.summary = "O teste acabou sem amostras suficientes. Nao usa o fps do vsync.";
  }
  out.popup = true;
  return true;
}

}  // namespace nexuscap
