#pragma once

#include <imgui.h>

// The docked layout of the two shell windows, Explorer and Workspace, and the help dialogs of the menu bar.
namespace skeleton_repair::shell {

inline constexpr const char* kExplorer = "Explorer";
inline constexpr const char* kWorkspace = "Workspace";

// A previous session stored Explorer or Workspace. A missing file, or one without those windows, is not a layout.
bool HasSavedLayout();
// Explorer on the left, Workspace on the right.
void BuildDefaultDock(ImGuiID dockspace);
// Moves a floating shell window that is off every monitor back onto the main viewport. Docked windows stay.
void BringOnScreen(const char* name, ImVec2 offset);
// The explorer and workspace are fixed panels, so their dock nodes show no tab. Call before Begin.
void DockWithoutTab();

// Popup names for ImGui::OpenPopup.
inline constexpr const char* kShortcutsDialog = "Keyboard shortcuts";
inline constexpr const char* kAboutDialog = "About";
inline constexpr const char* kQuickStartDialog = "Quick start";

// Each draws its modal while it is open, centered on the main viewport.
void DrawShortcutsDialog();
void DrawAboutDialog(const char* title, const char* commit);
void DrawQuickStartDialog();

}  // namespace skeleton_repair::shell
