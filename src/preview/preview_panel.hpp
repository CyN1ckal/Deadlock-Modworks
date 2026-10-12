#pragma once

#include "model_renderer.hpp"
#include "preview_scene.hpp"

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

namespace skeleton_repair {

// ImGui panel that shows a model through ModelRenderer.
// Requires imgui_impl_vulkan to be initialized; textures are registered with ImGui_ImplVulkan_AddTexture.
class PreviewPanel {
public:
    explicit PreviewPanel(ModelRenderer& renderer);
    ~PreviewPanel();
    PreviewPanel(const PreviewPanel&) = delete;
    PreviewPanel& operator=(const PreviewPanel&) = delete;

    void Load(const ModelGeometry& geometry, std::string title);
    void ShowError(std::string title, std::string message);

    // Draws the controls and viewport into the current ImGui window.
    void Draw();
    // Records the offscreen pass if Draw showed the viewport this frame. Call before the ImGui render pass.
    void Record(VkCommandBuffer command_buffer);

private:
    void Frame();
    void RefreshTexture();

    ModelRenderer& renderer_;
    PreviewScene scene_;
    std::string title_;
    std::string error_;
    std::vector<uint8_t> visible_;
    OrbitCamera camera_;
    RenderOptions options_;
    int lod_ = 0;
    VkDescriptorSet texture_ = VK_NULL_HANDLE;
    bool record_ = false;
};

}  // namespace skeleton_repair
