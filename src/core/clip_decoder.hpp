#pragma once

#include "animation_clip.hpp"

#include <cstdint>
#include <span>

namespace skeleton_repair {

// Reads a compiled AG2 animation clip (.vnmclip_c). Throws InvalidData when the file does not match the pinned layout.
AnimationClip DecodeAnimationClip(std::span<const uint8_t> file);

}  // namespace skeleton_repair
