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
bool EndsWithIgnoreCase(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && EqualsIgnoreCase(text.substr(text.size() - suffix.size()), suffix);
}
struct RepairJob {
    std::string Archive;
    // Set when only this model, and its paired rig file, should be written.
    std::optional<std::string> Model;
};
std::vector<RepairJob> RepairJobs(const std::vector<ArchiveReport>& reports, const std::unordered_set<std::string>& partial) {
    std::vector<RepairJob> jobs;
    for (const auto& archive : reports) {
        if (!partial.contains(archive.ModPath)) {
            if (archive.Report.CanRepair()) jobs.push_back({archive.ModPath, std::nullopt});
            continue;
        }
        std::vector<std::string> repairable;
        for (const auto& result : archive.Report.Results) {
            if (result.Outcome == ModelOutcome::Repairable) repairable.push_back(result.Model);
        }
        for (const auto& path : repairable) {
            // The model repair also writes the animation skeleton beside it.
            if (EndsWithIgnoreCase(path, ".vnmskel_c")) {
                const std::string model = path.substr(0, path.size() - std::string_view(".vnmskel_c").size()) + ".vmdl_c";
                if (std::any_of(repairable.begin(), repairable.end(), [&](const std::string& other) { return EqualsIgnoreCase(other, model); })) continue;
            }
            jobs.push_back({archive.ModPath, path});
        }
    }
    return jobs;
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
    partial_.clear();
    export_status_.clear();
    progress_ = {};
    status_ = "Ready to analyze. Repairs save a new version in your library and keep the original.";
}

void RepairPanel::AnalyzeModel(std::string archive, std::string model) {
    if (busy_ || game_.empty() || archive.empty() || model.empty()) return;
    Start(false, std::pair{std::move(archive), std::move(model)});
}

void RepairPanel::Start(bool repair, std::optional<std::pair<std::string, std::string>> one) {
    std::vector<RepairJob> jobs;
    bool already_complete = false;
    if (!repair && one) {
        jobs.push_back({one->first, one->second});
        already_complete = !partial_.contains(one->first)
            && std::any_of(reports_.begin(), reports_.end(), [&](const auto& archive) { return archive.ModPath == one->first; });
        if (!already_complete) partial_.insert(one->first);
    } else if (repair && one) {
        jobs.push_back({one->first, one->second});
    } else if (repair) {
        jobs = RepairJobs(reports_, partial_);
        for (const auto& job : jobs) {
            if (!job.Model) partial_.erase(job.Archive);
        }
    } else {
        for (const auto& mod : mods_) jobs.push_back({mod, std::nullopt});
        reports_.clear();
        partial_.clear();
    }
    if (jobs.empty()) return;
    if (worker_.joinable()) worker_.join();
    cancellation_ = CancellationSource{};
    done_ = false;
    busy_ = true;
    repairing_ = repair;
    export_status_.clear();
    const bool one_model = jobs.size() == 1 && jobs.front().Model.has_value();
    {
        std::lock_guard lock(mutex_);
        pending_reports_ = reports_;
        pending_progress_ = {};
        if (repair && one_model) pending_status_ = "Repairing " + *jobs.front().Model + "...";
        else if (repair) pending_status_ = "Repairing supported archives...";
        else if (one_model) pending_status_ = "Analyzing " + *jobs.front().Model + "...";
        else pending_status_ = "Analyzing archives...";
    }
    worker_ = std::thread([this, game = game_, jobs = std::move(jobs), repair, already_complete, one_model, token = cancellation_.Token()] {
        try {
            bool one_repairable = false;
            bool wrote = false;
            for (const auto& job : jobs) {
                token.ThrowIfCancelled();
                const auto progress = [this, archive = job.Archive](const UpdateProgress& value) {
                    std::lock_guard lock(mutex_);
                    pending_progress_ = value;
                    pending_status_ = Narrow(std::filesystem::path(Widen(archive)).filename().wstring()) + ": " + value.Message;
                };
                UpdateReport report;
                try {
                    report = repair ? HeroModelUpdater::Update(game, job.Archive, std::nullopt, true, true, progress, token, false, nullptr, job.Model)
                                    : HeroModelUpdater::Analyze(game, job.Archive, progress, token, job.Model);
                } catch (const OperationCancelled&) {
                    throw;
                } catch (const std::exception& error) {
                    report.Results.push_back({job.Model ? *job.Model : job.Archive, ModelOutcome::Failed, error.what()});
                }
                if (!repair && job.Model) {
                    one_repairable = std::any_of(report.Results.begin(), report.Results.end(), [](const ModelResult& result) {
                        return result.Outcome == ModelOutcome::Repairable;
                    });
                }
                wrote = wrote || report.WroteFile;
                std::lock_guard lock(mutex_);
                const auto existing = std::find_if(pending_reports_.begin(), pending_reports_.end(), [&](const auto& archive) {
                    return archive.ModPath == job.Archive;
                });
                if (existing == pending_reports_.end()) pending_reports_.push_back({job.Archive, std::move(report)});
                else if (job.Model) {
                    if (report.WroteFile) {
                        existing->Report.WroteFile = true;
                        existing->Report.OutputPath = report.OutputPath;
                        existing->Report.BackupPath = report.BackupPath;
                    }
                    for (auto& fresh : report.Results) {
                        const auto row = std::find_if(existing->Report.Results.begin(), existing->Report.Results.end(),
                            [&](const ModelResult& prior) { return EqualsIgnoreCase(prior.Model, fresh.Model); });
                        if (row == existing->Report.Results.end()) existing->Report.Results.push_back(std::move(fresh));
                        else *row = std::move(fresh);
                    }
                } else existing->Report = std::move(report);
            }
            std::lock_guard lock(mutex_);
            if (repair && one_model && wrote) pending_status_ = "Repaired " + *jobs.front().Model + ". The original archive was kept.";
            else if (repair && one_model) pending_status_ = "Repair did not change " + *jobs.front().Model + ". Review the result.";
            else if (repair) pending_status_ = "Repair finished. Review the results and test the mod in Deadlock.";
            else if (one_model && already_complete) pending_status_ = "Checked " + *jobs.front().Model + ".";
            else if (one_model && one_repairable) pending_status_ = "Checked " + *jobs.front().Model + ". Use Repair this model to update it.";
            else if (one_model) pending_status_ = "Checked " + *jobs.front().Model + ". Other models in that archive were not checked.";
            else pending_status_ = "Analysis finished. Supported archives can now be repaired.";
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
    return RepairJobs(reports_, partial_).size();
}

void RepairPanel::Draw(void* owner, const std::function<void()>& before_repair) {
    const auto jobs = RepairJobs(reports_, partial_);
    const bool one_model = jobs.size() == 1 && jobs.front().Model.has_value();
    if (ui::ToolButton("Analyze", "Check every mod in your library against the installed game", CanAnalyze())) Start(false);
    ImGui::SameLine();
    char repair_label[64];
    if (one_model) std::snprintf(repair_label, sizeof(repair_label), "Repair this model###repair");
    else std::snprintf(repair_label, sizeof(repair_label), "Repair %zu supported###repair", jobs.size());
    const char* repair_tip = one_model
        ? "Repair the model you checked, and its animation skeleton. Other models in the archive are left as they are. The original is kept."
        : "Repair the analyzed archives that can be repaired. A model checked on its own is repaired by itself. Each repaired copy is saved as a "
          "new version in your library and the original is kept. Archives that need review are left untouched.";
    if (ui::ToolButton(repair_label, repair_tip, !busy_ && !jobs.empty())) {
        before_repair();
        Start(true);
    }
    ImGui::SameLine();
    if (ui::ToolButton("Cancel", "Stop after the current archive. Completed results are kept.", busy_)) Cancel();
    ui::ToolSeparator();
    if (ui::ToolButton("Save report...", "Save the results as a JSON report", !busy_ && !reports_.empty())) SaveReport(owner);
    ImGui::SameLine();
    ui::HelpMarker("Analyze checks hero models and animation skeletons against your installed game. Right-click a skin and choose Analyze this "
        "model to check one model. When that model is repairable, Repair this model updates it and its animation skeleton and leaves the "
        "other models in the archive alone. The original version is kept. Close Deadlock and wait for Steam updates to finish before repairing.");

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
        ImGui::TextDisabled("Run Analyze to check your library, or right-click a skin and choose Analyze this model. Results appear here as a table.");
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
    std::optional<std::pair<std::string, std::string>> analyze_one;
    std::optional<std::pair<std::string, std::string>> repair_one;
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
                const bool resource = result.Model.find('/') != std::string::npos || result.Model.find('\\') != std::string::npos;
                if (ImGui::MenuItem("Analyze this model", nullptr, false, !busy_ && !game_.empty() && resource)) {
                    analyze_one = {archive.ModPath, result.Model};
                }
                ImGui::SetItemTooltip("Check this model and its animation skeleton again. Other models in the archive are left as they are.");
                if (result.Outcome == ModelOutcome::Repairable && ImGui::MenuItem("Repair this model", nullptr, false, !busy_ && !game_.empty() && resource)) {
                    repair_one = {archive.ModPath, result.Model};
                }
                if (result.Outcome == ModelOutcome::Repairable) {
                    ImGui::SetItemTooltip("Repair this model and its animation skeleton. Other models in the archive are left as they are.");
                }
                ImGui::Separator();
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
    if (repair_one) {
        before_repair();
        Start(true, std::move(*repair_one));
    } else if (analyze_one) AnalyzeModel(std::move(analyze_one->first), std::move(analyze_one->second));
}
}
