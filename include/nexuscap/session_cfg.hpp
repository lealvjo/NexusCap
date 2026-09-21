#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>

#include <string>
#include <vector>

#include "nexuscap/config.hpp"

namespace nexuscap {

// O que o CFG da sessão mostra e edita. main.cpp preenche antes do Present.
struct SessionCfgState {
  bool renodx_stack = false;    // RenoDX carregado nesta sessão
  bool multipass = false;       // motor ativo (AddonPath): 1,5x/2x/3x
  bool nr_on = false;           // runtime NR confirmado
  bool nr_busy = false;         // pulso do Bridge em andamento
  int renodx_preset = 0;        // 1x Default/Natural/Cinematic; 1.5x/2x/3x Model A/B/C
  int nr_pass_count = 1;        // 1 / 4 (1,5x) / 2 / 3
  int scale_percent = 100;      // escala do Feed escolhida na UI
  int feed_loaded_scale = 100;  // escala que o Feed travou neste processo
  bool fg_on = false;
  int fg_multiplier = 2;
  bool fg_busy = false;
  bool hud_visible = true;
  bool overlay_no_bg = false;
  bool overlay_console = false;
  int live_console = 0;
  int signal_w = 2560;
  int signal_h = 1440;
  int vk_cfg = 0x4D;
  bool cfg_ctrl = true;
  std::string status;  // último aviso da sessão (FG / NR / preset)
};

// Pedidos do usuário no frame. main.cpp decide o que é ao vivo e o que reinicia.
struct SessionCfgActions {
  bool toggle_nr = false;
  bool preset_changed = false;
  bool pass_count_changed = false;
  int pass_count_before = 1;
  bool toggle_fg = false;
  bool fg_multiplier_changed = false;
  bool hud_changed = false;
  bool overlay_no_bg_changed = false;
  bool console_changed = false;
  bool scale_apply = false;
  int scale_before = 100;
  bool close_requested = false;
  bool apply_user_preset = false;  // persist + apply snapshot (fill-safe)
  bool apply_hud = true;
  bool apply_overlay_no_bg = false;
  bool apply_overlay_console = false;
  int apply_live_console = 0;
  bool apply_nr_on = true;
  int apply_renodx_preset = 0;
  int apply_nr_pass_count = 1;
  bool apply_fg_on = false;
  int apply_fg_multiplier = 2;
  int apply_scale_percent = 100;
};

// ImGui desenhado no swapchain da sessão (mesmo caminho do mini overlay). Janela separada
// criaria um segundo swapchain no processo, e o Feed trava work_resolution no primeiro.
class SessionCfgOverlay {
public:
  ~SessionCfgOverlay();

  bool init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* ctx);
  void shutdown();
  bool ready() const { return ready_; }

  // true quando o ImGui consumiu a mensagem (mouse/teclado do menu).
  bool wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

  void set_open(bool open);
  void toggle_open() { set_open(!open_); }
  bool open() const { return open_; }

  // Espera do reinício: só o aviso, sem controles.
  void set_restart_hold(const std::string& title, const std::string& line, double seconds_left);
  void clear_restart_hold();

  SessionCfgState state;
  bool fg_can_enable = true;
  bool presets_locked = false;  // Benchmark / --perf-test
  SessionCfgActions take_actions();

  // Vértices do último desenho — usado pelo --selftest-cfg.
  int last_vertex_count() const { return last_vtx_; }

  // Trampolim para RendererD3D11::set_ui_draw_callback.
  static void draw_trampoline(void* user);

private:
  void draw();
  void draw_panel();
  void draw_restart_hold();
  void draw_presets();
  void reload_presets();
  void commit_save_preset();
  void commit_apply_preset();
  void commit_delete_preset();
  void commit_rename_preset(bool overwrite);
  bool browse_share_preset(const std::string& name, const std::string& text, std::string& out_path);
  bool browse_import_preset(std::string& text);

  bool ready_ = false;
  bool open_ = false;
  HWND hwnd_ = nullptr;
  void* imgui_ctx_ = nullptr;
  SessionCfgActions actions_{};
  int last_vtx_ = 0;
  // Cada abertura reposiciona: em tela cheia o cliente muda de tamanho e a posição
  // guardada de um Appearing anterior deixava o painel fora da tela.
  bool force_pos_ = false;
  float scale_slider_ = 100.f;
  bool hold_active_ = false;
  std::string hold_title_;
  std::string hold_line_;
  double hold_seconds_ = 0;
  std::vector<std::string> preset_names_;
  std::vector<std::string> preset_summaries_;
  int selected_preset_ = -1;
  char save_name_[51]{};  // 50 + NUL — kUserPresetNameMax
  bool save_name_open_ = false;
  bool rename_open_ = false;
  bool confirm_overwrite_ = false;
  bool confirm_rename_ow_ = false;
  bool confirm_delete_ = false;
  bool confirm_import_ow_ = false;
  std::string pending_import_text_;
};

void draw_user_preset_name_field_hint(const char* buf);
bool draw_user_preset_list_row(int id, const char* name, const char* summary, bool selected);

}  // namespace nexuscap
