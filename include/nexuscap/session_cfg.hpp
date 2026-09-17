#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>

#include <string>

#include "nexuscap/config.hpp"

namespace nexuscap {

// O que o CFG da sessão mostra e edita. main.cpp preenche antes do Present.
struct SessionCfgState {
  bool renodx_stack = false;    // RenoDX carregado nesta sessão
  bool multipass = false;       // motor ativo (AddonPath): 1,5x/2x/3x
  bool nr_on = false;           // runtime NR confirmado
  bool nr_busy = false;         // pulso do Bridge em andamento
  int renodx_preset = 0;        // Default / Natural / Cinematic
  int nr_pass_count = 1;        // 1 / 4 (1,5x) / 2 / 3
  int scale_percent = 100;      // escala do Feed escolhida na UI
  int feed_loaded_scale = 100;  // escala que o Feed travou neste processo
  bool fg_on = false;
  int fg_multiplier = 2;
  bool fg_busy = false;
  bool hud_visible = true;
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
  bool scale_apply = false;
  int scale_before = 100;
  bool close_requested = false;
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
  SessionCfgActions take_actions();

  // Vértices do último desenho — usado pelo --selftest-cfg.
  int last_vertex_count() const { return last_vtx_; }

  // Trampolim para RendererD3D11::set_ui_draw_callback.
  static void draw_trampoline(void* user);

private:
  void draw();
  void draw_panel();
  void draw_restart_hold();

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
};

}  // namespace nexuscap
