#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace skeleton_repair {

// How a compiled hero model's root_motion camera attachments relate to the
// frame the game has used since October 16, 2026.
enum class CameraFrame {
    // No standing or crouching pivot is parented to root_motion.
    None,
    // standing_pivot / crouching_pivot already use the current frame.
    Current,
    // Those pivots still use the pre-October 2026 frame. The camera stays
    // behind while the hero walks.
    Old,
    // A pivot is present, but it matches neither frame. Nothing is written.
    Unrecognized,
};

struct CameraAttachmentEdit {
    CameraFrame Frame = CameraFrame::None;
    // Meshes whose pivots are still in the old frame.
    int Meshes = 0;
    // root_motion influences on those meshes.
    int Influences = 0;
    std::string Detail;
    // Set when some meshes were repaired and others were left unchanged.
    std::string Warning;
    // Set by Apply when Frame is Old and every old mesh was rewritten.
    std::optional<std::vector<uint8_t>> Bytes;
};

// Identifies and rebases the camera attachments stored in a compiled model.
// The game reads standing_pivot from the root_motion bone. The October 16, 2026
// update changed that bone's bind frame. Older skins still store the pivot in
// the previous frame, so the camera does not follow the hero.
class CameraAttachmentRepair {
public:
    static CameraAttachmentEdit Inspect(std::span<const uint8_t> model);
    static CameraAttachmentEdit Apply(std::span<const uint8_t> model);
};

}  // namespace skeleton_repair
