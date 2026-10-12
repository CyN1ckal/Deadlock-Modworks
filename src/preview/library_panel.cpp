#include "library_panel.hpp"
#include "character_view.hpp"
#include "strings.hpp"
#include "ui.hpp"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <objbase.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <numeric>
#include <set>
#include <thread>

namespace skeleton_repair {
namespace {
bool Same(const LibraryModel& a, const LibraryModel& b) {
    return EqualsIgnoreCase(Narrow(a.Archive.wstring()), Narrow(b.Archive.wstring())) && EqualsIgnoreCase(a.Model, b.Model);
}
enum class FileKind { Pack, Library, SharedLibrary };
std::filesystem::path Dialog(void* owner, FileKind kind) {
    const bool save = kind != FileKind::Library;
    wchar_t buffer[32768] = {};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = static_cast<HWND>(owner);
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
    dialog.lpstrFilter = kind == FileKind::Pack ? L"VPK archive\0*.vpk\0"
        : kind == FileKind::SharedLibrary ? L"Mod library (*.db)\0*.db\0"
        : L"Mod libraries (*.db;*.json)\0*.db;*.json\0";
    dialog.lpstrDefExt = kind == FileKind::Pack ? L"vpk" : L"db";
    dialog.lpstrTitle = kind == FileKind::Pack ? L"Export combined model VPK (choose a new filename)"
        : kind == FileKind::SharedLibrary ? L"Share your mod library" : L"Add a shared mod library";
    dialog.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog))) return {};
    return buffer;
}
}
LibraryPanel::LibraryPanel(const VulkanDevice& device, std::shared_ptr<ArchiveCache> archives)
    : device_(device), archives_(std::move(archives)), legacy_thumbnails_(LegacyThumbnailFolder()) {
    try { store_.emplace(LocalModStore::DefaultRoot()); } catch (const std::exception&) {}
    const unsigned count = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
    for (unsigned i = 0; i < count; ++i) workers_.emplace_back([this] { RunThumbnailWorker(); });
}
LibraryPanel::~LibraryPanel() {
    cancellation_.Cancel();
    if (worker_.valid()) worker_.wait();
    {
        std::lock_guard lock(queue_mutex_);
        stopping_ = true;
        // Pending saves still finish, so thumbnails rendered this session are not rendered again next time.
        std::erase_if(jobs_, [](const ThumbnailJob& job) { return !job.Save; });
        thumbnail_cancellation_.Cancel();
    }
    queue_changed_.notify_all();
    for (auto& thread : workers_) thread.join();
    InvalidateThumbnails();
}
void LibraryPanel::RunThumbnailWorker() {
    // Thumbnail images are encoded and decoded with WIC.
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    std::unique_lock lock(queue_mutex_);
    while (true) {
        queue_changed_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
        if (jobs_.empty()) break;
        const ThumbnailJob job = std::move(jobs_.front());
        jobs_.pop_front();
        ++active_jobs_;
        lock.unlock();
        std::optional<ThumbnailResult> result;
        if (job.Save) {
            // A failed save only means the thumbnail is rendered again next time.
            try { store_->SaveThumbnail(job.SaveKey, EncodeThumbnail(*job.Save)); } catch (const std::exception&) {}
        } else {
            result = LoadThumbnail(job);
        }
        lock.lock();
        --active_jobs_;
        if (result) results_.push_back(std::move(*result));
        queue_idle_.notify_all();
    }
    lock.unlock();
    if (com) CoUninitialize();
}
LibraryPanel::ThumbnailResult LibraryPanel::LoadThumbnail(const ThumbnailJob& job) {
    ThumbnailResult result{job.Generation, job.Index};
    try {
        job.Cancellation.ThrowIfCancelled();
        if (store_) {
            result.CacheKey = ThumbnailKey(&*store_, job.Model, job.Game);
            // An unavailable database only means the thumbnail is rendered.
            try {
                if (const auto png = store_->Thumbnail(result.CacheKey))
                    if ((result.Pixels = DecodeThumbnail(*png))) return result;
                // Copies a thumbnail an earlier release rendered for the same stored mod. The old file is kept.
                if (!legacy_thumbnails_.empty()) for (const auto& legacy : store_->LegacyPaths(job.Model.Archive)) {
                    std::ifstream in(LegacyThumbnailPath(legacy_thumbnails_, {legacy, job.Model.Model}, job.Game), std::ios::binary);
                    if (!in) continue;
                    const std::vector<uint8_t> png(std::istreambuf_iterator<char>(in), {});
                    if ((result.Pixels = DecodeThumbnail(png))) {
                        store_->SaveThumbnail(result.CacheKey, png);
                        return result;
                    }
                }
            } catch (const std::exception&) {}
        }
        const auto archive = archives_->Get(job.Model.Archive);
        const auto lease = archive->Archive().LockSources();
        const auto base = job.Game.empty() ? nullptr : archives_->Get(job.Game);
        const IndexedArchive* resource_sources[] = {archive.get(), base.get()};
        const ResourceReader read = [&](const std::string& path) -> std::optional<std::vector<uint8_t>> {
            for (const auto* source : resource_sources) {
                if (!source) continue;
                if (auto bytes = source->Read(path, job.Cancellation)) return bytes;
            }
            return std::nullopt;
        };
        const auto bytes = read(job.Model.Model);
        if (!bytes) throw InvalidData("Model is no longer present. Refresh the library.");
        const auto geometry = DecodeModelGeometry(*bytes);
        job.Cancellation.ThrowIfCancelled();
        result.Model = PrepareModel(geometry, &read, 128);
        if (result.Model->Scene.Meshes.empty()) throw InvalidData("No renderable meshes in this model.");
    } catch (const std::exception& error) {
        result.Model.reset();
        result.Error = error.what();
    }
    return result;
}
void LibraryPanel::StopLoading() {
    std::unique_lock lock(queue_mutex_);
    std::erase_if(jobs_, [](const ThumbnailJob& job) { return !job.Save; });
    thumbnail_cancellation_.Cancel();
    thumbnail_cancellation_ = CancellationSource{};
    queue_idle_.wait(lock, [this] { return active_jobs_ == 0; });
    lock.unlock();
    scan_.Cancel(true);
    // Cancelled loads come back as errors and the scan was dropped; request both again afterwards.
    InvalidateThumbnails();
    sources_.clear();
}
void LibraryPanel::InvalidateThumbnails() {
    ++generation_;
    {
        std::lock_guard lock(queue_mutex_);
        std::erase_if(jobs_, [](const ThumbnailJob& job) { return !job.Save; });
        results_.clear();
    }
    ready_.clear();
    if (!thumbnails_.empty() || !retired_.empty() || renderer_) {
        vkDeviceWaitIdle(device_.Device);
        thumbnails_.clear();
        retired_.clear();
        renderer_.reset();
    }
}
void LibraryPanel::Evict(std::map<size_t, Thumbnail>::iterator thumbnail) {
    if (thumbnail->second.Image) retired_.push_back(std::move(thumbnail->second.Image));
    if (thumbnail->second.Pending) {
        std::lock_guard lock(queue_mutex_);
        std::erase_if(jobs_, [&](const ThumbnailJob& job) { return !job.Save && job.Index == thumbnail->first; });
    }
    thumbnails_.erase(thumbnail);
}
ThumbnailPixels LibraryPanel::RenderThumbnail(const PreparedModel& model) {
    if (!renderer_) renderer_ = std::make_unique<ModelRenderer>(device_);
    renderer_->SetScene(model.Scene);
    if (model.Materials) renderer_->SetMaterials(*model.Materials);
    renderer_->Resize(kThumbnailSize, kThumbnailSize);
    const auto visible = DefaultVisibility(model.Scene, 0);
    RenderOptions options;
    options.VisibleMeshes = visible;
    options.ShowSkeleton = false;
    options.ShowGrid = false;
    options.Coloring = ColorMode::Textured;
    // Framing puts the camera in front of the model.
    const auto camera = OrbitCamera::Framing(model.Scene.Min, model.Scene.Max);
    return {kThumbnailSize, kThumbnailSize, renderer_->Capture(camera, options)};
}
void LibraryPanel::UploadThumbnails() {
    {
        std::lock_guard lock(queue_mutex_);
        for (auto& result : results_) ready_.push_back(std::move(result));
        results_.clear();
    }
    // Rendering and uploading images must stay on this thread, so spread that work over frames.
    const auto start = std::chrono::steady_clock::now();
    while (!ready_.empty() && std::chrono::steady_clock::now() - start < std::chrono::milliseconds(6)) {
        ThumbnailResult result = std::move(ready_.front());
        ready_.pop_front();
        const auto found = thumbnails_.find(result.Index);
        if (result.Generation != generation_ || found == thumbnails_.end() || !found->second.Pending) continue;
        Thumbnail& thumbnail = found->second;
        thumbnail.Pending = false;
        try {
            if (!result.Pixels) {
                if (!result.Model) throw InvalidData(result.Error);
                // Render once; later loads read the saved image instead of the model.
                result.Pixels = RenderThumbnail(*result.Model);
                result.Model.reset();
                if (store_ && !result.CacheKey.empty()) {
                    {
                        std::lock_guard lock(queue_mutex_);
                        ThumbnailJob save{generation_, SIZE_MAX};
                        save.Save = *result.Pixels;
                        save.SaveKey = result.CacheKey;
                        jobs_.push_back(std::move(save));
                    }
                    queue_changed_.notify_one();
                }
            }
            thumbnail.Image = std::make_unique<ThumbnailImage>(device_, *result.Pixels);
        } catch (const std::exception& error) {
            thumbnail.Image.reset();
            thumbnail.Error = error.what();
        }
    }
}
const std::string* LibraryPanel::DrawThumbnail(size_t index, const std::filesystem::path& game, ImVec2 origin, float size,
    size_t cache_limit, bool pause_loading) {
    auto found = thumbnails_.find(index);
    if (found == thumbnails_.end() && !pause_loading) {
        // Never evict an image referenced by this frame's ImGui draw list.
        while (thumbnails_.size() >= cache_limit) {
            const auto oldest = std::min_element(thumbnails_.begin(), thumbnails_.end(), [](const auto& a, const auto& b) {
                return a.second.LastFrame < b.second.LastFrame;
            });
            if (oldest->second.LastFrame < ImGui::GetFrameCount()) Evict(oldest);
            else break;
        }
        if (thumbnails_.size() < cache_limit) {
            found = thumbnails_.emplace(index, Thumbnail{}).first;
            {
                std::lock_guard lock(queue_mutex_);
                jobs_.push_back({generation_, index, models_.at(index), game, thumbnail_cancellation_.Token()});
            }
            queue_changed_.notify_one();
        }
    }
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 corner{origin.x + size, origin.y + size};
    if (found != thumbnails_.end()) {
        found->second.LastFrame = ImGui::GetFrameCount();
        if (found->second.Image) {
            draw->AddImageRounded(found->second.Image->Texture(), origin, corner, {0, 0}, {1, 1}, IM_COL32_WHITE, 2);
            return nullptr;
        }
    }
    const bool loading = found == thumbnails_.end() || found->second.Pending;
    draw->AddRectFilled(origin, corner, ImGui::GetColorU32(ImGuiCol_FrameBg), 2);
    const char* label = loading ? "Loading..." : "No preview";
    const auto text_size = ImGui::CalcTextSize(label);
    if (text_size.x < size - 4) {
        draw->AddText({origin.x + (size - text_size.x) / 2, origin.y + (size - text_size.y) / 2}, ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
    }
    return loading ? nullptr : &found->second.Error;
}
void LibraryPanel::Poll() {
    if (worker_.valid() && worker_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) status_ = worker_.get();
    try {
        if (auto scan = scan_.Take()) {
            InvalidateThumbnails();
            models_ = std::move(scan->Models);
            errors_ = std::move(scan->Errors);
            current_.reset();
            source_counts_.clear();
            for (const auto& model : models_) ++source_counts_[model.Archive];
            SortModels(sort_column_, sort_descending_);
            pack_stale_ = true;
        }
    } catch (const std::exception& error) {
        errors_ = {error.what()};
    }
    if (!retired_.empty()) {
        vkDeviceWaitIdle(device_.Device);
        retired_.clear();
    }
    UploadThumbnails();
}
void LibraryPanel::Restore(const ModelLibrary& library) {
    selected_ = library.Selected;
    pack_stale_ = true;
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
    sources_ = sources;
    if (!source_filter_.empty() && std::find(sources.begin(), sources.end(), source_filter_) == sources.end()) source_filter_.clear();
    std::erase_if(selected_, [&](const auto& selected) {
        return std::none_of(sources.begin(), sources.end(), [&](const auto& source) { return EqualsIgnoreCase(Narrow(source.wstring()), Narrow(selected.Archive.wstring())); });
    });
    pack_stale_ = true;
    // Opening every archive is slow for large collections, so the gallery keeps the last scan until this finishes.
    scan_.Start([sources, archives = archives_](CancellationToken cancellation) {
        ScanResult result;
        for (const auto& path : sources) {
            cancellation.ThrowIfCancelled();
            try {
                const auto archive = archives->Get(path);
                for (const auto& entry : archive->Archive().Entries()) if (entry.FullPath.ends_with(".vmdl_c")) result.Models.push_back({path, entry.FullPath});
            } catch (const std::exception& error) { result.Errors.push_back(Narrow(path.wstring()) + ": " + error.what()); }
        }
        std::sort(result.Models.begin(), result.Models.end(), [](const auto& a, const auto& b) {
            if (a.Model != b.Model) return a.Model < b.Model;
            return a.Archive < b.Archive;
        });
        return result;
    });
}
namespace {
std::string SkinTitle(const LibraryModel& model) {
    const auto basename = model.Model.substr(model.Model.find_last_of('/') + 1);
    auto title = basename.ends_with(".vmdl_c") ? basename.substr(0, basename.size() - std::string(".vmdl_c").size()) : basename;
    std::replace(title.begin(), title.end(), '_', ' ');
    return title;
}
std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
std::string PackKey(const LibraryModel& model) { return Lower(Narrow(model.Archive.wstring())) + "|" + Lower(model.Model); }
// Table column ids, which are also the sort keys.
enum Column { ColumnName, ColumnModel, ColumnSource, ColumnPack, ColumnThumbnail };
void ModelTooltip(const LibraryModel& model, const std::string* error) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) || !ImGui::BeginTooltip()) return;
    ImGui::TextUnformatted(SkinTitle(model).c_str());
    ImGui::TextDisabled("%s", model.Model.c_str());
    ImGui::TextDisabled("%s", Narrow(model.Archive.wstring()).c_str());
    if (error != nullptr && !error->empty()) ImGui::TextColored(ui::kError, "%s", error->c_str());
    ImGui::TextDisabled("Double-click to inspect. Right-click for more.");
    ImGui::EndTooltip();
}
}

size_t LibraryPanel::ModelsIn(const std::filesystem::path& source) const {
    const auto found = source_counts_.find(source);
    return found == source_counts_.end() ? 0 : found->second;
}
void LibraryPanel::SortModels(int column, bool descending) {
    sort_column_ = column;
    sort_descending_ = descending;
    std::vector<std::string> keys(models_.size());
    for (size_t i = 0; i < models_.size(); ++i) {
        const auto& model = models_[i];
        keys[i] = Lower(column == ColumnSource ? Narrow(model.Archive.filename().wstring()) + "|" + model.Model
            : column == ColumnModel ? model.Model : SkinTitle(model) + "|" + model.Model);
    }
    order_.resize(models_.size());
    std::iota(order_.begin(), order_.end(), size_t{0});
    std::stable_sort(order_.begin(), order_.end(), [&](size_t a, size_t b) { return descending ? keys[b] < keys[a] : keys[a] < keys[b]; });
}
bool LibraryPanel::InPack(size_t index) {
    if (pack_stale_) {
        std::set<std::string> keys;
        for (const auto& item : selected_) keys.insert(PackKey(item));
        in_pack_.assign(models_.size(), 0);
        for (size_t i = 0; i < models_.size() && !keys.empty(); ++i) in_pack_[i] = keys.contains(PackKey(models_[i]));
        pack_stale_ = false;
    }
    return index < in_pack_.size() && in_pack_[index] != 0;
}
void LibraryPanel::SetInPack(size_t index, bool include) {
    const auto& model = models_.at(index);
    const auto found = std::find_if(selected_.begin(), selected_.end(), [&](const auto& item) { return Same(item, model); });
    if (include == (found != selected_.end())) return;
    if (include) selected_.push_back(model);
    else selected_.erase(found);
    if (!pack_stale_ && index < in_pack_.size()) in_pack_[index] = include;
    status_.clear();
}
void LibraryPanel::IncludeSource(const std::filesystem::path& source, bool include) {
    for (size_t i = 0; i < models_.size(); ++i) if (models_[i].Archive == source) SetInPack(i, include);
}
void LibraryPanel::ClearPack() {
    if (Busy()) return;
    selected_.clear();
    pack_stale_ = true;
    status_.clear();
}
void LibraryPanel::Reload(const std::vector<std::filesystem::path>& sources) {
    if (Busy()) return;
    archives_->Clear();
    InvalidateThumbnails();
    Refresh(sources);
}
void LibraryPanel::AddLibrary(void* owner, const std::function<void(const ModelLibrary&)>& load,
    const std::function<void(const std::filesystem::path&)>& merge) {
    if (Busy()) return;
    const auto path = Dialog(owner, FileKind::Library);
    if (path.empty()) return;
    try {
        source_filter_.clear();
        if (EqualsIgnoreCase(Narrow(path.extension().wstring()), ".db")) {
            merge(path);
            status_ = "Adding the shared library to yours...";
        } else {
            // Library lists from earlier releases name their VPKs, which are imported again.
            load(ModelLibrary::Load(path));
            status_ = "Restoring library...";
        }
    } catch (const std::exception& error) { status_ = error.what(); }
}
void LibraryPanel::ShareLibrary(void* owner) {
    if (!CanShare()) return;
    const auto path = Dialog(owner, FileKind::SharedLibrary);
    if (path.empty()) return;
    status_ = "Writing a copy of your library...";
    worker_ = std::async(std::launch::async, [store = *store_, path] {
        try { store.Export(path); return "Saved your whole library to " + Narrow(path.wstring()) + ". Others can add it with Add library."; }
        catch (const std::exception& error) { return std::string(error.what()); }
    });
}
void LibraryPanel::BuildPack(void* owner, const std::filesystem::path& game) {
    if (Busy() || selected_.empty()) return;
    const auto path = Dialog(owner, FileKind::Pack);
    if (path.empty()) return;
    cancellation_ = CancellationSource{};
    status_ = "Collecting dependencies and validating your pack...";
    worker_ = std::async(std::launch::async, [models = selected_, game, path, token = cancellation_.Token()] {
        try { const auto count = ExportModelLibrary(models, game, path, token);
            return "Exported " + std::to_string(count) + " files to " + Narrow(path.wstring()) + ". Test the pack in Deadlock.";
        } catch (const std::exception& error) { return std::string(error.what()); }
    });
}
void LibraryPanel::DrawModelMenu(size_t index, const std::vector<size_t>& shown, const std::function<void(const LibraryModel&)>& preview) {
    // A copy, because the menu's actions may change the pack.
    const LibraryModel model = models_.at(index);
    const bool in_pack = InPack(index);
    ImGui::TextDisabled("%s", SkinTitle(model).c_str());
    ImGui::Separator();
    if (ImGui::MenuItem("Inspect", "Enter")) preview(model);
    if (open_multi_) {
        const auto character = CharacterOfModel(model.Model);
        size_t character_models = 0;
        if (character) {
            for (const auto& other : models_) {
                const auto name = CharacterOfModel(other.Model);
                if (name && EqualsIgnoreCase(*name, *character)) ++character_models;
            }
        }
        if (ImGui::MenuItem("Multi view", nullptr, false, character.has_value())) open_multi_(model);
        if (character) {
            char tip[160];
            std::snprintf(tip, sizeof(tip), "Show %zu %s models together. Orbit, pan and zoom stay linked.", character_models, character->c_str());
            ImGui::SetItemTooltip("%s", tip);
        } else {
            ImGui::SetItemTooltip("Multi view groups models stored under models/heroes/<character>/.");
        }
    }
    if (ImGui::MenuItem(in_pack ? "Remove from pack" : "Include in pack", "Space")) SetInPack(index, !in_pack);
    if (ImGui::BeginMenu("Pack")) {
        if (ImGui::MenuItem(("Include the " + std::to_string(shown.size()) + " shown skins").c_str())) for (const size_t i : shown) SetInPack(i, true);
        if (ImGui::MenuItem(("Remove the " + std::to_string(shown.size()) + " shown skins").c_str())) for (const size_t i : shown) SetInPack(i, false);
        ImGui::Separator();
        if (ImGui::MenuItem("Include every skin of this source")) IncludeSource(model.Archive, true);
        if (ImGui::MenuItem("Remove every skin of this source")) IncludeSource(model.Archive, false);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    const bool filtered = source_filter_ == model.Archive;
    if (ImGui::MenuItem("Show only this source", nullptr, filtered)) source_filter_ = filtered ? std::filesystem::path{} : model.Archive;
    ImGui::Separator();
    ui::CopyMenuItem("Copy name", SkinTitle(model));
    ui::CopyMenuItem("Copy model path", model.Model);
    ui::CopyMenuItem("Copy source path", Narrow(model.Archive.wstring()));
    if (ImGui::MenuItem("Reveal source in Explorer")) ui::RevealInExplorer(model.Archive);
}
void LibraryPanel::ModelItemInput(size_t index, const std::vector<size_t>& shown, const std::function<void(const LibraryModel&)>& preview) {
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) current_ = index;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) preview(models_.at(index));
    if (ImGui::BeginPopupContextItem("model_menu")) {
        DrawModelMenu(index, shown, preview);
        ImGui::EndPopup();
    }
}
void LibraryPanel::DrawGrid(const std::vector<size_t>& shown, const std::filesystem::path& game, bool pause_loading,
    const std::function<void(const LibraryModel&)>& preview) {
    const auto& style = ImGui::GetStyle();
    const float scale = ImGui::GetFontSize() / 15.f;
    const float spacing = style.ItemSpacing.x;
    const float pad = std::round(4 * scale);
    const float available = ImGui::GetContentRegionAvail().x;
    const int columns = std::max(1, static_cast<int>((available + spacing) / (thumbnail_size_ * scale + 2 * pad + spacing)));
    // Cards stretch to fill the row, so the right edge lines up with the toolbar.
    const float card_width = std::floor((available - spacing * (columns - 1)) / columns);
    const float image_size = std::max(32.f, card_width - 2 * pad);
    const float line = ImGui::GetTextLineHeight();
    const float card_height = pad + image_size + pad + 2 * line + pad;
    const float row_height = card_height + style.ItemSpacing.y;
    const auto visible_rows = static_cast<size_t>(std::ceil(ImGui::GetWindowHeight() / row_height)) + 2;
    const auto cache_limit = std::clamp(visible_rows * static_cast<size_t>(columns), size_t{16}, size_t{256});
    auto* draw = ImGui::GetWindowDrawList();
    const ImU32 accent = ImGui::GetColorU32(ui::kAccent);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>((shown.size() + columns - 1) / columns), row_height);
    while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
        for (int column = 0; column < columns; ++column) {
            const size_t position = static_cast<size_t>(row) * columns + column;
            if (position >= shown.size()) break;
            const size_t index = shown[position];
            const auto& model = models_[index];
            bool checked = InPack(index);
            if (column > 0) ImGui::SameLine(0, spacing);
            ImGui::PushID(static_cast<int>(index));
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 max{min.x + card_width, min.y + card_height};
            ImGui::BeginGroup();
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("card", {card_width, card_height});
            const bool hovered = ImGui::IsItemHovered();
            ModelItemInput(index, shown, preview);
            const bool over = ImGui::IsMouseHoveringRect(min, max) && ImGui::IsWindowHovered();
            const bool current = current_ == index;
            draw->AddRectFilled(min, max, ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg, hovered ? 1.f : .55f), 3);
            if (checked) draw->AddRectFilled(min, max, ImGui::GetColorU32(ImVec4{ui::kAccent.x, ui::kAccent.y, ui::kAccent.z, .14f}), 3);
            draw->AddRect(min, max, checked || current ? accent : ImGui::GetColorU32(ImGuiCol_Border), 3, 0, current ? 2.f : 1.f);
            const std::string* error = DrawThumbnail(index, game, {min.x + pad, min.y + pad}, image_size, cache_limit, pause_loading);
            const float text_y = min.y + pad + image_size + pad;
            const std::string title = SkinTitle(model);
            const std::string source = Narrow(model.Archive.filename().wstring());
            ImGui::RenderTextEllipsis(draw, {min.x + pad, text_y}, {max.x - pad, text_y + line}, max.x - pad, max.x - pad, title.c_str(), nullptr, nullptr);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::RenderTextEllipsis(draw, {min.x + pad, text_y + line}, {max.x - pad, text_y + 2 * line}, max.x - pad, max.x - pad, source.c_str(), nullptr, nullptr);
            ImGui::PopStyleColor();
            ModelTooltip(model, error);
            if (checked || over) {
                ImGui::SetCursorScreenPos({min.x + pad + 2, min.y + pad + 2});
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {2, 2});
                if (ImGui::Checkbox("##pack", &checked)) SetInPack(index, checked);
                ImGui::PopStyleVar();
                ImGui::SetItemTooltip(checked ? "In the pack. Click to remove." : "Include in pack");
            }
            ImGui::EndGroup();
            ImGui::PopID();
        }
    }
}
void LibraryPanel::DrawList(const std::vector<size_t>& shown, const std::filesystem::path& game, bool pause_loading,
    const std::function<void(const LibraryModel&)>& preview) {
    const float line = ImGui::GetTextLineHeight();
    const float row_height = std::round(line * 1.6f);
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable
        | ImGuiTableFlags_Hideable | ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("skins", 5, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("##pack", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoResize
        | ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoReorder, ImGui::GetFrameHeight(), ColumnPack);
    ImGui::TableSetupColumn("##thumbnail", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoResize,
        row_height, ColumnThumbnail);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort, 1.f, ColumnName);
    ImGui::TableSetupColumn("Model path", ImGuiTableColumnFlags_WidthStretch, 1.8f, ColumnModel);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 1.f, ColumnSource);
    ImGui::TableHeadersRow();
    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs != nullptr && specs->SpecsDirty) {
        if (specs->SpecsCount > 0) SortModels(static_cast<int>(specs->Specs[0].ColumnUserID), specs->Specs[0].SortDirection == ImGuiSortDirection_Descending);
        specs->SpecsDirty = false;
    }
    const size_t cache_limit = std::clamp(static_cast<size_t>(ImGui::GetWindowHeight() / row_height) + 4, size_t{16}, size_t{256});
    const float text_offset = (row_height - line) / 2;
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, {0, .5f});
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(shown.size()), row_height);
    while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
        const size_t index = shown[static_cast<size_t>(row)];
        const auto& model = models_[index];
        ImGui::PushID(static_cast<int>(index));
        ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);
        ImGui::TableNextColumn();
        bool checked = InPack(index);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row_height - ImGui::GetFrameHeight()) / 2);
        if (ImGui::Checkbox("##pack", &checked)) SetInPack(index, checked);
        ImGui::SetItemTooltip("Include in pack");
        ImGui::TableNextColumn();
        const float image = row_height - 2;
        const std::string* error = DrawThumbnail(index, game, ImGui::GetCursorScreenPos(), image, cache_limit, pause_loading);
        ImGui::Dummy({image, image});
        ImGui::TableNextColumn();
        ImGui::Selectable((SkinTitle(model) + "##row").c_str(), current_ == index, ImGuiSelectableFlags_SpanAllColumns, {0, row_height});
        ModelItemInput(index, shown, preview);
        ModelTooltip(model, error);
        ImGui::TableNextColumn();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + text_offset);
        ImGui::TextDisabled("%s", model.Model.c_str());
        ImGui::TableNextColumn();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + text_offset);
        ImGui::TextUnformatted(Narrow(model.Archive.filename().wstring()).c_str());
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    if (ImGui::IsWindowFocused() && current_ && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) preview(models_.at(*current_));
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) SetInPack(*current_, !InPack(*current_));
    }
    ImGui::EndTable();
}
void LibraryPanel::Draw(void* owner, const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& game, bool repair_busy, bool pause_loading, bool pack_view, const std::function<void()>& add,
    const std::function<void(const ModelLibrary&)>&, const std::function<void(const std::filesystem::path&)>&,
    const std::function<void(const LibraryModel&)>& preview) {
    Poll();
    if (thumbnail_game_ != game) { InvalidateThumbnails(); thumbnail_game_ = game; }
    if (!Busy() && !repair_busy && sources_ != sources) Refresh(sources);
    const bool locked = Busy() || repair_busy;
    const auto& style = ImGui::GetStyle();
    const float scale = ImGui::GetFontSize() / 15.f;

    // Toolbar: everything on one row; the search box takes the width the other controls leave.
    if (ui::ToolButton("Refresh", "Reopen every source archive and load thumbnails again (F5)", !locked)) Reload(sources);
    ui::ToolSeparator();
    const float combo_width = 170 * scale;
    const float slider_width = 76 * scale;
    float reserved = combo_width + style.ItemSpacing.x * 6 + ImGui::CalcTextSize("GridList").x + style.FramePadding.x * 4 + 8;
    if (!pack_view) reserved += ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + ImGui::CalcTextSize("In pack").x;
    if (!list_view_) reserved += slider_width + style.ItemSpacing.x;
    ImGui::BeginDisabled(locked);
    ImGui::SetNextItemWidth(std::max(120.f, ImGui::GetContentRegionAvail().x - reserved));
    if (std::exchange(focus_search_, false)) ImGui::SetKeyboardFocusHere();
    ImGui::InputTextWithHint("##gallerysearch", "Search skins and sources (Ctrl+F)", filter_, sizeof(filter_));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(combo_width);
    const auto source_label = source_filter_.empty() ? std::string("All sources") : Narrow(source_filter_.filename().wstring());
    if (ImGui::BeginCombo("##sourcefilter", source_label.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable("All sources", source_filter_.empty())) source_filter_.clear();
        ImGui::Separator();
        for (const auto& source : sources) {
            const auto key = Narrow(source.wstring());
            ImGui::PushID(key.c_str());
            if (ImGui::Selectable(Narrow(source.filename().wstring()).c_str(), source == source_filter_)) source_filter_ = source;
            ImGui::SetItemTooltip("%s", key.c_str());
            ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::CalcTextSize("00000").x);
            ImGui::TextDisabled("%zu", ModelsIn(source));
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Show the skins of one source archive");
    if (!pack_view) {
        ImGui::SameLine();
        ImGui::Checkbox("In pack", &selected_only_);
        ImGui::SetItemTooltip("Show only skins included in the pack");
    }
    ImGui::EndDisabled();
    ui::ToolSeparator();
    const auto toggle = [](const char* label, bool active, const char* tooltip) {
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        const bool pressed = ImGui::SmallButton(label);
        if (active) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", tooltip);
        return pressed;
    };
    if (toggle("Grid", !list_view_, "Thumbnail grid")) list_view_ = false;
    ImGui::SameLine(0, 2);
    if (toggle("List", list_view_, "Sortable table with small thumbnails")) list_view_ = true;
    if (!list_view_) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(slider_width);
        ImGui::SliderFloat("##thumbnailsize", &thumbnail_size_, 80, 256, "%.0f px");
        ImGui::SetItemTooltip("Thumbnail size. Ctrl + mouse wheel over the gallery also changes it.");
    }

    if (pack_view) {
        ImGui::AlignTextToFramePadding();
        ImGui::Text(selected_.size() == 1 ? "%zu skin in pack" : "%zu skins in pack", selected_.size());
        ImGui::SameLine();
        if (ui::ToolButton("Build VPK...", "Export the pack as one VPK (Ctrl+B). Dependencies are included automatically and shared files are "
            "deduplicated; conflicting assets and missing dependencies block the export.", !locked && !selected_.empty())) BuildPack(owner, game);
        ImGui::SameLine();
        if (ui::ToolButton("Clear pack", "Remove every skin from the pack", !locked && !selected_.empty())) ClearPack();
        if (game.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ui::kWarning, "Connect Deadlock to resolve base-game assets.");
        }
    }
    if (Busy()) {
        if (ui::ToolButton("Cancel", "Stop the export")) CancelBuild();
        ImGui::SameLine();
    }
    if (Scanning()) {
        ImGui::TextDisabled("Reading %zu source archives...", sources_.size());
        if (!status_.empty()) ImGui::SameLine();
    }
    if (!status_.empty()) {
        ui::TextEllipsis(status_);
        if (ImGui::BeginPopupContextItem("status_menu")) {
            ui::CopyMenuItem("Copy message", status_);
            if (ImGui::MenuItem("Dismiss")) status_.clear();
            ImGui::EndPopup();
        }
    }

    ImGuiTextFilter filter(filter_);
    if (order_.size() != models_.size()) SortModels(sort_column_, sort_descending_);
    std::vector<size_t> shown;
    shown.reserve(order_.size());
    for (const size_t index : order_) {
        const auto& model = models_[index];
        if ((pack_view || selected_only_) && !InPack(index)) continue;
        if (!source_filter_.empty() && source_filter_ != model.Archive) continue;
        if (filter.IsActive() && !filter.PassFilter((model.Model + " " + Narrow(model.Archive.wstring())).c_str())) continue;
        shown.push_back(index);
    }
    shown_count_ = shown.size();

    ImGui::BeginDisabled(locked);
    if (!errors_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kError);
        const bool open = ImGui::TreeNode("errors", "%zu source archives could not be read", errors_.size());
        ImGui::PopStyleColor();
        if (open) {
            for (const auto& error : errors_) ImGui::TextWrapped("%s", error.c_str());
            ImGui::TreePop();
        }
    }
    // Until the scan finishes, every selected skin would look missing.
    std::vector<size_t> missing;
    for (size_t i = 0; i < selected_.size() && !Scanning(); ++i) {
        if (std::none_of(models_.begin(), models_.end(), [&](const auto& model) { return Same(model, selected_[i]); })) missing.push_back(i);
    }
    if (!missing.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::kWarning);
        const bool open = ImGui::TreeNode("missing", "%zu skins in the pack are missing from your library", missing.size());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        std::optional<size_t> remove;
        bool remove_all = ui::ToolButton("Remove all##missing", "Remove every missing skin from the pack");
        if (open) {
            for (const size_t i : missing) {
                ImGui::PushID(static_cast<int>(i));
                if (ui::ToolButton("Remove")) remove = i;
                ImGui::SameLine();
                ImGui::TextDisabled("%s  (%s)", selected_[i].Model.c_str(), Narrow(selected_[i].Archive.wstring()).c_str());
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        if (remove_all) {
            for (auto i = missing.rbegin(); i != missing.rend(); ++i) selected_.erase(selected_.begin() + static_cast<std::ptrdiff_t>(*i));
            pack_stale_ = true;
        } else if (remove) {
            selected_.erase(selected_.begin() + static_cast<std::ptrdiff_t>(*remove));
            pack_stale_ = true;
        }
    }
    if (shown.empty()) {
        ImGui::Spacing();
        if (models_.empty() && Scanning()) {
            ImGui::TextDisabled("Loading your collection...");
        } else {
            ImGui::TextUnformatted(models_.empty() ? "Your library has no mods yet." : pack_view && selected_.empty() ? "The pack is empty." : "No skins match.");
            ImGui::TextDisabled("%s", models_.empty() ? "Add or drop mod VPKs (Ctrl+O). Your source files stay available for future builds."
                : pack_view && selected_.empty() ? "Tick skins in the library, or right-click them and choose Include in pack."
                : "Change the search, or show every source.");
            if (models_.empty()) {
                if (ImGui::Button("Add mods...")) add();
            } else if (!pack_view || !selected_.empty()) {
                if (ImGui::Button("Clear filters")) { filter_[0] = '\0'; source_filter_.clear(); selected_only_ = false; }
            }
        }
    } else if (list_view_) {
        DrawList(shown, game, pause_loading, preview);
    } else {
        // Holding Ctrl turns the wheel into thumbnail zoom instead of scrolling.
        const ImGuiWindowFlags flags = ImGui::GetIO().KeyCtrl ? ImGuiWindowFlags_NoScrollWithMouse : ImGuiWindowFlags_None;
        ImGui::BeginChild("gallery", {0, 0}, ImGuiChildFlags_None, flags);
        DrawGrid(shown, game, pause_loading, preview);
        if (ImGui::IsWindowHovered() && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0) {
            thumbnail_size_ = std::clamp(thumbnail_size_ + ImGui::GetIO().MouseWheel * 16, 80.f, 256.f);
        }
        if (ImGui::IsWindowFocused() && current_ && !ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) preview(models_.at(*current_));
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) SetInPack(*current_, !InPack(*current_));
        }
        ImGui::EndChild();
    }
    ImGui::EndDisabled();
}
}
