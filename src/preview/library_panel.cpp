#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>

#include "library_panel.hpp"

#include "library_model.hpp"
#include "strings.hpp"

#include <imgui.h>

#include <exception>
#include <utility>

namespace skeleton_repair {
namespace {

std::optional<std::filesystem::path> AskForVpk(HWND owner) {
    wchar_t buffer[MAX_PATH] = {};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = L"VPK archives (*.vpk)\0*.vpk\0All files\0*.*\0";
    dialog.lpstrTitle = L"Import a mod VPK";
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) {
        return std::nullopt;
    }
    return std::filesystem::path(buffer);
}

std::filesystem::path DefaultLibraryDirectory() {
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required > 1) {
        std::wstring local(static_cast<size_t>(required), L'\0');
        const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), required);
        if (written > 0 && written < required) {
            local.resize(written);
            return std::filesystem::path(local) / L"DeadlockModelUpdater" / L"store";
        }
    }
    return std::filesystem::temp_directory_path() / L"DeadlockModelUpdater" / L"store";
}

bool SameBlob(const BlobId& left, const BlobId& right) { return left.Bytes == right.Bytes; }

}  // namespace

LibraryPanel::LibraryPanel() {
    try {
        store_.emplace(ContentStore::Open(DefaultLibraryDirectory()));
        Refresh();
    } catch (const std::exception& error) {
        open_error_ = error.what();
        store_.reset();
    }
}

LibraryPanel::~LibraryPanel() {
    cancel_.Cancel();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void LibraryPanel::Refresh() {
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
        history_lines_.clear();
        memberships_.clear();
        observation_ids_.clear();
        unproven_.clear();
        unproven_text_.clear();
        return;
    }
    LoadSelection();
}

void LibraryPanel::LoadSelection() {
    if (!store_ || !selected_ || running_) {
        return;
    }
    try {
        history_lines_ = FormatHistory(store_->HistoryOf(*selected_));
        memberships_ = store_->Memberships(*selected_);
        unproven_ = store_->UnprovenSamePath(*selected_);
        unproven_text_ = unproven_.empty() ? std::string() : FormatUnproven(unproven_);
        observation_ids_.assign(memberships_.size(), std::string());
        if (selected_copy_ && *selected_copy_ >= memberships_.size()) {
            selected_copy_.reset();
        }
    } catch (const std::exception& error) {
        status_ = error.what();
    }
}

void LibraryPanel::StartImport(const std::filesystem::path& path) {
    JoinFinishedImport();
    if (running_ || !store_) {
        return;
    }
    cancel_ = CancellationSource();
    importing_name_ = Narrow(path.filename().wstring());
    import_error_.clear();
    import_ok_ = false;
    import_done_.store(false, std::memory_order_relaxed);
    const CancellationToken token = cancel_.Token();
    running_ = true;
    try {
        worker_ = std::thread([this, path, token] {
            try {
                store_->ImportVpk(path, token);
                import_ok_ = true;
                import_error_.clear();
            } catch (const std::exception& error) {
                import_ok_ = false;
                import_error_ = error.what();
            } catch (...) {
                import_ok_ = false;
                import_error_ = "Could not import the VPK.";
            }
            import_done_.store(true, std::memory_order_release);
        });
    } catch (const std::exception& error) {
        running_ = false;
        import_error_ = error.what();
    }
}

void LibraryPanel::JoinFinishedImport() {
    if (!import_done_.load(std::memory_order_acquire)) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    import_done_.store(false, std::memory_order_relaxed);
    running_ = false;
    if (import_ok_) {
        import_error_.clear();
        Refresh();
    }
}

void LibraryPanel::HideSelectedCopy() {
    if (!store_ || !selected_copy_ || *selected_copy_ >= observation_ids_.size()) {
        return;
    }
    const std::string& observation_id = observation_ids_[*selected_copy_];
    // List rows do not carry the observation id DropObservation requires.
    if (observation_id.empty()) {
        status_ = "This copy has no observation id.";
        return;
    }
    try {
        store_->DropObservation(observation_id);
        status_.clear();
        selected_copy_.reset();
        Refresh();
    } catch (const std::exception& error) {
        status_ = error.what();
    }
}

void LibraryPanel::Draw(void* owner_window) {
    JoinFinishedImport();
    const HWND owner = static_cast<HWND>(owner_window);

    ImGui::Begin("Library");
    if (!store_) {
        ImGui::TextWrapped("%s", open_error_.empty() ? "The model library is unavailable." : open_error_.c_str());
    } else if (running_) {
        ImGui::TextWrapped("Importing %s", importing_name_.c_str());
        if (ImGui::Button("Cancel")) {
            cancel_.Cancel();
        }
    } else if (ImGui::Button("Import VPK...")) {
        if (const std::optional<std::filesystem::path> path = AskForVpk(owner)) {
            StartImport(*path);
        }
    }
    // import_error_ is written on the worker. Read it only after that thread has been joined.
    if (!running_ && !import_error_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", import_error_.c_str());
    }

    if (store_ && ImGui::BeginTabBar("library-tabs")) {
        if (ImGui::BeginTabItem("Models")) {
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##model-filter", "Filter", filter_, sizeof(filter_));
            const std::vector<ModelRow> shown = FilterModels(models_, filter_);
            if (ImGui::BeginChild("model-list")) {
                for (const ModelRow& row : shown) {
                    std::string label = FormatModelRow(row);
                    label += "##";
                    label += row.Blob.Hex();
                    const bool chosen = selected_ && SameBlob(*selected_, row.Blob);
                    if (ImGui::Selectable(label.c_str(), chosen) && !running_) {
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
            if (ImGui::BeginChild("pack-list")) {
                for (const PackRow& pack : packs_) {
                    const std::string line = pack.Label + "  " + std::to_string(pack.ModelCount) + " models  "
                        + std::to_string(pack.ObservationCount) + " observations";
                    ImGui::TextUnformatted(line.c_str());
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();

    ImGui::Begin("History");
    if (!status_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", status_.c_str());
    }
    if (!selected_) {
        ImGui::TextDisabled("Select a model.");
    } else {
        for (const std::string& line : history_lines_) {
            ImGui::TextWrapped("%s", line.c_str());
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
        const bool hide_disabled = running_ || !selected_copy_;
        if (hide_disabled) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Hide this copy")) {
            HideSelectedCopy();
        }
        if (hide_disabled) {
            ImGui::EndDisabled();
        }
        if (!unproven_.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("%s", unproven_text_.c_str());
        }
    }
    ImGui::End();
}

}  // namespace skeleton_repair
