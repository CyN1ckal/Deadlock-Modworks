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
// A page or empty-state heading in the larger bold face, with a little space under it.
void Heading(const char* text);
// The page's one main action, in the primary fill. Disabled buttons keep the plain look.
bool PrimaryButton(const char* label, const ImVec2& size = {0, 0}, bool enabled = true);
// An on/off switch the height of a frame. Returns true when clicked, after flipping `value`.
bool Toggle(const char* id, bool* value);
// A small rounded label in a tint of `color`, on the current line. It is an item, so a tooltip can follow it.
void Badge(const char* text, const ImVec4& color);
// The width Badge takes for `text`.
float BadgeWidth(const char* text);
// A full-width tinted panel with an accent edge in `tone`, around whatever is drawn between Begin and End.
// Not for use inside a table, which splits the draw list itself.
void BeginBanner(const ImVec4& tone);
// The right edge of the banner's content, in screen space, for right-aligned buttons.
float BannerRight();
void EndBanner();
// A full-width navigation row: an icon when the icon font is loaded, the label, and an optional muted badge on the
// right. The selected row is filled and has an accent edge. `dim` draws the label in the secondary colour.
bool NavItem(const char* icon, const char* label, bool selected, const char* badge = nullptr, bool dim = false,
    const ImVec4* badge_color = nullptr);
// A "(?)" marker with a tooltip, for help text that would otherwise take a line of its own.
void HelpMarker(const char* text);

// Context menu items. Each closes the menu when chosen.
void CopyMenuItem(const char* label, const std::string& text);
// "Copy path" and "Reveal in Explorer".
void PathMenuItems(const std::filesystem::path& path);

}  // namespace skeleton_repair::ui
