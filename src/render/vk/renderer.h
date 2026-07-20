#pragma once

// Vulkan forward rasteriser for a tessellated board.
//
// Written so the ray traced mode in phase 4 is additive, not a rewrite. The
// three RT-readiness rules from ARCHITECTURE.md are load-bearing here:
//
//   1. RT extensions are enabled at device creation but never required.
//   2. Mesh buffers carry SHADER_DEVICE_ADDRESS and the acceleration-structure
//      build-input usage from the start, so phase 4 builds a BLAS/TLAS over the
//      very same buffers rather than rebuilding the asset layer.
//   3. Materials live in a bindless SSBO indexed by instance ID -- never
//      per-draw descriptor sets, which a closest-hit shader cannot do.

#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <memory>

#include "geom/tessellate.h"
#include "render/common/device.h"
#include "render/cpu/cpu_tracer.h"

namespace pcbview::vk {

// Mirrors the `Material` struct in board.vert, board.frag, board_rt.frag AND
// pathtrace.comp -- one SSBO read by all four, so a field added here must be
// added to every one of them or they read at the wrong stride.
struct MaterialGpu {
    float albedo[4];  // rgb + opacity
    float params[4];  // roughness, metallic, reserved, reserved
    // x = this draw's FIRST GLOBAL TRIANGLE. The raster fragment shader only
    // gets gl_PrimitiveID, which restarts at 0 every draw; adding this base
    // turns it into an index into the per-triangle net buffer. One draw is
    // one part is one material, so the material table is the natural place
    // to hang it.
    uint32_t extra[4];
};

struct FrameStats {
    uint32_t drawCalls = 0;
    uint32_t triangles = 0;      // drawn this frame (respects visibility)
    uint32_t trianglesTotal = 0; // uploaded
};

// How the scene is shaded. Raster is the default (with optional RT shadows);
// PathTraced is the progressive path tracer.
enum class RenderMode { Raster, PathTraced };

// One toggleable piece of the board, surfaced for the UI.
struct PartInfo {
    std::string name;
    uint32_t triangles = 0;
    bool visible = true;
    bool blended = false;
    // What the part is made of -- lets appearance/effects setters find e.g. all
    // Component or all Copper parts without name games.
    geom::Material material = geom::Material::Copper;
    // "vias" parts only: a blind/buried barrel spanning part of the stack. It
    // travels with its layers during the peel instead of pinning to the
    // barrel plane -- see the explode-rank block in uploadBoard.
    bool partialBarrel = false;
};

class Renderer {
public:
    Renderer(Device& device, VkSurfaceKHR surface, uint32_t width,
             uint32_t height);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Uploads the board into one global vertex buffer and one global index
    // buffer, with per-part offsets. Not a buffer per part: rule 2 wants a
    // single allocation an acceleration structure can be built over.
    void uploadBoard(const geom::BoardMesh& mesh);

    // Returns false if the swapchain is out of date and the caller should resize.
    //
    // `drawUi` is invoked with the command buffer while the swapchain is bound at
    // NATIVE resolution, after the scene has been blitted in. That ordering is
    // the point of the offscreen target: the board can be super- or sub-sampled
    // while the interface stays pixel-crisp.
    bool drawFrame(const float viewProj[16], const float cameraPos[3],
                   const std::function<void(VkCommandBuffer)>& drawUi = {});

    void resize(uint32_t width, uint32_t height);
    void waitIdle();

    // Internal render scale. 1.0 = native; 2.0 supersamples; 0.5 halves it.
    // Applies to the 3D scene only, never the UI.
    void setRenderScale(float scale);
    float renderScale() const { return renderScale_; }

    // Substrate (FR4/flex) appearance. rgb in 0..1, opacity 1 = solid. Updating
    // these re-writes just the material table -- no re-tessellation -- so it is
    // cheap enough to drive from a live colour picker / slider. Opacity < 1
    // routes the substrate through the blended pass permanently (not only while
    // peeling), which is what "translucent flex" needs.
    void setSubstrateAppearance(float r, float g, float b, float opacity);

    // Soldermask colour (both sides). Opacity is kept at its film default -- the
    // mask reads as a translucent film regardless of tint. Same live-update,
    // no-retessellate path as the substrate.
    void setMaskColor(float r, float g, float b, float opacity);
    // Stylised "effects" (deliberately not physically accurate). Component
    // reflections: 0 = matte plastic (default), 1 = chrome -- component bodies
    // mirror the board and their neighbours, showing off what the path tracer
    // can do. Pad shine: how polished the exposed copper/pads are.
    void setComponentShine(float s01);
    void setPadShine(float s01);
    // Sun angular size for the path tracers: 0 = point sun (razor shadows),
    // 1 = 8-degree radius (very soft penumbras). Restarts accumulation.
    void setShadowSoftness(float s01);

    // The camera's forward axis and projection kind, for the RASTER shaders.
    // `orthoDistance` is the orbit distance in orthographic mode and 0 in
    // perspective. Orthographic has one view direction for every fragment;
    // without this the shaders fall back to the eye-point view vector, which
    // reverses for fragments at or behind the eye plane and blackens the
    // board's near edge. Costs nothing when unchanged.
    void setCameraAxis(const float fwd[3], float orthoDistance);

    // Screen-space overlay (measurements, dimension callouts): a triangle list
    // in PIXELS with the origin at the top-left, interleaved x,y,r,g,b,a --
    // 6 floats per vertex, pre-built by the caller (thick lines, arrowheads,
    // stroked text all arrive as triangles). Drawn in the UI pass at native
    // resolution, over every render mode. An empty vector clears it.
    void setOverlay(std::vector<float> tris) { overlayTris_ = std::move(tris); }

    // Net highlighting: light up every triangle on `net` (an index into the
    // BoardMesh net table) and mute the rest, so one signal can be followed
    // across layers and through the exploded view. -1 clears.
    //
    // In the path tracer the net becomes an EMITTER, so it spills red light
    // onto its surroundings and appears in reflections. That changes the
    // image, hence the accumulation restart.
    // Highlight one net, or several at once each in its own colour. The
    // shaders look the colour up per net rather than comparing against a
    // single index, so the count is not fixed. Empty clears.
    void setHighlightNets(const std::vector<int>& nets,
                          const std::vector<std::array<float, 3>>& colours);
    void setHighlightNet(int net);
    int highlightNet() const {
        return highlightNets_.empty() ? -1 : highlightNets_.front();
    }

    // How hard the highlighted net emits. In the PATH TRACER this is literal
    // radiosity -- the net is an emitter, so raising this genuinely throws
    // more red light onto the copper and laminate around it. In raster/RT it
    // only drives how far past white the trace clips. Restarts accumulation.
    void setNetGlow(float strength);
    float netGlow() const { return netGlow_; }

    // Exploded view, peeled outside-in.
    //
    // `mmPerStage` is how far a ring travels per stage of progress.
    // `progress` is already eased by the caller (dwell included) and runs 0 at a
    // solid board up to maxRank() when every ring has peeled.
    //
    // Free to animate: one push constant. Geometry and the per-part ranks in the
    // material table never change.
    void setExplode(float mmPerStage, float progress) {
        explodeStep_ = mmPerStage;
        explodeProgress_ = progress;
    }

    // Largest |rank| in the loaded board -- i.e. how many stages a full peel
    // takes. Zero until a board is uploaded.
    float maxRank() const { return maxRank_; }

    VkExtent2D sceneExtent() const { return sceneExtent_; }
    VkExtent2D windowExtent() const { return extent_; }

    // Read access for the UI tree. Visibility changes must go through
    // setPartVisible so the traced geometry (PT + RT shadows) follows -- a raw
    // PartInfo::visible write only affects the raster draws.
    std::vector<PartInfo>& parts() { return parts_; }
    void setPartVisible(const std::string& name, bool visible);
    const Device& device() const { return device_; }
    VkFormat colorFormat() const { return colorFormat_; }
    uint32_t imageCount() const { return static_cast<uint32_t>(images_.size()); }

    // Ray-traced contact shadows + ambient occlusion, traced with ray queries
    // from the fragment shader against an acceleration structure over the board.
    // A no-op if the device has no ray_query support (checked at construction).
    // Toggled per-frame via a push constant -- no pipeline switch.
    void setRayTracing(bool on) { rtRequested_ = on; }
    bool rayTracingSupported() const { return rtSupported_; }

    // Path tracing. Needs the same ray_query support as RT shadows. The camera ray
    // basis is pushed each frame (setRayCamera); accumulation resets whenever the
    // camera or scene changes. `accumulatedSamples()`/`maxSamples()` drive the
    // "keep rendering until converged" loop and a progress readout.
    void setRenderMode(RenderMode m);
    RenderMode renderMode() const { return mode_; }
    bool pathTracingSupported() const { return rtSupported_; }
    // `ortho`: rays are PARALLEL along fwd, with eye offset across the
    // right/up plane (right/up carry the half-extents in mm instead of
    // tan(halfFov)). Matches the raster orthographic projection, so the Ortho
    // toggle works in the traced modes too.
    void setRayCamera(const float eye[3], const float fwd[3],
                      const float right[3], const float up[3], bool ortho);
    int accumulatedSamples() const {
        return cpuMode_ && cpuTracer_ ? cpuTracer_->samples() : ptSampleCount_;
    }
    int maxSamples() const {
        // The CPU (Embree) tracer converges to a good OIDN-cleaned image in far
        // fewer samples, and every sample is expensive, so cap it lower.
        return cpuMode_ ? (ptMaxSamples_ < 128 ? ptMaxSamples_ : 128)
                        : ptMaxSamples_;
    }
    void setMaxSamples(int n) { ptMaxSamples_ = n < 1 ? 1 : n; }

    // Intel Open Image Denoise on the accumulated result. Denoising is a
    // synchronous GPU->CPU->GPU step the caller triggers (denoise()) when it wants
    // a clean frame. No-op if OIDN is off.
    void setDenoising(bool on);
    bool denoising() const { return denoisingEnabled_; }
    // Advance the continuous asynchronous denoise: kick a pass when the (still)
    // camera has fresh samples, poll the fenced readback, hand OIDN to a worker
    // thread, display the result when it lands. Never blocks the caller beyond
    // a few ms. Returns true while a pass is in flight or was just displayed --
    // the caller should keep frames coming so the state machine advances.
    bool denoiseTick();
    // OIDN device actually selected: "CPU", "CUDA", "HIP", "SYCL", … (or "none").
    std::string oidnDeviceName() const;

    // Write the next presented frame to a BMP. Exists so the Vulkan path can be
    // verified without a human watching a window -- the same reason the software
    // rasteriser exists. Requires TRANSFER_SRC on the swapchain images.
    //
    // `sceneTarget` grabs the OFFSCREEN scene image instead of the swapchain,
    // i.e. the board at the internal render scale rather than at window size.
    // That is what makes a high-resolution export possible: raise the scale,
    // capture this, restore. The overlay is drawn into that image too, so
    // measurements and dimension callouts survive the export.
    void requestCapture(const std::string& path, bool sceneTarget = false) {
        capturePath_ = path;
        captureScene_ = sceneTarget;
    }

    const FrameStats& stats() const { return stats_; }

private:
    struct Buffer {
        VkBuffer handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
    };

    struct DrawItem {
        uint32_t indexOffset = 0;
        uint32_t indexCount = 0;
        int32_t vertexOffset = 0;
        uint32_t material = 0;
        bool blended = false;       // always translucent (soldermask)
        bool fadesWhenPeeled = false;  // becomes translucent only while peeling
        bool hideWhenCollapsed = false;  // inner copper: buried, so at rest it
                                         // only shows as edge z-fighting lines --
                                         // hidden until the board peels open
        uint32_t part = 0;          // index into parts_
        float centre[3] = {0, 0, 0};   // unexploded world centre, for sorting
        float rank = 0.0f;
    };

    struct Image {
        VkImage handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    // Ray-query acceleration structures over the board mesh. See the .cpp.
    void loadRtFunctions();
    void buildAccelerationStructures(uint32_t vertexCount, uint32_t indexCount);
    void destroyAccelerationStructures();
    VkDeviceAddress bufferAddress(const Buffer& b) const;

    // Path tracer: a compute pipeline that fills an HDR accumulation image, and a
    // fullscreen tonemap pass that resolves it into the scene colour target so the
    // existing blit + UI present it. Descriptors are (re)written on upload/resize.
    void createPathTracer();
    void destroyPathTracer();
    void updatePathTraceDescriptors();
    void recordPathTrace(VkCommandBuffer cmd);

    void createSwapchain(uint32_t width, uint32_t height);
    void destroySwapchain();
    void createSceneTargets();
    void destroySceneTargets();
    void createPipeline();
    void createOverlayPipeline();
    // `drawArea` is the target being rendered into. Overlay vertices are in
    // WINDOW pixels, so the push constant always describes the window and the
    // viewport transform stretches that onto a larger export target -- line
    // widths and glyph strokes scale with it for free.
    void recordOverlay(VkCommandBuffer cmd, VkExtent2D drawArea);
    void createSyncAndCommands();
    void createDescriptors();

    Image createImage(VkExtent2D extent, VkFormat format,
                      VkImageUsageFlags usage, VkImageAspectFlags aspect);
    void destroyImage(Image& image);

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                        VkMemoryPropertyFlags props);
    // Host buffer the CPU reads back. Prefers HOST_CACHED memory (strided reads
    // from uncached/write-combined mapped memory are ~100x slower and were the
    // whole cost of denoising), falling back to plain coherent if unavailable.
    Buffer createReadbackBuffer(VkDeviceSize size);
    void destroyBuffer(Buffer& buffer);
    void uploadViaStaging(Buffer& dst, const void* data, VkDeviceSize size);
    uint32_t findMemoryType(uint32_t filter, VkMemoryPropertyFlags props) const;
    // Non-throwing variant: returns -1 when no memory type matches.
    int findMemoryTypeOrNeg(uint32_t filter, VkMemoryPropertyFlags props) const;

    Device& device_;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;

    // The scene renders here at sceneExtent_, then blits to the swapchain. The
    // UI is drawn afterwards straight onto the swapchain at native size.
    Image sceneColor_;
    Image sceneDepth_;
    VkExtent2D sceneExtent_{};
    float renderScale_ = 1.0f;
    // Camera forward + ortho orbit distance (0 = perspective) for the raster
    // shaders; see setCameraAxis.
    float camFwd_[3] = {0.0f, 0.0f, -1.0f};
    float camOrthoDistance_ = 0.0f;
    std::vector<int> highlightNets_;
    // One RGBA per net: rgb = glow colour, a = 1 when highlighted. Indexed by
    // the per-triangle net id, so any number of nets can glow at once.
    Buffer netColorBuffer_;
    uint32_t netCount_ = 0;
    void uploadNetColors(const std::vector<int>& nets,
                         const std::vector<std::array<float, 3>>& colours);
    float netGlow_ = 3.2f;
    float explodeStep_ = 0.0f;
    float explodeProgress_ = 0.0f;
    float maxRank_ = 0.0f;
    float boardMidZ_ = 0.0f;  // for the translucent-layer sort direction
    VkFormat depthFormat_ = VK_FORMAT_D32_SFLOAT;

    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipelineOpaque_ = VK_NULL_HANDLE;
    VkPipeline pipelineBlend_ = VK_NULL_HANDLE;

    // Screen-space overlay: one host-visible vertex buffer per frame in flight
    // (the CPU rewrites it while the previous frame may still be reading its
    // own -- same reasoning as cpuStaging_).
    VkPipelineLayout overlayLayout_ = VK_NULL_HANDLE;
    VkPipeline overlayPipeline_ = VK_NULL_HANDLE;
    std::vector<Buffer> overlayVb_;
    std::vector<float> overlayTris_;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet materialSet_ = VK_NULL_HANDLE;

    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> commandBuffers_;
    std::vector<VkSemaphore> imageAvailable_;
    std::vector<VkSemaphore> renderFinished_;
    std::vector<VkFence> inFlight_;
    uint32_t frame_ = 0;

    void captureImage(uint32_t imageIndex);
    bool captureScene_ = false;  // capture sceneColor_ instead of the swapchain

    Buffer vertexBuffer_;
    Buffer indexBuffer_;
    Buffer materialBuffer_;
    Buffer triMaterialBuffer_;  // per-triangle material index, for the path tracer
    // Per-triangle NET index (-1 = none), for net highlighting. Same global
    // triangle order as the index buffer; the raster fragment shader reaches
    // it via MaterialGpu::extra[0] + gl_PrimitiveID.
    Buffer triNetBuffer_;
    // The highlighted net as a SAMPLEABLE light: one entry per triangle on
    // that net (three corners + its area), rebuilt whenever the highlight
    // changes. Next-event estimation samples these directly instead of hoping
    // a diffuse bounce stumbles onto a 0.25mm trace -- which it essentially
    // never does, so emission alone lit nothing around it.
    Buffer netLightBuffer_;
    uint32_t netLightCount_ = 0;
    void buildNetLights();
    // CPU copies kept only so buildNetLights can walk the triangles without
    // reading back from the GPU.
    std::vector<uint32_t> restIndices_;
    std::vector<int32_t> triNetCpu_;
    // The BLAS is built from these EXPLODED positions rather than vertexBuffer_
    // (the rest geometry the raster path holds). At rest it is a copy of the rest
    // vertices; in path-traced mode it is re-baked whenever the peel changes so the
    // ray tracer sees the exploded stack. See rebuildTracedGeometry.
    Buffer tracedVertexBuffer_;
    std::vector<geom::Vertex> restVertices_;  // CPU copy, for re-baking the peel
    std::vector<float> vertexRank_;           // per-vertex explode rank
    std::vector<uint32_t> vertexPart_;        // per-vertex part index (visibility bake)
    // Set when part visibility (or anything else the traced geometry bakes in)
    // changes; the next traced frame rebuilds the BLAS so PT and RT shadows stop
    // rendering hidden parts.
    bool tracedVisDirty_ = false;
    float tracedExplode_ = 0.0f;              // peel currently baked into the BLAS
    uint32_t asVertexCount_ = 0;              // counts the BLAS/TLAS were built with
    uint32_t asIndexCount_ = 0;
    // Triangles [0, opaqueTriCount_) are opaque parts; [opaqueTriCount_, end) are
    // the translucent films (mask, substrate). uploadBoard emits them in that
    // order so the BLAS can carry two geometry ranges: the opaque one traverses
    // on the hardware fast path, only the films yield candidates for the path
    // tracer's stochastic alpha. The shader maps geometry-local primitive indices
    // back to global with this base.
    uint32_t opaqueTriCount_ = 0;
    // Re-bake tracedVertexBuffer_ to the given peel and rebuild the BLAS/TLAS over
    // it, then rebind the ray-tracing descriptors. Path-traced mode only.
    void bakeExplode(float progress);
    void rebuildTracedGeometry(float progress);
    std::vector<DrawItem> draws_;
    std::vector<PartInfo> parts_;
    std::vector<MaterialGpu> materials_;  // CPU copy, for live appearance edits

    // --- CPU path tracing (Embree) -----------------------------------------
    // On a CPU device the Vulkan ray-query path is unusable (lavapipe's software
    // BVH silently drops most triangles), so path tracing routes to Embree. The
    // result is copied into sceneColor_ and presented through the SHARED blit +
    // UI + present path, exactly like the raster and GPU-PT scenes.
    bool cpuMode_ = false;
    std::unique_ptr<cpu::CpuTracer> cpuTracer_;
    // One staging buffer PER FRAME IN FLIGHT: the CPU writes the next image
    // while the previous frame's copy may still be executing, so sharing one
    // buffer was a write-during-read race (torn frames).
    std::vector<Buffer> cpuStaging_;
    VkDeviceSize cpuStagingBytes_ = 0;
    uint32_t cpuTracerGen_ = 0xFFFFFFFFu;  // ptGeneration_ the tracer last saw
    uint32_t cpuPass_ = 0;                 // accumulate() pass since last reset
    bool cpuPreviewActive_ = false;        // last pass was the RT preview
    // Last DENOISED display image, shown between denoise milestones so the
    // render thread is not blocked on OIDN every frame (the GPU path does the
    // same asynchronously).
    std::vector<uint8_t> cpuDisplayCache_;
    // The RT-preview integrator converges fast; cap it low so the loop idles.
    int cpuTargetSamples() const {
        return mode_ == RenderMode::PathTraced ? maxSamples() : 32;
    }
    void recordCpuPathTrace(VkCommandBuffer cmd, bool preview);

public:
    // True while a progressive tracer (GPU PT, CPU PT, or the CPU RT preview)
    // still has samples to add -- the caller keeps frames coming while so.
    bool accumulating() const {
        if (cpuMode_) {
            if (!cpuTracer_) return false;
            const bool tracing = mode_ == RenderMode::PathTraced ||
                                 (rtRequested_ && explodeProgress_ < 0.01f);
            return tracing && cpuTracer_->samples() < cpuTargetSamples();
        }
        return mode_ == RenderMode::PathTraced && ptSampleCount_ < ptMaxSamples_;
    }

private:
    std::array<float, 3> substrateColor_ = {0.72f, 0.61f, 0.38f};  // FR4 tan
    float substrateOpacity_ = 1.0f;
    std::array<float, 3> maskColor_ = {0.05f, 0.29f, 0.12f};  // green
    float maskOpacity_ = 0.72f;  // mask albedo.a; drives raster blend + PT show-through
    float componentShine_ = 0.0f;  // 0 matte .. 1 chrome (stylised effect)
    float padShine_ = 0.94f;       // maps to copper roughness 0.5 .. 0.02
    float shadowSoftness_ = 0.15f; // sun radius = s * 8 deg (default 1.2 deg)
    std::string capturePath_;

    // Ray-query RT. Built only when the device advertised ray_query. `blas_` is a
    // bottom-level accel structure over the board's vertex/index buffers; `tlas_`
    // holds one identity instance of it. Traced from board_rt.frag for shadows/AO,
    // gated per-frame by rtRequested_ (and only at rest -- the BLAS is over the
    // un-exploded geometry). See ARCHITECTURE.md "Ray tracing".
    bool rtSupported_ = false;
    bool rtRequested_ = false;
    Buffer blasBuffer_;
    Buffer tlasBuffer_;
    Buffer instanceBuffer_;
    VkAccelerationStructureKHR blas_ = VK_NULL_HANDLE;
    VkAccelerationStructureKHR tlas_ = VK_NULL_HANDLE;
    PFN_vkGetAccelerationStructureBuildSizesKHR pfnAccelSizes_ = nullptr;
    PFN_vkCreateAccelerationStructureKHR pfnCreateAccel_ = nullptr;
    PFN_vkDestroyAccelerationStructureKHR pfnDestroyAccel_ = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR pfnCmdBuildAccel_ = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR pfnAccelAddress_ = nullptr;

    // Path tracer.
    RenderMode mode_ = RenderMode::Raster;
    int ptSampleCount_ = 0;     // samples accumulated at the current camera
    int ptMaxSamples_ = 512;    // stop accumulating past this
    bool rayOrtho_ = false;
    float rayEye_[3] = {0, 0, 0};
    float rayFwd_[3] = {0, 0, 1};
    float rayRight_[3] = {1, 0, 0};
    float rayUp_[3] = {0, 1, 0};
    Image ptAccum_;             // RGBA32F running radiance sum
    Image ptAlbedo_;            // first-hit albedo (denoiser guide)
    Image ptNormal_;            // first-hit normal (denoiser guide)
    Image ptDenoised_;          // OIDN output, shown when valid
    bool ptImagesInitialised_ = false;  // GENERAL-layout transition done
    bool denoisingEnabled_ = false;
    bool ptDenoisedValid_ = false;      // ptDenoised_ holds the current view
    void* oidnDevice_ = nullptr;        // OIDNDevice (opaque; kept out of the header)
    // Cached OIDN filter + device buffers, reused across denoises. Creating and
    // COMMITTING a filter every call reloads/recompiles the network to the GPU --
    // seconds each -- so they persist and only rebuild when the resolution changes.
    void* oidnFilter_ = nullptr;
    void* oidnAlbedoFilter_ = nullptr;  // aux prefilters (cleanAux mode)
    void* oidnNormalFilter_ = nullptr;
    void* oidnColorBuf_ = nullptr;
    void* oidnAlbedoBuf_ = nullptr;
    void* oidnNormalBuf_ = nullptr;
    void* oidnOutBuf_ = nullptr;
    uint32_t oidnW_ = 0, oidnH_ = 0;

    // --- Asynchronous denoise state machine --------------------------------
    // Idle -> (fenced GPU->host readback submitted, NOT waited) Reading ->
    // (worker thread packs guides + runs OIDN) Filtering -> writeback ->
    // Idle. ptGeneration_ stamps a pass at kickoff: if the accumulation
    // restarts (camera moved) before the pass lands, the stale result is
    // DISCARDED instead of ghosting the old view onto the new one.
    enum class DenoiseState { Idle, Reading, Filtering };
    DenoiseState dnState_ = DenoiseState::Idle;
    VkFence dnFence_ = VK_NULL_HANDLE;
    VkCommandBuffer dnCmd_ = VK_NULL_HANDLE;
    Buffer dnColorBuf_, dnAlbedoBuf_, dnNormalBuf_, dnOutBuf_;
    void* dnColorPtr_ = nullptr;   // persistent maps of the four host buffers
    void* dnAlbedoPtr_ = nullptr;
    void* dnNormalPtr_ = nullptr;
    void* dnOutPtr_ = nullptr;
    uint32_t dnW_ = 0, dnH_ = 0;
    int dnSamples_ = 0;    // sample count captured at kickoff
    int dnDisplayed_ = 0;  // sample count of the pass currently on screen
    uint32_t dnGen_ = 0;
    uint32_t ptGeneration_ = 0;
    std::thread dnThread_;
    std::atomic<bool> dnThreadDone_{false};
    void ensureDenoiseBuffers(uint32_t w, uint32_t h);
    void denoiseWorker(uint32_t w, uint32_t h, int samples);
    void abortDenoise();  // wait/join and discard any in-flight pass
    // Every accumulation restart goes through here so in-flight denoises know
    // their input became stale.
    void resetAccumulation() {
        ptSampleCount_ = 0;
        ptDenoisedValid_ = false;
        dnDisplayed_ = 0;
        ++ptGeneration_;
    }
    VkDescriptorSetLayout ptSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout tonemapSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool ptPool_ = VK_NULL_HANDLE;
    VkDescriptorSet ptSet_ = VK_NULL_HANDLE;
    VkDescriptorSet tonemapSet_ = VK_NULL_HANDLE;
    VkPipelineLayout ptLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout tonemapLayout_ = VK_NULL_HANDLE;
    VkPipeline ptPipeline_ = VK_NULL_HANDLE;
    VkPipeline tonemapPipeline_ = VK_NULL_HANDLE;

    FrameStats stats_;
};

}  // namespace pcbview::vk
