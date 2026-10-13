#include "theme.hpp"

namespace skeleton_repair::theme {

void Apply(float dpi_scale) {
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowPadding = {6, 6};
    style.FramePadding = {6, 3};
    style.CellPadding = {4, 2};
    style.ItemSpacing = {6, 4};
    style.ItemInnerSpacing = {4, 4};
    style.TouchExtraPadding = {0, 0};
    style.IndentSpacing = 14;
    style.ScrollbarSize = 10;
    style.GrabMinSize = 8;
    style.WindowRounding = 0;
    style.ChildRounding = 2;
    style.FrameRounding = 2;
    style.PopupRounding = 3;
    style.ScrollbarRounding = 3;
    style.GrabRounding = 2;
    style.TabRounding = 2;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 1;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 1;
    style.TabBorderSize = 0;
    style.TabBarBorderSize = 1;
    style.TabBarOverlineSize = 2;
    style.SeparatorTextBorderSize = 1;
    style.SeparatorTextPadding = {8, 2};
    style.DockingSeparatorSize = 1;
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.SelectableTextAlign = {0, .5f};
    style.HoverDelayShort = .25f;
    style.HoverDelayNormal = .5f;

    auto* colors = style.Colors;
    colors[ImGuiCol_Text] = kText;
    colors[ImGuiCol_TextDisabled] = kDisabled;
    colors[ImGuiCol_WindowBg] = kWorkspace;
    colors[ImGuiCol_ChildBg] = {0, 0, 0, 0};
    colors[ImGuiCol_PopupBg] = {kPanel.x, kPanel.y, kPanel.z, .98f};
    colors[ImGuiCol_Border] = kBorder;
    colors[ImGuiCol_BorderShadow] = {0, 0, 0, 0};
    colors[ImGuiCol_FrameBg] = kWorkspace;
    colors[ImGuiCol_FrameBgHovered] = kRaisedHover;
    colors[ImGuiCol_FrameBgActive] = kSelection;
    colors[ImGuiCol_TitleBg] = kPanel;
    colors[ImGuiCol_TitleBgActive] = kRaised;
    colors[ImGuiCol_TitleBgCollapsed] = kPanel;
    colors[ImGuiCol_MenuBarBg] = kPanel;
    colors[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
    colors[ImGuiCol_ScrollbarGrab] = kBorder;
    colors[ImGuiCol_ScrollbarGrabHovered] = kFocus;
    colors[ImGuiCol_ScrollbarGrabActive] = kAccent;
    colors[ImGuiCol_CheckMark] = kFocus;
    colors[ImGuiCol_SliderGrab] = {kFocus.x, kFocus.y, kFocus.z, .85f};
    colors[ImGuiCol_SliderGrabActive] = kFocus;
    colors[ImGuiCol_Button] = kRaised;
    colors[ImGuiCol_ButtonHovered] = kRaisedHover;
    colors[ImGuiCol_ButtonActive] = kSelectionActive;
    colors[ImGuiCol_Header] = kSelection;
    colors[ImGuiCol_HeaderHovered] = kSelectionHover;
    colors[ImGuiCol_HeaderActive] = kSelectionActive;
    colors[ImGuiCol_Separator] = kBorder;
    colors[ImGuiCol_SeparatorHovered] = kFocus;
    colors[ImGuiCol_SeparatorActive] = kAccent;
    colors[ImGuiCol_ResizeGrip] = {0, 0, 0, 0};
    colors[ImGuiCol_ResizeGripHovered] = {kFocus.x, kFocus.y, kFocus.z, .55f};
    colors[ImGuiCol_ResizeGripActive] = kFocus;
    colors[ImGuiCol_Tab] = kWorkspace;
    colors[ImGuiCol_TabHovered] = kRaisedHover;
    colors[ImGuiCol_TabSelected] = kPanel;
    colors[ImGuiCol_TabSelectedOverline] = kFocus;
    colors[ImGuiCol_TabDimmed] = kWorkspace;
    colors[ImGuiCol_TabDimmedSelected] = kPanel;
    colors[ImGuiCol_TabDimmedSelectedOverline] = kBorder;
    colors[ImGuiCol_DockingPreview] = {kAccent.x, kAccent.y, kAccent.z, .45f};
    colors[ImGuiCol_DockingEmptyBg] = kWorkspace;
    colors[ImGuiCol_PlotLines] = kFocus;
    colors[ImGuiCol_PlotLinesHovered] = kAccent;
    colors[ImGuiCol_PlotHistogram] = kFocus;
    colors[ImGuiCol_PlotHistogramHovered] = kAccent;
    colors[ImGuiCol_TableHeaderBg] = kRaised;
    colors[ImGuiCol_TableBorderStrong] = kBorder;
    colors[ImGuiCol_TableBorderLight] = kTableBorder;
    colors[ImGuiCol_TableRowBg] = {0, 0, 0, 0};
    colors[ImGuiCol_TableRowBgAlt] = {1, 1, 1, .03f};
    colors[ImGuiCol_TextSelectedBg] = {kFocus.x, kFocus.y, kFocus.z, .35f};
    colors[ImGuiCol_DragDropTarget] = kFocus;
    colors[ImGuiCol_NavCursor] = kFocus;
    colors[ImGuiCol_NavWindowingHighlight] = kFocus;
    colors[ImGuiCol_NavWindowingDimBg] = {0, 0, 0, .45f};
    colors[ImGuiCol_ModalWindowDimBg] = {0, 0, 0, .55f};
    style.ScaleAllSizes(dpi_scale);
}

void PushPrimaryButton() {
    ImGui::PushStyleColor(ImGuiCol_Button, kPrimary);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kPrimaryHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kPrimaryActive);
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
}

void PopPrimaryButton() {
    ImGui::PopStyleColor(4);
}

}  // namespace skeleton_repair::theme
