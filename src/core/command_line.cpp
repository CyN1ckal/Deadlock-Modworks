#include "command_line.hpp"

#include "content_store.hpp"
#include "error.hpp"
#include "report.hpp"
#include "strings.hpp"
#include "updater.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
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
    write("  --store <dir> import <file.vpk>");
    write("  --store <dir> models");
    write("  --store <dir> history <64-hex-blob>");
    write("  --store <dir> duplicates");
    write("Repair replaces the mod with a validated archive and keeps an original backup.");
    write("Exit codes: 0 = supported analysis/repair; 1 = needs review or unsupported; 2 = invalid input, failure, or cancellation.");
}

std::filesystem::path PathFromUtf8(std::string_view text) {
    return std::filesystem::path(Widen(text));
}

int CountBlobFiles(const std::filesystem::path& store) {
    const std::filesystem::path root = store / "blobs" / "sha256";
    std::error_code error;
    if (!std::filesystem::exists(root, error) || error) {
        return 0;
    }
    int count = 0;
    for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            ++count;
        }
    }
    return count;
}

std::string FormatModelLine(const ModelRow& row) {
    return row.Blob.Hex().substr(0, 12) + "  " + std::to_string(row.SnapshotCount) + " packs  " + row.DisplayPath;
}

void WriteModelLines(const std::vector<ModelRow>& rows, const std::function<void(std::string_view)>& write) {
    for (const ModelRow& row : rows) {
        write(FormatModelLine(row));
    }
}

std::string HistoryPath(const HistoryVersion& version) {
    if (version.Paths.empty()) {
        return {};
    }
    return *std::min_element(version.Paths.begin(), version.Paths.end(), [](const std::string& left, const std::string& right) {
        if (left.size() != right.size()) {
            return left.size() < right.size();
        }
        return left < right;
    });
}

std::string HistoryRelation(const History& history, const HistoryVersion& version) {
    for (const HistoryLink& link : history.Links) {
        if (link.To.Bytes == version.Blob.Bytes && link.From.Bytes != version.Blob.Bytes && !link.Relation.empty()) {
            return link.Relation;
        }
    }
    return "imported";
}

int Nibble(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

bool ParseBlobId(std::string_view hex, BlobId& id) {
    if (hex.size() != 64) {
        return false;
    }
    for (size_t index = 0; index < 32; ++index) {
        const int high = Nibble(hex[index * 2]);
        const int low = Nibble(hex[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        id.Bytes[index] = static_cast<uint8_t>((high << 4) | low);
    }
    return true;
}

int ImportStore(std::string_view directory, std::string_view file, const std::function<void(std::string_view)>& write,
    CancellationToken cancellation) {
    const std::filesystem::path store_path = PathFromUtf8(directory);
    const int blobs_before = CountBlobFiles(store_path);
    ContentStore store = ContentStore::Open(store_path);
    const SnapshotId snapshot = store.ImportVpk(PathFromUtf8(file), cancellation);
    const int new_blobs = CountBlobFiles(store_path) - blobs_before;
    int models = 0;
    bool found = false;
    for (const PackRow& pack : store.ListPacks()) {
        if (pack.Snapshot.Value == snapshot.Value) {
            models = pack.ModelCount;
            found = true;
            break;
        }
    }
    if (!found) {
        throw InvalidData("The model library has no snapshot with that id.");
    }
    int other_files = 0;
    for (const SnapshotMember& member : store.SnapshotMembers(snapshot)) {
        if (!EndsWithIgnoreCase(member.Path, ".vmdl_c")) ++other_files;
    }
    write(snapshot.Value);
    write("Imported " + std::to_string(models) + " models, " + std::to_string(other_files) + " other files, "
        + std::to_string(new_blobs) + " new blobs.");
    return 0;
}

int ExecuteStore(const std::vector<std::string>& args, const std::function<void(std::string_view)>& write,
    CancellationToken cancellation) {
    if (args.size() < 3) {
        Usage(write);
        return 2;
    }
    const std::string& directory = args[1];
    const std::string& command = args[2];
    try {
        cancellation.ThrowIfCancelled();
        if (command == "import" && args.size() == 4) {
            return ImportStore(directory, args[3], write, cancellation);
        }
        if (command == "models" && args.size() == 3) {
            ContentStore store = ContentStore::Open(PathFromUtf8(directory));
            WriteModelLines(store.ListModels(), write);
            return 0;
        }
        if (command == "duplicates" && args.size() == 3) {
            ContentStore store = ContentStore::Open(PathFromUtf8(directory));
            WriteModelLines(store.ExactDuplicateModels(), write);
            return 0;
        }
        if (command == "history" && args.size() == 4) {
            BlobId blob;
            if (!ParseBlobId(args[3], blob)) {
                write("Blob id must be 64 hex characters.");
                return 2;
            }
            ContentStore store = ContentStore::Open(PathFromUtf8(directory));
            const History history = store.HistoryOf(blob);
            for (const HistoryVersion& version : history.Versions) {
                write(version.Blob.Hex().substr(0, 12) + "  " + HistoryPath(version) + "  " + HistoryRelation(history, version));
            }
            return 0;
        }
    } catch (const std::exception& ex) {
        write(ex.what());
        return 2;
    }
    Usage(write);
    return 2;
}

}  // namespace

int CommandLine::Execute(const std::vector<std::string>& args, const std::function<void(std::string_view)>& write,
    CancellationToken cancellation) {
    if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) {
        Usage(write);
        return 0;
    }
    if (!args.empty() && args[0] == "--store") {
        return ExecuteStore(args, write, cancellation);
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
