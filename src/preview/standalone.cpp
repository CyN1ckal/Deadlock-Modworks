#include "model_tools.hpp"
#include "strings.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

int main() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::filesystem::path> args;
    for (int i = 1; argv != nullptr && i < argc; ++i) args.emplace_back(argv[i]);
    LocalFree(argv);
    return skeleton_repair::RunModelTools(args);
}
