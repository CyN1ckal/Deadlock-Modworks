#include "library_panel.hpp"
#include "strings.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace skeleton_repair {
namespace {
bool Same(const LibraryModel& a, const LibraryModel& b) {
    return EqualsIgnoreCase(Narrow(a.Archive.wstring()), Narrow(b.Archive.wstring())) && EqualsIgnoreCase(a.Model, b.Model);
}
std::filesystem::path Dialog(void* owner, bool save, bool vpk) {
    wchar_t buffer[32768] = {};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = static_cast<HWND>(owner);
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
    dialog.lpstrFilter = vpk ? L"VPK archive\0*.vpk\0" : L"Model library (*.json)\0*.json\0";
    dialog.lpstrDefExt = vpk ? L"vpk" : L"json";
    dialog.lpstrTitle = vpk ? L"Export combined model VPK (choose a new filename)" : save ? L"Save model library" : L"Open model library";
    dialog.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog))) return {};
    return buffer;
}
}
LibraryPanel::~LibraryPanel() {
    cancellation_.Cancel();
    if (worker_.valid()) worker_.wait();
    InvalidateThumbnails();
}
void LibraryPanel::InvalidateThumbnails() {
    if (!thumbnails_.empty()) {
        vkDeviceWaitIdle(device_.Device);
        thumbnails_.clear();
    }
}
void LibraryPanel::Record(VkCommandBuffer command_buffer) {
    const ViewSettings settings{0, false, false, ColorMode::Textured};
    for (auto& [index, thumbnail] : thumbnails_) if (thumbnail.Viewport && !thumbnail.Rendered) {
        thumbnail.Viewport->Record(command_buffer, thumbnail.Camera, settings);
        thumbnail.Rendered = true;
    }
}
void LibraryPanel::DrawThumbnail(size_t index, const std::filesystem::path& game, float size, size_t cache_limit, bool& loaded_this_frame) {
    auto found = thumbnails_.find(index);
    if (found == thumbnails_.end() && !loaded_this_frame) {
        // Never evict an image referenced by this frame's ImGui draw list.
        while (thumbnails_.size() >= cache_limit) {
            const auto oldest = std::min_element(thumbnails_.begin(), thumbnails_.end(), [](const auto& a, const auto& b) {
                return a.second.LastFrame < b.second.LastFrame;
            });
            if (oldest->second.LastFrame < ImGui::GetFrameCount()) {
                vkDeviceWaitIdle(device_.Device);
                thumbnails_.erase(oldest);
            } else break;
        }
        if (thumbnails_.size() < cache_limit) {
            loaded_this_frame = true;
            Thumbnail thumbnail;
            try {
                const auto& model = models_.at(index);
                const auto archive = VpkArchive::Open(model.Archive);
                auto lease = archive.LockSources();
                std::optional<VpkArchive> base;
                std::optional<ReadLease> base_lease;
                if (!game.empty()) { base.emplace(VpkArchive::Open(game)); base_lease.emplace(base->LockSources()); }
                const VpkArchive* resource_sources[] = {&archive, base ? &*base : nullptr};
                const ResourceReader read = [&](const std::string& path) -> std::optional<std::vector<uint8_t>> {
                    for (const auto* source : resource_sources) {
                        if (!source) continue;
                        for (const auto& entry : source->Entries()) if (EqualsIgnoreCase(entry.FullPath, path)) return source->Read(entry);
                    }
                    return std::nullopt;
                };
                const auto bytes = read(model.Model);
                if (!bytes) throw InvalidData("Model is no longer present. Refresh the library.");
                const auto geometry = DecodeModelGeometry(*bytes);
                thumbnail.Viewport = std::make_unique<ModelViewport>(device_);
                thumbnail.Viewport->SetModel(&geometry, 0, &read, 128);
                if (thumbnail.Viewport->Scene().Meshes.empty()) throw InvalidData("No renderable meshes in this model.");
                thumbnail.Camera = OrbitCamera::Framing(thumbnail.Viewport->Scene().Min, thumbnail.Viewport->Scene().Max);
            } catch (const std::exception& error) {
                thumbnail.Viewport.reset();
                thumbnail.Error = error.what();
            }
            found = thumbnails_.emplace(index, std::move(thumbnail)).first;
        }
    }
    if (found != thumbnails_.end()) {
        found->second.LastFrame = ImGui::GetFrameCount();
        if (found->second.Viewport) {
            if (found->second.RenderSize != size) {
                found->second.RenderSize = size;
                found->second.Rendered = false;
            }
            found->second.Viewport->Draw("thumbnail", {size, size}, found->second.Camera, false);
            if (!found->second.Viewport->MaterialNotes().empty()) ImGui::SetItemTooltip("Some materials are unavailable. Open the full preview for details.");
            return;
        }
    }
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy({size, size});
    const char* label = found == thumbnails_.end() ? "Loading..." : "No preview";
    const auto text_size = ImGui::CalcTextSize(label);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, {origin.x + size, origin.y + size}, ImGui::GetColorU32(ImGuiCol_FrameBg), 4);
    draw->AddText({origin.x + (size - text_size.x) / 2, origin.y + (size - text_size.y) / 2}, ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
    if (found != thumbnails_.end()) ImGui::SetItemTooltip("%s", found->second.Error.c_str());
}
void LibraryPanel::Poll() {
    if (worker_.valid() && worker_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) status_ = worker_.get();
}
void LibraryPanel::Restore(const ModelLibrary& library) {
    selected_ = library.Selected;
    Refresh(library.Sources);
    source_filter_.clear();
    status_.clear();
}
ModelLibrary LibraryPanel::Snapshot(const std::vector<std::filesystem::path>& sources, const std::filesystem::path& game) const {
    ModelLibrary result{sources, {}, game};
    for (const auto& selected : selected_) if (std::any_of(sources.begin(), sources.end(), [&](const auto& source) {
        return EqualsIgnoreCase(Narrow(source.wstring()), Narrow(selected.Archive.wstring()));
    })) result.Selected.push_back(selected);
    return result;
}
void LibraryPanel::Refresh(const std::vector<std::filesystem::path>& sources) {
    InvalidateThumbnails();
    sources_ = sources;
    if (!source_filter_.empty() && std::find(sources.begin(), sources.end(), source_filter_) == sources.end()) source_filter_.clear();
    models_.clear(); errors_.clear();
    for (const auto& path : sources) {
        try {
            const auto archive = VpkArchive::Open(path);
            for (const auto& entry : archive.Entries()) if (entry.FullPath.ends_with(".vmdl_c")) models_.push_back({path, entry.FullPath});
        } catch (const std::exception& error) { errors_.push_back(Narrow(path.wstring()) + ": " + error.what()); }
    }
    std::sort(models_.begin(), models_.end(), [](const auto& a, const auto& b) {
        if (a.Model != b.Model) return a.Model < b.Model;
        return a.Archive < b.Archive;
    });
    std::erase_if(selected_, [&](const auto& selected) {
        return std::none_of(sources.begin(), sources.end(), [&](const auto& source) { return EqualsIgnoreCase(Narrow(source.wstring()), Narrow(selected.Archive.wstring())); });
    });
}
void LibraryPanel::Draw(void* owner, const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& game, bool repair_busy, bool pack_view, const std::function<void()>& add,
    const std::function<void(const ModelLibrary&)>& load, const std::function<void(const LibraryModel&)>& preview) {
    Poll();
    if (thumbnail_game_ != game) { InvalidateThumbnails(); thumbnail_game_ = game; }
    if (!Busy() && sources_ != sources) Refresh(sources);
    const bool locked = Busy() || repair_busy;
    ImGui::TextColored({.47f, .85f, .77f, 1.f}, pack_view ? "YOUR LOADOUT" : "YOUR COLLECTION");
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted(pack_view ? "VPK builder" : "Mod library");
    ImGui::SetWindowFontScale(1.f);
    ImGui::TextDisabled(pack_view ? "Choose your skins. Export one pack." : "Browse your skins. Make them your own.");
    ImGui::Spacing();
    ImGui::BeginDisabled(locked);
    if (!pack_view) {
        if (ImGui::Button("Add mods...", {140, 36})) add();
        ImGui::SameLine();
    }
    if (ImGui::Button("Open library...")) {
        const auto path = Dialog(owner, false, false);
        if (!path.empty()) try {
            const auto library = ModelLibrary::Load(path);
            load(library);
            source_filter_.clear(); status_ = "Restoring library...";
        } catch (const std::exception& error) { status_ = error.what(); }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save library...")) {
        const auto path = Dialog(owner, true, false);
        if (!path.empty()) try { Snapshot(sources, game).Save(path); status_ = "Library saved."; }
        catch (const std::exception& error) { status_ = error.what(); }
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) Refresh(sources);
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (pack_view) {
        ImGui::SeparatorText("Pack contents");
        ImGui::Text("%zu skins selected from your library", selected_.size());
        ImGui::TextWrapped("Dependencies are included automatically. Shared files are deduplicated; conflicting assets and missing dependencies block export.");
        ImGui::BeginDisabled(locked || selected_.empty());
        if (ImGui::Button("Build VPK...", {180, 38})) {
            const auto path = Dialog(owner, true, true);
            if (!path.empty()) {
                cancellation_ = CancellationSource{};
                status_ = "Collecting dependencies and validating your pack...";
                worker_ = std::async(std::launch::async, [models = selected_, game, path, token = cancellation_.Token()] {
                    try { const auto count = ExportModelLibrary(models, game, path, token);
                        return "Exported " + std::to_string(count) + " files to " + Narrow(path.wstring()) + ". Test the pack in Deadlock.";
                    } catch (const std::exception& error) { return std::string(error.what()); }
                });
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear pack")) { selected_.clear(); status_.clear(); }
        ImGui::EndDisabled();
        if (game.empty()) ImGui::TextColored({1.f, .76f, .43f, 1.f}, "Connect Deadlock in the sidebar to resolve base-game assets.");
    }
    if (Busy() && ImGui::Button("Cancel build")) cancellation_.Cancel();
    if (!status_.empty()) ImGui::TextWrapped("%s", status_.c_str());
    ImGui::Separator();
    ImGui::BeginDisabled(locked);
    ImGui::SetNextItemWidth(std::max(140.f, ImGui::GetContentRegionAvail().x * .40f));
    ImGui::InputTextWithHint("##gallerysearch", "Search skins or source archives...", filter_, sizeof(filter_));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(140.f, ImGui::GetContentRegionAvail().x * .60f));
    const auto source_label = source_filter_.empty() ? std::string("All sources") : Narrow(source_filter_.filename().wstring());
    if (ImGui::BeginCombo("##sourcefilter", source_label.c_str())) {
        if (ImGui::Selectable("All sources", source_filter_.empty())) source_filter_.clear();
        for (const auto& source : sources) {
            const auto key = Narrow(source.wstring());
            ImGui::PushID(key.c_str());
            if (ImGui::Selectable(Narrow(source.filename().wstring()).c_str(), source == source_filter_)) source_filter_ = source;
            ImGui::SetItemTooltip("%s", key.c_str());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (!pack_view) ImGui::Checkbox("In pack", &selected_only_);
    ImGui::EndDisabled();
    ImGuiTextFilter filter(filter_);
    std::vector<size_t> filtered;
    for (size_t i = 0; i < models_.size(); ++i) {
        const auto& model = models_[i];
        const auto source = Narrow(model.Archive.wstring());
        const bool selected = std::any_of(selected_.begin(), selected_.end(), [&](const auto& item) { return Same(item, model); });
        if ((pack_view || selected_only_) && !selected) continue;
        if (!source_filter_.empty() && source_filter_ != model.Archive) continue;
        if (filter.PassFilter((model.Model + " " + source).c_str())) filtered.push_back(i);
    }
    ImGui::TextDisabled("%zu skins shown   /   %zu sources   /   %zu in pack", filtered.size(), sources.size(), selected_.size());
    ImGui::BeginChild("gallery", {0, 0});
    for (const auto& error : errors_) ImGui::TextWrapped("%s", error.c_str());
    ImGui::BeginDisabled(locked);
    for (size_t i = 0; i < selected_.size();) {
        if (std::none_of(models_.begin(), models_.end(), [&](const auto& model) { return Same(model, selected_[i]); })) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::TextWrapped("Missing skin: %s (%s)", selected_[i].Model.c_str(), Narrow(selected_[i].Archive.wstring()).c_str());
            if (ImGui::SmallButton("Remove missing skin")) { selected_.erase(selected_.begin() + i); ImGui::PopID(); continue; }
            ImGui::PopID();
        }
        ++i;
    }
    if (filtered.empty()) {
        ImGui::Spacing(); ImGui::Spacing();
        ImGui::SetWindowFontScale(1.3f);
        ImGui::TextUnformatted(models_.empty() ? "Start your mod collection" : pack_view && selected_.empty() ? "Your pack is empty" : "No matching skins");
        ImGui::SetWindowFontScale(1.f);
        ImGui::TextWrapped(models_.empty() ? "Add or drop reference VPKs to browse their skins here. Your source files stay available for future builds." : pack_view && selected_.empty() ? "Visit the mod library and choose Include in pack on the skins you want." : "Try another search or source filter.");
        if (models_.empty() && ImGui::Button("Add your first mods...", {190, 40})) add();
    } else {
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float available = ImGui::GetContentRegionAvail().x;
        const float minimum_card = 220.f * ImGui::GetFontSize() / 17.f;
        const int columns = std::max(1, static_cast<int>((available + spacing) / (minimum_card + spacing)));
        const float card_width = (available - spacing * (columns - 1)) / columns;
        const float padding = ImGui::GetStyle().WindowPadding.x;
        const float image_size = std::min(260.f * ImGui::GetFontSize() / 17.f, std::max(64.f, card_width - 2 * padding));
        const float card_height = image_size + ImGui::GetStyle().ItemSpacing.y + 2 * ImGui::GetStyle().WindowPadding.y + 2 * ImGui::GetTextLineHeightWithSpacing() + 2 * ImGui::GetFrameHeightWithSpacing();
        const float row_height = card_height + ImGui::GetStyle().ItemSpacing.y;
        const auto visible_rows = static_cast<size_t>(std::ceil(ImGui::GetWindowHeight() / row_height)) + 2;
        const auto cache_limit = std::clamp(visible_rows * static_cast<size_t>(columns), size_t{16}, size_t{64});
        bool loaded_this_frame = repair_busy;
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>((filtered.size() + columns - 1) / columns), row_height);
        while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            for (int column = 0; column < columns; ++column) {
                const size_t position = static_cast<size_t>(row) * columns + column;
                if (position >= filtered.size()) break;
                const size_t index = filtered[position];
                const auto& model = models_[index];
                auto found = std::find_if(selected_.begin(), selected_.end(), [&](const auto& item) { return Same(item, model); });
                bool checked = found != selected_.end();
                if (column > 0) ImGui::SameLine(0, spacing);
                ImGui::PushID(static_cast<int>(index));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, checked ? ImVec4{.095f, .19f, .19f, 1.f} : ImVec4{.09f, .12f, .15f, 1.f});
                ImGui::PushStyleColor(ImGuiCol_Border, checked ? ImVec4{.34f, .72f, .63f, 1.f} : ImVec4{.17f, .22f, .27f, 1.f});
                ImGui::BeginChild("card", {card_width, card_height}, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                const float centered = std::max(0.f, (ImGui::GetContentRegionAvail().x - image_size) * .5f);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + centered);
                DrawThumbnail(index, game, image_size, cache_limit, loaded_this_frame);
                const auto basename = model.Model.substr(model.Model.find_last_of('/') + 1);
                auto title = basename.substr(0, basename.size() - std::string(".vmdl_c").size());
                std::replace(title.begin(), title.end(), '_', ' ');
                ImGui::TextUnformatted(title.c_str());
                ImGui::SetItemTooltip("%s", model.Model.c_str());
                ImGui::TextDisabled("%s", Narrow(model.Archive.filename().wstring()).c_str());
                ImGui::SetItemTooltip("%s", Narrow(model.Archive.wstring()).c_str());
                if (ImGui::Checkbox("Include in pack", &checked)) {
                    if (checked) selected_.push_back(model); else selected_.erase(found);
                    status_.clear();
                }
                if (ImGui::Button("Inspect", {-1, 0})) preview(model);
                ImGui::EndChild();
                ImGui::PopStyleColor(2);
                ImGui::PopID();
            }
        }
    }
    ImGui::EndDisabled();
    ImGui::EndChild();
}
}
