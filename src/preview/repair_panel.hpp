#pragma once

#include "cancellation.hpp"
#include "report.hpp"
#include "updater.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <utility>

namespace skeleton_repair {

// Owns a worker; only snapshots cross into the UI thread.
class RepairPanel {
public:
    ~RepairPanel();
    void SetSources(std::string game, std::vector<std::string> mods);
    bool Busy() const { return busy_; }
    void Cancel() { cancellation_.Cancel(); }
    bool CanAnalyze() const { return !busy_ && !game_.empty() && !mods_.empty(); }
    bool CanRepair() const { return !busy_ && RepairableCount() > 0; }
    // Archives the last analysis found repairable.
    size_t RepairableCount() const;
    void Analyze() { if (CanAnalyze()) Start(false); }
    // Checks one model, and its paired animation skeleton or hero model, without checking the rest of the archive.
    void AnalyzeModel(std::string archive, std::string model);
    // before_repair releases the browser's read locks before replacement starts.
    void Repair(const std::function<void()>& before_repair) { if (CanRepair()) { before_repair(); Start(true); } }
    const std::string& Status() const { return status_; }
    // Returns true after a write operation finishes, so the browser can reopen its archive.
    bool Poll();
    // before_repair releases the browser's read locks before replacement starts.
    void Draw(void* owner, const std::function<void()>& before_repair);

private:
    // `one` is the archive path and the model path. Without it, analysis covers every archive.
    void Start(bool repair, std::optional<std::pair<std::string, std::string>> one = std::nullopt);
    void SaveReport(void* owner);
    std::string game_;
    std::vector<std::string> mods_;
    std::vector<ArchiveReport> reports_;
    // Archives whose latest check covered one model. Repair of those archives rewrites only the checked model.
    std::unordered_set<std::string> partial_;
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
    // Bit per ModelOutcome hidden from the results table.
    unsigned hidden_outcomes_ = 0;
    char filter_[128] = {};
};
}
