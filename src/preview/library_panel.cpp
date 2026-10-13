#include "profile.hpp"
#include "library_panel.hpp"
#include "character_view.hpp"
#include "startup.hpp"
#include "strings.hpp"
#include "texture.hpp"
#include "theme.hpp"
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
#include <utility>
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
bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b) {
    return EqualsIgnoreCase(Narrow(a.wstring()), Narrow(b.wstring()));
}
bool IsModelPath(std::string_view path) {
    constexpr std::string_view suffix = ".vmdl_c";
    return path.size() >= suffix.size() && EqualsIgnoreCase(path.substr(path.size() - suffix.size()), suffix);
}
bool IsTexturePath(std::string_view path) {
    constexpr std::string_view suffix = ".vtex_c";
    return path.size() >= suffix.size() && EqualsIgnoreCase(path.substr(path.size() - suffix.size()), suffix);
}
bool ContainsIgnoreCase(std::string_view text, std::string_view needle) {
    if (needle.empty() || needle.size() > text.size()) return false;
    for (size_t i = 0; i + needle.size() <= text.size(); ++i) {
        if (EqualsIgnoreCase(text.substr(i, needle.size()), needle)) return true;
    }
    return false;
}
bool IsMaskTexture(std::string_view path) {
    // "rough" covers roughness. "_ao" is the ambient-occlusion atlas, which is not named ambientocclusion.
    constexpr std::string_view keys[] = {"normal", "tintmask", "ambientocclusion", "_ao", "rough", "sheen", "outline", "selfillum",
        "transmissive", "metalness", "opacity", "mask"};
    return std::any_of(std::begin(keys), std::end(keys), [&](std::string_view key) { return ContainsIgnoreCase(path, key); });
}
// The albedo. A larger roughness or occlusion map must not take the card instead.
bool IsAlbedoTexture(std::string_view path) {
    constexpr std::string_view keys[] = {"color", "albedo", "diffuse", "basecolor"};
    return std::any_of(std::begin(keys), std::end(keys), [&](std::string_view key) { return ContainsIgnoreCase(path, key); });
}
// A portrait or hero card is the picture someone expects. Other panorama files are often tiny icons.
bool IsHeroArt(std::string_view path) {
    return ContainsIgnoreCase(path, "portrait") || ContainsIgnoreCase(path, "hideout") || ContainsIgnoreCase(path, "_card");
}
bool IsPanorama(std::string_view path) {
    return ContainsIgnoreCase(path, "panorama/");
}
struct PreviewCandidate {
    std::string Path;
    uint64_t Bytes = 0;
    bool Hero = false;
    bool Panorama = false;
    bool Albedo = false;
    bool Mask = false;
};
// A 4x4 mask is the smallest file in most character mods, and a single-channel one displays as a red square.
std::string ChoosePreviewTexture(const std::vector<PreviewCandidate>& textures) {
    constexpr uint64_t kTiny = 8192;
    std::vector<const PreviewCandidate*> hero;
    std::vector<const PreviewCandidate*> panorama;
    std::vector<const PreviewCandidate*> albedo;
    std::vector<const PreviewCandidate*> plain;
    std::vector<const PreviewCandidate*> big;
    const auto best = [](const std::vector<const PreviewCandidate*>& pool, bool smallest) {
        const PreviewCandidate* chosen = nullptr;
        for (const PreviewCandidate* texture : pool) {
            const bool better = chosen == nullptr || (smallest ? texture->Bytes < chosen->Bytes : texture->Bytes > chosen->Bytes)
                || (texture->Bytes == chosen->Bytes && texture->Path < chosen->Path);
            if (better) chosen = texture;
        }
        return chosen == nullptr ? std::string{} : chosen->Path;
    };
    for (const PreviewCandidate& texture : textures) {
        if (texture.Mask) continue;
        if (texture.Hero) hero.push_back(&texture);
        else if (texture.Panorama) panorama.push_back(&texture);
        else if (texture.Albedo) albedo.push_back(&texture);
        else plain.push_back(&texture);
        if (!texture.Hero && !texture.Panorama && !texture.Albedo && texture.Bytes >= kTiny) big.push_back(&texture);
    }
    if (!hero.empty()) return best(hero, true);
    if (!panorama.empty()) return best(panorama, true);
    if (!albedo.empty()) return best(albedo, false);
    if (!big.empty()) return best(big, false);
    if (!plain.empty()) return best(plain, false);
    std::vector<const PreviewCandidate*> all;
    all.reserve(textures.size());
    for (const PreviewCandidate& texture : textures) all.push_back(&texture);
    return best(all, false);
}
std::string PackageTitle(const std::filesystem::path& archive) {
    const auto stem = archive.stem().wstring();
    return Narrow(stem.empty() ? archive.filename().wstring() : stem);
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
LibraryPanel::LibraryPanel(const VulkanDevice& device, std::shared_ptr<ArchiveCache> archives, const std::filesystem::path& data_root)
    : device_(device), archives_(std::move(archives)), legacy_thumbnails_(LegacyThumbnailFolder(data_root)) {
    try { store_.emplace(data_root); } catch (const std::exception&) {}
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
    // The window has already waited for the device, but this destructor also runs if setup throws.
    vkDeviceWaitIdle(device_.Device);
    thumbnails_.clear();
    retired_.clear();
    ready_.clear();
    renderer_.reset();
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
LibraryPanel::ThumbnailResult LibraryPanel::LoadTextureThumbnail(const ThumbnailJob& job) {
    ThumbnailResult result{job.Generation, job.Index, true};
    try {
        job.Cancellation.ThrowIfCancelled();
        const LibraryModel identity{job.Model.Archive, job.TexturePath};
        if (store_) {
            result.CacheKey = ThumbnailKey(store_->ContentId(identity.Archive), identity, {});
            try {
                if (const auto png = store_->Thumbnail(result.CacheKey))
                    if ((result.Pixels = DecodeThumbnail(*png))) return result;
            } catch (const std::exception&) {}
        }
        const auto archive = archives_->Get(job.Model.Archive);
        const auto lease = archive->Archive().LockSources();
        const auto bytes = archive->Read(job.TexturePath, job.Cancellation);
        if (!bytes) throw InvalidData("Texture is no longer present. Refresh the library.");
        auto image = DecodeTexture(*bytes, kThumbnailSize);
        if (image.Mips.empty()) throw InvalidData("Texture has no pixels.");
        // The card shows the mip DecodeTexture kept at 256 pixels on a side.
        image.Mips.resize(1);
        result.Texture = std::move(image);
    } catch (const std::exception& error) {
        result.Texture.reset();
        result.Error = error.what();
    }
    return result;
}
LibraryPanel::ThumbnailResult LibraryPanel::LoadThumbnail(const ThumbnailJob& job) {
    ZoneScopedN("Load thumbnail");
    if (!job.TexturePath.empty()) return LoadTextureThumbnail(job);
    ThumbnailResult result{job.Generation, job.Index};
    try {
        job.Cancellation.ThrowIfCancelled();
        if (store_) {
            result.CacheKey = ThumbnailKey(store_->ContentId(job.Model.Archive), job.Model, job.Game);
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
    for (auto& entry : thumbnails_) Retire(std::move(entry.second.Image));
    thumbnails_.clear();
    // The thumbnail renderer is only used by Capture, which waits for its own submit.
    // Dropping it here would idle every frame that is still presenting.
}
void LibraryPanel::Retire(std::unique_ptr<ThumbnailImage> image) {
    if (!image) return;
    // The swapchain can still sample the image until this many frames have been submitted after it.
    constexpr int kThumbnailRetireFrames = 6;
    retired_.push_back({std::move(image), kThumbnailRetireFrames});
}
void LibraryPanel::AgeRetired() {
    if (retired_.empty()) return;
    const int frame = ImGui::GetFrameCount();
    if (frame == retired_aged_frame_) return;
    retired_aged_frame_ = frame;
    std::erase_if(retired_, [](RetiredThumbnail& item) { return --item.FramesLeft <= 0; });
}
void LibraryPanel::Evict(std::map<Card, Thumbnail>::iterator thumbnail) {
    Retire(std::move(thumbnail->second.Image));
    if (thumbnail->second.Pending) {
        std::lock_guard lock(queue_mutex_);
        std::erase_if(jobs_, [&](const ThumbnailJob& job) {
            return !job.Save && job.Package == thumbnail->first.Package && job.Index == thumbnail->first.Index;
        });
    }
    thumbnails_.erase(thumbnail);
}
ThumbnailPixels LibraryPanel::RenderThumbnail(const PreparedModel& model) {
    ZoneScopedN("Render thumbnail");
    if (!renderer_) renderer_ = std::make_unique<ModelRenderer>(device_);
    renderer_->SetScene(model.Scene);
    if (model.Materials) renderer_->SetMaterials(*model.Materials);
    renderer_->Resize(kThumbnailSize, kThumbnailSize);
    const auto visible = DefaultVisibility(model.Scene, 0);
    RenderOptions options;
    options.Background = theme::kClearRgb;
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
    if (ready_.empty()) return;
    ZoneScopedN("Thumbnail upload");
    // One image per frame. Its submit waits for the queue, including the frame that is still presenting.
    while (!ready_.empty()) {
        ThumbnailResult result = std::move(ready_.front());
        ready_.pop_front();
        const auto found = thumbnails_.find(Card{result.Package, result.Index});
        if (result.Generation != generation_ || found == thumbnails_.end() || !found->second.Pending) continue;
        Thumbnail& thumbnail = found->second;
        thumbnail.Pending = false;
        try {
            if (result.Texture) {
                thumbnail.Image = std::make_unique<ThumbnailImage>(device_, *result.Texture);
                return;
            }
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
        return;
    }
}
const std::string* LibraryPanel::DrawThumbnail(const Card& card, const std::filesystem::path& game, ImVec2 origin, float size,
    size_t cache_limit, bool pause_loading) {
    const bool texture_card = card.Package && !payloads_.at(card.Index).Preview.empty();
    const bool no_preview = card.Package && !texture_card;
    auto found = thumbnails_.find(card);
    if (found == thumbnails_.end() && !pause_loading && !no_preview) {
        // Never evict an image referenced by this frame's ImGui draw list.
        while (thumbnails_.size() >= cache_limit) {
            const auto oldest = std::min_element(thumbnails_.begin(), thumbnails_.end(), [](const auto& a, const auto& b) {
                return a.second.LastFrame < b.second.LastFrame;
            });
            if (oldest->second.LastFrame < ImGui::GetFrameCount()) Evict(oldest);
            else break;
        }
        if (thumbnails_.size() < cache_limit) {
            found = thumbnails_.emplace(card, Thumbnail{}).first;
            {
                std::lock_guard lock(queue_mutex_);
                ThumbnailJob job{generation_, card.Index, card.Package ? LibraryModel{payloads_.at(card.Index).Archive, payloads_.at(card.Index).Preview} : models_.at(card.Index),
                    game, thumbnail_cancellation_.Token()};
                if (card.Package) {
                    job.Package = true;
                    job.TexturePath = payloads_.at(card.Index).Preview;
                }
                jobs_.push_back(std::move(job));
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
    const bool loading = !no_preview && (found == thumbnails_.end() || found->second.Pending);
    draw->AddRectFilled(origin, corner, ImGui::GetColorU32(ImGuiCol_FrameBg), 2);
    const char* label = loading ? "Loading..." : "No preview";
    const auto text_size = ImGui::CalcTextSize(label);
    if (text_size.x < size - 4) {
        draw->AddText({origin.x + (size - text_size.x) / 2, origin.y + (size - text_size.y) / 2}, ImGui::GetColorU32(ImGuiCol_TextDisabled), label);
    }
    return loading || found == thumbnails_.end() ? nullptr : &found->second.Error;
}
void LibraryPanel::NoteStatus(std::string text, bool error) {
    status_ = std::move(text);
    status_error_ = error;
}

std::optional<AssembledPack> LibraryPanel::TakeExportNotice() {
    return std::exchange(export_notice_, std::nullopt);
}

void LibraryPanel::Poll() {
    ZoneScopedN("Library poll");
    if (worker_.valid() && worker_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        PanelJob job = worker_.get();
        if (job.Output) {
            export_notice_ = AssembledPack{*job.Output, job.Status};
        }
        NoteStatus(std::move(job.Status), job.Error);
    }
    try {
        if (auto scan = scan_.Take()) {
            ZoneScopedN("Apply library scan");
            InvalidateThumbnails();
            models_ = std::move(scan->Models);
            payloads_ = std::move(scan->Packages);
            errors_ = std::move(scan->Errors);
            current_.reset();
            source_counts_.clear();
            for (const auto& model : models_) ++source_counts_[model.Archive];
            SortModels(sort_column_, sort_descending_);
            pack_stale_ = true;
            missing_stale_ = true;
            RebuildSourceInfo();
        }
    } catch (const std::exception& error) {
        errors_ = {error.what()};
    }
    AgeRetired();
    const int frame = ImGui::GetFrameCount();
    if (frame != uploaded_frame_) {
        uploaded_frame_ = frame;
        UploadThumbnails();
    }
}
void LibraryPanel::Restore(const ModelLibrary& library) {
    selected_ = library.Selected;
    package_selection_ = library.Packages;
    pack_stale_ = true;
    Refresh(library.Sources);
    source_filter_.clear();
    NoteStatus({}, false);
}
ModelLibrary LibraryPanel::Snapshot(const std::vector<std::filesystem::path>& sources, const std::filesystem::path& game) const {
    ModelLibrary result{sources, {}, game};
    const auto listed = [&](const std::filesystem::path& path) {
        return std::any_of(sources.begin(), sources.end(), [&](const auto& source) { return SamePath(source, path); });
    };
    for (const auto& selected : selected_) if (listed(selected.Archive)) result.Selected.push_back(selected);
    for (const auto& package : package_selection_) if (listed(package)) result.Packages.push_back(package);
    return result;
}
void LibraryPanel::Refresh(const std::vector<std::filesystem::path>& sources) {
    sources_ = sources;
    if (!source_filter_.empty() && std::find(sources.begin(), sources.end(), source_filter_) == sources.end()) source_filter_.clear();
    const auto listed = [&](const std::filesystem::path& path) {
        return std::none_of(sources.begin(), sources.end(), [&](const auto& source) { return SamePath(source, path); });
    };
    std::erase_if(selected_, [&](const auto& selected) { return listed(selected.Archive); });
    std::erase_if(package_selection_, [&](const auto& package) { return listed(package); });
    pack_stale_ = true;
    Changed();
    RebuildSourceInfo();
    // Opening every archive is slow for large collections, so the gallery keeps the last scan until this finishes.
    scan_.Start([sources, archives = archives_](CancellationToken cancellation) {
        ZoneScopedN("Scan source archives");
        ScanResult result;
        for (const auto& path : sources) {
            cancellation.ThrowIfCancelled();
            try {
                const auto archive = archives->Get(path);
                PackagePayload payload{path};
                std::vector<PreviewCandidate> previews;
                for (const auto& entry : archive->Archive().Entries()) {
                    const auto normalized = VpkArchive::Normalize(entry.FullPath);
                    if (IsModelPath(normalized)) {
                        // Keep the archive's own spelling for skins that already used it.
                        result.Models.push_back({path, entry.FullPath.ends_with(".vmdl_c") ? entry.FullPath : normalized});
                    } else {
                        payload.Paths.push_back(normalized);
                        if (IsTexturePath(normalized)) {
                            previews.push_back({normalized, static_cast<uint64_t>(entry.PreloadBytes) + entry.Length, IsHeroArt(normalized),
                                IsPanorama(normalized), IsAlbedoTexture(normalized), IsMaskTexture(normalized)});
                        }
                    }
                }
                payload.Preview = ChoosePreviewTexture(previews);
                if (!payload.Paths.empty()) {
                    std::sort(payload.Paths.begin(), payload.Paths.end());
                    result.Packages.push_back(std::move(payload));
                }
            } catch (const std::exception& error) { result.Errors.push_back(Narrow(path.wstring()) + ": " + error.what()); }
        }
        std::sort(result.Models.begin(), result.Models.end(), [](const auto& a, const auto& b) {
            if (a.Model != b.Model) return a.Model < b.Model;
            return a.Archive < b.Archive;
        });
        std::sort(result.Packages.begin(), result.Packages.end(), [](const auto& a, const auto& b) {
            if (a.Archive.filename() != b.Archive.filename()) return a.Archive.filename() < b.Archive.filename();
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
size_t LibraryPanel::PackageFiles(const std::filesystem::path& source) const {
    for (const auto& package : payloads_) if (SamePath(package.Archive, source)) return package.Paths.size();
    return 0;
}
size_t LibraryPanel::SourceCount(const std::filesystem::path& source) const {
    const size_t skins = ModelsIn(source);
    return skins > 0 ? skins : PackageFiles(source);
}
size_t LibraryPanel::ListedCount() const {
    size_t total = 0;
    for (const auto& info : source_info_) total += info.Skins > 0 ? info.Skins : info.Files;
    return total;
}
const LibraryPanel::SourceInfo* LibraryPanel::FindSource(const std::filesystem::path& path) const {
    for (const auto& info : source_info_) if (SamePath(info.Path, path)) return &info;
    return nullptr;
}
bool LibraryPanel::PackageInPack(const std::filesystem::path& source) const {
    return std::any_of(package_selection_.begin(), package_selection_.end(), [&](const auto& path) { return SamePath(path, source); });
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
    listing_stale_ = true;
}
void LibraryPanel::EnsureListing() {
    if (order_.size() != models_.size()) SortModels(sort_column_, sort_descending_);
    if (!listing_stale_) return;
    ZoneScopedN("Gallery sort");
    listed_.clear();
    listed_.reserve(order_.size() + payloads_.size());
    for (const size_t index : order_) {
        const auto& model = models_[index];
        ListedCard card;
        card.Id = {false, index};
        const auto file = Narrow(model.Archive.filename().wstring());
        const auto title = SkinTitle(model);
        card.SortKey = Lower(sort_column_ == ColumnSource ? file + "|" + model.Model
            : sort_column_ == ColumnModel ? model.Model : title + "|" + model.Model);
        card.FilterText = model.Model + " " + Narrow(model.Archive.wstring());
        listed_.push_back(std::move(card));
    }
    for (size_t index = 0; index < payloads_.size(); ++index) {
        const auto& package = payloads_[index];
        ListedCard card;
        card.Id = {true, index};
        const auto file = Narrow(package.Archive.filename().wstring());
        const auto title = PackageTitle(package.Archive);
        const auto subtitle = PackageSubtitle(package.Paths);
        card.SortKey = Lower(sort_column_ == ColumnSource ? file : sort_column_ == ColumnModel ? subtitle : title + "|" + file);
        card.FilterText = title + " " + file + " " + subtitle;
        listed_.push_back(std::move(card));
    }
    std::stable_sort(listed_.begin(), listed_.end(), [descending = sort_descending_](const ListedCard& a, const ListedCard& b) {
        return descending ? b.SortKey < a.SortKey : a.SortKey < b.SortKey;
    });
    listing_stale_ = false;
}
void LibraryPanel::RebuildSourceInfo() {
    source_info_.clear();
    source_info_.reserve(sources_.size());
    for (const auto& source : sources_) {
        SourceInfo info;
        info.Path = source;
        info.Name = Narrow(source.filename().wstring());
        info.Full = Narrow(source.wstring());
        info.Skins = ModelsIn(source);
        for (const auto& package : payloads_) {
            if (!SamePath(package.Archive, source)) continue;
            info.Files = package.Paths.size();
            break;
        }
        info.Tip = info.Full;
        if (info.Skins > 0) {
            info.Tip += "\n" + std::to_string(info.Skins) + (info.Skins == 1 ? " skin" : " skins");
            if (info.Files > 0) info.Tip += ", " + std::to_string(info.Files) + (info.Files == 1 ? " other file" : " other files");
            info.Tip += "\nClick to show its skins. Right-click for more.";
        } else {
            info.Tip += "\n" + std::to_string(info.Files) + (info.Files == 1 ? " file" : " files");
            info.Tip += "\nClick to show it. Right-click for more.";
        }
        source_info_.push_back(std::move(info));
    }
}
void LibraryPanel::RebuildMissing() {
    missing_.clear();
    std::set<std::string> present;
    for (const auto& model : models_) present.insert(PackKey(model));
    for (size_t i = 0; i < selected_.size(); ++i) {
        if (!present.contains(PackKey(selected_[i]))) missing_.push_back(i);
    }
    missing_stale_ = false;
}
void LibraryPanel::Changed() {
    ++revision_;
    missing_stale_ = true;
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
    NoteStatus({}, false);
    Changed();
}
void LibraryPanel::IncludeSource(const std::filesystem::path& source, bool include) {
    for (size_t i = 0; i < models_.size(); ++i) if (SamePath(models_[i].Archive, source)) SetInPack(i, include);
}
void LibraryPanel::IncludePackage(const std::filesystem::path& source, bool include) {
    const auto found = std::find_if(package_selection_.begin(), package_selection_.end(), [&](const auto& path) { return SamePath(path, source); });
    if (include == (found != package_selection_.end())) return;
    if (include) package_selection_.push_back(source);
    else package_selection_.erase(found);
    NoteStatus({}, false);
    Changed();
}
void LibraryPanel::ClearPack() {
    if (Busy()) return;
    if (selected_.empty() && package_selection_.empty()) return;
    selected_.clear();
    package_selection_.clear();
    pack_stale_ = true;
    NoteStatus({}, false);
    Changed();
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
            NoteStatus("Adding the shared library to yours...", false);
        } else {
            // Library lists from earlier releases name their VPKs, which are imported again.
            load(ModelLibrary::Load(path));
            NoteStatus("Restoring library...", false);
        }
    } catch (const std::exception& error) { NoteStatus(error.what(), true); }
}
void LibraryPanel::ShareLibrary(void* owner) {
    if (!CanShare()) return;
    const auto path = Dialog(owner, FileKind::SharedLibrary);
    if (path.empty()) return;
    NoteStatus("Writing a copy of your library...", false);
    worker_ = std::async(std::launch::async, [store = *store_, path] {
        try {
            store.Export(path);
            return PanelJob{"Saved your whole library to " + Narrow(path.wstring()) + ". Others can add it with Add library.", false, {}};
        } catch (const std::exception& error) {
            return PanelJob{error.what(), true, {}};
        }
    });
}
void LibraryPanel::BuildPack(void* owner, const std::filesystem::path& game) {
    if (Busy() || (selected_.empty() && package_selection_.empty())) return;
    const auto path = Dialog(owner, FileKind::Pack);
    if (path.empty()) return;
    cancellation_ = CancellationSource{};
    NoteStatus("Collecting files and validating your pack...", false);
    worker_ = std::async(std::launch::async, [models = selected_, packages = package_selection_, game, path, token = cancellation_.Token()] {
        try {
            const auto count = ExportModelLibrary(models, game, path, token, packages);
            return PanelJob{ExportResultSummary(count, path), false, path};
        } catch (const std::exception& error) {
            return PanelJob{error.what(), true, {}};
        }
    });
}
void LibraryPanel::OpenFileList(const Card& card) {
    const auto& package = payloads_.at(card.Index);
    file_list_ = package.Paths;
    file_list_title_ = PackageTitle(package.Archive) + "  —  " + PackageSubtitle(package.Paths);
    open_file_list_ = true;
}
void LibraryPanel::Activate(const Card& card, const std::function<void(const LibraryModel&)>& preview) {
    if (card.Package) OpenFileList(card);
    else preview(models_.at(card.Index));
}
void LibraryPanel::Toggle(const Card& card) {
    if (card.Package) IncludePackage(payloads_.at(card.Index).Archive, !PackageInPack(payloads_.at(card.Index).Archive));
    else SetInPack(card.Index, !InPack(card.Index));
}
void LibraryPanel::DrawPackageMenu(const Card& card) {
    const auto& package = payloads_.at(card.Index);
    const bool in_pack = PackageInPack(package.Archive);
    const std::string title = PackageTitle(package.Archive);
    ImGui::TextDisabled("%s", title.c_str());
    ImGui::Separator();
    if (ImGui::MenuItem("Show files", "Enter")) OpenFileList(card);
    if (ImGui::MenuItem(in_pack ? "Remove from pack" : "Include in pack", "Space")) IncludePackage(package.Archive, !in_pack);
    ImGui::Separator();
    const bool filtered = source_filter_ == package.Archive;
    if (ImGui::MenuItem("Show only this source", nullptr, filtered)) source_filter_ = filtered ? std::filesystem::path{} : package.Archive;
    ImGui::Separator();
    ui::CopyMenuItem("Copy name", title);
    ui::CopyMenuItem("Copy source path", Narrow(package.Archive.wstring()));
    if (ImGui::MenuItem("Reveal source in Explorer")) ui::RevealInExplorer(package.Archive);
}
void LibraryPanel::DrawModelMenu(const Card& card, const std::vector<Card>& shown, const std::function<void(const LibraryModel&)>& preview,
    const std::function<void(const LibraryModel&)>& analyze, bool can_analyze) {
    if (card.Package) { DrawPackageMenu(card); return; }
    const size_t index = card.Index;
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
    if (analyze) {
        if (ImGui::MenuItem("Analyze this model", nullptr, false, can_analyze)) analyze(model);
        ImGui::SetItemTooltip(can_analyze
            ? "Check this model and its animation skeleton against the installed game. Other models in the archive are not checked."
            : "Connect Deadlock before checking a model.");
    }
    if (ImGui::MenuItem(in_pack ? "Remove from pack" : "Include in pack", "Space")) SetInPack(index, !in_pack);
    if (ImGui::BeginMenu("Pack")) {
        size_t skin_count = 0;
        for (const auto& item : shown) if (!item.Package) ++skin_count;
        if (ImGui::MenuItem(("Include the " + std::to_string(skin_count) + " shown skins").c_str())) for (const auto& item : shown) if (!item.Package) SetInPack(item.Index, true);
        if (ImGui::MenuItem(("Remove the " + std::to_string(skin_count) + " shown skins").c_str())) for (const auto& item : shown) if (!item.Package) SetInPack(item.Index, false);
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
void LibraryPanel::CardInput(const Card& card, const std::vector<Card>& shown, const std::function<void(const LibraryModel&)>& preview,
    const std::function<void(const LibraryModel&)>& analyze, bool can_analyze) {
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) current_ = card;
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) Activate(card, preview);
    if (ImGui::BeginPopupContextItem("model_menu")) {
        DrawModelMenu(card, shown, preview, analyze, can_analyze);
        ImGui::EndPopup();
    }
}
void LibraryPanel::DrawGrid(const std::vector<Card>& shown, const std::filesystem::path& game, bool pause_loading,
    const std::function<void(const LibraryModel&)>& preview, const std::function<void(const LibraryModel&)>& analyze, bool can_analyze) {
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
            const Card card = shown[position];
            const bool package = card.Package;
            const std::string title = package ? PackageTitle(payloads_[card.Index].Archive) : SkinTitle(models_[card.Index]);
            const std::string subtitle = package ? PackageSubtitle(payloads_[card.Index].Paths) : Narrow(models_[card.Index].Archive.filename().wstring());
            bool checked = package ? PackageInPack(payloads_[card.Index].Archive) : InPack(card.Index);
            if (column > 0) ImGui::SameLine(0, spacing);
            ImGui::PushID(package ? "package" : "skin");
            ImGui::PushID(static_cast<int>(card.Index));
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 max{min.x + card_width, min.y + card_height};
            ImGui::BeginGroup();
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("card", {card_width, card_height});
            const bool hovered = ImGui::IsItemHovered();
            CardInput(card, shown, preview, analyze, can_analyze);
            const bool over = ImGui::IsMouseHoveringRect(min, max) && ImGui::IsWindowHovered();
            const bool current = current_ == card;
            draw->AddRectFilled(min, max, ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg, hovered ? 1.f : .55f), 3);
            if (checked) draw->AddRectFilled(min, max, ImGui::GetColorU32(ImVec4{ui::kAccent.x, ui::kAccent.y, ui::kAccent.z, .14f}), 3);
            draw->AddRect(min, max, checked || current ? accent : ImGui::GetColorU32(ImGuiCol_Border), 3, 0, current ? 2.f : 1.f);
            const std::string* error = DrawThumbnail(card, game, {min.x + pad, min.y + pad}, image_size, cache_limit, pause_loading);
            const float text_y = min.y + pad + image_size + pad;
            ImGui::RenderTextEllipsis(draw, {min.x + pad, text_y}, {max.x - pad, text_y + line}, max.x - pad, max.x - pad, title.c_str(), nullptr, nullptr);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::RenderTextEllipsis(draw, {min.x + pad, text_y + line}, {max.x - pad, text_y + 2 * line}, max.x - pad, max.x - pad, subtitle.c_str(), nullptr, nullptr);
            ImGui::PopStyleColor();
            if (!package) ModelTooltip(models_[card.Index], error);
            else if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && ImGui::BeginTooltip()) {
                ImGui::TextUnformatted(title.c_str());
                ImGui::TextDisabled("%s", subtitle.c_str());
                ImGui::TextDisabled("%s", Narrow(payloads_[card.Index].Archive.wstring()).c_str());
                if (error != nullptr && !error->empty()) ImGui::TextColored(ui::kError, "%s", error->c_str());
                ImGui::TextDisabled("Double-click to list files. Right-click for more.");
                ImGui::EndTooltip();
            }
            if (checked || over) {
                ImGui::SetCursorScreenPos({min.x + pad + 2, min.y + pad + 2});
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {2, 2});
                if (ImGui::Checkbox("##pack", &checked)) Toggle(card);
                ImGui::PopStyleVar();
                ImGui::SetItemTooltip(checked ? "In the pack. Click to remove." : "Include in pack");
            }
            ImGui::EndGroup();
            ImGui::PopID();
            ImGui::PopID();
        }
    }
}
void LibraryPanel::DrawList(const std::vector<Card>& shown, const std::filesystem::path& game, bool pause_loading,
    const std::function<void(const LibraryModel&)>& preview, const std::function<void(const LibraryModel&)>& analyze, bool can_analyze) {
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
        const Card card = shown[static_cast<size_t>(row)];
        const bool package = card.Package;
        const std::string title = package ? PackageTitle(payloads_[card.Index].Archive) : SkinTitle(models_[card.Index]);
        const std::string detail = package ? PackageSubtitle(payloads_[card.Index].Paths) : models_[card.Index].Model;
        const auto& archive = package ? payloads_[card.Index].Archive : models_[card.Index].Archive;
        ImGui::PushID(package ? "package" : "skin");
        ImGui::PushID(static_cast<int>(card.Index));
        ImGui::TableNextRow(ImGuiTableRowFlags_None, row_height);
        ImGui::TableNextColumn();
        bool checked = package ? PackageInPack(archive) : InPack(card.Index);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row_height - ImGui::GetFrameHeight()) / 2);
        if (ImGui::Checkbox("##pack", &checked)) Toggle(card);
        ImGui::SetItemTooltip("Include in pack");
        ImGui::TableNextColumn();
        const float image = row_height - 2;
        const std::string* error = DrawThumbnail(card, game, ImGui::GetCursorScreenPos(), image, cache_limit, pause_loading);
        ImGui::Dummy({image, image});
        ImGui::TableNextColumn();
        ImGui::Selectable((title + "##row").c_str(), current_ == card, ImGuiSelectableFlags_SpanAllColumns, {0, row_height});
        CardInput(card, shown, preview, analyze, can_analyze);
        if (!package) ModelTooltip(models_[card.Index], error);
        ImGui::TableNextColumn();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + text_offset);
        ImGui::TextDisabled("%s", detail.c_str());
        ImGui::TableNextColumn();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + text_offset);
        ImGui::TextUnformatted(Narrow(archive.filename().wstring()).c_str());
        ImGui::PopID();
        ImGui::PopID();
    }
    ImGui::PopStyleVar();
    if (ImGui::IsWindowFocused() && current_ && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) Activate(*current_, preview);
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) Toggle(*current_);
    }
    ImGui::EndTable();
}
void LibraryPanel::Draw(void* owner, const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& game, bool repair_busy, bool pause_loading, bool pack_view, const std::function<void()>& add,
    const std::function<void(const ModelLibrary&)>&, const std::function<void(const std::filesystem::path&)>&,
    const std::function<void(const LibraryModel&)>& preview, const std::function<void(const LibraryModel&)>& analyze,
    const std::function<void()>& connect_game, const std::function<void()>& open_quick_start, bool collection_ready) {
    ZoneScopedN("Gallery");
    Poll();
    if (thumbnail_game_ != game) { InvalidateThumbnails(); thumbnail_game_ = game; }
    if (!Busy() && !repair_busy && sources_ != sources) Refresh(sources);
    const bool locked = Busy() || repair_busy;
    const bool can_analyze = !locked && !game.empty();
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
    ImGui::InputTextWithHint("##gallerysearch", "Search skins, packages and sources (Ctrl+F)", filter_, sizeof(filter_));
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
            ImGui::TextDisabled("%zu", SourceCount(source));
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Show one source");
    if (!pack_view) {
        ImGui::SameLine();
        ImGui::Checkbox("In pack", &selected_only_);
        ImGui::SetItemTooltip("Show only skins and packages included in the pack");
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
        const size_t skins = selected_.size();
        const size_t packages = package_selection_.size();
        std::string pack_label;
        if (packages == 0) pack_label = (skins == 1 ? "1 skin in pack" : std::to_string(skins) + " skins in pack");
        else if (skins == 0) pack_label = (packages == 1 ? "1 package in pack" : std::to_string(packages) + " packages in pack");
        else pack_label = std::to_string(skins) + (skins == 1 ? " skin and " : " skins and ") + std::to_string(packages) + (packages == 1 ? " package in pack" : " packages in pack");
        ImGui::TextUnformatted(pack_label.c_str());
        ImGui::SameLine();
        const bool pack_ready = skins > 0 || packages > 0;
        theme::PushPrimaryButton();
        if (ui::ToolButton("Build VPK...", "Export the pack as one VPK (Ctrl+B). Skin dependencies are included automatically. "
            "A package copies its other files as stored. Shared files are deduplicated; conflicting files block the export.",
            !locked && pack_ready)) BuildPack(owner, game);
        theme::PopPrimaryButton();
        if (build_as_mod_) {
            ImGui::SameLine();
            if (ui::ToolButton("Build as mod...", "Build the pack into your library as a named mod, which the Mods page can enable "
                "and apply to the game. Building again under the same name replaces it.", !locked && pack_ready)) build_as_mod_();
        }
        ImGui::SameLine();
        if (ui::ToolButton("Clear pack", "Remove every skin and package from the pack", !locked && pack_ready)) ClearPack();
        if (game.empty() && skins > 0) {
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
        ImVec4 status_color = status_error_ ? ui::kError : (Busy() ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : ui::kGood);
        ui::TextEllipsis(status_, 0, &status_color);
        if (ImGui::BeginPopupContextItem("status_menu")) {
            ui::CopyMenuItem("Copy message", status_);
            if (ImGui::MenuItem("Dismiss")) NoteStatus({}, false);
            ImGui::EndPopup();
        }
    }

    ImGuiTextFilter filter(filter_);
    EnsureListing();
    std::vector<Card> shown;
    shown.reserve(listed_.size());
    for (const ListedCard& card : listed_) {
        if (card.Id.Package) {
            const auto& package = payloads_[card.Id.Index];
            if ((pack_view || selected_only_) && !PackageInPack(package.Archive)) continue;
            if (!source_filter_.empty() && source_filter_ != package.Archive) continue;
        } else {
            const auto& model = models_[card.Id.Index];
            if ((pack_view || selected_only_) && !InPack(card.Id.Index)) continue;
            if (!source_filter_.empty() && source_filter_ != model.Archive) continue;
        }
        if (filter.IsActive() && !filter.PassFilter(card.FilterText.c_str())) continue;
        shown.push_back(card.Id);
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
    if (!Scanning() && missing_stale_) RebuildMissing();
    static const std::vector<size_t> kNoMissing;
    const std::vector<size_t>& missing = Scanning() ? kNoMissing : missing_;
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
            Changed();
        } else if (remove) {
            selected_.erase(selected_.begin() + static_cast<std::ptrdiff_t>(*remove));
            pack_stale_ = true;
            Changed();
        }
    }
    const bool no_sources = sources.empty();
    const bool have_items = !models_.empty() || !payloads_.empty();
    const bool pack_empty = selected_.empty() && package_selection_.empty();
    if (shown.empty()) {
        ImGui::Spacing();
        if (!have_items && Scanning()) {
            ImGui::TextDisabled("Loading your collection...");
        } else if (no_sources && !collection_ready) {
            ImGui::TextDisabled("Opening your library...");
        } else if (no_sources) {
            ImGui::TextUnformatted("Your library has no mods yet.");
            ImGui::PushTextWrapPos(0);
            ImGui::TextWrapped("%s", GameResourceSummary().c_str());
            ImGui::TextWrapped("Building a pack does not install it. Open quick start for how to install or remove one.");
            ImGui::PopTextWrapPos();
            const bool can_add = static_cast<bool>(add) && !locked;
            theme::PushPrimaryButton();
            if (ImGui::Button("Connect game") && connect_game) connect_game();
            theme::PopPrimaryButton();
            ImGui::SameLine();
            ImGui::BeginDisabled(!can_add);
            theme::PushPrimaryButton();
            if (ImGui::Button("Add mods") && can_add) add();
            theme::PopPrimaryButton();
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Open quick start") && open_quick_start) open_quick_start();
            ImGui::TextDisabled("You can also drop mod VPKs on the window. Your source files stay available for later builds.");
            ImGui::TextDisabled("Get mods (Ctrl+8) downloads mods from GameBanana. Mods (Ctrl+7) turns them on in the game.");
        } else if (pack_view && pack_empty) {
            ImGui::TextUnformatted("The pack is empty.");
            ImGui::TextDisabled("Tick skins in the library, include a mod's other files, or right-click them and choose Include in pack.");
        } else {
            ImGui::TextUnformatted("No mods match.");
            ImGui::TextDisabled("Change the search, or show every source.");
            if (ImGui::Button("Clear filters")) { filter_[0] = '\0'; source_filter_.clear(); selected_only_ = false; }
        }
    } else if (list_view_) {
        DrawList(shown, game, pause_loading, preview, analyze, can_analyze);
    } else {
        // Holding Ctrl turns the wheel into thumbnail zoom instead of scrolling.
        const ImGuiWindowFlags flags = ImGui::GetIO().KeyCtrl ? ImGuiWindowFlags_NoScrollWithMouse : ImGuiWindowFlags_None;
        ImGui::BeginChild("gallery", {0, 0}, ImGuiChildFlags_None, flags);
        DrawGrid(shown, game, pause_loading, preview, analyze, can_analyze);
        if (ImGui::IsWindowHovered() && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0) {
            thumbnail_size_ = std::clamp(thumbnail_size_ + ImGui::GetIO().MouseWheel * 16, 80.f, 256.f);
        }
        if (ImGui::IsWindowFocused() && current_ && !ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Enter, false)) Activate(*current_, preview);
            if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) Toggle(*current_);
        }
        ImGui::EndChild();
    }
    ImGui::EndDisabled();
    if (std::exchange(open_file_list_, false)) ImGui::OpenPopup("Package files");
    ImGui::SetNextWindowSize({ImGui::GetFontSize() * 36, ImGui::GetFontSize() * 24}, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Package files", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted(file_list_title_.c_str());
        ImGui::BeginChild("package_paths", {0, ImGui::GetFontSize() * 16}, ImGuiChildFlags_Borders);
        for (const auto& path : file_list_) ImGui::TextUnformatted(path.c_str());
        ImGui::EndChild();
        if (ImGui::Button("Close", {-1, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
}
