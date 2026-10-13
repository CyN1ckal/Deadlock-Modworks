#pragma once
#include "background.hpp"
#include "model_library.hpp"
#include "model_viewport.hpp"
#include "mod_store.hpp"
#include "startup.hpp"
#include "thumbnail_cache.hpp"
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <future>
#include <functional>
#include <thread>

namespace skeleton_repair {
class LibraryPanel {
public:
    LibraryPanel(const VulkanDevice& device, std::shared_ptr<ArchiveCache> archives, const std::filesystem::path& data_root);
    ~LibraryPanel();
    bool Busy() const { return worker_.valid(); }
    // True while source archives are being scanned for models.
    bool Scanning() const { return scan_.Busy(); }
    // Applies finished background work. Call once per frame before drawing.
    void Poll();
    void InvalidateThumbnails();
    // Cancels background reads and waits until no worker holds an archive open, for example before a repair replaces files.
    void StopLoading();
    // Skins plus package selections. Build, Clear and Ctrl+B follow this count.
    size_t SelectedCount() const { return selected_.size() + package_selection_.size(); }
    size_t ModelCount() const { return models_.size(); }
    // Non-model files of one source, or 0 when that source has no package.
    size_t PackageFiles(const std::filesystem::path& source) const;
    // Skins in one source, or its package file count when it has no skins.
    size_t SourceCount(const std::filesystem::path& source) const;
    // The source-list total: each source contributes SourceCount.
    size_t ListedCount() const;
    const std::vector<LibraryModel>& Models() const { return models_; }
    // Right-click Multi view on a skin. Empty hides the command.
    void SetMultiViewHandler(std::function<void(const LibraryModel&)> open) { open_multi_ = std::move(open); }
    // Cards the gallery showed on the last frame it was drawn, after search and filters.
    size_t ShownCount() const { return shown_count_; }
    // Bumps when the saved library would change: sources, pack selection, or a removed skin.
    uint64_t Revision() const { return revision_; }
    // Cached labels for one source. Counts stay at the last finished scan until the next one replaces them.
    struct SourceInfo {
        std::filesystem::path Path;
        std::string Name;
        std::string Full;
        std::string Tip;
        size_t Skins = 0;
        size_t Files = 0;
    };
    const SourceInfo* FindSource(const std::filesystem::path& path) const;
    // Models the last scan found in one source archive.
    size_t ModelsIn(const std::filesystem::path& source) const;
    const std::string& Status() const { return status_; }
    bool StatusIsError() const { return status_error_; }
    // Set after a pack export finishes. Empty for share and for a failed build.
    std::optional<AssembledPack> TakeExportNotice();
    // Shows only the skins of one source archive in the gallery. Empty shows every source.
    void SetSourceFilter(std::filesystem::path source) { source_filter_ = std::move(source); }
    const std::filesystem::path& SourceFilter() const { return source_filter_; }
    // Adds or removes every skin of one source archive in the pack.
    void IncludeSource(const std::filesystem::path& source, bool include);
    // Adds or removes one source's non-model files as a single package.
    void IncludePackage(const std::filesystem::path& source, bool include);
    void FocusSearch() { focus_search_ = true; }
    // Asks for a shared .db library to merge, or a library list from an earlier release to load.
    void AddLibrary(void* owner, const std::function<void(const ModelLibrary&)>& load,
        const std::function<void(const std::filesystem::path&)>& merge);
    // Asks where to write a copy of the whole library database.
    void ShareLibrary(void* owner);
    bool CanShare() const { return store_.has_value() && !Busy(); }
    // Asks where to export the pack, then builds it in the background.
    void BuildPack(void* owner, const std::filesystem::path& game);
    void ClearPack();
    void CancelBuild() { cancellation_.Cancel(); }
    // Reopens every source archive and loads thumbnails again.
    void Reload(const std::vector<std::filesystem::path>& sources);
    void Restore(const ModelLibrary& library);
    ModelLibrary Snapshot(const std::vector<std::filesystem::path>& sources, const std::filesystem::path& game) const;
    // `pause_loading` stops new thumbnail loads, for example while a repair replaces archives.
    void Draw(void* owner, const std::vector<std::filesystem::path>& sources,
        const std::filesystem::path& game, bool repair_busy, bool pause_loading, bool pack_view,
        const std::function<void()>& add,
        const std::function<void(const ModelLibrary&)>& load,
        const std::function<void(const std::filesystem::path&)>& merge,
        const std::function<void(const LibraryModel&)>& preview,
        const std::function<void(const LibraryModel&)>& analyze,
        const std::function<void()>& connect_game = {},
        const std::function<void()>& open_quick_start = {},
        bool collection_ready = true);
private:
    struct Thumbnail {
        std::unique_ptr<ThumbnailImage> Image;
        std::string Error;
        int LastFrame = 0;
        bool Pending = true;
    };
    struct ThumbnailJob {
        uint64_t Generation = 0;
        size_t Index = 0;
        LibraryModel Model;
        std::filesystem::path Game;
        CancellationToken Cancellation;
        // Set for jobs that only store a rendered thumbnail under `SaveKey`.
        std::optional<ThumbnailPixels> Save;
        std::string SaveKey;
        // Set when this job decodes a package texture instead of a model.
        bool Package = false;
        std::string TexturePath;
    };
    struct ThumbnailResult {
        uint64_t Generation = 0;
        size_t Index = 0;
        bool Package = false;
        // The cached image, when one exists. Otherwise `Model` is rendered once and stored under `CacheKey`.
        std::optional<ThumbnailPixels> Pixels;
        std::optional<PreparedModel> Model;
        // A package preview decoded from its texture. Shown directly, not through the model renderer.
        std::optional<TextureImage> Texture;
        std::string CacheKey;
        std::string Error;
    };
    // One source's non-model files. The card is this whole list, not one row per file.
    struct PackagePayload {
        std::filesystem::path Archive;
        std::vector<std::string> Paths;
        // A texture worth showing on the card, when the payload has one.
        std::string Preview;
    };
    struct ScanResult {
        std::vector<LibraryModel> Models;
        std::vector<PackagePayload> Packages;
        std::vector<std::string> Errors;
    };
    // One gallery card: a skin row or a source's other files.
    struct Card {
        bool Package = false;
        size_t Index = 0;
        bool operator==(const Card&) const = default;
        bool operator<(const Card& other) const { return Package == other.Package ? Index < other.Index : Package < other.Package; }
    };
    // Draws the thumbnail into the window's draw list at `origin`, without adding an item. Returns why it has no image, if it failed.
    const std::string* DrawThumbnail(const Card& card, const std::filesystem::path& game, ImVec2 origin, float size, size_t cache_limit,
        bool pause_loading);
    void DrawModelMenu(const Card& card, const std::vector<Card>& shown, const std::function<void(const LibraryModel&)>& preview,
        const std::function<void(const LibraryModel&)>& analyze, bool can_analyze);
    void DrawPackageMenu(const Card& card);
    void DrawGrid(const std::vector<Card>& shown, const std::filesystem::path& game, bool pause_loading,
        const std::function<void(const LibraryModel&)>& preview, const std::function<void(const LibraryModel&)>& analyze, bool can_analyze);
    void DrawList(const std::vector<Card>& shown, const std::filesystem::path& game, bool pause_loading,
        const std::function<void(const LibraryModel&)>& preview, const std::function<void(const LibraryModel&)>& analyze, bool can_analyze);
    // Handles clicks, keys and the context menu of the item just drawn for one card.
    void CardInput(const Card& card, const std::vector<Card>& shown, const std::function<void(const LibraryModel&)>& preview,
        const std::function<void(const LibraryModel&)>& analyze, bool can_analyze);
    void OpenFileList(const Card& card);
    void Activate(const Card& card, const std::function<void(const LibraryModel&)>& preview);
    void Toggle(const Card& card);
    bool InPack(size_t index);
    bool PackageInPack(const std::filesystem::path& source) const;
    void SetInPack(size_t index, bool include);
    void SortModels(int column, bool descending);
    void Evict(std::map<Card, Thumbnail>::iterator thumbnail);
    void Retire(std::unique_ptr<ThumbnailImage> image);
    void AgeRetired();
    void UploadThumbnails();
    void EnsureListing();
    void RebuildSourceInfo();
    void RebuildMissing();
    // Pack selection or the source list changed, so the next frame may save the library.
    void Changed();
    void NoteStatus(std::string text, bool error);
    struct PanelJob {
        std::string Status;
        bool Error = false;
        std::optional<std::filesystem::path> Output;
    };
    void RunThumbnailWorker();
    ThumbnailResult LoadThumbnail(const ThumbnailJob& job);
    ThumbnailResult LoadTextureThumbnail(const ThumbnailJob& job);
    ThumbnailPixels RenderThumbnail(const PreparedModel& model);
    VulkanDevice device_;
    std::shared_ptr<ArchiveCache> archives_;
    std::map<Card, Thumbnail> thumbnails_;
    // Images an in-flight frame may still sample. Destroyed a few frames later, without idling the device.
    struct RetiredThumbnail {
        std::unique_ptr<ThumbnailImage> Image;
        int FramesLeft = 0;
    };
    std::vector<RetiredThumbnail> retired_;
    int retired_aged_frame_ = -1;
    // One GPU thumbnail upload per ImGui frame. Each upload waits on the queue the window also presents with.
    int uploaded_frame_ = -1;
    // Renders thumbnails that are not cached yet. Created on first use.
    std::unique_ptr<ModelRenderer> renderer_;
    // Thumbnails are kept in the library database. Unset when local app data is unavailable.
    std::optional<LocalModStore> store_;
    std::filesystem::path legacy_thumbnails_;
    std::deque<ThumbnailResult> ready_;
    uint64_t generation_ = 0;
    std::filesystem::path thumbnail_game_;
    // Shared with the thumbnail workers.
    std::mutex queue_mutex_;
    std::condition_variable queue_changed_;
    std::condition_variable queue_idle_;
    std::deque<ThumbnailJob> jobs_;
    std::vector<ThumbnailResult> results_;
    size_t active_jobs_ = 0;
    bool stopping_ = false;
    CancellationSource thumbnail_cancellation_;
    std::vector<std::thread> workers_;
    void Refresh(const std::vector<std::filesystem::path>& sources);
    std::vector<std::filesystem::path> sources_;
    std::vector<LibraryModel> models_;
    std::vector<PackagePayload> payloads_;
    std::vector<LibraryModel> selected_;
    // Source archives whose non-model files are in the pack.
    std::vector<std::filesystem::path> package_selection_;
    std::vector<std::string> errors_;
    std::string status_;
    bool status_error_ = false;
    std::optional<AssembledPack> export_notice_;
    char filter_[256] = {};
    std::filesystem::path source_filter_;
    bool selected_only_ = false;
    bool list_view_ = false;
    bool focus_search_ = false;
    // Edge of a gallery thumbnail at a 15 px font.
    float thumbnail_size_ = 128;
    size_t shown_count_ = 0;
    // The card last clicked, which Enter and Space act on.
    std::optional<Card> current_;
    // Payload paths shown by the package file list. Opened on the next frame.
    std::vector<std::string> file_list_;
    std::string file_list_title_;
    bool open_file_list_ = false;
    std::function<void(const LibraryModel&)> open_multi_;
    std::map<std::filesystem::path, size_t> source_counts_;
    // Indexes of models_ in display order, and whether each model is in the pack. Rebuilt when marked stale.
    std::vector<size_t> order_;
    std::vector<uint8_t> in_pack_;
    bool pack_stale_ = true;
    // Skins and packages in display order, with the strings the search and the sort need. Rebuilt when marked stale.
    struct ListedCard {
        Card Id;
        std::string SortKey;
        std::string FilterText;
    };
    std::vector<ListedCard> listed_;
    bool listing_stale_ = true;
    std::vector<SourceInfo> source_info_;
    // Pack rows whose model is no longer in the library. Rebuilt when the selection or the scan changes.
    std::vector<size_t> missing_;
    bool missing_stale_ = true;
    uint64_t revision_ = 0;
    int sort_column_ = 0;
    bool sort_descending_ = false;
    std::future<PanelJob> worker_;
    CancellationSource cancellation_;
    LatestTask<ScanResult> scan_;
};
}
