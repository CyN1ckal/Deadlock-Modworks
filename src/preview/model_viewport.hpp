#pragma once

#include "animation_clip.hpp"
#include "background.hpp"
#include "model_renderer.hpp"
#include "preview_materials.hpp"
#include "preview_scene.hpp"
#include "skeleton_pose.hpp"

#include <imgui.h>
#include <vulkan/vulkan.h>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace skeleton_repair {

// Display settings shared by every viewport in a view.
struct ViewSettings {
    int Lod = 0;
    bool ShowSkeleton = true;
    bool ShowGrid = true;
    ColorMode Coloring = ColorMode::Textured;
};

// Draws the LOD, overlay and coloring controls on one line. Returns true when the LOD changed.
// Sets `frame` when the Frame button was pressed.
bool DrawViewSettings(ViewSettings& settings, int lod_count, bool& frame);
// The same settings as menu items, for a viewport's context menu. Returns true when the LOD changed.
bool DrawViewMenu(ViewSettings& settings, int lod_count, bool& frame);

// The CPU side of a viewport model: the scene and decoded color maps, ready to upload.
struct PreparedModel {
    PreviewScene Scene;
    std::vector<GeometryBone> Bones;
    // Null when no reader was given. Shared, because versions of one model often use the same materials.
    std::shared_ptr<const std::vector<PreviewMaterial>> Materials;
};

// Builds the scene and, with a reader, decodes its materials. Safe to call from any thread if `read` is.
// `reuse` supplies already decoded materials when it uses exactly the same ones and was read the same way.
PreparedModel PrepareModel(const ModelGeometry& geometry, const ResourceReader* read = nullptr, uint32_t max_texture_size = 2048,
    const PreparedModel* reuse = nullptr);

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

// One model drawn into an ImGui image with orbit-camera input. Owns its renderer and texture.
// Requires imgui_impl_vulkan to be initialized.
class ModelViewport {
public:
    explicit ModelViewport(const VulkanDevice& device);
    ~ModelViewport();
    ModelViewport(const ModelViewport&) = delete;
    ModelViewport& operator=(const ModelViewport&) = delete;

    // Null clears the viewport. With a reader, materials and color maps are loaded for Textured mode.
    // A lower texture limit keeps thumbnail viewports small.
    void SetModel(const ModelGeometry* geometry, int lod, const ResourceReader* read = nullptr, uint32_t max_texture_size = 2048);
    // Uploads a model prepared by PrepareModel. Null clears the viewport.
    void SetModel(const PreparedModel* model, int lod);
    // The skeleton drawn as lines. `animation` says whether it is the animation skeleton (else the render skeleton),
    // which decides which posed bones replace it while a clip plays.
    void SetSkeleton(const std::vector<GeometryBone>& bones, std::span<const uint32_t> colors, bool animation = false);
    // The animation skeleton clips play on. Null disables animation.
    void SetAnimationSkeleton(const std::vector<GeometryBone>* bones);
    // Poses the model for this frame. Null clip returns to the bind pose. Returns a warning when the clip does not fit the skeleton.
    std::string Animate(const AnimationClip* clip, float seconds);
    bool CanAnimate() const { return poser_.has_value(); }
    void SetLod(int lod);

    bool Empty() const { return scene_.Meshes.empty() && scene_.Skeleton.empty(); }
    const PreviewScene& Scene() const { return scene_; }
    std::vector<uint8_t>& Visible() { return visible_; }
    // Materials with a color map, out of all materials, and why the others have none.
    size_t TexturedMaterials() const { return textured_materials_; }
    size_t MaterialCount() const { return material_count_; }
    const std::vector<std::string>& MaterialNotes() const { return material_notes_; }

    // Shows the image at the cursor and applies mouse input to `camera`. Returns true while the user moves the camera.
    // Non-interactive images leave scrolling and mouse input to the containing library.
    bool Draw(const char* id, ImVec2 size, OrbitCamera& camera, bool interactive = true);
    // True once after a right click on the image that did not pan, to open a context menu.
    bool TakeContextMenu() { return std::exchange(context_requested_, false); }
    // Records the offscreen pass if Draw ran this frame. Call before the ImGui render pass.
    void Record(VkCommandBuffer command_buffer, const OrbitCamera& camera, const ViewSettings& settings);

private:
    ModelRenderer renderer_;
    PreviewScene scene_;
    std::vector<GeometryBone> model_bones_;
    std::vector<uint8_t> visible_;
    std::optional<SkeletonPoser> poser_;
    std::vector<uint32_t> overlay_colors_;
    bool overlay_animation_ = false;
    std::optional<PosedSkeleton> posed_;
    std::vector<LineVertex> posed_lines_;
    std::vector<BoneTransform> local_;
    size_t textured_materials_ = 0;
    size_t material_count_ = 0;
    std::vector<std::string> material_notes_;
    VkDescriptorSet texture_ = VK_NULL_HANDLE;
    bool drawn_ = false;
    bool context_requested_ = false;
};

}  // namespace skeleton_repair
