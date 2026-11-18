// Model tools entry point: checks the data root and the Vulkan loader, opens the window, and runs the frame loop.
// The window, the GPU, Dear ImGui and the workspace are each owned by one object, released in reverse order.

// Tracy's winsock2.h comes before windows.h.
#include "profile.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "model_tools.hpp"
#include "mod_store.hpp"
#include "product_version.hpp"
#include "shell_window.hpp"
#include "startup.hpp"
#include "strings.hpp"
#include "theme.hpp"
#include "vpk.hpp"
#include "vulkan_window.hpp"
#include "workspace.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_win32.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace skeleton_repair;

struct Options {
    std::optional<std::filesystem::path> DataRoot;
    std::optional<std::filesystem::path> Game;
    std::optional<std::string> Clip;
    std::vector<std::filesystem::path> Files;
};

Options ParseOptions(const std::vector<std::filesystem::path>& args) {
    Options options;
    for (size_t i = 0; i < args.size(); ++i) {
        const auto value = [&](const char* what) {
            if (i + 1 >= args.size()) throw std::runtime_error(Narrow(args[i].wstring()) + " needs " + what + ".");
            return args[++i];
        };
        if (args[i] == L"--data-root") options.DataRoot = value("a directory");
        else if (args[i] == L"--game") options.Game = value("a game folder");
        else if (args[i] == L"--clip") options.Clip = VpkArchive::Normalize(Narrow(value("a clip path").wstring()));
        else options.Files.push_back(args[i]);
    }
    return options;
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

void ShowStartupFailure(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    MessageBoxW(nullptr, Widen(message).c_str(), SR_PRODUCT_TITLE_W, MB_ICONERROR | MB_SETFOREGROUND);
}

// Dear ImGui's context with its Win32 and Vulkan backends, the theme and the fonts.
class ImGuiSession {
public:
    ImGuiSession(const ShellWindow& shell, VulkanWindow& gpu, std::string layout_ini) : layout_ini_(std::move(layout_ini)) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        try {
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
            // ImGui stores this pointer and does not copy it, so the string lives as long as the context.
            io.IniFilename = layout_ini_.c_str();
            const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(shell.Handle());
            theme::Apply(scale);
            LoadFonts(std::round(15.f * scale));
            ImGui_ImplWin32_Init(shell.Handle());
            win32_ = true;
            gpu.InitImGui();
            vulkan_ = true;
        } catch (...) {
            Shutdown();
            throw;
        }
    }
    ~ImGuiSession() { Shutdown(); }
    ImGuiSession(const ImGuiSession&) = delete;
    ImGuiSession& operator=(const ImGuiSession&) = delete;

private:
    static void LoadFonts(float size) {
        ImGuiIO& io = ImGui::GetIO();
        wchar_t windows_folder[MAX_PATH]{};
        GetWindowsDirectoryW(windows_folder, MAX_PATH);
        const auto fonts = std::filesystem::path(windows_folder) / L"Fonts";
        std::error_code error;
        if (std::filesystem::exists(fonts / L"segoeui.ttf", error)) io.Fonts->AddFontFromFileTTF(Narrow((fonts / L"segoeui.ttf").wstring()).c_str(), size);
        else io.Fonts->AddFontDefault();
        // Status dots, arrows and check marks come from Segoe UI Symbol.
        if (std::filesystem::exists(fonts / L"seguisym.ttf", error)) {
            static const ImWchar kSymbolRanges[] = {0x2022, 0x2026, 0x2190, 0x21FF, 0x2300, 0x23FF, 0x25A0, 0x25FF, 0x2600, 0x27BF, 0};
            ImFontConfig merge;
            merge.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(Narrow((fonts / L"seguisym.ttf").wstring()).c_str(), size, &merge, kSymbolRanges);
        }
        // Headings use Segoe UI Bold at a larger size. Without it, ui::Heading keeps the body font.
        if (std::filesystem::exists(fonts / L"segoeuib.ttf", error)) {
            theme::SetHeadingFont(io.Fonts->AddFontFromFileTTF(Narrow((fonts / L"segoeuib.ttf").wstring()).c_str(),
                std::round(size * theme::kHeadingScale)));
        }
    }

    void Shutdown() {
        if (std::exchange(vulkan_, false)) ImGui_ImplVulkan_Shutdown();
        if (std::exchange(win32_, false)) ImGui_ImplWin32_Shutdown();
        if (ImGui::GetCurrentContext() != nullptr) ImGui::DestroyContext();
    }

    std::string layout_ini_;
    bool win32_ = false;
    bool vulkan_ = false;
};

// Waits for the GPU when it goes out of scope, so the viewports destroyed after it free nothing the GPU still reads.
struct GpuIdleGuard {
    const VulkanWindow& Gpu;
    ~GpuIdleGuard() {
        try {
            Gpu.WaitIdle();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s\n", error.what());
        }
    }
};

int Run(const std::vector<std::filesystem::path>& args) {
    [[maybe_unused]] const TracySession tracy_session;
    const Options options = ParseOptions(args);
    const std::wstring data_root_env = EnvironmentValue(L"DEADLOCK_MODWORKS_DATA_ROOT");
    const std::filesystem::path data_root = std::filesystem::absolute(
        ResolveDataRoot(options.DataRoot, data_root_env.empty() ? nullptr : data_root_env.c_str())).lexically_normal();
    const DataPaths paths = DataPathsFor(data_root);
    const auto log = [&](std::string_view message) {
        AppendErrorLog(paths.LogFile, DiagnosticText(SR_PRODUCT_TITLE, SR_GIT_COMMIT, paths.Root, message));
    };

    const DataRootInspection storage = InspectDataRoot(paths.Root);
    if (!storage.Writable) {
        ShowStartupFailure(storage.Message.empty() ? UnwritableDataRootMessage(paths.Root) : storage.Message);
        return 1;
    }
    // vulkan-1.dll is delay-loaded, so a missing loader is reported here instead of failing before main.
    if (LoadLibraryW(L"vulkan-1.dll") == nullptr) {
        log(kMissingVulkanLoaderMessage);
        ShowStartupFailure(kMissingVulkanLoaderMessage);
        return 1;
    }
    const SettingsInspection settings = InspectLibrarySettings(paths.StoreDirectory);
    if (settings.State == SettingsState::Corrupt) {
        log(settings.Message);
        ShowStartupFailure(settings.Message);
    }

    try {
        ShellWindow shell(paths.WindowIni);
        VulkanWindow gpu(shell.Handle(), shell.Instance());
        shell.Show();
        const ImGuiSession imgui(shell, gpu, Narrow(paths.LayoutIni.wstring()));
        Workspace workspace(gpu.Device(), paths.Root, shell.Handle());
        const GpuIdleGuard idle{gpu};
        workspace.Start(settings, options.Game, options.Files, options.Clip);

        while (shell.PumpMessages()) {
            ZoneScopedN("Frame");
            if (shell.Minimized()) {
                Sleep(10);
                FrameMark;
                continue;
            }
            if (shell.TakeResized()) gpu.RequestRebuild();
            const auto [width, height] = shell.ClientSize();
            gpu.RebuildIfNeeded(width, height);
            {
                ZoneScopedN("ImGui new frame");
                ImGui_ImplVulkan_NewFrame();
                ImGui_ImplWin32_NewFrame();
                ImGui::NewFrame();
            }
            workspace.Frame(shell.TakeDropped());
            {
                ZoneScopedN("ImGui render");
                ImGui::Render();
            }
            gpu.Draw(ImGui::GetDrawData(), theme::kClearRgb, [&](VkCommandBuffer command_buffer) { workspace.Record(command_buffer); });
            FrameMark;
        }
        workspace.Close();
        return 0;
    } catch (const std::exception& error) {
        log(error.what());
        throw;
    }
}

}  // namespace

int skeleton_repair::RunModelTools(const std::vector<std::filesystem::path>& args) {
    try {
        return Run(args);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        MessageBoxW(nullptr, Widen(error.what()).c_str(), SR_PRODUCT_TITLE_W, MB_ICONERROR);
        return 1;
    }
}
