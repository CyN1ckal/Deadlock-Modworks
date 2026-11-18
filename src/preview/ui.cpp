#include "ui.hpp"

#include "strings.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <imgui_internal.h>

#include <cmath>
#include <vector>

namespace skeleton_repair::ui {

void CopyToClipboard(const std::string& text) {
    ImGui::SetClipboardText(text.c_str());
}

void RevealInExplorer(const std::filesystem::path& path) {
    std::error_code error;
    if (std::filesystem::is_directory(path, error)) {
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    const std::wstring arguments = L"/select,\"" + path.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL);
}

bool ToolButton(const char* label, const char* tooltip, bool enabled) {
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::SmallButton(label);
    ImGui::EndDisabled();
    if (tooltip != nullptr) {
        ImGui::SetItemTooltip("%s", tooltip);
    }
    return pressed && enabled;
}

void ToolSeparator() {
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();
}

void TextEllipsis(const std::string& text, float width, const ImVec4* color) {
    if (width <= 0) {
        width = ImGui::GetContentRegionAvail().x;
    }
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetTextLineHeight();
    ImGui::Dummy({width, height});
    if (color != nullptr) {
        ImGui::PushStyleColor(ImGuiCol_Text, *color);
    }
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), origin, {origin.x + width, origin.y + height}, origin.x + width,
        origin.x + width, text.c_str(), nullptr, &size);
    if (color != nullptr) {
        ImGui::PopStyleColor();
    }
    if (size.x > width) {
        ImGui::SetItemTooltip("%s", text.c_str());
    }
}

void SectionLabel(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void Heading(const char* text) {
    ImFont* font = theme::HeadingFont();
    if (font) ImGui::PushFont(font);
    ImGui::TextUnformatted(text);
    if (font) ImGui::PopFont();
    ImGui::Dummy({0, ImGui::GetStyle().ItemSpacing.y * .5f});
}

bool PrimaryButton(const char* label, const ImVec2& size, bool enabled) {
    if (enabled) theme::PushPrimaryButton();
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(label, size);
    ImGui::EndDisabled();
    if (enabled) theme::PopPrimaryButton();
    return pressed;
}

bool Toggle(const char* id, bool* value) {
    const float frame = ImGui::GetFrameHeight();
    const float height = std::round(frame * .8f);
    const float width = std::round(height * 1.8f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, {width, frame});
    if (pressed) *value = !*value;
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 min{origin.x, origin.y + (frame - height) * .5f};
    const ImVec2 max{min.x + width, min.y + height};
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec4 fill = *value ? (hovered ? theme::kPrimaryHover : theme::kPrimary) : (hovered ? theme::kRaisedHover : theme::kRaised);
    draw->AddRectFilled(min, max, ImGui::GetColorU32(fill), height * .5f);
    if (!*value) draw->AddRect(min, max, ImGui::GetColorU32(theme::kBorder), height * .5f);
    const float radius = height * .5f - 3.f;
    const float x = *value ? max.x - height * .5f : min.x + height * .5f;
    draw->AddCircleFilled({x, min.y + height * .5f}, radius, ImGui::GetColorU32(*value ? theme::kOnPrimary : theme::kSecondary));
    if (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)
        draw->AddRect({min.x - 2, min.y - 2}, {max.x + 2, max.y + 2}, ImGui::GetColorU32(theme::kFocus), height * .5f + 2, 0, 2.f);
    return pressed;
}

float BadgeWidth(const char* text) {
    return ImGui::CalcTextSize(text).x + ImGui::GetFontSize() * .8f;
}

void Badge(const char* text, const ImVec4& color) {
    const ImVec2 size{BadgeWidth(text), ImGui::GetTextLineHeight() + 2.f};
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, {origin.x + size.x, origin.y + size.y}, ImGui::GetColorU32({color.x, color.y, color.z, .16f}), size.y * .5f);
    draw->AddText({origin.x + ImGui::GetFontSize() * .4f, origin.y + 1.f}, ImGui::GetColorU32(color), text);
}

namespace {
struct BannerFrame {
    ImVec4 Tone;
    ImVec2 Start;
    float Right;
};
std::vector<BannerFrame> g_banners;
}  // namespace

void BeginBanner(const ImVec4& tone) {
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float right = start.x + ImGui::GetContentRegionAvail().x - pad.x * 1.5f;
    g_banners.push_back({tone, start, right});
    ImGui::GetWindowDrawList()->ChannelsSplit(2);
    ImGui::GetWindowDrawList()->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos({start.x + pad.x * 2.f, start.y + pad.y * 1.5f});
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(right - ImGui::GetWindowPos().x + ImGui::GetScrollX());
}

float BannerRight() {
    return g_banners.empty() ? ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x : g_banners.back().Right;
}

void EndBanner() {
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    const BannerFrame frame = g_banners.back();
    g_banners.pop_back();
    const ImVec2 pad = ImGui::GetStyle().FramePadding;
    const ImVec2 max{frame.Right + pad.x * 1.5f, ImGui::GetItemRectMax().y + pad.y * 1.5f};
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->ChannelsSetCurrent(0);
    const float rounding = ImGui::GetStyle().FrameRounding;
    draw->AddRectFilled(frame.Start, max, ImGui::GetColorU32({frame.Tone.x, frame.Tone.y, frame.Tone.z, .12f}), rounding);
    draw->AddRectFilled(frame.Start, {frame.Start.x + 3.f, max.y}, ImGui::GetColorU32(frame.Tone), rounding, ImDrawFlags_RoundCornersLeft);
    draw->ChannelsMerge();
    ImGui::SetCursorScreenPos(frame.Start);
    ImGui::Dummy({max.x - frame.Start.x, max.y - frame.Start.y});
}

void HelpMarker(const char* text) {
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void CopyMenuItem(const char* label, const std::string& text) {
    if (ImGui::MenuItem(label, nullptr, false, !text.empty())) {
        CopyToClipboard(text);
    }
}

void PathMenuItems(const std::filesystem::path& path) {
    CopyMenuItem("Copy path", Narrow(path.wstring()));
    if (ImGui::MenuItem("Reveal in Explorer", nullptr, false, !path.empty())) {
        RevealInExplorer(path);
    }
}

}  // namespace skeleton_repair::ui
