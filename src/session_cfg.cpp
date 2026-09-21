#include "nexuscap/session_cfg.hpp"

#include "nexuscap/dlss5_bridge.hpp"
#include "nexuscap/imgui_theme.hpp"
#include "nexuscap/live_mode.hpp"
#include "nexuscap/settings_store.hpp"
#include "nexuscap/version.hpp"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace nexuscap {
namespace {

const ImVec4 kAccent = nexuscap_accent_color();
const ImVec4 kWarn = nexuscap_warn_color();

// A fonte embutida do ImGui só cobre Latin-1: travessão e reticências saem como "?".
std::string ascii_dashes(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size();) {
    if (in.compare(i, 3, "\xE2\x80\x94") == 0 || in.compare(i, 3, "\xE2\x80\x93") == 0) {
      out += '-';
      i += 3;
    } else if (in.compare(i, 3, "\xE2\x80\xA6") == 0) {
      out += "...";
      i += 3;
    } else {
      out += in[i];
      ++i;
    }
  }
  return out;
}

}  // namespace

SessionCfgOverlay::~SessionCfgOverlay() { shutdown(); }

bool SessionCfgOverlay::init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* ctx) {
  if (ready_) return true;
  if (!hwnd || !device || !ctx) return false;
  IMGUI_CHECKVERSION();
  ImGuiContext* c = ImGui::CreateContext();
  if (!c) return false;
  imgui_ctx_ = c;
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;
  io.FontGlobalScale = 1.35f;
  // Borderless na captura: o cursor do Windows pode não aparecer, então o ImGui desenha o dele.
  io.MouseDrawCursor = true;
  apply_nexuscap_imgui_theme();
  if (!ImGui_ImplWin32_Init(hwnd)) {
    ImGui::DestroyContext(c);
    imgui_ctx_ = nullptr;
    return false;
  }
  if (!ImGui_ImplDX11_Init(device, ctx)) {
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(c);
    imgui_ctx_ = nullptr;
    return false;
  }
  hwnd_ = hwnd;
  ready_ = true;
  scale_slider_ = static_cast<float>(clamp_scale_percent(state.scale_percent));
  return true;
}

void SessionCfgOverlay::shutdown() {
  if (!ready_) return;
  ImGui::SetCurrentContext(static_cast<ImGuiContext*>(imgui_ctx_));
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext(static_cast<ImGuiContext*>(imgui_ctx_));
  imgui_ctx_ = nullptr;
  hwnd_ = nullptr;
  ready_ = false;
  open_ = false;
  hold_active_ = false;
}

bool SessionCfgOverlay::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (!ready_ || (!open_ && !hold_active_)) return false;
  ImGui::SetCurrentContext(static_cast<ImGuiContext*>(imgui_ctx_));
  return ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp) != 0;
}

void SessionCfgOverlay::set_open(bool open) {
  if (open == open_) return;
  open_ = open;
  if (open_) {
    scale_slider_ = static_cast<float>(clamp_scale_percent(state.scale_percent));
    actions_ = SessionCfgActions{};
    force_pos_ = true;
  }
}

void SessionCfgOverlay::set_restart_hold(const std::string& title, const std::string& line,
                                         double seconds_left) {
  hold_active_ = true;
  hold_title_ = title;
  hold_line_ = line;
  hold_seconds_ = seconds_left;
  open_ = false;
}

void SessionCfgOverlay::clear_restart_hold() {
  hold_active_ = false;
  hold_seconds_ = 0;
}

SessionCfgActions SessionCfgOverlay::take_actions() {
  const SessionCfgActions out = actions_;
  actions_ = SessionCfgActions{};
  return out;
}

void SessionCfgOverlay::draw_trampoline(void* user) {
  if (user) static_cast<SessionCfgOverlay*>(user)->draw();
}

void SessionCfgOverlay::draw() {
  if (!ready_) return;
  if (!open_ && !hold_active_) {
    last_vtx_ = 0;
    return;
  }
  ImGui::SetCurrentContext(static_cast<ImGuiContext*>(imgui_ctx_));
  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  // Sessão pode ser 720p ou 1440p: a fonte acompanha a altura, senão fica ilegível na TV.
  ImGuiIO& io = ImGui::GetIO();
  io.FontGlobalScale = (std::max)(1.25f, (std::min)(io.DisplaySize.y / 800.f, 2.1f));
  ImGui::NewFrame();

  if (hold_active_) {
    draw_restart_hold();
  } else {
    draw_panel();
  }

  ImGui::Render();
  ImDrawData* dd = ImGui::GetDrawData();
  last_vtx_ = dd ? dd->TotalVtxCount : 0;
  ImGui_ImplDX11_RenderDrawData(dd);
}

void SessionCfgOverlay::draw_panel() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float w = (std::max)(360.f, (std::min)(vp->WorkSize.x * 0.32f, 860.f));
  const float h = (std::max)(240.f, (std::min)(vp->WorkSize.y - 60.f, 1120.f));
  const ImGuiCond cond = force_pos_ ? ImGuiCond_Always : ImGuiCond_Appearing;
  force_pos_ = false;
  const float x = (std::max)(vp->WorkPos.x, vp->WorkPos.x + vp->WorkSize.x - w - 40.f);
  const float y = (std::max)(vp->WorkPos.y, vp->WorkPos.y + (vp->WorkSize.y - h) * 0.5f);
  ImGui::SetNextWindowPos(ImVec2(x, y), cond);
  ImGui::SetNextWindowSize(ImVec2(w, h), cond);
  bool keep_open = true;
  if (ImGui::Begin("Configuracoes (CFG)", &keep_open, ImGuiWindowFlags_NoCollapse)) {
    char hk[48]{};
    if (state.cfg_ctrl) {
      std::snprintf(hk, sizeof(hk), "Ctrl+%s", vk_display_name(state.vk_cfg));
    } else {
      std::snprintf(hk, sizeof(hk), "%s", vk_display_name(state.vk_cfg));
    }
    ImGui::TextColored(kAccent, "Ajuste sem sair da captura  |  %s fecha", hk);
    // Linha fixa: se o aviso sumisse, todos os controles subiriam e o clique erraria o alvo.
    ImGui::TextDisabled("%s", state.status.empty() ? " " : ascii_dashes(state.status).c_str());
    ImGui::PushTextWrapPos(0.f);  // as notas longas não podem sair pela borda
    ImGui::Separator();

    if (ImGui::Checkbox("Overlay HUD", &state.hud_visible)) actions_.hud_changed = true;
    if (ImGui::Checkbox("Overlay sem fundo", &state.overlay_no_bg)) actions_.overlay_no_bg_changed = true;
    ImGui::TextDisabled(state.overlay_no_bg ? "so texto, sem placa preta"
                                            : "placa preta atras do texto");

    ImGui::Separator();
      if (ImGui::Checkbox("Mostrar console", &state.overlay_console)) actions_.console_changed = true;
      if (state.overlay_console) {
        LiveConsole cur = clamp_live_console(state.live_console);
        auto console_imcol = [](LiveConsole c) -> ImVec4 {
          const COLORREF rgb = live_console_color(c);
          return ImVec4(GetRValue(rgb) / 255.f, GetGValue(rgb) / 255.f, GetBValue(rgb) / 255.f, 1.f);
        };
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, console_imcol(cur));
        char console_btn[48]{};
        std::snprintf(console_btn, sizeof(console_btn), "%s##console_pick", live_console_label(cur));
        if (ImGui::Button(console_btn)) ImGui::OpenPopup("##console_menu");
        ImGui::PopStyleColor();
        if (ImGui::BeginPopup("##console_menu")) {
          for (LiveConsole o : kLiveConsoleOrder) {
            const bool selected = (o == cur);
            ImGui::PushStyleColor(ImGuiCol_Text, console_imcol(o));
            if (ImGui::MenuItem(live_console_label(o), nullptr, selected)) {
              state.live_console = static_cast<int>(o);
              actions_.console_changed = true;
            }
            ImGui::PopStyleColor();
          }
          ImGui::EndPopup();
        }
        ImGui::TextDisabled("  -> nome no overlay (padrao PS5)");
      }

    ImGui::Separator();
    ImGui::TextUnformatted("Neural Rendering");
    const bool nr_toggle_ok = session_cfg_nr_toggle_allowed(state.renodx_stack, state.multipass);
    ImGui::BeginDisabled(!nr_toggle_ok || state.nr_busy);
    bool nr = state.nr_on;
    if (ImGui::Checkbox("Neural Rendering ON", &nr)) actions_.toggle_nr = true;
    ImGui::EndDisabled();
    if (!state.renodx_stack) {
      ImGui::TextColored(kWarn, "Sem RenoDX nesta sessao - marque RenoDX no lobby.");
    } else {
      ImGui::TextDisabled(state.nr_busy ? "Aplicando..." : "Delete ou este check reinicia e volta para a sessao.");
    }

    ImGui::BeginDisabled(!state.renodx_stack);
    ImGui::Separator();
    ImGui::TextUnformatted("Preset visual");
    const int preset_before = clamp_renodx_visual_preset(state.renodx_preset);
    int preset = preset_before;
    if (ImGui::RadioButton(renodx_visual_preset_name(0, state.nr_pass_count), preset == 0)) preset = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton(renodx_visual_preset_name(1, state.nr_pass_count), preset == 1)) preset = 1;
    ImGui::SameLine();
    if (ImGui::RadioButton(renodx_visual_preset_name(2, state.nr_pass_count), preset == 2)) preset = 2;
    state.renodx_preset = clamp_renodx_visual_preset(preset);
    if (state.renodx_preset != preset_before) actions_.preset_changed = true;
    ImGui::TextColored(kWarn, "Trocar o preset reinicia o app e volta para a sessao.");

    ImGui::Separator();
    ImGui::TextUnformatted("Pass Count");
    const int pass_before = clamp_nr_pass_count(state.nr_pass_count);
    int pass = pass_before;
    if (ImGui::RadioButton("1x##nrpass", pass == 1)) pass = 1;
    ImGui::SameLine();
    if (ImGui::RadioButton("1,5x##nrpass", pass == 4)) pass = 4;
    ImGui::SameLine();
    if (ImGui::RadioButton("2x default##nrpass", pass == 2)) pass = 2;
    if (ImGui::RadioButton("2x otimizado##nrpass", pass == 5)) pass = 5;
    ImGui::SameLine();
    if (ImGui::RadioButton("3x##nrpass", pass == 3)) pass = 3;
    state.nr_pass_count = clamp_nr_pass_count(pass);
    if (state.nr_pass_count != pass_before) {
      actions_.pass_count_changed = true;
      actions_.pass_count_before = pass_before;
    }
    ImGui::TextDisabled("%s", nr_pass_count_15x_hint());
    ImGui::TextDisabled("%s", nr_pass_count_2x_opt_hint());
    ImGui::TextDisabled("%s", nr_pass_count_23x_hint());
    // O addon só lê o PassCount no init: qualquer troca aqui passa por processo novo.
    ImGui::TextColored(kWarn, "Trocar o Pass Count reinicia o app e volta para a sessao.");
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextUnformatted("Frame Gen (Lossless Scaling)");
    ImGui::BeginDisabled(state.fg_busy);
    bool fg = state.fg_on;
    if (ImGui::Checkbox("Habilitar Frame Gen", &fg)) actions_.toggle_fg = true;
    const int mult_before = clamp_fg_multiplier(state.fg_multiplier);
    int mult = mult_before;
    ImGui::BeginDisabled(!state.fg_on);
    if (ImGui::RadioButton("x2##fg", mult == 2)) mult = 2;
    ImGui::SameLine();
    if (ImGui::RadioButton("x3##fg", mult == 3)) mult = 3;
    ImGui::SameLine();
    if (ImGui::RadioButton("x4##fg", mult == 4)) mult = 4;
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (mult != mult_before) {
      state.fg_multiplier = clamp_fg_multiplier(mult);
      actions_.fg_multiplier_changed = true;
    }
    ImGui::TextDisabled(state.fg_busy ? "Aplicando..." : "Sem reinicio.");

    ImGui::Separator();
    ImGui::TextUnformatted("Escala de resolucao (trabalho GPU)");
    int slider = static_cast<int>(std::lround(scale_slider_));
    if (ImGui::SliderInt("##scale", &slider, kMinScalePercent, kMaxScalePercent, "%d%%")) {
      scale_slider_ = static_cast<float>(clamp_scale_percent(slider));
    }
    const int want = clamp_scale_percent(static_cast<int>(std::lround(scale_slider_)));
    const int eff = effective_scale_percent(state.renodx_stack, want);
    ImGui::TextColored(kAccent, "Trabalho Feed/NR: %dx%d (%d%%)", scaled_extent(state.signal_w, eff),
                       scaled_extent(state.signal_h, eff), eff);
    if (state.renodx_stack && feed_scale_below_floor(true, want)) {
      ImGui::TextColored(kWarn, "Feed nao aceita abaixo de %d%% - usando %d%%", kMinFeedWorkResolution, eff);
    }
    const bool scale_restarts = feed_scale_change_needs_relaunch(state.renodx_stack, state.feed_loaded_scale,
                                                                 want);
    if (scale_restarts) {
      ImGui::TextColored(kWarn, "Feed ativo em %d%% - aplicar reinicia e volta para a sessao.",
                         effective_scale_percent(state.renodx_stack, state.feed_loaded_scale));
    }
    ImGui::BeginDisabled(want == clamp_scale_percent(state.scale_percent) && !scale_restarts);
    if (ImGui::Button(scale_restarts ? "Aplicar escala (reinicio)" : "Aplicar escala", ImVec2(240, 0))) {
      actions_.scale_before = clamp_scale_percent(state.scale_percent);
      state.scale_percent = want;
      actions_.scale_apply = true;
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextDisabled("Esc fecha o menu | Esc de novo volta ao lobby");
    if (ImGui::Button("Fechar", ImVec2(140, 0))) actions_.close_requested = true;
    ImGui::PopTextWrapPos();
  }
  ImGui::End();
  if (!keep_open) actions_.close_requested = true;
}

void SessionCfgOverlay::draw_restart_hold() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.45f),
                          ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowBgAlpha(0.96f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.08f, 0.13f, 0.98f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.95f, 0.70f, 0.25f, 0.95f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(30.f, 22.f));
  if (ImGui::Begin("##session_restart", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                       ImGuiWindowFlags_NoInputs)) {
    ImGui::TextColored(kWarn, "%s", ascii_dashes(hold_title_).c_str());
    ImGui::Spacing();
    if (!hold_line_.empty()) ImGui::TextUnformatted(ascii_dashes(hold_line_).c_str());
    ImGui::TextDisabled("Volto direto para a sessao - nao feche nada.");
    ImGui::TextColored(kAccent, "%.1fs", (std::max)(0.0, hold_seconds_));
  }
  ImGui::End();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(2);
}

}  // namespace nexuscap
