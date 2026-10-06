#include "nexuscap/analyze.hpp"
#include "nexuscap/audio.hpp"
#include "nexuscap/audio_select.hpp"
#include "nexuscap/capture.hpp"
#include "nexuscap/capture_format.hpp"
#include "nexuscap/clock.hpp"
#include "nexuscap/config.hpp"
#include "nexuscap/display_geom.hpp"
#include "nexuscap/dlss_options.hpp"
#include "nexuscap/dlss5_bridge.hpp"
#include "nexuscap/enhance.hpp"
#include "nexuscap/frametime.hpp"
#include "nexuscap/hitch.hpp"
#include "nexuscap/live_mode.hpp"
#include "nexuscap/lobby.hpp"
#include "nexuscap/lossless_fg.hpp"
#include "nexuscap/max_perf.hpp"
#include "nexuscap/perf_test.hpp"
#include "nexuscap/pipeline.hpp"
#include "nexuscap/renderer.hpp"
#include "nexuscap/session_cfg.hpp"
#include "nexuscap/session_report.hpp"
#include "nexuscap/settings_store.hpp"
#include "nexuscap/user_preset.hpp"
#include "nexuscap/stats_window.hpp"
#include "nexuscap/system_metrics.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::atomic<bool> g_running{true};
std::atomic<bool> g_overlay{true};
std::atomic<bool> g_analyze{false};
std::atomic<bool> g_toggle_fs{false};
std::atomic<bool> g_toggle_dlss{false};
std::atomic<bool> g_toggle_fg{false};
std::atomic<bool> g_toggle_nr{false};
std::atomic<int> g_pass_step{0};
std::atomic<int> g_look_step{0};
// RenoDX runtime NR — carry from lobby via cfg.nr_runtime_on (same process often keeps NR ON).
bool g_nr_runtime_on = false;
bool g_renodx_multipass = false;  // active stack (AddonPath) — multipass owns NR, no hotkey
int g_nr_boot_retries = 0;
double g_nr_toggle_last_ms = 0;  // Delete debounce (bit30 + time)
nexuscap::NrConfirmPending g_nr_confirm{};  // async log ACK — no Present freeze
std::atomic<bool> g_cycle_fg{false};
std::atomic<bool> g_cycle_overlay{false};
// CFG na captura (Ctrl+M): a janela só encosta nas flags; o loop aplica.
std::atomic<bool> g_toggle_cfg{false};
std::atomic<bool> g_close_cfg{false};
std::atomic<bool> g_cfg_open{false};
nexuscap::SessionCfgOverlay* g_session_cfg = nullptr;
// Global de propósito: tem de continuar na tela depois do run_app, até o ExitProcess do relaunch.
nexuscap::RestartNotice g_restart_notice;

void note_restart_boot(const char* msg) {
  std::printf("%s\n", msg);
  OutputDebugStringA(msg);
  OutputDebugStringA("\n");
  const std::string dir = nexuscap::app_logs_dir();
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::ofstream out(dir + "\\restart_boot.txt", std::ios::app);
  if (out) out << msg << "\n";
}
nexuscap::RestartNotice g_hdmi_notice;
std::atomic<bool> g_return_to_lobby{false};
std::atomic<bool> g_session_exiting{false};
bool g_session_from_lobby = false;
nexuscap::RendererD3D11* g_session_renderer = nullptr;
bool g_session_fullscreen = true;
int g_session_signal_w = 2560;
int g_session_signal_h = 1440;
nexuscap::HotkeyConfig g_hotkeys = nexuscap::default_hotkeys();
nexuscap::SessionQuitGate g_quit_gate{};
nexuscap::QpcClock* g_quit_clock = nullptr;  // set for duration of run_app

constexpr int kExitBackToLobby = nexuscap::kSessionExitBackToLobby;

void write_perf_test_session_result(const nexuscap::AppConfig& cfg, const nexuscap::PerfScoreAcc* acc,
                                    const char* fail_reason) {
  nexuscap::PerfTestResult r;
  if (!nexuscap::fill_perf_test_session_result(cfg, acc, fail_reason, r)) return;
  (void)nexuscap::write_perf_test_result_file(nexuscap::default_perf_test_result_path(nexuscap::app_data_dir()),
                                              r);
}

LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  // Mouse e teclado do CFG (ImGui). Só devolve true no que o backend realmente consome.
  if (g_session_cfg && g_session_cfg->wnd_proc(hwnd, msg, wp, lp)) return true;
  switch (msg) {
    case WM_SIZE:
      if (g_session_renderer && wp != SIZE_MINIMIZED) {
        const int cw = LOWORD(lp);
        const int ch = HIWORD(lp);
        if (cw > 0 && ch > 0) g_session_renderer->resize(cw, ch);
      }
      return 0;
    case WM_ERASEBKGND: {
      // Com o frame congelado por cima, apagar de preto vaza um flash só na imagem.
      if (g_restart_notice.visible()) return 1;
      HDC hdc = reinterpret_cast<HDC>(wp);
      RECT rc{};
      GetClientRect(hwnd, &rc);
      FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      return 1;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
      const int vk = static_cast<int>(wp);
      g_quit_gate.on_keyup(vk, g_hotkeys.vk_quit);
      return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      const int vk = static_cast<int>(wp);
      const double now = g_quit_clock ? g_quit_clock->now_ms() : 0.0;
      const bool cfg_open = g_cfg_open.load();
      // Ctrl+M abre/fecha o CFG por cima da captura.
      if ((lp & (1u << 30)) == 0 &&
          nexuscap::cfg_hotkey_fired(vk, (GetKeyState(VK_CONTROL) & 0x8000) != 0, g_hotkeys.vk_cfg,
                                     g_hotkeys.cfg_ctrl)) {
        g_toggle_cfg = true;
        return 0;
      }
      // Com o menu aberto, o primeiro Esc fecha o menu e não volta ao lobby.
      if (vk == g_hotkeys.vk_quit &&
          nexuscap::session_esc_action(cfg_open) == nexuscap::SessionEscAction::CloseCfg) {
        g_close_cfg = true;
        return 0;
      }
      if (nexuscap::session_hotkeys_blocked_while_cfg_open(cfg_open)) {
        return 0;  // teclado é do CFG enquanto ele está aberto
      }
      if (g_quit_gate.should_accept_quit_keydown(vk, g_hotkeys.vk_quit, now)) {
        // Esc: with lobby → back to menu (never PostQuitMessage — that killed the process).
        g_return_to_lobby = g_session_from_lobby;
        g_running = false;
        if (!g_session_from_lobby) {
        PostQuitMessage(0);
        }
        return 0;
      }
      if (vk == g_hotkeys.vk_quit) {
        // Stale / held Esc right after Iniciar — ignore.
        return 0;
      }
      // Bit 30 = auto-repeat KEYDOWN — one key = one overlay cycle.
      const bool is_repeat = (lp & (1u << 30)) != 0;
      const bool ctrl_down = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      const bool shift_down = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      if (!is_repeat &&
          nexuscap::hud_hotkey_fired(vk, ctrl_down, g_hotkeys.vk_hud, g_hotkeys.hud_ctrl)) {
        g_cycle_overlay = true;
      }
      if (vk == g_hotkeys.vk_analyze || vk == VK_INSERT || vk == 'A' || vk == 'a') g_analyze = !g_analyze;
      if (vk == g_hotkeys.vk_dlss) g_toggle_dlss = true;
      if (vk == g_hotkeys.vk_fg_toggle) g_toggle_fg = true;
      if (vk == g_hotkeys.vk_fg_cycle) g_cycle_fg = true;
      // Bit 30 = auto-repeat KEYDOWN — one Delete must be one Bridge pulse (else HUD/RenoDX desync).
      if (vk == g_hotkeys.vk_nr_toggle && (lp & (1u << 30)) == 0) g_toggle_nr = true;
      if (!is_repeat) {
        const int pass_step = nexuscap::pass_cycle_hotkey_step(vk, ctrl_down, g_hotkeys, shift_down);
        if (pass_step != 0) g_pass_step.store(pass_step);
        const int look_step = nexuscap::look_cycle_hotkey_step(vk, ctrl_down, shift_down, g_hotkeys);
        if (look_step != 0) g_look_step.store(look_step);
      }
      if (vk == g_hotkeys.vk_fullscreen) g_toggle_fs = true;
      return 0;
    }
    case WM_CLOSE:
      // X = Esc: nao destroi agora. O teardown mata LS/pipeline e so depois DestroyWindow.
      g_running = false;
      if (g_session_from_lobby) g_return_to_lobby = true;
      else PostQuitMessage(0);
      return 0;
    case WM_DESTROY:
      g_running = false;
      if (!g_session_exiting) {
        if (g_session_from_lobby) g_return_to_lobby = true;
        else PostQuitMessage(0);
      }
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

int run_app(HINSTANCE hi, nexuscap::AppConfig& cfg) {
  using namespace nexuscap;
  // Lobby DestroyWindow must not kill the session (discard stale WM_QUIT / Esc).
  g_running = true;
  g_return_to_lobby = false;
  g_session_exiting = false;
  {
    MSG drain{};
    while (PeekMessageW(&drain, nullptr, 0, 0, PM_REMOVE)) {
      if (drain.message == WM_QUIT) continue;
      const int vk = static_cast<int>(drain.wParam);
      if (SessionQuitGate::is_stale_quit_message(drain.message, vk, cfg.hotkeys.vk_quit)) continue;
      TranslateMessage(&drain);
      DispatchMessageW(&drain);
    }
  }
  g_hotkeys = cfg.hotkeys;
  g_overlay = cfg.show_overlay;
  set_thread_priorities();
  QpcClock clock;
  g_quit_clock = &clock;
  const bool esc_down = (GetAsyncKeyState(cfg.hotkeys.vk_quit) & 0x8000) != 0;
  g_quit_gate.reset(clock.now_ms(), esc_down, 750.0);

  // Escala que o Feed travou neste processo (lobby abre em 100%, --autostart já entra escalado).
  // O CFG da sessão compara com ela pra saber se aplicar a escala pede reinício.
  int feed_locked_scale = feed_work_resolution_at_boot(
      cfg.show_lobby && !cfg.autostart && !cfg.perf_test, cfg.scale_percent, cfg.nr_pass_count, cfg.width,
      cfg.height);

  // Session: RenoDX feed scale + NR state. Disk NeuralUplift alone never enables runtime NR.
  {
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const fs::path exe_dir = fs::path(exe_path).parent_path();
    const bool want_nr = (cfg.enhance == EnhanceBackend::Renodx) && cfg.neural_rendering;
    // Lobby preview may already have NR ON in this process — never force false then pulse.
    g_nr_runtime_on = cfg.nr_runtime_on;
    g_nr_toggle_last_ms = 0;
    // Do NOT resync from ReShade.log here: a prior process leaves "NR toggled OFF" on disk and
    // a boot enable-pulse would flip NeuralUplift's already-ON runtime back to OFF.
    {
      const NeuralRuntimePlan boot = plan_neural_runtime(want_nr, g_nr_runtime_on);
      if (cfg.perf_test && want_nr) {
        // Multipass nao loga EvaluateFeature succeeded — o wait travava o Benchmark.
        // Ficheiros NR=ON + techniques ja gravados: mesmo trust do HDMI.
        g_nr_boot_retries = 0;
        g_nr_runtime_on = true;
        cfg.nr_runtime_on = true;
      } else if (cfg.perf_test) {
        cfg.nr_runtime_on = false;
        g_nr_runtime_on = false;
        g_nr_boot_retries = 0;
      } else if (want_nr && !g_nr_runtime_on) {
        // Fresh session + NeuralUplift=1: RenoDX boots NR ON. Skip Bridge pulse.
        g_nr_boot_retries = 0;
        g_nr_runtime_on = true;
        cfg.nr_runtime_on = true;
      } else {
        g_nr_boot_retries = boot.need_bridge_pulse ? 180 : 0;  // e.g. turn OFF after lobby
      }
    }

    if (cfg.enhance == EnhanceBackend::Renodx) {
      const fs::path feed_cfg = exe_dir / "dlss5-feed.cfg";
      const bool any_feed = fs::exists(feed_cfg) || fs::exists(exe_dir / "addons-multipass" / "dlss5-feed.cfg");
      if (any_feed) {
        int before = 100;
        if (fs::exists(feed_cfg)) {
          std::ifstream in(feed_cfg);
          std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
          in.close();
          before = parse_feed_work_resolution(text, 100);
        }
        const int want = feed_work_resolution_for_session(cfg.scale_percent, cfg.nr_pass_count, cfg.width,
                                                         cfg.height);
        // Este write e anterior ao primeiro swapchain (--autostart / Iniciar com relancamento).
        // O addon le o arquivo no init, entao o valor travado e o que acabamos de gravar.
        if (sync_feed_work_resolution_in_dir(exe_dir.string(), want, cfg.nr_pass_count)) {
          feed_locked_scale = want;
          if (before != want) {
            std::printf("renodx feed work_resolution %d → %d (session scale)\n", before, want);
          }
        } else {
          feed_locked_scale = before;
        }
      }
      const NrAdvancedLook* session_look = session_nr_look_for_sync(cfg);
      if (sync_nr_pass_count_files(exe_dir.string(), cfg.nr_pass_count, cfg.renodx_preset, session_look)) {
        std::printf("nr pass count: %d (%s)\n", clamp_nr_pass_count(cfg.nr_pass_count),
                    nr_pass_count_label(cfg.nr_pass_count));
      }
      // Visual por ultimo: look defaults do Pass Count nao podem deixar NRStyle=0 no Cinema.
      if (sync_renodx_visual_preset_files(exe_dir.string(), cfg.renodx_preset, cfg.nr_pass_count,
                                         session_look)) {
        std::printf("renodx preset: %s (NR=%d model=%d)\n",
                    renodx_visual_preset_name(cfg.renodx_preset, cfg.nr_pass_count),
                    renodx_nr_preset_index(cfg.renodx_preset),
                    renodx_feed_model_preset(cfg.renodx_preset, cfg.nr_pass_count));
      }
      // Um GPU: ReShade no Present desta janela. O filho GPU (segunda tela) pesa e nao corre.
      pipeline_global().stop();
    }

    // Sessão sempre reinicia no NR: o feed pode seguir o NeuralUplift. Se ficar
    // enabled=1 com NR=OFF, o DLSS5 continua a avaliar (log: restart sem mudar a imagem).
    if (cfg.enhance == EnhanceBackend::Renodx) {
      if (sync_neural_rendering_enabled_files(exe_dir.string(), want_nr, /*feed_on=*/want_nr)) {
        std::printf("neural rendering: NR=%s feed=%s (Delete=reinicio)\n", want_nr ? "ON" : "OFF",
                    want_nr ? "ON" : "OFF");
      }
    } else if (session_writes_renodx_off_files(cfg) &&
               sync_neural_rendering_enabled_files(exe_dir.string(), false, false)) {
      std::printf("neural rendering files: OFF\n");
    }
  }

  const int cap_w = cfg.width;
  const int cap_h = cfg.height;
  g_session_signal_w = cap_w;
  g_session_signal_h = cap_h;
  g_session_fullscreen = cfg.exclusive_fullscreen && !cfg.smoke;
  const std::wstring session_title = [&]() {
    const std::string t = nexuscap::app_window_title_session();
    int n = MultiByteToWideChar(CP_UTF8, 0, t.c_str(), -1, nullptr, 0);
    if (n <= 0) return std::wstring(L"Nexus Cap");
    std::wstring w(static_cast<size_t>(n), 0);
    MultiByteToWideChar(CP_UTF8, 0, t.c_str(), -1, w.data(), n);
    if (!w.empty() && w.back() == 0) w.pop_back();
    return w;
  }();
  const bool hold_boot = cfg.autostart && motor_hold_image_exists();
  const bool synthetic_capture = cfg.fake_capture || cfg.perf_test;
  const bool window_source =
      capture_source_is_window(cfg.capture_source) && !synthetic_capture && !cfg.fake_capture;
  // Janela nasce escondida: o primeiro Present não estoura branco. Sem cartão de pipeline.
  HWND hwnd = create_app_window(hi, cap_w, cap_h, g_session_fullscreen, session_title.c_str(),
                                hold_boot || window_source);
  if (!hwnd) {
    std::fprintf(stderr, "create window failed\n");
    return 1;
  }
  SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookWndProc));

  std::string err;
  RendererD3D11 renderer;
  g_session_renderer = &renderer;
  RECT rc{};
  GetClientRect(hwnd, &rc);
  int bw = rc.right - rc.left;
  int bh = rc.bottom - rc.top;
  if (bw <= 0) bw = cap_w;
  if (bh <= 0) bh = cap_h;

  // Restart: a placa abre antes do ReShade. O grafo entrega o frame enquanto o motor sobe.
  FrameSlot slot;
  std::unique_ptr<CaptureBase> capture;
  CaptureStartOptions cap_opt;
  cap_opt.width = cap_w;
  cap_opt.height = cap_h;
  cap_opt.fps = cfg.fps;
  cap_opt.video_fourcc = cfg.video_fourcc;
  cap_opt.color_space = cfg.color_space;
  cap_opt.color_range = cfg.color_range;
  cap_opt.capture_buffering = cfg.capture_buffering;
  cap_opt.hold_playhead = cfg.perf_test;
  const bool overlap_boot =
      session_overlaps_capture_with_renderer(window_source, hold_boot, synthetic_capture, cfg.smoke);
  const bool overlap_hdmi = overlap_boot;
  DWORD restart_boot_tick = 0;
  if (overlap_hdmi) {
    restart_boot_tick = GetTickCount();
    cap_opt.wait_first_frame = false;
    capture = std::make_unique<DShowCapture>();
    const DWORD t0 = GetTickCount();
    std::string cerr;
    const bool opened = capture->start(cfg.device_name, cap_opt, slot, clock, cerr);
    char line[180];
    std::snprintf(line, sizeof(line), "restart boot: captura aberta em %ums running=%d",
                  static_cast<unsigned>(GetTickCount() - t0), (opened && capture->running()) ? 1 : 0);
    note_restart_boot(line);
    cap_opt.wait_first_frame = true;
    if (!opened) {
      if (capture) capture->stop();
      capture.reset();
    }
  }

  DWORD window_boot_tick = 0;
  if (window_source) {
    window_boot_tick = GetTickCount();
    note_window_boot_line("aviso");
    if (window_boot_cover(hold_boot) == WindowBootCover::HoldFrame) {
      if (!g_restart_notice.visible()) {
        g_restart_notice.show(hi, session_restart_notice_title(),
                              session_restart_notice_detail(SessionCfgApply::Live));
      }
      g_restart_notice.pump();
    }
  }
  const DWORD renderer_t0 = window_source ? window_boot_tick : (overlap_boot ? GetTickCount() : 0);
  if (!renderer.init(hwnd, bw, bh, err)) {
    if (capture) {
      capture->stop();
      capture.reset();
    }
    g_session_renderer = nullptr;
    std::fprintf(stderr, "renderer: %s\n", err.c_str());
    if (cfg.perf_test) {
      write_perf_test_session_result(cfg, nullptr, ("Renderer: " + err).c_str());
      g_restart_notice.show(hi, nexuscap::perf_test_pipeline_fail_title(), err);
      g_restart_notice.pump();
      Sleep(1600);
      g_restart_notice.destroy();
      renderer.shutdown();
      DestroyWindow(hwnd);
      g_return_to_lobby = g_session_from_lobby;
      return kExitBackToLobby;
    }
    return 2;
  }
  if (overlap_hdmi) {
    char line[120];
    std::snprintf(line, sizeof(line), "restart boot: renderer.init %ums",
                  static_cast<unsigned>(GetTickCount() - renderer_t0));
    note_restart_boot(line);
  }
  if (window_source && window_boot_tick != 0) {
    char line[80];
    std::snprintf(line, sizeof(line), "renderer.init %ums",
                  static_cast<unsigned>(GetTickCount() - window_boot_tick));
    note_window_boot_line(line);
  }
  if (g_restart_notice.visible()) g_restart_notice.pump();
  // O cover já segura o último frame. Um Present preto aqui vira o flash da troca de X.
  if (!(hold_boot && g_restart_notice.has_hold_image())) {
    renderer.present_black();  // Benchmark/sessao: sem clarão branco no primeiro Present
  } else {
    // Janela nova em FLIP furava a capa e mostrava o frame de FPS 0 com o menu pela metade.
    renderer.release_flip_for_cover();
  }
  if (window_source && hwnd && !(hold_boot && g_restart_notice.has_hold_image())) {
    ShowWindow(hwnd, g_session_fullscreen ? SW_SHOW : SW_SHOWMAXIMIZED);
    UpdateWindow(hwnd);
    if (g_restart_notice.visible()) g_restart_notice.pump();
  }
  renderer.set_vsync(cfg.sync_image);
  // v2.0: RenoDX → Present native; UI scale only drives Feed work_resolution.
  const bool renodx_stack = (cfg.enhance == EnhanceBackend::Renodx);
  g_renodx_multipass = renodx_stack;
  renderer.set_scale_percent(app_present_scale_percent(renodx_stack, cfg.scale_percent));
  // RenoDX: app RT native, Feed does the scaling — HUD must show the Feed resolution.
  renderer.set_hud_work_scale_percent(
      renodx_stack ? effective_scale_percent(true, cfg.scale_percent, cfg.nr_pass_count, cfg.width, cfg.height)
                   : 0);
  renderer.set_color_format(cfg.color_space, cfg.color_range);

  std::unique_ptr<IFrameEnhance> enhance;
  {
    std::string eerr;
    const bool use_streamline = (cfg.enhance == EnhanceBackend::Streamline) ||
                                (cfg.enhance == EnhanceBackend::Off && cfg.dlss);
    const bool use_renodx = (cfg.enhance == EnhanceBackend::Renodx);

    if (use_renodx) {
      // ReShade dxgi.dll + renodx-dlss5 + dlss5-feed hook Present; keep our enhance off.
      enhance = create_passthrough_enhance();
      enhance->init(renderer.device(), eerr);
      EnhanceOptions eo;
      eo.enabled = false;
      enhance->set_options(eo);
      renderer.set_enhance(enhance.get());

      wchar_t exe_path[MAX_PATH]{};
      GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
      fs::path exe_dir = fs::path(exe_path).parent_path();
      const fs::path worker = exe_dir / "worker";
      const bool has_dxgi = fs::exists(worker / "dxgi.dll") || fs::exists(exe_dir / "dxgi.dll") ||
                            fs::exists(exe_dir / "dxgi.session.dll");
      const bool has_renodx = fs::exists(worker / "addons-multipass" / "renodx-dlss5.addon64") ||
                              fs::exists(exe_dir / "addons-multipass" / "renodx-dlss5.addon64") ||
                              fs::exists(exe_dir / "renodx-dlss5.addon64");
      const bool has_feed = fs::exists(worker / "dlss5-feed.addon64") ||
                            fs::exists(worker / "addons-multipass" / "dlss5-feed.addon64") ||
                            fs::exists(exe_dir / "dlss5-feed.addon64") ||
                            fs::exists(exe_dir / "addons-multipass" / "dlss5-feed.addon64");
      std::printf("enhance: backend=renodx (ReShade) dxgi=%d renodx=%d feed=%d\n", has_dxgi ? 1 : 0,
                  has_renodx ? 1 : 0, has_feed ? 1 : 0);
      if (!has_dxgi || !has_renodx || !has_feed) {
        std::fprintf(stderr,
                     "renodx incompleto. Rode:\n  powershell -File tools\\setup_renodx.ps1\n"
                     "Home = overlay RenoDX / presets\n");
      } else {
        std::printf("renodx: boot NR=%s | Home=presets | Delete=NR toggle (persiste)\n",
                    cfg.neural_rendering ? "ON" : "OFF");
      }
    } else if (use_streamline) {
      enhance = create_dlss_enhance();
      if (!enhance->init(renderer.device(), eerr)) {
        std::fprintf(stderr, "dlss init: %s — using passthrough\n", eerr.c_str());
        enhance = create_passthrough_enhance();
        enhance->init(renderer.device(), eerr);
      }
      EnhanceOptions eo;
      eo.enabled = true;
      eo.mode = cfg.dlss_mode;
      eo.sharpness = cfg.dlss_sharpness;
      eo.mvec = cfg.dlss_mvec;
      enhance->set_options(eo);
      renderer.set_enhance(enhance.get());
      std::printf("enhance: backend=%s supported=%d mode=%s\n", enhance->backend_name(),
                  enhance->feature_supported() ? 1 : 0, dlss_mode_name(cfg.dlss_mode));
      if (!enhance->feature_supported()) {
        std::fprintf(stderr, "dlss note: %s\n", eerr.c_str());
      }
      if (cfg.selftest_dlss && !enhance->feature_supported()) {
        std::printf("SKIP selftest-dlss: feature unsupported (%s)\n", eerr.c_str());
        return 0;
      }
    } else {
      enhance = create_passthrough_enhance();
      enhance->init(renderer.device(), eerr);
      EnhanceOptions eo;
      eo.enabled = false;
      enhance->set_options(eo);
      renderer.set_enhance(enhance.get());
      std::printf("enhance: backend=off\n");
    }
  }

  if (!capture) {
  if (cfg.fake_capture) {
    capture = std::make_unique<FakeCapture>();
  } else if (cfg.perf_test) {
    capture = std::make_unique<VirtualCapturePin>();
  } else if (window_source) {
    capture = std::make_unique<WindowCapture>();
  } else {
    capture = std::make_unique<DShowCapture>();
  }
  }
  const std::string capture_name =
      cfg.perf_test ? cfg.video_path : (window_source ? cfg.window_title : cfg.device_name);
    if (window_source) {
      cap_opt.hwnd = cfg.window_hwnd;
      cap_opt.wait_first_frame = false;
      cap_opt.async_open = true;
    }
  auto try_start_capture = [&]() -> bool {
    err.clear();
    return capture->start(capture_name, cap_opt, slot, clock, err);
  };
  const bool skip_hdmi_open = session_hdmi_open_blocked_by_busy_sim(
      cfg.simulate_hdmi_busy, g_session_from_lobby, synthetic_capture, window_source);
  if (skip_hdmi_open) {
    err = hdmi_open_busy_hint();
  } else if (capture && !capture->running() && !try_start_capture()) {
    // Mode churn / exclusive device: one settle+retry before fake fallback.
    if (!synthetic_capture && !window_source) {
      Sleep(static_cast<DWORD>(capture_device_settle_ms()));
      capture = std::make_unique<DShowCapture>();
      if (try_start_capture()) {
      }
    }
  }
  if (window_source && window_boot_tick != 0) {
    char line[120];
    std::snprintf(line, sizeof(line), "start voltou %ums pending=%d running=%d",
                  static_cast<unsigned>(GetTickCount() - window_boot_tick),
                  capture && capture->startup_pending() ? 1 : 0,
                  capture && capture->running() ? 1 : 0);
    note_window_boot_line(line);
  }
  if (!capture->running() && !capture->startup_pending()) {
    std::fprintf(stderr, "capture: %s\n", err.c_str());
    if (cfg.selftest_cap) {
      std::printf("SKIP selftest-cap: %s\n", err.c_str());
      g_session_renderer = nullptr;
      renderer.shutdown();
      DestroyWindow(hwnd);
      return 0;
    }
    if (cfg.perf_test) {
      write_perf_test_session_result(cfg, nullptr, ("Video nao abriu: " + err).c_str());
      g_restart_notice.show(hi, nexuscap::perf_test_pipeline_fail_title(), err);
      g_restart_notice.pump();
      Sleep(1600);
      g_restart_notice.destroy();
      std::fprintf(stderr, "perf-test: %s — voltando ao lobby\n", err.c_str());
      g_session_renderer = nullptr;
      if (enhance) {
        renderer.set_enhance(nullptr);
        enhance->shutdown();
        enhance.reset();
      }
      renderer.shutdown();
      DestroyWindow(hwnd);
      hwnd = nullptr;
      g_return_to_lobby = true;
      return kExitBackToLobby;
    }
    // Lobby sessions: NEVER fake — fake+RenoDX = tela branca (visto em 4K MJPEG).
    if (g_session_from_lobby) {
      const std::string fail_title = session_capture_fail_title();
      const std::string fail_detail = session_capture_fail_detail(err);
      std::fprintf(stderr, "capture falhou — voltando ao lobby (sem fake): %s\n", fail_detail.c_str());
      g_restart_notice.show(hi, fail_title, fail_detail);
      g_restart_notice.pump();
      Sleep(1800);
      g_restart_notice.destroy();
      cfg.lobby_boot_toast = session_capture_fail_notice(err);
      g_session_renderer = nullptr;
      if (enhance) {
        renderer.set_enhance(nullptr);
        enhance->shutdown();
        enhance.reset();
      }
      renderer.shutdown();
      DestroyWindow(hwnd);
      hwnd = nullptr;
      g_return_to_lobby = true;
      return kExitBackToLobby;
    }
    if (!synthetic_capture) {
      std::fprintf(stderr, "falling back to fake capture\n");
      capture = std::make_unique<FakeCapture>();
      err.clear();
      if (!capture->start(cfg.device_name, cap_opt, slot, clock, err)) {
        std::fprintf(stderr, "fake capture failed: %s\n", err.c_str());
        return 3;
      }
      cfg.fake_capture = true;
    } else {
      return 3;
    }
  }
  if (g_restart_notice.visible()) g_restart_notice.pump();

  const std::string opened = capture->opened_device();
  std::printf("capture format: %s  color %s/%s  buf %s\n", video_fourcc_name(capture->opened_fourcc()),
              color_space_name(cfg.color_space), color_range_name(cfg.color_range),
              capture_buffering_name(cfg.capture_buffering));
  if (capture->opened_width() > 0 && capture->opened_height() > 0) {
    // Keep cfg for request preference; HUD/overlay use real negotiated signal size.
    std::printf("capture: %s  signal %dx%d (requested %dx%d)\n", opened.c_str(), capture->opened_width(),
                capture->opened_height(), cap_w, cap_h);
  } else {
    std::printf("capture: %s\n", opened.c_str());
  }

  // Soft audio hint from --device; empty = any capture (HDMI preferred).
  const std::string audio_prefer = is_auto_device(cfg.device_name) ? std::string{} : cfg.device_name;

  AudioMonitor audio;
  ClipAudioPlayer clip_audio;
  // No restart, o som espera o primeiro frame HDMI. Não segura a imagem.
  bool audio_start_pending = false;
  if (!synthetic_capture && !cfg.smoke && session_hdmi_audio_wanted(false, false, window_source)) {
    if (hold_boot && !cfg.perf_test) {
      audio_start_pending = true;
    } else {
    std::string aerr;
    if (!audio.start(audio_prefer, aerr)) {
      std::fprintf(stderr, "audio: %s (continuando sem som)\n", aerr.c_str());
    } else {
      std::printf("audio: monitorando %s -> saida padrao\n",
                  audio_prefer.empty() ? "auto" : audio_prefer.c_str());
      audio.set_volume(cfg.capture_volume);
      audio.set_tone(cfg.capture_tone);
    }
    }
  } else if (cfg.perf_test && cfg.perf_test_audio && !cfg.smoke) {
    std::string aerr;
    const std::string audio_side = perf_test_audio_sidecar_path(cfg.video_path);
    clip_audio.set_volume(cfg.capture_volume);
    clip_audio.set_tone(cfg.capture_tone);
    if (!clip_audio.start(cfg.video_path, audio_side, aerr)) {
      std::fprintf(stderr, "perf-test audio: %s (continuando sem som)\n", aerr.c_str());
    } else {
    }
  }

  AnalyzeWriter analyze;
  SessionInfo info;
  info.device = cfg.fake_capture ? (opened + " (fake)") : opened;
  info.width = capture->opened_width() > 0 ? capture->opened_width() : cap_w;
  info.height = capture->opened_height() > 0 ? capture->opened_height() : cap_h;
  info.fps = cfg.fps;
  info.fourcc = video_fourcc_name(capture->opened_fourcc());
  info.buffering = cfg.capture_buffering != static_cast<int>(CaptureBuffering::Disabled);
  info.swapchain_flags = renderer.swapchain_flags();
  info.waitable = renderer.waitable();
  info.fake_capture = cfg.fake_capture;
  info.dlss_enabled = (cfg.enhance == EnhanceBackend::Renodx) ||
                      (cfg.enhance == EnhanceBackend::Streamline && enhance->feature_supported()) ||
                      (cfg.dlss && enhance->feature_supported());
  info.dlss_mode = (cfg.enhance == EnhanceBackend::Renodx) ? "renodx-presets" : dlss_mode_name(cfg.dlss_mode);
  info.dlss_backend = (cfg.enhance == EnhanceBackend::Renodx) ? "renodx+feed" : enhance->backend_name();
  info.renodx_preset = renodx_visual_preset_name(cfg.renodx_preset, cfg.nr_pass_count);

  LosslessFgController fg;
  const int boot_hz = display_refresh_hz(hwnd);
  cfg.fg_target_fps = clamp_fg_adaptive_target(cfg.fg_target_fps, boot_hz);
  fg.configure(cfg.fg_multiplier, cfg.fg_path, cfg.fg_queue, cfg.fg_adaptive ? 1 : 0, cfg.fg_target_fps,
               boot_hz, -1.0, cfg.fg_flow_scale);
  // No restart o XML do Lossless espera o primeiro frame. O arm_autostart grava de novo.
  bool profile_pending = hold_boot && !cfg.perf_test;
  if (!profile_pending) {
    std::string ferr;
    // Sempre grava mira-lock (Flow 80 / WGC), auto Steam ou Pasta — mesmo com FG off.
    if (!fg.apply_profile(ferr)) {
      std::fprintf(stderr, "fg profile: %s\n", ferr.c_str());
    } else {
      const char* src = fg.state().exe_path.empty() ? "exe missing" : (cfg.fg_path.empty() ? "auto" : "manual");
      std::printf("fg: LSFG profile mira-lock flow80 WGC x%d (%s)\n", fg.state().multiplier, src);
    }
  }
  info.fg_enabled = fg.state().active;
  info.fg_multiplier = fg.state().multiplier;
  info.fg_backend = fg.state().backend;
  info.fg_status = fg_status_name(fg.state().status);
  analyze.session_start(info, clock.now_ms());
  analyze.set_verbose(false);

  const double hitch_ms = effective_hitch_threshold_ms(cfg.hitch_threshold_ms, cfg.fps);
  HitchDetector hitch(hitch_ms);
  {
    char hbuf[128];
    std::snprintf(hbuf, sizeof(hbuf), "hitch threshold=%.1fms (cfg=%.1f @ %.0ffps)", hitch_ms,
                  cfg.hitch_threshold_ms, cfg.fps);
    std::printf("%s\n", hbuf);
  }
  ClusterDetector clusters;
  StatsWindow600 stats600;
  FrametimeWindow ft_win(180);

  OverlayState overlay;
  overlay.device = info.device;
  overlay.capture_w = info.width;
  overlay.capture_h = info.height;
  overlay.capture_fmt = info.fourcc;
  overlay.output_w = bw;
  overlay.output_h = bh;
  overlay.target_fps = cfg.fps;
  overlay.fake_capture = cfg.fake_capture;
  overlay.advanced_metrics = cfg.advanced_metrics;
  overlay.mini_overlay = cfg.mini_overlay;
  overlay.overlay_no_bg = cfg.overlay_no_bg;
  overlay.visible = cfg.show_overlay;
  overlay.hud_on = cfg.show_overlay;
  // HUD work size = what the Feed really builds (floors at 50% under RenoDX), not the
  // requested UI scale — otherwise the overlay reports a scale the Feed refused.
  const int shown_scale =
      effective_scale_percent(renodx_stack, cfg.scale_percent, cfg.nr_pass_count, info.width, info.height);
  overlay.scale_percent = shown_scale;
  overlay.work_w = scaled_extent(info.width > 0 ? info.width : cap_w, shown_scale);
  overlay.work_h = scaled_extent(info.height > 0 ? info.height : cap_h, shown_scale);
  overlay.renodx_on = g_nr_runtime_on;
  overlay.renodx_preset = renodx_visual_preset_name(cfg.renodx_preset, cfg.nr_pass_count);
  overlay.nr_pass_count = clamp_nr_pass_count(cfg.nr_pass_count);
  overlay.feed_on = (cfg.enhance == EnhanceBackend::Renodx) && g_nr_runtime_on;
  overlay.sync_image = cfg.sync_image;
  overlay.hw_info = cfg.hw_info;
  const bool live_unlocked = live_mode_unlocked();
  bool live_active = live_overlay_active(live_unlocked, cfg.live_overlay);
  overlay.live_overlay = live_active;
  overlay.live_console = cfg.live_console;
  overlay.overlay_console = cfg.overlay_console;
  if (live_active) {
    // Live owns the HUD surface — suppress mini burn + ADV twin.
    overlay.mini_overlay = false;
  }
  g_overlay = cfg.show_overlay;
  SessionNotice session_notice;
  std::string session_boot_toast = take_restart_toast();
  // Soft tip on enter only when CFG asked to start with FG on.
  if (session_boot_toast.empty() && cfg.fg_auto_start && cfg.fg_multiplier >= 2) {
    session_notice.push(session_notice_fg_on(), clock.now_ms());
  }
  g_analyze = cfg.bug_report;
  analyze.set_verbose(cfg.bug_report);

  MetricsHud hud;
  hud.create(hwnd, hi);
  LiveOverlay live;
  live.create(hwnd, hi);
  live.set_console(cfg.live_console);
  if (live_active) {
    live.update(overlay);
  } else {
    hud.update(overlay);  // size + show advanced panel immediately on Iniciar
  }

  SystemMetricsSampler hw_sampler;
  const bool sample_hw = cfg.hw_info || cfg.mini_overlay;
  // A primeira leitura de CPU/GPU (NVML) não pode atrasar o frame do restart.
  if (sample_hw && !(hold_boot && !cfg.perf_test)) {
    SystemMetricsSample hw{};
    hw_sampler.sample(renderer.device(), hw);
    overlay.cpu_ok = hw.cpu_ok;
    overlay.cpu_percent = hw.cpu_percent;
    overlay.cpu_temp_ok = hw.cpu_temp_ok;
    overlay.cpu_temp_c = hw.cpu_temp_c;
    overlay.gpu_ok = hw.gpu_ok;
    overlay.gpu_percent = hw.gpu_percent;
    overlay.gpu_temp_ok = hw.gpu_temp_ok;
    overlay.gpu_temp_c = hw.gpu_temp_c;
    overlay.vram_ok = hw.vram_ok;
    overlay.vram_used_mb = hw.vram_used_mb;
    overlay.vram_budget_mb = hw.vram_budget_mb;
    overlay.ram_ok = hw.ram_ok;
    overlay.ram_used_mb = hw.ram_used_mb;
    overlay.ram_total_mb = hw.ram_total_mb;
    overlay.power_ok = hw.power_ok;
    overlay.power_w = hw.power_w;
    overlay.cpu_name = hw.cpu_name;
    overlay.gpu_name = hw.gpu_name;
    overlay.hw_info = cfg.hw_info;
    if (live_active) live.update(overlay);
    else hud.update(overlay);
    std::printf("hw sample: ON%s", cfg.hw_info ? " (painel)" : " (mini)");
    if (!hw.cpu_name.empty() || !hw.gpu_name.empty()) {
      std::printf(" [%s / %s]", hw.cpu_name.empty() ? "CPU" : hw.cpu_name.c_str(),
                  hw.gpu_name.empty() ? "GPU" : hw.gpu_name.c_str());
    }
    std::printf("\n");
  } else {
    std::printf("hw info: off\n");
  }

  SessionReportWriter bug_rep;
  SessionReportWriter perf_rep;
  uint64_t report_hitch_count = 0;
  bool overlay_was = g_overlay.load();
  {
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const std::string exe_dir = fs::path(exe_path).parent_path().string();

    auto fill_snap = [&](SessionReportSnapshot& snap) {
      snap.build = kAppAnalyzeBuild;
      snap.device = info.device;
      snap.width = info.width;
      snap.height = info.height;
      snap.fps = cfg.fps;
      snap.scale_percent =
          effective_scale_percent(renodx_stack, cfg.scale_percent, cfg.nr_pass_count, info.width, info.height);
      snap.sync_image = cfg.sync_image;
      snap.fullscreen = cfg.exclusive_fullscreen;
      snap.show_fps = cfg.show_overlay;
      snap.mini_overlay = cfg.mini_overlay;
      snap.advanced_metrics = cfg.advanced_metrics;
      snap.hw_info = cfg.hw_info;
      snap.fg_enabled = cfg.fg_auto_start;
      snap.fg_multiplier = cfg.fg_multiplier;
      snap.neural_rendering = cfg.neural_rendering;
      snap.nr_runtime_on = g_nr_runtime_on;
      snap.renodx_preset = cfg.renodx_preset;
      snap.nr_pass_count = cfg.nr_pass_count;
      snap.video_fourcc = cfg.video_fourcc;
      snap.color_space = cfg.color_space;
      snap.color_range = cfg.color_range;
      snap.capture_buffering = cfg.capture_buffering;
      snap.output_w = bw;
      snap.output_h = bh;
    };

    auto start_one = [&](SessionReportKind kind, SessionReportWriter& w) {
      SYSTEMTIME st{};
      GetLocalTime(&st);
      const std::string path =
          make_session_report_path(exe_dir, kind, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                                   st.wSecond);
      SessionReportSnapshot snap;
      fill_snap(snap);
      if (kind == SessionReportKind::Perf) {
        snap.os_version = windows_version_string();
        SystemMetricsSample hw{};
        hw_sampler.sample(renderer.device(), hw);
        snap.cpu_name = hw.cpu_name;
        snap.gpu_name = hw.gpu_name;
        snap.ram_total_mb = hw.ram_total_mb;
        snap.vram_budget_mb = hw.vram_budget_mb;
        collect_renodx_report_snippets(exe_dir, snap.reshade_snippet, snap.feed_snippet, snap.mgf_snippet);
      }
      w.start(kind, path, snap);
      std::printf("%s report: %s\n", session_report_file_prefix(kind), path.c_str());
    };

    if (cfg.perf_test) cfg.bug_report = true;
    if (cfg.bug_report) start_one(SessionReportKind::Bug, bug_rep);
    if (cfg.perf_report) start_one(SessionReportKind::Perf, perf_rep);
  }

  auto bug_exc = [&](const char* where, const std::string& msg) {
    if (!bug_rep.active()) return;
    bug_rep.note_exception(clock.now_ms(), where, msg.c_str());
  };

  // CFG na captura: ImGui no swapchain da sessão (mesmo Present da imagem).
  SessionCfgOverlay cfg_ui;
  cfg_ui.state.signal_w = cap_w;
  cfg_ui.state.signal_h = cap_h;
  cfg_ui.state.scale_percent = clamp_scale_percent(cfg.scale_percent);
  const bool cfg_ui_ready = cfg_ui.init(hwnd, renderer.device(), renderer.context());
  // Nenhuma saída (selftest, falha de captura, Esc) pode deixar a janela apontando pro overlay morto.
  struct SessionCfgGuard {
    ~SessionCfgGuard() { g_session_cfg = nullptr; }
  } cfg_ui_guard;
  if (cfg_ui_ready) {
    g_session_cfg = &cfg_ui;
    renderer.set_ui_draw_callback(&SessionCfgOverlay::draw_trampoline, &cfg_ui);
    renderer.set_hold_probe(&SessionCfgOverlay::restart_visual_pending_trampoline, &cfg_ui);
    std::printf("session cfg: Ctrl+%s abre o menu na captura\n", vk_display_name(cfg.hotkeys.vk_cfg));
  } else {
    std::fprintf(stderr, "session cfg: ImGui nao inicializou — menu na captura indisponivel\n");
  }
  g_cfg_open = false;
  g_toggle_cfg = false;
  g_close_cfg = false;
  int cfg_ui_max_vtx = 0;
  if (cfg.selftest_cfg && cfg_ui_ready) {
    cfg_ui.set_open(true);
    g_cfg_open = true;
    std::printf("selftest-cfg: menu aberto na sessao\n");
  } else if (cfg.open_session_cfg && cfg_ui_ready) {
    cfg_ui.set_open(true);
    g_cfg_open = true;
    cfg.open_session_cfg = false;
    if (cfg.open_nr_advanced) cfg_ui.reopen_advanced_section();
    cfg.open_nr_advanced = false;
    std::printf("session cfg: voltou aberto (so fecha se o utilizador fechar)\n");
  }

  std::printf("session panel: off (captura limpa)\n");
  std::printf("present sync: %s Present(%u) — %s\n", sync_image_badge(cfg.sync_image),
              present_sync_interval(cfg.sync_image), sync_image_how_to_see(cfg.sync_image));

  if (cfg.selftest_fg) {
    std::string ferr;
    if (fg.state().status == FgStatus::Missing || fg.state().exe_path.empty()) {
      std::printf("SKIP selftest-fg: LosslessScaling.exe not found\n");
      live.destroy();
      hud.destroy();
      renderer.set_enhance(nullptr);
      enhance->shutdown();
      capture->stop();
      audio.stop();
      clip_audio.stop();
      return 0;
    }
    if (!fg.ensure_process(ferr)) {
      std::printf("FAIL selftest-fg: ensure_process %s\n", ferr.c_str());
      fg.shutdown();
      live.destroy();
      hud.destroy();
      return 7;
    }
    std::printf("selftest-fg: process pid=%lu ready — shutting down\n",
                static_cast<unsigned long>(fg.state().pid));
    fg.shutdown();
    std::printf("PASS selftest-fg\n");
    live.destroy();
    hud.destroy();
    renderer.set_enhance(nullptr);
    enhance->shutdown();
    capture->stop();
    audio.stop();
    clip_audio.stop();
    return 0;
  }

  // Reinício pedido pelo CFG: aviso na tela primeiro, teardown depois.
  bool cfg_restart_pending = false;
  SessionCfgApply cfg_restart_kind = SessionCfgApply::Live;
  double cfg_restart_until = 0;

  double prev_t = clock.now_ms();
  double last_cap_fps_t = 0;
  double shown_fps = 0;
  double shown_ft = 0;
  uint64_t presented = 0;
  int hold_live_frames = 0;
  double perf_t0 = 0;
  double perf_hold_t0 = 0;
  double perf_nr_ready_t0 = 0;
  bool perf_scoring = false;
  bool perf_revealed = !cfg.perf_test;
  bool perf_window_shown = cfg.perf_test;  // janela preta visivel; HUD so depois do relogio
  bool perf_fg_kicked = false;
  bool perf_result_written = false;
  std::string session_exe_dir;
  {
    wchar_t p[MAX_PATH]{};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    session_exe_dir = fs::path(p).parent_path().string();
  }
  std::streamoff reshade_eval_off = 0;
  int nr_evals = 0;
  int perf_score_skip = 0;
  PerfScoreAcc perf_acc;
  uint64_t cluster_count = 0;
  uint64_t stalls_since_600 = 0;
  uint64_t stalls_after_warmup = 0;
  uint64_t clusters_after_warmup = 0;
  uint64_t steady_presented = 0;  // frames after stats warmup (drives --frames / smoke)
  bool past_warmup = false;
  bool warmup_logged = false;
  double max_boot_interval_ms = 0;
  double fps_smooth = 0;
  double last_dlss_ms = 0;
  bool analyze_was = false;
  HdmiRecover hdmi;
  FramePayload last_good;
  FramePayload file_last;
  FramePayload hold_black;
  Nv12ChromaMean last_chroma;
  Nv12ChromaMean last_reject_ch;
  double last_hold_present_ms = 0;
  double hdmi_notice_until_ms = 0;
  bool hdmi_hold_logged = false;

  // Gear FG on → liga na abertura (não só perfil pronto). Off = PageDn habilita.
  // No teste a janela fica oculta até o NR aquecer; o LS precisa dela visível.
  // No restart, adotar o Lossless só depois do primeiro frame HDMI.
  bool fg_arm_pending = cfg.fg_auto_start && cfg.fg_multiplier >= 2 && !cfg.perf_test;
  if (!fg_arm_pending && cfg.fg_auto_start && cfg.fg_multiplier >= 2 && !cfg.perf_test) {
    std::string ferr;
    if (fg.arm_autostart(hwnd, ferr)) {
      std::printf("fg auto-on x%d (async)\n", fg.state().multiplier);
    } else {
      std::fprintf(stderr, "fg auto-on: %s\n", ferr.c_str());
      bug_exc("fg_auto_on", ferr);
      if (ferr.find("LosslessScaling") != std::string::npos || ferr.find("missing") != std::string::npos) {
        session_notice.push(session_notice_fg_missing(), clock.now_ms());
      }
    }
    info.fg_enabled = fg.state().active;
    info.fg_multiplier = fg.state().multiplier;
    info.fg_status = fg_status_name(fg.state().status);
  }


  while (g_running) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        // Ignore orphan WM_QUIT left from lobby/teardown — only real quit path sets flags.
        if (!g_return_to_lobby.load() && !g_session_from_lobby) g_running = false;
        continue;
      }
      const int vk = static_cast<int>(msg.wParam);
      // Drop auto-repeat Esc KEYDOWN while gate disarmed; never drop KEYUP (arms the gate).
      if (!g_quit_gate.armed && vk == g_hotkeys.vk_quit &&
          (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN)) {
        continue;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (!g_running) break;
    if (window_source && capture) {
      std::string werr;
      if (capture->take_startup_error(werr)) {
        std::fprintf(stderr, "capture falhou — voltando ao lobby: %s\n", werr.c_str());
        cfg.lobby_boot_toast = session_capture_fail_notice(werr);
        g_return_to_lobby = g_session_from_lobby;
        g_running = false;
        break;
      }
    }

    // Ctrl+M / Esc / botão Fechar — sempre pela flag, nunca dentro da janela.
    if (g_toggle_cfg.exchange(false) && cfg_ui_ready && !cfg_restart_pending) {
      if (cfg_ui.open()) cfg_ui.request_user_close();
      else {
        cfg_ui.set_open(true);
        g_cfg_open = true;
      }
    }
    if (g_close_cfg.exchange(false) && cfg_ui_ready) {
      if (cfg_ui.open()) cfg_ui.request_user_close();
      else {
        cfg_ui.set_open(false);
        g_cfg_open = false;
      }
    }

    if (g_toggle_dlss.exchange(false)) {
      if (cfg.enhance == EnhanceBackend::Renodx) {
        std::printf("renodx: use Home for overlay/presets (F5 is Streamline-only)\n");
      } else {
        EnhanceOptions eo = enhance->options();
        eo.enabled = !eo.enabled;
        enhance->set_options(eo);
        cfg.dlss = eo.enabled;
        if (eo.enabled) cfg.enhance = EnhanceBackend::Streamline;
        info.dlss_enabled = eo.enabled && enhance->feature_supported();
        std::printf("DLSS %s\n", eo.enabled ? "ON" : "OFF");
      }
    }

    if (g_toggle_nr.exchange(false)) {
      const double now_nr = clock.now_ms();
      if (cfg_restart_pending) {
      } else if (g_nr_confirm.active) {
      } else if (!nr_toggle_debounce_ok(now_nr, g_nr_toggle_last_ms)) {
      } else if (!session_nr_hotkey_allowed(cfg.enhance)) {
        std::printf("nr toggle: RenoDX off — marque RenoDX no lobby (FG continua independente)\n");
        bug_exc("nr_toggle", "blocked: RenoDX not loaded this session");
        session_notice.push(session_notice_nr_needs_renodx(), clock.now_ms());
      } else {
        // Qualquer Pass Count: grava NeuralUplift e reinicia. O pulso ao vivo invertia a imagem.
        wchar_t exe_path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        const fs::path exe_dir = fs::path(exe_path).parent_path();
        const bool want_on = !g_nr_runtime_on;
        apply_session_nr_runtime_to_config(want_on, cfg);
        g_nr_runtime_on = want_on;
        cfg.nr_runtime_on = want_on;
        overlay.renodx_on = want_on;
        info.dlss_enabled = want_on;
        (void)sync_neural_rendering_enabled_files(exe_dir.string(), want_on, /*feed_on=*/want_on);
        cfg_restart_kind = plan_session_cfg_nr_apply(true, true);
        cfg_restart_pending = true;
        cfg_restart_until = now_nr;
        write_restart_toast(session_notice_nr_apply(want_on));
        std::printf("neural rendering %s (Delete→reinicio) — Frame Gen nao muda\n", want_on ? "ON" : "OFF");
      }
    }

    if (const int step = g_pass_step.exchange(0)) {
      if (!cfg_restart_pending && !cfg.perf_test && cfg.enhance == EnhanceBackend::Renodx) {
        const int before = clamp_nr_pass_count(cfg.nr_pass_count);
        const int after = step_nr_pass_count(before, step);
        wchar_t exe_path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        const std::string exe_dir = fs::path(exe_path).parent_path().string();
        if (!prepare_renodx_runtime_dirs(exe_dir)) {
          session_notice.push(renodx_write_denied_notice(), clock.now_ms());
        } else {
          const int pass = clamp_nr_pass_count(after);
          cfg.nr_pass_count = pass;
          (void)sync_nr_pass_count_files(exe_dir, pass, cfg.renodx_preset, &cfg.nr_advanced[pass]);
          (void)sync_feed_work_resolution_in_dir(
              exe_dir,
              feed_work_resolution_for_session(cfg.scale_percent, pass, info.width, info.height), pass);
          cfg_restart_kind =
              plan_session_cfg_apply(true, before, after, feed_locked_scale, cfg.scale_percent);
          cfg_restart_pending = true;
          cfg_restart_until = clock.now_ms();
          write_restart_toast(session_notice_nr_pass_count_apply(before, after));
          overlay.nr_pass_count = pass;
        }
      }
    }

    if (const int step = g_look_step.exchange(0)) {
      if (!cfg_restart_pending && !cfg.perf_test && cfg.enhance == EnhanceBackend::Renodx) {
        const int before = clamp_renodx_visual_preset(cfg.renodx_preset);
        const int after = step_renodx_visual_preset(before, step);
        if (after != before) {
          wchar_t exe_path[MAX_PATH]{};
          GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
          const std::string exe_dir = fs::path(exe_path).parent_path().string();
          if (!prepare_renodx_runtime_dirs(exe_dir)) {
            session_notice.push(renodx_write_denied_notice(), clock.now_ms());
          } else {
            const int pass = clamp_nr_pass_count(cfg.nr_pass_count);
            cfg.renodx_preset = after;
            (void)sync_nr_pass_count_files(exe_dir, pass, after, &cfg.nr_advanced[pass]);
            (void)sync_renodx_visual_preset_files(exe_dir, after, pass, &cfg.nr_advanced[pass]);
            if (!cfg.perf_test) {
              PersistedSettings ps = default_settings();
              std::string serr;
              load_settings(ps, settings_default_path(), serr);
              ps.renodx_preset = after;
              ps.nr_pass_count = pass;
              save_settings(ps, settings_default_path(), serr);
            }
            cfg_restart_kind = plan_session_cfg_preset_apply(true);
            cfg_restart_pending = true;
            cfg_restart_until = clock.now_ms();
            write_restart_toast(session_notice_preset_apply(before, after, pass));
            overlay.renodx_preset = renodx_visual_preset_name(after, pass);
          }
        }
      }
    }

    // Finish async NR confirm without blocking Present.
    if (g_nr_confirm.active) {
      const auto polled = poll_nr_confirm_pending(g_nr_confirm, &g_nr_runtime_on, clock.now_ms());
      if (polled == NrConfirmPoll::Confirmed) {
        overlay.renodx_on = g_nr_runtime_on;
        cfg.nr_runtime_on = g_nr_runtime_on;
        apply_session_nr_runtime_to_config(g_nr_runtime_on, cfg);
        info.dlss_enabled = g_nr_runtime_on;
        std::printf("neural rendering %s (confirm) — Frame Gen nao muda\n",
                    g_nr_runtime_on ? "ON" : "OFF");
      }
    }

    // Boot: pulse Bridge only when carried runtime ≠ want (lobby may already match).
    if (g_nr_boot_retries > 0 && !g_nr_confirm.active) {
      --g_nr_boot_retries;
      const bool want_nr = (cfg.enhance == EnhanceBackend::Renodx) && cfg.neural_rendering;
      if (want_nr == g_nr_runtime_on) {
        g_nr_boot_retries = 0;
        overlay.renodx_on = g_nr_runtime_on;
      } else if (dlss5_bridge_ready_for_nr()) {
        wchar_t exe_path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        const double now_boot = clock.now_ms();
        if (apply_neural_rendering_state(fs::path(exe_path).parent_path().string(), want_nr, &g_nr_runtime_on,
                                         /*feed_on=*/true, &g_nr_confirm, now_boot)) {
          g_nr_boot_retries = 0;
          if (!g_nr_confirm.active) {
            overlay.renodx_on = g_nr_runtime_on;
            info.dlss_enabled = g_nr_runtime_on;
            cfg.nr_runtime_on = g_nr_runtime_on;
            std::printf("neural rendering runtime: %s (Bridge pulse)\n", g_nr_runtime_on ? "ON" : "OFF");
          } else {
          }
        }
      }
    }

    if (g_toggle_fs.exchange(false) && !cfg.perf_test) {
      g_session_fullscreen = !g_session_fullscreen;
      cfg.exclusive_fullscreen = g_session_fullscreen;
      int cw = 0, ch = 0;
      if (apply_display_mode(hwnd, renderer, g_session_fullscreen, g_session_signal_w, g_session_signal_h, &cw,
                             &ch)) {
        std::printf("display: %s (%dx%d)\n", g_session_fullscreen ? "fullscreen" : "windowed", cw, ch);
      }
    }

    if (g_toggle_fg.exchange(false)) {
      std::string ferr;
      if (fg.busy()) {
      } else if (!fg.request_toggle(hwnd, ferr)) {
        std::printf("fg toggle: %s\n", ferr.c_str());
        bug_exc("fg_toggle", ferr);
        if (ferr.find("LosslessScaling") != std::string::npos || ferr.find("missing") != std::string::npos) {
          session_notice.push(session_notice_fg_missing(), clock.now_ms());
        } else {
          session_notice.push("Frame Gen — sem confirmacao; overlay nao mudou", clock.now_ms());
        }
      } else {
        session_notice.push(fg.state().active ? "Frame Gen — desligando…" : "Frame Gen — aplicando…",
                            clock.now_ms());
      }
    }
    if (g_cycle_fg.exchange(false)) {
      std::string ferr;
      if (fg.busy()) {
      } else if (!fg.request_cycle_multiplier(hwnd, ferr)) {
        std::printf("fg cycle: %s\n", ferr.c_str());
      } else {
        session_notice.push("Frame Gen — trocando x" + std::to_string(fg.state().multiplier) + "…",
                            clock.now_ms());
      }
    }
    if (g_cycle_overlay.exchange(false)) {
      const OverlayHudFlags next =
          cycle_overlay_hud(g_overlay.load(), cfg.mini_overlay, cfg.advanced_metrics, cfg.live_overlay);
      g_overlay = next.visible;
      cfg.show_overlay = next.visible;
      cfg.mini_overlay = next.mini;
      cfg.advanced_metrics = next.advanced;
      cfg.live_overlay = next.live;
      if (cfg_ui_ready) cfg_ui.state.hud_visible = next.visible;
    }

    // Finish async FG without blocking Present (same idea as NR log confirm).
    if (fg.busy() && fg.tick(clock.now_ms())) {
      apply_session_fg_to_config(fg.state().active, fg.state().multiplier, cfg);
      info.fg_enabled = fg.state().active;
      info.fg_multiplier = fg.state().multiplier;
      info.fg_status = fg_status_name(fg.state().status);
      if (!fg.state().last_error.empty() && fg.state().status == FgStatus::Error) {
        session_notice.push("Frame Gen — " + fg.state().last_error, clock.now_ms());
        bug_exc("fg_async", fg.state().last_error);
      } else {
        std::printf("fg %s x%d\n", fg.state().active ? "ON" : "off", fg.state().multiplier);
        // Contador do LS: FG segue valendo, mas o hide falhado nao fica invisivel no log.
        if (!fg.state().ini_hide_error.empty()) {
        }
        if (session_notice_for_fg_toggle(fg.state().active)) {
          session_notice.push(session_notice_fg_on(), clock.now_ms());
        } else if (fg.state().active) {
          session_notice.push(session_notice_fg_multiplier_up(fg.state().multiplier), clock.now_ms());
        }
      }
    }
    info.fg_enabled = fg.state().active;
    info.fg_multiplier = fg.state().multiplier;
    info.fg_status = fg_status_name(fg.state().status);

    const bool analyze_on = g_analyze.load();
    if (analyze_on && !analyze_was) {
      analyze.set_verbose(true);
    }
    if (!analyze_on && analyze_was) {
      analyze.set_verbose(false);
      analyze.session_end(clock.now_ms());
      std::string jsonl, summary;
      if (write_analyze_files(cfg.log_dir, analyze, clock.now_ms(), jsonl, summary)) {
        std::printf("Analyze written:\n  %s\n  %s\n", jsonl.c_str(), summary.c_str());
      }
      // start a fresh session buffer for continued run
      analyze.session_start(info, clock.now_ms());
    }
    analyze_was = analyze_on;

    FramePayload frame;
    const double now_idle = clock.now_ms();
    bool have_new = slot.consume(frame);
    bool holding = false;
    HdmiRecoverAction hdmi_act = HdmiRecoverAction::None;
    const bool app_relaunching = hdmi_blocked_by_nexuscap_restart(
        cfg_restart_pending, g_restart_notice.visible() && !cfg.perf_test,
        cfg.autostart && !cfg.perf_test && presented < kStatsWarmupFrames);
    if (hdmi_recover_enabled(synthetic_capture, app_relaunching, window_source)) {
      const int ow = capture->opened_width() > 0 ? capture->opened_width() : cap_w;
      const int oh = capture->opened_height() > 0 ? capture->opened_height() : cap_h;
      if (have_new) {
        const bool size_ok = frame.width == ow && frame.height == oh &&
                             nv12_buffer_matches_mode(frame.nv12.size(), ow, oh);
        const Nv12ChromaMean now_ch = nv12_chroma_mean(frame.nv12, frame.width, frame.height);
        const bool jumped = nv12_chroma_jumped_to_pink(last_chroma, now_ch);
        const bool junk = size_ok && nv12_looks_like_hdmi_junk(frame.nv12, frame.width, frame.height);
        const bool content_ok = size_ok && !junk && !jumped;
        if (!content_ok) last_reject_ch = now_ch;
        hdmi_act = hdmi_on_arrived(hdmi, now_idle, size_ok, content_ok);
      } else {
        hdmi_act = hdmi_on_idle(hdmi, now_idle);
        if (hdmi.lost) {
          const size_t nbytes = capture->last_sample_bytes();
          if (nbytes > 0 && !nv12_buffer_matches_mode(nbytes, ow, oh)) {
            const HdmiRecoverAction pulse = hdmi_on_sample(hdmi, now_idle, false, false);
            if (pulse == HdmiRecoverAction::Reopen) hdmi_act = pulse;
          }
        }
      }

      if (hdmi_act == HdmiRecoverAction::Reopen && !cfg_restart_pending) {
        const size_t last_bytes = capture->last_sample_bytes();
        const int prev_w = ow;
        const int prev_h = oh;
        slot.clear();
        capture = std::make_unique<DShowCapture>();
        CaptureStartOptions rec = cap_opt;
        rec.follow_hdmi_size = true;
        rec.wait_first_frame = false;
        int try_w = prev_w > 0 ? prev_w : rec.width;
        int try_h = prev_h > 0 ? prev_h : rec.height;
        if (last_bytes > 0 && !nv12_buffer_matches_mode(last_bytes, try_w, try_h)) {
          const auto modes = list_video_capture_modes(cfg.device_name);
          (void)hdmi_pick_reopen_size(last_bytes, cap_opt.width, cap_opt.height, modes, &try_w, &try_h);
        }
        rec.width = try_w;
        rec.height = try_h;
        err.clear();
        bool ok = capture->start(cfg.device_name, rec, slot, clock, err);
        if (!ok && (try_w != cap_opt.width || try_h != cap_opt.height)) {
          capture = std::make_unique<DShowCapture>();
          rec.width = cap_opt.width;
          rec.height = cap_opt.height;
          rec.follow_hdmi_size = true;
          rec.wait_first_frame = false;
          err.clear();
          ok = capture->start(cfg.device_name, rec, slot, clock, err);
        }
        hdmi_note_reopened(hdmi, clock.now_ms());
        if (ok && capture->running()) {
          info.width = capture->opened_width() > 0 ? capture->opened_width() : try_w;
          info.height = capture->opened_height() > 0 ? capture->opened_height() : try_h;
          const bool size_changed =
              std::abs(info.width - prev_w) > 16 || std::abs(info.height - prev_h) > 16;
          if (size_changed) {
            wchar_t exe_path[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
            (void)sync_feed_work_resolution_in_dir(
                fs::path(exe_path).parent_path().string(),
                feed_work_resolution_for_session(cfg.scale_percent, cfg.nr_pass_count, info.width,
                                                info.height),
                cfg.nr_pass_count);
          }
          char okbuf[192];
          std::snprintf(okbuf, sizeof(okbuf), "hdmi recover ok signal %dx%d (pedido %dx%d) feed_rebuild=%d",
                        capture->opened_width(), capture->opened_height(), rec.width, rec.height,
                        size_changed ? 1 : 0);
          std::printf("%s\n", okbuf);
        } else {
        }
        have_new = false;
        hdmi_act = HdmiRecoverAction::Hold;
      }

      if (hdmi_keep_last_good(hdmi_act)) {
        have_new = false;
        hdmi_act = HdmiRecoverAction::Hold;
      }
    }

    const bool last_good_ok =
        last_good.width > 0 && last_good.height > 0 && !last_good.nv12.empty() &&
        !nv12_looks_like_hdmi_junk(last_good.nv12, last_good.width, last_good.height);
    bool file_repeat = false;
    if (app_relaunching) {
      if (!have_new) {
      Sleep(1);
      continue;
      }
    } else if (!have_new || hdmi_act == HdmiRecoverAction::Hold) {
      if (synthetic_capture && file_cap_repeat_ok(file_last.width, file_last.height, file_last.nv12)) {
        // Capture card always has a current buffer. Empty slot + VSYNC = 33ms red spike.
        file_repeat = true;
        file_last.cap_qpc_ms = now_idle;
      } else if (!synthetic_capture && (hdmi_act == HdmiRecoverAction::Hold || hdmi.lost)) {
        if ((now_idle - last_hold_present_ms) < static_cast<double>(hdmi_hold_present_gap_ms())) {
          Sleep(1);
          continue;
        }
        last_hold_present_ms = now_idle;
        holding = true;
        if (hdmi_lost_long_enough(hdmi, now_idle) &&
            (hdmi_notice_until_ms <= 0.0 || now_idle >= hdmi_notice_until_ms)) {
          hdmi_notice_until_ms = now_idle + static_cast<double>(hdmi_notice_dwell_ms());
        }
        if (hdmi_lost_long_enough(hdmi, now_idle) && !hdmi_hold_logged) {
          hdmi_hold_logged = true;
        }
        if (!last_good_ok) {
          const int bw_h = capture->opened_width() > 0 ? capture->opened_width() : cap_w;
          const int bh_h = capture->opened_height() > 0 ? capture->opened_height() : cap_h;
          if (hold_black.width != bw_h || hold_black.height != bh_h) {
            hold_black.width = bw_h;
            hold_black.height = bh_h;
            nv12_fill_limited_black(hold_black.nv12, bw_h, bh_h);
          }
          hold_black.cap_qpc_ms = now_idle;
          frame.width = bw_h;
          frame.height = bh_h;
          frame.cap_qpc_ms = now_idle;
        } else {
          frame.width = last_good.width;
          frame.height = last_good.height;
          frame.cap_qpc_ms = last_good.cap_qpc_ms;
        }
      } else {
        if (window_source && g_restart_notice.visible()) g_restart_notice.pump();
        if (window_source && presented == 0) prev_t = now_idle;
        Sleep(1);
        continue;
      }
    }

    const double now = clock.now_ms();
    const double interval = now - prev_t;
    prev_t = now;
    const double cap_t_meter =
        holding ? last_good.cap_qpc_ms : (file_repeat ? file_last.cap_qpc_ms : frame.cap_qpc_ms);
    const double cap_iv = overlay_capture_interval_ms(cap_t_meter, last_cap_fps_t, holding, file_repeat);
    if (!holding && !file_repeat && cap_t_meter > last_cap_fps_t) last_cap_fps_t = cap_t_meter;
    if (cap_iv > 0.0) {
      ft_win.push(cap_iv);
      const OverlaySourceRate rate = ft_win.source_rate();
      if (rate.fps > 0.0) {
        shown_ft = rate.period_ms;
        shown_fps = rate.fps;
        fps_smooth = rate.fps;
        overlay.ft_avg_ms = rate.period_ms;
        overlay.ft_p95_ms = rate.p95_ms > 0.0 ? rate.p95_ms : rate.period_ms;
        overlay.ft_max_ms = rate.max_ms > 0.0 ? rate.max_ms : rate.period_ms;
        overlay.ft_stable = ft_win.stable_for_source(
            overlay.ft_p95_ms, overlay.ft_max_ms, 1000.0 / (std::max)(1.0, cfg.fps), hitch_ms);
        ft_win.chronological(overlay.ft_hist);
        // Mediana no vão curto: o HUD inteiro (fps, ft, avg) dizia 60. A média do ritmo vale
        // para o número e para a linha.
        double beat_ms = 0;
        if (overlay_calm_frametime_graph(overlay.ft_hist, &beat_ms)) {
          shown_ft = beat_ms;
          shown_fps = 1000.0 / beat_ms;
          fps_smooth = shown_fps;
          overlay.ft_avg_ms = beat_ms;
          overlay.ft_p95_ms = beat_ms;
          double peak = beat_ms;
          for (double g : overlay.ft_hist)
            if (g > peak) peak = g;
          overlay.ft_max_ms = peak;
          overlay.ft_stable = ft_win.stable_for_source(
              beat_ms, peak, 1000.0 / (std::max)(1.0, cfg.fps), hitch_ms);
        } else {
          for (double& g : overlay.ft_hist) g = overlay_source_gap_ms(g, rate.period_ms);
        }
        overlay.ft_spark = ft_win.sparkline(56, 1000.0 / (std::max)(1.0, cfg.fps), rate.period_ms);
      }
    }

    double present_ms = 0;
    live_active = live_overlay_active(live_unlocked, cfg.live_overlay);
    overlay.visible = g_overlay && !(cfg.perf_test && !perf_scoring);
    overlay.hud_on = overlay.visible;
    overlay.advanced_metrics = cfg.advanced_metrics;
    overlay.mini_overlay = live_active ? false : cfg.mini_overlay;
    overlay.overlay_no_bg = cfg.overlay_no_bg;
    overlay.live_overlay = live_active;
    overlay.live_console = cfg.live_console;
    overlay.overlay_console = cfg.overlay_console;
    overlay.analyze_on = analyze_on;
    overlay.frame_ms = shown_ft;
    {
      const double cap_t = holding ? last_good.cap_qpc_ms : (file_repeat ? file_last.cap_qpc_ms : frame.cap_qpc_ms);
      overlay.e2e_ms = (cap_t > 0.0 && now >= cap_t) ? (now - cap_t) : 0.0;
    }
    overlay.fps = shown_fps;
    overlay.fps_smooth = fps_smooth;
    if (cfg.perf_test) {
      overlay.perf_remain_sec = perf_scoring ? perf_test_remain_sec(now, perf_t0) : -1;
    } else {
      overlay.perf_remain_sec = -2;
    }
    overlay.capture_w = holding ? (last_good_ok ? last_good.width : frame.width)
                                : (frame.width > 0 ? frame.width : cap_w);
    overlay.capture_h = holding ? (last_good_ok ? last_good.height : frame.height)
                                : (frame.height > 0 ? frame.height : cap_h);
    overlay.output_w = bw;
    overlay.output_h = bh;
    overlay.frames = presented;
    overlay.dropped = slot.dropped();
    overlay.stalls_window = stalls_since_600;
    overlay.clusters = cluster_count;
    overlay.fake_capture = cfg.fake_capture;
    overlay.device = info.device;
    fg.reconcile_process();
    overlay.fg_on = fg.overlay_reads_on(hwnd);
    overlay.fg_multiplier = fg.state().multiplier;
    overlay.fg_status = fg_status_name(fg.state().status);
    overlay.fg_backend = fg.state().backend;
    overlay.fg_flow_scale = fg.state().flow_scale;
    overlay.fg_queue = fg.state().queue_target;
    overlay.fg_adaptive = fg.state().adaptive;
    overlay.fg_target_fps = fg.state().target_fps;
    overlay.fg_capture_api = fg.state().capture_api;
    overlay.session_notice = session_notice.visible(now);
    overlay.hdmi_hold_notice =
        !app_relaunching &&
        (hdmi_lost_long_enough(hdmi, now) || hdmi_notice_visible(holding, now, hdmi_notice_until_ms));
    if (overlay.hdmi_hold_notice) {
      overlay.hdmi_notice_title = session_hdmi_notice_title();
      overlay.hdmi_notice_detail = session_hdmi_notice_detail();
    } else {
      overlay.hdmi_notice_title.clear();
      overlay.hdmi_notice_detail.clear();
    }
    // Soft tips must be readable even if the user hid the HUD.
    if (session_notice.active(now)) overlay.visible = true;
    if (overlay.fg_on && overlay.fg_multiplier >= 2) {
      if (overlay.fg_adaptive) {
        overlay.fps_fg_est = static_cast<double>(overlay.fg_target_fps);
      } else {
        overlay.fps_fg_est = fps_smooth * static_cast<double>(overlay.fg_multiplier);
      }
    } else {
      overlay.fps_fg_est = 0;
    }

    // O CFG desenha dentro do Present, então o estado tem de estar fresco antes dele.
    if (cfg_ui_ready) {
      SessionCfgState& ui = cfg_ui.state;
      ui.renodx_stack = (cfg.enhance == EnhanceBackend::Renodx);
      ui.multipass = g_renodx_multipass;
      ui.nr_on = g_nr_runtime_on;
      ui.nr_busy = g_nr_confirm.active;
      ui.renodx_preset = clamp_renodx_visual_preset(cfg.renodx_preset);
      ui.nr_pass_count = clamp_nr_pass_count(cfg.nr_pass_count);
      copy_nr_advanced(ui.nr_advanced, cfg.nr_advanced);
      ui.scale_percent = clamp_scale_percent(cfg.scale_percent);
      ui.feed_loaded_scale = feed_locked_scale;
      ui.fg_on = fg.state().active;
      ui.fg_multiplier = fg.state().multiplier >= 2 ? fg.state().multiplier : cfg.fg_multiplier;
      ui.fg_queue = clamp_fg_queue(fg.state().queue_target);
      ui.fg_flow_scale = clamp_fg_flow_scale(fg.state().flow_scale);
      ui.fg_adaptive = fg.state().adaptive;
      ui.fg_target_fps = fg.state().target_fps;
      ui.monitor_hz = display_refresh_hz(hwnd);
      ui.capture_fps = overlay.fps_smooth > 1.0 ? overlay.fps_smooth : overlay.fps;
      ui.fg_busy = fg.busy();
      ui.hud_visible = g_overlay.load();
      ui.overlay_no_bg = cfg.overlay_no_bg;
      ui.capture_volume = clamp_capture_volume_percent(cfg.capture_volume);
      ui.capture_tone = clamp_capture_tone(cfg.capture_tone);
      ui.max_perf = max_perf_is_on();
      ui.live_console = cfg.live_console;
      ui.overlay_console = cfg.overlay_console;
      ui.signal_w = overlay.capture_w > 0 ? overlay.capture_w : cap_w;
      ui.signal_h = overlay.capture_h > 0 ? overlay.capture_h : cap_h;
      if (!cfg_ui.hotkeys_edit_locked()) {
        ui.hotkeys = g_hotkeys;
        ui.vk_cfg = g_hotkeys.vk_cfg;
        ui.cfg_ctrl = g_hotkeys.cfg_ctrl;
      }
      ui.status = overlay.session_notice;
      if (cfg_ui.open()) {
        SystemMetricsSample hw{};
        hw_sampler.sample(renderer.device(), hw);
        ui.cpu_ok = hw.cpu_ok;
        ui.cpu_percent = hw.cpu_percent;
        ui.gpu_ok = hw.gpu_ok;
        ui.gpu_percent = hw.gpu_percent;
        ui.vram_ok = hw.vram_ok;
        ui.vram_used_mb = hw.vram_used_mb;
        ui.vram_budget_mb = hw.vram_budget_mb;
        ui.power_ok = hw.power_ok;
        ui.power_w = hw.power_w;
        ui.power_limit_ok = hw.power_limit_ok;
        ui.power_limit_w = hw.power_limit_w;
      }
      cfg_ui.fg_can_enable = lobby_fg_can_enable(cfg.fg_path);
      cfg_ui.presets_locked = cfg.perf_test;
    }

    const FramePayload& present_src =
        holding ? (last_good_ok ? last_good : hold_black) : (file_repeat ? file_last : frame);
    // Delete / Ctrl+/- gravam o INI antes deste Present. A foto é o frame anterior.
    // Apresentar de novo já mostra o DLSS 5 mudando e pisca.
    if (cfg_restart_pending && !renderer.took_hold_snapshot() &&
        !renderer.snapshot_hold_from_last_presented()) {
      renderer.arm_hold_snapshot();
    }
    if (!cfg_restart_pending || renderer.hold_snapshot_armed()) {
      if (!renderer.present_frame(present_src, overlay, present_ms, err)) {
        bug_exc("present", err);
        if (perf_rep.active()) perf_rep.note_event(now, "present_error", err.c_str());
      }
    }
    if (g_hdmi_notice.visible()) g_hdmi_notice.destroy();
    if (!overlay.hdmi_hold_notice) hdmi_hold_logged = false;
    if (!holding && have_new && synthetic_capture && frame.width > 0 && !frame.nv12.empty()) {
      file_last.width = frame.width;
      file_last.height = frame.height;
      file_last.cap_qpc_ms = frame.cap_qpc_ms;
      file_last.frame_index = frame.frame_index;
      file_last.nv12.swap(frame.nv12);
    } else if (!holding && !synthetic_capture &&
               (hdmi_act == HdmiRecoverAction::Present || hdmi_act == HdmiRecoverAction::None) &&
               frame.width > 0 && !frame.nv12.empty() &&
               !nv12_looks_like_hdmi_junk(frame.nv12, frame.width, frame.height)) {
      last_good.width = frame.width;
      last_good.height = frame.height;
      last_good.cap_qpc_ms = frame.cap_qpc_ms;
      last_chroma = nv12_chroma_mean(frame.nv12, frame.width, frame.height);
      last_good.nv12.swap(frame.nv12);
    }

    if (cfg_ui_ready && cfg.selftest_cfg) {
      cfg_ui_max_vtx = (std::max)(cfg_ui_max_vtx, cfg_ui.last_vertex_count());
    }

    if (cfg_ui_ready && !cfg_restart_pending) {
      const SessionCfgActions act = cfg_ui.take_actions();
      SessionCfgState& ui = cfg_ui.state;
      if (!act.notice.empty()) session_notice.push(act.notice, clock.now_ms());
      if (act.apply_user_preset) {
        ui.hud_visible = act.apply_hud;
        ui.overlay_no_bg = act.apply_overlay_no_bg;
        ui.overlay_console = act.apply_overlay_console;
        ui.live_console = act.apply_live_console;
        ui.nr_on = act.apply_nr_on;
        ui.renodx_preset = act.apply_renodx_preset;
        ui.nr_pass_count = act.apply_nr_pass_count;
        ui.scale_percent = act.apply_scale_percent;
        ui.fg_on = act.apply_fg_on;
        ui.fg_multiplier = act.apply_fg_multiplier;
        ui.fg_queue = clamp_fg_queue(act.apply_fg_queue);
        ui.fg_flow_scale = clamp_fg_flow_scale(act.apply_fg_flow_scale);
        ui.fg_adaptive = act.apply_fg_adaptive;
        ui.fg_target_fps = act.apply_fg_target_fps;
        // Restart do Pass/look grava cfg no settings.ini. Sem isto, escala/FG/NR
        // do preset perdem para o valor antigo da sessao.
        cfg.scale_percent = clamp_scale_percent(act.apply_scale_percent);
        apply_session_fg_to_config(act.apply_fg_on, act.apply_fg_multiplier, cfg);
        cfg.fg_queue = clamp_fg_queue(act.apply_fg_queue);
        cfg.fg_flow_scale = clamp_fg_flow_scale(act.apply_fg_flow_scale);
        cfg.fg_adaptive = act.apply_fg_adaptive;
        cfg.fg_target_fps = clamp_fg_adaptive_target_stored(act.apply_fg_target_fps);
        cfg.neural_rendering = act.apply_nr_on;
        cfg.renodx_preset = clamp_renodx_visual_preset(act.apply_renodx_preset);
        cfg.nr_pass_count = clamp_nr_pass_count(act.apply_nr_pass_count);
        copy_nr_advanced(cfg.nr_advanced, ui.nr_advanced);
        cfg.overlay_no_bg = act.apply_overlay_no_bg;
        cfg.overlay_console = act.apply_overlay_console;
        cfg.live_console = static_cast<int>(clamp_live_console(act.apply_live_console));
        cfg.show_overlay = act.apply_hud;
      }
      if (act.close_requested) {
        cfg_ui.set_open(false);
        g_cfg_open = false;
      }
      if (act.hotkeys_changed) {
        g_hotkeys = ui.hotkeys;
        cfg.hotkeys = ui.hotkeys;
        ui.vk_cfg = ui.hotkeys.vk_cfg;
        ui.cfg_ctrl = ui.hotkeys.cfg_ctrl;
        if (!cfg.perf_test) {
          PersistedSettings ps = default_settings();
          std::string serr;
          load_settings(ps, settings_default_path(), serr);
          ps.hotkeys = ui.hotkeys;
          save_settings(ps, settings_default_path(), serr);
        }
      }
      if (act.hud_changed) g_overlay = ui.hud_visible;
      if (act.overlay_no_bg_changed) {
        cfg.overlay_no_bg = ui.overlay_no_bg;
        overlay.overlay_no_bg = cfg.overlay_no_bg;
        if (!cfg.perf_test) {
          PersistedSettings ps = default_settings();
          std::string serr;
          load_settings(ps, settings_default_path(), serr);
          ps.ui.overlay_no_bg = cfg.overlay_no_bg;
          save_settings(ps, settings_default_path(), serr);
        }
      }
      if (act.volume_changed) {
        cfg.capture_volume = clamp_capture_volume_percent(ui.capture_volume);
        audio.set_volume(cfg.capture_volume);
        clip_audio.set_volume(cfg.capture_volume);
        PersistedSettings ps = default_settings();
        std::string serr;
        load_settings(ps, settings_default_path(), serr);
        ps.capture_volume = cfg.capture_volume;
        save_settings(ps, settings_default_path(), serr);
      }
      if (act.tone_changed) {
        cfg.capture_tone = clamp_capture_tone(ui.capture_tone);
        audio.set_tone(cfg.capture_tone);
        clip_audio.set_tone(cfg.capture_tone);
        PersistedSettings ps = default_settings();
        std::string serr;
        load_settings(ps, settings_default_path(), serr);
        ps.capture_tone = cfg.capture_tone;
        save_settings(ps, settings_default_path(), serr);
      }
      if (act.max_perf_changed) {
        max_perf_sync(ui.max_perf);
        PersistedSettings ps = default_settings();
        std::string serr;
        load_settings(ps, settings_default_path(), serr);
        ps.max_perf = ui.max_perf;
        save_settings(ps, settings_default_path(), serr);
      }
      if (act.console_changed) {
        cfg.live_console = static_cast<int>(clamp_live_console(ui.live_console));
        cfg.overlay_console = ui.overlay_console;
        overlay.live_console = cfg.live_console;
        overlay.overlay_console = cfg.overlay_console;
        live.set_console(cfg.live_console);
        if (!cfg.perf_test) {
          PersistedSettings ps = default_settings();
          std::string serr;
          load_settings(ps, settings_default_path(), serr);
          ps.ui.live_console = live_console_key(clamp_live_console(cfg.live_console));
          ps.ui.overlay_console = cfg.overlay_console;
          save_settings(ps, settings_default_path(), serr);
        }
      }
      // NR e FG reaproveitam os caminhos das teclas (debounce, ACK por log, async).
      if (act.toggle_nr) g_toggle_nr = true;
      if (act.toggle_fg) g_toggle_fg = true;
      // Fixo/Adaptive antes do xN: o restart do multiplier tem de gravar o modo novo, nao o Adaptive leftover.
      if (act.fg_queue_changed || act.fg_flow_changed || act.fg_adaptive_changed || act.fg_target_changed) {
        const int hz = display_refresh_hz(hwnd);
        const double cap_fps = overlay.fps_smooth > 1.0 ? overlay.fps_smooth : overlay.fps;
        cfg.fg_queue = clamp_fg_queue(ui.fg_queue);
        cfg.fg_flow_scale = clamp_fg_flow_scale(ui.fg_flow_scale);
        cfg.fg_adaptive = ui.fg_adaptive;
        cfg.fg_target_fps = clamp_fg_adaptive_target(ui.fg_target_fps, hz, cap_fps);
        const int keep_mult = fg.state().multiplier >= 2 ? fg.state().multiplier : cfg.fg_multiplier;
        fg.configure(keep_mult, cfg.fg_path, cfg.fg_queue, cfg.fg_adaptive ? 1 : 0, cfg.fg_target_fps, hz,
                     cap_fps, cfg.fg_flow_scale);
        overlay.fg_queue = cfg.fg_queue;
        overlay.fg_flow_scale = cfg.fg_flow_scale;
        overlay.fg_adaptive = cfg.fg_adaptive;
        overlay.fg_target_fps = cfg.fg_target_fps;
        if (!cfg.perf_test) {
          PersistedSettings ps = default_settings();
          std::string serr;
          load_settings(ps, settings_default_path(), serr);
          ps.fg_queue = cfg.fg_queue;
          ps.fg_flow_scale = cfg.fg_flow_scale;
          ps.fg_adaptive = cfg.fg_adaptive;
          ps.fg_target_fps = cfg.fg_target_fps;
          save_settings(ps, settings_default_path(), serr);
        }
        const bool fg_live = fg.overlay_reads_on(hwnd) || fg.state().active;
        const bool mode_or_target = act.fg_adaptive_changed || act.fg_target_changed;
        const bool flow_notice = act.fg_flow_changed && !act.fg_queue_changed && !mode_or_target;
        const std::string notice_live =
            mode_or_target ? session_notice_fg_adaptive_apply(cfg.fg_adaptive, cfg.fg_target_fps, hz, true, cap_fps)
            : flow_notice  ? session_notice_fg_flow_apply(cfg.fg_flow_scale, true)
                           : session_notice_fg_queue_apply(cfg.fg_queue, true);
        const std::string notice_idle =
            mode_or_target ? session_notice_fg_adaptive_apply(cfg.fg_adaptive, cfg.fg_target_fps, hz, false, cap_fps)
            : flow_notice  ? session_notice_fg_flow_apply(cfg.fg_flow_scale, false)
                           : session_notice_fg_queue_apply(cfg.fg_queue, false);
        if (act.fg_multiplier_changed) {
          // xN ja vai reabrir o LS; apply_profile la pega a fila/modo novos.
        } else if (fg.busy()) {
        } else if (fg_live) {
          std::string ferr;
          if (!fg.request_set_multiplier(keep_mult, hwnd, ferr)) {
            std::printf("cfg fg perfil: %s\n", ferr.c_str());
          } else {
            session_notice.push(notice_live, now);
          }
        } else {
          std::string ferr;
          if (!fg.apply_profile(ferr)) {
          }
          session_notice.push(notice_idle, now);
        }
      }
      if (act.fg_multiplier_changed) {
        std::string ferr;
        if (fg.busy()) {
        } else if (!fg.request_set_multiplier(ui.fg_multiplier, hwnd, ferr)) {
          std::printf("cfg fg x%d: %s\n", ui.fg_multiplier, ferr.c_str());
        } else {
          session_notice.push(session_notice_fg_multiplier_up(ui.fg_multiplier), now);
        }
      }

      const bool renodx_now = (cfg.enhance == EnhanceBackend::Renodx);
      wchar_t exe_path_cfg[MAX_PATH]{};
      GetModuleFileNameW(nullptr, exe_path_cfg, MAX_PATH);
      const std::string exe_dir_cfg = fs::path(exe_path_cfg).parent_path().string();

      if (act.preset_changed && renodx_now) {
        if (!prepare_renodx_runtime_dirs(exe_dir_cfg)) {
          session_notice.push(renodx_write_denied_notice(), now);
          ui.renodx_preset = clamp_renodx_visual_preset(cfg.renodx_preset);
        } else {
          const int preset_before = clamp_renodx_visual_preset(cfg.renodx_preset);
          cfg.renodx_preset = clamp_renodx_visual_preset(ui.renodx_preset);
          (void)sync_nr_pass_count_files(exe_dir_cfg, cfg.nr_pass_count, cfg.renodx_preset,
                                         &cfg.nr_advanced[clamp_nr_pass_count(cfg.nr_pass_count)]);
          (void)sync_renodx_visual_preset_files(exe_dir_cfg, cfg.renodx_preset, cfg.nr_pass_count,
                                                &cfg.nr_advanced[clamp_nr_pass_count(cfg.nr_pass_count)]);
          if (!cfg.perf_test) {
            PersistedSettings ps = default_settings();
            std::string serr;
            load_settings(ps, settings_default_path(), serr);
            ps.renodx_preset = cfg.renodx_preset;
            ps.nr_pass_count = cfg.nr_pass_count;
            save_settings(ps, settings_default_path(), serr);
          }
          cfg_restart_kind = plan_session_cfg_preset_apply(true);
          cfg_restart_pending = true;
          cfg_restart_until = now;
          write_restart_toast(session_notice_preset_apply(preset_before, cfg.renodx_preset, cfg.nr_pass_count));
          overlay.renodx_preset = renodx_visual_preset_name(cfg.renodx_preset, cfg.nr_pass_count);
        }
      }

      const SessionCfgApply plan =
          act.pass_count_changed
              ? plan_session_cfg_apply(renodx_now, act.pass_count_before, ui.nr_pass_count,
                                       feed_locked_scale, clamp_scale_percent(ui.scale_percent))
              : (act.scale_apply
                     ? plan_session_cfg_apply(renodx_now, cfg.nr_pass_count, cfg.nr_pass_count,
                                               feed_locked_scale,
                                               clamp_scale_percent(ui.scale_percent))
                     : SessionCfgApply::Live);

      if (act.pass_count_changed && renodx_now) {
        if (!prepare_renodx_runtime_dirs(exe_dir_cfg)) {
          session_notice.push(renodx_write_denied_notice(), now);
          ui.nr_pass_count = clamp_nr_pass_count(cfg.nr_pass_count);
        } else if (plan != SessionCfgApply::Live) {
          // Motor novo ou PassCount novo: o addon só lê isso no init, então processo novo.
          cfg.nr_pass_count = clamp_nr_pass_count(ui.nr_pass_count);
          (void)sync_nr_pass_count_files(exe_dir_cfg, cfg.nr_pass_count, cfg.renodx_preset,
                                         &cfg.nr_advanced[clamp_nr_pass_count(cfg.nr_pass_count)]);
          (void)sync_feed_work_resolution_in_dir(
              exe_dir_cfg, feed_work_resolution_for_session(cfg.scale_percent, cfg.nr_pass_count, info.width,
                                                           info.height),
              cfg.nr_pass_count);
          if (!cfg.perf_test) {
            PersistedSettings ps = default_settings();
            std::string serr;
            load_settings(ps, settings_default_path(), serr);
            ps.nr_pass_count = cfg.nr_pass_count;
            ps.renodx_preset = cfg.renodx_preset;
            ps.scale_percent = cfg.scale_percent;
            save_settings(ps, settings_default_path(), serr);
          }
          cfg_restart_kind = plan;
          cfg_restart_pending = true;
          cfg_restart_until = now;
          const bool motor = (plan == SessionCfgApply::RelaunchMotor);
          write_restart_toast(motor ? session_notice_dlss5_motor_swap(act.pass_count_before, cfg.nr_pass_count)
                                    : session_notice_nr_pass_count_apply(act.pass_count_before,
                                                                         cfg.nr_pass_count));
        } else {
          cfg.nr_pass_count = clamp_nr_pass_count(ui.nr_pass_count);
          const bool want = want_neural_rendering(true, cfg.neural_rendering);
          apply_lobby_nr_pass_count_live(exe_dir_cfg, cfg.nr_pass_count, want, &g_nr_runtime_on,
                                         cfg.renodx_preset,
                                         &cfg.nr_advanced[clamp_nr_pass_count(cfg.nr_pass_count)]);
          cfg.nr_runtime_on = g_nr_runtime_on;
          overlay.nr_pass_count = cfg.nr_pass_count;
          overlay.renodx_preset = renodx_visual_preset_name(cfg.renodx_preset, cfg.nr_pass_count);
          session_notice.push(std::string("Pass Count: ") + nr_pass_count_label(cfg.nr_pass_count), now);
        }
      } else if (act.pass_count_changed) {
        ui.nr_pass_count = clamp_nr_pass_count(cfg.nr_pass_count);
      }

      if (act.scale_apply) {
        const int want_scale = clamp_scale_percent(ui.scale_percent);
        cfg.scale_percent = want_scale;
        if (cfg_restart_pending) {
          (void)sync_feed_work_resolution_in_dir(
              exe_dir_cfg, feed_work_resolution_for_session(want_scale, cfg.nr_pass_count, info.width,
                                                           info.height),
              cfg.nr_pass_count);
        } else if (plan == SessionCfgApply::RecycleWorker) {
          cfg.scale_percent = want_scale;
          cfg_restart_kind = SessionCfgApply::RecycleWorker;
          cfg_restart_pending = true;
          cfg_restart_until = now;
          write_restart_toast(session_notice_feed_scale_apply(
              effective_scale_percent(renodx_now, feed_locked_scale, cfg.nr_pass_count, info.width, info.height),
              effective_scale_percent(renodx_now, want_scale, cfg.nr_pass_count, info.width, info.height)));
        } else {
          cfg.scale_percent = want_scale;
          renderer.set_scale_percent(app_present_scale_percent(renodx_now, want_scale));
          renderer.set_hud_work_scale_percent(
              renodx_now ? effective_scale_percent(true, want_scale, cfg.nr_pass_count, info.width, info.height)
                         : 0);
          overlay.scale_percent =
              effective_scale_percent(renodx_now, want_scale, cfg.nr_pass_count, info.width, info.height);
          session_notice.push(
              "Escala: " + std::to_string(effective_scale_percent(renodx_now, want_scale, cfg.nr_pass_count,
                                                                  info.width, info.height)) +
                  "%",
                              now);
        }
      }

      if ((act.nr_advanced_apply || act.nr_advanced_changed) && renodx_now) {
        if (!prepare_renodx_runtime_dirs(exe_dir_cfg)) {
          session_notice.push(renodx_write_denied_notice(), now);
        } else {
          if (act.nr_advanced_apply) copy_nr_advanced(cfg.nr_advanced, ui.nr_advanced);
          const int pass = clamp_nr_pass_count(cfg.nr_pass_count);
          (void)sync_nr_pass_count_files(exe_dir_cfg, pass, cfg.renodx_preset, &cfg.nr_advanced[pass]);
          (void)sync_renodx_visual_preset_files(exe_dir_cfg, cfg.renodx_preset, pass, &cfg.nr_advanced[pass]);
          if (!cfg.perf_test) {
            PersistedSettings ps = default_settings();
            std::string serr;
            load_settings(ps, settings_default_path(), serr);
            copy_nr_advanced(ps.nr_advanced, cfg.nr_advanced);
            ps.nr_pass_count = pass;
            ps.renodx_preset = cfg.renodx_preset;
            save_settings(ps, settings_default_path(), serr);
          }
          if (!cfg_restart_pending) {
            cfg_restart_kind = plan_session_cfg_preset_apply(true);
            cfg_restart_pending = true;
            cfg_restart_until = now;
            if (act.nr_advanced_apply && !act.close_requested) {
              cfg.open_session_cfg = true;
              cfg.open_nr_advanced = true;
            }
            write_restart_toast(std::string("Configs avancadas: ") + nr_pass_count_label(pass));
          }
        }
      }

      if (act.apply_user_preset && !cfg.perf_test) {
        PersistedSettings ps = default_settings();
        std::string serr;
        load_settings(ps, settings_default_path(), serr);
        UserPreset snap;
        snap.name = "applied";
        snap.hud_visible = act.apply_hud;
        snap.overlay_no_bg = act.apply_overlay_no_bg;
        snap.overlay_console = act.apply_overlay_console;
        snap.live_console = act.apply_live_console;
        snap.neural_rendering = act.apply_nr_on;
        snap.renodx_preset = act.apply_renodx_preset;
        snap.nr_pass_count = act.apply_nr_pass_count;
        snap.fg_enabled = act.apply_fg_on;
        snap.fg_multiplier = act.apply_fg_multiplier;
        snap.fg_queue = act.apply_fg_queue;
        snap.fg_flow_scale = act.apply_fg_flow_scale;
        snap.fg_adaptive = act.apply_fg_adaptive;
        snap.fg_target_fps = act.apply_fg_target_fps;
        snap.scale_percent = act.apply_scale_percent;
        copy_nr_advanced(snap.nr_advanced, cfg.nr_advanced);
        apply_user_preset_to_persisted(ps, snap, cfg_ui.fg_can_enable);
        save_settings(ps, settings_default_path(), serr);
        cfg.show_overlay = act.apply_hud;
        cfg.neural_rendering = act.apply_nr_on;
        if (!act.apply_preset_name.empty()) {
          const std::string msg = session_notice_named_preset_apply(act.apply_preset_name);
          if (cfg_restart_pending) write_restart_toast(msg);
          else session_notice.push(msg, now);
        }
      }

    }

    // 8.5 só lê o X no init: GDI segura o último frame (sem GPU); o processo novo é o motor.
    if (cfg_restart_pending && now >= cfg_restart_until) {
      if (cfg_ui.open() || g_cfg_open.load()) cfg.open_session_cfg = true;
      if (!renderer.took_hold_snapshot()) (void)save_motor_hold_from_hwnd(hwnd);
      renderer.clear_hold_snapshot();
      g_restart_notice.show(hi, session_restart_notice_title(),
                            session_restart_notice_detail(cfg_restart_kind));
      const bool cover_latched =
          g_restart_notice.has_hold_image() && g_restart_notice.present_hold_flip();
      if (g_restart_notice.visible()) renderer.release_flip_for_cover(cover_latched);
      std::printf("cfg restart: saindo para reiniciar na sessao\n");
      g_running = false;
      break;
    }
    // Passthrough enhance overwrites dlss_* — keep RenoDX NR status authoritative for HUD.
    if (cfg.enhance == EnhanceBackend::Renodx) {
      overlay.renodx_on = g_nr_runtime_on;
      overlay.dlss_on = g_nr_runtime_on;
      overlay.dlss_backend = "renodx+feed";
      overlay.dlss_status = g_nr_runtime_on ? "nr-on" : "nr-off";
      overlay.nr_pass_count = clamp_nr_pass_count(cfg.nr_pass_count);
      overlay.renodx_preset = renodx_visual_preset_name(cfg.renodx_preset, overlay.nr_pass_count);
      overlay.feed_on = g_nr_runtime_on;
    }
    last_dlss_ms = overlay.dlss_ms;
    overlay.present_ms = present_ms;
    ++presented;
    overlay.frames = presented;
    // Sessão de volta na tela: só agora o aviso de reinício pode sair.
    // No teste, o aviso fica até o DLSS 5 ligar — o relógio não começa no 1º frame.
    if (window_source && window_boot_tick != 0 && have_new && !holding) {
      char line[80];
      std::snprintf(line, sizeof(line), "primeiro frame %ums",
                    static_cast<unsigned>(GetTickCount() - window_boot_tick));
      note_window_boot_line(line);
      window_boot_tick = 0;
    }
    if (g_restart_notice.visible() && !cfg.perf_test && have_new && !holding) {
      if (hold_boot) ++hold_live_frames;
      // Soltar no 1º frame mostra FPS 0 e o menu do Ctrl+M ainda sem as abas.
      if (motor_hold_ready_to_show(hold_boot, hold_live_frames)) {
        if (restart_boot_tick != 0) {
          char line[160];
          std::snprintf(line, sizeof(line), "restart boot: imagem ao vivo em %ums",
                        static_cast<unsigned>(GetTickCount() - restart_boot_tick));
          note_restart_boot(line);
          restart_boot_tick = 0;
        }
        if (hold_boot) {
          renderer.restore_flip_after_cover();
          ShowWindow(hwnd, g_session_fullscreen ? SW_SHOW : SW_SHOWMAXIMIZED);
          MSG reveal{};
          while (PeekMessageW(&reveal, hwnd, 0, 0, PM_REMOVE)) {
            TranslateMessage(&reveal);
            DispatchMessageW(&reveal);
          }
        }
        g_restart_notice.destroy();
        clear_motor_hold_files();
      } else {
        g_restart_notice.pump();
      }
    }
    if ((fg_arm_pending || profile_pending || audio_start_pending) && presented >= 1 &&
        !(hold_boot && g_restart_notice.visible()) &&
        ((have_new && !holding) || presented >= 5)) {
      if (fg_arm_pending) {
        fg_arm_pending = false;
        profile_pending = false;
        std::string ferr;
        if (fg.arm_autostart(hwnd, ferr)) {
          std::printf("fg auto-on x%d (async, after frame)\n", fg.state().multiplier);
        } else {
          std::fprintf(stderr, "fg auto-on: %s\n", ferr.c_str());
          bug_exc("fg_auto_on", ferr);
          if (ferr.find("LosslessScaling") != std::string::npos || ferr.find("missing") != std::string::npos) {
            session_notice.push(session_notice_fg_missing(), clock.now_ms());
          }
        }
      } else if (profile_pending) {
        profile_pending = false;
        std::string ferr;
        if (!fg.apply_profile(ferr)) std::fprintf(stderr, "fg profile: %s\n", ferr.c_str());
      }
      if (audio_start_pending) {
        audio_start_pending = false;
        std::string aerr;
        if (!audio.start(audio_prefer, aerr)) {
          std::fprintf(stderr, "audio: %s (continuando sem som)\n", aerr.c_str());
        } else {
          audio.set_volume(cfg.capture_volume);
          audio.set_tone(cfg.capture_tone);
        }
      }
    }
    if (fg.autostart_waits_first_frame() && have_new && !holding) {
      fg.release_autostart_hotkey();
    }
    if (presented == 1 && !session_boot_toast.empty()) {
      session_notice.push(session_boot_toast, now);
      session_boot_toast.clear();
    }
    if (cfg.perf_test && !perf_scoring) {
      if (g_restart_notice.visible()) g_restart_notice.pump();
      if (perf_hold_t0 <= 0) perf_hold_t0 = now;
      if (presented % 8 == 0 || presented <= 2) {
        nr_evals += consume_reshade_nr_evals(session_exe_dir, reshade_eval_off);
      }
        const bool nr_live = perf_test_nr_evals_are_live(nr_evals) || g_nr_runtime_on;
      const bool want_nr_live = (cfg.enhance == EnhanceBackend::Renodx) && cfg.neural_rendering;
      if (nr_live && want_nr_live && !g_nr_runtime_on) {
        g_nr_runtime_on = true;
        cfg.nr_runtime_on = true;
        overlay.renodx_on = true;
        overlay.dlss_on = true;
        capture->rebase_playhead(now);
        if (clip_audio.running()) clip_audio.rebase_playhead();
      }
      const bool want_fg = cfg.fg_auto_start && cfg.fg_multiplier >= 2;
      const bool fg_on = overlay.fg_on;
      bool fg_unavailable = false;
      if (want_fg && perf_fg_kicked && !fg.busy()) {
        const FgStatus st = fg.state().status;
        fg_unavailable = (st == FgStatus::Missing || st == FgStatus::Error);
      }
      double fg_wait_ms = 0;
      if (perf_test_dlss5_ready(g_nr_runtime_on, presented, nr_live)) {
        if (perf_nr_ready_t0 <= 0) perf_nr_ready_t0 = now;
        fg_wait_ms = now - perf_nr_ready_t0;
      }
      // FG começa com o vídeo, em paralelo com os 120 frames do NR. O relógio espera os dois.
      if (want_fg && !perf_fg_kicked && !fg.busy() && presented >= 1) {
        std::string ferr;
        const bool warm = LosslessFgController::count_running_lossless_processes(true) > 0;
        const bool ok = warm ? fg.arm_autostart(hwnd, ferr) : fg.request_toggle(hwnd, ferr);
        if (ok) {
          perf_fg_kicked = true;
        } else if (presented <= 3) {
        }
      }
      const bool clock_ready =
          perf_test_should_start_clock(g_nr_runtime_on, presented, want_fg, fg_on, fg_unavailable, fg_wait_ms,
                                      nr_live);
      if (!clock_ready && perf_test_should_fail_pipeline(false, now - perf_hold_t0)) {
        write_perf_test_session_result(cfg, nullptr, "Pipeline timeout");
        perf_result_written = true;
        g_restart_notice.show(GetModuleHandleW(nullptr), perf_test_pipeline_fail_title(),
                              perf_test_pipeline_fail_detail());
        g_restart_notice.pump();
        Sleep(1600);
        g_restart_notice.destroy();
        g_return_to_lobby = g_session_from_lobby;
        g_running = false;
      } else if (clock_ready) {
        if (!perf_revealed) {
          renderer.present_black();
          show_session_window(hwnd, true);
          perf_revealed = true;
          perf_window_shown = true;
        }
        g_restart_notice.destroy();
        perf_scoring = true;
        perf_t0 = now;
        capture->rebase_playhead(now);
        if (clip_audio.running()) {
          clip_audio.rebase_playhead();
        } else if (cfg.perf_test_audio) {
        }
        perf_score_skip = 2;
        stats600.reset();
        perf_acc.reset();
        ft_win.reset();
        stalls_since_600 = 0;
        overlay.has_stats600 = false;
        if (g_restart_notice.visible()) g_restart_notice.destroy();
        session_notice.push(perf_test_counting_notice(), now);
      }
    }

    const bool hdmi_perf = session_perf_popup_wanted(cfg.perf_report, cfg.perf_test);
    const bool is_stall = (cfg.perf_test || hdmi_perf)
                              ? perf_test_is_stall(interval, cfg.fps, cfg.hitch_threshold_ms)
                              : (interval >= hitch_ms);
    if (cfg.perf_test && perf_scoring) {
      if (perf_score_skip > 0) {
        --perf_score_skip;
      } else {
        perf_acc.push(interval, present_ms, is_stall);
      }
    }
    if (!stats_frame_counts_toward_window(presented)) {
      max_boot_interval_ms = (std::max)(max_boot_interval_ms, interval);
    } else if (!warmup_logged) {
      warmup_logged = true;
      past_warmup = true;
      char wbuf[192];
      std::snprintf(wbuf, sizeof(wbuf),
                    "stats warmup done frames=%llu max_boot_interval=%.1f (excluded from 600-window)",
                    static_cast<unsigned long long>(kStatsWarmupFrames), max_boot_interval_ms);
      std::printf("%s\n", wbuf);
    }
    if (hdmi_perf && past_warmup) {
      perf_acc.push(interval, present_ms, is_stall);
    }

    if (is_stall) {
      ++stalls_since_600;
      if (past_warmup) ++stalls_after_warmup;
    }
    if (auto h = hitch.observe(presented, now, interval, 0, 0, present_ms)) {
      analyze.hitch(*h);
      ++report_hitch_count;
      {
        char dbuf[96];
        std::snprintf(dbuf, sizeof(dbuf), "interval_ms=%.2f", interval);
        if (bug_rep.active()) bug_rep.note_event(now, "hitch", dbuf);
        if (perf_rep.active()) perf_rep.note_event(now, "hitch", dbuf);
      }
      if (auto c = clusters.on_hitch(*h)) {
        ++cluster_count;
        if (past_warmup) ++clusters_after_warmup;
        overlay.clusters = cluster_count;
        analyze.cluster(*c);
        if (bug_rep.active()) bug_rep.note_event(now, "cluster", "");
        if (perf_rep.active()) perf_rep.note_event(now, "cluster", "");
      }
    } else {
      clusters.note_quiet(now);
    }

    analyze.frame_sample(now, presented, 0, 0, present_ms, overlay.e2e_ms, interval, slot.dropped(),
                         overlay.e2e_ms);

    if (stats_frame_counts_toward_window(presented) && (!cfg.perf_test || perf_scoring)) {
      ++steady_presented;
    if (auto s = stats600.push(interval, overlay.e2e_ms, present_ms, is_stall)) {
      hitch.reset_window_log_cap();
        analyze.stats_600(*s, now, last_dlss_ms, g_nr_runtime_on);
        overlay.has_stats600 = true;
        overlay.s600_fps = s->fps;
        overlay.s600_p95_e2e = s->p95_e2e_ms;
        overlay.s600_p95_present = s->p95_present_ms;
        overlay.s600_max_interval = s->max_interval_ms;
        overlay.s600_stalls = s->stalls;
        stalls_since_600 = 0;
        if (cfg.perf_test) nr_evals += consume_reshade_nr_evals(session_exe_dir, reshade_eval_off);
        char buf[320];
      std::snprintf(buf, sizeof(buf),
                      "600 frames: fps=%.2f mean=%.2fms p95_e2e=%.2f p95_present=%.2f stalls=%llu max=%.1f nr=%s "
                      "fg=%s evals=%d dlss_ms=%.2f",
                      s->fps, s->mean_interval_ms, s->p95_e2e_ms, s->p95_present_ms,
                      static_cast<unsigned long long>(s->stalls), s->max_interval_ms,
                      g_nr_runtime_on ? "on" : "off", overlay.fg_on ? "on" : "off", nr_evals, last_dlss_ms);
      std::printf("%s\n", buf);
      }
    }

    overlay.sync_image = renderer.vsync();
    overlay.hw_info = cfg.hw_info;
    if (cfg.hw_info || cfg.mini_overlay) {
      SystemMetricsSample hw{};
      hw_sampler.sample(renderer.device(), hw);
      overlay.cpu_ok = hw.cpu_ok;
      overlay.cpu_percent = hw.cpu_percent;
      overlay.cpu_temp_ok = hw.cpu_temp_ok;
      overlay.cpu_temp_c = hw.cpu_temp_c;
      overlay.gpu_ok = hw.gpu_ok;
      overlay.gpu_percent = hw.gpu_percent;
      overlay.gpu_temp_ok = hw.gpu_temp_ok;
      overlay.gpu_temp_c = hw.gpu_temp_c;
      overlay.vram_ok = hw.vram_ok;
      overlay.vram_used_mb = hw.vram_used_mb;
      overlay.vram_budget_mb = hw.vram_budget_mb;
      overlay.ram_ok = hw.ram_ok;
      overlay.ram_used_mb = hw.ram_used_mb;
      overlay.ram_total_mb = hw.ram_total_mb;
      overlay.power_ok = hw.power_ok;
      overlay.power_w = hw.power_w;
      overlay.cpu_name = hw.cpu_name;
      overlay.gpu_name = hw.gpu_name;
    }
    live.update(overlay);
    if (live_active && live.take_console_changed()) {
      cfg.live_console = live.console();
      overlay.live_console = cfg.live_console;
      if (!cfg.perf_test) {
        PersistedSettings ps = default_settings();
        std::string serr;
        load_settings(ps, settings_default_path(), serr);
        ps.ui.live_overlay = true;
        ps.ui.overlay_console = true;
        ps.ui.live_console = live_console_key(clamp_live_console(cfg.live_console));
        save_settings(ps, settings_default_path(), serr);
      }
    }
    hud.update(overlay);
    {
      const bool ov = g_overlay.load();
      if (ov != overlay_was) {
        overlay_was = ov;
        if (bug_rep.active()) bug_rep.note_event(now, "hud_toggle", ov ? "on" : "off");
        if (perf_rep.active()) perf_rep.note_event(now, "hud_toggle", ov ? "on" : "off");
      }
      const bool have_hw = sample_hw;
      auto tick_rep = [&](SessionReportWriter& w) {
        if (!w.active()) return;
        w.heartbeat(now, fps_smooth, overlay.e2e_ms, present_ms, overlay.ft_avg_ms, overlay.ft_p95_ms,
                    slot.dropped(), stalls_since_600, report_hitch_count, overlay.fg_on, g_nr_runtime_on,
                    overlay.cpu_percent, overlay.gpu_percent, have_hw, overlay.has_stats600,
                    overlay.s600_p95_e2e, overlay.s600_p95_present);
      };
      tick_rep(bug_rep);
      tick_rep(perf_rep);
    }

    if ((cfg.smoke || cfg.frames_limited || cfg.selftest_dlss) &&
        static_cast<int>(steady_presented) >= cfg.smoke_frames) {
      g_running = false;
    }
    if (cfg.selftest_cap && presented >= 180) {
      g_running = false;
    }
    if (cfg.perf_test) {
      const bool time_up = perf_t0 > 0 && (now - perf_t0) >= static_cast<double>(kPerfTestMaxSec) * 1000.0;
      if (perf_test_should_stop(time_up)) {
        g_running = false;
        if (g_session_from_lobby) g_return_to_lobby = true;
      }
    }
  }

  cfg.nr_runtime_on = g_nr_runtime_on;
  if (cfg.perf_test && !perf_result_written) write_perf_test_session_result(cfg, &perf_acc, nullptr);

  if (bug_rep.active()) {
    bug_rep.end(clock.now_ms(), presented, 0, report_hitch_count, cluster_count);
  }
  if (perf_rep.active()) {
    perf_rep.end(clock.now_ms(), presented, 0, report_hitch_count, cluster_count);
  }

  live.destroy();
  hud.destroy();
  g_session_cfg = nullptr;
  renderer.set_ui_draw_callback(nullptr, nullptr);
  renderer.set_hold_probe(nullptr, nullptr);
  cfg_ui.shutdown();
  g_cfg_open = false;
  g_hdmi_notice.destroy();
  g_restart_notice.pump();  // o aviso (último frame) segue vivo enquanto FG/captura/D3D caem
  if (!cfg_restart_pending) renderer.present_black();
  // Esc/timer do Benchmark: mata FG+LS ANTES de esconder a janela (WM_CLOSE so vai ao tray).
    if (cfg.perf_test) {
    if (overlay.fg_on || fg.overlay_reads_on(hwnd) || fg.state().active) fg.pulse_fg_hotkey();
    (void)fg.kill_all_lossless_and_wait();
  }
  hide_session_window(hwnd);
  analyze.update_runtime_flags(g_nr_runtime_on, overlay.fg_on, overlay.fg_multiplier, overlay.fg_status);
  if (analyze_was || analyze.line_count() > 1) {
    analyze.session_end(clock.now_ms());
    std::string jsonl, summary;
    write_analyze_files(cfg.log_dir, analyze, clock.now_ms(), jsonl, summary);
  }
  if (cfg_restart_pending) {
    (void)fg.detach_for_restart();
  } else if (g_return_to_lobby.load() && cfg.fg_auto_start && cfg.fg_multiplier >= 2) {
    (void)fg.detach_for_restart();
  } else {
    fg.shutdown();
  }

  // Release capture device + D3D before HWND so lobby preview can reopen.
  g_session_exiting = true;
  g_session_renderer = nullptr;
  renderer.set_enhance(nullptr);
  if (enhance) {
    enhance->shutdown();
    enhance.reset();
  }
  if (capture) {
    capture->stop();
    capture.reset();
  }
  audio.stop();
  clip_audio.stop();
  pipeline_global().stop();
  renderer.shutdown();
  if (hwnd) {
    DestroyWindow(hwnd);
    hwnd = nullptr;
  }
  g_restart_notice.pump();
  {
    MSG drain{};
    while (PeekMessageW(&drain, nullptr, 0, 0, PM_REMOVE)) {
      if (drain.message == WM_QUIT) continue;
      TranslateMessage(&drain);
      DispatchMessageW(&drain);
    }
  }

  // CFG pediu reinício: main() salva e relança com --autostart (o aviso continua na tela).
  if (cfg_restart_pending) {
    if (!g_restart_notice.visible()) {
      g_restart_notice.show(hi, session_restart_notice_title(),
                            session_restart_notice_detail(cfg_restart_kind));
    } else {
      g_restart_notice.pump();
    }
    g_quit_clock = nullptr;
    return kSessionExitRelaunch;
  }

  if (g_return_to_lobby.load()) {
    if (session_perf_popup_wanted(cfg.perf_report, cfg.perf_test) && !perf_result_written) {
      write_perf_test_session_result(cfg, &perf_acc, nullptr);
    }
    // Strip session scale from ReShade feed before lobby Present (menu must stay sharp).
    {
      wchar_t exe_path[MAX_PATH]{};
      GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
      const fs::path exe_dir = fs::path(exe_path).parent_path();
      const int lobby_feed = feed_work_resolution_for_lobby();
      if (sync_feed_work_resolution_in_dir(exe_dir.string(), lobby_feed)) {
      }
      sync_mgf_techniques_for_phase(exe_dir.string(), cfg.enhance == EnhanceBackend::Renodx);
    }
    std::printf("back to lobby\n");
    g_quit_clock = nullptr;
    return kExitBackToLobby;
  }
  g_quit_clock = nullptr;

  if (cfg.selftest_cap) {
    if (presented < 100) {
      std::printf("FAIL selftest-cap: only %llu frames\n", static_cast<unsigned long long>(presented));
      return 4;
    }
    std::printf("PASS selftest-cap frames=%llu\n", static_cast<unsigned long long>(presented));
  }
  if (cfg.selftest_dlss) {
    const double elapsed_s = (clock.now_ms() - (prev_t - presented * (1000.0 / 60.0))) / 1000.0;
    const double fps = (presented > 1 && elapsed_s > 0) ? (presented / elapsed_s) : 0;
    if (presented < 100) {
      std::printf("FAIL selftest-dlss: only %llu frames\n", static_cast<unsigned long long>(presented));
      return 4;
    }
    if (clusters_after_warmup > 0) {
      std::printf("FAIL selftest-dlss: clusters_after_warmup=%llu\n",
                  static_cast<unsigned long long>(clusters_after_warmup));
      return 5;
    }
    if (past_warmup && fps < 59.7) {
      std::printf("FAIL selftest-dlss: fps=%.2f < 59.7\n", fps);
      return 6;
    }
    std::printf("PASS selftest-dlss frames=%llu fps≈%.2f stalls_after_warmup=%llu clusters_after_warmup=%llu\n",
                static_cast<unsigned long long>(presented), fps,
                static_cast<unsigned long long>(stalls_after_warmup),
                static_cast<unsigned long long>(clusters_after_warmup));
  }
  if (cfg.selftest_cfg) {
    if (!cfg_ui_ready) {
      std::printf("FAIL selftest-cfg: ImGui nao inicializou na sessao\n");
      return 8;
    }
    if (presented < 30) {
      std::printf("FAIL selftest-cfg: only %llu frames\n", static_cast<unsigned long long>(presented));
      return 8;
    }
    if (cfg_ui_max_vtx <= 0) {
      std::printf("FAIL selftest-cfg: menu aberto mas sem draw data\n");
      return 8;
    }
    std::printf("PASS selftest-cfg frames=%llu vertices=%d\n", static_cast<unsigned long long>(presented),
                cfg_ui_max_vtx);
  }
  if (cfg.smoke) {
    std::printf("PASS smoke frames=%llu\n", static_cast<unsigned long long>(presented));
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // Avoid Windows DPI virtualization cropping/zooming the swapchain.
#if defined(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif

  auto cfg = nexuscap::parse_args(argc, argv);
  nexuscap::init_app_paths(cfg.portable);
  wchar_t exe_path[MAX_PATH];
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  fs::path base = fs::path(exe_path).parent_path();
  // Logs: AppData no setup; pasta do exe no portable.
  cfg.log_dir = nexuscap::app_logs_dir();
  // Reinício pedido pelo CFG: o aviso reaparece aqui e só sai depois que a imagem assenta.
  if (cfg.perf_test) {
    g_restart_notice.show(GetModuleHandleW(nullptr), nexuscap::perf_test_pipeline_notice_title(),
                          nexuscap::perf_test_pipeline_notice_detail());
  } else if (nexuscap::restart_notice_visible_at_boot(cfg.autostart, nexuscap::motor_hold_image_exists())) {
    g_restart_notice.show(GetModuleHandleW(nullptr), nexuscap::session_restart_notice_title(),
                          nexuscap::session_restart_notice_detail(nexuscap::SessionCfgApply::Live));
    if (g_restart_notice.has_hold_image()) nexuscap::write_motor_hold_ready();
  }
  (void)nexuscap::prepare_renodx_runtime_dirs(base.string());
  if (g_restart_notice.visible()) g_restart_notice.pump();
  // Park/restore happens after settings — o lobby não carrega o ReShade.
  if (!fs::path(cfg.log_dir).is_absolute() && cfg.log_dir == "logs") {
    cfg.log_dir = (base / cfg.log_dir).string();
  }

  nexuscap::PersistedSettings settings = nexuscap::default_settings();
  {
    std::string err;
    nexuscap::load_settings(settings, nexuscap::settings_default_path(), err);
  }
  nexuscap::max_perf_boot(settings.max_perf);
  if (cfg.perf_test) {
    nexuscap::PerfTestPlan plan;
    if (!cfg.perf_cli_override &&
        nexuscap::read_perf_test_plan_file(nexuscap::default_perf_test_plan_path(nexuscap::app_data_dir()),
                                           plan)) {
      nexuscap::apply_perf_test_plan_to_config(plan, cfg);
    } else {
      nexuscap::apply_perf_test_plan_to_config(nexuscap::perf_test_plan_from_config(cfg), cfg);
    }
    cfg.capture_volume = nexuscap::clamp_capture_volume_percent(settings.capture_volume);
    cfg.capture_tone = nexuscap::clamp_capture_tone(settings.capture_tone);
    cfg.fg_flow_scale = nexuscap::clamp_fg_flow_scale(settings.fg_flow_scale);
    if (cfg.fg_path.empty()) cfg.fg_path = settings.fg_path;
    {
      std::string bundled;
      if (nexuscap::resolve_perf_test_video(base.string(), cfg.width, cfg.height, bundled))
        cfg.video_path = bundled;
    }
  }
  // --autostart / --no-lobby não passam pelo lobby. O cfg vazio apagaria o 1,5x no sync da sessão.
  if (!cfg.perf_test) nexuscap::seed_cfg_nr_advanced_from_settings(cfg, settings);
  // Before ReShade loads: AddonPath → addons-multipass (corrige ini antigo em addons-stable).
  {
    const int boot_pass = cfg.perf_test ? cfg.nr_pass_count : settings.nr_pass_count;
    const int boot_preset = cfg.perf_test ? cfg.renodx_preset : settings.renodx_preset;
    const int boot_scale = cfg.perf_test ? cfg.scale_percent : settings.scale_percent;
    const int boot_w = cfg.perf_test ? cfg.width : settings.width;
    const int boot_h = cfg.perf_test ? cfg.height : settings.height;
    const auto ar = nexuscap::sync_renodx_addon_variant(base.string(), boot_pass);
    if (ar == nexuscap::RenodxAddonSyncResult::OkSwapped) {
      std::printf("renodx addon: multipass\n");
    } else if (ar == nexuscap::RenodxAddonSyncResult::MissingVariant) {
      std::printf("renodx addon: stack missing (run tools\\setup_renodx.ps1)\n");
    }
    const nexuscap::NrAdvancedLook* boot_look =
        cfg.perf_test ? nullptr : &settings.nr_advanced[nexuscap::clamp_nr_pass_count(boot_pass)];
    (void)nexuscap::sync_nr_pass_count_files(base.string(), boot_pass, boot_preset, boot_look);
    (void)nexuscap::sync_renodx_visual_preset_files(base.string(), boot_preset, boot_pass, boot_look);
    // Feed locks work_resolution at the first swapchain: 100% when the lobby opens first
    // (sharp menus), session scale when we boot straight into capture (--autostart / --perf-test).
    const bool lobby_first = cfg.show_lobby && !cfg.autostart && !cfg.perf_test;
    {
      const bool want_nr = cfg.perf_test ? true : (settings.enhance_renodx && settings.neural_rendering);
      (void)nexuscap::sync_neural_rendering_enabled_files(base.string(), want_nr, want_nr);
      // HDMI --autostart must mount the same MGF chain as lobby Iniciar. Leaving
      // Techniques= empty (lobby CFG live OFF) boots passthrough with HUD nr=on.
      (void)nexuscap::sync_mgf_techniques_for_phase(base.string(), want_nr && !lobby_first);
    }
    (void)nexuscap::sync_feed_work_resolution_in_dir(
        base.string(),
        nexuscap::feed_work_resolution_at_boot(lobby_first, boot_scale, boot_pass, boot_w, boot_h),
        lobby_first ? 1 : boot_pass);
  }
  if (g_restart_notice.visible()) g_restart_notice.pump();
  // CLI overrides persisted where explicit.
  {
    const std::string exe = base.string();
    const bool session_dxgi = cfg.autostart || cfg.perf_test || !cfg.show_lobby;
    const bool lobby_dxgi =
        !session_dxgi &&
        nexuscap::lobby_loads_reshade_dxgi(settings.ui.preview_enabled, settings.ui.preview_cfg_live,
                                           settings.enhance_renodx);
    if (session_dxgi || lobby_dxgi) {
      (void)nexuscap::restore_reshade_dxgi(exe);
    } else {
      (void)nexuscap::park_reshade_dxgi_wait(exe, 2500);
    }
  }
  if (g_restart_notice.visible()) g_restart_notice.pump();
  if (!cfg.show_lobby) {
    // keep cfg from CLI
  } else if (cfg.perf_test) {
    // Plano ja aplicado — nao sobrescrever com o CFG HDMI.
  } else {
    // Seed lobby from disk unless CLI forced renodx/fg/device already atypical — lobby owns UX.
    cfg.device_name = settings.device_name;
    cfg.width = settings.width;
    cfg.height = settings.height;
    cfg.fps = settings.fps;
    if (cfg.autostart && !cfg.perf_test &&
        !nexuscap::capture_source_is_window(nexuscap::clamp_capture_source(settings.capture_source))) {
      const auto card_modes = nexuscap::list_video_capture_modes(settings.device_name);
      const double snapped =
          nexuscap::capture_fps_saved_on_card(card_modes, cfg.width, cfg.height, cfg.fps);
      cfg.fps = snapped;
      settings.fps = snapped;
    }
    cfg.scale_percent = settings.scale_percent;
    cfg.renodx_preset = nexuscap::clamp_renodx_visual_preset(settings.renodx_preset);
    cfg.nr_pass_count = nexuscap::clamp_nr_pass_count(settings.nr_pass_count);
    cfg.video_fourcc = nexuscap::clamp_video_fourcc(settings.video_fourcc);
    cfg.color_space = nexuscap::clamp_color_space(settings.color_space);
    cfg.color_range = nexuscap::clamp_color_range(settings.color_range);
    cfg.capture_buffering = nexuscap::clamp_capture_buffering(settings.capture_buffering);
    cfg.hotkeys = settings.hotkeys;
    cfg.show_overlay = settings.ui.show_fps;
    cfg.advanced_metrics = settings.ui.advanced_metrics;
    cfg.mini_overlay = settings.ui.mini_overlay;
    cfg.overlay_no_bg = settings.ui.overlay_no_bg;
    cfg.capture_volume = nexuscap::clamp_capture_volume_percent(settings.capture_volume);
    cfg.capture_tone = nexuscap::clamp_capture_tone(settings.capture_tone);
    cfg.capture_source = nexuscap::clamp_capture_source(settings.capture_source);
    cfg.window_title = settings.window_title;
    cfg.window_hwnd = settings.window_hwnd;
    cfg.hw_info = settings.ui.hw_info;
    cfg.hide_panel_on_capture = true;
    cfg.sync_image = settings.ui.sync_image;
    // Also needed for --autostart (lobby skipped): reports must survive a scale relaunch.
    cfg.bug_report = settings.ui.bug_report;
    cfg.perf_report = settings.ui.perf_report;
    cfg.live_overlay = settings.ui.live_overlay;
    cfg.overlay_console = settings.ui.overlay_console;
    cfg.live_console = static_cast<int>(nexuscap::parse_live_console(settings.ui.live_console));
    cfg.exclusive_fullscreen = settings.ui.fullscreen;
    if (settings.fg_enabled) {
      cfg.fg_multiplier = nexuscap::armed_fg_multiplier(true, settings.fg_multiplier);
      cfg.fg_auto_start = true;
    } else {
      cfg.fg_multiplier = nexuscap::armed_fg_multiplier(false, settings.fg_multiplier);
      cfg.fg_auto_start = false;
    }
    if (cfg.fg_path.empty()) cfg.fg_path = settings.fg_path;
    cfg.fg_queue = nexuscap::clamp_fg_queue(settings.fg_queue);
    cfg.fg_flow_scale = nexuscap::clamp_fg_flow_scale(settings.fg_flow_scale);
    cfg.fg_adaptive = settings.fg_adaptive;
    if (settings.enhance_renodx) {
      cfg.enhance = nexuscap::EnhanceBackend::Renodx;
      cfg.dlss = false;
      cfg.neural_rendering = settings.neural_rendering;
    } else {
      cfg.enhance = nexuscap::EnhanceBackend::Off;
      cfg.neural_rendering = false;
    }
  }
  // Motor / Pass Count / escala: o overlay da sessão volta; os checks do lobby ficam.
  (void)nexuscap::apply_autostart_overlay_hud(cfg);

  HINSTANCE hi = GetModuleHandleW(nullptr);
  if (!cfg.show_lobby) {
    g_session_from_lobby = false;
    const int rc = run_app(hi, cfg);
    // Sessão sem lobby (--no-lobby / selftests) também reinicia pelo CFG e volta pra captura.
    if (rc == nexuscap::kSessionExitRelaunch) {
      if (!cfg.perf_test) {
        nexuscap::apply_session_cfg_to_persisted(cfg, settings);
        std::string err;
        nexuscap::save_settings(settings, nexuscap::settings_default_path(), err);
      }
      const std::string relaunch =
          nexuscap::session_cfg_relaunch_args_for_hud(cfg.show_overlay, cfg.mini_overlay,
                                                     cfg.advanced_metrics, cfg.live_overlay,
                                                     cfg.open_session_cfg, cfg.open_nr_advanced);
      std::printf("cfg restart: relaunch %s\n", relaunch.c_str());
      nexuscap::reset_motor_hold_ready();
      if (nexuscap::relaunch_current_process(relaunch.c_str())) {
        (void)nexuscap::wait_motor_hold_ready(2500, &g_restart_notice);
        return 0;
      }
      g_restart_notice.destroy();
      std::fprintf(stderr, "cfg restart: falha ao reiniciar\n");
      return rc;
    }
    return rc;
  }

  // Lobby ↔ session loop: always open lobby first (maximized window). Esc from capture returns.
  g_session_from_lobby = true;
  bool skip_lobby_once = cfg.autostart || cfg.perf_test;
  for (;;) {
    if (skip_lobby_once) {
      skip_lobby_once = false;
    } else if (!nexuscap::run_lobby(hi, cfg, settings)) {
      std::printf("lobby cancelled\n");
      return 0;
    }
    if (!cfg.perf_test) {
      std::string err;
      nexuscap::save_settings(settings, nexuscap::settings_default_path(), err);
    }
    const bool was_perf_test = cfg.perf_test;
    const int session_scale = cfg.scale_percent;
    const bool session_renodx = cfg.enhance == nexuscap::EnhanceBackend::Renodx;
    const int rc = run_app(hi, cfg);
    if (was_perf_test) {
      // Teste so vive em perf_test_plan/last. Lobby volta ao HDMI (escala/Pass/tom/NR/FG).
      cfg.perf_test = false;
      nexuscap::apply_persisted_to_config(settings, cfg);
      cfg.popup_perf_result = true;
      {
        wchar_t exe_path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        const auto exe_dir = fs::path(exe_path).parent_path().string();
        (void)nexuscap::restore_hdmi_pipeline_after_perf_test(exe_dir, settings);
        nexuscap::LosslessFgController fg;
        std::string ferr;
        if (!fg.apply_hdmi_profile_without_launch(settings.fg_enabled, settings.fg_multiplier,
                                                  settings.fg_path, ferr, settings.fg_queue,
                                                  settings.fg_adaptive, settings.fg_target_fps,
                                                  nexuscap::display_refresh_hz(nullptr),
                                                  settings.fg_flow_scale)) {
        } else {
        }
      }
    } else {
      // PageDn/PageUp mid-session must stick after Esc → lobby (checkbox + xN).
      apply_session_fg_to_persisted(cfg.fg_auto_start, cfg.fg_multiplier, settings);
      settings.fg_queue = nexuscap::clamp_fg_queue(cfg.fg_queue);
      settings.fg_flow_scale = nexuscap::clamp_fg_flow_scale(cfg.fg_flow_scale);
      settings.fg_adaptive = cfg.fg_adaptive;
      settings.fg_target_fps = nexuscap::clamp_fg_adaptive_target_stored(cfg.fg_target_fps);
      // Delete mid-session must stick after Esc → lobby (NR flag), but keep RenoDX loaded.
      apply_session_nr_to_persisted(cfg.neural_rendering, cfg.enhance == nexuscap::EnhanceBackend::Renodx,
                                    settings);
      settings.ui.live_overlay = cfg.live_overlay;
      settings.ui.overlay_console = cfg.overlay_console;
      settings.ui.overlay_no_bg = cfg.overlay_no_bg;
      settings.capture_volume = nexuscap::clamp_capture_volume_percent(cfg.capture_volume);
      settings.ui.live_console =
          nexuscap::live_console_key(nexuscap::clamp_live_console(cfg.live_console));
      {
        std::string err;
        nexuscap::save_settings(settings, nexuscap::settings_default_path(), err);
      }
    }
    // CFG na captura mudou o motor 1x ou a escala do Feed: salva e volta direto pra sessão.
    if (rc == nexuscap::kSessionExitRelaunch) {
      if (was_perf_test) {
        nexuscap::write_perf_test_plan_file(nexuscap::default_perf_test_plan_path(nexuscap::app_data_dir()),
                                            nexuscap::perf_test_plan_from_config(cfg));
        const std::string relaunch = "--perf-test";
        std::printf("cfg restart: relaunch %s\n", relaunch.c_str());
        if (nexuscap::relaunch_current_process(relaunch.c_str())) return 0;
      } else {
      nexuscap::apply_session_cfg_to_persisted(cfg, settings);
      {
        std::string err;
        nexuscap::save_settings(settings, nexuscap::settings_default_path(), err);
      }
      const std::string relaunch =
          nexuscap::session_cfg_relaunch_args_for_hud(cfg.show_overlay, cfg.mini_overlay,
                                                     cfg.advanced_metrics, cfg.live_overlay,
                                                     cfg.open_session_cfg, cfg.open_nr_advanced);
      std::printf("cfg restart: relaunch %s\n", relaunch.c_str());
      nexuscap::reset_motor_hold_ready();
      if (nexuscap::relaunch_current_process(relaunch.c_str())) {
        (void)nexuscap::wait_motor_hold_ready(2500, &g_restart_notice);
        return 0;
      }
      }
      // Não deu pra relançar: fecha o aviso e abre o lobby em vez de sair calado.
      g_restart_notice.destroy();
      std::fprintf(stderr, "cfg restart: falha ao reiniciar — abrindo o lobby\n");
      continue;
    }
    if (rc != kExitBackToLobby) return rc;
    // Benchmark: processo novo. Pass/tom/NR/Feed/LS do teste morrem com o processo antigo.
    // Escala <100%: o Feed travou no swapchain e borraria o menu.
    if (session_renodx || nexuscap::perf_test_return_needs_fresh_process(was_perf_test) ||
        nexuscap::feed_scale_change_needs_relaunch(session_renodx, session_scale,
                                                   nexuscap::kLobbyFeedWorkResolution)) {
      nexuscap::PerfTestResult last_perf{};
      const bool hdmi_popup =
          !was_perf_test &&
          nexuscap::read_perf_test_result_file(nexuscap::default_perf_test_result_path(nexuscap::app_data_dir()),
                                               last_perf) &&
          last_perf.valid && last_perf.hdmi_session && last_perf.popup;
      std::printf("back to lobby: restarting so lobby boots HDMI (not the test pipeline)\n");
      if (nexuscap::relaunch_current_process(nexuscap::lobby_return_relaunch_args(was_perf_test, hdmi_popup)))
        return 0;
    }
    // Drop leftover Esc from "back to lobby" before the menu sees it.
    {
      MSG drain{};
      while (PeekMessageW(&drain, nullptr, 0, 0, PM_REMOVE)) {
        if (drain.message == WM_QUIT) continue;
        const int vk = static_cast<int>(drain.wParam);
        if (nexuscap::SessionQuitGate::is_stale_quit_message(drain.message, vk, settings.hotkeys.vk_quit))
          continue;
        TranslateMessage(&drain);
        DispatchMessageW(&drain);
      }
    }
  }
}
