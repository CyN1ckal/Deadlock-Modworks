#include "animation_player.hpp"

#include "clip_decoder.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <string_view>

namespace skeleton_repair {

void AnimationPlayer::SetClips(const std::vector<std::string>& paths) {
    constexpr std::string_view kTwin = ".vnmclip+non_additive.vnmclip_c";
    constexpr std::string_view kClip = ".vnmclip_c";
    std::vector<std::string> sorted = paths;
    std::sort(sorted.begin(), sorted.end());
    clips_.clear();
    loading_.Cancel();
    for (const std::string& path : sorted) {
        const bool twin = path.size() > kTwin.size() && path.compare(path.size() - kTwin.size(), kTwin.size(), kTwin) == 0;
        std::string base = twin ? path.substr(0, path.size() - kTwin.size()) : path.substr(0, path.size() - kClip.size());
        if (!twin && std::binary_search(sorted.begin(), sorted.end(), base + std::string(kTwin))) {
            continue;
        }
        const size_t slash = base.rfind('/');
        clips_.push_back({path, slash == std::string::npos ? base : base.substr(slash + 1)});
    }
    selected_ = -1;
    clip_.reset();
    error_.clear();
    time_ = 0;
}

bool AnimationPlayer::Draw(const ResourceReader& read) {
    bool changed = Poll();
    const float scale = ImGui::GetFontSize() / 15.0f;
    ImGui::SetNextItemWidth(220 * scale);
    const char* preview = selected_ >= 0 ? clips_[static_cast<size_t>(selected_)].Label.c_str() : "Bind pose";
    if (ImGui::BeginCombo("##clip", clips_.empty() ? "No animation clips" : preview, ImGuiComboFlags_HeightLarge)) {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputTextWithHint("##clip_filter", "Filter", filter_, sizeof(filter_));
        if (ImGui::Selectable("Bind pose", selected_ < 0)) {
            selected_ = -1;
            loading_.Cancel();
            clip_.reset();
            error_.clear();
            changed = true;
        }
        for (size_t i = 0; i < clips_.size(); ++i) {
            if (filter_[0] != '\0' && clips_[i].Label.find(filter_) == std::string::npos) {
                continue;
            }
            if (ImGui::Selectable(clips_[i].Label.c_str(), static_cast<int>(i) == selected_)) {
                Select(clips_[i].Path, read);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Animation clip. Type in the list to filter it.");
    ImGui::SameLine();
    if (ImGui::SmallButton(playing_ ? "Pause" : "Play")) {
        playing_ = !playing_;
    }
    if (clip_) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150 * scale);
        float shown = clip_->Duration > 0 ? std::fmod(time_, clip_->Duration) : 0;
        if (ImGui::SliderFloat("##time", &shown, 0, std::max(clip_->Duration, 0.001f), "%.2f s")) {
            time_ = shown;
            playing_ = false;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70 * scale);
        ImGui::SliderFloat("##speed", &speed_, 0.1f, 2.0f, "%.1fx");
        ImGui::SetItemTooltip("Playback speed");
        ImGui::SameLine();
        ImGui::TextDisabled("%u frames%s", clip_->FrameCount, clip_->Additive ? ", additive" : "");
    }
    if (loading_.Busy()) {
        ImGui::SameLine();
        ImGui::TextDisabled("Loading clip...");
    }
    if (!error_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", error_.c_str());
    }
    return changed;
}

bool AnimationPlayer::Select(const std::string& path, const ResourceReader& read) {
    const auto found = std::find_if(clips_.begin(), clips_.end(), [&](const Entry& entry) { return entry.Path == path; });
    if (found == clips_.end()) {
        return false;
    }
    selected_ = static_cast<int>(found - clips_.begin());
    clip_.reset();
    error_.clear();
    time_ = 0;
    playing_ = true;
    loading_.Start([read, path](CancellationToken) {
        const auto bytes = read(path);
        if (!bytes) {
            throw std::runtime_error("Clip is missing.");
        }
        return DecodeAnimationClip(*bytes);
    });
    return true;
}

bool AnimationPlayer::Poll() {
    try {
        if (auto clip = loading_.Take()) {
            clip_ = std::move(*clip);
            time_ = 0;
            return true;
        }
    } catch (const std::exception& error) {
        error_ = error.what();
    }
    return false;
}

void AnimationPlayer::Advance(float seconds) {
    if (playing_ && clip_) {
        time_ += seconds * speed_;
        if (clip_->Duration > 0) {
            time_ = std::fmod(time_, clip_->Duration);
        }
    }
}

}  // namespace skeleton_repair
