#include "repair_panel.hpp"
#include "strings.hpp"
#include "ui.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <utility>

namespace skeleton_repair {
namespace {
const char* OutcomeName(ModelOutcome outcome) {
    switch (outcome) {
    case ModelOutcome::Repairable: return "Repairable";
    case ModelOutcome::NeedsReview: return "Needs review";
    case ModelOutcome::Updated: return "Updated";
    case ModelOutcome::Current: return "Current";
    case ModelOutcome::Skipped: return "Skipped";
    case ModelOutcome::Failed: return "Failed";
    }
    return "Failed";
}
ImVec4 OutcomeColor(ModelOutcome outcome) {
    if (outcome == ModelOutcome::Failed) return ui::kError;
    if (outcome == ModelOutcome::NeedsReview || outcome == ModelOutcome::Skipped) return ui::kWarning;
    if (outcome == ModelOutcome::Repairable) return ui::kAccent;
    return ui::kGood;
}
}

RepairPanel::~RepairPanel() {
    Cancel();
    if (worker_.joinable()) worker_.join();
}

void RepairPanel::SetSources(std::string game, std::vector<std::string> mods) {
    if (busy_ || (game_ == game && mods_ == mods)) return;
    game_ = std::move(game);
    mods_ = std::move(mods);
    reports_.clear();
    export_status_.clear();
    progress_ = {};
    status_ = "Ready to analyze. Repairs save a new version in your library and keep the original.";
}

void RepairPanel::Start(bool repair) {
    std::vector<std::string> targets;
    if (repair) {
        for (const auto& archive : reports_) {
            if (archive.Report.CanRepair()) targets.push_back(archive.ModPath);
        }
    } else {
        targets = mods_;
        reports_.clear();
    }
    if (targets.empty()) return;
    if (worker_.joinable()) worker_.join();
    cancellation_ = CancellationSource{};
    done_ = false;
    busy_ = true;
    repairing_ = repair;
    export_status_.clear();
    {
        std::lock_guard lock(mutex_);
        pending_reports_ = reports_;
        pending_progress_ = {};
        pending_status_ = repair ? "Repairing supported archives..." : "Analyzing archives...";
    }
    worker_ = std::thread([this, game = game_, targets = std::move(targets), repair, token = cancellation_.Token()] {
        try {
            for (const auto& mod : targets) {
                token.ThrowIfCancelled();
                const auto progress = [this, mod](const UpdateProgress& value) {
                    std::lock_guard lock(mutex_);
                    pending_progress_ = value;
                    pending_status_ = Narrow(std::filesystem::path(Widen(mod)).filename().wstring()) + ": " + value.Message;
                };
                UpdateReport report;
                try {
                    report = repair ? HeroModelUpdater::Update(game, mod, std::nullopt, true, true, progress, token)
                                    : HeroModelUpdater::Analyze(game, mod, progress, token);
                } catch (const OperationCancelled&) {
                    throw;
                } catch (const std::exception& error) {
                    report.Results.push_back({mod, ModelOutcome::Failed, error.what()});
                }
                std::lock_guard lock(mutex_);
                const auto existing = std::find_if(pending_reports_.begin(), pending_reports_.end(), [&](const auto& a) { return a.ModPath == mod; });
                if (existing == pending_reports_.end()) pending_reports_.push_back({mod, std::move(report)});
                else existing->Report = std::move(report);
            }
            std::lock_guard lock(mutex_);
            pending_status_ = repair ? "Repair finished. Review the results and test the mod in Deadlock." : "Analysis finished. Supported archives can now be repaired.";
        } catch (const OperationCancelled&) {
            std::lock_guard lock(mutex_);
            pending_status_ = "Cancelled. Completed results are shown below.";
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex_);
            pending_status_ = error.what();
        }
        done_ = true;
    });
}

bool RepairPanel::Poll() {
    if (!busy_) return false;
    const bool finished = done_.load();
    {
        std::lock_guard lock(mutex_);
        reports_ = pending_reports_;
        progress_ = pending_progress_;
        status_ = pending_status_;
    }
    if (!finished) return false;
    worker_.join();
    busy_ = false;
    return repairing_;
}

void RepairPanel::SaveReport(void* owner) {
    wchar_t filename[32768] = L"model-report.json";
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = static_cast<HWND>(owner);
    dialog.lpstrFilter = L"JSON report\0*.json\0";
    dialog.lpstrFile = filename;
    dialog.nMaxFile = 32768;
    dialog.lpstrDefExt = L"json";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&dialog)) {
        try { export_status_ = "Saved " + ReportExporter::Write(Narrow(filename), game_, reports_); }
        catch (const std::exception& error) { export_status_ = error.what(); }
    }
}

size_t RepairPanel::RepairableCount() const {
    return static_cast<size_t>(std::count_if(reports_.begin(), reports_.end(), [](const auto& a) { return a.Report.CanRepair(); }));
}

void RepairPanel::Draw(void* owner, const std::function<void()>& before_repair) {
    const size_t count = RepairableCount();
    if (ui::ToolButton("Analyze", "Check every mod in your library against the installed game", CanAnalyze())) Start(false);
    ImGui::SameLine();
    char repair_label[64];
    std::snprintf(repair_label, sizeof(repair_label), "Repair %zu supported###repair", count);
    if (ui::ToolButton(repair_label, "Repair the analyzed archives that can be repaired. Each repaired copy is saved as a new version in your "
        "library and the original is kept. Archives that need review are left untouched.", CanRepair())) {
        before_repair();
        Start(true);
    }
    ImGui::SameLine();
    if (ui::ToolButton("Cancel", "Stop after the current archive. Completed results are kept.", busy_)) Cancel();
    ui::ToolSeparator();
    if (ui::ToolButton("Save report...", "Save the results as a JSON report", !busy_ && !reports_.empty())) SaveReport(owner);
    ImGui::SameLine();
    ui::HelpMarker("Analyze checks hero models and animation skeletons against your installed game. Repair rechecks the files and saves a "
        "new version in your library; the original version is kept. Close Deadlock and wait for Steam updates to finish before repairing.");

    // Outcome totals double as filters.
    size_t totals[6] = {};
    size_t models = 0;
    for (const auto& archive : reports_) {
        for (const auto& result : archive.Report.Results) {
            ++totals[static_cast<size_t>(result.Outcome)];
            ++models;
        }
    }
    ui::ToolSeparator();
    ImGui::TextDisabled("%zu archives, %zu models", mods_.size(), models);
    for (const ModelOutcome outcome : {ModelOutcome::Failed, ModelOutcome::NeedsReview, ModelOutcome::Repairable, ModelOutcome::Updated,
             ModelOutcome::Current, ModelOutcome::Skipped}) {
        const size_t total = totals[static_cast<size_t>(outcome)];
        if (total == 0) continue;
        ImGui::SameLine();
        const bool hidden = (hidden_outcomes_ & (1u << static_cast<unsigned>(outcome))) != 0;
        ImGui::PushStyleColor(ImGuiCol_Text, hidden ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : OutcomeColor(outcome));
        char label[64];
        std::snprintf(label, sizeof(label), "%zu %s", total, OutcomeName(outcome));
        if (ImGui::SmallButton(label)) hidden_outcomes_ ^= 1u << static_cast<unsigned>(outcome);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip(hidden ? "Hidden. Click to show these results." : "Click to hide these results.");
    }

    if (busy_) ImGui::ProgressBar(progress_.Total > 0 ? static_cast<float>(progress_.Completed) / progress_.Total : 0.f, {-1, 3}, "");
    if (game_.empty()) ImGui::TextColored(ui::kWarning, "Connect Deadlock (File > Choose game archive) to enable analysis.");
    ui::TextEllipsis(status_, 0, nullptr);
    if (!export_status_.empty()) ui::TextEllipsis(export_status_);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##resultfilter", "Filter results by archive, model or detail", filter_, sizeof(filter_));

    if (reports_.empty()) {
        ImGui::TextDisabled("Run Analyze to check your library. Results appear here as a table.");
        return;
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_Resizable
        | ImGuiTableFlags_Hideable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("repair_results", 4, flags)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Outcome", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Needs review").x);
    ImGui::TableSetupColumn("Archive", ImGuiTableColumnFlags_WidthStretch, .8f);
    ImGui::TableSetupColumn("Model", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch, 2.f);
    ImGui::TableHeadersRow();
    const ImGuiTextFilter filter(filter_);
    int row = 0;
    for (const auto& archive : reports_) {
        const std::filesystem::path archive_path(Widen(archive.ModPath));
        const std::string archive_name = Narrow(archive_path.filename().wstring());
        for (const auto& result : archive.Report.Results) {
            if ((hidden_outcomes_ & (1u << static_cast<unsigned>(result.Outcome))) != 0) continue;
            if (filter.IsActive() && !filter.PassFilter((archive.ModPath + " " + result.Model + " " + result.Detail).c_str())) continue;
            ImGui::PushID(row++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, OutcomeColor(result.Outcome));
            ImGui::Selectable(OutcomeName(result.Outcome), false, ImGuiSelectableFlags_SpanAllColumns);
            ImGui::PopStyleColor();
            if (ImGui::BeginItemTooltip()) {
                ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.f);
                ImGui::TextUnformatted(archive.ModPath.c_str());
                ImGui::TextDisabled("%s", result.Model.c_str());
                ImGui::Separator();
                ImGui::TextUnformatted(result.Detail.c_str());
                if (result.Warnings) for (const auto& warning : *result.Warnings) ImGui::BulletText("%s", warning.c_str());
                if (archive.Report.BackupPath) ImGui::TextDisabled("Backup: %s", archive.Report.BackupPath->c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            if (ImGui::BeginPopupContextItem("result_menu")) {
                ui::CopyMenuItem("Copy detail", result.Detail);
                ui::CopyMenuItem("Copy model path", result.Model);
                if (result.Warnings) {
                    std::string warnings;
                    for (const auto& warning : *result.Warnings) warnings += warning + "\n";
                    ui::CopyMenuItem("Copy warnings", warnings);
                }
                ImGui::Separator();
                ui::CopyMenuItem("Copy archive path", archive.ModPath);
                if (ImGui::MenuItem("Reveal archive in Explorer")) ui::RevealInExplorer(archive_path);
                if (archive.Report.BackupPath && ImGui::MenuItem("Reveal backup in Explorer")) {
                    ui::RevealInExplorer(std::filesystem::path(Widen(*archive.Report.BackupPath)));
                }
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(archive_name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(result.Model.c_str());
            ImGui::TableNextColumn();
            if (result.Warnings && !result.Warnings->empty()) {
                ImGui::TextColored(ui::kWarning, "[%zu]", result.Warnings->size());
                ImGui::SameLine();
            }
            ImGui::TextUnformatted(result.Detail.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}
}
