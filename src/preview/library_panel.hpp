#pragma once
#include "background.hpp"
#include "model_library.hpp"
#include "model_viewport.hpp"
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
    LibraryPanel(const VulkanDevice& device, std::shared_ptr<ArchiveCache> archives);
    ~LibraryPanel();
    bool Busy() const { return worker_.valid(); }
    // True while source archives are being scanned for models.
    bool Scanning() const { return scan_.Busy(); }
    // Applies finished background work. Call once per frame before drawing.
    void Poll();
    void InvalidateThumbnails();
    // Cancels background reads and waits until no worker holds an archive open, for example before a repair replaces files.
    void StopLoading();
    size_t SelectedCount() const { return selected_.size(); }
    size_t ModelCount() const { return models_.size(); }
    const std::vector<LibraryModel>& Models() const { return models_; }
    // Right-click Multi view on a skin. Empty hides the command.
    void SetMultiViewHandler(std::function<void(const LibraryModel&)> open) { open_multi_ = std::move(open); }
    // Skins the gallery showed on the last frame it was drawn, after search and filters.
    size_t ShownCount() const { return shown_count_; }
    // Models the last scan found in one source archive.
    size_t ModelsIn(const std::filesystem::path& source) const;
    const std::string& Status() const { return status_; }
    // Shows only the skins of one source archive in the gallery. Empty shows every source.
    void SetSourceFilter(std::filesystem::path source) { source_filter_ = std::move(source); }
    const std::filesystem::path& SourceFilter() const { return source_filter_; }
    // Adds or removes every skin of one source archive in the pack.
    void IncludeSource(const std::filesystem::path& source, bool include);
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
        const std::function<void(const LibraryModel&)>& preview);
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
    };
    struct ThumbnailResult {
        uint64_t Generation = 0;
        size_t Index = 0;
        // The cached image, when one exists. Otherwise `Model` is rendered once and stored under `CacheKey`.
        std::optional<ThumbnailPixels> Pixels;
        std::optional<PreparedModel> Model;
        std::string CacheKey;
        std::string Error;
    };
    struct ScanResult {
        std::vector<LibraryModel> Models;
        std::vector<std::string> Errors;
    };
    // Draws the thumbnail into the window's draw list at `origin`, without adding an item. Returns why it has no image, if it failed.
    const std::string* DrawThumbnail(size_t index, const std::filesystem::path& game, ImVec2 origin, float size, size_t cache_limit,
        bool pause_loading);
    void DrawModelMenu(size_t index, const std::vector<size_t>& shown, const std::function<void(const LibraryModel&)>& preview);
    void DrawGrid(const std::vector<size_t>& shown, const std::filesystem::path& game, bool pause_loading,
        const std::function<void(const LibraryModel&)>& preview);
    void DrawList(const std::vector<size_t>& shown, const std::filesystem::path& game, bool pause_loading,
        const std::function<void(const LibraryModel&)>& preview);
    // Handles clicks, keys and the context menu of the item just drawn for one model.
    void ModelItemInput(size_t index, const std::vector<size_t>& shown, const std::function<void(const LibraryModel&)>& preview);
    bool InPack(size_t index);
    void SetInPack(size_t index, bool include);
    void SortModels(int column, bool descending);
    void Evict(std::map<size_t, Thumbnail>::iterator thumbnail);
    void UploadThumbnails();
    void RunThumbnailWorker();
    ThumbnailResult LoadThumbnail(const ThumbnailJob& job);
    ThumbnailPixels RenderThumbnail(const PreparedModel& model);
    VulkanDevice device_;
    std::shared_ptr<ArchiveCache> archives_;
    std::map<size_t, Thumbnail> thumbnails_;
    // Evicted thumbnails whose images an in-flight frame may still sample; destroyed after the next device wait.
    std::vector<std::unique_ptr<ThumbnailImage>> retired_;
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
    std::vector<LibraryModel> selected_;
    std::vector<std::string> errors_;
    std::string status_;
    char filter_[256] = {};
    std::filesystem::path source_filter_;
    bool selected_only_ = false;
    bool list_view_ = false;
    bool focus_search_ = false;
    // Edge of a gallery thumbnail at a 15 px font.
    float thumbnail_size_ = 128;
    size_t shown_count_ = 0;
    // The model last clicked, which Enter and Space act on.
    std::optional<size_t> current_;
    std::function<void(const LibraryModel&)> open_multi_;
    std::map<std::filesystem::path, size_t> source_counts_;
    // Indexes of models_ in display order, and whether each model is in the pack. Rebuilt when marked stale.
    std::vector<size_t> order_;
    std::vector<uint8_t> in_pack_;
    bool pack_stale_ = true;
    int sort_column_ = 0;
    bool sort_descending_ = false;
    std::future<std::string> worker_;
    CancellationSource cancellation_;
    LatestTask<ScanResult> scan_;
};
}
