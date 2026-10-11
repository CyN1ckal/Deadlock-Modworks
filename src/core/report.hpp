#pragma once

#include <algorithm>
#include <optional>
#include <string>
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
    std::vector<std::string> Warnings;
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

}  // namespace skeleton_repair
