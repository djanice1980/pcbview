#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pcbview {

// What a physical device can do. Ray tracing is reported, never required --
// see ARCHITECTURE.md "RT-readiness rule 1".
struct GpuInfo {
    VkPhysicalDevice handle = VK_NULL_HANDLE;
    std::string name;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    uint32_t apiVersion = 0;
    uint32_t graphicsQueueFamily = UINT32_MAX;
    // Real ceiling for the render-scale slider; clamping to a guess would either
    // waste headroom or blow past the device on a big monitor.
    uint32_t maxImageDimension2D = 0;

    bool hasRayTracingPipeline = false;
    bool hasAccelerationStructure = false;
    bool hasDeferredHostOperations = false;
    bool hasBufferDeviceAddress = false;
    bool hasDescriptorIndexing = false;
    bool hasRayQuery = false;
    // Variable-rate shading. The route to foveation that needs no eye tracker:
    // an attachment says how coarsely to shade each tile, so the periphery can
    // shade one fragment per 2x2 or 4x4 pixels. Ours is a fragment-shading
    // cost -- rays per fragment -- so this cuts the rays directly.
    bool hasFragmentShadingRate = false;

    // True only when every prerequisite for the RT-pipeline path is present.
    bool rayTracingReady() const {
        return hasRayTracingPipeline && hasAccelerationStructure &&
               hasDeferredHostOperations && hasBufferDeviceAddress &&
               hasDescriptorIndexing;
    }

    // The lighter path pcbview actually uses: ray queries issued from the
    // fragment shader (RT shadows / AO). Needs acceleration structures and buffer
    // device address, but not the full RT pipeline / SBT machinery.
    bool rayQueryReady() const {
        return hasRayQuery && hasAccelerationStructure &&
               hasDeferredHostOperations && hasBufferDeviceAddress &&
               hasDescriptorIndexing;
    }

    bool usable() const { return graphicsQueueFamily != UINT32_MAX; }
    const char* typeName() const;
};

// A created logical device plus the capability flag the renderers branch on.
struct Device {
    VkDevice handle = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    GpuInfo gpu;

    // Set at creation: whether RT extensions were actually enabled, not merely
    // advertised. Renderers gate the RT path on this.
    bool rayTracingEnabled = false;
    bool rayQueryEnabled = false;
    // Whether multiview was enabled, which is what lets the VR raster path draw
    // both eyes in one pass. Not advertised by CPU devices we care about, and
    // useless to the compute path tracer either way.
    bool multiviewEnabled = false;
    // Attachment-based variable-rate shading, and the tile size one texel of
    // that attachment covers. Foveation without an eye tracker.
    bool shadingRateEnabled = false;
    uint32_t shadingRateTexelW = 16;
    uint32_t shadingRateTexelH = 16;
    // Fragment-shader invocation counting. The adaptive quality model prices a
    // frame as roughly base + k * fragments-shaded, and this is the only way to
    // learn the second factor rather than guess it from distance -- it already
    // accounts for how much of the view the board covers, for the shading rate,
    // for the hidden-area mask and for depth rejection, none of which a
    // distance threshold can know.
    bool pipelineStatsEnabled = false;
    // maxPushConstantsSize, carried so the renderer can tell whether the wider
    // two-eye push block fits before it commits to multiview.
    uint32_t maxPushConstants = 128;
};

// ---- OpenXR takes over Vulkan creation --------------------------------------
//
// An OpenXR session does not accept an arbitrary Vulkan setup. The runtime WRAPS
// instance and device creation so it can inject the extensions it needs, and it
// NAMES the physical device -- a session against any other GPU fails. So when VR
// is in play these calls have to go through the runtime rather than straight to
// Vulkan.
//
// Done as hooks rather than by rewriting the creation path so that the ordinary
// case is untouched: with nothing installed, createInstance/createDevice/
// selectGpu behave exactly as they always did, which keeps the desktop renderer
// out of the blast radius of the VR work.
struct VulkanCreationHooks {
    // Each returns VK_SUCCESS and fills the out-param, or a failure code.
    VkResult (*createInstance)(void* user, const VkInstanceCreateInfo* info,
                               VkInstance* out) = nullptr;
    VkResult (*createDevice)(void* user, VkPhysicalDevice gpu,
                             const VkDeviceCreateInfo* info,
                             VkDevice* out) = nullptr;
    void* user = nullptr;
};
// Pass nullptr to clear. Not owned; must outlive the creation calls.
void setVulkanCreationHooks(const VulkanCreationHooks* hooks);

// The GPU the runtime insists on. VK_NULL_HANDLE means "no constraint", which is
// the normal desktop case. selectGpu() honours this above any user preference,
// because a user preference that disagrees simply cannot produce a session.
void setRequiredGpu(VkPhysicalDevice gpu);
VkPhysicalDevice requiredGpu();

// `extensions` lets the window layer add its surface extensions (GLFW supplies
// them); pass none for a headless instance.
VkInstance createInstance(bool enableValidation,
                          const std::vector<const char*>& extensions = {});

// Routes validation output to stderr. Without this the validation layer runs and
// reports to nobody, which is worse than not enabling it -- it looks like
// coverage that is not there. Returns VK_NULL_HANDLE if the extension is absent.
VkDebugUtilsMessengerEXT createDebugMessenger(VkInstance instance);
void destroyDebugMessenger(VkInstance instance, VkDebugUtilsMessengerEXT messenger);

std::vector<GpuInfo> enumerateGpus(VkInstance instance);

// Prefers discrete + ray-tracing-ready. This box has an RTX 5070 Ti alongside a
// Radeon iGPU, so index 0 is never a safe assumption.
const GpuInfo* pickBestGpu(const std::vector<GpuInfo>& gpus);

// Honour a user preference: if `preferNameSubstring` (case-insensitive) matches a
// usable GPU's name, return that; otherwise fall back to pickBestGpu. Empty
// preference is just pickBestGpu.
const GpuInfo* selectGpu(const std::vector<GpuInfo>& gpus,
                         const std::string& preferNameSubstring);

// Enables RT extensions when the GPU supports them, and cleanly does not when it
// doesn't. Never fails just because RT is absent.
// `extensions` adds device extensions the caller needs (e.g. swapchain).
Device createDevice(const GpuInfo& gpu,
                    const std::vector<const char*>& extensions = {});

void destroyDevice(Device& device);

}  // namespace pcbview
