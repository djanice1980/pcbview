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

// Seeded from the environment / --vr so a launch flag still works, then owned
// by the menu toggle. Static: the viewport is destroyed and rebuilt to enter or
// leave VR, so this cannot live on the window it outlives.
bool VulkanWindow::vrRequested_ = qEnvironmentVariableIsSet("PCBVIEW_VR");

void VulkanWindow::setVrMode(bool on) {
    if (on == vrRequested_) return;
    vrRequested_ = on;
    // NOT persisted, deliberately. Remembering it would mean the next launch
    // brings SteamVR up on its own -- creating an OpenXR instance starts the
    // runtime -- so an app opened to glance at a board would haul the whole
    // compositor in with it. The toggle is the way in, per session.
    //
    // Queued at the far end: this is called from a menu handler, and the
    // rebuild deletes this window.
    emit viewportRebuildRequired();
}

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

// Pick the board up and put it down.
//
// The follow is rigid: while a hand is squeezing, the board keeps the same
// position and orientation RELATIVE TO THAT HAND, so it behaves like something
// held rather than something nudged. Working it out from the pose at the
// moment of the grab rather than accumulating per-frame deltas means no drift,
// and letting go leaves it exactly where it was released.
//
// The algebra, since it is not obvious. The board's model matrix is
//
//     M(t,R) = T(t + c) * R * T(-c)
//
// with c the bounds centre, so rotation happens about the centre. A rigid
// follow wants M_new = D * M_old for the hand's own movement
//
//     D = T(p) * dR * T(-p0)
//
// where p0 and p are the grab point then and now. Expanding and matching the
// M(t,R) form again gives
//
//     R' = dR * R0
//     t' = p + dR * (t0 + c - p0) - c
//
// which is what this computes. Both hands are equal; the first to squeeze
// holds it, so it works left-handed or right-handed without a setting.
void VulkanWindow::stepVrGrab() {
    if (!vr_ || !mesh_) return;

    const auto& b = mesh_->bounds;
    const glm::vec3 c(static_cast<float>((b.min[0] + b.max[0]) * 0.5),
                      static_cast<float>((b.min[1] + b.max[1]) * 0.5),
                      static_cast<float>((b.min[2] + b.max[2]) * 0.5));

    for (int h = 0; h < 2; ++h) {
        float pos[3], quat[4];
        const bool held = vr_->grabbing(h) && vr_->gripPose(h, pos, quat);
        if (!held) {
            if (grabHand_ == h) grabHand_ = -1;   // released, or lost tracking
            continue;
        }
        if (grabHand_ != -1 && grabHand_ != h) continue;  // the other hand has it

        const glm::vec3 p(pos[0], pos[1], pos[2]);
        const glm::quat q(quat[3], quat[0], quat[1], quat[2]);
        if (grabHand_ != h) {
            // Take hold: remember where the hand and the board were.
            grabHand_ = h;
            grabHandPos0_ = p;
            grabHandRot0_ = q;
            grabBoard0_ = board_;
            emit statusMessage("Board grabbed");
            continue;
        }

        const glm::quat dR = glm::normalize(q * glm::inverse(grabHandRot0_));
        board_.rotation = glm::normalize(dR * grabBoard0_.rotation);
        board_.translation =
            p + dR * (grabBoard0_.translation + c - grabHandPos0_) - c;
    }
}

void VulkanWindow::stepVr() {
    if (!vr_ || !renderer_ || !mesh_) return;

    static const bool allowPt = qEnvironmentVariableIsSet("PCBVIEW_VR_PT");

    // Every VR setting, read once, in one place. They are environment
    // variables and an environment variable set in a shell OUTLIVES the run
    // that set it -- so these are also what gets printed below, because a
    // leftover PCBVIEW_VR_PT reads as a mysteriously slow, mysteriously fuzzy
    // "raster" mode and there is no way to tell from inside the headset.
    static const int spp = [] {
        bool ok = false;
        const int n = qgetenv("PCBVIEW_VR_SPP").toInt(&ok);
        return (ok && n >= 1 && n <= 256) ? n : 16;
    }();
    static const int dnPasses = [] {
        bool ok = false;
        const int n = qgetenv("PCBVIEW_VR_DENOISE").toInt(&ok);
        return (ok && n >= 0 && n <= 5) ? n : 5;
    }();
    // 1.0: render EXACTLY what the runtime asks for, and nothing more.
    //
    // The recommendation is not a raw panel size, it is already the answer to
    // this question. SteamVR's log shows the working: the driver wants
    // 3400x3468 to cover lens distortion, SteamVR applies its own resolution
    // setting on top -- "Clamping render target scale to 1.5x total area" --
    // and reports 4164x4244. Supersampling again here multiplies a number that
    // has already been multiplied, and 1.5x on top came to roughly 9.7x the
    // panel's native pixel count per eye.
    //
    // So the runtime is the authority and SteamVR's own resolution slider is
    // the one place to tune this. An app-side factor is a second, invisible
    // multiplier that makes that slider mean something different for us than
    // for every other title.
    static const float ss = [] {
        bool ok = false;
        const float f = qgetenv("PCBVIEW_VR_SS").toFloat(&ok);
        return (ok && f >= 0.5f && f <= 2.0f) ? f : 1.0f;
    }();
    static const int rateDiv = [] {
        bool ok = false;
        const int n = qgetenv("PCBVIEW_VR_RATE_DIV").toInt(&ok);
        return (ok && n >= 1 && n <= 4) ? n : 1;
    }();
    // PCBVIEW_VR_RT=0 drops the ray-traced shadows and AO for plain raster
    // shading -- no rays anywhere in the frame. Worth having as a baseline:
    // everything else is layered on top of it, so when the picture misbehaves
    // this is the floor to compare against. Read here rather than at the point
    // of use so the sweep below can override it.
    static const bool wantRt = [] {
        const QByteArray v = qgetenv("PCBVIEW_VR_RT");
        return !(v == "0" || v == "false");
    }();
    // PCBVIEW_VR_RAYQ picks the ray budget: 0 full (11 rays a fragment), 1
    // reduced (7), 2 cheap (4). Defaults to 0 so nothing changes until the
    // sweep has said what the trade actually costs.
    static const int rayQ = [] {
        bool ok = false;
        const int n = qgetenv("PCBVIEW_VR_RAYQ").toInt(&ok);
        return (ok && n >= 0 && n <= 2) ? n : 0;
    }();
    // What this frame actually uses. Identical to the settings above unless
    // the adaptive ladder or the sweep is driving them.
    float ssEff = ss;
    bool rtEff = wantRt;
    int rayQEff = rayQ;
    // PCBVIEW_VR_FOVEATE forces a level; the ladder drives it otherwise.
    static const int fovEnv = [] {
        bool ok = false;
        const int n = qgetenv("PCBVIEW_VR_FOVEATE").toInt(&ok);
        return (ok && n >= 0 && n <= 2) ? n : -1;
    }();
    int fovEff = fovEnv >= 0 ? fovEnv : 0;
    // PCBVIEW_VR_RT=0 forces the shadow and AO rays off for the whole session,
    // whatever the ladder, the model or the Render menu would have chosen.
    //
    // A diagnostic, and a decisive one. The rays are the only part of the frame
    // whose cost depends on which way a surface faces: on the shadowed side
    // they meet the board slab within a few BVH nodes, on the lit side they fly
    // into open space and traverse the whole structure before missing. Turning
    // them off makes both faces cost the same, so if an artefact that appears
    // on one face and not the other survives it, lighting cost is not what
    // causes it. The Render menu can do this too, but only before entering VR
    // and only if the ladder does not override it, which is exactly how the
    // last attempt ended up logging "ray-traced raster" anyway.
    static const int rtEnv = [] {
        const QByteArray v = qgetenv("PCBVIEW_VR_RT");
        if (v.isEmpty()) return -1;
        return (v == "0" || v == "false") ? 0 : 1;
    }();

    // Both eyes' GPU time for the frame just completed. Taken once, here, so
    // the ladder below and the sweep see the same number.
    const double frameGpuMs = renderer_->takeGpuMs();
    // Its matched pair: how many fragments that cost paid for. Taken here for
    // the same reason -- one read per frame, one number everyone below agrees
    // on. Zero when the device cannot count them, which the model checks for.
    const double frameFragInv = renderer_->takeFragInvocations();
    // How far back temporal reuse averages. This is the quality/latency dial:
    // higher is cleaner and slower to react to a change in the picture, and it
    // is what lets a handful of samples a frame look like a great many.
    static const int histFrames = [] {
        bool ok = false;
        const int n = qgetenv("PCBVIEW_VR_HISTORY").toInt(&ok);
        return (ok && n >= 1 && n <= 128) ? n : 32;
    }();

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

    // Everything the pad and the mouse have done to the board, handed to the
    // session so the eye matrices carry it. Set BEFORE beginFrame, which is
    // where the placement is composed.
    const glm::mat4 bm = boardMatrix();
    vr_->setBoardPose(&bm[0][0]);

    std::vector<xr::VrSession::Eye> eyes;
    const bool render = vr_->beginFrame(&eyes);
    stepVrGrab();
    if (!vr_->active()) {  // the runtime ended the session
        if (vrTimer_) vrTimer_->stop();
        vr_.reset();
        // Ended from the runtime's side -- SteamVR closed, or the user quit the
        // session there. The menu has to follow that, or it sits checked over a
        // desktop window.
        vrRequested_ = false;
        emit vrActiveChanged(false);
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
    // A handover is expensive, so it has to mean something.
    //
    // setCaptureExtent(0, 0) runs Renderer::resize: a device wait, the
    // swapchain destroyed and rebuilt, and every scene and bloom target with
    // it. Doing that the instant the runtime declines a SINGLE frame -- and
    // then again on the next frame when it asks for one -- is two full rebuilds
    // for nothing. The session log showed the scene extent flipping between the
    // window's size and the eye's over and over, and each flip is a stall the
    // viewer sees.
    //
    // What this exists for is the headset being set down, and that lasts. Half
    // a second of consistent silence tells it apart from a hiccup and costs
    // nothing when the headset really has come off.
    vrIdleFrames_ = render ? 0 : vrIdleFrames_ + 1;
    if (!render) {
        if (vrOwnsRenderer_ && vrIdleFrames_ >= 45) {
            vrOwnsRenderer_ = false;
            renderer_->setOffscreenOnly(false);
            // Give the scene resolution back to the WINDOW.
            //
            // The capture extent is how VR asks for an eye-sized render, and
            // leaving it set meant the desktop carried on at eye resolution
            // after the headset came off -- 6246x6366 with supersampling, forty
            // megapixels, while also switching path tracing back on at that
            // size. That is thirteen RGBA32F buffers of some 636 MB each on the
            // GPU and OIDN's readback buffers of comparable size in host RAM,
            // allocated the instant the headset is set down. Both climb, and
            // the app stops responding while it tries.
            renderer_->setCaptureExtent(0, 0);
            // The hidden-area mesh belongs to the headset's lenses. Drawing it
            // into a desktop window would punch holes in the corners.
            renderer_->selectVisibilityMask(-1);
            // Foveation is for lenses. A desktop window has none, and blurring
            // the edges of a monitor would simply look broken.
            renderer_->setFoveation(0);
            // Back to the interactive sample ramp and OIDN for a mouse camera:
            // the desktop can afford the round trip and gets the better result.
            renderer_->setPathTraceBatch(0);
            renderer_->setGpuDenoisePasses(0);
            // The desktop wants its key light back over the viewer's shoulder,
            // and its shadows unbounded -- it has the budget, and its
            // camera-relative rig does not go through the capped path anyway.
            renderer_->setRasterWorldSun(false);
            renderer_->setShadowRangeIndex(0);
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
        renderer_->setPathTraceBatch(allowPt ? spp : 0);

        // GPU a-trous cleanup on top, so a modest sample count can look like a
        // large one. OIDN is not an option here: it round-trips through host
        // memory, about 200 MB per eye per frame at this resolution, which is
        // why it is asynchronous and why it can never serve a headset.
        // PCBVIEW_VR_DENOISE=0 disables it for comparison.
        renderer_->setGpuDenoisePasses(allowPt ? dnPasses : 0);
        // Anchor the temporal test and the filter's reach to the BOARD rather
        // than to the screen, so neither changes character as you walk up to it
        // or step back.
        renderer_->setWorldScale(span);
        // NOT freeing the path tracer's buffers in raster mode, despite the
        // temptation: they are thirteen RGBA32F images at scene resolution and
        // raster touches none of them.
        //
        // Tearing them down crashed on the first VR frame. The obvious causes
        // are all ruled out -- destroyImage clears its handles so there is no
        // double free, recordPathTrace is gated on the render mode, and the
        // descriptor updates bail out on null views -- so the real reason is
        // still unknown, and it is only reachable with a headset attached,
        // which is not something the desktop path can exercise. An unexplained
        // crash is not worth a memory saving nobody has asked for.
        //
        // It does put a ceiling on supersampling: at 1.5x on a full-resolution
        // eye those buffers are over eight gigabytes. PCBVIEW_VR_RES is the
        // release valve until this is understood properly.

        // --- Adaptive quality -----------------------------------------------
        //
        // The wobble is a frame-rate symptom and frame cost is fill cost, so
        // the two things that decide it are how much of the eye the board
        // covers and whether we are actually keeping up. Steer on both.
        //
        // Distance thresholds are the ones that came out of testing rather
        // than a guess: at 0.5x resolution every rung held 90 Hz until the
        // board came inside about a quarter of a metre, where even four rays
        // stopped fitting.
        //
        // Pacing is the backstop, because distance alone cannot know what the
        // GPU is doing -- a denser board, a bigger one, or a slower machine
        // all move the line. Measured GPU time against the headset's own frame
        // budget catches those.
        struct Tier {
            bool rt;
            int rayQ;
            int fov;
            float ss;
            const char* name;
        };
        // The bottom rung keeps its shadows.
        //
        // It used to be plain raster, which is what made getting close
        // unsatisfying: walk up to the board to look at something and the
        // shadows vanish, exactly when you most want the depth cue. Foveation
        // buys that back -- coarsening the periphery to one shaded fragment
        // per 2x2 and then 4x4 pixels cuts rays where the lens is blurring
        // anyway, so the middle of the view can keep tracing.
        //
        // Plain raster survives only as a final fallback for a device with no
        // variable-rate shading, chosen at runtime below.
        // Measured, after an estimate said otherwise.
        //
        // Rays are NOT linear in cost, which a previous version of this ladder
        // assumed: extrapolating from four rays put eleven at about 6.7 ms, and
        // measured at the same distance it is 11.09 -- the whole 11.11 ms
        // budget, with nothing left for the compositor. Eleven foveated came in
        // at 8.84 and was still paced down to 45 Hz. Four rays foveated hard
        // measured 5.53 and held 90 Hz solidly, and is the configuration that
        // reads as most stable through the lenses.
        //
        // So the usable ceiling is around half the nominal budget, not all of
        // it, and the extra taps buy less than they cost -- consistent with the
        // shader's own note that nine shadow taps against five is visible only
        // if you go looking. Foveation is applied throughout instead, since
        // that is what bought the close range back without losing shadows.
        static const Tier kTiers[] = {
            {true, 0, 1, 1.00f, "11 rays, foveated"},
            {true, 1, 1, 1.00f, "7 rays, foveated"},
            {true, 2, 1, 1.00f, "4 rays, foveated"},
            {true, 2, 2, 1.00f, "4 rays, foveated hard"},
            // Below here the rays are already as cheap as they get, so the
            // remaining lever is RESOLUTION -- the scene target shrinks and the
            // eye blit scales it back up.
            //
            // These exist because a denser board runs out of budget where the
            // first one did not: four rays foveated hard measured 5.4 ms at
            // 0.30 m on one board and 8.9 to 11.8 ms on another, and at 11.8
            // the runtime paced us to 45 Hz with the ladder already at its
            // bottom rung and nowhere left to go. Dropping shadows would fix
            // that too, and is exactly the thing that was worth removing.
            // Softer pixels are the better trade.
            {true, 2, 2, 0.80f, "4 rays, 0.80x"},
            {true, 2, 2, 0.65f, "4 rays, 0.65x"},
        };
        static const Tier kNoFoveation = {false, 2, 0, 1.00f, "plain raster"};
        static const int kLast = static_cast<int>(std::size(kTiers)) - 1;
        // Explicitly asking for a ray budget pins it -- an override that a
        // controller quietly walks away from is not an override.
        static const bool adapt = [] {
            const QByteArray a = qgetenv("PCBVIEW_VR_ADAPT");
            if (a == "0" || a == "false") return false;
            // The sweep drives quality itself; two controllers on one dial
            // would each be measuring the other.
            const QByteArray sw = qgetenv("PCBVIEW_VR_SWEEP");
            if (!sw.isEmpty() && sw != "0") return false;
            return qgetenv("PCBVIEW_VR_RAYQ").isEmpty();
        }();
        static int tierNow = 0;
        static int pacingTier = 0;
        static int over = 0, under = 0, dwell = 0;
        static float distSmooth = 0.0f;
        static int tierHold = 0;

        // ---- measured cost model, in place of tuned distance thresholds ----
        //
        // The ladder below works, and every number in it was measured -- on ONE
        // board, on ONE GPU. That is the problem. Distance is only ever a proxy
        // for how much of the eye the board covers, and it cannot know how big
        // the board is: thresholds tuned on a 191 mm board make a 50 mm board
        // drop to 0.65x at 0.22 m while the frame costs 2.5 ms of 11.1. It
        // throws away resolution it has no reason to.
        //
        // So price the frame instead. Two things are now measured directly:
        // the GPU milliseconds (timestamps) and the fragments actually shaded
        // (a pipeline-statistics query). The second folds in coverage, shading
        // rate, hidden-area mask and depth rejection at once -- everything a
        // distance threshold was standing in for.
        //
        //     ms  ~=  base + k[rays] * fragments
        //
        // base and k are fitted from the frames as they go by, so they are this
        // board on this GPU at this moment, not a constant from a session in
        // July. k is per ray count rather than a multiplier on one coefficient
        // because rays are measurably NOT linear: extrapolating from four rays
        // put eleven at 6.7 ms and eleven measured 11.09.
        //
        // To price a configuration we have not run, scale the fragments we DID
        // measure: resolution is exactly quadratic, and the shading rate's
        // effect is computed from the same radial pattern the rate image is
        // built from, integrated over the disc the board actually covers. Then
        // pick the nicest configuration that fits.
        //
        // PCBVIEW_VR_ADAPT=ladder goes back to the distance thresholds, so the
        // two can be compared in the headset rather than argued about.
        static const bool useModel = [] {
            const QByteArray a = qgetenv("PCBVIEW_VR_ADAPT");
            return !(a == "ladder" || a == "distance");
        }();

        // The knobs, and how much each is worth giving up.
        //
        // Ordering is a judgement a cost model cannot make: it knows what
        // things cost, not which of two equally affordable pictures looks
        // better. This encodes the one the ladder already embodied -- give up
        // rays first, then peripheral sharpness, and resolution last -- which
        // is also what was asked for: shadows kept when close, and the sharp
        // image protected, because softening the whole frame is what made
        // traces wiggle.
        //
        // Foveation level 0 (none) is deliberately not a candidate. The ladder
        // never used it either: it coarsens only the periphery, which the lens
        // is blurring anyway, so paying full rate out there buys nothing.
        struct Cand {
            int rayQ;    // 0 = 11 rays, 1 = 7, 2 = 4
            int fov;     // 1 = gentle, 2 = hard
            float ss;    // scene size as a fraction of the eye
            int score;   // higher is nicer to look at
        };
        static const std::vector<Cand> kCands = [] {
            const float kSs[3] = {0.65f, 0.80f, 1.00f};
            std::vector<Cand> v;
            for (int r = 0; r < 3; ++r)
                for (int f = 1; f <= 2; ++f)
                    for (int s = 0; s < 3; ++s)
                        v.push_back({r, f, kSs[s],
                                     100 * s + 10 * (2 - f) + (2 - r)});
            std::sort(v.begin(), v.end(),
                      [](const Cand& a, const Cand& b) {
                          return a.score > b.score;
                      });
            return v;
        }();

        // Mean fragments shaded per pixel for a foveation level, over the disc
        // the board covers. The radii are the ones buildShadingRateImage uses,
        // in the same units (fraction of half the SHORTER axis), and a 2x2 tile
        // shades one fragment for four pixels, a 4x4 one for sixteen.
        //
        // Integrating over the BOARD's disc rather than the whole view is the
        // point. A board filling the middle of the eye sits almost entirely in
        // the sharp zone, so switching to hard foveation saves it almost
        // nothing -- while a whole-view average would promise a large saving
        // and be wrong exactly when the viewer is closest.
        auto fovFactor = [](int level, float r) {
            if (level <= 0) return 1.0f;
            const float sharp = level == 1 ? 0.55f : 0.38f;
            const float mid = level == 1 ? 0.80f : 0.62f;
            const float R = std::clamp(r, 0.02f, 1.42f);
            const float R2 = R * R;
            const float a1 = std::min(R, sharp) * std::min(R, sharp);
            const float a2 =
                std::max(0.0f, std::min(R, mid) * std::min(R, mid) -
                                   std::min(R, sharp) * std::min(R, sharp));
            const float a3 = std::max(0.0f, R2 - std::min(R, mid) * std::min(R, mid));
            return (a1 + 0.25f * a2 + 0.0625f * a3) / R2;
        };

        // Fitted online. base is the frame's fixed cost -- everything that is
        // not a shaded fragment -- tracked as a slowly recovering floor, since
        // the cheapest frames observed are the ones with almost no board in
        // view. k starts at zero meaning "not yet known", and until every level
        // the chooser wants has a value the distance ladder is left in charge.
        // A LEAST-SQUARES FIT of milliseconds against fragments, per ray level.
        // Both the slope and the intercept are fitted; neither is assumed.
        //
        // The intercept was previously taken to be the cheapest frame observed,
        // on the reasoning that the cheapest frames have almost no board in
        // view. They do not. The viewer is looking AT the board, so even the
        // cheapest frame still shades a great many fragments -- the minimum is
        // a point ON the line, not the line's intercept. Reading it as the
        // intercept charged several milliseconds of genuine per-fragment work
        // to a constant: the log showed base at 3 to 5 ms against a 6.1 ms
        // target, leaving about one millisecond of apparent headroom, which
        // pinned the chooser to the bottom rung the moment it engaged. The
        // chooser was not at fault; the arithmetic beneath it was.
        //
        // Coverage varies constantly as the viewer moves, so the fragment count
        // spans a wide range on its own and the regression is well conditioned
        // without having to perturb anything deliberately. Accumulators decay
        // so the fit follows the board and the view rather than averaging the
        // whole session.
        // ONE fit, over fragments TIMES rays -- not one per ray level.
        //
        // Three independent lines was the mistake, and the log showed exactly
        // how it fails. Each level is only ever fitted on the frames it happens
        // to run during, so k[0] was learned while the viewer was far away and
        // everything was cheap while k[2] was learned up close where it was
        // not. The regression cannot tell "this configuration is cheaper" from
        // "the view got easier", so the three lines drift apart on view history
        // alone. One session ended at k=[0.06 0.32 0.46]: eleven rays priced
        // seven times cheaper per fragment than four, which is impossible, and
        // a chooser fed that will thrash forever.
        //
        // Rays are very nearly linear in cost -- a clean session measured
        // [1.89 1.35 0.74] for 11, 7 and 4 rays, ratios of 2.55 and 1.82
        // against ray-count ratios of 2.75 and 1.75 -- so folding the ray count
        // into the regressor gives one well-conditioned line that every frame
        // contributes to, whatever configuration it ran. It cannot invert,
        // because there is nothing left to invert against.
        struct Fit {
            double n = 0.0, x = 0.0, y = 0.0, xx = 0.0, xy = 0.0;
            double k = 0.0, b = 0.0;   // ms per (Mfrag * ray), ms fixed
            bool known = false;
        };
        static Fit fit;
        static const double kRayCount[3] = {11.0, 7.0, 4.0};
        // How badly the fit has recently UNDER-predicted, as a multiplier on
        // every prediction.
        //
        // A single line cannot describe both sides of the board. The reported
        // symptom is the proof: the shadowed side never glitches. Same rays
        // either way -- the shader fires the centre shadow ray and all AO taps
        // regardless -- but on the shadowed side those rays meet the board slab
        // within a few BVH nodes, while on the lit side they fly into open
        // space and traverse the entire structure before concluding nothing was
        // hit. Neptune's is 8.65M vertices against cx4's 1.24M, which is why
        // the gap is so much wider there.
        //
        // Fitting one slope through both regimes averages them, so the model
        // overcommits the moment the viewer turns to the expensive one. Instead
        // of trying to model orientation, watch the residual: if reality has
        // recently cost 1.6x what was predicted, inflate predictions by 1.6x
        // until that memory decays. Instantly up, forgetting over about half a
        // minute, and it needs to know nothing about why.
        static double safety = 1.0;
        static int stableFrames = 0;
        static int modelHold = 0;
        static int modelRayQ = 2, modelFov = 2;
        static float modelSs = 1.0f;
        static bool modelReady = false;
        // What the frames now being measured were actually drawn with. The
        // measurement arrives two slots late, so this is the only honest thing
        // to credit a sample to.
        static int appliedRayQ = 2;
        // The score last stepped DOWN from, and how long it stays out of
        // bounds. Stops the chooser climbing back into a setting it has just
        // established does not fit.
        static int rejectedScore = 1000;
        static int rejectCooldown = 0;
        // Consecutive evaluations on which the CURRENT setting fitted. Reaching
        // for something richer needs a run of these, not one lucky frame.
        static int comfortable = 0;
        // Whether the model is the one choosing. The distance ladder still runs
        // underneath -- it seeds the fit and covers devices that cannot count
        // fragments -- but once the model takes over, the ladder's own
        // announcements describe a decision nothing acts on, so they are
        // silenced rather than left to litter the log with fictional changes.
        static bool modelDriving = false;

        if (adapt && !allowPt) {
            const double budget = vr_->nativeFrameMs();

            // Smoothed distance, because the input is a head and heads are
            // never still. The raw value swings several centimetres frame to
            // frame just from breathing and micro-motion, and a threshold
            // reads every one of those as an intention to move.
            const float raw = vr_->boardDistance();
            distSmooth = distSmooth <= 0.0f ? raw
                                            : distSmooth * 0.94f + raw * 0.06f;
            const float d = distSmooth;

            // Coarser as soon as the board crosses a threshold; finer only
            // once it is well clear of it again.
            //
            // 15% was not nearly enough. At the 0.30 m step that is a 4.5 cm
            // band, and leaning gently in and out crossed it about twenty
            // times in one session -- and the 4-rays-to-plain-raster step
            // removes every shadow in the scene, so that is the most visible
            // transition of the lot, flickering. 40%, smoothing, and a hold
            // after each change together make a move mean the viewer actually
            // went somewhere.
            // One threshold per rung, all the way to the bottom.
            //
            // There used to be three, for six rungs. The bottom two were
            // reachable only through the pacing backstop, which needs 45
            // consecutive late frames -- half a second of missing frames every
            // single time you lean in. The runtime does not wait that long: it
            // halves the target to 45 Hz, then 30, then 23, and everything it
            // then has to fill in is a reprojection. That IS the swimming. It
            // happens whether or not we submit depth (measured both ways), and
            // it has to, because no warp of a stale frame can invent the
            // parallax of a surface 100 mm from your face.
            //
            // So distance drives every rung now, and the numbers come from the
            // 191 mm board, which is the one that runs out of budget. Measured
            // there, converted to a common rung by the resolution ratio:
            //
            //   4 rays foveated       0.30 m  10.9 ms
            //   4 rays foveated hard  0.11 m  17.4 ms      (0.21 m ~14.8)
            //   4 rays 0.80x          0.21 m   9.5 ms, 0.13 m 10.9 ms
            //   4 rays 0.65x          0.47 m   3.1 ms
            //
            // Cost stops climbing below about 0.13 m, where the board covers
            // the whole view and there is nothing left to add. The usable
            // ceiling is around half the 11.11 ms budget, since the
            // compositor's share is real and invisible to us -- so each rung is
            // placed where the one above it passes ~7 ms, not where it passes
            // 11.
            const float kStep[5] = {1.00f, 0.60f, 0.45f, 0.32f, 0.22f};
            int distTier = 0;
            for (int i = 0; i < 5; ++i) {
                const float t = (tierNow > i) ? kStep[i] * 1.40f : kStep[i];
                if (d < t) distTier = i + 1;
            }

            // Pacing, steered on what the runtime actually decided rather
            // than on a cost model.
            //
            // A budget threshold on our own GPU time cannot work on its own:
            // 8.88 ms at full resolution was paced down to 45 Hz while 8.62 ms
            // at half resolution held 90. Nearly identical app cost, opposite
            // outcomes -- because the compositor's share of the frame grows
            // with the swapchain, and we cannot see that number. Being
            // throttled is the ground truth, so use it.
            //
            // Stepping back up needs the opposite evidence and much more of
            // it: at native pace AND comfortably inside budget. Being paced at
            // native proves nothing on its own once throttled, since we hit a
            // slower target easily -- that asymmetry is why the counters
            // differ by a factor of four.
            //
            // Two step-down signals, not one. Being throttled is the ground
            // truth but it ARRIVES LATE -- by then the viewer has already
            // watched the image swim. Our own GPU time exceeding the entire
            // frame budget is a safe leading indicator: the compositor's share
            // is invisible to us, but it is never negative, so if we alone
            // have spent the whole budget we are certainly too slow. That is
            // the case a denser board hits, and it is worth catching before
            // the runtime reacts rather than after.
            const bool throttled = vr_->pacedFrameMs() > budget * 1.5;
            const bool overBudget = frameGpuMs > budget;
            if (throttled || overBudget) {
                ++over;
                under = 0;
            } else if (frameGpuMs > 0.0 && frameGpuMs < budget * 0.35) {
                // 0.35 of budget, not 0.5, and the difference is the cost of
                // the step ITSELF.
                //
                // Climbing a rung multiplies the frame cost -- 0.80x back to
                // full resolution is 1/0.64, about 1.56x. Measuring 4.9 ms at
                // 0.80x satisfies "under half the budget" and then lands at
                // roughly 9 ms, which fits until the smallest lean forward
                // pushes it over and the rung drops again. That is the pumping
                // in the log: fourteen changes in one session, several of them
                // resolution, which is the most visible kind. Leaving room for
                // the step's own increase is what stops it.
                ++under;
                over = 0;
            } else {
                over = under = 0;
            }
            if (dwell > 0) --dwell;
            if (!dwell && over >= 45 && pacingTier < kLast) {
                ++pacingTier;
                over = 0;
                dwell = 120;
            } else if (!dwell && under >= 360 && pacingTier > 0) {
                --pacingTier;
                under = 0;
                dwell = 120;
            }

            // The coarser of the two. Distance is the fast, predictive one --
            // it acts before the frames are missed. Pacing is the corrective
            // one, and it only ever makes things cheaper than distance alone
            // would, never richer.
            //
            // A minimum hold between changes, whichever input asked for it.
            // Dropping a rung is allowed to jump straight to wherever it needs
            // to be, so walking right up to the board is not a staircase of
            // visible steps -- but having moved, it stays put for a moment.
            if (tierHold > 0) --tierHold;
            const int want = std::max(distTier, pacingTier);
            if (want != tierNow && tierHold == 0) {
                // Two seconds, not one. A resolution change re-sharpens or
                // softens the entire image, which is far more noticeable than
                // a shadow getting a little coarser, and the rungs that move
                // it are the ones the pacing backstop reaches.
                tierHold = 180;
                if (!modelDriving) {
                    std::printf("vr-adapt: %s -> %s  (%.2f m, %.1f ms of %.1f, "
                                "paced %.0f Hz)\n",
                                kTiers[tierNow].name, kTiers[want].name, d,
                                frameGpuMs, budget, 1000.0 / vr_->pacedFrameMs());
                    std::fflush(stdout);
                }
                tierNow = want;
            }
            // Without variable-rate shading the bottom rung cannot buy its
            // rays back, so it falls back to plain raster as before.
            const Tier& t = (kTiers[tierNow].fov > 0 &&
                             !renderer_->foveationAvailable())
                                ? kNoFoveation
                                : kTiers[tierNow];
            rtEff = t.rt;
            rayQEff = t.rayQ;
            fovEff = t.fov;
            ssEff = ss * t.ss;

            // ---- and now the measured model, which overrides the above once
            // it knows enough to. ----
            if (useModel && renderer_->fragCountingAvailable() &&
                renderer_->foveationAvailable()) {
                // Only learn from a frame whose configuration was already
                // settled. Two frames are in flight, so a measurement arriving
                // now describes a frame recorded two slots ago -- crediting it
                // to the settings in force at this instant would fit the model
                // to the wrong picture every time the quality changed.
                ++stableFrames;
                if (frameGpuMs > 0.0 && frameFragInv > 10000.0 &&
                    stableFrames > 4) {
                    // Credited to the settings the measured frame was drawn
                    // with -- appliedRayQ -- and NOT to rayQEff, which at this
                    // point still holds what the distance ladder would have
                    // chosen. Getting that wrong charged every 7- and 11-ray
                    // frame to the 4-ray coefficient, so k[0] and k[1] stayed
                    // at zero forever and k[2] swung over a twelvefold range.
                    const int q = std::clamp(appliedRayQ, 0, 2);
                    Fit& f = fit;
                    // Fragments in millions TIMES the ray count they each fired
                    // -- the actual unit of work. Millions so the accumulators
                    // stay in a range where the normal equations are
                    // numerically sane; squaring two million raw would not be.
                    const double x = frameFragInv * 1.0e-6 * kRayCount[q];
                    const double y = frameGpuMs;
                    const double decay = 0.999;
                    f.n = f.n * decay + 1.0;
                    f.x = f.x * decay + x;
                    f.y = f.y * decay + y;
                    f.xx = f.xx * decay + x * x;
                    f.xy = f.xy * decay + x * y;
                    // Ordinary least squares. The denominator is n times the
                    // variance of the fragment count, so it goes to zero
                    // exactly when the viewer has held still and there is
                    // nothing to learn -- which is when the previous fit is
                    // still the best answer available, so keep it.
                    const double den = f.n * f.xx - f.x * f.x;
                    // Refuse to refit when the workload has not MOVED.
                    //
                    // Slope and intercept trade off against each other, and the
                    // only thing that separates them is variation in x. Hold
                    // still and the fragment count barely changes, so the fit
                    // slides freely along that trade-off and lands anywhere:
                    // the log showed k=[0.02 0.01 0.01] with fixed at 3.65 ms
                    // and again at 5.25 ms -- the constant swallowing the whole
                    // frame while fragments came out nearly free. An absolute
                    // floor on the determinant cannot catch that, because with
                    // a thousand samples it passes on noise alone.
                    //
                    // Require the spread to be a real FRACTION of the mean
                    // instead. Below that the previous fit is the better
                    // answer, and there is nothing to learn from standing
                    // still anyway.
                    const double meanX = f.x / std::max(f.n, 1.0);
                    const double varX = f.xx / std::max(f.n, 1.0) - meanX * meanX;
                    const double spread =
                        meanX > 1.0e-6 ? std::sqrt(std::max(varX, 0.0)) / meanX
                                       : 0.0;
                    if (f.n > 30.0 && den > 1.0e-6 && spread > 0.12) {
                        const double k = (f.n * f.xy - f.x * f.y) / den;
                        const double b = (f.y - k * f.x) / f.n;
                        // A negative slope or intercept is the fit telling us
                        // the data was degenerate this instant, not that rays
                        // are free. Clamp rather than believe it.
                        //
                        // The intercept ceiling is 2.5 ms and that is a
                        // statement about the renderer, not a fudge: the
                        // non-fragment work is the visibility mask, the eye
                        // blit, bloom and the per-eye fixed cost. It is not
                        // five milliseconds. Allowing it to be lets it eat the
                        // budget and leaves the chooser pricing every
                        // configuration identically.
                        f.k = std::clamp(k, 0.0, 100.0);
                        f.b = std::clamp(b, 0.0, 2.5);
                        f.known = true;
                    }
                }

                // The board's projected radius, in the same units the rate
                // pattern uses: a fraction of half the shorter axis. This is
                // the "how much of the eye" that distance was approximating,
                // and unlike distance it knows how big the board is.
                const float sizeM = vr_->boardSizeMetres();
                const float tanHalf = std::tan(vr_->eyeFovHalfY());
                const float coverR =
                    (d > 0.01f && tanHalf > 0.01f)
                        ? std::clamp((sizeM * 0.5f) / d / tanHalf, 0.02f, 1.42f)
                        : 0.3f;

                // A ray count that has not been run yet still needs a price,
                // or the model would rate it as free and jump straight to it.
                //
                // Seeded from whichever level HAS been measured, scaled by ray
                // count with a deliberately pessimistic exponent. Rays measured
                // worse than linear -- extrapolating four to eleven linearly
                // predicted 6.7 ms against an actual 11.09 -- so 1.3 errs
                // towards "that will be more expensive than you think", which
                // costs a little quality and never a dropped frame. The moment
                // that level actually runs, measurement replaces the guess.
                modelReady = fit.known;

                if (modelReady && frameFragInv > 10000.0) {
                    // Half the nominal budget, not all of it. The compositor's
                    // share of a frame is real, grows with the swapchain, and
                    // is invisible to us -- measured repeatedly as the point
                    // where the runtime starts pacing us down.
                    const double target = budget * 0.55;
                    const float fovNow = fovFactor(fovEff, coverR);
                    const float ssNow = std::max(0.01f, ssEff);

                    auto predict = [&](const Cand& c) {
                        const double fragScale =
                            double(c.ss * c.ss) / double(ssNow * ssNow) *
                            double(fovFactor(c.fov, coverR)) / double(fovNow);
                        return safety *
                               (fit.b + fit.k * frameFragInv * 1.0e-6 *
                                            fragScale * kRayCount[c.rayQ]);
                    };

                    // Update the safety multiplier from how the CURRENT setting
                    // actually turned out. Predicting the configuration we are
                    // already running is the one prediction that can be checked
                    // against a measurement, so it is the one that calibrates
                    // everything else.
                    {
                        const double pred =
                            fit.b + fit.k * frameFragInv * 1.0e-6 *
                                        kRayCount[std::clamp(appliedRayQ, 0, 2)];
                        if (pred > 0.05 && frameGpuMs > 0.0) {
                            const double ratio = frameGpuMs / pred;
                            safety = std::clamp(
                                std::max(ratio, safety * 0.9995), 1.0, 4.0);
                        }
                    }

                    const int scoreNow = 100 * (modelSs > 0.9f    ? 2
                                                : modelSs > 0.72f ? 1
                                                                  : 0) +
                                         10 * (2 - modelFov) + (2 - modelRayQ);

                    // IF WHAT IS RUNNING STILL FITS, LEAVE IT ALONE.
                    //
                    // This is the change that matters. The chooser used to pick
                    // the best-scoring affordable candidate on every evaluation,
                    // so it re-ran the whole contest constantly -- and since the
                    // prediction moves with the fit, the safety multiplier and
                    // the viewer's head, the winner changed whenever any of
                    // those wobbled, even though nothing was wrong with what was
                    // already on screen. The log was dozens of vr-model lines a
                    // session, frequently two inside one 240-frame window, each
                    // one a visible change and often a scene-target rebuild.
                    //
                    // A controller should act on a PROBLEM, not on a
                    // recalculation. So: step down only when the current
                    // configuration no longer fits, and step up only when a
                    // better one fits with room to spare AND the current one has
                    // been comfortable for a while. Holding is the default and
                    // costs nothing.
                    const Cand curCand{modelRayQ, modelFov, modelSs, scoreNow};
                    const double curPred = predict(curCand);
                    const bool curFits = curPred <= target;
                    if (curFits) ++comfortable; else comfortable = 0;

                    int pick = -1;
                    if (rejectCooldown > 0) --rejectCooldown;
                    // Reaching for MORE requires the current setting to have
                    // been comfortable for a while, not merely comfortable this
                    // instant -- one cheap frame is not evidence of headroom.
                    const bool considerAny = !curFits || comfortable > 240;
                    for (size_t i = 0; considerAny && i < kCands.size(); ++i) {
                        // Never consider anything WORSE than what is running
                        // while it still fits, and never anything better while
                        // it does not.
                        if (curFits && kCands[i].score <= scoreNow) break;
                        if (!curFits && kCands[i].score >= scoreNow) continue;
                        // Do not climb straight back to something just
                        // abandoned. The log showed 4 rays to 7 and back within
                        // two decisions: nothing remembered that the richer
                        // setting had already been tried and found wanting, so
                        // the smallest wobble in the fit re-proposed it. This
                        // is what a hysteresis band cannot express, because the
                        // two configurations are not adjacent on one axis.
                        if (rejectCooldown > 0 && kCands[i].score >= rejectedScore)
                            continue;
                        // Improving the picture has to clear a wider bar than
                        // holding it. Stepping up multiplies the frame cost, so
                        // a candidate that only just fits at today's price will
                        // not fit once it is running -- that asymmetry is what
                        // stops the ladder pumping between two rungs.
                        const bool up = kCands[i].score > scoreNow;
                        // Raising RESOLUTION is held to a tighter bar than any
                        // other improvement. It multiplies the cost the most,
                        // it is the most visible change, and it is the only one
                        // that still drags a scene-target rebuild behind it --
                        // so a resolution rung that turns out not to fit is
                        // paid for twice, once going up and once coming back.
                        const bool resUp = kCands[i].ss > modelSs + 0.01f;
                        const double limit = resUp   ? target * 0.60
                                             : up    ? target * 0.75
                                                     : target;
                        if (predict(kCands[i]) <= limit) {
                            pick = static_cast<int>(i);
                            break;
                        }
                    }

                    if (modelHold > 0) --modelHold;
                    if (pick >= 0) {
                        const Cand& c = kCands[pick];
                        const bool changed = c.rayQ != modelRayQ ||
                                             c.fov != modelFov ||
                                             std::fabs(c.ss - modelSs) > 0.01f;
                        if (changed && modelHold == 0) {
                            // A resolution change resamples the entire image
                            // and still costs a scene-target rebuild, so it
                            // gets a longer hold than a shadow getting coarser.
                            const bool resMoved =
                                std::fabs(c.ss - modelSs) > 0.01f;
                            modelHold = resMoved ? 240 : 60;
                            // Stepping DOWN records what did not fit, so the
                            // chooser does not climb straight back into it.
                            if (c.score < scoreNow) {
                                rejectedScore = scoreNow;
                                rejectCooldown = 900;  // ten seconds
                            }
                            std::printf(
                                "vr-model: %d rays fov%d %.2fx -> %d rays fov%d "
                                "%.2fx | cover %.2f, pred %.1f ms of %.1f, "
                                "k=[%.2f %.2f %.2f] ms/Mfrag, fixed %.2f ms, "
                                "safety %.2fx\n",
                                modelRayQ == 0 ? 11 : (modelRayQ == 1 ? 7 : 4),
                                modelFov, modelSs,
                                c.rayQ == 0 ? 11 : (c.rayQ == 1 ? 7 : 4), c.fov,
                                c.ss, coverR, predict(c), target,
                                fit.k * kRayCount[0], fit.k * kRayCount[1],
                                fit.k * kRayCount[2], fit.b, safety);
                            std::fflush(stdout);
                            modelRayQ = c.rayQ;
                            modelFov = c.fov;
                            modelSs = c.ss;
                            stableFrames = 0;
                        }
                    }

                    rtEff = true;
                    rayQEff = modelRayQ;
                    fovEff = modelFov;
                    ssEff = ss * modelSs;
                }
            }
            // Whatever ended up in force, model or ladder. Read at the top of a
            // later frame to price the measurement that frame produces.
            appliedRayQ = rayQEff;
            modelDriving = useModel && modelReady;
        }

        // --- Measured sweep -------------------------------------------------
        //
        // PCBVIEW_VR_SWEEP=1 walks the two fill-rate levers and reports pacing
        // for each, so they get chosen from numbers instead of impressions.
        // Both are per-frame settable: ray tracing is a renderer flag, and the
        // render scale is the scene target's size, which is decoupled from the
        // swapchain and blitted to it. The swapchain itself cannot change
        // mid-session, which is why this sweeps the render scale and not
        // PCBVIEW_VR_RES.
        //
        // Hold the board in view and stay roughly still for the run: the whole
        // point is that fill cost tracks how much of the eye the board covers,
        // so wandering makes the rows incomparable.
        struct SweepStep {
            const char* name;
            bool rt;
            float ss;
            int rayQ;
            int fov;
        };
        // The ray budget, at fixed full resolution.
        //
        // The first sweep already answered the two coarse questions: ray
        // tracing costs two thirds of the frame rate (90 Hz plain against 30 Hz
        // traced) and render scale barely matters (0.70x recovered only half of
        // it). So the cost is the rays behind each pixel rather than the pixel
        // count, and this walks that dial instead. Plain raster stays as the
        // last row: it is the ceiling everything else is measured against.
        // The ladder's own rungs, so "is there headroom for more rays at the
        // distance I actually work at" is answered by measurement rather than
        // by extrapolating a cost model. The first row is the richest thing
        // the shader can do; if it fits at working distance, the ladder can
        // simply hand it over sooner.
        static const SweepStep kSweep[] = {
            {"11 rays, no foveation  ", true, 1.00f, 0, 0},
            {"11 rays, foveated      ", true, 1.00f, 0, 1},
            {"7 rays, foveated       ", true, 1.00f, 1, 1},
            {"4 rays, foveated hard  ", true, 1.00f, 2, 2},
        };
        static const bool sweeping = [this] {
            const QByteArray v = qgetenv("PCBVIEW_VR_SWEEP");
            const bool on = !v.isEmpty() && v != "0";
            if (on) {
                vr_->setRateAutoReport(false);
                std::printf(
                    "\nvr-sweep: %zu configurations. Press SPACE (blind, with "
                    "the headset on) when you have seen enough of one.\n"
                    "vr-sweep: You control the boundaries, so what you say "
                    "about each row can be trusted to be about THAT row --\n"
                    "vr-sweep: the timed version gave rows you could not tell "
                    "apart, at wildly different durations.\n"
                    "vr-sweep: GPU ms is the measurement; 'dist' is the mean "
                    "board distance, since fill cost follows coverage.\n",
                    std::size(kSweep));
                std::fflush(stdout);
            }
            return on;
        }();
        static size_t sweepStep = 0;
        static int sweepFrame = 0;
        static int sweepIdle = 0;
        static bool sweepDone = false;
        if (sweeping) {
            // Clamped, because on the last SPACE sweepStep runs one past the
            // end. Without this the settings fell back to the environment the
            // moment the sweep finished -- so "holding the last configuration"
            // was a lie, and the vr-rate lines after a sweep were reporting
            // the env config while appearing to describe the final row.
            const size_t i = sweepStep < std::size(kSweep)
                                 ? sweepStep
                                 : std::size(kSweep) - 1;
            ssEff = kSweep[i].ss;
            rtEff = kSweep[i].rt;
            rayQEff = kSweep[i].rayQ;
            fovEff = kSweep[i].fov;
        }
        // Count only frames the runtime actually asked us to render, so an
        // idle headset stalls the sweep instead of walking through every
        // configuration measuring nothing and reporting tidy zeroes.
        if (sweeping && !sweepDone && !render) {
            if ((++sweepIdle % 300) == 1) {
                std::printf("vr-sweep: waiting -- the runtime is not asking "
                            "for frames. Put the headset ON and look at the "
                            "board; nothing is being measured until then.\n");
                std::fflush(stdout);
            }
        }
        if (sweeping && !sweepDone && render) {
            sweepIdle = 0;
            ++sweepFrame;
            // Recover, then settle, then measure.
            //
            // The recovery phase runs plain raster before every row, and it is
            // not politeness -- the runtime's pacing is a control loop with
            // hysteresis. Coming straight off an expensive configuration it
            // stays throttled into the next one, so a cheap row measured after
            // a dear one inherits its penalty and reads as worse than it is.
            // That is what made the first ray-budget sweep report 7 rays as
            // slower than 11. Letting it climb back to full rate first makes
            // each row independent of the order they were tried in, and gives
            // an unmistakable visual boundary between them besides.
            // 150 frames of recovery was not enough. Row 1 measured 11.09 ms,
            // right at the budget, and the row after it -- cheaper at 8.84 --
            // came back paced at 45 Hz, having inherited the throttle. Five
            // seconds gives the runtime time to actually climb back before the
            // next row is judged on it.
            const int kRecover = 450, kSettle = 90;
            const int kMinFrames = kRecover + kSettle + 60;
            if (sweepFrame <= kRecover) {
                rtEff = false;
                rayQEff = 2;
                ssEff = 1.0f;
                fovEff = 0;
            } else if (sweepFrame == kRecover + 1) {
                std::printf("\nvr-sweep: [%zu/%zu] NOW SHOWING: %s\n"
                            "vr-sweep: press SPACE when you have seen enough.\n",
                            sweepStep + 1, std::size(kSweep),
                            kSweep[sweepStep].name);
                std::fflush(stdout);
            } else if (sweepFrame == kRecover + kSettle) {
                vr_->takeRate();
                sweepGpuMs_ = 0.0;
                sweepDist_ = 0.0;
                sweepSamples_ = 0;
            } else if (sweepFrame > kRecover + kSettle) {
                if (frameGpuMs > 0.0) {
                    sweepGpuMs_ += frameGpuMs;
                    sweepDist_ += vr_->boardDistance();
                    ++sweepSamples_;
                }
            }
            // Ignore SPACE until there is something to report, so a stray
            // press cannot skip a row before it has been measured.
            if (vrSweepAdvance_ && sweepFrame >= kMinFrames) {
                vrSweepAdvance_ = false;
                const auto st = vr_->takeRate();
                const double n = sweepSamples_ ? sweepSamples_ : 1;
                std::printf("vr-sweep: [%zu/%zu] %-28s %7.2f ms GPU  %.2f m  "
                            "paced %.0f Hz\n",
                            sweepStep + 1, std::size(kSweep),
                            kSweep[sweepStep].name, sweepGpuMs_ / n,
                            sweepDist_ / n, st.hz);
                std::fflush(stdout);
                sweepFrame = 0;
                if (++sweepStep >= std::size(kSweep)) {
                    sweepDone = true;
                    std::printf(
                        "\nvr-sweep: done -- GPU ms is the measurement. The "
                        "paced Hz lags it by about a step and is context "
                        "only.\nvr-sweep: holding the last configuration.\n\n");
                    std::fflush(stdout);
                    vr_->setRateAutoReport(true);
                }
            } else if (vrSweepAdvance_) {
                vrSweepAdvance_ = false;   // too early; swallow it
            }
        }

        // The override goes here, after every chooser has had its say, so
        // nothing downstream can quietly turn the rays back on.
        if (rtEnv >= 0) rtEff = rtEnv != 0;

        // Say what is actually running, once.
        //
        // Every one of these is an environment variable, and an environment
        // variable set in a shell OUTLIVES the run that set it. Testing a
        // path-traced build and then launching again without it leaves path
        // tracing on, which reads as a mysteriously slow, mysteriously fuzzy
        // "raster" mode. Printing the configuration costs one line and removes
        // the whole class of confusion.
        static bool cfgDumped = false;
        if (!cfgDumped) {
            cfgDumped = true;
            const bool rtOn =
                !allowPt && renderer_->rayTracingSupported() && rtEff;
            std::printf("vr-cfg: %s, spp=%d, supersample=%.2fx, denoise=%d, "
                        "rate=1/%d, world-sun=yes\n",
                        allowPt ? "PATH TRACED (PCBVIEW_VR_PT)"
                                : (rtOn ? "ray-traced raster"
                                        : "PLAIN RASTER (no rays)"),
                        allowPt ? spp : 0, ssEff, allowPt ? dnPasses : 0,
                        rateDiv);
            std::fflush(stdout);
        }
        // Held every frame: initialisation applies the saved ptEnabled_ after
        // the session is created and would otherwise overwrite this, and the
        // render-mode menu can change it mid-session. setRenderMode returns
        // immediately when the mode already matches, so this is free.
        if (!allowPt) {
            renderer_->setRenderMode(vk::RenderMode::Raster);
            renderer_->setRayTracing(rtEff && rtAvailable());
            renderer_->setRayQuality(rayQEff);
            renderer_->setFoveation(fovEnv >= 0 ? fovEnv : fovEff);
        }
        // A sun that stays put in the room. The desktop's key light rides the
        // camera, which is right when you orbit with a mouse and wrong the
        // moment the camera is your head: every shadow would swing as you
        // looked around, and the board would never read as an object standing
        // in a lit room.
        renderer_->setRasterWorldSun(true);
        // Bound the sun shadow ray in the headset.
        //
        // Measured: with rays off the headset holds 90 Hz with zero late frames
        // right down to 0.08 m on the dense board; with them on it paces to
        // 45 Hz by 0.20 m. An unbounded ray from a lit fragment is the reason
        // -- nothing to hit, so it traverses the whole structure to find that
        // out. The desktop keeps its unbounded shadows, where there is budget
        // for them and the camera-relative rig is used anyway.
        //
        // Default 3, so 24 mm: taller than any component on a board and far
        // shorter than a walk across it. PCBVIEW_VR_SHADOW_MM overrides in
        // millimetres, rounded to the 8 mm the packing allows, so the visual
        // trade can be judged in the headset rather than argued about.
        static const int shadowIdx = [] {
            bool ok = false;
            const int mm = qgetenv("PCBVIEW_VR_SHADOW_MM").toInt(&ok);
            if (!ok) return 3;
            if (mm <= 0) return 0;  // explicit 0 = unbounded, as before
            return std::clamp((mm + 4) / 8, 1, 9);
        }();
        renderer_->setShadowRangeIndex(shadowIdx);
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
    // every frame. PCBVIEW_VR_RATE_DIV (read at the top) forces a divisor.
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
            // Defaults to 1.0 -- native, no supersampling.
            //
            // 1.5x was added to fix aliased edges and costs 2.25x the PIXELS,
            // which multiplies every shadow and AO ray behind each one. That is
            // a poor trade at eye resolution across two eyes: it buys smoother
            // silhouettes at more than double the frame time. Raise it only if
            // there is headroom to spare.
            renderer_->setCaptureExtent(
                static_cast<uint32_t>(std::lround(e.width * ssEff)),
                static_cast<uint32_t>(std::lround(e.height * ssEff)));
            // The path tracer ignores viewProj entirely -- it traces from a ray
            // basis, and the only place that basis was ever set is the desktop
            // render path. So traced VR was rendering the DESKTOP camera's
            // direction and zoom into both eyes, with no head tracking, while
            // the matrices this loop hands over were used by the raster path
            // alone. Set it per eye, from that eye's own frustum.
            // This eye's own history. Sharing one between the eyes would blend
            // two viewpoints together every frame.
            renderer_->setTemporalSlot(allowPt ? static_cast<int>(i) : -1,
                                       histFrames);
            renderer_->setRayCamera(e.eye, e.fwd, e.right, e.up, false);
            // Eye 0 presents, so the desktop window becomes a mirror of the
            // left eye; eye 1 renders offscreen. Presenting both made the
            // window flip between two viewpoints every frame.
            renderer_->setOffscreenOnly(i != 0);
            // Acquire BEFORE rendering so the copy into the eye rides inside
            // the frame's own command buffer. Done afterwards, as its own
            // submit, it could not know the frame had finished and had to
            // bracket itself with full device and queue waits -- four GPU
            // stalls a frame, with nothing pipelined.
            uint32_t ew = 0, eh = 0;
            const VkImage eyeImage =
                vr_->acquireEye(static_cast<int>(i), &ew, &eh);
            if (eyeImage != VK_NULL_HANDLE)
                renderer_->setVrTarget(eyeImage, ew, eh);
            // Depth for this eye, when the runtime takes it. The scene pass
            // writes into it directly, so a missed frame gets reprojected using
            // the board's actual shape rather than slid about as a flat sheet.
            renderer_->setVrDepthTarget(vr_->acquiredDepthView(),
                                        vr_->acquiredDepthImage());

            // This eye's hidden-area mesh. Uploaded on the first frame it is
            // available -- the runtime needs a located view before it can be
            // projected -- and a no-op on every frame after.
            const auto& mask = vr_->visibilityMask(static_cast<int>(i));
            if (!mask.indices.empty()) {
                renderer_->setVisibilityMask(
                    static_cast<int>(i), mask.ndc.data(),
                    static_cast<uint32_t>(mask.ndc.size() / 2),
                    mask.indices.data(),
                    static_cast<uint32_t>(mask.indices.size()), mask.version);
                renderer_->selectVisibilityMask(static_cast<int>(i));
            } else {
                // No mesh for this eye -- clear the selection rather than let
                // the other eye's mask be drawn over it.
                renderer_->selectVisibilityMask(-1);
            }
            renderer_->drawFrame(e.viewProj, e.eye);
            if (i < 2) vrSamples_[i] = renderer_->accumulatedSamples();
            if (eyeImage != VK_NULL_HANDLE)
                vr_->releaseEye(static_cast<int>(i));
        }
        // Whether the depth layer may describe what was just drawn. Both eyes
        // render at the same size, so the last eye's answer is the frame's.
        // Only set on a drawn frame: a skipped one is resubmitting images the
        // last drawn frame produced, and its depth still matches them.
        const bool depthOk = renderer_->vrDepthWritten();
        vr_->setDepthValid(depthOk);
        // And how much of the images that frame actually covers.
        vr_->setSubmitExtent(renderer_->vrSubmitWidth(),
                             renderer_->vrSubmitHeight());
        if (depthOk != vrDepthWasOk_) {
            vrDepthWasOk_ = depthOk;
            std::printf("vr-depth: %s (scene %.2fx eye)\n",
                        depthOk ? "submitted" : "WITHHELD, rigid warp instead",
                        static_cast<double>(ssEff));
            std::fflush(stdout);
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
    // Keep the zoom readout alive and fading, BEFORE any of the early returns
    // below. It has to tick even when the pad has been put down, the app has
    // lost focus or the controller has been unplugged mid-fade -- otherwise the
    // panel freezes at whatever brightness it had and never clears.
    if (!padStatus_.empty()) {
        buildOverlay();
        requestUpdate();
    }

    // The video recorder owns the clock while paused; a stray stick must not
    // be able to shift the camera midway through a render.
    if (animationsPaused_) return;
    // Only drive the view while pcbview is the active application, or a pad
    // being used in another window would quietly steer the board in the
    // background.
    //
    // Except in the headset, where that reasoning inverts. The premise is that
    // an unfocused window is one the viewer is not looking at -- but in VR the
    // display IS the headset, the desktop window is a mirror, and the viewer
    // cannot see which window Windows considers focused, let alone click one.
    // Entering VR rebuilds the window outright, so focus routinely lands
    // somewhere else the moment the session starts, and every pad poll returned
    // here before reading a button. The pad appeared dead in the one mode where
    // it is the only way to move anything.
    if (!vr_ && QGuiApplication::applicationState() != Qt::ApplicationActive) {
        padSteering_ = false;
        return;
    }

    const input::GamepadState& g = gamepad_.poll();
    if (!g.connected) {
        padSteering_ = false;
        return;
    }

    // In the headset the camera IS the head: nothing the pad does to camera_
    // can be seen, because the eye matrices come from the runtime. So VR forces
    // object mode -- the sticks turn and slide the BOARD, which is the only
    // thing here that can still be moved, and is what R3 already toggles on the
    // desktop.
    const bool vrMode = vr_ != nullptr;
    const bool objectMode = padObjectMode_ || vrMode;

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
        if (objectMode) adoptCameraDeltaIntoBoard(camera_, after);
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
        if (objectMode) {
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
        if (vrMode) {
            // In the headset the shoulders GROW the board rather than sliding
            // it closer, and the difference matters.
            //
            // Readability depends only on ANGULAR size, and approaching and
            // enlarging buy identical amounts of it -- but a 191 mm board shown
            // 0.35 m across renders 1 mm of silkscreen at about five pixels, so
            // reading it means either 0.13 m away, where the eyes have to cross
            // and the board swallows the view, or three times the size at a
            // comfortable arm's length. The second costs no more, because fill
            // is bounded by the screen rather than by the board and whatever
            // overflows the view is free. Confirmed in the headset: comfortable,
            // readable, and none of the close-range glitching.
            //
            // This replaces a dolly that changed distance, which did the same
            // job worse. It deliberately does NOT go on the triggers: those
            // have peeled the stack apart since long before VR existed, and
            // briefly having size on them too meant every attempt to zoom also
            // exploded the board -- which is what "the silkscreen is floating"
            // and "I can see through the mask" actually were.
            const bool bigger = g.heldRightShoulder;
            vrSizeMul_ = std::clamp(
                vrSizeMul_ * std::exp((bigger ? 1.0f : -1.0f) * 0.9f * fdt),
                0.25f, 12.0f);
            vr_->setBoardSizeMul(vrSizeMul_);
            // The width in ROOM metres, which is the number that means
            // something: it says how big the thing in front of you is, and it
            // is the one that decides whether the silkscreen is readable.
            char s[96];
            std::snprintf(s, sizeof(s), "%s   board %.2f m wide",
                          bigger ? "BIGGER" : "SMALLER",
                          static_cast<double>(vr_->boardSizeMetres()));
            setPadStatus(s);
            // Once, so "is the pad reaching the board at all" is answered by
            // the log rather than by squinting through the lenses.
            static bool said = false;
            if (!said) {
                said = true;
                std::printf("vr-pad: R1/L1 grow and shrink the board, R2/L2 "
                            "bring it nearer and further; hold CROSS with a "
                            "trigger to explode\n");
                std::fflush(stdout);
            }
        } else {
            const float rate = (g.heldLeftShoulder ? 1.0f : -1.0f) * 1.6f * fdt;
            camera_.distance =
                std::clamp(camera_.distance * std::exp(rate), 0.5f, 5000.0f);
            // A wheel glide in flight would fight this.
            zoomAnimating_ = false;
            char s[96];
            std::snprintf(s, sizeof(s), "%s   %.1f mm away",
                          g.heldRightShoulder ? "NEARER" : "FURTHER",
                          static_cast<double>(camera_.distance));
            setPadStatus(s);
        }
        moved = true;
    }

    // L2/R2 explode and collapse the stack. These are ANALOG, so unlike the
    // shoulders they give a pressure-proportional peel rate: ease into it for
    // a slow reveal, bury the trigger to throw the stack apart.
    // HELD AS A CHORD: cross (south) plus a trigger, not a trigger alone.
    //
    // Taking the stack apart rearranges the whole board, and the tell that it
    // was too easy to reach is that it happened by accident -- a zoom control
    // briefly shared these triggers and every attempt to zoom peeled the
    // layers, which was reported in good faith as the silkscreen floating and
    // the mask turning transparent. A modifier makes it deliberate without
    // costing anything: the pressure-proportional peel is exactly as it was
    // once the chord is held.
    //
    // Cross rather than one of the others because it is the button a thumb
    // rests on, so the chord is comfortable to hold while the trigger sweeps.
    if (g.heldSouth && (g.rightTrigger > 0.0f || g.leftTrigger > 0.0f)) {
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
        static bool said = false;
        if (!said) {
            said = true;
            std::printf("pad: explode chord live (hold CROSS + R2 to peel, "
                        "L2 to collapse)\n");
            std::fflush(stdout);
        }
        moved = true;
    } else if (g.rightTrigger > 0.0f || g.leftTrigger > 0.0f) {
        // WITHOUT the chord, the triggers bring the board NEARER and FURTHER.
        //
        // Size and distance are different tools in the headset and both are
        // wanted. Growing the board is how you read fine detail without leaning
        // in; moving it is how you place it in the room -- push it away to take
        // in the whole thing, pull it in to work on part of it.
        //
        // On a MONITOR they are the same picture. There is no room to sit in,
        // so a bigger board and a nearer board are indistinguishable, and both
        // pairs necessarily fall back to the camera's zoom. They stay distinct
        // in feel rather than in kind: the shoulders step at a fixed rate, the
        // triggers are pressure-proportional, so this pair is the fine control.
        //
        // Analog either way, so a light pull nudges and a full pull travels.
        constexpr float kCurve = 0.55f;
        const float pull = std::pow(g.rightTrigger, kCurve) -
                           std::pow(g.leftTrigger, kCurve);
        if (vrMode) {
            // Scaled by the board's span so a 50 mm board and a 191 mm one
            // travel at the same apparent rate.
            const auto& bb = mesh_->bounds;
            const float span = static_cast<float>(
                std::max(bb.max[0] - bb.min[0], bb.max[1] - bb.min[1]));
            board_.translation.z += pull * span * 0.6f * fdt;
            char s[96];
            std::snprintf(s, sizeof(s), "%s   %.2f m away",
                          pull > 0.0f ? "NEARER" : "FURTHER",
                          static_cast<double>(vr_->boardDistance()));
            setPadStatus(s);
        } else {
            // Exponential, like the shoulders, so the rate is constant in
            // perceived terms rather than crawling when close and rocketing
            // when far. Half their rate: this is the fine one.
            camera_.distance = std::clamp(
                camera_.distance * std::exp(-pull * 0.8f * fdt), 0.5f, 5000.0f);
            zoomAnimating_ = false;  // a wheel glide would fight this
            char s[96];
            std::snprintf(s, sizeof(s), "%s   %.1f mm away",
                          pull > 0.0f ? "NEARER" : "FURTHER",
                          static_cast<double>(camera_.distance));
            setPadStatus(s);
        }
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

    // VIEWS MOVE ONTO A HELD MENU, off the four face buttons.
    //
    // They used to be one button each, which spent the whole face on four
    // presets and left nothing for a modifier -- and a controller with no
    // spare buttons is how the explode chord ended up sharing the zoom
    // triggers. Holding SQUARE now raises a labelled menu and the D-pad picks,
    // which costs one extra button-press, fits four more views whenever they
    // are wanted, and says on screen what each direction does instead of
    // requiring it to be memorised. The D-pad was completely unbound.
    //
    // That frees CROSS for the explode chord, and leaves CIRCLE and TRIANGLE
    // unassigned.
    const bool menuWasOpen = viewMenuOpen_;
    viewMenuOpen_ = g.heldWest;
    if (viewMenuOpen_) {
        if (g.pressedDpadUp) setViewTop();
        if (g.pressedDpadDown) setViewBottom();
        if (g.pressedDpadLeft) setViewIso();
        if (g.pressedDpadRight) frameBoard();
    } else {
        // Unmodified, the D-pad turns the BOARD in quarter-turns: up and down
        // tumble it away and toward you, left and right spin it in its own
        // plane. Two presses of up is end over end, which is the move you make
        // with a real board when you want the other side.
        //
        // Axes in BOARD space, so "flip it over" means about its own edge
        // however it happens to be oriented at the time -- the same thing your
        // hands would do. Screen-space axes would make the result depend on
        // where you were standing.
        //
        // Discrete quarter-turns rather than a rate, which is what makes them
        // worth having alongside the sticks: the sticks turn the board to any
        // angle, these return it to a square one.
        constexpr float kQuarter = 1.5707963f;
        if (g.pressedDpadUp) turnBoardBy({1.0f, 0.0f, 0.0f}, kQuarter);
        if (g.pressedDpadDown) turnBoardBy({1.0f, 0.0f, 0.0f}, -kQuarter);
        if (g.pressedDpadLeft) turnBoardBy({0.0f, 0.0f, 1.0f}, kQuarter);
        if (g.pressedDpadRight) turnBoardBy({0.0f, 0.0f, 1.0f}, -kQuarter);
    }

    // Ease toward the quarter-turn target. A large object teleporting a half
    // turn right in front of your face is unpleasant even when it is the board
    // moving and not you, so this covers the distance in about a quarter of a
    // second. A stick cancels it: fighting an animation for control of the
    // same rotation is worse than losing the animation.
    if (boardTurning_) {
        if (g.rightX != 0.0f || g.rightY != 0.0f || g.heldTouchpad) {
            boardTurning_ = false;
        } else {
            const float t = std::clamp(fdt * 6.0f, 0.0f, 1.0f);
            board_.rotation =
                glm::normalize(glm::slerp(board_.rotation, boardTurnTarget_, t));
            if (glm::abs(glm::dot(board_.rotation, boardTurnTarget_)) >
                0.99999f) {
                board_.rotation = boardTurnTarget_;
                boardTurning_ = false;
            }
            moved = true;
        }
    }
    if (viewMenuOpen_ != menuWasOpen) {
        buildOverlay();
        requestUpdate();
    }
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
    const bool acted = (viewMenuOpen_ &&
                        (g.pressedDpadUp || g.pressedDpadDown ||
                         g.pressedDpadLeft || g.pressedDpadRight)) ||
                       g.pressedLeftStick || g.pressedRightStick ||
                       g.pressedStart;

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
    // The VR session goes FIRST, before the renderer and long before the Vulkan
    // device.
    //
    // The session was created against that device and holds swapchain images
    // living in it, so destroying the device underneath it leaves the runtime
    // referencing freed memory -- and SteamVR's compositor wedges, which is
    // what "a key component of SteamVR isn't working properly" means. It then
    // stays wedged until SteamVR is restarted, so every run poisoned the next.
    //
    // The timer has to stop too: it fires stepVr on a 1 ms interval and would
    // happily drive a half-destroyed renderer.
    if (vrTimer_) {
        vrTimer_->stop();
        vrTimer_ = nullptr;   // parented to this; Qt owns the deletion
    }
    vr_.reset();      // ends the session properly -- see VrSession::end
    xrSystem_.reset();

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
    if (vrRequested_ && !xrSystem_ && instance_ == VK_NULL_HANDLE) {
        // Step-by-step, because every one of these can block and from the
        // outside they are indistinguishable: the window simply stops
        // responding. Creating the OpenXR instance starts SteamVR, asking for
        // the system waits on the headset, and the hand-over talks to the
        // compositor -- knowing WHICH one is the difference between a fix and
        // a guess.
        std::printf("vr: bring-up 1/4 -- asking the runtime for a session\n");
        std::fflush(stdout);
        xrSystem_ = std::make_unique<xr::System>();
        if (xrSystem_->start()) {
            std::printf("vr: bring-up 2/4 -- runtime answered\n");
            std::fflush(stdout);
            xrSystem_->installHooks();
            std::printf("vr: %s -- Vulkan will be created through the runtime\n",
                        xrSystem_->headsetName());
            std::fflush(stdout);
        } else {
            // Asked for and not available: SteamVR down, no headset, or the
            // runtime refused. Carry on as a desktop session and clear the
            // request, so the menu does not sit checked over a viewport that
            // has no session and every later rebuild does not retry it.
            xrSystem_.reset();
            vrRequested_ = false;
            std::printf("vr: requested, but no runtime answered -- staying on "
                        "the desktop\n");
            std::fflush(stdout);
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

        // Validation is a DEBUG tool and was being enabled unconditionally,
        // including in release builds. It is not free: the layer intercepts
        // every Vulkan call, and a ray-traced frame makes a great many, so it
        // was taxing the frame rate as well as burying every useful line of
        // output under screenfuls of spec text -- most of it about images the
        // OpenXR runtime creates, which we neither own nor can fix.
        //
        // Debug builds keep it. Release builds ask for it with
        // PCBVIEW_VALIDATION=1; debug builds opt out with PCBVIEW_NO_VALIDATION.
#ifdef NDEBUG
        const bool wantValidation =
            qEnvironmentVariableIsSet("PCBVIEW_VALIDATION");
#else
        const bool wantValidation =
            !qEnvironmentVariableIsSet("PCBVIEW_NO_VALIDATION");
#endif
        if (xrSystem_) {
            std::printf("vr: bring-up 3/4 -- creating the Vulkan instance "
                        "through the runtime\n");
            std::fflush(stdout);
        }
        instance_ = createInstance(wantValidation, extensions);
        if (xrSystem_) {
            std::printf("vr: bring-up 4/4 -- instance created\n");
            std::fflush(stdout);
        }
        if (wantValidation) messenger_ = createDebugMessenger(instance_);

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
        // Whether or not the session opened, say so once -- the menu check
        // follows the session, not the request.
        emit vrActiveChanged(vr_ != nullptr);
        if (!vr_) {
            vrRequested_ = false;
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

void VulkanWindow::setPadStatus(const std::string& s) {
    padStatus_ = s;
    padStatusClock_.restart();
    buildOverlay();
    requestUpdate();
}

void VulkanWindow::turnBoardBy(const glm::vec3& axisBoardSpace, float radians) {
    const float len = glm::length(axisBoardSpace);
    if (radians == 0.0f || len < 1e-6f) return;
    // Compose onto the TARGET, not onto the current rotation, so pressing twice
    // quickly gives a half turn rather than the second press restarting from
    // wherever the first had eased to.
    const glm::quat from = boardTurning_ ? boardTurnTarget_ : board_.rotation;
    boardTurnTarget_ = glm::normalize(
        glm::angleAxis(radians, axisBoardSpace / len) * from);
    boardTurning_ = true;
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

    // ---- transient zoom readout ---------------------------------------------
    //
    // Growing the board and moving it closer look IDENTICAL while they are
    // happening -- the board fills more of the view either way -- so without a
    // readout there is no telling which pair you have hold of, nor how far a
    // blind trigger pull has taken you with a headset on. The number is the
    // point: metres across for size, metres away for distance.
    //
    // Bottom centre, clear of the measurement panel (top right) and the view
    // menu (centre). Fades over the last third of its life so it leaves
    // quietly rather than blinking out.
    if (!padStatus_.empty() && padStatusClock_.isValid()) {
        constexpr qint64 kHoldMs = 1400;
        const qint64 age = padStatusClock_.elapsed();
        if (age > kHoldMs) {
            padStatus_.clear();
        } else {
            const VkExtent2D se = renderer_->sceneExtent();
            const float w = se.width > 0 ? static_cast<float>(se.width)
                                         : static_cast<float>(width()) * dpr;
            const float h = se.height > 0 ? static_cast<float>(se.height)
                                          : static_cast<float>(height()) * dpr;
            const float fade =
                age < kHoldMs * 2 / 3
                    ? 1.0f
                    : 1.0f - static_cast<float>(age - kHoldMs * 2 / 3) /
                                 static_cast<float>(kHoldMs / 3);
            const float ts = std::max(13.0f, h * 0.020f);
            const float pad = ts * 0.8f;
            text::TextStyle st;
            st.size = {static_cast<double>(ts) * 0.9,
                       static_cast<double>(ts)};
            st.thickness = ts * 0.14;
            const float tw =
                static_cast<float>(text::measure(padStatus_, st));
            const float panelW = tw + 2.0f * pad;
            const float panelH = ts * 2.2f;
            const float x0 = (w - panelW) * 0.5f;
            const float cy = h - panelH * 1.6f;
            const float bg[4] = {0.06f, 0.06f, 0.08f, 0.80f * fade};
            const float fg[4] = {kAmber[0], kAmber[1], kAmber[2], fade};
            quad(x0, cy, x0 + panelW, cy, panelH, bg);
            drawText(padStatus_, w * 0.5f, cy, ts, fg);
        }
    }

    // ---- view menu, while SQUARE is held ------------------------------------
    //
    // Centred on the SCENE extent rather than the window, because in the
    // headset those differ: the overlay is recorded into the eye target, so
    // laying it out in window pixels would push it off to one side of both
    // eyes. On the desktop the two are the same and this changes nothing.
    if (viewMenuOpen_) {
        const VkExtent2D se = renderer_->sceneExtent();
        const float w = se.width > 0 ? static_cast<float>(se.width)
                                     : static_cast<float>(width()) * dpr;
        const float h = se.height > 0 ? static_cast<float>(se.height)
                                      : static_cast<float>(height()) * dpr;
        // Scaled to the target rather than to the desktop's DPI, so it reads
        // the same size through the lenses as it does on a monitor.
        const float ts = std::max(13.0f, h * 0.022f);
        const float lh = ts * 1.9f;
        const float pad = ts * 1.1f;

        struct Row { const char* key; const char* label; };
        static const Row kRows[4] = {{"UP", "Top"},
                                     {"DOWN", "Bottom"},
                                     {"LEFT", "Isometric"},
                                     {"RIGHT", "Fit board"}};
        text::TextStyle st;
        st.size = {static_cast<double>(ts) * 0.9, static_cast<double>(ts)};
        st.thickness = ts * 0.14;
        float wMax = 0.0f;
        for (const Row& r : kRows) {
            const std::string s = std::string(r.key) + "   " + r.label;
            wMax = std::max(wMax, static_cast<float>(text::measure(s, st)));
        }
        const std::string title = "VIEW";
        wMax = std::max(wMax, static_cast<float>(text::measure(title, st)));

        const float panelW = wMax + 2.0f * pad;
        const float panelH = 5.0f * lh + 2.0f * pad;
        const float x0 = (w - panelW) * 0.5f;
        const float x1 = x0 + panelW;
        const float y0 = (h - panelH) * 0.5f;

        static const float kMenuBg[4] = {0.06f, 0.06f, 0.08f, 0.88f};
        quad(x0, y0 + panelH * 0.5f, x1, y0 + panelH * 0.5f, panelH, kMenuBg);
        quad(x0, y0 + panelH * 0.5f, x0 + ts * 0.25f, y0 + panelH * 0.5f,
             panelH, kAmber);
        drawText(title, (x0 + x1) * 0.5f, y0 + pad + lh * 0.5f, ts, kAmber);
        for (int i = 0; i < 4; ++i) {
            const std::string s =
                std::string(kRows[i].key) + "   " + kRows[i].label;
            drawText(s, (x0 + x1) * 0.5f,
                     y0 + pad + lh * (static_cast<float>(i) + 1.5f), ts,
                     kWhite);
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
            // Advance the VR configuration sweep. Pressed blind, with the
            // headset on -- which is the point: only the wearer knows when
            // they have seen enough of a configuration to judge it, and a
            // fixed timer gave rows the wearer could not tell apart.
            case Qt::Key_Space: vrSweepAdvance_ = true; return;
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
