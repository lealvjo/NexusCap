#include "nexuscap/session_cfg.hpp"

#include "nexuscap/audio.hpp"
#include "nexuscap/dlss5_bridge.hpp"
#include "nexuscap/imgui_theme.hpp"
#include "nexuscap/live_mode.hpp"
#include "nexuscap/settings_store.hpp"
#include "nexuscap/user_preset.hpp"
#include "nexuscap/version.hpp"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
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

void draw_user_preset_name_field_hint(const char* buf) {
  const std::string line = user_preset_name_limit_hint(buf);
  if (user_preset_name_at_limit(buf))
    ImGui::TextColored(kWarn, "%s", line.c_str());
  else
    ImGui::TextDisabled("%s", line.c_str());
}

bool draw_user_preset_list_row(int id, const char* name, const char* summary, bool selected) {
  ImGui::PushID(id);
  const float line_h = ImGui::GetTextLineHeight();
  const float pad_y = 3.f;
  const float row_h = line_h * 2.f + pad_y * 3.f;
  const bool clicked = ImGui::Selectable("##row", selected, 0, ImVec2(0.f, row_h));
  const ImVec2 rmin = ImGui::GetItemRectMin();
  const ImVec2 rmax = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float x0 = rmin.x + 6.f;
  const float x1 = rmax.x - 6.f;
  const float avail = (x1 > x0) ? (x1 - x0) : 0.f;
  const char* n = name ? name : "";
  const char* s = summary ? summary : "";
  const ImVec2 tw = ImGui::CalcTextSize(n);
  ImGui::PushClipRect(ImVec2(x0, rmin.y), ImVec2(x1, rmax.y), true);
  const ImU32 col_name = ImGui::GetColorU32(ImGuiCol_Text);
  const ImU32 col_sum = ImGui::GetColorU32(ImGuiCol_TextDisabled);
  const float y_name = rmin.y + pad_y;
  if (tw.x <= avail) {
    dl->AddText(ImVec2(x0, y_name), col_name, n);
  } else {
    const float off = user_preset_marquee_offset(tw.x, avail, ImGui::GetTime());
    dl->AddText(ImVec2(x0 - off, y_name), col_name, n);
    dl->AddText(ImVec2(x0 - off + tw.x + kUserPresetMarqueeGapPx, y_name), col_name, n);
  }
  dl->AddText(ImVec2(x0, y_name + line_h + 1.f), col_sum, s);
  ImGui::PopClipRect();
  ImGui::PopID();
  return clicked;
}

static int preset_name_filter(ImGuiInputTextCallbackData* data) {
  return user_preset_name_filter_event(data->EventChar, data->BufTextLen);
}

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
    fg_draft_sync_ = true;
    save_name_open_ = false;
    rename_open_ = false;
    confirm_overwrite_ = false;
    confirm_rename_ow_ = false;
    confirm_delete_ = false;
    confirm_import_ow_ = false;
    reload_presets();
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

    ImGui::TextUnformatted("Volume da captura");
    if (ImGui::SliderInt("##capture_vol", &state.capture_volume, kMinCaptureVolumePercent,
                         kMaxCaptureVolumePercent, "%d%%")) {
      state.capture_volume = clamp_capture_volume_percent(state.capture_volume);
      actions_.volume_changed = true;
    }
    ImGui::TextDisabled("HDMI no fone / caixa  |  0-100  |  nao vai para o preset");
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
    if (ImGui::RadioButton("1,7x##nrpass", pass == 7)) pass = 7;
    if (ImGui::RadioButton("2x default##nrpass", pass == 2)) pass = 2;
    ImGui::SameLine();
    if (ImGui::RadioButton("2x otimizado##nrpass", pass == 5)) pass = 5;
    if (ImGui::RadioButton("3x default##nrpass", pass == 3)) pass = 3;
    ImGui::SameLine();
    if (ImGui::RadioButton("3x otimizado##nrpass", pass == 6)) pass = 6;
    state.nr_pass_count = clamp_nr_pass_count(pass);
    if (state.nr_pass_count != pass_before) {
      const int scaled =
          nr_pass_count_on_select_scale(pass_before, state.nr_pass_count, state.scale_percent);
      if (scaled != state.scale_percent) {
        actions_.scale_before = clamp_scale_percent(state.scale_percent);
        state.scale_percent = scaled;
        scale_slider_ = static_cast<float>(scaled);
      }
      actions_.pass_count_changed = true;
      actions_.pass_count_before = pass_before;
    }
    ImGui::TextDisabled("%s", nr_pass_count_15x_hint());
    ImGui::TextDisabled("%s", nr_pass_count_15x_plus_hint());
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
    if (fg_draft_sync_) {
      state.fg_adaptive_draft = state.fg_adaptive;
      state.fg_target_draft = state.fg_target_fps;
      fg_draft_sync_ = false;
    }
    ImGui::BeginDisabled(!state.fg_on || state.fg_adaptive_draft);
    if (ImGui::RadioButton("x2##fg", mult == 2)) mult = 2;
    ImGui::SameLine();
    if (ImGui::RadioButton("x3##fg", mult == 3)) mult = 3;
    ImGui::SameLine();
    if (ImGui::RadioButton("x4##fg", mult == 4)) mult = 4;
    ImGui::EndDisabled();
    if (mult != mult_before) {
      state.fg_multiplier = clamp_fg_multiplier(mult);
      actions_.fg_multiplier_changed = true;
    }
    ImGui::TextUnformatted("Modo FG");
    state.monitor_hz = clamp_display_refresh_hz(state.monitor_hz);
    if (ImGui::BeginCombo("##sess_fg_mode", fg_mode_label(state.fg_adaptive_draft))) {
      const bool sel_fixed = !state.fg_adaptive_draft;
      if (ImGui::Selectable(fg_mode_label(false), sel_fixed)) {
        state.fg_adaptive_draft = false;
        if (state.fg_adaptive) {
          state.fg_adaptive = false;
          actions_.fg_adaptive_changed = true;
        }
      }
      if (sel_fixed) ImGui::SetItemDefaultFocus();
      if (ImGui::Selectable(fg_mode_label(true), state.fg_adaptive_draft)) state.fg_adaptive_draft = true;
      ImGui::EndCombo();
    }
    ImGui::BeginDisabled(!state.fg_on || !state.fg_adaptive_draft);
    int tgt = state.fg_target_draft;
    if (ImGui::InputInt("Alvo FPS##sess_fg_tgt", &tgt, 0, 0)) state.fg_target_draft = tgt;
    state.fg_target_editing = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      state.fg_target_draft =
          clamp_fg_adaptive_target(state.fg_target_draft, state.monitor_hz, state.capture_fps);
    }
    if (ImGui::Button("Hz do monitor##sess_fg_hz")) {
      state.fg_target_draft =
          clamp_fg_adaptive_target(state.monitor_hz, state.monitor_hz, state.capture_fps);
    }
    ImGui::EndDisabled();
    {
      const int draft_clamped =
          clamp_fg_adaptive_target(state.fg_target_draft, state.monitor_hz, state.capture_fps);
      const int lo = clamp_fg_adaptive_target(1, state.monitor_hz, state.capture_fps);
      const bool apply_on = fg_adaptive_apply_enabled(state.fg_adaptive_draft, state.fg_adaptive,
                                                      state.fg_target_fps, draft_clamped);
      ImGui::BeginDisabled(!apply_on || state.fg_busy);
      if (ImGui::Button("Aplicar Adaptive##sess_fg_apply")) {
        state.fg_target_draft = draft_clamped;
        const bool mode_ch = !state.fg_adaptive;
        const bool tgt_ch = draft_clamped != state.fg_target_fps;
        state.fg_adaptive = true;
        state.fg_target_fps = draft_clamped;
        actions_.fg_adaptive_changed = mode_ch;
        actions_.fg_target_changed = tgt_ch;
      }
      ImGui::EndDisabled();
      char hz_hint[176]{};
      std::snprintf(hz_hint, sizeof(hz_hint),
                    "No Adaptive, escreve o alvo (%d a %d) e carrega Aplicar. Fixo vale na hora.", lo,
                    state.monitor_hz);
      ImGui::TextDisabled("%s", hz_hint);
      ImGui::TextDisabled("%s", fg_adaptive_ghost_hint());
    }
    ImGui::TextUnformatted("Perfil FG");
    state.fg_queue = clamp_fg_queue(state.fg_queue);
    if (ImGui::BeginCombo("##sess_fg_queue", fg_queue_label(state.fg_queue))) {
      for (int q = 0; q < 3; ++q) {
        const bool selected = state.fg_queue == q;
        if (ImGui::Selectable(fg_queue_label(q), selected)) {
          state.fg_queue = q;
          actions_.fg_queue_changed = true;
        }
        if (selected) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::TextDisabled("0 menor latencia. Se o FG estiver ligado, reabre o Lossless.");
    ImGui::TextDisabled(state.fg_busy ? "Aplicando..." : "Nao reinicia o NexusCap.");
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextUnformatted("Escala de resolucao (trabalho GPU)");
    int slider = static_cast<int>(std::lround(scale_slider_));
    if (ImGui::SliderInt("##scale", &slider, kMinScalePercent, kMaxScalePercent, "%d%%")) {
      scale_slider_ = static_cast<float>(clamp_scale_percent(slider));
    }
    const int want = clamp_scale_percent(static_cast<int>(std::lround(scale_slider_)));
    const int eff = effective_scale_percent(state.renodx_stack, want, state.nr_pass_count, state.signal_w,
                                           state.signal_h);
    ImGui::TextColored(kAccent, "Trabalho Feed/NR: %dx%d (%d%%)", scaled_extent(state.signal_w, eff),
                       scaled_extent(state.signal_h, eff), eff);
    if (state.renodx_stack && feed_scale_below_floor(true, want)) {
      ImGui::TextColored(kWarn, "Feed nao aceita abaixo de %d%% - usando %d%%", kMinFeedWorkResolution, eff);
    }
    const bool scale_restarts = feed_scale_change_needs_relaunch(state.renodx_stack, state.feed_loaded_scale,
                                                                 want, state.nr_pass_count, state.signal_w,
                                                                 state.signal_h);
    if (scale_restarts) {
      ImGui::TextColored(kWarn, "Feed ativo em %d%% - aplicar reinicia e volta para a sessao.",
                         effective_scale_percent(state.renodx_stack, state.feed_loaded_scale,
                                                state.nr_pass_count, state.signal_w, state.signal_h));
    }
    ImGui::BeginDisabled(want == clamp_scale_percent(state.scale_percent) && !scale_restarts);
    if (ImGui::Button(scale_restarts ? "Aplicar escala (reinicio)" : "Aplicar escala", ImVec2(240, 0))) {
      actions_.scale_before = clamp_scale_percent(state.scale_percent);
      state.scale_percent = want;
      actions_.scale_apply = true;
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    draw_presets();

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

void SessionCfgOverlay::reload_presets() {
  std::string keep;
  if (selected_preset_ >= 0 && selected_preset_ < static_cast<int>(preset_names_.size())) {
    keep = preset_names_[static_cast<size_t>(selected_preset_)];
  }
  preset_names_.clear();
  preset_summaries_.clear();
  if (presets_locked) {
    selected_preset_ = -1;
    return;
  }
  std::vector<UserPreset> have;
  list_user_presets_in(default_user_presets_path(), have);
  for (const auto& p : have) {
    preset_names_.push_back(p.name);
    preset_summaries_.push_back(user_preset_list_summary(p));
  }
  selected_preset_ = keep.empty() ? -1 : find_user_preset_index(have, keep);
}

void SessionCfgOverlay::commit_save_preset() {
  if (presets_locked || !user_preset_io_allowed(presets_locked)) return;
  std::string name;
  const UserPresetNameStatus st = normalize_user_preset_name(save_name_, name);
  if (st != UserPresetNameStatus::Ok) {
    state.status = user_preset_name_status_toast(st);
    return;
  }
  state.scale_percent = clamp_scale_percent(static_cast<int>(std::lround(scale_slider_)));
  UserPreset snap = user_preset_from_session(name, state);
  bool replaced = false;
  std::string err;
  if (!upsert_user_preset_in(default_user_presets_path(), snap, replaced, err)) {
    state.status = err.empty() ? "Nao deu para salvar o preset" : err;
    return;
  }
  save_name_open_ = false;
  confirm_overwrite_ = false;
  reload_presets();
  std::vector<UserPreset> have;
  list_user_presets_in(default_user_presets_path(), have);
  selected_preset_ = find_user_preset_index(have, name);
  state.status = replaced ? ("Preset substituido: " + name) : ("Preset salvo: " + name);
}

void SessionCfgOverlay::commit_apply_preset() {
  if (presets_locked || selected_preset_ < 0 || selected_preset_ >= static_cast<int>(preset_names_.size()))
    return;
  std::vector<UserPreset> have;
  list_user_presets_in(default_user_presets_path(), have);
  const int idx = find_user_preset_index(have, preset_names_[static_cast<size_t>(selected_preset_)]);
  if (idx < 0) return;
  const UserPreset& p = have[static_cast<size_t>(idx)];
  if (user_preset_already_active(user_preset_from_session({}, state), p, fg_can_enable)) {
    state.status = user_preset_already_active_toast(p.name);
    actions_.notice = state.status;
    return;
  }
  const SessionCfgActions planned = plan_user_preset_apply(state, p, fg_can_enable);
  actions_.toggle_nr = planned.toggle_nr;
  actions_.preset_changed = planned.preset_changed;
  actions_.pass_count_changed = planned.pass_count_changed;
  actions_.pass_count_before = planned.pass_count_before;
  actions_.toggle_fg = planned.toggle_fg;
  actions_.fg_multiplier_changed = planned.fg_multiplier_changed;
  actions_.fg_queue_changed = planned.fg_queue_changed;
  actions_.fg_adaptive_changed = planned.fg_adaptive_changed;
  actions_.fg_target_changed = planned.fg_target_changed;
  actions_.hud_changed = planned.hud_changed;
  actions_.overlay_no_bg_changed = planned.overlay_no_bg_changed;
  actions_.console_changed = planned.console_changed;
  actions_.scale_apply = planned.scale_apply;
  actions_.scale_before = planned.scale_before;
  actions_.apply_user_preset = true;
  actions_.apply_hud = planned.apply_hud;
  actions_.apply_overlay_no_bg = planned.apply_overlay_no_bg;
  actions_.apply_overlay_console = planned.apply_overlay_console;
  actions_.apply_live_console = planned.apply_live_console;
  actions_.apply_nr_on = planned.apply_nr_on;
  actions_.apply_renodx_preset = planned.apply_renodx_preset;
  actions_.apply_nr_pass_count = planned.apply_nr_pass_count;
  actions_.apply_fg_on = planned.apply_fg_on;
  actions_.apply_fg_multiplier = planned.apply_fg_multiplier;
  actions_.apply_fg_queue = planned.apply_fg_queue;
  actions_.apply_fg_adaptive = planned.apply_fg_adaptive;
  actions_.apply_fg_target_fps = planned.apply_fg_target_fps;
  actions_.apply_scale_percent = planned.apply_scale_percent;
  stamp_session_from_user_preset(state, p, fg_can_enable);
  scale_slider_ = static_cast<float>(state.scale_percent);
  fg_draft_sync_ = true;
  if (p.fg_enabled && !fg_can_enable) {
    state.status = session_notice_fg_missing();
  } else {
    state.status = "Preset aplicado: " + p.name;
  }
}

void SessionCfgOverlay::commit_delete_preset() {
  if (presets_locked || selected_preset_ < 0 || selected_preset_ >= static_cast<int>(preset_names_.size()))
    return;
  std::string err;
  const std::string name = preset_names_[static_cast<size_t>(selected_preset_)];
  if (!delete_user_preset_in(default_user_presets_path(), name, err)) {
    state.status = err.empty() ? "Nao deu para apagar" : err;
    return;
  }
  selected_preset_ = -1;
  reload_presets();
  state.status = "Preset apagado: " + name;
}

void SessionCfgOverlay::commit_rename_preset(bool overwrite) {
  if (presets_locked || selected_preset_ < 0 || selected_preset_ >= static_cast<int>(preset_names_.size()))
    return;
  std::string name;
  const UserPresetNameStatus st = normalize_user_preset_name(save_name_, name);
  if (st != UserPresetNameStatus::Ok) {
    state.status = user_preset_name_status_toast(st);
    return;
  }
  const std::string old = preset_names_[static_cast<size_t>(selected_preset_)];
  bool replaced = false;
  std::string err;
  if (!rename_user_preset_in(default_user_presets_path(), old, name, overwrite, replaced, err)) {
    if (err == "existe") {
      confirm_rename_ow_ = true;
      return;
    }
    state.status = err.empty() ? "Nao deu para renomear" : err;
    return;
  }
  rename_open_ = false;
  confirm_rename_ow_ = false;
  reload_presets();
  std::vector<UserPreset> have;
  list_user_presets_in(default_user_presets_path(), have);
  selected_preset_ = find_user_preset_index(have, name);
  state.status = replaced ? ("Preset substituido: " + name) : ("Preset renomeado: " + name);
}

bool browse_share_user_preset(HWND owner, const std::string& name, const std::string& text,
                             std::string& out_path) {
  wchar_t file[MAX_PATH]{};
  std::string suggest = name + kUserPresetExtShare;
  MultiByteToWideChar(CP_UTF8, 0, suggest.c_str(), -1, file, MAX_PATH);
  wchar_t desk[MAX_PATH]{};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = L"Preset NexusCap\0*.nexuscap-preset\0Todos\0*.*\0";
  ofn.nFilterIndex = 1;
  ofn.lpstrDefExt = L"nexuscap-preset";
  ofn.lpstrTitle = L"Compartilhar preset";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desk))) {
    ofn.lpstrInitialDir = desk;
  }
  if (!GetSaveFileNameW(&ofn)) return false;
  char narrow[MAX_PATH * 4]{};
  WideCharToMultiByte(CP_UTF8, 0, file, -1, narrow, static_cast<int>(sizeof(narrow)), nullptr, nullptr);
  out_path = narrow;
  if (out_path.empty()) return false;
  {
    std::ofstream out(std::filesystem::u8path(out_path), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
  }
  std::wstring sel = L"/select,\"";
  sel += file;
  sel += L"\"";
  ShellExecuteW(nullptr, L"open", L"explorer.exe", sel.c_str(), nullptr, SW_SHOWNORMAL);
  return true;
}

bool browse_import_user_preset(HWND owner, std::string& text) {
  wchar_t file[MAX_PATH]{};
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrFilter = L"Preset NexusCap\0*.nexuscap-preset\0Todos\0*.*\0";
  ofn.nFilterIndex = 1;
  ofn.lpstrTitle = L"Importar preset";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetOpenFileNameW(&ofn)) return false;
  std::ifstream in(file, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  text = ss.str();
  return !text.empty();
}

bool SessionCfgOverlay::browse_share_preset(const std::string& name, const std::string& text,
                                            std::string& out_path) {
  return browse_share_user_preset(hwnd_, name, text, out_path);
}

bool SessionCfgOverlay::browse_import_preset(std::string& text) {
  return browse_import_user_preset(hwnd_, text);
}

void SessionCfgOverlay::draw_presets() {
  ImGui::TextUnformatted("Presets");
  if (presets_locked) {
    ImGui::TextDisabled("Benchmark nao grava preset.");
    return;
  }

  if (ImGui::BeginChild("##preset_list", ImVec2(0.f, 168.f), true)) {
    if (preset_names_.empty()) {
      ImGui::TextDisabled("Nenhum preset. Salve o look deste jogo.");
    }
    for (int i = 0; i < static_cast<int>(preset_names_.size()); ++i) {
      if (draw_user_preset_list_row(i, preset_names_[static_cast<size_t>(i)].c_str(),
                                    preset_summaries_[static_cast<size_t>(i)].c_str(), selected_preset_ == i))
        selected_preset_ = i;
    }
  }
  ImGui::EndChild();

  const bool have_sel = selected_preset_ >= 0 && selected_preset_ < static_cast<int>(preset_names_.size());
  if (ImGui::Button("Salvar preset")) {
    rename_open_ = false;
    save_name_open_ = true;
    save_name_[0] = 0;
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!have_sel);
  if (ImGui::Button("Aplicar")) commit_apply_preset();
  ImGui::SameLine();
  if (ImGui::Button("Apagar")) confirm_delete_ = true;
  ImGui::SameLine();
  if (ImGui::Button("Renomear") && have_sel) {
    save_name_open_ = false;
    rename_open_ = true;
    fill_user_preset_name_buf(save_name_, sizeof(save_name_), preset_names_[static_cast<size_t>(selected_preset_)]);
  }
  ImGui::SameLine();
  if (ImGui::Button("Compartilhar") && have_sel) {
    std::vector<UserPreset> have;
    list_user_presets_in(default_user_presets_path(), have);
    const int idx = find_user_preset_index(have, preset_names_[static_cast<size_t>(selected_preset_)]);
    if (idx >= 0) {
      std::string path;
      if (browse_share_preset(have[static_cast<size_t>(idx)].name, serialize_user_preset(have[static_cast<size_t>(idx)]),
                             path)) {
        state.status = "Arquivo pronto: " + std::filesystem::u8path(path).filename().string();
      }
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Importar")) {
    std::string text;
    if (browse_import_preset(text)) {
      UserPreset p;
      if (!parse_user_preset(text, p)) {
        state.status = "Arquivo invalido";
      } else {
        std::vector<UserPreset> have;
        list_user_presets_in(default_user_presets_path(), have);
        if (find_user_preset_index(have, p.name) >= 0) {
          pending_import_text_ = text;
          confirm_import_ow_ = true;
        } else {
          bool replaced = false;
          std::string err;
          if (import_user_preset_in(default_user_presets_path(), text, p, replaced, err)) {
            reload_presets();
            selected_preset_ = find_user_preset_index(have, p.name);
            std::vector<UserPreset> now;
            list_user_presets_in(default_user_presets_path(), now);
            selected_preset_ = find_user_preset_index(now, p.name);
            state.status = "Preset importado: " + p.name;
          } else {
            state.status = err.empty() ? "Import falhou" : err;
          }
        }
      }
    }
  }
  ImGui::TextDisabled("Renomear troca so o nome. Compartilhar gera um arquivo. Importar nao aplica sozinho.");

  if (save_name_open_) {
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputText("##preset_name", save_name_, sizeof(save_name_), ImGuiInputTextFlags_CallbackCharFilter,
                     preset_name_filter);
    draw_user_preset_name_field_hint(save_name_);
    if (ImGui::Button("Salvar##go")) {
      std::string name;
      const UserPresetNameStatus st = normalize_user_preset_name(save_name_, name);
      if (st != UserPresetNameStatus::Ok) {
        state.status = user_preset_name_status_toast(st);
      } else {
        std::vector<UserPreset> have;
        list_user_presets_in(default_user_presets_path(), have);
        if (find_user_preset_index(have, name) >= 0) {
          confirm_overwrite_ = true;
        } else {
          commit_save_preset();
        }
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancelar##save")) save_name_open_ = false;
  }

  if (rename_open_) {
    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputText("##preset_rename", save_name_, sizeof(save_name_), ImGuiInputTextFlags_CallbackCharFilter,
                     preset_name_filter);
    draw_user_preset_name_field_hint(save_name_);
    if (ImGui::Button("Renomear##go")) commit_rename_preset(false);
    ImGui::SameLine();
    if (ImGui::Button("Cancelar##rename")) rename_open_ = false;
  }

  if (confirm_overwrite_) {
    ImGui::OpenPopup("##ow_preset");
    confirm_overwrite_ = false;
  }
  if (ImGui::BeginPopupModal("##ow_preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    std::string name;
    normalize_user_preset_name(save_name_, name);
    ImGui::Text("Substituir %s?", name.empty() ? save_name_ : name.c_str());
    if (ImGui::Button("Substituir")) {
      commit_save_preset();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancelar##ow")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  if (confirm_rename_ow_) {
    ImGui::OpenPopup("##rn_ow_preset");
    confirm_rename_ow_ = false;
  }
  if (ImGui::BeginPopupModal("##rn_ow_preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    std::string name;
    normalize_user_preset_name(save_name_, name);
    ImGui::Text("Substituir %s?", name.empty() ? save_name_ : name.c_str());
    if (ImGui::Button("Substituir##rn")) {
      commit_rename_preset(true);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancelar##rnow")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  if (confirm_delete_) {
    ImGui::OpenPopup("##del_preset");
    confirm_delete_ = false;
  }
  if (ImGui::BeginPopupModal("##del_preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    const char* n = have_sel ? preset_names_[static_cast<size_t>(selected_preset_)].c_str() : "";
    ImGui::Text("Apagar %s?", n);
    if (ImGui::Button("Apagar##go")) {
      commit_delete_preset();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancelar##del")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  if (confirm_import_ow_) {
    ImGui::OpenPopup("##imp_ow");
    confirm_import_ow_ = false;
  }
  if (ImGui::BeginPopupModal("##imp_ow", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    UserPreset p;
    parse_user_preset(pending_import_text_, p);
    ImGui::Text("Substituir %s?", p.name.c_str());
    if (ImGui::Button("Substituir##imp")) {
      bool replaced = false;
      std::string err;
      if (import_user_preset_in(default_user_presets_path(), pending_import_text_, p, replaced, err)) {
        reload_presets();
        std::vector<UserPreset> now;
        list_user_presets_in(default_user_presets_path(), now);
        selected_preset_ = find_user_preset_index(now, p.name);
        state.status = "Preset importado: " + p.name;
      } else {
        state.status = err.empty() ? "Import falhou" : err;
      }
      pending_import_text_.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancelar##imp")) {
      pending_import_text_.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

}  // namespace nexuscap
