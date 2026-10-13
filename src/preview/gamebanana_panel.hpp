#pragma once

#include "background.hpp"
#include "gamebanana.hpp"
#include "thumbnail_cache.hpp"

#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace skeleton_repair {

// The Get mods page: browse and search Deadlock mods on GameBanana, open a mod from a pasted link, and download
// one of its files into the library. Downloads themselves are run by the caller.
class GameBananaPanel {
public:
    explicit GameBananaPanel(const VulkanDevice& device);
    ~GameBananaPanel();
    GameBananaPanel(const GameBananaPanel&) = delete;
    GameBananaPanel& operator=(const GameBananaPanel&) = delete;

    using Download = std::function<void(const GbMod& mod, const GbFile& file, bool enable)>;
    // `installed` counts the library's mods downloaded from a GameBanana mod. `can_download` is false while other
    // library work runs, with `busy_reason` saying why.
    void Draw(bool can_download, const char* busy_reason, const std::function<size_t(int64_t)>& installed, const Download& download);
    // Opens a mod from a link or id, or searches for the text.
    void Go(const std::string& text);

private:
    enum class Mode { Browse, Search };
    struct Image {
        std::unique_ptr<ThumbnailImage> Texture;
        bool Failed = false;
        int Attempts = 0;
        double RetryAt = 0;
        int LastFrame = 0;
    };
    struct Retired {
        std::unique_ptr<ThumbnailImage> Texture;
        int FramesLeft = 0;
    };

    void Query();
    void OpenMod(int64_t id);
    void PollTasks();
    void PollImages();
    // The texture for an image URL, or null while it loads or when it failed. Starts loading it.
    ImTextureID ImageFor(const std::string& url);
    void DrawToolbar();
    void DrawResults(const std::function<size_t(int64_t)>& installed);
    void DrawDetails(bool can_download, const char* busy_reason, const std::function<size_t(int64_t)>& installed, const Download& download);
    void DrawImage(const std::string& url, ImVec2 size, bool hidden);

    VulkanDevice device_;
    char input_[512] = {};
    Mode mode_ = Mode::Browse;
    std::string query_;
    GbSort sort_ = GbSort::MostDownloaded;
    int64_t category_ = 0;
    int page_ = 1;
    bool show_rated_ = false;

    LatestTask<GbPage> list_task_;
    std::optional<GbPage> page_data_;
    std::string list_error_;
    LatestTask<std::vector<GbCategory>> categories_task_;
    std::vector<GbCategory> categories_;
    bool categories_requested_ = false;
    LatestTask<GbMod> mod_task_;
    std::optional<GbMod> mod_;
    int64_t mod_loading_ = 0;
    std::string mod_error_;
    size_t screenshot_ = 0;
    std::string description_;

    std::map<std::string, Image> images_;
    std::vector<std::pair<std::string, std::future<std::optional<ThumbnailPixels>>>> image_jobs_;
    std::vector<Retired> retired_;
    int retired_frame_ = -1;
    CancellationSource image_cancel_;
};

}  // namespace skeleton_repair
