#pragma once

#include "addons_deployer.hpp"
#include "background.hpp"
#include "mod_state.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace skeleton_repair {

// One stored mod of the library, as the Mods page lists it.
struct ModEntry {
    std::string ContentId;
    std::string Name;
    std::filesystem::path Path;
};

// The Mods page: which library mods are enabled, their load order, profiles, and applying them to Deadlock's
// addons folder. The state is saved as mods.json in the data root. Nothing is written into the game until Apply.
class ModsPanel {
public:
    explicit ModsPanel(const std::filesystem::path& data_root);
    ~ModsPanel();
    ModsPanel(const ModsPanel&) = delete;
    ModsPanel& operator=(const ModsPanel&) = delete;

    ModState& State() { return state_; }
    // Saves mods.json. A failure is shown on the page and returned by TakeError.
    void Save();
    // Records a download: its stored mods, where they came from, and enables them when asked.
    void AddDownloaded(const std::vector<std::string>& ids, const ModOrigin& origin, bool enable);
    // Enables or disables one mod in the active profile.
    void SetEnabled(const std::string& id, bool enabled);

    bool Busy() const { return deploy_.valid(); }
    void Poll();
    // A new error for the log, once.
    std::optional<std::string> TakeError() { return std::exchange(new_error_, std::nullopt); }

    // `entries` are the library's mods in library order. `game` is the configured game archive or folder.
    // `files_busy` is set while other work may replace stored mods, which also blocks Apply.
    void Draw(const std::vector<ModEntry>& entries, const std::filesystem::path& game, bool files_busy,
        const std::function<void(const std::filesystem::path&)>& show_in_library, const std::function<void()>& get_mods);

    // True when the enabled mods differ from what Modworks last placed in the addons folder.
    bool HasPendingChanges(const std::vector<ModEntry>& entries) const;
    // Starts placing the enabled mods in the addons folder of `game`. Errors go to the page status.
    void Apply(const std::vector<ModEntry>& entries, const std::filesystem::path& game);
    const std::string& Status() const { return status_; }

private:
    struct GameCheck {
        std::optional<std::filesystem::path> Citadel;
        bool MountsAddons = false;
        bool Running = false;
        AddonsScan Scan;
        std::string Error;
    };
    void Refresh(const std::filesystem::path& game, bool force);
    void SetStatus(std::string text, bool error);
    void UpdateConflicts(const std::vector<ModEntry>& entries);
    void DrawToolbar(const std::vector<ModEntry>& entries, const std::filesystem::path& game, bool files_busy);
    void DrawGameState(const std::vector<ModEntry>& entries);
    void DrawList(const std::vector<ModEntry>& entries, const std::function<void(const std::filesystem::path&)>& show_in_library,
        const std::function<void()>& get_mods);
    void DrawProfilePopups();

    std::filesystem::path file_;
    ModState state_;
    bool writable_ = true;
    std::string status_;
    bool status_error_ = false;
    std::optional<std::string> new_error_;
    char filter_[128] = {};

    GameCheck check_;
    std::filesystem::path checked_game_;
    std::chrono::steady_clock::time_point checked_at_{};

    std::future<DeployResult> deploy_;
    CancellationSource deploy_cancel_;
    std::atomic<uint64_t> deploy_done_ = 0;
    std::atomic<uint64_t> deploy_total_ = 0;

    LatestTask<std::vector<ModConflict>> conflict_task_;
    std::vector<std::string> conflicts_for_;
    std::vector<ModConflict> conflicts_;

    bool open_new_profile_ = false;
    bool open_rename_profile_ = false;
    bool open_delete_profile_ = false;
    bool open_patch_gameinfo_ = false;
    // Set by the gameinfo.gi prompt; Draw starts the apply once the prompt has closed.
    bool pending_apply_ = false;
    bool copy_profile_ = true;
    char profile_name_[64] = {};
};

}  // namespace skeleton_repair
