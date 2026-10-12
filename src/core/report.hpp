#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace skeleton_repair {

enum class ModelOutcome {
    Repairable,
    NeedsReview,
    Updated,
    Current,
    Skipped,
    Failed,
};

struct ModelResult {
    std::string Model;
    ModelOutcome Outcome = ModelOutcome::Failed;
    std::string Detail;
    std::optional<std::vector<std::string>> Warnings;
    // Old spelling to installed spelling. Unset when the file has no rename map.
    std::optional<std::vector<std::pair<std::string, std::string>>> BoneRenames;
};

struct UpdateReport {
    std::vector<ModelResult> Results;
    bool WroteFile = false;
    std::optional<std::string> OutputPath;
    std::optional<std::string> BackupPath;

    bool HasFailures() const {
        return std::any_of(Results.begin(), Results.end(), [](const ModelResult& result) {
            return result.Outcome == ModelOutcome::Failed;
        });
    }

    bool HasBlockers() const {
        return std::any_of(Results.begin(), Results.end(), [](const ModelResult& result) {
            return result.Outcome == ModelOutcome::Failed
                || result.Outcome == ModelOutcome::NeedsReview
                || result.Outcome == ModelOutcome::Skipped;
        });
    }

    bool CanRepair() const {
        return !HasBlockers() && std::any_of(Results.begin(), Results.end(), [](const ModelResult& result) {
            return result.Outcome == ModelOutcome::Repairable;
        });
    }
};

struct ArchiveReport {
    std::string ModPath;
    UpdateReport Report;
};

class ReportExporter {
public:
    static constexpr const char* Scope =
        "Hero models and AG2 animation-skeleton overrides. Non-blocking custom-rig differences are notes; in-game playback is not verified.";

    static std::string Serialize(std::string_view game_directory, const std::vector<ArchiveReport>& archives);
    // Returns the full path that was written.
    static std::string Write(const std::string& path, std::string_view game_directory, const std::vector<ArchiveReport>& archives);
};

}  // namespace skeleton_repair
