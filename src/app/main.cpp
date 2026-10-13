#include "cancellation.hpp"
#include "command_line.hpp"
#include "main_window.hpp"
#include "strings.hpp"
#ifdef SR_MODEL_TOOLS
#include "model_tools.hpp"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

skeleton_repair::CancellationSource g_cancellation;

BOOL WINAPI OnConsoleControl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        g_cancellation.Cancel();
        return TRUE;
    }
    return FALSE;
}

#ifdef SR_MODEL_TOOLS
// These open the model-tools window. They are not repair-CLI commands.
bool IsGuiFlag(const std::string& arg) {
    return arg == "--data-root" || arg == "--game" || arg == "--clip";
}
#endif

// Files dropped onto the executable open the window with those mods selected.
bool AreModFiles(const std::vector<std::string>& args) {
    return std::all_of(args.begin(), args.end(), [](const std::string& arg) {
        const std::filesystem::path path(skeleton_repair::Widen(arg));
        std::error_code error;
        const auto extension = skeleton_repair::Narrow(path.extension().wstring());
        return (skeleton_repair::EqualsIgnoreCase(extension, ".vpk")
#ifdef SR_MODEL_TOOLS
            || skeleton_repair::EqualsIgnoreCase(extension, ".vmdl_c")
#endif
            )
            && std::filesystem::is_regular_file(path, error);
    });
}

// Before Windows 11 24H2 the manifest's detached console policy is ignored, so a launch from
// Explorer still gets its own console. Close it unless a terminal shares it.
void ReleaseOwnConsole() {
    DWORD processes[2];
    if (GetConsoleWindow() != nullptr && GetConsoleProcessList(processes, 2) == 1) {
        FreeConsole();
    }
}

}  // namespace

int main() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        return 2;
    }

    std::vector<std::string> args;
    args.reserve(argc > 0 ? static_cast<size_t>(argc - 1) : 0);
    for (int i = 1; i < argc; ++i) {
        args.push_back(skeleton_repair::Narrow(argv[i]));
    }
    LocalFree(argv);

#ifdef SR_MODEL_TOOLS
    if (AreModFiles(args) || std::any_of(args.begin(), args.end(), IsGuiFlag)) {
        ReleaseOwnConsole();
        std::vector<std::filesystem::path> paths;
        for (const auto& arg : args) paths.emplace_back(skeleton_repair::Widen(arg));
        return skeleton_repair::RunModelTools(paths);
    }
#else
    if (AreModFiles(args)) {
        ReleaseOwnConsole();
        return skeleton_repair::RunMainWindow(args);
    }
#endif

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(&OnConsoleControl, TRUE);
    return skeleton_repair::CommandLine::Execute(args, [](std::string_view line) {
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }, g_cancellation.Token());
}
