#include "gamebanana_panel.hpp"

#include "strings.hpp"
#include "ui.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace skeleton_repair {
namespace {

constexpr size_t kMaxImageJobs = 4;
// GameBanana's image host sometimes refuses a burst of requests, so a failed image is tried again later.
constexpr int kImageAttempts = 3;
constexpr size_t kMaxImages = 160;
constexpr int kRetireFrames = 6;

void OpenUrl(const std::string& url) {
    if (!url.starts_with("https://")) return;
    ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::string Size(uint64_t bytes) {
    char text[32];
    if (bytes >= 1024 * 1024) std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else std::snprintf(text, sizeof(text), "%.0f KB", static_cast<double>(bytes) / 1024.0);
    return text;
}

std::string Date(int64_t seconds) {
    if (seconds <= 0) return {};
    const std::time_t time = static_cast<std::time_t>(seconds);
    std::tm local{};
    localtime_s(&local, &time);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d", &local);
    return text;
}

std::string Count(int64_t value) {
    char text[32];
    if (value >= 1000000) std::snprintf(text, sizeof(text), "%.1fM", static_cast<double>(value) / 1e6);
    else if (value >= 1000) std::snprintf(text, sizeof(text), "%.1fk", static_cast<double>(value) / 1e3);
    else std::snprintf(text, sizeof(text), "%lld", static_cast<long long>(value));
    return text;
}

const char* SortLabel(GbSort sort) {
    switch (sort) {
    case GbSort::Newest: return "Newest";
    case GbSort::Updated: return "Recently updated";
    case GbSort::MostDownloaded: return "Most downloaded";
    case GbSort::MostLiked: return "Most liked";
    case GbSort::MostViewed: return "Most viewed";
    }
    return "";
}
}  // namespace

GameBananaPanel::GameBananaPanel(const VulkanDevice& device) : device_(device) {}

GameBananaPanel::~GameBananaPanel() {
    image_cancel_.Cancel();
    for (auto& [url, job] : image_jobs_) if (job.valid()) job.wait();
}

void GameBananaPanel::Go(const std::string& text) {
    std::snprintf(input_, sizeof(input_), "%s", text.c_str());
    if (const auto id = ParseGameBananaModId(text)) { OpenMod(*id); return; }
    const auto first = text.find_first_not_of(" \t");
    query_ = first == std::string::npos ? std::string() : text.substr(first);
    mode_ = query_.empty() ? Mode::Browse : Mode::Search;
    page_ = 1;
    mod_.reset();
    mod_loading_ = 0;
    Query();
}

void GameBananaPanel::Query() {
    list_error_.clear();
    if (mode_ == Mode::Search) {
        list_task_.Start([query = query_, page = page_](CancellationToken token) { return gamebanana::Search(query, page, token); });
    } else {
        list_task_.Start([sort = sort_, page = page_, category = category_](CancellationToken token) {
            return gamebanana::Browse(sort, page, category, token);
        });
    }
}

void GameBananaPanel::OpenMod(int64_t id) {
    mod_loading_ = id;
    mod_error_.clear();
    screenshot_ = 0;
    mod_task_.Start([id](CancellationToken token) { return gamebanana::Mod(id, token); });
}

void GameBananaPanel::PollTasks() {
    try {
        if (auto page = list_task_.Take()) page_data_ = std::move(*page);
    } catch (const std::exception& error) {
        list_error_ = error.what();
    }
    try {
        if (auto mod = mod_task_.Take()) {
            mod_ = std::move(*mod);
            description_ = GbPlainText(mod_->Text);
            mod_loading_ = 0;
        }
    } catch (const std::exception& error) {
        mod_error_ = error.what();
        mod_loading_ = 0;
    }
    try {
        if (auto categories = categories_task_.Take()) categories_ = std::move(*categories);
    } catch (const std::exception&) {
        // Browsing works without categories.
    }
}

void GameBananaPanel::PollImages() {
    const int frame = ImGui::GetFrameCount();
    if (frame != retired_frame_) {
        retired_frame_ = frame;
        std::erase_if(retired_, [](Retired& item) { return --item.FramesLeft <= 0; });
    }
    for (auto job = image_jobs_.begin(); job != image_jobs_.end();) {
        if (job->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++job; continue; }
        auto& image = images_[job->first];
        try {
            const auto pixels = job->second.get();
            if (pixels) image.Texture = std::make_unique<ThumbnailImage>(device_, *pixels);
            else image.Failed = true;  // Not an image WIC can read. Trying again would not help.
        } catch (const std::exception&) {
            if (++image.Attempts >= kImageAttempts) image.Failed = true;
            else image.RetryAt = ImGui::GetTime() + 2.0 * image.Attempts;
        }
        job = image_jobs_.erase(job);
    }
    // Forget the images not drawn for a while. One drawn this frame is never released.
    while (images_.size() > kMaxImages) {
        auto oldest = std::min_element(images_.begin(), images_.end(), [](const auto& a, const auto& b) { return a.second.LastFrame < b.second.LastFrame; });
        if (oldest->second.LastFrame >= frame - 1) break;
        if (oldest->second.Texture) retired_.push_back({std::move(oldest->second.Texture), kRetireFrames});
        images_.erase(oldest);
    }
}

ImTextureID GameBananaPanel::ImageFor(const std::string& url) {
    if (url.empty()) return ImTextureID{};
    auto& image = images_[url];
    image.LastFrame = ImGui::GetFrameCount();
    if (image.Texture) return image.Texture->Texture();
    if (image.Failed || ImGui::GetTime() < image.RetryAt) return ImTextureID{};
    const bool loading = std::any_of(image_jobs_.begin(), image_jobs_.end(), [&](const auto& job) { return job.first == url; });
    if (!loading && image_jobs_.size() < kMaxImageJobs) {
        image_jobs_.emplace_back(url, std::async(std::launch::async, [url, token = image_cancel_.Token()]() -> std::optional<ThumbnailPixels> {
            const auto bytes = gamebanana::Image(url, token);
            // WIC needs COM on this thread.
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            auto pixels = DecodeThumbnail(std::vector<uint8_t>(bytes.begin(), bytes.end()));
            if (SUCCEEDED(com)) CoUninitialize();
            return pixels;
        }));
    }
    return ImTextureID{};
}

void GameBananaPanel::DrawImage(const std::string& url, ImVec2 size, bool hidden) {
    const ImVec2 start = ImGui::GetCursorScreenPos();
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(start, {start.x + size.x, start.y + size.y}, ImGui::GetColorU32(ImGuiCol_FrameBg), 4);
    const char* placeholder = nullptr;
    if (hidden) placeholder = "Rated content";
    else if (url.empty()) placeholder = "No preview";
    else if (const ImTextureID texture = ImageFor(url)) draw->AddImageRounded(texture, start, {start.x + size.x, start.y + size.y}, {0, 0}, {1, 1}, IM_COL32_WHITE, 4);
    else placeholder = images_[url].Failed ? "No preview" : "Loading...";
    if (placeholder) {
        const ImVec2 text = ImGui::CalcTextSize(placeholder);
        draw->AddText({start.x + (size.x - text.x) * .5f, start.y + (size.y - text.y) * .5f}, ImGui::GetColorU32(ImGuiCol_TextDisabled), placeholder);
    }
    ImGui::Dummy(size);
}

void GameBananaPanel::Draw(bool can_download, const char* busy_reason, const std::function<size_t(int64_t)>& installed, const Download& download) {
    PollTasks();
    PollImages();
    if (!categories_requested_) {
        categories_requested_ = true;
        categories_task_.Start([](CancellationToken token) { return gamebanana::Categories(token); });
        Query();
    }
    DrawToolbar();
    if (mod_ || mod_loading_ || !mod_error_.empty()) DrawDetails(can_download, busy_reason, installed, download);
    else DrawResults(installed);
}

void GameBananaPanel::DrawToolbar() {
    const float go_width = ImGui::CalcTextSize("Search").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - go_width - ImGui::GetStyle().ItemSpacing.x);
    const bool entered = ImGui::InputTextWithHint("##gbsearch", "Paste a GameBanana mod link, or search Deadlock mods", input_, sizeof(input_),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Search") || entered) Go(input_);
    ImGui::SetItemTooltip("A gamebanana.com/mods/... link opens that mod. Other text searches Deadlock mods. Empty shows every mod.");

    if (mod_ || mod_loading_ || !mod_error_.empty()) return;
    bool changed = false;
    if (mode_ == Mode::Search) {
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Results for \"%s\"", query_.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) { input_[0] = '\0'; query_.clear(); mode_ = Mode::Browse; page_ = 1; changed = true; }
    } else {
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
        if (ImGui::BeginCombo("##sort", SortLabel(sort_))) {
            for (const auto sort : {GbSort::MostDownloaded, GbSort::MostLiked, GbSort::MostViewed, GbSort::Newest, GbSort::Updated}) {
                if (ImGui::Selectable(SortLabel(sort), sort == sort_)) { sort_ = sort; page_ = 1; changed = true; }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
        std::string current = "Every category";
        for (const auto& category : categories_) if (category.Id == category_) current = category.Name;
        if (ImGui::BeginCombo("##category", current.c_str())) {
            if (ImGui::Selectable("Every category", category_ == 0)) { category_ = 0; page_ = 1; changed = true; }
            for (const auto& category : categories_) {
                char label[160];
                std::snprintf(label, sizeof(label), "%s (%lld)", category.Name.c_str(), static_cast<long long>(category.Count));
                if (ImGui::Selectable(label, category.Id == category_)) { category_ = category.Id; page_ = 1; changed = true; }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("Show rated previews", &show_rated_);
    ImGui::SetItemTooltip("Some mods are marked by their authors with a content rating, such as mature content. Their previews are hidden unless this is on.");

    if (page_data_ && page_data_->PerPage > 0) {
        const int64_t pages = std::max<int64_t>(1, (page_data_->Total + page_data_->PerPage - 1) / page_data_->PerPage);
        char label[64];
        std::snprintf(label, sizeof(label), "Page %d of %lld", page_, static_cast<long long>(pages));
        const float width = ImGui::CalcTextSize(label).x + ImGui::GetFrameHeight() * 2 + ImGui::GetStyle().ItemSpacing.x * 4;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - width));
        ImGui::BeginDisabled(page_ <= 1);
        if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) { --page_; changed = true; }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        ImGui::BeginDisabled(page_ >= pages);
        if (ImGui::ArrowButton("##next", ImGuiDir_Right)) { ++page_; changed = true; }
        ImGui::EndDisabled();
    }
    if (changed) Query();
}

void GameBananaPanel::DrawResults(const std::function<size_t(int64_t)>& installed) {
    ImGui::BeginChild("gbresults", {0, 0}, ImGuiChildFlags_None);
    if (!list_error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kError);
        ImGui::TextWrapped("Could not reach GameBanana: %s", list_error_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::Button("Try again")) Query();
    }
    if (list_task_.Busy()) ImGui::TextDisabled("Loading from GameBanana...");
    if (page_data_ && page_data_->Mods.empty() && !list_task_.Busy()) ImGui::TextDisabled("No mods found.");
    if (page_data_) {
        const float scale = ImGui::GetFontSize() / 15.f;
        const ImVec2 image{220 * scale, 124 * scale};
        const float card = image.x + ImGui::GetStyle().ItemSpacing.x;
        const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + ImGui::GetStyle().ItemSpacing.x) / card));
        int column = 0;
        for (const auto& mod : page_data_->Mods) {
            if (column > 0) ImGui::SameLine();
            ImGui::PushID(static_cast<int>(mod.Id));
            ImGui::BeginGroup();
            const ImVec2 top = ImGui::GetCursorScreenPos();
            DrawImage(mod.ThumbnailUrl, image, mod.Rated && !show_rated_);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + image.x);
            ui::TextEllipsis(mod.Name, image.x);
            ImGui::PopTextWrapPos();
            char line[160];
            std::snprintf(line, sizeof(line), "%s  \xC2\xB7  %s likes", mod.Category.empty() ? "Mod" : mod.Category.c_str(), Count(mod.Likes).c_str());
            ui::TextEllipsis(line, image.x, &ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
            ImGui::EndGroup();
            const ImVec2 bottom = ImGui::GetItemRectMax();
            ImGui::SetCursorScreenPos(top);
            if (ImGui::InvisibleButton("card", {image.x, bottom.y - top.y})) OpenMod(mod.Id);
            const bool hovered = ImGui::IsItemHovered();
            if (hovered) {
                ImGui::GetWindowDrawList()->AddRect(top, {top.x + image.x, bottom.y}, ImGui::GetColorU32(ui::kAccent), 4, 0, 2);
                ImGui::SetTooltip("%s\nby %s\nUpdated %s  \xC2\xB7  %s views\nClick to see files.", mod.Name.c_str(), mod.Submitter.c_str(),
                    Date(mod.DateModified).c_str(), Count(mod.Views).c_str());
            }
            if (installed(mod.Id) > 0) {
                const char* badge = "In library";
                const ImVec2 size = ImGui::CalcTextSize(badge);
                const ImVec2 at{top.x + 6, top.y + 6};
                ImGui::GetWindowDrawList()->AddRectFilled(at, {at.x + size.x + 8, at.y + size.y + 4}, ImGui::GetColorU32(ui::kGood), 3);
                ImGui::GetWindowDrawList()->AddText({at.x + 4, at.y + 2}, IM_COL32(0, 0, 0, 255), badge);
            }
            ImGui::PopID();
            column = (column + 1) % columns;
        }
    }
    ImGui::EndChild();
}

void GameBananaPanel::DrawDetails(bool can_download, const char* busy_reason, const std::function<size_t(int64_t)>& installed, const Download& download) {
    if (ui::ToolButton("<", "Back to the results")) {
        mod_task_.Cancel();
        mod_.reset();
        mod_loading_ = 0;
        mod_error_.clear();
        return;
    }
    ImGui::SameLine();
    if (mod_loading_) { ImGui::TextDisabled("Loading the mod page..."); return; }
    if (!mod_error_.empty()) {
        ImGui::TextColored(ui::kError, "%s", mod_error_.c_str());
        return;
    }
    const auto& mod = *mod_;
    ImGui::TextUnformatted(mod.Name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("by %s  \xC2\xB7  %s  \xC2\xB7  %s likes  \xC2\xB7  updated %s", mod.Submitter.c_str(), mod.Category.c_str(),
        Count(mod.Likes).c_str(), Date(mod.DateModified).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Open page")) OpenUrl(mod.ProfileUrl);
    if (const size_t count = installed(mod.Id)) {
        ImGui::SameLine();
        ImGui::TextColored(ui::kGood, "\xE2\x97\x8F %zu in your library", count);
    }
    if (mod.Obsolete) ImGui::TextColored(ui::kWarning, "The author marked this mod as obsolete. It may not work with the current game.");

    const float scale = ImGui::GetFontSize() / 15.f;
    const ImVec2 image{std::min(530 * scale, ImGui::GetContentRegionAvail().x * .5f), 0};
    const ImVec2 shown{image.x, image.x * 9 / 16};
    ImGui::BeginChild("gbinfo", {shown.x, 0}, ImGuiChildFlags_None);
    if (!mod.Images.empty()) {
        screenshot_ = std::min(screenshot_, mod.Images.size() - 1);
        DrawImage(mod.Images[screenshot_], shown, mod.Rated && !show_rated_);
        if (mod.Images.size() > 1) {
            ImGui::BeginDisabled(screenshot_ == 0);
            if (ImGui::ArrowButton("##shotprev", ImGuiDir_Left)) --screenshot_;
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::Text("%zu / %zu", screenshot_ + 1, mod.Images.size());
            ImGui::SameLine();
            ImGui::BeginDisabled(screenshot_ + 1 >= mod.Images.size());
            if (ImGui::ArrowButton("##shotnext", ImGuiDir_Right)) ++screenshot_;
            ImGui::EndDisabled();
        }
    }
    ImGui::Spacing();
    ImGui::TextWrapped("%s", description_.empty() ? "No description." : description_.c_str());
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("gbfiles", {0, 0}, ImGuiChildFlags_None);
    ui::SectionLabel("FILES");
    if (mod.Files.empty()) ImGui::TextDisabled("This mod has no downloadable files. Its page may link to them elsewhere.");
    if (!can_download && busy_reason) ImGui::TextColored(ui::kWarning, "%s", busy_reason);
    for (const auto& file : mod.Files) {
        ImGui::PushID(static_cast<int>(file.Id));
        ImGui::Separator();
        ImGui::TextUnformatted(file.Name.c_str());
        ImGui::TextDisabled("%s  \xC2\xB7  %s  \xC2\xB7  %s downloads", Size(file.Size).c_str(), Date(file.DateAdded).c_str(), Count(file.Downloads).c_str());
        if (!file.ScanResult.empty() && file.ScanResult != "clean") {
            ImGui::SameLine();
            ImGui::TextColored(ui::kWarning, "(scan: %s)", file.ScanResult.c_str());
        }
        if (!file.Description.empty()) ImGui::TextWrapped("%s", file.Description.c_str());
        ImGui::BeginDisabled(!can_download);
        if (ImGui::Button("Download & enable")) download(mod, file, true);
        ImGui::SetItemTooltip("Adds the mod to your library and enables it in the current profile. Apply it on the Mods page.");
        ImGui::SameLine();
        if (ImGui::Button("Download")) download(mod, file, false);
        ImGui::SetItemTooltip("Adds the mod to your library without enabling it.");
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

}  // namespace skeleton_repair
