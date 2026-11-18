#include "shell_layout.hpp"

#include "startup.hpp"
#include "ui.hpp"

#include <imgui_internal.h>

#include <algorithm>

namespace skeleton_repair::shell {
namespace {

bool WindowInSettings(ImGuiID id) {
    ImGuiContext& context = *ImGui::GetCurrentContext();
    for (ImGuiWindowSettings* settings = context.SettingsWindows.begin(); settings != nullptr;
        settings = context.SettingsWindows.next_chunk(settings)) {
        if (settings->ID == id) return true;
    }
    return false;
}

bool OverlapsWorkArea(ImVec2 min, ImVec2 max) {
    const ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    if (platform.Monitors.Size == 0) return true;
    for (int i = 0; i < platform.Monitors.Size; ++i) {
        const ImGuiPlatformMonitor& monitor = platform.Monitors[i];
        const float left = std::max(min.x, monitor.WorkPos.x);
        const float top = std::max(min.y, monitor.WorkPos.y);
        const float right = std::min(max.x, monitor.WorkPos.x + monitor.WorkSize.x);
        const float bottom = std::min(max.y, monitor.WorkPos.y + monitor.WorkSize.y);
        if (right - left >= 32.f && bottom - top >= 32.f) return true;
    }
    return false;
}

void CenterNextDialog() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {.5f, .5f});
}

bool CloseButton() {
    return ImGui::Button("Close", {-1, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape);
}

}  // namespace

bool HasSavedLayout() {
    return WindowInSettings(ImHashStr(kExplorer)) || WindowInSettings(ImHashStr(kWorkspace));
}

void BuildDefaultDock(ImGuiID dockspace) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
    ImGuiID right = 0;
    const ImGuiID left = ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Left, 0.17f, nullptr, &right);
    ImGui::DockBuilderDockWindow(kExplorer, left);
    ImGui::DockBuilderDockWindow(kWorkspace, right);
    ImGui::DockBuilderFinish(dockspace);
    ImGui::MarkIniSettingsDirty();
}

void BringOnScreen(const char* name, ImVec2 offset) {
    ImGuiWindow* window = ImGui::FindWindowByName(name);
    if (window == nullptr || window->DockIsActive) return;
    const ImVec2 corner(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y);
    if (OverlapsWorkArea(window->Pos, corner)) return;
    const ImVec2 work = ImGui::GetMainViewport()->WorkPos;
    ImGui::SetWindowPos(name, ImVec2(work.x + offset.x, work.y + offset.y), ImGuiCond_Always);
    ImGui::MarkIniSettingsDirty(window);
}

void DockWithoutTab() {
    ImGuiWindowClass window_class;
    window_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
    ImGui::SetNextWindowClass(&window_class);
}

void DrawShortcutsDialog() {
    CenterNextDialog();
    if (!ImGui::BeginPopupModal(kShortcutsDialog, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    static const char* const kShortcuts[][2] = {
        {"Ctrl+O", "Add mods"}, {"Ctrl+Shift+O", "Add every mod in a folder"}, {"Ctrl+G", "Choose the game archive"},
        {"Ctrl+B", "Build the pack as a VPK"}, {"Ctrl+F", "Search skins, packages and sources"}, {"F5", "Reopen every source and reload thumbnails"},
        {"Ctrl+1 ... Ctrl+8", "My mods, Discover, Skins, Pack builder, Repairs, Inspector, Side by side, History"}, {"Alt+Left", "Back to the previous page"},
        {"Ctrl+Shift+E", "Show or hide the explorer"}, {"View > Workspace", "Focus the workspace"},
        {"View > Reset layout", "Restore the explorer and workspace split"},
        {"Double-click", "Inspect a skin, list a package's files, or view a version"},
        {"Enter / Space", "Inspect a skin or list a package's files / include it in the pack"}, {"Ctrl+Wheel", "Thumbnail size in the gallery"},
        {"Drag / right-drag / wheel", "Orbit / pan / zoom a viewport"}, {"F", "Frame the model in the hovered viewport"},
        {"Right-click", "More actions on skins, sources, versions, results and viewports"}, {"F1", "This list"},
    };
    if (ImGui::BeginTable("shortcuts", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        for (const auto& [keys, action] : kShortcuts) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(ui::kAccent, "%s", keys);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(action);
        }
        ImGui::EndTable();
    }
    if (CloseButton()) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawAboutDialog(const char* title, const char* commit) {
    CenterNextDialog();
    if (!ImGui::BeginPopupModal(kAboutDialog, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted(title);
    ImGui::TextDisabled("Build %s", commit);
    ImGui::TextDisabled("Browse skins, build packs, inspect models and repair skeleton changes.");
    ImGui::TextDisabled("Dear ImGui %s, Vulkan 1.3", ImGui::GetVersion());
#ifdef TRACY_ENABLE
    ImGui::TextDisabled("Tracy is announcing this window. Connect tracy-profiler, or open 127.0.0.1 port 8086.");
#endif
    if (CloseButton()) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DrawQuickStartDialog() {
    CenterNextDialog();
    ImGui::SetNextWindowSize({ImGui::GetFontSize() * 36, 0}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kQuickStartDialog, nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    ImGui::TextWrapped("These steps are the whole first run. Assembly does not install the pack.");
    ImGui::Spacing();
    for (const GuideStep& step : QuickStartSteps()) {
        ImGui::TextColored(ui::kAccent, "%s", step.Title);
        ImGui::TextWrapped("%s", step.Detail);
        ImGui::Spacing();
    }
    if (CloseButton()) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

}  // namespace skeleton_repair::shell
