#include "nexuscap/analyze.hpp"
#include "nexuscap/audio.hpp"
#include "nexuscap/audio_select.hpp"
#include "nexuscap/capture.hpp"
#include "nexuscap/capture_format.hpp"
#include "nexuscap/clock.hpp"
#include "nexuscap/config.hpp"
#include "nexuscap/dlss_options.hpp"
#include "nexuscap/dlss5_bridge.hpp"
#include "nexuscap/enhance.hpp"
#include "nexuscap/frametime.hpp"
#include "nexuscap/hitch.hpp"
#include "nexuscap/live_mode.hpp"
#include "nexuscap/lobby.hpp"
#include "nexuscap/lossless_fg.hpp"
#include "nexuscap/renderer.hpp"
#include "nexuscap/session_cfg.hpp"
#include "nexuscap/session_report.hpp"
#include "nexuscap/settings_store.hpp"
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
// RenoDX runtime NR — carry from lobby via cfg.nr_runtime_on (same process often keeps NR ON).
bool g_nr_runtime_on = false;
bool g_renodx_multipass = false;  // active stack (AddonPath) — multipass owns NR, no hotkey
int g_nr_boot_retries = 0;
double g_nr_toggle_last_ms = 0;  // Delete debounce (bit30 + time)
nexuscap::NrConfirmPending g_nr_confirm{};  // async log ACK — no Present freeze
std::atomic<bool> g_cycle_fg{false};
// CFG na captura (Ctrl+M): a janela só encosta nas flags; o loop aplica.
std::atomic<bool> g_toggle_cfg{false};
std::atomic<bool> g_close_cfg{false};
std::atomic<bool> g_cfg_open{false};
nexuscap::SessionCfgOverlay* g_session_cfg = nullptr;
// Global de propósito: tem de continuar na tela depois do run_app, até o ExitProcess do relaunch.
nexuscap::RestartNotice g_restart_notice;
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
      // Bit 30 = auto-repeat KEYDOWN — one key = one overlay toggle.
      const bool is_repeat = (lp & (1u << 30)) != 0;
      const bool ctrl_down = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      if (!is_repeat &&
          nexuscap::hud_hotkey_fired(vk, ctrl_down, g_hotkeys.vk_hud, g_hotkeys.hud_ctrl)) {
        g_overlay = !g_overlay;
      }
      if (vk == g_hotkeys.vk_analyze || vk == VK_INSERT || vk == 'A' || vk == 'a') g_analyze = !g_analyze;
      if (vk == g_hotkeys.vk_dlss) g_toggle_dlss = true;
      if (vk == g_hotkeys.vk_fg_toggle) g_toggle_fg = true;
      if (vk == g_hotkeys.vk_fg_cycle) g_cycle_fg = true;
      // Bit 30 = auto-repeat KEYDOWN — one Delete must be one Bridge pulse (else HUD/RenoDX desync).
      if (vk == g_hotkeys.vk_nr_toggle && (lp & (1u << 30)) == 0) g_toggle_nr = true;
      if (vk == g_hotkeys.vk_fullscreen) g_toggle_fs = true;
      return 0;
    }
    case WM_CLOSE:
      g_running = false;
      if (g_session_from_lobby) g_return_to_lobby = true;
      DestroyWindow(hwnd);
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

void append_log(const std::string& dir, const std::string& line) {
  fs::create_directories(dir);
  std::ofstream f(dir + "/nexuscap.log", std::ios::app);
  if (f) f << line << "\n";
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
  if (esc_down) append_log(cfg.log_dir, "quit-gate: Esc held at session start — waiting KEYUP");

  // Escala que o Feed travou neste processo (lobby abre em 100%, --autostart já entra escalado).
  // O CFG da sessão compara com ela pra saber se aplicar a escala pede reinício.
  int feed_locked_scale =
      feed_work_resolution_at_boot(cfg.show_lobby && !cfg.autostart, cfg.scale_percent);

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
      if (want_nr && !g_nr_runtime_on) {
        // Fresh session + NeuralUplift=1: RenoDX boots NR ON. Skip Bridge pulse.
        g_nr_boot_retries = 0;
        g_nr_runtime_on = true;
        cfg.nr_runtime_on = true;
        append_log(cfg.log_dir, "nr boot: trust NeuralUplift ON (no enable-pulse)");
      } else {
        g_nr_boot_retries = boot.need_bridge_pulse ? 180 : 0;  // e.g. turn OFF after lobby
      }
    }

    if (cfg.enhance == EnhanceBackend::Renodx) {
      const fs::path feed_cfg = exe_dir / "dlss5-feed.cfg";
      const bool any_feed = fs::exists(feed_cfg) || fs::exists(exe_dir / "addons-stable" / "dlss5-feed.cfg") ||
                            fs::exists(exe_dir / "addons-multipass" / "dlss5-feed.cfg");
      if (any_feed) {
        int before = 100;
        if (fs::exists(feed_cfg)) {
          std::ifstream in(feed_cfg);
          std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
          in.close();
          before = parse_feed_work_resolution(text, 100);
        }
        const int want = feed_work_resolution_for_session(cfg.scale_percent);
        // Este write e anterior ao primeiro swapchain (--autostart / Iniciar com relancamento).
        // O addon le o arquivo no init, entao o valor travado e o que acabamos de gravar.
        if (sync_feed_work_resolution_in_dir(exe_dir.string(), want)) {
          feed_locked_scale = want;
          append_log(cfg.log_dir, "scale feed work_resolution " + std::to_string(before) + " -> " +
                                      std::to_string(want) + " (session ui=" + std::to_string(cfg.scale_percent) + ")");
          if (before != want) {
            std::printf("renodx feed work_resolution %d → %d (session scale)\n", before, want);
          }
        } else {
          feed_locked_scale = before;
        }
      }
      if (sync_nr_pass_count_files(exe_dir.string(), cfg.nr_pass_count, cfg.renodx_preset)) {
        append_log(cfg.log_dir, std::string("nr pass count → ") + std::to_string(clamp_nr_pass_count(cfg.nr_pass_count)));
        std::printf("nr pass count: %d (%s)\n", clamp_nr_pass_count(cfg.nr_pass_count),
                    nr_pass_count_label(cfg.nr_pass_count));
      }
      // Visual por ultimo: look defaults do Pass Count nao podem deixar NRStyle=0 no Cinema.
      if (sync_renodx_visual_preset_files(exe_dir.string(), cfg.renodx_preset)) {
        append_log(cfg.log_dir, std::string("renodx preset → ") + renodx_visual_preset_name(cfg.renodx_preset));
        std::printf("renodx preset: %s (NR=%d)\n", renodx_visual_preset_name(cfg.renodx_preset),
                    renodx_nr_preset_index(cfg.renodx_preset));
      }
    }

    // Sessão sempre reinicia no NR: o feed pode seguir o NeuralUplift. Se ficar
    // enabled=1 com NR=OFF, o DLSS5 continua a avaliar (log: restart sem mudar a imagem).
    if (cfg.enhance == EnhanceBackend::Renodx) {
      if (sync_neural_rendering_enabled_files(exe_dir.string(), want_nr, /*feed_on=*/want_nr)) {
        append_log(cfg.log_dir, std::string("neural rendering files NR=") + (want_nr ? "ON" : "OFF") +
                                    " feed=" + (want_nr ? "ON" : "OFF") + " (runtime pulse pending=" +
                                    std::to_string(g_nr_boot_retries > 0) + ")");
        std::printf("neural rendering: NR=%s feed=%s (Delete=reinicio)\n", want_nr ? "ON" : "OFF",
                    want_nr ? "ON" : "OFF");
      }
    } else if (session_writes_renodx_off_files(cfg) &&
               sync_neural_rendering_enabled_files(exe_dir.string(), false, false)) {
      append_log(cfg.log_dir, "neural rendering files OFF (runtime pulse pending=0)");
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
  HWND hwnd = create_app_window(hi, cap_w, cap_h, g_session_fullscreen, session_title.c_str());
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
  if (!renderer.init(hwnd, bw, bh, err)) {
    g_session_renderer = nullptr;
    std::fprintf(stderr, "renderer: %s\n", err.c_str());
    return 2;
  }
  renderer.set_vsync(cfg.sync_image);
  // v2.0: RenoDX → Present native; UI scale only drives Feed work_resolution.
  const bool renodx_stack = (cfg.enhance == EnhanceBackend::Renodx);
  {
    wchar_t exe_path_stack[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path_stack, MAX_PATH);
    g_renodx_multipass =
        renodx_stack && renodx_active_addon_is_multipass(fs::path(exe_path_stack).parent_path().string());
  }
  renderer.set_scale_percent(app_present_scale_percent(renodx_stack, cfg.scale_percent));
  // RenoDX: app RT native, Feed does the scaling — HUD must show the Feed resolution.
  renderer.set_hud_work_scale_percent(renodx_stack ? effective_scale_percent(true, cfg.scale_percent) : 0);
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
      const bool has_dxgi = fs::exists(exe_dir / "dxgi.dll");
      const auto has_renodx_stack = [&](const char* sub) {
        return fs::exists(exe_dir / sub / "renodx-dlss5.addon64");
      };
      const bool has_renodx = has_renodx_stack("addons-stable") || has_renodx_stack("addons-multipass") ||
                              fs::exists(exe_dir / "renodx-dlss5.addon64");
      const bool has_feed = fs::exists(exe_dir / "dlss5-feed.addon64") ||
                            fs::exists(exe_dir / "addons-stable" / "dlss5-feed.addon64") ||
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
  if (cfg.fake_capture) {
    capture = std::make_unique<FakeCapture>();
  } else {
    capture = std::make_unique<DShowCapture>();
  }
  auto try_start_capture = [&]() -> bool {
    err.clear();
    return capture->start(cfg.device_name, cap_opt, slot, clock, err);
  };
  if (!try_start_capture()) {
    // Mode churn / exclusive device: one settle+retry before fake fallback.
    if (!cfg.fake_capture) {
      append_log(cfg.log_dir, "capture start fail: " + err + " — retry apos settle");
      Sleep(static_cast<DWORD>(capture_device_settle_ms()));
      capture = std::make_unique<DShowCapture>();
      if (try_start_capture()) {
        append_log(cfg.log_dir, "capture start ok on retry");
      }
    }
  }
  if (!capture->running()) {
    std::fprintf(stderr, "capture: %s\n", err.c_str());
    append_log(cfg.log_dir, "capture start FAILED: " + err);
    if (cfg.selftest_cap) {
      std::printf("SKIP selftest-cap: %s\n", err.c_str());
      g_session_renderer = nullptr;
      renderer.shutdown();
      DestroyWindow(hwnd);
      return 0;
    }
    // Lobby sessions: NEVER fake — fake+RenoDX = tela branca (visto em 4K MJPEG).
    if (g_session_from_lobby) {
      std::fprintf(stderr, "capture falhou — voltando ao lobby (sem fake)\n");
      append_log(cfg.log_dir, "capture fail → lobby (no fake fallback)");
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
    if (!cfg.fake_capture) {
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
  if (!cfg.fake_capture && !cfg.smoke) {
    std::string aerr;
    if (!audio.start(audio_prefer, aerr)) {
      std::fprintf(stderr, "audio: %s (continuando sem som)\n", aerr.c_str());
    } else {
      std::printf("audio: monitorando %s -> saida padrao\n",
                  audio_prefer.empty() ? "auto" : audio_prefer.c_str());
      append_log(cfg.log_dir, "audio monitor started (" + (audio_prefer.empty() ? "auto" : audio_prefer) + ")");
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
  info.renodx_preset = renodx_visual_preset_name(cfg.renodx_preset);

  LosslessFgController fg;
  fg.configure(cfg.fg_multiplier, cfg.fg_path);
  {
    std::string ferr;
    if (cfg.fg_multiplier >= 2) {
      if (!fg.apply_profile(ferr)) {
        std::fprintf(stderr, "fg profile: %s\n", ferr.c_str());
      } else {
        std::printf("fg: LSFG profile mira-lock flow100 WGC x%d (%s)\n", cfg.fg_multiplier,
                    fg.state().exe_path.empty() ? "exe missing" : "Steam OK");
      }
    } else {
      std::printf("fg: off (use --fg-x 2|3|4, PageDn toggle, PageUp cycle)\n");
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
    append_log(cfg.log_dir, hbuf);
    std::printf("%s\n", hbuf);
  }
  ClusterDetector clusters;
  StatsWindow600 stats600;
  FrametimeWindow ft_win(180);

  OverlayState overlay;
  overlay.device = info.device;
  overlay.capture_w = info.width;
  overlay.capture_h = info.height;
  overlay.output_w = bw;
  overlay.output_h = bh;
  overlay.target_fps = cfg.fps;
  overlay.fake_capture = cfg.fake_capture;
  overlay.advanced_metrics = cfg.advanced_metrics;
  overlay.mini_overlay = cfg.mini_overlay;
  overlay.visible = cfg.show_overlay;
  // HUD work size = what the Feed really builds (floors at 50% under RenoDX), not the
  // requested UI scale — otherwise the overlay reports a scale the Feed refused.
  const int shown_scale = effective_scale_percent(renodx_stack, cfg.scale_percent);
  overlay.scale_percent = shown_scale;
  overlay.work_w = scaled_extent(info.width > 0 ? info.width : cap_w, shown_scale);
  overlay.work_h = scaled_extent(info.height > 0 ? info.height : cap_h, shown_scale);
  overlay.renodx_on = g_nr_runtime_on;
  overlay.renodx_preset = renodx_visual_preset_name(cfg.renodx_preset);
  overlay.nr_pass_count = clamp_nr_pass_count(cfg.nr_pass_count);
  overlay.feed_on = (cfg.enhance == EnhanceBackend::Renodx) && g_nr_runtime_on;
  overlay.sync_image = cfg.sync_image;
  overlay.hw_info = cfg.hw_info;
  const bool live_unlocked = live_mode_unlocked();
  const bool live_active = live_overlay_active(live_unlocked, cfg.live_overlay);
  overlay.live_overlay = live_active;
  overlay.live_console = cfg.live_console;
  if (live_active) {
    // Live owns the HUD surface — suppress mini burn + ADV twin.
    overlay.mini_overlay = false;
  }
  g_overlay = cfg.show_overlay;
  SessionNotice session_notice;
  // Soft tip on enter only when CFG asked to start with FG on.
  if (cfg.fg_auto_start && cfg.fg_multiplier >= 2) {
    session_notice.push(session_notice_fg_on(), clock.now_ms());
  } else if ((cfg.enhance == EnhanceBackend::Renodx) && cfg.neural_rendering) {
    session_notice.push(session_notice_nr_on(), clock.now_ms());
  }
  g_analyze = true;
  analyze.set_verbose(true);

  MetricsHud hud;
  hud.create(hwnd, hi);
  LiveOverlay live;
  if (live_active) {
    live.create(hwnd, hi);
    live.set_console(cfg.live_console);
    live.update(overlay);
  } else {
    hud.update(overlay);  // size + show advanced panel immediately on Iniciar
  }

  SystemMetricsSampler hw_sampler;
  const bool sample_hw = cfg.hw_info || cfg.mini_overlay;
  if (sample_hw) {
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
      snap.scale_percent = effective_scale_percent(renodx_stack, cfg.scale_percent);
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
      append_log(cfg.log_dir, std::string(session_report_subdir(kind)) + " -> " + path);
      std::printf("%s report: %s\n", session_report_file_prefix(kind), path.c_str());
    };

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
    std::printf("session cfg: Ctrl+%s abre o menu na captura\n", vk_display_name(cfg.hotkeys.vk_cfg));
  } else {
    std::fprintf(stderr, "session cfg: ImGui nao inicializou — menu na captura indisponivel\n");
    append_log(cfg.log_dir, "session cfg init failed");
  }
  g_cfg_open = false;
  g_toggle_cfg = false;
  g_close_cfg = false;
  int cfg_ui_max_vtx = 0;
  if (cfg.selftest_cfg && cfg_ui_ready) {
    cfg_ui.set_open(true);
    g_cfg_open = true;
    std::printf("selftest-cfg: menu aberto na sessao\n");
  }

  SessionControlPanel session_panel;
  const bool show_session_panel = session_shows_control_panel(cfg.hide_panel_on_capture);
  if (show_session_panel) {
    session_panel.create(hwnd, hi);
    session_panel.set_visible(true);
    std::printf("session panel: ON (hide_panel_on_capture=0)\n");
  } else {
    std::printf("session panel: off (captura limpa)\n");
  }
  std::printf("present sync: %s Present(%u) — %s\n", sync_image_badge(cfg.sync_image),
              present_sync_interval(cfg.sync_image), sync_image_how_to_see(cfg.sync_image));

  if (cfg.selftest_fg) {
    std::string ferr;
    if (fg.state().status == FgStatus::Missing || fg.state().exe_path.empty()) {
      std::printf("SKIP selftest-fg: LosslessScaling.exe not found\n");
      session_panel.destroy();
      live.destroy();
      hud.destroy();
      renderer.set_enhance(nullptr);
      enhance->shutdown();
      capture->stop();
      audio.stop();
      return 0;
    }
    if (!fg.ensure_process(ferr)) {
      std::printf("FAIL selftest-fg: ensure_process %s\n", ferr.c_str());
      fg.shutdown();
      session_panel.destroy();
      live.destroy();
      hud.destroy();
      return 7;
    }
    std::printf("selftest-fg: process pid=%lu ready — shutting down\n",
                static_cast<unsigned long>(fg.state().pid));
    fg.shutdown();
    std::printf("PASS selftest-fg\n");
    session_panel.destroy();
    live.destroy();
    hud.destroy();
    renderer.set_enhance(nullptr);
    enhance->shutdown();
    capture->stop();
    audio.stop();
    return 0;
  }

  // Reinício pedido pelo CFG: aviso na tela primeiro, teardown depois.
  bool cfg_restart_pending = false;
  SessionCfgApply cfg_restart_kind = SessionCfgApply::Live;
  double cfg_restart_until = 0;

  double prev_t = clock.now_ms();
  uint64_t presented = 0;
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

  // Gear FG on → liga na abertura (não só perfil pronto). Off = PageDn habilita.
  if (cfg.fg_auto_start && cfg.fg_multiplier >= 2) {
    std::string ferr;
    if (fg.toggle(hwnd, ferr)) {
      std::printf("fg auto-on x%d\n", fg.state().multiplier);
      append_log(cfg.log_dir, "fg auto-on x" + std::to_string(fg.state().multiplier));
    } else {
      std::fprintf(stderr, "fg auto-on: %s\n", ferr.c_str());
      append_log(cfg.log_dir, "fg auto-on fail: " + ferr);
      bug_exc("fg_auto_on", ferr);
      if (ferr.find("LosslessScaling") != std::string::npos || ferr.find("missing") != std::string::npos) {
        session_notice.push(session_notice_fg_missing(), clock.now_ms());
      }
    }
    info.fg_enabled = fg.state().active;
    info.fg_multiplier = fg.state().multiplier;
    info.fg_status = fg_status_name(fg.state().status);
  }

  append_log(cfg.log_dir,
             std::string("session start device=") + info.device +
                 " hud=" + (cfg.show_overlay ? "on" : "off") +
                 " advanced=" + (cfg.advanced_metrics ? "1" : "0"));

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

    // Ctrl+M / Esc / botão Fechar — sempre pela flag, nunca dentro da janela.
    if (g_toggle_cfg.exchange(false) && cfg_ui_ready && !cfg_restart_pending) {
      cfg_ui.toggle_open();
      g_cfg_open = cfg_ui.open();
      session_notice.push(cfg_ui.open() ? session_notice_cfg_open(g_hotkeys.vk_cfg, g_hotkeys.cfg_ctrl)
                                        : session_notice_cfg_closed(),
                          clock.now_ms());
    }
    if (g_close_cfg.exchange(false) && cfg_ui_ready) {
      cfg_ui.set_open(false);
      g_cfg_open = false;
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
        append_log(cfg.log_dir, std::string("DLSS toggle ") + (eo.enabled ? "ON" : "OFF"));
        std::printf("DLSS %s\n", eo.enabled ? "ON" : "OFF");
      }
    }

    if (g_toggle_nr.exchange(false)) {
      const double now_nr = clock.now_ms();
      if (cfg_restart_pending) {
        append_log(cfg.log_dir, "nr toggle ignored (restart pending)");
      } else if (g_nr_confirm.active) {
        append_log(cfg.log_dir, "nr toggle ignored (confirm pending)");
      } else if (!nr_toggle_debounce_ok(now_nr, g_nr_toggle_last_ms)) {
        append_log(cfg.log_dir, "nr toggle ignored (debounce <350ms)");
      } else if (!session_nr_hotkey_allowed(cfg.enhance)) {
        std::printf("nr toggle: RenoDX off — marque RenoDX no lobby (FG continua independente)\n");
        append_log(cfg.log_dir, "nr toggle blocked: RenoDX not loaded this session");
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
        cfg_restart_kind = SessionCfgApply::RelaunchNr;
        cfg_restart_pending = true;
        cfg_restart_until = now_nr + kDlss5MotorSwapHoldSec * 1000.0;
        session_notice.push(session_notice_nr_apply(want_on), now_nr);
        append_log(cfg.log_dir, std::string("cfg nr restart → ") + (want_on ? "ON" : "OFF"));
        std::printf("neural rendering %s (Delete→reinicio) — Frame Gen nao muda\n", want_on ? "ON" : "OFF");
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
        append_log(cfg.log_dir, std::string("neural rendering ") + (g_nr_runtime_on ? "ON" : "OFF") +
                                    " (log confirm)");
        std::printf("neural rendering %s (confirm) — Frame Gen nao muda\n",
                    g_nr_runtime_on ? "ON" : "OFF");
        session_notice.push(g_nr_runtime_on ? session_notice_nr_on() : session_notice_nr_off(),
                            clock.now_ms());
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
            append_log(cfg.log_dir, std::string("neural rendering runtime ") +
                                        (g_nr_runtime_on ? "ON" : "OFF") + " (Bridge pulse)");
            std::printf("neural rendering runtime: %s (Bridge pulse)\n", g_nr_runtime_on ? "ON" : "OFF");
          } else {
            append_log(cfg.log_dir, "neural rendering boot pulse sent (await log confirm)");
          }
        }
      }
    }

    if (g_toggle_fs.exchange(false)) {
      g_session_fullscreen = !g_session_fullscreen;
      cfg.exclusive_fullscreen = g_session_fullscreen;
      int cw = 0, ch = 0;
      if (apply_display_mode(hwnd, renderer, g_session_fullscreen, g_session_signal_w, g_session_signal_h, &cw,
                             &ch)) {
        append_log(cfg.log_dir, std::string("display ") + (g_session_fullscreen ? "fullscreen" : "windowed") +
                                    " client=" + std::to_string(cw) + "x" + std::to_string(ch));
        std::printf("display: %s (%dx%d)\n", g_session_fullscreen ? "fullscreen" : "windowed", cw, ch);
      }
    }

    if (g_toggle_fg.exchange(false)) {
      std::string ferr;
      if (fg.busy()) {
        append_log(cfg.log_dir, "fg toggle ignored (op pending)");
      } else if (!fg.request_toggle(hwnd, ferr)) {
        std::printf("fg toggle: %s\n", ferr.c_str());
        append_log(cfg.log_dir, "fg toggle fail: " + ferr);
        bug_exc("fg_toggle", ferr);
        if (ferr.find("LosslessScaling") != std::string::npos || ferr.find("missing") != std::string::npos) {
          session_notice.push(session_notice_fg_missing(), clock.now_ms());
        } else {
          session_notice.push("Frame Gen — sem confirmacao; overlay nao mudou", clock.now_ms());
        }
      } else {
        session_notice.push(fg.state().active ? "Frame Gen — desligando…" : "Frame Gen — aplicando…",
                            clock.now_ms());
        append_log(cfg.log_dir, "fg toggle requested (async)");
      }
    }
    if (g_cycle_fg.exchange(false)) {
      std::string ferr;
      if (fg.busy()) {
        append_log(cfg.log_dir, "fg cycle ignored (op pending)");
      } else if (!fg.request_cycle_multiplier(hwnd, ferr)) {
        std::printf("fg cycle: %s\n", ferr.c_str());
      } else {
        session_notice.push("Frame Gen — trocando x" + std::to_string(fg.state().multiplier) + "…",
                            clock.now_ms());
        append_log(cfg.log_dir, "fg cycle requested (async)");
      }
    }

    // Finish async FG without blocking Present (same idea as NR log confirm).
    if (fg.busy() && fg.tick(clock.now_ms())) {
      apply_session_fg_to_config(fg.state().active, fg.state().multiplier, cfg);
      info.fg_enabled = fg.state().active;
      info.fg_multiplier = fg.state().multiplier;
      info.fg_status = fg_status_name(fg.state().status);
      if (!fg.state().last_error.empty() && fg.state().status == FgStatus::Error) {
        session_notice.push("Frame Gen — " + fg.state().last_error, clock.now_ms());
        append_log(cfg.log_dir, "fg async fail: " + fg.state().last_error);
        bug_exc("fg_async", fg.state().last_error);
      } else {
        std::printf("fg %s x%d\n", fg.state().active ? "ON" : "off", fg.state().multiplier);
        append_log(cfg.log_dir, std::string("fg ") + (fg.state().active ? "ON" : "off") + " x" +
                                    std::to_string(fg.state().multiplier) + " (async confirm)");
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
      append_log(cfg.log_dir, "ANALYZE ON");
    }
    if (!analyze_on && analyze_was) {
      analyze.set_verbose(false);
      analyze.session_end(clock.now_ms());
      std::string jsonl, summary;
      if (write_analyze_files(cfg.log_dir, analyze, clock.now_ms(), jsonl, summary)) {
        append_log(cfg.log_dir, "ANALYZE flush " + summary);
        std::printf("Analyze written:\n  %s\n  %s\n", jsonl.c_str(), summary.c_str());
      }
      // start a fresh session buffer for continued run
      analyze.session_start(info, clock.now_ms());
    }
    analyze_was = analyze_on;

    FramePayload frame;
    if (!slot.consume(frame)) {
      Sleep(1);
      continue;
    }

    const double now = clock.now_ms();
    const double interval = now - prev_t;
    prev_t = now;
    const double instant_fps = interval > 0 ? 1000.0 / interval : 0;
    fps_smooth = fps_smooth <= 0 ? instant_fps : (fps_smooth * 0.9 + instant_fps * 0.1);

    if (presented > 0) {
      ft_win.push(interval);
      overlay.ft_avg_ms = ft_win.avg();
      overlay.ft_p95_ms = ft_win.p95();
      overlay.ft_max_ms = ft_win.max();
      overlay.ft_stable = ft_win.stable(1000.0 / (std::max)(1.0, cfg.fps), hitch_ms);
      overlay.ft_spark = ft_win.sparkline(56, 1000.0 / (std::max)(1.0, cfg.fps));
      ft_win.chronological(overlay.ft_hist);
    }

    double present_ms = 0;
    overlay.visible = g_overlay;
    overlay.advanced_metrics = cfg.advanced_metrics;
    overlay.mini_overlay = live_active ? false : cfg.mini_overlay;
    overlay.live_overlay = live_active;
    overlay.live_console = cfg.live_console;
    overlay.analyze_on = analyze_on;
    overlay.frame_ms = interval;
    overlay.e2e_ms = now - frame.cap_qpc_ms;
    overlay.fps = instant_fps;
    overlay.fps_smooth = fps_smooth;
    overlay.capture_w = frame.width > 0 ? frame.width : cap_w;
    overlay.capture_h = frame.height > 0 ? frame.height : cap_h;
    overlay.output_w = bw;
    overlay.output_h = bh;
    overlay.frames = presented;
    overlay.dropped = slot.dropped();
    overlay.stalls_window = stalls_since_600;
    overlay.clusters = cluster_count;
    overlay.fake_capture = cfg.fake_capture;
    overlay.device = info.device;
    fg.reconcile_process();
    overlay.fg_on = fg.state().active &&
                    fg_overlay_on_confirmed(true, LosslessFgController::count_running_lossless_processes());
    overlay.fg_multiplier = fg.state().multiplier;
    overlay.fg_status = fg_status_name(fg.state().status);
    overlay.fg_backend = fg.state().backend;
    overlay.fg_flow_scale = fg.state().flow_scale;
    overlay.fg_capture_api = fg.state().capture_api;
    overlay.session_notice = session_notice.visible(now);
    // Soft tips must be readable even if the user hid the HUD.
    if (session_notice.active(now)) overlay.visible = true;
    if (overlay.fg_on && overlay.fg_multiplier >= 2) {
      overlay.fps_fg_est = fps_smooth * static_cast<double>(overlay.fg_multiplier);
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
      ui.scale_percent = clamp_scale_percent(cfg.scale_percent);
      ui.feed_loaded_scale = feed_locked_scale;
      ui.fg_on = fg.state().active;
      ui.fg_multiplier = fg.state().multiplier >= 2 ? fg.state().multiplier : cfg.fg_multiplier;
      ui.fg_busy = fg.busy();
      ui.hud_visible = g_overlay.load();
      ui.signal_w = overlay.capture_w > 0 ? overlay.capture_w : cap_w;
      ui.signal_h = overlay.capture_h > 0 ? overlay.capture_h : cap_h;
      ui.vk_cfg = g_hotkeys.vk_cfg;
      ui.cfg_ctrl = g_hotkeys.cfg_ctrl;
      ui.status = overlay.session_notice;
      if (cfg_restart_pending) {
        cfg_ui.set_restart_hold(session_restart_notice_title(),
                                session_restart_notice_detail(cfg_restart_kind),
                                (cfg_restart_until - now) / 1000.0);
        g_cfg_open = false;
      }
    }

    if (!renderer.present_frame(frame, overlay, present_ms, err)) {
      append_log(cfg.log_dir, "present error: " + err);
      bug_exc("present", err);
      if (perf_rep.active()) perf_rep.note_event(now, "present_error", err.c_str());
    }

    if (cfg_ui_ready && cfg.selftest_cfg) {
      cfg_ui_max_vtx = (std::max)(cfg_ui_max_vtx, cfg_ui.last_vertex_count());
    }

    if (cfg_ui_ready && !cfg_restart_pending) {
      const SessionCfgActions act = cfg_ui.take_actions();
      SessionCfgState& ui = cfg_ui.state;
      if (act.close_requested) {
        cfg_ui.set_open(false);
        g_cfg_open = false;
        session_notice.push(session_notice_cfg_closed(), now);
      }
      if (act.hud_changed) g_overlay = ui.hud_visible;
      // NR e FG reaproveitam os caminhos das teclas (debounce, ACK por log, async).
      if (act.toggle_nr) g_toggle_nr = true;
      if (act.toggle_fg) g_toggle_fg = true;
      if (act.fg_multiplier_changed) {
        std::string ferr;
        if (fg.busy()) {
          append_log(cfg.log_dir, "cfg fg multiplier ignored (op pending)");
        } else if (!fg.request_set_multiplier(ui.fg_multiplier, hwnd, ferr)) {
          std::printf("cfg fg x%d: %s\n", ui.fg_multiplier, ferr.c_str());
          append_log(cfg.log_dir, "cfg fg multiplier fail: " + ferr);
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
          (void)sync_nr_pass_count_files(exe_dir_cfg, cfg.nr_pass_count, cfg.renodx_preset);
          (void)sync_renodx_visual_preset_files(exe_dir_cfg, cfg.renodx_preset);
          cfg_restart_kind = SessionCfgApply::RelaunchPreset;
          cfg_restart_pending = true;
          cfg_restart_until = now + kDlss5MotorSwapHoldSec * 1000.0;
          session_notice.push(session_notice_preset_apply(preset_before, cfg.renodx_preset), now);
          append_log(cfg.log_dir, std::string("cfg preset restart → ") +
                                      renodx_visual_preset_name(cfg.renodx_preset));
        }
      }

      const SessionCfgApply plan =
          act.pass_count_changed
              ? plan_session_cfg_apply(renodx_now, act.pass_count_before, ui.nr_pass_count,
                                       feed_locked_scale, clamp_scale_percent(cfg.scale_percent))
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
          (void)sync_nr_pass_count_files(exe_dir_cfg, cfg.nr_pass_count, cfg.renodx_preset);
          (void)sync_feed_work_resolution_in_dir(
              exe_dir_cfg, feed_work_resolution_for_session(cfg.scale_percent));
          cfg_restart_kind = plan;
          cfg_restart_pending = true;
          cfg_restart_until = now + kDlss5MotorSwapHoldSec * 1000.0;
          const bool motor = (plan == SessionCfgApply::RelaunchMotor);
          session_notice.push(motor ? session_notice_dlss5_motor_swap(act.pass_count_before, cfg.nr_pass_count)
                                    : session_notice_nr_pass_count_apply(act.pass_count_before,
                                                                         cfg.nr_pass_count),
                              now);
          append_log(cfg.log_dir, std::string(motor ? "cfg motor swap → " : "cfg pass count restart → ") +
                                      nr_pass_count_label(cfg.nr_pass_count));
        } else {
          cfg.nr_pass_count = clamp_nr_pass_count(ui.nr_pass_count);
          const bool want = want_neural_rendering(true, cfg.neural_rendering);
          apply_lobby_nr_pass_count_live(exe_dir_cfg, cfg.nr_pass_count, want, &g_nr_runtime_on,
                                         cfg.renodx_preset);
          cfg.nr_runtime_on = g_nr_runtime_on;
          overlay.nr_pass_count = cfg.nr_pass_count;
          session_notice.push(std::string("Pass Count: ") + nr_pass_count_label(cfg.nr_pass_count), now);
          append_log(cfg.log_dir, "cfg pass count → " + std::string(nr_pass_count_label(cfg.nr_pass_count)));
        }
      } else if (act.pass_count_changed) {
        ui.nr_pass_count = clamp_nr_pass_count(cfg.nr_pass_count);
      }

      if (act.scale_apply && !cfg_restart_pending) {
        const int want_scale = clamp_scale_percent(ui.scale_percent);
        if (plan == SessionCfgApply::RelaunchFeedScale) {
          cfg.scale_percent = want_scale;
          cfg_restart_kind = SessionCfgApply::RelaunchFeedScale;
          cfg_restart_pending = true;
          cfg_restart_until = now + kDlss5MotorSwapHoldSec * 1000.0;
          session_notice.push(
              session_notice_feed_scale_apply(effective_scale_percent(renodx_now, feed_locked_scale),
                                              effective_scale_percent(renodx_now, want_scale)),
              now);
          append_log(cfg.log_dir, "cfg feed scale → " + std::to_string(want_scale) + "% (reinicio)");
        } else {
          cfg.scale_percent = want_scale;
          renderer.set_scale_percent(app_present_scale_percent(renodx_now, want_scale));
          renderer.set_hud_work_scale_percent(renodx_now ? effective_scale_percent(true, want_scale) : 0);
          overlay.scale_percent = effective_scale_percent(renodx_now, want_scale);
          session_notice.push("Escala: " + std::to_string(effective_scale_percent(renodx_now, want_scale)) + "%",
                              now);
          append_log(cfg.log_dir, "cfg scale → " + std::to_string(want_scale) + "% (ao vivo)");
        }
      }

      if (cfg_restart_pending) {
        cfg_ui.set_open(false);
        g_cfg_open = false;
      }
    }

    // Aviso na tela do começo ao fim: contagem aqui, janela GDI no teardown, e de novo no boot.
    if (cfg_restart_pending && now >= cfg_restart_until) {
      g_restart_notice.show(hi, session_restart_notice_title(),
                            session_restart_notice_detail(cfg_restart_kind));
      append_log(cfg.log_dir, "session end → relaunch (--autostart)");
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
      overlay.feed_on = g_nr_runtime_on;
    }
    last_dlss_ms = overlay.dlss_ms;
    overlay.present_ms = present_ms;
    ++presented;
    overlay.frames = presented;
    // Sessão de volta na tela: só agora o aviso de reinício pode sair.
    if (presented == 1 && g_restart_notice.visible()) g_restart_notice.destroy();

    const bool is_stall = interval >= hitch_ms;
    if (!stats_frame_counts_toward_window(presented)) {
      max_boot_interval_ms = (std::max)(max_boot_interval_ms, interval);
    } else if (!warmup_logged) {
      warmup_logged = true;
      past_warmup = true;
      char wbuf[192];
      std::snprintf(wbuf, sizeof(wbuf),
                    "stats warmup done frames=%llu max_boot_interval=%.1f (excluded from 600-window)",
                    static_cast<unsigned long long>(kStatsWarmupFrames), max_boot_interval_ms);
      append_log(cfg.log_dir, wbuf);
      std::printf("%s\n", wbuf);
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
        append_log(cfg.log_dir,
                   "CLUSTER n=" + std::to_string(c->n) + " gap_ms=" + std::to_string(c->gap_since_prev_cluster_ms) +
                       " fgap=" + std::to_string(c->frames_since_prev));
        if (bug_rep.active()) bug_rep.note_event(now, "cluster", "");
        if (perf_rep.active()) perf_rep.note_event(now, "cluster", "");
      }
    } else {
      clusters.note_quiet(now);
    }

    analyze.frame_sample(now, presented, 0, 0, present_ms, overlay.e2e_ms, interval, slot.dropped(),
                         overlay.e2e_ms);

    if (stats_frame_counts_toward_window(presented)) {
      ++steady_presented;
      if (auto s = stats600.push(interval, overlay.e2e_ms, present_ms, is_stall)) {
        hitch.reset_window_log_cap();
        analyze.stats_600(*s, now, last_dlss_ms, enhance->enabled());
        overlay.has_stats600 = true;
        overlay.s600_fps = s->fps;
        overlay.s600_p95_e2e = s->p95_e2e_ms;
        overlay.s600_p95_present = s->p95_present_ms;
        overlay.s600_max_interval = s->max_interval_ms;
        overlay.s600_stalls = s->stalls;
        stalls_since_600 = 0;
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "600 frames: fps=%.2f mean=%.2fms p95_e2e=%.2f p95_present=%.2f stalls=%llu max=%.1f dlss=%s "
                      "dlss_ms=%.2f",
                      s->fps, s->mean_interval_ms, s->p95_e2e_ms, s->p95_present_ms,
                      static_cast<unsigned long long>(s->stalls), s->max_interval_ms,
                      enhance->enabled() ? "on" : "off", last_dlss_ms);
        append_log(cfg.log_dir, buf);
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
    if (live_active) {
      live.update(overlay);
      if (live.take_console_changed()) {
        cfg.live_console = live.console();
        overlay.live_console = cfg.live_console;
        PersistedSettings ps = default_settings();
        std::string serr;
        load_settings(ps, settings_default_path(), serr);
        ps.ui.live_overlay = true;
        ps.ui.live_console = live_console_key(clamp_live_console(cfg.live_console));
        save_settings(ps, settings_default_path(), serr);
      }
    } else {
      hud.update(overlay);
    }
    if (session_panel.visible()) session_panel.reposition();

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
  }

  cfg.nr_runtime_on = g_nr_runtime_on;

  if (bug_rep.active()) {
    bug_rep.end(clock.now_ms(), presented, 0, report_hitch_count, cluster_count);
  }
  if (perf_rep.active()) {
    perf_rep.end(clock.now_ms(), presented, 0, report_hitch_count, cluster_count);
  }

  session_panel.destroy();
  live.destroy();
  hud.destroy();
  g_session_cfg = nullptr;
  renderer.set_ui_draw_callback(nullptr, nullptr);
  cfg_ui.shutdown();
  g_cfg_open = false;
  g_restart_notice.pump();  // o aviso segue vivo enquanto FG/captura/D3D caem
  fg.shutdown();
  if (analyze_was || analyze.line_count() > 1) {
    analyze.session_end(clock.now_ms());
    std::string jsonl, summary;
    write_analyze_files(cfg.log_dir, analyze, clock.now_ms(), jsonl, summary);
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
    g_restart_notice.show(hi, session_restart_notice_title(),
                          session_restart_notice_detail(cfg_restart_kind));
    g_quit_clock = nullptr;
    return kSessionExitRelaunch;
  }

  if (g_return_to_lobby.load()) {
    // Strip session scale from ReShade feed before lobby Present (menu must stay sharp).
    {
      wchar_t exe_path[MAX_PATH]{};
      GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
      const fs::path exe_dir = fs::path(exe_path).parent_path();
      const int lobby_feed = feed_work_resolution_for_lobby();
      if (sync_feed_work_resolution_in_dir(exe_dir.string(), lobby_feed)) {
        append_log(cfg.log_dir, "lobby feed work_resolution -> " + std::to_string(lobby_feed) +
                                    " (ui=" + std::to_string(cfg.scale_percent) +
                                    "; Feed locks at first swapchain)");
      }
      sync_mgf_techniques_for_phase(exe_dir.string(), cfg.enhance == EnhanceBackend::Renodx);
    }
    append_log(cfg.log_dir, "session end → lobby (Esc)");
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
  wchar_t exe_path[MAX_PATH];
  GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  fs::path base = fs::path(exe_path).parent_path();
  // Logs always in AppData — Program Files is not writable after install.
  cfg.log_dir = nexuscap::app_logs_dir();
  // Reinício pedido pelo CFG: o aviso reaparece aqui e só sai no primeiro frame da sessão.
  if (nexuscap::restart_notice_visible_at_boot(cfg.autostart)) {
    g_restart_notice.show(GetModuleHandleW(nullptr), nexuscap::session_restart_notice_title(),
                          nexuscap::session_restart_notice_detail(nexuscap::SessionCfgApply::Live));
  }
  (void)nexuscap::prepare_renodx_runtime_dirs(base.string());
  if (!fs::path(cfg.log_dir).is_absolute() && cfg.log_dir == "logs") {
    cfg.log_dir = (base / cfg.log_dir).string();
  }

  nexuscap::PersistedSettings settings = nexuscap::default_settings();
  {
    std::string err;
    nexuscap::load_settings(settings, nexuscap::settings_default_path(), err);
  }
  // Before ReShade loads: Pass Count 1 → addons-stable, 2/3/4 → addons-multipass (AddonPath).
  {
    const auto ar = nexuscap::sync_renodx_addon_variant(base.string(), settings.nr_pass_count);
    if (ar == nexuscap::RenodxAddonSyncResult::OkSwapped) {
      std::printf("renodx addon: %s\n",
                  nexuscap::nr_pass_count_uses_multipass_addon(settings.nr_pass_count) ? "multipass"
                                                                                       : "stable");
    } else if (ar == nexuscap::RenodxAddonSyncResult::MissingVariant) {
      std::printf("renodx addon: stack missing (run tools\\setup_renodx.ps1)\n");
    }
    (void)nexuscap::sync_nr_pass_count_files(base.string(), settings.nr_pass_count, settings.renodx_preset);
    // Feed locks work_resolution at the first swapchain: 100% when the lobby opens first
    // (sharp menus), session scale when we boot straight into capture (--autostart).
    const bool lobby_first = cfg.show_lobby && !cfg.autostart;
    (void)nexuscap::sync_feed_work_resolution_in_dir(
        base.string(), nexuscap::feed_work_resolution_at_boot(lobby_first, settings.scale_percent));
  }
  // CLI overrides persisted where explicit.
  if (!cfg.show_lobby) {
    // keep cfg from CLI
  } else {
    // Seed lobby from disk unless CLI forced renodx/fg/device already atypical — lobby owns UX.
    cfg.device_name = settings.device_name;
    cfg.width = settings.width;
    cfg.height = settings.height;
    cfg.fps = settings.fps;
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
    cfg.hw_info = settings.ui.hw_info;
    cfg.hide_panel_on_capture = settings.ui.hide_panel_on_capture;
    cfg.sync_image = settings.ui.sync_image;
    // Also needed for --autostart (lobby skipped): reports must survive a scale relaunch.
    cfg.bug_report = settings.ui.bug_report;
    cfg.perf_report = settings.ui.perf_report;
    cfg.live_overlay = settings.ui.live_overlay;
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
    if (settings.enhance_renodx) {
      cfg.enhance = nexuscap::EnhanceBackend::Renodx;
      cfg.dlss = false;
      cfg.neural_rendering = settings.neural_rendering;
    } else {
      cfg.enhance = nexuscap::EnhanceBackend::Off;
      cfg.neural_rendering = false;
    }
  }

  HINSTANCE hi = GetModuleHandleW(nullptr);
  if (!cfg.show_lobby) {
    g_session_from_lobby = false;
    const int rc = run_app(hi, cfg);
    // Sessão sem lobby (--no-lobby / selftests) também reinicia pelo CFG e volta pra captura.
    if (rc == nexuscap::kSessionExitRelaunch) {
      nexuscap::apply_session_cfg_to_persisted(cfg, settings);
      std::string err;
      nexuscap::save_settings(settings, nexuscap::settings_default_path(), err);
      std::printf("cfg restart: relaunch %s\n", nexuscap::session_cfg_relaunch_args());
      if (nexuscap::relaunch_current_process(nexuscap::session_cfg_relaunch_args())) return 0;
      g_restart_notice.destroy();
      std::fprintf(stderr, "cfg restart: falha ao reiniciar\n");
      return rc;
    }
    return rc;
  }

  // Lobby ↔ session loop: always open lobby first (maximized window). Esc from capture returns.
  g_session_from_lobby = true;
  bool skip_lobby_once = cfg.autostart;  // relaunch for feed scale resumes straight into the session
  for (;;) {
    if (skip_lobby_once) {
      skip_lobby_once = false;
    } else if (!nexuscap::run_lobby(hi, cfg, settings)) {
      std::printf("lobby cancelled\n");
      return 0;
    }
    {
      std::string err;
      nexuscap::save_settings(settings, nexuscap::settings_default_path(), err);
    }
    const int rc = run_app(hi, cfg);
    // PageDn/PageUp mid-session must stick after Esc → lobby (checkbox + xN).
    apply_session_fg_to_persisted(cfg.fg_auto_start, cfg.fg_multiplier, settings);
    // Delete mid-session must stick after Esc → lobby (NR flag), but keep RenoDX loaded.
    apply_session_nr_to_persisted(cfg.neural_rendering, cfg.enhance == nexuscap::EnhanceBackend::Renodx,
                                  settings);
    settings.ui.live_overlay = cfg.live_overlay;
    settings.ui.live_console =
        nexuscap::live_console_key(nexuscap::clamp_live_console(cfg.live_console));
    {
      std::string err;
      nexuscap::save_settings(settings, nexuscap::settings_default_path(), err);
    }
    // CFG na captura mudou o motor 1x ou a escala do Feed: salva e volta direto pra sessão.
    if (rc == nexuscap::kSessionExitRelaunch) {
      nexuscap::apply_session_cfg_to_persisted(cfg, settings);
      {
        std::string err;
        nexuscap::save_settings(settings, nexuscap::settings_default_path(), err);
      }
      std::printf("cfg restart: relaunch %s\n", nexuscap::session_cfg_relaunch_args());
      if (nexuscap::relaunch_current_process(nexuscap::session_cfg_relaunch_args())) return 0;
      // Não deu pra relançar: fecha o aviso e abre o lobby em vez de sair calado.
      g_restart_notice.destroy();
      std::fprintf(stderr, "cfg restart: falha ao reiniciar — abrindo o lobby\n");
      continue;
    }
    if (rc != kExitBackToLobby) return rc;
    // Session ran below 100%: this process' Feed is locked there and would blur the menus.
    // The Esc path already rewrote the cfg to 100%, so a fresh process opens a sharp lobby.
    if (nexuscap::feed_scale_change_needs_relaunch(cfg.enhance == nexuscap::EnhanceBackend::Renodx,
                                                   cfg.scale_percent, nexuscap::kLobbyFeedWorkResolution)) {
      std::printf("back to lobby: restarting so menus present at 100%%\n");
      if (nexuscap::relaunch_current_process()) return 0;
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
