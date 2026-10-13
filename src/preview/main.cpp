// Model preview tool: opens a .vmdl_c or a VPK and shows the decoded model with PreviewPanel.
// Window and swapchain setup follow Dear ImGui's example_win32_vulkan.

#include "profile.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

// Before any vulkan.h include, so the Win32 surface API is declared.
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "model_geometry.hpp"
#include "model_renderer.hpp"
#include "comparison_view.hpp"
#include "multi_view.hpp"
#include "history_panel.hpp"
#include "version_builder.hpp"
#include "preview_panel.hpp"
#include "strings.hpp"
#include "vpk.hpp"
#include "game_folder.hpp"
#include "model_tools.hpp"
#include "repair_panel.hpp"
#include "library_panel.hpp"
#include "mod_store.hpp"
#include "mods_panel.hpp"
#include "gamebanana_panel.hpp"
#include "archive_extract.hpp"
#include "product_version.hpp"
#include "startup.hpp"
#include "background.hpp"
#include "theme.hpp"
#include "ui.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
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
#include <future>
#include <atomic>
#include <chrono>
#include <utility>

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
// OS window rectangle. Separate from layout.ini so View > Reset layout does not move the window.
std::filesystem::path g_window_ini;

struct ShellPlacement {
    int x = 100;
    int y = 100;
    int width = 1440;
    int height = 960;
    bool maximized = false;
};

RECT PrimaryWorkOrigin() {
    RECT work{0, 0, 0, 0};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    return work;
}

// `screen` is in screen coordinates. A sliver on a monitor counts as visible.
bool RectMeetsMonitor(RECT screen) {
    struct Probe {
        RECT target;
        bool hit = false;
    } probe{screen};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, LPRECT monitor, LPARAM param) -> BOOL {
        auto* probe = reinterpret_cast<Probe*>(param);
        RECT overlap{};
        if (IntersectRect(&overlap, &probe->target, monitor)
            && overlap.right - overlap.left >= 80 && overlap.bottom - overlap.top >= 48) {
            probe->hit = true;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&probe));
    return probe.hit;
}

ShellPlacement LoadShellPlacement(const std::filesystem::path& path) {
    ShellPlacement place;
    std::ifstream in(path);
    int maximized = 0;
    if (!in || !(in >> place.x >> place.y >> place.width >> place.height >> maximized)) return {};
    if (place.width < 640 || place.height < 480) return {};
    const RECT work = PrimaryWorkOrigin();
    const RECT screen{place.x + work.left, place.y + work.top,
        place.x + work.left + place.width, place.y + work.top + place.height};
    if (!RectMeetsMonitor(screen)) return {};
    place.maximized = maximized != 0;
    return place;
}

void SaveShellPlacement(HWND hwnd) {
    if (g_window_ini.empty() || hwnd == nullptr) return;
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (!GetWindowPlacement(hwnd, &placement)) return;
    const int width = placement.rcNormalPosition.right - placement.rcNormalPosition.left;
    const int height = placement.rcNormalPosition.bottom - placement.rcNormalPosition.top;
    if (width < 640 || height < 480) return;
    std::ofstream out(g_window_ini, std::ios::trunc);
    if (!out) return;
    out << placement.rcNormalPosition.left << ' ' << placement.rcNormalPosition.top << ' '
        << width << ' ' << height << ' ' << (placement.showCmd == SW_SHOWMAXIMIZED ? 1 : 0) << '\n';
}

void ShowStartupFailure(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    MessageBoxW(nullptr, Widen(message).c_str(), SR_PRODUCT_TITLE_W, MB_ICONERROR | MB_SETFOREGROUND);
}

std::wstring EnvironmentValue(const wchar_t* name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed <= 1) return {};
    std::wstring value(static_cast<size_t>(needed), L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0 || written >= needed) return {};
    value.resize(written);
    return value;
}

bool ShellWindowInSettings(ImGuiID id) {
    ImGuiContext& context = *ImGui::GetCurrentContext();
    for (ImGuiWindowSettings* settings = context.SettingsWindows.begin(); settings != nullptr;
        settings = context.SettingsWindows.next_chunk(settings)) {
        if (settings->ID == id) return true;
    }
    return false;
}

// A previous session stored Explorer or Workspace. A missing file, or one without those windows, is not a layout.
bool HasSavedShellLayout() {
    return ShellWindowInSettings(ImHashStr("Explorer")) || ShellWindowInSettings(ImHashStr("Workspace"));
}

void BuildDefaultDock(ImGuiID dockspace) {
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
    ImGuiID right = 0;
    const ImGuiID left = ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Left, 0.17f, nullptr, &right);
    ImGui::DockBuilderDockWindow("Explorer", left);
    ImGui::DockBuilderDockWindow("Workspace", right);
    ImGui::DockBuilderFinish(dockspace);
    ImGui::MarkIniSettingsDirty();
}

bool OverlapsWorkArea(ImVec2 min, ImVec2 max) {
    const ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    if (platform.Monitors.Size == 0) return true;
    for (int i = 0; i < platform.Monitors.Size; ++i) {
        const ImGuiPlatformMonitor& monitor = platform.Monitors[i];
        const float left = std::max(min.x, monitor.WorkPos.x);
        const float top = std::max(min.y, monitor.WorkPos.y);
        const float right = std::min(max.x, monitor.WorkPos.x + monitor.WorkSize.x);
        const float bottom = std::min(max.y, monitor.WorkPos.y + monitor.WorkSize.y);
        if (right - left >= 32.f && bottom - top >= 32.f) return true;
    }
    return false;
}

// Floating shell windows only. Docked windows stay inside the workspace.
void BringShellOnScreen(const char* name, ImVec2 offset) {
    ImGuiWindow* window = ImGui::FindWindowByName(name);
    if (window == nullptr || window->DockIsActive) return;
    const ImVec2 corner(window->Pos.x + window->Size.x, window->Pos.y + window->Size.y);
    if (OverlapsWorkArea(window->Pos, corner)) return;
    const ImVec2 work = ImGui::GetMainViewport()->WorkPos;
    ImGui::SetWindowPos(name, ImVec2(work.x + offset.x, work.y + offset.y), ImGuiCond_Always);
    ImGui::MarkIniSettingsDirty(window);
}

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
    app.pApplicationName = SR_PRODUCT_NAME;
    app.applicationVersion = VK_MAKE_API_VERSION(0, SR_PRODUCT_VERSION_MAJOR, SR_PRODUCT_VERSION_MINOR, SR_PRODUCT_VERSION_PATCH);
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

    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE + 512};
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
    ZoneScopedN("GPU submit");
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
    ZoneScopedN("Present");
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
        SaveShellPlacement(hwnd);
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

std::optional<std::filesystem::path> AskForFolder(HWND owner, const wchar_t* title) {
    BROWSEINFOW browse{};
    browse.hwndOwner = owner;
    browse.lpszTitle = title;
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
    if (item == nullptr) return std::nullopt;
    wchar_t buffer[MAX_PATH] = {};
    const BOOL found = SHGetPathFromIDListW(item, buffer);
    ILFree(item);
    if (!found || buffer[0] == L'\0') return std::nullopt;
    return std::filesystem::path(buffer);
}

// Accepts the game/citadel folder or its pak01_dir.vpk, like the updater's game directory.
std::filesystem::path GamePak(const std::filesystem::path& chosen) {
    if (EndsWith(Narrow(chosen.wstring()), ".vpk")) {
        return chosen;
    }
    return chosen / L"pak01_dir.vpk";
}

using ArchivePtr = std::shared_ptr<const IndexedArchive>;

// The game directory is huge, so both the open and the clip list stay off the UI thread.
struct LoadedGame {
    ArchivePtr Archive;
    std::vector<std::string> Clips;
};

// How the game resolves a mod's files: the mod archive overrides the game's.
// The reader holds its own archive references, so worker threads can use it while the UI changes archives.
ResourceReader ModThenGame(ArchivePtr mod, ArchivePtr game) {
    return [mod = std::move(mod), game = std::move(game)](const std::string& path) -> std::optional<std::vector<uint8_t>> {
        if (mod) {
            if (auto bytes = mod->Read(path)) return bytes;
        }
        return game ? game->Read(path) : std::nullopt;
    };
}

ComparisonView::VersionReader VersionReaderFor(ArchivePtr mod, ArchivePtr game) {
    return [read = ModThenGame(mod, game), game](ModelVersion version, const std::string& path) -> std::optional<std::vector<uint8_t>> {
        if (version == ModelVersion::Game) return game ? game->Read(path) : std::nullopt;
        return read(path);
    };
}

// Clips in the model's folder (".../clips/", ".../clip/"), from the mod and the game.
std::vector<std::string> ClipsFor(const std::string& model_path, const IndexedArchive* mod, const IndexedArchive* game) {
    const size_t slash = model_path.rfind('/');
    const std::string folder = slash == std::string::npos ? std::string() : model_path.substr(0, slash + 1);
    std::vector<std::string> clips;
    for (const IndexedArchive* archive : {mod, game}) {
        if (archive == nullptr) {
            continue;
        }
        for (const VpkEntry& entry : archive->Archive().Entries()) {
            if (entry.FullPath.size() > folder.size() && entry.FullPath.compare(0, folder.size(), folder) == 0
                && EndsWith(entry.FullPath, ".vnmclip_c")
                && std::find(clips.begin(), clips.end(), entry.FullPath) == clips.end()) {
                clips.push_back(entry.FullPath);
            }
        }
    }
    return clips;
}

ModelSources Sources(const std::string& path, std::vector<uint8_t> bytes, const IndexedArchive& mod, const IndexedArchive& game) {
    ModelSources sources;
    sources.ModelPath = path;
    std::optional<std::string> skeleton;
    try {
        skeleton = AnimationSkeletonPath(DecodeModelGeometry(bytes));
    } catch (const std::exception&) {
        // The comparison view reports the decode error itself.
    }
    sources.ModModel = std::move(bytes);
    if (const VpkEntry* game_model = game.Find(path)) {
        sources.GameModelPath = game_model->FullPath;
        sources.GameModel = game.Archive().Read(*game_model);
        if (!skeleton) {
            try {
                skeleton = AnimationSkeletonPath(DecodeModelGeometry(*sources.GameModel));
            } catch (const std::exception&) {
            }
        }
    }
    if (skeleton) {
        sources.SkeletonPath = *skeleton;
        sources.ModSkeleton = mod.Read(*skeleton);
        sources.GameSkeleton = game.Read(*skeleton);
    }
    return sources;
}

// Everything the inspector shows for one model, decoded on a worker thread.
struct Inspection {
    ArchivePtr Mod;
    ArchivePtr Game;
    std::string Model;
    std::string Title;
    std::optional<PreparedModel> Preview;
    std::string PreviewError;
    std::optional<std::vector<GeometryBone>> Animation;
    std::vector<std::string> Clips;
    std::optional<ComparisonView::Prepared> Comparison;
    std::string ComparisonError;
};

// An empty `model` picks the archive's first model.
Inspection InspectArchiveModel(ArchiveCache& archives, const std::filesystem::path& archive, std::string model, ArchivePtr game,
    CancellationToken cancellation) {
    Inspection result;
    result.Game = game;
    result.Title = Narrow(archive.filename().wstring());
    try {
        result.Mod = archives.Get(archive);
    } catch (const std::exception& error) {
        result.Title = Narrow(archive.wstring());
        result.PreviewError = result.ComparisonError = error.what();
        return result;
    }
    if (model.empty()) {
        for (const VpkEntry& entry : result.Mod->Archive().Entries()) {
            if (EndsWith(entry.FullPath, ".vmdl_c") && (model.empty() || entry.FullPath < model)) model = entry.FullPath;
        }
        if (model.empty()) {
            result.PreviewError = "This archive contains no compiled models.";
            return result;
        }
    }
    result.Model = model;
    result.Title = model;
    const VpkEntry* entry = result.Mod->Find(model);
    if (entry == nullptr) {
        result.PreviewError = result.ComparisonError = "This model is no longer in " + Narrow(archive.filename().wstring()) + ".";
        return result;
    }
    const ResourceReader read = ModThenGame(result.Mod, game);
    std::vector<uint8_t> bytes;
    try {
        bytes = result.Mod->Archive().Read(*entry, cancellation);
        const ModelGeometry geometry = DecodeModelGeometry(bytes);
        if (const std::optional<std::string> skeleton = AnimationSkeletonPath(geometry)) {
            if (const auto skeleton_bytes = read(*skeleton)) {
                try {
                    result.Animation = DecodeAnimationSkeleton(*skeleton_bytes);
                } catch (const std::exception&) {
                    // Shown as "no animation skeleton"; the comparison view reports the decode error.
                }
            }
        }
        result.Clips = ClipsFor(model, result.Mod.get(), game.get());
        cancellation.ThrowIfCancelled();
        result.Preview = PrepareModel(geometry, &read);
    } catch (const std::exception& error) {
        result.PreviewError = error.what();
    }
    if (!game) {
        result.ComparisonError = "Set the game folder to compare this model with the installed game.";
        return result;
    }
    try {
        cancellation.ThrowIfCancelled();
        result.Comparison = ComparisonView::Prepare(BuildModelVersions(Sources(model, std::move(bytes), *result.Mod, *game)),
            VersionReaderFor(result.Mod, game), result.Preview ? &*result.Preview : nullptr);
    } catch (const std::exception& error) {
        result.ComparisonError = error.what();
    }
    return result;
}

Inspection InspectModelFile(const std::filesystem::path& path) {
    Inspection result;
    result.Title = Narrow(path.wstring());
    try {
        result.Preview = PrepareModel(DecodeModelGeometry(ReadFile(path)));
    } catch (const std::exception& error) {
        result.PreviewError = error.what();
    }
    result.ComparisonError = "Comparison needs a model opened from a mod VPK.";
    return result;
}

// Archives, the model list, and the two views that show the selection.
// Archives are opened and models decoded on worker threads; this thread only applies finished results and uploads them.
struct App {
    enum class Page { Gallery, Pack, History, Inspect, Repair, Multi, Mods, Browse };
    Page CurrentPage = Page::Gallery;
    Page PreviousPage = Page::Gallery;
    // Set while a page shown from code waits for its tab to become selected.
    bool PageRequested = true;
    bool ShowExplorer = true;
    bool FocusExplorer = false;
    bool FocusWorkspace = false;
    bool ResetLayout = false;
    bool ShowStatusBar = true;
    bool OpenShortcuts = false;
    bool OpenAbout = false;
    char SourceFilter[128] = {};
    std::shared_ptr<ArchiveCache> Archives = std::make_shared<ArchiveCache>();
    PreviewPanel Panel;
    ComparisonView Compare;
    MultiView Multi;
    // Set when a repair or rename drops viewport uploads. The next finished import reloads them.
    bool MultiSuspended = false;
    ArchivePtr Mod;
    std::string ModName;
    std::string Selected;
    ArchivePtr Game;
    std::string GameName;
    std::string GameError;
    std::filesystem::path GameLoading;
    LatestTask<LoadedGame> GameLoader;
    // Parent of the connected game archive. Filled when the archive finishes opening.
    std::string GameDirectory;
    // Last library revision and game path written by Persist. Snapshotting the pack every frame is not required.
    uint64_t saved_revision_ = 0;
    std::filesystem::path saved_game_;
    LatestTask<Inspection> Inspector;
    std::optional<std::string> PendingClip;
    // A model to inspect once its archive has been imported, from the command line.
    std::optional<std::pair<std::filesystem::path, std::string>> InspectAfterImport;
    RepairPanel Repair;
    LibraryPanel Builder;
    HistoryPanel History;
    ModsPanel Mods;
    GameBananaPanel Banana;
    // Animation clips in the game archive, which history previews also offer.
    std::vector<std::string> GameClips;
    // Shows the comparison tab of the inspector on the next frame.
    bool FocusCompare = false;
    std::vector<std::filesystem::path> ModPaths;
    std::filesystem::path ActiveMod;
    bool StandaloneModel = false;
    std::filesystem::path DataRoot;
    std::filesystem::path LayoutIni;
    char ManualGame[2048] = {};
    std::string SettingsGame;
    bool auto_detect_done_ = false;
    std::string StartupNotice;
    std::string LastError;
    std::string LastLogged;
    bool StoreStatusIsError = false;
    bool OpenQuickStart = false;
    bool OpenExport = false;
    std::optional<AssembledPack> PendingExport;
    std::optional<LocalModStore> Store;
    std::filesystem::path ConfiguredGame;
    // Set when the game was chosen on the command line, so the saved game does not replace it.
    bool KeepGame = false;
    std::optional<ModelLibrary> SavedState;
    // Set once the saved library is restored, so a failed or cancelled restore never overwrites it.
    bool CanPersist = false;
    bool Restoring = false;
    // Imports requested while the library was being restored.
    ModelLibrary QueuedImports;
    std::string StoreStatus;
    std::string SaveError;
    double LastSaveAttempt = -10;
    struct ImportResult {
        ModelLibrary Library;
        std::string Errors;
        // Each source with its stored copy.
        std::vector<std::pair<std::filesystem::path, std::filesystem::path>> Imported;
        // The library was read back from the database, which now matches it.
        bool Restored = false;
        // Shown when the work finished without errors, instead of the import message.
        std::string Done;
        // A GameBanana download or a pack built as a mod: the content ids of its stored mods, where they came from,
        // and whether to enable them. A rebuilt mod replaces ReplacedId on the Mods page.
        std::vector<std::string> AddedIds;
        std::optional<ModOrigin> Origin;
        bool Enable = false;
        std::string ReplacedId;
    };
    // The Build as mod prompt.
    bool OpenBuildMod = false;
    char BuildModName[101] = "My pack";
    bool BuildModEnable = true;
    // The source being renamed, and the name being typed.
    std::filesystem::path RenameTarget;
    char RenameText[201] = {};
    bool OpenRename = false;
    // A confirmed rename, started before the next frame draws, because it frees thumbnails this frame already drew.
    std::optional<std::pair<std::filesystem::path, std::string>> PendingRename;
    std::future<ImportResult> ImportWorker;
    CancellationSource ImportCancellation;
    std::atomic<size_t> ImportedCount = 0;
    std::atomic<size_t> ImportTotal = 0;
    // Set while the import worker is downloading from GameBanana.
    bool Downloading = false;
    bool BuildingMod = false;
    std::string DownloadName;
    std::atomic<uint64_t> DownloadBytes = 0;
    std::atomic<uint64_t> DownloadTotal = 0;

    bool ImportBusy() const { return ImportWorker.valid(); }
    // Work that reads or replaces the library's stored mods. Applying mods to the game reads them too.
    bool LibraryWorkBusy() const { return Repair.Busy() || Builder.Busy() || History.Busy() || ImportBusy(); }
    bool FilesBusy() const { return LibraryWorkBusy() || Mods.Busy(); }
    bool LibraryReady() const { return !Restoring; }

    void RememberGamePath(const std::filesystem::path& path) {
        const std::string text = Narrow(path.wstring());
        std::snprintf(ManualGame, sizeof(ManualGame), "%s", text.c_str());
    }

    void ReportError(const std::string& message) {
        if (message.empty()) return;
        LastError = message;
        if (message == LastLogged) return;
        LastLogged = message;
        AppendErrorLog(DataPathsFor(DataRoot).LogFile, DiagnosticText(SR_PRODUCT_TITLE, SR_GIT_COMMIT, DataRoot, message));
    }

    void NoteStartupError(const std::string& message) {
        if (message.empty()) return;
        StartupNotice = message;
        LastError = message;
        LastLogged = message;
    }

    void SetStoreStatus(std::string text, bool error) {
        StoreStatus = std::move(text);
        StoreStatusIsError = error;
        if (error) ReportError(StoreStatus);
    }

    void MaybeAutoDetect() {
        if (auto_detect_done_) return;
        if (KeepGame || !ConfiguredGame.empty()) {
            auto_detect_done_ = true;
            return;
        }
        if (Restoring || (Store && !CanPersist)) return;
        if (!SettingsGame.empty()) {
            const std::string saved = std::exchange(SettingsGame, {});
            OpenGame(std::filesystem::path(Widen(saved)));
            auto_detect_done_ = true;
            return;
        }
        auto_detect_done_ = true;
        try {
            if (const auto detected = FindGameFolder()) OpenGame(Widen(*detected));
        } catch (const std::exception& error) {
            GameError = error.what();
            ReportError(GameError);
        }
    }

    void ConnectManual(HWND hwnd, bool browse) {
        if (!browse && ManualGame[0] != '\0') {
            OpenGame(std::filesystem::path(Widen(std::string(ManualGame))));
            return;
        }
        ChooseGame(hwnd);
    }

    void OpenLogs() {
        const std::filesystem::path log = DataPathsFor(DataRoot).LogFile;
        std::error_code error;
        if (std::filesystem::is_regular_file(log, error)) {
            ui::RevealInExplorer(log);
            return;
        }
        SetStoreStatus("No log file yet. Errors are written to " + Narrow(log.wstring()) + ".", false);
    }

    void CopyDiagnostics() {
        ui::CopyToClipboard(DiagnosticText(SR_PRODUCT_TITLE, SR_GIT_COMMIT, DataRoot, LastError));
    }

    void CollectExportNotice() {
        if (const auto notice = Builder.TakeExportNotice()) {
            PendingExport = *notice;
            OpenExport = true;
        }
        if (const auto notice = History.TakeExportNotice()) {
            PendingExport = *notice;
            OpenExport = true;
        }
    }

    void CloseInspection() {
        Inspector.Cancel();
        Mod.reset(); Selected.clear(); ActiveMod.clear(); StandaloneModel = false;
        Panel.ShowError("", ""); Compare.ShowError("", "");
    }

    void ApplyLibrary(const ModelLibrary& library) {
        ModPaths = library.Sources;
        CloseInspection();
        Builder.Restore(library);
        if (!KeepGame && !library.GameArchive.empty() && library.GameArchive != ConfiguredGame) OpenGame(library.GameArchive);
        SyncSources();
    }

    void NoteSaved() {
        saved_revision_ = Builder.Revision();
        saved_game_ = ConfiguredGame;
    }

    void Persist(bool force = false) {
        ZoneScopedN("Persist library");
        if (!Store || ImportBusy() || !CanPersist) return;
        if (!force && Builder.Revision() == saved_revision_ && ConfiguredGame == saved_game_) return;
        const auto current = Builder.Snapshot(ModPaths, ConfiguredGame);
        if (SavedState && *SavedState == current) { NoteSaved(); return; }
        if (!force && !SaveError.empty() && ImGui::GetTime() - LastSaveAttempt < 5) return;
        LastSaveAttempt = ImGui::GetTime();
        try { Store->Save(current); SavedState = current; SaveError.clear(); NoteSaved(); }
        catch (const std::exception& error) { SaveError = error.what(); ReportError(SaveError); }
    }

    // Reads the saved library from the database, merging the folder-based store of earlier releases on first use.
    void StartRestore() {
        Restoring = true;
        ImportCancellation = CancellationSource{};
        ImportedCount = 0; ImportTotal = 0;
        SetStoreStatus("Opening your library...", false);
        ImportWorker = std::async(std::launch::async, [root = Store->Root(), token = ImportCancellation.Token(),
            completed = &ImportedCount, total = &ImportTotal] {
            ImportResult result;
            result.Library = LocalModStore(root).Load(token, [=](size_t done, size_t all) { *completed = done; *total = all; }, &result.Errors);
            result.Restored = true;
            return result;
        });
    }

    // Adds a shared library database's mods and selections to this library.
    void StartMerge(const std::filesystem::path& other) {
        if (FilesBusy() || !Store) return;
        if (!CanPersist) { SetStoreStatus("Your library has not been opened, so nothing can be added to it yet. " + SaveError, true); return; }
        // The merge appends to the saved collection, so it must match what is on screen.
        Persist(true);
        if (!SaveError.empty()) { SetStoreStatus("Could not save your library before adding another: " + SaveError, true); return; }
        ImportCancellation = CancellationSource{};
        ImportedCount = 0; ImportTotal = 0;
        SetStoreStatus("Adding " + Narrow(other.filename().wstring()) + " to your library...", false);
        ImportWorker = std::async(std::launch::async, [root = Store->Root(), other, token = ImportCancellation.Token(),
            completed = &ImportedCount, total = &ImportTotal] {
            ImportResult result;
            const LocalModStore store(root);
            store.Merge(other, token);
            result.Library = store.Load(token, [=](size_t done, size_t all) { *completed = done; *total = all; }, &result.Errors);
            result.Restored = true;
            return result;
        });
    }

    // Names a source's mod, then reopens the library so its copy carries the new name.
    void StartRename(const std::filesystem::path& archive, const std::string& name) {
        if (FilesBusy() || !Store) return;
        if (!CanPersist) { SetStoreStatus("Your library has not been opened, so nothing can be renamed yet. " + SaveError, true); return; }
        // The reopened library replaces what is on screen, so it must be saved first.
        Persist(true);
        if (!SaveError.empty()) { SetStoreStatus("Could not save your library before renaming: " + SaveError, true); return; }
        ReleaseArchives();
        ImportCancellation = CancellationSource{};
        ImportedCount = 0; ImportTotal = 0;
        SetStoreStatus("Renaming...", false);
        ImportWorker = std::async(std::launch::async, [root = Store->Root(), archive, name, token = ImportCancellation.Token(),
            completed = &ImportedCount, total = &ImportTotal] {
            ImportResult result;
            const LocalModStore store(root);
            store.Rename(archive, name);
            result.Library = store.Load(token, [=](size_t done, size_t all) { *completed = done; *total = all; }, &result.Errors);
            result.Restored = true;
            result.Done = name.empty() ? "The mod's name is derived again." : "Renamed to " + name + ".";
            return result;
        });
    }

    // Call before anything is drawn in the frame.
    void StartPendingRename() {
        if (const auto rename = std::exchange(PendingRename, std::nullopt)) StartRename(rename->first, rename->second);
    }

    void DrawRenamePopup() {
        if (std::exchange(OpenRename, false)) ImGui::OpenPopup("Rename mod");
        ImGui::SetNextWindowSize({ImGui::GetFontSize() * 26, 0}, ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal("Rename mod", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
        ImGui::TextWrapped("The name is shown wherever this mod appears. Leave it empty to use the name derived from the file it was imported from or the models it contains.");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        const bool entered = ImGui::InputText("##name", RenameText, sizeof(RenameText), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::BeginDisabled(FilesBusy());
        if (ImGui::Button("Rename") || (entered && !FilesBusy())) {
            PendingRename.emplace(RenameTarget, RenameText);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    void StartImport(const ModelLibrary& incoming, bool replace) {
        if (Restoring) {
            // Started after the restore, so these are added to the restored collection.
            for (const auto& source : incoming.Sources) QueuedImports.Sources.push_back(source);
            return;
        }
        if (FilesBusy()) return;
        if (!Store) { SetStoreStatus("Local mod storage is unavailable. " + SaveError, true); return; }
        ImportCancellation = CancellationSource{};
        ImportedCount = 0; ImportTotal = incoming.Sources.size();
        SetStoreStatus("Importing mods into local storage...", false);
        auto starting = replace ? incoming : Builder.Snapshot(ModPaths, ConfiguredGame);
        if (replace) starting.Sources.clear();
        ImportWorker = std::async(std::launch::async, [root = Store->Root(), incoming, starting = std::move(starting),
            token = ImportCancellation.Token(), completed = &ImportedCount, checkpoints = CanPersist]() mutable {
            ZoneScopedN("Import mods");
            ImportResult result{std::move(starting), {}, {}};
            LocalModStore store(root);
            for (const auto& source : incoming.Sources) {
                try {
                    const auto cached = store.Import(source, token);
                    result.Imported.emplace_back(source, cached);
                    const auto same = [](const std::filesystem::path& a, const std::filesystem::path& b) {
                        return skeleton_repair::EqualsIgnoreCase(Narrow(std::filesystem::absolute(a).lexically_normal().wstring()),
                            Narrow(std::filesystem::absolute(b).lexically_normal().wstring()));
                    };
                    auto& sources = result.Library.Sources;
                    const bool stored = std::any_of(sources.begin(), sources.end(), [&](const auto& other) { return same(other, cached); });
                    const auto previous = std::find_if(sources.begin(), sources.end(), [&](const auto& other) { return same(other, source); });
                    // A changed copy, such as a repaired one, is stored as a new version that takes its place.
                    if (previous != sources.end() && !same(source, cached)) {
                        if (stored) sources.erase(previous);
                        else *previous = cached;
                    } else if (!stored) sources.push_back(cached);
                    for (auto& model : result.Library.Selected) if (skeleton_repair::EqualsIgnoreCase(
                        Narrow(std::filesystem::absolute(model.Archive).lexically_normal().wstring()),
                        Narrow(std::filesystem::absolute(source).lexically_normal().wstring()))) model.Archive = cached;
                    for (auto& package : result.Library.Packages) if (same(package, source)) package = cached;
                    auto checkpoint = result.Library;
                    const auto listed = [](const std::vector<std::filesystem::path>& paths, const std::filesystem::path& path) {
                        return std::none_of(paths.begin(), paths.end(), [&](const auto& other) {
                            return skeleton_repair::EqualsIgnoreCase(Narrow(other.wstring()), Narrow(path.wstring()));
                        });
                    };
                    std::erase_if(checkpoint.Selected, [&](const auto& model) { return listed(checkpoint.Sources, model.Archive); });
                    std::erase_if(checkpoint.Packages, [&](const auto& package) { return listed(checkpoint.Sources, package); });
                    if (checkpoints) store.Save(checkpoint);
                } catch (const OperationCancelled&) { result.Errors += "Import cancelled. Completed imports were kept.\n"; break; }
                catch (const std::exception& error) { result.Errors += Narrow(source.filename().wstring()) + ": " + error.what() + "\n"; }
                ++*completed;
            }
            const auto listed = [](const std::filesystem::path& path, const std::vector<std::filesystem::path>& paths) {
                return std::none_of(paths.begin(), paths.end(), [&](const auto& other) {
                    return skeleton_repair::EqualsIgnoreCase(Narrow(other.wstring()), Narrow(path.wstring()));
                });
            };
            std::erase_if(result.Library.Selected, [&](const auto& model) { return listed(model.Archive, result.Library.Sources); });
            std::erase_if(result.Library.Packages, [&](const auto& package) { return listed(package, result.Library.Sources); });
            return result;
        });
    }

    // Downloads one file of a GameBanana mod, stores every mod VPK in it under the mod's name, and adds them to the
    // collection. The Mods page records where they came from and, with `enable`, enables them in the current profile.
    void StartDownload(const GbMod& mod, const GbFile& file, bool enable) {
        if (Restoring || FilesBusy()) return;
        if (!Store) { SetStoreStatus("Local mod storage is unavailable. " + SaveError, true); return; }
        if (!CanPersist) { SetStoreStatus("Your library has not been opened, so nothing can be added to it yet. " + SaveError, true); return; }
        // The download is added to the saved collection, so it must match what is on screen.
        Persist(true);
        if (!SaveError.empty()) { SetStoreStatus("Could not save your library before downloading: " + SaveError, true); return; }
        ImportCancellation = CancellationSource{};
        ImportedCount = 0; ImportTotal = 0;
        DownloadBytes = 0; DownloadTotal = file.Size;
        Downloading = true;
        DownloadName = mod.Name;
        SetStoreStatus("Downloading " + mod.Name + "...", false);
        const ModOrigin origin{mod.Id, file.Id, file.Name, mod.ProfileUrl, mod.DateModified};
        ImportWorker = std::async(std::launch::async, [root = Store->Root(), name = mod.Name, file, origin, enable,
            token = ImportCancellation.Token(), bytes = &DownloadBytes, total = &DownloadTotal, completed = &ImportedCount, count = &ImportTotal] {
            ZoneScopedN("Download mod");
            namespace fs = std::filesystem;
            ImportResult result;
            const LocalModStore store(root);
            const auto folder = root / L"downloads" / (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            struct Cleanup { fs::path Path; ~Cleanup() { std::error_code error; fs::remove_all(Path, error); } } cleanup{folder};
            fs::create_directories(folder);
            std::wstring file_name = fs::path(Widen(file.Name)).filename().wstring();
            for (auto& c : file_name) if (c < 32 || std::wstring_view(L"<>:\"/\\|?*").find(c) != std::wstring_view::npos) c = L'_';
            if (file_name.empty() || file_name == L"." || file_name == L"..") file_name = L"download";
            const auto archive = folder / file_name;
            gamebanana::Download(file, archive, token, [=](uint64_t done, std::optional<uint64_t> all) { *bytes = done; if (all) *total = *all; });

            std::vector<fs::path> vpks;
            if (IsModArchive(archive)) vpks = ExtractModArchive(archive, folder / L"extracted", token).Archives;
            else if (EqualsIgnoreCase(Narrow(archive.extension().wstring()), ".vpk")) vpks.push_back(archive);
            else throw InvalidData(file.Name + " is not a zip, 7z, rar or VPK file. Follow the mod page to install it by hand.");
            *count = vpks.size();
            std::vector<fs::path> imported;
            for (const auto& vpk : vpks) {
                try {
                    const auto cached = store.Import(vpk, token);
                    store.Rename(cached, vpks.size() == 1 ? name : name + " (" + Narrow(vpk.stem().wstring()) + ")");
                    if (const auto id = store.ContentId(cached)) result.AddedIds.push_back(*id);
                    imported.push_back(cached);
                } catch (const OperationCancelled&) { throw; }
                catch (const std::exception& error) { result.Errors += Narrow(vpk.filename().wstring()) + ": " + error.what() + "\n"; }
                ++*completed;
            }
            // Add each new mod to the saved collection, then read it back so the copies carry the new names.
            std::string ignored;
            auto library = store.Load(token, {}, &ignored);
            for (const auto& cached : imported) {
                const auto id = store.ContentId(cached);
                const bool listed = std::any_of(library.Sources.begin(), library.Sources.end(), [&](const auto& source) { return store.ContentId(source) == id; });
                if (!listed) library.Sources.push_back(cached);
            }
            store.Save(library);
            // Load replaces its notice, so the import errors above are kept apart.
            std::string notice;
            result.Library = store.Load(token, {}, &notice);
            result.Errors += notice;
            result.Restored = true;
            result.Origin = origin;
            result.Enable = enable;
            if (!result.AddedIds.empty()) {
                result.Done = "Downloaded " + name + (enable ? ". It is enabled; choose Apply to game on the Mods page." : ". Enable it on the Mods page.");
            }
            return result;
        });
    }

    // The library mod built from the pack under `name` earlier, which a new build of that name replaces.
    std::optional<std::string> BuiltModNamed(const std::string& name) const {
        for (const auto& entry : ModEntries()) {
            const auto origin = Mods.State().Origin(entry.ContentId);
            if (origin && origin->Built && EqualsIgnoreCase(entry.Name, name)) return entry.ContentId;
        }
        return std::nullopt;
    }

    // Builds the pack like Build VPK, stores the result in the library as a mod called `name`, and adds it to the
    // collection. An earlier build of the same name leaves the collection, and the Mods page gives the new build its
    // place in the load order and its enabled state.
    void StartBuildMod(const std::string& name, bool enable) {
        if (Restoring || FilesBusy()) return;
        if (!Store) { SetStoreStatus("Local mod storage is unavailable. " + SaveError, true); return; }
        if (!CanPersist) { SetStoreStatus("Your library has not been opened, so nothing can be added to it yet. " + SaveError, true); return; }
        Persist(true);
        if (!SaveError.empty()) { SetStoreStatus("Could not save your library before building: " + SaveError, true); return; }
        const auto selection = Builder.Snapshot(ModPaths, ConfiguredGame);
        if (selection.Selected.empty() && selection.Packages.empty()) return;
        const auto replaced = BuiltModNamed(name);
        ImportCancellation = CancellationSource{};
        ImportedCount = 0; ImportTotal = 0;
        BuildingMod = true;
        SetStoreStatus("Building " + name + "...", false);
        ImportWorker = std::async(std::launch::async, [root = Store->Root(), name, enable, models = selection.Selected,
            packages = selection.Packages, game = GamePath(), replaced, token = ImportCancellation.Token()] {
            ZoneScopedN("Build mod");
            namespace fs = std::filesystem;
            ImportResult result;
            const LocalModStore store(root);
            const auto folder = root / L"downloads" / (L"build-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            struct Cleanup { fs::path Path; ~Cleanup() { std::error_code error; fs::remove_all(Path, error); } } cleanup{folder};
            fs::create_directories(folder);
            const auto vpk = folder / L"pack.vpk";
            ExportModelLibrary(models, game, vpk, token, packages);
            const auto cached = store.Import(vpk, token);
            const auto id = store.ContentId(cached);
            if (!id) throw InvalidData("The built pack was not stored in the library.");

            std::string ignored;
            auto library = store.Load(token, {}, &ignored);
            // The library stores identical files once, so a pack made of exactly one mod's files is that mod.
            // Naming it would rename that mod, and a later rebuild would take it out of the collection.
            if (id != replaced) {
                for (const auto& source : library.Sources) {
                    if (store.ContentId(source) != id) continue;
                    const auto other = store.Name(source).value_or(Narrow(source.stem().wstring()));
                    throw InvalidOperation("The pack has exactly the same files as " + other
                        + ", which is already in your library. Enable that mod on the Mods page instead.");
                }
            }
            store.Rename(cached, name);
            const bool replacing = replaced && *replaced != *id;
            if (replacing) {
                const auto old = [&](const fs::path& path) { return store.ContentId(path) == replaced; };
                std::erase_if(library.Sources, old);
                std::erase_if(library.Packages, old);
                std::erase_if(library.Selected, [&](const LibraryModel& model) { return old(model.Archive); });
            }
            if (std::none_of(library.Sources.begin(), library.Sources.end(), [&](const auto& source) { return store.ContentId(source) == id; }))
                library.Sources.push_back(cached);
            store.Save(library);
            std::string notice;
            result.Library = store.Load(token, {}, &notice);
            result.Errors += notice;
            result.Restored = true;
            result.AddedIds = {*id};
            ModOrigin origin;
            origin.Built = true;
            result.Origin = origin;
            result.Enable = enable;
            if (replacing) result.ReplacedId = *replaced;
            result.Done = "Built " + name + (replacing ? ", replacing the earlier build" : "")
                + (enable ? ". It is enabled; choose Apply to game on the Mods page." : ". Enable it on the Mods page.");
            return result;
        });
    }

    void DrawBuildModPopup() {
        if (std::exchange(OpenBuildMod, false)) ImGui::OpenPopup("Build as mod");
        ImGui::SetNextWindowSize({ImGui::GetFontSize() * 28, 0}, ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal("Build as mod", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
        ImGui::TextWrapped("Builds the pack into your library as a mod with this name. Turn it on and off on the Mods page like any other mod.");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        const bool entered = ImGui::InputTextWithHint("##modname", "Mod name", BuildModName, sizeof(BuildModName), ImGuiInputTextFlags_EnterReturnsTrue);
        std::string name = BuildModName;
        name.erase(0, name.find_first_not_of(" \t"));
        name.erase(name.find_last_not_of(" \t") + 1);
        if (const auto replaced = name.empty() ? std::nullopt : BuiltModNamed(name)) {
            ImGui::TextColored(ui::kWarning, "Replaces your earlier build named %s.", name.c_str());
            ImGui::TextDisabled("The new build keeps its place in the load order and stays enabled where it was.");
        }
        ImGui::Checkbox("Enable it in the current profile", &BuildModEnable);
        const bool can_build = !name.empty() && !FilesBusy() && !Restoring;
        ImGui::BeginDisabled(!can_build);
        if (ImGui::Button("Build") || (entered && can_build)) {
            StartBuildMod(name, BuildModEnable);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // The library's stored mods for the Mods page, in library order.
    std::vector<ModEntry> ModEntries() const {
        std::vector<ModEntry> entries;
        if (!Store) return entries;
        for (const auto& path : ModPaths) {
            const auto id = Store->ContentId(path);
            if (!id) continue;
            const LibraryPanel::SourceInfo* info = Builder.FindSource(path);
            std::string name = info ? info->Name : Narrow(path.stem().wstring());
            if (name.size() > 4 && EqualsIgnoreCase(std::string_view(name).substr(name.size() - 4), ".vpk")) name.resize(name.size() - 4);
            entries.push_back({*id, std::move(name), path});
        }
        return entries;
    }

    void PollImport() {
        ZoneScopedN("Poll import");
        if (!ImportBusy() || ImportWorker.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        try {
            const auto result = ImportWorker.get();
            ApplyLibrary(result.Library);
            if (!result.ReplacedId.empty() && result.AddedIds.size() == 1) Mods.State().Replace(result.ReplacedId, result.AddedIds.front());
            if (result.Origin && !result.AddedIds.empty()) Mods.RecordAdded(result.AddedIds, *result.Origin, result.Enable);
            if (result.Restored) { SavedState = result.Library; CanPersist = true; }
            if (!result.Errors.empty()) SetStoreStatus(result.Errors, true);
            else if (!result.Done.empty()) SetStoreStatus(result.Done, false);
            else if (Restoring) SetStoreStatus({}, false);
            else SetStoreStatus("Saved in your library. Your collection will reopen automatically.", false);
            Persist(true);
            if (InspectAfterImport && !Restoring) {
                for (const auto& [source, cached] : result.Imported) {
                    if (source == InspectAfterImport->first) StartInspect(cached, InspectAfterImport->second);
                }
                InspectAfterImport.reset();
            }
        } catch (const std::exception& error) {
            SetStoreStatus(Downloading ? "Could not download " + DownloadName + ": " + error.what() : std::string(error.what()), true);
            if (!Restoring) InspectAfterImport.reset();
        }
        Downloading = false;
        BuildingMod = false;
        if (Restoring) {
            Restoring = false;
            // Imports requested during a failed restore still need storage, but are not saved over the library.
            if (!QueuedImports.Sources.empty()) StartImport(std::exchange(QueuedImports, {}), false);
            else InspectAfterImport.reset();
        }
        // Imports and merges add models to the version history too.
        History.Refresh();
        if (MultiSuspended) {
            MultiSuspended = false;
            Multi.Reload();
        }
    }

    void SyncSources() {
        std::vector<std::string> paths;
        for (const auto& path : ModPaths) paths.push_back(Narrow(path.wstring()));
        Repair.SetSources(Game ? Narrow(std::filesystem::path(Widen(GameName)).parent_path().wstring()) : "", std::move(paths));
    }

    static std::filesystem::path ContentDirectoryFor(const std::filesystem::path& root) {
        try { return LocalModStore(root).ContentDirectory(); }
        catch (const std::exception&) { return {}; }
    }

    explicit App(const VulkanDevice& device, const std::filesystem::path& data_root)
        : Panel(device), Compare(device), Multi(device), Builder(device, Archives, data_root),
          History(ContentDirectoryFor(data_root)), Mods(data_root), Banana(device), DataRoot(data_root), LayoutIni(data_root / L"layout.ini") {
        Multi.SetArchives(Archives);
        Builder.SetMultiViewHandler([this](const LibraryModel& model) {
            Multi.Open(model, Builder.Models());
            ShowPage(Page::Multi);
        });
        Builder.SetBuildAsModHandler([this] { OpenBuildMod = true; });
        try {
            Store.emplace(DataRoot);
            StartRestore();
        } catch (const std::exception& error) {
            SaveError = error.what();
            ReportError(SaveError);
        }
    }
    ~App() {
        ImportCancellation.Cancel();
        if (ImportBusy()) { ImportWorker.wait(); PollImport(); }
        Persist(true);
    }

    void OpenGame(const std::filesystem::path& chosen) {
        Game.reset();
        GameClips.clear();
        GameDirectory.clear();
        GameName.clear();
        GameError.clear();
        GameLoading = GamePak(chosen);
        ConfiguredGame = std::filesystem::absolute(GameLoading).lexically_normal();
        RememberGamePath(ConfiguredGame);
        // The game directory has hundreds of thousands of entries, so it is parsed off the UI thread.
        GameLoader.Start([archives = Archives, pak = GameLoading](CancellationToken) {
            ZoneScopedN("Open game archive");
            LoadedGame loaded;
            loaded.Archive = archives->Get(pak);
            {
                ZoneScopedN("Collect game clips");
                for (const VpkEntry& entry : loaded.Archive->Archive().Entries()) {
                    if (EndsWith(entry.FullPath, ".vnmclip_c")) loaded.Clips.push_back(entry.FullPath);
                }
            }
            return loaded;
        });
        SyncSources();
    }

    void PollGame() {
        ZoneScopedN("Poll game");
        try {
            const auto loaded = GameLoader.Take();
            if (!loaded) return;
            Game = loaded->Archive;
            GameClips = std::move(loaded->Clips);
            GameName = Narrow(GameLoading.wstring());
            GameDirectory = Narrow(std::filesystem::path(Widen(GameName)).parent_path().wstring());
            Multi.SetGame(Game);
        } catch (const std::exception& error) {
            std::error_code missing;
            if (!std::filesystem::is_regular_file(GameLoading, missing)) GameError = MissingGameMessage(GameLoading);
            else GameError = Narrow(GameLoading.wstring()) + ": " + error.what();
            ReportError(GameError);
            Multi.SetGame(nullptr);
        }
        // The comparison and textures depend on the game, so reload the model being inspected.
        if (!ActiveMod.empty()) StartInspect(ActiveMod, Selected);
        SyncSources();
    }

    void StartInspect(const std::filesystem::path& archive, const std::string& model) {
        ActiveMod = archive;
        ModName = Narrow(archive.filename().wstring());
        Selected = model;
        StandaloneModel = false;
        Panel.ShowError("", "");
        Compare.ShowError("", "");
        Inspector.Start([archives = Archives, archive, model, game = Game](CancellationToken cancellation) {
            return InspectArchiveModel(*archives, archive, model, game, cancellation);
        });
    }

    void OpenModelFile(const std::filesystem::path& path) {
        CloseInspection();
        StandaloneModel = true;
        ModName = Narrow(path.filename().wstring());
        Inspector.Start([path](CancellationToken) { return InspectModelFile(path); });
    }

    // Imports the archive into local storage first when storage is available, like any added mod.
    void OpenArchiveModel(const std::filesystem::path& archive, const std::string& model) {
        if (Store && !FilesBusy()) {
            InspectAfterImport.emplace(archive, model);
            ModelLibrary incoming;
            incoming.Sources.push_back(archive);
            StartImport(incoming, false);
        } else {
            StartInspect(archive, model);
        }
    }

    void PollInspect() {
        ZoneScopedN("Poll inspect");
        std::optional<Inspection> result;
        try {
            result = Inspector.Take();
        } catch (const std::exception& error) {
            Panel.ShowError(Selected, error.what());
            Compare.ShowError(Selected, error.what());
            return;
        }
        if (!result) return;
        Mod = result->Mod;
        if (!result->Model.empty()) Selected = result->Model;
        if (result->Preview) {
            Panel.Load(*result->Preview, result->Title, Mod ? ModThenGame(Mod, result->Game) : ResourceReader{},
                result->Animation ? &*result->Animation : nullptr, result->Clips);
        } else {
            Panel.ShowError(result->Title, result->PreviewError);
        }
        if (result->Comparison) {
            Compare.Load(std::move(*result->Comparison), VersionReaderFor(Mod, result->Game), result->Clips);
        } else {
            Compare.ShowError(result->Title, result->ComparisonError);
        }
        if (PendingClip) {
            Panel.PlayClip(*PendingClip);
            Compare.PlayClip(*PendingClip);
            PendingClip.reset();
        }
    }

    void AddPaths(const std::vector<std::filesystem::path>& paths) {
        if (paths.empty()) return;
        ModelLibrary incoming;
        for (const auto& path : paths) {
            if (skeleton_repair::EqualsIgnoreCase(Narrow(path.extension().wstring()), ".vpk")) incoming.Sources.push_back(path);
        }
        if (!incoming.Sources.empty()) StartImport(incoming, false);
        if (skeleton_repair::EqualsIgnoreCase(Narrow(paths.front().extension().wstring()), ".vmdl_c")) {
            OpenModelFile(paths.front());
            ShowPage(Page::Inspect);
        } else ShowPage(Page::Gallery);
        SyncSources();
    }

    void AddMods(HWND hwnd) {
        AddPaths(AskForFiles(hwnd, L"Mod VPKs and models (*.vpk;*.vmdl_c)\0*.vpk;*.vmdl_c\0All files\0*.*\0",
            L"Add reference mods (Ctrl / Shift to select multiple)", true));
    }

    // Adds every mod VPK directly in a folder, such as the game's addons folder.
    void AddFolder(HWND hwnd) {
        const auto folder = AskForFolder(hwnd, L"Add every mod VPK in a folder");
        if (!folder) return;
        try {
            const auto mods = FolderImportCandidates(*folder);
            if (mods.empty()) { SetStoreStatus("No mod VPKs found in " + Narrow(folder->wstring()) + ".", true); return; }
            AddPaths(mods);
        } catch (const std::exception& error) {
            SetStoreStatus(error.what(), true);
        }
    }

    // The explorer and workspace are fixed panels, so their dock nodes show no tab.
    static void DockWithoutTab() {
        ImGuiWindowClass window_class;
        window_class.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
        ImGui::SetNextWindowClass(&window_class);
    }

    // Switches pages from code; the page tab bar follows on the next frame.
    void ShowPage(Page page) {
        if (page != CurrentPage) PreviousPage = CurrentPage;
        CurrentPage = page;
        PageRequested = true;
    }

    bool PackBusy() const { return Builder.Busy() || Repair.Busy() || ImportBusy(); }
    std::filesystem::path GamePath() const { return Game ? std::filesystem::path(Widen(GameName)) : std::filesystem::path{}; }

    void ChooseGame(HWND hwnd) {
        const auto paths = AskForFiles(hwnd, L"Game archive (pak01_dir.vpk)\0pak01_dir.vpk\0VPK archives (*.vpk)\0*.vpk\0",
            L"Select game\\citadel\\pak01_dir.vpk");
        if (!paths.empty()) OpenGame(paths.front());
    }

    void AddLibrary(HWND hwnd) {
        Builder.AddLibrary(hwnd, [&](const ModelLibrary& library) { StartImport(library, true); },
            [&](const std::filesystem::path& other) { StartMerge(other); });
        ShowPage(Page::Gallery);
    }

    void RemoveSource(const std::filesystem::path& path) {
        std::erase(ModPaths, path);
        if (ActiveMod == path) CloseInspection();
        SyncSources();
        Builder.Restore(Builder.Snapshot(ModPaths, ConfiguredGame));
    }

    void FindSkins() {
        if (CurrentPage != Page::Gallery && CurrentPage != Page::Pack) ShowPage(Page::Gallery);
        Builder.FocusSearch();
    }

    void HandleShortcuts(HWND hwnd) {
        constexpr ImGuiInputFlags global = ImGuiInputFlags_RouteGlobal;
        if (!FilesBusy() && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, global)) AddMods(hwnd);
        if (!FilesBusy() && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_O, global)) AddFolder(hwnd);
        if (!FilesBusy() && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_G, global)) ChooseGame(hwnd);
        if (!PackBusy() && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, global)) { ShowPage(Page::Pack); Builder.BuildPack(hwnd, GamePath()); }
        if (!PackBusy() && ImGui::Shortcut(ImGuiKey_F5, global)) Builder.Reload(ModPaths);
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F, global)) FindSkins();
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_E, global)) {
            ShowExplorer = !ShowExplorer;
            if (ShowExplorer) FocusExplorer = true;
        }
        if (ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_LeftArrow, global)) ShowPage(PreviousPage);
        if (ImGui::Shortcut(ImGuiKey_F1, global)) OpenShortcuts = true;
        const Page pages[] = {Page::Gallery, Page::Pack, Page::History, Page::Repair, Page::Inspect, Page::Multi, Page::Mods, Page::Browse};
        const ImGuiKey keys[] = {ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5, ImGuiKey_6, ImGuiKey_7, ImGuiKey_8};
        for (size_t i = 0; i < std::size(pages); ++i) {
            if (ImGui::Shortcut(ImGuiMod_Ctrl | keys[i], global)) ShowPage(pages[i]);
        }
    }

    void DrawMenuBar(HWND hwnd) {
        ZoneScopedN("Menu");
        if (!ImGui::BeginMainMenuBar()) return;
        HandleShortcuts(hwnd);
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Add mods...", "Ctrl+O", false, !FilesBusy())) AddMods(hwnd);
            if (ImGui::MenuItem("Add folder...", "Ctrl+Shift+O", false, !FilesBusy())) AddFolder(hwnd);
            ImGui::SetItemTooltip("Adds every mod VPK in a folder, such as Deadlock's addons folder. Chunk files and the game archive are skipped.");
            if (ImGui::MenuItem("Get mods from GameBanana", "Ctrl+8")) ShowPage(Page::Browse);
            ImGui::Separator();
            if (ImGui::MenuItem("Manage mods", "Ctrl+7")) ShowPage(Page::Mods);
            ImGui::SetItemTooltip("Enable mods, set their load order and apply them to Deadlock's addons folder.");
            ImGui::Separator();
            if (ImGui::MenuItem("Add shared library...", nullptr, false, !FilesBusy())) AddLibrary(hwnd);
            ImGui::SetItemTooltip("Adds the mods and pack selections of a shared .db library to yours. Nothing in your library is replaced.");
            if (ImGui::MenuItem("Share library...", nullptr, false, !FilesBusy() && Builder.CanShare())) Builder.ShareLibrary(hwnd);
            ImGui::SetItemTooltip("Saves your mods, pack selections and thumbnails as one .db file others can add.");
            ImGui::Separator();
            if (ImGui::MenuItem("Choose game archive...", "Ctrl+G", false, !FilesBusy())) ChooseGame(hwnd);
            ImGui::Separator();
            if (ImGui::MenuItem("Build VPK...", "Ctrl+B", false, !PackBusy() && Builder.SelectedCount() > 0)) {
                ShowPage(Page::Pack);
                Builder.BuildPack(hwnd, GamePath());
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Open library folder", nullptr, false, Store.has_value())) ui::RevealInExplorer(Store->ContentDirectory());
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) PostMessageW(hwnd, WM_CLOSE, 0, 0);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Find skins", "Ctrl+F")) FindSkins();
            if (ImGui::MenuItem("Show every source", nullptr, false, !Builder.SourceFilter().empty())) Builder.SetSourceFilter({});
            ImGui::Separator();
            if (ImGui::MenuItem("Clear pack", nullptr, false, !PackBusy() && Builder.SelectedCount() > 0)) Builder.ClearPack();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            const struct { const char* Label; const char* Keys; Page Value; } pages[] = {
                {"Mod library", "Ctrl+1", Page::Gallery}, {"VPK builder", "Ctrl+2", Page::Pack}, {"Version history", "Ctrl+3", Page::History},
                {"Skin repairs", "Ctrl+4", Page::Repair}, {"Model inspector", "Ctrl+5", Page::Inspect},
                {"Multi view", "Ctrl+6", Page::Multi}, {"Mods", "Ctrl+7", Page::Mods}, {"Get mods", "Ctrl+8", Page::Browse}};
            for (const auto& page : pages) {
                if (ImGui::MenuItem(page.Label, page.Keys, CurrentPage == page.Value)) ShowPage(page.Value);
            }
            if (ImGui::MenuItem("Back", "Alt+Left")) ShowPage(PreviousPage);
            ImGui::Separator();
            if (ImGui::MenuItem("Explorer", "Ctrl+Shift+E", &ShowExplorer) && ShowExplorer) FocusExplorer = true;
            if (ImGui::MenuItem("Workspace")) FocusWorkspace = true;
            ImGui::MenuItem("Status bar", nullptr, &ShowStatusBar);
            ImGui::Separator();
            if (ImGui::MenuItem("Reset layout")) ResetLayout = true;
            ImGui::SetItemTooltip("Puts the explorer on the left and the workspace on the right. Mods, the catalog and thumbnails stay.");
            if (ImGui::MenuItem("Refresh library", "F5", false, !PackBusy())) Builder.Reload(ModPaths);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            const bool repair_allowed = !Builder.Busy() && !ImportBusy() && !Mods.Busy();
            if (ImGui::MenuItem("Analyze mods", nullptr, false, repair_allowed && Repair.CanAnalyze())) {
                ShowPage(Page::Repair);
                Repair.Analyze();
            }
            if (ImGui::MenuItem("Repair supported mods", nullptr, false, repair_allowed && Repair.CanRepair())) {
                ShowPage(Page::Repair);
                Repair.Repair([&] { ReleaseArchives(); });
            }
            if (ImGui::MenuItem("Cancel repair", nullptr, false, Repair.Busy())) Repair.Cancel();
            ImGui::Separator();
            if (ImGui::MenuItem("Cancel import", nullptr, false, ImportBusy())) ImportCancellation.Cancel();
            if (ImGui::MenuItem("Cancel VPK build", nullptr, false, Builder.Busy())) Builder.CancelBuild();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Keyboard shortcuts", "F1")) OpenShortcuts = true;
            if (ImGui::MenuItem("Quick start")) OpenQuickStart = true;
            if (ImGui::MenuItem("Open logs")) OpenLogs();
            if (ImGui::MenuItem("Copy diagnostic details")) CopyDiagnostics();
            ImGui::Separator();
            if (ImGui::MenuItem("About")) OpenAbout = true;
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // Context menu items for the game connection, shared by the explorer and the status bar.
    void GameMenuItems(HWND hwnd) {
        if (ImGui::MenuItem("Choose game archive...", "Ctrl+G", false, !FilesBusy())) ChooseGame(hwnd);
        if (ImGui::MenuItem("Reconnect", nullptr, false, !FilesBusy() && !ConfiguredGame.empty() && !GameLoader.Busy())) OpenGame(ConfiguredGame);
        ImGui::Separator();
        ui::PathMenuItems(Game ? GamePath() : GameLoading);
    }

    void GameState(ImVec4& color, const char*& label) const {
        if (Game) { color = ui::kGood; label = "Deadlock connected"; }
        else if (GameLoader.Busy()) { color = ui::kWarning; label = "Connecting to Deadlock..."; }
        else if (!GameError.empty()) { color = ui::kError; label = "Deadlock not connected"; }
        else { color = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled); label = "Deadlock not connected"; }
    }

    // The most important message for the status bar, with a way to dismiss it.
    std::string* StatusMessage(ImVec4& color) {
        if (!StartupNotice.empty()) { color = ui::kError; return &StartupNotice; }
        if (!SaveError.empty()) { color = ui::kError; return &SaveError; }
        if (!GameError.empty()) { color = ui::kError; return &GameError; }
        if (!StoreStatus.empty() && !ImportBusy()) {
            color = StoreStatusIsError ? ui::kError : ImGui::GetStyleColorVec4(ImGuiCol_Text);
            return &StoreStatus;
        }
        return nullptr;
    }

    void DrawStatusBar(HWND hwnd) {
        ZoneScopedN("Status bar");
        if (!ShowStatusBar) return;
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
        if (ImGui::BeginViewportSideBar("##statusbar", ImGui::GetMainViewport(), ImGuiDir_Down, ImGui::GetFrameHeight(), flags)
            && ImGui::BeginMenuBar()) {
            ImVec4 color;
            const char* label = nullptr;
            GameState(color, label);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            char game_menu[96];
            std::snprintf(game_menu, sizeof(game_menu), "\xE2\x97\x8F %s###game", label);
            const bool open = ImGui::BeginMenu(game_menu);
            ImGui::PopStyleColor();
            if (!open) ImGui::SetItemTooltip("%s", Game ? GameName.c_str() : GameLoading.empty() ? "Choose the game archive to connect" : Narrow(GameLoading.wstring()).c_str());
            if (open) {
                GameMenuItems(hwnd);
                ImGui::EndMenu();
            }
            ImGui::Separator();

            // Background work, with its progress and a way to stop it.
            if (ImportBusy()) {
                const size_t done = ImportedCount.load();
                const size_t total = ImportTotal.load();
                ImGui::TextUnformatted(Restoring ? "Opening library" : Downloading ? "Downloading" : BuildingMod ? "Building mod" : "Importing");
                const uint64_t received = DownloadBytes.load(), size = DownloadTotal.load();
                if (Downloading && total == 0) {
                    ImGui::ProgressBar(size ? static_cast<float>(received) / static_cast<float>(size) : 0.f, {90, ImGui::GetTextLineHeight() * .55f}, "");
                    ImGui::Text("%.1f / %.1f MB", static_cast<double>(received) / 1048576.0, static_cast<double>(size) / 1048576.0);
                    ImGui::SetItemTooltip("%s", DownloadName.c_str());
                } else if (total > 0) {
                    ImGui::ProgressBar(static_cast<float>(done) / static_cast<float>(total), {90, ImGui::GetTextLineHeight() * .55f}, "");
                    ImGui::Text("%zu / %zu", done, total);
                }
                if (ImGui::SmallButton("Cancel##import")) ImportCancellation.Cancel();
                ImGui::Separator();
            }
            if (Mods.Busy()) {
                ImGui::TextColored(ui::kWarning, "Applying mods to the game...");
                ImGui::Separator();
            }
            if (Builder.Busy()) {
                ImGui::TextColored(ui::kWarning, "Working on the library...");
                if (ImGui::SmallButton("Cancel##build")) Builder.CancelBuild();
                ImGui::Separator();
            }
            if (Repair.Busy()) {
                ImGui::TextColored(ui::kWarning, "Repairs:");
                ui::TextEllipsis(Repair.Status(), 260);
                if (ImGui::SmallButton("Cancel##repair")) Repair.Cancel();
                ImGui::Separator();
            }
            if (History.Busy()) {
                ImGui::TextColored(ui::kWarning, "Working on the version history...");
                ImGui::Separator();
            }
            if (Builder.Scanning()) {
                ImGui::TextDisabled("Reading %zu archives...", ModPaths.size());
                ImGui::Separator();
            }

            char counts[160];
            std::snprintf(counts, sizeof(counts), "%zu shown  \xC2\xB7  %zu skins  \xC2\xB7  %zu sources  \xC2\xB7  %zu in pack",
                Builder.ShownCount(), Builder.ModelCount(), ModPaths.size(), Builder.SelectedCount());
            const float counts_width = ImGui::CalcTextSize(counts).x + ImGui::GetStyle().ItemSpacing.x * 2;
            ImVec4 message_color;
            if (std::string* message = StatusMessage(message_color)) {
                const float width = ImGui::GetContentRegionAvail().x - counts_width;
                const auto first_line = message->substr(0, message->find('\n'));
                if (width > 40) {
                    ui::TextEllipsis(first_line, width, &message_color);
                    if (first_line.size() != message->size()) ImGui::SetItemTooltip("%s", message->c_str());
                    if (ImGui::BeginPopupContextItem("message_menu")) {
                        ui::CopyMenuItem("Copy message", *message);
                        if (ImGui::MenuItem("Dismiss")) message->clear();
                        ImGui::EndPopup();
                    }
                }
            }
            ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - counts_width));
            ImGui::TextDisabled("%s", counts);
            ImGui::EndMenuBar();
        }
        ImGui::End();
    }

    void DrawExplorer(HWND hwnd) {
        ZoneScopedN("Explorer");
        if (!ShowExplorer) return;
        DockWithoutTab();
        const bool focus = std::exchange(FocusExplorer, false);
        if (focus) ImGui::SetNextWindowFocus();
        ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::kPanel);
        ImGui::Begin("Explorer", &ShowExplorer, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
        const auto& style = ImGui::GetStyle();
        const auto right_aligned = [&](const char* labels) {
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(labels).x - style.FramePadding.x * 4 - style.ItemSpacing.x);
        };

        ui::SectionLabel("SOURCES");
        ImGui::SameLine();
        ImGui::TextDisabled("%zu", ModPaths.size());
        right_aligned("AddFolder");
        if (ui::ToolButton("Add", "Add mod VPKs or models (Ctrl+O). You can also drop files on the window.", !FilesBusy())) AddMods(hwnd);
        ImGui::SameLine();
        if (ui::ToolButton("Folder", "Add every mod VPK in a folder (Ctrl+Shift+O)", !FilesBusy())) AddFolder(hwnd);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##sourcefilter", "Filter sources", SourceFilter, sizeof(SourceFilter));

        const float footer = ImGui::GetFrameHeightWithSpacing() * 6 + ImGui::GetTextLineHeightWithSpacing() * 2
            + (GameError.empty() ? 0.f : ImGui::GetTextLineHeightWithSpacing() * 2) + (SaveError.empty() ? 0.f : ImGui::GetTextLineHeightWithSpacing() * 2);
        ImGui::BeginChild("sources", {0, std::max(ImGui::GetFrameHeight() * 3, ImGui::GetContentRegionAvail().y - footer)}, ImGuiChildFlags_Borders);
        const ImGuiTextFilter filter(SourceFilter);
        const auto& active = Builder.SourceFilter();
        const auto count_column = [&](size_t count) {
            char text[32];
            std::snprintf(text, sizeof(text), "%zu", count);
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(text).x);
            ImGui::TextDisabled("%s", text);
        };
        if (!ModPaths.empty() && !filter.IsActive()) {
            if (ImGui::Selectable("All sources", active.empty() && (CurrentPage == Page::Gallery || CurrentPage == Page::Pack))) {
                Builder.SetSourceFilter({});
                if (CurrentPage != Page::Pack) ShowPage(Page::Gallery);
            }
            count_column(Builder.ListedCount());
        }
        std::optional<std::filesystem::path> remove;
        for (const auto& path : ModPaths) {
            const LibraryPanel::SourceInfo* info = Builder.FindSource(path);
            const std::string local_name = info ? std::string{} : Narrow(path.filename().wstring());
            const std::string local_key = info ? std::string{} : Narrow(path.wstring());
            const std::string& name = info ? info->Name : local_name;
            const std::string& key = info ? info->Full : local_key;
            if (!filter.PassFilter(name.c_str())) continue;
            ImGui::PushID(key.c_str());
            if (ImGui::Selectable(name.c_str(), active == path)) {
                Builder.SetSourceFilter(active == path ? std::filesystem::path{} : path);
                if (CurrentPage != Page::Pack) ShowPage(Page::Gallery);
            }
            const size_t skins = info ? info->Skins : Builder.ModelsIn(path);
            const size_t files = info ? info->Files : Builder.PackageFiles(path);
            const std::string local_tip = info ? std::string{} : key;
            const std::string& tip = info ? info->Tip : local_tip;
            ImGui::SetItemTooltip("%s", tip.c_str());
            if (ImGui::BeginPopupContextItem("source_menu")) {
                ImGui::TextDisabled("%s", name.c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("Show skins")) { Builder.SetSourceFilter(path); ShowPage(Page::Gallery); }
                if (skins > 0) {
                    if (ImGui::MenuItem("Include every skin in the pack", nullptr, false, !PackBusy())) Builder.IncludeSource(path, true);
                    if (ImGui::MenuItem("Remove every skin from the pack", nullptr, false, !PackBusy())) Builder.IncludeSource(path, false);
                }
                if (files > 0) {
                    if (ImGui::MenuItem("Include other files in the pack", nullptr, false, !PackBusy())) Builder.IncludePackage(path, true);
                    if (ImGui::MenuItem("Remove other files from the pack", nullptr, false, !PackBusy())) Builder.IncludePackage(path, false);
                }
                ImGui::Separator();
                if (const auto id = Store ? Store->ContentId(path) : std::nullopt) {
                    const bool enabled = Mods.State().Enabled(*id);
                    if (ImGui::MenuItem(enabled ? "Disable for the game" : "Enable for the game")) Mods.SetEnabled(*id, !enabled);
                    ImGui::SetItemTooltip("Changes the current mod profile. Apply it on the Mods page (Ctrl+7).");
                }
                if (ImGui::MenuItem("Rename...", nullptr, false, !FilesBusy() && Store.has_value())) {
                    RenameTarget = path;
                    std::string current = Narrow(path.stem().wstring());
                    try { if (const auto stored = Store->Name(path)) current = *stored; } catch (const std::exception&) {}
                    std::snprintf(RenameText, sizeof(RenameText), "%s", current.c_str());
                    OpenRename = true;
                }
                ui::PathMenuItems(path);
                ImGui::Separator();
                if (ImGui::MenuItem("Remove from library", nullptr, false, !FilesBusy())) remove = path;
                ImGui::EndPopup();
            }
            count_column(skins > 0 ? skins : files);
            ImGui::PopID();
        }
        if (ModPaths.empty()) {
            ImGui::PushTextWrapPos();
            ImGui::TextDisabled("No sources yet. Connect the game, add mods, or open the quick start from the library.");
            ImGui::PopTextWrapPos();
        }
        if (remove) RemoveSource(*remove);
        ImGui::EndChild();

        ui::SectionLabel("GAME");
        ImVec4 color;
        const char* label = nullptr;
        GameState(color, label);
        ImGui::TextColored(color, "\xE2\x97\x8F");
        ImGui::SameLine(0, 4);
        ui::TextEllipsis(label, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("...").x - style.FramePadding.x * 2 - style.ItemSpacing.x);
        if (ImGui::BeginPopupContextItem("game_menu")) {
            GameMenuItems(hwnd);
            ImGui::EndPopup();
        }
        ImGui::SetItemTooltip("%s\nRight-click for more. %s", Game ? GameName.c_str() : "No game is connected.",
            GameResourceSummary().c_str());
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##manualgame", "game\\citadel or pak01_dir.vpk", ManualGame, sizeof(ManualGame));
        const float game_button = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * 0.5f;
        ImGui::BeginDisabled(FilesBusy());
        if (ImGui::Button("Connect", {game_button, 0})) ConnectManual(hwnd, false);
        ImGui::SameLine();
        if (ImGui::Button("Browse...", {-1, 0})) ConnectManual(hwnd, true);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Browse to game\\citadel\\pak01_dir.vpk (Ctrl+G). A typed path is used as entered.");
        }
        if (!GameError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::kError);
            ImGui::TextWrapped("%s", GameError.c_str());
            ImGui::PopStyleColor();
        }

        ui::SectionLabel("LIBRARY");
        if (Store) {
            const auto folder = Store->ContentDirectory();
            ui::TextEllipsis(Narrow(folder.wstring()), 0, &style.Colors[ImGuiCol_TextDisabled]);
            if (ImGui::BeginPopupContextItem("library_menu")) {
                ui::PathMenuItems(folder);
                ImGui::EndPopup();
            }
            ImGui::BeginDisabled(FilesBusy());
            ImGui::BeginDisabled(!Builder.CanShare());
            if (ImGui::Button("Share...", {ImGui::GetContentRegionAvail().x * .5f - style.ItemSpacing.x * .5f, 0})) Builder.ShareLibrary(hwnd);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Save your mods, pack selections and thumbnails as one .db file others can add.");
            ImGui::SameLine();
            if (ImGui::Button("Add shared...", {-1, 0})) AddLibrary(hwnd);
            ImGui::SetItemTooltip("Add the mods and pack selections of a shared .db library to yours. Nothing in yours is replaced.");
            ImGui::EndDisabled();
        } else {
            ImGui::TextDisabled("Local library storage is unavailable.");
        }
        if (!SaveError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui::kError);
            ImGui::TextWrapped("%s", SaveError.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::End();
        ImGui::PopStyleColor();
        if (focus) BringShellOnScreen("Explorer", {40.f, 40.f});
    }

    // Releases every read of the mod archives before a repair replaces them.
    void ReleaseArchives() {
        Inspector.Cancel(true);
        Builder.StopLoading();
        Mod.reset();
        Panel.ShowError("", "");
        Compare.ShowError("", "");
        Multi.Release();
        MultiSuspended = true;
    }

    // The page tabs. A tab the user clicks becomes the page; a page shown from code selects its tab.
    void DrawPageTabs() {
        if (!ImGui::BeginTabBar("pages", ImGuiTabBarFlags_FittingPolicyScroll)) return;
        char pack_label[48];
        std::snprintf(pack_label, sizeof(pack_label), "VPK builder (%zu)###pack", Builder.SelectedCount());
        const std::string inspect_label = (ModName.empty() ? std::string("Model inspector") : "Inspect: " + ModName) + "###inspect";
        const struct { const char* Label; const char* Tooltip; Page Value; } tabs[] = {
            {"Mod library###gallery", "Browse every skin in your library (Ctrl+1)", Page::Gallery},
            {pack_label, "Choose skins and other files, then export them as one VPK (Ctrl+2)", Page::Pack},
            {"Version history###history", "Every stored copy of each model, and how repairs changed it (Ctrl+3)", Page::History},
            {"Skin repairs###repair", "Check and repair your mods after game updates (Ctrl+4)", Page::Repair},
            {inspect_label.c_str(), "Textures, animations and skeleton of one model (Ctrl+5)", Page::Inspect},
            {"Multi view###multi", "Several models of one character, with one shared camera (Ctrl+6)", Page::Multi},
            {"Mods###mods", "Enable mods, set their load order and profiles, and apply them to the game (Ctrl+7)", Page::Mods},
            {"Get mods###browse", "Browse, search and download Deadlock mods from GameBanana (Ctrl+8)", Page::Browse},
        };
        Page visible = CurrentPage;
        for (const auto& tab : tabs) {
            const ImGuiTabItemFlags flags = PageRequested && tab.Value == CurrentPage ? ImGuiTabItemFlags_SetSelected : 0;
            const bool open = ImGui::BeginTabItem(tab.Label, nullptr, flags);
            ImGui::SetItemTooltip("%s", tab.Tooltip);
            if (open) {
                visible = tab.Value;
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
        if (PageRequested) {
            if (visible == CurrentPage) PageRequested = false;
        } else if (visible != CurrentPage) {
            PreviousPage = CurrentPage;
            CurrentPage = visible;
        }
    }

    void DrawWorkspace(HWND hwnd) {
        ZoneScopedN("Workspace");
        DockWithoutTab();
        const bool focus = std::exchange(FocusWorkspace, false);
        if (focus) ImGui::SetNextWindowFocus();
        ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::kPanel);
        ImGui::Begin("Workspace", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
        DrawPageTabs();
        if (CurrentPage == Page::Gallery || CurrentPage == Page::Pack) {
            Builder.Draw(hwnd, ModPaths, GamePath(),
                // Thumbnails wait for a connecting game, which supplies most textures, instead of loading twice.
                Repair.Busy() || ImportBusy(), Repair.Busy() || GameLoader.Busy(), CurrentPage == Page::Pack, [&] { AddMods(hwnd); },
                [&](const ModelLibrary& library) { StartImport(library, true); },
                [&](const std::filesystem::path& other) { StartMerge(other); },
                [&](const LibraryModel& model) { StartInspect(model.Archive, model.Model); ShowPage(Page::Inspect); },
                [&](const LibraryModel& model) {
                    ShowPage(Page::Repair);
                    Repair.AnalyzeModel(Narrow(model.Archive.wstring()), model.Model);
                },
                [&] { ConnectManual(hwnd, false); },
                [&] { OpenQuickStart = true; },
                LibraryReady());
        } else if (CurrentPage == Page::History) {
            const std::string& game_directory = GameDirectory;
            History.Draw(hwnd, Panel, [game = Game](const std::string& path) { return game ? game->Read(path) : std::nullopt; }, GameClips,
                Compare, game_directory, Repair.Busy() || Builder.Busy() || ImportBusy() || Mods.Busy(),
                [&](bool compare) {
                    // A stored version is not one of the gallery's archives.
                    Inspector.Cancel();
                    Mod.reset(); ActiveMod.clear();
                    StandaloneModel = true;
                    ModName = "Version history";
                    FocusCompare = compare;
                    ShowPage(Page::Inspect);
                },
                [&] { ReleaseArchives(); });
        } else if (CurrentPage == Page::Inspect) {
            if (ui::ToolButton("<", "Back to the previous page (Alt+Left)")) ShowPage(PreviousPage == Page::Inspect ? Page::Gallery : PreviousPage);
            ImGui::SameLine();
            const std::string location = Selected.empty() ? ModName : ModName + "  /  " + Selected;
            ui::TextEllipsis(location);
            if (ImGui::BeginPopupContextItem("location_menu")) {
                ui::CopyMenuItem("Copy model path", Selected);
                if (!ActiveMod.empty()) {
                    ui::CopyMenuItem("Copy source path", Narrow(ActiveMod.wstring()));
                    if (ImGui::MenuItem("Reveal source in Explorer")) ui::RevealInExplorer(ActiveMod);
                    if (ImGui::MenuItem("Show source in library")) { Builder.SetSourceFilter(ActiveMod); ShowPage(Page::Gallery); }
                    if (ImGui::MenuItem("Multi view", nullptr, false, CharacterOfModel(Selected).has_value())) {
                        Multi.Open({ActiveMod, Selected}, Builder.Models());
                        ShowPage(Page::Multi);
                    }
                    ImGui::SetItemTooltip("Show this character's models together, with one shared camera.");
                    if (ImGui::MenuItem("Analyze this model", nullptr, false, Game && !FilesBusy() && !Selected.empty())) {
                        ShowPage(Page::Repair);
                        Repair.AnalyzeModel(Narrow(ActiveMod.wstring()), Selected);
                    }
                    ImGui::SetItemTooltip("Check this model and its animation skeleton against the installed game.");
                }
                ImGui::EndPopup();
            }
            if (Inspector.Busy()) {
                ImGui::TextDisabled("Loading %s...", Selected.empty() ? ModName.c_str() : Selected.c_str());
            } else if (Selected.empty() && !StandaloneModel) {
                ImGui::TextDisabled("Double-click a skin in the library, or right-click it and choose Inspect, to view its textures, "
                    "animations and skeleton or compare it with the installed game.");
                if (ImGui::SmallButton("Browse skins")) ShowPage(Page::Gallery);
            } else if (ImGui::BeginTabBar("inspection")) {
                if (ImGui::BeginTabItem("Preview")) { Panel.Draw(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Compare versions", nullptr, std::exchange(FocusCompare, false) ? ImGuiTabItemFlags_SetSelected : 0)) {
                    Compare.Draw();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        } else if (CurrentPage == Page::Multi) {
            Multi.Draw(Builder.Models());
        } else if (CurrentPage == Page::Mods) {
            Mods.Draw(ModEntries(), ConfiguredGame, LibraryWorkBusy(),
                [&](const std::filesystem::path& source) { Builder.SetSourceFilter(source); ShowPage(Page::Gallery); },
                [&] { ShowPage(Page::Browse); }, [&] { AddMods(hwnd); });
        } else if (CurrentPage == Page::Browse) {
            const auto entries = ModEntries();
            const char* reason = Restoring ? "Your library is still opening." : !CanPersist ? "Your library could not be opened, so nothing can be added."
                : FilesBusy() ? "Wait for the library to finish its current work." : nullptr;
            Banana.Draw(reason == nullptr, reason,
                [&](int64_t mod) {
                    size_t count = 0;
                    for (const auto& id : Mods.State().FromGameBanana(mod))
                        count += std::any_of(entries.begin(), entries.end(), [&](const ModEntry& entry) { return entry.ContentId == id; }) ? 1 : 0;
                    return count;
                },
                [&](const GbMod& mod, const GbFile& file, bool enable) { StartDownload(mod, file, enable); });
        } else {
            // Applying mods copies the stored files a repair would replace.
            ImGui::BeginDisabled(Builder.Busy() || ImportBusy() || Mods.Busy());
            Repair.Draw(hwnd, [&] { ReleaseArchives(); });
            ImGui::EndDisabled();
        }
        ImGui::End();
        ImGui::PopStyleColor();
        if (focus) BringShellOnScreen("Workspace", {48.f, 48.f});
    }

    void DrawDialogs() {
        ZoneScopedN("Dialogs");
        CollectExportNotice();
        if (std::exchange(OpenShortcuts, false)) ImGui::OpenPopup("Keyboard shortcuts");
        if (std::exchange(OpenAbout, false)) ImGui::OpenPopup("About");
        if (std::exchange(OpenQuickStart, false)) ImGui::OpenPopup("Quick start");
        if (std::exchange(OpenExport, false)) ImGui::OpenPopup("Pack assembled");
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {.5f, .5f});
        DrawRenamePopup();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {.5f, .5f});
        DrawBuildModPopup();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {.5f, .5f});
        if (ImGui::BeginPopupModal("Keyboard shortcuts", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            static const char* const kShortcuts[][2] = {
                {"Ctrl+O", "Add mods"}, {"Ctrl+Shift+O", "Add every mod in a folder"}, {"Ctrl+G", "Choose the game archive"},
                {"Ctrl+B", "Build the pack as a VPK"}, {"Ctrl+F", "Search skins, packages and sources"}, {"F5", "Reopen every source and reload thumbnails"},
                {"Ctrl+1 ... Ctrl+8", "Library, builder, history, repairs, inspector, multi view, mods, get mods"}, {"Alt+Left", "Back to the previous page"},
                {"Ctrl+Shift+E", "Show or hide the explorer"}, {"View > Workspace", "Focus the workspace"},
                {"View > Reset layout", "Restore the explorer and workspace split"},
                {"Double-click", "Inspect a skin, list a package's files, or view a version"},
                {"Enter / Space", "Inspect a skin or list a package's files / include it in the pack"}, {"Ctrl+Wheel", "Thumbnail size in the gallery"},
                {"Drag / right-drag / wheel", "Orbit / pan / zoom a viewport"}, {"F", "Frame the model in the hovered viewport"},
                {"Right-click", "More actions on skins, sources, versions, results and viewports"}, {"F1", "This list"},
            };
            if (ImGui::BeginTable("shortcuts", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                for (const auto& [keys, action] : kShortcuts) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(ui::kAccent, "%s", keys);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(action);
                }
                ImGui::EndTable();
            }
            if (ImGui::Button("Close", {-1, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {.5f, .5f});
        if (ImGui::BeginPopupModal("About", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(SR_PRODUCT_TITLE);
            ImGui::TextDisabled("Build %s", SR_GIT_COMMIT);
            ImGui::TextDisabled("Browse skins, build packs, inspect models and repair skeleton changes.");
            ImGui::TextDisabled("Dear ImGui %s, Vulkan 1.3", ImGui::GetVersion());
#ifdef TRACY_ENABLE
            ImGui::TextDisabled("Tracy is announcing this window. Connect tracy-profiler, or open 127.0.0.1 port 8086.");
#endif
            if (ImGui::Button("Close", {-1, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {.5f, .5f});
        ImGui::SetNextWindowSize({ImGui::GetFontSize() * 36, 0}, ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Quick start", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextWrapped("These steps are the whole first run. Assembly does not install the pack.");
            ImGui::Spacing();
            for (const GuideStep& step : QuickStartSteps()) {
                ImGui::TextColored(ui::kAccent, "%s", step.Title);
                ImGui::TextWrapped("%s", step.Detail);
                ImGui::Spacing();
            }
            if (ImGui::Button("Close", {-1, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {.5f, .5f});
        ImGui::SetNextWindowSize({ImGui::GetFontSize() * 32, 0}, ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal("Pack assembled", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
            if (PendingExport) {
                ImGui::TextWrapped("%s", PendingExport->Summary.c_str());
                ImGui::TextWrapped("%s", kAssemblyDoesNotInstall);
                ImGui::TextWrapped("Install or remove the pack from Deadlock yourself. The quick start has those steps.");
                if (ImGui::Button("Reveal in Explorer")) ui::RevealInExplorer(PendingExport->Output);
                ImGui::SameLine();
                if (ImGui::Button("Open quick start")) OpenQuickStart = true;
            }
            if (ImGui::Button("Close", {-1, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
};


void DestroyWindowIcons(HICON icon, HICON icon_small) {
    if (icon != nullptr) DestroyIcon(icon);
    if (icon_small != nullptr) DestroyIcon(icon_small);
}

int Run(const std::vector<std::filesystem::path>& args) {
    [[maybe_unused]] const TracySession tracy_session;
    std::optional<std::filesystem::path> data_root_flag;
    std::optional<std::filesystem::path> game_folder;
    std::optional<std::string> clip;
    std::vector<std::filesystem::path> positional;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == L"--data-root") {
            if (i + 1 >= args.size()) throw std::runtime_error("--data-root needs a directory.");
            data_root_flag = args[++i];
        } else if (args[i] == L"--game" && i + 1 < args.size()) {
            game_folder = args[++i];
        } else if (args[i] == L"--clip" && i + 1 < args.size()) {
            clip = VpkArchive::Normalize(Narrow(args[++i].wstring()));
        } else {
            positional.push_back(args[i]);
        }
    }
    const std::wstring data_root_env = EnvironmentValue(L"DEADLOCK_MODWORKS_DATA_ROOT");
    const std::filesystem::path data_root = std::filesystem::absolute(
        ResolveDataRoot(data_root_flag, data_root_env.empty() ? nullptr : data_root_env.c_str())).lexically_normal();
    const DataPaths paths = DataPathsFor(data_root);
    const DataRootInspection storage = InspectDataRoot(paths.Root);
    if (!storage.Writable) {
        ShowStartupFailure(storage.Message.empty() ? UnwritableDataRootMessage(paths.Root) : storage.Message);
        return 1;
    }
    if (LoadLibraryW(L"vulkan-1.dll") == nullptr) {
        AppendErrorLog(paths.LogFile, DiagnosticText(SR_PRODUCT_TITLE, SR_GIT_COMMIT, paths.Root, kMissingVulkanLoaderMessage));
        ShowStartupFailure(kMissingVulkanLoaderMessage);
        return 1;
    }
    const SettingsInspection settings = InspectLibrarySettings(paths.StoreDirectory);
    if (settings.State == SettingsState::Corrupt) {
        AppendErrorLog(paths.LogFile, DiagnosticText(SR_PRODUCT_TITLE, SR_GIT_COMMIT, paths.Root, settings.Message));
        ShowStartupFailure(settings.Message);
    }
    try {
    // ImGui stores this pointer and does not copy it. The string has to outlive the context.
    const std::string layout_ini = Narrow(paths.LayoutIni.wstring());
    const ShellPlacement placement = LoadShellPlacement(paths.WindowIni);
    const RECT work = PrimaryWorkOrigin();

    ImGui_ImplWin32_EnableDpiAwareness();
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    // Resource 1 is the ICON in cmake/app.rc.in. Each executable that opens this
    // window embeds that ICO, and LoadImage selects the frame for these sizes.
    const HICON icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    const HICON icon_small = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    // Matches the workspace clear so a resize does not flash white or the old grey.
    const HBRUSH background = CreateSolidBrush(RGB(0x14, 0x1B, 0x23));
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, instance, icon, nullptr, nullptr, nullptr, L"DeadlockModelPreview", icon_small};
    wc.hbrBackground = background;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, SR_PRODUCT_TITLE_W, WS_OVERLAPPEDWINDOW,
        placement.x + work.left, placement.y + work.top, placement.width, placement.height, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        DeleteObject(background);
        DestroyWindowIcons(icon, icon_small);
        throw std::runtime_error("Could not create the model tools window.");
    }
    g_window_ini = paths.WindowIni;
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_small));

    SetupVulkan();
    VkWin32SurfaceCreateInfoKHR surface_info{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surface_info.hwnd = hwnd;
    surface_info.hinstance = wc.hInstance;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    CheckVk(vkCreateWin32SurfaceKHR(g_instance, &surface_info, nullptr, &surface));
    RECT client{};
    GetClientRect(hwnd, &client);
    SetupWindow(surface, client.right - client.left, client.bottom - client.top);
    ShowWindow(hwnd, placement.maximized ? SW_SHOWMAXIMIZED : SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
    io.IniFilename = layout_ini.c_str();
    const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    theme::Apply(scale);
    wchar_t windows_folder[MAX_PATH]{};
    GetWindowsDirectoryW(windows_folder, MAX_PATH);
    const auto font = std::filesystem::path(windows_folder) / L"Fonts" / L"segoeui.ttf";
    const float font_size = std::round(15.f * scale);
    if (std::filesystem::exists(font)) io.Fonts->AddFontFromFileTTF(Narrow(font.wstring()).c_str(), font_size);
    else io.Fonts->AddFontDefault();
    // Status dots, arrows and check marks come from Segoe UI Symbol.
    const auto symbols = std::filesystem::path(windows_folder) / L"Fonts" / L"seguisym.ttf";
    if (std::filesystem::exists(symbols)) {
        static const ImWchar kSymbolRanges[] = {0x2022, 0x2026, 0x2190, 0x21FF, 0x2300, 0x23FF, 0x25A0, 0x25FF, 0x2600, 0x27BF, 0};
        ImFontConfig merge;
        merge.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(Narrow(symbols.wstring()).c_str(), font_size, &merge, kSymbolRanges);
    }
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
        App app(g_device, paths.Root);
        if (settings.State == SettingsState::Corrupt) app.NoteStartupError(settings.Message);
        else if (settings.State == SettingsState::Ok) app.SettingsGame = settings.GameDirectory;
        // A saved library game is applied when the restore finishes. Detect Steam only when that path is empty.
        const ResolvedGamePath chosen_game = ResolveGamePath(game_folder, {});
        if (!chosen_game.AutoDetect) {
            app.KeepGame = true;
            app.OpenGame(chosen_game.Path);
        }
        if (!positional.empty()) {
            if (positional.size() > 1 && EndsWith(Narrow(positional[0].wstring()), ".vpk") && EndsWith(Narrow(positional[1].wstring()), ".vmdl_c")) {
                app.PendingClip = clip;
                app.OpenArchiveModel(positional[0], VpkArchive::Normalize(Narrow(positional[1].wstring())));
                app.ShowPage(App::Page::Inspect);
            } else {
                app.AddPaths(positional);
            }
        }

        bool done = false;
        bool first_frame = true;
        while (!done) {
            ZoneScopedN("Frame");
            {
                ZoneScopedN("Message pump");
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                    done = done || msg.message == WM_QUIT;
                }
            }
            if (done) {
                app.Repair.Cancel();
                break;
            }
            if (IsIconic(hwnd)) {
                Sleep(10);
                FrameMark;
                continue;
            }
            if (g_swapchain_rebuild) {
                ZoneScopedN("Swapchain");
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

            {
                ZoneScopedN("ImGui new frame");
                ImGui_ImplVulkan_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();
            }

            app.Builder.Poll();
            app.PollImport();
            app.Mods.Poll();
            if (const auto error = app.Mods.TakeError()) app.ReportError(*error);
            app.MaybeAutoDetect();
            app.PollGame();
            app.PollInspect();
            app.Multi.Poll();
            app.StartPendingRename();

            // A repair from the version history may replace one of the gallery's stored copies too.
            const bool repaired = app.Repair.Poll() | app.History.TakeRepaired();
            if (repaired) {
                // Repaired archives were replaced on disk, so reopen them.
                app.Archives->Remove(app.ModPaths);
                app.Builder.InvalidateThumbnails();
                // Stores each repaired copy as a new version; the version before the repair stays in the library.
                if (app.Store) app.StartImport(app.Builder.Snapshot(app.ModPaths, app.ConfiguredGame), false);
            }
            if (repaired && !app.ActiveMod.empty()) app.StartInspect(app.ActiveMod, app.Selected);
            if (repaired) {
                app.MultiSuspended = false;
                app.Multi.Reload();
            }
            if (!g_dropped.empty()) {
                if (!app.FilesBusy()) app.AddPaths(g_dropped);
                g_dropped.clear();
            }

            // The menu and status bars take their rows before the dockspace fills the rest of the window.
            app.DrawMenuBar(hwnd);
            app.DrawStatusBar(hwnd);
            bool reset_layout = false;
            if (app.ResetLayout) {
                app.ResetLayout = false;
                app.ShowExplorer = true;
                app.FocusExplorer = true;
                app.FocusWorkspace = true;
                std::error_code remove_error;
                std::filesystem::remove(app.LayoutIni, remove_error);
                reset_layout = true;
            }
            const ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_NoWindowMenuButton);
            // Rebuild only when nothing was saved, or the user asked. A saved floating layout is left alone.
            if (reset_layout || (first_frame && !HasSavedShellLayout())) BuildDefaultDock(dockspace);

            app.DrawExplorer(hwnd);
            app.DrawWorkspace(hwnd);
            if (first_frame) {
                BringShellOnScreen("Explorer", {40.f, 40.f});
                BringShellOnScreen("Workspace", {48.f, 48.f});
                first_frame = false;
            }
            app.DrawDialogs();
            app.Persist();

            {
                ZoneScopedN("ImGui render");
                ImGui::Render();
            }
            ImDrawData* draw_data = ImGui::GetDrawData();
            if (draw_data->DisplaySize.x > 0 && draw_data->DisplaySize.y > 0) {
                g_window.ClearValue.color = {{theme::kClearRgb[0], theme::kClearRgb[1], theme::kClearRgb[2], 1.0f}};
                FrameRender(draw_data, [&](VkCommandBuffer command_buffer) {
                    app.Panel.Record(command_buffer);
                    app.Compare.Record(command_buffer);
                    app.Multi.Record(command_buffer);
                });
                FramePresent();
            }
            FrameMark;
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
    DeleteObject(background);
    DestroyWindowIcons(icon, icon_small);
    return exit_code;
    } catch (const std::exception& error) {
        AppendErrorLog(paths.LogFile, DiagnosticText(SR_PRODUCT_TITLE, SR_GIT_COMMIT, paths.Root, error.what()));
        throw;
    }
}

}  // namespace

int skeleton_repair::RunModelTools(const std::vector<std::filesystem::path>& args) {
    try {
        return Run(args);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        MessageBoxA(nullptr, error.what(), SR_PRODUCT_TITLE, MB_ICONERROR);
        return 1;
    }
}
