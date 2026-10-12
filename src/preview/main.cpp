// Model preview tool: opens a .vmdl_c or a VPK and shows the decoded model with PreviewPanel.
// Window and swapchain setup follow Dear ImGui's example_win32_vulkan.

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
    enum class Page { Gallery, Pack, History, Inspect, Repair, Multi };
    Page CurrentPage = Page::Gallery;
    Page PreviousPage = Page::Gallery;
    // Set while a page shown from code waits for its tab to become selected.
    bool PageRequested = true;
    bool ShowExplorer = true;
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
    LatestTask<ArchivePtr> GameLoader;
    LatestTask<Inspection> Inspector;
    std::optional<std::string> PendingClip;
    // A model to inspect once its archive has been imported, from the command line.
    std::optional<std::pair<std::filesystem::path, std::string>> InspectAfterImport;
    RepairPanel Repair;
    LibraryPanel Builder;
    HistoryPanel History;
    // Animation clips in the game archive, which history previews also offer.
    std::vector<std::string> GameClips;
    // Shows the comparison tab of the inspector on the next frame.
    bool FocusCompare = false;
    std::vector<std::filesystem::path> ModPaths;
    std::filesystem::path ActiveMod;
    bool StandaloneModel = false;
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
    };
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

    bool ImportBusy() const { return ImportWorker.valid(); }
    bool FilesBusy() const { return Repair.Busy() || Builder.Busy() || History.Busy() || ImportBusy(); }

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

    void Persist(bool force = false) {
        if (!Store || ImportBusy() || !CanPersist) return;
        const auto current = Builder.Snapshot(ModPaths, ConfiguredGame);
        if (SavedState && *SavedState == current) return;
        if (!force && !SaveError.empty() && ImGui::GetTime() - LastSaveAttempt < 5) return;
        LastSaveAttempt = ImGui::GetTime();
        try { Store->Save(current); SavedState = current; SaveError.clear(); }
        catch (const std::exception& error) { SaveError = error.what(); }
    }

    // Reads the saved library from the database, merging the folder-based store of earlier releases on first use.
    void StartRestore() {
        Restoring = true;
        ImportCancellation = CancellationSource{};
        ImportedCount = 0; ImportTotal = 0;
        StoreStatus = "Opening your library...";
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
        if (!CanPersist) { StoreStatus = "Your library has not been opened, so nothing can be added to it yet. " + SaveError; return; }
        // The merge appends to the saved collection, so it must match what is on screen.
        Persist(true);
        if (!SaveError.empty()) { StoreStatus = "Could not save your library before adding another: " + SaveError; return; }
        ImportCancellation = CancellationSource{};
        ImportedCount = 0; ImportTotal = 0;
        StoreStatus = "Adding " + Narrow(other.filename().wstring()) + " to your library...";
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
        if (!CanPersist) { StoreStatus = "Your library has not been opened, so nothing can be renamed yet. " + SaveError; return; }
        // The reopened library replaces what is on screen, so it must be saved first.
        Persist(true);
        if (!SaveError.empty()) { StoreStatus = "Could not save your library before renaming: " + SaveError; return; }
        ReleaseArchives();
        ImportCancellation = CancellationSource{};
        ImportedCount = 0; ImportTotal = 0;
        StoreStatus = "Renaming...";
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
        if (!Store) { StoreStatus = "Local mod storage is unavailable. " + SaveError; return; }
        ImportCancellation = CancellationSource{};
        ImportedCount = 0; ImportTotal = incoming.Sources.size();
        StoreStatus = "Importing mods into local storage...";
        auto starting = replace ? incoming : Builder.Snapshot(ModPaths, ConfiguredGame);
        if (replace) starting.Sources.clear();
        ImportWorker = std::async(std::launch::async, [root = Store->Root(), incoming, starting = std::move(starting),
            token = ImportCancellation.Token(), completed = &ImportedCount, checkpoints = CanPersist]() mutable {
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

    void PollImport() {
        if (!ImportBusy() || ImportWorker.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        try {
            const auto result = ImportWorker.get();
            ApplyLibrary(result.Library);
            if (result.Restored) { SavedState = result.Library; CanPersist = true; }
            StoreStatus = !result.Errors.empty() ? result.Errors
                : !result.Done.empty() ? result.Done
                : Restoring ? "" : "Saved in your library. Your collection will reopen automatically.";
            Persist(true);
            if (InspectAfterImport && !Restoring) {
                for (const auto& [source, cached] : result.Imported) {
                    if (source == InspectAfterImport->first) StartInspect(cached, InspectAfterImport->second);
                }
                InspectAfterImport.reset();
            }
        } catch (const std::exception& error) {
            StoreStatus = error.what();
            if (!Restoring) InspectAfterImport.reset();
        }
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

    static std::filesystem::path ContentDirectory() {
        try { return LocalModStore(LocalModStore::DefaultRoot()).ContentDirectory(); }
        catch (const std::exception&) { return {}; }
    }

    explicit App(const VulkanDevice& device) : Panel(device), Compare(device), Multi(device), Builder(device, Archives), History(ContentDirectory()) {
        Multi.SetArchives(Archives);
        Builder.SetMultiViewHandler([this](const LibraryModel& model) {
            Multi.Open(model, Builder.Models());
            ShowPage(Page::Multi);
        });
        try {
            Store.emplace(LocalModStore::DefaultRoot());
            StartRestore();
        } catch (const std::exception& error) {
            SaveError = error.what();
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
            for (const VpkEntry& entry : Game->Archive().Entries()) {
                if (EndsWith(entry.FullPath, ".vnmclip_c")) GameClips.push_back(entry.FullPath);
            }
            Multi.SetGame(Game);
        } catch (const std::exception& error) {
            GameError = Narrow(GameLoading.wstring()) + ": " + error.what();
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
            if (mods.empty()) { StoreStatus = "No mod VPKs found in " + Narrow(folder->wstring()) + "."; return; }
            AddPaths(mods);
        } catch (const std::exception& error) {
            StoreStatus = error.what();
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
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_E, global)) ShowExplorer = !ShowExplorer;
        if (ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_LeftArrow, global)) ShowPage(PreviousPage);
        if (ImGui::Shortcut(ImGuiKey_F1, global)) OpenShortcuts = true;
        const Page pages[] = {Page::Gallery, Page::Pack, Page::History, Page::Repair, Page::Inspect, Page::Multi};
        const ImGuiKey keys[] = {ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5, ImGuiKey_6};
        for (size_t i = 0; i < std::size(pages); ++i) {
            if (ImGui::Shortcut(ImGuiMod_Ctrl | keys[i], global)) ShowPage(pages[i]);
        }
    }

    void DrawMenuBar(HWND hwnd) {
        if (!ImGui::BeginMainMenuBar()) return;
        HandleShortcuts(hwnd);
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Add mods...", "Ctrl+O", false, !FilesBusy())) AddMods(hwnd);
            if (ImGui::MenuItem("Add folder...", "Ctrl+Shift+O", false, !FilesBusy())) AddFolder(hwnd);
            ImGui::SetItemTooltip("Adds every mod VPK in a folder, such as Deadlock's addons folder. Chunk files and the game archive are skipped.");
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
                {"Multi view", "Ctrl+6", Page::Multi}};
            for (const auto& page : pages) {
                if (ImGui::MenuItem(page.Label, page.Keys, CurrentPage == page.Value)) ShowPage(page.Value);
            }
            if (ImGui::MenuItem("Back", "Alt+Left")) ShowPage(PreviousPage);
            ImGui::Separator();
            ImGui::MenuItem("Explorer", "Ctrl+Shift+E", &ShowExplorer);
            ImGui::MenuItem("Status bar", nullptr, &ShowStatusBar);
            ImGui::Separator();
            if (ImGui::MenuItem("Refresh library", "F5", false, !PackBusy())) Builder.Reload(ModPaths);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            const bool repair_allowed = !Builder.Busy() && !ImportBusy();
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
        if (!SaveError.empty()) { color = ui::kError; return &SaveError; }
        if (!GameError.empty()) { color = ui::kError; return &GameError; }
        if (!StoreStatus.empty() && !ImportBusy()) { color = ImGui::GetStyleColorVec4(ImGuiCol_Text); return &StoreStatus; }
        return nullptr;
    }

    void DrawStatusBar(HWND hwnd) {
        if (!ShowStatusBar) return;
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
        if (ImGui::BeginViewportSideBar("##statusbar", ImGui::GetMainViewport(), ImGuiDir_Down, ImGui::GetFrameHeight(), flags)
            && ImGui::BeginMenuBar()) {
            ImVec4 color;
            const char* label = nullptr;
            GameState(color, label);
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            const bool open = ImGui::BeginMenu((std::string("\xE2\x97\x8F ") + label + "###game").c_str());
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
                ImGui::TextUnformatted(Restoring ? "Opening library" : "Importing");
                if (total > 0) {
                    ImGui::ProgressBar(static_cast<float>(done) / static_cast<float>(total), {90, ImGui::GetTextLineHeight() * .55f}, "");
                    ImGui::Text("%zu / %zu", done, total);
                }
                if (ImGui::SmallButton("Cancel##import")) ImportCancellation.Cancel();
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
        if (!ShowExplorer) return;
        DockWithoutTab();
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

        const float footer = ImGui::GetFrameHeightWithSpacing() * 4 + ImGui::GetTextLineHeightWithSpacing() * 2
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
            const auto name = Narrow(path.filename().wstring());
            if (!filter.PassFilter(name.c_str())) continue;
            const auto key = Narrow(path.wstring());
            ImGui::PushID(key.c_str());
            if (ImGui::Selectable(name.c_str(), active == path)) {
                Builder.SetSourceFilter(active == path ? std::filesystem::path{} : path);
                if (CurrentPage != Page::Pack) ShowPage(Page::Gallery);
            }
            const size_t skins = Builder.ModelsIn(path);
            const size_t files = Builder.PackageFiles(path);
            std::string tip = key;
            if (skins > 0) {
                tip += "\n" + std::to_string(skins) + (skins == 1 ? " skin" : " skins");
                if (files > 0) tip += ", " + std::to_string(files) + (files == 1 ? " other file" : " other files");
                tip += "\nClick to show its skins. Right-click for more.";
            } else {
                tip += "\n" + std::to_string(files) + (files == 1 ? " file" : " files");
                tip += "\nClick to show it. Right-click for more.";
            }
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
            ImGui::TextDisabled("No sources yet. Click Add, or drop mod VPKs on the window.");
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
        ImGui::SetItemTooltip("%s\nRight-click for more.", Game ? GameName.c_str() : "Connect Deadlock for textures, skin repairs and pack dependencies.");
        ImGui::SameLine();
        if (ui::ToolButton("...", "Choose game\\citadel\\pak01_dir.vpk (Ctrl+G)", !FilesBusy())) ChooseGame(hwnd);
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
        DockWithoutTab();
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
                });
        } else if (CurrentPage == Page::History) {
            const std::string game_directory = Game ? Narrow(GamePath().parent_path().wstring()) : std::string();
            History.Draw(hwnd, Panel, [game = Game](const std::string& path) { return game ? game->Read(path) : std::nullopt; }, GameClips,
                Compare, game_directory, Repair.Busy() || Builder.Busy() || ImportBusy(),
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
        } else {
            ImGui::BeginDisabled(Builder.Busy() || ImportBusy());
            Repair.Draw(hwnd, [&] { ReleaseArchives(); });
            ImGui::EndDisabled();
        }
        ImGui::End();
    }

    void DrawDialogs() {
        if (std::exchange(OpenShortcuts, false)) ImGui::OpenPopup("Keyboard shortcuts");
        if (std::exchange(OpenAbout, false)) ImGui::OpenPopup("About");
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {.5f, .5f});
        DrawRenamePopup();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, {.5f, .5f});
        if (ImGui::BeginPopupModal("Keyboard shortcuts", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            static const char* const kShortcuts[][2] = {
                {"Ctrl+O", "Add mods"}, {"Ctrl+Shift+O", "Add every mod in a folder"}, {"Ctrl+G", "Choose the game archive"},
                {"Ctrl+B", "Build the pack as a VPK"}, {"Ctrl+F", "Search skins, packages and sources"}, {"F5", "Reopen every source and reload thumbnails"},
                {"Ctrl+1 ... Ctrl+6", "Library, builder, history, repairs, inspector, multi view"}, {"Alt+Left", "Back to the previous page"},
                {"Ctrl+Shift+E", "Show or hide the explorer"}, {"Double-click", "Inspect a skin, list a package's files, or view a version"},
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
            ImGui::TextUnformatted("Deadlock Mod Manager");
            ImGui::TextDisabled("Browse skins, build packs, inspect models and repair skeleton changes.");
            ImGui::TextDisabled("Dear ImGui %s, Vulkan 1.3", ImGui::GetVersion());
            if (ImGui::Button("Close", {-1, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
};

// A compact dark theme in the style of a desktop IDE: neutral greys, one blue accent, thin borders and tight spacing.
void ApplyTheme(float scale) {
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowPadding = {6, 6};
    style.FramePadding = {6, 3};
    style.CellPadding = {4, 2};
    style.ItemSpacing = {6, 4};
    style.ItemInnerSpacing = {4, 4};
    style.TouchExtraPadding = {0, 0};
    style.IndentSpacing = 14;
    style.ScrollbarSize = 10;
    style.GrabMinSize = 8;
    style.WindowRounding = 0;
    style.ChildRounding = 2;
    style.FrameRounding = 2;
    style.PopupRounding = 3;
    style.ScrollbarRounding = 3;
    style.GrabRounding = 2;
    style.TabRounding = 2;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 1;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 0;
    style.TabBorderSize = 0;
    style.TabBarBorderSize = 1;
    style.TabBarOverlineSize = 2;
    style.SeparatorTextBorderSize = 1;
    style.SeparatorTextPadding = {8, 2};
    style.DockingSeparatorSize = 1;
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.SelectableTextAlign = {0, .5f};
    style.HoverDelayShort = .25f;
    style.HoverDelayNormal = .5f;

    const ImVec4 background{.118f, .122f, .133f, 1.f};
    const ImVec4 panel{.153f, .157f, .169f, 1.f};
    const ImVec4 raised{.200f, .208f, .224f, 1.f};
    const ImVec4 hover{.255f, .263f, .282f, 1.f};
    const ImVec4 line{.235f, .243f, .259f, 1.f};
    const ImVec4 accent = ui::kAccent;
    const ImVec4 selection{.176f, .247f, .345f, 1.f};
    auto* colors = style.Colors;
    colors[ImGuiCol_Text] = {.874f, .882f, .898f, 1.f};
    colors[ImGuiCol_TextDisabled] = {.525f, .541f, .569f, 1.f};
    colors[ImGuiCol_WindowBg] = background;
    colors[ImGuiCol_ChildBg] = {0, 0, 0, 0};
    colors[ImGuiCol_PopupBg] = {.168f, .173f, .184f, .98f};
    colors[ImGuiCol_Border] = line;
    colors[ImGuiCol_BorderShadow] = {0, 0, 0, 0};
    colors[ImGuiCol_FrameBg] = panel;
    colors[ImGuiCol_FrameBgHovered] = raised;
    colors[ImGuiCol_FrameBgActive] = hover;
    colors[ImGuiCol_TitleBg] = panel;
    colors[ImGuiCol_TitleBgActive] = panel;
    colors[ImGuiCol_TitleBgCollapsed] = panel;
    colors[ImGuiCol_MenuBarBg] = panel;
    colors[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
    colors[ImGuiCol_ScrollbarGrab] = {.30f, .31f, .33f, 1.f};
    colors[ImGuiCol_ScrollbarGrabHovered] = {.38f, .39f, .41f, 1.f};
    colors[ImGuiCol_ScrollbarGrabActive] = {.45f, .46f, .48f, 1.f};
    colors[ImGuiCol_CheckMark] = accent;
    colors[ImGuiCol_SliderGrab] = {accent.x, accent.y, accent.z, .8f};
    colors[ImGuiCol_SliderGrabActive] = accent;
    colors[ImGuiCol_Button] = raised;
    colors[ImGuiCol_ButtonHovered] = hover;
    colors[ImGuiCol_ButtonActive] = selection;
    // Shared by selected rows and collapsing headers, so it stays muted.
    colors[ImGuiCol_Header] = selection;
    colors[ImGuiCol_HeaderHovered] = {.22f, .23f, .25f, 1.f};
    colors[ImGuiCol_HeaderActive] = {.21f, .30f, .43f, 1.f};
    colors[ImGuiCol_Separator] = line;
    colors[ImGuiCol_SeparatorHovered] = accent;
    colors[ImGuiCol_SeparatorActive] = accent;
    colors[ImGuiCol_ResizeGrip] = {0, 0, 0, 0};
    colors[ImGuiCol_ResizeGripHovered] = {accent.x, accent.y, accent.z, .5f};
    colors[ImGuiCol_ResizeGripActive] = accent;
    colors[ImGuiCol_Tab] = panel;
    colors[ImGuiCol_TabHovered] = raised;
    colors[ImGuiCol_TabSelected] = background;
    colors[ImGuiCol_TabSelectedOverline] = accent;
    colors[ImGuiCol_TabDimmed] = panel;
    colors[ImGuiCol_TabDimmedSelected] = background;
    colors[ImGuiCol_TabDimmedSelectedOverline] = line;
    colors[ImGuiCol_DockingPreview] = {accent.x, accent.y, accent.z, .4f};
    colors[ImGuiCol_DockingEmptyBg] = background;
    colors[ImGuiCol_TableHeaderBg] = panel;
    colors[ImGuiCol_TableBorderStrong] = line;
    colors[ImGuiCol_TableBorderLight] = {.19f, .196f, .21f, 1.f};
    colors[ImGuiCol_TableRowBg] = {0, 0, 0, 0};
    colors[ImGuiCol_TableRowBgAlt] = {1, 1, 1, .02f};
    colors[ImGuiCol_TextSelectedBg] = {accent.x, accent.y, accent.z, .35f};
    colors[ImGuiCol_NavCursor] = accent;
    colors[ImGuiCol_ModalWindowDimBg] = {0, 0, 0, .45f};
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
            app.KeepGame = true;
            app.OpenGame(*game_folder);
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
            const ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_NoWindowMenuButton);
            if (first_frame) {
                // Explorer on the left, pages filling the workspace. Neither shows a dock tab, since the pages have their own.
                ImGui::DockBuilderRemoveNode(dockspace);
                ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dockspace, ImGui::GetMainViewport()->WorkSize);
                ImGuiID right = 0;
                const ImGuiID left = ImGui::DockBuilderSplitNode(dockspace, ImGuiDir_Left, 0.17f, nullptr, &right);
                ImGui::DockBuilderDockWindow("Explorer", left);
                ImGui::DockBuilderDockWindow("Workspace", right);
                ImGui::DockBuilderFinish(dockspace);
                first_frame = false;
            }

            app.DrawExplorer(hwnd);
            app.DrawWorkspace(hwnd);
            app.DrawDialogs();
            app.Persist();

            ImGui::Render();
            ImDrawData* draw_data = ImGui::GetDrawData();
            if (draw_data->DisplaySize.x > 0 && draw_data->DisplaySize.y > 0) {
                g_window.ClearValue.color = {{0.118f, 0.122f, 0.133f, 1.0f}};
                FrameRender(draw_data, [&](VkCommandBuffer command_buffer) {
                    app.Panel.Record(command_buffer);
                    app.Compare.Record(command_buffer);
                    app.Multi.Record(command_buffer);
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
