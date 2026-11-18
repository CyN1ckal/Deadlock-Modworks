#pragma once

#include "model_renderer.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <functional>
#include <memory>

struct HWND__;
struct HINSTANCE__;
struct ImDrawData;
struct ImGui_ImplVulkanH_Window;

namespace skeleton_repair {

// The Vulkan 1.3 instance, device and swapchain of the model tools window, following Dear ImGui's
// example_win32_vulkan. Everything it creates is destroyed with it, also when construction fails part way.
class VulkanWindow {
public:
    // Throws std::runtime_error with a message for the user when the driver lacks Vulkan 1.3 or cannot present.
    VulkanWindow(HWND__* hwnd, HINSTANCE__* instance);
    // Waits for the GPU first. Shut Dear ImGui's Vulkan backend down before this.
    ~VulkanWindow();
    VulkanWindow(const VulkanWindow&) = delete;
    VulkanWindow& operator=(const VulkanWindow&) = delete;

    const VulkanDevice& Device() const { return device_; }
    // Initializes Dear ImGui's Vulkan backend for this swapchain. The ImGui context must exist.
    void InitImGui();
    // The swapchain no longer matches the window, for example after a resize.
    void RequestRebuild() { rebuild_ = true; }
    // Rebuilds the swapchain for the client size when requested. A zero size waits for a later call.
    void RebuildIfNeeded(int width, int height);
    // Records `record_offscreen` (model images the UI samples), then the ImGui pass, and presents the frame.
    void Draw(ImDrawData* draw_data, const std::array<float, 3>& clear, const std::function<void(VkCommandBuffer)>& record_offscreen);
    void WaitIdle() const;

private:
    void Destroy();
    void Render(ImDrawData* draw_data, const std::function<void(VkCommandBuffer)>& record_offscreen);
    void Present();

    VkInstance instance_ = VK_NULL_HANDLE;
    VulkanDevice device_;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    std::unique_ptr<ImGui_ImplVulkanH_Window> window_;
    bool rebuild_ = false;
};

}  // namespace skeleton_repair
