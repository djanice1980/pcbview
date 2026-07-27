#include "xr/xr_system.h"

// Vulkan first, then the XR platform header, which builds its Vulkan structs on
// those types. XR_USE_PLATFORM_WIN32 is deliberately NOT defined -- see
// xr_probe.cpp for why.
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "render/vk/renderer.h"

#include <chrono>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
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

    // The depth layer extension is OPTIONAL -- ask only if the runtime lists it,
    // because naming an unsupported extension fails instance creation outright
    // and VR would stop working entirely rather than losing one feature.
    //
    // It is worth having because it changes what happens when a frame is
    // missed. Without depth the runtime can only reproject the previous frame
    // RIGIDLY, sliding it as a flat sheet, so anything standing proud of the
    // board -- a chip's legs, a package edge -- shears against the background
    // and appears to flicker between two positions. With depth it reprojects
    // per pixel using the actual geometry, and a missed frame degrades into
    // slight smearing instead of the scene coming apart. You can always lean in
    // close enough to miss a deadline, so this matters more than the last of
    // the aliasing.
    bool haveDepthExt = false;
    bool haveMaskExt = false;
    bool haveGazeExt = false;
    bool haveFoveationExt = false;
    {
        uint32_t n = 0;
        xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
        std::vector<XrExtensionProperties> avail(n, {XR_TYPE_EXTENSION_PROPERTIES});
        if (n) xrEnumerateInstanceExtensionProperties(nullptr, n, &n, avail.data());
        for (const auto& e : avail) {
            if (std::strcmp(e.extensionName,
                            XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME) == 0)
                haveDepthExt = true;
            if (std::strcmp(e.extensionName,
                            XR_KHR_VISIBILITY_MASK_EXTENSION_NAME) == 0)
                haveMaskExt = true;
            // Not enabled here -- only reported. Whether eye-tracked foveation
            // is even POSSIBLE on this headset and this connection is a
            // question to answer before designing around it, and the runtime's
            // own extension list is the only authority worth trusting for it.
            if (std::strcmp(e.extensionName, "XR_EXT_eye_gaze_interaction") == 0)
                haveGazeExt = true;
            if (std::strcmp(e.extensionName, "XR_FB_foveation") == 0 ||
                std::strcmp(e.extensionName, "XR_VARJO_foveated_rendering") ==
                    0 ||
                std::strcmp(e.extensionName,
                            "XR_META_foveation_eye_tracked") == 0)
                haveFoveationExt = true;
        }
        std::printf("vr-caps: eye gaze extension %s, runtime foveation %s\n",
                    haveGazeExt ? "offered" : "not offered",
                    haveFoveationExt ? "offered" : "not offered");
        std::fflush(stdout);
    }
    eyeGazeExtension_ = haveGazeExt;

    // The visibility mask is the shape of what the LENSES can actually show.
    // SteamVR puts it at 22% of each eye on this headset -- better than a fifth
    // of every pixel we render is bent away by the optics and never seen. Drawn
    // into depth before anything else, the rasterizer skips those fragments
    // entirely, and on a fill-bound frame that is close to a fifth of the cost
    // back for no visual change whatsoever.
    std::vector<const char*> ext{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
    if (haveDepthExt) ext.push_back(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
    if (haveMaskExt) ext.push_back(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME);
    // Enabled purely so the system-properties query below is legal. Nothing
    // consumes gaze yet; this is what tells us whether it COULD.
    if (haveGazeExt) ext.push_back("XR_EXT_eye_gaze_interaction");
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(ici.applicationInfo.applicationName,
                  sizeof(ici.applicationInfo.applicationName), "pcbview");
    ici.applicationInfo.applicationVersion = 1;
    std::snprintf(ici.applicationInfo.engineName,
                  sizeof(ici.applicationInfo.engineName), "pcbview");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = static_cast<uint32_t>(ext.size());
    ici.enabledExtensionNames = ext.data();
    depthLayerSupported_ = haveDepthExt;
    visibilityMaskSupported_ = haveMaskExt;

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

    // Does the HEADSET actually do eye gaze, or does the runtime merely list
    // the extension?
    //
    // Those are different questions and only the second one is answered by the
    // extension list -- a runtime can offer XR_EXT_eye_gaze_interaction and
    // then report no support for the attached device. Since eye-tracked
    // foveation lives or dies on this, ask the system properties rather than
    // inferring it, and say which of the two we actually established.
    XrSystemEyeGazeInteractionPropertiesEXT gaze{
        XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (eyeGazeExtension_) sp.next = &gaze;
    if (XR_SUCCEEDED(xrGetSystemProperties(inst, sys, &sp)))
        std::snprintf(headsetName_, sizeof(headsetName_), "%s", sp.systemName);
    if (eyeGazeExtension_) {
        eyeGazeSupported_ = gaze.supportsEyeGazeInteraction != 0;
        std::printf("vr-caps: this headset reports eye gaze %s\n",
                    eyeGazeSupported_ ? "SUPPORTED -- eye-tracked foveation is "
                                        "reachable"
                                      : "unsupported (extension listed, device "
                                        "does not provide it)");
        std::fflush(stdout);
    }

    // MANDATORY, not informational. The spec requires the graphics-requirements
    // call for the chosen API before xrCreateSession, and skipping it makes
    // session creation fail with XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING
    // (-50) long after the fact, with nothing else to point at. The returned
    // version range is advisory here -- it is the CALL that is required.
    if (auto req = proc<PFN_xrGetVulkanGraphicsRequirements2KHR>(
            inst, "xrGetVulkanGraphicsRequirements2KHR")) {
        XrGraphicsRequirementsVulkan2KHR gr{
            XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
        req(inst, sys, &gr);
    }

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

// How much of the runtime's recommended eye resolution to actually allocate.
//
// Shared, because it is needed in two places and having two copies is exactly
// how this went wrong: the scale was made adjustable in presentTest and left
// hardcoded at half in the renderer's own path, so the setting appeared to have
// no effect at all while the diagnostic dutifully honoured it.
float eyeResolutionScale() {
    // 0.5 of what the runtime recommends, and that is not a compromise on
    // quality -- it is the difference between ray-traced VR working and not.
    //
    // SteamVR's recommendation for this headset is 4164x4244 an eye against a
    // 2000x2040 panel: it has already applied its own 1.5x supersampling
    // before we see the number. Measured across the ray budget, at full
    // recommendation only plain raster sustained 90 Hz and everything traced
    // was paced down to 45 or 23; at 0.5 every rung of the ladder held 90.
    // 0.5 still lands slightly ABOVE the panel's own resolution, so the
    // headset is not being undersampled -- SteamVR's extra supersampling is
    // simply not affordable alongside shadow and AO rays, and rays are worth
    // far more here than the supersample was.
    static const float scale = [] {
        const char* v = std::getenv("PCBVIEW_VR_RES");
        if (!v || !v[0]) return 0.5f;
        char* end = nullptr;
        const float f = std::strtof(v, &end);
        return (end && end != v && f >= 0.25f && f <= 1.0f) ? f : 0.5f;
    }();
    return scale;
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

namespace {

// Small helpers so the session code below reads as the sequence it is rather
// than as error handling with logic hidden in it.
XrPath path(XrInstance inst, const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(inst, s, &p);
    return p;
}

bool ok(XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    std::printf("  FAILED %s (XrResult %d)\n", what, static_cast<int>(r));
    return false;
}

}  // namespace

int inputTest() {
    std::printf("OpenXR Sense controller input test\n"
                "==================================\n\n");
    System xr;
    if (!xr.start()) return 1;
    std::printf("headset: %s\n", xr.headsetName());

    // Vulkan first, through the runtime -- a session needs a graphics binding
    // even though this test draws nothing.
    xr.installHooks();
    VkInstance vkInstance = VK_NULL_HANDLE;
    Device dev{};
    try {
        vkInstance = createInstance(false, {"VK_KHR_surface",
                                            "VK_KHR_win32_surface"});
        const VkPhysicalDevice want = xr.adoptRuntimeGpu(vkInstance);
        if (want == VK_NULL_HANDLE) {
            std::printf("the runtime named no GPU\n");
            return 2;
        }
        const std::vector<GpuInfo> gpus = enumerateGpus(vkInstance);
        const GpuInfo* chosen = selectGpu(gpus, "");
        if (!chosen) { std::printf("no usable GPU\n"); return 3; }
        dev = createDevice(*chosen, {"VK_KHR_swapchain"});
        std::printf("vulkan ready on %s\n", chosen->name.c_str());
    } catch (const std::exception& e) {
        std::printf("vulkan setup failed: %s\n", e.what());
        return 4;
    }
    xr.removeHooks();

    XrInstance inst = static_cast<XrInstance>(xr.rawInstance());
    const XrSystemId sysId = static_cast<XrSystemId>(xr.rawSystem());

    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = vkInstance;
    binding.physicalDevice = dev.gpu.handle;
    binding.device = dev.handle;
    binding.queueFamilyIndex = dev.gpu.graphicsQueueFamily;
    binding.queueIndex = 0;

    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = sysId;
    XrSession session = XR_NULL_HANDLE;
    if (!ok(xrCreateSession(inst, &sci, &session), "xrCreateSession")) return 5;
    std::printf("session created\n");

    // LOCAL is seated-origin and always available; STAGE needs a room setup
    // that a desk-bound PSVR2 may not have.
    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace stage = XR_NULL_HANDLE;
    ok(xrCreateReferenceSpace(session, &rsci, &stage), "reference space");

    // --- Actions -----------------------------------------------------------
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(asci.actionSetName, sizeof(asci.actionSetName), "board");
    std::snprintf(asci.localizedActionSetName,
                  sizeof(asci.localizedActionSetName), "Board handling");
    XrActionSet actionSet = XR_NULL_HANDLE;
    if (!ok(xrCreateActionSet(inst, &asci, &actionSet), "action set")) return 6;

    XrPath hands[2] = {path(inst, "/user/hand/left"),
                       path(inst, "/user/hand/right")};

    XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
    std::snprintf(aci.actionName, sizeof(aci.actionName), "grip_pose");
    std::snprintf(aci.localizedActionName, sizeof(aci.localizedActionName),
                  "Grip pose");
    aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
    aci.countSubactionPaths = 2;
    aci.subactionPaths = hands;
    XrAction gripPose = XR_NULL_HANDLE;
    ok(xrCreateAction(actionSet, &aci, &gripPose), "grip pose action");

    XrActionCreateInfo sci2{XR_TYPE_ACTION_CREATE_INFO};
    std::snprintf(sci2.actionName, sizeof(sci2.actionName), "grab");
    std::snprintf(sci2.localizedActionName, sizeof(sci2.localizedActionName),
                  "Grab the board");
    sci2.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    sci2.countSubactionPaths = 2;
    sci2.subactionPaths = hands;
    XrAction grab = XR_NULL_HANDLE;
    ok(xrCreateAction(actionSet, &sci2, &grab), "grab action");

    // Bind against the simple controller profile. SteamVR re-targets its own
    // bindings onto whatever the physical device is, so this reaches the Sense
    // controllers without needing a PSVR2-specific profile.
    const XrActionSuggestedBinding binds[] = {
        {gripPose, path(inst, "/user/hand/left/input/grip/pose")},
        {gripPose, path(inst, "/user/hand/right/input/grip/pose")},
        {grab, path(inst, "/user/hand/left/input/select/click")},
        {grab, path(inst, "/user/hand/right/input/select/click")},
    };
    XrInteractionProfileSuggestedBinding prof{
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    prof.interactionProfile =
        path(inst, "/interaction_profiles/khr/simple_controller");
    prof.suggestedBindings = binds;
    prof.countSuggestedBindings = 4;
    ok(xrSuggestInteractionProfileBindings(inst, &prof), "suggest bindings");

    XrSessionActionSetsAttachInfo attach{
        XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &actionSet;
    ok(xrAttachSessionActionSets(session, &attach), "attach action sets");

    XrSpace handSpace[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    for (int i = 0; i < 2; ++i) {
        XrActionSpaceCreateInfo asp{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        asp.action = gripPose;
        asp.subactionPath = hands[i];
        asp.poseInActionSpace.orientation.w = 1.0f;
        ok(xrCreateActionSpace(session, &asp, &handSpace[i]), "action space");
    }

    // --- Frame loop --------------------------------------------------------
    // Actions only deliver input once the runtime takes the app to FOCUSED.
    // Reaching that is the runtime's call, and it generally wants the headset
    // to be WORN and the app to be the active scene -- so the state transitions
    // are logged here, because "no input" and "never got focus" look identical
    // from the outside and have completely different fixes.
    static const char* kStates[] = {"UNKNOWN",      "IDLE",    "READY",
                                    "SYNCHRONIZED", "VISIBLE", "FOCUSED",
                                    "STOPPING",     "LOSS_PENDING", "EXITING"};
    std::printf("\nrunning ~25s -- PUT THE HEADSET ON and hold a Sense "
                "controller\n(input needs the session to reach FOCUSED)\n\n");
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    int printed = 0;
    for (int frame = 0; frame < 2600 && printed < 18; ++frame) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto* s = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                state = s->state;
                const int si = static_cast<int>(state);
                std::printf("  [state] %s\n",
                            (si >= 0 && si < 9) ? kStates[si] : "?");
                std::fflush(stdout);
                if (state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType =
                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (ok(xrBeginSession(session, &bi), "xrBeginSession"))
                        running = true;
                } else if (state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session);
                    running = false;
                }
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (!running) continue;

        XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        if (XR_FAILED(xrWaitFrame(session, &fwi, &fs))) break;
        XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
        xrBeginFrame(session, &fbi);

        XrActiveActionSet active{actionSet, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        xrSyncActions(session, &sync);

        // Report regardless of shouldRender: when the runtime is holding the
        // app below FOCUSED, shouldRender is false and gating on it hides the
        // very state we are trying to diagnose.
        if ((frame % 90) == 0) {
            for (int i = 0; i < 2; ++i) {
                XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
                if (XR_FAILED(xrLocateSpace(handSpace[i], stage,
                                            fs.predictedDisplayTime, &loc)))
                    continue;
                const bool tracked =
                    (loc.locationFlags &
                     XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
                XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
                gi.action = grab;
                gi.subactionPath = hands[i];
                XrActionStateBoolean gs{XR_TYPE_ACTION_STATE_BOOLEAN};
                xrGetActionStateBoolean(session, &gi, &gs);
                std::printf(
                    "  %-5s %s pos(%+6.3f %+6.3f %+6.3f) "
                    "quat(%+5.2f %+5.2f %+5.2f %+5.2f) grab=%d\n",
                    i == 0 ? "left" : "right", tracked ? "TRACKED" : "  ---  ",
                    loc.pose.position.x, loc.pose.position.y,
                    loc.pose.position.z, loc.pose.orientation.x,
                    loc.pose.orientation.y, loc.pose.orientation.z,
                    loc.pose.orientation.w,
                    gs.isActive && gs.currentState ? 1 : 0);
            }
            std::fflush(stdout);
            ++printed;
        }

        // Zero layers: legal, and it is what keeps the session alive without
        // drawing anything.
        XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = 0;
        xrEndFrame(session, &fei);
    }

    for (XrSpace s : handSpace) if (s) xrDestroySpace(s);
    if (stage) xrDestroySpace(stage);
    xrDestroyActionSet(actionSet);
    if (running) xrEndSession(session);
    xrDestroySession(session);
    destroyDevice(dev);
    vkDestroyInstance(vkInstance, nullptr);
    xr.stop();
    std::printf("\ninput test done (session state %d)\n",
                static_cast<int>(state));
    return 0;
}

// ---- VrSession --------------------------------------------------------------

struct VrSession::Chain {
    XrSwapchain chain = XR_NULL_HANDLE;
    std::vector<VkImage> images;
    uint32_t width = 0, height = 0;
    // Matching depth swapchain, when the runtime supports the depth layer.
    XrSwapchain depthChain = XR_NULL_HANDLE;
    std::vector<VkImage> depthImages;
    std::vector<VkImageView> depthViews;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    uint32_t depthAcquired = 0;
};

namespace {

XrSession asSession(void* p) { return static_cast<XrSession>(p); }

bool qEnvSwapEyes() {
    const char* v = std::getenv("PCBVIEW_VR_SWAP_EYES");
    return v && v[0] && v[0] != '0';
}

// pcbview's projection convention, made ASYMMETRIC.
//
// The desktop path builds an infinite reverse-Z frustum (see
// infiniteReverseZPerspective): p[0][0]=f/aspect, p[1][1]=f, p[2][3]=-1,
// p[3][2]=zNear, then flips Y for Vulkan. OpenXR hands back four separate FOV
// half-angles per eye instead of one symmetric fovY -- VR lenses are not
// centred -- so the scale terms gain matching OFFSETS. Matching the existing
// convention exactly matters: a frustum that merely looks reasonable renders
// plausibly and reads as wrong depth through the lenses.
//
// The VR frustum's two ends, in ROOM metres. One definition each, because the
// depth layer has to declare the same pair to the runtime and a copy that
// drifted would describe a buffer we are not producing -- which the compositor
// would then reproject wrongly, silently.
constexpr float kVrNear = 0.01f;
constexpr float kVrFar = 100.0f;

// glm is column-major, so p[col][row].
void projectionFromFov(const XrFovf& fov, float zNear, float out[16]) {
    const float tanL = std::tan(fov.angleLeft);
    const float tanR = std::tan(fov.angleRight);
    const float tanU = std::tan(fov.angleUp);
    const float tanD = std::tan(fov.angleDown);
    const float w = tanR - tanL;
    const float h = tanU - tanD;

    float p[16] = {};  // column-major, all zero
    p[0] = 2.0f / w;                 // p[0][0]
    p[5] = 2.0f / h;                 // p[1][1]
    p[8] = (tanR + tanL) / w;        // p[2][0]  x offset
    p[9] = (tanU + tanD) / h;        // p[2][1]  y offset
    // Reverse-Z with a FINITE far plane, and the finiteness is the point.
    //
    // This was an infinite reverse-Z projection: p[10] = 0, p[14] = zNear, so
    // depth came out zNear/d -- 1 at the near plane, 0 at infinity. Correct,
    // and free of a far plane to tune. But the depth layer has to tell the
    // runtime what distance each end of the buffer means, and with infinity at
    // minDepth that makes nearZ infinite. The spec allows farZ to be infinite;
    // it never allows nearZ to be, and the usual linearisation
    //
    //     d = nearZ*farZ / (farZ + D*(nearZ - farZ))
    //
    // is inf/inf at nearZ = infinity. It is the right answer only as a limit,
    // and a runtime that does not special-case it gets a NaN and reprojects
    // with garbage. That matches what was measured: with depth submitted the
    // image swims, with PCBVIEW_VR_DEPTH=0 it is steadier, because without
    // depth the compositor falls back to a rigid warp it cannot get wrong.
    //
    // A finite far plane makes both ends ordinary numbers. Solving for
    // ndc.z = 1 at zNear and 0 at zFar gives the two terms below. Reverse-Z
    // keeps its precision either way -- the float exponent does the work, not
    // the far plane -- and 100 m is past anything a board on a desk needs.
    p[10] = zNear / (kVrFar - zNear);            // p[2][2]
    p[11] = -1.0f;                               // p[2][3]
    p[14] = zNear * kVrFar / (kVrFar - zNear);   // p[3][2]  reverse-Z
    // Vulkan's Y is flipped. clip.y is p[1][1]*y + p[2][1]*z, so BOTH terms
    // flip -- negating only the scale, as the symmetric path can get away with
    // because it has no offset, would shear the image off-centre.
    p[5] = -p[5];
    p[9] = -p[9];
    // NO X mirror, and NO negation of p[8]. Both were added while chasing the
    // flipped-eye report and both were wrong; this is back to the original.
    //
    // Negating p[8] is not a coin flip between two plausible conventions -- it
    // is precisely an eye swap, which is why it looked like a candidate and why
    // it had to go. Check it against this headset's own numbers. Solving
    // clip.x/clip.w = -1 and +1 for the rendered angular span gives
    // atan(p[8] +/- 1) / (2/w) ... in plain terms, with eye 0's real FOV
    // (L -61.5 deg, R +43.4 deg) the correct matrix renders -61.5..+43.4, and
    // negating p[8] renders -43.4..+61.6 -- exactly eye 1's frustum. So the
    // negated form hands each eye the OTHER eye's view of the world.
    //
    // Verify, don't trust the sign: at the left frustum edge x = tanL*d, z = -d,
    //   clip.x = (2/w)(tanL*d) - ((tanR+tanL)/w)*d = (d/w)(tanL - tanR) = -d
    // and clip.w = -z = d, so NDC x = -1. The unnegated term is correct.

    // PCBVIEW_VR_NO_CANT=1 zeroes the frustum skew, centring both eyes' views
    // straight ahead. This is deliberately NOT physically correct -- the
    // rendered frustum stops matching the fov we declare in the layer -- but it
    // isolates one thing cleanly.
    //
    // Measured on hardware, the two eyes' images are separated by 0.718 in NDC:
    // 0.648 of that is this skew, which exists because the lens centres are
    // offset from the panel centres and which the optics are supposed to undo,
    // and only 0.070 is the real parallax of a board 0.6 m away seen from eyes
    // 63 mm apart. If the skew is NOT being undone downstream, the eyes are
    // asked to diverge past infinity and the image cannot fuse -- which is what
    // "each eye has the wrong image" feels like from the inside.
    //
    // With this set the board lands near centre in both eyes and the remaining
    // disparity is just that 0.070. If it suddenly fuses into one solid board,
    // the fault is in how the skew reaches the panels, not in our geometry.
    static const bool noCant = [] {
        const char* v = std::getenv("PCBVIEW_VR_NO_CANT");
        return v && v[0] && v[0] != '0';
    }();
    if (noCant) p[8] = 0.0f;

    for (int i = 0; i < 16; ++i) out[i] = p[i];
}

// One eye's swapchain and the Vulkan images behind it.
struct EyeChain {
    XrSwapchain chain = XR_NULL_HANDLE;
    std::vector<VkImage> images;
    uint32_t width = 0, height = 0;
};

// The runtime hands back images in an undefined layout and expects them back as
// colour attachments. Clearing needs TRANSFER_DST in between, so every frame
// walks UNDEFINED -> TRANSFER_DST -> COLOR_ATTACHMENT.
void barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from,
             VkImageLayout to, VkAccessFlags srcAccess,
             VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
}

}  // namespace

int presentTest() {
    std::printf("OpenXR presentation test\n========================\n\n");
    System xr;
    if (!xr.start()) return 1;
    std::printf("headset: %s\n", xr.headsetName());

    xr.installHooks();
    VkInstance vkInstance = VK_NULL_HANDLE;
    Device dev{};
    try {
        vkInstance = createInstance(false, {"VK_KHR_surface",
                                            "VK_KHR_win32_surface"});
        if (xr.adoptRuntimeGpu(vkInstance) == VK_NULL_HANDLE) {
            std::printf("the runtime named no GPU\n");
            return 2;
        }
        const std::vector<GpuInfo> gpus = enumerateGpus(vkInstance);
        const GpuInfo* chosen = selectGpu(gpus, "");
        if (!chosen) { std::printf("no usable GPU\n"); return 3; }
        dev = createDevice(*chosen, {"VK_KHR_swapchain"});
    } catch (const std::exception& e) {
        std::printf("vulkan setup failed: %s\n", e.what());
        return 4;
    }
    xr.removeHooks();

    XrInstance inst = static_cast<XrInstance>(xr.rawInstance());
    const XrSystemId sysId = static_cast<XrSystemId>(xr.rawSystem());

    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = vkInstance;
    binding.physicalDevice = dev.gpu.handle;
    binding.device = dev.handle;
    binding.queueFamilyIndex = dev.gpu.graphicsQueueFamily;
    binding.queueIndex = 0;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = sysId;
    XrSession session = XR_NULL_HANDLE;
    if (!ok(xrCreateSession(inst, &sci, &session), "xrCreateSession")) return 5;

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace appSpace = XR_NULL_HANDLE;
    ok(xrCreateReferenceSpace(session, &rsci, &appSpace), "reference space");

    // --- Swapchains --------------------------------------------------------
    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(session, 0, &fmtCount, nullptr);
    std::vector<int64_t> formats(fmtCount);
    xrEnumerateSwapchainFormats(session, fmtCount, &fmtCount, formats.data());
    int64_t format = formats.empty() ? 0 : formats[0];
    for (int64_t f : formats) {  // prefer a plain sRGB colour target
        if (f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB) {
            format = f;
            break;
        }
    }
    std::printf("swapchain format: %lld\n", static_cast<long long>(format));

    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(
        inst, sysId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount,
        nullptr);
    std::vector<XrViewConfigurationView> cfg(
        viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(
        inst, sysId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount,
        &viewCount, cfg.data());

    // The runtime's recommendation, in full, by default.
    //
    // This was fixed at HALF, on the reasoning that 4164x4244 per eye is ~35 MP
    // a frame across both and no renderer holds that at 90 Hz. True of a path
    // tracer at a hundred samples; badly wrong for ray-traced raster, and it
    // was never revisited.
    //
    // Halving the swapchain does not merely soften the image -- it caps what
    // can be DELIVERED. Every frame was downscaled into a quarter-resolution
    // buffer, handed to the compositor, and stretched back up to the panel.
    // Which is why supersampling barely helped: at 2x the scene was already
    // being rendered at 4164x4244 and then thrown away at this line, paying
    // full price for the pixels and delivering a quarter of them. On a headset
    // already about three times short of visual acuity, that is the difference
    // between "aliased mess" and merely "aliased".
    //
    // PCBVIEW_VR_RES scales it: 0.5 restores the old behaviour if a frame
    // budget needs the room back.
    const float kScale = eyeResolutionScale();
    std::vector<EyeChain> eyes(viewCount);
    for (uint32_t i = 0; i < viewCount; ++i) {
        eyes[i].width =
            static_cast<uint32_t>(cfg[i].recommendedImageRectWidth * kScale);
        eyes[i].height =
            static_cast<uint32_t>(cfg[i].recommendedImageRectHeight * kScale);
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                        XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = format;
        ci.sampleCount = 1;
        ci.width = eyes[i].width;
        ci.height = eyes[i].height;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        if (!ok(xrCreateSwapchain(session, &ci, &eyes[i].chain), "swapchain"))
            return 6;

        uint32_t n = 0;
        xrEnumerateSwapchainImages(eyes[i].chain, 0, &n, nullptr);
        std::vector<XrSwapchainImageVulkanKHR> imgs(
            n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(
            eyes[i].chain, n, &n,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
        for (const auto& im : imgs) eyes[i].images.push_back(im.image);
        std::printf("eye %u: %u x %u, %u images\n", i, eyes[i].width,
                    eyes[i].height, n);
    }

    // --- Vulkan command recording -----------------------------------------
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = dev.gpu.graphicsQueueFamily;
    vkCreateCommandPool(dev.handle, &pci, nullptr, &pool);
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbi{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev.handle, &cbi, &cmd);
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(dev.handle, &fci, nullptr, &fence);

    // --- Actions (identical to the input test) -----------------------------
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(asci.actionSetName, sizeof(asci.actionSetName), "board");
    std::snprintf(asci.localizedActionSetName,
                  sizeof(asci.localizedActionSetName), "Board handling");
    XrActionSet actionSet = XR_NULL_HANDLE;
    xrCreateActionSet(inst, &asci, &actionSet);
    XrPath hands[2] = {path(inst, "/user/hand/left"),
                       path(inst, "/user/hand/right")};
    XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
    std::snprintf(aci.actionName, sizeof(aci.actionName), "grip_pose");
    std::snprintf(aci.localizedActionName, sizeof(aci.localizedActionName),
                  "Grip pose");
    aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
    aci.countSubactionPaths = 2;
    aci.subactionPaths = hands;
    XrAction gripPose = XR_NULL_HANDLE;
    xrCreateAction(actionSet, &aci, &gripPose);
    const XrActionSuggestedBinding binds[] = {
        {gripPose, path(inst, "/user/hand/left/input/grip/pose")},
        {gripPose, path(inst, "/user/hand/right/input/grip/pose")},
    };
    XrInteractionProfileSuggestedBinding prof{
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    prof.interactionProfile =
        path(inst, "/interaction_profiles/khr/simple_controller");
    prof.suggestedBindings = binds;
    prof.countSuggestedBindings = 2;
    xrSuggestInteractionProfileBindings(inst, &prof);
    XrSessionActionSetsAttachInfo attach{
        XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &actionSet;
    xrAttachSessionActionSets(session, &attach);
    XrSpace handSpace[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    for (int i = 0; i < 2; ++i) {
        XrActionSpaceCreateInfo asp{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        asp.action = gripPose;
        asp.subactionPath = hands[i];
        asp.poseInActionSpace.orientation.w = 1.0f;
        xrCreateActionSpace(session, &asp, &handSpace[i]);
    }

    // --- Frame loop --------------------------------------------------------
    static const char* kStates[] = {"UNKNOWN",      "IDLE",    "READY",
                                    "SYNCHRONIZED", "VISIBLE", "FOCUSED",
                                    "STOPPING",     "LOSS_PENDING", "EXITING"};
    // Runs long enough to actually put the headset on. The runtime holds an app
    // at SYNCHRONIZED while the HMD is off the head -- a proximity standby --
    // and only promotes to VISIBLE/FOCUSED once it is worn, so a test that
    // finishes before you can pick the thing up proves nothing.
    std::printf("\nPUT THE HEADSET ON NOW -- waiting up to 60s for the runtime\n"
                "to grant focus. You should see a slowly pulsing colour, and\n"
                "the controllers should start reporting.\n\n");
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    int reports = 0;
    for (int frame = 0; frame < 5400 && reports < 12; ++frame) {
        if ((frame % 270) == 0 && state != XR_SESSION_STATE_FOCUSED) {
            std::printf("  ... waiting, still %s\n",
                        (static_cast<int>(state) >= 0 &&
                         static_cast<int>(state) < 9)
                            ? kStates[static_cast<int>(state)]
                            : "?");
            std::fflush(stdout);
        }
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                state = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev)
                            ->state;
                const int si = static_cast<int>(state);
                std::printf("  [state] %s\n",
                            (si >= 0 && si < 9) ? kStates[si] : "?");
                std::fflush(stdout);
                if (state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType =
                        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    running = ok(xrBeginSession(session, &bi), "beginSession");
                } else if (state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session);
                    running = false;
                }
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (!running) continue;

        XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState fs{XR_TYPE_FRAME_STATE};
        if (XR_FAILED(xrWaitFrame(session, &fwi, &fs))) break;
        XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
        xrBeginFrame(session, &fbi);

        XrActiveActionSet active{actionSet, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        xrSyncActions(session, &sync);

        std::vector<XrCompositionLayerProjectionView> projViews;
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};

        if (fs.shouldRender) {
            XrViewState vs{XR_TYPE_VIEW_STATE};
            uint32_t got = 0;
            std::vector<XrView> views(viewCount, {XR_TYPE_VIEW});
            XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
            vli.viewConfigurationType =
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = appSpace;
            xrLocateViews(session, &vli, &vs, viewCount, &got, views.data());

            const float t = static_cast<float>(frame) * 0.02f;
            const float pulse = 0.25f + 0.2f * (0.5f + 0.5f * std::sin(t));

            for (uint32_t i = 0; i < got; ++i) {
                uint32_t idx = 0;
                XrSwapchainImageAcquireInfo ai{
                    XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                if (XR_FAILED(xrAcquireSwapchainImage(eyes[i].chain, &ai, &idx)))
                    continue;
                XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wi.timeout = XR_INFINITE_DURATION;
                xrWaitSwapchainImage(eyes[i].chain, &wi);

                vkResetCommandBuffer(cmd, 0);
                VkCommandBufferBeginInfo bi{
                    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(cmd, &bi);
                VkImage img = eyes[i].images[idx];
                barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                        VK_ACCESS_TRANSFER_WRITE_BIT);
                // Eyes tinted differently so a wrong-eye mix-up would be
                // obvious rather than subtle.
                VkClearColorValue col{};
                col.float32[0] = i == 0 ? pulse : pulse * 0.35f;
                col.float32[1] = pulse * 0.55f;
                col.float32[2] = i == 0 ? pulse * 0.35f : pulse;
                col.float32[3] = 1.0f;
                VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1,
                                              0, 1};
                vkCmdClearColorImage(cmd, img,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col,
                                     1, &range);
                barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
                vkEndCommandBuffer(cmd);

                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                si.commandBufferCount = 1;
                si.pCommandBuffers = &cmd;
                vkQueueSubmit(dev.graphicsQueue, 1, &si, fence);
                vkWaitForFences(dev.handle, 1, &fence, VK_TRUE, UINT64_MAX);
                vkResetFences(dev.handle, 1, &fence);

                XrSwapchainImageReleaseInfo ri{
                    XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(eyes[i].chain, &ri);

                XrCompositionLayerProjectionView pv{
                    XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                pv.pose = views[i].pose;
                pv.fov = views[i].fov;
                pv.subImage.swapchain = eyes[i].chain;
                pv.subImage.imageRect.offset = {0, 0};
                pv.subImage.imageRect.extent = {
                    static_cast<int32_t>(eyes[i].width),
                    static_cast<int32_t>(eyes[i].height)};
                pv.subImage.imageArrayIndex = 0;
                projViews.push_back(pv);
            }
            layer.space = appSpace;
            layer.viewCount = static_cast<uint32_t>(projViews.size());
            layer.views = projViews.data();
        }

        if ((frame % 120) == 0 && state == XR_SESSION_STATE_FOCUSED) {
            for (int i = 0; i < 2; ++i) {
                // WHICH profile the runtime actually bound is the thing worth
                // knowing: XR_NULL_PATH means no live controller on that hand
                // (asleep, or off), whereas a profile name with an inactive
                // action means the controller is there but our bindings did not
                // take. Those look identical from the pose alone.
                XrInteractionProfileState ips{
                    XR_TYPE_INTERACTION_PROFILE_STATE};
                char profName[XR_MAX_PATH_LENGTH] = "none";
                if (XR_SUCCEEDED(
                        xrGetCurrentInteractionProfile(session, hands[i], &ips)) &&
                    ips.interactionProfile != XR_NULL_PATH) {
                    uint32_t len = 0;
                    xrPathToString(inst, ips.interactionProfile,
                                   sizeof(profName), &len, profName);
                }

                XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
                gi.action = gripPose;
                gi.subactionPath = hands[i];
                XrActionStatePose ps{XR_TYPE_ACTION_STATE_POSE};
                xrGetActionStatePose(session, &gi, &ps);

                XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
                xrLocateSpace(handSpace[i], appSpace, fs.predictedDisplayTime,
                              &loc);
                const bool tracked =
                    (loc.locationFlags &
                     XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
                std::printf("  %-5s %s action=%s pos(%+6.3f %+6.3f %+6.3f) "
                            "profile=%s\n",
                            i == 0 ? "left" : "right",
                            tracked ? "TRACKED" : "  ---  ",
                            ps.isActive ? "active" : "INACTIVE",
                            loc.pose.position.x, loc.pose.position.y,
                            loc.pose.position.z, profName);
            }
            std::fflush(stdout);
            ++reports;
        }

        const XrCompositionLayerBaseHeader* layers[] = {
            reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
        XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = projViews.empty() ? 0 : 1;
        fei.layers = projViews.empty() ? nullptr : layers;
        xrEndFrame(session, &fei);
    }

    vkDeviceWaitIdle(dev.handle);
    vkDestroyFence(dev.handle, fence, nullptr);
    vkDestroyCommandPool(dev.handle, pool, nullptr);
    for (auto& e : eyes) if (e.chain) xrDestroySwapchain(e.chain);
    for (XrSpace s : handSpace) if (s) xrDestroySpace(s);
    if (appSpace) xrDestroySpace(appSpace);
    xrDestroyActionSet(actionSet);
    if (running) xrEndSession(session);
    xrDestroySession(session);
    destroyDevice(dev);
    vkDestroyInstance(vkInstance, nullptr);
    xr.stop();
    std::printf("\npresent test done (final state %d)\n",
                static_cast<int>(state));
    return 0;
}

// ---- VrSession implementation ----------------------------------------------

namespace {
// Board (millimetres) -> room (metres). Everything the renderer is handed goes
// through this, so the board arrives in front of the viewer at a sane size
// instead of being a 50-metre slab centred on their head.
glm::mat4 placement(const float centre[3], float scale,
                    const glm::vec3& anchorPos, const glm::quat& anchorRot) {
    // Anchored to WHERE THE VIEWER ACTUALLY IS, not to a fixed offset in LOCAL
    // space. LOCAL's origin is wherever the headset happened to be when the
    // session started, so a constant offset parks the board in some corner of
    // the room -- measured at 1.3 m away and 0.7 m above eye level, which is
    // exactly the sliver-through-a-wide-frustum view that was being reported.
    //
    // Yaw only: taking the full head rotation would tilt the board with every
    // glance, and rolling a PCB because you tipped your head is nauseating.
    const glm::vec3 fwd = anchorRot * glm::vec3(0.0f, 0.0f, -1.0f);
    // atan2(-x, -z), and the negations are not cosmetic. angleAxis(t, +Y)
    // maps (0,0,-1) to (-sin t, 0, -cos t), so matching the facing direction
    // needs sin t = -fwd.x and cos t = -fwd.z. Using atan2(fwd.x, -fwd.z)
    // yields the yaw MIRRORED about the Z axis, which for anyone not facing
    // -Z puts the board BEHIND them -- facing +X it landed at -0.6 on X,
    // exactly backwards. A board seen from behind has its left and right
    // exchanged in BOTH eyes, which is why swapping the eye images, mirroring
    // the projection and flipping the frustum offset all changed nothing:
    // none of them address standing on the wrong side of the thing.
    const float yaw = std::atan2(-fwd.x, -fwd.z);
    const glm::quat flat = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));

    glm::mat4 m(1.0f);
    m = glm::translate(m, anchorPos);
    m = m * glm::mat4_cast(flat);
    // How far in front. PCBVIEW_VR_DIST, in metres.
    //
    // Closer is not only a preference: the board covers more of the view, so
    // every feature spans more pixels and aliases less. Convergence strain
    // starts to bite below about 0.2 m, so that is the floor.
    //
    // 0.4 m rather than the 0.3 m this started at, and the reason is measured
    // rather than taste. Frame pacing tracked distance directly: up close the
    // frame rate fell and the compositor reprojected hard, and it recovered on
    // backing away. That is a fill-bound frame -- the nearer the board, the
    // more of each eye it covers, and every covered pixel carries a shadow and
    // AO trace. Distance is a fill-rate control here, not just comfort.
    static const float dist = [] {
        const char* v = std::getenv("PCBVIEW_VR_DIST");
        if (!v || !v[0]) return 0.40f;
        char* end = nullptr;
        const float f = std::strtof(v, &end);
        return (end && end != v && f >= 0.15f && f <= 3.0f) ? f : 0.40f;
    }();
    m = glm::translate(m, glm::vec3(0.0f, 0.0f, -dist));
    m = glm::scale(m, glm::vec3(scale));
    m = glm::translate(m, -glm::vec3(centre[0], centre[1], centre[2]));
    return m;
}
glm::mat4 poseMatrix(const XrPosef& p) {
    const glm::quat q(p.orientation.w, p.orientation.x, p.orientation.y,
                      p.orientation.z);
    return glm::translate(glm::mat4(1.0f),
                          glm::vec3(p.position.x, p.position.y, p.position.z)) *
           glm::mat4_cast(q);
}
}  // namespace

VrSession::~VrSession() { end(); }

void VrSession::setBoardPlacement(const float centreMm[3], float spanMm) {
    for (int i = 0; i < 3; ++i) placeCentre_[i] = centreMm[i];
    // How big across, whatever the board actually measures.
    // PCBVIEW_VR_SIZE, in metres -- independent of distance, so the two can be
    // traded: nearer at the same size fills more of the view, nearer at a
    // smaller size keeps the same apparent size but improves stereo depth.
    static const float sizeM = [] {
        const char* v = std::getenv("PCBVIEW_VR_SIZE");
        if (!v || !v[0]) return 0.35f;
        char* end = nullptr;
        const float f = std::strtof(v, &end);
        return (end && end != v && f >= 0.05f && f <= 2.0f) ? f : 0.35f;
    }();
    placeScale_ = spanMm > 1.0f ? sizeM / spanMm : 0.001f;
}

bool VrSession::begin(System& sys, VkInstance vkInstance, VkDevice device,
                      VkPhysicalDevice gpu, uint32_t queueFamily,
                      VkQueue /*queue*/) {
    if (!sys.ready()) return false;
    inst_ = sys.rawInstance();
    sysId_ = sys.rawSystem();
    depthSupported_ = sys.depthLayerSupported();
    // PCBVIEW_VR_DEPTH=0 submits colour alone, exactly as before the depth
    // layer existed. A switch rather than a rebuild because the depth layer is
    // a prime suspect for reprojection artifacts and has never been tested in
    // isolation: with depth, the compositor warps per pixel using our buffer,
    // so a buffer it reads wrongly warps the image wrongly. Toggling this is
    // the one-minute experiment that tells us whether depth is helping or
    // hurting, instead of reasoning about it.
    if (const char* d = std::getenv("PCBVIEW_VR_DEPTH"))
        if (d[0] == '0') {
            depthSupported_ = false;
            std::printf("vr: depth layer DISABLED by PCBVIEW_VR_DEPTH=0\n");
        }
    maskSupported_ = sys.visibilityMaskSupported();
    device_ = device;
    XrInstance inst = static_cast<XrInstance>(inst_);

    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = vkInstance;
    binding.physicalDevice = gpu;
    binding.device = device;
    binding.queueFamilyIndex = queueFamily;
    binding.queueIndex = 0;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = static_cast<XrSystemId>(sysId_);
    XrSession s = XR_NULL_HANDLE;
    if (!ok(xrCreateSession(inst, &sci, &s), "xrCreateSession")) return false;
    session_ = s;

    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    XrSpace sp = XR_NULL_HANDLE;
    ok(xrCreateReferenceSpace(s, &rsci, &sp), "reference space");
    appSpace_ = sp;

    // Swapchains at half the runtime's recommendation -- see presentTest.
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(
        inst, static_cast<XrSystemId>(sysId_),
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
    std::vector<XrViewConfigurationView> cfg(
        viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(
        inst, static_cast<XrSystemId>(sysId_),
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount,
        cfg.data());

    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(s, 0, &fmtCount, nullptr);
    std::vector<int64_t> formats(fmtCount);
    xrEnumerateSwapchainFormats(s, fmtCount, &fmtCount, formats.data());
    int64_t format = formats.empty() ? VK_FORMAT_R8G8B8A8_SRGB : formats[0];
    for (int64_t f : formats)
        if (f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB) {
            format = f;
            break;
        }

    chains_ = new std::vector<Chain>(viewCount);
    for (uint32_t i = 0; i < viewCount; ++i) {
        Chain& c = (*chains_)[i];
        // THE live path. presentTest has its own copy of this sizing, and the
        // scale factor was only fixed there first -- so the setting appeared to
        // do nothing, because the diagnostic honoured it and the renderer did
        // not. Both read the same helper now.
        c.width = static_cast<uint32_t>(cfg[i].recommendedImageRectWidth *
                                        eyeResolutionScale());
        c.height = static_cast<uint32_t>(cfg[i].recommendedImageRectHeight *
                                         eyeResolutionScale());
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                        XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = format;
        ci.sampleCount = 1;
        ci.width = c.width;
        ci.height = c.height;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        if (!ok(xrCreateSwapchain(s, &ci, &c.chain), "swapchain")) return false;
        uint32_t n = 0;
        xrEnumerateSwapchainImages(c.chain, 0, &n, nullptr);
        std::vector<XrSwapchainImageVulkanKHR> imgs(
            n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(
            c.chain, n, &n,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
        for (const auto& im : imgs) c.images.push_back(im.image);

        // Matching DEPTH swapchain. Optional in every sense: the extension may
        // not exist, the runtime may offer no depth format, and creation may
        // fail -- any of which just means we submit colour alone, exactly as
        // before. Nothing else depends on it.
        if (depthSupported_) {
            // D32_SFLOAT only, and not merely as a preference: the scene
            // pipelines are built against the renderer's own depth format, and
            // rendering into an attachment of a different format is invalid.
            // If the runtime cannot offer it we simply submit colour alone.
            int64_t dfmt = 0;
            for (int64_t f : formats)
                if (f == VK_FORMAT_D32_SFLOAT) {
                    dfmt = f;
                    break;
                }
            if (dfmt) {
                XrSwapchainCreateInfo dci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
                dci.usageFlags =
                    XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
                dci.format = dfmt;
                dci.sampleCount = 1;
                dci.width = c.width;
                dci.height = c.height;
                dci.faceCount = 1;
                dci.arraySize = 1;
                dci.mipCount = 1;
                if (XR_SUCCEEDED(xrCreateSwapchain(s, &dci, &c.depthChain))) {
                    c.depthFormat = static_cast<VkFormat>(dfmt);
                    uint32_t dn = 0;
                    xrEnumerateSwapchainImages(c.depthChain, 0, &dn, nullptr);
                    std::vector<XrSwapchainImageVulkanKHR> dimgs(
                        dn, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
                    xrEnumerateSwapchainImages(
                        c.depthChain, dn, &dn,
                        reinterpret_cast<XrSwapchainImageBaseHeader*>(
                            dimgs.data()));
                    // Views made ONCE here, not per frame: the runtime's images
                    // are fixed for the session, and creating a view every
                    // frame would be a leak in a 90 Hz loop.
                    for (const auto& im : dimgs) {
                        c.depthImages.push_back(im.image);
                        VkImageViewCreateInfo vi{
                            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                        vi.image = im.image;
                        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
                        vi.format = c.depthFormat;
                        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1,
                                               0, 1};
                        VkImageView view = VK_NULL_HANDLE;
                        vkCreateImageView(device, &vi, nullptr, &view);
                        c.depthViews.push_back(view);
                    }
                } else {
                    c.depthChain = XR_NULL_HANDLE;
                }
            }
        }
    }
    if (depthSupported_ && !(*chains_)[0].depthImages.empty())
        std::printf("vr: submitting depth (reprojection follows geometry)\n");
    acquired_.assign(viewCount, 0);
    swapEyes_ = qEnvSwapEyes();

    // Grip poses, bound through the simple-controller profile -- SteamVR
    // re-targets it onto the Sense controllers.
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::snprintf(asci.actionSetName, sizeof(asci.actionSetName), "board");
    std::snprintf(asci.localizedActionSetName,
                  sizeof(asci.localizedActionSetName), "Board handling");
    XrActionSet as = XR_NULL_HANDLE;
    xrCreateActionSet(inst, &asci, &as);
    actionSet_ = as;
    handPath_[0] = path(inst, "/user/hand/left");
    handPath_[1] = path(inst, "/user/hand/right");
    XrPath hands[2] = {static_cast<XrPath>(handPath_[0]),
                       static_cast<XrPath>(handPath_[1])};
    XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
    std::snprintf(aci.actionName, sizeof(aci.actionName), "grip_pose");
    std::snprintf(aci.localizedActionName, sizeof(aci.localizedActionName),
                  "Grip pose");
    aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
    aci.countSubactionPaths = 2;
    aci.subactionPaths = hands;
    XrAction ga = XR_NULL_HANDLE;
    xrCreateAction(as, &aci, &ga);
    gripAction_ = ga;
    const XrActionSuggestedBinding binds[] = {
        {ga, path(inst, "/user/hand/left/input/grip/pose")},
        {ga, path(inst, "/user/hand/right/input/grip/pose")},
    };
    XrInteractionProfileSuggestedBinding prof{
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    prof.interactionProfile =
        path(inst, "/interaction_profiles/khr/simple_controller");
    prof.suggestedBindings = binds;
    prof.countSuggestedBindings = 2;
    xrSuggestInteractionProfileBindings(inst, &prof);
    XrSessionActionSetsAttachInfo attach{
        XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &as;
    xrAttachSessionActionSets(s, &attach);
    for (int i = 0; i < 2; ++i) {
        XrActionSpaceCreateInfo asp{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        asp.action = ga;
        asp.subactionPath = hands[i];
        asp.poseInActionSpace.orientation.w = 1.0f;
        XrSpace hs = XR_NULL_HANDLE;
        xrCreateActionSpace(s, &asp, &hs);
        handSpace_[i] = hs;
    }
    std::printf("vr: session up, %u eyes at %u x %u\n", viewCount,
                (*chains_)[0].width, (*chains_)[0].height);
    std::fflush(stdout);
    return true;
}

bool VrSession::beginFrame(std::vector<Eye>* eyes) {
    if (!session_) return false;
    XrSession s = asSession(session_);
    XrInstance inst = static_cast<XrInstance>(inst_);

    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrSessionState st =
                reinterpret_cast<XrEventDataSessionStateChanged*>(&ev)->state;
            focused_ = (st == XR_SESSION_STATE_FOCUSED);
            if (st == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType =
                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                running_ = XR_SUCCEEDED(xrBeginSession(s, &bi));
            } else if (st == XR_SESSION_STATE_STOPPING) {
                xrEndSession(s);
                running_ = false;
            } else if (st == XR_SESSION_STATE_EXITING ||
                       st == XR_SESSION_STATE_LOSS_PENDING) {
                running_ = false;
                end();
                return false;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR) {
            // The lens geometry moved -- an IPD change on headsets that can do
            // it. Drop the cached mesh; the block below re-queries it on the
            // next frame that has a located view.
            maskReady_ = false;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    if (!running_) return false;

    XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(s, &fwi, &fs))) return false;
    XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
    xrBeginFrame(s, &fbi);
    // From here on the frame MUST be ended, whatever else happens -- the
    // returns below mean "do not render", not "do not finish". endFrame() keys
    // off this so the caller can simply always call it.
    frameOpen_ = true;
    displayTime_ = fs.predictedDisplayTime;
    shouldRender_ = fs.shouldRender != 0;

    // Are we actually hitting the headset's rate?
    //
    // This matters more than it looks. Everything the wobble reports describe
    // -- swimming that worsens as you lean in, ghosting as you move -- is what
    // the compositor's reprojection looks like, and reprojection only engages
    // for frames the app failed to deliver. So "is the image warping" and "are
    // we missing frames" are the same question, and nothing here was measuring
    // it.
    //
    // predictedDisplayTime advances by exactly one predictedDisplayPeriod when
    // the app keeps up. Two periods means every other frame is being
    // synthesised by the compositor from the last one we did deliver. That
    // ratio is the honest missed-frame count, without needing a vendor API.
    if (fs.predictedDisplayPeriod > 0) {
        if (lastDisplayTime_ != 0) {
            const long long delta = fs.predictedDisplayTime - lastDisplayTime_;
            const long long periods =
                (delta + fs.predictedDisplayPeriod / 2) / fs.predictedDisplayPeriod;
            ++timedFrames_;
            if (periods > 1) {
                ++missedFrames_;
                missedTotal_ += static_cast<unsigned>(periods - 1);
            }
        }
        lastDisplayTime_ = fs.predictedDisplayTime;
        lastPeriodNs_ = fs.predictedDisplayPeriod;
        if (nativePeriodNs_ == 0 || fs.predictedDisplayPeriod < nativePeriodNs_)
            nativePeriodNs_ = fs.predictedDisplayPeriod;

        if (rateAuto_ && timedFrames_ >= 240) {
            const double hz = 1.0e9 / static_cast<double>(fs.predictedDisplayPeriod);
            // "frame" and not "cpu": this is wall time from xrBeginFrame to
            // xrEndFrame, which includes drawFrame blocking on the previous
            // submission's fence. Reading it as CPU cost would send anyone
            // optimising in the wrong direction.
            std::printf("vr-rate: %.0f Hz target | %u frames, %u late "
                        "(%.1f%%), %u display periods lost | app frame %.2f ms "
                        "of %.2f ms\n",
                        hz, timedFrames_, missedFrames_,
                        100.0 * missedFrames_ / static_cast<double>(timedFrames_),
                        missedTotal_,
                        frameCpuMs_ / static_cast<double>(timedFrames_),
                        1000.0 / hz);
            std::fflush(stdout);
            timedFrames_ = 0;
            missedFrames_ = 0;
            missedTotal_ = 0;
            frameCpuMs_ = 0.0;
        }
    }
    frameStart_ = std::chrono::steady_clock::now();

    XrActiveActionSet active{static_cast<XrActionSet>(actionSet_), XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    xrSyncActions(s, &sync);

    // Views first, then the anchor, and only THEN the placement matrix.
    //
    // This used to run the other way round: `place` was built from anchorPos_
    // at the top of the function, but anchorPos_ was not captured until after
    // xrLocateViews further down. So the FIRST frame placed the board 0.6 m in
    // front of LOCAL's origin rather than in front of the viewer, and every
    // later frame used an anchor one frame stale. Measured on a real session,
    // that put the board low and off to one side -- eye 0's centre landed at
    // ndc +0.002 and eye 1's at -0.678, where the frustum skew alone predicts a
    // symmetric +/-0.324. The eye-to-eye DIFFERENCE was right (0.680 against a
    // predicted 0.648), so stereo was never the problem; both eyes were simply
    // looking at a board that had been parked in the wrong place.
    uint32_t got = 0;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    std::vector<XrView> views(chains_->size(), {XR_TYPE_VIEW});
    {
        XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = displayTime_;
        vli.space = static_cast<XrSpace>(appSpace_);
        xrLocateViews(s, &vli, &vs, static_cast<uint32_t>(views.size()), &got,
                      views.data());
    }

    // Is the head pose REAL, or is the runtime just handing back a filled-in
    // struct?
    //
    // xrLocateViews succeeds and reports two views whether or not it actually
    // knows where the head is; viewStateFlags is the only thing that says so,
    // and this used to ignore it entirely. While the headset is being put on --
    // exactly when the handover below re-anchors -- SteamVR returns identity
    // poses with these bits clear. Anchoring to one of those places the board
    // at LOCAL's origin facing -Z, which for anyone not already facing -Z is
    // off to the side and edge-on: the sliver that keeps getting reported.
    //
    // TRACKED and not merely VALID: VALID says the fields hold meaningful data,
    // TRACKED says the data is CURRENT rather than the last pose known before
    // tracking dropped. An anchor decides where the board sits for the rest of
    // the session off a single frame, so it is worth waiting for the real one.
    const XrViewStateFlags kUsable = XR_VIEW_STATE_POSITION_VALID_BIT |
                                     XR_VIEW_STATE_ORIENTATION_VALID_BIT |
                                     XR_VIEW_STATE_POSITION_TRACKED_BIT |
                                     XR_VIEW_STATE_ORIENTATION_TRACKED_BIT;
    const bool posesTracked = (vs.viewStateFlags & kUsable) == kUsable;

    // Anchor the board to the viewer on the first TRACKED frame, and never
    // again -- an anchor that updates every frame would drag the board along
    // with your head, which is the opposite of an object sitting in a room.
    // Until one arrives the previous anchor stands, so the board stays put
    // rather than jumping to the origin while tracking settles.
    if (!anchored_ && got >= 2 && !posesTracked && anchorWaited_ < 2) {
        // Once per wait, not once per frame: if this ever does hang, the log
        // says which bits were missing instead of leaving us to guess.
        ++anchorWaited_;
        std::printf("vr: anchor deferred, head pose not tracked yet "
                    "(viewStateFlags 0x%llx)\n",
                    static_cast<unsigned long long>(vs.viewStateFlags));
        std::fflush(stdout);
    }
    if (!anchored_ && got >= 2 && posesTracked) {
        anchorWaited_ = 0;
        anchored_ = true;
        anchorPos_[0] =
            (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
        anchorPos_[1] =
            (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
        anchorPos_[2] =
            (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;
        anchorRot_[0] = views[0].pose.orientation.x;
        anchorRot_[1] = views[0].pose.orientation.y;
        anchorRot_[2] = views[0].pose.orientation.z;
        anchorRot_[3] = views[0].pose.orientation.w;
        // Yaw as well as position. Yaw is what decides which way the board
        // faces, so an anchor that looks fine by position alone can still put
        // the board off to one side -- and that is invisible in a log that only
        // prints where your head was.
        const glm::quat aq(anchorRot_[3], anchorRot_[0], anchorRot_[1],
                           anchorRot_[2]);
        const glm::vec3 afwd = aq * glm::vec3(0.0f, 0.0f, -1.0f);
        std::printf("vr: board anchored at head (%.2f %.2f %.2f) facing %.0f "
                    "deg\n",
                    anchorPos_[0], anchorPos_[1], anchorPos_[2],
                    glm::degrees(std::atan2(-afwd.x, -afwd.z)));
        std::fflush(stdout);
    }

    const glm::mat4 place = placement(
        placeCentre_, placeScale_,
        glm::vec3(anchorPos_[0], anchorPos_[1], anchorPos_[2]),
        glm::quat(anchorRot_[3], anchorRot_[0], anchorRot_[1], anchorRot_[2]));
    const glm::mat4 invPlace = glm::inverse(place);

    // WHERE IS THE BOARD, from where the head is RIGHT NOW.
    //
    // Every diagnostic so far described the moment of anchoring, which is no
    // help when the complaint is about what is visible some seconds later. This
    // measures the thing actually being reported: how far away the board is,
    // and how far off to the side and above/below the current gaze it sits.
    // Bearing is signed -- positive is to the right -- so "off to my left" has
    // a number attached instead of being a description.
    if (got >= 2) {
        const glm::vec3 h(
            (views[0].pose.position.x + views[1].pose.position.x) * 0.5f,
            (views[0].pose.position.y + views[1].pose.position.y) * 0.5f,
            (views[0].pose.position.z + views[1].pose.position.z) * 0.5f);
        boardDist_ = glm::length(
            glm::vec3(place * glm::vec4(placeCentre_[0], placeCentre_[1],
                                        placeCentre_[2], 1.0f)) -
            h);
    }
    if (got >= 2 && (++diagFrames_ % 120) == 1) {
        const glm::vec3 head(
            (views[0].pose.position.x + views[1].pose.position.x) * 0.5f,
            (views[0].pose.position.y + views[1].pose.position.y) * 0.5f,
            (views[0].pose.position.z + views[1].pose.position.z) * 0.5f);
        const glm::quat hq(views[0].pose.orientation.w, views[0].pose.orientation.x,
                           views[0].pose.orientation.y, views[0].pose.orientation.z);
        const glm::vec3 fwd = hq * glm::vec3(0.0f, 0.0f, -1.0f);
        const glm::vec3 boardRoom =
            glm::vec3(place * glm::vec4(placeCentre_[0], placeCentre_[1],
                                        placeCentre_[2], 1.0f));
        const glm::vec3 d = boardRoom - head;
        const float dist = glm::length(d);
        // Horizontal bearing: both flattened to the room's floor plane, so a
        // glance up or down does not read as being off to the side.
        const glm::vec2 fh = glm::vec2(fwd.x, fwd.z);
        const glm::vec2 dh = glm::vec2(d.x, d.z);
        float bearing = 0.0f;
        if (glm::length(fh) > 1e-4f && glm::length(dh) > 1e-4f) {
            const glm::vec2 f = glm::normalize(fh), t = glm::normalize(dh);
            // atan2 of the 2D cross and dot: signed, and right-handed about the
            // room's up axis, so positive comes out to the viewer's right.
            bearing = glm::degrees(std::atan2(f.x * t.y - f.y * t.x,
                                              f.x * t.x + f.y * t.y));
        }
        const float elev =
            dist > 1e-4f ? glm::degrees(std::asin(d.y / dist)) : 0.0f;
        std::printf("vr-where: board %.2f m away, bearing %+.0f deg (%s), "
                    "elevation %+.0f deg | head(%.2f %.2f %.2f) "
                    "board(%.2f %.2f %.2f)\n",
                    dist, bearing,
                    std::fabs(bearing) < 15.0f ? "ahead"
                                               : (bearing > 0 ? "RIGHT" : "LEFT"),
                    elev, head.x, head.y, head.z, boardRoom.x, boardRoom.y,
                    boardRoom.z);
        std::fflush(stdout);
    }

    for (int i = 0; i < 2; ++i) {
        gripTracked_[i] = false;
        if (!handSpace_[i]) continue;
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(static_cast<XrSpace>(handSpace_[i]),
                                    static_cast<XrSpace>(appSpace_),
                                    displayTime_, &loc)))
            continue;
        if (!(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
            continue;
        const glm::vec4 mm =
            invPlace * glm::vec4(loc.pose.position.x, loc.pose.position.y,
                                 loc.pose.position.z, 1.0f);
        gripPosMm_[i][0] = mm.x;
        gripPosMm_[i][1] = mm.y;
        gripPosMm_[i][2] = mm.z;
        gripQuat_[i][0] = loc.pose.orientation.x;
        gripQuat_[i][1] = loc.pose.orientation.y;
        gripQuat_[i][2] = loc.pose.orientation.z;
        gripQuat_[i][3] = loc.pose.orientation.w;
        gripTracked_[i] = true;
    }

    eyes->clear();
    if (!shouldRender_ || got < 2) return false;

    // Keep the poses actually rendered with. endFrame used to re-locate, which
    // is wrong on principle -- the layer must describe the frame that was
    // drawn, not a fresh prediction -- and wasteful.

    // One-shot dump of what the two eyes actually got. Divergent views and a
    // bad placement look identical through the lenses, and the numbers tell
    // them apart instantly: healthy stereo is ~65mm of X separation and near
    // identical FOV, with the board a sane size in front of the viewer.
    static bool dumped = false;
    if (!dumped && got >= 2) {
        dumped = true;
        std::printf("vr-diag: placement scale=%.6f (board span -> 0.35 m), "
                    "anchored at head (%.2f %.2f %.2f)\n",
                    placeScale_, anchorPos_[0], anchorPos_[1], anchorPos_[2]);
        for (uint32_t i = 0; i < got; ++i) {
            std::printf("vr-diag: eye %u room-pos(%+.4f %+.4f %+.4f) "
                        "fov L%+.3f R%+.3f U%+.3f D%+.3f\n",
                        i, views[i].pose.position.x, views[i].pose.position.y,
                        views[i].pose.position.z, views[i].fov.angleLeft,
                        views[i].fov.angleRight, views[i].fov.angleUp,
                        views[i].fov.angleDown);
        }
        const float dx = views[1].pose.position.x - views[0].pose.position.x;
        const float dy = views[1].pose.position.y - views[0].pose.position.y;
        const float dz = views[1].pose.position.z - views[0].pose.position.z;
        std::printf("vr-diag: eye separation %.1f mm (expect ~60-70)\n",
                    std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0f);
        std::printf("vr-diag: board centre mm(%.1f %.1f %.1f)\n",
                    placeCentre_[0], placeCentre_[1], placeCentre_[2]);
        std::fflush(stdout);
    }

    // The hidden-area mesh, resolved once the FOV is known.
    //
    // Its vertices are 2D points on the z = -1 plane of the view frustum, so
    // they need the eye's PROJECTION to become screen positions -- and the FOV
    // only arrives with a located view, which is why this cannot happen at
    // session start. The projection is fixed per eye though, so it is done once
    // and the result cached: at z = -1 the perspective divide is trivial (w
    // comes out 1), leaving ndc.x = p0*x - p8 and ndc.y = p5*y - p9.
    if (maskSupported_ && !maskReady_ && got >= 2) {
        auto getMask = proc<PFN_xrGetVisibilityMaskKHR>(asXr(inst_),
                                                        "xrGetVisibilityMaskKHR");
        if (getMask) {
            maskReady_ = true;   // one attempt, whatever the outcome
            for (uint32_t i = 0; i < got && i < 2; ++i) {
                XrVisibilityMaskKHR m{XR_TYPE_VISIBILITY_MASK_KHR};
                if (XR_FAILED(getMask(s, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                      i, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR,
                                      &m)) ||
                    m.indexCountOutput == 0 || m.vertexCountOutput == 0)
                    continue;

                std::vector<XrVector2f> verts(m.vertexCountOutput);
                std::vector<uint32_t> idx(m.indexCountOutput);
                m.vertexCapacityInput = m.vertexCountOutput;
                m.indexCapacityInput = m.indexCountOutput;
                m.vertices = verts.data();
                m.indices = idx.data();
                if (XR_FAILED(getMask(s, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                      i, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR,
                                      &m)))
                    continue;

                float proj[16];
                projectionFromFov(views[i].fov, kVrNear, proj);
                mask_[i].ndc.clear();
                mask_[i].ndc.reserve(verts.size() * 2);
                for (const XrVector2f& v : verts) {
                    mask_[i].ndc.push_back(proj[0] * v.x - proj[8]);
                    mask_[i].ndc.push_back(proj[5] * v.y - proj[9]);
                }
                mask_[i].indices = std::move(idx);
                ++mask_[i].version;
                std::printf("vr: hidden-area mesh eye %u: %u triangles\n", i,
                            m.indexCountOutput / 3);
            }
            std::fflush(stdout);
        }
    }

    lastViews_.clear();
    for (uint32_t i = 0; i < got; ++i) {
        ViewPose vp;
        vp.pos[0] = views[i].pose.position.x;
        vp.pos[1] = views[i].pose.position.y;
        vp.pos[2] = views[i].pose.position.z;
        vp.quat[0] = views[i].pose.orientation.x;
        vp.quat[1] = views[i].pose.orientation.y;
        vp.quat[2] = views[i].pose.orientation.z;
        vp.quat[3] = views[i].pose.orientation.w;
        vp.fov[0] = views[i].fov.angleLeft;
        vp.fov[1] = views[i].fov.angleRight;
        vp.fov[2] = views[i].fov.angleUp;
        vp.fov[3] = views[i].fov.angleDown;
        lastViews_.push_back(vp);
    }

    // One-shot: push the board's own extremities through each finished matrix
    // and report where they land. This answers, without anybody having to look
    // through a lens and judge what "correct" means, the two questions the
    // reported symptom actually turns on:
    //
    //   Does the board FIT? At 0.35 m across and 0.6 m away it subtends about
    //   32 degrees inside a 105-degree frustum, so every corner must come back
    //   comfortably inside NDC -1..+1 in BOTH eyes. If corners are off-screen
    //   the board overflows the view, and each eye showing a different PART of
    //   it stops being mysterious -- it becomes arithmetic.
    //
    //   Which part is in which eye? The board sits straight ahead while eye 0's
    //   frustum is centred about 9 degrees to the LEFT, so the board should
    //   land RIGHT of centre in eye 0 and LEFT of centre in eye 1, by equal and
    //   opposite amounts. That is correct and expected in the raw images -- the
    //   lenses undo it. Anything else, and the numbers say where to look.
    static bool projDumped = false;
    const float halfSpan = 0.5f * (0.35f / placeScale_);

    for (uint32_t i = 0; i < got; ++i) {
        Eye e;
        e.width = (*chains_)[i].width;
        e.height = (*chains_)[i].height;
        float proj[16];
        // 1mm near plane in ROOM metres, then scaled with everything else.
        projectionFromFov(views[i].fov, kVrNear, proj);
        const glm::mat4 P = glm::make_mat4(proj);
        const glm::mat4 V = glm::inverse(poseMatrix(views[i].pose));
        // World(mm) -> room -> eye -> clip, in one matrix, so the renderer is
        // handed exactly what it always takes.
        const glm::mat4 vp = P * V * place;

        if (!projDumped) {
            const glm::vec3 c(placeCentre_[0], placeCentre_[1], placeCentre_[2]);
            const struct { const char* name; glm::vec3 p; } pts[] = {
                {"centre", c},
                {"-X    ", c + glm::vec3(-halfSpan, 0.0f, 0.0f)},
                {"+X    ", c + glm::vec3(+halfSpan, 0.0f, 0.0f)},
                {"-Y    ", c + glm::vec3(0.0f, -halfSpan, 0.0f)},
                {"+Y    ", c + glm::vec3(0.0f, +halfSpan, 0.0f)},
            };
            for (const auto& q : pts) {
                const glm::vec4 clip = vp * glm::vec4(q.p, 1.0f);
                if (clip.w <= 1e-6f) {
                    std::printf("vr-proj: eye %u %s -> BEHIND VIEWER (w=%.4f)\n",
                                i, q.name, clip.w);
                    continue;
                }
                const float nx = clip.x / clip.w, ny = clip.y / clip.w;
                std::printf("vr-proj: eye %u %s -> ndc(%+.3f %+.3f)%s\n", i,
                            q.name, nx, ny,
                            (std::fabs(nx) > 1.0f || std::fabs(ny) > 1.0f)
                                ? "   OFF-SCREEN"
                                : "");
            }
            std::fflush(stdout);
        }
        std::memcpy(e.viewProj, glm::value_ptr(vp), sizeof(e.viewProj));
        // The lighting rig wants the viewpoint in board millimetres.
        const glm::vec4 eyeMm =
            invPlace * glm::vec4(views[i].pose.position.x,
                                 views[i].pose.position.y,
                                 views[i].pose.position.z, 1.0f);
        e.eye[0] = eyeMm.x;
        e.eye[1] = eyeMm.y;
        e.eye[2] = eyeMm.z;

        // The same frustum again, as a ray basis for the path tracer. invPlace
        // is rotation times a uniform scale, so rotating the eye's axes through
        // it and renormalising gives the board-space directions.
        const glm::mat3 toBoard(invPlace);
        const glm::quat q(views[i].pose.orientation.w, views[i].pose.orientation.x,
                          views[i].pose.orientation.y, views[i].pose.orientation.z);
        const glm::vec3 f =
            glm::normalize(toBoard * (q * glm::vec3(0.0f, 0.0f, -1.0f)));
        const glm::vec3 r =
            glm::normalize(toBoard * (q * glm::vec3(1.0f, 0.0f, 0.0f)));
        const glm::vec3 u =
            glm::normalize(toBoard * (q * glm::vec3(0.0f, 1.0f, 0.0f)));

        const float tL = std::tan(views[i].fov.angleLeft);
        const float tR = std::tan(views[i].fov.angleRight);
        const float tU = std::tan(views[i].fov.angleUp);
        const float tD = std::tan(views[i].fov.angleDown);
        // Centre of the frustum, which is NOT the eye's forward axis: these
        // lenses sit off the panel centres, so eye 0's cone points about 9
        // degrees left of straight ahead and eye 1's the same to the right.
        const glm::vec3 centre =
            f + r * ((tR + tL) * 0.5f) + u * ((tU + tD) * 0.5f);
        const glm::vec3 halfR = r * ((tR - tL) * 0.5f);
        const glm::vec3 halfU = u * ((tU - tD) * 0.5f);
        for (int k = 0; k < 3; ++k) {
            e.fwd[k] = centre[k];
            e.right[k] = halfR[k];
            e.up[k] = halfU[k];
        }
        eyes->push_back(e);
    }
    projDumped = true;
    return !eyes->empty();
}

VkImage VrSession::acquireEye(int index, uint32_t* width, uint32_t* height) {
    if (!chains_ || index < 0 || index >= static_cast<int>(chains_->size()))
        return VK_NULL_HANDLE;
    // Eye 0 opens a rendered frame: from here the images hold THESE poses, and
    // that is what the layer must advertise until they are redrawn.
    if (index == 0) submitViews_ = lastViews_;
    Chain& c = (*chains_)[index];
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(c.chain, &ai, &idx)))
        return VK_NULL_HANDLE;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(c.chain, &wi);
    acquired_[index] = idx;
    if (width) *width = c.width;
    if (height) *height = c.height;

    // The depth image travels with the colour one: same frame, same eye,
    // acquired and released together so they can never describe different
    // moments.
    depthTarget_ = VK_NULL_HANDLE;
    if (c.depthChain) {
        uint32_t didx = 0;
        XrSwapchainImageAcquireInfo dai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_SUCCEEDED(xrAcquireSwapchainImage(c.depthChain, &dai, &didx))) {
            XrSwapchainImageWaitInfo dwi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            dwi.timeout = XR_INFINITE_DURATION;
            xrWaitSwapchainImage(c.depthChain, &dwi);
            c.depthAcquired = didx;
            depthTarget_ = c.depthImages[didx];
            depthTargetView_ = c.depthViews[didx];
        }
    }
    return c.images[idx];
}

VkImage VrSession::acquiredDepthImage() const { return depthTarget_; }
VkImageView VrSession::acquiredDepthView() const { return depthTargetView_; }

void VrSession::releaseEye(int index) {
    if (!chains_ || index < 0 || index >= static_cast<int>(chains_->size()))
        return;
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage((*chains_)[index].chain, &ri);
    if ((*chains_)[index].depthChain)
        xrReleaseSwapchainImage((*chains_)[index].depthChain, &ri);
    depthTarget_ = VK_NULL_HANDLE;
}

VrSession::RateStats VrSession::takeRate() {
    RateStats s;
    s.frames = timedFrames_;
    s.late = missedFrames_;
    s.periodsLost = missedTotal_;
    s.frameMs = frameCpuMs_;
    s.hz = lastPeriodNs_ > 0 ? 1.0e9 / static_cast<double>(lastPeriodNs_) : 0.0;
    timedFrames_ = 0;
    missedFrames_ = 0;
    missedTotal_ = 0;
    frameCpuMs_ = 0.0;
    return s;
}

void VrSession::endFrame() {
    if (!session_ || !frameOpen_) return;
    frameOpen_ = false;
    if (frameStart_.time_since_epoch().count() != 0) {
        frameCpuMs_ +=
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - frameStart_)
                .count();
    }
    XrSession s = asSession(session_);
    std::vector<XrCompositionLayerProjectionView> pv;
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};

    // submitViews_, not lastViews_: on a frame that was NOT redrawn the images
    // still hold the previous frame's content, so the layer has to keep
    // describing it. The runtime then reprojects those pixels to the current
    // head pose itself -- which is the whole point of skipping the render, and
    // is safe in a way that simply showing an old frame is not.
    const std::vector<ViewPose>& views =
        submitViews_.empty() ? lastViews_ : submitViews_;
    // Storage must outlive xrEndFrame -- the layer holds pointers into it.
    std::vector<XrCompositionLayerDepthInfoKHR> depths;
    depths.reserve(views.size());
    if (shouldRender_ && chains_ && !views.empty()) {
        for (size_t i = 0; i < views.size() && i < chains_->size(); ++i) {
            XrCompositionLayerProjectionView v{
                XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            const ViewPose& lv = views[i];
            v.pose.position = {lv.pos[0], lv.pos[1], lv.pos[2]};
            v.pose.orientation = {lv.quat[0], lv.quat[1], lv.quat[2],
                                  lv.quat[3]};
            v.fov = {lv.fov[0], lv.fov[1], lv.fov[2], lv.fov[3]};
            // PCBVIEW_VR_SWAP_EYES=1 hands each eye the other's image. Present
            // because "right eye shows the left side" is the signature of a
            // swap, and one run with this set settles whether the ordering is
            // the fault far faster than reasoning about sign conventions.
            v.subImage.swapchain = (*chains_)[swapEyes_ ? (1 - i) : i].chain;
            v.subImage.imageRect.offset = {0, 0};
            v.subImage.imageRect.extent = {
                static_cast<int32_t>((*chains_)[i].width),
                static_cast<int32_t>((*chains_)[i].height)};
            v.subImage.imageArrayIndex = 0;

            // REVERSED-Z, and the mapping has to say so or the runtime will
            // reproject the scene inside out.
            //
            // pcbview's projection puts the NEAR plane at depth 1 and infinity
            // at depth 0. XrCompositionLayerDepthInfoKHR asks for the distance
            // each end of the buffer's range corresponds to: nearZ is the
            // distance at minDepth, farZ the distance at maxDepth. So with
            // depth reversed, minDepth 0 is infinitely far and maxDepth 1 is
            // the near plane -- nearZ infinite, farZ 0.01 m. The spec allows
            // nearZ > farZ precisely to describe this.
            if ((*chains_)[i].depthChain) {
                XrCompositionLayerDepthInfoKHR d{
                    XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
                d.subImage.swapchain = (*chains_)[i].depthChain;
                d.subImage.imageRect.offset = {0, 0};
                d.subImage.imageRect.extent = {
                    static_cast<int32_t>((*chains_)[i].width),
                    static_cast<int32_t>((*chains_)[i].height)};
                d.subImage.imageArrayIndex = 0;
                d.minDepth = 0.0f;
                d.maxDepth = 1.0f;
                // Both finite now. minDepth 0 is the far plane and maxDepth 1
                // the near plane, so nearZ (the distance at minDepth) is 100 m
                // and farZ (the distance at maxDepth) is the near plane --
                // nearZ > farZ, which is how the spec says to declare reversed
                // depth. Keep these in step with projectionFromFov's zFar.
                d.nearZ = kVrFar;
                d.farZ = kVrNear;
                depths.push_back(d);
                v.next = &depths.back();
            }
            pv.push_back(v);
        }
        layer.space = static_cast<XrSpace>(appSpace_);
        layer.viewCount = static_cast<uint32_t>(pv.size());
        layer.views = pv.data();
    }

    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
    XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
    fei.displayTime = displayTime_;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = pv.empty() ? 0 : 1;
    fei.layers = pv.empty() ? nullptr : layers;
    xrEndFrame(s, &fei);
}

bool VrSession::gripPose(int hand, float outPosMm[3], float outQuat[4]) const {
    if (hand < 0 || hand > 1 || !gripTracked_[hand]) return false;
    for (int i = 0; i < 3; ++i) outPosMm[i] = gripPosMm_[hand][i];
    for (int i = 0; i < 4; ++i) outQuat[i] = gripQuat_[hand][i];
    return true;
}

void VrSession::end() {
    // Leave the session the way the runtime expects, or SteamVR is left holding
    // a client that vanished mid-session and its compositor wedges -- which
    // shows up afterwards as "a key component of SteamVR isn't working
    // properly" and needs a restart before anything can run again.
    //
    // The sequence is not optional and not obvious:
    //   1. finish any frame already begun -- xrBeginFrame owes an xrEndFrame
    //   2. xrRequestExitSession, then PUMP EVENTS until the runtime reports
    //      STOPPING. xrEndSession is only legal from that state; calling it
    //      while the session is still running fails with
    //      XR_ERROR_SESSION_NOT_STOPPING, and since nothing checked the result
    //      the session was then destroyed while the runtime still believed it
    //      was live.
    //   3. only then tear down swapchains, spaces and the session itself.
    if (session_) {
        XrSession s = asSession(session_);

        if (frameOpen_) {
            XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
            fei.displayTime = displayTime_;
            fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            fei.layerCount = 0;
            fei.layers = nullptr;
            xrEndFrame(s, &fei);
            frameOpen_ = false;
        }

        if (running_ && inst_) {
            xrRequestExitSession(s);
            // Bounded: a runtime that never answers must not hang the app on
            // the way out. Two hundred turns at 5 ms is a second of patience,
            // far longer than this transition takes in practice.
            for (int spin = 0; spin < 200 && running_; ++spin) {
                XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
                while (xrPollEvent(asXr(inst_), &ev) == XR_SUCCESS) {
                    if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                        const auto* sc =
                            reinterpret_cast<const XrEventDataSessionStateChanged*>(
                                &ev);
                        if (sc->state == XR_SESSION_STATE_STOPPING) {
                            xrEndSession(s);
                            running_ = false;
                        } else if (sc->state == XR_SESSION_STATE_EXITING ||
                                   sc->state == XR_SESSION_STATE_LOSS_PENDING) {
                            running_ = false;
                        }
                    }
                    ev = {XR_TYPE_EVENT_DATA_BUFFER};
                }
                if (running_)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    }

    if (chains_) {
        for (Chain& c : *chains_) {
            for (VkImageView v : c.depthViews)
                if (v) vkDestroyImageView(device_, v, nullptr);
            c.depthViews.clear();
            if (c.depthChain) xrDestroySwapchain(c.depthChain);
        }
        for (Chain& c : *chains_)
            if (c.chain) xrDestroySwapchain(c.chain);
        delete chains_;
        chains_ = nullptr;
    }
    for (void*& h : handSpace_) {
        if (h) xrDestroySpace(static_cast<XrSpace>(h));
        h = nullptr;
    }
    if (appSpace_) xrDestroySpace(static_cast<XrSpace>(appSpace_));
    appSpace_ = nullptr;
    if (actionSet_) xrDestroyActionSet(static_cast<XrActionSet>(actionSet_));
    actionSet_ = nullptr;
    if (session_) {
        // running_ is cleared above once STOPPING arrives; this is the fallback
        // for a runtime that never sent it.
        if (running_) xrEndSession(asSession(session_));
        xrDestroySession(asSession(session_));
    }
    session_ = nullptr;
    running_ = false;
    focused_ = false;
}

}  // namespace pcbview::xr
