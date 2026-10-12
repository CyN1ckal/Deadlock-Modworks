#pragma once

#include "content_store.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace skeleton_repair {

class PreviewPanel;
class ComparisonView;

// The version history page: every stored model and pack, and how repairs changed each model. Repair and assembly
// run on a worker thread with their own library connection; drawing stays on the UI thread.
class HistoryPanel {
public:
    using GameReader = std::function<std::optional<std::vector<uint8_t>>(const std::string&)>;
    // Called after the preview (false) or the comparison (true) was given a model to show.
    using Shown = std::function<void(bool compare)>;

    explicit HistoryPanel(std::filesystem::path store_directory);
    ~HistoryPanel();

    HistoryPanel(const HistoryPanel&) = delete;
    HistoryPanel& operator=(const HistoryPanel&) = delete;

    bool Busy() const { return running_; }
    // Reads the model and pack lists again, for example after the gallery imported mods.
    void Refresh();
    // True once after a repair replaced a pack on disk.
    bool TakeRepaired() { return std::exchange(repaired_, false); }

    // `files_busy` blocks repairs while other work uses the mod archives. `before_repair` runs before a repair
    // replaces a pack, so previews and thumbnails can release it.
    void Draw(void* owner_window, PreviewPanel& preview, const GameReader& read_game,
        const std::vector<std::string>& game_clip_paths, ComparisonView& compare, const std::string& game_directory,
        bool files_busy, const Shown& shown, const std::function<void()>& before_repair);

private:
    enum class Job { Repair, Assemble };

    void LoadSelection();
    void StartRepair(const std::string& vpk_path, const std::string& game_directory);
    void StartAssemble(const SnapshotId& snapshot, const std::filesystem::path& destination);
    std::optional<SnapshotId> AssemblySnapshot() const;
    void JoinFinishedWork();
    void HideObservation(const std::string& observation_id);
    void ViewSelected(PreviewPanel& preview, const GameReader& read_game, const std::vector<std::string>& game_clip_paths,
        const Shown& shown);
    void CompareSelected(ComparisonView& compare, const Shown& shown, const GameReader& read_game,
        const std::vector<std::string>& game_clip_paths);
    void CompareWithGame(ComparisonView& compare, const Shown& shown, const GameReader& read_game,
        const std::vector<std::string>& game_clip_paths);
    // The selected pack's recorded path. Empty when that pack has no observation path.
    // Null when no pack is selected.
    std::optional<std::string> SelectedPackPath() const;

    std::filesystem::path store_directory_;
    // Shared with readers that load clips on worker threads.
    std::shared_ptr<ContentStore> store_;
    std::string open_error_;
    std::string status_;
    std::vector<ModelRow> models_;
    std::vector<PackRow> packs_;
    std::optional<BlobId> selected_;
    History history_;
    std::vector<std::string> history_lines_;
    std::optional<size_t> selected_version_;
    std::optional<size_t> compare_version_;
    std::vector<MembershipRow> memberships_;
    std::vector<SlotObservation> unproven_;
    std::string unproven_text_;
    std::optional<size_t> selected_copy_;
    std::optional<std::string> selected_pack_;
    char filter_[128] = {};

    std::thread worker_;
    CancellationSource cancel_;
    std::string importing_name_;
    std::string import_error_;
    bool import_ok_ = false;
    bool running_ = false;
    Job job_ = Job::Repair;
    bool repaired_ = false;
    std::atomic<bool> import_done_{false};
};

}  // namespace skeleton_repair
