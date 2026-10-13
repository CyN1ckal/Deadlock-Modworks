#pragma once

// Compact widgets and shell actions shared by the model tools panels.

#include "theme.hpp"

#include <filesystem>
#include <string>

namespace skeleton_repair::ui {

// kAccent is the focus blue, not #4682B4: these call sites paint text and selection borders.
inline constexpr ImVec4 kAccent = theme::kFocus;
inline constexpr ImVec4 kGood = theme::kSuccess;
inline constexpr ImVec4 kWarning = theme::kWarning;
inline constexpr ImVec4 kError = theme::kError;

void CopyToClipboard(const std::string& text);
// Opens Explorer with a file selected, or a folder itself.
void RevealInExplorer(const std::filesystem::path& path);

// A small framed button whose tooltip also shows while it is disabled, so it can say why.
bool ToolButton(const char* label, const char* tooltip = nullptr, bool enabled = true);
// A thin vertical divider between toolbar groups, on the current line.
void ToolSeparator();
// Text on one line, cut with an ellipsis to `width` (the remaining width when not positive). A cut text shows in full in a tooltip.
void TextEllipsis(const std::string& text, float width = 0, const ImVec4* color = nullptr);
// A small muted heading row for a sidebar or side pane section.
void SectionLabel(const char* text);
// A "(?)" marker with a tooltip, for help text that would otherwise take a line of its own.
void HelpMarker(const char* text);

// Context menu items. Each closes the menu when chosen.
void CopyMenuItem(const char* label, const std::string& text);
// "Copy path" and "Reveal in Explorer".
void PathMenuItems(const std::filesystem::path& path);

}  // namespace skeleton_repair::ui
