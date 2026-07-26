#include "app/vulkan_window.h"

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QTimer>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QSettings>

#include "app/settings.h"
#include <QWheelEvent>

// GLM_FORCE_DEPTH_ZERO_TO_ONE (Vulkan clip space, not OpenGL's) is a COMPILE
// DEFINITION in CMakeLists, not a #define here. It configures glm's projection
// matrices, so it must be set before glm is included ANYWHERE in the
// translation unit -- and vulkan_window.h includes glm itself, ahead of this
// file's body. A local #define silently lost that race and glm::ortho started
// emitting OpenGL's [-1,1] depth, half of which Vulkan clips: the board was
// sliced along a plane in orthographic view. Never move it back into a .cpp.
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "text/stroke_text.h"

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

    // Roll spins right/up about the view axis after the pole-safe basis is
    // built, so it composes with the yaw/pitch turntable instead of breaking
    // its no-degenerate-poles guarantee.
    if (c.roll != 0.0f) {
        const float cr = std::cos(c.roll), sr = std::sin(c.roll);
        const glm::vec3 r = b.right * cr + b.up * sr;
        b.up = b.up * cr - b.right * sr;
        b.right = r;
    }
    return b;
}

// Rodrigues rotation of v about the unit axis k by angle a.
glm::vec3 rotateAbout(const glm::vec3& v, const glm::vec3& k, float a) {
    const float c = std::cos(a), s = std::sin(a);
    return v * c + glm::cross(k, v) * s + k * glm::dot(k, v) * (1.0f - c);
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

    // Headless hook for the board's own pose, in radians about board +Z. The
    // point of it is to prove the board turns INDEPENDENTLY of the camera:
    // with this set, the view direction is untouched but the board is not.
    if (qEnvironmentVariableIsSet("PCBVIEW_BOARD_YAW")) {
        rotateBoard(glm::vec3(0.0f, 0.0f, 1.0f),
                    qEnvironmentVariable("PCBVIEW_BOARD_YAW").toFloat());
    }

    // ~120 Hz. Polling is cheap (SDL keeps the state; this just reads it) and
    // it has to run even when nothing is being drawn, because on-demand
    // rendering means an idle app produces no frames to piggyback on.
    if (gamepad_.available()) {
        padTimer_ = new QTimer(this);
        padTimer_->setInterval(8);
        connect(padTimer_, &QTimer::timeout, this, &VulkanWindow::stepGamepad);
        padTimer_->start();
        padClock_.start();
    }
}

void VulkanWindow::stepVr() {
    if (!vr_ || !renderer_ || !mesh_) return;

    static const bool allowPt = qEnvironmentVariableIsSet("PCBVIEW_VR_PT");

    // Where the board sits in the room. Refreshed each frame so loading a
    // different board re-places it rather than leaving it the old size.
    const auto& b = mesh_->bounds;
    const float centre[3] = {
        static_cast<float>((b.min[0] + b.max[0]) * 0.5),
        static_cast<float>((b.min[1] + b.max[1]) * 0.5),
        static_cast<float>((b.min[2] + b.max[2]) * 0.5)};
    const float span = static_cast<float>(
        std::max(b.max[0] - b.min[0], b.max[1] - b.min[1]));
    vr_->setBoardPlacement(centre, span);

    std::vector<xr::VrSession::Eye> eyes;
    const bool render = vr_->beginFrame(&eyes);
    if (!vr_->active()) {  // the runtime ended the session
        if (vrTimer_) vrTimer_->stop();
        vr_.reset();
        // Hand the window back. Leaving offscreenOnly_ set would stop the
        // desktop presenting entirely -- a dead window after taking the
        // headset off -- and leaving the mode forced would strand the user in
        // a renderer they never picked.
        renderer_->setOffscreenOnly(false);
        // Back to the user's own settings rather than a mode snapshot:
        // ptEnabled_/rtEnabled_ are the source of truth everywhere else, and a
        // value captured at session start would be the pre-restore one anyway.
        renderer_->setRenderMode(ptEnabled_ && ptAvailable()
                                     ? vk::RenderMode::PathTraced
                                     : vk::RenderMode::Raster);
        renderer_->setRayTracing(rtEnabled_ && rtAvailable());
        renderer_->setUncappedPresent(false);
        requestUpdate();
        return;
    }
    // PCBVIEW_VR_MONO=1 renders eye 0 ONCE and sends those identical pixels to
    // BOTH eyes. It halves what is left: if the board still reads left/right
    // exchanged while both eyes are seeing the SAME image, then nothing is
    // swapped BETWEEN the eyes and the board itself is turned the wrong way
    // round; if it reads correctly, the fault really is per-eye.
    //
    // This replaces PCBVIEW_VR_ONE_EYE, which could never have worked: endFrame
    // always submits a two-view projection layer, but OpenXR requires every
    // swapchain a layer references to have had an image acquired AND released
    // that frame. Skipping an eye left its chain untouched, so xrEndFrame
    // rejected the whole layer and the headset went black -- which is exactly
    // what happened. Rendering once and blitting twice keeps both chains fed.
    static const bool mono = [] {
        const char* v = std::getenv("PCBVIEW_VR_MONO");
        return v && v[0] && v[0] != '0';
    }();

    // Who owns the renderer this frame.
    //
    // `render` is false whenever the runtime is not asking for frames -- most
    // often because the headset has been taken off and its proximity sensor
    // dropped the session out of FOCUSED. The window was gated on a session
    // merely EXISTING, so in that state nothing drew at all: stepVr rendered
    // nothing and render() had already returned early. The window froze.
    //
    // So ownership follows whether VR is actually rendering, and handing back
    // is a real handover -- the offscreen flag has to be cleared or the window
    // still cannot present, and the render mode has to go back to the user's
    // own settings rather than the raster VR forces.
    vrRendering_ = render;
    if (!render) {
        if (vrOwnsRenderer_) {
            vrOwnsRenderer_ = false;
            renderer_->setOffscreenOnly(false);
            // Back to the interactive sample ramp for a mouse camera.
            renderer_->setPathTraceBatch(0);
            renderer_->setRenderMode(ptEnabled_ && ptAvailable()
                                         ? vk::RenderMode::PathTraced
                                         : vk::RenderMode::Raster);
            renderer_->setRayTracing(rtEnabled_ && rtAvailable());
            requestUpdate();
        }
    } else {
        // Taking over: put the board in front of wherever the viewer is NOW.
        //
        // The anchor is otherwise captured once, at the instant the session
        // opens -- which is while you are still at the keyboard looking at the
        // monitor. Turn to face the room afterwards and the board is off to one
        // side, seen edge-on. Re-anchoring on every handover means putting the
        // headset on always presents the board in front of you.
        if (!vrOwnsRenderer_) vr_->reanchor();
        vrOwnsRenderer_ = true;

        // Every frame stands alone: trace a whole budget of samples now, show
        // it, start over.
        //
        // Accumulating across frames cannot work against a tracked head. It
        // never holds still enough for the camera to count as unchanged, so the
        // accumulator restarts anyway and every frame ends up showing a single
        // sample -- which is what all the per-eye buffering and tolerance
        // widening was chasing, and why none of it worked. Tracing the whole
        // budget in one frame sidesteps the question entirely.
        //
        // PCBVIEW_VR_SPP sets the budget.
        static const int spp = [] {
            bool ok = false;
            const int n = qgetenv("PCBVIEW_VR_SPP").toInt(&ok);
            return (ok && n >= 1 && n <= 256) ? n : 16;
        }();
        renderer_->setPathTraceBatch(allowPt ? spp : 0);
        // Held every frame: initialisation applies the saved ptEnabled_ after
        // the session is created and would otherwise overwrite this, and the
        // render-mode menu can change it mid-session. setRenderMode returns
        // immediately when the mode already matches, so this is free.
        if (!allowPt) {
            renderer_->setRenderMode(vk::RenderMode::Raster);
            renderer_->setRayTracing(rtAvailable());
        }
    }

    // Render at a fraction of the headset's rate and let the COMPOSITOR fill
    // the gaps. On a skipped frame nothing is drawn and no swapchain image is
    // touched; endFrame resubmits the previous images still labelled with the
    // poses they were drawn at, and the runtime reprojects them to wherever the
    // head is at scanout. That is the safe form of reusing a frame -- simply
    // showing an old one again pins the world to your head for a beat, which
    // reads as the scene sliding and gets sickening quickly.
    //
    // Defaults to 1 -- every frame rendered, nothing reprojected.
    //
    // Skipping frames looked like free smoothness and is not. The layer we
    // submit carries no depth buffer, so the runtime can only warp it RIGIDLY,
    // and a rigid warp cannot reproduce parallax: things at different depths
    // slide against each other as the head moves. The error grows the closer
    // the subject is, and a board at 0.6 m is very close, so it reads as parts
    // of the image shifting about.
    //
    // Worth revisiting via XR_KHR_composition_layer_depth, which hands the
    // runtime a depth buffer and lets it warp per pixel. Until then, render
    // every frame. PCBVIEW_VR_RATE_DIV still forces a divisor for testing.
    static const int rateDiv = [] {
        bool ok = false;
        const int n = qgetenv("PCBVIEW_VR_RATE_DIV").toInt(&ok);
        return (ok && n >= 1 && n <= 4) ? n : 1;
    }();
    const bool drawThisFrame = render && (++vrFrameCount_ % rateDiv) == 0;

    if (drawThisFrame) {
        for (size_t i = 0; i < eyes.size(); ++i) {
            const xr::VrSession::Eye& e = eyes[mono ? 0 : i];
            // The ordinary render path, at this eye's size. setCaptureExtent
            // already decouples the scene target from the window -- it was
            // built for 4K video capture and does exactly what is needed here.
            // No aspect override: the runtime's viewProj already carries this
            // eye's asymmetric frustum, aspect included. That override exists
            // for the desktop projection path, which is bypassed here.
            // Supersample, then let the eye blit resolve it down.
            //
            // The swapchain is already about the panel's native resolution, so
            // there is no headroom left for edges: geometry against the sky
            // aliased visibly. captureExtent overrides renderScale outright, so
            // asking for a larger scene here is the whole change --
            // blitSceneToImage maps the full source onto the full destination
            // with a LINEAR filter, which is the downsample.
            static const float ss = [] {
                bool ok = false;
                const float f = qgetenv("PCBVIEW_VR_SS").toFloat(&ok);
                return (ok && f >= 1.0f && f <= 2.0f) ? f : 1.5f;
            }();
            renderer_->setCaptureExtent(
                static_cast<uint32_t>(std::lround(e.width * ss)),
                static_cast<uint32_t>(std::lround(e.height * ss)));
            // The path tracer ignores viewProj entirely -- it traces from a ray
            // basis, and the only place that basis was ever set is the desktop
            // render path. So traced VR was rendering the DESKTOP camera's
            // direction and zoom into both eyes, with no head tracking, while
            // the matrices this loop hands over were used by the raster path
            // alone. Set it per eye, from that eye's own frustum.
            renderer_->setRayCamera(e.eye, e.fwd, e.right, e.up, false);
            // Eye 0 presents, so the desktop window becomes a mirror of the
            // left eye; eye 1 renders offscreen. Presenting both made the
            // window flip between two viewpoints every frame.
            renderer_->setOffscreenOnly(i != 0);
            renderer_->drawFrame(e.viewProj, e.eye);
            if (i < 2) vrSamples_[i] = renderer_->accumulatedSamples();
            vr_->submitEye(static_cast<int>(i), *renderer_);
        }
    }
    // Per-eye convergence, about once a second while path tracing. Whether
    // these numbers CLIMB is the difference between "accumulation is working
    // and the image needs longer" and "something is resetting it every frame",
    // which look identical through the lenses.
    if (allowPt && drawThisFrame && (vrFrameCount_ % 90) == 0) {
        std::printf("vr-pt: samples eye0=%d eye1=%d\n", vrSamples_[0],
                    vrSamples_[1]);
        std::fflush(stdout);
    }

    // Always: xrBeginFrame is owed an xrEndFrame even on a frame we skipped.
    vr_->endFrame();
}

void VulkanWindow::stepGamepad() {
    // The video recorder owns the clock while paused; a stray stick must not
    // be able to shift the camera midway through a render.
    if (animationsPaused_) return;
    // Only drive the view while pcbview is the active application, or a pad
    // being used in another window would quietly steer the board in the
    // background.
    if (QGuiApplication::applicationState() != Qt::ApplicationActive) {
        padSteering_ = false;
        return;
    }

    const input::GamepadState& g = gamepad_.poll();
    if (!g.connected) {
        padSteering_ = false;
        return;
    }

    // Real elapsed time, so the response is identical at any poll rate or
    // frame rate.
    double dt = static_cast<double>(padClock_.restart()) / 1000.0;
    if (dt <= 0.0 || dt > 0.25) dt = 0.016;  // first tick / after a stall
    const float fdt = static_cast<float>(dt);
    bool moved = false;

    // RIGHT stick TURNS THE BOARD, matching the mouse exactly: push right and
    // the face you are looking at goes right. Same negated yaw as the drag
    // path -- see mouseMoveEvent.
    if (g.rightX != 0.0f || g.rightY != 0.0f) {
        constexpr float kTurnRate = 2.2f;  // rad/s at full deflection
        // R3 picks the MECHANISM, not the direction: same stick sense either
        // way, but object mode turns the board under a fixed sun while view
        // mode flies the camera around a fixed board.
        Camera after = camera_;
        after.yaw = wrapPi(after.yaw - g.rightX * kTurnRate * fdt);
        after.pitch = wrapPi(after.pitch - g.rightY * kTurnRate * fdt);
        if (padObjectMode_) adoptCameraDeltaIntoBoard(camera_, after);
        else camera_ = after;
        moved = true;
    }

    // LEFT stick pans across the screen plane, like the middle-drag.
    if (g.leftX != 0.0f || g.leftY != 0.0f) {
        const Basis b = cameraBasis(camera_);
        const float scale = camera_.distance * 1.1f * fdt;
        // Both axes reversed on request: the pad's pan felt backwards against
        // the mouse's middle-drag.
        const glm::vec3 move =
            b.right * g.leftX * scale + b.up * g.leftY * scale;
        if (padObjectMode_) {
            board_.translation += board_.rotation * (-move);  // see the mouse
        } else {
            camera_.targetX += move.x;
            camera_.targetY += move.y;
            camera_.targetZ += move.z;
        }
        moved = true;
    }

    // L1/R1 dolly: R1 pulls in, L1 pushes out. Exponential so the rate is
    // constant in perceived terms rather than crawling when close and
    // rocketing when far. Digital buttons, so this is a fixed rate.
    if (g.heldLeftShoulder != g.heldRightShoulder) {
        const float rate = (g.heldLeftShoulder ? 1.0f : -1.0f) * 1.6f * fdt;
        camera_.distance =
            std::clamp(camera_.distance * std::exp(rate), 0.5f, 5000.0f);
        // A wheel glide in flight would fight this.
        zoomAnimating_ = false;
        moved = true;
    }

    // L2/R2 explode and collapse the stack. These are ANALOG, so unlike the
    // shoulders they give a pressure-proportional peel rate: ease into it for
    // a slow reveal, bury the trigger to throw the stack apart.
    if (g.rightTrigger > 0.0f || g.leftTrigger > 0.0f) {
        // Two knobs, because "eager" has two causes. kRate is the top speed at
        // a full pull; kCurve bends the response so LIGHT pressure already
        // moves the stack -- an exponent below 1 lifts the bottom of the travel
        // without moving either endpoint, which is where a linear trigger feels
        // dead. Turn kRate down for a slower sweep, kCurve up (toward 1.0) for
        // a more linear, less twitchy pull.
        constexpr float kRate = 4.0f;   // full pull: end to end in ~0.25s
        constexpr float kCurve = 0.55f;
        const float pull = std::pow(g.rightTrigger, kCurve) -
                           std::pow(g.leftTrigger, kCurve);
        setExplodeProgress(explodeProgress_ + pull * kRate * fdt);
        moved = true;
    }

    // GYRO HOLD-AND-TURN: hold the touchpad and physically twist the pad, and
    // the board turns with it -- the flat-screen rehearsal of grabbing the
    // board with a Sense controller in VR. Gain is 1:1 on purpose: twist 30
    // degrees, the board turns 30 degrees, which is what makes it feel like an
    // object rather than a rate control.
    if (g.hasGyro) {
        if (!g.heldTouchpad) {
            // Not grabbing: learn the zero-rate bias. Slow, so a deliberate
            // motion while not grabbing cannot poison it much, and it keeps
            // tracking the thermal drift these MEMS parts have.
            const float a = gyroBiasReady_ ? 0.02f : 1.0f;
            gyroBias_[0] += (g.gyroX - gyroBias_[0]) * a;
            gyroBias_[1] += (g.gyroY - gyroBias_[1]) * a;
            gyroBias_[2] += (g.gyroZ - gyroBias_[2]) * a;
            gyroBiasReady_ = true;
        } else {
            const float gx = g.gyroX - gyroBias_[0];
            const float gy = g.gyroY - gyroBias_[1];
            const float gz = g.gyroZ - gyroBias_[2];
            // Anything below the noise floor is bias we failed to model, not
            // intent; without this the board creeps while you hold still.
            constexpr float kGyroNoise = 0.02f;  // rad/s
            const float ax = std::abs(gx) > kGyroNoise ? gx : 0.0f;
            const float ay = std::abs(gy) > kGyroNoise ? gy : 0.0f;
            const float az = std::abs(gz) > kGyroNoise ? gz : 0.0f;
            if (ax != 0.0f || ay != 0.0f || az != 0.0f) {
                // Hold-and-turn always moves the BOARD, whatever R3 says --
                // that is what "hold and turn" means, and it is the rehearsal
                // for grabbing the board with a Sense controller.
                const Camera before = camera_;
                // SDL reports rad/s about the pad's OWN axes, right-handed:
                // +x is nose up/down, +y is turning it flat, +z is twisting it
                // like a steering wheel.
                //
                // Roll is deliberately NOT driven here. Rolling the view just
                // tips the horizon and is disorienting to steer by hand; the
                // useful third motion is the left-right FLIP, which is what
                // actually gets you to the other side of the board. So the
                // twist axis turns the board and the flat-turn axis flips it.
                camera_.pitch = wrapPi(camera_.pitch + ax * fdt);  // nose
                // Twist is NEGATED: user-confirmed on hardware that the
                // steering-wheel motion turned the board the wrong way. Do not
                // "correct" this back from SDL's right-hand rule.
                camera_.yaw = wrapPi(camera_.yaw - az * fdt);      // twist
                if (ay != 0.0f) applyGlobeTumble(ay * fdt);        // flip
                const Camera after = camera_;
                camera_ = before;
                adoptCameraDeltaIntoBoard(before, after);
                moved = true;
            }
            padSteering_ = true;
        }
    }

    // Face buttons pick views, by position so one mapping fits both pads:
    // south/east/west/north = cross/circle/square/triangle = A/B/X/Y.
    if (g.pressedSouth) frameBoard();
    if (g.pressedEast) setViewIso();
    if (g.pressedWest) setViewTop();
    if (g.pressedNorth) setViewBottom();
    // L3 levels the roll; R3 toggles object mode.
    if (g.pressedLeftStick) {
        camera_.roll = 0.0f;
        moved = true;
    }
    if (g.pressedRightStick) {
        padObjectMode_ = !padObjectMode_;
        emit objectModeChanged(padObjectMode_);
    }
    // Start recentres -- the way out of having slid or spun the board somewhere
    // unhelpful without reaching for the keyboard.
    if (g.pressedStart) recenterAll();
    const bool acted = g.pressedSouth || g.pressedEast || g.pressedWest ||
                       g.pressedNorth || g.pressedLeftStick ||
                       g.pressedRightStick || g.pressedStart;

    // Assigned, not OR'd into the gyro block's earlier write, because that
    // write would otherwise be clobbered here.
    padSteering_ = g.steering || (g.hasGyro && g.heldTouchpad);

    // PCBVIEW_PAD_DEBUG=1: throttled dump of what the pad is actually
    // reporting. Sticks that read stuck at an extreme, a trigger that rests
    // off zero, or a gyro frozen at exactly 0,0,0 (rather than jittering) each
    // point at a different layer, which is otherwise guesswork.
    static const bool padDebug = qEnvironmentVariableIsSet("PCBVIEW_PAD_DEBUG");
    if (padDebug) {
        static QElapsedTimer logClock;
        if (!logClock.isValid() || logClock.elapsed() > 250) {
            logClock.restart();
            std::printf("pad L(%+.2f,%+.2f) R(%+.2f,%+.2f) T(%.2f,%.2f) "
                        "gyro(%+.3f,%+.3f,%+.3f) yaw=%+.3f pitch=%+.3f\n",
                        g.leftX, g.leftY, g.rightX, g.rightY, g.leftTrigger,
                        g.rightTrigger, g.gyroX, g.gyroY, g.gyroZ, camera_.yaw,
                        camera_.pitch);
            std::fflush(stdout);
        }
    }

    if (moved || acted) requestUpdate();
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

    // The instance is created ONCE and kept for the window's lifetime; a device
    // switch may recreate the platform surface (see setPreferredGpu) and re-enter
    // initialise(), but must not build a second instance.
    // VR has to be brought up BEFORE the Vulkan instance exists: the runtime
    // wraps instance and device creation to inject its own extensions, and it
    // names the physical device. See xr_system.h -- the ordering is forced by
    // OpenXR, not chosen. Failing to find a headset is an ordinary outcome, not
    // an error: the app simply carries on as a desktop session.
    if (qEnvironmentVariableIsSet("PCBVIEW_VR") && !xrSystem_ &&
        instance_ == VK_NULL_HANDLE) {
        xrSystem_ = std::make_unique<xr::System>();
        if (xrSystem_->start()) {
            xrSystem_->installHooks();
            std::printf("vr: %s -- Vulkan will be created through the runtime\n",
                        xrSystem_->headsetName());
            std::fflush(stdout);
        } else {
            xrSystem_.reset();
        }
    }

    if (instance_ == VK_NULL_HANDLE) {
        // Surface extensions for this platform. Qt will create the surface, but
        // the instance is ours, so we must enable them ourselves.
        std::vector<const char*> extensions{VK_KHR_SURFACE_EXTENSION_NAME};
#ifdef Q_OS_WIN
        extensions.push_back("VK_KHR_win32_surface");
#elif defined(Q_OS_LINUX)
        extensions.push_back("VK_KHR_xcb_surface");
#endif

        instance_ = createInstance(/*enableValidation=*/true, extensions);
        messenger_ = createDebugMessenger(instance_);

        // Hand Qt OUR instance rather than letting it make one. This is what
        // keeps the RT extension setup ours.
        qtInstance_.setVkInstance(instance_);
        if (!qtInstance_.create()) {
            throw std::runtime_error(
                "QVulkanInstance::create() failed: " +
                std::to_string(qtInstance_.errorCode()));
        }
        setVulkanInstance(&qtInstance_);
    }

    surface_ = QVulkanInstance::surfaceForWindow(this);
    if (surface_ == VK_NULL_HANDLE) {
        throw std::runtime_error("Qt produced no VkSurfaceKHR for this window");
    }

    // Device preference: an explicit env override wins, else the persisted pick.
    const QByteArray envGpu = qgetenv("PCBVIEW_GPU");
    preferredGpu_ = !envGpu.isEmpty() ? QString::fromLocal8Bit(envGpu)
                                      : appSettings().value("gpuName").toString();
    // Ray-traced shadows + AO are ALWAYS ON -- no longer a user toggle. The
    // CPU device renders everything through Embree, where RT-on measured both
    // faster-converging and better-looking than the flat preview; on a GPU the
    // ray-query cost applies only at rest. Any previously persisted
    // "rayTracing" setting is deliberately ignored (users who turned it off
    // could otherwise never get it back once the menu item was removed).
    // PCBVIEW_RT=0 stays as a headless hook: it exercises the flat preview.
    rtEnabled_ = qEnvironmentVariableIsSet("PCBVIEW_RT")
                     ? qgetenv("PCBVIEW_RT").toInt() != 0
                     : true;
    ptEnabled_ = qEnvironmentVariableIsSet("PCBVIEW_PT")
                     ? qgetenv("PCBVIEW_PT").toInt() != 0
                     : appSettings().value("pathTracing", false).toBool();
    oidnEnabled_ = qEnvironmentVariableIsSet("PCBVIEW_OIDN")
                       ? qgetenv("PCBVIEW_OIDN").toInt() != 0
                       : appSettings().value("denoising", true).toBool();
    dimsOverlay_ = appSettings().value("dimensionsOverlay", false).toBool();
    // fastMove_ is loaded in createDeviceAndRenderer -- its default depends on
    // the device (on for the CPU renderer, off for a GPU).

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
    // In VR the runtime names the GPU, and a session against any other one
    // cannot be created. Asking BEFORE selectGpu is what lets that outrank the
    // saved preference -- see setRequiredGpu.
    if (xrSystem_) xrSystem_->adoptRuntimeGpu(instance_);

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
    // The hooks have done their job; leaving them installed would silently
    // route a later GPU switch through the runtime too.
    if (xrSystem_) {
        xrSystem_->removeHooks();
        std::printf("vr: vulkan device created through the runtime on %s\n",
                    pick->name.c_str());
        std::fflush(stdout);
    }

    const qreal dpr = devicePixelRatio();
    renderer_ = std::make_unique<vk::Renderer>(
        device_, surface_, static_cast<uint32_t>(width() * dpr),
        static_cast<uint32_t>(height() * dpr));
    renderer_->setRayTracing(rtEnabled_ && rtAvailable());

    if (xrSystem_) {
        vr_ = std::make_unique<xr::VrSession>();
        if (vr_->begin(*xrSystem_, instance_, device_.handle,
                       device_.gpu.handle, device_.gpu.graphicsQueueFamily,
                       device_.graphicsQueue)) {
            // Eye 0 presents to the desktop window as a mirror; uncapping stops
            // vsync throttling the headset down to the monitor's refresh.
            renderer_->setUncappedPresent(true);

            // The render mode is forced per frame in stepVr, NOT here. Setting
            // it at this point does nothing: initialisation goes on to apply
            // the saved ptEnabled_ setting a few dozen lines below, which
            // overwrites whatever is chosen here. That is exactly what happened
            // -- VR announced it was using raster and then path-traced anyway.
            vrTimer_ = new QTimer(this);
            vrTimer_->setInterval(1);  // the runtime does the real pacing
            connect(vrTimer_, &QTimer::timeout, this, &VulkanWindow::stepVr);
            vrTimer_->start();
        } else {
            vr_.reset();
        }
    }

    if (qEnvironmentVariableIsSet("PCBVIEW_PT_SPP"))
        renderer_->setMaxSamples(qgetenv("PCBVIEW_PT_SPP").toInt());
    // Headless hook for the internal-resolution slider -- exists because some
    // artifacts (OIDN tiling seams) only appear above a pixel-count threshold
    // that the default headless window never reaches.
    if (qEnvironmentVariableIsSet("PCBVIEW_RENDER_SCALE"))
        renderer_->setRenderScale(qgetenv("PCBVIEW_RENDER_SCALE").toFloat());

    // Fast movement: ON by default for the CPU device, OFF for a GPU, persisted
    // per device class with an env override. The CPU downgrade no longer means
    // llvmpipe raster (which profiling showed was the SLOW path) -- since the
    // Embree-everything change it drops to the flat preview, one primary ray
    // per pixel, genuinely the cheapest way this device can draw a frame while
    // the camera moves.
    if (qEnvironmentVariableIsSet("PCBVIEW_FAST_MOVE"))
        fastMove_ = qgetenv("PCBVIEW_FAST_MOVE").toInt() != 0;
    else
        fastMove_ = appSettings()
                        .value(cpuRender() ? "fastMovementCpu" : "fastMovementGpu",
                               cpuRender())
                        .toBool();
    // A fresh renderer starts in whatever mode we set below; clear any stale
    // motion-downgrade latch from the previous device so the two agree.
    motionDowngraded_ = false;

    renderer_->setRenderMode(ptEnabled_ && ptAvailable()
                                 ? vk::RenderMode::PathTraced
                                 : vk::RenderMode::Raster);
    renderer_->setDenoising(oidnEnabled_);
    if (mesh_) renderer_->uploadBoard(*mesh_);

    const bool isCpu = device_.gpu.type == VK_PHYSICAL_DEVICE_TYPE_CPU;
    emit statusMessage(
        QString("%1  |  ray tracing: %2  |  %3 triangles")
            .arg(QString::fromStdString(device_.gpu.name))
            .arg(isCpu ? "all rendering via Embree (CPU)"
                 : !device_.rayQueryEnabled ? "unsupported"
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

bool VulkanWindow::cpuRender() const {
    return device_.gpu.type == VK_PHYSICAL_DEVICE_TYPE_CPU;
}

// RT (shadows + AO on the raster look) runs via Vulkan ray queries on a GPU,
// and via the Embree preview integrator on the CPU device.
bool VulkanWindow::rtAvailable() const {
    return device_.rayQueryEnabled || cpuRender();
}

// Path tracing: Vulkan ray query on a GPU, Embree on the CPU device.
bool VulkanWindow::ptAvailable() const {
    return device_.rayQueryEnabled || cpuRender();
}

void VulkanWindow::setPreferredGpu(const QString& nameSubstring) {
    preferredGpu_ = nameSubstring;
    appSettings().setValue("gpuName", nameSubstring);
    if (!initialised_ || !renderer_) return;

    // Whether this swap involves the software CPU device. Switching the
    // PRESENTING driver (a hardware GPU <-> Mesa lavapipe) on the SAME native
    // window leaves the Windows compositor stuck on the old swapchain -- frames
    // render (the FPS counter keeps ticking) but nothing new reaches the screen.
    // The cure is a whole new native window, and since this QWindow lives inside
    // a QWidget::createWindowContainer, only the OWNER can rebuild that pair --
    // recreating the platform window in place detaches the container (tried:
    // blank viewport). GPU<->GPU swaps have no such problem and keep the cheap
    // in-place device swap.
    const bool oldWasCpu = device_.gpu.type == VK_PHYSICAL_DEVICE_TYPE_CPU;
    const bool newIsCpu = nameSubstring.contains("llvmpipe", Qt::CaseInsensitive);
    if (oldWasCpu || newIsCpu) {
        emit viewportRebuildRequired();  // queued; MainWindow replaces us
        return;
    }

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

void VulkanWindow::setPathTracing(bool on) {
    ptEnabled_ = on;
    appSettings().setValue("pathTracing", on);
    if (renderer_) {
        renderer_->setRenderMode(on && ptAvailable()
                                     ? vk::RenderMode::PathTraced
                                     : vk::RenderMode::Raster);
    }
    emit statusMessage(QString("Path tracing %1")
                           .arg(!ptAvailable() ? "unsupported on this device"
                                : on ? (cpuRender() ? "ON — CPU (Embree), accumulating…"
                                                    : "ON — accumulating…")
                                     : "off"));
    requestUpdate();
}

int VulkanWindow::ptSamples() const {
    return renderer_ ? renderer_->accumulatedSamples() : 0;
}
int VulkanWindow::ptMaxSamples() const {
    return renderer_ ? renderer_->maxSamples() : 0;
}

void VulkanWindow::setFastMovement(bool on) {
    fastMove_ = on;
    // Persist per device class so the CPU and GPU keep independent defaults.
    appSettings().setValue(cpuRender() ? "fastMovementCpu" : "fastMovementGpu", on);
    // If motion is not active, nothing to change now; if it turned off mid-drag,
    // the next frame's applyMotionQuality restores full quality.
    if (!on && motionDowngraded_) requestUpdate();
    emit statusMessage(QString("Fast movement (raster while moving) %1")
                           .arg(on ? "ON" : "off"));
}

void VulkanWindow::applyMotionQuality(bool moving) {
    if (!renderer_) return;
    // Path tracing at CPU (Embree) speed is a slideshow while moving, so the
    // downgrade must fire for the CPU device too -- ptAvailable(), not just the
    // Vulkan ray-query flag.
    const bool downgrade = fastMove_ && moving &&
                           ((ptEnabled_ && ptAvailable()) ||
                            (rtEnabled_ && rtAvailable()));
    if (downgrade == motionDowngraded_) return;  // no transition -> no thrash
    motionDowngraded_ = downgrade;
    if (downgrade) {
        // Force plain raster for the duration of the motion. The user's ptEnabled_
        // / rtEnabled_ intents are untouched, so restore is exact.
        renderer_->setRenderMode(vk::RenderMode::Raster);
        renderer_->setRayTracing(false);
    } else {
        renderer_->setRenderMode(ptEnabled_ && ptAvailable()
                                     ? vk::RenderMode::PathTraced
                                     : vk::RenderMode::Raster);
        renderer_->setRayTracing(rtEnabled_ && rtAvailable());
    }
}

void VulkanWindow::setDenoising(bool on) {
    oidnEnabled_ = on;
    appSettings().setValue("denoising", on);
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
    while (viewTarget_.roll - camera_.roll > kPi) viewTarget_.roll -= kTwoPi;
    while (viewTarget_.roll - camera_.roll < -kPi) viewTarget_.roll += kTwoPi;

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

// The distance frameBoard chooses -- the "default loaded size" that zoom
// percentages are measured against.
glm::vec3 VulkanWindow::boardPivot() const {
    if (!mesh_) return glm::vec3(0.0f);
    const auto& b = mesh_->bounds;
    return glm::vec3(static_cast<float>((b.min[0] + b.max[0]) * 0.5),
                     static_cast<float>((b.min[1] + b.max[1]) * 0.5),
                     static_cast<float>((b.min[2] + b.max[2]) * 0.5));
}

glm::mat4 VulkanWindow::boardMatrix() const {
    const glm::vec3 c = boardPivot();
    return glm::translate(glm::mat4(1.0f), board_.translation + c) *
           glm::mat4_cast(board_.rotation) *
           glm::translate(glm::mat4(1.0f), -c);
}

void VulkanWindow::rotateBoard(const glm::vec3& axisBoardSpace, float radians) {
    if (radians == 0.0f) return;
    const float len = glm::length(axisBoardSpace);
    if (len < 1e-6f) return;
    // Compose on the LEFT of the existing rotation: the axis arrives in board
    // space, which is the frame the caller (mouse, gyro, a Sense grip) was
    // already reasoning in.
    board_.rotation = glm::normalize(
        glm::angleAxis(radians, axisBoardSpace / len) * board_.rotation);
    requestUpdate();
}

void VulkanWindow::setBoardRotation(const glm::quat& q) {
    board_.rotation = glm::normalize(q);
    requestUpdate();
}

namespace {
// The camera's orientation as a rotation. Built from cross(right, up) rather
// than the stored forward so the basis is guaranteed right-handed and
// orthonormal -- quat_cast on a matrix with a determinant of -1 returns
// nonsense. The exact convention does not matter here: only RELATIVE
// orientations are ever taken, and any consistent convention cancels.
glm::quat cameraOrientation(const Basis& b) {
    const glm::vec3 r = glm::normalize(b.right);
    const glm::vec3 u = glm::normalize(b.up);
    return glm::quat_cast(glm::mat3(r, u, glm::normalize(glm::cross(r, u))));
}
}  // namespace

void VulkanWindow::setRotationRoutedToBoard(bool on) {
    if (on == routeToBoard_) return;
    if (on) {
        routeAnchor_ = camera_;
        routePrev_ = camera_;
    } else {
        // Hand the camera back the orientation it was actually being rendered
        // from. Without this the picture would jump the instant routing ends,
        // and a later view-mode drag would start from a pose nobody had seen.
        camera_.yaw = routeAnchor_.yaw;
        camera_.pitch = routeAnchor_.pitch;
        camera_.roll = routeAnchor_.roll;
    }
    routeToBoard_ = on;
    requestUpdate();
}

void VulkanWindow::recenterAll() {
    // Recentring means the same thing in the headset: bring the board back in
    // front of the viewer. Reachable from the menu and Home, so the board can
    // be retrieved after turning without taking the headset off.
    if (vr_) vr_->reanchor();
    board_ = BoardPose{};
    Camera dest = camera_;
    const Camera fresh;  // the opening orientation
    dest.yaw = fresh.yaw;
    dest.pitch = fresh.pitch;
    dest.roll = fresh.roll;
    if (mesh_) {
        const auto& b = mesh_->bounds;
        dest.targetX = static_cast<float>((b.min[0] + b.max[0]) * 0.5);
        dest.targetY = static_cast<float>((b.min[1] + b.max[1]) * 0.5);
        dest.targetZ = static_cast<float>((b.min[2] + b.max[2]) * 0.5);
        dest.distance = framedDistance();
    }
    setViewTarget(dest, /*snap=*/true);
    // Re-anchor, or a showcase mid-flight would immediately hand the board the
    // difference between the old anchor and the recentred camera.
    routeAnchor_ = camera_;
    routePrev_ = camera_;
    requestUpdate();
}

void VulkanWindow::adoptCameraDeltaIntoBoard(const Camera& before,
                                             const Camera& after) {
    const glm::quat qb = cameraOrientation(cameraBasis(before));
    const glm::quat qa = cameraOrientation(cameraBasis(after));
    // Composed on the RIGHT, and that is not a style choice. The pose has to
    // satisfy B = anchor * inverse(camera) at every step. Right-composition
    // telescopes to exactly that:
    //     B2 = B1*(q1*q2') = (q0*q1')*(q1*q2') = q0*q2'
    // whereas left-composition yields q1*q2'*q0*q1', which is only the same
    // thing when the rotations commute -- i.e. when they share an axis. That
    // is why a pure-yaw drag looked perfect while jumping to the Top preset,
    // which mixes pitch into an existing yaw, came out oblique.
    board_.rotation =
        glm::normalize(board_.rotation * (qb * glm::inverse(qa)));
    // No requestUpdate(): this runs from inside render(), where asking for
    // another frame would just burn one doing nothing. Gesture handlers
    // request their own.
}

float VulkanWindow::framedDistance() const {
    if (!mesh_) return camera_.distance;
    const auto& b = mesh_->bounds;
    const float spanX = static_cast<float>(b.max[0] - b.min[0]);
    const float spanY = static_cast<float>(b.max[1] - b.min[1]);
    const float span = std::max(spanX, spanY);
    // Back off far enough that the larger dimension fits the vertical FOV
    // with a little margin.
    const float halfFov = glm::radians(camera_.fovDegrees) * 0.5f;
    return (span * 0.62f) / std::tan(halfFov);
}

void VulkanWindow::frameBoard(bool snap) {
    if (!mesh_) return;
    // Fit is the way back. Object mode can slide the board clean out of the
    // frame, and without this there would be no recovery short of dragging it
    // home by hand. Orientation is deliberately kept -- you asked to frame it,
    // not to un-turn it.
    board_.translation = glm::vec3(0.0f);
    const auto& b = mesh_->bounds;
    Camera dest = camera_;
    dest.targetX = static_cast<float>((b.min[0] + b.max[0]) * 0.5);
    dest.targetY = static_cast<float>((b.min[1] + b.max[1]) * 0.5);
    dest.targetZ = static_cast<float>((b.min[2] + b.max[2]) * 0.5);
    dest.distance = framedDistance();
    setViewTarget(dest, snap);
}

void VulkanWindow::startTimedZoom(float percent, float seconds) {
    if (seconds <= 0.05f || percent <= 0.0f) return;
    tzStart_ = camera_.distance;
    // 200% shows the board twice its framed size = half the distance.
    tzTarget_ = std::max(1e-3f, framedDistance() * (100.0f / percent));
    // Pin the approach to the BOARD: wheel zoom-to-cursor and panning move
    // the orbit target around, and flying toward a stray target sends the
    // board sliding out of frame. The sweep glides the target back to the
    // bounds centre, so the zoom lands on the true framed view.
    tzFromTarget_[0] = camera_.targetX;
    tzFromTarget_[1] = camera_.targetY;
    tzFromTarget_[2] = camera_.targetZ;
    if (mesh_) {
        const auto& b = mesh_->bounds;
        tzToTarget_[0] = static_cast<float>((b.min[0] + b.max[0]) * 0.5);
        tzToTarget_[1] = static_cast<float>((b.min[1] + b.max[1]) * 0.5);
        tzToTarget_[2] = static_cast<float>((b.min[2] + b.max[2]) * 0.5);
    } else {
        tzToTarget_[0] = tzFromTarget_[0];
        tzToTarget_[1] = tzFromTarget_[1];
        tzToTarget_[2] = tzFromTarget_[2];
    }
    tzT_ = 0.0;
    tzDur_ = seconds;
    timedZoomActive_ = true;
    // The zoom owns the distance; a leftover glide would fight it.
    zoomAnimating_ = false;
    cameraAnimating_ = false;
    tzClock_.restart();
    requestUpdate();
}

bool VulkanWindow::stepTimedZoomAnimation() {
    if (!timedZoomActive_) return false;
    tzT_ += clockDt(tzClock_);
    double u = tzT_ / tzDur_;
    if (u >= 1.0) {
        u = 1.0;
        timedZoomActive_ = false;
    }
    // Log-space sweep: constant perceptual rate at any distance, and it
    // lands exactly on the target.
    const float fu = static_cast<float>(u);
    camera_.distance = tzStart_ * std::pow(tzTarget_ / tzStart_, fu);
    // Smoothstep the re-centre so it eases rather than jerking sideways.
    const float su = fu * fu * (3.0f - 2.0f * fu);
    camera_.targetX = tzFromTarget_[0] + (tzToTarget_[0] - tzFromTarget_[0]) * su;
    camera_.targetY = tzFromTarget_[1] + (tzToTarget_[1] - tzFromTarget_[1]) * su;
    camera_.targetZ = tzFromTarget_[2] + (tzToTarget_[2] - tzFromTarget_[2]) * su;
    return timedZoomActive_;
}

void VulkanWindow::setViewTop() {
    Camera dest = camera_;
    dest.yaw = 0.0f;
    // Exactly vertical is fine now: the basis comes from yaw, so there is no
    // pole to avoid. This used to need a fudge factor.
    dest.pitch = 1.57079633f;  // +pi/2
    dest.roll = 0.0f;          // presets are canonical: untwist
    setViewTarget(dest, false);
}

void VulkanWindow::setViewBottom() {
    Camera dest = camera_;
    // yaw pi: the industry "flip board" is LEFT-RIGHT (about the vertical
    // axis), not top-over-bottom. Reaching the underside with yaw 0 shows a
    // view rotated 180 degrees from what Altium/KiCad (and a board in your
    // hands) present -- bottom silk reads upside down and gets reported as
    // "mirrored".
    dest.yaw = 3.14159265f;
    dest.pitch = -1.57079633f;  // -pi/2
    dest.roll = 0.0f;
    setViewTarget(dest, false);
}

void VulkanWindow::setViewIso() {
    Camera dest = camera_;
    dest.yaw = 0.6f;
    dest.pitch = 0.62f;
    dest.roll = 0.0f;
    setViewTarget(dest, false);
}

// One increment of the globe tumble: rotate the eye offset about the
// CURRENT camera-up, then decompose the result back into yaw/pitch/roll --
// that parameterisation cannot express the rotation as a single-angle
// change. Shared by right-drag and the "flip" showcase spin.
void VulkanWindow::applyGlobeTumble(float ax) {
    if (ax == 0.0f) return;
    const Basis b = cameraBasis(camera_);
    // Up is the rotation axis, so it is invariant; only the eye offset
    // moves. Same sign convention as yaw (matches left-drag feel at a level
    // view, where up == world Z).
    const glm::vec3 offset = rotateAbout(-b.forward, b.up, ax);
    camera_.pitch = std::asin(glm::clamp(offset.z, -1.0f, 1.0f));
    if (std::abs(offset.x) + std::abs(offset.y) > 1e-6f)
        camera_.yaw = std::atan2(offset.x, -offset.y);
    // Whatever part of the new orientation yaw/pitch can't express lands in
    // roll: compare the carried-over up against the unrolled reference basis
    // at the new yaw/pitch.
    const glm::vec3 right0(std::cos(camera_.yaw), std::sin(camera_.yaw),
                           0.0f);
    const glm::vec3 up0 = glm::cross(right0, -offset);
    camera_.roll = std::atan2(-glm::dot(b.up, right0), glm::dot(b.up, up0));
}

void VulkanWindow::advanceAnimationsBy(double dt) {
    fixedDt_ = dt;
    stepExplodeAnimation();
    stepCameraAnimation();
    stepZoomAnimation();
    stepSpinAnimation();
    stepPathAnimation();
    stepTimedZoomAnimation();
    fixedDt_ = -1.0;
    requestUpdate();
}

void VulkanWindow::startPath(std::vector<PathKey> keys, double durationSec) {
    if (keys.size() < 2 || durationSec <= 0.05) return;
    pathKeys_ = std::move(keys);
    pathDuration_ = durationSec;
    pathT_ = 0.0;
    pathActive_ = true;
    cameraAnimating_ = false;
    zoomAnimating_ = false;
    spinActive_ = false;
    pathClock_.restart();
    requestUpdate();
}

bool VulkanWindow::stepPathAnimation() {
    if (!pathActive_) return false;
    pathT_ += clockDt(pathClock_);
    // Normalised position along the key list; the keys are uniform in time.
    double u = pathT_ / pathDuration_;
    if (u >= 1.0) {
        u = 1.0;
        pathActive_ = false;
    }
    const int n = static_cast<int>(pathKeys_.size());
    const double f = u * (n - 1);
    const int i1 = std::min(static_cast<int>(f), n - 2);
    const float t = static_cast<float>(f - i1);
    const int i0 = std::max(i1 - 1, 0);
    const int i2 = i1 + 1;
    const int i3 = std::min(i1 + 2, n - 1);
    // Catmull-Rom per channel. The capture unwrapped angles, so no wrap
    // handling is needed here.
    const auto cr = [&](auto get) {
        const float p0 = get(pathKeys_[i0]), p1 = get(pathKeys_[i1]);
        const float p2 = get(pathKeys_[i2]), p3 = get(pathKeys_[i3]);
        const float t2 = t * t, t3 = t2 * t;
        return 0.5f * ((2.0f * p1) + (-p0 + p2) * t +
                       (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                       (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
    };
    camera_.yaw = cr([](const PathKey& k) { return k.yaw; });
    camera_.pitch = cr([](const PathKey& k) { return k.pitch; });
    camera_.roll = cr([](const PathKey& k) { return k.roll; });
    camera_.distance =
        std::max(1e-3f, cr([](const PathKey& k) { return k.distance; }));
    camera_.targetX = cr([](const PathKey& k) { return k.tx; });
    camera_.targetY = cr([](const PathKey& k) { return k.ty; });
    camera_.targetZ = cr([](const PathKey& k) { return k.tz; });
    const float ex =
        std::max(0.0f, cr([](const PathKey& k) { return k.explode; }));
    explodeProgress_ = explodeTarget_ = ex;
    pushExplode();
    return pathActive_;
}

void VulkanWindow::startSpin(int axis, float degrees, float seconds) {
    if (seconds <= 0.01f || degrees == 0.0f) return;
    spinAxis_ = axis;
    spinRemaining_ = glm::radians(degrees);
    spinRate_ = spinRemaining_ / seconds;
    spinActive_ = true;
    // The spin owns the camera; a leftover glide would fight it.
    cameraAnimating_ = false;
    zoomAnimating_ = false;
    spinClock_.restart();
    requestUpdate();
}

bool VulkanWindow::stepSpinAnimation() {
    if (!spinActive_) return false;
    const double dt = clockDt(spinClock_);
    float d = static_cast<float>(spinRate_ * dt);
    if (std::abs(d) >= std::abs(spinRemaining_)) {
        d = spinRemaining_;
        spinActive_ = false;
    }
    spinRemaining_ -= d;
    if (spinAxis_ == 3) {
        // Flip: the screen-vertical tumble. The decomposition keeps the
        // angles canonical on its own.
        applyGlobeTumble(d);
        return spinActive_;
    }
    float* angle = spinAxis_ == 1   ? &camera_.pitch
                   : spinAxis_ == 2 ? &camera_.roll
                                    : &camera_.yaw;
    *angle += d;
    if (!spinActive_) {
        // Land on the canonical wrap so the next preset takes the short way.
        constexpr float kPi = 3.14159265f, kTwoPi = 6.28318531f;
        while (*angle > kPi) *angle -= kTwoPi;
        while (*angle < -kPi) *angle += kTwoPi;
    }
    return spinActive_;
}

void VulkanWindow::render() {
    if (!initialised_ || !renderer_) return;
    // While the headset is actually being rendered to, stepVr owns the
    // renderer. Both paths call setRayCamera, and the path tracer keys
    // accumulation off the camera, so the two alternating meant every VR frame
    // reset the accumulator to a desktop viewpoint and back -- the window
    // flickering between two cameras, and the headset stuck at one sample. The
    // window is fed by eye 0's present instead.
    //
    // Gated on RENDERING, not on the session existing: take the headset off and
    // the runtime stops asking for frames, at which point nobody was drawing
    // and the window froze.
    if (vr_ && vr_->active() && vrRendering_) return;

    // The peel eases toward its target here rather than on a QTimer: rendering
    // is on demand, so the animation drives the frames and the frames drive the
    // animation. When it settles, the loop stops and the GPU goes idle again.
    // Paused = the video recorder owns the clock; state only moves through
    // advanceAnimationsBy between frames.
    const bool exploding = !animationsPaused_ && stepExplodeAnimation();
    const bool gliding = !animationsPaused_ && stepCameraAnimation();
    const bool zooming = !animationsPaused_ && stepZoomAnimation();
    const bool spinning = !animationsPaused_ && stepSpinAnimation();
    const bool pathing = !animationsPaused_ && stepPathAnimation();
    const bool timedZooming = !animationsPaused_ && stepTimedZoomAnimation();
    const bool stillAnimating =
        exploding || gliding || zooming || spinning || pathing || timedZooming;

    // Fast-movement: a drag, pan, or any in-flight animation counts as motion.
    // While moving, render plain raster; restore the requested mode when it
    // stops. The animation flags double as the settle timer -- they stay true
    // until the ease reaches its target, and a mouse drag restores on release.
    applyMotionQuality(dragging_ || draggingInv_ || panning_ || padSteering_ ||
                       stillAnimating);

    QElapsedTimer timer;
    timer.start();

    const qreal dpr = devicePixelRatio();
    const float w = static_cast<float>(width() * dpr);
    const float h = static_cast<float>(height() * dpr);
    if (w < 1.0f || h < 1.0f) return;

    // TWO GENUINELY DIFFERENT MECHANISMS, not two directions:
    //   view mode   - the CAMERA turns (camera_), the board holds still. With
    //                 the sun world-fixed, the board's shading is CONSTANT and
    //                 the sky sweeps past: you are flying around a lit object.
    //   object mode - the BOARD turns (board_), the camera holds still. The sun
    //                 stays put, so light sweeps ACROSS the board and the sky
    //                 does not move: you are turning an object under a lamp.
    // An earlier attempt had both modes turning the board and differing only in
    // sign, which is why they felt identical -- the mechanism is the difference,
    // not the direction. Gestures pick their sink; render() just draws.
    // Showcase "move the board": convert whatever the steppers just did to
    // camera_ into board rotation, and render from the frozen anchor.
    Camera renderCam = camera_;
    if (routeToBoard_) {
        if (camera_.yaw != routePrev_.yaw ||
            camera_.pitch != routePrev_.pitch ||
            camera_.roll != routePrev_.roll) {
            adoptCameraDeltaIntoBoard(routePrev_, camera_);
            routePrev_ = camera_;
        }
        renderCam.yaw = routeAnchor_.yaw;
        renderCam.pitch = routeAnchor_.pitch;
        renderCam.roll = routeAnchor_.roll;
    }
    Basis basis = cameraBasis(renderCam);

    // MOVE THE CAMERA INTO THE BOARD'S FRAME, rather than moving the board's
    // geometry into the world. The two are equivalent, and this direction is
    // enormously cheaper and safer: the vertex/index buffers, the BLAS and
    // TLAS, the net-span and net-light tables and every CPU pick test stay
    // exactly as they are, all of them already expressed in board space. Do
    // this ONCE here and everything downstream -- raster, both tracers,
    // picking, the depth sort, the overlay -- is consistently in board space
    // with no further changes.
    // Test BOTH halves of the pose. Guarding on rotation alone silently threw
    // away every translation while the board was still square-on -- the board
    // simply refused to slide until you had also turned it.
    if (board_.rotation != glm::quat(1.0f, 0.0f, 0.0f, 0.0f) ||
        board_.translation != glm::vec3(0.0f)) {
        const glm::mat4 inv = glm::inverse(boardMatrix());
        const glm::mat3 invR(inv);
        basis.eye = glm::vec3(inv * glm::vec4(basis.eye, 1.0f));
        basis.forward = invR * basis.forward;
        basis.right = invR * basis.right;
        basis.up = invR * basis.up;
    }
    const glm::vec3 eye = basis.eye;
    const glm::mat4 view = viewFromBasis(basis);
    // Recording at another aspect: the projection must match the capture
    // extent, not the window.
    const float aspect = aspectOverride_ > 0.0f ? aspectOverride_ : w / h;

    // Near plane scales with the orbit distance rather than sitting at a fixed
    // hair's breadth. Reversed-Z makes precision forgiving, but there is no
    // reason to ask for a 200,000:1 range on a 50mm board.
    const float zNear = std::max(camera_.distance * 0.005f, 0.02f);

    glm::mat4 proj;
    if (camera_.orthographic) {
        const float halfH = orthoHalfHeight();
        const float halfW = halfH * aspect;
        // Depth range brackets the SCENE, not the eye.
        //
        // A parallel projection has no viewpoint: geometry BEHIND the camera
        // plane still projects into the image and must still be drawn. Keeping
        // a positive near plane (the perspective habit) clips whatever crosses
        // the eye plane, which at a grazing angle is the corner nearest the
        // viewer -- the board visibly got sliced along a straight line as you
        // zoomed or tilted. Bracketing the target +-radius removes the clip
        // and TIGHTENS depth precision at the same time (a ~100mm range beats
        // the old 4*distance + 1000).
        const float r = sceneRadius();
        proj = reverseZOrtho(-halfW, halfW, -halfH, halfH,
                             camera_.distance - r, camera_.distance + r);
    } else {
        proj = infiniteReverseZPerspective(glm::radians(camera_.fovDegrees),
                                           aspect, zNear);
    }
    proj[1][1] *= -1.0f;  // Vulkan's Y is flipped relative to GL
    const glm::mat4 viewProj = proj * view;

    // Tracers need the camera as a ray basis (eye + pixel-plane spans), not a
    // matrix. Setting it resets accumulation whenever the view changed. The
    // software device renders EVERY mode through the Embree tracer (its raster
    // mode is the flat preview), so it always needs the basis -- gating it on
    // the RT toggle left the flat view frozen mid-orbit.
    if (renderer_->renderMode() == vk::RenderMode::PathTraced || cpuRender()) {
        const glm::vec3 fwd = glm::normalize(basis.forward);
        glm::vec3 rayEye = eye;
        glm::vec3 right, up;
        if (camera_.orthographic) {
            // Half-extents in mm, from the same definition as the raster
            // ortho projection above.
            const float halfH = orthoHalfHeight();
            const float halfW = halfH * aspect;
            right = basis.right * halfW;
            up = basis.up * halfH;
            // Parallel rays start ON the camera plane, so anything behind it
            // would simply never be hit -- the tracers' version of the near
            // clip that sliced the raster board. Pushing the origin back by
            // the scene radius cannot change WHAT a parallel ray hits, only
            // how much of the scene is in front of it.
            rayEye -= fwd * sceneRadius();
        } else {
            const float tanY = std::tan(glm::radians(camera_.fovDegrees) * 0.5f);
            const float tanX = tanY * aspect;
            right = basis.right * tanX;
            up = basis.up * tanY;
        }
        renderer_->setRayCamera(&rayEye[0], &fwd[0], &right[0], &up[0],
                                camera_.orthographic);
        // The sun and sky live in the WORLD; the tracer works in board space.
        // Hand it the world->board rotation so a turning board sweeps the light
        // across itself instead of carrying the lighting around with it.
        const glm::quat inv = glm::inverse(board_.rotation);
        const float q[4] = {inv.x, inv.y, inv.z, inv.w};
        renderer_->setBoardRotationInverse(q);
    }

    // The raster shaders need the projection kind: a parallel projection has
    // one view direction for every fragment, and the eye-point fallback
    // reverses at/behind the eye plane (black near edge).
    {
        const glm::vec3 fwd = glm::normalize(basis.forward);
        renderer_->setCameraAxis(
            &fwd[0], camera_.orthographic ? camera_.distance : 0.0f);
    }

    lastViewProj_ = viewProj;
    haveViewProj_ = true;
    buildOverlay();

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

    // Keep the loop alive while the peel moves OR a progressive tracer (GPU PT,
    // CPU PT, or the CPU RT preview) is still accumulating toward convergence
    // OR a net chase is running.
    //
    // The chase now runs in path tracing too, because it is applied in the
    // TONEMAP pass rather than during tracing -- repainting re-runs only the
    // display resolve, and accumulation is never reset. That is what lets a
    // converged image animate instead of collapsing back to one noisy sample.
    if (stillAnimating || renderer_->accumulating() || renderer_->netAnimating())
        requestUpdate();
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
    // Shift latches OBJECT MODE for the whole drag: hand on the board rather
    // than camera around it.
    // PCBVIEW_OBJECT_MODE=1 forces it on. Qt builds mouse modifiers from
    // GetKeyState rather than the message's MK_ flags, so NO keyboard modifier
    // can be delivered by a posted-message harness -- this hook is what lets
    // the object-mode behaviour itself be tested, leaving only "did the Shift
    // arrive" to a real keyboard. Also handy for scripted captures.
    static const bool forceObject =
        qEnvironmentVariableIsSet("PCBVIEW_OBJECT_MODE");
    objectDrag_ = forceObject || (e->modifiers() & Qt::ShiftModifier) != 0;
    // Say so. Object mode is otherwise invisible until you have already moved
    // something, and if the modifier is not arriving at all there is nothing to
    // tell you that either.
    if (objectDrag_) emit objectModeChanged(true);
    // PCBVIEW_INPUT_DEBUG=1 reports what Qt actually delivered. Separates "the
    // modifier never arrived" from "the modifier arrived and was ignored",
    // which look identical from the outside.
    static const bool inputDebug =
        qEnvironmentVariableIsSet("PCBVIEW_INPUT_DEBUG");
    if (inputDebug) {
        std::printf("press: button=%d modifiers=0x%08x objectDrag=%d\n",
                    static_cast<int>(e->button()),
                    static_cast<unsigned>(e->modifiers()),
                    static_cast<int>(objectDrag_));
        std::fflush(stdout);
    }
    if (e->button() == Qt::LeftButton) {
        dragging_ = true;
        // A left CLICK (press+release without a drag) is a pick: a
        // measurement point in measure mode, otherwise the net under the
        // cursor. A drag still orbits either way. Track the candidacy here
        // and cancel it once the cursor moves.
        clickCandidate_ = true;
        pressPos_ = e->position();
    }
    // Left-drag is yaw+pitch; right-drag is yaw+ROLL (horizontal spins the
    // board on its axis, vertical twists it cw/ccw); middle pans. User
    // request: the right button covers the rotation left-drag can't do.
    if (e->button() == Qt::RightButton) draggingInv_ = true;
    if (e->button() == Qt::MiddleButton) panning_ = true;
}

void VulkanWindow::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        dragging_ = false;
        if (clickCandidate_) {
            clickCandidate_ = false;
            if (measureMode_) {
                handleMeasureClick(e->position());
                updateReadout();
            } else {
                // Net pick: snap points, the nearest track segment, or (deep)
                // the copper triangle under the cursor -- so a click names the
                // signal on pads, mid-trace and pours alike, on real AND
                // derived nets. A click on bare laminate still clears: nothing
                // there names a net, so net stays -1.
                glm::vec3 p;
                bool snapped = false;
                int net = -1;
                screenToBoard(e->position(), p, snapped, net, /*deep=*/true);
                // Ctrl adds to the selection instead of replacing it.
                emit netPicked(net,
                               (e->modifiers() & Qt::ControlModifier) != 0);
            }
        }
    }
    if (e->button() == Qt::RightButton) draggingInv_ = false;
    if (e->button() == Qt::MiddleButton) panning_ = false;
    // A drag has no easing animation to keep the loop alive, so without this the
    // fast-movement downgrade would never get the frame that restores PT/RT once
    // the button comes up.
    if (!dragging_ && !draggingInv_ && !panning_) requestUpdate();
}

void VulkanWindow::mouseMoveEvent(QMouseEvent* e) {
    const QPointF delta = e->position() - lastPos_;
    lastPos_ = e->position();
    cursorPos_ = e->position();

    // A real drag cancels the measure-click candidacy.
    if (clickCandidate_ &&
        (e->position() - pressPos_).manhattanLength() > 4.0) {
        clickCandidate_ = false;
    }
    // Hover pick for the rubber band / snap highlight, only while no button
    // is steering the camera.
    if (measureMode_ && !dragging_ && !draggingInv_ && !panning_) {
        haveHover_ = screenToBoard(cursorPos_, hover_, hoverSnapped_, hoverNet_);
        updateReadout();
        requestUpdate();
    }

    if (dragging_) {
        const float s = 0.008f;
        // Identical gesture maths either way, so the board APPEARS to move the
        // same. What differs is which thing actually turns, and therefore
        // whether the light sweeps across the board or stays put on it.
        Camera after = camera_;
        after.yaw = wrapPi(after.yaw + static_cast<float>(delta.x()) * s);
        // No clamp: the basis is pole-safe, so pitch rotates through and keeps
        // going. Past vertical the view inverts, which is correct.
        after.pitch = wrapPi(after.pitch + static_cast<float>(delta.y()) * s);
        if (objectDrag_) adoptCameraDeltaIntoBoard(camera_, after);
        else camera_ = after;
        requestUpdate();
    } else if (draggingInv_) {
        // Right-drag covers the two SCREEN-relative rotations left-drag
        // doesn't: horizontal tumbles the board about the screen-VERTICAL
        // axis (globe spin -- left edge toward you, right edge away),
        // vertical twists it cw/ccw about the view axis (roll). The globe
        // spin is a rotation about the current camera up, which the
        // yaw/pitch/roll parameterisation can't express as one increment --
        // so rotate the basis and decompose back into yaw/pitch/roll.
        const float s = 0.008f;
        const Camera before = camera_;
        applyGlobeTumble(static_cast<float>(delta.x()) * s);
        camera_.roll =
            wrapPi(camera_.roll + static_cast<float>(delta.y()) * s);
        if (objectDrag_) {
            const Camera after = camera_;
            camera_ = before;  // the camera holds; the board turns
            adoptCameraDeltaIntoBoard(before, after);
        }
        requestUpdate();
    } else if (panning_) {
        // Pan across the SCREEN plane, using the camera's own right/up. Using
        // world Z as "up" made a vertical drag push the board through its own
        // thickness instead of moving it up the screen.
        const Basis b = cameraBasis(camera_);
        const float scale = camera_.distance * 0.0015f;
        const glm::vec3 move = -b.right * static_cast<float>(delta.x()) * scale +
                               b.up * static_cast<float>(delta.y()) * scale;
        if (objectDrag_) {
            // OBJECT MODE: slide the BOARD through space and leave the
            // viewpoint anchored. This is the half that was missing -- rotation
            // alone only spins the board in place at frame centre, which reads
            // as a turntable however it turns. Negated because moving the
            // viewpoint one way and the board the other look the same, and
            // rotated into world space because cameraBasis() here is the
            // BOARD-space basis (see render(): the render basis in board space
            // is exactly cameraBasis(camera_)), while translation is applied in
            // world space by boardMatrix().
            board_.translation += board_.rotation * (-move);
        } else {
            camera_.targetX += move.x;
            camera_.targetY += move.y;
            camera_.targetZ += move.z;
        }
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

    // Re-anchor to whatever the cursor is over NOW. Recomputing every wheel
    // event (even mid-glide) keeps it continuous -- at the instant of capture
    // distance == anchor distance, so the pivot shift is zero and there is no
    // jump. A view preset drives the pivot itself, so defer to it there.
    zoomToCursor_ = !cameraAnimating_ && computeZoomAnchor(e->position());
    requestUpdate();
}

// Capture the world point under the cursor for zoom-to-cursor. It is taken on
// the focal plane (through the pivot, perpendicular to the view axis) by
// unprojecting the cursor through the very matrix the last frame rendered with
// -- so it is correct for the perspective AND the orthographic projection with
// no special casing, since both bake `distance` into that matrix. Stores the
// pivot shift per unit of distance change; stepZoomAnimation applies it.
bool VulkanWindow::computeZoomAnchor(const QPointF& posDip) {
    if (!haveViewProj_ || camera_.distance < 1e-4f) return false;
    const qreal dpr = devicePixelRatio();
    const float wpx = static_cast<float>(width() * dpr);
    const float hpx = static_cast<float>(height() * dpr);
    if (wpx < 1.0f || hpx < 1.0f) return false;
    const float u = static_cast<float>(posDip.x() * dpr) / wpx * 2.0f - 1.0f;
    const float v = static_cast<float>(posDip.y() * dpr) / hpx * 2.0f - 1.0f;

    // Two points down the cursor ray. Reversed-Z: NDC depth 1 is the near
    // plane, 0.25 an arbitrary second point farther along.
    const glm::mat4 inv = glm::inverse(lastViewProj_);
    const glm::vec4 h0 = inv * glm::vec4(u, v, 1.0f, 1.0f);
    const glm::vec4 h1 = inv * glm::vec4(u, v, 0.25f, 1.0f);
    if (std::abs(h0.w) < 1e-12f || std::abs(h1.w) < 1e-12f) return false;
    const glm::vec3 a = glm::vec3(h0) / h0.w;
    const glm::vec3 b = glm::vec3(h1) / h1.w;
    const glm::vec3 dir = glm::normalize(b - a);

    const Basis basis = cameraBasis(camera_);
    const glm::vec3 target0{camera_.targetX, camera_.targetY, camera_.targetZ};
    const float denom = glm::dot(dir, basis.forward);
    if (std::abs(denom) < 1e-4f) return false;  // ray parallel to focal plane
    const float t = glm::dot(target0 - a, basis.forward) / denom;
    const glm::vec3 pFocal = a + dir * t;

    // pivot(distance) = target0 + (dist0 - distance) * K keeps pFocal fixed
    // under the cursor, where K = (pFocal - target0) / dist0.
    zoomAnchorK_ = (pFocal - target0) / camera_.distance;
    zoomAnchorTarget0_ = target0;
    zoomAnchorDist0_ = camera_.distance;
    return true;
}

bool VulkanWindow::stepZoomAnimation() {
    if (!zoomAnimating_) return false;

    const double dt = clockDt(zoomClock_);

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

    // Slide the pivot so the point captured under the cursor stays put as the
    // dolly changes distance. A view preset owns the pivot, so stand down then.
    if (zoomToCursor_ && !cameraAnimating_) {
        const glm::vec3 tgt =
            zoomAnchorTarget0_ + (zoomAnchorDist0_ - camera_.distance) * zoomAnchorK_;
        camera_.targetX = tgt.x;
        camera_.targetY = tgt.y;
        camera_.targetZ = tgt.z;
    }
    if (!zoomAnimating_) zoomToCursor_ = false;  // spent; next wheel re-anchors
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
    const double dt = clockDt(explodeClock_);

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

    const double dt = clockDt(cameraClock_);
    // Exponential approach, framerate-independent and eases out on its own.
    // Slightly slower than the peel so a view swing reads as deliberate.
    constexpr double kTimeConstant = 0.10;
    const float k = 1.0f - static_cast<float>(std::exp(-dt / kTimeConstant));

    const auto ease = [&](float& cur, float tgt) { cur += (tgt - cur) * k; };
    ease(camera_.yaw, viewTarget_.yaw);
    ease(camera_.pitch, viewTarget_.pitch);
    ease(camera_.roll, viewTarget_.roll);
    ease(camera_.distance, viewTarget_.distance);
    ease(camera_.targetX, viewTarget_.targetX);
    ease(camera_.targetY, viewTarget_.targetY);
    ease(camera_.targetZ, viewTarget_.targetZ);

    // Settled? Distances/positions are in mm, angles in radians, so weight the
    // positional terms down before summing to one comparable residual.
    const float residual =
        std::abs(camera_.yaw - viewTarget_.yaw) +
        std::abs(camera_.pitch - viewTarget_.pitch) +
        std::abs(camera_.roll - viewTarget_.roll) +
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

// --- Measurement + dimensions overlay ---------------------------------------

void VulkanWindow::setMeasureMode(bool on) {
    if (measureMode_ == on) return;
    measureMode_ = on;
    measureStage_ = 0;
    haveHover_ = false;
    clickCandidate_ = false;
    emit measureReadout(QString());
    requestUpdate();
}

// Net for an arbitrary world point: a snap point within a hair wins; failing
// that, the nearest track segment names the net -- mid-trace points are the
// common case for measuring along a run.
int VulkanWindow::netAtWorld(const glm::vec3& p) const {
    if (!mesh_) return -1;
    for (const geom::SnapPoint& sp : mesh_->snapPoints) {
        const glm::vec3 w(sp.pos[0], sp.pos[1], sp.pos[2]);
        // Only a NET-CARRYING snap answers here; a netless one (untagged pad
        // flash, bare drill) must not block the segment lookup below.
        if (sp.net >= 0 && glm::length(w - p) < 0.05f) return sp.net;
    }
    int net = -1;
    double bestD = 0.6;  // mm
    for (const geom::LayerArt::NetSeg& s : mesh_->netSegments) {
        if (s.net < 0) continue;
        const double vx = s.bx - s.ax, vy = s.by - s.ay;
        const double ll = vx * vx + vy * vy;
        double tt =
            ll > 1e-12 ? ((p.x - s.ax) * vx + (p.y - s.ay) * vy) / ll : 0.0;
        tt = std::clamp(tt, 0.0, 1.0);
        const double d = std::hypot(p.x - (s.ax + vx * tt),
                                    p.y - (s.ay + vy * tt));
        if (d < bestD) {
            bestD = d;
            net = s.net;
        }
    }
    return net;
}

// Re-resolve a pinned measurement's nets against the CURRENT net table. Nets
// can appear after the pins were placed -- inferring pseudo-nets is exactly
// that flow -- and without this the along-the-copper readout stayed dark on
// endpoints that now sit on perfectly good nets.
void VulkanWindow::refreshMeasurementNets() {
    if (measureStage_ < 1) return;
    measureANet_ = netAtWorld(measureA_);
    if (measureStage_ >= 2) measureBNet_ = netAtWorld(measureB_);
    updateReadout();
    requestUpdate();
}

void VulkanWindow::setMeasurement(float ax, float ay, float az, float bx,
                                  float by, float bz) {
    measureMode_ = true;
    measureA_ = {ax, ay, az};
    measureB_ = {bx, by, bz};
    measureANet_ = netAtWorld(measureA_);
    measureBNet_ = netAtWorld(measureB_);
    measureStage_ = 2;
    emit measureModeChanged(true);
    updateReadout();
    requestUpdate();
}

void VulkanWindow::setDimensionsOverlay(bool on) {
    if (dimsOverlay_ == on) return;
    dimsOverlay_ = on;
    appSettings().setValue("dimensionsOverlay", on);
    requestUpdate();
}

bool VulkanWindow::worldToScreen(const glm::vec3& w, float& px,
                                 float& py) const {
    if (!haveViewProj_) return false;
    const glm::vec4 clip = lastViewProj_ * glm::vec4(w, 1.0f);
    if (clip.w <= 1e-6f) return false;
    const qreal dpr = devicePixelRatio();
    px = (clip.x / clip.w * 0.5f + 0.5f) * static_cast<float>(width() * dpr);
    py = (clip.y / clip.w * 0.5f + 0.5f) * static_cast<float>(height() * dpr);
    return true;
}

bool VulkanWindow::screenToBoard(const QPointF& posDip, glm::vec3& out,
                                 bool& snapped, int& net, bool deep) {
    snapped = false;
    net = -1;
    if (!haveViewProj_ || !mesh_) return false;
    const qreal dpr = devicePixelRatio();
    const float px = static_cast<float>(posDip.x() * dpr);
    const float py = static_cast<float>(posDip.y() * dpr);

    // Snap targets first: nearest pad/drill centre or outline vertex within a
    // small screen radius wins, making the measurement fab-exact. Strict `<`
    // keeps the FIRST of equally-near points -- net-carrying points are
    // emitted first, so they beat their netless drill twins.
    const float thresh = 14.0f * static_cast<float>(dpr);
    float best = thresh * thresh;
    bool found = false;
    glm::vec3 bestP{0.0f};
    int bestNet = -1;
    for (const geom::SnapPoint& sp : mesh_->snapPoints) {
        const glm::vec3 wpt(sp.pos[0], sp.pos[1], sp.pos[2]);
        float sx, sy;
        if (!worldToScreen(wpt, sx, sy)) continue;
        const float d2 = (sx - px) * (sx - px) + (sy - py) * (sy - py);
        if (d2 < best) {
            best = d2;
            bestP = wpt;
            bestNet = sp.net;
            found = true;
        }
    }
    if (found && bestNet >= 0) {
        out = bestP;
        snapped = true;
        net = bestNet;
        return true;
    }
    if (found) {
        // Snapped to a NETLESS point (a Gerber pad flash without X2 tags, a
        // bare drill). Keep the magnetic position, but fall through to the
        // segment/triangle net lookups below -- returning here made snapping
        // to a pad WORSE at naming nets than clicking beside it.
        out = bestP;
        snapped = true;
    }

    // Free point (no snap): unproject the cursor through the SAME matrix the
    // frame rendered with and intersect the board-top plane. Reversed-Z: NDC
    // depth 1 is the near plane, 0.25 just a second point along the ray. A
    // netless SNAP skips this -- its position is already exact -- and goes
    // straight to the net lookups below.
    if (!snapped) {
        const float u = px / static_cast<float>(width() * dpr) * 2.0f - 1.0f;
        const float v = py / static_cast<float>(height() * dpr) * 2.0f - 1.0f;
        const glm::mat4 inv = glm::inverse(lastViewProj_);
        const glm::vec4 h0 = inv * glm::vec4(u, v, 1.0f, 1.0f);
        const glm::vec4 h1 = inv * glm::vec4(u, v, 0.25f, 1.0f);
        if (std::abs(h0.w) < 1e-12f || std::abs(h1.w) < 1e-12f) return false;
        const glm::vec3 a = glm::vec3(h0) / h0.w;
        const glm::vec3 b = glm::vec3(h1) / h1.w;
        const glm::vec3 dir = glm::normalize(b - a);
        if (std::abs(dir.z) < 1e-6f) return false;
        const float topZ = static_cast<float>(mesh_->boardTopZ);
        const float t = (topZ - a.z) / dir.z;
        if (t < 0.0f) return false;
        out = a + dir * t;
    }
    // A free point can still sit ON a track. Naming its net from the nearest
    // segment is what makes "length along the trace between these two points"
    // work for clicks in the MIDDLE of a run -- the usual gesture -- where no
    // snap point exists to carry the net.
    {
        double bestD = 0.6;  // mm: about a trace width
        for (const geom::LayerArt::NetSeg& s : mesh_->netSegments) {
            if (s.net < 0) continue;
            const double vx = s.bx - s.ax, vy = s.by - s.ay;
            const double ll = vx * vx + vy * vy;
            double tt = ll > 1e-12 ? ((out.x - s.ax) * vx + (out.y - s.ay) * vy) / ll
                                   : 0.0;
            tt = std::clamp(tt, 0.0, 1.0);
            const double d = std::hypot(out.x - (s.ax + vx * tt),
                                        out.y - (s.ay + vy * tt));
            if (d < bestD) {
                bestD = d;
                net = s.net;
            }
        }
    }
    // Deep lookup (clicks only -- a full triangle scan is too slow for every
    // hover move): whichever net-tagged copper TRIANGLE contains the point.
    // This is what names pads, pours and zone copper -- geometry that has no
    // centreline segment -- on real and derived nets alike. Topmost wins,
    // since the pick ray came from above.
    if (deep && net < 0) {
        float bestZ = -std::numeric_limits<float>::infinity();
        for (const geom::Part& part : mesh_->parts) {
            if (part.triNet.empty()) continue;
            const auto& vs = part.mesh.vertices;
            const auto& is = part.mesh.indices;
            const size_t tris = std::min(part.triNet.size(), is.size() / 3);
            for (size_t ti = 0; ti < tris; ++ti) {
                const int tn = part.triNet[ti];
                if (tn < 0) continue;
                const auto& v0 = vs[is[ti * 3 + 0]].position;
                const auto& v1 = vs[is[ti * 3 + 1]].position;
                const auto& v2 = vs[is[ti * 3 + 2]].position;
                const float z =
                    static_cast<float>((v0[2] + v1[2] + v2[2]) / 3.0);
                if (z <= bestZ) continue;
                // 2D sign test, tolerant of either winding.
                const auto side = [&](const auto& a, const auto& b) {
                    return (out.x - a[0]) * (b[1] - a[1]) -
                           (out.y - a[1]) * (b[0] - a[0]);
                };
                const double s0 = side(v0, v1), s1 = side(v1, v2),
                             s2 = side(v2, v0);
                const bool allNeg = s0 <= 0 && s1 <= 0 && s2 <= 0;
                const bool allPos = s0 >= 0 && s1 >= 0 && s2 >= 0;
                if (!allNeg && !allPos) continue;
                bestZ = z;
                net = tn;
            }
        }
    }
    return true;
}

void VulkanWindow::handleMeasureClick(const QPointF& posDip) {
    glm::vec3 p;
    bool snapped = false;
    int net = -1;
    if (!screenToBoard(posDip, p, snapped, net, /*deep=*/true)) return;
    if (measureStage_ == 1) {
        measureB_ = p;
        measureBNet_ = net;
        measureStage_ = 2;
    } else {
        // Idle or already pinned: this click starts a fresh measurement.
        measureA_ = p;
        measureANet_ = net;
        measureStage_ = 1;
    }
    requestUpdate();
}

void VulkanWindow::buildOverlay() {
    if (!renderer_) return;
    std::vector<float> tris;
    const float dpr = static_cast<float>(devicePixelRatio());

    const auto push = [&](float x, float y, const float c[4]) {
        tris.push_back(x);
        tris.push_back(y);
        tris.insert(tris.end(), c, c + 4);
    };
    const auto quad = [&](float ax, float ay, float bx, float by, float wpx,
                          const float c[4]) {
        float dx = bx - ax, dy = by - ay;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-3f) return;
        dx /= len;
        dy /= len;
        const float nx = -dy * wpx * 0.5f, ny = dx * wpx * 0.5f;
        push(ax + nx, ay + ny, c);
        push(bx + nx, by + ny, c);
        push(bx - nx, by - ny, c);
        push(ax + nx, ay + ny, c);
        push(bx - nx, by - ny, c);
        push(ax - nx, ay - ny, c);
    };
    const auto marker = [&](float x, float y, float r, const float c[4]) {
        push(x, y - r, c);
        push(x + r, y, c);
        push(x, y + r, c);
        push(x, y - r, c);
        push(x, y + r, c);
        push(x - r, y, c);
    };
    const auto drawText = [&](const std::string& s, float x, float y,
                              float sizePx, const float c[4],
                              double rotation = 0.0) {
        text::TextStyle st;
        st.size = {sizePx * 0.9, sizePx};
        st.thickness = sizePx * 0.14;
        st.rotation = rotation;
        // Stroke layout is Y-down (KiCad sense) -- exactly screen pixels.
        const float shadow[4] = {0.0f, 0.0f, 0.0f, 0.75f};
        for (int pass = 0; pass < 2; ++pass) {
            const float off = (pass == 0) ? dpr : 0.0f;
            const auto lines = text::layout(
                s, {static_cast<double>(x + off), static_cast<double>(y + off)},
                st);
            for (const auto& pl : lines) {
                for (size_t i = 0; i + 1 < pl.size(); ++i) {
                    quad(static_cast<float>(pl[i].x),
                         static_cast<float>(pl[i].y),
                         static_cast<float>(pl[i + 1].x),
                         static_cast<float>(pl[i + 1].y),
                         static_cast<float>(st.thickness),
                         pass == 0 ? shadow : c);
                }
            }
        }
    };

    static const float kAmber[4] = {1.0f, 0.82f, 0.25f, 0.95f};
    static const float kWhite[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const float kDim[4] = {0.72f, 0.84f, 1.0f, 0.9f};

    // Camera readout. Deliberately part of the OVERLAY rather than the status
    // bar: the overlay is drawn into the frame itself, so it survives
    // PCBVIEW_CAPTURE and a screenshot carries the exact view that produced
    // it. Reading a capture without this means guessing the projection, and
    // guessing it is how a pixel gets mapped to the wrong board coordinate.
    //
    // The numbers are exactly the ones the headless hooks take, so a view can
    // be read off a screenshot and reproduced verbatim.
    if (cameraHud_) {
        const float wpx = static_cast<float>(width()) * dpr;
        const float hpx = static_cast<float>(height()) * dpr;
        const float size = 13.0f * dpr;
        const float lh = size * 1.45f;
        float y = size * 1.8f;
        const float x = 10.0f * dpr;

        // Millimetres per pixel. Exact in orthographic; in perspective it is
        // only true at the target plane, and is labelled so nobody measures
        // off-plane geometry with it.
        const float halfH = camera_.orthographic
                                ? orthoHalfHeight()
                                : camera_.distance *
                                      std::tan(camera_.fovDegrees * 0.5f *
                                               3.14159265f / 180.0f);
        const float mmPerPx = (hpx > 0.0f) ? (2.0f * halfH / hpx) : 0.0f;

        // text::layout CENTRES on the origin (KiCad's convention for
        // reference/value text), so a left-aligned HUD has to offset by half
        // the advance width or it runs off the left edge of the frame.
        const auto drawLeft = [&](const std::string& t, float ty) {
            text::TextStyle st;
            st.size = {size * 0.9, size};
            st.thickness = size * 0.14;
            drawText(t, x + static_cast<float>(text::measure(t, st)) * 0.5f, ty,
                     size, kAmber);
        };

        char buf[256];
        std::snprintf(buf, sizeof buf, "yaw %.2f  pitch %.2f  roll %.2f",
                      camera_.yaw, camera_.pitch, camera_.roll);
        drawLeft(buf, y);
        y += lh;
        std::snprintf(buf, sizeof buf, "dist %.3f  target %.3f, %.3f, %.3f",
                      camera_.distance, camera_.targetX, camera_.targetY,
                      camera_.targetZ);
        drawLeft(buf, y);
        y += lh;
        std::snprintf(buf, sizeof buf, "%s  %.6f mm/px%s  %.0fx%.0f",
                      camera_.orthographic ? "ortho" : "persp",
                      mmPerPx,
                      camera_.orthographic ? "" : " at target",
                      wpx, hpx);
        drawLeft(buf, y);
    }

    // Board dimension callouts, fab-drawing style: W below the board, H at its
    // left, world-anchored so they follow the camera.
    if (dimsOverlay_ && mesh_ && mesh_->outlineValid) {
        const float x0 = static_cast<float>(mesh_->outlineMin[0]);
        const float y0 = static_cast<float>(mesh_->outlineMin[1]);
        const float x1 = static_cast<float>(mesh_->outlineMax[0]);
        const float y1 = static_cast<float>(mesh_->outlineMax[1]);
        const float z = static_cast<float>(mesh_->boardTopZ);
        const float m =
            0.08f * std::max(x1 - x0, y1 - y0);  // callout offset, mm

        const auto dimLine = [&](glm::vec3 wa, glm::vec3 wb,
                                 const std::string& label) {
            float ax, ay, bx, by;
            if (!worldToScreen(wa, ax, ay) || !worldToScreen(wb, bx, by))
                return;
            quad(ax, ay, bx, by, 1.5f * dpr, kDim);
            // End ticks, perpendicular on screen.
            float dx = bx - ax, dy = by - ay;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len > 1e-3f) {
                dx /= len;
                dy /= len;
                const float t = 6.0f * dpr;
                quad(ax - dy * t, ay + dx * t, ax + dy * t, ay - dx * t,
                     1.5f * dpr, kDim);
                quad(bx - dy * t, by + dx * t, bx + dy * t, by - dx * t,
                     1.5f * dpr, kDim);
            }
            drawText(label, (ax + bx) * 0.5f, (ay + by) * 0.5f - 11.0f * dpr,
                     12.0f * dpr, kDim);
        };
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.3f mm", x1 - x0);
        dimLine({x0, y0 - m, z}, {x1, y0 - m, z}, buf);
        std::snprintf(buf, sizeof(buf), "%.3f mm", y1 - y0);
        dimLine({x0 - m, y0, z}, {x0 - m, y1, z}, buf);
    }

    // The measurement itself: first point marker, rubber-band (or pinned)
    // line, live distance label.
    if (measureMode_ && measureStage_ >= 1) {
        const bool haveEnd = (measureStage_ >= 2) || haveHover_;
        const glm::vec3 end = (measureStage_ >= 2) ? measureB_ : hover_;
        float ax, ay;
        if (worldToScreen(measureA_, ax, ay)) {
            marker(ax, ay, 5.0f * dpr, kAmber);
            float bx, by;
            if (haveEnd && worldToScreen(end, bx, by)) {
                quad(ax, ay, bx, by, 2.0f * dpr, kAmber);
                marker(bx, by, 5.0f * dpr, kAmber);
                const glm::vec3 d = end - measureA_;
                char buf[96];
                std::snprintf(buf, sizeof(buf), "%.3f mm", glm::length(d));
                drawText(buf, (ax + bx) * 0.5f, (ay + by) * 0.5f - 14.0f * dpr,
                         14.0f * dpr, kWhite);
                // When both ends sit on the same net, the number people came
                // for -- the length ALONG the copper -- goes right under the
                // straight-line figure, at the measurement, not only in the
                // corner panel where nobody is looking.
                const int endNet = (measureStage_ >= 2)
                                       ? measureBNet_
                                       : (haveHover_ ? hoverNet_ : -1);
                if (mesh_ && measureANet_ >= 0 && endNet == measureANet_) {
                    const double path = geom::netPathLength(
                        *mesh_, measureANet_, measureA_.x, measureA_.y, end.x,
                        end.y);
                    if (path >= 0.0) {
                        std::snprintf(buf, sizeof(buf), "route %.3f mm", path);
                        drawText(buf, (ax + bx) * 0.5f,
                                 (ay + by) * 0.5f + 14.0f * dpr, 13.0f * dpr,
                                 kAmber);
                    }
                }
            }
        }
    }
    if (measureMode_ && haveHover_ && hoverSnapped_) {
        float sx, sy;
        if (worldToScreen(hover_, sx, sy)) {
            marker(sx, sy, 6.0f * dpr, kWhite);
        }
    }

    // Net panel: when both measurement endpoints sit on the SAME net, show
    // that net's routed length in the corner -- the measured straight line is
    // the crow-flies distance, this is the copper the signal actually takes.
    if (measureMode_ && measureStage_ >= 1 && mesh_ && measureANet_ >= 0) {
        const int endNet =
            (measureStage_ >= 2) ? measureBNet_ : (haveHover_ ? hoverNet_ : -1);
        if (endNet == measureANet_ &&
            measureANet_ < static_cast<int>(mesh_->nets.size())) {
            const auto& net = mesh_->nets[measureANet_];
            const glm::vec3 end = (measureStage_ >= 2) ? measureB_ : hover_;
            const double path = geom::netPathLength(
                *mesh_, measureANet_, measureA_.x, measureA_.y, end.x, end.y);
            char l1[96], l2[64], l3[64];
            std::snprintf(l1, sizeof(l1), "Net %s", net.name.c_str());
            if (path >= 0.0) {
                std::snprintf(l2, sizeof(l2), "Shortest route %.3f mm", path);
            } else if (mesh_->netsArePseudo) {
                // Derived nets ARE connectivity: same group = proven joined.
                std::snprintf(l2, sizeof(l2),
                              "Connected through copper (pour/plane)");
            } else if (net.hasPlane) {
                std::snprintf(l2, sizeof(l2),
                              "Joined through pour/plane copper");
            } else {
                std::snprintf(l2, sizeof(l2), "No track route between points");
            }
            std::snprintf(l3, sizeof(l3), "Net total %.3f mm, %d via%s",
                          net.routedMm, net.viaCount,
                          net.viaCount == 1 ? "" : "s");
            const char* rows[3] = {l1, l2, l3};

            const float ts = 13.0f * dpr;   // text size
            const float lh = ts * 1.5f;     // line height
            const float pad = 10.0f * dpr;  // panel padding
            text::TextStyle st;
            st.size = {static_cast<double>(ts) * 0.9,
                       static_cast<double>(ts)};
            st.thickness = ts * 0.14;
            float wMax = 0.0f;
            for (const char* r : rows)
                wMax = std::max(wMax,
                                static_cast<float>(text::measure(r, st)));

            const float panelW = wMax + 2.0f * pad;
            const float panelH = 3.0f * lh + 2.0f * pad;
            const float x1 =
                static_cast<float>(width() * dpr) - 14.0f * dpr;
            const float x0 = x1 - panelW;
            const float y0 = 14.0f * dpr;

            static const float kPanelBg[4] = {0.07f, 0.07f, 0.09f, 0.85f};
            // Background as one thick "line", plus an amber accent bar.
            quad(x0, y0 + panelH * 0.5f, x1, y0 + panelH * 0.5f, panelH,
                 kPanelBg);
            quad(x0, y0 + panelH * 0.5f, x0 + 3.0f * dpr, y0 + panelH * 0.5f,
                 panelH, kAmber);
            for (int i = 0; i < 3; ++i) {
                // layout() centres on the origin; centre each row in the panel.
                drawText(rows[i], (x0 + x1) * 0.5f,
                         y0 + pad + lh * (static_cast<float>(i) + 0.5f), ts,
                         i == 0 ? kAmber : kWhite);
            }
        }
    }

    renderer_->setOverlay(std::move(tris));
}

void VulkanWindow::updateReadout() {
    QString text;
    if (measureMode_ && measureStage_ >= 1) {
        const bool haveEnd = (measureStage_ >= 2) || haveHover_;
        if (haveEnd) {
            const glm::vec3 end =
                (measureStage_ >= 2) ? measureB_ : hover_;
            const glm::vec3 d = end - measureA_;
            text = QString("Measure: %1 mm   (dx %2  dy %3  dz %4)")
                       .arg(glm::length(d), 0, 'f', 3)
                       .arg(std::abs(d.x), 0, 'f', 3)
                       .arg(std::abs(d.y), 0, 'f', 3)
                       .arg(std::abs(d.z), 0, 'f', 3);
            const int endNet =
                (measureStage_ >= 2) ? measureBNet_ : hoverNet_;
            if (mesh_ && measureANet_ >= 0 && endNet == measureANet_ &&
                measureANet_ < static_cast<int>(mesh_->nets.size())) {
                const auto& net = mesh_->nets[measureANet_];
                const double path = geom::netPathLength(
                    *mesh_, measureANet_, measureA_.x, measureA_.y, end.x,
                    end.y);
                text += QString("   |   net %1: ")
                            .arg(QString::fromStdString(net.name));
                if (path >= 0.0)
                    text += QString("shortest route %1 mm, ")
                                .arg(path, 0, 'f', 3);
                else if (mesh_->netsArePseudo)
                    text += "connected through copper (pour/plane), ";
                else if (net.hasPlane)
                    text += "joined through pour/plane copper, ";
                text += QString("total %1 mm routed")
                            .arg(net.routedMm, 0, 'f', 3);
            }
        }
    } else if (measureMode_) {
        text = "Measure: click the first point";
    }
    emit measureReadout(text);
}

float VulkanWindow::orthoHalfHeight() const {
    return camera_.distance *
           std::tan(glm::radians(camera_.fovDegrees) * 0.5f);
}

float VulkanWindow::sceneRadius() const {
    // Farthest board corner from the orbit target, plus however far a full
    // peel throws the outermost ring, plus a margin. Used to bracket the
    // orthographic depth range (and to push the ortho ray origin back) so
    // nothing is ever clipped for being near or behind the camera plane.
    float radius = 1.0f;
    if (mesh_) {
        const auto& b = mesh_->bounds;
        const glm::vec3 target(camera_.targetX, camera_.targetY,
                               camera_.targetZ);
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 corner(
                static_cast<float>((i & 1) ? b.max[0] : b.min[0]),
                static_cast<float>((i & 2) ? b.max[1] : b.min[1]),
                static_cast<float>((i & 4) ? b.max[2] : b.min[2]));
            radius = std::max(radius, glm::length(corner - target));
        }
    }
    // A peeled stack reaches well outside the rest bounds.
    if (renderer_) radius += renderer_->maxRank() * explodeStepMm();
    return radius * 1.10f + 5.0f;  // margin for components and slack
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
            // Handled here as well as on the View menu action: QAction
            // shortcuts never fire while this native QWindow has focus.
            case Qt::Key_Home: recenterAll(); return;
            case Qt::Key_O:
                camera_.orthographic = !camera_.orthographic;
                emit orthoChanged(camera_.orthographic);
                requestUpdate();
                return;
            case Qt::Key_M:
                setMeasureMode(!measureMode_);
                emit measureModeChanged(measureMode_);
                return;
            case Qt::Key_R:
                emit moveRecordToggled();
                return;
            case Qt::Key_Escape:
                // Clear the current measurement but stay in measure mode.
                if (measureMode_) {
                    measureStage_ = 0;
                    updateReadout();
                    requestUpdate();
                    return;
                }
                // Otherwise: un-highlight every net, same as clicking bare
                // board -- the pick handler treats -1 as "clear".
                emit netPicked(-1, false);
                return;
            default:
                break;
        }
    }
    emit unhandledKey(e->key(), e->modifiers());
    QWindow::keyPressEvent(e);
}

}  // namespace pcbview::app
