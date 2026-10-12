#include "model_renderer.hpp"
#include "preview_materials.hpp"
#include "preview_scene.hpp"
#include "preview_test_model.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using skeleton_repair::BuildPreviewScene;
using skeleton_repair::DefaultVisibility;
using skeleton_repair::ModelRenderer;
using skeleton_repair::OrbitCamera;
using skeleton_repair::PreviewScene;
using skeleton_repair::RenderOptions;
using skeleton_repair::VulkanDevice;

std::vector<std::string> g_validation_errors;

VKAPI_ATTR VkBool32 VKAPI_CALL OnValidation(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        g_validation_errors.emplace_back(data->pMessage);
    }
    return VK_FALSE;
}

bool HasLayer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const auto& layer : layers) {
        if (std::strcmp(layer.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

// A headless Vulkan 1.3 device with validation when the SDK layer is installed.
struct Gpu {
    VkInstance Instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT Messenger = VK_NULL_HANDLE;
    VulkanDevice Device;
    bool Validation = false;

    static std::unique_ptr<Gpu> Create() {
        auto gpu = std::make_unique<Gpu>();
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_3;
        const char* layer = "VK_LAYER_KHRONOS_validation";
        const char* extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        gpu->Validation = HasLayer(layer);
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        if (gpu->Validation) {
            info.enabledLayerCount = 1;
            info.ppEnabledLayerNames = &layer;
            info.enabledExtensionCount = 1;
            info.ppEnabledExtensionNames = &extension;
        }
        if (vkCreateInstance(&info, nullptr, &gpu->Instance) != VK_SUCCESS) {
            return nullptr;
        }
        if (gpu->Validation) {
            VkDebugUtilsMessengerCreateInfoEXT messenger{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
            messenger.pfnUserCallback = OnValidation;
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(gpu->Instance, "vkCreateDebugUtilsMessengerEXT"));
            create(gpu->Instance, &messenger, nullptr, &gpu->Messenger);
        }

        uint32_t count = 0;
        vkEnumeratePhysicalDevices(gpu->Instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(gpu->Instance, &count, devices.data());
        for (VkPhysicalDevice physical : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(physical, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_3) {
                continue;
            }
            uint32_t family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
            for (uint32_t family = 0; family < family_count; ++family) {
                if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                    continue;
                }
                const float priority = 1.0f;
                VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                queue.queueFamilyIndex = family;
                queue.queueCount = 1;
                queue.pQueuePriorities = &priority;
                VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
                features13.dynamicRendering = VK_TRUE;
                features13.synchronization2 = VK_TRUE;
                VkPhysicalDeviceFeatures supported{};
                vkGetPhysicalDeviceFeatures(physical, &supported);
                VkPhysicalDeviceFeatures enabled{};
                enabled.textureCompressionBC = supported.textureCompressionBC;
                VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
                device.pNext = &features13;
                device.pEnabledFeatures = &enabled;
                device.queueCreateInfoCount = 1;
                device.pQueueCreateInfos = &queue;
                if (vkCreateDevice(physical, &device, nullptr, &gpu->Device.Device) != VK_SUCCESS) {
                    continue;
                }
                gpu->Device.Physical = physical;
                gpu->Device.QueueFamily = family;
                vkGetDeviceQueue(gpu->Device.Device, family, 0, &gpu->Device.Queue);
                return gpu;
            }
        }
        return nullptr;
    }

    ~Gpu() {
        if (Device.Device != VK_NULL_HANDLE) {
            vkDestroyDevice(Device.Device, nullptr);
        }
        if (Messenger != VK_NULL_HANDLE) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(Instance, "vkDestroyDebugUtilsMessengerEXT"));
            destroy(Instance, Messenger, nullptr);
        }
        if (Instance != VK_NULL_HANDLE) {
            vkDestroyInstance(Instance, nullptr);
        }
    }
};

constexpr uint32_t kWidth = 160;
constexpr uint32_t kHeight = 120;

struct Pixel {
    uint8_t R, G, B, A;
};

class RendererTest : public testing::Test {
protected:
    static void SetUpTestSuite() { gpu_ = Gpu::Create().release(); }
    static void TearDownTestSuite() {
        delete gpu_;
        gpu_ = nullptr;
    }

    void SetUp() override {
        if (gpu_ == nullptr) {
            GTEST_SKIP() << "No Vulkan 1.3 device is available.";
        }
        g_validation_errors.clear();
        renderer_ = std::make_unique<ModelRenderer>(gpu_->Device);
        renderer_->Resize(kWidth, kHeight);
        scene_ = BuildPreviewScene(preview_test::TwoLodModel());
        visible_ = DefaultVisibility(scene_, 0);
        camera_ = OrbitCamera::Framing(scene_.Min, scene_.Max);
        camera_.Pitch = 0;
        options_.VisibleMeshes = visible_;
        options_.ShowGrid = false;
        options_.ShowSkeleton = false;
    }

    void TearDown() override {
        renderer_.reset();
        if (gpu_ != nullptr) {
            for (const std::string& error : g_validation_errors) {
                ADD_FAILURE() << "Vulkan validation: " << error;
            }
        }
    }

    std::vector<Pixel> Render() {
        const VulkanDevice& device = gpu_->Device;
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.queueFamilyIndex = device.QueueFamily;
        VkCommandPool pool = VK_NULL_HANDLE;
        vkCreateCommandPool(device.Device, &pool_info, nullptr, &pool);
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = pool;
        allocate.commandBufferCount = 1;
        VkCommandBuffer command_buffer = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(device.Device, &allocate, &command_buffer);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(command_buffer, &begin);
        renderer_->Record(command_buffer, camera_, options_);
        vkEndCommandBuffer(command_buffer);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer;
        vkQueueSubmit(device.Queue, 1, &submit, VK_NULL_HANDLE);
        vkQueueWaitIdle(device.Queue);
        vkDestroyCommandPool(device.Device, pool, nullptr);

        const std::vector<uint8_t> bytes = renderer_->ReadPixels();
        std::vector<Pixel> pixels(kWidth * kHeight);
        std::memcpy(pixels.data(), bytes.data(), bytes.size());
        return pixels;
    }

    static Pixel At(const std::vector<Pixel>& pixels, uint32_t x, uint32_t y) { return pixels[y * kWidth + x]; }

    bool IsBackground(Pixel p) const {
        auto near = [](uint8_t value, float expected) { return std::abs(value - expected * 255.0f) <= 2.0f; };
        return near(p.R, options_.Background[0]) && near(p.G, options_.Background[1]) && near(p.B, options_.Background[2]);
    }

    // A 1-pixel line can straddle two columns, so MSAA may blend it half and half with the clay behind it.
    static bool IsBoneYellow(Pixel p) { return p.R > 150 && p.R - p.B > 60; }

    static size_t Count(const std::vector<Pixel>& pixels, bool (*predicate)(Pixel)) {
        size_t count = 0;
        for (Pixel p : pixels) {
            count += predicate(p) ? 1 : 0;
        }
        return count;
    }

    static Gpu* gpu_;
    std::unique_ptr<ModelRenderer> renderer_;
    PreviewScene scene_;
    std::vector<uint8_t> visible_;
    OrbitCamera camera_;
    RenderOptions options_;
};

Gpu* RendererTest::gpu_ = nullptr;

TEST_F(RendererTest, EmptyRendererClearsToBackground) {
    const auto pixels = Render();
    EXPECT_TRUE(IsBackground(At(pixels, 0, 0)));
    EXPECT_TRUE(IsBackground(At(pixels, kWidth / 2, kHeight / 2)));
}

TEST_F(RendererTest, DrawsLitMeshInCenterAndBackgroundInCorners) {
    renderer_->SetScene(scene_);
    const auto pixels = Render();
    const Pixel center = At(pixels, kWidth / 2, kHeight / 2);
    EXPECT_FALSE(IsBackground(center));
    // Facing the headlight: close to the clay base color, well above the background.
    EXPECT_GT(center.R, 180);
    EXPECT_GT(center.R, center.B);
    EXPECT_TRUE(IsBackground(At(pixels, 0, 0)));
    EXPECT_TRUE(IsBackground(At(pixels, kWidth - 1, kHeight - 1)));
}

TEST_F(RendererTest, HiddenMeshesAreNotDrawn) {
    renderer_->SetScene(scene_);
    visible_ = {0, 0};
    options_.VisibleMeshes = visible_;
    const auto pixels = Render();
    EXPECT_TRUE(IsBackground(At(pixels, kWidth / 2, kHeight / 2)));
}

TEST_F(RendererTest, SkeletonDrawsOverTheMesh) {
    renderer_->SetScene(scene_);
    EXPECT_EQ(Count(Render(), IsBoneYellow), 0u);
    options_.ShowSkeleton = true;
    // The spine sits inside the quad's plane and is drawn without depth testing.
    EXPECT_GT(Count(Render(), IsBoneYellow), 10u);
}

TEST_F(RendererTest, MaterialColorsChangeTheShading) {
    renderer_->SetScene(scene_);
    const Pixel clay = At(Render(), kWidth / 2, kHeight / 2);
    options_.Coloring = skeleton_repair::ColorMode::Material;
    const Pixel tinted = At(Render(), kWidth / 2, kHeight / 2);
    EXPECT_TRUE(clay.R != tinted.R || clay.G != tinted.G || clay.B != tinted.B);
}

TEST_F(RendererTest, ResizeReportsOnlyRealChanges) {
    EXPECT_FALSE(renderer_->Resize(kWidth, kHeight));
    EXPECT_TRUE(renderer_->Resize(kWidth * 2, kHeight));
    EXPECT_EQ(renderer_->Extent().width, kWidth * 2);
    EXPECT_FALSE(renderer_->Resize(0, 0) && renderer_->Extent().width == 0);
}

TEST_F(RendererTest, ReplacingTheSceneKeepsRendering) {
    renderer_->SetScene(scene_);
    Render();
    renderer_->SetScene(BuildPreviewScene(preview_test::TwoLodModel()));
    options_.ShowGrid = true;
    options_.ShowSkeleton = true;
    const auto pixels = Render();
    EXPECT_FALSE(IsBackground(At(pixels, kWidth / 2, kHeight / 2)));
}

skeleton_repair::PreviewMaterial SolidMaterial(const std::string& name, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    skeleton_repair::PreviewMaterial material;
    material.Name = name;
    skeleton_repair::TextureImage image;
    image.Width = 2;
    image.Height = 2;
    image.Mips = {{r, g, b, a, r, g, b, a, r, g, b, a, r, g, b, a}, {r, g, b, a}};
    material.Color = image;
    return material;
}

TEST_F(RendererTest, TexturedModeSamplesTheColorMap) {
    renderer_->SetScene(scene_);
    const skeleton_repair::PreviewMaterial materials[] = {SolidMaterial("models/test/body.vmat", 255, 0, 0, 255)};
    EXPECT_TRUE(renderer_->SetMaterials(materials).empty());
    EXPECT_EQ(renderer_->TextureCount(), 1u);
    options_.Coloring = skeleton_repair::ColorMode::Textured;
    const Pixel center = At(Render(), kWidth / 2, kHeight / 2);
    EXPECT_GT(center.R, 150);
    EXPECT_LT(center.G, 60);
    EXPECT_LT(center.B, 60);
}

TEST_F(RendererTest, DrawsWithoutAColorMapStayClay) {
    renderer_->SetScene(scene_);
    const Pixel clay = At(Render(), kWidth / 2, kHeight / 2);
    // A material for another draw only.
    const skeleton_repair::PreviewMaterial materials[] = {SolidMaterial("models/test/other.vmat", 255, 0, 0, 255)};
    renderer_->SetMaterials(materials);
    options_.Coloring = skeleton_repair::ColorMode::Textured;
    const Pixel textured = At(Render(), kWidth / 2, kHeight / 2);
    EXPECT_EQ(clay.R, textured.R);
    EXPECT_EQ(clay.G, textured.G);
    EXPECT_EQ(clay.B, textured.B);
}

TEST_F(RendererTest, AlphaTestDiscardsTransparentTexels) {
    renderer_->SetScene(scene_);
    skeleton_repair::PreviewMaterial material = SolidMaterial("models/test/body.vmat", 255, 0, 0, 0);
    material.AlphaTest = true;
    renderer_->SetMaterials(std::span(&material, 1));
    options_.Coloring = skeleton_repair::ColorMode::Textured;
    EXPECT_TRUE(IsBackground(At(Render(), kWidth / 2, kHeight / 2)));
}

TEST_F(RendererTest, TranslucentMaterialsBlendOverTheBackground) {
    renderer_->SetScene(scene_);
    skeleton_repair::PreviewMaterial material = SolidMaterial("models/test/body.vmat", 255, 255, 255, 128);
    material.Translucent = true;
    renderer_->SetMaterials(std::span(&material, 1));
    options_.Coloring = skeleton_repair::ColorMode::Textured;
    const Pixel center = At(Render(), kWidth / 2, kHeight / 2);
    EXPECT_FALSE(IsBackground(center));
    // Half white over a dark background lands between the two.
    EXPECT_GT(center.R, options_.Background[0] * 255 + 40);
    EXPECT_LT(center.R, 240);
}

TEST_F(RendererTest, ReplacingMaterialsKeepsRendering) {
    renderer_->SetScene(scene_);
    options_.Coloring = skeleton_repair::ColorMode::Textured;
    const skeleton_repair::PreviewMaterial red[] = {SolidMaterial("models/test/body.vmat", 255, 0, 0, 255)};
    renderer_->SetMaterials(red);
    Render();
    const skeleton_repair::PreviewMaterial blue[] = {SolidMaterial("models/test/body.vmat", 0, 0, 255, 255)};
    renderer_->SetMaterials(blue);
    const Pixel center = At(Render(), kWidth / 2, kHeight / 2);
    EXPECT_GT(center.B, 150);
    EXPECT_LT(center.R, 60);
}

TEST(PreviewMaterials, ReportsMissingFilesInsteadOfThrowing) {
    const auto scene = BuildPreviewScene(preview_test::TwoLodModel());
    const auto materials = skeleton_repair::LoadPreviewMaterials(scene, [](const std::string&) { return std::nullopt; });
    ASSERT_EQ(materials.size(), 2u);
    EXPECT_EQ(materials[0].Name, "models/test/body.vmat");
    EXPECT_FALSE(materials[0].Color);
    EXPECT_EQ(materials[0].Problem, "Material is not in the mod or the game.");
}

TEST(PreviewMaterials, ReportsUnreadableMaterials) {
    const auto scene = BuildPreviewScene(preview_test::TwoLodModel());
    std::vector<std::string> asked;
    const auto materials = skeleton_repair::LoadPreviewMaterials(scene, [&](const std::string& path) {
        asked.push_back(path);
        return std::optional<std::vector<uint8_t>>(std::vector<uint8_t>{1, 2, 3});
    });
    EXPECT_EQ(asked.front(), "models/test/body.vmat_c");
    EXPECT_FALSE(materials[0].Problem.empty());
    EXPECT_FALSE(materials[0].Color);
}

}  // namespace
