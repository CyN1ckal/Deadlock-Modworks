#pragma once

#include "animation_player.hpp"
#include "model_viewport.hpp"

#include <vulkan/vulkan.h>

#include <string>

namespace skeleton_repair {

// ImGui panel that shows one model with its controls and mesh list.
class PreviewPanel {
public:
    explicit PreviewPanel(const VulkanDevice& device);

    // Uploads a model prepared off the UI thread. `animation_skeleton` and `clips` enable playback; `read` loads
    // the clips on a worker thread, so it must be safe to call from one.
    void Load(const PreparedModel& model, std::string title, ResourceReader read = {},
        const std::vector<GeometryBone>* animation_skeleton = nullptr, const std::vector<std::string>& clips = {});
    void ShowError(std::string title, std::string message);
    void PlayClip(const std::string& path);

    // Draws the controls and viewport into the current ImGui window.
    void Draw();
    // Records the offscreen pass if Draw showed the viewport this frame. Call before the ImGui render pass.
    void Record(VkCommandBuffer command_buffer);

private:
    // The side pane: statistics, mesh visibility and material notes.
    void DrawDetails();

    ModelViewport viewport_;
    AnimationPlayer player_;
    ResourceReader read_;
    std::string animation_warning_;
    std::string title_;
    std::string error_;
    OrbitCamera camera_;
    ViewSettings settings_;
};

}  // namespace skeleton_repair
