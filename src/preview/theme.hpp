#pragma once

// Steel-blue workspace tokens. Values are sRGB. Contrast is WCAG relative luminance.
// #E6EDF3 on #141B23 is 14.68:1 and on #1D2833 is 12.66:1. #9BAEBE on #1D2833 is 6.55:1.
// #4682B4 behind #E6EDF3 is 3.48:1, so it is not a button fill. Primary fill #2A5678 is 6.58:1,
// its hover #346888 is 5.10:1, and its pressed state #1E4460 is 8.67:1.
// Text accent and focus use #659BC3 (5.01:1 on #1D2833). Disabled #8B9DAB is 5.35:1 on #1D2833
// and darker than #9BAEBE. Success #6BCC85, warning #F2B357, and failure #F56B66 on #1D2833
// are 7.55:1, 8.09:1, and 5.11:1. The viewport clear matches the workspace so a resize does not
// flash the previous grey.

#include <imgui.h>

#include <array>

namespace skeleton_repair::theme {

constexpr ImVec4 Rgb(int red, int green, int blue, float alpha = 1.f) {
    return {red / 255.f, green / 255.f, blue / 255.f, alpha};
}

inline constexpr ImVec4 kWorkspace = Rgb(0x14, 0x1B, 0x23);
inline constexpr ImVec4 kPanel = Rgb(0x1D, 0x28, 0x33);
inline constexpr ImVec4 kBorder = Rgb(0x34, 0x46, 0x57);
inline constexpr ImVec4 kAccent = Rgb(0x46, 0x82, 0xB4);
inline constexpr ImVec4 kFocus = Rgb(0x65, 0x9B, 0xC3);
inline constexpr ImVec4 kText = Rgb(0xE6, 0xED, 0xF3);
inline constexpr ImVec4 kSecondary = Rgb(0x9B, 0xAE, 0xBE);
inline constexpr ImVec4 kDisabled = Rgb(0x8B, 0x9D, 0xAB);
inline constexpr ImVec4 kRaised = Rgb(0x2A, 0x39, 0x48);
inline constexpr ImVec4 kRaisedHover = Rgb(0x3A, 0x4C, 0x5E);
inline constexpr ImVec4 kRaisedActive = Rgb(0x24, 0x32, 0x44);
inline constexpr ImVec4 kSelection = Rgb(0x1E, 0x3C, 0x56);
inline constexpr ImVec4 kSelectionHover = Rgb(0x2A, 0x4E, 0x70);
inline constexpr ImVec4 kSelectionActive = Rgb(0x34, 0x5E, 0x86);
inline constexpr ImVec4 kSelectionBorder = kFocus;
inline constexpr ImVec4 kPrimary = Rgb(0x2A, 0x56, 0x78);
inline constexpr ImVec4 kPrimaryHover = Rgb(0x34, 0x68, 0x88);
inline constexpr ImVec4 kPrimaryActive = Rgb(0x1E, 0x44, 0x60);
inline constexpr ImVec4 kSuccess = Rgb(0x6B, 0xCC, 0x85);
inline constexpr ImVec4 kWarning = Rgb(0xF2, 0xB3, 0x57);
inline constexpr ImVec4 kError = Rgb(0xF5, 0x6B, 0x66);
inline constexpr ImVec4 kTableBorder = Rgb(0x2A, 0x3C, 0x4C);
// Same RGB as the workspace. model_renderer's default clear copies these channels.
inline constexpr std::array<float, 3> kClearRgb{kWorkspace.x, kWorkspace.y, kWorkspace.z};

// Unscaled metrics, then one ScaleAllSizes(dpi_scale). Do not scale again.
void Apply(float dpi_scale);
void PushPrimaryButton();
void PopPrimaryButton();

}  // namespace skeleton_repair::theme
