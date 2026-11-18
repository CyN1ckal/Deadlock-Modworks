#include "mods_panel.hpp"

#include "strings.hpp"
#include "theme.hpp"
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
    status_detail_.clear();
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
        if (result.Deployed.empty()) text = "Done. No mods are on, so Modworks took its mods out of the game.";
        else text = "Done. Deadlock will load " + std::to_string(result.Deployed.size()) + (result.Deployed.size() == 1 ? " mod" : " mods")
            + " next time it starts.";
        for (const auto& note : result.Notes) text += "\n" + note;
        SetStatus(text, false);
        status_detail_ = std::to_string(result.Copied) + " copied, " + std::to_string(result.Moved) + " moved, "
            + std::to_string(result.Removed) + " removed, " + std::to_string(result.Kept) + " unchanged.";
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
    return check_.Citadel && Deployed() != state_.EnabledInOrder(Ids(entries));
}

void ModsPanel::Apply(const std::vector<ModEntry>& entries, const fs::path& game) {
    if (Busy()) return;
    Refresh(game, true);
    if (!check_.Citadel) { SetStatus("Locate Deadlock first (Ctrl+G), so Modworks knows where its addons folder is.", true); return; }
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

std::vector<std::string> ModsPanel::Deployed() const {
    auto managed = check_.Scan.Managed;
    std::sort(managed.begin(), managed.end(), [](const auto& a, const auto& b) { return a.Slot < b.Slot; });
    std::vector<std::string> ids;
    for (const auto& file : managed) ids.push_back(file.ContentId);
    return ids;
}

void ModsPanel::Draw(const std::vector<ModEntry>& entries, const fs::path& game, bool files_busy,
    const std::function<void(const fs::path&)>& show_in_library, const std::function<void()>& get_mods,
    const std::function<void()>& add_files, const std::function<void()>& locate_game) {
    Poll();
    Refresh(game, false);
    state_.Sync(Ids(entries));
    UpdateConflicts(entries);
    DrawToolbar(entries, files_busy, add_files);
    DrawNotices();

    const ImGuiStyle& style = ImGui::GetStyle();
    const bool show_bar = !entries.empty() || !Deployed().empty() || Busy();
    const float bar = ImGui::GetTextLineHeight() * 2 + 2 + style.WindowPadding.y * 2;
    const float list_height = std::max(ImGui::GetFrameHeight() * 3,
        ImGui::GetContentRegionAvail().y - (show_bar ? bar + style.ItemSpacing.y : 0));
    if (load_order_ && !entries.empty()) {
        const float avail = ImGui::GetContentRegionAvail().x;
        const float side = std::max(ImGui::GetFontSize() * 16, avail * .3f);
        ImGui::BeginChild("modlist", {avail - side - style.ItemSpacing.x, list_height});
        DrawList(entries, show_in_library, get_mods, add_files, files_busy);
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("conflicts", {0, list_height}, ImGuiChildFlags_Borders);
        DrawConflicts(entries);
        ImGui::EndChild();
    } else {
        ImGui::BeginChild("modlist", {0, list_height});
        DrawList(entries, show_in_library, get_mods, add_files, files_busy);
        ImGui::EndChild();
    }
    if (show_bar) DrawApplyBar(entries, game, files_busy, locate_game);
    DrawProfilePopups();
    if (std::exchange(pending_apply_, false)) Apply(entries, game);
}

void ModsPanel::DrawToolbar(const std::vector<ModEntry>& entries, bool files_busy, const std::function<void()>& add_files) {
    DrawProfileMenu();
    ImGui::SameLine();
    const ImGuiStyle& style = ImGui::GetStyle();
    const char* order_label = "Load order";
    const char* add_label = "Add mods...";
    const float buttons = ImGui::CalcTextSize(order_label).x + ImGui::CalcTextSize(add_label).x + style.FramePadding.x * 4
        + style.ItemSpacing.x * 2;
    ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 8, ImGui::GetContentRegionAvail().x - buttons));
    ImGui::InputTextWithHint("##modfilter", "Search my mods", filter_, sizeof(filter_));
    ImGui::SameLine();
    const bool ordering = load_order_;
    if (ordering) {
        ImGui::PushStyleColor(ImGuiCol_Button, theme::kSelection);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::kSelectionHover);
    }
    ImGui::BeginDisabled(entries.empty());
    if (ImGui::Button(order_label)) load_order_ = !load_order_;
    ImGui::EndDisabled();
    if (ordering) ImGui::PopStyleColor(2);
    ImGui::SetItemTooltip(ordering ? "Back to the plain list." : "Choose which mod wins when two change the same file.");
    ImGui::SameLine();
    ImGui::BeginDisabled(files_busy || Busy());
    if (ImGui::Button(add_label)) add_files();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Add mod VPKs you already have (Ctrl+O). You can also drop them on the window.");
}

void ModsPanel::DrawProfileMenu() {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Profile");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    if (ImGui::BeginCombo("##profile", state_.ActiveProfile().c_str())) {
        for (size_t i = 0; i < state_.Profiles().size(); ++i) {
            const auto& profile = state_.Profiles()[i];
            const bool active = profile.Name == state_.ActiveProfile();
            char label[160];
            std::snprintf(label, sizeof(label), "%s  \xC2\xB7  %zu on###profile%zu", profile.Name.c_str(), profile.Enabled.size(), i);
            if (ImGui::Selectable(label, active) && !active) { state_.SwitchProfile(profile.Name); Save(); }
        }
        ImGui::Separator();
        if (ImGui::Selectable("New profile...")) { open_new_profile_ = true; profile_name_[0] = '\0'; copy_profile_ = true; }
        char rename[96], remove[96];
        std::snprintf(rename, sizeof(rename), "Rename \"%s\"...", state_.ActiveProfile().c_str());
        std::snprintf(remove, sizeof(remove), "Delete \"%s\"...", state_.ActiveProfile().c_str());
        if (ImGui::Selectable(rename)) {
            open_rename_profile_ = true;
            std::snprintf(profile_name_, sizeof(profile_name_), "%s", state_.ActiveProfile().c_str());
        }
        if (ImGui::Selectable(remove, false, state_.Profiles().size() > 1 ? 0 : ImGuiSelectableFlags_Disabled)) open_delete_profile_ = true;
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Each profile remembers which mods are on. Profiles share one load order.");
}

void ModsPanel::DrawNotices() {
    if (!status_.empty() && !Busy()) {
        ui::BeginBanner(status_error_ ? ui::kError : ui::kAccent);
        ImGui::TextWrapped("%s", status_.c_str());
        if (!status_detail_.empty()) {
            ui::HelpMarker(status_detail_.c_str());
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("Copy##status")) ui::CopyToClipboard(status_detail_.empty() ? status_ : status_ + " " + status_detail_);
        ImGui::SameLine();
        if (ImGui::SmallButton("Dismiss##status")) { status_.clear(); status_detail_.clear(); }
        ui::EndBanner();
    }
    if (!check_.Citadel) return;
    if (!check_.Error.empty()) {
        ui::BeginBanner(ui::kError);
        ImGui::TextWrapped("Couldn't read Deadlock's mod setup: %s", check_.Error.c_str());
        ui::EndBanner();
    } else if (!check_.MountsAddons) {
        ui::BeginBanner(ui::kWarning);
        ImGui::TextUnformatted("Deadlock isn't set up to load mods yet. Apply to game will add one line to gameinfo.gi.");
        ImGui::SameLine();
        ui::HelpMarker("Deadlock only loads the addons folder when gameinfo.gi lists it. Modworks asks before changing it "
            "and keeps the original as gameinfo.gi.modworks.bak.");
        ui::EndBanner();
    }
}

void ModsPanel::DrawList(const std::vector<ModEntry>& entries, const std::function<void(const fs::path&)>& show_in_library,
    const std::function<void()>& get_mods, const std::function<void()>& add_files, bool files_busy) {
    const auto ordered = state_.Ordered(Ids(entries));
    if (ordered.empty()) {
        ImGui::Spacing();
        ui::Heading("No mods yet");
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40);
        ImGui::TextWrapped("Find mods on GameBanana, or add VPK files you already have. Then turn them on here and choose Apply to game.");
        ImGui::PopTextWrapPos();
        if (ui::PrimaryButton("Discover mods")) get_mods();
        ImGui::SameLine();
        ImGui::BeginDisabled(files_busy || Busy());
        if (ImGui::Button("Add mods...")) add_files();
        ImGui::EndDisabled();
        ImGui::TextDisabled("You can also drop mod VPKs on the window.");
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
    const auto deployed_list = check_.Citadel ? Deployed() : std::vector<std::string>{};
    const std::set<std::string> deployed(deployed_list.begin(), deployed_list.end());

    const ImGuiStyle& style = ImGui::GetStyle();
    const float em = ImGui::GetFontSize();
    const float line = ImGui::GetTextLineHeight();
    const float frame = ImGui::GetFrameHeight();
    const float pad_x = style.FramePadding.x * 1.5f;
    const float row_height = line * 2 + 2 + (style.FramePadding.y + 3) * 2;
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const ImGuiTextFilter filter(filter_);
    std::optional<std::pair<std::string, std::string>> move;  // Moved mod, and the mod it now sits before.
    std::optional<std::pair<std::string, bool>> step;
    std::optional<bool> all;  // Turn every mod on or off.
    int position = 0;
    size_t shown = 0;
    for (size_t row = 0; row < ordered.size(); ++row) {
        const auto& id = ordered[row];
        const auto* entry = Find(entries, id);
        if (!entry) continue;
        const bool enabled = state_.Enabled(id);
        if (enabled) ++position;
        if (!filter.PassFilter(entry->Name.c_str())) continue;
        ++shown;
        ImGui::PushID(id.c_str());
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float mid = start.y + row_height * .5f;

        // The card itself: hover, double-click, right-click and, in the load order view, dragging.
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##row", {width, row_height});
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) SetEnabled(id, !enabled);
        if (load_order_) {
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("MODWORKS_MOD", id.data(), id.size());
                ImGui::Text("Move %s", entry->Name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MODWORKS_MOD"))
                    move.emplace(std::string(static_cast<const char*>(payload->Data), static_cast<size_t>(payload->DataSize)), id);
                ImGui::EndDragDropTarget();
            }
        }
        const auto origin = state_.Origin(id);
        if (ImGui::BeginPopupContextItem("mod_menu")) {
            if (ImGui::MenuItem(enabled ? "Turn off" : "Turn on")) SetEnabled(id, !enabled);
            if (ImGui::MenuItem("Turn all on")) all = true;
            if (ImGui::MenuItem("Turn all off")) all = false;
            ImGui::Separator();
            if (ImGui::MenuItem("Move to top")) move.emplace(id, ordered.front());
            if (ImGui::MenuItem("Move to bottom")) move.emplace(id, std::string());
            ImGui::Separator();
            if (ImGui::MenuItem("Show skins")) show_in_library(entry->Path);
            if (origin && !origin->PageUrl.empty() && ImGui::MenuItem("Open GameBanana page")) OpenUrl(origin->PageUrl);
            if (ImGui::MenuItem("Reveal in Explorer")) ui::RevealInExplorer(entry->Path);
            if (ImGui::BeginMenu("Copy")) {
                ui::CopyMenuItem("Name", entry->Name);
                ui::CopyMenuItem("Path", Narrow(entry->Path.wstring()));
                ui::CopyMenuItem("Content id", id);
                ImGui::EndMenu();
            }
            ImGui::EndPopup();
        }
        // Cards sit on the panel colour, so they use the raised fill; turned-off mods get a fainter one.
        const ImVec4 base = hovered ? theme::kRaisedHover : theme::kRaised;
        const ImVec4 card{base.x, base.y, base.z, enabled ? .7f : .35f};
        draw->AddRectFilled(start, {start.x + width, start.y + row_height}, ImGui::GetColorU32(card), style.FrameRounding);

        float x = start.x + pad_x;
        if (load_order_) {
            // Drag handle: three short bars.
            const ImU32 grip = ImGui::GetColorU32(theme::kDisabled);
            for (int i = -1; i <= 1; ++i)
                draw->AddRectFilled({x, mid + i * em * .25f - 1}, {x + em * .7f, mid + i * em * .25f + 1}, grip);
            x += em * 1.2f;
            char number[16];
            if (enabled) std::snprintf(number, sizeof(number), "%d", position);
            else std::snprintf(number, sizeof(number), "-");
            draw->AddText({x, mid - line * .5f}, ImGui::GetColorU32(enabled ? theme::kText : theme::kDisabled), number);
            x += em * 1.6f;
        }
        ImGui::SetCursorScreenPos({x, mid - frame * .5f});
        bool on = enabled;
        if (ui::Toggle("##on", &on)) SetEnabled(id, on);
        ImGui::SetItemTooltip(enabled ? "On. Click to turn it off." : "Off. Click to turn it on.");
        x = ImGui::GetItemRectMax().x + style.ItemSpacing.x * 1.5f;

        float right = start.x + width - pad_x;
        if (load_order_) {
            right -= frame * 2 + 2;
            ImGui::SetCursorScreenPos({right, mid - frame * .5f});
            ImGui::BeginDisabled(row == 0);
            if (ImGui::ArrowButton("##up", ImGuiDir_Up)) step.emplace(id, true);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("Move up. Higher mods win.");
            ImGui::SameLine(0, 2);
            ImGui::BeginDisabled(row + 1 == ordered.size());
            if (ImGui::ArrowButton("##down", ImGuiDir_Down)) step.emplace(id, false);
            ImGui::EndDisabled();
            right -= style.ItemSpacing.x;
        }

        // Badges, right-aligned. Each has a tooltip that explains it.
        enum class TagKind { Pack, Conflict, Pending };
        struct Tag { std::string Text; ImVec4 Color; TagKind Kind; };
        std::vector<Tag> tags;
        if (origin && origin->Built) tags.push_back({"Your pack", ui::kAccent, TagKind::Pack});
        const auto won = wins.find(id);
        const auto lost = losses.find(id);
        if (enabled && lost != losses.end()) {
            size_t files = 0;
            for (const auto* conflict : lost->second) files += conflict->Files;
            tags.push_back({"Loses " + std::to_string(files) + (files == 1 ? " file" : " files"), ui::kWarning, TagKind::Conflict});
        }
        if (enabled && won != wins.end()) {
            size_t files = 0;
            for (const auto* conflict : won->second) files += conflict->Files;
            tags.push_back({"Wins " + std::to_string(files) + (files == 1 ? " file" : " files"), theme::kSecondary, TagKind::Conflict});
        }
        if (check_.Citadel && enabled != deployed.contains(id))
            tags.push_back({enabled ? "Not in game yet" : "Still in game", theme::kSecondary, TagKind::Pending});
        float tags_width = 0;
        for (const auto& tag : tags) tags_width += ui::BadgeWidth(tag.Text.c_str()) + style.ItemSpacing.x * .5f;
        float badge_x = right - tags_width;
        for (const auto& tag : tags) {
            ImGui::SetCursorScreenPos({badge_x, mid - (line + 2) * .5f});
            ui::Badge(tag.Text.c_str(), tag.Color);
            badge_x = ImGui::GetItemRectMax().x + style.ItemSpacing.x * .5f;
            if (!ImGui::BeginItemTooltip()) continue;
            if (tag.Kind == TagKind::Pack) ImGui::TextUnformatted("Built in Pack builder with Build as mod...\nBuilding again under the same name replaces it.");
            if (tag.Kind == TagKind::Conflict) {
                if (won != wins.end()) for (const auto* conflict : won->second)
                    ImGui::Text("Wins %zu shared %s over %s", conflict->Files, conflict->Files == 1 ? "file" : "files", name_of(conflict->Loser).c_str());
                if (lost != losses.end()) for (const auto* conflict : lost->second)
                    ImGui::TextColored(ui::kWarning, "%s wins %zu shared %s", name_of(conflict->Winner).c_str(), conflict->Files, conflict->Files == 1 ? "file" : "files");
                ImGui::TextDisabled("Mods higher in the load order win.");
            }
            if (tag.Kind == TagKind::Pending) ImGui::TextUnformatted(enabled ? "Choose Apply to game to put it in the game." : "Choose Apply to game to take it out of the game.");
            ImGui::EndTooltip();
        }

        // Name, and where it came from.
        const float text_width = std::max(em * 4, right - tags_width - style.ItemSpacing.x - x);
        std::string from = "Added from a file";
        if (origin && origin->GameBananaMod > 0) from = origin->FileName.empty() ? "GameBanana" : "GameBanana \xC2\xB7 " + origin->FileName;
        else if (origin && origin->Built) from = "Built in Pack builder";
        const ImVec4 name_color = enabled ? theme::kText : theme::kSecondary;
        const ImVec4 from_color = theme::kDisabled;
        ImGui::SetCursorScreenPos({x, mid - line - 1});
        ui::TextEllipsis(entry->Name, text_width, &name_color);
        ImGui::SetCursorScreenPos({x, mid + 1});
        ui::TextEllipsis(from, text_width, &from_color);

        ImGui::SetCursorScreenPos({start.x, start.y + row_height});
        ImGui::Dummy({width, 0});
        ImGui::PopID();
    }
    if (shown == 0) {
        ImGui::TextUnformatted("Nothing matches.");
        ImGui::TextDisabled("Change the search to see more mods.");
    }
    if (ImGui::BeginPopupContextWindow("list_menu", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem("Turn all on")) all = true;
        if (ImGui::MenuItem("Turn all off")) all = false;
        ImGui::EndPopup();
    }
    if (all) {
        for (const auto& id : ordered) state_.SetEnabled(id, *all);
        Save();
    }
    if (move && move->first != move->second) { state_.MoveBefore(move->first, move->second); Save(); }
    if (step) { state_.Step(step->first, step->second, Ids(entries)); Save(); }
}

void ModsPanel::DrawConflicts(const std::vector<ModEntry>& entries) {
    ui::SectionLabel("WHICH MOD WINS");
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("When two mods change the same file, the one higher in the list wins. Drag mods or use the arrows to change it.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    if (conflict_task_.Busy()) { ImGui::TextDisabled("Checking..."); return; }
    if (conflicts_.empty()) { ImGui::TextDisabled("No conflicts between the mods that are on."); return; }
    const auto name_of = [&](const std::string& id) {
        const auto* entry = Find(entries, id);
        return entry ? entry->Name : id.substr(0, 8);
    };
    ImGui::PushTextWrapPos(0);
    for (const auto& conflict : conflicts_) {
        ImGui::Bullet();
        ImGui::TextWrapped("%s wins %zu %s over %s", name_of(conflict.Winner).c_str(), conflict.Files,
            conflict.Files == 1 ? "file" : "files", name_of(conflict.Loser).c_str());
    }
    ImGui::PopTextWrapPos();
}

void ModsPanel::DrawApplyBar(const std::vector<ModEntry>& entries, const fs::path& game, bool files_busy,
    const std::function<void()>& locate_game) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float line = ImGui::GetTextLineHeight();
    const float height = line * 2 + 2 + style.WindowPadding.y * 2;
    const auto deployed = check_.Citadel ? Deployed() : std::vector<std::string>{};
    const auto enabled = state_.EnabledInOrder(Ids(entries));
    const auto changes = DiffDeployed(deployed, enabled);
    const bool pending = check_.Citadel && changes.Count() > 0;
    const auto name_of = [&](const std::string& id) {
        const auto* entry = Find(entries, id);
        return entry ? entry->Name : std::string("a removed mod");
    };
    const auto names = [&](const std::vector<std::string>& ids) {
        std::string text;
        for (size_t i = 0; i < ids.size() && i < 3; ++i) text += (i ? ", " : "") + name_of(ids[i]);
        if (ids.size() > 3) text += " and " + std::to_string(ids.size() - 3) + " more";
        return text;
    };

    // What to say, and the main button.
    std::string headline, detail;
    ImVec4 tone = ui::kGood;
    std::string action = "Apply to game";
    bool action_enabled = !files_busy;
    bool action_primary = false;
    if (Busy()) {
        headline = "Applying...";
        tone = theme::kText;
        action = "Cancel";
        action_enabled = true;
    } else if (!check_.Citadel) {
        headline = "Deadlock wasn't found";
        detail = "Locate it to apply mods. You can still turn them on now.";
        tone = ui::kWarning;
        action = "Locate Deadlock...";
        action_primary = true;
    } else if (pending) {
        headline = changes.Count() == 1 ? "1 change isn't in the game yet"
            : std::to_string(changes.Count()) + " changes aren't in the game yet";
        if (!changes.TurnedOn.empty()) detail += "Turned on " + names(changes.TurnedOn) + ". ";
        if (!changes.TurnedOff.empty()) detail += "Turned off " + names(changes.TurnedOff) + ". ";
        if (changes.Reordered) detail += "Changed the load order.";
        tone = ui::kWarning;
        action_primary = true;
    } else {
        headline = "Up to date";
        detail = enabled.empty() ? "No mods are on, so the game has none from Modworks."
            : enabled.size() == 1 ? "The game has the 1 mod you turned on."
            : "The game has the " + std::to_string(enabled.size()) + " mods you turned on.";
    }
    if (!Busy() && check_.Citadel && check_.Running) {
        if (pending) {
            headline = "Close Deadlock to apply";
            detail = std::to_string(changes.Count()) + (changes.Count() == 1 ? " change is" : " changes are")
                + " waiting. Start Deadlock again afterwards to load them.";
        }
        action = "Close Deadlock to apply";
        action_enabled = false;
        action_primary = false;
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kRaised);
    ImGui::BeginChild("applybar", {0, height}, ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float frame = ImGui::GetFrameHeight();
    const float inner = height - style.WindowPadding.y * 2;
    const float button_y = origin.y + (inner - frame) * .5f;

    // Buttons from the right: the main action, Undo, then the files from other tools.
    const auto button_width = [&](const char* label) { return ImGui::CalcTextSize(label, nullptr, true).x + style.FramePadding.x * 2; };
    const std::string action_label = action + "###apply";
    const float action_width = std::max(button_width(action_label.c_str()), ImGui::GetFontSize() * 9);
    float right = origin.x + avail - action_width;
    ImGui::SetCursorScreenPos({right, button_y});
    bool clicked = false;
    if (action_primary) clicked = ui::PrimaryButton(action_label.c_str(), {action_width, 0}, action_enabled);
    else {
        ImGui::BeginDisabled(!action_enabled);
        clicked = ImGui::Button(action_label.c_str(), {action_width, 0});
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        const char* tip = Busy() ? "Stop applying. The addons folder is left as it was."
            : files_busy ? "Wait for the library to finish its current work."
            : !check_.Citadel ? "Find Deadlock's game folder (Ctrl+G)."
            : check_.Running ? "Deadlock keeps its mod files open while it runs."
            : "Put the mods that are on into Deadlock's addons folder. Modworks only changes files it placed there.";
        ImGui::SetTooltip("%s", tip);
    }
    if (clicked) {
        if (Busy()) deploy_cancel_.Cancel();
        else if (!check_.Citadel) { if (locate_game) locate_game(); }
        else if (!check_.MountsAddons) open_patch_gameinfo_ = true;
        else Apply(entries, game);
    }
    if (pending && !Busy() && !check_.Running) {
        right -= button_width("Undo") + style.ItemSpacing.x;
        ImGui::SetCursorScreenPos({right, button_y});
        if (ImGui::Button("Undo")) { state_.Restore(deployed, Ids(entries)); Save(); }
        ImGui::SetItemTooltip("Go back to the mods that are in the game now.");
    }
    if (check_.Citadel && (!check_.Scan.ForeignFiles.empty() || !check_.Scan.Notes.empty())) {
        char label[64];
        std::snprintf(label, sizeof(label), "Other files (%zu)", check_.Scan.ForeignFiles.size());
        right -= button_width(label) + style.ItemSpacing.x;
        ImGui::SetCursorScreenPos({right, button_y});
        if (ImGui::Button(label)) ImGui::OpenPopup("other_files");
        ImGui::SetItemTooltip("Files in the addons folder from other tools. Modworks won't touch these.");
        if (ImGui::BeginPopup("other_files")) {
            ImGui::TextUnformatted("Files from other tools");
            ImGui::TextDisabled("Installed by hand or by another mod manager. Modworks won't touch these.");
            for (const auto& file : check_.Scan.ForeignFiles) ImGui::BulletText("%s", file.c_str());
            for (const auto& note : check_.Scan.Notes) ImGui::TextColored(ui::kWarning, "%s", note.c_str());
            if (ImGui::SmallButton("Open addons folder")) ui::RevealInExplorer(*check_.Citadel / L"addons");
            ImGui::EndPopup();
        }
    }

    // State on the left: a coloured headline, and a muted line under it.
    const float text_width = std::max(ImGui::GetFontSize() * 6, right - style.ItemSpacing.x * 2 - origin.x);
    const float text_y = origin.y + (inner - (line * 2 + 2)) * .5f;
    ImGui::SetCursorScreenPos({origin.x, text_y});
    const std::string title = Busy() ? headline : "\xE2\x97\x8F " + headline;
    ui::TextEllipsis(title, text_width, &tone);
    ImGui::SetCursorScreenPos({origin.x, text_y + line + 2});
    if (Busy()) {
        const uint64_t done = deploy_done_.load(), total = deploy_total_.load();
        ImGui::ProgressBar(total ? static_cast<float>(done) / static_cast<float>(total) : 0.f,
            {std::min(text_width, ImGui::GetFontSize() * 18), line}, total ? (Megabytes(done) + " / " + Megabytes(total)).c_str() : "");
    } else {
        const ImVec4 muted = theme::kSecondary;
        ui::TextEllipsis(detail, text_width, &muted);
    }
    ImGui::EndChild();
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
        ImGui::TextWrapped("Deadlock's gameinfo.gi does not load citadel/addons safely: either the line is missing, so no mod "
            "placed there loads, or it comes first without Mod and Write lines, so the game looks for your keybinds and settings in addons.");
        ImGui::TextWrapped("Modworks can add the addons line and, when needed, Mod citadel and Write citadel after it. The original "
            "is kept as gameinfo.gi.modworks.bak. A game update may replace the file again; this page will tell you when that happens.");
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
