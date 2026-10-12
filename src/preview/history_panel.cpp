#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>

#include "history_panel.hpp"

#include "comparison_view.hpp"
#include "history_compare.hpp"
#include "library_model.hpp"
#include "model_geometry.hpp"
#include "model_versions.hpp"
#include "preview_panel.hpp"
#include "store_reader.hpp"
#include "strings.hpp"
#include "updater.hpp"

#include <imgui.h>

#include <algorithm>
#include <exception>
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

using GameReader = std::function<std::optional<std::vector<uint8_t>>(const std::string&)>;

struct PreparedVersion {
    std::string Label;
    std::string Title;
    std::optional<ModelGeometry> Geometry;
    std::optional<std::vector<GeometryBone>> Animation;
    SnapshotId Snapshot;
    bool HasSnapshot = false;
    std::string Error;
};

const SnapshotId* PreferredSnapshot(const std::optional<size_t>& selected_copy, const std::vector<MembershipRow>& memberships,
    SnapshotId& storage) {
    if (!selected_copy || *selected_copy >= memberships.size()) {
        return nullptr;
    }
    storage = memberships[*selected_copy].Snapshot;
    return &storage;
}

PreparedVersion PrepareVersion(ContentStore& store, const HistoryVersion& version, const SnapshotId* preferred, const GameReader& read_game) {
    PreparedVersion prepared;
    const std::string hex = version.Blob.Hex();
    prepared.Label = hex.substr(0, hex.size() < 12 ? hex.size() : 12);
    prepared.Title = prepared.Label;
    try {
        const std::vector<uint8_t> bytes = store.ReadBlob(version.Blob);
        prepared.Geometry = DecodeModelGeometry(bytes);
        const std::vector<MembershipRow> rows = store.Memberships(version.Blob);
        if (const MembershipRow* membership = VersionMembership(rows, preferred)) {
            prepared.Snapshot = membership->Snapshot;
            prepared.HasSnapshot = true;
            if (!membership->Path.empty()) {
                prepared.Title = membership->Path;
            }
        } else if (!version.Paths.empty()) {
            prepared.Title = version.Paths.front();
        }
        const SnapshotId* snapshot = prepared.HasSnapshot ? &prepared.Snapshot : nullptr;
        if (prepared.Geometry) {
            if (const std::optional<std::string> skeleton = AnimationSkeletonPath(*prepared.Geometry)) {
                if (const std::optional<std::vector<uint8_t>> skeleton_bytes = ReadSnapshotThenGame(store, snapshot, *skeleton, read_game)) {
                    try {
                        prepared.Animation = DecodeAnimationSkeleton(*skeleton_bytes);
                    } catch (const std::exception& error) {
                        prepared.Error = error.what();
                    }
                }
            }
        }
    } catch (const std::exception& error) {
        prepared.Geometry.reset();
        prepared.Error = error.what();
    } catch (...) {
        prepared.Geometry.reset();
        prepared.Error = "The model could not be read.";
    }
    return prepared;
}

VersionData SideData(const PreparedVersion& prepared) {
    VersionData data;
    data.Available = prepared.Geometry.has_value();
    data.Model = prepared.Geometry;
    data.AnimationSkeleton = prepared.Animation;
    data.Error = prepared.Error;
    data.Notes.push_back(prepared.Label);
    return data;
}

VersionData GameSide(const std::string& model_path, const std::optional<std::string>& skeleton_path, const GameReader& read_game) {
    VersionData data;
    std::optional<std::vector<uint8_t>> bytes;
    try {
        if (read_game) {
            bytes = read_game(model_path);
        }
    } catch (const std::exception& error) {
        data.Error = error.what();
        return data;
    }
    if (!bytes) {
        data.Notes.push_back("The game has no model at this path.");
        return data;
    }
    data.Available = true;
    data.Notes.push_back("From the game archive.");
    try {
        data.Model = DecodeModelGeometry(*bytes);
    } catch (const std::exception& error) {
        data.Error = error.what();
    }
    if (!skeleton_path || !read_game) {
        return data;
    }
    try {
        if (const std::optional<std::vector<uint8_t>> skeleton_bytes = read_game(*skeleton_path)) {
            data.AnimationSkeleton = DecodeAnimationSkeleton(*skeleton_bytes);
        }
    } catch (const std::exception& error) {
        if (!data.Error.empty()) {
            data.Error += " ";
        }
        data.Error += error.what();
    }
    return data;
}

std::vector<std::string> ClipsForPrepared(ContentStore& store, const PreparedVersion& prepared, const std::vector<std::string>& game_clips) {
    std::vector<std::string> candidates;
    if (prepared.HasSnapshot) {
        try {
            candidates = store.SnapshotPaths(prepared.Snapshot, "vnmclip_c");
        } catch (const std::exception&) {
            candidates.clear();
        }
    }
    candidates.insert(candidates.end(), game_clips.begin(), game_clips.end());
    return ClipsUnderModel(prepared.Title, candidates);
}

void AppendClips(std::vector<std::string>& clips, const std::vector<std::string>& more) {
    for (const std::string& path : more) {
        if (std::find(clips.begin(), clips.end(), path) == clips.end()) {
            clips.push_back(path);
        }
    }
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
    if (!store_) {
        return;
    }
    try {
        models_ = store_->ListModels();
        packs_ = store_->ListPacks();
    } catch (const std::exception& error) {
        status_ = error.what();
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

void HistoryPanel::LoadSelection() {
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
        status_ = error.what();
    }
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
        status_.clear();
        Refresh();
        return;
    }
    status_ = import_error_;
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
        status_ = error.what();
        return;
    }
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
                ContentStore store = ContentStore::Open(store_directory_);
                store.Assemble(source, {model}, destination, token);
                store.ImportVpk(destination, token);
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
        status_ = import_error_;
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
        status_ = error.what();
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
                ContentStore store = ContentStore::Open(store_directory_);
                const UpdateReport report = HeroModelUpdater::Update(game_directory, vpk_path, std::nullopt, true, true, {}, token,
                    false, &store);
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
        status_ = import_error_;
    }
}

void HistoryPanel::HideObservation(const std::string& observation_id) {
    if (!store_ || observation_id.empty() || running_) {
        return;
    }
    try {
        store_->DropObservation(observation_id);
        status_.clear();
        Refresh();
    } catch (const std::exception& error) {
        status_ = error.what();
    }
}

void HistoryPanel::ViewSelected(PreviewPanel& preview,
    const std::function<std::optional<std::vector<uint8_t>>(const std::string&)>& read_game,
    const std::vector<std::string>& game_clip_paths, const Shown& shown) {
    if (!store_ || !selected_version_ || *selected_version_ >= history_.Versions.size() || running_) {
        return;
    }
    const HistoryVersion& version = history_.Versions[*selected_version_];
    std::string title = version.Blob.Hex().substr(0, 12);
    try {
        const std::vector<uint8_t> bytes = store_->ReadBlob(version.Blob);
        const ModelGeometry geometry = DecodeModelGeometry(bytes);
        const SnapshotId* preferred = nullptr;
        SnapshotId preferred_id;
        if (selected_copy_ && *selected_copy_ < memberships_.size()) {
            preferred_id = memberships_[*selected_copy_].Snapshot;
            preferred = &preferred_id;
        }
        const std::vector<MembershipRow> version_rows = store_->Memberships(version.Blob);
        const MembershipRow* membership = VersionMembership(version_rows, preferred);
        const SnapshotId* snapshot = membership == nullptr ? nullptr : &membership->Snapshot;
        if (membership != nullptr && !membership->Path.empty()) {
            title = membership->Path;
        } else if (!version.Paths.empty()) {
            title = version.Paths.front();
        }
        std::vector<std::string> clip_candidates;
        if (snapshot != nullptr) {
            clip_candidates = store_->SnapshotPaths(*snapshot, "vnmclip_c");
        }
        clip_candidates.insert(clip_candidates.end(), game_clip_paths.begin(), game_clip_paths.end());
        const std::vector<std::string> clips = ClipsUnderModel(title, clip_candidates);
        const SnapshotId snapshot_copy = snapshot != nullptr ? *snapshot : SnapshotId{};
        const bool has_snapshot = snapshot != nullptr;
        // Clips load on a worker thread, which may outlive this panel, so the reader shares the store.
        const ResourceReader read = [store = store_, snapshot_copy, has_snapshot, read_game](const std::string& path) {
            return ReadSnapshotThenGame(*store, has_snapshot ? &snapshot_copy : nullptr, path, read_game);
        };
        std::optional<std::vector<GeometryBone>> animation;
        if (const std::optional<std::string> skeleton = AnimationSkeletonPath(geometry)) {
            if (const std::optional<std::vector<uint8_t>> skeleton_bytes = read(*skeleton)) {
                try {
                    animation = DecodeAnimationSkeleton(*skeleton_bytes);
                } catch (const std::exception&) {
                }
            }
        }
        preview.Load(PrepareModel(geometry, &read), title, read, animation ? &*animation : nullptr, clips);
    } catch (const std::exception& error) {
        preview.ShowError(title, error.what());
    } catch (...) {
        preview.ShowError(title, "The model could not be read.");
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

void HistoryPanel::Draw(void* owner_window, PreviewPanel& preview, const GameReader& read_game,
    const std::vector<std::string>& game_clip_paths, ComparisonView& compare, const std::string& game_directory,
    bool files_busy, const Shown& shown, const std::function<void()>& before_repair) {
    JoinFinishedWork();
    const HWND owner = static_cast<HWND>(owner_window);

    ImGui::TextColored({.47f, .85f, .77f, 1.f}, "YOUR COLLECTION");
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted("Version history");
    ImGui::SetWindowFontScale(1.f);
    ImGui::TextDisabled("Every copy of each model you have added, and how repairs changed it.");
    ImGui::Spacing();
    if (!store_) {
        ImGui::TextWrapped("%s", open_error_.empty() ? "The model library is unavailable." : open_error_.c_str());
        return;
    }
    if (running_) {
        ImGui::TextWrapped("%s %s", job_ == Job::Repair ? "Repairing" : "Assembling", importing_name_.c_str());
        if (ImGui::Button("Cancel")) {
            cancel_.Cancel();
        }
    }
    if (!status_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", status_.c_str());
    }
    ImGui::Separator();

    if (!ImGui::BeginTable("history-columns", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        return;
    }
    ImGui::TableSetupColumn("Library", ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableSetupColumn("History", ImGuiTableColumnFlags_WidthStretch, 0.58f);
    ImGui::TableNextColumn();
    ImGui::BeginChild("library-column");
    if (ImGui::BeginTabBar("library-tabs")) {
        if (ImGui::BeginTabItem("Models")) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##model-filter", "Filter", filter_, sizeof(filter_));
            const std::vector<ModelRow> rows = FilterModels(models_, filter_);
            if (ImGui::BeginChild("model-list")) {
                for (const ModelRow& row : rows) {
                    std::string label = FormatModelRow(row);
                    label += "##";
                    label += row.Blob.Hex();
                    const bool chosen = selected_ && SameBlob(*selected_, row.Blob);
                    if (ImGui::Selectable(label.c_str(), chosen) && !running_) {
                        if (!selected_ || !SameBlob(*selected_, row.Blob)) {
                            selected_version_.reset();
                            compare_version_.reset();
                        }
                        selected_ = row.Blob;
                        selected_copy_.reset();
                        LoadSelection();
                    }
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Packs")) {
            std::string pack_observation_id;
            if (selected_pack_) {
                for (const PackRow& pack : packs_) {
                    if (pack.Snapshot.Value == *selected_pack_) {
                        pack_observation_id = pack.ObservationId;
                        break;
                    }
                }
            }
            const bool hide_pack_disabled = running_ || pack_observation_id.empty();
            if (hide_pack_disabled) {
                ImGui::BeginDisabled();
            }
            if (ImGui::Button("Hide this copy##pack")) {
                HideObservation(pack_observation_id);
            }
            if (hide_pack_disabled) {
                ImGui::EndDisabled();
            }
            if (ImGui::BeginChild("pack-list")) {
                for (const PackRow& pack : packs_) {
                    std::string line = pack.Label + "  " + std::to_string(pack.ModelCount) + " models  "
                        + std::to_string(pack.ObservationCount) + " observations##" + pack.Snapshot.Value;
                    const bool chosen = selected_pack_ && *selected_pack_ == pack.Snapshot.Value;
                    if (ImGui::Selectable(line.c_str(), chosen) && !running_) {
                        selected_pack_ = pack.Snapshot.Value;
                    }
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::TableNextColumn();
    ImGui::BeginChild("history-column");
    if (!selected_) {
        ImGui::TextDisabled("Select a model.");
    } else {
        for (size_t i = 0; i < history_lines_.size() && i < history_.Versions.size(); ++i) {
            std::string label = history_lines_[i] + "##version" + std::to_string(i);
            const bool chosen = (selected_version_ && *selected_version_ == i) || (compare_version_ && *compare_version_ == i);
            if (ImGui::Selectable(label.c_str(), chosen) && !running_) {
                if (selected_version_ && *selected_version_ != i) {
                    compare_version_ = selected_version_;
                }
                selected_version_ = i;
                if (compare_version_ && *compare_version_ == *selected_version_) {
                    compare_version_.reset();
                }
            }
        }
        const bool two_versions = selected_version_ && compare_version_ && *selected_version_ != *compare_version_
            && *selected_version_ < history_.Versions.size() && *compare_version_ < history_.Versions.size();
        const bool view_disabled = running_ || !selected_version_;
        if (view_disabled) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("View")) {
            ViewSelected(preview, read_game, game_clip_paths, shown);
        }
        if (view_disabled) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        const bool compare_disabled = running_ || !two_versions;
        if (compare_disabled) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Compare")) {
            CompareSelected(compare, shown, read_game, game_clip_paths);
        }
        if (compare_disabled) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        const std::optional<std::string> pack_path = SelectedPackPath();
        const bool pack_file = pack_path && ObservationFileExists(*pack_path);
        const RepairGate repair_gate = pack_path ? RepairGateFor(pack_file, !game_directory.empty()) : RepairGate{};
        const bool repair_disabled = running_ || files_busy || !repair_gate.Enabled;
        if (repair_disabled) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Repair") && pack_path && pack_file) {
            // The pack may be open for previews or thumbnails, and the repair replaces it.
            if (before_repair) {
                before_repair();
            }
            StartRepair(*pack_path, game_directory);
        }
        if (repair_disabled) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        const std::optional<SnapshotId> assembly = AssemblySnapshot();
        const bool assemble_disabled = running_ || !assembly;
        if (assemble_disabled) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Assemble") && assembly) {
            if (const std::optional<std::filesystem::path> path = AskForSaveVpk(owner)) {
                StartAssemble(*assembly, *path);
            }
        }
        if (assemble_disabled) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        const bool compare_game_disabled = running_ || !selected_version_ || game_directory.empty();
        if (compare_game_disabled) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Compare with game")) {
            CompareWithGame(compare, shown, read_game, game_clip_paths);
        }
        if (compare_game_disabled) {
            ImGui::EndDisabled();
        }
        if (repair_gate.Message != nullptr) {
            ImGui::TextDisabled("%s", repair_gate.Message);
        }
        if (game_directory.empty()) {
            ImGui::TextDisabled("%s", kCompareNeedsGameFolder);
        }
        ImGui::SeparatorText("Copies");
        for (size_t i = 0; i < memberships_.size(); ++i) {
            const MembershipRow& membership = memberships_[i];
            std::string label = membership.PackLabel.empty() ? membership.Snapshot.Value : membership.PackLabel;
            label += "  ";
            label += membership.Path;
            label += "##";
            label += membership.Snapshot.Value;
            label += "/";
            label += std::to_string(i);
            const bool chosen = selected_copy_ && *selected_copy_ == i;
            if (ImGui::Selectable(label.c_str(), chosen) && !running_) {
                selected_copy_ = i;
            }
        }
        const std::string membership_observation_id = selected_copy_ && *selected_copy_ < memberships_.size()
            ? memberships_[*selected_copy_].ObservationId
            : std::string();
        const bool hide_disabled = running_ || membership_observation_id.empty();
        if (hide_disabled) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Hide this copy")) {
            HideObservation(membership_observation_id);
        }
        if (hide_disabled) {
            ImGui::EndDisabled();
        }
        if (!unproven_.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", unproven_text_.c_str());
        }
    }
    ImGui::EndChild();
    ImGui::EndTable();
}

}  // namespace skeleton_repair
