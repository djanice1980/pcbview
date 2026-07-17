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

    // True only when every prerequisite for phase 4 is present.
    bool rayTracingReady() const {
        return hasRayTracingPipeline && hasAccelerationStructure &&
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
};

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

// Enables RT extensions when the GPU supports them, and cleanly does not when it
// doesn't. Never fails just because RT is absent.
// `extensions` adds device extensions the caller needs (e.g. swapchain).
Device createDevice(const GpuInfo& gpu,
                    const std::vector<const char*>& extensions = {});

void destroyDevice(Device& device);

}  // namespace pcbview
