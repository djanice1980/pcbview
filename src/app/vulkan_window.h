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
#include <glm/gtc/quaternion.hpp>

#include <memory>

#include "geom/tessellate.h"
#include "input/gamepad.h"
#include "xr/xr_system.h"
#include "render/common/device.h"
#include "render/vk/renderer.h"

class QTimer;

namespace pcbview::app {

using pcbview::input::Gamepad;

// The board's own orientation in the world, independent of the camera.
//
// Why this exists at all: until now the board was nailed to the origin and
// "turning it" meant orbiting the camera the other way. Those are visually
// identical -- everything world-fixed in this scene is directional -- but the
// board had no pose, and VR breaks the trick outright, because there the
// camera IS the user's head and the app cannot counter-rotate it.
//
// How it is applied is the subtle part. Rather than transforming a million
// vertices, the CAMERA is transformed into the board's frame once per frame
// (see render()). That works because every consumer downstream -- picking, the
// translucent depth sort, the Z-axis explode, netPathLength's 2D test, the
// BLAS/TLAS, the net-span and net-light tables -- is ALREADY written in board
// space; it was simply called world space when the two always coincided.
// Transforming rays into object space is the standard ray-tracing move, and it
// keeps the acceleration structures static (no per-frame TLAS rebuild) and the
// path tracer's world-hit-point / model-normal mix consistent.
struct BoardPose {
    // Rotation about the board's bounds centre, so the centre holds still and
    // the orbit target stays valid without transforming it.
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    // WORLD-space offset, applied after the rotation. This is what lets the
    // board leave the middle of the frame: rotation alone only ever spins it
    // in place, which is indistinguishable from a turntable no matter which
    // way it turns. Sliding it through space while the viewpoint stays put is
    // the thing that actually reads as "I moved the object".
    glm::vec3 translation = glm::vec3(0.0f);
};

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
               pathActive_ || timedZoomActive_ || d > 1e-4f || d < -1e-4f;
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

    // Timed zoom to a percentage of the DEFAULT FRAMED size (100% = the
    // distance frameBoard would pick, so "zoom to 100% in 3s" lands the
    // board at its loaded size). Distance only -- yaw/pitch/roll and the
    // target stay wherever the user placed them -- swept at constant rate
    // in log space over `seconds`.
    void startTimedZoom(float percent, float seconds);
    float framedDistance() const;

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
    // Object mode came on or off from the pad, so the UI can say so -- a
    // toggle with no readout is a trap.
    void objectModeChanged(bool on);
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
    // The board's pose, and the point it turns about (its bounds centre, in
    // board space). Kept as a quaternion so repeated small rotations cannot
    // accumulate shear or scale the way a re-multiplied matrix does.
    BoardPose board_;
    glm::vec3 boardPivot() const;
    // OBJECT MODE selects the MECHANISM, not the direction. The gesture maths
    // is identical either way, so the board appears to move the same; what
    // changes is which thing actually turns, and with the sun fixed in the
    // world that difference is plainly visible:
    //   object mode - the board turns, light sweeps ACROSS it, sky holds still
    //   view mode   - the camera turns, the board's shading holds, sky sweeps
    // An earlier attempt had BOTH modes turning the board and differing only in
    // sign, which is exactly why they felt like the same thing.
    // Latched at mouse-press so releasing Shift mid-drag cannot change the
    // sense of a drag underneath you. The pad's copy is a TOGGLE, not a hold: a
    // stick click you must keep held while pushing that same stick is unusable.
    bool objectDrag_ = false;
    bool padObjectMode_ = false;
    // VR. Non-null only when PCBVIEW_VR was set AND a headset answered; it is
    // created before the Vulkan instance because the runtime has to wrap that
    // creation. Absent, everything below behaves as an ordinary desktop
    // session.
    std::unique_ptr<xr::System> xrSystem_;
    // The live session and its frame loop. Driven by its own timer rather than
    // the on-demand render path, because the runtime paces frames (xrWaitFrame
    // blocks until the headset wants one) and will not wait for a mouse move.
    std::unique_ptr<xr::VrSession> vr_;
    QTimer* vrTimer_ = nullptr;
    // Whether the runtime asked for a frame on the last stepVr. False with the
    // headset off (proximity sensor drops the session out of FOCUSED), which is
    // what lets the desktop window take the renderer back instead of freezing.
    bool vrRendering_ = false;
    // Set while VR holds the renderer, so handing back to the window happens
    // once on the transition rather than every idle frame.
    bool vrOwnsRenderer_ = false;
    // Counts frames the runtime asked for, so rendering can run at a divisor
    // of the headset's rate with the compositor reprojecting the gaps.
    unsigned long long vrFrameCount_ = 0;
    // Path-trace samples standing in each eye's accumulator, reported
    // periodically. Climbing means accumulation works and only needs longer;
    // pinned near 1 means something resets it every frame. The two are
    // indistinguishable through the lenses.
    int vrSamples_[2] = {0, 0};
    void stepVr();

    // Showcase routing: the orientation actually rendered from while active,
    // and the last camera orientation already handed to the board.
    bool routeToBoard_ = false;
    Camera routeAnchor_;
    Camera routePrev_;

public:
    // Turn the board itself about a WORLD-space axis (the rotation is composed
    // on the left, i.e. applied in the parent frame). This is what VR will
    // drive from a Sense controller's grip.
    void rotateBoard(const glm::vec3& axisWorldSpace, float radians);
    void setBoardRotation(const glm::quat& q);

    // Turn the board by exactly as much as moving the camera from `before` to
    // `after` would have appeared to, and leave the camera alone.
    //
    // This is how every rotation gesture is routed, and it exists to kill a
    // whole bug class. Rendering an un-rotated board from camera orientation B
    // is the SAME IMAGE as rendering a board rotated by A*inverse(B) from
    // camera orientation A. So instead of hand-deriving an axis and a sign for
    // each gesture -- which is exactly what has gone wrong repeatedly here --
    // the gesture is run against a throwaway copy of the camera and the
    // resulting orientation change is handed over wholesale. The on-screen
    // result is identical to the old camera-orbit code BY CONSTRUCTION.
    void adoptCameraDeltaIntoBoard(const Camera& before, const Camera& after);

    // Send every camera-authored rotation to the BOARD instead, until turned
    // off again. This is how the showcase plays its existing moves as object
    // motion: the spin/path/preset steppers keep animating camera_ exactly as
    // they always have -- so nothing persisted changes and a PathKey still
    // means the same picture -- while the scene renders from a frozen anchor
    // and the accumulated change lands on the board.
    //
    // Deliberately NOT done by resetting camera_ each frame: the steppers ease
    // TOWARD a target, so rewinding them mid-flight means they never converge
    // and re-apply the same delta forever.
    void setRotationRoutedToBoard(bool on);
    bool rotationRoutedToBoard() const { return routeToBoard_; }

    // Undo whichever mode moved things: the board goes square-on and back to
    // the origin, and the camera returns to its opening orientation, framed.
    // Snaps rather than glides -- a recentre is a command, not a move.
    void recenterAll();
    // Accumulators for the VR configuration sweep's current window.
    double sweepGpuMs_ = 0.0;
    double sweepDist_ = 0.0;
    int sweepSamples_ = 0;
    const BoardPose& boardPose() const { return board_; }
    // Board -> world. Rotation happens about the bounds centre so the centre
    // holds still and the camera's orbit target stays meaningful.
    glm::mat4 boardMatrix() const;

private:
    // Controller. Polled on its own timer because rendering is on demand: with
    // nothing moving there are no frames, so there would be nothing to notice a
    // stick being pushed. A poll that finds motion requests a frame, and from
    // there the render loop carries it like any other animation.
    void stepGamepad();
    input::Gamepad gamepad_;
    QTimer* padTimer_ = nullptr;
    QElapsedTimer padClock_;
    bool padSteering_ = false;
    // Gyro zero-rate bias. A DualSense at rest reports roughly 0.01 rad/s, not
    // zero (measured), which is ~0.6 deg/s -- enough to rotate the board 38
    // degrees a minute on its own. Learned continuously while NOT grabbing,
    // then subtracted, so the board is dead still until the pad actually moves.
    float gyroBias_[3] = {0.0f, 0.0f, 0.0f};
    bool gyroBiasReady_ = false;

    bool stepTimedZoomAnimation();
    bool timedZoomActive_ = false;
    float tzStart_ = 0.0f, tzTarget_ = 0.0f;
    float tzFromTarget_[3] = {0, 0, 0}, tzToTarget_[3] = {0, 0, 0};
    double tzT_ = 0.0, tzDur_ = 0.0;
    QElapsedTimer tzClock_;
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
