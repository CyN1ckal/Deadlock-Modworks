#pragma once

#include "animation_clip.hpp"
#include "background.hpp"
#include "resource_reader.hpp"

#include <optional>
#include <string>
#include <vector>

namespace skeleton_repair {

// Picks and plays an animation clip for one or more viewports.
class AnimationPlayer {
public:
    // Clip resources (.vnmclip_c). Additive clips with a "+non_additive" twin are hidden in favour of the twin.
    void SetClips(const std::vector<std::string>& paths);
    // Draws the clip picker and transport controls, loading a picked clip through `read`. Returns true when the clip changed.
    // `read` runs on a worker thread, so it must not touch UI state.
    bool Draw(const ResourceReader& read);
    // Starts loading and playing a listed clip by path. Returns false when it is not in the list.
    bool Select(const std::string& path, const ResourceReader& read);
    // Applies a clip that finished loading. Returns true when the clip changed.
    bool Poll();
    void Advance(float seconds);

    // Null for the bind pose.
    const AnimationClip* Clip() const { return clip_ ? &*clip_ : nullptr; }
    float Time() const { return time_; }

private:
    struct Entry {
        std::string Path;
        std::string Label;
    };
    std::vector<Entry> clips_;
    int selected_ = -1;
    std::optional<AnimationClip> clip_;
    LatestTask<AnimationClip> loading_;
    std::string error_;
    float time_ = 0;
    float speed_ = 1;
    bool playing_ = true;
    char filter_[96] = {};
};

}  // namespace skeleton_repair
