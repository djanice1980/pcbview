#pragma once

// Vulkan forward rasteriser for a tessellated board.
//
// Written so the ray traced mode in phase 4 is additive, not a rewrite. The
// three RT-readiness rules from ARCHITECTURE.md are load-bearing here:
//
//   1. RT extensions are enabled at device creation but never required.
//   2. Mesh buffers carry SHADER_DEVICE_ADDRESS and the acceleration-structure
//      build-input usage from the start, so phase 4 builds a BLAS/TLAS over the
//      very same buffers rather than rebuilding the asset layer.
//   3. Materials live in a bindless SSBO indexed by instance ID -- never
//      per-draw descriptor sets, which a closest-hit shader cannot do.

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "geom/tessellate.h"
#include "render/common/device.h"

namespace pcbview::vk {

// Mirrors the `Material` struct in board.frag. std430 layout.
struct MaterialGpu {
    float albedo[4];  // rgb + opacity
    float params[4];  // roughness, metallic, reserved, reserved
};

struct FrameStats {
    uint32_t drawCalls = 0;
    uint32_t triangles = 0;      // drawn this frame (respects visibility)
    uint32_t trianglesTotal = 0; // uploaded
};

// One toggleable piece of the board, surfaced for the UI.
struct PartInfo {
    std::string name;
    uint32_t triangles = 0;
    bool visible = true;
    bool blended = false;
};

class Renderer {
public:
    Renderer(Device& device, VkSurfaceKHR surface, uint32_t width,
             uint32_t height);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Uploads the board into one global vertex buffer and one global index
    // buffer, with per-part offsets. Not a buffer per part: rule 2 wants a
    // single allocation an acceleration structure can be built over.
    void uploadBoard(const geom::BoardMesh& mesh);

    // Returns false if the swapchain is out of date and the caller should resize.
    //
    // `drawUi` is invoked with the command buffer while the swapchain is bound at
    // NATIVE resolution, after the scene has been blitted in. That ordering is
    // the point of the offscreen target: the board can be super- or sub-sampled
    // while the interface stays pixel-crisp.
    bool drawFrame(const float viewProj[16], const float cameraPos[3],
                   const std::function<void(VkCommandBuffer)>& drawUi = {});

    void resize(uint32_t width, uint32_t height);
    void waitIdle();

    // Internal render scale. 1.0 = native; 2.0 supersamples; 0.5 halves it.
    // Applies to the 3D scene only, never the UI.
    void setRenderScale(float scale);
    float renderScale() const { return renderScale_; }

    // Substrate (FR4/flex) appearance. rgb in 0..1, opacity 1 = solid. Updating
    // these re-writes just the material table -- no re-tessellation -- so it is
    // cheap enough to drive from a live colour picker / slider. Opacity < 1
    // routes the substrate through the blended pass permanently (not only while
    // peeling), which is what "translucent flex" needs.
    void setSubstrateAppearance(float r, float g, float b, float opacity);

    // Soldermask colour (both sides). Opacity is kept at its film default -- the
    // mask reads as a translucent film regardless of tint. Same live-update,
    // no-retessellate path as the substrate.
    void setMaskColor(float r, float g, float b);

    // Exploded view, peeled outside-in.
    //
    // `mmPerStage` is how far a ring travels per stage of progress.
    // `progress` is already eased by the caller (dwell included) and runs 0 at a
    // solid board up to maxRank() when every ring has peeled.
    //
    // Free to animate: one push constant. Geometry and the per-part ranks in the
    // material table never change.
    void setExplode(float mmPerStage, float progress) {
        explodeStep_ = mmPerStage;
        explodeProgress_ = progress;
    }

    // Largest |rank| in the loaded board -- i.e. how many stages a full peel
    // takes. Zero until a board is uploaded.
    float maxRank() const { return maxRank_; }

    VkExtent2D sceneExtent() const { return sceneExtent_; }
    VkExtent2D windowExtent() const { return extent_; }

    // UI mutates PartInfo::visible directly.
    std::vector<PartInfo>& parts() { return parts_; }
    const Device& device() const { return device_; }
    VkFormat colorFormat() const { return colorFormat_; }
    uint32_t imageCount() const { return static_cast<uint32_t>(images_.size()); }

    // Write the next presented frame to a BMP. Exists so the Vulkan path can be
    // verified without a human watching a window -- the same reason the software
    // rasteriser exists. Requires TRANSFER_SRC on the swapchain images.
    void requestCapture(const std::string& path) { capturePath_ = path; }

    const FrameStats& stats() const { return stats_; }

private:
    struct Buffer {
        VkBuffer handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
    };

    struct DrawItem {
        uint32_t indexOffset = 0;
        uint32_t indexCount = 0;
        int32_t vertexOffset = 0;
        uint32_t material = 0;
        bool blended = false;       // always translucent (soldermask)
        bool fadesWhenPeeled = false;  // becomes translucent only while peeling
        bool hideWhenCollapsed = false;  // inner copper: buried, so at rest it
                                         // only shows as edge z-fighting lines --
                                         // hidden until the board peels open
        uint32_t part = 0;          // index into parts_
        float centre[3] = {0, 0, 0};   // unexploded world centre, for sorting
        float rank = 0.0f;
    };

    struct Image {
        VkImage handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    void createSwapchain(uint32_t width, uint32_t height);
    void destroySwapchain();
    void createSceneTargets();
    void destroySceneTargets();
    void createPipeline();
    void createSyncAndCommands();
    void createDescriptors();

    Image createImage(VkExtent2D extent, VkFormat format,
                      VkImageUsageFlags usage, VkImageAspectFlags aspect);
    void destroyImage(Image& image);

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                        VkMemoryPropertyFlags props);
    void destroyBuffer(Buffer& buffer);
    void uploadViaStaging(Buffer& dst, const void* data, VkDeviceSize size);
    uint32_t findMemoryType(uint32_t filter, VkMemoryPropertyFlags props) const;

    Device& device_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;

    // The scene renders here at sceneExtent_, then blits to the swapchain. The
    // UI is drawn afterwards straight onto the swapchain at native size.
    Image sceneColor_;
    Image sceneDepth_;
    VkExtent2D sceneExtent_{};
    float renderScale_ = 1.0f;
    float explodeStep_ = 0.0f;
    float explodeProgress_ = 0.0f;
    float maxRank_ = 0.0f;
    float boardMidZ_ = 0.0f;  // for the translucent-layer sort direction
    VkFormat depthFormat_ = VK_FORMAT_D32_SFLOAT;

    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipelineOpaque_ = VK_NULL_HANDLE;
    VkPipeline pipelineBlend_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet materialSet_ = VK_NULL_HANDLE;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;
    std::vector<VkSemaphore> imageAvailable_;
    std::vector<VkSemaphore> renderFinished_;
    std::vector<VkFence> inFlight_;
    uint32_t frame_ = 0;

    void captureImage(uint32_t imageIndex);

    Buffer vertexBuffer_;
    Buffer indexBuffer_;
    Buffer materialBuffer_;
    std::vector<DrawItem> draws_;
    std::vector<PartInfo> parts_;
    std::vector<MaterialGpu> materials_;  // CPU copy, for live appearance edits
    std::array<float, 3> substrateColor_ = {0.72f, 0.61f, 0.38f};  // FR4 tan
    float substrateOpacity_ = 1.0f;
    std::array<float, 3> maskColor_ = {0.05f, 0.29f, 0.12f};  // green
    std::string capturePath_;

    FrameStats stats_;
};

}  // namespace pcbview::vk
