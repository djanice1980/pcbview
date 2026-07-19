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

    // Ray-traced shadows/AO on top of the raster shading. No-op if the device
    // has no ray-query support. Persisted.
    void setRayTracing(bool on);
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

    // How far a ring travels per stage, scaled to the board's size.
    float explodeStepMm() const;

    // Milliseconds for the last frame, smoothed. Zero until the first frame.
    double frameMs() const { return frameMs_; }

signals:
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

    // Tear down everything that depends on the VkSurfaceKHR. Must run BEFORE Qt
    // destroys the platform window, or the surface outlives its swapchain and
    // validation (rightly) objects. Driven by QEvent::PlatformSurface.
    void releaseResources();

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
    bool initialised_ = false;
    bool dragging_ = false;
    bool draggingInv_ = false;  // right-drag: mirrored orbit
    bool panning_ = false;
    QPointF lastPos_;
    double frameMs_ = 0.0;
};

}  // namespace pcbview::app
