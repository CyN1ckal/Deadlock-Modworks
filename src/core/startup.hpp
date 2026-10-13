#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace skeleton_repair {

// Files the GUI keeps under the data root. Nothing here is relative to the process working directory.
struct DataPaths {
    std::filesystem::path Root;
    std::filesystem::path LayoutIni;
    std::filesystem::path WindowIni;
    std::filesystem::path LogFile;
    std::filesystem::path StoreDirectory;
    std::filesystem::path SettingsFile;
    std::filesystem::path Catalog;
};

DataPaths DataPathsFor(const std::filesystem::path& root);

// `root` itself counts. A path that escapes with `..` does not.
bool IsInsideDataRoot(const std::filesystem::path& root, const std::filesystem::path& path);

enum class GamePathSource { None, CommandLine, Saved };

// Command line wins, then a saved path. Auto-detect only when both are empty.
struct ResolvedGamePath {
    GamePathSource Source = GamePathSource::None;
    std::filesystem::path Path;
    bool AutoDetect = false;
};

ResolvedGamePath ResolveGamePath(const std::optional<std::filesystem::path>& command_line,
    const std::filesystem::path& saved);

enum class SettingsState { Missing, Ok, Corrupt };

struct SettingsInspection {
    SettingsState State = SettingsState::Missing;
    std::string GameDirectory;
    // Set only when the file is present and unreadable. The file is not rewritten.
    std::string Message;
};

inline constexpr const char* kCorruptSettingsMessage =
    "settings.json could not be read. Saved preferences were left unchanged, and the model library was not replaced.";

// Reads store/settings.json. Does not create the file, write it, or open the catalog.
SettingsInspection InspectLibrarySettings(const std::filesystem::path& store_directory);

inline constexpr const char* kMissingVulkanLoaderMessage =
    "vulkan-1.dll was not found. Install or update the graphics driver that provides the Vulkan loader, then start "
    "Deadlock Modworks again. The model library was not opened.";

inline constexpr const char* kAssemblyDoesNotInstall =
    "Assembly writes a VPK. It does not install the pack and does not guarantee compatibility with Deadlock.";

std::string ExportResultSummary(std::size_t file_count, const std::filesystem::path& output);
std::string AssembleResultSummary(const std::filesystem::path& output);

struct AssembledPack {
    std::filesystem::path Output;
    std::string Summary;
};

struct GuideStep {
    const char* Title = nullptr;
    const char* Detail = nullptr;
};

// Extract, launch, connect, import, inspect, select, build, then install or remove.
std::span<const GuideStep> QuickStartSteps();

// What still works with no game connected, and which actions need the game archive.
std::string GameResourceSummary();

struct DataRootInspection {
    bool Writable = false;
    bool CatalogPresent = false;
    std::uintmax_t CatalogBytes = 0;
    std::string Message;
};

// Creates the data root when that is possible. Does not open or replace catalog.sqlite.
DataRootInspection InspectDataRoot(const std::filesystem::path& root);

std::string MissingGameMessage(const std::filesystem::path& path);
std::string UnwritableDataRootMessage(const std::filesystem::path& root);

// Version, build, data root, and the error. An empty error says none is waiting.
std::string DiagnosticText(std::string_view title, std::string_view commit, const std::filesystem::path& data_root,
    std::string_view error);

// Appends one error under the data root. An empty error writes nothing, so a quiet start creates no log.
bool AppendErrorLog(const std::filesystem::path& log_file, std::string_view text);

}  // namespace skeleton_repair
