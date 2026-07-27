#pragma once

#include <vulkan/vulkan.h>

#include <chrono>
#include <vector>

#include "render/common/device.h"

namespace pcbview::vk {
class Renderer;
}

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
    // Whether the runtime offered XR_KHR_composition_layer_depth, so the
    // session knows whether to build depth swapchains.
    bool depthLayerSupported() const { return depthLayerSupported_; }
    bool visibilityMaskSupported() const { return visibilityMaskSupported_; }

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
    bool depthLayerSupported_ = false;
    bool visibilityMaskSupported_ = false;
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

// `pcbview --xr-present-test`: the same session, but actually PRESENTING --
// per-eye swapchains, a cleared image submitted as a projection layer every
// frame. That is what takes the runtime to VISIBLE/FOCUSED, which is in turn
// what makes the Sense controllers report poses, so this test reports those
// too. No render pass is needed to prove it: clearing the swapchain image
// directly is a legitimate frame and keeps the test to the XR plumbing rather
// than dragging the board renderer in.
int presentTest();

// A live VR session that renders pcbview's board through the ordinary
// renderer, one eye at a time.
//
// Kept separate from System so the device hand-over (which must happen before
// Vulkan exists) stays independent of the frame loop (which cannot start until
// the renderer does).
class VrSession {
public:
    ~VrSession();
    bool begin(System& sys, VkInstance vkInstance, VkDevice device,
               VkPhysicalDevice gpu, uint32_t queueFamily, VkQueue queue);
    void end();
    bool active() const { return session_ != nullptr; }

    // What one eye needs. `viewProj` maps pcbview's WORLD (millimetres)
    // straight to clip space, so the renderer needs no special case; `eye` is
    // the viewpoint back in those same millimetres, which is what the lighting
    // rig wants.
    // What one eye needs. `viewProj` serves the raster path; `fwd`/`right`/`up`
    // serve the PATH TRACER, which never looks at a matrix -- it builds rays as
    // fwd + ndc.x*right - ndc.y*up and so needs the frustum as a basis instead.
    //
    // That ray model is symmetric about fwd, and a VR frustum is not, so the
    // frustum's centre offset is folded into fwd and right/up carry only the
    // half-extents. Without this the tracer falls back on whatever camera the
    // desktop window last set -- which is how path-traced VR ended up showing
    // the desktop's zoom, with no head tracking at all.
    struct Eye {
        float viewProj[16];
        float eye[3];
        float fwd[3];
        float right[3];
        float up[3];
        uint32_t width = 0, height = 0;
    };

    // Pumps events and waits on the runtime's frame pacing. False means do not
    // render this frame (not focused, or the session ended -- check active()).
    bool beginFrame(std::vector<Eye>* eyes);
    // Acquire eye `index`'s next swapchain image, ready to be rendered INTO.
    // Returns VK_NULL_HANDLE if the runtime would not give one up.
    //
    // Split from the release so the renderer can copy into the image inside its
    // own command buffer. The old submitEye did acquire, copy and release in
    // one call, which forced the copy to be an independent submit -- and an
    // independent submit has no way to know the frame it is copying has
    // finished, so it had to bracket itself with full device and queue waits.
    // Four GPU stalls a frame, and the whole reason the headset felt choppy.
    VkImage acquireEye(int index, uint32_t* width, uint32_t* height);
    void releaseEye(int index);

    // The depth image acquired alongside the last acquireEye, or null when the
    // runtime has no depth layer. Rendering into it lets the runtime reproject
    // a missed frame PER PIXEL using real geometry, instead of sliding the
    // whole frame as a flat sheet and shearing anything that stands proud of
    // the board.
    VkImage acquiredDepthImage() const;
    VkImageView acquiredDepthView() const;

    // The hidden-area mesh for an eye, already projected to screen space:
    // pairs of floats in NDC, plus a triangle index list. Empty when the
    // runtime has no visibility mask, or before the first located frame -- the
    // eye's field of view is needed to project it and only arrives with a view.
    //
    // Drawing this into depth before the board means the rasterizer never
    // shades the corners the lenses bend away, which on this headset is 22% of
    // every eye.
    struct MaskMesh {
        std::vector<float> ndc;        // x,y pairs
        std::vector<uint32_t> indices;
        uint32_t version = 0;          // bumped on every successful (re)query
    };
    const MaskMesh& visibilityMask(int eye) const {
        return mask_[(eye == 1) ? 1 : 0];
    }
    void endFrame();

    // Where the board sits in the room: how far in front, how high, and how
    // big across. Millimetres in, metres out.
    void setBoardPlacement(const float centreMm[3], float spanMm);

    // Put the board back in front of the viewer on the next frame.
    //
    // The anchor is captured once and then left alone, so the board stays put
    // in the room instead of following your head. That is right, but it means
    // the board lands wherever you happened to be facing at the instant the
    // session opened -- typically at the monitor, since that is where you were
    // when you launched it. Turn around afterwards and the board is off to your
    // side, edge-on.
    void reanchor() {
        anchored_ = false;
        anchorWaited_ = 0;
    }

    // Frame pacing, as counted against the runtime's own display clock.
    // frameMs is the TOTAL across `frames`, so callers divide.
    struct RateStats {
        unsigned frames = 0;
        unsigned late = 0;         // frames that arrived after their slot
        unsigned periodsLost = 0;  // display periods the compositor filled in
        double frameMs = 0.0;
        double hz = 0.0;           // the rate the runtime is currently pacing us at
    };
    // Read and reset. Used to attribute a measured window to one configuration.
    RateStats takeRate();
    // Metres from the head to the board centre, as of the last located frame.
    // Fill cost follows how much of the eye the board covers, so any timing
    // comparison has to say what distance it was taken at.
    float boardDistance() const { return boardDist_; }
    // The periodic vr-rate line. Off while sweeping, so the sweep owns the
    // output and its rows are not interleaved with unrelated ones.
    void setRateAutoReport(bool on) { rateAuto_ = on; }

    // The grip pose of a controller, in pcbview world millimetres, when one is
    // tracked. Index 0 = left, 1 = right.
    bool gripPose(int hand, float outPosMm[3], float outQuat[4]) const;

private:
    void* session_ = nullptr;
    void* appSpace_ = nullptr;
    void* actionSet_ = nullptr;
    void* gripAction_ = nullptr;
    void* handSpace_[2] = {nullptr, nullptr};
    unsigned long long handPath_[2] = {0, 0};
    void* inst_ = nullptr;
    unsigned long long sysId_ = 0;
    struct Chain;
    std::vector<Chain>* chains_ = nullptr;
    std::vector<uint32_t> acquired_;
    long long displayTime_ = 0;
    bool running_ = false;
    bool focused_ = false;
    bool shouldRender_ = false;
    // xrBeginFrame was called and xrEndFrame is still owed.
    bool frameOpen_ = false;
    // The poses actually rendered with, so the submitted layer describes the
    // frame that was drawn rather than a fresh prediction. Kept as a plain POD
    // because this header is included by the app and render layers, which have
    // no business seeing OpenXR types.
    struct ViewPose {
        float pos[3];
        float quat[4];   // xyzw
        float fov[4];    // left, right, up, down, radians
    };
    std::vector<ViewPose> lastViews_;
    // The poses the swapchain images were actually RENDERED with, which is not
    // the same thing as the latest prediction once frames are skipped. A
    // submitted layer describes the content of its images, and the compositor
    // reprojects from that pose to wherever the head is at scanout -- hand it
    // fresh poses for stale pixels and it warps from the wrong origin, which
    // shows up as the world sliding under head motion.
    std::vector<ViewPose> submitViews_;
    bool swapEyes_ = false;
    bool depthSupported_ = false;
    bool maskSupported_ = false;
    bool maskReady_ = false;
    MaskMesh mask_[2];
    VkImage depthTarget_ = VK_NULL_HANDLE;
    VkImageView depthTargetView_ = VK_NULL_HANDLE;
    // Kept so the depth image views can be destroyed with the session.
    VkDevice device_ = VK_NULL_HANDLE;
    float placeCentre_[3] = {0, 0, 0};
    float placeScale_ = 0.001f;   // mm -> m
    // Where the viewer was when the board was last anchored. Captured once, so
    // the board stays put in the room instead of following your head around.
    float anchorPos_[3] = {0.0f, 0.0f, 0.0f};
    float anchorRot_[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // xyzw
    bool anchored_ = false;
    // How many times we have logged "waiting for a tracked pose" since the last
    // reanchor. Capped so a genuinely stuck session says so twice rather than
    // once per frame forever.
    int anchorWaited_ = 0;
    // Frame counter, only so the where-is-the-board line is throttled.
    unsigned diagFrames_ = 0;
    // Frame pacing. predictedDisplayTime advancing by more than one display
    // period means the compositor had to synthesise the frames in between,
    // which is when reprojection artifacts appear.
    long long lastDisplayTime_ = 0;
    unsigned timedFrames_ = 0;
    unsigned missedFrames_ = 0;
    unsigned missedTotal_ = 0;
    double frameCpuMs_ = 0.0;
    long long lastPeriodNs_ = 0;
    float boardDist_ = 0.0f;
    bool rateAuto_ = true;
    std::chrono::steady_clock::time_point frameStart_{};
    float gripPosMm_[2][3] = {};
    float gripQuat_[2][4] = {};
    bool gripTracked_[2] = {false, false};
};

}  // namespace pcbview::xr
