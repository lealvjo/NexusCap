#pragma once

#include "nexuscap/settings_store.hpp"

#include "imgui.h"

namespace nexuscap {

// Painel acima dos radios do Pass Count. Edita o rascunho; nao grava sozinho.
// apply/reset saem true no clique. Reset tambem devolve o rascunho para a fabrica do X.
inline void draw_nr_advanced_panel(NrAdvancedLook& draft, int pass, bool& apply, bool& reset,
                                   bool force_open = false) {
  apply = false;
  reset = false;
  if (force_open) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
  else ImGui::SetNextItemOpen(false, ImGuiCond_Once);
  if (!ImGui::CollapsingHeader("Configuracoes avancadas")) return;

  const int p = clamp_nr_pass_count(pass);
  ImGui::TextDisabled("%s", nr_pass_count_label(p));
  auto slider = [](const char* label, double& v, float lo, float hi, const char* fmt) {
    float f = static_cast<float>(v);
    if (ImGui::SliderFloat(label, &f, lo, hi, fmt)) v = static_cast<double>(f);
  };

  slider("Tom local", draft.local_tone, 0.f, 2.f, "%.2f");
  slider("Estrutura", draft.local_structure, 0.f, 2.f, "%.2f");
  {
    const bool padrao = draft.skin < -0.001;
    const char* fmt = padrao ? "1.00 (padrao)" : "%.2f";
    float f = static_cast<float>(draft.skin);
    if (ImGui::SliderFloat("Personagem / pele", &f, -1.f, 2.f, fmt)) draft.skin = static_cast<double>(f);
  }

  bool mask = draft.auto_mask != 0;
  if (ImGui::Checkbox("Mascara de personagem", &mask)) draft.auto_mask = mask ? 1 : 0;
  bool skin_ind = draft.skin_independent != 0;
  if (ImGui::Checkbox("Pele independente", &skin_ind)) draft.skin_independent = skin_ind ? 1 : 0;

  const char* ui_items[] = {"Desligada", "Ligada", "Gancho otimizado"};
  int ui = draft.ui_correction;
  if (ui < 0) ui = 0;
  if (ui > 2) ui = 2;
  if (ImGui::Combo("Correcao de UI", &ui, ui_items, 3)) draft.ui_correction = ui;

  ImGui::TextDisabled("Resultado = ao vivo, remodela a saida do NR");
  const char* stab_items[] = {"Auto (recomendado)", "Desligado", "Sempre"};
  int stab = draft.detail_stability;
  if (stab < 0) stab = 0;
  if (stab > 2) stab = 2;
  if (ImGui::Combo("Estabilidade de detalhes", &stab, stab_items, 3)) draft.detail_stability = stab;

  bool look = draft.look_mode != 0;
  if (ImGui::Checkbox("Ajustar a aparencia do NR", &look)) draft.look_mode = look ? 1 : 0;
  ImGui::BeginDisabled(draft.look_mode == 0);
  slider("Intensidade da edicao", draft.look_strength, 0.f, 2.f, "%.2f");
  slider("Clareamento", draft.look_brighten, 0.f, 2.f, "%.2f");
  slider("Escurecimento", draft.look_darken, 0.f, 2.f, "%.2f");
  slider("Cor", draft.look_colour, 0.f, 2.f, "%.2f");
  slider("Desvio de matiz", draft.look_hue, 0.f, 2.f, "%.2f");
  slider("Sombras", draft.look_shadows, 0.f, 2.f, "%.2f");
  slider("Tons medios", draft.look_midtones, 0.f, 2.f, "%.2f");
  slider("Realces", draft.look_highlights, 0.f, 2.f, "%.2f");
  ImGui::EndDisabled();

  if (ImGui::Button("Aplicar configs avancadas no X atual")) apply = true;
  ImGui::SameLine();
  if (ImGui::Button("Resetar para default")) {
    draft = nr_advanced_default(p);
    reset = true;
  }
}

}  // namespace nexuscap
