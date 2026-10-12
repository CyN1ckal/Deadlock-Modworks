#pragma once
#include "model_library.hpp"
#include "model_viewport.hpp"
#include <map>
#include <memory>
#include <future>
#include <functional>

namespace skeleton_repair {
class LibraryPanel {
public:
    explicit LibraryPanel(const VulkanDevice& device) : device_(device) {}
    ~LibraryPanel();
    bool Busy() const { return worker_.valid(); }
    void Poll();
    void InvalidateThumbnails();
    void Record(VkCommandBuffer command_buffer);
    size_t SelectedCount() const { return selected_.size(); }
    size_t ModelCount() const { return models_.size(); }
    void Restore(const ModelLibrary& library);
    ModelLibrary Snapshot(const std::vector<std::filesystem::path>& sources, const std::filesystem::path& game) const;
    void Draw(void* owner, const std::vector<std::filesystem::path>& sources,
        const std::filesystem::path& game, bool repair_busy, bool pack_view,
        const std::function<void()>& add,
        const std::function<void(const ModelLibrary&)>& load,
        const std::function<void(const LibraryModel&)>& preview);
private:
    struct Thumbnail {
        std::unique_ptr<ModelViewport> Viewport;
        OrbitCamera Camera;
        std::string Error;
        int LastFrame = 0;
        float RenderSize = 0;
        bool Rendered = false;
    };
    void DrawThumbnail(size_t index, const std::filesystem::path& game, float size, size_t cache_limit, bool& loaded_this_frame);
    VulkanDevice device_;
    std::map<size_t, Thumbnail> thumbnails_;
    std::filesystem::path thumbnail_game_;
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
};
}
