#include "updater.hpp"

#include "animation.hpp"
#include "archive_transaction.hpp"
#include "content_hash.hpp"
#include "content_store.hpp"
#include "error.hpp"
#include "kv3.hpp"
#include "model.hpp"
#include "resource.hpp"
#include "strings.hpp"
#include "vpk.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace skeleton_repair {
namespace {

bool IsBlank(std::string_view text) {
    for (unsigned char character : text) {
        if (character != ' ' && character != '\t' && character != '\n' && character != '\r' && character != '\f' && character != '\v') {
            return false;
        }
    }
    return true;
}

bool EndsWithOrdinal(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

bool EndsWithIgnoreCase(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && EqualsIgnoreCase(text.substr(text.size() - suffix.size()), suffix);
}

// The animation skeleton compiled beside a hero model, or the model beside a skeleton.
std::optional<std::string> PairedRigPath(std::string_view path) {
    constexpr std::string_view kModelSuffix = ".vmdl_c";
    constexpr std::string_view kSkeletonSuffix = ".vnmskel_c";
    if (EndsWithIgnoreCase(path, kModelSuffix)) {
        std::string paired(path.substr(0, path.size() - kModelSuffix.size()));
        paired += kSkeletonSuffix;
        return paired;
    }
    if (EndsWithIgnoreCase(path, kSkeletonSuffix)) {
        std::string paired(path.substr(0, path.size() - kSkeletonSuffix.size()));
        paired += kModelSuffix;
        return paired;
    }
    return std::nullopt;
}

bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

bool WideEqualIgnoreCase(const std::wstring& left, const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::wstring FullPath(const std::wstring& path) {
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        throw IoError("Could not resolve a full path.");
    }
    std::wstring buffer(required, L'\0');
    const DWORD written = GetFullPathNameW(path.c_str(), required, buffer.data(), nullptr);
    if (written == 0 || written >= required) {
        throw IoError("Could not resolve a full path.");
    }
    buffer.resize(written);
    return buffer;
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring ParentDirectory(const std::wstring& path) {
    return std::filesystem::path(path).parent_path().wstring();
}

void ReportProgress(const std::function<void(const UpdateProgress&)>& progress, int completed, int total, std::string message) {
    if (progress) {
        progress(UpdateProgress{completed, total, std::move(message)});
    }
}

std::string Join(const std::vector<std::string>& parts, size_t limit) {
    std::string out;
    const size_t count = std::min(parts.size(), limit);
    for (size_t i = 0; i < count; ++i) {
        if (i != 0) {
            out += ", ";
        }
        out += parts[i];
    }
    return out;
}

const char* OutcomeText(ModelOutcome outcome) {
    switch (outcome) {
    case ModelOutcome::Repairable: return "Repairable";
    case ModelOutcome::NeedsReview: return "Needs review";
    case ModelOutcome::Updated: return "Updated";
    case ModelOutcome::Current: return "Current";
    case ModelOutcome::Skipped: return "Skipped";
    default: return "Failed";
    }
}

std::string ExtensionLower(std::string_view path) {
    for (size_t index = path.size(); index-- > 0;) {
        const char character = path[index];
        if (character == '/' || character == '\\' || character == ':') {
            return {};
        }
        if (character == '.') {
            if (index + 1 >= path.size()) {
                return {};
            }
            std::string extension(path.substr(index));
            for (char& letter : extension) {
                if (letter >= 'A' && letter <= 'Z') {
                    letter = static_cast<char>(letter - 'A' + 'a');
                }
            }
            return extension;
        }
    }
    return {};
}

bool IsRigReference(std::string path) {
    if (EndsWithOrdinal(path, "_c")) {
        path.resize(path.size() - 2);
    }
    const std::string extension = ExtensionLower(path);
    return extension == ".vmdl" || extension == ".vagrp" || extension == ".vanim" || extension == ".vanmgrph"
        || extension == ".vnmskel" || extension == ".vnmclip" || extension == ".vnmgraph" || extension == ".vphys";
}

struct IgnoreCaseLess {
    bool operator()(const std::string& left, const std::string& right) const {
        return CompareIgnoreCase(left, right) < 0;
    }
};

bool ContainsPath(const std::vector<std::string>& paths, const std::string& path) {
    return std::binary_search(paths.begin(), paths.end(), path, IgnoreCaseLess{});
}

bool ContainsIgnoreCase(const std::vector<std::string>& paths, std::string_view path) {
    for (const std::string& item : paths) {
        if (EqualsIgnoreCase(item, path)) {
            return true;
        }
    }
    return false;
}

uint32_t ReadU32(std::span<const uint8_t> bytes) {
    uint32_t value = 0;
    if (bytes.size() < 4) {
        return 0;
    }
    std::memcpy(&value, bytes.data(), sizeof(value));
    return value;
}

KvDocument ReadModel(std::span<const uint8_t> bytes) {
    const std::span<const uint8_t> data = FindDataBlock(bytes);
    if (data.size() < 4 || !IsBinaryKv3(ReadU32(data))) {
        throw InvalidData("Model DATA is not KV3.");
    }
    return ReadKv3(data);
}

void AppendStrings(const KvValue* value, std::vector<std::string>& out) {
    if (value == nullptr || !value->IsArray()) {
        return;
    }
    for (const KvValue& item : value->Items()) {
        if (item.Kind == KvKind::String) {
            out.push_back(item.String());
        }
    }
}

void ApplyDependencies(MigrationEdit& edit, std::span<const uint8_t> bytes, const std::vector<std::string>& available) {
    const KvDocument document = ReadModel(bytes);
    const KvValue& data = document.Root;
    std::vector<std::string> names = ExternalReferenceNames(bytes);
    for (const char* key : {"m_vecNmSkeletonRefs", "m_refAnimIncludeModels", "m_refAnimGroups", "m_refPhysicsData"}) {
        AppendStrings(data.Find(key), names);
    }
    if (const KvValue* graphs = data.Find("m_animGraph2Refs"); graphs != nullptr && graphs->IsArray()) {
        for (const KvValue& value : graphs->Items()) {
            if (!value.IsObject()) {
                continue;
            }
            const KvValue* graph = value.Find("m_hGraph");
            if (graph != nullptr && graph->Kind == KvKind::String) {
                names.push_back(graph->String());
            }
        }
    }

    const KvValue* skeletons = data.Find("m_vecNmSkeletonRefs");
    const bool uses_ag2 = skeletons != nullptr && skeletons->IsArray() && skeletons->Count() > 0;
    std::vector<std::string> legacy;
    std::vector<std::string> missing;
    for (const std::string& path : names) {
        if (IsBlank(path) || !IsRigReference(path)) {
            continue;
        }
        std::string compiled = path;
        if (!EndsWithOrdinal(compiled, "_c")) {
            compiled += "_c";
        }
        compiled = VpkArchive::Normalize(std::move(compiled));
        if (ContainsPath(available, compiled)) {
            continue;
        }
        const bool legacy_graph = uses_ag2
            && (EndsWithIgnoreCase(path, ".vanmgrph") || EndsWithIgnoreCase(path, ".vanmgrph_c"));
        std::vector<std::string>& bucket = legacy_graph ? legacy : missing;
        if (std::find(bucket.begin(), bucket.end(), path) == bucket.end()) {
            bucket.push_back(path);
        }
    }

    if (!legacy.empty()) {
        std::vector<std::string> warnings = edit.Warnings.value_or(std::vector<std::string>{});
        warnings.push_back("Legacy AG1 graph references are absent, but this model declares AG2 graph/skeleton bindings: " + Join(legacy, legacy.size()));
        edit.Warnings = std::move(warnings);
    }
    if (!missing.empty()) {
        const std::optional<std::vector<std::string>> warnings = edit.Warnings;
        edit = MigrationEdit{};
        edit.Outcome = ModelOutcome::NeedsReview;
        edit.Detail = "Rig dependencies are missing from the game and this mod: " + Join(missing, 6)
            + ". The mod may depend on another addon or need author repair.";
        edit.Warnings = warnings;
    }
}

std::unordered_set<std::string> RenderBoneNames(const KvValue& root) {
    std::unordered_set<std::string> names;
    const KvValue* skeleton = root.Find("m_modelSkeleton");
    if (skeleton == nullptr) {
        return names;
    }
    const KvValue* bones = skeleton->Find("m_boneName");
    if (bones == nullptr || !bones->IsArray()) {
        return names;
    }
    for (const KvValue& bone : bones->Items()) {
        if (bone.Kind == KvKind::String) {
            names.insert(bone.String());
        }
    }
    return names;
}

bool ReferencesSkeleton(const KvValue& root, std::string_view skeleton_path) {
    const KvValue* refs = root.Find("m_vecNmSkeletonRefs");
    if (refs == nullptr || !refs->IsArray()) {
        return false;
    }
    for (const KvValue& value : refs->Items()) {
        if (value.Kind != KvKind::String) {
            continue;
        }
        std::string text = value.String();
        if (!EndsWithOrdinal(text, "_c")) {
            text += "_c";
        }
        if (EqualsIgnoreCase(VpkArchive::Normalize(std::move(text)), skeleton_path)) {
            return true;
        }
    }
    return false;
}

struct Plan {
    VpkEntry Entry;
    std::optional<std::string> Resolved;
    std::optional<std::string> Error;
};

struct ChangeList {
    std::vector<VpkChange> Items;

    void Set(std::string path, std::optional<std::vector<uint8_t>> data) {
        for (VpkChange& change : Items) {
            if (EqualsIgnoreCase(change.Path, path)) {
                change.Data = std::move(data);
                return;
            }
        }
        Items.push_back(VpkChange{std::move(path), std::move(data)});
    }
};

const VpkEntry* FindEntry(const VpkArchive& archive, std::string_view path) {
    for (const VpkEntry& entry : archive.Entries()) {
        if (EqualsIgnoreCase(entry.FullPath, path)) {
            return &entry;
        }
    }
    return nullptr;
}

void AcceptEdits(std::vector<Plan>& plans, const std::vector<std::optional<MigrationEdit>>& edits, const std::vector<VpkEntry>& entries) {
    std::vector<std::pair<std::string, size_t>> owners;
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < plans.size(); ++i) {
            Plan& plan = plans[i];
            if (!edits[i] || !edits[i]->Bytes || !plan.Resolved || plan.Error) {
                continue;
            }
            const bool moved = !EqualsIgnoreCase(*plan.Resolved, plan.Entry.FullPath);
            if ((pass == 0 && moved) || (pass == 1 && !moved)) {
                continue;
            }
            bool claimed = false;
            for (const auto& owner : owners) {
                if (EqualsIgnoreCase(owner.first, *plan.Resolved)) {
                    claimed = true;
                    break;
                }
            }
            if (claimed) {
                plan.Error = "Another model in this VPK already uses " + *plan.Resolved + ".";
            } else {
                owners.emplace_back(*plan.Resolved, i);
            }
        }
    }

    std::vector<std::string> moving_away;
    for (size_t i = 0; i < plans.size(); ++i) {
        const Plan& plan = plans[i];
        if (!edits[i] || !edits[i]->Bytes || !plan.Resolved || plan.Error) {
            continue;
        }
        if (!EqualsIgnoreCase(*plan.Resolved, plan.Entry.FullPath)) {
            moving_away.push_back(plan.Entry.FullPath);
        }
    }

    for (size_t i = 0; i < plans.size(); ++i) {
        Plan& plan = plans[i];
        if (!edits[i] || !edits[i]->Bytes || !plan.Resolved || plan.Error) {
            continue;
        }
        if (EqualsIgnoreCase(*plan.Resolved, plan.Entry.FullPath)) {
            continue;
        }
        for (const VpkEntry& entry : entries) {
            if (!EqualsIgnoreCase(entry.FullPath, *plan.Resolved)) {
                continue;
            }
            if (EqualsIgnoreCase(entry.FullPath, plan.Entry.FullPath)) {
                continue;
            }
            if (ContainsIgnoreCase(moving_away, entry.FullPath)) {
                continue;
            }
            plan.Error = *plan.Resolved + " is already used by " + entry.FullPath + ".";
            std::erase_if(moving_away, [&](const std::string& path) {
                return EqualsIgnoreCase(path, plan.Entry.FullPath);
            });
            break;
        }
    }
}

class HeroPaths {
public:
    static bool IsHeroModel(std::string_view path) {
        if (!EndsWithIgnoreCase(path, ".vmdl_c")) {
            return false;
        }
        size_t start = 0;
        while (start <= path.size()) {
            const size_t slash = path.find('/', start);
            const std::string_view part = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
            if (EqualsIgnoreCase(part, "heroes") || StartsWithIgnoreCase(part, "heroes_")) {
                return true;
            }
            if (slash == std::string_view::npos) {
                break;
            }
            start = slash + 1;
        }
        return false;
    }

    static bool IsHeroResource(std::string_view path) {
        if (IsHeroModel(path)) {
            return true;
        }
        constexpr std::string_view kSuffix = ".vnmskel_c";
        if (!EndsWithIgnoreCase(path, kSuffix)) {
            return false;
        }
        std::string model(path.substr(0, path.size() - kSuffix.size()));
        model += ".vmdl_c";
        return IsHeroModel(model);
    }

    static std::optional<std::string> Resolve(std::string_view mod_path, const std::vector<std::pair<std::string, std::string>>& exact,
        const std::vector<std::string>& game_paths, std::string& error) {
        error.clear();
        const std::string norm = VpkArchive::Normalize(std::string(mod_path));
        for (const auto& item : exact) {
            if (EqualsIgnoreCase(item.first, norm)) {
                return item.second;
            }
        }

        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= norm.size()) {
            const size_t slash = norm.find('/', start);
            if (slash == std::string::npos) {
                parts.push_back(norm.substr(start));
                break;
            }
            parts.push_back(norm.substr(start, slash - start));
            start = slash + 1;
        }

        for (int segments = static_cast<int>(parts.size()) - 1; segments >= 1; --segments) {
            std::string suffix;
            for (size_t index = parts.size() - static_cast<size_t>(segments); index < parts.size(); ++index) {
                if (!suffix.empty()) {
                    suffix += '/';
                }
                suffix += parts[index];
            }
            const std::string slash_suffix = "/" + suffix;
            std::optional<std::string> match;
            int count = 0;
            for (const std::string& path : game_paths) {
                const bool tail = path.size() >= slash_suffix.size()
                    && EqualsIgnoreCase(std::string_view(path).substr(path.size() - slash_suffix.size()), slash_suffix);
                if (EqualsIgnoreCase(path, suffix) || tail) {
                    ++count;
                    match = path;
                    if (count > 1) {
                        break;
                    }
                }
            }
            if (count == 1) {
                return match;
            }
            if (count > 1) {
                error = "Ambiguous: " + std::to_string(count) + " game models end with " + suffix + ".";
                return std::nullopt;
            }
        }

        error = "No matching model in the current game.";
        return std::nullopt;
    }
};

MigrationEdit FailedEdit(const std::string& message) {
    MigrationEdit edit;
    edit.Outcome = ModelOutcome::Failed;
    edit.Detail = message;
    return edit;
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

RepairRequest MakeRepairRequest(const SnapshotId& input, const UpdateReport& report, const std::vector<Plan>& plans,
    const std::vector<std::optional<MigrationEdit>>& edits, const VpkArchive& game, CancellationToken cancellation) {
    RepairRequest request;
    request.Input = input;
    request.OutputPath = report.OutputPath ? *report.OutputPath : std::string{};
    request.BackupPath = report.BackupPath;
    for (size_t i = 0; i < plans.size(); ++i) {
        if (!edits[i] || !edits[i]->Bytes || plans[i].Error || !plans[i].Resolved) {
            continue;
        }
        const bool moved = !EqualsIgnoreCase(*plans[i].Resolved, plans[i].Entry.FullPath);
        RepairChange change;
        change.InputPath = plans[i].Entry.FullPath;
        change.OutputPath = moved ? *plans[i].Resolved : plans[i].Entry.FullPath;
        change.Bytes = edits[i]->Bytes;
        request.Changes.push_back(std::move(change));

        RepairModelNote note;
        note.InputPath = plans[i].Entry.FullPath;
        note.GamePath = *plans[i].Resolved;
        note.Detail = edits[i]->Detail;
        note.Outcome = "Updated";
        if (edits[i]->Warnings) {
            note.Warnings = *edits[i]->Warnings;
        }
        if (edits[i]->BoneAliases) {
            note.BoneRenames = *edits[i]->BoneAliases;
        }
        for (const ModelResult& result : report.Results) {
            if (!EqualsIgnoreCase(result.Model, plans[i].Entry.FullPath)) {
                continue;
            }
            note.Outcome = OutcomeName(result.Outcome);
            note.Detail = result.Detail;
            if (result.Warnings) {
                note.Warnings = *result.Warnings;
            }
            if (result.BoneRenames) {
                note.BoneRenames = *result.BoneRenames;
            }
            break;
        }
        if (const VpkEntry* game_entry = FindEntry(game, *plans[i].Resolved)) {
            const std::vector<uint8_t> game_bytes = game.Read(*game_entry, cancellation);
            note.GameSha256 = Sha256(game_bytes);
        }
        request.Models.push_back(std::move(note));
    }
    return request;
}

}  // namespace

UpdateReport HeroModelUpdater::Analyze(const std::string& game_directory, const std::string& mod_vpk,
    const std::function<void(const UpdateProgress&)>& progress, CancellationToken cancellation,
    const std::optional<std::string>& only_model) {
    return Update(game_directory, mod_vpk, std::nullopt, false, false, progress, cancellation, true, nullptr, only_model);
}

UpdateReport HeroModelUpdater::Update(const std::string& game_directory, const std::string& mod_vpk,
    const std::optional<std::string>& output_path, bool replace_source, bool make_backup,
    const std::function<void(const UpdateProgress&)>& progress, CancellationToken cancellation, bool analyze_only,
    ContentStore* store, const std::optional<std::string>& only_model) {
    if (IsBlank(game_directory)) {
        throw std::invalid_argument("Game folder is required.");
    }
    if (IsBlank(mod_vpk)) {
        throw std::invalid_argument("Mod VPK is required.");
    }
    if (only_model && IsBlank(*only_model)) {
        throw std::invalid_argument("Model path is required.");
    }
    if (!analyze_only && !replace_source && (!output_path || IsBlank(*output_path))) {
        throw std::invalid_argument("Output VPK is required.");
    }
    if (!analyze_only && replace_source && !make_backup) {
        throw InvalidOperation("Replacing a mod requires a backup. Enable backups or write to a new file.");
    }

    const std::wstring game_pak = FullPath((std::filesystem::path(Widen(game_directory)) / L"pak01_dir.vpk").wstring());
    const std::wstring mod_full = FullPath(Widen(mod_vpk));
    if (!FileExists(game_pak)) {
        throw IoError("Could not find pak01_dir.vpk in the game folder.");
    }
    if (!FileExists(mod_full)) {
        throw IoError("Could not find the mod VPK.");
    }
    if (WideEqualIgnoreCase(mod_full, game_pak)) {
        throw InvalidOperation("That file is the game archive. Choose a mod VPK from the addons folder.");
    }
    const std::wstring game_folder = ParentDirectory(game_pak);
    if (WideEqualIgnoreCase(ParentDirectory(mod_full), game_folder)) {
        throw InvalidOperation("Choose a mod outside the game's archive folder, such as a VPK in addons.");
    }

    std::optional<std::wstring> destination;
    if (!analyze_only && !replace_source) {
        destination = FullPath(Widen(*output_path));
        if (WideEqualIgnoreCase(ParentDirectory(*destination), game_folder)) {
            throw InvalidOperation("Output must not be placed beside the official game archives.");
        }
        if (WideEqualIgnoreCase(*destination, game_pak)) {
            throw InvalidOperation("Refusing to overwrite the game archive.");
        }
        if (WideEqualIgnoreCase(*destination, mod_full)) {
            throw InvalidOperation("The output VPK has to be a different file from the mod. The tool reads the mod while it writes.");
        }
    }

    ReportProgress(progress, 0, 0, "Reading the game archive...");
    cancellation.ThrowIfCancelled();
    const VpkArchive game = VpkArchive::Open(std::filesystem::path(game_pak));
    const ReadLease game_lease = game.LockSources();
    std::vector<std::string> game_heroes;
    for (const VpkEntry& entry : game.Entries()) {
        if (HeroPaths::IsHeroResource(entry.FullPath)) {
            game_heroes.push_back(entry.FullPath);
        }
    }
    std::vector<std::pair<std::string, std::string>> exact;
    for (const std::string& path : game_heroes) {
        bool seen = false;
        for (const auto& item : exact) {
            if (EqualsIgnoreCase(item.first, path)) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            exact.emplace_back(path, path);
        }
    }

    cancellation.ThrowIfCancelled();
    const VpkArchive mod = VpkArchive::Open(std::filesystem::path(mod_full));
    ReadLease mod_lease = mod.LockSources();
    ReportProgress(progress, 0, 0, "Checking mod archive integrity...");
    mod.Validate(cancellation);

    std::vector<VpkEntry> mod_heroes;
    for (const VpkEntry& entry : mod.Entries()) {
        if (HeroPaths::IsHeroResource(entry.FullPath)) {
            mod_heroes.push_back(entry);
        }
    }
    // Bone checks still see every model in the archive. `mod_heroes` is what this run checks.
    const std::vector<VpkEntry> archive_heroes = mod_heroes;
    if (only_model) {
        const std::optional<std::string> paired = PairedRigPath(*only_model);
        std::erase_if(mod_heroes, [&](const VpkEntry& entry) {
            return !EqualsIgnoreCase(entry.FullPath, *only_model)
                && !(paired && EqualsIgnoreCase(entry.FullPath, *paired));
        });
    }
    std::vector<std::string> available;
    available.reserve(game.Entries().size() + mod.Entries().size());
    for (const VpkEntry& entry : game.Entries()) {
        available.push_back(entry.FullPath);
    }
    for (const VpkEntry& entry : mod.Entries()) {
        available.push_back(entry.FullPath);
    }
    std::sort(available.begin(), available.end(), IgnoreCaseLess{});
    available.erase(std::unique(available.begin(), available.end(), [](const std::string& left, const std::string& right) {
        return EqualsIgnoreCase(left, right);
    }), available.end());

    ReportProgress(progress, 0, static_cast<int>(mod_heroes.size()),
        "Game hero rig resources: " + std::to_string(game_heroes.size()) + ". Mod hero rig resources: " + std::to_string(mod_heroes.size()) + ".");

    std::vector<Plan> plans;
    plans.reserve(mod_heroes.size());
    std::vector<std::string> seen_source;
    for (const VpkEntry& entry : mod_heroes) {
        if (ContainsIgnoreCase(seen_source, entry.FullPath)) {
            plans.push_back(Plan{entry, std::nullopt, std::string("Duplicate path in the mod VPK.")});
            continue;
        }
        seen_source.push_back(entry.FullPath);
        std::string error;
        std::optional<std::string> resolved = HeroPaths::Resolve(entry.FullPath, exact, game_heroes, error);
        std::optional<std::string> resolved_error;
        if (!resolved) {
            resolved_error = std::move(error);
        }
        plans.push_back(Plan{entry, std::move(resolved), std::move(resolved_error)});
    }

    std::vector<std::optional<MigrationEdit>> edits(plans.size());
    for (size_t i = 0; i < plans.size(); ++i) {
        cancellation.ThrowIfCancelled();
        Plan& plan = plans[i];
        if (plan.Error || !plan.Resolved) {
            continue;
        }
        ReportProgress(progress, static_cast<int>(i), static_cast<int>(plans.size()), "Checking " + plan.Entry.FullPath + "...");
        try {
            const VpkEntry* game_entry = FindEntry(game, *plan.Resolved);
            if (game_entry == nullptr) {
                throw InvalidData("Game archive is missing " + *plan.Resolved + ".");
            }
            const std::vector<uint8_t> mod_bytes = mod.Read(plan.Entry, cancellation);
            const std::vector<uint8_t> game_bytes = game.Read(*game_entry, cancellation);
            if (EndsWithIgnoreCase(plan.Entry.FullPath, ".vnmskel_c")) {
                edits[i] = AnimationSkeletonMigration::Migrate(mod_bytes, plan.Entry.FullPath, game_bytes, *plan.Resolved);
            } else {
                edits[i] = ModelMigration::Migrate(mod_bytes, plan.Entry.FullPath, game_bytes, *plan.Resolved);
            }
            if (HeroPaths::IsHeroModel(plan.Entry.FullPath)
                && (edits[i]->Outcome == ModelOutcome::Updated || edits[i]->Outcome == ModelOutcome::Current)) {
                ApplyDependencies(*edits[i], mod_bytes, available);
            }
        } catch (const InvalidData& ex) {
            edits[i] = FailedEdit(ex.what());
        } catch (const IoError& ex) {
            edits[i] = FailedEdit(ex.what());
        } catch (const std::invalid_argument& ex) {
            edits[i] = FailedEdit(ex.what());
        }
    }

    for (size_t i = 0; i < plans.size(); ++i) {
        if (!edits[i] || !edits[i]->Bytes || !edits[i]->BoneAliases || edits[i]->BoneAliases->empty()) {
            continue;
        }
        for (const VpkEntry& model_entry : archive_heroes) {
            if (!HeroPaths::IsHeroModel(model_entry.FullPath)) {
                continue;
            }
            const KvDocument render = ReadModel(mod.Read(model_entry, cancellation));
            if (!ReferencesSkeleton(render.Root, plans[i].Entry.FullPath)) {
                continue;
            }
            const std::unordered_set<std::string> render_names = RenderBoneNames(render.Root);
            std::vector<std::string> unresolved;
            for (const auto& alias : *edits[i]->BoneAliases) {
                if (render_names.contains(alias.second)) {
                    continue;
                }
                bool spelled_differently = false;
                for (const std::string& name : render_names) {
                    if (EqualsIgnoreCase(name, alias.second)) {
                        spelled_differently = true;
                        break;
                    }
                }
                if (spelled_differently) {
                    unresolved.push_back(alias.first);
                }
            }
            if (unresolved.empty()) {
                continue;
            }
            MigrationEdit blocked;
            blocked.Outcome = ModelOutcome::NeedsReview;
            blocked.Detail = "Animation casing changes also require renaming bones in " + model_entry.FullPath + ": "
                + Join(unresolved, unresolved.size()) + ". Render-resource references need review.";
            edits[i] = std::move(blocked);
            break;
        }
    }

    AcceptEdits(plans, edits, mod.Entries());

    std::vector<std::string> written_to;
    for (size_t i = 0; i < plans.size(); ++i) {
        const Plan& plan = plans[i];
        if (!edits[i] || !edits[i]->Bytes || plan.Error || !plan.Resolved) {
            continue;
        }
        const bool moved = !EqualsIgnoreCase(*plan.Resolved, plan.Entry.FullPath);
        written_to.push_back(moved ? *plan.Resolved : plan.Entry.FullPath);
    }

    std::vector<ModelResult> results;
    ChangeList changes;
    results.reserve(plans.size());
    for (size_t i = 0; i < plans.size(); ++i) {
        const Plan& plan = plans[i];
        ModelResult result;
        result.Model = plan.Entry.FullPath;
        if (plan.Error || !plan.Resolved) {
            result.Outcome = ModelOutcome::NeedsReview;
            result.Detail = plan.Error ? *plan.Error : "No matching game model.";
        } else if (!edits[i]) {
            result.Outcome = ModelOutcome::Failed;
            result.Detail = "The model was not processed.";
        } else {
            const MigrationEdit& edit = *edits[i];
            result.Outcome = edit.Outcome == ModelOutcome::Updated ? ModelOutcome::Repairable : edit.Outcome;
            result.Detail = edit.Detail;
            result.Warnings = edit.Warnings;
            result.BoneRenames = edit.BoneAliases;
            if (edit.Bytes) {
                const bool moved = !EqualsIgnoreCase(*plan.Resolved, plan.Entry.FullPath);
                const std::string destination_path = moved ? *plan.Resolved : plan.Entry.FullPath;
                if (moved && !ContainsIgnoreCase(written_to, plan.Entry.FullPath)) {
                    changes.Set(plan.Entry.FullPath, std::nullopt);
                }
                changes.Set(destination_path, edit.Bytes);
            }
        }
        results.push_back(std::move(result));
        ReportProgress(progress, static_cast<int>(i + 1), static_cast<int>(plans.size()),
            results.back().Model + ": " + OutcomeText(results.back().Outcome) + ". " + results.back().Detail);
    }

    if (mod_heroes.empty()) {
        ModelResult skipped;
        skipped.Outcome = ModelOutcome::Skipped;
        if (only_model) {
            skipped.Model = *only_model;
            bool present = false;
            for (const VpkEntry& entry : mod.Entries()) {
                if (EqualsIgnoreCase(entry.FullPath, *only_model)) {
                    present = true;
                    break;
                }
            }
            skipped.Detail = present ? "That model is not a supported hero model in this archive." : "That model is not in this archive.";
        } else {
            skipped.Model = Narrow(std::filesystem::path(mod_full).filename().wstring());
            skipped.Detail = "No supported hero models. This MVP cannot assess this mod.";
        }
        results.push_back(std::move(skipped));
    }

    UpdateReport analysis;
    analysis.Results = std::move(results);
    if (analyze_only || analysis.HasBlockers()) {
        ReportProgress(progress, static_cast<int>(plans.size()), static_cast<int>(plans.size()),
            analysis.HasBlockers()
                ? "Repair blocked. Resolve the reported issues first; no files were written."
                : "Analysis complete. No files were written.");
        return analysis;
    }

    if (changes.Items.empty()) {
        ReportProgress(progress, static_cast<int>(plans.size()), static_cast<int>(plans.size()),
            "No hero models needed an update. The VPK was not modified.");
        return analysis;
    }

    cancellation.ThrowIfCancelled();
    const std::wstring final_path = replace_source ? mod_full : *destination;
    std::optional<SnapshotId> input_snapshot;
    if (store != nullptr) {
        input_snapshot = store->ImportVpk(std::filesystem::path(mod_full), cancellation);
    }
    ReportProgress(progress, static_cast<int>(plans.size()), static_cast<int>(plans.size()), "Writing and validating the repaired VPK...");
    const std::optional<std::filesystem::path> backup = ArchiveTransaction::Write(
        mod, std::filesystem::path(mod_full), std::filesystem::path(final_path), changes.Items, replace_source, cancellation,
        [&] { mod_lease.Release(); });
    for (ModelResult& result : analysis.Results) {
        if (result.Outcome == ModelOutcome::Repairable) {
            result.Outcome = ModelOutcome::Updated;
        }
    }
    analysis.WroteFile = true;
    analysis.OutputPath = Narrow(final_path);
    if (backup) {
        analysis.BackupPath = Narrow(backup->wstring());
    }
    if (store != nullptr) {
        store->RecordRepair(MakeRepairRequest(*input_snapshot, analysis, plans, edits, game, cancellation), cancellation);
    }
    ReportProgress(progress, static_cast<int>(plans.size()), static_cast<int>(plans.size()),
        analysis.BackupPath ? "Repaired VPK. Original backup: " + *analysis.BackupPath : "Wrote " + *analysis.OutputPath + ".");
    (void)game_lease;
    return analysis;
}

}  // namespace skeleton_repair
