// Tracy's winsock2.h comes before windows.h.
#include "profile.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "workspace.hpp"

#include "character_view.hpp"
#include "content_store.hpp"
#include "game_folder.hpp"
#include "product_version.hpp"
#include "shell_layout.hpp"
#include "shell_window.hpp"
#include "strings.hpp"
#include "theme.hpp"
#include "ui.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>

namespace skeleton_repair {
namespace {

std::filesystem::path ContentDirectoryFor(const std::filesystem::path& root) {
    try {
        return LocalModStore(root).ContentDirectory();
    } catch (const std::exception&) {
        return {};
    }
}

bool HasExtension(const std::filesystem::path& path, const char* extension) {
    return EqualsIgnoreCase(Narrow(path.extension().wstring()), extension);
}

std::string Trimmed(std::string text) {
    text.erase(0, text.find_first_not_of(" \t"));
    text.erase(text.find_last_not_of(" \t") + 1);
    return text;
}

}  // namespace

Workspace::Workspace(const VulkanDevice& device, const std::filesystem::path& data_root, HWND__* window)
    : window_(window), data_root_(data_root), layout_ini_(DataPathsFor(data_root).LayoutIni), panel_(device), compare_(device),
      multi_(device), builder_(device, archives_, data_root), history_(ContentDirectoryFor(data_root)), mods_(data_root),
      banana_(device), library_(data_root) {
    multi_.SetArchives(archives_);
    builder_.SetMultiViewHandler([this](const LibraryModel& model) {
        multi_.Open(model, builder_.Models());
        ShowPage(Page::Multi);
    });
    builder_.SetBuildAsModHandler([this] { open_build_mod_ = true; });
    if (library_.Store()) SetStoreStatus("Opening your library...", false);
    else ReportError(library_.SaveError());
}

Workspace::~Workspace() {
    // Work finished during shutdown is still applied and saved, such as imports completed before a cancel.
    try {
        library_.Cancel();
        library_.Wait();
        PollLibrary();
        Persist(true);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
    }
}

void Workspace::Start(const SettingsInspection& settings, const std::optional<std::filesystem::path>& game_folder,
    const std::vector<std::filesystem::path>& files, const std::optional<std::string>& clip) {
    if (settings.State == SettingsState::Corrupt) NoteStartupError(settings.Message);
    else if (settings.State == SettingsState::Ok) settings_game_ = settings.GameDirectory;
    // A saved library game is applied when the restore finishes. Detect Steam only when that path is empty.
    const ResolvedGamePath chosen_game = ResolveGamePath(game_folder, {});
    if (!chosen_game.AutoDetect) {
        keep_game_ = true;
        OpenGame(chosen_game.Path);
    }
    if (files.empty()) return;
    if (files.size() > 1 && HasExtension(files[0], ".vpk") && HasExtension(files[1], ".vmdl_c")) {
        pending_clip_ = clip;
        OpenArchiveModel(files[0], VpkArchive::Normalize(Narrow(files[1].wstring())));
        ShowPage(Page::Inspect);
    } else {
        AddPaths(files);
    }
}

void Workspace::Frame(const std::vector<std::filesystem::path>& dropped) {
    builder_.Poll();
    PollLibrary();
    mods_.Poll();
    if (const auto error = mods_.TakeError()) ReportError(*error);
    MaybeAutoDetect();
    PollGame();
    PollInspect();
    multi_.Poll();
    if (const auto rename = std::exchange(pending_rename_, std::nullopt)) StartRename(rename->first, rename->second);
    // A repair from the version history may replace one of the gallery's stored copies too.
    if (repair_.Poll() | history_.TakeRepaired()) AfterRepair();
    PollGameUpdate();
    if (!dropped.empty() && !FilesBusy()) AddPaths(dropped);

    // The menu and status bars take their rows before the dockspace fills the rest of the window.
    DrawMenuBar();
    DrawStatusBar();
    // The navigation takes the left edge, so it stays put whether or not the mod files panel is docked beside a page.
    DrawNavRail();
    const bool reset_layout = std::exchange(reset_layout_, false);
    if (reset_layout) {
        show_explorer_ = true;
        focus_explorer_ = true;
        focus_workspace_ = true;
        std::error_code remove_error;
        std::filesystem::remove(layout_ini_, remove_error);
    }
    const ImGuiID dockspace = ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_NoWindowMenuButton);
    // Rebuild only when nothing was saved, or the user asked. A saved floating layout is left alone.
    if (reset_layout || (first_frame_ && !shell::HasSavedLayout())) shell::BuildDefaultDock(dockspace);
    DrawExplorer();
    DrawWorkspace();
    if (std::exchange(first_frame_, false)) {
        shell::BringOnScreen(shell::kExplorer, {40.f, 40.f});
        shell::BringOnScreen(shell::kWorkspace, {48.f, 48.f});
    }
    DrawDialogs();
    Persist();
}

void Workspace::Record(VkCommandBuffer command_buffer) {
    panel_.Record(command_buffer);
    compare_.Record(command_buffer);
    multi_.Record(command_buffer);
}

void Workspace::Close() {
    repair_.Cancel();
}

// ---------- state ----------

// Work that reads or replaces the library's stored mods. Applying mods to the game reads them too.
bool Workspace::LibraryWorkBusy() const { return repair_.Busy() || builder_.Busy() || history_.Busy() || library_.Busy(); }
bool Workspace::FilesBusy() const { return LibraryWorkBusy() || mods_.Busy(); }
bool Workspace::PackBusy() const { return builder_.Busy() || repair_.Busy() || library_.Busy(); }
std::filesystem::path Workspace::GamePath() const { return game_ ? std::filesystem::path(Widen(game_name_)) : std::filesystem::path{}; }

void Workspace::ReportError(const std::string& message) {
    if (message.empty()) return;
    last_error_ = message;
    if (message == last_logged_) return;
    last_logged_ = message;
    AppendErrorLog(DataPathsFor(data_root_).LogFile, DiagnosticText(SR_PRODUCT_TITLE, SR_GIT_COMMIT, data_root_, message));
}

void Workspace::NoteStartupError(const std::string& message) {
    if (message.empty()) return;
    startup_notice_ = message;
    last_error_ = message;
    last_logged_ = message;
}

void Workspace::SetStoreStatus(std::string text, bool error) {
    store_status_ = std::move(text);
    store_status_is_error_ = error;
    if (error) ReportError(store_status_);
}

// ---------- library ----------

void Workspace::Persist(bool force) {
    ZoneScopedN("Persist library");
    if (!library_.Store() || library_.Busy() || !library_.CanPersist()) return;
    if (!force && builder_.Revision() == saved_revision_ && configured_game_ == saved_game_) return;
    switch (library_.Save(builder_.Snapshot(mod_paths_, configured_game_), force)) {
    case LibrarySession::SaveOutcome::Saved:
    case LibrarySession::SaveOutcome::Unchanged:
        saved_revision_ = builder_.Revision();
        saved_game_ = configured_game_;
        break;
    case LibrarySession::SaveOutcome::Failed:
        ReportError(library_.SaveError());
        break;
    case LibrarySession::SaveOutcome::Skipped:
        break;
    }
}

// A job that adds to or reopens the saved collection needs it to match what is on screen, so it saves first.
// False, with the reason shown, when the job cannot start.
bool Workspace::PrepareLibraryJob(const char* blocked, const char* before) {
    if (!library_.Store()) {
        SetStoreStatus("Local mod storage is unavailable. " + library_.SaveError(), true);
        return false;
    }
    if (!library_.CanPersist()) {
        SetStoreStatus(std::string("Your library has not been opened, so nothing can be ") + blocked + " yet. " + library_.SaveError(), true);
        return false;
    }
    Persist(true);
    if (!library_.SaveError().empty()) {
        SetStoreStatus(std::string("Could not save your library before ") + before + ": " + library_.SaveError(), true);
        return false;
    }
    return true;
}

void Workspace::StartMerge(const std::filesystem::path& other) {
    if (FilesBusy() || !PrepareLibraryJob("added to it", "adding another")) return;
    SetStoreStatus("Adding " + Narrow(other.filename().wstring()) + " to your library...", false);
    library_.StartMerge(other);
}

void Workspace::StartRename(const std::filesystem::path& archive, const std::string& name) {
    if (FilesBusy() || !PrepareLibraryJob("renamed", "renaming")) return;
    // The reopened library replaces every archive on screen.
    ReleaseArchives();
    SetStoreStatus("Renaming...", false);
    library_.StartRename(archive, name);
}

void Workspace::StartImport(const ModelLibrary& incoming, bool replace) {
    if (library_.Restoring()) {
        // Started after the restore, so these are added to the restored collection.
        for (const auto& source : incoming.Sources) queued_imports_.Sources.push_back(source);
        return;
    }
    if (FilesBusy()) return;
    if (!library_.Store()) {
        SetStoreStatus("Local mod storage is unavailable. " + library_.SaveError(), true);
        return;
    }
    SetStoreStatus("Importing mods into local storage...", false);
    auto starting = replace ? incoming : builder_.Snapshot(mod_paths_, configured_game_);
    if (replace) starting.Sources.clear();
    library_.StartImport(incoming.Sources, std::move(starting));
}

void Workspace::StartDownload(const GbMod& mod, const GbFile& file, bool enable) {
    if (library_.Restoring() || FilesBusy() || !PrepareLibraryJob("added to it", "downloading")) return;
    SetStoreStatus("Downloading " + mod.Name + "...", false);
    library_.StartDownload(mod, file, enable);
}

// The library mod built from the pack under `name` earlier, which a new build of that name replaces.
std::optional<std::string> Workspace::BuiltModNamed(const std::string& name) const {
    for (const auto& entry : ModEntries()) {
        const auto origin = mods_.State().Origin(entry.ContentId);
        if (origin && origin->Built && EqualsIgnoreCase(entry.Name, name)) return entry.ContentId;
    }
    return std::nullopt;
}

void Workspace::StartBuildMod(const std::string& name, bool enable) {
    if (library_.Restoring() || FilesBusy() || !PrepareLibraryJob("added to it", "building")) return;
    auto selection = builder_.Snapshot(mod_paths_, configured_game_);
    if (selection.Selected.empty() && selection.Packages.empty()) return;
    SetStoreStatus("Building " + name + "...", false);
    library_.StartBuild(name, enable, std::move(selection), GamePath(), BuiltModNamed(name));
}

// The library's stored mods for the Mods page, in library order.
std::vector<ModEntry> Workspace::ModEntries() const {
    std::vector<ModEntry> entries;
    const LocalModStore* store = library_.Store();
    if (store == nullptr) return entries;
    for (const auto& path : mod_paths_) {
        const auto id = store->ContentId(path);
        if (!id) continue;
        const LibraryPanel::SourceInfo* info = builder_.FindSource(path);
        std::string name = info ? info->Name : Narrow(path.stem().wstring());
        if (name.size() > 4 && EqualsIgnoreCase(std::string_view(name).substr(name.size() - 4), ".vpk")) name.resize(name.size() - 4);
        entries.push_back({*id, std::move(name), path});
    }
    return entries;
}

void Workspace::PollLibrary() {
    ZoneScopedN("Poll import");
    const auto finished = library_.Poll();
    if (!finished) return;
    ApplyFinished(*finished);
    if (finished->WasRestoring) {
        // Imports requested during a failed restore still need storage, but are not saved over the library.
        if (!queued_imports_.Sources.empty()) StartImport(std::exchange(queued_imports_, {}), false);
        else inspect_after_import_.reset();
    }
    // Imports and merges add models to the version history too.
    history_.Refresh();
    if (std::exchange(multi_suspended_, false)) multi_.Reload();
}

void Workspace::ApplyFinished(const LibrarySession::Finished& finished) {
    const auto fail = [&](const std::string& error) {
        SetStoreStatus(finished.Kind == LibrarySession::Job::Downloading ? "Could not download " + finished.Name + ": " + error : error, true);
        if (!finished.WasRestoring) inspect_after_import_.reset();
    };
    if (!finished.Result) {
        fail(finished.Error);
        return;
    }
    const LibraryJobResult& result = *finished.Result;
    try {
        ApplyLibrary(result.Library);
        if (!result.ReplacedId.empty() && result.AddedIds.size() == 1) mods_.State().Replace(result.ReplacedId, result.AddedIds.front());
        if (result.Origin && !result.AddedIds.empty()) mods_.RecordAdded(result.AddedIds, *result.Origin, result.Enable);
        if (!result.Errors.empty()) SetStoreStatus(result.Errors, true);
        else if (!result.Done.empty()) SetStoreStatus(result.Done, false);
        else if (finished.WasRestoring) SetStoreStatus({}, false);
        else SetStoreStatus("Saved in your library. Your collection will reopen automatically.", false);
        Persist(true);
        if (inspect_after_import_ && !finished.WasRestoring) {
            for (const auto& [source, cached] : result.Imported) {
                if (source == inspect_after_import_->first) StartInspect(cached, inspect_after_import_->second);
            }
            inspect_after_import_.reset();
        }
    } catch (const std::exception& error) {
        fail(error.what());
    }
}

void Workspace::ApplyLibrary(const ModelLibrary& library) {
    mod_paths_ = library.Sources;
    CloseInspection();
    builder_.Restore(library);
    if (!keep_game_ && !library.GameArchive.empty() && library.GameArchive != configured_game_) OpenGame(library.GameArchive);
    SyncSources();
}

void Workspace::AfterRepair() {
    // Repaired archives were replaced on disk, so reopen them.
    archives_->Remove(mod_paths_);
    builder_.InvalidateThumbnails();
    // Stores each repaired copy as a new version; the version before the repair stays in the library.
    if (library_.Store()) StartImport(builder_.Snapshot(mod_paths_, configured_game_), false);
    if (!active_mod_.empty()) StartInspect(active_mod_, selected_);
    multi_suspended_ = false;
    multi_.Reload();
}

// ---------- game and inspection ----------

void Workspace::MaybeAutoDetect() {
    if (auto_detect_done_) return;
    if (keep_game_ || !configured_game_.empty()) {
        auto_detect_done_ = true;
        return;
    }
    if (library_.Restoring() || (library_.Store() && !library_.CanPersist())) return;
    auto_detect_done_ = true;
    if (!settings_game_.empty()) {
        OpenGame(std::filesystem::path(Widen(std::exchange(settings_game_, {}))));
        return;
    }
    try {
        if (const auto detected = FindGameFolder()) OpenGame(Widen(*detected));
    } catch (const std::exception& error) {
        game_error_ = error.what();
        ReportError(game_error_);
    }
}

void Workspace::OpenGame(const std::filesystem::path& chosen) {
    game_.reset();
    game_clips_.clear();
    game_directory_.clear();
    game_name_.clear();
    game_error_.clear();
    game_loading_ = GamePak(chosen);
    configured_game_ = std::filesystem::absolute(game_loading_).lexically_normal();
    std::snprintf(manual_game_, sizeof(manual_game_), "%s", Narrow(configured_game_.wstring()).c_str());
    // The game directory has hundreds of thousands of entries, so it is parsed off the UI thread.
    game_loader_.Start([archives = archives_, pak = game_loading_](CancellationToken) { return LoadGame(*archives, pak); });
    SyncSources();
}

void Workspace::PollGame() {
    ZoneScopedN("Poll game");
    try {
        auto loaded = game_loader_.Take();
        if (!loaded) return;
        game_ = loaded->Archive;
        game_clips_ = std::move(loaded->Clips);
        game_name_ = Narrow(game_loading_.wstring());
        game_directory_ = Narrow(game_loading_.parent_path().wstring());
        multi_.SetGame(game_, GamePath());
        game_build_ = GameBuildId(game_loading_.parent_path());
        // The first game found is the one the mods are taken to work with. A different build later is an update.
        if (!game_build_.empty() && mods_.State().CheckedGameBuild().empty()) AcceptGameBuild();
        else if (!game_build_.empty() && mods_.State().CheckedGameBuild() != game_build_) game_update_ = GameUpdate::Found;
        else game_update_ = GameUpdate::None;
    } catch (const std::exception& error) {
        std::error_code missing;
        if (!std::filesystem::is_regular_file(game_loading_, missing)) game_error_ = MissingGameMessage(game_loading_);
        else game_error_ = Narrow(game_loading_.wstring()) + ": " + error.what();
        ReportError(game_error_);
        multi_.SetGame(nullptr);
    }
    // The comparison and textures depend on the game, so reload the model being inspected.
    if (!active_mod_.empty()) StartInspect(active_mod_, selected_);
    SyncSources();
}

void Workspace::AcceptGameBuild() {
    if (game_build_.empty()) return;
    mods_.State().SetCheckedGameBuild(game_build_);
    mods_.Save();
}

void Workspace::PollGameUpdate() {
    switch (game_update_) {
    case GameUpdate::Found:
        // Nothing to check in an empty library.
        if (!library_.Restoring() && library_.CanPersist() && mod_paths_.empty()) {
            AcceptGameBuild();
            game_update_ = GameUpdate::None;
        }
        break;
    case GameUpdate::Checking:
        if (repair_.Busy()) break;
        if (!repair_.Completed()) {
            game_update_ = GameUpdate::Found;
            break;
        }
        update_fixable_ = repair_.RepairableCount();
        game_update_ = GameUpdate::Checked;
        break;
    case GameUpdate::Fixing:
        if (repair_.Busy()) break;
        if (!repair_.Completed()) {
            update_fixable_ = repair_.RepairableCount();
            game_update_ = GameUpdate::Checked;
            break;
        }
        AcceptGameBuild();
        game_update_ = GameUpdate::Fixed;
        break;
    default:
        break;
    }
}

// A banner above My mods while a game update needs a look.
void Workspace::DrawGameUpdate() {
    if (game_update_ == GameUpdate::None) return;
    const auto mods_label = [](size_t count) { return count == 1 ? std::string("1 mod") : std::to_string(count) + " mods"; };
    switch (game_update_) {
    case GameUpdate::Found: {
        ui::BeginBanner(ui::kWarning);
        ImGui::TextWrapped("Deadlock has updated since your mods were last checked. Some skins may need a fix to work again.");
        const bool can = repair_.CanAnalyze() && !FilesBusy();
        ImGui::BeginDisabled(!can);
        if (ImGui::SmallButton("Check my mods")) {
            repair_.Analyze();
            game_update_ = GameUpdate::Checking;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", can ? "Checks every mod against the new game. Nothing changes until you choose to fix them."
                : "Wait for the library to finish its current work.");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Not now")) game_update_ = GameUpdate::None;
        ImGui::SetItemTooltip("Ask again next time Modworks starts.");
        ui::EndBanner();
        break;
    }
    case GameUpdate::Checking:
    case GameUpdate::Fixing:
        ui::BeginBanner(ui::kAccent);
        ImGui::TextWrapped("%s", game_update_ == GameUpdate::Checking ? "Checking your mods against the new game..."
            : ("Fixing " + mods_label(update_fixable_) + "...").c_str());
        ImGui::TextDisabled("%s", repair_.Status().c_str());
        if (ImGui::SmallButton("Cancel##update")) repair_.Cancel();
        ui::EndBanner();
        break;
    case GameUpdate::Checked: {
        if (update_fixable_ == 0) {
            ui::BeginBanner(ui::kGood);
            ImGui::TextWrapped("Nothing needs a fix after the update. Repairs has the full results.");
        } else {
            ui::BeginBanner(ui::kWarning);
            ImGui::TextWrapped("%s need a fix after the update. Fixing saves a new version of each and keeps the original.",
                mods_label(update_fixable_).c_str());
            const std::string fix = "Fix " + mods_label(update_fixable_);
            ImGui::BeginDisabled(!repair_.CanRepair() || FilesBusy());
            if (ImGui::SmallButton(fix.c_str())) {
                game_update_ = GameUpdate::Fixing;
                repair_.Repair([&] { ReleaseArchives(); });
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("Review in Repairs")) ShowPage(Page::Repair);
        ImGui::SameLine();
        if (ImGui::SmallButton(update_fixable_ == 0 ? "Dismiss##update" : "Not now")) {
            if (update_fixable_ == 0) AcceptGameBuild();
            game_update_ = GameUpdate::None;
        }
        ui::EndBanner();
        break;
    }
    case GameUpdate::Fixed:
        ui::BeginBanner(ui::kGood);
        ImGui::TextWrapped("%s", mods_.HasPendingChanges(ModEntries())
            ? "Mods fixed. Apply to game to put the fixed versions in Deadlock."
            : "Mods fixed. Repairs has the results.");
        if (ImGui::SmallButton("Dismiss##update")) game_update_ = GameUpdate::None;
        ui::EndBanner();
        break;
    default:
        break;
    }
}

void Workspace::ChooseGame() {
    const auto paths = AskForFiles(window_, L"Game archive (pak01_dir.vpk)\0pak01_dir.vpk\0VPK archives (*.vpk)\0*.vpk\0",
        L"Select game\\citadel\\pak01_dir.vpk");
    if (!paths.empty()) OpenGame(paths.front());
}

void Workspace::ConnectManual(bool browse) {
    if (!browse && manual_game_[0] != '\0') {
        OpenGame(std::filesystem::path(Widen(std::string(manual_game_))));
        return;
    }
    ChooseGame();
}

void Workspace::StartInspect(const std::filesystem::path& archive, const std::string& model) {
    active_mod_ = archive;
    mod_name_ = builder_.IsBaseArchive(archive) ? std::string("Base game") : Narrow(archive.filename().wstring());
    selected_ = model;
    standalone_model_ = false;
    panel_.ShowError("", "");
    compare_.ShowError("", "");
    inspector_.Start([archives = archives_, archive, model, game = game_](CancellationToken cancellation) {
        return InspectArchiveModel(*archives, archive, model, game, cancellation);
    });
}

void Workspace::OpenModelFile(const std::filesystem::path& path) {
    CloseInspection();
    standalone_model_ = true;
    mod_name_ = Narrow(path.filename().wstring());
    inspector_.Start([path](CancellationToken) { return InspectModelFile(path); });
}

// Imports the archive into local storage first when storage is available, like any added mod.
void Workspace::OpenArchiveModel(const std::filesystem::path& archive, const std::string& model) {
    if (library_.Store() && !FilesBusy()) {
        inspect_after_import_.emplace(archive, model);
        ModelLibrary incoming;
        incoming.Sources.push_back(archive);
        StartImport(incoming, false);
    } else {
        StartInspect(archive, model);
    }
}

void Workspace::PollInspect() {
    ZoneScopedN("Poll inspect");
    std::optional<Inspection> result;
    try {
        result = inspector_.Take();
    } catch (const std::exception& error) {
        panel_.ShowError(selected_, error.what());
        compare_.ShowError(selected_, error.what());
        return;
    }
    if (!result) return;
    mod_ = result->Mod;
    if (!result->Model.empty()) selected_ = result->Model;
    if (result->Preview) {
        panel_.Load(*result->Preview, result->Title, mod_ ? ModThenGame(mod_, result->Game) : ResourceReader{},
            result->Animation ? &*result->Animation : nullptr, result->Clips);
    } else {
        panel_.ShowError(result->Title, result->PreviewError);
    }
    if (result->Comparison) {
        compare_.Load(std::move(*result->Comparison), VersionReaderFor(mod_, result->Game), result->Clips);
    } else {
        compare_.ShowError(result->Title, result->ComparisonError);
    }
    if (pending_clip_) {
        panel_.PlayClip(*pending_clip_);
        compare_.PlayClip(*pending_clip_);
        pending_clip_.reset();
    }
}

void Workspace::CloseInspection() {
    inspector_.Cancel();
    mod_.reset();
    selected_.clear();
    active_mod_.clear();
    standalone_model_ = false;
    panel_.ShowError("", "");
    compare_.ShowError("", "");
}

// Releases every read of the mod archives before a repair replaces them.
void Workspace::ReleaseArchives() {
    inspector_.Cancel(true);
    builder_.StopLoading();
    mod_.reset();
    panel_.ShowError("", "");
    compare_.ShowError("", "");
    multi_.Release();
    multi_suspended_ = true;
}

void Workspace::SyncSources() {
    std::vector<std::string> paths;
    for (const auto& path : mod_paths_) paths.push_back(Narrow(path.wstring()));
    repair_.SetSources(game_ ? Narrow(GamePath().parent_path().wstring()) : "", std::move(paths));
}

// ---------- sources ----------

void Workspace::AddPaths(const std::vector<std::filesystem::path>& paths) {
    if (paths.empty()) return;
    ModelLibrary incoming;
    for (const auto& path : paths) {
        if (HasExtension(path, ".vpk")) incoming.Sources.push_back(path);
    }
    if (!incoming.Sources.empty()) StartImport(incoming, false);
    if (HasExtension(paths.front(), ".vmdl_c")) {
        OpenModelFile(paths.front());
        ShowPage(Page::Inspect);
    } else {
        ShowPage(Page::Gallery);
    }
    SyncSources();
}

void Workspace::AddMods() {
    AddPaths(AskForFiles(window_, L"Mod VPKs and models (*.vpk;*.vmdl_c)\0*.vpk;*.vmdl_c\0All files\0*.*\0",
        L"Add reference mods (Ctrl / Shift to select multiple)", true));
}

// Adds every mod VPK directly in a folder, such as the game's addons folder.
void Workspace::AddFolder() {
    const auto folder = AskForFolder(window_, L"Add every mod VPK in a folder");
    if (!folder) return;
    try {
        const auto mods = FolderImportCandidates(*folder);
        if (mods.empty()) {
            SetStoreStatus("No mod VPKs found in " + Narrow(folder->wstring()) + ".", true);
            return;
        }
        AddPaths(mods);
    } catch (const std::exception& error) {
        SetStoreStatus(error.what(), true);
    }
}

void Workspace::AddLibrary() {
    builder_.AddLibrary(window_, [&](const ModelLibrary& library) { StartImport(library, true); },
        [&](const std::filesystem::path& other) { StartMerge(other); });
    ShowPage(Page::Gallery);
}

void Workspace::RemoveSource(const std::filesystem::path& path) {
    std::erase(mod_paths_, path);
    if (active_mod_ == path) CloseInspection();
    SyncSources();
    builder_.Restore(builder_.Snapshot(mod_paths_, configured_game_));
}

// ---------- menus and bars ----------

void Workspace::ShowPage(Page page) {
    if (page != page_) previous_page_ = page_;
    page_ = page;
}

void Workspace::FindSkins() {
    if (page_ != Page::Gallery && page_ != Page::Pack) ShowPage(Page::Gallery);
    builder_.FocusSearch();
}

void Workspace::HandleShortcuts() {
    constexpr ImGuiInputFlags global = ImGuiInputFlags_RouteGlobal;
    if (!FilesBusy() && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, global)) AddMods();
    if (!FilesBusy() && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_O, global)) AddFolder();
    if (!FilesBusy() && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_G, global)) ChooseGame();
    if (!PackBusy() && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, global)) {
        ShowPage(Page::Pack);
        builder_.BuildPack(window_, GamePath());
    }
    if (!PackBusy() && ImGui::Shortcut(ImGuiKey_F5, global)) builder_.Reload(mod_paths_);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F, global)) FindSkins();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_E, global)) {
        show_explorer_ = !show_explorer_;
        if (show_explorer_) {
            focus_explorer_ = true;
            if (!ExplorerOnPage()) ShowPage(Page::Gallery);
        }
    }
    if (ImGui::Shortcut(ImGuiMod_Alt | ImGuiKey_LeftArrow, global)) ShowPage(previous_page_);
    if (ImGui::Shortcut(ImGuiKey_F1, global)) open_shortcuts_ = true;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Comma, global)) ShowPage(Page::Settings);
    static_assert(std::size(kPages) <= 9);
    for (size_t i = 0; i < std::size(kPages); ++i) {
        const auto key = static_cast<ImGuiKey>(ImGuiKey_1 + static_cast<int>(i));
        if (ImGui::Shortcut(ImGuiMod_Ctrl | key, global)) ShowPage(kPages[i].Value);
    }
}

void Workspace::DrawMenuBar() {
    ZoneScopedN("Menu");
    if (!ImGui::BeginMainMenuBar()) return;
    HandleShortcuts();
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Add mods...", "Ctrl+O", false, !FilesBusy())) AddMods();
        if (ImGui::MenuItem("Add folder...", "Ctrl+Shift+O", false, !FilesBusy())) AddFolder();
        ImGui::SetItemTooltip("Adds every mod VPK in a folder, such as Deadlock's addons folder. Chunk files and the game archive are skipped.");
        if (ImGui::MenuItem("Discover mods on GameBanana", "Ctrl+2")) ShowPage(Page::Browse);
        ImGui::Separator();
        if (ImGui::MenuItem("My mods", "Ctrl+1")) ShowPage(Page::Mods);
        ImGui::SetItemTooltip("Turn mods on or off, set their load order and apply them to Deadlock's addons folder.");
        ImGui::Separator();
        if (ImGui::MenuItem("Import a shared library...", nullptr, false, !FilesBusy())) AddLibrary();
        ImGui::SetItemTooltip("Adds the mods and pack selections of a shared .db library to yours. Nothing in your library is replaced.");
        if (ImGui::MenuItem("Export library to share...", nullptr, false, !FilesBusy() && builder_.CanShare())) builder_.ShareLibrary(window_);
        ImGui::SetItemTooltip("Saves your mods, pack selections and thumbnails as one .db file others can add.");
        ImGui::Separator();
        if (ImGui::MenuItem("Locate Deadlock...", "Ctrl+G", false, !FilesBusy())) ChooseGame();
        ImGui::SetItemTooltip("Choose game\\citadel\\pak01_dir.vpk if Modworks did not find Deadlock through Steam.");
        ImGui::Separator();
        if (ImGui::MenuItem("Build VPK file...", "Ctrl+B", false, !PackBusy() && builder_.SelectedCount() > 0)) {
            ShowPage(Page::Pack);
            builder_.BuildPack(window_, GamePath());
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Open library folder", nullptr, false, library_.Store() != nullptr)) ui::RevealInExplorer(library_.Store()->ContentDirectory());
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4")) PostMessageW(window_, WM_CLOSE, 0, 0);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Find skins", "Ctrl+F")) FindSkins();
        if (ImGui::MenuItem("Show every source", nullptr, false, !builder_.SourceFilter().empty())) builder_.SetSourceFilter({});
        ImGui::Separator();
        if (ImGui::MenuItem("Clear pack", nullptr, false, !PackBusy() && builder_.SelectedCount() > 0)) builder_.ClearPack();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        for (size_t i = 0; i < std::size(kPages); ++i) {
            if (i == kPlayPages) ImGui::SeparatorText("Tools");
            if (ImGui::MenuItem(kPages[i].Name, kPages[i].Keys, page_ == kPages[i].Value)) ShowPage(kPages[i].Value);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Settings", "Ctrl+,", page_ == Page::Settings)) ShowPage(Page::Settings);
        if (ImGui::MenuItem("Back", "Alt+Left")) ShowPage(previous_page_);
        ImGui::Separator();
        if (ImGui::MenuItem("Mod files panel", "Ctrl+Shift+E", &show_explorer_) && show_explorer_) {
            focus_explorer_ = true;
            if (!ExplorerOnPage()) ShowPage(Page::Gallery);
        }
        ImGui::SetItemTooltip("The list of mod files beside Skins, the pack builder and the tools. My mods, Discover and Settings do not show it.");
        if (ImGui::MenuItem("Workspace")) focus_workspace_ = true;
        ImGui::MenuItem("Status bar", nullptr, &show_status_bar_);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) reset_layout_ = true;
        ImGui::SetItemTooltip("Puts the mod files panel back beside the page. Mods, the catalog and thumbnails stay.");
        if (ImGui::MenuItem("Refresh library", "F5", false, !PackBusy())) builder_.Reload(mod_paths_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        const bool repair_allowed = !builder_.Busy() && !library_.Busy() && !mods_.Busy();
        if (ImGui::MenuItem("Check mods for problems", nullptr, false, repair_allowed && repair_.CanAnalyze())) {
            ShowPage(Page::Repair);
            repair_.Analyze();
        }
        if (ImGui::MenuItem("Fix supported problems", nullptr, false, repair_allowed && repair_.CanRepair())) {
            ShowPage(Page::Repair);
            repair_.Repair([&] { ReleaseArchives(); });
        }
        if (ImGui::MenuItem("Cancel repair", nullptr, false, repair_.Busy())) repair_.Cancel();
        ImGui::Separator();
        if (ImGui::MenuItem("Cancel import", nullptr, false, library_.Busy())) library_.Cancel();
        if (ImGui::MenuItem("Cancel VPK build", nullptr, false, builder_.Busy())) builder_.CancelBuild();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Keyboard shortcuts", "F1")) open_shortcuts_ = true;
        if (ImGui::MenuItem("Quick start")) open_quick_start_ = true;
        if (ImGui::MenuItem("Open logs")) OpenLogs();
        if (ImGui::MenuItem("Copy diagnostic details")) ui::CopyToClipboard(DiagnosticText(SR_PRODUCT_TITLE, SR_GIT_COMMIT, data_root_, last_error_));
        ImGui::Separator();
        if (ImGui::MenuItem("About")) open_about_ = true;
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void Workspace::OpenLogs() {
    const std::filesystem::path log = DataPathsFor(data_root_).LogFile;
    std::error_code error;
    if (std::filesystem::is_regular_file(log, error)) {
        ui::RevealInExplorer(log);
        return;
    }
    SetStoreStatus("No log file yet. Errors are written to " + Narrow(log.wstring()) + ".", false);
}

// Context menu items for the game connection, shared by the explorer and the status bar.
void Workspace::GameMenuItems() {
    if (ImGui::MenuItem("Locate Deadlock...", "Ctrl+G", false, !FilesBusy())) ChooseGame();
    if (ImGui::MenuItem("Reconnect", nullptr, false, !FilesBusy() && !configured_game_.empty() && !game_loader_.Busy())) OpenGame(configured_game_);
    ImGui::Separator();
    ui::PathMenuItems(game_ ? GamePath() : game_loading_);
}

void Workspace::GameState(ImVec4& color, const char*& label) const {
    if (game_) { color = ui::kGood; label = "Deadlock found"; }
    else if (game_loader_.Busy()) { color = ui::kWarning; label = "Looking for Deadlock..."; }
    else if (!game_error_.empty()) { color = ui::kError; label = "Deadlock not found"; }
    else { color = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled); label = "Deadlock not found"; }
}

// The most important message for the status bar, with a way to dismiss it.
std::string* Workspace::StatusMessage(ImVec4& color) {
    if (!startup_notice_.empty()) { color = ui::kError; return &startup_notice_; }
    if (!library_.SaveError().empty()) { color = ui::kError; return &library_.SaveError(); }
    if (!game_error_.empty()) { color = ui::kError; return &game_error_; }
    if (!store_status_.empty() && !library_.Busy()) {
        color = store_status_is_error_ ? ui::kError : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        return &store_status_;
    }
    return nullptr;
}

void Workspace::DrawStatusBar() {
    ZoneScopedN("Status bar");
    if (!show_status_bar_) return;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
    if (ImGui::BeginViewportSideBar("##statusbar", ImGui::GetMainViewport(), ImGuiDir_Down, ImGui::GetFrameHeight(), flags)
        && ImGui::BeginMenuBar()) {
        ImVec4 color;
        const char* label = nullptr;
        GameState(color, label);
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        char game_menu[96];
        std::snprintf(game_menu, sizeof(game_menu), "\xE2\x97\x8F %s###game", label);
        const bool open = ImGui::BeginMenu(game_menu);
        ImGui::PopStyleColor();
        if (!open) {
            ImGui::SetItemTooltip("%s", game_ ? game_name_.c_str()
                : game_loading_.empty() ? "Locate Deadlock (Ctrl+G)" : Narrow(game_loading_.wstring()).c_str());
        }
        if (open) {
            GameMenuItems();
            ImGui::EndMenu();
        }
        ImGui::Separator();

        // Background work, with its progress and a way to stop it.
        if (library_.Busy()) {
            const LibraryJobProgress& progress = library_.Progress();
            const size_t done = progress.Done.load();
            const size_t total = progress.Total.load();
            const auto job = library_.CurrentJob();
            const bool downloading = job == LibrarySession::Job::Downloading;
            ImGui::TextUnformatted(job == LibrarySession::Job::Opening ? "Opening library" : downloading ? "Downloading"
                : job == LibrarySession::Job::Building ? "Building mod" : "Importing");
            const uint64_t received = progress.Bytes.load(), size = progress.ByteTotal.load();
            if (downloading && total == 0) {
                ImGui::ProgressBar(size ? static_cast<float>(received) / static_cast<float>(size) : 0.f, {90, ImGui::GetTextLineHeight() * .55f}, "");
                ImGui::Text("%.1f / %.1f MB", static_cast<double>(received) / 1048576.0, static_cast<double>(size) / 1048576.0);
                ImGui::SetItemTooltip("%s", library_.JobName().c_str());
            } else if (total > 0) {
                ImGui::ProgressBar(static_cast<float>(done) / static_cast<float>(total), {90, ImGui::GetTextLineHeight() * .55f}, "");
                ImGui::Text("%zu / %zu", done, total);
            }
            if (ImGui::SmallButton("Cancel##import")) library_.Cancel();
            ImGui::Separator();
        }
        if (mods_.Busy()) {
            ImGui::TextColored(ui::kWarning, "Applying mods to the game...");
            ImGui::Separator();
        }
        if (builder_.Busy()) {
            ImGui::TextColored(ui::kWarning, "Working on the library...");
            if (ImGui::SmallButton("Cancel##build")) builder_.CancelBuild();
            ImGui::Separator();
        }
        if (repair_.Busy()) {
            ImGui::TextColored(ui::kWarning, "Repairs:");
            ui::TextEllipsis(repair_.Status(), 260);
            if (ImGui::SmallButton("Cancel##repair")) repair_.Cancel();
            ImGui::Separator();
        }
        if (history_.Busy()) {
            ImGui::TextColored(ui::kWarning, "Working on the version history...");
            ImGui::Separator();
        }
        if (builder_.Scanning()) {
            ImGui::TextDisabled("Reading %zu archives...", mod_paths_.size());
            ImGui::Separator();
        }

        char counts[160];
        std::snprintf(counts, sizeof(counts), "%zu shown  \xC2\xB7  %zu skins  \xC2\xB7  %zu sources  \xC2\xB7  %zu in pack",
            builder_.ShownCount(), builder_.ModelCount(), mod_paths_.size(), builder_.SelectedCount());
        const float counts_width = ImGui::CalcTextSize(counts).x + ImGui::GetStyle().ItemSpacing.x * 2;
        ImVec4 message_color;
        if (std::string* message = StatusMessage(message_color)) {
            const float width = ImGui::GetContentRegionAvail().x - counts_width;
            const auto first_line = message->substr(0, message->find('\n'));
            if (width > 40) {
                ui::TextEllipsis(first_line, width, &message_color);
                if (first_line.size() != message->size()) ImGui::SetItemTooltip("%s", message->c_str());
                if (ImGui::BeginPopupContextItem("message_menu")) {
                    ui::CopyMenuItem("Copy message", *message);
                    if (ImGui::MenuItem("Dismiss")) message->clear();
                    ImGui::EndPopup();
                }
            }
        }
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - counts_width));
        ImGui::TextDisabled("%s", counts);
        ImGui::EndMenuBar();
    }
    ImGui::End();
}

// ---------- explorer ----------

bool Workspace::ExplorerOnPage() const { return page_ != Page::Mods && page_ != Page::Browse && page_ != Page::Settings; }

void Workspace::DrawExplorer() {
    ZoneScopedN("Explorer");
    if (!show_explorer_ || !ExplorerOnPage()) return;
    shell::DockWithoutTab();
    const bool focus = std::exchange(focus_explorer_, false);
    if (focus) ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::kPanel);
    ImGui::Begin(shell::kExplorer, &show_explorer_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
    const auto& style = ImGui::GetStyle();
    const auto right_aligned = [&](const char* labels) {
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(labels).x - style.FramePadding.x * 4 - style.ItemSpacing.x);
    };

    ui::SectionLabel("MOD FILES");
    ImGui::SameLine();
    ImGui::TextDisabled("%zu", mod_paths_.size());
    right_aligned("AddFolder");
    if (ui::ToolButton("Add", "Add mod VPKs or models (Ctrl+O). You can also drop files on the window.", !FilesBusy())) AddMods();
    ImGui::SameLine();
    if (ui::ToolButton("Folder", "Add every mod VPK in a folder (Ctrl+Shift+O)", !FilesBusy())) AddFolder();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##sourcefilter", "Filter mod files", source_filter_, sizeof(source_filter_));

    ImGui::BeginChild("sources", {0, 0}, ImGuiChildFlags_Borders);
    DrawSourceList();
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleColor();
    if (focus) shell::BringOnScreen(shell::kExplorer, {40.f, 40.f});
}

void Workspace::DrawSourceList() {
    const ImGuiTextFilter filter(source_filter_);
    const auto& active = builder_.SourceFilter();
    const bool library_page = page_ == Page::Gallery || page_ == Page::Pack;
    const auto count_column = [&](size_t count) {
        char text[32];
        std::snprintf(text, sizeof(text), "%zu", count);
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(text).x);
        ImGui::TextDisabled("%s", text);
    };
    if (!mod_paths_.empty() && !filter.IsActive()) {
        if (ImGui::Selectable("All mod files", active.empty() && library_page)) {
            builder_.SetSourceFilter({});
            if (page_ != Page::Pack) ShowPage(Page::Gallery);
        }
        count_column(builder_.ListedCount());
    }
    // The connected game's own hero models, listed read-only for reference and comparison.
    if (builder_.BaseModelCount() > 0 && filter.PassFilter("Base game")) {
        const auto& base = builder_.BaseArchive();
        const bool base_active = !active.empty() && builder_.IsBaseArchive(active);
        if (ImGui::Selectable("Base game", base_active && library_page)) {
            builder_.SetSourceFilter(base_active ? std::filesystem::path{} : base);
            ShowPage(Page::Gallery);
        }
        ImGui::SetItemTooltip("%s\nThe installed game's own hero models. Click to browse them; inspect one to compare with your mods.",
            Narrow(base.wstring()).c_str());
        count_column(builder_.BaseModelCount());
        if (!mod_paths_.empty()) ImGui::Separator();
    }
    const LocalModStore* store = library_.Store();
    std::optional<std::filesystem::path> remove;
    for (const auto& path : mod_paths_) {
        const LibraryPanel::SourceInfo* info = builder_.FindSource(path);
        const std::string name = info ? info->Name : Narrow(path.filename().wstring());
        const std::string key = info ? info->Full : Narrow(path.wstring());
        if (!filter.PassFilter(name.c_str())) continue;
        ImGui::PushID(key.c_str());
        if (ImGui::Selectable(name.c_str(), active == path)) {
            builder_.SetSourceFilter(active == path ? std::filesystem::path{} : path);
            if (page_ != Page::Pack) ShowPage(Page::Gallery);
        }
        const size_t skins = info ? info->Skins : builder_.ModelsIn(path);
        const size_t files = info ? info->Files : builder_.PackageFiles(path);
        ImGui::SetItemTooltip("%s", info ? info->Tip.c_str() : key.c_str());
        if (ImGui::BeginPopupContextItem("source_menu")) {
            ImGui::TextDisabled("%s", name.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Show skins")) {
                builder_.SetSourceFilter(path);
                ShowPage(Page::Gallery);
            }
            if (skins > 0) {
                if (ImGui::MenuItem("Include every skin in the pack", nullptr, false, !PackBusy())) builder_.IncludeSource(path, true);
                if (ImGui::MenuItem("Remove every skin from the pack", nullptr, false, !PackBusy())) builder_.IncludeSource(path, false);
            }
            if (files > 0) {
                if (ImGui::MenuItem("Include other files in the pack", nullptr, false, !PackBusy())) builder_.IncludePackage(path, true);
                if (ImGui::MenuItem("Remove other files from the pack", nullptr, false, !PackBusy())) builder_.IncludePackage(path, false);
            }
            ImGui::Separator();
            if (const auto id = store ? store->ContentId(path) : std::nullopt) {
                const bool enabled = mods_.State().Enabled(*id);
                if (ImGui::MenuItem(enabled ? "Disable for the game" : "Enable for the game")) mods_.SetEnabled(*id, !enabled);
                ImGui::SetItemTooltip("Changes the current mod profile. Apply it on My mods (Ctrl+1).");
            }
            if (ImGui::MenuItem("Rename...", nullptr, false, !FilesBusy() && store != nullptr)) {
                rename_target_ = path;
                std::string current = Narrow(path.stem().wstring());
                try {
                    if (const auto stored = store->Name(path)) current = *stored;
                } catch (const std::exception&) {
                }
                std::snprintf(rename_text_, sizeof(rename_text_), "%s", current.c_str());
                open_rename_ = true;
            }
            ui::PathMenuItems(path);
            ImGui::Separator();
            if (ImGui::MenuItem("Remove from library", nullptr, false, !FilesBusy())) remove = path;
            ImGui::EndPopup();
        }
        count_column(skins > 0 ? skins : files);
        ImGui::PopID();
    }
    if (mod_paths_.empty()) {
        ImGui::PushTextWrapPos();
        ImGui::TextDisabled("No mod files yet. Add some here, or find mods on Discover (Ctrl+2).");
        ImGui::PopTextWrapPos();
    }
    if (remove) RemoveSource(*remove);
}

// ---------- workspace pages ----------

// The navigation down the left of the window: the pages players use, the tools, then the game and Settings.
void Workspace::DrawNavRail() {
    ZoneScopedN("Navigation");
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::kWorkspace);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
    const bool visible = ImGui::BeginViewportSideBar("##nav", ImGui::GetMainViewport(), ImGuiDir_Left, ImGui::GetFontSize() * 12.f, flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (!visible) {
        ImGui::End();
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {style.ItemSpacing.x, 2.f});
    ImGui::Dummy({0, 2});
    for (size_t i = 0; i < std::size(kPages); ++i) {
        const auto& page = kPages[i];
        if (i == 0 || i == kPlayPages) {
            if (i == kPlayPages) ImGui::Dummy({0, style.ItemSpacing.y * 2});
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + style.FramePadding.x * 1.5f);
            ui::SectionLabel(i == 0 ? "PLAY" : "TOOLS");
            ImGui::Dummy({0, 2});
        }
        std::string badge;
        const ImVec4* badge_color = nullptr;
        if (page.Value == Page::Mods && mods_.HasPendingChanges(ModEntries())) {
            badge = "Apply";
            badge_color = &ui::kWarning;
        }
        if (page.Value == Page::Pack && builder_.SelectedCount() > 0) badge = std::to_string(builder_.SelectedCount());
        const bool tool = i >= kPlayPages;
        if (ui::NavItem(page.Icon, page.Name, page_ == page.Value, badge.c_str(), tool, badge_color)) ShowPage(page.Value);
        if (page.Value == Page::Inspect && !mod_name_.empty()) ImGui::SetItemTooltip("%s (%s)\nOpen: %s", page.Tooltip, page.Keys, mod_name_.c_str());
        else ImGui::SetItemTooltip("%s (%s)", page.Tooltip, page.Keys);
    }

    // The game and Settings sit at the bottom.
    const float bottom = ImGui::GetFrameHeight() * 1.25f * 2 + ImGui::GetTextLineHeight() * 2 + style.FramePadding.y * 4 + 8.f;
    const float y = ImGui::GetWindowHeight() - style.WindowPadding.y - bottom;
    if (y > ImGui::GetCursorPosY()) ImGui::SetCursorPosY(y);
    DrawGameCard();
    ImGui::Dummy({0, 4});
    if (ui::NavItem(icons::kSettings, "Settings", page_ == Page::Settings)) ShowPage(Page::Settings);
    ImGui::SetItemTooltip("Deadlock's folder, your library, and the mod files panel (Ctrl+,)");
    ImGui::PopStyleVar();
    ImGui::End();
}

// Whether Deadlock was found, as a card. Clicking it locates the game, or offers Reconnect and the path when found.
void Workspace::DrawGameCard() {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImVec4 color;
    const char* label = nullptr;
    GameState(color, label);
    const char* detail = game_ ? (game_name_.find("steamapps") != std::string::npos ? "Steam install" : "Custom folder")
        : game_loader_.Busy() ? "One moment" : "Click to locate it";
    const float height = ImGui::GetTextLineHeight() * 2 + style.FramePadding.y * 4;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const bool pressed = ImGui::InvisibleButton("##gamecard", {width, height});
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, {min.x + width, min.y + height}, ImGui::GetColorU32(hovered ? theme::kRaisedHover : theme::kRaised), style.FrameRounding);
    float x = min.x + style.FramePadding.x * 1.5f;
    const float line = ImGui::GetTextLineHeight();
    const float top = min.y + (height - line * 2) * .5f;
    if (theme::IconsLoaded()) {
        draw->AddText({x, min.y + (height - line) * .5f}, ImGui::GetColorU32(color), icons::kGame);
        x += ImGui::GetFontSize() * 1.25f + style.ItemSpacing.x;
    } else {
        draw->AddText({x, top}, ImGui::GetColorU32(color), "\xE2\x97\x8F");
        x += ImGui::GetFontSize() + style.ItemSpacing.x * .5f;
    }
    draw->AddText({x, top}, ImGui::GetColorU32(theme::kText), label);
    draw->AddText({x, top + line}, ImGui::GetColorU32(theme::kDisabled), detail);
    if (!ImGui::IsPopupOpen("game_card_menu")) {
        ImGui::SetItemTooltip("%s", game_ ? game_name_.c_str() : game_loading_.empty() ? "Locate Deadlock (Ctrl+G)"
            : Narrow(game_loading_.wstring()).c_str());
    }
    if (pressed) {
        if (game_) ImGui::OpenPopup("game_card_menu");
        else if (!FilesBusy() && !game_loader_.Busy()) ChooseGame();
    }
    if (ImGui::BeginPopupContextItem("game_card_menu")) {
        GameMenuItems();
        ImGui::Separator();
        if (ImGui::MenuItem("Settings...", "Ctrl+,")) ShowPage(Page::Settings);
        ImGui::EndPopup();
    }
}

// Deadlock's folder, the library's storage and sharing, and the mod files panel.
void Workspace::DrawSettingsPage() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float column = std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 40.f);
    ui::Heading("Settings");

    ImGui::SeparatorText("Deadlock");
    ImVec4 color;
    const char* label = nullptr;
    GameState(color, label);
    ImGui::TextColored(color, "\xE2\x97\x8F");
    ImGui::SameLine(0, 4);
    ImGui::TextUnformatted(label);
    if (game_) {
        ImGui::SameLine();
        ui::TextEllipsis(game_name_, column - ImGui::GetCursorPosX(), &style.Colors[ImGuiCol_TextDisabled]);
        if (ImGui::BeginPopupContextItem("game_path_menu")) {
            ui::PathMenuItems(GamePath());
            ImGui::EndPopup();
        }
    }
    ImGui::PushTextWrapPos(column);
    ImGui::TextDisabled("%s", GameResourceSummary().c_str());
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(column);
    ImGui::InputTextWithHint("##manualgame", "game\\citadel or pak01_dir.vpk", manual_game_, sizeof(manual_game_));
    ImGui::BeginDisabled(FilesBusy());
    if (ImGui::Button("Use this path")) ConnectManual(false);
    ImGui::SetItemTooltip("Use the folder or pak01_dir.vpk typed above, as entered.");
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) ConnectManual(true);
    ImGui::SetItemTooltip("Browse to game\\citadel\\pak01_dir.vpk (Ctrl+G).");
    ImGui::SameLine();
    ImGui::BeginDisabled(configured_game_.empty() || game_loader_.Busy());
    if (ImGui::Button("Reconnect")) OpenGame(configured_game_);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (!game_error_.empty()) {
        ImGui::PushTextWrapPos(column);
        ImGui::TextColored(ui::kError, "%s", game_error_.c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Library");
    if (const LocalModStore* store = library_.Store()) {
        const auto folder = store->ContentDirectory();
        ImGui::TextUnformatted("Your mods are stored in");
        ImGui::SameLine();
        ui::TextEllipsis(Narrow(folder.wstring()), column - ImGui::GetCursorPosX(), &style.Colors[ImGuiCol_TextDisabled]);
        if (ImGui::BeginPopupContextItem("library_menu")) {
            ui::PathMenuItems(folder);
            ImGui::EndPopup();
        }
        if (ImGui::Button("Open folder")) ui::RevealInExplorer(folder);
        ImGui::SameLine();
        ImGui::BeginDisabled(FilesBusy());
        ImGui::BeginDisabled(!builder_.CanShare());
        if (ImGui::Button("Export library to share...")) builder_.ShareLibrary(window_);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Save your mods, pack selections and thumbnails as one .db file others can add.");
        ImGui::SameLine();
        if (ImGui::Button("Import a shared library...")) AddLibrary();
        ImGui::SetItemTooltip("Add the mods and pack selections of a shared .db library to yours. Nothing in yours is replaced.");
        ImGui::EndDisabled();
    } else {
        ImGui::TextDisabled("Local library storage is unavailable.");
    }
    if (const std::string& save_error = library_.SaveError(); !save_error.empty()) {
        ImGui::PushTextWrapPos(column);
        ImGui::TextColored(ui::kError, "%s", save_error.c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Interface");
    ImGui::Checkbox("Show the mod files panel beside Skins and the tools (Ctrl+Shift+E)", &show_explorer_);
    ImGui::Checkbox("Show the status bar", &show_status_bar_);
    if (ImGui::Button("Reset layout")) reset_layout_ = true;
    ImGui::SetItemTooltip("Puts the mod files panel on the left and the workspace on the right. Mods, the catalog and thumbnails stay.");
}

void Workspace::DrawWorkspace() {
    ZoneScopedN("Workspace");
    shell::DockWithoutTab();
    const bool focus = std::exchange(focus_workspace_, false);
    if (focus) ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::kPanel);
    ImGui::Begin(shell::kWorkspace, nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
    switch (page_) {
    case Page::Settings:
        DrawSettingsPage();
        break;
    case Page::Gallery:
    case Page::Pack:
        builder_.Draw(window_, mod_paths_, GamePath(),
            // Thumbnails wait for a connecting game, which supplies most textures, instead of loading twice.
            repair_.Busy() || library_.Busy(), repair_.Busy() || game_loader_.Busy(), page_ == Page::Pack, [&] { AddMods(); },
            [&](const ModelLibrary& library) { StartImport(library, true); },
            [&](const std::filesystem::path& other) { StartMerge(other); },
            [&](const LibraryModel& model) {
                StartInspect(model.Archive, model.Model);
                ShowPage(Page::Inspect);
            },
            [&](const LibraryModel& model) {
                ShowPage(Page::Repair);
                repair_.AnalyzeModel(Narrow(model.Archive.wstring()), model.Model);
            },
            [&] { ConnectManual(false); }, [&] { open_quick_start_ = true; }, !library_.Restoring());
        break;
    case Page::History:
        history_.Draw(window_, panel_, [game = game_](const std::string& path) { return game ? game->Read(path) : std::nullopt; }, game_clips_,
            compare_, game_directory_, repair_.Busy() || builder_.Busy() || library_.Busy() || mods_.Busy(),
            [&](bool compare) {
                // A stored version is not one of the gallery's archives.
                inspector_.Cancel();
                mod_.reset();
                active_mod_.clear();
                standalone_model_ = true;
                mod_name_ = "Version history";
                focus_compare_ = compare;
                ShowPage(Page::Inspect);
            },
            [&] { ReleaseArchives(); });
        break;
    case Page::Inspect:
        DrawInspectPage();
        break;
    case Page::Multi:
        multi_.Draw(builder_.Models());
        break;
    case Page::Mods:
        DrawGameUpdate();
        mods_.Draw(ModEntries(), configured_game_, LibraryWorkBusy(),
            [&](const std::filesystem::path& source) {
                builder_.SetSourceFilter(source);
                ShowPage(Page::Gallery);
            },
            [&] { ShowPage(Page::Browse); }, [&] { AddMods(); }, [&] { if (!FilesBusy()) ChooseGame(); });
        break;
    case Page::Browse: {
        const auto entries = ModEntries();
        const char* reason = library_.Restoring() ? "Your library is still opening."
            : !library_.CanPersist() ? "Your library could not be opened, so nothing can be added."
            : FilesBusy() ? "Wait for the library to finish its current work." : nullptr;
        banana_.Draw(reason == nullptr, reason,
            [&](int64_t mod) {
                size_t count = 0;
                for (const auto& id : mods_.State().FromGameBanana(mod)) {
                    count += std::any_of(entries.begin(), entries.end(), [&](const ModEntry& entry) { return entry.ContentId == id; }) ? 1 : 0;
                }
                return count;
            },
            [&](const GbMod& mod, const GbFile& file, bool enable) { StartDownload(mod, file, enable); });
        break;
    }
    case Page::Repair:
        // Applying mods copies the stored files a repair would replace.
        ImGui::BeginDisabled(builder_.Busy() || library_.Busy() || mods_.Busy());
        repair_.Draw(window_, [&] { ReleaseArchives(); });
        ImGui::EndDisabled();
        break;
    }
    ImGui::End();
    ImGui::PopStyleColor();
    if (focus) shell::BringOnScreen(shell::kWorkspace, {48.f, 48.f});
}

void Workspace::DrawInspectPage() {
    if (ui::ToolButton("<", "Back to the previous page (Alt+Left)")) ShowPage(previous_page_ == Page::Inspect ? Page::Gallery : previous_page_);
    ImGui::SameLine();
    const std::string location = selected_.empty() ? mod_name_ : mod_name_ + "  /  " + selected_;
    ui::TextEllipsis(location);
    if (ImGui::BeginPopupContextItem("location_menu")) {
        ui::CopyMenuItem("Copy model path", selected_);
        if (!active_mod_.empty()) {
            ui::CopyMenuItem("Copy source path", Narrow(active_mod_.wstring()));
            if (ImGui::MenuItem("Reveal source in Explorer")) ui::RevealInExplorer(active_mod_);
            if (ImGui::MenuItem("Show source in library")) {
                builder_.SetSourceFilter(active_mod_);
                ShowPage(Page::Gallery);
            }
            if (ImGui::MenuItem("Side by side", nullptr, false, CharacterOfModel(selected_).has_value())) {
                multi_.Open({active_mod_, selected_}, builder_.Models());
                ShowPage(Page::Multi);
            }
            ImGui::SetItemTooltip("Show this character's models together, with one shared camera.");
            // The game's own models are what a mod is analyzed against.
            if (!builder_.IsBaseArchive(active_mod_)) {
                if (ImGui::MenuItem("Analyze this model", nullptr, false, game_ && !FilesBusy() && !selected_.empty())) {
                    ShowPage(Page::Repair);
                    repair_.AnalyzeModel(Narrow(active_mod_.wstring()), selected_);
                }
                ImGui::SetItemTooltip("Check this model and its animation skeleton against the installed game.");
            }
        }
        ImGui::EndPopup();
    }
    if (inspector_.Busy()) {
        ImGui::TextDisabled("Loading %s...", selected_.empty() ? mod_name_.c_str() : selected_.c_str());
    } else if (selected_.empty() && !standalone_model_) {
        ImGui::TextDisabled("Double-click a skin in the library, or right-click it and choose Inspect, to view its textures, "
            "animations and skeleton or compare it with the installed game.");
        if (ImGui::SmallButton("Browse skins")) ShowPage(Page::Gallery);
    } else if (ImGui::BeginTabBar("inspection")) {
        if (ImGui::BeginTabItem("Preview")) {
            panel_.Draw();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Compare versions", nullptr, std::exchange(focus_compare_, false) ? ImGuiTabItemFlags_SetSelected : 0)) {
            compare_.Draw();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// ---------- dialogs ----------

void Workspace::DrawDialogs() {
    ZoneScopedN("Dialogs");
    for (auto notice : {builder_.TakeExportNotice(), history_.TakeExportNotice()}) {
        if (notice) {
            pending_export_ = *notice;
            open_export_ = true;
        }
    }
    if (std::exchange(open_shortcuts_, false)) ImGui::OpenPopup(shell::kShortcutsDialog);
    if (std::exchange(open_about_, false)) ImGui::OpenPopup(shell::kAboutDialog);
    if (std::exchange(open_quick_start_, false)) ImGui::OpenPopup(shell::kQuickStartDialog);
    if (std::exchange(open_export_, false)) ImGui::OpenPopup("Pack assembled");
    DrawRenamePopup();
    DrawBuildModPopup();
    shell::DrawShortcutsDialog();
    shell::DrawAboutDialog(SR_PRODUCT_TITLE, SR_GIT_COMMIT);
    shell::DrawQuickStartDialog();
    DrawPackAssembledPopup();
}

void Workspace::DrawRenamePopup() {
    if (std::exchange(open_rename_, false)) ImGui::OpenPopup("Rename mod");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {.5f, .5f});
    ImGui::SetNextWindowSize({ImGui::GetFontSize() * 26, 0}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Rename mod", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    ImGui::TextWrapped("The name is shown wherever this mod appears. Leave it empty to use the name derived from the file it was imported from or the models it contains.");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    const bool entered = ImGui::InputText("##name", rename_text_, sizeof(rename_text_), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::BeginDisabled(FilesBusy());
    if (ImGui::Button("Rename") || (entered && !FilesBusy())) {
        pending_rename_.emplace(rename_target_, rename_text_);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void Workspace::DrawBuildModPopup() {
    if (std::exchange(open_build_mod_, false)) ImGui::OpenPopup("Add to My mods");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {.5f, .5f});
    ImGui::SetNextWindowSize({ImGui::GetFontSize() * 28, 0}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Add to My mods", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    ImGui::TextWrapped("Builds the pack into your library as a mod with this name. Turn it on and off in My mods like any other mod.");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    const bool entered = ImGui::InputTextWithHint("##modname", "Mod name", build_mod_name_, sizeof(build_mod_name_), ImGuiInputTextFlags_EnterReturnsTrue);
    const std::string name = Trimmed(build_mod_name_);
    if (!name.empty() && BuiltModNamed(name)) {
        ImGui::TextColored(ui::kWarning, "Replaces your earlier build named %s.", name.c_str());
        ImGui::TextDisabled("The new build keeps its place in the load order and stays enabled where it was.");
    }
    ImGui::Checkbox("Enable it in the current profile", &build_mod_enable_);
    const bool can_build = !name.empty() && !FilesBusy() && !library_.Restoring();
    ImGui::BeginDisabled(!can_build);
    if (ui::PrimaryButton("Add to My mods", {0, 0}, can_build) || (entered && can_build)) {
        StartBuildMod(name, build_mod_enable_);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void Workspace::DrawPackAssembledPopup() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {.5f, .5f});
    ImGui::SetNextWindowSize({ImGui::GetFontSize() * 32, 0}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Pack assembled", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    if (pending_export_) {
        ImGui::TextWrapped("%s", pending_export_->Summary.c_str());
        ImGui::TextWrapped("%s", kAssemblyDoesNotInstall);
        ImGui::TextWrapped("Install or remove the pack from Deadlock yourself. The quick start has those steps.");
        if (ImGui::Button("Reveal in Explorer")) ui::RevealInExplorer(pending_export_->Output);
        ImGui::SameLine();
        if (ImGui::Button("Open quick start")) open_quick_start_ = true;
    }
    if (ImGui::Button("Close", {-1, 0}) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

}  // namespace skeleton_repair
