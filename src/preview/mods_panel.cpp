#include "mods_panel.hpp"

#include "strings.hpp"
#include "ui.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>

namespace skeleton_repair {
namespace {
namespace fs = std::filesystem;

std::vector<std::string> Ids(const std::vector<ModEntry>& entries) {
    std::vector<std::string> ids;
    for (const auto& entry : entries) if (!entry.ContentId.empty()) ids.push_back(entry.ContentId);
    return ids;
}

const ModEntry* Find(const std::vector<ModEntry>& entries, const std::string& id) {
    const auto found = std::find_if(entries.begin(), entries.end(), [&](const ModEntry& entry) { return entry.ContentId == id; });
    return found == entries.end() ? nullptr : &*found;
}

std::string Megabytes(uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

void OpenUrl(const std::string& url) {
    if (!url.starts_with("https://")) return;
    ShellExecuteW(nullptr, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
}  // namespace

ModsPanel::ModsPanel(const fs::path& data_root) : file_(data_root / L"mods.json") {
    try {
        state_ = ModState::Load(file_);
    } catch (const std::exception& error) {
        // Never overwrite a file that could not be read; changes this session stay in memory.
        writable_ = false;
        SetStatus(std::string(error.what()) + " Changes on this page will not be saved until it is fixed or removed.", true);
    }
}

ModsPanel::~ModsPanel() {
    deploy_cancel_.Cancel();
    if (deploy_.valid()) deploy_.wait();
}

void ModsPanel::SetStatus(std::string text, bool error) {
    status_ = std::move(text);
    status_error_ = error;
    if (error && !status_.empty()) new_error_ = status_;
}

void ModsPanel::Save() {
    if (!writable_) return;
    try { state_.Save(file_); }
    catch (const std::exception& error) { SetStatus(error.what(), true); }
}

void ModsPanel::RecordAdded(const std::vector<std::string>& ids, const ModOrigin& origin, bool enable) {
    state_.Sync(ids);
    for (const auto& id : ids) {
        state_.SetOrigin(id, origin);
        if (enable) state_.SetEnabled(id, true);
    }
    Save();
}

void ModsPanel::SetEnabled(const std::string& id, bool enabled) {
    state_.SetEnabled(id, enabled);
    Save();
}

void ModsPanel::Poll() {
    try {
        if (const auto conflicts = conflict_task_.Take()) conflicts_ = *conflicts;
    } catch (const std::exception&) {
        conflicts_.clear();
    }
    if (!deploy_.valid() || deploy_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    try {
        const auto result = deploy_.get();
        std::string text;
        if (result.Deployed.empty()) text = "No mods are enabled, so the addons folder holds no Modworks mods now.";
        else text = "Deadlock will load " + std::to_string(result.Deployed.size()) + (result.Deployed.size() == 1 ? " mod" : " mods")
            + " next time it starts.";
        text += " (" + std::to_string(result.Copied) + " copied, " + std::to_string(result.Moved) + " moved, "
            + std::to_string(result.Removed) + " removed, " + std::to_string(result.Kept) + " unchanged.)";
        for (const auto& note : result.Notes) text += "\n" + note;
        SetStatus(text, false);
    } catch (const OperationCancelled&) {
        SetStatus("Applying was cancelled. The addons folder was left as it was.", false);
    } catch (const std::exception& error) {
        SetStatus(std::string("Could not apply the mods: ") + error.what(), true);
    }
    checked_at_ = {};
}

void ModsPanel::Refresh(const fs::path& game, bool force) {
    const auto now = std::chrono::steady_clock::now();
    if (!force && game == checked_game_ && now - checked_at_ < std::chrono::seconds(2)) return;
    if (Busy()) return;
    checked_game_ = game;
    checked_at_ = now;
    check_ = {};
    check_.Running = IsDeadlockRunning();
    check_.Citadel = CitadelFolder(game);
    if (!check_.Citadel) return;
    try {
        std::ifstream in(*check_.Citadel / L"gameinfo.gi", std::ios::binary);
        check_.MountsAddons = GameInfoMountsAddons(std::string((std::istreambuf_iterator<char>(in)), {}));
        check_.Scan = ScanAddons(*check_.Citadel / L"addons");
    } catch (const std::exception& error) {
        check_.Error = error.what();
    }
}

bool ModsPanel::HasPendingChanges(const std::vector<ModEntry>& entries) const {
    if (!check_.Citadel) return false;
    auto managed = check_.Scan.Managed;
    std::sort(managed.begin(), managed.end(), [](const auto& a, const auto& b) { return a.Slot < b.Slot; });
    std::vector<std::string> deployed;
    for (const auto& file : managed) deployed.push_back(file.ContentId);
    return deployed != state_.EnabledInOrder(Ids(entries));
}

void ModsPanel::Apply(const std::vector<ModEntry>& entries, const fs::path& game) {
    if (Busy()) return;
    Refresh(game, true);
    if (!check_.Citadel) { SetStatus("Connect the game first, so Modworks knows where Deadlock's addons folder is.", true); return; }
    if (check_.Running) { SetStatus("Close Deadlock first. It keeps its mod files open while it runs.", true); return; }
    std::vector<DeployMod> mods;
    for (const auto& id : state_.EnabledInOrder(Ids(entries))) {
        const auto* entry = Find(entries, id);
        if (entry) mods.push_back({entry->ContentId, entry->Name, entry->Path});
    }
    deploy_cancel_ = CancellationSource{};
    deploy_done_ = 0;
    deploy_total_ = 0;
    SetStatus("Applying...", false);
    deploy_ = std::async(std::launch::async, [addons = *check_.Citadel / L"addons", mods = std::move(mods), token = deploy_cancel_.Token(),
        done = &deploy_done_, total = &deploy_total_] {
        return DeployAddons(addons, mods, token, [=](uint64_t copied, uint64_t all) { *done = copied; *total = all; });
    });
}

void ModsPanel::UpdateConflicts(const std::vector<ModEntry>& entries) {
    const auto enabled = state_.EnabledInOrder(Ids(entries));
    if (enabled == conflicts_for_) return;
    conflicts_for_ = enabled;
    std::vector<DeployMod> mods;
    for (const auto& id : enabled) if (const auto* entry = Find(entries, id)) mods.push_back({entry->ContentId, entry->Name, entry->Path});
    if (mods.size() < 2) { conflict_task_.Cancel(); conflicts_.clear(); return; }
    conflict_task_.Start([mods = std::move(mods)](CancellationToken token) { return FindModConflicts(mods, token); });
}

void ModsPanel::Draw(const std::vector<ModEntry>& entries, const fs::path& game, bool files_busy,
    const std::function<void(const fs::path&)>& show_in_library, const std::function<void()>& get_mods,
    const std::function<void()>& add_files) {
    Poll();
    Refresh(game, false);
    state_.Sync(Ids(entries));
    UpdateConflicts(entries);
    DrawToolbar(entries, game, files_busy, add_files);
    DrawGameState(entries);
    DrawList(entries, show_in_library, get_mods, add_files, files_busy);
    DrawProfilePopups();
    if (std::exchange(pending_apply_, false)) Apply(entries, game);
}

void ModsPanel::DrawToolbar(const std::vector<ModEntry>& entries, const fs::path& game, bool files_busy,
    const std::function<void()>& add_files) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Profile");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11);
    if (ImGui::BeginCombo("##profile", state_.ActiveProfile().c_str())) {
        for (const auto& profile : state_.Profiles()) {
            const bool active = profile.Name == state_.ActiveProfile();
            char label[160];
            std::snprintf(label, sizeof(label), "%s  (%zu enabled)", profile.Name.c_str(), profile.Enabled.size());
            if (ImGui::Selectable(label, active) && !active) { state_.SwitchProfile(profile.Name); Save(); }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Each profile remembers its own enabled mods. The load order is shared.");
    ImGui::SameLine();
    if (ui::ToolButton("New", "Create a profile")) { open_new_profile_ = true; profile_name_[0] = '\0'; copy_profile_ = true; }
    ImGui::SameLine();
    if (ui::ToolButton("Rename", "Rename this profile")) {
        open_rename_profile_ = true;
        std::snprintf(profile_name_, sizeof(profile_name_), "%s", state_.ActiveProfile().c_str());
    }
    ImGui::SameLine();
    if (ui::ToolButton("Delete", "Delete this profile", state_.Profiles().size() > 1)) open_delete_profile_ = true;
    ui::ToolSeparator();
    const auto ids = Ids(entries);
    if (ui::ToolButton("Enable all", "Enable every mod in this profile", !ids.empty())) {
        for (const auto& id : ids) state_.SetEnabled(id, true);
        Save();
    }
    ImGui::SameLine();
    if (ui::ToolButton("Disable all", "Disable every mod in this profile", !ids.empty())) {
        for (const auto& id : ids) state_.SetEnabled(id, false);
        Save();
    }
    ImGui::SameLine();
    if (ui::ToolButton("Add VPKs...", "Add mod VPKs you already have, such as packs you built, to the library (Ctrl+O). "
        "The VPK builder's Build as mod... adds a pack under a name of your choice.", !files_busy && !Busy())) add_files();
    ui::ToolSeparator();
    ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 8, ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 9));
    ImGui::InputTextWithHint("##modfilter", "Filter mods", filter_, sizeof(filter_));
    ImGui::SameLine();
    const bool can_apply = !Busy() && !files_busy && check_.Citadel && !check_.Running;
    const char* apply_tip = Busy() ? "Applying..." : files_busy ? "Wait for the library to finish its current work."
        : !check_.Citadel ? "Connect the game first." : check_.Running ? "Close Deadlock first." : "Place the enabled mods in Deadlock's addons folder.";
    const bool pending = HasPendingChanges(entries);
    if (pending) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    ImGui::BeginDisabled(!can_apply);
    const bool apply = ImGui::Button("Apply to game", {-1, 0});
    ImGui::EndDisabled();
    if (pending) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", apply_tip);
    if (apply) {
        if (!check_.MountsAddons) open_patch_gameinfo_ = true;
        else Apply(entries, game);
    }
}

void ModsPanel::DrawGameState(const std::vector<ModEntry>& entries) {
    if (Busy()) {
        const uint64_t done = deploy_done_.load(), total = deploy_total_.load();
        ImGui::ProgressBar(total ? static_cast<float>(done) / static_cast<float>(total) : 0.f, {ImGui::GetFontSize() * 14, 0},
            total ? (Megabytes(done) + " / " + Megabytes(total)).c_str() : "Applying...");
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel##deploy")) deploy_cancel_.Cancel();
    } else if (!check_.Citadel) {
        ImGui::TextColored(ui::kWarning, "Connect the game (File > Choose game archive) to apply mods. You can still enable them now.");
    } else {
        if (check_.Running) ImGui::TextColored(ui::kWarning, "Deadlock is running. Close it before applying, and restart it to load changes.");
        else if (HasPendingChanges(entries)) ImGui::TextColored(ui::kWarning, "Changes not applied yet. Choose Apply to game.");
        else ImGui::TextColored(ui::kGood, "\xE2\x97\x8F The addons folder matches the enabled mods.");
        if (!check_.MountsAddons && check_.Error.empty())
            ImGui::TextColored(ui::kWarning, "gameinfo.gi does not load the addons folder, so Deadlock would ignore every mod. Apply offers to fix it.");
        if (!check_.Error.empty()) ImGui::TextColored(ui::kError, "%s", check_.Error.c_str());
    }
    if (!status_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, status_error_ ? ui::kError : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", status_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::BeginPopupContextItem("mods_status")) {
            ui::CopyMenuItem("Copy message", status_);
            if (ImGui::MenuItem("Dismiss")) status_.clear();
            ImGui::EndPopup();
        }
    }
    if (check_.Citadel && (!check_.Scan.ForeignFiles.empty() || !check_.Scan.Notes.empty())) {
        char header[96];
        std::snprintf(header, sizeof(header), "Other files in addons (%zu)###foreign", check_.Scan.ForeignFiles.size());
        if (ImGui::CollapsingHeader(header)) {
            ImGui::TextDisabled("Installed by hand or by another manager. Modworks leaves them alone and skips their numbers.");
            for (const auto& file : check_.Scan.ForeignFiles) ImGui::BulletText("%s", file.c_str());
            for (const auto& note : check_.Scan.Notes) ImGui::TextColored(ui::kWarning, "%s", note.c_str());
            if (ImGui::SmallButton("Open addons folder")) ui::RevealInExplorer(*check_.Citadel / L"addons");
        }
    }
}

void ModsPanel::DrawList(const std::vector<ModEntry>& entries, const std::function<void(const fs::path&)>& show_in_library,
    const std::function<void()>& get_mods, const std::function<void()>& add_files, bool files_busy) {
    const auto ordered = state_.Ordered(Ids(entries));
    if (ordered.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("Your library has no mods yet. Download some from GameBanana, add VPKs you already have, "
            "or build a pack in the VPK builder and choose Build as mod...");
        if (ImGui::Button("Get mods")) get_mods();
        ImGui::SameLine();
        ImGui::BeginDisabled(files_busy || Busy());
        if (ImGui::Button("Add VPKs...")) add_files();
        ImGui::EndDisabled();
        return;
    }
    // Conflict summary per mod.
    std::map<std::string, std::vector<const ModConflict*>> wins, losses;
    for (const auto& conflict : conflicts_) {
        wins[conflict.Winner].push_back(&conflict);
        losses[conflict.Loser].push_back(&conflict);
    }
    const auto name_of = [&](const std::string& id) {
        const auto* entry = Find(entries, id);
        return entry ? entry->Name : id.substr(0, 8);
    };

    const ImGuiTextFilter filter(filter_);
    std::optional<std::pair<std::string, std::string>> move;  // Moved mod, and the mod it now sits before.
    std::optional<std::pair<std::string, bool>> step;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("mods", 5, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Mod", ImGuiTableColumnFlags_WidthStretch, 3);
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch, 1);
    ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    int position = 0;
    for (size_t row = 0; row < ordered.size(); ++row) {
        const auto& id = ordered[row];
        const auto* entry = Find(entries, id);
        if (!entry) continue;
        const bool enabled = state_.Enabled(id);
        if (enabled) ++position;
        if (!filter.PassFilter(entry->Name.c_str())) continue;
        ImGui::PushID(id.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        bool on = enabled;
        if (ImGui::Checkbox("##on", &on)) SetEnabled(id, on);
        ImGui::TableNextColumn();
        if (enabled) ImGui::Text("%d", position);
        else ImGui::TextDisabled("-");

        ImGui::TableNextColumn();
        if (!enabled) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::Selectable(entry->Name.c_str(), false, ImGuiSelectableFlags_AllowOverlap);
        if (!enabled) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) SetEnabled(id, !enabled);
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("MODWORKS_MOD", id.data(), id.size());
            ImGui::Text("Move %s", entry->Name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MODWORKS_MOD")) {
                move.emplace(std::string(static_cast<const char*>(payload->Data), static_cast<size_t>(payload->DataSize)), id);
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SetItemTooltip("%s\nDrag to change the load order. Double-click to turn on or off.", Narrow(entry->Path.wstring()).c_str());
        const auto origin = state_.Origin(id);
        if (ImGui::BeginPopupContextItem("mod_menu")) {
            if (ImGui::MenuItem(enabled ? "Disable" : "Enable")) SetEnabled(id, !enabled);
            if (ImGui::MenuItem("Move to top")) move.emplace(id, ordered.front());
            if (ImGui::MenuItem("Move to bottom")) move.emplace(id, std::string());
            ImGui::Separator();
            if (ImGui::MenuItem("Show skins in library")) show_in_library(entry->Path);
            if (origin && !origin->PageUrl.empty() && ImGui::MenuItem("Open GameBanana page")) OpenUrl(origin->PageUrl);
            ui::PathMenuItems(entry->Path);
            ImGui::EndPopup();
        }
        if (enabled) {
            const auto won = wins.find(id);
            const auto lost = losses.find(id);
            if (won != wins.end() || lost != losses.end()) {
                ImGui::SameLine();
                const bool losing = lost != losses.end();
                ImGui::TextColored(losing ? ui::kWarning : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), losing ? "overridden" : "overrides");
                if (ImGui::BeginItemTooltip()) {
                    if (won != wins.end()) for (const auto* conflict : won->second)
                        ImGui::Text("Wins %zu shared %s over %s", conflict->Files, conflict->Files == 1 ? "file" : "files", name_of(conflict->Loser).c_str());
                    if (lost != losses.end()) for (const auto* conflict : lost->second)
                        ImGui::TextColored(ui::kWarning, "%s wins %zu shared %s", name_of(conflict->Winner).c_str(), conflict->Files, conflict->Files == 1 ? "file" : "files");
                    ImGui::TextDisabled("Mods higher in the list win. Drag to change the order.");
                    ImGui::EndTooltip();
                }
            }
        }

        ImGui::TableNextColumn();
        if (origin && origin->GameBananaMod > 0) {
            ImGui::TextUnformatted("GameBanana");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s\nClick to open the mod page.", origin->FileName.c_str(), origin->PageUrl.c_str());
            if (ImGui::IsItemClicked()) OpenUrl(origin->PageUrl);
        } else if (origin && origin->Built) {
            ImGui::TextUnformatted("Built pack");
            ImGui::SetItemTooltip("Built in the VPK builder with Build as mod... Building again under the same name replaces it.");
        } else {
            ImGui::TextDisabled("Local file");
        }

        ImGui::TableNextColumn();
        ImGui::BeginDisabled(row == 0);
        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) step.emplace(id, true);
        ImGui::EndDisabled();
        ImGui::SameLine(0, 2);
        ImGui::BeginDisabled(row + 1 == ordered.size());
        if (ImGui::ArrowButton("##down", ImGuiDir_Down)) step.emplace(id, false);
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (move && move->first != move->second) { state_.MoveBefore(move->first, move->second); Save(); }
    if (step) { state_.Step(step->first, step->second, Ids(entries)); Save(); }
}

void ModsPanel::DrawProfilePopups() {
    const auto name_popup = [&](bool& open, const char* title, bool create) {
        if (std::exchange(open, false)) ImGui::OpenPopup(title);
        ImGui::SetNextWindowSize({ImGui::GetFontSize() * 22, 0}, ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        const bool entered = ImGui::InputTextWithHint("##profilename", "Profile name", profile_name_, sizeof(profile_name_), ImGuiInputTextFlags_EnterReturnsTrue);
        if (create) ImGui::Checkbox("Start with the mods enabled now", &copy_profile_);
        if (ImGui::Button(create ? "Create" : "Rename") || entered) {
            try {
                if (create) state_.CreateProfile(profile_name_, copy_profile_);
                else state_.RenameProfile(state_.ActiveProfile(), profile_name_);
                Save();
                ImGui::CloseCurrentPopup();
            } catch (const std::exception& error) {
                SetStatus(error.what(), false);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    };
    name_popup(open_new_profile_, "New profile", true);
    name_popup(open_rename_profile_, "Rename profile", false);

    if (std::exchange(open_delete_profile_, false)) ImGui::OpenPopup("Delete profile");
    if (ImGui::BeginPopupModal("Delete profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::Text("Delete the profile %s? Its mods stay in your library.", state_.ActiveProfile().c_str());
        if (ImGui::Button("Delete")) {
            try { state_.DeleteProfile(state_.ActiveProfile()); Save(); } catch (const std::exception& error) { SetStatus(error.what(), true); }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (std::exchange(open_patch_gameinfo_, false)) ImGui::OpenPopup("Load the addons folder?");
    ImGui::SetNextWindowSize({ImGui::GetFontSize() * 30, 0}, ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Load the addons folder?", nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("Deadlock's gameinfo.gi does not list citadel/addons, so the game would not load any mod placed there.");
        ImGui::TextWrapped("Modworks can add that one line. The original is kept as gameinfo.gi.modworks.bak. A game update may "
            "replace the file again; this page will tell you when that happens.");
        // Refresh only updates Running every couple of seconds, and this popup stays open across frames.
        const bool running = IsDeadlockRunning();
        if (running) ImGui::TextColored(ui::kWarning, "Close Deadlock first. It keeps its mod files open while it runs.");
        ImGui::BeginDisabled(running || !check_.Citadel);
        if (ImGui::Button("Add the line and apply")) {
            try {
                if (IsDeadlockRunning()) SetStatus("Close Deadlock first. It keeps its mod files open while it runs.", true);
                else {
                    PatchGameInfo(*check_.Citadel);
                    checked_at_ = {};
                    pending_apply_ = true;
                }
            } catch (const std::exception& error) {
                SetStatus(std::string("Could not change gameinfo.gi: ") + error.what(), true);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Apply without it")) { pending_apply_ = true; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

}  // namespace skeleton_repair
