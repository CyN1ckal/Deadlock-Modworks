#pragma once

#include "archive_cache.hpp"
#include "comparison_view.hpp"
#include "gamebanana_panel.hpp"
#include "history_panel.hpp"
#include "latest_task.hpp"
#include "library_panel.hpp"
#include "library_session.hpp"
#include "model_inspection.hpp"
#include "mods_panel.hpp"
#include "multi_view.hpp"
#include "preview_panel.hpp"
#include "repair_panel.hpp"
#include "startup.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct HWND__;

namespace skeleton_repair {

// The model tools window's contents: the menu and status bars, the Explorer, and the Workspace pages, with the
// archives, the library and the game they show. Archives are opened and models decoded on worker threads; Frame
// applies finished results and draws, all on the UI thread.
class Workspace {
public:
    enum class Page { Gallery, Pack, History, Inspect, Repair, Multi, Mods, Browse };

    // `window` owns the dialogs the menus open. Starts reading the library under `data_root`.
    Workspace(const VulkanDevice& device, const std::filesystem::path& data_root, HWND__* window);
    // Waits for library work, applies and saves what it finished.
    ~Workspace();
    Workspace(const Workspace&) = delete;
    Workspace& operator=(const Workspace&) = delete;

    // What the process was started with: saved settings, a game folder from the command line, and files to open. A
    // mod VPK followed by one of its .vmdl_c paths inspects that model, playing `clip` when given.
    void Start(const SettingsInspection& settings, const std::optional<std::filesystem::path>& game_folder,
        const std::vector<std::filesystem::path>& files, const std::optional<std::string>& clip);
    // Polls the background work, then draws one frame between ImGui::NewFrame and ImGui::Render.
    void Frame(const std::vector<std::filesystem::path>& dropped);
    // Records the viewports' offscreen passes, before the ImGui pass samples them.
    void Record(VkCommandBuffer command_buffer);
    // The window is closing: stops a running repair.
    void Close();

private:
    bool LibraryWorkBusy() const;
    bool FilesBusy() const;
    bool PackBusy() const;
    std::filesystem::path GamePath() const;

    void ReportError(const std::string& message);
    void NoteStartupError(const std::string& message);
    void SetStoreStatus(std::string text, bool error);

    // Library
    void Persist(bool force = false);
    bool PrepareLibraryJob(const char* blocked, const char* before);
    void StartMerge(const std::filesystem::path& other);
    void StartRename(const std::filesystem::path& archive, const std::string& name);
    void StartImport(const ModelLibrary& incoming, bool replace);
    void StartDownload(const GbMod& mod, const GbFile& file, bool enable);
    void StartBuildMod(const std::string& name, bool enable);
    std::optional<std::string> BuiltModNamed(const std::string& name) const;
    std::vector<ModEntry> ModEntries() const;
    void PollLibrary();
    void ApplyFinished(const LibrarySession::Finished& finished);
    void ApplyLibrary(const ModelLibrary& library);
    void AfterRepair();

    // Game and inspection
    void MaybeAutoDetect();
    void OpenGame(const std::filesystem::path& chosen);
    void PollGame();
    void ChooseGame();
    void ConnectManual(bool browse);
    void StartInspect(const std::filesystem::path& archive, const std::string& model);
    void OpenModelFile(const std::filesystem::path& path);
    void OpenArchiveModel(const std::filesystem::path& archive, const std::string& model);
    void PollInspect();
    void CloseInspection();
    void ReleaseArchives();
    void SyncSources();

    // Sources
    void AddPaths(const std::vector<std::filesystem::path>& paths);
    void AddMods();
    void AddFolder();
    void AddLibrary();
    void RemoveSource(const std::filesystem::path& path);

    // Drawing
    void ShowPage(Page page);
    void FindSkins();
    void HandleShortcuts();
    void DrawMenuBar();
    void GameMenuItems();
    void GameState(ImVec4& color, const char*& label) const;
    std::string* StatusMessage(ImVec4& color);
    void DrawStatusBar();
    void DrawExplorer();
    void DrawSourceList();
    void DrawPageTabs();
    void DrawWorkspace();
    void DrawInspectPage();
    void DrawDialogs();
    void DrawRenamePopup();
    void DrawBuildModPopup();
    void DrawPackAssembledPopup();
    void OpenLogs();

    HWND__* window_;
    std::filesystem::path data_root_;
    std::filesystem::path layout_ini_;

    // Pages and shell windows
    Page page_ = Page::Gallery;
    Page previous_page_ = Page::Gallery;
    // Set while a page shown from code waits for its tab to become selected.
    bool page_requested_ = true;
    bool show_explorer_ = true;
    bool focus_explorer_ = false;
    bool focus_workspace_ = false;
    bool reset_layout_ = false;
    bool show_status_bar_ = true;
    bool first_frame_ = true;
    char source_filter_[128] = {};

    // Archives and views
    std::shared_ptr<ArchiveCache> archives_ = std::make_shared<ArchiveCache>();
    PreviewPanel panel_;
    ComparisonView compare_;
    MultiView multi_;
    // Set when a repair or rename drops viewport uploads. The next finished import reloads them.
    bool multi_suspended_ = false;
    RepairPanel repair_;
    LibraryPanel builder_;
    HistoryPanel history_;
    ModsPanel mods_;
    GameBananaPanel banana_;

    // The model being inspected
    ArchivePtr mod_;
    std::string mod_name_;
    std::string selected_;
    std::filesystem::path active_mod_;
    bool standalone_model_ = false;
    LatestTask<Inspection> inspector_;
    std::optional<std::string> pending_clip_;
    // A model to inspect once its archive has been imported, from the command line.
    std::optional<std::pair<std::filesystem::path, std::string>> inspect_after_import_;
    // Shows the comparison tab of the inspector on the next frame.
    bool focus_compare_ = false;

    // The game
    ArchivePtr game_;
    std::string game_name_;
    std::string game_error_;
    std::filesystem::path game_loading_;
    // Parent of the connected game archive. Filled when the archive finishes opening.
    std::string game_directory_;
    // Animation clips in the game archive, which history previews also offer.
    std::vector<std::string> game_clips_;
    LatestTask<LoadedGame> game_loader_;
    std::filesystem::path configured_game_;
    // Set when the game was chosen on the command line, so the saved game does not replace it.
    bool keep_game_ = false;
    std::string settings_game_;
    bool auto_detect_done_ = false;
    char manual_game_[2048] = {};

    // The library
    LibrarySession library_;
    std::vector<std::filesystem::path> mod_paths_;
    // Imports requested while the library was being restored.
    ModelLibrary queued_imports_;
    // Last library revision and game path saved. Snapshotting the pack every frame is not required.
    uint64_t saved_revision_ = 0;
    std::filesystem::path saved_game_;

    // Status and errors
    std::string startup_notice_;
    std::string last_error_;
    std::string last_logged_;
    std::string store_status_;
    bool store_status_is_error_ = false;

    // Dialogs
    bool open_shortcuts_ = false;
    bool open_about_ = false;
    bool open_quick_start_ = false;
    bool open_export_ = false;
    std::optional<AssembledPack> pending_export_;
    bool open_build_mod_ = false;
    char build_mod_name_[101] = "My pack";
    bool build_mod_enable_ = true;
    // The source being renamed, and the name being typed.
    std::filesystem::path rename_target_;
    char rename_text_[201] = {};
    bool open_rename_ = false;
    // A confirmed rename, started before the next frame draws, because it frees thumbnails this frame already drew.
    std::optional<std::pair<std::filesystem::path, std::string>> pending_rename_;
};

}  // namespace skeleton_repair
