// Model preview tool: opens a .vmdl_c or a VPK and shows the decoded model with PreviewPanel.
// Window and swapchain setup follow Dear ImGui's example_win32_vulkan.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

// Before any vulkan.h include, so the Win32 surface API is declared.
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "model_geometry.hpp"
#include "model_renderer.hpp"
#include "comparison_view.hpp"
#include "model_versions.hpp"
#include "preview_panel.hpp"
#include "strings.hpp"
#include "vpk.hpp"
#include "game_folder.hpp"
#include "model_tools.hpp"
#include "repair_panel.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace {

using namespace skeleton_repair;

VkInstance g_instance = VK_NULL_HANDLE;
VulkanDevice g_device;
VkDescriptorPool g_descriptor_pool = VK_NULL_HANDLE;
ImGui_ImplVulkanH_Window g_window;
constexpr uint32_t kMinImageCount = 2;
bool g_swapchain_rebuild = false;
std::vector<std::filesystem::path> g_dropped;

void CheckVk(VkResult result) {
    if (result == VK_SUCCESS) {
        return;
    }
    std::fprintf(stderr, "[vulkan] VkResult = %d\n", result);
    if (result < 0) {
        throw std::runtime_error("Vulkan initialization or rendering failed (" + std::to_string(result) + "). Check that your graphics driver supports Vulkan 1.3.");
    }
}

void SetupVulkan() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Deadlock Model Tools";
    app.apiVersion = VK_API_VERSION_1_3;
    const char* extensions[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
    VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance.pApplicationInfo = &app;
    instance.enabledExtensionCount = 2;
    instance.ppEnabledExtensionNames = extensions;
    CheckVk(vkCreateInstance(&instance, nullptr, &g_instance));

    g_device.Physical = ImGui_ImplVulkanH_SelectPhysicalDevice(g_instance);
    if (g_device.Physical == VK_NULL_HANDLE) {
        throw std::runtime_error("No Vulkan device was found.");
    }
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(g_device.Physical, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_3) {
        throw std::runtime_error("The Vulkan driver does not support version 1.3.");
    }
    g_device.QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_device.Physical);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = g_device.QueueFamily;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    const char* device_extensions[] = {"VK_KHR_swapchain"};
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(g_device.Physical, &supported);
    VkPhysicalDeviceFeatures enabled{};
    enabled.textureCompressionBC = supported.textureCompressionBC;
    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device.pNext = &features13;
    device.pEnabledFeatures = &enabled;
    device.queueCreateInfoCount = 1;
    device.pQueueCreateInfos = &queue;
    device.enabledExtensionCount = 1;
    device.ppEnabledExtensionNames = device_extensions;
    CheckVk(vkCreateDevice(g_device.Physical, &device, nullptr, &g_device.Device));
    vkGetDeviceQueue(g_device.Device, g_device.QueueFamily, 0, &g_device.Queue);

    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE + 4};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool.maxSets = pool_size.descriptorCount;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &pool_size;
    CheckVk(vkCreateDescriptorPool(g_device.Device, &pool, nullptr, &g_descriptor_pool));
}

void SetupWindow(VkSurfaceKHR surface, int width, int height) {
    g_window.Surface = surface;
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_device.Physical, g_device.QueueFamily, surface, &supported);
    if (supported != VK_TRUE) {
        throw std::runtime_error("The Vulkan device cannot present to this window.");
    }
    const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM};
    g_window.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(g_device.Physical, surface, formats, std::size(formats),
        VK_COLORSPACE_SRGB_NONLINEAR_KHR);
    VkPresentModeKHR present_modes[] = {VK_PRESENT_MODE_FIFO_KHR};
    g_window.PresentMode = ImGui_ImplVulkanH_SelectPresentMode(g_device.Physical, surface, present_modes, std::size(present_modes));
    ImGui_ImplVulkanH_CreateOrResizeWindow(g_instance, g_device.Physical, g_device.Device, &g_window, g_device.QueueFamily, nullptr,
        width, height, kMinImageCount);
}

void FrameRender(ImDrawData* draw_data, const std::function<void(VkCommandBuffer)>& record_offscreen) {
    ImGui_ImplVulkanH_Window* wd = &g_window;
    VkSemaphore acquired = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore complete = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkResult result = vkAcquireNextImageKHR(g_device.Device, wd->Swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &wd->FrameIndex);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        g_swapchain_rebuild = true;
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return;
    }
    if (result != VK_SUBOPTIMAL_KHR) {
        CheckVk(result);
    }

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    CheckVk(vkWaitForFences(g_device.Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX));
    CheckVk(vkResetFences(g_device.Device, 1, &fd->Fence));
    CheckVk(vkResetCommandPool(g_device.Device, fd->CommandPool, 0));
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
    CheckVk(vkQueueSubmit(g_device.Queue, 1, &submit, fd->Fence));
}

void FramePresent() {
    if (g_swapchain_rebuild) {
        return;
    }
    ImGui_ImplVulkanH_Window* wd = &g_window;
    VkSemaphore complete = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &complete;
    present.swapchainCount = 1;
    present.pSwapchains = &wd->Swapchain;
    present.pImageIndices = &wd->FrameIndex;
    const VkResult result = vkQueuePresentKHR(g_device.Queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        g_swapchain_rebuild = true;
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return;
    }
    if (result != VK_SUBOPTIMAL_KHR) {
        CheckVk(result);
    }
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) {
        return true;
    }
    switch (msg) {
    case WM_DROPFILES: {
        const auto drop = reinterpret_cast<HDROP>(wparam);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            std::wstring path(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
            path.resize(DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size())));
            g_dropped.emplace_back(path);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = {1000, 680};
        return 0;
    case WM_SIZE:
        if (g_device.Device != VK_NULL_HANDLE && wparam != SIZE_MINIMIZED) {
            g_swapchain_rebuild = true;
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU) {
            return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Could not open " + Narrow(path.wstring()) + ".");
    }
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool EndsWith(const std::string& text, const char* suffix) {
    const size_t length = std::char_traits<char>::length(suffix);
    return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

const VpkEntry* FindEntry(const VpkArchive& archive, std::string_view path) {
    for (const VpkEntry& entry : archive.Entries()) {
        if (EqualsIgnoreCase(entry.FullPath, path)) {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<std::filesystem::path> AskForFiles(HWND owner, const wchar_t* filter, const wchar_t* title, bool multiple = false) {
    std::vector<wchar_t> buffer(65536, L'\0');
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrTitle = title;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (multiple) dialog.Flags |= OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameW(&dialog)) {
        return {};
    }
    const std::filesystem::path first(buffer.data());
    const wchar_t* next = buffer.data() + wcslen(buffer.data()) + 1;
    if (*next == L'\0') return {first};
    std::vector<std::filesystem::path> files;
    for (; *next != L'\0'; next += wcslen(next) + 1) files.push_back(first / next);
    return files;
}

// Accepts the game/citadel folder or its pak01_dir.vpk, like the updater's game directory.
std::filesystem::path GamePak(const std::filesystem::path& chosen) {
    if (EndsWith(Narrow(chosen.wstring()), ".vpk")) {
        return chosen;
    }
    return chosen / L"pak01_dir.vpk";
}

// Archives, the model list, and the two views that show the selection.
struct App {
    PreviewPanel Panel;
    ComparisonView Compare;
    std::optional<VpkArchive> Mod;
    std::string ModName;
    std::vector<const VpkEntry*> Models;
    std::string Selected;
    char Filter[128] = {};
    std::optional<VpkArchive> Game;
    std::string GameName;
    std::string GameError;
    RepairPanel Repair;
    std::vector<std::filesystem::path> ModPaths;
    std::filesystem::path ActiveMod;
    bool StandaloneModel = false;

    void SyncSources() {
        std::vector<std::string> paths;
        for (const auto& path : ModPaths) paths.push_back(Narrow(path.wstring()));
        Repair.SetSources(Game ? Narrow(std::filesystem::path(Widen(GameName)).parent_path().wstring()) : "", std::move(paths));
    }

    explicit App(const VulkanDevice& device) : Panel(device), Compare(device) {}

    void OpenGame(const std::filesystem::path& chosen) {
        Game.reset();
        GameName.clear();
        GameError.clear();
        const std::filesystem::path pak = GamePak(chosen);
        try {
            Game.emplace(VpkArchive::Open(pak));
            GameName = Narrow(pak.wstring());
        } catch (const std::exception& error) {
            GameError = Narrow(pak.wstring()) + ": " + error.what();
        }
        if (!Selected.empty()) {
            Select(Selected);
        }
        SyncSources();
    }

    void OpenMod(const std::filesystem::path& path) {
        // Clear the views before changing the archive used by their resource readers.
        Panel.ShowError("", "");
        Compare.ShowError("", "");
        Mod.reset();
        Models.clear();
        Selected.clear();
        ModName = Narrow(path.filename().wstring());
        StandaloneModel = false;
        ActiveMod = path;
        Filter[0] = '\0';
        try {
            Mod.emplace(VpkArchive::Open(path));
        } catch (const std::exception& error) {
            Panel.ShowError(Narrow(path.wstring()), error.what());
            Compare.ShowError(Narrow(path.wstring()), error.what());
            return;
        }
        for (const VpkEntry& entry : Mod->Entries()) {
            if (EndsWith(entry.FullPath, ".vmdl_c")) {
                Models.push_back(&entry);
            }
        }
        std::sort(Models.begin(), Models.end(), [](const VpkEntry* a, const VpkEntry* b) { return a->FullPath < b->FullPath; });
        if (!Models.empty()) Select(Models.front()->FullPath);
        else Panel.ShowError(ModName, "This archive contains no compiled models.");
    }

    std::filesystem::path AddArchive(const std::filesystem::path& path) {
        const auto full = std::filesystem::absolute(path).lexically_normal();
        if (std::none_of(ModPaths.begin(), ModPaths.end(), [&](const auto& other) {
            return skeleton_repair::EqualsIgnoreCase(Narrow(other.wstring()), Narrow(full.wstring()));
        })) ModPaths.push_back(full);
        return full;
    }

    void AddPaths(const std::vector<std::filesystem::path>& paths) {
        if (paths.empty()) return;
        // Register the whole selection, but decode only the model shown in the workspace.
        for (const auto& path : paths) {
            if (skeleton_repair::EqualsIgnoreCase(Narrow(path.extension().wstring()), ".vpk")) AddArchive(path);
        }
        OpenPath(paths.front());
        SyncSources();
    }

    void OpenPath(const std::filesystem::path& path) {
        const std::string name = Narrow(path.wstring());
        if (skeleton_repair::EqualsIgnoreCase(Narrow(path.extension().wstring()), ".vpk")) {
            OpenMod(AddArchive(path));
            SyncSources();
            return;
        }
        Mod.reset();
        Models.clear();
        Selected.clear();
        ActiveMod.clear();
        StandaloneModel = true;
        try {
            Panel.Load(DecodeModelGeometry(ReadFile(path)), name);
        } catch (const std::exception& error) {
            Panel.ShowError(name, error.what());
        }
        Compare.ShowError(name, "Comparison needs a model opened from a mod VPK.");
    }

    std::optional<std::vector<uint8_t>> ReadFrom(const std::optional<VpkArchive>& archive, const std::string& path) const {
        if (!archive) {
            return std::nullopt;
        }
        const VpkEntry* entry = FindEntry(*archive, path);
        if (entry == nullptr) {
            return std::nullopt;
        }
        return archive->Read(*entry);
    }

    // How the game resolves a mod's files: the mod archive overrides the game's.
    std::optional<std::vector<uint8_t>> ReadModThenGame(const std::string& path) const {
        if (auto bytes = ReadFrom(Mod, path)) {
            return bytes;
        }
        return ReadFrom(Game, path);
    }

    // Clips in the model's folder (".../clips/", ".../clip/"), from the mod and the game.
    std::vector<std::string> ClipsFor(const std::string& model_path) const {
        const size_t slash = model_path.rfind('/');
        const std::string folder = slash == std::string::npos ? std::string() : model_path.substr(0, slash + 1);
        std::vector<std::string> clips;
        for (const std::optional<VpkArchive>* archive : {&Mod, &Game}) {
            if (!*archive) {
                continue;
            }
            for (const VpkEntry& entry : (*archive)->Entries()) {
                if (entry.FullPath.size() > folder.size() && entry.FullPath.compare(0, folder.size(), folder) == 0
                    && EndsWith(entry.FullPath, ".vnmclip_c")
                    && std::find(clips.begin(), clips.end(), entry.FullPath) == clips.end()) {
                    clips.push_back(entry.FullPath);
                }
            }
        }
        return clips;
    }

    void Select(const std::string& path) {
        Selected = path;
        const VpkEntry* entry = Mod ? FindEntry(*Mod, path) : nullptr;
        if (entry == nullptr) {
            return;
        }
        std::vector<uint8_t> bytes;
        try {
            bytes = Mod->Read(*entry);
            const ResourceReader read = [this](const std::string& resource) { return ReadModThenGame(resource); };
            const ModelGeometry geometry = DecodeModelGeometry(bytes);
            std::optional<std::vector<GeometryBone>> animation;
            if (const std::optional<std::string> skeleton = AnimationSkeletonPath(geometry)) {
                if (const auto skeleton_bytes = ReadModThenGame(*skeleton)) {
                    try {
                        animation = DecodeAnimationSkeleton(*skeleton_bytes);
                    } catch (const std::exception&) {
                        // Shown as "no animation skeleton"; the comparison view reports the decode error.
                    }
                }
            }
            Panel.Load(geometry, path, &read, animation ? &*animation : nullptr, ClipsFor(path));
        } catch (const std::exception& error) {
            Panel.ShowError(path, error.what());
        }
        if (!Game) {
            Compare.ShowError(path, "Set the game folder to compare this model with the installed game.");
            return;
        }
        try {
            Compare.Load(
                BuildModelVersions(Sources(path, std::move(bytes))),
                [this](ModelVersion version, const std::string& resource) {
                    return version == ModelVersion::Game ? ReadFrom(Game, resource) : ReadModThenGame(resource);
                },
                ClipsFor(path));
        } catch (const std::exception& error) {
            Compare.ShowError(path, error.what());
        }
    }

    ModelSources Sources(const std::string& path, std::vector<uint8_t> bytes) const {
        ModelSources sources;
        sources.ModelPath = path;
        std::optional<std::string> skeleton;
        try {
            skeleton = AnimationSkeletonPath(DecodeModelGeometry(bytes));
        } catch (const std::exception&) {
            // The comparison view reports the decode error itself.
        }
        sources.ModModel = std::move(bytes);
        if (const VpkEntry* game_model = FindEntry(*Game, path)) {
            sources.GameModelPath = game_model->FullPath;
            sources.GameModel = Game->Read(*game_model);
            if (!skeleton) {
                try {
                    skeleton = AnimationSkeletonPath(DecodeModelGeometry(*sources.GameModel));
                } catch (const std::exception&) {
                }
            }
        }
        if (skeleton) {
            sources.SkeletonPath = *skeleton;
            if (const VpkEntry* mod_skeleton = FindEntry(*Mod, *skeleton)) {
                sources.ModSkeleton = Mod->Read(*mod_skeleton);
            }
            if (const VpkEntry* game_skeleton = FindEntry(*Game, *skeleton)) {
                sources.GameSkeleton = Game->Read(*game_skeleton);
            }
        }
        return sources;
    }

    void DrawBrowser(HWND hwnd) {
        ImGui::Begin("Library", nullptr, ImGuiWindowFlags_NoCollapse);
        ImGui::TextColored({.45f, .81f, .91f, 1.f}, "DEADLOCK");
        ImGui::TextUnformatted("Model Tools");
        ImGui::TextDisabled("View / Compare / Repair");
        ImGui::Spacing();
        ImGui::SeparatorText("Installed game");
        if (Game) {
            ImGui::TextColored({.47f, .85f, .67f, 1.f}, "Connected");
            ImGui::TextWrapped("%s", GameName.c_str());
        } else if (!GameError.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", GameError.c_str());
        } else {
            ImGui::TextDisabled("Not set. Comparisons need the installed game.");
        }
        ImGui::BeginDisabled(Repair.Busy());
        if (ImGui::Button("Choose game archive...", {-1, 32})) {
            const auto paths = AskForFiles(hwnd, L"Game archive (pak01_dir.vpk)\0pak01_dir.vpk\0VPK archives (*.vpk)\0*.vpk\0",
                    L"Select game\\citadel\\pak01_dir.vpk");
            if (!paths.empty()) {
                OpenGame(paths.front());
            }
        }

        ImGui::SeparatorText("Mod library");
        if (ImGui::Button("Add VPKs or model...", {-1, 36})) {
            AddPaths(AskForFiles(hwnd, L"Mod VPKs and models (*.vpk;*.vmdl_c)\0*.vpk;*.vmdl_c\0All files\0*.*\0",
                L"Add mod archives or a compiled model (Ctrl / Shift to select multiple)", true));
        }
        if (!ModPaths.empty()) {
            ImGui::TextDisabled("%zu archives in library", ModPaths.size());
            const std::string active = ActiveMod.empty() ? "Choose an archive" : Narrow(ActiveMod.filename().wstring());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##archives", active.c_str())) {
                for (const auto& path : ModPaths) {
                    if (ImGui::Selectable(Narrow(path.filename().wstring()).c_str(), path == ActiveMod)) OpenMod(path);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Narrow(path.wstring()).c_str());
                }
                ImGui::EndCombo();
            }
            if (ImGui::Button("Clear library", {-1, 0})) {
                Mod.reset(); Models.clear(); Selected.clear(); ActiveMod.clear(); ModPaths.clear();
                StandaloneModel = false;
                Panel.ShowError("", ""); Compare.ShowError("", ""); SyncSources();
            }
        }
        ImGui::EndDisabled();
        if (Mod) {
            ImGui::SeparatorText("Models");
            ImGui::TextDisabled("%zu models in this archive", Models.size());
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##filter", "Filter", Filter, sizeof(Filter));

            std::vector<const VpkEntry*> shown;
            for (const VpkEntry* entry : Models) {
                if (Filter[0] == '\0' || entry->FullPath.find(Filter) != std::string::npos) {
                    shown.push_back(entry);
                }
            }
            std::optional<std::string> clicked;
            if (ImGui::BeginChild("models", {0, -50})) {
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(shown.size()));
                while (clipper.Step()) {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                        const VpkEntry* entry = shown[static_cast<size_t>(i)];
                        const auto label = Narrow(std::filesystem::path(Widen(entry->FullPath)).stem().wstring());
                        ImGui::BeginDisabled(Repair.Busy());
                        if (ImGui::Selectable((label + "##" + entry->FullPath).c_str(), entry->FullPath == Selected)) {
                            clicked = entry->FullPath;
                        }
                        ImGui::EndDisabled();
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", entry->FullPath.c_str());
                    }
                }
            }
            ImGui::EndChild();
            if (clicked) {
                Select(*clicked);
            }
        }
        else {
            ImGui::Spacing();
            ImGui::TextWrapped("Drop a mod VPK here to get started. Choose a model to view its textures, play animation clips, or compare its rig.");
        }
        ImGui::TextDisabled("Drag: orbit   /   Scroll: zoom");
        ImGui::TextDisabled("Middle / right drag: pan");
        ImGui::End();
    }
};

void ApplyTheme(float scale) {
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowPadding = {18, 16};
    style.FramePadding = {10, 7};
    style.ItemSpacing = {10, 9};
    style.WindowRounding = 0;
    style.ChildRounding = 8;
    style.FrameRounding = 6;
    style.PopupRounding = 8;
    style.TabRounding = 6;
    style.ScrollbarSize = 12;
    style.WindowBorderSize = 0;
    style.FrameBorderSize = 0;
    auto* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = {.075f, .09f, .12f, 1.f};
    colors[ImGuiCol_ChildBg] = {.06f, .075f, .10f, 1.f};
    colors[ImGuiCol_Text] = {.90f, .93f, .96f, 1.f};
    colors[ImGuiCol_TextDisabled] = {.53f, .61f, .69f, 1.f};
    colors[ImGuiCol_Border] = {.18f, .23f, .29f, 1.f};
    colors[ImGuiCol_FrameBg] = {.12f, .16f, .21f, 1.f};
    colors[ImGuiCol_FrameBgHovered] = {.17f, .25f, .31f, 1.f};
    colors[ImGuiCol_FrameBgActive] = {.19f, .31f, .38f, 1.f};
    colors[ImGuiCol_Button] = {.13f, .32f, .40f, 1.f};
    colors[ImGuiCol_ButtonHovered] = {.18f, .45f, .54f, 1.f};
    colors[ImGuiCol_ButtonActive] = {.21f, .53f, .61f, 1.f};
    colors[ImGuiCol_Header] = {.14f, .27f, .34f, 1.f};
    colors[ImGuiCol_HeaderHovered] = {.19f, .37f, .44f, 1.f};
    colors[ImGuiCol_HeaderActive] = {.21f, .45f, .52f, 1.f};
    colors[ImGuiCol_Tab] = {.10f, .14f, .19f, 1.f};
    colors[ImGuiCol_TabSelected] = {.16f, .32f, .39f, 1.f};
    colors[ImGuiCol_TabHovered] = {.20f, .42f, .50f, 1.f};
    colors[ImGuiCol_CheckMark] = {.45f, .81f, .91f, 1.f};
    colors[ImGuiCol_SliderGrab] = colors[ImGuiCol_CheckMark];
    colors[ImGuiCol_Separator] = {.18f, .23f, .29f, 1.f};
    style.ScaleAllSizes(scale);
}

int Run(const std::vector<std::filesystem::path>& args) {
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"DeadlockModelPreview", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"Deadlock Model Tools", WS_OVERLAPPEDWINDOW, 100, 100, 1440, 960, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) throw std::runtime_error("Could not create the model tools window.");

    SetupVulkan();
    VkWin32SurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surface_info.hwnd = hwnd;
    surface_info.hinstance = wc.hInstance;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    CheckVk(vkCreateWin32SurfaceKHR(g_instance, &surface_info, nullptr, &surface));
    RECT client{};
    GetClientRect(hwnd, &client);
    SetupWindow(surface, client.right - client.left, client.bottom - client.top);
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;
    const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    ApplyTheme(scale);
    wchar_t windows_folder[MAX_PATH]{};
    GetWindowsDirectoryW(windows_folder, MAX_PATH);
    const auto font = std::filesystem::path(windows_folder) / L"Fonts" / L"segoeui.ttf";
    if (std::filesystem::exists(font)) io.Fonts->AddFontFromFileTTF(Narrow(font.wstring()).c_str(), 17.f * scale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = VK_API_VERSION_1_3;
    init.Instance = g_instance;
    init.PhysicalDevice = g_device.Physical;
    init.Device = g_device.Device;
    init.QueueFamily = g_device.QueueFamily;
    init.Queue = g_device.Queue;
    init.DescriptorPool = g_descriptor_pool;
    init.RenderPass = g_window.RenderPass;
    init.MinImageCount = kMinImageCount;
    init.ImageCount = g_window.ImageCount;
    init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.CheckVkResultFn = CheckVk;
    ImGui_ImplVulkan_Init(&init);

    int exit_code = 0;
    {
        App app(g_device);
        std::optional<std::filesystem::path> game_folder;
        std::optional<std::string> clip;
        std::vector<std::filesystem::path> positional;
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == L"--game" && i + 1 < args.size()) {
                game_folder = args[++i];
            } else if (args[i] == L"--clip" && i + 1 < args.size()) {
                clip = VpkArchive::Normalize(Narrow(args[++i].wstring()));
            } else {
                positional.push_back(args[i]);
            }
        }
        if (!game_folder) {
            try {
                if (const auto detected = FindGameFolder()) game_folder = Widen(*detected);
            } catch (const std::exception& error) {
                app.GameError = error.what();
            }
        }
        if (game_folder) {
            app.OpenGame(*game_folder);
        }
        if (!positional.empty()) {
            app.OpenPath(positional[0]);
            if (positional.size() > 1 && app.Mod && EndsWith(Narrow(positional[1].wstring()), ".vmdl_c")) {
                app.Select(VpkArchive::Normalize(Narrow(positional[1].wstring())));
                if (clip) {
                    app.Panel.PlayClip(*clip);
                    app.Compare.PlayClip(*clip);
                }
            } else {
                for (size_t i = 1; i < positional.size(); ++i) app.OpenPath(positional[i]);
            }
        }

        bool done = false;
        bool first_frame = true;
        while (!done) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                done = done || msg.message == WM_QUIT;
            }
            if (done) {
                app.Repair.Cancel();
                break;
            }
            if (IsIconic(hwnd)) {
                Sleep(10);
                continue;
            }
            if (g_swapchain_rebuild) {
                RECT rect{};
                GetClientRect(hwnd, &rect);
                const int width = rect.right - rect.left;
                const int height = rect.bottom - rect.top;
                if (width > 0 && height > 0) {
                    ImGui_ImplVulkan_SetMinImageCount(kMinImageCount);
                    ImGui_ImplVulkanH_CreateOrResizeWindow(g_instance, g_device.Physical, g_device.Device, &g_window,
                        g_device.QueueFamily, nullptr, width, height, kMinImageCount);
                    g_window.FrameIndex = 0;
                    g_swapchain_rebuild = false;
                }
            }

            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            if (app.Repair.Poll() && !app.ActiveMod.empty()) {
                const auto path = app.ActiveMod;
                const auto selected = app.Selected;
                app.OpenMod(path);
                if (!selected.empty()) app.Select(selected);
            }
            if (!g_dropped.empty()) {
                if (!app.Repair.Busy()) app.AddPaths(g_dropped);
                g_dropped.clear();
            }

            const ImGuiID dockspace = ImGui::DockSpaceOverViewport();
            if (first_frame) {
                // Browser on the left, preview filling the rest.
                ImGui::DockBuilderRemoveNode(dockspace);
                ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
                ImGuiID right = 0;
                const ImGuiID left = ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Left, 0.24f, nullptr, &right);
                ImGui::DockBuilderDockWindow("Library", left);
                ImGui::DockBuilderDockWindow("Workspace", right);
                ImGui::DockBuilderFinish(dockspace);
                first_frame = false;
            }

            app.DrawBrowser(hwnd);
            ImGui::Begin("Workspace", nullptr, ImGuiWindowFlags_NoCollapse);
            if (ImGui::BeginTabBar("tools", ImGuiTabBarFlags_FittingPolicyResizeDown)) {
                if (ImGui::BeginTabItem("View model")) {
                    if (app.Selected.empty() && app.ActiveMod.empty() && !app.StandaloneModel) {
                        ImGui::Spacing();
                        ImGui::SeparatorText("Explore your models");
                        ImGui::TextWrapped("Add or drop a mod VPK in the library. The first model opens automatically, with textures and animation controls when available.");
                        ImGui::Spacing();
                        ImGui::TextDisabled("Compare: mod, installed game and proposed repair");
                        ImGui::TextDisabled("Repair: analyze archives, apply supported fixes and save a report");
                    }
                    app.Panel.Draw();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Compare versions")) {
                    app.Compare.Draw();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Repair mods")) {
                    app.Repair.Draw(hwnd, [&] {
                        app.Mod.reset();
                        app.Models.clear();
                    });
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::End();

            ImGui::Render();
            ImDrawData* draw_data = ImGui::GetDrawData();
            if (draw_data->DisplaySize.x > 0 && draw_data->DisplaySize.y > 0) {
                g_window.ClearValue.color = {{0.06f, 0.06f, 0.07f, 1.0f}};
                FrameRender(draw_data, [&](VkCommandBuffer command_buffer) {
                    app.Panel.Record(command_buffer);
                    app.Compare.Record(command_buffer);
                });
                FramePresent();
            }
        }
        CheckVk(vkDeviceWaitIdle(g_device.Device));
    }

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ImGui_ImplVulkanH_DestroyWindow(g_instance, g_device.Device, &g_window, nullptr);
    vkDestroyDescriptorPool(g_device.Device, g_descriptor_pool, nullptr);
    vkDestroyDevice(g_device.Device, nullptr);
    vkDestroyInstance(g_instance, nullptr);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return exit_code;
}

}  // namespace

int skeleton_repair::RunModelTools(const std::vector<std::filesystem::path>& args) {
    try {
        return Run(args);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        MessageBoxA(nullptr, error.what(), "Deadlock Model Tools", MB_ICONERROR);
        return 1;
    }
}
