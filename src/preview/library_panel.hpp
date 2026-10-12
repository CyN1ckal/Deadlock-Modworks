#pragma once

#include "content_store.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace skeleton_repair {

class PreviewPanel;
class ComparisonView;

// Library and history columns. Import and repair run on a worker thread; drawing stays on the UI thread.
class LibraryPanel {
public:
    LibraryPanel();
    ~LibraryPanel();

    LibraryPanel(const LibraryPanel&) = delete;
    LibraryPanel& operator=(const LibraryPanel&) = delete;

    const std::string& SavedGameDirectory() const { return saved_game_directory_; }
    void SaveGameDirectory(const std::string& game_directory);

    void Draw(void* owner_window, PreviewPanel* preview,
        const std::function<std::optional<std::vector<uint8_t>>(const std::string&)>& read_game,
        const std::vector<std::string>& game_clip_paths, ComparisonView* compare, bool* focus_compare,
        const std::string& game_directory);

private:
    enum class Job { Import, Repair, Assemble };

    void Refresh();
    void LoadSelection();
    void StartImport(const std::filesystem::path& path);
    void StartRepair(const std::string& vpk_path, const std::string& game_directory);
    void StartAssemble(const SnapshotId& snapshot, const std::filesystem::path& destination);
    std::optional<SnapshotId> AssemblySnapshot() const;
    void JoinFinishedWork();
    void HideObservation(const std::string& observation_id);
    void ViewSelected(PreviewPanel& preview,
        const std::function<std::optional<std::vector<uint8_t>>(const std::string&)>& read_game,
        const std::vector<std::string>& game_clip_paths);
    void CompareSelected(ComparisonView& compare, bool* focus_compare,
        const std::function<std::optional<std::vector<uint8_t>>(const std::string&)>& read_game,
        const std::vector<std::string>& game_clip_paths);
    void CompareWithGame(ComparisonView& compare, bool* focus_compare,
        const std::function<std::optional<std::vector<uint8_t>>(const std::string&)>& read_game,
        const std::vector<std::string>& game_clip_paths);
    // The selected pack's recorded path. Empty when that pack has no observation path.
    // Null when no pack is selected.
    std::optional<std::string> SelectedPackPath() const;

    std::filesystem::path store_directory_;
    std::string saved_game_directory_;
    std::optional<ContentStore> store_;
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
    Job job_ = Job::Import;
    std::atomic<bool> import_done_{false};
};

}  // namespace skeleton_repair
