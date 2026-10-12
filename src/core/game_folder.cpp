#include "game_folder.hpp"

#include "strings.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>

namespace skeleton_repair {
namespace {

namespace fs = std::filesystem;

std::optional<std::wstring> SteamPath() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, nullptr, &size)
        != ERROR_SUCCESS) {
        return std::nullopt;
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, value.data(), &size)
        != ERROR_SUCCESS) {
        return std::nullopt;
    }
    value.resize(wcsnlen(value.c_str(), value.size()));
    return value;
}

}  // namespace

std::vector<std::string> ParseSteamLibraryPaths(std::string_view vdf) {
    static const std::regex pattern(R"rx("path"\s+"([^"]+)")rx");
    std::vector<std::string> paths;
    const std::string text(vdf);
    for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern); it != std::sregex_iterator(); ++it) {
        std::string path = (*it)[1].str();
        for (size_t at = path.find("\\\\"); at != std::string::npos; at = path.find("\\\\", at + 1)) {
            path.erase(at, 1);
        }
        paths.push_back(std::move(path));
    }
    return paths;
}

std::optional<std::string> FindGameFolder() {
    std::vector<std::string> libraries;
    if (const auto steam = SteamPath(); steam && !steam->empty()) {
        libraries.push_back(Narrow(*steam));
    }
    libraries.push_back("C:\\Program Files (x86)\\Steam");
    const std::vector<std::string> steam_roots = libraries;
    for (const std::string& steam : steam_roots) {
        std::error_code error;
        const fs::path vdf = fs::path(Widen(steam)) / L"steamapps" / L"libraryfolders.vdf";
        if (!fs::is_regular_file(vdf, error)) {
            continue;
        }
        std::ifstream stream(vdf, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        const std::vector<std::string> listed = ParseSteamLibraryPaths(text);
        libraries.insert(libraries.end(), listed.begin(), listed.end());
    }

    std::vector<std::string> seen;
    for (const std::string& library : libraries) {
        if (std::any_of(seen.begin(), seen.end(), [&](const std::string& other) { return EqualsIgnoreCase(other, library); })) {
            continue;
        }
        seen.push_back(library);
        const fs::path citadel = fs::path(Widen(library)) / L"steamapps" / L"common" / L"Deadlock" / L"game" / L"citadel";
        std::error_code error;
        if (fs::is_regular_file(citadel / L"pak01_dir.vpk", error)) {
            return Narrow(citadel.lexically_normal().wstring());
        }
    }
    return std::nullopt;
}

}  // namespace skeleton_repair
