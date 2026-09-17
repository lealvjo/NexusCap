#pragma once

#include "imgui.h"

namespace nexuscap {

// Um tema só para os dois menus: o CFG do lobby e o CFG da captura (Ctrl+M).
// Base escura do ImGui + fundo mais fechado, borda azul de contorno e botões neutros —
// quem quiser destaque (Iniciar) empurra a própria cor.
inline void apply_nexuscap_imgui_theme() {
  ImGui::StyleColorsDark();
  ImGuiStyle& s = ImGui::GetStyle();
  s.WindowRounding = 10.f;
  s.ChildRounding = 10.f;
  s.FrameRounding = 6.f;
  s.GrabRounding = 6.f;
  s.PopupRounding = 10.f;
  s.ScrollbarRounding = 10.f;
  s.WindowPadding = ImVec2(18.f, 14.f);
  s.ItemSpacing = ImVec2(10.f, 8.f);
  s.ItemInnerSpacing = ImVec2(10.f, 8.f);
  s.ScrollbarSize = 16.f;
  s.GrabMinSize = 14.f;
  s.WindowBorderSize = 1.f;
  s.Colors[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.07f, 0.10f, 0.96f);
  s.Colors[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.10f, 0.14f, 1.f);
  s.Colors[ImGuiCol_PopupBg] = ImVec4(0.06f, 0.08f, 0.12f, 0.98f);
  s.Colors[ImGuiCol_Border] = ImVec4(0.27f, 0.55f, 0.80f, 0.85f);
  s.Colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.11f, 0.16f, 1.f);
  s.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.16f, 0.24f, 1.f);
  s.Colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.15f, 0.20f, 1.f);
  s.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.17f, 0.22f, 0.30f, 1.f);
  s.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.35f, 0.55f, 1.f);
  s.Colors[ImGuiCol_Button] = ImVec4(0.14f, 0.22f, 0.32f, 1.f);
  s.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.19f, 0.31f, 0.45f, 1.f);
  s.Colors[ImGuiCol_ButtonActive] = ImVec4(0.12f, 0.38f, 0.75f, 1.f);
  s.Colors[ImGuiCol_Header] = ImVec4(0.16f, 0.35f, 0.65f, 0.7f);
  s.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.45f, 0.80f, 0.85f);
  s.Colors[ImGuiCol_CheckMark] = ImVec4(0.45f, 0.85f, 1.f, 1.f);
  s.Colors[ImGuiCol_SliderGrab] = ImVec4(0.35f, 0.70f, 1.f, 1.f);
  s.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.80f, 1.f, 1.f);
  s.Colors[ImGuiCol_Separator] = ImVec4(0.22f, 0.28f, 0.38f, 0.7f);
  s.Colors[ImGuiCol_Text] = ImVec4(0.92f, 0.94f, 0.98f, 1.f);
  s.Colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.55f, 0.62f, 1.f);
}

// Mesmo azul de destaque nos dois menus (títulos de linha, valores calculados).
inline ImVec4 nexuscap_accent_color() { return ImVec4(0.45f, 0.85f, 1.f, 1.f); }
inline ImVec4 nexuscap_warn_color() { return ImVec4(1.f, 0.82f, 0.35f, 1.f); }

}  // namespace nexuscap
