#pragma once

#include "background.hpp"
#include "character_view.hpp"
#include "model_viewport.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace skeleton_repair {

// Several models of one character, each in its own viewport, sharing one orbit camera when Link is on.
class MultiView {
public:
    static constexpr int kMaxSlots = 4;

    explicit MultiView(const VulkanDevice& device);

    void SetArchives(std::shared_ptr<ArchiveCache> archives);
    // Reloads the open slots when the connected game archive changes, so textures can resolve.
    void SetGame(std::shared_ptr<const IndexedArchive> game);

    // Fills the grid from `library` with models of `model`'s character and starts loading them.
    // `keep_layout` leaves the viewport count alone, for a character change from the toolbar.
    void Open(const LibraryModel& model, const std::vector<LibraryModel>& library, bool keep_layout = false);
    void OpenCharacter(const std::string& name, const std::vector<LibraryModel>& library);

    bool Busy() const;
    // Drops GPU copies and waits until background loads finish. The slot assignment is kept for Reload.
    void Release();
    void Reload();

    void Poll();
    void Draw(const std::vector<LibraryModel>& library);
    // Records every viewport Draw showed this frame. Call before the ImGui render pass.
    void Record(VkCommandBuffer command_buffer);

private:
    struct LoadResult {
        LibraryModel Model;
        std::optional<PreparedModel> Prepared;
        std::optional<std::vector<GeometryBone>> Animation;
        std::vector<std::string> Clips;
        std::string Error;
    };
    struct Slot {
        std::optional<LibraryModel> Model;
        std::optional<PreparedModel> Prepared;
        std::optional<std::vector<GeometryBone>> Animation;
        std::vector<std::string> Clips;
        std::string Error;
        std::string Warning;
        OrbitCamera Camera;
    };

    static LoadResult LoadSlot(std::shared_ptr<ArchiveCache> archives, std::shared_ptr<const IndexedArchive> game,
        LibraryModel model, CancellationToken token);
    void StartLoad(int slot);
    void Apply(int slot, LoadResult result);
    void Assign(int slot, std::optional<LibraryModel> model);
    void SyncClips();
    void SetLinked(bool linked);
    ResourceReader ClipReader() const;
    void DrawSlot(int slot, ImVec2 size, const std::vector<LibraryModel>& choices, bool& frame, bool& lod_changed);

    std::shared_ptr<ArchiveCache> archives_;
    std::shared_ptr<const IndexedArchive> game_;
    std::string character_;
    int slot_count_ = 4;
    int last_moved_ = 0;
    bool link_cameras_ = true;
    bool framed_ = false;
    ViewSettings settings_;
    OrbitCamera shared_;
    AnimationPlayer player_;
    std::vector<std::string> clip_paths_;
    char picker_filter_[64] = {};
    std::array<Slot, kMaxSlots> slots_;
    std::array<std::unique_ptr<ModelViewport>, kMaxSlots> views_;
    // Declared last so its threads are joined before the viewports are destroyed.
    std::array<LatestTask<LoadResult>, kMaxSlots> loads_;
};

}  // namespace skeleton_repair
