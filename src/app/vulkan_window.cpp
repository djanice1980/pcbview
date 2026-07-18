#include "app/vulkan_window.h"

#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QSettings>
#include <QWheelEvent>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE  // Vulkan clip space, not OpenGL's
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace pcbview::app {
namespace {

struct Basis {
    glm::vec3 eye;
    glm::vec3 forward;  // eye -> target
    glm::vec3 right;
    glm::vec3 up;
};

// Camera basis for a turntable that can rotate through the poles forever.
//
// The obvious lookAt(eye, target, worldUp) degenerates when you look straight
// down: forward becomes parallel to up and the cross product collapses. The
// usual bodge is to clamp pitch just shy of vertical, which is exactly the
// "rotation hits a wall" behaviour we do not want.
//
// Instead `right` is derived from YAW ALONE. It is horizontal by construction,
// always unit length, always perpendicular to forward, and never degenerate --
// so pitch is free to wrap the whole way round. Past vertical, up naturally
// comes out inverted and the view goes upside down, which is what continuing to
// rotate should do.
Basis cameraBasis(const Camera& c) {
    const float cosP = std::cos(c.pitch);
    const float sinP = std::sin(c.pitch);
    const glm::vec3 target{c.targetX, c.targetY, c.targetZ};
    const glm::vec3 offset{cosP * std::sin(c.yaw), -cosP * std::cos(c.yaw), sinP};

    Basis b;
    b.eye = target + c.distance * offset;
    b.forward = -offset;
    b.right = glm::vec3(std::cos(c.yaw), std::sin(c.yaw), 0.0f);
    b.up = glm::cross(b.right, b.forward);
    return b;
}

// Right-handed view matrix from an explicit basis. Same construction as
// glm::lookAt, minus the up-vector cross product that blows up at the poles.
glm::mat4 viewFromBasis(const Basis& b) {
    glm::mat4 v(1.0f);
    v[0][0] = b.right.x;    v[1][0] = b.right.y;    v[2][0] = b.right.z;
    v[0][1] = b.up.x;       v[1][1] = b.up.y;       v[2][1] = b.up.z;
    v[0][2] = -b.forward.x; v[1][2] = -b.forward.y; v[2][2] = -b.forward.z;
    v[3][0] = -glm::dot(b.right, b.eye);
    v[3][1] = -glm::dot(b.up, b.eye);
    v[3][2] = glm::dot(b.forward, b.eye);
    return v;
}

// Infinite reversed-Z perspective. Depth is 1.0 at the near plane and tends to
// 0.0 at infinity; the renderer clears depth to 0 and compares with GREATER.
//
// Two things come free. There is no far plane, so nothing can ever be clipped
// away behind the board. And reversed-Z puts the float32 exponent's dense region
// (near zero) where the hyperbolic depth curve is coarsest, so the two errors
// cancel and precision is near-uniform instead of being hoarded at the near
// plane. That is what kills the zoom-dependent flicker between copper at 1.590
// and mask at 1.600.
glm::mat4 infiniteReverseZPerspective(float fovYRadians, float aspect,
                                      float zNear) {
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);
    glm::mat4 p(0.0f);
    p[0][0] = f / aspect;
    p[1][1] = f;
    p[2][3] = -1.0f;
    p[3][2] = zNear;
    return p;
}

// Reversed-Z orthographic: swap near/far so depth runs 1 -> 0 like the
// perspective path, keeping one depth convention for both.
glm::mat4 reverseZOrtho(float l, float r, float b, float t, float zNear,
                        float zFar) {
    return glm::ortho(l, r, b, t, zFar, zNear);
}

// Fraction of each explode stage spent moving. The remainder is the DWELL --
// scrolling through it changes nothing, so the stack holds still and you can
// stop and look at the ring that just peeled. Without this the layers slide
// continuously and it is fiddly to land on a clean state.
constexpr float kExplodeMoveFraction = 0.62f;

// Turn raw scroll progress into eased progress with a dwell at each stage.
//
// Within a stage: move (smoothstepped) for the first kExplodeMoveFraction, then
// hold. The held value is exactly the stage boundary, so a dwell is a genuinely
// stationary stack rather than a slow crawl.
float easedExplodeProgress(float raw) {
    const float stage = std::floor(raw);
    const float frac = raw - stage;
    float t = std::clamp(frac / kExplodeMoveFraction, 0.0f, 1.0f);
    t = t * t * (3.0f - 2.0f * t);  // smoothstep: ease in and out of each stage
    return stage + t;
}

// Keep pitch bounded so it cannot drift to a value where float precision starts
// to bite after a few thousand revolutions.
float wrapPi(float a) {
    constexpr float kTwoPi = 6.28318530718f;
    while (a > 3.14159265f) a -= kTwoPi;
    while (a < -3.14159265f) a += kTwoPi;
    return a;
}

}  // namespace

VulkanWindow::VulkanWindow(const geom::BoardMesh* mesh) : mesh_(mesh) {
    setSurfaceType(QSurface::VulkanSurface);
}

void VulkanWindow::setMesh(const geom::BoardMesh* mesh) {
    mesh_ = mesh;
    if (!renderer_ || !mesh_) return;
    // The GPU may still be reading the old buffers.
    renderer_->waitIdle();
    renderer_->uploadBoard(*mesh_);
    // maxRank changes with the board, so re-clamp and re-push. Snap: a new board
    // should not animate its way open.
    setExplodeProgress(explodeTarget_, /*snap=*/true);
    frameBoard(/*snap=*/true);
    requestUpdate();
}

VulkanWindow::~VulkanWindow() {
    releaseResources();

    destroyDebugMessenger(instance_, messenger_);
    messenger_ = VK_NULL_HANDLE;

    // qtInstance_ wraps a VkInstance we created, so we destroy it -- Qt only
    // destroys instances it made itself.
    if (instance_ != VK_NULL_HANDLE) {
        qtInstance_.destroy();
        vkDestroyInstance(instance_, nullptr);
        instance_ = VK_NULL_HANDLE;
    }
}

void VulkanWindow::releaseResources() {
    // Order matters: the swapchain must die before the surface it was made from.
    // Qt destroys the surface with the platform window, which happens before
    // ~VulkanWindow, so this is driven from QEvent::PlatformSurface too.
    if (renderer_) {
        renderer_->waitIdle();
        renderer_.reset();
    }
    if (device_.handle != VK_NULL_HANDLE) {
        vkDestroyDevice(device_.handle, nullptr);
        device_ = {};
    }
    initialised_ = false;
}

void VulkanWindow::initialise() {
    if (initialised_) return;

    // Surface extensions for this platform. Qt will create the surface, but the
    // instance is ours, so we must enable them ourselves.
    std::vector<const char*> extensions{VK_KHR_SURFACE_EXTENSION_NAME};
#ifdef Q_OS_WIN
    extensions.push_back("VK_KHR_win32_surface");
#elif defined(Q_OS_LINUX)
    extensions.push_back("VK_KHR_xcb_surface");
#endif

    instance_ = createInstance(/*enableValidation=*/true, extensions);
    messenger_ = createDebugMessenger(instance_);

    // Hand Qt OUR instance rather than letting it make one. This is what keeps
    // the RT extension setup ours.
    qtInstance_.setVkInstance(instance_);
    if (!qtInstance_.create()) {
        throw std::runtime_error(
            "QVulkanInstance::create() failed: " +
            std::to_string(qtInstance_.errorCode()));
    }
    setVulkanInstance(&qtInstance_);

    surface_ = QVulkanInstance::surfaceForWindow(this);
    if (surface_ == VK_NULL_HANDLE) {
        throw std::runtime_error("Qt produced no VkSurfaceKHR for this window");
    }

    // Device preference: an explicit env override wins, else the persisted pick.
    const QByteArray envGpu = qgetenv("PCBVIEW_GPU");
    preferredGpu_ = !envGpu.isEmpty() ? QString::fromLocal8Bit(envGpu)
                                      : QSettings().value("gpuName").toString();
    // PCBVIEW_RT=1/0 forces the ray-traced path for a headless capture; otherwise
    // the persisted setting decides.
    rtEnabled_ = qEnvironmentVariableIsSet("PCBVIEW_RT")
                     ? qgetenv("PCBVIEW_RT").toInt() != 0
                     : QSettings().value("rayTracing", false).toBool();
    ptEnabled_ = qEnvironmentVariableIsSet("PCBVIEW_PT")
                     ? qgetenv("PCBVIEW_PT").toInt() != 0
                     : QSettings().value("pathTracing", false).toBool();
    oidnEnabled_ = qEnvironmentVariableIsSet("PCBVIEW_OIDN")
                       ? qgetenv("PCBVIEW_OIDN").toInt() != 0
                       : QSettings().value("denoising", true).toBool();

    createDeviceAndRenderer();

    frameBoard(/*snap=*/true);
    initialised_ = true;

    // Re-apply state configured before the renderer existed. The renderer is
    // only created on first expose, so anything MainWindow set during
    // construction went to a null pointer and was dropped. Snap rather than
    // animate -- the board should not peel itself open on startup.
    setExplodeProgress(explodeTarget_, /*snap=*/true);
    emit boardUploaded();
}

void VulkanWindow::createDeviceAndRenderer() {
    // Enumerate every usable GPU (for the picker) and choose one by preference,
    // falling back to discrete + RT-ready.
    const auto gpus = enumerateGpus(instance_);
    gpuNames_.clear();
    for (const GpuInfo& g : gpus) {
        if (g.usable()) gpuNames_ << QString::fromStdString(g.name);
    }
    const GpuInfo* pick = selectGpu(gpus, preferredGpu_.toStdString());
    if (!pick) throw std::runtime_error("no usable Vulkan device");

    VkBool32 canPresent = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(pick->handle, pick->graphicsQueueFamily,
                                         surface_, &canPresent);
    if (!canPresent) {
        throw std::runtime_error(
            "selected GPU's graphics queue cannot present to this surface");
    }

    device_ = createDevice(*pick, {VK_KHR_SWAPCHAIN_EXTENSION_NAME});

    const qreal dpr = devicePixelRatio();
    renderer_ = std::make_unique<vk::Renderer>(
        device_, surface_, static_cast<uint32_t>(width() * dpr),
        static_cast<uint32_t>(height() * dpr));
    renderer_->setRayTracing(rtEnabled_ && device_.rayQueryEnabled);
    if (qEnvironmentVariableIsSet("PCBVIEW_PT_SPP"))
        renderer_->setMaxSamples(qgetenv("PCBVIEW_PT_SPP").toInt());
    renderer_->setRenderMode(ptEnabled_ && device_.rayQueryEnabled
                                 ? vk::RenderMode::PathTraced
                                 : vk::RenderMode::Raster);
    renderer_->setDenoising(oidnEnabled_);
    if (mesh_) renderer_->uploadBoard(*mesh_);

    emit statusMessage(
        QString("%1  |  ray tracing: %2  |  %3 triangles")
            .arg(QString::fromStdString(device_.gpu.name))
            .arg(!device_.rayQueryEnabled ? "unsupported"
                 : rtEnabled_             ? "ON"
                                          : "available (off)")
            .arg(renderer_->stats().trianglesTotal));

    // Headless verification hook: dump the chosen device + RT state to a file so
    // GPU selection can be confirmed without reading the status bar.
    const QByteArray reportPath = qgetenv("PCBVIEW_GPU_REPORT");
    if (!reportPath.isEmpty()) {
        QFile f(QString::fromLocal8Bit(reportPath));
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            f.write(QString("device=%1\nrayQuery=%2\nrtRequested=%3\noidnDevice=%4\n")
                        .arg(QString::fromStdString(device_.gpu.name))
                        .arg(device_.rayQueryEnabled ? "yes" : "no")
                        .arg(rtEnabled_ ? "yes" : "no")
                        .arg(QString::fromStdString(renderer_->oidnDeviceName()))
                        .toUtf8());
        }
    }
}

QString VulkanWindow::activeGpuName() const {
    return QString::fromStdString(device_.gpu.name);
}

bool VulkanWindow::rtAvailable() const { return device_.rayQueryEnabled; }

void VulkanWindow::setPreferredGpu(const QString& nameSubstring) {
    preferredGpu_ = nameSubstring;
    QSettings().setValue("gpuName", nameSubstring);
    if (!initialised_ || !renderer_) return;

    // Rebuild the device and renderer on the chosen GPU. Instance and surface are
    // kept; the camera and explode state persist across the swap.
    renderer_->waitIdle();
    renderer_.reset();
    if (device_.handle != VK_NULL_HANDLE) {
        vkDestroyDevice(device_.handle, nullptr);
        device_ = {};
    }
    createDeviceAndRenderer();
    setExplodeProgress(explodeTarget_, /*snap=*/true);
    emit boardUploaded();
    requestUpdate();
}

void VulkanWindow::setRayTracing(bool on) {
    rtEnabled_ = on;
    QSettings().setValue("rayTracing", on);
    if (renderer_) renderer_->setRayTracing(on && device_.rayQueryEnabled);
    emit statusMessage(QString("Ray tracing %1")
                           .arg(!rtAvailable() ? "unsupported on this GPU"
                                : on            ? "ON"
                                                : "off"));
    requestUpdate();
}

void VulkanWindow::setPathTracing(bool on) {
    ptEnabled_ = on;
    QSettings().setValue("pathTracing", on);
    if (renderer_) {
        renderer_->setRenderMode(on && device_.rayQueryEnabled
                                     ? vk::RenderMode::PathTraced
                                     : vk::RenderMode::Raster);
    }
    emit statusMessage(QString("Path tracing %1")
                           .arg(!rtAvailable() ? "unsupported on this GPU"
                                : on            ? "ON — accumulating…"
                                                : "off"));
    requestUpdate();
}

int VulkanWindow::ptSamples() const {
    return renderer_ ? renderer_->accumulatedSamples() : 0;
}
int VulkanWindow::ptMaxSamples() const {
    return renderer_ ? renderer_->maxSamples() : 0;
}

void VulkanWindow::setDenoising(bool on) {
    oidnEnabled_ = on;
    QSettings().setValue("denoising", on);
    if (renderer_) renderer_->setDenoising(on);
    const QString dev =
        renderer_ ? QString::fromStdString(renderer_->oidnDeviceName()) : "?";
    emit statusMessage(QString("Neural denoise %1%2")
                           .arg(on ? "ON" : "off")
                           .arg(on ? "  |  OIDN device: " + dev : QString()));
    requestUpdate();
}

void VulkanWindow::setViewTarget(const Camera& dest, bool snap) {
    viewTarget_ = dest;
    // A view preset owns the distance now; a leftover wheel glide would fight it.
    zoomAnimating_ = false;
    // Orthographic and FOV switch immediately -- they are not eased as scalars.
    camera_.orthographic = dest.orthographic;
    camera_.fovDegrees = dest.fovDegrees;

    // Take the SHORTEST way round in yaw: bring the target within +-pi of where
    // the camera is now, so a spin from iso to top never takes the long way.
    constexpr float kPi = 3.14159265f, kTwoPi = 6.28318531f;
    while (viewTarget_.yaw - camera_.yaw > kPi) viewTarget_.yaw -= kTwoPi;
    while (viewTarget_.yaw - camera_.yaw < -kPi) viewTarget_.yaw += kTwoPi;

    // Snap when asked, OR before the first frame exists. A view configured
    // pre-expose (env hooks, a CLI path) must persist -- if it started an
    // animation, initialise()'s frameBoard() would clobber it a moment later.
    // This is the same before-first-expose trap that has bitten substrate and
    // explode state.
    if (snap || !initialised_) {
        camera_ = viewTarget_;
        cameraAnimating_ = false;
    } else {
        cameraAnimating_ = true;
        cameraClock_.restart();
    }
    requestUpdate();
}

void VulkanWindow::frameBoard(bool snap) {
    if (!mesh_) return;
    const auto& b = mesh_->bounds;
    Camera dest = camera_;
    dest.targetX = static_cast<float>((b.min[0] + b.max[0]) * 0.5);
    dest.targetY = static_cast<float>((b.min[1] + b.max[1]) * 0.5);
    dest.targetZ = static_cast<float>((b.min[2] + b.max[2]) * 0.5);

    const float spanX = static_cast<float>(b.max[0] - b.min[0]);
    const float spanY = static_cast<float>(b.max[1] - b.min[1]);
    const float span = std::max(spanX, spanY);

    // Back off far enough that the larger dimension fits the vertical FOV with
    // a little margin.
    const float halfFov = glm::radians(dest.fovDegrees) * 0.5f;
    dest.distance = (span * 0.62f) / std::tan(halfFov);
    setViewTarget(dest, snap);
}

void VulkanWindow::setViewTop() {
    Camera dest = camera_;
    dest.yaw = 0.0f;
    // Exactly vertical is fine now: the basis comes from yaw, so there is no
    // pole to avoid. This used to need a fudge factor.
    dest.pitch = 1.57079633f;  // +pi/2
    setViewTarget(dest, false);
}

void VulkanWindow::setViewBottom() {
    Camera dest = camera_;
    dest.yaw = 0.0f;
    dest.pitch = -1.57079633f;  // -pi/2
    setViewTarget(dest, false);
}

void VulkanWindow::setViewIso() {
    Camera dest = camera_;
    dest.yaw = 0.6f;
    dest.pitch = 0.62f;
    setViewTarget(dest, false);
}

void VulkanWindow::render() {
    if (!initialised_ || !renderer_) return;

    // The peel eases toward its target here rather than on a QTimer: rendering
    // is on demand, so the animation drives the frames and the frames drive the
    // animation. When it settles, the loop stops and the GPU goes idle again.
    const bool exploding = stepExplodeAnimation();
    const bool gliding = stepCameraAnimation();
    const bool zooming = stepZoomAnimation();
    const bool stillAnimating = exploding || gliding || zooming;

    QElapsedTimer timer;
    timer.start();

    const qreal dpr = devicePixelRatio();
    const float w = static_cast<float>(width() * dpr);
    const float h = static_cast<float>(height() * dpr);
    if (w < 1.0f || h < 1.0f) return;

    const Basis basis = cameraBasis(camera_);
    const glm::vec3 eye = basis.eye;
    const glm::mat4 view = viewFromBasis(basis);

    // Near plane scales with the orbit distance rather than sitting at a fixed
    // hair's breadth. Reversed-Z makes precision forgiving, but there is no
    // reason to ask for a 200,000:1 range on a 50mm board.
    const float zNear = std::max(camera_.distance * 0.005f, 0.02f);

    glm::mat4 proj;
    if (camera_.orthographic) {
        const float halfH = camera_.distance * 0.5f;
        const float halfW = halfH * (w / h);
        proj = reverseZOrtho(-halfW, halfW, -halfH, halfH, zNear,
                             camera_.distance * 4.0f + 1000.0f);
    } else {
        proj = infiniteReverseZPerspective(glm::radians(camera_.fovDegrees),
                                           w / h, zNear);
    }
    proj[1][1] *= -1.0f;  // Vulkan's Y is flipped relative to GL
    const glm::mat4 viewProj = proj * view;

    // Path tracer needs the camera as a ray basis (eye + pixel-plane spans), not
    // a matrix. Setting it resets accumulation whenever the view changed.
    if (renderer_->renderMode() == vk::RenderMode::PathTraced) {
        const float tanY = std::tan(glm::radians(camera_.fovDegrees) * 0.5f);
        const float tanX = tanY * (w / h);
        const glm::vec3 fwd = glm::normalize(basis.forward);
        const glm::vec3 right = basis.right * tanX;
        const glm::vec3 up = basis.up * tanY;
        renderer_->setRayCamera(&eye[0], &fwd[0], &right[0], &up[0]);
    }

    if (!renderer_->drawFrame(&viewProj[0][0], &eye[0])) {
        renderer_->resize(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    }

    const double ms = static_cast<double>(timer.nsecsElapsed()) / 1.0e6;
    frameMs_ = (frameMs_ == 0.0) ? ms : frameMs_ * 0.9 + ms * 0.1;
    emit frameRendered();

    // Continuous asynchronous denoise: whenever the still camera has fresh
    // samples, a pass is read back (fenced, never waited), filtered on a worker
    // thread, and displayed when it lands. No milestones, no UI stalls -- the
    // image cleans up within a couple of frames of the camera stopping and then
    // refines continuously. Keep frames coming while a pass is in flight so the
    // state machine advances.
    if (renderer_->renderMode() == vk::RenderMode::PathTraced && oidnEnabled_) {
        if (renderer_->denoiseTick()) requestUpdate();
    }

    // Keep the loop alive while the peel moves OR the path tracer is still
    // accumulating samples toward convergence.
    const bool accumulating =
        renderer_->renderMode() == vk::RenderMode::PathTraced &&
        renderer_->accumulatedSamples() < renderer_->maxSamples();
    if (stillAnimating || accumulating) requestUpdate();
}

void VulkanWindow::exposeEvent(QExposeEvent*) {
    if (isExposed()) {
        initialise();
        render();
    }
}

void VulkanWindow::resizeEvent(QResizeEvent*) {
    if (!initialised_ || !renderer_) return;
    const qreal dpr = devicePixelRatio();
    renderer_->resize(static_cast<uint32_t>(width() * dpr),
                      static_cast<uint32_t>(height() * dpr));
    requestUpdate();
}

bool VulkanWindow::event(QEvent* e) {
    switch (e->type()) {
        case QEvent::UpdateRequest:
            render();
            return true;
        case QEvent::PlatformSurface:
            // Qt is about to destroy the native window, taking the VkSurfaceKHR
            // with it. Everything built on that surface must go first.
            if (static_cast<QPlatformSurfaceEvent*>(e)->surfaceEventType() ==
                QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
                releaseResources();
            }
            break;
        default:
            break;
    }
    return QWindow::event(e);
}

void VulkanWindow::mousePressEvent(QMouseEvent* e) {
    // Grabbing the camera cancels any in-flight view glide -- the user is now
    // driving, so the animation should not fight the drag.
    cameraAnimating_ = false;
    lastPos_ = e->position();
    if (e->button() == Qt::LeftButton) dragging_ = true;
    if (e->button() == Qt::RightButton || e->button() == Qt::MiddleButton) {
        panning_ = true;
    }
}

void VulkanWindow::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) dragging_ = false;
    if (e->button() == Qt::RightButton || e->button() == Qt::MiddleButton) {
        panning_ = false;
    }
}

void VulkanWindow::mouseMoveEvent(QMouseEvent* e) {
    const QPointF delta = e->position() - lastPos_;
    lastPos_ = e->position();

    if (dragging_) {
        camera_.yaw = wrapPi(camera_.yaw + static_cast<float>(delta.x()) * 0.008f);
        // No clamp: the basis is pole-safe, so pitch rotates through and keeps
        // going. Past vertical the view inverts, which is correct.
        camera_.pitch =
            wrapPi(camera_.pitch + static_cast<float>(delta.y()) * 0.008f);
        requestUpdate();
    } else if (panning_) {
        // Pan across the SCREEN plane, using the camera's own right/up. Using
        // world Z as "up" made a vertical drag push the board through its own
        // thickness instead of moving it up the screen.
        const Basis b = cameraBasis(camera_);
        const float scale = camera_.distance * 0.0015f;
        const glm::vec3 move = -b.right * static_cast<float>(delta.x()) * scale +
                               b.up * static_cast<float>(delta.y()) * scale;
        camera_.targetX += move.x;
        camera_.targetY += move.y;
        camera_.targetZ += move.z;
        requestUpdate();
    }
}

void VulkanWindow::wheelEvent(QWheelEvent* e) {
    const float steps = static_cast<float>(e->angleDelta().y()) / 120.0f;

    if (e->modifiers() & Qt::ControlModifier) {
        // ~3 wheel clicks per stage: two to lift the ring, one of dwell.
        setExplodeProgress(explodeProgress_ + steps * 0.34f);
        return;
    }

    // Accumulate into a TARGET and glide there (stepZoomAnimation) instead of
    // stepping the camera per click. Rapid wheel clicks compound into one smooth
    // dolly -- the same exponential-approach feel as the exploding view.
    if (!zoomAnimating_) {
        zoomTarget_ = camera_.distance;
        zoomAnimating_ = true;
        zoomClock_.restart();
    }
    zoomTarget_ *= std::pow(0.88f, steps);
    zoomTarget_ = std::clamp(zoomTarget_, 0.5f, 5000.0f);
    // If a view preset is mid-glide it also eases distance; keep both targets
    // agreed so the two animations pull the same way instead of fighting.
    if (cameraAnimating_) viewTarget_.distance = zoomTarget_;
    requestUpdate();
}

bool VulkanWindow::stepZoomAnimation() {
    if (!zoomAnimating_) return false;

    const double dt =
        std::min(static_cast<double>(zoomClock_.restart()) / 1000.0, 0.1);

    // Approach in LOG space: zoom is multiplicative, so a linear approach would
    // rush the far end and crawl the near end of a long glide. A log approach
    // moves at a constant perceptual rate at any distance. Same time constant
    // as the peel, so the two gestures feel related.
    const float ratio = zoomTarget_ / camera_.distance;
    if (std::abs(ratio - 1.0f) < 1e-3f) {
        camera_.distance = zoomTarget_;
        zoomAnimating_ = false;
    } else {
        constexpr double kTimeConstant = 0.07;
        const float k = 1.0f - static_cast<float>(std::exp(-dt / kTimeConstant));
        camera_.distance *= std::pow(ratio, k);
    }
    return zoomAnimating_;
}

void VulkanWindow::setExplodeProgress(float progress, bool snap) {
    explodeTarget_ = std::max(0.0f, progress);

    // Only clamp to maxRank once the renderer exists. maxRank is 0 until a board
    // is uploaded, so clamping unconditionally would silently zero any value set
    // before first expose -- which is exactly when MainWindow configures things.
    // initialise() re-invokes this, and the clamp lands then.
    if (renderer_) {
        explodeTarget_ = std::min(explodeTarget_, renderer_->maxRank());
    }

    if (snap) {
        explodeProgress_ = explodeTarget_;
        explodeAnimating_ = false;
    } else if (!explodeAnimating_) {
        explodeAnimating_ = true;
        explodeClock_.restart();
    }

    pushExplode();
    requestUpdate();
}

void VulkanWindow::pushExplode() {
    if (renderer_) {
        renderer_->setExplode(explodeStepMm(),
                              easedExplodeProgress(explodeProgress_));
    }
    emit explodeChanged(explodeProgress_,
                        renderer_ ? renderer_->maxRank() : 0.0f);
}

bool VulkanWindow::stepExplodeAnimation() {
    if (!explodeAnimating_) return false;

    // Clamp dt so a stall (a breakpoint, a swapchain rebuild) does not teleport
    // the stack -- the point of this is that it never jumps.
    const double dt =
        std::min(static_cast<double>(explodeClock_.restart()) / 1000.0, 0.1);

    const float remaining = explodeTarget_ - explodeProgress_;
    if (std::abs(remaining) < 1e-3f) {
        explodeProgress_ = explodeTarget_;
        explodeAnimating_ = false;
    } else {
        // Exponential approach: framerate-independent, and it eases out on its
        // own. ~0.2s to close most of the gap. A linear ramp would arrive with a
        // hard stop.
        constexpr double kTimeConstant = 0.07;
        const float k =
            1.0f - static_cast<float>(std::exp(-dt / kTimeConstant));
        explodeProgress_ += remaining * k;
    }
    pushExplode();
    return explodeAnimating_;
}

bool VulkanWindow::stepCameraAnimation() {
    if (!cameraAnimating_) return false;

    const double dt =
        std::min(static_cast<double>(cameraClock_.restart()) / 1000.0, 0.1);
    // Exponential approach, framerate-independent and eases out on its own.
    // Slightly slower than the peel so a view swing reads as deliberate.
    constexpr double kTimeConstant = 0.10;
    const float k = 1.0f - static_cast<float>(std::exp(-dt / kTimeConstant));

    const auto ease = [&](float& cur, float tgt) { cur += (tgt - cur) * k; };
    ease(camera_.yaw, viewTarget_.yaw);
    ease(camera_.pitch, viewTarget_.pitch);
    ease(camera_.distance, viewTarget_.distance);
    ease(camera_.targetX, viewTarget_.targetX);
    ease(camera_.targetY, viewTarget_.targetY);
    ease(camera_.targetZ, viewTarget_.targetZ);

    // Settled? Distances/positions are in mm, angles in radians, so weight the
    // positional terms down before summing to one comparable residual.
    const float residual =
        std::abs(camera_.yaw - viewTarget_.yaw) +
        std::abs(camera_.pitch - viewTarget_.pitch) +
        0.01f * (std::abs(camera_.distance - viewTarget_.distance) +
                 std::abs(camera_.targetX - viewTarget_.targetX) +
                 std::abs(camera_.targetY - viewTarget_.targetY) +
                 std::abs(camera_.targetZ - viewTarget_.targetZ));
    if (residual < 1e-3f) {
        camera_ = viewTarget_;
        cameraAnimating_ = false;
    }
    return cameraAnimating_;
}

float VulkanWindow::explodeStepMm() const {
    // "A few mm off the stack" reads right on a 50mm board but would be
    // invisible on a 300mm backplane, so scale gently with board size and hold a
    // sane floor.
    if (!mesh_) return 4.0f;
    const float span = static_cast<float>(
        std::max(mesh_->bounds.max[0] - mesh_->bounds.min[0],
                 mesh_->bounds.max[1] - mesh_->bounds.min[1]));
    return std::clamp(span * 0.08f, 3.0f, 14.0f);
}

void VulkanWindow::keyPressEvent(QKeyEvent* e) {
    // Unmodified keys are view controls. Anything with a modifier is a menu
    // shortcut and belongs to the main window -- note Ctrl+O must NOT toggle
    // orthographic.
    if (e->modifiers() == Qt::NoModifier) {
        switch (e->key()) {
            case Qt::Key_T: setViewTop(); return;
            case Qt::Key_B: setViewBottom(); return;
            case Qt::Key_I: setViewIso(); return;
            case Qt::Key_F: frameBoard(); return;
            case Qt::Key_O:
                camera_.orthographic = !camera_.orthographic;
                requestUpdate();
                return;
            default:
                break;
        }
    }
    emit unhandledKey(e->key(), e->modifiers());
    QWindow::keyPressEvent(e);
}

}  // namespace pcbview::app
