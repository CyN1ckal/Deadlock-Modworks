#pragma once

#include "cancellation.hpp"
#include "report.hpp"
#include "updater.hpp"

#include <atomic>
#include <mutex>
#include <thread>

namespace skeleton_repair {

// Owns a worker; only snapshots cross into the UI thread.
class RepairPanel {
public:
    ~RepairPanel();
    void SetSources(std::string game, std::vector<std::string> mods);
    bool Busy() const { return busy_; }
    void Cancel() { cancellation_.Cancel(); }
    // Returns true after a write operation finishes, so the browser can reopen its archive.
    bool Poll();
    // before_repair releases the browser's read locks before replacement starts.
    void Draw(void* owner, const std::function<void()>& before_repair);

private:
    void Start(bool repair);
    std::string game_;
    std::vector<std::string> mods_;
    std::vector<ArchiveReport> reports_;
    std::string status_ = "Choose mod archives, then analyze them before repairing.";
    std::string export_status_;
    UpdateProgress progress_;
    std::thread worker_;
    CancellationSource cancellation_;
    std::atomic<bool> done_ = false;
    std::mutex mutex_;
    std::vector<ArchiveReport> pending_reports_;
    UpdateProgress pending_progress_;
    std::string pending_status_;
    bool busy_ = false;
    bool repairing_ = false;
};
}
