#pragma once

// A QWindow that hosts our own Vulkan renderer.
//
// Deliberately NOT QVulkanWindow: that class creates the VkDevice itself, which
// would take away control of the ray tracing extensions and break RT-readiness
// rule 1. Instead we hand Qt our VkInstance via QVulkanInstance::setVkInstance()
// and ask only for a surface. Qt supplies the window and the chrome; every line
// of device, buffer and pipeline code is unchanged.

// Vulkan MUST come first. Qt's Vulkan headers define VK_NO_PROTOTYPES so Qt can
// resolve entry points through its own loader; if Qt is included first, vulkan.h
// declares no prototypes and every direct vkFoo() call fails to compile. Pulling
// vulkan.h in ahead of Qt gets us the prototypes, and we link vulkan-1 directly.
#include <vulkan/vulkan.h>

#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <QVulkanInstance>
#include <QWindow>

#include <glm/glm.hpp>

#include <memory>

#include "geom/tessellate.h"
#include "render/common/device.h"
#include "render/vk/renderer.h"

namespace pcbview::app {

struct Camera {
    float targetX = 0.0f, targetY = 0.0f, targetZ = 0.0f;
    float distance = 100.0f;
    float yaw = 0.0f;
    float pitch = 1.2f;
    float roll = 0.0f;  // about the view axis; right-drag vertical drives it
    float fovDegrees = 45.0f;
    bool orthographic = false;
};

class VulkanWindow : public QWindow {
    Q_OBJECT

public:
    explicit VulkanWindow(const geom::BoardMesh* mesh);
    ~VulkanWindow() override;

    // Swap the board. Re-uploads and re-frames if the renderer already exists;
    // otherwise the mesh is picked up when the window is first exposed. The
    // pointer must outlive the window -- MainWindow owns the mesh.
    void setMesh(const geom::BoardMesh* mesh);

    vk::Renderer* renderer() { return renderer_.get(); }
    Camera& camera() { return camera_; }

    // Graphics-device selection. Names of every usable GPU, the one currently
    // rendering, and whether it is running the ray-traced path. Switching tears
    // the device+renderer down and rebuilds them on the chosen GPU (the instance
    // and surface are kept), then persists the choice.
    QStringList availableGpuNames() const { return gpuNames_; }
    QString activeGpuName() const;
    bool rtAvailable() const;   // active device can do Vulkan ray queries (RT shadows)
    bool cpuRender() const;     // active device is a software (CPU) device
    bool ptAvailable() const;   // path tracing available (Vulkan RT on a GPU, Embree on CPU)
    void setPreferredGpu(const QString& nameSubstring);

    // Ray-traced shadows/AO on top of the raster shading. ALWAYS ON (no user
    // toggle; PCBVIEW_RT=0 is a headless hook for the flat preview); silently
    // absent when the device has no ray-query support.
    bool rayTracing() const { return rtEnabled_; }

    // Full path-tracing mode. Progressive: accumulates while the camera is still.
    // No-op without ray_query. Persisted.
    void setPathTracing(bool on);
    bool pathTracing() const { return ptEnabled_; }
    // Accumulated / target sample counts, for a progress readout.
    int ptSamples() const;
    int ptMaxSamples() const;

    // Intel OIDN neural denoising of the path-traced result. Persisted.
    void setDenoising(bool on);
    bool denoising() const { return oidnEnabled_; }

    // "Fast movement": while the board is being orbited / panned / zoomed /
    // exploded, drop to plain raster (no path tracing, no RT shadows) so a
    // low-power GPU or the CPU device stays interactive, then restore the
    // requested mode the instant motion settles. RT/PT at CPU speeds is a
    // slideshow otherwise. Persisted; default on.
    void setFastMovement(bool on);
    bool fastMovement() const { return fastMove_; }

    // Frame the whole board. Animates unless snap=true (board load/reload should
    // not swoop the camera).
    void frameBoard(bool snap = false);
    void setViewTop();
    void setViewBottom();
    void setViewIso();

    // Exploded view, peeled outside-in. `progress` counts stages: 0 is a solid
    // board, 1 has the outermost ring lifted, 2 has the next one lifted while
    // the first keeps travelling, and so on up to renderer()->maxRank().
    // Ctrl+wheel drives it.
    //
    // This sets a TARGET; the stack eases toward it over ~0.2s rather than
    // snapping. A wheel click is a discrete jump, so without this the layers
    // teleport between positions no matter how nicely the stages are eased.
    // Pass snap=true to jump immediately (startup, board reload).
    void setExplodeProgress(float progress, bool snap = false);
    float explodeProgress() const { return explodeProgress_; }

    // True while any view animation (camera glide, zoom, peel, spin) is
    // still easing toward its target -- the showcase engine waits on this
    // before starting a step's hold time.
    bool viewAnimating() const {
        const float d = explodeProgress_ - explodeTarget_;
        return cameraAnimating_ || zoomAnimating_ || spinActive_ ||
               pathActive_ || d > 1e-4f || d < -1e-4f;
    }

    // Constant-rate camera spin: sweep `degrees` about one axis (0 = yaw /
    // turntable, 1 = pitch / tumble, 2 = roll / twist, 3 = flip -- the
    // screen-vertical tumble right-drag does) over `seconds`. The eased
    // view glide cannot express this -- setViewTarget normalises to the
    // shortest way round, which a 360 by definition is not.
    void startSpin(int axis, float degrees, float seconds);

    // ---- offline (video) animation control ---------------------------------
    // Recording renders every video frame to full convergence, so animations
    // must not advance with wall time: pause them, then advance the virtual
    // clock explicitly between frames. Deterministic by construction -- the
    // same steppers run, just fed a fixed dt.
    void setAnimationsPaused(bool on) { animationsPaused_ = on; }
    void advanceAnimationsBy(double dt);

    // Camera aspect override for offscreen video: the projection normally
    // follows the window, but a recording at a different aspect must follow
    // the capture extent instead. 0 = follow the window.
    void setAspectOverride(float aspect) {
        aspectOverride_ = aspect;
        requestUpdate();
    }

    // ---- custom movement paths ---------------------------------------------
    // A recorded user movement: uniformly-sampled camera poses (plus the
    // peel), replayed with Catmull-Rom interpolation over `durationSec`.
    // The showcase's "record a custom move" feature samples cameraPose()
    // while the user drives, then plays the result back through this.
    struct PathKey {
        float yaw = 0, pitch = 0, roll = 0, distance = 0;
        float tx = 0, ty = 0, tz = 0;
        float explode = 0;
    };
    const Camera& cameraPose() const { return camera_; }
    void startPath(std::vector<PathKey> keys, double durationSec);

    // How far a ring travels per stage, scaled to the board's size.
    float explodeStepMm() const;

    // Orthographic half-height in mm at the orbit distance.
    //
    // It MATCHES the perspective frustum's half-height at that same distance,
    // so toggling projection keeps the board exactly the same size on screen.
    // It used to be distance/2 -- a constant unrelated to the FOV, which made
    // the O key jump the zoom by ~21%.
    //
    // THE ONE definition: the raster projection, the tracers' ray spans and
    // the 1:1 print scale all derive from this. A private copy in any of them
    // silently mis-scales a printed board.
    float orthoHalfHeight() const;

    // Distance from the orbit target to the farthest thing worth drawing
    // (board corner + a full peel + margin). Brackets the orthographic depth
    // range and pushes the ortho ray origin back, so a parallel projection
    // never clips geometry for sitting near or behind the camera plane.
    float sceneRadius() const;

    // Milliseconds for the last frame, smoothed. Zero until the first frame.
    double frameMs() const { return frameMs_; }

    // Measure mode: left-CLICK (press+release without a drag) sets the two
    // endpoints; the readout follows the cursor between them, snapping to
    // pad/drill centres and outline vertices. Orbit/pan/zoom stay live.
    void setMeasureMode(bool on);
    bool measureMode() const { return measureMode_; }
    // Fab-drawing style board width/height callouts.
    void setDimensionsOverlay(bool on);
    bool dimensionsOverlay() const { return dimsOverlay_; }
    // Headless hook (PCBVIEW_MEASURE): pin a measurement between two world
    // points as if the user had clicked them -- mouse picks cannot be
    // synthesised, but the rendering/readout path can still be verified.
    void setMeasurement(float ax, float ay, float az, float bx, float by,
                        float bz);
    // Re-resolve a pinned measurement's endpoint nets -- call after the net
    // table changes (pseudo-net inference), so the routed readout catches up.
    void refreshMeasurementNets();

    // Camera readout drawn INTO the frame (not the status bar), so a capture
    // carries the exact view that produced it and can be reproduced from the
    // headless hooks. Off by default.
    void setCameraHud(bool on) { cameraHud_ = on; requestUpdate(); }
    bool cameraHud() const { return cameraHud_; }

signals:
    // Live measurement readout for the status bar ("12.345 mm ..."), empty
    // when measuring is idle.
    void measureReadout(const QString& text);
    // Fired when the M key toggles the mode, so the menu checkbox follows.
    void measureModeChanged(bool on);
    // Likewise for the O key and the Orthographic action.
    void orthoChanged(bool on);
    // A click on the board picked this net (index into BoardMesh::nets), or
    // -1 for a click that hit no net-carrying feature. Only outside measure
    // mode, where a click places measurement points instead.
    void netPicked(int net, bool add);

    void frameRendered();
    void statusMessage(const QString& text);
    // Emitted once the renderer exists and a board is on the GPU. The stackup
    // tree cannot reconcile with renderer parts before this.
    void boardUploaded();

    // Keys this window does not consume, forwarded so the main window can run
    // menu actions.
    //
    // This exists because the viewport is a NATIVE QWindow: its key events never
    // enter the QWidget shortcut machinery, so QAction shortcuts (Ctrl+O, F5)
    // never fire while it has focus -- which is nearly always. Setting
    // Qt::ApplicationShortcut does not help, and an application-level event
    // filter never sees these events either. Both were tried. Forwarding from
    // the one place known to receive them is the reliable route.
    void unhandledKey(int key, Qt::KeyboardModifiers modifiers);
    // R while the viewport has focus: toggle showcase movement recording --
    // the mouse is busy driving the camera, so it must be a key.
    void moveRecordToggled();
    // progress in stages, and the total number of stages a full peel takes.
    void explodeChanged(float progress, float maxProgress);

    // The device switch needs a WHOLE NEW viewport window. Switching the
    // presenting driver (hardware GPU <-> the software CPU driver) on the same
    // native HWND leaves the Windows compositor stuck on the old swapchain --
    // frames present "successfully" but never reach the screen. Recreating just
    // the platform window in place breaks the QWidget::createWindowContainer
    // embedding (the container keeps the dead native handle), so the OWNER must
    // rebuild the container + VulkanWindow pair. MainWindow connects QUEUED and
    // does exactly that; the persisted settings carry every preference across.
    void viewportRebuildRequired();

protected:
    void exposeEvent(QExposeEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    bool event(QEvent*) override;

private:
    void initialise();
    void createDeviceAndRenderer();  // (re)build device + renderer on the chosen GPU
    void render();

    // Advance the peel toward its target. Returns true while still moving, which
    // is what keeps the on-demand renderer ticking during the animation.
    bool stepExplodeAnimation();
    void pushExplode();

    // Ease the camera toward viewTarget_. Same on-demand-driven pattern as the
    // peel: returns true while still moving so render() keeps the loop alive.
    // View presets and Fit set a target and let this glide there.
    void setViewTarget(const Camera& dest, bool snap);
    bool stepCameraAnimation();
    bool stepSpinAnimation();
    bool stepPathAnimation();
    std::vector<PathKey> pathKeys_;
    double pathT_ = 0.0, pathDuration_ = 0.0;
    bool pathActive_ = false;
    QElapsedTimer pathClock_;
    void applyGlobeTumble(float ax);
    // Wall-clock dt with a stall clamp, or the video recorder's fixed dt.
    double clockDt(QElapsedTimer& clock) {
        if (fixedDt_ >= 0.0) return fixedDt_;
        return std::min(static_cast<double>(clock.restart()) / 1000.0, 0.1);
    }
    double fixedDt_ = -1.0;
    bool animationsPaused_ = false;
    float aspectOverride_ = 0.0f;
    bool spinActive_ = false;
    int spinAxis_ = 0;          // 0 yaw, 1 pitch, 2 roll
    float spinRemaining_ = 0;   // radians still to sweep (signed)
    float spinRate_ = 0;        // radians per second (signed)
    QElapsedTimer spinClock_;

    // Tear down everything that depends on the VkSurfaceKHR. Must run BEFORE Qt
    // destroys the platform window, or the surface outlives its swapchain and
    // validation (rightly) objects. Driven by QEvent::PlatformSurface.
    void releaseResources();

    // --- Measurement overlay ---
    // World -> framebuffer pixels via the same viewProj the frame rendered
    // with; cursor -> board point by unprojecting through its inverse and
    // intersecting the board-top plane, with snap targets checked first.
    bool worldToScreen(const glm::vec3& w, float& px, float& py) const;
    // `net` returns the point's index into mesh_->nets, -1 when nothing under
    // it names one. Resolution order: snap point, nearest track segment, and
    // (with `deep`, clicks only -- too slow for hover) the copper triangle
    // containing the point, which covers pads and pours on real and derived
    // nets alike.
    bool screenToBoard(const QPointF& posDip, glm::vec3& out, bool& snapped,
                       int& net, bool deep = false);
    void handleMeasureClick(const QPointF& posDip);
    int netAtWorld(const glm::vec3& p) const;
    // Rebuild the renderer overlay (measure line + dimension callouts) for
    // this frame. Cheap; called from render() after the matrices are known.
    void buildOverlay();

    // Push the live measurement text out via measureReadout.
    void updateReadout();

    const geom::BoardMesh* mesh_ = nullptr;

    QVulkanInstance qtInstance_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;  // owned by the instance, kept across device swaps
    Device device_;
    std::unique_ptr<vk::Renderer> renderer_;
    QStringList gpuNames_;         // every usable GPU, for the picker
    QString preferredGpu_;         // name substring; empty = auto (discrete + RT)
    bool rtEnabled_ = false;       // ray-traced shadows/AO requested
    bool ptEnabled_ = false;       // path-tracing mode requested
    bool oidnEnabled_ = false;     // neural denoising requested
    bool fastMove_ = true;         // drop to raster while the view is moving

    // Apply the fast-movement rule for this frame. Switches the renderer between
    // the requested mode and plain raster ONLY on a transition, so PT
    // accumulation is not reset every frame while the view is still. `moving` is
    // any orbit/pan/zoom/explode/glide in progress.
    void applyMotionQuality(bool moving);
    bool motionDowngraded_ = false;  // currently forced to raster by motion

    Camera camera_;
    Camera viewTarget_;             // where a view preset / Fit is gliding to
    bool cameraAnimating_ = false;
    QElapsedTimer cameraClock_;
    float explodeProgress_ = 0.0f;  // animated
    float explodeTarget_ = 0.0f;    // where the wheel put it
    bool explodeAnimating_ = false;
    QElapsedTimer explodeClock_;

    // Wheel zoom glides to a target distance instead of stepping -- same
    // exponential-approach treatment as the peel, so rapid clicks compound into
    // one smooth dolly rather than a stutter of jumps.
    float zoomTarget_ = 0.0f;
    bool zoomAnimating_ = false;
    QElapsedTimer zoomClock_;
    bool stepZoomAnimation();

    // Zoom-to-cursor: the wheel zooms INTO the world point under the cursor
    // rather than the screen centre. computeZoomAnchor captures that point (on
    // the focal plane through the pivot) when the wheel turns; stepZoomAnimation
    // slides the pivot to keep it fixed under the cursor as `distance` changes.
    bool computeZoomAnchor(const QPointF& posDip);
    bool zoomToCursor_ = false;
    glm::vec3 zoomAnchorK_{0.0f};        // pivot shift per unit distance change
    glm::vec3 zoomAnchorTarget0_{0.0f};  // pivot at the moment of anchoring
    float zoomAnchorDist0_ = 0.0f;       // distance at the moment of anchoring
    bool initialised_ = false;
    bool dragging_ = false;
    bool draggingInv_ = false;  // right-drag: yaw (horizontal) + roll (vertical)
    bool panning_ = false;
    QPointF lastPos_;
    double frameMs_ = 0.0;

    // --- Measurement state ---
    bool measureMode_ = false;
    bool dimsOverlay_ = false;
    int measureStage_ = 0;  // 0 idle, 1 first point placed, 2 pinned
    glm::vec3 measureA_{0.0f};
    glm::vec3 measureB_{0.0f};
    int measureANet_ = -1;  // net of each snapped endpoint (-1 = none)
    int measureBNet_ = -1;
    bool haveHover_ = false;
    bool hoverSnapped_ = false;
    glm::vec3 hover_{0.0f};
    int hoverNet_ = -1;
    QPointF cursorPos_;          // device-independent px, for hover re-picks
    QPointF pressPos_;
    bool clickCandidate_ = false;  // press seen; becomes a pick if no drag
    glm::mat4 lastViewProj_{1.0f};
    bool haveViewProj_ = false;
    bool cameraHud_ = false;
};

}  // namespace pcbview::app
