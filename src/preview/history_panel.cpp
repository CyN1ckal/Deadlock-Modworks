#include "profile.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>

#include "history_panel.hpp"

#include "comparison_view.hpp"
#include "history_compare.hpp"
#include "library_model.hpp"
#include "startup.hpp"
#include "model_versions.hpp"
#include "preview_panel.hpp"
#include "store_jobs.hpp"
#include "store_reader.hpp"
#include "strings.hpp"
#include "ui.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <string_view>
#include <utility>

namespace skeleton_repair {
namespace {

std::optional<std::filesystem::path> AskForSaveVpk(HWND owner) {
    wchar_t buffer[MAX_PATH] = {};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"VPK archives (*.vpk)\0*.vpk\0All files\0*.*\0";
    dialog.lpstrTitle = L"Assemble a pack";
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"vpk";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&dialog)) {
        return std::nullopt;
    }
    return std::filesystem::path(buffer);
}

bool SameBlob(const BlobId& left, const BlobId& right) { return left.Bytes == right.Bytes; }

bool ContainsIgnoreCase(std::string_view text, std::string_view query) {
    if (query.empty()) return true;
    if (query.size() > text.size()) return false;
    for (size_t i = 0; i + query.size() <= text.size(); ++i) {
        if (EqualsIgnoreCase(text.substr(i, query.size()), query)) return true;
    }
    return false;
}

using Shown = std::function<void(bool compare)>;

void Focus(const Shown& shown) {
    if (shown) {
        shown(true);
    }
}

}  // namespace

HistoryPanel::HistoryPanel(std::filesystem::path store_directory) : store_directory_(std::move(store_directory)) {
    if (store_directory_.empty()) {
        open_error_ = "The model library is unavailable because the local app data folder could not be found.";
        return;
    }
    try {
        store_ = std::make_shared<ContentStore>(ContentStore::Open(store_directory_));
        Refresh();
    } catch (const std::exception& error) {
        open_error_ = error.what();
        store_.reset();
    }
}

HistoryPanel::~HistoryPanel() {
    cancel_.Cancel();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void HistoryPanel::Refresh() {
    ZoneScopedN("History refresh");
    shown_dirty_ = true;
    if (!store_) {
        return;
    }
    try {
        models_ = store_->ListModels();
        packs_ = store_->ListPacks();
    } catch (const std::exception& error) {
        NoteStatus(error.what(), true);
        return;
    }
    if (selected_pack_) {
        bool pack_found = false;
        for (const PackRow& pack : packs_) {
            if (pack.Snapshot.Value == *selected_pack_) {
                pack_found = true;
                break;
            }
        }
        if (!pack_found) {
            selected_pack_.reset();
        }
    }
    if (!selected_) {
        return;
    }
    bool found = false;
    for (const ModelRow& row : models_) {
        if (SameBlob(row.Blob, *selected_)) {
            found = true;
            break;
        }
    }
    if (!found) {
        selected_.reset();
        selected_copy_.reset();
        selected_version_.reset();
        compare_version_.reset();
        history_ = {};
        history_lines_.clear();
        memberships_.clear();
        unproven_.clear();
        unproven_text_.clear();
        return;
    }
    LoadSelection();
}

void HistoryPanel::RebuildShownModels() {
    shown_models_.clear();
    shown_models_.reserve(models_.size());
    const std::string_view query(filter_);
    for (size_t i = 0; i < models_.size(); ++i) {
        if (ContainsIgnoreCase(models_[i].DisplayPath, query)) shown_models_.push_back(i);
    }
    if (shown_column_ >= 0) {
        const int column = shown_column_;
        const bool descending = shown_descending_;
        std::stable_sort(shown_models_.begin(), shown_models_.end(), [&](size_t left, size_t right) {
            const ModelRow& first = models_[descending ? right : left];
            const ModelRow& second = models_[descending ? left : right];
            if (column == 1) return first.SnapshotCount < second.SnapshotCount;
            if (column == 2) return first.Blob.Bytes < second.Blob.Bytes;
            return first.DisplayPath < second.DisplayPath;
        });
    }
    shown_filter_ = filter_;
    shown_dirty_ = false;
}

void HistoryPanel::LoadSelection() {
    ZoneScopedN("History query");
    if (!store_ || !selected_ || running_) {
        return;
    }
    try {
        history_ = store_->HistoryOf(*selected_);
        history_lines_ = FormatHistory(history_);
        if (history_.Versions.empty()) {
            selected_version_.reset();
        } else if (!selected_version_ || *selected_version_ >= history_.Versions.size()) {
            size_t index = history_.Versions.size() - 1;
            for (size_t i = 0; i < history_.Versions.size(); ++i) {
                if (SameBlob(history_.Versions[i].Blob, *selected_)) {
                    index = i;
                    break;
                }
            }
            selected_version_ = index;
        }
        if (compare_version_ && *compare_version_ >= history_.Versions.size()) {
            compare_version_.reset();
        }
        if (compare_version_ && selected_version_ && *compare_version_ == *selected_version_) {
            compare_version_.reset();
        }
        memberships_ = store_->Memberships(*selected_);
        unproven_ = store_->UnprovenSamePath(*selected_);
        unproven_text_ = unproven_.empty() ? std::string() : FormatUnproven(unproven_);
        if (selected_copy_ && *selected_copy_ >= memberships_.size()) {
            selected_copy_.reset();
        }
    } catch (const std::exception& error) {
        NoteStatus(error.what(), true);
    }
}

void HistoryPanel::NoteStatus(std::string text, bool error) {
    status_ = std::move(text);
    status_error_ = error;
}

std::optional<AssembledPack> HistoryPanel::TakeExportNotice() {
    if (!export_ready_) {
        return std::nullopt;
    }
    export_ready_ = false;
    return AssembledPack{assemble_output_, status_};
}

void HistoryPanel::JoinFinishedWork() {
    if (!import_done_.load(std::memory_order_acquire)) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    import_done_.store(false, std::memory_order_relaxed);
    running_ = false;
    if (import_ok_ && job_ == Job::Repair) {
        repaired_ = true;
    }
    if (import_ok_) {
        import_error_.clear();
        if (job_ == Job::Assemble) {
            NoteStatus(AssembleResultSummary(assemble_output_), false);
            export_ready_ = true;
        } else {
            NoteStatus({}, false);
        }
        Refresh();
        return;
    }
    NoteStatus(import_error_, true);
}

std::optional<SnapshotId> HistoryPanel::AssemblySnapshot() const {
    if (!selected_ || memberships_.empty()) {
        return std::nullopt;
    }
    if (selected_pack_) {
        for (const MembershipRow& membership : memberships_) {
            if (membership.Snapshot.Value == *selected_pack_) {
                return membership.Snapshot;
            }
        }
    }
    if (selected_copy_ && *selected_copy_ < memberships_.size()) {
        return memberships_[*selected_copy_].Snapshot;
    }
    const std::string& first = memberships_.front().Snapshot.Value;
    for (const MembershipRow& membership : memberships_) {
        if (membership.Snapshot.Value != first) {
            return std::nullopt;
        }
    }
    return memberships_.front().Snapshot;
}

void HistoryPanel::StartAssemble(const SnapshotId& snapshot, const std::filesystem::path& destination) {
    JoinFinishedWork();
    if (running_ || !store_ || !selected_ || snapshot.Value.empty()) {
        return;
    }
    try {
        importing_name_ = Narrow(destination.filename().wstring());
    } catch (const std::exception& error) {
        NoteStatus(error.what(), true);
        return;
    }
    assemble_output_ = destination;
    export_ready_ = false;
    const BlobId model = *selected_;
    const SnapshotId source = snapshot;
    cancel_ = CancellationSource();
    import_error_.clear();
    import_ok_ = false;
    import_done_.store(false, std::memory_order_relaxed);
    job_ = Job::Assemble;
    const CancellationToken token = cancel_.Token();
    running_ = true;
    try {
        worker_ = std::thread([this, source, model, destination, token] {
            try {
                AssembleIntoStore(store_directory_, source, model, destination, token);
                import_ok_ = true;
                import_error_.clear();
            } catch (const std::exception& error) {
                import_ok_ = false;
                import_error_ = error.what();
            } catch (...) {
                import_ok_ = false;
                import_error_ = "Could not assemble the pack.";
            }
            import_done_.store(true, std::memory_order_release);
        });
    } catch (const std::exception& error) {
        running_ = false;
        import_error_ = error.what();
        NoteStatus(import_error_, true);
    }
}

void HistoryPanel::StartRepair(const std::string& vpk_path, const std::string& game_directory) {
    JoinFinishedWork();
    if (running_ || !store_ || vpk_path.empty() || game_directory.empty()) {
        return;
    }
    try {
        importing_name_ = Narrow(std::filesystem::path(Widen(vpk_path)).filename().wstring());
    } catch (const std::exception& error) {
        NoteStatus(error.what(), true);
        return;
    }
    cancel_ = CancellationSource();
    import_error_.clear();
    import_ok_ = false;
    import_done_.store(false, std::memory_order_relaxed);
    job_ = Job::Repair;
    const CancellationToken token = cancel_.Token();
    running_ = true;
    try {
        worker_ = std::thread([this, vpk_path, game_directory, token] {
            try {
                const UpdateReport report = RepairIntoStore(store_directory_, vpk_path, game_directory, token);
                if (report.WroteFile) {
                    import_ok_ = true;
                    import_error_.clear();
                } else {
                    import_ok_ = false;
                    import_error_ = RepairFailureText(report);
                }
            } catch (const std::exception& error) {
                import_ok_ = false;
                import_error_ = error.what();
            } catch (...) {
                import_ok_ = false;
                import_error_ = "Could not repair the VPK.";
            }
            import_done_.store(true, std::memory_order_release);
        });
    } catch (const std::exception& error) {
        running_ = false;
        import_error_ = error.what();
        NoteStatus(import_error_, true);
    }
}

void HistoryPanel::HideObservation(const std::string& observation_id) {
    if (!store_ || observation_id.empty() || running_) {
        return;
    }
    try {
        store_->DropObservation(observation_id);
        NoteStatus({}, false);
        Refresh();
    } catch (const std::exception& error) {
        NoteStatus(error.what(), true);
    }
}

void HistoryPanel::ViewSelected(PreviewPanel& preview,
    const std::function<std::optional<std::vector<uint8_t>>(const std::string&)>& read_game,
    const std::vector<std::string>& game_clip_paths, const Shown& shown) {
    ZoneScopedN("View stored model");
    if (!store_ || !selected_version_ || *selected_version_ >= history_.Versions.size() || running_) {
        return;
    }
    const HistoryVersion& version = history_.Versions[*selected_version_];
    SnapshotId preferred_id;
    const SnapshotId* preferred = PreferredSnapshot(selected_copy_, memberships_, preferred_id);
    const PreparedVersion prepared = PrepareVersion(*store_, version, preferred, read_game);
    if (!prepared.Geometry) {
        preview.ShowError(prepared.Title, prepared.Error.empty() ? "The model could not be read." : prepared.Error);
    } else {
        try {
            // Clips load on a worker thread, which may outlive this panel, so the reader shares the store.
            const ResourceReader read = [store = store_, snapshot = prepared.Snapshot, has_snapshot = prepared.HasSnapshot,
                                            read_game](const std::string& path) {
                return ReadSnapshotThenGame(*store, has_snapshot ? &snapshot : nullptr, path, read_game);
            };
            preview.Load(PrepareModel(*prepared.Geometry, &read), prepared.Title, read,
                prepared.Animation ? &*prepared.Animation : nullptr, ClipsForPrepared(*store_, prepared, game_clip_paths));
        } catch (const std::exception& error) {
            preview.ShowError(prepared.Title, error.what());
        }
    }
    if (shown) {
        shown(false);
    }
}

std::optional<std::string> HistoryPanel::SelectedPackPath() const {
    const auto find_pack = [this](const std::string& snapshot) -> const PackRow* {
        for (const PackRow& pack : packs_) {
            if (pack.Snapshot.Value == snapshot) {
                return &pack;
            }
        }
        return nullptr;
    };
    const PackRow* pack = nullptr;
    if (selected_pack_) {
        pack = find_pack(*selected_pack_);
    }
    if (pack == nullptr && selected_copy_ && *selected_copy_ < memberships_.size()) {
        pack = find_pack(memberships_[*selected_copy_].Snapshot.Value);
    }
    if (pack == nullptr && !memberships_.empty()) {
        const std::string& snapshot = memberships_.front().Snapshot.Value;
        bool shared = true;
        for (const MembershipRow& row : memberships_) {
            if (row.Snapshot.Value != snapshot) {
                shared = false;
                break;
            }
        }
        if (shared) {
            pack = find_pack(snapshot);
        }
    }
    if (pack == nullptr) {
        return std::nullopt;
    }
    return pack->ObservationPath;
}

void HistoryPanel::CompareSelected(ComparisonView& compare, const Shown& focus_compare, const GameReader& read_game,
    const std::vector<std::string>& game_clip_paths) {
    ZoneScopedN("Compare stored versions");
    if (!store_ || !selected_version_ || !compare_version_ || *selected_version_ == *compare_version_
        || *selected_version_ >= history_.Versions.size() || *compare_version_ >= history_.Versions.size()) {
        return;
    }
    const HistoryVersion& primary = history_.Versions[*selected_version_];
    const HistoryVersion& secondary = history_.Versions[*compare_version_];
    const std::string caption = HistoryCompareCaption(history_, primary.Blob, secondary.Blob);
    try {
        SnapshotId preferred_id;
        const SnapshotId* preferred = PreferredSnapshot(selected_copy_, memberships_, preferred_id);
        const HistoryVersion* left_version = &primary;
        const HistoryVersion* right_version = &secondary;
        if (const HistoryLink* link = DirectHistoryLink(history_, primary.Blob, secondary.Blob)) {
            const HistoryVersion* from = nullptr;
            const HistoryVersion* to = nullptr;
            for (const HistoryVersion& version : history_.Versions) {
                if (SameBlob(version.Blob, link->From)) {
                    from = &version;
                }
                if (SameBlob(version.Blob, link->To)) {
                    to = &version;
                }
            }
            if (from != nullptr && to != nullptr) {
                left_version = from;
                right_version = to;
            }
        }
        const PreparedVersion left = PrepareVersion(*store_, *left_version, preferred, read_game);
        const PreparedVersion right = PrepareVersion(*store_, *right_version, preferred, read_game);
        if (!left.Geometry || !right.Geometry) {
            const std::string& message = !left.Geometry ? left.Error : right.Error;
            compare.ShowError(caption, message.empty() ? "The model could not be read." : message);
            Focus(focus_compare);
            return;
        }
        ModelVersions versions;
        versions.ModelPath = caption;
        versions.Versions[static_cast<size_t>(ModelVersion::Mod)] = SideData(left);
        versions.Versions[static_cast<size_t>(ModelVersion::Repaired)] = SideData(right);
        // Game stays unavailable so the existing view keeps the right viewport on the second stored payload.
        std::vector<std::string> clips = ClipsForPrepared(*store_, left, game_clip_paths);
        AppendClips(clips, ClipsForPrepared(*store_, right, game_clip_paths));
        const SnapshotId left_snapshot = left.Snapshot;
        const bool left_has = left.HasSnapshot;
        const SnapshotId right_snapshot = right.Snapshot;
        const bool right_has = right.HasSnapshot;
        const ComparisonView::VersionReader read =
            [store = store_, left_snapshot, left_has, right_snapshot, right_has, read_game](ModelVersion version, const std::string& path) {
                if (version == ModelVersion::Game) {
                    return read_game ? read_game(path) : std::optional<std::vector<uint8_t>>{};
                }
                const bool repaired = version == ModelVersion::Repaired;
                const SnapshotId& snapshot = repaired ? right_snapshot : left_snapshot;
                const bool has = repaired ? right_has : left_has;
                return ReadSnapshotThenGame(*store, has ? &snapshot : nullptr, path, read_game);
            };
        compare.Load(ComparisonView::Prepare(std::move(versions), read), read, std::move(clips));
        Focus(focus_compare);
    } catch (const std::exception& error) {
        compare.ShowError(caption, error.what());
        Focus(focus_compare);
    } catch (...) {
        compare.ShowError(caption, "The model could not be read.");
        Focus(focus_compare);
    }
}

void HistoryPanel::CompareWithGame(ComparisonView& compare, const Shown& focus_compare, const GameReader& read_game,
    const std::vector<std::string>& game_clip_paths) {
    ZoneScopedN("Compare with game");
    if (!store_ || !selected_version_ || *selected_version_ >= history_.Versions.size()) {
        return;
    }
    const HistoryVersion& version = history_.Versions[*selected_version_];
    std::string title = version.Blob.Hex().substr(0, 12);
    try {
        SnapshotId preferred_id;
        const SnapshotId* preferred = PreferredSnapshot(selected_copy_, memberships_, preferred_id);
        const PreparedVersion stored = PrepareVersion(*store_, version, preferred, read_game);
        title = stored.Title.empty() ? stored.Label : stored.Title;
        if (!stored.Geometry) {
            compare.ShowError(title, stored.Error.empty() ? "The model could not be read." : stored.Error);
            Focus(focus_compare);
            return;
        }
        const std::optional<std::string> skeleton = AnimationSkeletonPath(*stored.Geometry);
        ModelVersions versions;
        versions.ModelPath = title;
        versions.SkeletonPath = skeleton;
        versions.Versions[static_cast<size_t>(ModelVersion::Mod)] = SideData(stored);
        versions.Versions[static_cast<size_t>(ModelVersion::Game)] = GameSide(title, skeleton, read_game);
        const SnapshotId snapshot = stored.Snapshot;
        const bool has_snapshot = stored.HasSnapshot;
        const ComparisonView::VersionReader read =
            [store = store_, snapshot, has_snapshot, read_game](ModelVersion side, const std::string& path) {
                if (side == ModelVersion::Game) {
                    return read_game ? read_game(path) : std::optional<std::vector<uint8_t>>{};
                }
                return ReadSnapshotThenGame(*store, has_snapshot ? &snapshot : nullptr, path, read_game);
            };
        compare.Load(ComparisonView::Prepare(std::move(versions), read), read, ClipsForPrepared(*store_, stored, game_clip_paths));
        Focus(focus_compare);
    } catch (const std::exception& error) {
        compare.ShowError(title, error.what());
        Focus(focus_compare);
    } catch (...) {
        compare.ShowError(title, "The model could not be read.");
        Focus(focus_compare);
    }
}

void HistoryPanel::SelectModel(const BlobId& blob) {
    if (running_) {
        return;
    }
    if (!selected_ || !SameBlob(*selected_, blob)) {
        selected_version_.reset();
        compare_version_.reset();
    }
    selected_ = blob;
    selected_copy_.reset();
    LoadSelection();
}

void HistoryPanel::Draw(void* owner_window, PreviewPanel& preview, const GameReader& read_game,
    const std::vector<std::string>& game_clip_paths, ComparisonView& compare, const std::string& game_directory,
    bool files_busy, const Shown& shown, const std::function<void()>& before_repair) {
    ZoneScopedN("History");
    JoinFinishedWork();
    const HWND owner = static_cast<HWND>(owner_window);
    if (!store_) {
        ImGui::TextWrapped("%s", open_error_.empty() ? "The model library is unavailable." : open_error_.c_str());
        return;
    }

    // Toolbar: every action on the selected model, with the reason in the tooltip when one is unavailable.
    const bool two_versions = selected_version_ && compare_version_ && *selected_version_ != *compare_version_
        && *selected_version_ < history_.Versions.size() && *compare_version_ < history_.Versions.size();
    if (ui::ToolButton("View", selected_version_ ? "Open the selected version (A) in the inspector. Double-click a version also opens it."
        : "Select a model and a version first.", !running_ && selected_version_.has_value())) {
        ViewSelected(preview, read_game, game_clip_paths, shown);
    }
    ImGui::SameLine();
    if (ui::ToolButton("Compare A/B", two_versions ? "Compare versions A and B side by side."
        : "Click two versions: the last one clicked is A, the one before it B.", !running_ && two_versions)) {
        CompareSelected(compare, shown, read_game, game_clip_paths);
    }
    ImGui::SameLine();
    if (ui::ToolButton("Compare with game", game_directory.empty() ? kCompareNeedsGameFolder : "Compare version A with the installed game.",
        !running_ && selected_version_.has_value() && !game_directory.empty())) {
        CompareWithGame(compare, shown, read_game, game_clip_paths);
    }
    ui::ToolSeparator();
    const std::optional<std::string> pack_path = selected_ ? SelectedPackPath() : std::nullopt;
    bool pack_file = false;
    if (pack_path) {
        const double now = ImGui::GetTime();
        if (*pack_path != pack_exists_path_ || now - pack_exists_at_ >= 0.5) {
            pack_exists_path_ = *pack_path;
            pack_exists_ = ObservationFileExists(*pack_path);
            pack_exists_at_ = now;
        }
        pack_file = pack_exists_;
    }
    const RepairGate repair_gate = pack_path ? RepairGateFor(pack_file, !game_directory.empty()) : RepairGate{};
    const char* repair_tooltip = repair_gate.Message != nullptr ? repair_gate.Message
        : pack_path ? "Repair the pack of the selected copy. The repaired pack is saved as a new version; this one is kept."
        : "Select a copy or a pack to repair.";
    if (ui::ToolButton("Repair", repair_tooltip, !running_ && !files_busy && repair_gate.Enabled) && pack_path) {
        pack_exists_ = ObservationFileExists(*pack_path);
        pack_exists_at_ = ImGui::GetTime();
        if (pack_exists_) {
            // The pack may be open for previews or thumbnails, and the repair replaces it.
            if (before_repair) {
                before_repair();
            }
            StartRepair(*pack_path, game_directory);
        }
    }
    ImGui::SameLine();
    const std::optional<SnapshotId> assembly = AssemblySnapshot();
    if (ui::ToolButton("Assemble...", assembly ? "Write a new VPK from the selected copy's pack with this model."
        : "Select a copy or a pack to assemble from.", !running_ && assembly.has_value()) && assembly) {
        if (const std::optional<std::filesystem::path> path = AskForSaveVpk(owner)) {
            StartAssemble(*assembly, *path);
        }
    }
    ui::ToolSeparator();
    if (ui::ToolButton("Refresh", "Read the model and pack lists again", !running_)) {
        Refresh();
    }
    if (running_) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s %s...", job_ == Job::Repair ? "Repairing" : "Assembling", importing_name_.c_str());
        ImGui::SameLine();
        if (ui::ToolButton("Cancel")) {
            cancel_.Cancel();
        }
    }
    if (!status_.empty()) {
        const ImVec4 status_color = status_error_ ? ui::kError : ui::kGood;
        ui::TextEllipsis(status_, 0, &status_color);
        if (ImGui::BeginPopupContextItem("status_menu")) {
            ui::CopyMenuItem("Copy message", status_);
            if (ImGui::MenuItem("Dismiss")) {
                NoteStatus({}, false);
            }
            ImGui::EndPopup();
        }
    }

    if (!ImGui::BeginTable("history-columns", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoPadOuterX)) {
        return;
    }
    ImGui::TableSetupColumn("Library", ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableSetupColumn("History", ImGuiTableColumnFlags_WidthStretch, 0.58f);
    ImGui::TableNextColumn();
    // Cell padding is added below the row, so the columns leave room for it.
    const float height = ImGui::GetContentRegionAvail().y - ImGui::GetStyle().CellPadding.y;
    ImGui::BeginChild("library-column", {0, height});
    if (ImGui::BeginTabBar("library-tabs")) {
        char label[64];
        std::snprintf(label, sizeof(label), "Models (%zu)###models", models_.size());
        if (ImGui::BeginTabItem(label)) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##model-filter", "Filter models by path", filter_, sizeof(filter_));
            const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable
                | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable | ImGuiTableFlags_Hideable | ImGuiTableFlags_SizingFixedFit;
            if (ImGui::BeginTable("model-list", 3, flags)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort);
                ImGui::TableSetupColumn("Packs", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Hash", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableHeadersRow();
                bool resort = shown_dirty_ || shown_filter_ != filter_;
                if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
                    if (specs->SpecsDirty && specs->SpecsCount > 0) {
                        shown_column_ = specs->Specs[0].ColumnIndex;
                        shown_descending_ = specs->Specs[0].SortDirection == ImGuiSortDirection_Descending;
                        resort = true;
                    }
                    specs->SpecsDirty = false;
                }
                if (resort) {
                    ZoneScopedN("History filter");
                    RebuildShownModels();
                }
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(shown_models_.size()));
                while (clipper.Step()) {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                        const ModelRow& row = models_[shown_models_[static_cast<size_t>(i)]];
                        const std::string hex = row.Blob.Hex();
                        ImGui::PushID(hex.c_str());
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        const bool chosen = selected_ && SameBlob(*selected_, row.Blob);
                        if (ImGui::Selectable(row.DisplayPath.c_str(), chosen, ImGuiSelectableFlags_SpanAllColumns)) {
                            SelectModel(row.Blob);
                        }
                        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            ViewSelected(preview, read_game, game_clip_paths, shown);
                        }
                        ImGui::SetItemTooltip("%s\n%s\nIn %d packs under %d paths", row.DisplayPath.c_str(), hex.c_str(), row.SnapshotCount, row.PathCount);
                        if (ImGui::BeginPopupContextItem("model_menu")) {
                            if (ImGui::MenuItem("View", nullptr, false, !running_)) {
                                SelectModel(row.Blob);
                                ViewSelected(preview, read_game, game_clip_paths, shown);
                            }
                            if (ImGui::MenuItem("Compare with game", nullptr, false, !running_ && !game_directory.empty())) {
                                SelectModel(row.Blob);
                                CompareWithGame(compare, shown, read_game, game_clip_paths);
                            }
                            ImGui::Separator();
                            ui::CopyMenuItem("Copy path", row.DisplayPath);
                            ui::CopyMenuItem("Copy hash", hex);
                            ImGui::EndPopup();
                        }
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%d", row.SnapshotCount);
                        ImGui::TableNextColumn();
                        ImGui::TextDisabled("%s", hex.substr(0, 12).c_str());
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        std::snprintf(label, sizeof(label), "Packs (%zu)###packs", packs_.size());
        if (ImGui::BeginTabItem(label)) {
            const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable
                | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Hideable | ImGuiTableFlags_SizingFixedFit;
            if (ImGui::BeginTable("pack-list", 4, flags)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Pack", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Models", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Seen", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Observed", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableHeadersRow();
                std::optional<std::string> hide;
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(packs_.size()));
                while (clipper.Step()) for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const PackRow& pack = packs_[static_cast<size_t>(row)];
                    ImGui::PushID(pack.Snapshot.Value.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const bool chosen = selected_pack_ && *selected_pack_ == pack.Snapshot.Value;
                    if (ImGui::Selectable(pack.Label.c_str(), chosen, ImGuiSelectableFlags_SpanAllColumns) && !running_) {
                        selected_pack_ = pack.Snapshot.Value;
                    }
                    if (!pack.ObservationPath.empty()) {
                        ImGui::SetItemTooltip("%s", pack.ObservationPath.c_str());
                    }
                    if (ImGui::BeginPopupContextItem("pack_menu")) {
                        if (ImGui::MenuItem("Select", nullptr, chosen, !running_)) {
                            selected_pack_ = pack.Snapshot.Value;
                        }
                        if (ImGui::MenuItem("Hide this copy", nullptr, false, !running_ && !pack.ObservationId.empty())) {
                            hide = pack.ObservationId;
                        }
                        ImGui::Separator();
                        const std::filesystem::path path(Widen(pack.ObservationPath));
                        ui::PathMenuItems(path);
                        ui::CopyMenuItem("Copy snapshot id", pack.Snapshot.Value);
                        ImGui::EndPopup();
                    }
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%d", pack.ModelCount);
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%d", pack.ObservationCount);
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s", pack.ObservedUtc.c_str());
                    ImGui::PopID();
                }
                ImGui::EndTable();
                if (hide) {
                    HideObservation(*hide);
                }
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::TableNextColumn();
    ImGui::BeginChild("history-column", {0, height});
    if (!selected_) {
        ImGui::TextDisabled("Select a model to see its versions and copies.");
        ImGui::EndChild();
        ImGui::EndTable();
        return;
    }
    ui::SectionLabel("VERSIONS");
    ImGui::SameLine();
    ui::HelpMarker("Click a version to make it A; the version clicked before it becomes B. View opens A; Compare A/B opens both. "
        "Double-click a version to view it, or right-click it for more.");
    // As tall as its rows, up to half the column; the copies take the rest.
    const float row_height = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2;
    const float versions_height = std::min(row_height * static_cast<float>(history_lines_.size() + 1) + ImGui::GetStyle().CellPadding.y * 2 + 2,
        ImGui::GetContentRegionAvail().y * 0.5f);
    const ImGuiTableFlags list_flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable
        | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("versions", 2, list_flags, {0, versions_height})) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("##role", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, ImGui::CalcTextSize("A").x * 2);
        ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < history_lines_.size() && i < history_.Versions.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool primary = selected_version_ && *selected_version_ == i;
            const bool secondary = compare_version_ && *compare_version_ == i;
            if (primary) {
                ImGui::TextColored(ui::kAccent, "A");
            } else if (secondary) {
                ImGui::TextColored(ui::kWarning, "B");
            }
            ImGui::TableNextColumn();
            if (ImGui::Selectable(history_lines_[i].c_str(), primary || secondary, ImGuiSelectableFlags_SpanAllColumns) && !running_) {
                if (selected_version_ && *selected_version_ != i) {
                    compare_version_ = selected_version_;
                }
                selected_version_ = i;
                if (compare_version_ && *compare_version_ == *selected_version_) {
                    compare_version_.reset();
                }
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !running_) {
                selected_version_ = i;
                ViewSelected(preview, read_game, game_clip_paths, shown);
            }
            if (ImGui::BeginPopupContextItem("version_menu")) {
                if (ImGui::MenuItem("View", nullptr, false, !running_)) {
                    selected_version_ = i;
                    ViewSelected(preview, read_game, game_clip_paths, shown);
                }
                const bool can_compare = selected_version_ && *selected_version_ != i;
                if (ImGui::MenuItem("Compare with A", nullptr, false, !running_ && can_compare)) {
                    compare_version_ = i;
                    CompareSelected(compare, shown, read_game, game_clip_paths);
                }
                if (ImGui::MenuItem("Compare with game", nullptr, false, !running_ && !game_directory.empty())) {
                    selected_version_ = i;
                    CompareWithGame(compare, shown, read_game, game_clip_paths);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Set as A", nullptr, primary, !running_)) {
                    selected_version_ = i;
                    if (secondary) compare_version_.reset();
                }
                if (ImGui::MenuItem("Set as B", nullptr, secondary, !running_ && !primary)) {
                    compare_version_ = i;
                }
                ImGui::Separator();
                ui::CopyMenuItem("Copy hash", history_.Versions[i].Blob.Hex());
                ui::CopyMenuItem("Copy line", history_lines_[i]);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ui::SectionLabel("COPIES");
    ImGui::SameLine();
    const std::string membership_observation_id = selected_copy_ && *selected_copy_ < memberships_.size()
        ? memberships_[*selected_copy_].ObservationId
        : std::string();
    std::optional<std::string> hide;
    if (ui::ToolButton("Hide", "Hide the selected copy from the history. The pack file is not changed.",
        !running_ && !membership_observation_id.empty())) {
        hide = membership_observation_id;
    }
    const float copies_height = unproven_.empty() ? 0.0f : std::max(ImGui::GetTextLineHeightWithSpacing() * 4,
        ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing());
    if (ImGui::BeginTable("copies", 3, list_flags, {0, copies_height})) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Pack", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Observed", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < memberships_.size(); ++i) {
            const MembershipRow& membership = memberships_[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string& pack = membership.PackLabel.empty() ? membership.Snapshot.Value : membership.PackLabel;
            const bool chosen = selected_copy_ && *selected_copy_ == i;
            if (ImGui::Selectable(pack.c_str(), chosen, ImGuiSelectableFlags_SpanAllColumns) && !running_) {
                selected_copy_ = i;
            }
            if (ImGui::BeginPopupContextItem("copy_menu")) {
                if (ImGui::MenuItem("Select", nullptr, chosen, !running_)) {
                    selected_copy_ = i;
                }
                if (ImGui::MenuItem("Hide this copy", nullptr, false, !running_ && !membership.ObservationId.empty())) {
                    hide = membership.ObservationId;
                }
                ImGui::Separator();
                ui::CopyMenuItem("Copy path", membership.Path);
                ui::CopyMenuItem("Copy pack name", pack);
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(membership.Path.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", membership.ObservedUtc.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (hide) {
        HideObservation(*hide);
    }
    if (!unproven_.empty() && ImGui::CollapsingHeader("Unproven matches")) {
        ImGui::TextWrapped("%s", unproven_text_.c_str());
    }
    ImGui::EndChild();
    ImGui::EndTable();
}

}  // namespace skeleton_repair
