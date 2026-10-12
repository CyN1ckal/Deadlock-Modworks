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
    void Restore(const ModelLibrary& library);
    ModelLibrary Snapshot(const std::vector<std::filesystem::path>& sources, const std::filesystem::path& game) const;
    // `pause_loading` stops new thumbnail loads, for example while a repair replaces archives.
    void Draw(void* owner, const std::vector<std::filesystem::path>& sources,
        const std::filesystem::path& game, bool repair_busy, bool pause_loading, bool pack_view,
        const std::function<void()>& add,
        const std::function<void(const ModelLibrary&)>& load,
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
        // Set for jobs that only write a rendered thumbnail to `SavePath`.
        std::optional<ThumbnailPixels> Save;
        std::filesystem::path SavePath;
    };
    struct ThumbnailResult {
        uint64_t Generation = 0;
        size_t Index = 0;
        // The cached image, when one exists. Otherwise `Model` is rendered once and saved to `CachePath`.
        std::optional<ThumbnailPixels> Pixels;
        std::optional<PreparedModel> Model;
        std::filesystem::path CachePath;
        std::string Error;
    };
    struct ScanResult {
        std::vector<LibraryModel> Models;
        std::vector<std::string> Errors;
    };
    void DrawThumbnail(size_t index, const std::filesystem::path& game, float size, size_t cache_limit, bool pause_loading);
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
    std::filesystem::path cache_folder_;
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
    std::future<std::string> worker_;
    CancellationSource cancellation_;
    LatestTask<ScanResult> scan_;
};
}
