#pragma once

#include "content_store.hpp"

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace skeleton_repair {

// Library and history columns. Import runs on a worker thread; drawing stays on the UI thread.
class LibraryPanel {
public:
    LibraryPanel();
    ~LibraryPanel();

    LibraryPanel(const LibraryPanel&) = delete;
    LibraryPanel& operator=(const LibraryPanel&) = delete;

    void Draw(void* owner_window);

private:
    void Refresh();
    void LoadSelection();
    void StartImport(const std::filesystem::path& path);
    void JoinFinishedImport();
    void HideSelectedCopy();

    std::optional<ContentStore> store_;
    std::string open_error_;
    std::string status_;
    std::vector<ModelRow> models_;
    std::vector<PackRow> packs_;
    std::optional<BlobId> selected_;
    std::vector<std::string> history_lines_;
    std::vector<MembershipRow> memberships_;
    std::vector<SlotObservation> unproven_;
    std::string unproven_text_;
    std::optional<size_t> selected_copy_;
    char filter_[128] = {};

    std::thread worker_;
    CancellationSource cancel_;
    std::string importing_name_;
    std::string import_error_;
    bool import_ok_ = false;
    bool running_ = false;
    std::atomic<bool> import_done_{false};
};

}  // namespace skeleton_repair
