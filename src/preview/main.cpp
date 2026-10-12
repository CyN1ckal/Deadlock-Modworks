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
#include "library_panel.hpp"
#include "mod_store.hpp"
#include "background.hpp"

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
#include <future>
#include <atomic>
#include <chrono>

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
    app.pApplicationName = "Deadlock Mod Manager";
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

    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE + 128};
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

using ArchivePtr = std::shared_ptr<const IndexedArchive>;

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
    enum class Page { Gallery, Pack, Inspect, Repair };
    Page CurrentPage = Page::Gallery;
    std::shared_ptr<ArchiveCache> Archives = std::make_shared<ArchiveCache>();
    PreviewPanel Panel;
    ComparisonView Compare;
    ArchivePtr Mod;
    std::string ModName;
    std::string Selected;
    ArchivePtr Game;
    std::string GameName;
    std::string GameError;
    std::filesystem::path GameLoading;
    LatestTask<ArchivePtr> GameLoader;
    LatestTask<Inspection> Inspector;
    std::optional<std::string> PendingClip;
    // A model to inspect once its archive has been imported, from the command line.
    std::optional<std::pair<std::filesystem::path, std::string>> InspectAfterImport;
    RepairPanel Repair;
    LibraryPanel Builder;
    std::vector<std::filesystem::path> ModPaths;
    std::filesystem::path ActiveMod;
    bool StandaloneModel = false;
    std::optional<LocalModStore> Store;
    std::filesystem::path ConfiguredGame;
    std::optional<ModelLibrary> SavedState;
    std::string StoreStatus;
    std::string SaveError;
    double LastSaveAttempt = -10;
    struct ImportResult {
        ModelLibrary Library;
        std::string Errors;
        // Each source with its stored copy.
        std::vector<std::pair<std::filesystem::path, std::filesystem::path>> Imported;
    };
    std::future<ImportResult> ImportWorker;
    CancellationSource ImportCancellation;
    std::atomic<size_t> ImportedCount = 0;
    size_t ImportTotal = 0;

    bool ImportBusy() const { return ImportWorker.valid(); }
    bool FilesBusy() const { return Repair.Busy() || Builder.Busy() || ImportBusy(); }

    void CloseInspection() {
        Inspector.Cancel();
        Mod.reset(); Selected.clear(); ActiveMod.clear(); StandaloneModel = false;
        Panel.ShowError("", ""); Compare.ShowError("", "");
    }

    void ApplyLibrary(const ModelLibrary& library) {
        ModPaths = library.Sources;
        CloseInspection();
        Builder.Restore(library);
        if (!library.GameArchive.empty() && library.GameArchive != ConfiguredGame) OpenGame(library.GameArchive);
        SyncSources();
    }

    void Persist(bool force = false) {
        if (!Store || ImportBusy()) return;
        const auto current = Builder.Snapshot(ModPaths, ConfiguredGame);
        if (SavedState && *SavedState == current) return;
        if (!force && !SaveError.empty() && ImGui::GetTime() - LastSaveAttempt < 5) return;
        LastSaveAttempt = ImGui::GetTime();
        try { Store->Save(current); SavedState = current; SaveError.clear(); }
        catch (const std::exception& error) { SaveError = error.what(); }
    }

    void StartImport(const ModelLibrary& incoming, bool replace) {
        if (FilesBusy()) return;
        if (!Store) { StoreStatus = "Local mod storage is unavailable. " + SaveError; return; }
        ImportCancellation = CancellationSource{};
        ImportedCount = 0; ImportTotal = incoming.Sources.size();
        StoreStatus = "Importing mods into local storage...";
        auto starting = replace ? incoming : Builder.Snapshot(ModPaths, ConfiguredGame);
        if (replace) starting.Sources.clear();
        ImportWorker = std::async(std::launch::async, [root = Store->Root(), incoming, starting = std::move(starting),
            token = ImportCancellation.Token(), completed = &ImportedCount]() mutable {
            ImportResult result{std::move(starting), {}, {}};
            LocalModStore store(root);
            for (const auto& source : incoming.Sources) {
                try {
                    const auto cached = store.Import(source, token);
                    result.Imported.emplace_back(source, cached);
                    if (std::none_of(result.Library.Sources.begin(), result.Library.Sources.end(), [&](const auto& other) {
                        return skeleton_repair::EqualsIgnoreCase(Narrow(other.wstring()), Narrow(cached.wstring()));
                    })) result.Library.Sources.push_back(cached);
                    for (auto& model : result.Library.Selected) if (skeleton_repair::EqualsIgnoreCase(
                        Narrow(std::filesystem::absolute(model.Archive).lexically_normal().wstring()),
                        Narrow(std::filesystem::absolute(source).lexically_normal().wstring()))) model.Archive = cached;
                    auto checkpoint = result.Library;
                    std::erase_if(checkpoint.Selected, [&](const auto& model) { return std::none_of(checkpoint.Sources.begin(), checkpoint.Sources.end(),
                        [&](const auto& path) { return skeleton_repair::EqualsIgnoreCase(Narrow(path.wstring()), Narrow(model.Archive.wstring())); }); });
                    store.Save(checkpoint);
                } catch (const OperationCancelled&) { result.Errors += "Import cancelled. Completed imports were kept.\n"; break; }
                catch (const std::exception& error) { result.Errors += Narrow(source.filename().wstring()) + ": " + error.what() + "\n"; }
                ++*completed;
            }
            std::erase_if(result.Library.Selected, [&](const auto& model) { return std::none_of(result.Library.Sources.begin(), result.Library.Sources.end(),
                [&](const auto& source) { return skeleton_repair::EqualsIgnoreCase(Narrow(source.wstring()), Narrow(model.Archive.wstring())); }); });
            return result;
        });
    }

    void PollImport() {
        if (!ImportBusy() || ImportWorker.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        try {
            const auto result = ImportWorker.get();
            ApplyLibrary(result.Library);
            StoreStatus = result.Errors.empty() ? "Imports saved locally. Your collection will reopen automatically." : result.Errors;
            Persist(true);
            if (InspectAfterImport) {
                for (const auto& [source, cached] : result.Imported) {
                    if (source == InspectAfterImport->first) StartInspect(cached, InspectAfterImport->second);
                }
            }
        } catch (const std::exception& error) { StoreStatus = error.what(); }
        InspectAfterImport.reset();
    }

    void SyncSources() {
        std::vector<std::string> paths;
        for (const auto& path : ModPaths) paths.push_back(Narrow(path.wstring()));
        Repair.SetSources(Game ? Narrow(std::filesystem::path(Widen(GameName)).parent_path().wstring()) : "", std::move(paths));
    }

    explicit App(const VulkanDevice& device) : Panel(device), Compare(device), Builder(device, Archives) {
        try {
            Store.emplace(LocalModStore::DefaultRoot());
            const auto library = Store->Load();
            ApplyLibrary(library);
            SavedState = library;
        } catch (const std::exception& error) {
            SaveError = error.what();
            SavedState = Builder.Snapshot(ModPaths, ConfiguredGame);
        }
    }
    ~App() {
        ImportCancellation.Cancel();
        if (ImportBusy()) { ImportWorker.wait(); PollImport(); }
        Persist(true);
    }

    void OpenGame(const std::filesystem::path& chosen) {
        Game.reset();
        GameName.clear();
        GameError.clear();
        GameLoading = GamePak(chosen);
        ConfiguredGame = std::filesystem::absolute(GameLoading).lexically_normal();
        // The game directory has hundreds of thousands of entries, so it is parsed off the UI thread.
        GameLoader.Start([archives = Archives, pak = GameLoading](CancellationToken) { return archives->Get(pak); });
        SyncSources();
    }

    void PollGame() {
        try {
            const auto game = GameLoader.Take();
            if (!game) return;
            Game = *game;
            GameName = Narrow(GameLoading.wstring());
        } catch (const std::exception& error) {
            GameError = Narrow(GameLoading.wstring()) + ": " + error.what();
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
            CurrentPage = Page::Inspect;
        } else CurrentPage = Page::Gallery;
        SyncSources();
    }

    void AddMods(HWND hwnd) {
        AddPaths(AskForFiles(hwnd, L"Mod VPKs and models (*.vpk;*.vmdl_c)\0*.vpk;*.vmdl_c\0All files\0*.*\0",
            L"Add reference mods (Ctrl / Shift to select multiple)", true));
    }

    void DrawBrowser(HWND hwnd) {
        ImGui::Begin("Navigation", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
        ImGui::TextColored({.47f, .85f, .77f, 1.f}, "DEADLOCK");
        ImGui::SetWindowFontScale(1.45f);
        ImGui::TextUnformatted("Mod Manager");
        ImGui::SetWindowFontScale(1.f);
        ImGui::TextDisabled("Your skins. Your loadout.");
        ImGui::Spacing(); ImGui::Spacing();
        const auto nav = [&](const char* label, Page page) {
            if (ImGui::Selectable(label, CurrentPage == page, 0, {0, 36})) CurrentPage = page;
        };
        nav("Mod library", Page::Gallery);
        nav("VPK builder", Page::Pack);
        nav("Skin repairs", Page::Repair);
        nav("Model inspector", Page::Inspect);
        ImGui::Spacing();
        ImGui::SeparatorText("Collection");
        ImGui::Text("%zu source archives", ModPaths.size());
        ImGui::Text("%zu skins in pack", Builder.SelectedCount());
        ImGui::BeginDisabled(FilesBusy());
        if (ImGui::Button("Add mods...", {-1, 38})) AddMods(hwnd);
        if (!ModPaths.empty() && ImGui::CollapsingHeader("Manage sources")) {
            std::optional<std::filesystem::path> remove;
            for (const auto& path : ModPaths) {
                const auto key = Narrow(path.wstring());
                ImGui::PushID(key.c_str());
                ImGui::TextWrapped("%s", Narrow(path.filename().wstring()).c_str());
                ImGui::SetItemTooltip("%s", key.c_str());
                if (ImGui::SmallButton("Remove from library")) remove = path;
                ImGui::PopID();
                ImGui::Spacing();
            }
            if (remove) {
                std::erase(ModPaths, *remove);
                if (ActiveMod == *remove) CloseInspection();
                SyncSources();
                Builder.Restore(Builder.Snapshot(ModPaths, ConfiguredGame));
            }
        }
        ImGui::Spacing();
        ImGui::SeparatorText("Deadlock connection");
        if (Game) {
            ImGui::TextColored({.47f, .85f, .67f, 1.f}, "Game connected");
            ImGui::SetItemTooltip("%s", GameName.c_str());
        } else if (GameLoader.Busy()) {
            ImGui::TextDisabled("Connecting to Deadlock...");
            ImGui::SetItemTooltip("%s", Narrow(GameLoading.wstring()).c_str());
        } else {
            ImGui::TextDisabled("Game not connected");
            ImGui::TextWrapped("Connect Deadlock for textures, skin repairs and pack dependencies.");
        }
        if (!GameError.empty()) ImGui::TextWrapped("%s", GameError.c_str());
        if (ImGui::Button("Choose game archive...", {-1, 32})) {
            const auto paths = AskForFiles(hwnd, L"Game archive (pak01_dir.vpk)\0pak01_dir.vpk\0VPK archives (*.vpk)\0*.vpk\0",
                L"Select game\\citadel\\pak01_dir.vpk");
            if (!paths.empty()) OpenGame(paths.front());
        }
        ImGui::EndDisabled();
        if (Store) {
            ImGui::TextDisabled("Stored on this computer");
            ImGui::SetItemTooltip("%s", Narrow(Store->Root().wstring()).c_str());
        }
        if (!SaveError.empty()) ImGui::TextWrapped("%s", SaveError.c_str());
        if (ImportBusy()) {
            ImGui::Text("Importing %zu / %zu", ImportedCount.load(), ImportTotal);
            if (ImGui::Button("Cancel import")) ImportCancellation.Cancel();
        } else if (!StoreStatus.empty()) ImGui::TextWrapped("%s", StoreStatus.c_str());
        if (Repair.Busy() || Builder.Busy()) {
            ImGui::Spacing();
            ImGui::TextColored({1.f, .76f, .43f, 1.f}, Repair.Busy() ? "Checking / repairing skins..." : "Building your VPK...");
        }
        ImGui::End();
    }

    void DrawWorkspace(HWND hwnd) {
        ImGui::Begin("Workspace", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
        if (CurrentPage == Page::Gallery || CurrentPage == Page::Pack) {
            Builder.Draw(hwnd, ModPaths, Game ? std::filesystem::path(Widen(GameName)) : std::filesystem::path{},
                // Thumbnails wait for a connecting game, which supplies most textures, instead of loading twice.
                Repair.Busy() || ImportBusy(), Repair.Busy() || GameLoader.Busy(), CurrentPage == Page::Pack, [&] { AddMods(hwnd); },
                [&](const ModelLibrary& library) {
                    StartImport(library, true);
                },
                [&](const LibraryModel& model) { StartInspect(model.Archive, model.Model); CurrentPage = Page::Inspect; });
        } else if (CurrentPage == Page::Inspect) {
            if (ImGui::Button("< Back to library")) CurrentPage = Page::Gallery;
            ImGui::SameLine(); ImGui::TextDisabled("%s", ModName.c_str());
            ImGui::SeparatorText("Model inspector");
            if (Inspector.Busy()) {
                ImGui::TextDisabled("Loading %s...", Selected.empty() ? ModName.c_str() : Selected.c_str());
            } else if (Selected.empty() && !StandaloneModel) {
                ImGui::TextWrapped("Choose Inspect on a skin in the gallery to view its textures, animations and skeleton, or compare it with the installed game.");
                if (ImGui::Button("Browse skins", {150, 36})) CurrentPage = Page::Gallery;
            } else if (ImGui::BeginTabBar("inspection")) {
                if (ImGui::BeginTabItem("Preview")) { Panel.Draw(); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Compare versions")) { Compare.Draw(); ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
        } else {
            ImGui::TextColored({.47f, .85f, .77f, 1.f}, "COLLECTION TOOLS");
            ImGui::SetWindowFontScale(1.6f); ImGui::TextUnformatted("Skin repairs"); ImGui::SetWindowFontScale(1.f);
            ImGui::TextDisabled("Keep your collection compatible with game updates.");
            ImGui::Spacing();
            ImGui::BeginDisabled(Builder.Busy() || ImportBusy());
            Repair.Draw(hwnd, [&] {
                // Release every read of the mod archives before they are replaced.
                Inspector.Cancel(true);
                Builder.StopLoading();
                Mod.reset();
                Panel.ShowError("", "");
                Compare.ShowError("", "");
            });
            ImGui::EndDisabled();
        }
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
    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"Deadlock Mod Manager", WS_OVERLAPPEDWINDOW, 100, 100, 1440, 960, nullptr, nullptr, wc.hInstance, nullptr);
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
        // A game restored from the saved library may still be connecting; only detect one when none is configured.
        if (!game_folder && app.ConfiguredGame.empty()) {
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
            if (positional.size() > 1 && EndsWith(Narrow(positional[0].wstring()), ".vpk") && EndsWith(Narrow(positional[1].wstring()), ".vmdl_c")) {
                app.PendingClip = clip;
                app.OpenArchiveModel(positional[0], VpkArchive::Normalize(Narrow(positional[1].wstring())));
                app.CurrentPage = App::Page::Inspect;
            } else {
                app.AddPaths(positional);
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

            app.Builder.Poll();
            app.PollImport();
            app.PollGame();
            app.PollInspect();

            const bool repaired = app.Repair.Poll();
            if (repaired) {
                // Repaired archives were replaced on disk, so reopen them.
                app.Archives->Remove(app.ModPaths);
                app.Builder.InvalidateThumbnails();
            }
            if (repaired && !app.ActiveMod.empty()) app.StartInspect(app.ActiveMod, app.Selected);
            if (!g_dropped.empty()) {
                if (!app.FilesBusy()) app.AddPaths(g_dropped);
                g_dropped.clear();
            }

            const ImGuiID dockspace = ImGui::DockSpaceOverViewport();
            if (first_frame) {
                // Collection navigation on the left, thumbnail gallery filling the workspace.
                ImGui::DockBuilderRemoveNode(dockspace);
                ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
                ImGuiID right = 0;
                const ImGuiID left = ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Left, 0.19f, nullptr, &right);
                ImGui::DockBuilderDockWindow("Navigation", left);
                ImGui::DockBuilderDockWindow("Workspace", right);
                ImGui::DockBuilderFinish(dockspace);
                first_frame = false;
            }

            app.DrawBrowser(hwnd);
            app.DrawWorkspace(hwnd);
            app.Persist();

            ImGui::Render();
            ImDrawData* draw_data = ImGui::GetDrawData();
            if (draw_data->DisplaySize.x > 0 && draw_data->DisplaySize.y > 0) {
                g_window.ClearValue.color = {{0.06f, 0.06f, 0.07f, 1.0f}};
                FrameRender(draw_data, [&](VkCommandBuffer command_buffer) {
                    app.Panel.Record(command_buffer);
                    app.Compare.Record(command_buffer);
                    app.Builder.Record(command_buffer);
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
