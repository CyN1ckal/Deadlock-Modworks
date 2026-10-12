#include "command_line.hpp"

#include "error.hpp"
#include "report.hpp"
#include "strings.hpp"
#include "updater.hpp"

#include <algorithm>
#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace skeleton_repair {
namespace {

bool EndsWithIgnoreCase(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && EqualsIgnoreCase(text.substr(text.size() - suffix.size()), suffix);
}

const char* OutcomeName(ModelOutcome outcome) {
    switch (outcome) {
    case ModelOutcome::Repairable: return "Repairable";
    case ModelOutcome::NeedsReview: return "NeedsReview";
    case ModelOutcome::Updated: return "Updated";
    case ModelOutcome::Current: return "Current";
    case ModelOutcome::Skipped: return "Skipped";
    case ModelOutcome::Failed: return "Failed";
    }
    return "Failed";
}

void Usage(const std::function<void(std::string_view)>& write) {
    write("Deadlock Model Updater — hero models and animation skeleton repairs");
    write("  --analyze <citadel-folder> <mod.vpk> [--report <report.json>]");
    write("  --update  <citadel-folder> <mod.vpk> <new-output.vpk> [--report <report.json>]");
    write("  --repair  <citadel-folder> <mod.vpk> [--report <report.json>]");
    write("Repair replaces the mod with a validated archive and keeps an original backup.");
    write("Exit codes: 0 = supported analysis/repair; 1 = needs review or unsupported; 2 = invalid input, failure, or cancellation.");
}

}  // namespace

int CommandLine::Execute(const std::vector<std::string>& args, const std::function<void(std::string_view)>& write,
    CancellationToken cancellation) {
    if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) {
        Usage(write);
        return 0;
    }

    std::vector<std::string> positional = args;
    std::optional<std::string> report_path;
    const auto report_index = std::find(positional.begin(), positional.end(), "--report");
    if (report_index != positional.end()) {
        if (static_cast<size_t>(report_index - positional.begin()) != positional.size() - 2) {
            Usage(write);
            return 2;
        }
        report_path = positional.back();
        positional.erase(report_index, positional.end());
        if (!EndsWithIgnoreCase(*report_path, ".json")) {
            write("Report output must be a .json file.");
            return 2;
        }
    }

    const std::string action = positional.empty() ? std::string() : positional.front();
    const bool analyze_or_repair = (action == "--analyze" || action == "--repair") && positional.size() == 3;
    const bool update = action == "--update" && positional.size() == 4;
    if (!analyze_or_repair && !update) {
        Usage(write);
        return 2;
    }

    UpdateReport report;
    try {
        const auto progress = [&](const UpdateProgress& update_progress) { write(update_progress.Message); };
        if (action == "--analyze") {
            report = HeroModelUpdater::Analyze(positional[1], positional[2], progress, cancellation);
        } else {
            report = HeroModelUpdater::Update(positional[1], positional[2],
                action == "--update" ? std::optional<std::string>(positional[3]) : std::nullopt,
                action == "--repair", true, progress, cancellation);
        }
    } catch (const OperationCancelled&) {
        write("Cancelled. No pending repair was installed.");
        return 2;
    } catch (const std::exception& ex) {
        ModelResult failed;
        failed.Model = positional[2];
        failed.Outcome = ModelOutcome::Failed;
        failed.Detail = ex.what();
        report.Results.push_back(std::move(failed));
    }

    for (const ModelResult& result : report.Results) {
        write(std::string(OutcomeName(result.Outcome)) + ": " + result.Model + ": " + result.Detail);
        if (result.Warnings) {
            for (const std::string& warning : *result.Warnings) {
                write("  Note: " + warning);
            }
        }
    }
    write(report.WroteFile ? "Wrote " + *report.OutputPath + "." : "No VPK was written.");
    if (report.BackupPath) {
        write("Original backup: " + *report.BackupPath);
    }
    if (report_path) {
        try {
            const std::string full = ReportExporter::Write(*report_path, positional[1], {ArchiveReport{positional[2], report}});
            write("Saved report: " + full);
        } catch (const std::exception& ex) {
            write(std::string("Report could not be saved: ") + ex.what());
            return 2;
        }
    }
    if (report.HasFailures()) {
        return 2;
    }
    if (report.HasBlockers()) {
        return 1;
    }
    return 0;
}

}  // namespace skeleton_repair
