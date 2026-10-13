#include "startup.hpp"

#include "strings.hpp"

#include <nlohmann/json.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <utility>

namespace skeleton_repair {
namespace {

namespace fs = std::filesystem;

std::string ReadFile(const fs::path& path, bool& opened) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        opened = false;
        return {};
    }
    opened = true;
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

}  // namespace

DataPaths DataPathsFor(const fs::path& root) {
    DataPaths paths;
    paths.Root = fs::absolute(root).lexically_normal();
    paths.LayoutIni = paths.Root / L"layout.ini";
    paths.WindowIni = paths.Root / L"window.ini";
    paths.LogFile = paths.Root / L"logs" / L"modworks.log";
    paths.StoreDirectory = paths.Root / L"store";
    paths.SettingsFile = paths.StoreDirectory / L"settings.json";
    paths.Catalog = paths.StoreDirectory / L"catalog.sqlite";
    return paths;
}

bool IsInsideDataRoot(const fs::path& root, const fs::path& path) {
    const fs::path base = fs::absolute(root).lexically_normal();
    const fs::path full = fs::absolute(path).lexically_normal();
    const fs::path relative = full.lexically_relative(base);
    if (relative.empty()) {
        return false;
    }
    for (const fs::path& part : relative) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

ResolvedGamePath ResolveGamePath(const std::optional<fs::path>& command_line, const fs::path& saved) {
    ResolvedGamePath result;
    if (command_line.has_value() && !command_line->empty()) {
        result.Source = GamePathSource::CommandLine;
        result.Path = *command_line;
        return result;
    }
    if (!saved.empty()) {
        result.Source = GamePathSource::Saved;
        result.Path = saved;
        return result;
    }
    result.AutoDetect = true;
    return result;
}

SettingsInspection InspectLibrarySettings(const fs::path& store_directory) {
    SettingsInspection inspection;
    const fs::path path = store_directory / "settings.json";
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error) {
        return inspection;
    }
    bool opened = false;
    const std::string text = ReadFile(path, opened);
    if (!opened) {
        inspection.State = SettingsState::Corrupt;
        inspection.Message = kCorruptSettingsMessage;
        return inspection;
    }
    try {
        const nlohmann::json body = nlohmann::json::parse(text);
        if (!body.is_object() || (body.contains("gameDirectory") && !body["gameDirectory"].is_string())) {
            inspection.State = SettingsState::Corrupt;
            inspection.Message = kCorruptSettingsMessage;
            return inspection;
        }
        inspection.State = SettingsState::Ok;
        if (body.contains("gameDirectory")) {
            inspection.GameDirectory = body["gameDirectory"].get<std::string>();
        }
        return inspection;
    } catch (const std::exception&) {
        inspection.State = SettingsState::Corrupt;
        inspection.Message = kCorruptSettingsMessage;
        return inspection;
    }
}

std::string ExportResultSummary(std::size_t file_count, const fs::path& output) {
    return "Exported " + std::to_string(file_count) + (file_count == 1 ? " file to " : " files to ") + Narrow(output.wstring()) + ".";
}

std::string AssembleResultSummary(const fs::path& output) {
    return "Assembled a pack at " + Narrow(output.wstring()) + ".";
}

std::span<const GuideStep> QuickStartSteps() {
    static constexpr GuideStep kSteps[] = {
        {"Extract", "Extract the zip. No installer or administrator prompt is required."},
        {"Launch", "Start Deadlock Modworks. A normal start does not need a terminal or a build tool."},
        {"Connect", "Connect the game. A Steam install is used when none is saved. You can type a path or browse to game\\citadel."},
        {"Import", "Get mods from GameBanana on the Get mods page, add mod VPKs, or drop them on the window. The library can be browsed before a game is connected."},
        {"Inspect", "Open a skin to view its model. Textures and the original skeleton that live in the game archive need the connected game."},
        {"Select", "Tick the skins and other files that belong in the pack."},
        {"Build", "Build the VPK. Assembly writes a file. It does not install the pack and does not guarantee compatibility."},
        {"Install or remove", "Turn mods on or off on the Mods page and choose Apply to game while Deadlock is closed. A pack you build is not installed; copy it into the addons folder yourself."},
    };
    return kSteps;
}

std::string GameResourceSummary() {
    return "You can browse the library and add mods without a connected game. Textures stored in the game, comparison, "
           "repair, and skin packs that use base-game files need the game archive, because those bytes are not in the mod. "
           "A pack made only of other files is copied as stored and does not need the game.";
}

std::string MissingGameMessage(const fs::path& path) {
    return "The game folder was not found at " + Narrow(path.wstring())
        + ". Browse to game\\citadel or pak01_dir.vpk. The model library was not changed.";
}

std::string UnwritableDataRootMessage(const fs::path& root) {
    return "Deadlock Modworks cannot write its data folder at " + Narrow(root.wstring())
        + ". Choose a folder you can write to with --data-root, or make this folder writable. The model library was not replaced.";
}

std::string DiagnosticText(std::string_view title, std::string_view commit, const fs::path& data_root, std::string_view error) {
    std::string text;
    text.append(title);
    text += "\nBuild ";
    text.append(commit);
    text += "\nData root: ";
    text += Narrow(data_root.wstring());
    text += '\n';
    text.append(error.empty() ? std::string_view("No error is waiting.") : error);
    text += '\n';
    return text;
}

bool AppendErrorLog(const fs::path& log_file, std::string_view text) {
    if (text.empty()) {
        return true;
    }
    std::error_code error;
    fs::create_directories(log_file.parent_path(), error);
    if (error) {
        return false;
    }
    std::ofstream output(log_file, std::ios::binary | std::ios::app);
    if (!output) {
        return false;
    }
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char stamp[64];
    std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u %02u:%02u:%02u\n", time.wYear, time.wMonth, time.wDay, time.wHour,
        time.wMinute, time.wSecond);
    output << stamp;
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (text.back() != '\n') {
        output << '\n';
    }
    output << '\n';
    return static_cast<bool>(output);
}

DataRootInspection InspectDataRoot(const fs::path& root) {
    DataRootInspection result;
    fs::path full;
    try {
        full = fs::absolute(root).lexically_normal();
    } catch (const fs::filesystem_error& error) {
        result.Message = std::string(UnwritableDataRootMessage(root)) + " " + error.what();
        return result;
    }

    std::error_code error;
    if (fs::exists(full, error) && !error && !fs::is_directory(full, error)) {
        result.Message = UnwritableDataRootMessage(full);
        return result;
    }
    if (error && error != std::errc::no_such_file_or_directory) {
        result.Message = UnwritableDataRootMessage(full);
        return result;
    }
    try {
        fs::create_directories(full);
    } catch (const fs::filesystem_error&) {
        result.Message = UnwritableDataRootMessage(full);
        return result;
    }

    const fs::path catalog = full / L"store" / L"catalog.sqlite";
    std::uintmax_t before = 0;
    bool present = false;
    if (fs::is_regular_file(catalog, error) && !error) {
        before = fs::file_size(catalog, error);
        if (error) {
            result.Message = UnwritableDataRootMessage(full);
            return result;
        }
        present = true;
        result.CatalogPresent = true;
        result.CatalogBytes = before;
        const HANDLE handle = CreateFileW(catalog.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            result.Message = UnwritableDataRootMessage(full);
            return result;
        }
        CloseHandle(handle);
    }

    const fs::path probe = full / L".modworks-write-probe";
    {
        std::ofstream output(probe, std::ios::binary | std::ios::trunc);
        if (!output) {
            result.Message = UnwritableDataRootMessage(full);
            return result;
        }
        output << "ok";
        if (!output) {
            result.Message = UnwritableDataRootMessage(full);
            return result;
        }
    }
    fs::remove(probe, error);

    if (present) {
        const auto after = fs::file_size(catalog, error);
        if (error || after != before) {
            result.CatalogPresent = true;
            result.CatalogBytes = error ? before : after;
            result.Message = "The model library changed while checking the data folder. It was not replaced.";
            return result;
        }
        result.CatalogBytes = after;
    }
    result.Writable = true;
    result.Message.clear();
    return result;
}

}  // namespace skeleton_repair
