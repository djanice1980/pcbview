#include "xr/xr_system.h"

// Vulkan first, then the XR platform header, which builds its Vulkan structs on
// those types. XR_USE_PLATFORM_WIN32 is deliberately NOT defined -- see
// xr_probe.cpp for why.
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace pcbview::xr {
namespace {

XrInstance asXr(void* p) { return static_cast<XrInstance>(p); }

template <class Fn>
Fn proc(XrInstance inst, const char* name) {
    PFN_xrVoidFunction fn = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(inst, name, &fn))) return nullptr;
    return reinterpret_cast<Fn>(fn);
}

// The hooks are plain function pointers so device.h stays free of <functional>
// and of any OpenXR type. `user` is the System.
struct HookCtx {
    XrInstance instance;
    XrSystemId system;
};

VkResult hookCreateInstance(void* user, const VkInstanceCreateInfo* info,
                            VkInstance* out) {
    auto* ctx = static_cast<HookCtx*>(user);
    auto fn = proc<PFN_xrCreateVulkanInstanceKHR>(ctx->instance,
                                                  "xrCreateVulkanInstanceKHR");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;

    XrVulkanInstanceCreateInfoKHR ci{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    ci.systemId = ctx->system;
    ci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    ci.vulkanCreateInfo = info;
    ci.vulkanAllocator = nullptr;

    VkResult vkResult = VK_ERROR_INITIALIZATION_FAILED;
    const XrResult r = fn(ctx->instance, &ci, out, &vkResult);
    // Two failure channels: the XR call itself, and the wrapped Vulkan call.
    if (XR_FAILED(r)) return VK_ERROR_INITIALIZATION_FAILED;
    return vkResult;
}

VkResult hookCreateDevice(void* user, VkPhysicalDevice gpu,
                          const VkDeviceCreateInfo* info, VkDevice* out) {
    auto* ctx = static_cast<HookCtx*>(user);
    auto fn = proc<PFN_xrCreateVulkanDeviceKHR>(ctx->instance,
                                                "xrCreateVulkanDeviceKHR");
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;

    XrVulkanDeviceCreateInfoKHR ci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    ci.systemId = ctx->system;
    ci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    ci.vulkanPhysicalDevice = gpu;
    ci.vulkanCreateInfo = info;
    ci.vulkanAllocator = nullptr;

    VkResult vkResult = VK_ERROR_INITIALIZATION_FAILED;
    const XrResult r = fn(ctx->instance, &ci, out, &vkResult);
    if (XR_FAILED(r)) return VK_ERROR_INITIALIZATION_FAILED;
    return vkResult;
}

HookCtx g_ctx{};

}  // namespace

System::~System() { stop(); }

bool System::start() {
    if (system_) return true;

    const char* ext[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(ici.applicationInfo.applicationName,
                  sizeof(ici.applicationInfo.applicationName), "pcbview");
    ici.applicationInfo.applicationVersion = 1;
    std::snprintf(ici.applicationInfo.engineName,
                  sizeof(ici.applicationInfo.engineName), "pcbview");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = ext;

    XrInstance inst = XR_NULL_HANDLE;
    if (XR_FAILED(xrCreateInstance(&ici, &inst))) {
        std::printf("xr: no runtime answered (is SteamVR installed?)\n");
        return false;
    }

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    if (XR_FAILED(xrGetSystem(inst, &sgi, &sys))) {
        // Overwhelmingly the common cause: SteamVR scans for the headset when
        // vrserver STARTS, so connecting it afterwards is not noticed.
        std::printf("xr: runtime is up but reports no headset. If it is "
                    "plugged in and powered on, restart SteamVR -- it looks "
                    "for the HMD at startup, not on hot-plug.\n");
        xrDestroyInstance(inst);
        return false;
    }

    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(inst, sys, &sp)))
        std::snprintf(headsetName_, sizeof(headsetName_), "%s", sp.systemName);

    instance_ = inst;
    system_ = sys;
    return true;
}

void System::stop() {
    removeHooks();
    if (instance_) xrDestroyInstance(asXr(instance_));
    instance_ = nullptr;
    system_ = 0;
    setRequiredGpu(VK_NULL_HANDLE);
}

void System::installHooks() {
    if (!system_) return;
    g_ctx.instance = asXr(instance_);
    g_ctx.system = static_cast<XrSystemId>(system_);
    hooks_.createInstance = &hookCreateInstance;
    hooks_.createDevice = &hookCreateDevice;
    hooks_.user = &g_ctx;
    setVulkanCreationHooks(&hooks_);
}

void System::removeHooks() { setVulkanCreationHooks(nullptr); }

VkPhysicalDevice System::adoptRuntimeGpu(VkInstance instance) {
    if (!system_) return VK_NULL_HANDLE;
    auto fn = proc<PFN_xrGetVulkanGraphicsDevice2KHR>(
        asXr(instance_), "xrGetVulkanGraphicsDevice2KHR");
    if (!fn) return VK_NULL_HANDLE;

    XrVulkanGraphicsDeviceGetInfoKHR gi{
        XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gi.systemId = static_cast<XrSystemId>(system_);
    gi.vulkanInstance = instance;

    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    if (XR_FAILED(fn(asXr(instance_), &gi, &gpu))) return VK_NULL_HANDLE;
    setRequiredGpu(gpu);
    return gpu;
}

bool System::viewSize(uint32_t* width, uint32_t* height) const {
    if (!system_) return false;
    uint32_t count = 0;
    xrEnumerateViewConfigurationViews(
        asXr(instance_), static_cast<XrSystemId>(system_),
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr);
    if (!count) return false;
    std::vector<XrViewConfigurationView> views(
        count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(
        asXr(instance_), static_cast<XrSystemId>(system_),
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, views.data());
    *width = views[0].recommendedImageRectWidth;
    *height = views[0].recommendedImageRectHeight;
    return true;
}

int deviceTest() {
    std::printf("OpenXR -> Vulkan hand-over test\n"
                "===============================\n\n");
    System xr;
    if (!xr.start()) return 1;
    std::printf("headset: %s\n", xr.headsetName());
    uint32_t vw = 0, vh = 0;
    if (xr.viewSize(&vw, &vh)) std::printf("per-eye: %u x %u\n", vw, vh);

    xr.installHooks();

    // The surface extension is what the real app asks for; including it here
    // keeps this test on the same path rather than a headless special case.
    const std::vector<const char*> instExts = {"VK_KHR_surface",
                                               "VK_KHR_win32_surface"};
    VkInstance vkInstance = VK_NULL_HANDLE;
    try {
        vkInstance = createInstance(/*enableValidation=*/false, instExts);
    } catch (const std::exception& e) {
        std::printf("FAILED creating the Vulkan instance through OpenXR: %s\n",
                    e.what());
        return 2;
    }
    std::printf("vulkan instance created THROUGH the runtime\n");

    const VkPhysicalDevice want = xr.adoptRuntimeGpu(vkInstance);
    if (want == VK_NULL_HANDLE) {
        std::printf("FAILED: the runtime named no physical device\n");
        return 3;
    }

    const std::vector<GpuInfo> gpus = enumerateGpus(vkInstance);
    const GpuInfo* chosen = selectGpu(gpus, /*prefer=*/"");
    if (!chosen) {
        std::printf("FAILED: no usable GPU\n");
        return 4;
    }
    std::printf("runtime requires: %s\n", chosen->name.c_str());
    std::printf("  selectGpu honoured it: %s\n",
                chosen->handle == want ? "YES" : "NO -- MISMATCH");
    // Matters for VR: RT shadows are affordable at 90Hz where path tracing is
    // not, so this is the quality ceiling in the headset.
    std::printf("  ray query ready: %s\n",
                chosen->rayQueryReady() ? "yes" : "no");

    Device dev{};
    try {
        dev = createDevice(*chosen, {"VK_KHR_swapchain"});
    } catch (const std::exception& e) {
        std::printf("FAILED creating the Vulkan device through OpenXR: %s\n",
                    e.what());
        return 5;
    }
    std::printf("vulkan device created THROUGH the runtime\n");

    destroyDevice(dev);
    xr.removeHooks();
    vkDestroyInstance(vkInstance, nullptr);
    xr.stop();
    std::printf("\nhand-over OK\n");
    return 0;
}

}  // namespace pcbview::xr
