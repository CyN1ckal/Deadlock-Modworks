// Tracy's winsock2.h comes before windows.h.
#include "profile.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// Before any vulkan.h include, so the Win32 surface API is declared.
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "vulkan_window.hpp"

#include "product_version.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <cstdio>
#include <iterator>
#include <stdexcept>
#include <string>

namespace skeleton_repair {
namespace {

constexpr uint32_t kMinImageCount = 2;

void CheckVk(VkResult result) {
    if (result == VK_SUCCESS) return;
    std::fprintf(stderr, "[vulkan] VkResult = %d\n", result);
    if (result < 0) {
        throw std::runtime_error("Vulkan initialization or rendering failed (" + std::to_string(result)
            + "). Check that your graphics driver supports Vulkan 1.3.");
    }
}

// Out of date and suboptimal swapchains are rebuilt on the next frame instead of failing.
bool NeedsRebuild(VkResult result) {
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) return true;
    CheckVk(result);
    return false;
}

}  // namespace

VulkanWindow::VulkanWindow(HWND__* hwnd, HINSTANCE__* instance) : window_(std::make_unique<ImGui_ImplVulkanH_Window>()) {
    try {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = SR_PRODUCT_NAME;
        app.applicationVersion = VK_MAKE_API_VERSION(0, SR_PRODUCT_VERSION_MAJOR, SR_PRODUCT_VERSION_MINOR, SR_PRODUCT_VERSION_PATCH);
        app.apiVersion = VK_API_VERSION_1_3;
        const char* extensions[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
        VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instance_info.pApplicationInfo = &app;
        instance_info.enabledExtensionCount = static_cast<uint32_t>(std::size(extensions));
        instance_info.ppEnabledExtensionNames = extensions;
        CheckVk(vkCreateInstance(&instance_info, nullptr, &instance_));

        device_.Physical = ImGui_ImplVulkanH_SelectPhysicalDevice(instance_);
        if (device_.Physical == VK_NULL_HANDLE) throw std::runtime_error("No Vulkan device was found.");
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device_.Physical, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3) throw std::runtime_error("The Vulkan driver does not support version 1.3.");
        device_.QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(device_.Physical);

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = device_.QueueFamily;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        features13.dynamicRendering = VK_TRUE;
        features13.synchronization2 = VK_TRUE;
        const char* device_extensions[] = {"VK_KHR_swapchain"};
        VkPhysicalDeviceFeatures supported{};
        vkGetPhysicalDeviceFeatures(device_.Physical, &supported);
        VkPhysicalDeviceFeatures enabled{};
        enabled.textureCompressionBC = supported.textureCompressionBC;
        VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device.pNext = &features13;
        device.pEnabledFeatures = &enabled;
        device.queueCreateInfoCount = 1;
        device.pQueueCreateInfos = &queue;
        device.enabledExtensionCount = static_cast<uint32_t>(std::size(device_extensions));
        device.ppEnabledExtensionNames = device_extensions;
        CheckVk(vkCreateDevice(device_.Physical, &device, nullptr, &device_.Device));
        vkGetDeviceQueue(device_.Device, device_.QueueFamily, 0, &device_.Queue);

        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE + 512};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool.maxSets = pool_size.descriptorCount;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &pool_size;
        CheckVk(vkCreateDescriptorPool(device_.Device, &pool, nullptr, &descriptor_pool_));

        VkWin32SurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surface_info.hwnd = hwnd;
        surface_info.hinstance = instance;
        CheckVk(vkCreateWin32SurfaceKHR(instance_, &surface_info, nullptr, &window_->Surface));
        VkBool32 presentable = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device_.Physical, device_.QueueFamily, window_->Surface, &presentable);
        if (presentable != VK_TRUE) throw std::runtime_error("The Vulkan device cannot present to this window.");
        const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM};
        window_->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(device_.Physical, window_->Surface, formats,
            static_cast<int>(std::size(formats)), VK_COLORSPACE_SRGB_NONLINEAR_KHR);
        const VkPresentModeKHR present_modes[] = {VK_PRESENT_MODE_FIFO_KHR};
        window_->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(device_.Physical, window_->Surface, present_modes,
            static_cast<int>(std::size(present_modes)));
        RECT client{};
        GetClientRect(hwnd, &client);
        ImGui_ImplVulkanH_CreateOrResizeWindow(instance_, device_.Physical, device_.Device, window_.get(), device_.QueueFamily, nullptr,
            client.right - client.left, client.bottom - client.top, kMinImageCount);
    } catch (...) {
        Destroy();
        throw;
    }
}

VulkanWindow::~VulkanWindow() {
    Destroy();
}

void VulkanWindow::Destroy() {
    if (device_.Device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_.Device);
        // Destroys the swapchain, its frames and the surface.
        ImGui_ImplVulkanH_DestroyWindow(instance_, device_.Device, window_.get(), nullptr);
        if (descriptor_pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_.Device, descriptor_pool_, nullptr);
        vkDestroyDevice(device_.Device, nullptr);
    } else if (window_->Surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, window_->Surface, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
    *window_ = ImGui_ImplVulkanH_Window{};
    descriptor_pool_ = VK_NULL_HANDLE;
    device_ = {};
    instance_ = VK_NULL_HANDLE;
}

void VulkanWindow::InitImGui() {
    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = VK_API_VERSION_1_3;
    init.Instance = instance_;
    init.PhysicalDevice = device_.Physical;
    init.Device = device_.Device;
    init.QueueFamily = device_.QueueFamily;
    init.Queue = device_.Queue;
    init.DescriptorPool = descriptor_pool_;
    init.RenderPass = window_->RenderPass;
    init.MinImageCount = kMinImageCount;
    init.ImageCount = window_->ImageCount;
    init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.CheckVkResultFn = CheckVk;
    ImGui_ImplVulkan_Init(&init);
}

void VulkanWindow::RebuildIfNeeded(int width, int height) {
    if (!rebuild_ || width <= 0 || height <= 0) return;
    ZoneScopedN("Swapchain");
    ImGui_ImplVulkan_SetMinImageCount(kMinImageCount);
    ImGui_ImplVulkanH_CreateOrResizeWindow(instance_, device_.Physical, device_.Device, window_.get(), device_.QueueFamily, nullptr,
        width, height, kMinImageCount);
    window_->FrameIndex = 0;
    rebuild_ = false;
}

void VulkanWindow::Draw(ImDrawData* draw_data, const std::array<float, 3>& clear,
    const std::function<void(VkCommandBuffer)>& record_offscreen) {
    if (draw_data->DisplaySize.x <= 0 || draw_data->DisplaySize.y <= 0) return;
    window_->ClearValue.color = {{clear[0], clear[1], clear[2], 1.0f}};
    Render(draw_data, record_offscreen);
    Present();
}

void VulkanWindow::Render(ImDrawData* draw_data, const std::function<void(VkCommandBuffer)>& record_offscreen) {
    ZoneScopedN("GPU submit");
    ImGui_ImplVulkanH_Window* wd = window_.get();
    VkSemaphore acquired = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore complete = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    const VkResult result = vkAcquireNextImageKHR(device_.Device, wd->Swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &wd->FrameIndex);
    if (NeedsRebuild(result)) rebuild_ = true;
    if (result == VK_ERROR_OUT_OF_DATE_KHR) return;

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    CheckVk(vkWaitForFences(device_.Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX));
    CheckVk(vkResetFences(device_.Device, 1, &fd->Fence));
    CheckVk(vkResetCommandPool(device_.Device, fd->CommandPool, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CheckVk(vkBeginCommandBuffer(fd->CommandBuffer, &begin));

    // Model images must be rendered before the ImGui pass samples them.
    record_offscreen(fd->CommandBuffer);

    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = wd->RenderPass;
    pass.framebuffer = fd->Framebuffer;
    pass.renderArea.extent = {static_cast<uint32_t>(wd->Width), static_cast<uint32_t>(wd->Height)};
    pass.clearValueCount = 1;
    pass.pClearValues = &wd->ClearValue;
    vkCmdBeginRenderPass(fd->CommandBuffer, &pass, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);
    vkCmdEndRenderPass(fd->CommandBuffer);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquired;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &fd->CommandBuffer;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &complete;
    CheckVk(vkEndCommandBuffer(fd->CommandBuffer));
    CheckVk(vkQueueSubmit(device_.Queue, 1, &submit, fd->Fence));
}

void VulkanWindow::Present() {
    ZoneScopedN("Present");
    if (rebuild_) return;
    ImGui_ImplVulkanH_Window* wd = window_.get();
    VkSemaphore complete = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &complete;
    present.swapchainCount = 1;
    present.pSwapchains = &wd->Swapchain;
    present.pImageIndices = &wd->FrameIndex;
    const VkResult result = vkQueuePresentKHR(device_.Queue, &present);
    if (NeedsRebuild(result)) rebuild_ = true;
    if (result == VK_ERROR_OUT_OF_DATE_KHR) return;
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}

void VulkanWindow::WaitIdle() const {
    if (device_.Device != VK_NULL_HANDLE) CheckVk(vkDeviceWaitIdle(device_.Device));
}

}  // namespace skeleton_repair
