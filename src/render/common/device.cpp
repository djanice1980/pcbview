#include "render/common/device.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace pcbview {
namespace {

bool hasExtension(const std::vector<VkExtensionProperties>& available,
                  const char* wanted) {
    return std::any_of(available.begin(), available.end(),
                       [wanted](const VkExtensionProperties& e) {
                           return std::strcmp(e.extensionName, wanted) == 0;
                       });
}

std::vector<VkExtensionProperties> deviceExtensions(VkPhysicalDevice gpu) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, props.data());
    return props;
}

uint32_t findGraphicsQueue(VkPhysicalDevice gpu) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());

    for (uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) return i;
    }
    return UINT32_MAX;
}

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " failed: VkResult " +
                                 std::to_string(static_cast<int>(r)));
    }
}

}  // namespace

const char* GpuInfo::typeName() const {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "cpu";
        default:                                     return "other";
    }
}

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const char* level = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                            ? "ERROR"
                        : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
                            ? "WARNING"
                            : "info";
    std::fprintf(stderr, "[vulkan %s] %s\n", level, data->pMessage);
    return VK_FALSE;
}

}  // namespace

VkDebugUtilsMessengerEXT createDebugMessenger(VkInstance instance) {
    auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    if (!create) return VK_NULL_HANDLE;

    VkDebugUtilsMessengerCreateInfoEXT info{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = debugCallback;

    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (create(instance, &info, nullptr, &messenger) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return messenger;
}

void destroyDebugMessenger(VkInstance instance,
                           VkDebugUtilsMessengerEXT messenger) {
    if (messenger == VK_NULL_HANDLE) return;
    auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (destroy) destroy(instance, messenger, nullptr);
}

VkInstance createInstance(bool enableValidation,
                          const std::vector<const char*>& extensions) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "pcbview";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "pcbview";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    // 1.2 is the floor: buffer device address and descriptor indexing are core
    // there, and both are RT prerequisites.
    app.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> layers;
    std::vector<const char*> exts = extensions;

    if (enableValidation) {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> available(count);
        vkEnumerateInstanceLayerProperties(&count, available.data());

        const bool present =
            std::any_of(available.begin(), available.end(),
                        [](const VkLayerProperties& l) {
                            return std::strcmp(l.layerName,
                                               "VK_LAYER_KHRONOS_validation") == 0;
                        });
        if (present) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            // Without debug_utils the layer has nowhere to report.
            exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
    }

    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    info.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    info.ppEnabledExtensionNames = exts.data();

    VkInstance instance = VK_NULL_HANDLE;
    check(vkCreateInstance(&info, nullptr, &instance), "vkCreateInstance");
    return instance;
}

std::vector<GpuInfo> enumerateGpus(VkInstance instance) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> handles(count);
    vkEnumeratePhysicalDevices(instance, &count, handles.data());

    std::vector<GpuInfo> gpus;
    gpus.reserve(count);

    for (VkPhysicalDevice handle : handles) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(handle, &props);

        GpuInfo info;
        info.handle = handle;
        info.name = props.deviceName;
        info.type = props.deviceType;
        info.apiVersion = props.apiVersion;
        info.maxImageDimension2D = props.limits.maxImageDimension2D;
        info.graphicsQueueFamily = findGraphicsQueue(handle);

        const auto exts = deviceExtensions(handle);
        info.hasRayTracingPipeline =
            hasExtension(exts, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        info.hasAccelerationStructure =
            hasExtension(exts, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        info.hasDeferredHostOperations =
            hasExtension(exts, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);

        // Core since 1.2, but confirm the features are actually on -- an
        // extension being listed is not the same as the feature being enabled.
        VkPhysicalDeviceVulkan12Features v12{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 features{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &v12;
        vkGetPhysicalDeviceFeatures2(handle, &features);

        info.hasBufferDeviceAddress = v12.bufferDeviceAddress == VK_TRUE;
        info.hasDescriptorIndexing = v12.descriptorIndexing == VK_TRUE;

        gpus.push_back(std::move(info));
    }
    return gpus;
}

const GpuInfo* pickBestGpu(const std::vector<GpuInfo>& gpus) {
    const GpuInfo* best = nullptr;
    int bestScore = -1;

    for (const GpuInfo& gpu : gpus) {
        if (!gpu.usable()) continue;

        int score = 0;
        if (gpu.rayTracingReady()) score += 1000;
        if (gpu.type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 100;
        else if (gpu.type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 10;

        if (score > bestScore) {
            bestScore = score;
            best = &gpu;
        }
    }
    return best;
}

Device createDevice(const GpuInfo& gpu,
                    const std::vector<const char*>& requested) {
    if (!gpu.usable()) {
        throw std::runtime_error("GPU has no graphics queue family");
    }

    // Rule 1: enable RT when present, proceed without it when absent.
    const bool wantRt = gpu.rayTracingReady();

    std::vector<const char*> extensions = requested;
    if (wantRt) {
        extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        extensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    }

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipeline{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    rtPipeline.rayTracingPipeline = VK_TRUE;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR accel{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    accel.accelerationStructure = VK_TRUE;
    accel.pNext = &rtPipeline;

    // Dynamic rendering and synchronization2 are 1.3 core; both remove a lot of
    // boilerplate and neither conflicts with the RT path.
    VkPhysicalDeviceVulkan13Features v13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.dynamicRendering = VK_TRUE;
    v13.synchronization2 = VK_TRUE;
    if (wantRt) v13.pNext = &accel;

    // Rules 2 and 3 both need these on from the very first device we create:
    // buffer device address for RT vertex fetch, descriptor indexing for the
    // bindless material SSBO.
    VkPhysicalDeviceVulkan12Features v12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.bufferDeviceAddress = VK_TRUE;
    v12.descriptorIndexing = VK_TRUE;
    v12.runtimeDescriptorArray = VK_TRUE;
    v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    v12.descriptorBindingPartiallyBound = VK_TRUE;
    v12.pNext = &v13;

    VkPhysicalDeviceFeatures2 features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &v12;
    features.features.samplerAnisotropy = VK_TRUE;
    features.features.fillModeNonSolid = VK_TRUE;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = gpu.graphicsQueueFamily;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;

    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &features;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();

    Device device;
    device.gpu = gpu;
    device.rayTracingEnabled = wantRt;
    check(vkCreateDevice(gpu.handle, &info, nullptr, &device.handle),
          "vkCreateDevice");
    vkGetDeviceQueue(device.handle, gpu.graphicsQueueFamily, 0,
                     &device.graphicsQueue);
    return device;
}

void destroyDevice(Device& device) {
    if (device.handle != VK_NULL_HANDLE) {
        vkDestroyDevice(device.handle, nullptr);
        device.handle = VK_NULL_HANDLE;
    }
}

}  // namespace pcbview
