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

}  // namespace pcbview::xr
