#pragma once

// A standalone CPU path tracer built on Intel Embree.
//
// Why this exists: the GPU path tracer is a Vulkan compute shader that ray-
// queries a Vulkan BLAS. On the CPU "device" that falls apart -- Mesa lavapipe's
// software BVH builder silently drops most triangles on our ~1M-triangle boards,
// and SwiftShader has no ray tracing at all. So the CPU render mode bypasses
// Vulkan for ray tracing entirely and uses Embree, whose BVH build + traversal
// are production-proven on the CPU. Vulkan is still used on the CPU device for
// raster and to present this tracer's output.
//
// This is deliberately a SECOND path tracer: it mirrors the lighting/material
// model of shaders/pathtrace.comp in C++, because GLSL and C++ cannot share the
// code. KEEP THE TWO IN STEP when either changes.

#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "geom/tessellate.h"

namespace pcbview::cpu {

// Camera as a ray basis, matching the push constants the GPU tracer receives:
// a ray through pixel (u,v) in [0,1] is  fwd + (2u-1)*right - (2v-1)*up,  from
// eye. right/up already include tan(halfFov).
struct TraceCamera {
    std::array<float, 3> eye{};
    std::array<float, 3> fwd{};
    std::array<float, 3> right{};
    std::array<float, 3> up{};
    // Orthographic: rays parallel along fwd, origin offset across right/up
    // (which then carry mm half-extents instead of tan(halfFov)).
    bool ortho = false;
};

// Per-material shading parameters, mirroring shaders/pathtrace.comp's Mat.
struct Material {
    std::array<float, 4> albedo{};  // rgb + alpha (film opacity)
    float roughness = 0.5f;
    float metallic = 0.0f;
    float fade = 0.0f;  // 1 = substrate: fades to translucent on peel
};

class CpuTracer {
public:
    CpuTracer();
    ~CpuTracer();
    CpuTracer(const CpuTracer&) = delete;
    CpuTracer& operator=(const CpuTracer&) = delete;

    // Flatten the board mesh into shared vertex/index buffers and build the
    // Embree scene. Per-triangle material is captured for shading. Rebuilds on
    // each call.
    void setScene(const geom::BoardMesh& mesh);
    bool ready() const { return scene_ != nullptr; }

    // Re-bake the exploded view: displace each part's vertices by its peel rank
    // (mirroring the GPU bakeExplode) and rebuild the Embree BVH. `stepMm` is the
    // per-stage travel. Returns true if anything changed (the caller then resets
    // accumulation). No-op when the peel is unchanged.
    bool setExplode(float progress, float stepMm);
    float maxRank() const { return maxRank_; }

    // RT-preview integrator: primary hit + sun shadow + one-sample AO, no GI
    // bounces, and films composited ANALYTICALLY (like the raster blend) rather
    // than by stochastic alpha -- so the soldermask looks crisp and opaque-ish
    // exactly as it does in raster + RT on a GPU, not like the path tracer.
    // Converges in a handful of samples. The caller resets accumulation when
    // switching (mixed integrators in one accumulation would be wrong).
    void setPreview(bool on) { preview_ = on; }

    // Appearance parity with the Vulkan renderer's material edits. Same
    // formulas as Renderer::setMaskColor / setSubstrateAppearance /
    // setComponentShine / setPadShine; matched by material KIND, since the
    // renderer reorders parts and indices do not line up. Caller restarts
    // accumulation (these arrive via paths that already do).
    void setMaskAppearance(float r, float g, float b, float opacity);
    void setSubstrateAppearance(float r, float g, float b, float opacity);
    void setComponentShine(float s01);
    void setPadShine(float s01);
    // cos(sun angular radius) for the soft-shadow cone; 1 = point sun.
    void setSunCosMax(float c) { sunCosMax_ = c; }

    // Net highlighting, mirroring shaders/pathtrace.comp and board_rt.frag.
    //
    // The per-triangle net table is NOT passed in: it is built by setScene, in
    // this tracer's own triangle order. The renderer's equivalent array is in a
    // different order (it hoists films to the end of the index buffer), so
    // accepting one from outside just invites the two to drift apart -- which
    // is exactly the bug that made an unrelated plane light up.
    //
    // `netColour` is rgb + a, where a > 0 marks a net as highlighted. The
    // highlighted net is both an emitter and, via next-event estimation
    // (rebuildNetLights + the NEE block in trace()), an explicitly-sampled area
    // light, so its red glow spills onto the surrounding copper and laminate --
    // matching the GPU tracer within ~1% of reddish pixels on cx4multicart_v3/
    // GND. `netSpan` is the per-net chase origin (xyz) and inverse span (w) --
    // phase is computed from the HIT POSITION against it, so the sweep follows
    // the copper's shape rather than its triangulation, exactly as the GPU
    // paths do. Empty vectors turn highlighting off.
    void setHighlight(std::vector<std::array<float, 4>> netColour,
                      std::vector<std::array<float, 4>> netSpan);
    void setNetGlow(float strength) { netGlow_ = strength; }
    bool highlighting() const { return netOn_; }

    // Visibility parity: hidden parts are removed from the traced scene by
    // NaN-ing their vertices in the next bake (Embree ignores non-finite
    // triangles), mirroring the GPU's NaN-bake. Returns true if anything
    // changed; the next setExplode()/bake call rebuilds.
    bool setPartVisible(const std::string& name, bool visible);

    // Progressive HDR accumulation. Trace `spp` samples per pixel for THIS call
    // and add them into the internal accumulators, keyed by `frame` (pass 0
    // clears; later passes with different `frame` keep accumulating). Camera and
    // resolution are captured on the clearing pass. Multithreaded via TBB.
    void accumulate(const TraceCamera& cam, uint32_t width, uint32_t height,
                    int spp, uint32_t frame);

    // Total samples accumulated so far for the current image.
    int samples() const { return accumSamples_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

    // Resolve the current accumulation to an 8-bit RGBA image (tonemapped,
    // gamma-encoded, top-left origin), optionally OIDN-denoised first. For the
    // headless CLI / BMP.
    std::vector<uint8_t> resolve(bool denoise) const;

    // Resolve for on-screen display: 8-bit BGRA, sRGB-encoded, to be copied
    // straight into a B8G8R8A8_SRGB Vulkan image and blit to the swapchain.
    // `chaseMs` + `animate` drive the highlight chase, applied HERE rather than
    // during tracing -- like the GPU's tonemap pass, so the animation costs no
    // convergence and a converged image can keep animating.
    std::vector<uint8_t> resolveDisplay(bool denoise, uint32_t chaseMs = 0,
                                        bool animate = false) const;

    // Convenience for the headless CLI: clear + accumulate spp + resolve.
    std::vector<uint8_t> renderImage(const TraceCamera& cam, uint32_t width,
                                     uint32_t height, int spp, bool denoise);

    // --- Cheap per-frame chase update -------------------------------------
    //
    // A converged image being chased must refresh every frame, but only the
    // tagged (highlighted-net) pixels actually change -- a few percent of the
    // image. resolveDisplay and the async denoise record, alongside the buffer
    // they produce, each tagged pixel's UNmodulated linear colour and phase;
    // patchChase re-modulates and re-encodes just those pixels in place.
    // (Display buffers are produced UNmodulated -- the chase is only ever
    // applied by this call, so patching and full resolves compose.)
    //
    // Returns false when no patch list matches `bgra`. With `allowStale`
    // false the list must also match the CURRENT sample count -- the caller
    // wants a fresh base once accumulation has moved on. With it true a
    // stale base is fine (the denoised-display path: its base refreshes on
    // milestones, and patching it beats showing unfiltered noise).
    bool patchChase(std::vector<uint8_t>& bgra, uint32_t chaseMs,
                    bool allowStale) const;

    // --- Asynchronous denoise + resolve -----------------------------------
    //
    // OIDN takes ~0.5s on a full frame; running it inline froze the frame
    // loop at every milestone. kickDenoiseResolve snapshots the accumulators
    // on the CALLING thread (the only writer, so no race) and hands them to a
    // worker that denoises, tonemaps and encodes; fetchDenoiseResolve polls
    // for the finished image and swaps it into `bgra` (installing its chase
    // patch list) when ready. A kick while busy, or for a sample count
    // already kicked, is a no-op. discardDenoise invalidates anything kicked
    // or in flight -- call on accumulation restart, so a result traced from
    // the OLD camera can never be shown under the new one.
    void kickDenoiseResolve(bool animate);
    bool fetchDenoiseResolve(std::vector<uint8_t>& bgra);
    void discardDenoise();

private:
    void releaseScene();
    // (Re)build the Embree scene from vertsPadded_/indices_. `fastBuild` trades
    // BVH quality for build speed -- used for explode re-bakes, where the user
    // is waiting on the rebuild between wheel clicks.
    void rebuildScene(bool fastBuild);

    bool preview_ = false;
    float sunCosMax_ = 0.99978f;  // cos(1.2 deg): slider default 15

    void* device_ = nullptr;  // RTCDevice
    void* scene_ = nullptr;   // RTCScene

    // Owned geometry -- Embree holds SHARED pointers into vertsPadded_/indices_,
    // so they must outlive the scene.
    std::vector<float> vertsPadded_;   // 4 floats/vertex (xyz + pad) for Embree
    std::vector<float> restVerts_;     // rest positions, before any explode
    std::vector<float> normals_;       // 3 floats/vertex, for smooth shading
    std::vector<uint32_t> indices_;    // 3 per triangle, global
    std::vector<Material> materials_;  // one per part with geometry
    std::vector<uint32_t> triMat_;     // per triangle -> materials_ index

    // Per-part explode/visibility data (mesh.parts order, empties skipped), to
    // re-bake the peel exactly like the GPU path.
    struct PartSpan {
        uint32_t firstVertex = 0, vertexCount = 0;
        float rank = 0.0f;  // signed peel rank; 0 = stays put
        std::string name;
        geom::Material kind = geom::Material::Copper;
        bool visible = true;
    };
    std::vector<PartSpan> partSpans_;
    float maxRank_ = 0.0f;
    float peel_ = 0.0f;           // substrate fade amount (matches GPU misc.x)
    float bakedProgress_ = 0.0f;  // explode progress the scene is baked to
    float bakedStep_ = 0.0f;      // step the scene is baked with
    bool bakeDirty_ = false;      // visibility changed -> next setExplode rebakes
    void bake(float progress, float stepMm);  // displace + hide + rebuild BVH

    // Accumulation buffers (HDR, one float3 per pixel), plus denoiser guides.
    mutable std::vector<float> accumColor_;   // sum of radiance
    mutable std::vector<float> accumAlbedo_;  // sum of first-hit albedo guide
    mutable std::vector<float> accumNormal_;  // sum of first-hit normal guide
    int accumSamples_ = 0;
    uint32_t width_ = 0, height_ = 0;
    TraceCamera cam_{};

    // Adaptive sampling (PT only): per-pixel sample counts plus luminance sum
    // and sum-of-squares. A pixel whose relative standard error drops below
    // threshold stops receiving samples, so the flat background -- most of the
    // image -- goes quiet after a handful of batches while edges and glints
    // keep refining. All resolve paths divide by the PER-PIXEL count.
    std::vector<uint16_t> pixSamples_;
    std::vector<float> lumSum_, lumSum2_;

    // Chase patch list for patchChase: pixel index, phase, unmodulated linear
    // colour. Tied to the display buffer it was built beside;
    // chasePatchSamples_ records the sample count that buffer resolved, so
    // patchChase can tell a stale base from a current one.
    struct ChasePatch {
        uint32_t px;
        float phase, r, g, b;
    };
    mutable std::vector<ChasePatch> chasePatches_;
    mutable bool chasePatchesValid_ = false;
    mutable int chasePatchSamples_ = -1;

    // Encode a tonemapped linear image to display BGRA (pooled, LUT sRGB),
    // recording the chase patch list when `animate`. Never modulates -- see
    // patchChase.
    void encodeDisplay(const std::vector<float>& lin,
                       const std::vector<float>& aov, bool animate,
                       std::vector<uint8_t>& out,
                       std::vector<ChasePatch>& patches, bool& valid) const;

    // Normalise (per-pixel counts) + OIDN, from explicit buffers so the async
    // worker can run it on a snapshot while accumulation continues.
    void denoiseBuffers(const float* col, const float* alb, const float* nor,
                        const uint16_t* cnt, int globalSamples, uint32_t w,
                        uint32_t h, std::vector<float>& out) const;

    // Async denoise state. The snapshot is taken on the render thread (the
    // only accumulator writer, so it is race-free); the worker owns it and
    // the cached OIDN objects until done. Results carry a sequence number so
    // discardDenoise can drop an in-flight job's output.
    std::thread dnThread_;
    mutable std::mutex dnMutex_;
    std::condition_variable dnCv_;
    bool dnKick_ = false, dnStop_ = false, dnBusy_ = false, dnReady_ = false;
    int dnKickedSamples_ = -1;
    int dnPatchSamples_ = -1;  // samples of the READY result
    uint64_t dnSeq_ = 0, dnMinSeq_ = 0;
    bool dnAnimate_ = false;
    uint32_t dnW_ = 0, dnH_ = 0;
    std::vector<float> dnCol_, dnAlb_, dnNor_, dnPhase_;
    std::vector<uint16_t> dnCnt_;
    std::vector<uint8_t> dnOut_;
    std::vector<ChasePatch> dnPatches_;
    void denoiseWorker();

    // Cached OIDN state (CPU device), rebuilt on resolution change. Mutable so
    // resolve() stays const like the rest of the read path.
    mutable void* oidn_ = nullptr;        // OIDNDevice
    mutable void* oidnFilter_ = nullptr;  // OIDNFilter
    mutable void* oidnColor_ = nullptr;   // OIDNBuffer
    mutable void* oidnAlbedo_ = nullptr;
    mutable void* oidnNormal_ = nullptr;
    mutable void* oidnOut_ = nullptr;
    mutable uint32_t oidnW_ = 0, oidnH_ = 0;
    void denoiseInto(std::vector<float>& colorOut) const;
    void resolveLinear(std::vector<float>& lin, bool denoise) const;

    // Net highlight state. triNet_ is per triangle; the other two per net.
    std::vector<int32_t> triNet_;
    std::vector<std::array<float, 4>> netColour_;
    std::vector<std::array<float, 4>> netSpan_;
    bool netOn_ = false;
    float netGlow_ = 1.0f;
    // The highlighted nets' triangles, as an emitter list for next-event
    // estimation -- 11 floats each: corners a,b,c (xyz), triangle area, and the
    // net id as a float. Built from the BAKED geometry (post-explode/-hide) so
    // the shadow ray and the traced scene agree; rebuilt whenever either the
    // highlight or the bake changes. Mirrors the GPU's netLit buffer, except the
    // GPU builds its copy once from rest positions.
    std::vector<float> netLights_;
    static constexpr int kNetLightStride = 11;
    void rebuildNetLights();
    // First-hit chase AOV, 2 floats per pixel: phase along the net, and 1 when
    // the pixel shows highlighted copper directly. The GPU keeps this as a
    // storage image for the same reason -- the chase is a display-time
    // modulation and must not be baked into the accumulated radiance.
    mutable std::vector<float> phaseAov_;
};

}  // namespace pcbview::cpu
