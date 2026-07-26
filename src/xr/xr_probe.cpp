#include "xr/xr_probe.h"

// Only the Vulkan binding is requested. Defining XR_USE_PLATFORM_WIN32 would
// declare structs built on LARGE_INTEGER and IUnknown, which then requires
// <windows.h> and <unknwn.h> ahead of it -- and dragging windows.h into a
// Qt/Vulkan translation unit brings its min/max macros along with it.
#define XR_USE_GRAPHICS_API_VULKAN
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace pcbview::xr {
namespace {

// OpenXR reports failures as codes, and xrResultToString needs an instance --
// which is exactly what you do not have when xrCreateInstance is what failed.
std::string resultName(XrInstance inst, XrResult r) {
    char buf[XR_MAX_RESULT_STRING_SIZE] = {};
    if (inst != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(inst, r, buf)))
        return buf;
    return "XrResult " + std::to_string(static_cast<int>(r));
}

bool check(XrInstance inst, XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    std::printf("  FAILED: %s -> %s\n", what, resultName(inst, r).c_str());
    return false;
}

}  // namespace

int probe() {
    std::printf("OpenXR probe\n============\n\n");

    // --- What the loader can see before an instance exists -----------------
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(
        extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    if (extCount) {
        xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount,
                                               exts.data());
    }
    std::printf("runtime extensions: %u\n", extCount);

    auto has = [&exts](const char* name) {
        for (const auto& e : exts)
            if (std::strcmp(e.extensionName, name) == 0) return true;
        return false;
    };
    // XR_KHR_vulkan_enable2 is the one that matters: it is how the runtime
    // hands over the physical device and the instance/device extensions the
    // session requires. Without it there is no Vulkan VR here.
    const bool vk2 = has(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    std::printf("  %-42s %s\n", XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
                vk2 ? "YES" : "no");
    std::printf("  %-42s %s\n", XR_KHR_VULKAN_ENABLE_EXTENSION_NAME,
                has(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) ? "yes" : "no");
    std::printf("  %-42s %s\n", "XR_EXT_hand_tracking",
                has("XR_EXT_hand_tracking") ? "yes" : "no");

    // --- Instance ----------------------------------------------------------
    std::vector<const char*> enabled;
    if (vk2) enabled.push_back(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);

    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(ici.applicationInfo.applicationName,
                  sizeof(ici.applicationInfo.applicationName), "pcbview");
    ici.applicationInfo.applicationVersion = 1;
    std::snprintf(ici.applicationInfo.engineName,
                  sizeof(ici.applicationInfo.engineName), "pcbview");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    ici.enabledExtensionNames = enabled.data();

    XrInstance instance = XR_NULL_HANDLE;
    XrResult r = xrCreateInstance(&ici, &instance);
    if (!check(XR_NULL_HANDLE, r, "xrCreateInstance")) {
        std::printf(
            "\nNo OpenXR runtime answered. Check that SteamVR is installed and\n"
            "has been run at least once, so it registers itself as the active\n"
            "runtime.\n");
        return 1;
    }

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &ip))) {
        std::printf("\nruntime: %s %u.%u.%u\n", ip.runtimeName,
                    XR_VERSION_MAJOR(ip.runtimeVersion),
                    XR_VERSION_MINOR(ip.runtimeVersion),
                    XR_VERSION_PATCH(ip.runtimeVersion));
    }

    // --- The headset -------------------------------------------------------
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    r = xrGetSystem(instance, &sgi, &system);
    if (!check(instance, r, "xrGetSystem")) {
        std::printf(
            "\nThe runtime is there but reports no head-mounted display.\n"
            "Is the headset connected and awake?\n");
        xrDestroyInstance(instance);
        return 2;
    }

    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(instance, system, &sp))) {
        std::printf("headset: %s (vendor 0x%08x)\n", sp.systemName,
                    sp.vendorId);
        std::printf("  max swapchain   %u x %u, %u layers\n",
                    sp.graphicsProperties.maxSwapchainImageWidth,
                    sp.graphicsProperties.maxSwapchainImageHeight,
                    sp.graphicsProperties.maxLayerCount);
        std::printf("  orientation     %s\n",
                    sp.trackingProperties.orientationTracking ? "tracked"
                                                              : "NO");
        std::printf("  position        %s\n",
                    sp.trackingProperties.positionTracking ? "tracked" : "NO");
    }

    // --- What stereo rendering has to hit ----------------------------------
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(
        instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
        &viewCount, nullptr);
    std::vector<XrViewConfigurationView> views(
        viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (viewCount) {
        xrEnumerateViewConfigurationViews(
            instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            viewCount, &viewCount, views.data());
    }
    std::printf("\nprimary stereo: %u views\n", viewCount);
    for (uint32_t i = 0; i < viewCount; ++i) {
        std::printf("  eye %u  recommended %u x %u (%ux MSAA), max %u x %u\n", i,
                    views[i].recommendedImageRectWidth,
                    views[i].recommendedImageRectHeight,
                    views[i].recommendedSwapchainSampleCount,
                    views[i].maxImageRectWidth, views[i].maxImageRectHeight);
    }

    uint32_t blendCount = 0;
    xrEnumerateEnvironmentBlendModes(
        instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
        &blendCount, nullptr);
    std::vector<XrEnvironmentBlendMode> blends(blendCount);
    if (blendCount) {
        xrEnumerateEnvironmentBlendModes(
            instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            blendCount, &blendCount, blends.data());
    }
    std::printf("blend modes:");
    for (auto b : blends) {
        std::printf(" %s", b == XR_ENVIRONMENT_BLEND_MODE_OPAQUE ? "opaque"
                           : b == XR_ENVIRONMENT_BLEND_MODE_ADDITIVE
                               ? "additive"
                               : "alpha-blend");
    }
    std::printf("\n");

    // --- The Vulkan device the session would force on us -------------------
    if (vk2) {
        auto get = [&](const char* name, PFN_xrVoidFunction* fn) {
            return XR_SUCCEEDED(xrGetInstanceProcAddr(instance, name, fn));
        };
        PFN_xrGetVulkanGraphicsRequirements2KHR req = nullptr;
        if (get("xrGetVulkanGraphicsRequirements2KHR",
                reinterpret_cast<PFN_xrVoidFunction*>(&req)) &&
            req) {
            XrGraphicsRequirementsVulkan2KHR gr{
                XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
            if (XR_SUCCEEDED(req(instance, system, &gr))) {
                std::printf(
                    "\nvulkan required: %u.%u.%u .. %u.%u.%u\n",
                    XR_VERSION_MAJOR(gr.minApiVersionSupported),
                    XR_VERSION_MINOR(gr.minApiVersionSupported),
                    XR_VERSION_PATCH(gr.minApiVersionSupported),
                    XR_VERSION_MAJOR(gr.maxApiVersionSupported),
                    XR_VERSION_MINOR(gr.maxApiVersionSupported),
                    XR_VERSION_PATCH(gr.maxApiVersionSupported));
                std::printf(
                    "  NOTE: the runtime also dictates WHICH physical device\n"
                    "  and which instance/device extensions the session needs,\n"
                    "  so VR rendering has to route pcbview's Vulkan setup\n"
                    "  through OpenXR rather than choosing its own GPU.\n");
            }
        }
    }

    xrDestroyInstance(instance);
    std::printf("\nprobe OK\n");
    return 0;
}

}  // namespace pcbview::xr
