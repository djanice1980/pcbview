#pragma once

#include <vulkan/vulkan.h>

#include "render/common/device.h"

namespace pcbview::xr {

// Owns the OpenXR instance and system, and supplies the Vulkan creation hooks
// the runtime requires.
//
// The ordering here is forced by OpenXR and is easy to get wrong: the XR
// instance and system must exist BEFORE the Vulkan instance, because the
// runtime wraps vkCreateInstance to add its own extensions; and the physical
// device cannot be chosen by us at all -- it is named by the runtime once the
// Vulkan instance exists. So the sequence is
//
//     start() -> installHooks() -> createInstance() -> adoptRuntimeGpu()
//             -> selectGpu()    -> createDevice()   -> removeHooks()
//
// Nothing here creates a session or renders; that is the next layer up.
class System {
public:
    ~System();
    System() = default;
    System(const System&) = delete;
    System& operator=(const System&) = delete;

    // Connect to the runtime and find the headset. False (with a printed
    // reason) when there is no runtime or no HMD -- never throws, because VR
    // being unavailable is an ordinary state, not an error.
    bool start();
    void stop();
    bool ready() const { return system_ != 0; }

    // Route pcbview's Vulkan creation through the runtime. Must bracket the
    // createInstance/createDevice calls.
    void installHooks();
    void removeHooks();

    // Ask the runtime which physical device its session needs, and record it so
    // selectGpu() honours it. Call once the Vulkan instance exists.
    VkPhysicalDevice adoptRuntimeGpu(VkInstance instance);

    // Per-eye recommended render target, for sizing swapchains later.
    bool viewSize(uint32_t* width, uint32_t* height) const;
    const char* headsetName() const { return headsetName_; }

    // Raw handles for the session layer. Typed as void*/unsigned long long so
    // this header stays free of OpenXR types -- it is included from the render
    // and app layers, which have no business seeing them.
    void* rawInstance() const { return instance_; }
    unsigned long long rawSystem() const { return system_; }

private:
    void* instance_ = nullptr;   // XrInstance
    unsigned long long system_ = 0;  // XrSystemId
    char headsetName_[256] = {};
    VulkanCreationHooks hooks_{};
};

// `pcbview --xr-device-test`: drive the sequence above end to end and report
// which GPU the runtime chose and whether the device came up. Proves the
// hand-over works before anything depends on it.
int deviceTest();

// `pcbview --xr-input-test`: bring a real session up and report the Sense
// controllers' grip poses and trigger values live for a few seconds.
//
// A session is unavoidable even though nothing is drawn: the controllers are
// tracked by the HEADSET's cameras, so there is no pose outside a running
// session, and poses only resolve once the runtime has taken the app to the
// FOCUSED state -- which it will not do unless a frame loop is running. So this
// runs a proper xrWaitFrame/xrBeginFrame/xrEndFrame loop and submits zero
// layers, which is legal and is what lets the poses come alive.
int inputTest();

}  // namespace pcbview::xr
