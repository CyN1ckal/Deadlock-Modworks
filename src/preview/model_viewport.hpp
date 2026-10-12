#pragma once

#include "model_renderer.hpp"
#include "preview_materials.hpp"
#include "preview_scene.hpp"

#include <imgui.h>
#include <vulkan/vulkan.h>

#include <span>
#include <string>
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

// One model drawn into an ImGui image with orbit-camera input. Owns its renderer and texture.
// Requires imgui_impl_vulkan to be initialized.
class ModelViewport {
public:
    explicit ModelViewport(const VulkanDevice& device);
    ~ModelViewport();
    ModelViewport(const ModelViewport&) = delete;
    ModelViewport& operator=(const ModelViewport&) = delete;

    // Null clears the viewport. With a reader, materials and color maps are loaded for Textured mode.
    void SetModel(const ModelGeometry* geometry, int lod, const ResourceReader* read = nullptr);
    void SetSkeleton(const std::vector<GeometryBone>& bones, std::span<const uint32_t> colors);
    void SetLod(int lod);

    bool Empty() const { return scene_.Meshes.empty() && scene_.Skeleton.empty(); }
    const PreviewScene& Scene() const { return scene_; }
    std::vector<uint8_t>& Visible() { return visible_; }
    // Materials with a color map, out of all materials, and why the others have none.
    size_t TexturedMaterials() const { return textured_materials_; }
    size_t MaterialCount() const { return material_count_; }
    const std::vector<std::string>& MaterialNotes() const { return material_notes_; }

    // Shows the image at the cursor and applies mouse input to `camera`. Returns true while the user moves the camera.
    bool Draw(const char* id, ImVec2 size, OrbitCamera& camera);
    // Records the offscreen pass if Draw ran this frame. Call before the ImGui render pass.
    void Record(VkCommandBuffer command_buffer, const OrbitCamera& camera, const ViewSettings& settings);

private:
    ModelRenderer renderer_;
    PreviewScene scene_;
    std::vector<uint8_t> visible_;
    size_t textured_materials_ = 0;
    size_t material_count_ = 0;
    std::vector<std::string> material_notes_;
    VkDescriptorSet texture_ = VK_NULL_HANDLE;
    bool drawn_ = false;
};

}  // namespace skeleton_repair
