#include "repair_panel.hpp"
#include "strings.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <imgui.h>

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
    if (outcome == ModelOutcome::Failed) return {1.f, .42f, .42f, 1.f};
    if (outcome == ModelOutcome::NeedsReview || outcome == ModelOutcome::Skipped) return {1.f, .72f, .38f, 1.f};
    if (outcome == ModelOutcome::Repairable) return {.40f, .78f, 1.f, 1.f};
    return {.47f, .85f, .67f, 1.f};
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
    status_ = "Ready to analyze. Repairs keep an original backup beside each VPK.";
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

void RepairPanel::Draw(void* owner, const std::function<void()>& before_repair) {
    ImGui::SeparatorText("Analyze & repair");
    ImGui::TextWrapped("Check hero models and animation skeletons against your installed game. Repair rechecks the files and keeps a .bak backup. Archives needing review are left untouched.");
    ImGui::TextDisabled("Close Deadlock and wait for Steam updates to finish before repairing.");
    ImGui::Spacing();
    ImGui::BeginDisabled(busy_ || game_.empty() || mods_.empty());
    if (ImGui::Button("Analyze mods", {150, 36})) Start(false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    const auto count = std::count_if(reports_.begin(), reports_.end(), [](const auto& a) { return a.Report.CanRepair(); });
    ImGui::BeginDisabled(busy_ || count == 0);
    if (ImGui::Button("Repair supported mods", {210, 36})) {
        before_repair();
        Start(true);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!busy_);
    if (ImGui::Button("Cancel", {90, 36})) Cancel();
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", status_.c_str());
    if (busy_) ImGui::ProgressBar(progress_.Total > 0 ? static_cast<float>(progress_.Completed) / progress_.Total : 0.f, {-1, 5}, "");
    if (game_.empty()) ImGui::TextColored({1.f, .72f, .38f, 1.f}, "Set the installed game archive in the library to enable analysis.");
    ImGui::Spacing();
    ImGui::Text("%zu selected archives   |   %zu ready to repair", mods_.size(), static_cast<size_t>(count));
    ImGui::SameLine();
    ImGui::BeginDisabled(busy_ || reports_.empty());
    if (ImGui::Button("Save JSON report...")) {
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
    ImGui::EndDisabled();
    if (!export_status_.empty()) ImGui::TextWrapped("%s", export_status_.c_str());
    ImGui::BeginChild("repair_results", {0, 0}, ImGuiChildFlags_Borders);
    for (const auto& archive : reports_) {
        ImGui::PushID(archive.ModPath.c_str());
        if (ImGui::CollapsingHeader(archive.ModPath.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (archive.Report.BackupPath) ImGui::TextWrapped("Backup: %s", archive.Report.BackupPath->c_str());
            for (const auto& result : archive.Report.Results) {
                ImGui::TextColored(OutcomeColor(result.Outcome), "%s", OutcomeName(result.Outcome));
                ImGui::SameLine();
                ImGui::TextWrapped("%s", result.Model.c_str());
                ImGui::Indent();
                ImGui::TextWrapped("%s", result.Detail.c_str());
                if (result.Warnings) for (const auto& warning : *result.Warnings) ImGui::BulletText("%s", warning.c_str());
                ImGui::Unindent();
                ImGui::Separator();
            }
        }
        ImGui::PopID();
    }
    if (reports_.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Analysis results will appear here.");
        ImGui::TextWrapped("Use View to inspect materials and animation, or Compare to check the mod, game and proposed repair side by side.");
    }
    ImGui::EndChild();
}
}
