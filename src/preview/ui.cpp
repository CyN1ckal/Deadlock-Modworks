#include "ui.hpp"

#include "strings.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <imgui_internal.h>

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
