#include "render/vk/renderer.h"

#include <OpenImageDenoise/oidn.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <vector>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace {

// SPIR-V embedded at build time by glslc -mfmt=c. No .spv files to ship.
const uint32_t kBoardVert[] =
#include "shaders/board.vert.inc"
    ;
const uint32_t kBoardFrag[] =
#include "shaders/board.frag.inc"
    ;
// Raster + ray-query shadows/AO. Used instead of kBoardFrag when the device has
// ray_query; it declares the RayQueryKHR capability, which a non-RT device could
// not load -- hence a separate module rather than one branchy shader.
const uint32_t kBoardFragRt[] =
#include "shaders/board_rt.frag.inc"
    ;
const uint32_t kPathTrace[] =
#include "shaders/pathtrace.comp.inc"
    ;
const uint32_t kFullscreenVert[] =
#include "shaders/fullscreen.vert.inc"
    ;
const uint32_t kTonemapFrag[] =
#include "shaders/tonemap.frag.inc"
    ;
const uint32_t kOverlayVert[] =
#include "shaders/overlay.vert.inc"
    ;
const uint32_t kOverlayFrag[] =
#include "shaders/overlay.frag.inc"
    ;
const uint32_t kBloomExtract[] =
#include "shaders/bloom_extract.frag.inc"
    ;
const uint32_t kBloomComposite[] =
#include "shaders/bloom_composite.frag.inc"
    ;

constexpr uint32_t kFramesInFlight = 2;

// set 0 binding for the per-triangle net buffer. Binding 1 is the TLAS on an
// RT device and unused otherwise, so 2 is free either way and the fragment
// shaders can hard-code it.
constexpr uint32_t kNetBinding = 2;
// Per-net glow colour (rgb + highlighted flag), indexed by the triangle's net.
constexpr uint32_t kNetColorBinding = 3;

struct PtPush {
    float eye[4];    // xyz, w = sampleIndex
    float fwd[4];
    float right[4];
    float up[4];
    uint32_t dim[4]; // width, height, maxDepth, frameSeed
    float misc[4];   // x = peel amount 0..1 (substrate fade), yzw unused
    // x = first translucent-film triangle (global index) -- the shader re-bases
    // BLAS geometry 1's geometry-local primitive indices with this.
    uint32_t counts[4];
};
struct TonemapPush {
    uint32_t dim[2];
    uint32_t sampleCount;
    uint32_t flags;
};

// Round `v` up to a multiple of `a` (a power of two).
inline VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a) {
    return (v + a - 1) & ~(a - 1);
}

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        throw std::runtime_error(std::string(what) + " failed: VkResult " +
                                 std::to_string(static_cast<int>(r)));
    }
}

// Push constants must match the layout declared in both shader stages.
struct PushConstants {
    float viewProj[16];
    float cameraPos[4];
    float params[4];  // x = explode distance (mm per rank)
    // xyz = camera forward (unit, eye -> target).
    // w   = 0 in perspective; the ORBIT DISTANCE in orthographic.
    //
    // The fragment shaders need to know the projection kind. A parallel
    // projection has ONE view direction for every fragment; deriving it from
    // the eye POINT (the perspective shortcut) reverses for fragments at or
    // behind the eye plane -- which ortho happily shows -- and that flipped
    // vector then flips the normal and swings the whole camera-relative light
    // rig, rendering the board's near edge black.
    float camAxis[4];
    // x = highlighted net index, -1 for none. Total push size is 128 bytes,
    // exactly the Vulkan minimum guarantee -- do not add another vec4 without
    // checking maxPushConstantsSize.
    int32_t highlight[4];
};

}  // namespace

namespace pcbview::vk {

Renderer::Renderer(Device& device, VkSurfaceKHR surface, uint32_t width,
                   uint32_t height)
    : device_(device), surface_(surface) {
    // RT support is fixed for this device's lifetime; it decides the descriptor
    // layout (a TLAS binding) and which fragment shader the pipelines use, so it
    // must be known before createDescriptors()/createPipeline().
    rtSupported_ = device_.rayQueryEnabled;
    if (rtSupported_) loadRtFunctions();

    // On a CPU device, path tracing runs on Embree instead of Vulkan ray query.
    cpuMode_ = device_.gpu.type == VK_PHYSICAL_DEVICE_TYPE_CPU;
    if (cpuMode_) cpuTracer_ = std::make_unique<cpu::CpuTracer>();

    createSwapchain(width, height);
    createSceneTargets();
    createDescriptors();
    createPipeline();
    createOverlayPipeline();
    createBloomPipelines();
    createBloomTargets();  // scene targets exist by now
    if (rtSupported_) createPathTracer();
    createSyncAndCommands();
}

Renderer::~Renderer() {
    if (device_.handle == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(device_.handle);

    if (rtSupported_) destroyPathTracer();
    destroyAccelerationStructures();
    destroyBuffer(vertexBuffer_);
    destroyBuffer(indexBuffer_);
    destroyBuffer(materialBuffer_);
    destroyBuffer(triMaterialBuffer_);
    destroyBuffer(triNetBuffer_);
    destroyBuffer(netColorBuffer_);
    destroyBuffer(netLightBuffer_);
    destroyBuffer(tracedVertexBuffer_);
    destroySceneTargets();

    for (VkSemaphore s : imageAvailable_) vkDestroySemaphore(device_.handle, s, nullptr);
    for (VkSemaphore s : renderFinished_) vkDestroySemaphore(device_.handle, s, nullptr);
    for (VkFence f : inFlight_) vkDestroyFence(device_.handle, f, nullptr);
    if (commandPool_) vkDestroyCommandPool(device_.handle, commandPool_, nullptr);

    if (pipelineOpaque_) vkDestroyPipeline(device_.handle, pipelineOpaque_, nullptr);
    if (pipelineBlend_) vkDestroyPipeline(device_.handle, pipelineBlend_, nullptr);
    destroyBloom();
    if (overlayPipeline_) vkDestroyPipeline(device_.handle, overlayPipeline_, nullptr);
    if (overlayLayout_) vkDestroyPipelineLayout(device_.handle, overlayLayout_, nullptr);
    for (Buffer& b : overlayVb_) destroyBuffer(b);
    if (layout_) vkDestroyPipelineLayout(device_.handle, layout_, nullptr);
    if (descriptorPool_) vkDestroyDescriptorPool(device_.handle, descriptorPool_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(device_.handle, setLayout_, nullptr);

    destroySwapchain();
}

void Renderer::loadRtFunctions() {
    auto load = [&](const char* name) {
        return vkGetDeviceProcAddr(device_.handle, name);
    };
    pfnAccelSizes_ =
        reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
            load("vkGetAccelerationStructureBuildSizesKHR"));
    pfnCreateAccel_ = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(
        load("vkCreateAccelerationStructureKHR"));
    pfnDestroyAccel_ = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(
        load("vkDestroyAccelerationStructureKHR"));
    pfnCmdBuildAccel_ =
        reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
            load("vkCmdBuildAccelerationStructuresKHR"));
    pfnAccelAddress_ =
        reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
            load("vkGetAccelerationStructureDeviceAddressKHR"));

    // If any pointer failed to resolve, disable RT rather than crash later.
    if (!pfnAccelSizes_ || !pfnCreateAccel_ || !pfnDestroyAccel_ ||
        !pfnCmdBuildAccel_ || !pfnAccelAddress_) {
        rtSupported_ = false;
    }
}

VkDeviceAddress Renderer::bufferAddress(const Buffer& b) const {
    VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = b.handle;
    return vkGetBufferDeviceAddress(device_.handle, &info);
}

void Renderer::destroyAccelerationStructures() {
    if (tlas_) { pfnDestroyAccel_(device_.handle, tlas_, nullptr); tlas_ = VK_NULL_HANDLE; }
    if (blas_) { pfnDestroyAccel_(device_.handle, blas_, nullptr); blas_ = VK_NULL_HANDLE; }
    destroyBuffer(tlasBuffer_);
    destroyBuffer(blasBuffer_);
    destroyBuffer(instanceBuffer_);
}

// Build a BLAS over the board's vertex/index buffers and a TLAS with one identity
// instance of it. The board is static per upload, so this is a full rebuild each
// uploadBoard rather than an animated refit -- the exploded view instead gates RT
// off (its geometry is displaced in the vertex shader, not in these buffers).
void Renderer::buildAccelerationStructures(uint32_t vertexCount,
                                           uint32_t indexCount) {
    if (!rtSupported_ || indexCount < 3) return;
    asVertexCount_ = vertexCount;
    asIndexCount_ = indexCount;
    destroyAccelerationStructures();

    // Scratch must be aligned to this device-specific value.
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 props2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &asProps;
    vkGetPhysicalDeviceProperties2(device_.gpu.handle, &props2);
    const VkDeviceSize scratchAlign =
        std::max<VkDeviceSize>(asProps.minAccelerationStructureScratchOffsetAlignment, 256);

    const VkBufferUsageFlags asStore =
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const VkBufferUsageFlags scratchUsage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    auto oneTime = [&](const std::function<void(VkCommandBuffer)>& record) {
        VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        a.commandPool = commandPool_;
        a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        a.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        check(vkAllocateCommandBuffers(device_.handle, &a, &cmd), "alloc(as)");
        VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &b);
        record(cmd);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        s.commandBufferCount = 1;
        s.pCommandBuffers = &cmd;
        check(vkQueueSubmit(device_.graphicsQueue, 1, &s, VK_NULL_HANDLE), "submit(as)");
        vkQueueWaitIdle(device_.graphicsQueue);
        vkFreeCommandBuffers(device_.handle, commandPool_, 1, &cmd);
    };

    // ---- BLAS: TWO geometry ranges over the same buffers ----
    //
    // uploadBoard emits opaque parts' triangles first, the translucent films
    // (mask, substrate) last, so one contiguous split serves both worlds:
    // geometry 0 carries VK_GEOMETRY_OPAQUE_BIT_KHR and traverses entirely on
    // the hardware fast path; geometry 1 (the films) omits it, so ONLY film
    // hits reach the path tracer's candidate loop for stochastic alpha. The
    // earlier all-in-one opaque geometry forced gl_RayFlagsNoOpaqueEXT in the
    // shader, which pushed EVERY triangle along every ray through the shader --
    // the single biggest PT cost, for identical output. Primitive indices are
    // geometry-LOCAL; the shader re-bases geometry 1 by opaqueTriCount_.
    const uint32_t totalTris = indexCount / 3;
    const uint32_t opaqueTris = std::min(opaqueTriCount_, totalTris);
    const uint32_t filmTris = totalTris - opaqueTris;

    VkAccelerationStructureGeometryKHR geoms[2]{};
    VkAccelerationStructureBuildRangeInfoKHR blasRanges[2]{};
    uint32_t maxPrims[2]{};
    uint32_t geomCount = 0;
    const auto addGeometry = [&](VkGeometryFlagsKHR flags, uint32_t firstTri,
                                 uint32_t triCount) {
        if (triCount == 0) return;
        VkAccelerationStructureGeometryKHR& g = geoms[geomCount];
        g = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.flags = flags;
        auto& t = g.geometry.triangles;
        t.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        t.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        // Built from the exploded copy, not vertexBuffer_ (the rest geometry). At
        // rest the two are identical; rebuildTracedGeometry re-bakes on peel.
        t.vertexData.deviceAddress = bufferAddress(tracedVertexBuffer_);
        t.vertexStride = sizeof(geom::Vertex);
        t.maxVertex = vertexCount - 1;
        t.indexType = VK_INDEX_TYPE_UINT32;
        t.indexData.deviceAddress = bufferAddress(indexBuffer_);
        blasRanges[geomCount].primitiveCount = triCount;
        // For indexed triangles this is a BYTE offset into the index buffer.
        blasRanges[geomCount].primitiveOffset =
            firstTri * 3u * static_cast<uint32_t>(sizeof(uint32_t));
        maxPrims[geomCount] = triCount;
        ++geomCount;
    };
    addGeometry(VK_GEOMETRY_OPAQUE_BIT_KHR, 0, opaqueTris);
    addGeometry(0, opaqueTris, filmTris);
    if (geomCount == 0) return;

    VkAccelerationStructureBuildGeometryInfoKHR blasBuild{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    blasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    blasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    blasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    blasBuild.geometryCount = geomCount;
    blasBuild.pGeometries = geoms;

    VkAccelerationStructureBuildSizesInfoKHR blasSizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    pfnAccelSizes_(device_.handle,
                   VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &blasBuild,
                   maxPrims, &blasSizes);

    blasBuffer_ = createBuffer(blasSizes.accelerationStructureSize, asStore,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkAccelerationStructureCreateInfoKHR blasCreate{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    blasCreate.buffer = blasBuffer_.handle;
    blasCreate.size = blasSizes.accelerationStructureSize;
    blasCreate.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    check(pfnCreateAccel_(device_.handle, &blasCreate, nullptr, &blas_),
          "vkCreateAccelerationStructureKHR(blas)");

    Buffer blasScratch = createBuffer(blasSizes.buildScratchSize + scratchAlign,
                                      scratchUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    blasBuild.dstAccelerationStructure = blas_;
    blasBuild.scratchData.deviceAddress =
        alignUp(bufferAddress(blasScratch), scratchAlign);

    const VkAccelerationStructureBuildRangeInfoKHR* pBlasRange = blasRanges;
    oneTime([&](VkCommandBuffer cmd) {
        pfnCmdBuildAccel_(cmd, 1, &blasBuild, &pBlasRange);
    });
    destroyBuffer(blasScratch);

    // ---- TLAS: one identity instance ----
    VkAccelerationStructureDeviceAddressInfoKHR addrInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    addrInfo.accelerationStructure = blas_;
    const VkDeviceAddress blasAddr =
        pfnAccelAddress_(device_.handle, &addrInfo);

    VkAccelerationStructureInstanceKHR inst{};
    inst.transform.matrix[0][0] = 1.0f;
    inst.transform.matrix[1][1] = 1.0f;
    inst.transform.matrix[2][2] = 1.0f;
    inst.mask = 0xFF;
    inst.accelerationStructureReference = blasAddr;

    instanceBuffer_ = createBuffer(
        sizeof(inst),
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    uploadViaStaging(instanceBuffer_, &inst, sizeof(inst));

    VkAccelerationStructureGeometryKHR tlasGeom{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tlasGeom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    tlasGeom.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    tlasGeom.geometry.instances.data.deviceAddress = bufferAddress(instanceBuffer_);

    VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    tlasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    tlasBuild.geometryCount = 1;
    tlasBuild.pGeometries = &tlasGeom;

    const uint32_t instCount = 1;
    VkAccelerationStructureBuildSizesInfoKHR tlasSizes{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    pfnAccelSizes_(device_.handle,
                   VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &tlasBuild,
                   &instCount, &tlasSizes);

    tlasBuffer_ = createBuffer(tlasSizes.accelerationStructureSize, asStore,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkAccelerationStructureCreateInfoKHR tlasCreate{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    tlasCreate.buffer = tlasBuffer_.handle;
    tlasCreate.size = tlasSizes.accelerationStructureSize;
    tlasCreate.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    check(pfnCreateAccel_(device_.handle, &tlasCreate, nullptr, &tlas_),
          "vkCreateAccelerationStructureKHR(tlas)");

    Buffer tlasScratch = createBuffer(tlasSizes.buildScratchSize + scratchAlign,
                                      scratchUsage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    tlasBuild.dstAccelerationStructure = tlas_;
    tlasBuild.scratchData.deviceAddress =
        alignUp(bufferAddress(tlasScratch), scratchAlign);

    VkAccelerationStructureBuildRangeInfoKHR tlasRange{};
    tlasRange.primitiveCount = instCount;
    const VkAccelerationStructureBuildRangeInfoKHR* pTlasRange = &tlasRange;
    oneTime([&](VkCommandBuffer cmd) {
        pfnCmdBuildAccel_(cmd, 1, &tlasBuild, &pTlasRange);
    });
    destroyBuffer(tlasScratch);
}

// Re-bake tracedVertexBuffer_ to `progress`: apply each vertex's rigid peel
// offset (identical to board.vert's travel()) on the CPU and stage it up. Cheap
// enough that path tracing can rebuild the BLAS from it on every peel change.
void Renderer::bakeExplode(float progress) {
    if (restVertices_.empty() || vertexRank_.size() != restVertices_.size())
        return;

    // Effective traced visibility per part: the user's stackup toggles PLUS the
    // collapsed-board rule the raster path applies (inner copper hidden at rest
    // while the substrate is opaque). Baking the same rule keeps PT and RT
    // shadows in lockstep with what raster actually draws.
    std::vector<uint8_t> vis(parts_.size(), 1);
    const bool peeling = progress > 0.0f;
    const bool substrateOpaque = substrateOpacity_ >= 0.999f;
    for (const DrawItem& item : draws_) {
        bool v = item.part < parts_.size() ? parts_[item.part].visible : true;
        if (item.hideWhenCollapsed && !peeling && substrateOpaque) v = false;
        if (item.material < vis.size()) vis[item.material] = v ? 1 : 0;
    }

    // A hidden part's vertices are set to NaN: the Vulkan spec defines a
    // triangle with a NaN vertex X as INACTIVE, so the BLAS build simply drops
    // it -- no index surgery, no shader change, and the geometry ranges (and
    // therefore the primitive re-basing) stay exactly as uploadBoard laid them.
    const float kHide = std::numeric_limits<float>::quiet_NaN();
    std::vector<geom::Vertex> exploded = restVertices_;
    for (size_t i = 0; i < exploded.size(); ++i) {
        const uint32_t part = vertexPart_[i];
        if (part < vis.size() && !vis[part]) {
            exploded[i].position[0] = kHide;
            continue;
        }
        const float rank = vertexRank_[i];
        const float ring = std::abs(rank);
        const float travel =
            std::max(progress - (maxRank_ - ring), 0.0f) * explodeStep_;
        const float sign = rank > 0.0f ? 1.0f : (rank < 0.0f ? -1.0f : 0.0f);
        exploded[i].position[2] += sign * travel;
    }
    uploadViaStaging(tracedVertexBuffer_, exploded.data(),
                     exploded.size() * sizeof(geom::Vertex));
}

void Renderer::setPartVisible(const std::string& name, bool visible) {
    bool changed = false;
    for (PartInfo& p : parts_) {
        if (p.name == name && p.visible != visible) {
            p.visible = visible;
            changed = true;
        }
    }
    if (!changed) return;
    // The traced geometry (PT and RT shadows) bakes visibility into the BLAS;
    // rebuild on the next traced frame, and restart the path-trace accumulation
    // so the change shows immediately rather than after the next camera move.
    tracedVisDirty_ = true;
    // Mirror to the Embree tracer (CPU device) -- its next bake drops the part.
    if (cpuMode_ && cpuTracer_) cpuTracer_->setPartVisible(name, visible);
    resetAccumulation();
    ptDenoisedValid_ = false;
}

// Bake the peel into the traced geometry, rebuild the BLAS/TLAS over it, and
// rebind the ray-tracing descriptors. Path-traced mode calls this whenever the
// peel changes so the ray tracer sees the exploded stack (the raster path
// explodes in its vertex shader instead, and RT-raster shadows stay gated at
// rest -- rebuilding the AS every frame is only affordable for non-real-time PT).
void Renderer::rebuildTracedGeometry(float progress) {
    if (!rtSupported_ || restVertices_.empty()) return;
    // The BLAS/TLAS may still be referenced by an in-flight frame; the build
    // below destroys and recreates them, so drain the device first.
    vkDeviceWaitIdle(device_.handle);

    bakeExplode(progress);
    buildAccelerationStructures(asVertexCount_, asIndexCount_);
    tracedExplode_ = progress;

    // Rebind the new TLAS handle everywhere it is referenced: the raster RT
    // fragment shader (material set, binding 1) and the path tracer.
    if (tlas_ != VK_NULL_HANDLE) {
        VkWriteDescriptorSetAccelerationStructureKHR asInfo{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        asInfo.accelerationStructureCount = 1;
        asInfo.pAccelerationStructures = &tlas_;
        VkWriteDescriptorSet asWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        asWrite.pNext = &asInfo;
        asWrite.dstSet = materialSet_;
        asWrite.dstBinding = 1;
        asWrite.descriptorCount = 1;
        asWrite.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        vkUpdateDescriptorSets(device_.handle, 1, &asWrite, 0, nullptr);
    }
    updatePathTraceDescriptors();  // rebinds PT TLAS + resets accumulation
}

void Renderer::setRenderMode(RenderMode m) {
    if (m == mode_) return;
    mode_ = m;
    resetAccumulation();
}

void Renderer::setRayCamera(const float eye[3], const float fwd[3],
                            const float right[3], const float up[3],
                            bool ortho) {
    // Any camera change restarts accumulation.
    auto diff = [](const float a[3], const float b[3]) {
        return std::abs(a[0]-b[0]) + std::abs(a[1]-b[1]) + std::abs(a[2]-b[2]) > 1e-5f;
    };
    if (diff(eye, rayEye_) || diff(fwd, rayFwd_) || diff(right, rayRight_) ||
        diff(up, rayUp_) || ortho != rayOrtho_) {
        resetAccumulation();
        // Crucial: also drop the denoised frame. Otherwise the tonemap keeps
        // showing the last (now stale) denoised image while the camera moves --
        // the screen freezes on it and only updates when the next denoise lands,
        // which reads as a slideshow. Clearing it makes movement show the live
        // (noisy but real-time) accumulation, and denoise resumes once still.
        ptDenoisedValid_ = false;
    }
    for (int i = 0; i < 3; ++i) {
        rayEye_[i] = eye[i]; rayFwd_[i] = fwd[i];
        rayRight_[i] = right[i]; rayUp_[i] = up[i];
    }
    rayOrtho_ = ortho;
}

namespace {
VkShaderModule makeModule(VkDevice dev, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &info, nullptr, &m) != VK_SUCCESS)
        throw std::runtime_error("vkCreateShaderModule (path tracer)");
    return m;
}
}  // namespace

void Renderer::createPathTracer() {
    // Compute set: 0 TLAS, 1 vtx, 2 idx, 3 triMat, 4 mats, 5 accum, 6 albedo,
    // 7 normal.
    // 8 = per-triangle net, for the highlight glow.
    // 9 = the highlighted net as a sampleable area light (next-event
    // estimation), so it actually illuminates its surroundings.
    VkDescriptorSetLayoutBinding b[11]{};
    for (uint32_t i = 0; i < 11; ++i) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    for (int i = 1; i <= 4; ++i) b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    for (int i = 5; i <= 7; ++i) b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    VkDescriptorSetLayoutCreateInfo li{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 11;
    li.pBindings = b;
    check(vkCreateDescriptorSetLayout(device_.handle, &li, nullptr, &ptSetLayout_),
          "pt set layout");

    // Tonemap reads binding 0 (raw accumulation) or 1 (OIDN-denoised); a push
    // flag chooses.
    VkDescriptorSetLayoutBinding tb[2]{};
    for (int i = 0; i < 2; ++i) {
        tb[i].binding = i;
        tb[i].descriptorCount = 1;
        tb[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        tb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo tli{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    tli.bindingCount = 2;
    tli.pBindings = tb;
    check(vkCreateDescriptorSetLayout(device_.handle, &tli, nullptr,
                                      &tonemapSetLayout_),
          "tonemap set layout");

    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7},  // + netColour
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5}};  // 3 pt + 2 tonemap
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 2;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    check(vkCreateDescriptorPool(device_.handle, &pi, nullptr, &ptPool_), "pt pool");

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = ptPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &ptSetLayout_;
    check(vkAllocateDescriptorSets(device_.handle, &ai, &ptSet_), "pt set");
    ai.pSetLayouts = &tonemapSetLayout_;
    check(vkAllocateDescriptorSets(device_.handle, &ai, &tonemapSet_), "tonemap set");

    // Compute pipeline.
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PtPush)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &ptSetLayout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    check(vkCreatePipelineLayout(device_.handle, &pl, nullptr, &ptLayout_),
          "pt pipeline layout");

    VkShaderModule cs = makeModule(device_.handle, kPathTrace, sizeof(kPathTrace));
    VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = cs;
    cpi.stage.pName = "main";
    cpi.layout = ptLayout_;
    check(vkCreateComputePipelines(device_.handle, VK_NULL_HANDLE, 1, &cpi, nullptr,
                                   &ptPipeline_),
          "pt compute pipeline");
    vkDestroyShaderModule(device_.handle, cs, nullptr);

    // Tonemap graphics pipeline: fullscreen triangle, no vertex input, no depth,
    // dynamic rendering into the scene colour format.
    VkPushConstantRange tpc{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(TonemapPush)};
    VkPipelineLayoutCreateInfo tpl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    tpl.setLayoutCount = 1;
    tpl.pSetLayouts = &tonemapSetLayout_;
    tpl.pushConstantRangeCount = 1;
    tpl.pPushConstantRanges = &tpc;
    check(vkCreatePipelineLayout(device_.handle, &tpl, nullptr, &tonemapLayout_),
          "tonemap pipeline layout");

    VkShaderModule vs = makeModule(device_.handle, kFullscreenVert, sizeof(kFullscreenVert));
    VkShaderModule fs = makeModule(device_.handle, kTonemapFrag, sizeof(kTonemapFrag));
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vin{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rend{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rend.colorAttachmentCount = 1;
    rend.pColorAttachmentFormats = &colorFormat_;

    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.pNext = &rend;
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vin;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cb;
    gpi.pDynamicState = &ds;
    gpi.layout = tonemapLayout_;
    check(vkCreateGraphicsPipelines(device_.handle, VK_NULL_HANDLE, 1, &gpi, nullptr,
                                    &tonemapPipeline_),
          "tonemap pipeline");
    vkDestroyShaderModule(device_.handle, vs, nullptr);
    vkDestroyShaderModule(device_.handle, fs, nullptr);

    // OIDN device for denoising. DEFAULT picks the fastest available -- a GPU
    // (CUDA on NVIDIA, HIP on AMD) when its device DLL + driver are present, else
    // the CPU. If a GPU device fails to commit, fall back to CPU so denoising
    // still works. Kept around for the process; filters are per-call.
    OIDNDevice dev = oidnNewDevice(OIDN_DEVICE_TYPE_DEFAULT);
    oidnCommitDevice(dev);
    const char* err = nullptr;
    if (oidnGetDeviceError(dev, &err) != OIDN_ERROR_NONE) {
        oidnReleaseDevice(dev);
        dev = oidnNewDevice(OIDN_DEVICE_TYPE_CPU);
        oidnCommitDevice(dev);
    }
    oidnDevice_ = dev;
}

void Renderer::destroyPathTracer() {
    // Drain the async denoise BEFORE touching anything it uses: the worker
    // thread owns the OIDN objects while running, and the fenced readback
    // references the pt images and host buffers.
    abortDenoise();
    const auto destroyMapped = [&](Buffer& b, void*& p) {
        if (p) { vkUnmapMemory(device_.handle, b.memory); p = nullptr; }
        destroyBuffer(b);
    };
    destroyMapped(dnColorBuf_, dnColorPtr_);
    destroyMapped(dnAlbedoBuf_, dnAlbedoPtr_);
    destroyMapped(dnNormalBuf_, dnNormalPtr_);
    destroyMapped(dnOutBuf_, dnOutPtr_);
    dnW_ = dnH_ = 0;
    if (dnFence_) {
        vkDestroyFence(device_.handle, dnFence_, nullptr);
        dnFence_ = VK_NULL_HANDLE;
    }
    if (oidnFilter_) oidnReleaseFilter(static_cast<OIDNFilter>(oidnFilter_));
    if (oidnAlbedoFilter_)
        oidnReleaseFilter(static_cast<OIDNFilter>(oidnAlbedoFilter_));
    if (oidnNormalFilter_)
        oidnReleaseFilter(static_cast<OIDNFilter>(oidnNormalFilter_));
    if (oidnColorBuf_) oidnReleaseBuffer(static_cast<OIDNBuffer>(oidnColorBuf_));
    if (oidnAlbedoBuf_) oidnReleaseBuffer(static_cast<OIDNBuffer>(oidnAlbedoBuf_));
    if (oidnNormalBuf_) oidnReleaseBuffer(static_cast<OIDNBuffer>(oidnNormalBuf_));
    if (oidnOutBuf_) oidnReleaseBuffer(static_cast<OIDNBuffer>(oidnOutBuf_));
    oidnFilter_ = oidnAlbedoFilter_ = oidnNormalFilter_ = nullptr;
    oidnColorBuf_ = oidnAlbedoBuf_ = oidnNormalBuf_ = oidnOutBuf_ = nullptr;
    oidnW_ = oidnH_ = 0;
    if (oidnDevice_) {
        oidnReleaseDevice(static_cast<OIDNDevice>(oidnDevice_));
        oidnDevice_ = nullptr;
    }
    if (ptPipeline_) vkDestroyPipeline(device_.handle, ptPipeline_, nullptr);
    if (tonemapPipeline_) vkDestroyPipeline(device_.handle, tonemapPipeline_, nullptr);
    if (ptLayout_) vkDestroyPipelineLayout(device_.handle, ptLayout_, nullptr);
    if (tonemapLayout_) vkDestroyPipelineLayout(device_.handle, tonemapLayout_, nullptr);
    if (ptPool_) vkDestroyDescriptorPool(device_.handle, ptPool_, nullptr);
    if (ptSetLayout_) vkDestroyDescriptorSetLayout(device_.handle, ptSetLayout_, nullptr);
    if (tonemapSetLayout_) vkDestroyDescriptorSetLayout(device_.handle, tonemapSetLayout_, nullptr);
    ptPipeline_ = tonemapPipeline_ = VK_NULL_HANDLE;
    ptLayout_ = tonemapLayout_ = VK_NULL_HANDLE;
    ptPool_ = VK_NULL_HANDLE;
    ptSetLayout_ = tonemapSetLayout_ = VK_NULL_HANDLE;
    destroyImage(ptAccum_);
    destroyImage(ptAlbedo_);
    destroyImage(ptNormal_);
    destroyImage(ptDenoised_);
}

void Renderer::updatePathTraceDescriptors() {
    if (!ptSet_ || vertexBuffer_.handle == VK_NULL_HANDLE) return;

    VkWriteDescriptorSetAccelerationStructureKHR asInfo{
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    asInfo.accelerationStructureCount = 1;
    asInfo.pAccelerationStructures = &tlas_;

    VkDescriptorBufferInfo vtx{vertexBuffer_.handle, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo idx{indexBuffer_.handle, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo tri{triMaterialBuffer_.handle, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo mat{materialBuffer_.handle, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo net{triNetBuffer_.handle, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo acc{VK_NULL_HANDLE, ptAccum_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo alb{VK_NULL_HANDLE, ptAlbedo_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo nrm{VK_NULL_HANDLE, ptNormal_.view, VK_IMAGE_LAYOUT_GENERAL};

    VkWriteDescriptorSet w[11]{};
    for (int i = 0; i < 11; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = ptSet_;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
    }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    w[0].pNext = &asInfo;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &vtx;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[2].pBufferInfo = &idx;
    w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[3].pBufferInfo = &tri;
    w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[4].pBufferInfo = &mat;
    w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[5].pImageInfo = &acc;
    w[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[6].pImageInfo = &alb;
    w[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[7].pImageInfo = &nrm;
    w[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[8].pBufferInfo = &net;
    // The net light list only exists while a net is highlighted; point the
    // binding at the triangle-net buffer as a harmless stand-in otherwise, so
    // the descriptor is never null.
    VkDescriptorBufferInfo lights{
        netLightBuffer_.handle != VK_NULL_HANDLE ? netLightBuffer_.handle
                                                 : triNetBuffer_.handle,
        0, VK_WHOLE_SIZE};
    w[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[9].pBufferInfo = &lights;
    VkDescriptorBufferInfo ncol{netColorBuffer_.handle != VK_NULL_HANDLE ? netColorBuffer_.handle : triNetBuffer_.handle, 0, VK_WHOLE_SIZE};
    w[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[10].pBufferInfo = &ncol;
    vkUpdateDescriptorSets(device_.handle, 11, w, 0, nullptr);

    VkDescriptorImageInfo den{VK_NULL_HANDLE, ptDenoised_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet tw[2]{};
    for (int i = 0; i < 2; ++i) {
        tw[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        tw[i].dstSet = tonemapSet_;
        tw[i].dstBinding = i;
        tw[i].descriptorCount = 1;
        tw[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    tw[0].pImageInfo = &acc;
    tw[1].pImageInfo = &den;
    vkUpdateDescriptorSets(device_.handle, 2, tw, 0, nullptr);
    resetAccumulation();
    ptDenoisedValid_ = false;
}

void Renderer::recordCpuPathTrace(VkCommandBuffer cmd, bool preview) {
    // Restart accumulation when the view changed (resetAccumulation bumps
    // ptGeneration_ on any camera move, exactly like the GPU path), or when the
    // integrator switched between full path tracing and the RT preview.
    if (cpuTracerGen_ != ptGeneration_ || cpuPreviewActive_ != preview) {
        cpuTracerGen_ = ptGeneration_;
        cpuPreviewActive_ = preview;
        cpuPass_ = 0;
        cpuDisplayCache_.clear();  // the old view is stale
    }
    cpuTracer_->setPreview(preview);

    // Re-bake the exploded geometry (rebuilds the Embree BVH) when the peel
    // moved; that restarts accumulation just like a camera change.
    if (cpuTracer_->setExplode(explodeProgress_, explodeStep_)) {
        cpuPass_ = 0;
        cpuDisplayCache_.clear();
    }

    cpu::TraceCamera cam;
    for (int i = 0; i < 3; ++i) {
        cam.eye[i] = rayEye_[i];
        cam.fwd[i] = rayFwd_[i];
        cam.right[i] = rayRight_[i];
        cam.up[i] = rayUp_[i];
    }
    cam.ortho = rayOrtho_;

    // A few samples per frame keeps the UI responsive while the image converges
    // over successive frames. Stop adding samples once converged -- redraws
    // (focus changes, overlays) must not burn CPU re-tracing a finished image.
    const int target = cpuTargetSamples();
    if (cpuTracer_->samples() < target || cpuPass_ == 0) {
        const int batch = 4;
        cpuTracer_->accumulate(cam, sceneExtent_.width, sceneExtent_.height,
                               batch, cpuPass_);
        ++cpuPass_;
    }

    // Denoising (OIDN, on this thread) is expensive, so run it at sample
    // milestones -- powers of two early for fast cleanup, then every 32 so the
    // late convergence still shows progress -- and at convergence. Between
    // milestones keep showing the last denoised image and accumulate silently
    // underneath. The RT preview skips OIDN entirely: its integrator is
    // near-noise-free by 8 samples, so it just refreshes every frame.
    const int s = cpuTracer_->samples();
    const bool milestone = s <= 32 ? (s & (s - 1)) == 0 : (s % 32) == 0;
    const bool converged = s >= target;
    const bool wantDenoise = !preview && denoisingEnabled_ && s >= 4 &&
                             (milestone || converged);
    const bool refresh =
        wantDenoise || preview || !denoisingEnabled_ || cpuDisplayCache_.empty();
    if (refresh) cpuDisplayCache_ = cpuTracer_->resolveDisplay(wantDenoise);

    // This frame slot's fence was waited at the top of drawFrame, so ITS staging
    // buffer is idle; other slots may still be copying theirs.
    Buffer& staging = cpuStaging_[frame_];
    void* mapped = nullptr;
    vkMapMemory(device_.handle, staging.memory, 0, cpuStagingBytes_, 0, &mapped);
    std::memcpy(mapped, cpuDisplayCache_.data(),
                std::min<size_t>(cpuDisplayCache_.size(),
                                 static_cast<size_t>(cpuStagingBytes_)));
    vkUnmapMemory(device_.handle, staging.memory);

    // sceneColor_: UNDEFINED -> TRANSFER_DST, copy the traced image in, then
    // -> COLOR_ATTACHMENT_OPTIMAL so the shared blit path finds it exactly where
    // it expects the raster/GPU-PT scene.
    VkImageMemoryBarrier2 toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toDst.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    toDst.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toDst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.image = sceneColor_.handle;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d1{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d1.imageMemoryBarrierCount = 1;
    d1.pImageMemoryBarriers = &toDst;
    vkCmdPipelineBarrier2(cmd, &d1);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {sceneExtent_.width, sceneExtent_.height, 1};
    vkCmdCopyBufferToImage(cmd, staging.handle, sceneColor_.handle,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier2 toColor{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toColor.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toColor.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toColor.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toColor.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toColor.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toColor.image = sceneColor_.handle;
    toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d2.imageMemoryBarrierCount = 1;
    d2.pImageMemoryBarriers = &toColor;
    vkCmdPipelineBarrier2(cmd, &d2);
}

void Renderer::recordPathTrace(VkCommandBuffer cmd) {
    const auto barrier = [&](VkImageMemoryBarrier2* b, uint32_t count) {
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.imageMemoryBarrierCount = count;
        d.pImageMemoryBarriers = b;
        vkCmdPipelineBarrier2(cmd, &d);
    };

    // First use: UNDEFINED -> GENERAL for the storage images.
    if (!ptImagesInitialised_) {
        Image* imgs[4] = {&ptAccum_, &ptAlbedo_, &ptNormal_, &ptDenoised_};
        VkImageMemoryBarrier2 tb[4]{};
        for (int i = 0; i < 4; ++i) {
            tb[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            tb[i].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            tb[i].dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            tb[i].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            tb[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            tb[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            tb[i].image = imgs[i]->handle;
            tb[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        barrier(tb, 4);
        ptImagesInitialised_ = true;
    }

    // Accumulate one more sample until converged.
    if (ptSampleCount_ < ptMaxSamples_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ptPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ptLayout_, 0,
                                1, &ptSet_, 0, nullptr);

        // Several samples per frame ONCE THE CAMERA HAS SETTLED: the per-frame
        // overhead (tonemap, blit, UI, present) is then paid once per batch
        // instead of once per sample. While interacting (accumulation just
        // restarted) the batch stays at 1 so the view keeps full frame rate.
        // Same samples either way -- pure wall-clock, zero quality change.
        const int remaining = ptMaxSamples_ - ptSampleCount_;
        const int batch = std::min(remaining, ptSampleCount_ < 4 ? 1 : 4);

        for (int s = 0; s < batch; ++s) {
            if (s > 0) {
                // The accumulation images are read-modify-write; order the
                // batch's dispatches.
                VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                d.memoryBarrierCount = 1;
                d.pMemoryBarriers = &mb;
                vkCmdPipelineBarrier2(cmd, &d);
            }

            PtPush p{};
            for (int i = 0; i < 3; ++i) {
                p.eye[i] = rayEye_[i]; p.fwd[i] = rayFwd_[i];
                p.right[i] = rayRight_[i]; p.up[i] = rayUp_[i];
            }
            p.eye[3] = static_cast<float>(ptSampleCount_);
            p.dim[0] = sceneExtent_.width;
            p.dim[1] = sceneExtent_.height;
            p.dim[2] = 5;  // max bounce depth
            // Mix the accumulation GENERATION into the seed: without it the
            // random stream at a pixel is identical after every restart, so any
            // residual sampling structure sits at the same screen positions
            // forever (it read as a fixed pattern painted on the glass).
            p.dim[3] = static_cast<uint32_t>(ptSampleCount_) * 9781u + 1u +
                       ptGeneration_ * 2654435761u;
            // Normalised peel 0..1 (matches board.vert's push.params.w) so the
            // substrate fades translucent while exploding, revealing inner copper.
            p.misc[0] = (maxRank_ > 0.0f)
                            ? std::clamp(explodeProgress_ / maxRank_, 0.0f, 1.0f)
                            : 0.0f;
            // cos(sun angular radius); slider maps 0..1 -> 0..8 degrees.
            p.misc[1] = std::cos(shadowSoftness_ * 8.0f * 3.14159265f / 180.0f);
            p.misc[2] = rayOrtho_ ? 1.0f : 0.0f;
            p.counts[0] = opaqueTriCount_;
            // Highlighted net, as int bits (-1 = none), and its emission
            // strength in hundredths.
            p.counts[1] = static_cast<uint32_t>(highlightNets_.empty() ? -1 : 1);
            p.counts[2] = static_cast<uint32_t>(netGlow_ * 100.0f);
            p.counts[3] = netLightCount_;  // sampleable net triangles
            vkCmdPushConstants(cmd, ptLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(p), &p);
            vkCmdDispatch(cmd, (sceneExtent_.width + 7) / 8,
                          (sceneExtent_.height + 7) / 8, 1);
            ++ptSampleCount_;
        }

        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.image = ptAccum_.handle;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier(&b, 1);
    }

    // Tonemap into sceneColor_ (an SRGB attachment -> gamma on store).
    VkImageMemoryBarrier2 sc{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    sc.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    sc.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    sc.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    sc.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    sc.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    sc.image = sceneColor_.handle;
    sc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier(&sc, 1);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = sceneColor_.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo r{VK_STRUCTURE_TYPE_RENDERING_INFO};
    r.renderArea = {{0, 0}, sceneExtent_};
    r.layerCount = 1;
    r.colorAttachmentCount = 1;
    r.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &r);
    VkViewport vp{0.0f, 0.0f, static_cast<float>(sceneExtent_.width),
                  static_cast<float>(sceneExtent_.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, sceneExtent_};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapLayout_, 0,
                            1, &tonemapSet_, 0, nullptr);
    TonemapPush tp{};
    tp.dim[0] = sceneExtent_.width;
    tp.dim[1] = sceneExtent_.height;
    const bool showDenoised = denoisingEnabled_ && ptDenoisedValid_;
    // The denoised image already holds averaged colour, so it is divided by 1.
    tp.sampleCount = showDenoised ? 1u
                                  : static_cast<uint32_t>(std::max(ptSampleCount_, 1));
    tp.flags = showDenoised ? 1u : 0u;
    vkCmdPushConstants(cmd, tonemapLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(tp), &tp);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    stats_.drawCalls = 1;
    stats_.triangles = 0;
}

void Renderer::setDenoising(bool on) {
    denoisingEnabled_ = on;
    if (!on) {
        abortDenoise();
        ptDenoisedValid_ = false;
    }
}

std::string Renderer::oidnDeviceName() const {
    if (!oidnDevice_) return "none";
    switch (oidnGetDeviceInt(static_cast<OIDNDevice>(oidnDevice_), "type")) {
        case OIDN_DEVICE_TYPE_CPU:   return "CPU";
        case OIDN_DEVICE_TYPE_SYCL:  return "SYCL";
        case OIDN_DEVICE_TYPE_CUDA:  return "CUDA (GPU)";
        case OIDN_DEVICE_TYPE_HIP:   return "HIP (GPU)";
        case OIDN_DEVICE_TYPE_METAL: return "Metal (GPU)";
        default:                     return "unknown";
    }
}

// ---- Asynchronous denoise --------------------------------------------------
//
// Replaces the old milestone-gated SYNCHRONOUS denoise, which blocked the UI
// ~175ms per pass and left raw grain on screen between milestones. Now: a
// fenced GPU->host readback is submitted (never waited), a worker thread packs
// the guides and runs OIDN (including the expensive first-run filter commit),
// and the result is written back when it lands -- the frame loop never stalls,
// and the image refines continuously while the camera is still. During motion
// the accumulation resets every frame so the sample count never reaches the
// kickoff threshold: no denoise of a moving image, hence no ghosting.

void Renderer::ensureDenoiseBuffers(uint32_t w, uint32_t h) {
    if (dnW_ == w && dnH_ == h && dnColorBuf_.handle != VK_NULL_HANDLE) return;
    abortDenoise();
    const auto destroyMapped = [&](Buffer& b, void*& p) {
        if (p) { vkUnmapMemory(device_.handle, b.memory); p = nullptr; }
        destroyBuffer(b);
    };
    destroyMapped(dnColorBuf_, dnColorPtr_);
    destroyMapped(dnAlbedoBuf_, dnAlbedoPtr_);
    destroyMapped(dnNormalBuf_, dnNormalPtr_);
    destroyMapped(dnOutBuf_, dnOutPtr_);

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 16;  // RGBA32F
    // The three CPU-read buffers are HOST_CACHED (strided reads from
    // write-combined memory were the original 5s denoise); the output is only
    // written by the CPU, so plain coherent is right for it.
    dnColorBuf_ = createReadbackBuffer(bytes);
    dnAlbedoBuf_ = createReadbackBuffer(bytes);
    dnNormalBuf_ = createReadbackBuffer(bytes);
    dnOutBuf_ = createBuffer(bytes,
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkMapMemory(device_.handle, dnColorBuf_.memory, 0, bytes, 0,
                      &dnColorPtr_),
          "vkMapMemory(dnColor)");
    check(vkMapMemory(device_.handle, dnAlbedoBuf_.memory, 0, bytes, 0,
                      &dnAlbedoPtr_),
          "vkMapMemory(dnAlbedo)");
    check(vkMapMemory(device_.handle, dnNormalBuf_.memory, 0, bytes, 0,
                      &dnNormalPtr_),
          "vkMapMemory(dnNormal)");
    check(vkMapMemory(device_.handle, dnOutBuf_.memory, 0, bytes, 0, &dnOutPtr_),
          "vkMapMemory(dnOut)");
    dnW_ = w;
    dnH_ = h;
}

// Runs ENTIRELY on the worker thread: pack the readbacks into tight float3
// arrays, (re)build the cached OIDN filter if the resolution changed, filter,
// and expand the result into the mapped output buffer. All OIDN access lives
// here; the main thread only touches OIDN in destroyPathTracer, after
// abortDenoise() has joined the worker.
void Renderer::denoiseWorker(uint32_t w, uint32_t h, int samples) {
    const size_t n = static_cast<size_t>(w) * h;
    const float* rc = static_cast<const float*>(dnColorPtr_);
    const float* ra = static_cast<const float*>(dnAlbedoPtr_);
    const float* rn = static_cast<const float*>(dnNormalPtr_);
    std::vector<float> col(n * 3), alb(n * 3), nor(n * 3), out(n * 3);
    const float inv = 1.0f / static_cast<float>(std::max(samples, 1));
    for (size_t i = 0; i < n; ++i) {
        for (int k = 0; k < 3; ++k) {
            col[i * 3 + k] = rc[i * 4 + k] * inv;
            // OIDN wants albedo in [0,1]; sky pixels accumulate sky(dir), whose
            // sun term exceeds 1 and would degrade the denoise around it.
            alb[i * 3 + k] = std::clamp(ra[i * 4 + k] * inv, 0.0f, 1.0f);
        }
        float nx = rn[i * 4 + 0] * inv;
        float ny = rn[i * 4 + 1] * inv;
        float nz = rn[i * 4 + 2] * inv;
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-6f) { nx /= len; ny /= len; nz /= len; }
        nor[i * 3 + 0] = nx;
        nor[i * 3 + 1] = ny;
        nor[i * 3 + 2] = nz;
    }

    // Cached OIDN filter + device buffers; only (re)built when the resolution
    // changes -- oidnCommitFilter recompiles the network to the GPU (~0.4s),
    // which on this thread costs the UI nothing.
    OIDNDevice dev = static_cast<OIDNDevice>(oidnDevice_);
    const size_t rgbBytes = n * 3 * sizeof(float);
    if (oidnW_ != w || oidnH_ != h || oidnFilter_ == nullptr) {
        if (oidnFilter_) oidnReleaseFilter(static_cast<OIDNFilter>(oidnFilter_));
        if (oidnAlbedoFilter_)
            oidnReleaseFilter(static_cast<OIDNFilter>(oidnAlbedoFilter_));
        if (oidnNormalFilter_)
            oidnReleaseFilter(static_cast<OIDNFilter>(oidnNormalFilter_));
        if (oidnColorBuf_)
            oidnReleaseBuffer(static_cast<OIDNBuffer>(oidnColorBuf_));
        if (oidnAlbedoBuf_)
            oidnReleaseBuffer(static_cast<OIDNBuffer>(oidnAlbedoBuf_));
        if (oidnNormalBuf_)
            oidnReleaseBuffer(static_cast<OIDNBuffer>(oidnNormalBuf_));
        if (oidnOutBuf_) oidnReleaseBuffer(static_cast<OIDNBuffer>(oidnOutBuf_));

        OIDNBuffer bc = oidnNewBuffer(dev, rgbBytes);
        OIDNBuffer ba = oidnNewBuffer(dev, rgbBytes);
        OIDNBuffer bn = oidnNewBuffer(dev, rgbBytes);
        OIDNBuffer bo = oidnNewBuffer(dev, rgbBytes);
        const size_t px = sizeof(float) * 3, row = px * w;

        // Aux PREFILTERS (OIDN's official noisy-guide mode): the stochastic
        // mask alpha leaves residual noise in the guides, and a filter fed
        // noisy guides reproduces that noise as blotches. Each aux image is
        // denoised in place first, and the main filter is told the aux are
        // clean (cleanAux).
        OIDNFilter fa = oidnNewFilter(dev, "RT");
        oidnSetFilterImage(fa, "albedo", ba, OIDN_FORMAT_FLOAT3, w, h, 0, px, row);
        oidnSetFilterImage(fa, "output", ba, OIDN_FORMAT_FLOAT3, w, h, 0, px, row);
        oidnSetFilterInt(fa, "maxMemoryMB", 4096);
        oidnCommitFilter(fa);
        OIDNFilter fn = oidnNewFilter(dev, "RT");
        oidnSetFilterImage(fn, "normal", bn, OIDN_FORMAT_FLOAT3, w, h, 0, px, row);
        oidnSetFilterImage(fn, "output", bn, OIDN_FORMAT_FLOAT3, w, h, 0, px, row);
        oidnSetFilterInt(fn, "maxMemoryMB", 4096);
        oidnCommitFilter(fn);

        OIDNFilter f = oidnNewFilter(dev, "RT");
        oidnSetFilterImage(f, "color", bc, OIDN_FORMAT_FLOAT3, w, h, 0, px, row);
        oidnSetFilterImage(f, "albedo", ba, OIDN_FORMAT_FLOAT3, w, h, 0, px, row);
        oidnSetFilterImage(f, "normal", bn, OIDN_FORMAT_FLOAT3, w, h, 0, px, row);
        oidnSetFilterImage(f, "output", bo, OIDN_FORMAT_FLOAT3, w, h, 0, px, row);
        oidnSetFilterBool(f, "hdr", true);
        oidnSetFilterBool(f, "cleanAux", true);
        // A generous memory budget keeps OIDN from processing the image in
        // internal TILES -- tile seams surface as a faint, screen-anchored, even
        // square grid over noisy content (worst on large boards that fill the
        // frame).
        oidnSetFilterInt(f, "maxMemoryMB", 4096);
        oidnCommitFilter(f);

        oidnColorBuf_ = bc;
        oidnAlbedoBuf_ = ba;
        oidnNormalBuf_ = bn;
        oidnOutBuf_ = bo;
        oidnFilter_ = f;
        oidnAlbedoFilter_ = fa;
        oidnNormalFilter_ = fn;
        oidnW_ = w;
        oidnH_ = h;
    }

    oidnWriteBuffer(static_cast<OIDNBuffer>(oidnColorBuf_), 0, rgbBytes,
                    col.data());
    oidnWriteBuffer(static_cast<OIDNBuffer>(oidnAlbedoBuf_), 0, rgbBytes,
                    alb.data());
    oidnWriteBuffer(static_cast<OIDNBuffer>(oidnNormalBuf_), 0, rgbBytes,
                    nor.data());
    oidnExecuteFilter(static_cast<OIDNFilter>(oidnAlbedoFilter_));
    oidnExecuteFilter(static_cast<OIDNFilter>(oidnNormalFilter_));
    oidnExecuteFilter(static_cast<OIDNFilter>(oidnFilter_));
    oidnReadBuffer(static_cast<OIDNBuffer>(oidnOutBuf_), 0, rgbBytes,
                   out.data());

    // Expand the denoised RGB into the mapped RGBA write-back buffer.
    float* op = static_cast<float*>(dnOutPtr_);
    for (size_t i = 0; i < n; ++i) {
        op[i * 4 + 0] = out[i * 3 + 0];
        op[i * 4 + 1] = out[i * 3 + 1];
        op[i * 4 + 2] = out[i * 3 + 2];
        op[i * 4 + 3] = 1.0f;
    }
}

void Renderer::abortDenoise() {
    if (dnState_ == DenoiseState::Reading) {
        vkWaitForFences(device_.handle, 1, &dnFence_, VK_TRUE, UINT64_MAX);
        vkResetFences(device_.handle, 1, &dnFence_);
        if (dnCmd_) {
            vkFreeCommandBuffers(device_.handle, commandPool_, 1, &dnCmd_);
            dnCmd_ = VK_NULL_HANDLE;
        }
    } else if (dnState_ == DenoiseState::Filtering) {
        if (dnThread_.joinable()) dnThread_.join();
    }
    dnState_ = DenoiseState::Idle;
}

bool Renderer::denoiseTick() {
    if (!oidnDevice_ || !rtSupported_ || !denoisingEnabled_ ||
        ptAccum_.handle == VK_NULL_HANDLE) {
        return false;
    }

    switch (dnState_) {
        case DenoiseState::Idle: {
            // Two samples minimum: a 1spp guide is too thin, and during camera
            // motion the count resets every frame and never reaches 2 -- which
            // is exactly the "never denoise a moving image" gate.
            if (ptSampleCount_ < 2 || ptSampleCount_ == dnDisplayed_)
                return false;
            const uint32_t w = sceneExtent_.width, h = sceneExtent_.height;
            ensureDenoiseBuffers(w, h);
            if (dnFence_ == VK_NULL_HANDLE) {
                VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                check(vkCreateFence(device_.handle, &fi, nullptr, &dnFence_),
                      "vkCreateFence(denoise)");
            }

            VkCommandBufferAllocateInfo a{
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            a.commandPool = commandPool_;
            a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            a.commandBufferCount = 1;
            check(vkAllocateCommandBuffers(device_.handle, &a, &dnCmd_),
                  "vkAllocateCommandBuffers(denoise)");
            VkCommandBufferBeginInfo b{
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(dnCmd_, &b);

            // The path tracer wrote these via storage this frame; make the
            // writes visible to the transfer reads.
            VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(dnCmd_, &dep);

            const auto copy = [&](Image& img, Buffer& buf) {
                VkBufferImageCopy r{};
                r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                r.imageExtent = {w, h, 1};
                vkCmdCopyImageToBuffer(dnCmd_, img.handle,
                                       VK_IMAGE_LAYOUT_GENERAL, buf.handle, 1,
                                       &r);
            };
            copy(ptAccum_, dnColorBuf_);
            copy(ptAlbedo_, dnAlbedoBuf_);
            copy(ptNormal_, dnNormalBuf_);
            vkEndCommandBuffer(dnCmd_);

            VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            s.commandBufferCount = 1;
            s.pCommandBuffers = &dnCmd_;
            check(vkQueueSubmit(device_.graphicsQueue, 1, &s, dnFence_),
                  "vkQueueSubmit(denoise readback)");

            dnSamples_ = ptSampleCount_;
            dnGen_ = ptGeneration_;
            dnState_ = DenoiseState::Reading;
            return true;
        }

        case DenoiseState::Reading: {
            if (vkGetFenceStatus(device_.handle, dnFence_) != VK_SUCCESS)
                return true;  // still copying; come back next frame
            vkResetFences(device_.handle, 1, &dnFence_);
            vkFreeCommandBuffers(device_.handle, commandPool_, 1, &dnCmd_);
            dnCmd_ = VK_NULL_HANDLE;
            if (dnGen_ != ptGeneration_) {
                dnState_ = DenoiseState::Idle;  // view changed mid-copy: discard
                return true;
            }
            dnThreadDone_.store(false);
            const uint32_t w = dnW_, h = dnH_;
            const int samples = dnSamples_;
            dnThread_ = std::thread([this, w, h, samples] {
                denoiseWorker(w, h, samples);
                dnThreadDone_.store(true);
            });
            dnState_ = DenoiseState::Filtering;
            return true;
        }

        case DenoiseState::Filtering: {
            if (!dnThreadDone_.load()) return true;  // OIDN still running
            dnThread_.join();
            if (dnGen_ == ptGeneration_) {
                // Host -> GPU: the denoised result. Small synchronous copy.
                VkCommandBufferAllocateInfo a{
                    VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
                a.commandPool = commandPool_;
                a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                a.commandBufferCount = 1;
                VkCommandBuffer c = VK_NULL_HANDLE;
                vkAllocateCommandBuffers(device_.handle, &a, &c);
                VkCommandBufferBeginInfo b{
                    VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                vkBeginCommandBuffer(c, &b);
                VkBufferImageCopy r{};
                r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                r.imageExtent = {dnW_, dnH_, 1};
                vkCmdCopyBufferToImage(c, dnOutBuf_.handle, ptDenoised_.handle,
                                       VK_IMAGE_LAYOUT_GENERAL, 1, &r);
                vkEndCommandBuffer(c);
                VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                s.commandBufferCount = 1;
                s.pCommandBuffers = &c;
                vkQueueSubmit(device_.graphicsQueue, 1, &s, VK_NULL_HANDLE);
                vkQueueWaitIdle(device_.graphicsQueue);
                vkFreeCommandBuffers(device_.handle, commandPool_, 1, &c);
                ptDenoisedValid_ = true;
                dnDisplayed_ = dnSamples_;
            }
            dnState_ = DenoiseState::Idle;
            return true;
        }
    }
    return false;
}


int Renderer::findMemoryTypeOrNeg(uint32_t filter,
                                  VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(device_.gpu.handle, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if ((filter & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & props) == props) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

uint32_t Renderer::findMemoryType(uint32_t filter,
                                  VkMemoryPropertyFlags props) const {
    const int t = findMemoryTypeOrNeg(filter, props);
    if (t < 0) throw std::runtime_error("no suitable memory type");
    return static_cast<uint32_t>(t);
}

Renderer::Buffer Renderer::createReadbackBuffer(VkDeviceSize size) {
    Buffer buffer;
    buffer.size = size;

    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device_.handle, &info, nullptr, &buffer.handle),
          "vkCreateBuffer");

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_.handle, buffer.handle, &req);

    const VkMemoryPropertyFlags coherent =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    // HOST_CACHED + HOST_COHERENT gives fast strided CPU reads with no manual
    // cache invalidation. Fall back to plain coherent (write-combined) if the
    // device has no cached-coherent type.
    int type =
        findMemoryTypeOrNeg(req.memoryTypeBits,
                            coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type < 0) type = findMemoryTypeOrNeg(req.memoryTypeBits, coherent);
    if (type < 0) throw std::runtime_error("no host-visible memory type");

    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = static_cast<uint32_t>(type);
    check(vkAllocateMemory(device_.handle, &alloc, nullptr, &buffer.memory),
          "vkAllocateMemory");
    check(vkBindBufferMemory(device_.handle, buffer.handle, buffer.memory, 0),
          "vkBindBufferMemory");
    return buffer;
}

Renderer::Buffer Renderer::createBuffer(VkDeviceSize size,
                                        VkBufferUsageFlags usage,
                                        VkMemoryPropertyFlags props) {
    Buffer buffer;
    buffer.size = size;

    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device_.handle, &info, nullptr, &buffer.handle),
          "vkCreateBuffer");

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_.handle, buffer.handle, &req);

    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    // Rule 2: any buffer that may later feed an acceleration structure must be
    // allocated with the device-address flag. Doing it now costs nothing.
    if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) alloc.pNext = &flags;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits, props);
    check(vkAllocateMemory(device_.handle, &alloc, nullptr, &buffer.memory),
          "vkAllocateMemory");
    check(vkBindBufferMemory(device_.handle, buffer.handle, buffer.memory, 0),
          "vkBindBufferMemory");
    return buffer;
}

void Renderer::destroyBuffer(Buffer& buffer) {
    if (buffer.handle) vkDestroyBuffer(device_.handle, buffer.handle, nullptr);
    if (buffer.memory) vkFreeMemory(device_.handle, buffer.memory, nullptr);
    buffer = {};
}

void Renderer::uploadViaStaging(Buffer& dst, const void* data,
                                VkDeviceSize size) {
    Buffer staging = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* mapped = nullptr;
    check(vkMapMemory(device_.handle, staging.memory, 0, size, 0, &mapped),
          "vkMapMemory");
    std::memcpy(mapped, data, size);
    vkUnmapMemory(device_.handle, staging.memory);

    VkCommandBufferAllocateInfo alloc{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = commandPool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(device_.handle, &alloc, &cmd),
          "vkAllocateCommandBuffers");

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(cmd, staging.handle, dst.handle, 1, &copy);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    check(vkQueueSubmit(device_.graphicsQueue, 1, &submit, VK_NULL_HANDLE),
          "vkQueueSubmit");
    vkQueueWaitIdle(device_.graphicsQueue);

    vkFreeCommandBuffers(device_.handle, commandPool_, 1, &cmd);
    destroyBuffer(staging);
}

void Renderer::createSwapchain(uint32_t width, uint32_t height) {
    VkSurfaceCapabilitiesKHR caps{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device_.gpu.handle, surface_,
                                                    &caps),
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device_.gpu.handle, surface_,
                                         &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(device_.gpu.handle, surface_,
                                         &formatCount, formats.data());

    VkSurfaceFormatKHR chosen = formats[0];
    for (const VkSurfaceFormatKHR& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }
    colorFormat_ = chosen.format;

    extent_ = caps.currentExtent;
    if (extent_.width == UINT32_MAX) {
        extent_.width = std::clamp(width, caps.minImageExtent.width,
                                   caps.maxImageExtent.width);
        extent_.height = std::clamp(height, caps.minImageExtent.height,
                                    caps.maxImageExtent.height);
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_;
    info.minImageCount = imageCount;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    // COLOR_ATTACHMENT for the UI pass, TRANSFER_DST because the scene is
    // blitted in, TRANSFER_SRC so frames can be captured.
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if ((caps.supportedUsageFlags & info.imageUsage) != info.imageUsage) {
        throw std::runtime_error(
            "surface does not support the required swapchain image usage "
            "(colour attachment + transfer src/dst)");
    }
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always available
    info.clipped = VK_TRUE;
    check(vkCreateSwapchainKHR(device_.handle, &info, nullptr, &swapchain_),
          "vkCreateSwapchainKHR");

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device_.handle, swapchain_, &count, nullptr);
    images_.resize(count);
    vkGetSwapchainImagesKHR(device_.handle, swapchain_, &count, images_.data());

    views_.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v.image = images_[i];
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.format = colorFormat_;
        v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(device_.handle, &v, nullptr, &views_[i]),
              "vkCreateImageView");
    }
}

void Renderer::destroySwapchain() {
    for (VkImageView v : views_) vkDestroyImageView(device_.handle, v, nullptr);
    views_.clear();
    images_.clear();

    if (swapchain_) vkDestroySwapchainKHR(device_.handle, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

Renderer::Image Renderer::createImage(VkExtent2D extent, VkFormat format,
                                      VkImageUsageFlags usage,
                                      VkImageAspectFlags aspect) {
    Image image;

    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(device_.handle, &info, nullptr, &image.handle),
          "vkCreateImage");

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_.handle, image.handle, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex =
        findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device_.handle, &alloc, nullptr, &image.memory),
          "vkAllocateMemory(image)");
    check(vkBindImageMemory(device_.handle, image.handle, image.memory, 0),
          "vkBindImageMemory");

    VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    v.image = image.handle;
    v.viewType = VK_IMAGE_VIEW_TYPE_2D;
    v.format = format;
    v.subresourceRange = {aspect, 0, 1, 0, 1};
    check(vkCreateImageView(device_.handle, &v, nullptr, &image.view),
          "vkCreateImageView");
    return image;
}

void Renderer::destroyImage(Image& image) {
    if (image.view) vkDestroyImageView(device_.handle, image.view, nullptr);
    if (image.handle) vkDestroyImage(device_.handle, image.handle, nullptr);
    if (image.memory) vkFreeMemory(device_.handle, image.memory, nullptr);
    image = {};
}

void Renderer::createSceneTargets() {
    // Scene resolution is the window scaled, clamped so a silly slider value
    // cannot ask for a zero-sized or device-limit-busting image.
    const uint32_t maxDim = device_.gpu.maxImageDimension2D;
    sceneExtent_.width = std::clamp(
        static_cast<uint32_t>(std::lround(extent_.width * renderScale_)), 1u,
        maxDim);
    sceneExtent_.height = std::clamp(
        static_cast<uint32_t>(std::lround(extent_.height * renderScale_)), 1u,
        maxDim);

    // SAMPLED so the bloom extract pass can read the finished scene.
    sceneColor_ = createImage(sceneExtent_, colorFormat_,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT);
    sceneDepth_ = createImage(sceneExtent_, depthFormat_,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                              VK_IMAGE_ASPECT_DEPTH_BIT);

    // Host-visible staging for the Embree tracer's BGRA output, copied into
    // sceneColor_ each CPU-traced frame. One buffer per frame in flight -- the
    // slot's fence guarantees its previous copy retired before the CPU rewrites.
    if (cpuMode_) {
        for (Buffer& b : cpuStaging_) destroyBuffer(b);
        cpuStaging_.clear();
        cpuStagingBytes_ =
            static_cast<VkDeviceSize>(sceneExtent_.width) * sceneExtent_.height * 4;
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            cpuStaging_.push_back(
                createBuffer(cpuStagingBytes_, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
        }
        cpuTracerGen_ = 0xFFFFFFFFu;  // force a fresh accumulation
        cpuDisplayCache_.clear();
    }

    // Path-tracer targets, at the same resolution. HDR accumulation + the two
    // denoiser guides, all storage images kept in GENERAL layout.
    if (rtSupported_) {
        // The async denoise's fenced readback references these images; drain it
        // before they go away.
        abortDenoise();
        destroyImage(ptAccum_);
        destroyImage(ptAlbedo_);
        destroyImage(ptNormal_);
        destroyImage(ptDenoised_);
        // STORAGE for compute R/W, TRANSFER_SRC/DST for the denoise readback and
        // write-back copies.
        const VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ptAccum_ = createImage(sceneExtent_, VK_FORMAT_R32G32B32A32_SFLOAT, u,
                               VK_IMAGE_ASPECT_COLOR_BIT);
        ptAlbedo_ = createImage(sceneExtent_, VK_FORMAT_R32G32B32A32_SFLOAT, u,
                                VK_IMAGE_ASPECT_COLOR_BIT);
        ptNormal_ = createImage(sceneExtent_, VK_FORMAT_R32G32B32A32_SFLOAT, u,
                                VK_IMAGE_ASPECT_COLOR_BIT);
        ptDenoised_ = createImage(sceneExtent_, VK_FORMAT_R32G32B32A32_SFLOAT, u,
                                  VK_IMAGE_ASPECT_COLOR_BIT);
        ptImagesInitialised_ = false;  // need the UNDEFINED->GENERAL transition
        ptDenoisedValid_ = false;
        resetAccumulation();            // resolution changed; restart accumulation
        if (ptSet_) updatePathTraceDescriptors();
    }
}

void Renderer::destroySceneTargets() {
    destroyImage(sceneColor_);
    destroyImage(sceneDepth_);
    destroyImage(bloomTex_);  // sized from sceneExtent_, so it dies with it
    for (Buffer& b : cpuStaging_) destroyBuffer(b);
    cpuStaging_.clear();
}

void Renderer::setRenderScale(float scale) {
    scale = std::clamp(scale, 0.25f, 4.0f);
    if (std::abs(scale - renderScale_) < 1e-4f) return;
    vkDeviceWaitIdle(device_.handle);
    renderScale_ = scale;
    destroySceneTargets();
    createSceneTargets();
    createBloomTargets();
    // Every accumulated image is sized to the OLD scene extent, so the new
    // resolution has to start over. Without this the raster path followed the
    // slider but the traced paths did not: a converged CPU trace skips
    // accumulate() entirely and keeps blitting its cached display buffer, so
    // the internal-resolution slider did nothing at all on the CPU device.
    // resetAccumulation bumps ptGeneration_, which is also what makes
    // recordCpuPathTrace drop its pass counter and cache on the next frame.
    resetAccumulation();
    cpuDisplayCache_.clear();
    cpuPass_ = 0;
}

void Renderer::createDescriptors() {
    // Rule 3: one bindless SSBO of materials, indexed by instance ID. Not a
    // descriptor set per draw -- a closest-hit shader cannot rebind.
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    VkDescriptorSetLayoutBinding mat{};
    mat.binding = 0;
    mat.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    mat.descriptorCount = 1;
    // Vertex too: it reads the per-part explode rank from the same table.
    mat.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings.push_back(mat);

    if (rtSupported_) {
        // binding 1: the TLAS the fragment shader traces against for RT shadows/AO.
        VkDescriptorSetLayoutBinding as{};
        as.binding = 1;
        as.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        as.descriptorCount = 1;
        as.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(as);
    }

    // Per-triangle net, for highlighting. A FIXED binding number regardless
    // of RT support, so the two fragment shaders can declare it once.
    {
        VkDescriptorSetLayoutBinding net{};
        net.binding = kNetBinding;
        net.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        net.descriptorCount = 1;
        net.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(net);
        net.binding = kNetColorBinding;
        bindings.push_back(net);
    }

    VkDescriptorSetLayoutCreateInfo info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    check(vkCreateDescriptorSetLayout(device_.handle, &info, nullptr, &setLayout_),
          "vkCreateDescriptorSetLayout");

    std::vector<VkDescriptorPoolSize> sizes{
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3}};  // materials, triNet, netColour
    if (rtSupported_)
        sizes.push_back({VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1});
    VkDescriptorPoolCreateInfo pool{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = static_cast<uint32_t>(sizes.size());
    pool.pPoolSizes = sizes.data();
    check(vkCreateDescriptorPool(device_.handle, &pool, nullptr, &descriptorPool_),
          "vkCreateDescriptorPool");

    VkDescriptorSetAllocateInfo alloc{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = descriptorPool_;
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &setLayout_;
    check(vkAllocateDescriptorSets(device_.handle, &alloc, &materialSet_),
          "vkAllocateDescriptorSets");
}

void Renderer::createPipeline() {
    VkShaderModuleCreateInfo vsInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    vsInfo.codeSize = sizeof(kBoardVert);
    vsInfo.pCode = kBoardVert;
    VkShaderModule vs = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device_.handle, &vsInfo, nullptr, &vs),
          "vkCreateShaderModule(vert)");

    // RT device gets the ray-query fragment shader; everything else the plain
    // raster one. The RT shader still runs when RT is toggled off -- an rtOn push
    // constant gates the tracing -- so there is no pipeline to switch at runtime.
    VkShaderModuleCreateInfo fsInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    fsInfo.codeSize = rtSupported_ ? sizeof(kBoardFragRt) : sizeof(kBoardFrag);
    fsInfo.pCode = rtSupported_ ? kBoardFragRt : kBoardFrag;
    VkShaderModule fs = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device_.handle, &fsInfo, nullptr, &fs),
          "vkCreateShaderModule(frag)");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    // Matches geom::Vertex: two tightly packed vec3s.
    VkVertexInputBindingDescription bind{};
    bind.binding = 0;
    bind.stride = sizeof(geom::Vertex);
    bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(geom::Vertex, position)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(geom::Vertex, normal)};

    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bind;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    // Backface culling is load-bearing, not an optimisation.
    //
    // The stackup is full of exactly-coplanar surfaces: copper's top face and
    // the mask's underside both sit at 1.590; copper's underside and the
    // substrate's top face both sit at 1.555. Those pairs are triangulated
    // independently by earcut, so their interpolated depth differs by an ULP per
    // pixel and the depth test flickers -- speckled, torn-looking traces.
    //
    // Culling deletes the inward-facing half of every pair, so nothing coplanar
    // is ever rasterised together. Winding is consistent: extrude() emits top
    // caps CCW from +Z, bottom caps reversed, and side-wall quads wound from the
    // ring's signed area, so every front face points out of the solid.
    raster.cullMode = VK_CULL_MODE_BACK_BIT;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // REVERSED-Z. Depth 1.0 at the near plane, 0.0 at infinity, cleared to 0,
    // compared with GREATER.
    //
    // Not a micro-optimisation -- it is what makes this board renderable. A
    // conventional 0..1 depth mapping spends nearly all its precision at the
    // near plane, so with near=0.05 the 0.010mm gap between copper (z=1.590) and
    // soldermask (z=1.600) resolved to roughly 7 float ULPs when zoomed out to
    // ~70mm: visible flicker that vanished as you zoomed in. Reversed-Z pairs
    // the float exponent's density near zero with the projection's hyperbolic
    // depth distribution, and the two cancel almost exactly -- precision becomes
    // near-uniform across the whole range.
    //
    // Requires D32_SFLOAT (a UNORM depth buffer would gain nothing).
    VkPipelineDepthStencilStateCreateInfo depth{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_GREATER;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                            VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(device_.handle, &layoutInfo, nullptr, &layout_),
          "vkCreatePipelineLayout");

    VkPipelineRenderingCreateInfo rendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colorFormat_;
    rendering.depthAttachmentFormat = depthFormat_;

    VkGraphicsPipelineCreateInfo info{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;  // dynamic rendering: no VkRenderPass
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout_;
    check(vkCreateGraphicsPipelines(device_.handle, VK_NULL_HANDLE, 1, &info,
                                    nullptr, &pipelineOpaque_),
          "vkCreateGraphicsPipelines(opaque)");

    // Soldermask is a translucent film. Alpha blended, depth-tested but not
    // depth-written, and drawn after the opaque pass.
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    depth.depthWriteEnable = VK_FALSE;
    check(vkCreateGraphicsPipelines(device_.handle, VK_NULL_HANDLE, 1, &info,
                                    nullptr, &pipelineBlend_),
          "vkCreateGraphicsPipelines(blend)");

    vkDestroyShaderModule(device_.handle, vs, nullptr);
    vkDestroyShaderModule(device_.handle, fs, nullptr);
}

// Screen-space overlay pipeline: pixel-space triangles with per-vertex colour,
// alpha-blended straight onto the swapchain in the UI pass. No depth, no
// descriptors -- just a viewport-size push constant.
void Renderer::createOverlayPipeline() {
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 4 * sizeof(float)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    check(vkCreatePipelineLayout(device_.handle, &pl, nullptr, &overlayLayout_),
          "overlay pipeline layout");

    VkShaderModule vs =
        makeModule(device_.handle, kOverlayVert, sizeof(kOverlayVert));
    VkShaderModule fs =
        makeModule(device_.handle, kOverlayFrag, sizeof(kOverlayFrag));
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkVertexInputBindingDescription bind{0, 6 * sizeof(float),
                                         VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 2 * sizeof(float)};
    VkPipelineVertexInputStateCreateInfo vin{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &bind;
    vin.vertexAttributeDescriptionCount = 2;
    vin.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rend{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rend.colorAttachmentCount = 1;
    rend.pColorAttachmentFormats = &colorFormat_;

    VkGraphicsPipelineCreateInfo gpi{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.pNext = &rend;
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vin;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cb;
    gpi.pDynamicState = &ds;
    gpi.layout = overlayLayout_;
    check(vkCreateGraphicsPipelines(device_.handle, VK_NULL_HANDLE, 1, &gpi,
                                    nullptr, &overlayPipeline_),
          "overlay pipeline");
    vkDestroyShaderModule(device_.handle, vs, nullptr);
    vkDestroyShaderModule(device_.handle, fs, nullptr);
}

// Upload this frame's overlay triangles and draw them. Called inside the UI
// pass (swapchain bound at native resolution).
void Renderer::recordOverlay(VkCommandBuffer cmd, VkExtent2D drawArea) {
    if (overlayTris_.empty()) return;
    const VkDeviceSize bytes = overlayTris_.size() * sizeof(float);

    if (overlayVb_.empty()) overlayVb_.resize(kFramesInFlight);
    Buffer& vb = overlayVb_[frame_];
    if (vb.size < bytes) {
        destroyBuffer(vb);
        vb = createBuffer(std::max<VkDeviceSize>(bytes, 64 * 1024),
                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    void* mapped = nullptr;
    vkMapMemory(device_.handle, vb.memory, 0, bytes, 0, &mapped);
    std::memcpy(mapped, overlayTris_.data(), bytes);
    vkUnmapMemory(device_.handle, vb.memory);

    VkViewport viewport{0.0f, 0.0f, static_cast<float>(drawArea.width),
                        static_cast<float>(drawArea.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, drawArea};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipeline_);
    // Always the WINDOW size: vertices are in window pixels, and the viewport
    // above does the stretching when drawArea is a bigger export target.
    const float vpPush[4] = {static_cast<float>(extent_.width),
                             static_cast<float>(extent_.height), 0.0f, 0.0f};
    vkCmdPushConstants(cmd, overlayLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(vpPush), vpPush);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb.handle, &zero);
    vkCmdDraw(cmd, static_cast<uint32_t>(overlayTris_.size() / 6), 1, 0, 0);
}

void Renderer::createBloomPipelines() {
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    // CLAMP_TO_EDGE, or the halo wraps around the opposite edge of the image.
    si.addressModeU = si.addressModeV = si.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(vkCreateSampler(device_.handle, &si, nullptr, &bloomSampler_),
          "bloom sampler");

    VkDescriptorSetLayoutBinding b{};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &b;
    check(vkCreateDescriptorSetLayout(device_.handle, &li, nullptr,
                                      &bloomSetLayout_),
          "bloom set layout");

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 2;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    check(vkCreateDescriptorPool(device_.handle, &pi, nullptr, &bloomPool_),
          "bloom pool");

    VkDescriptorSetLayout layouts[2] = {bloomSetLayout_, bloomSetLayout_};
    VkDescriptorSetAllocateInfo ai{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = bloomPool_;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    VkDescriptorSet sets[2]{};
    check(vkAllocateDescriptorSets(device_.handle, &ai, sets), "bloom sets");
    bloomSrcSet_ = sets[0];
    bloomTexSet_ = sets[1];

    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 * sizeof(float)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &bloomSetLayout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    check(vkCreatePipelineLayout(device_.handle, &pl, nullptr, &bloomLayout_),
          "bloom pipeline layout");

    // Two pipelines over the same fullscreen vertex shader: extract writes a
    // fresh target (no blend), composite adds onto the scene (ONE/ONE).
    VkShaderModule vs = makeModule(device_.handle, kFullscreenVert,
                                   sizeof(kFullscreenVert));
    const auto build = [&](const uint32_t* code, size_t bytes, bool additive) {
        VkShaderModule fs = makeModule(device_.handle, code, bytes);
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vin{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (additive) {
            cba.blendEnable = VK_TRUE;
            cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.colorBlendOp = VK_BLEND_OP_ADD;
            cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            cba.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo cb{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo rend{
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rend.colorAttachmentCount = 1;
        rend.pColorAttachmentFormats = &colorFormat_;

        VkGraphicsPipelineCreateInfo gpi{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gpi.pNext = &rend;
        gpi.stageCount = 2;
        gpi.pStages = stages;
        gpi.pVertexInputState = &vin;
        gpi.pInputAssemblyState = &ia;
        gpi.pViewportState = &vp;
        gpi.pRasterizationState = &rs;
        gpi.pMultisampleState = &ms;
        gpi.pColorBlendState = &cb;
        gpi.pDynamicState = &ds;
        gpi.layout = bloomLayout_;
        VkPipeline p = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device_.handle, VK_NULL_HANDLE, 1, &gpi,
                                        nullptr, &p),
              "bloom pipeline");
        vkDestroyShaderModule(device_.handle, fs, nullptr);
        return p;
    };
    bloomExtractPipeline_ = build(kBloomExtract, sizeof(kBloomExtract), false);
    bloomCompositePipeline_ =
        build(kBloomComposite, sizeof(kBloomComposite), true);
    vkDestroyShaderModule(device_.handle, vs, nullptr);
}

// Quarter-resolution bloom target, plus the two descriptor writes. Called
// whenever the scene targets are (re)created, since both depend on the size.
void Renderer::createBloomTargets() {
    if (bloomSetLayout_ == VK_NULL_HANDLE) return;
    // HALF resolution, not quarter: a highlighted trace is only a pixel or two
    // wide, and a quarter-res downsample attenuates it almost to nothing
    // before it can bleed.
    bloomExtent_ = {std::max(1u, sceneExtent_.width / 2),
                    std::max(1u, sceneExtent_.height / 2)};
    bloomTex_ = createImage(bloomExtent_, colorFormat_,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT);

    VkDescriptorImageInfo src{bloomSampler_, sceneColor_.view,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo tex{bloomSampler_, bloomTex_.view,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[2]{};
    for (int i = 0; i < 2; ++i) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstBinding = 0;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    w[0].dstSet = bloomSrcSet_;
    w[0].pImageInfo = &src;
    w[1].dstSet = bloomTexSet_;
    w[1].pImageInfo = &tex;
    vkUpdateDescriptorSets(device_.handle, 2, w, 0, nullptr);
}

void Renderer::destroyBloom() {
    if (bloomExtractPipeline_)
        vkDestroyPipeline(device_.handle, bloomExtractPipeline_, nullptr);
    if (bloomCompositePipeline_)
        vkDestroyPipeline(device_.handle, bloomCompositePipeline_, nullptr);
    if (bloomLayout_)
        vkDestroyPipelineLayout(device_.handle, bloomLayout_, nullptr);
    if (bloomPool_) vkDestroyDescriptorPool(device_.handle, bloomPool_, nullptr);
    if (bloomSetLayout_)
        vkDestroyDescriptorSetLayout(device_.handle, bloomSetLayout_, nullptr);
    if (bloomSampler_) vkDestroySampler(device_.handle, bloomSampler_, nullptr);
    bloomExtractPipeline_ = bloomCompositePipeline_ = VK_NULL_HANDLE;
    bloomLayout_ = VK_NULL_HANDLE;
    bloomPool_ = VK_NULL_HANDLE;
    bloomSetLayout_ = VK_NULL_HANDLE;
    bloomSampler_ = VK_NULL_HANDLE;
}

// Threshold+downsample the finished scene, then add the result back over it.
// Runs only while a net is highlighted: that is the one case with pixels
// deliberately pushed past white, and it keeps ordinary renders untouched.
void Renderer::recordBloom(VkCommandBuffer cmd) {
    if (!bloomEnabled_ || highlightNets_.empty() ||
        bloomExtractPipeline_ == VK_NULL_HANDLE)
        return;

    const auto barrier = [&](VkImage image, VkImageLayout from,
                             VkImageLayout to) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT |
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        b.oldLayout = from;
        b.newLayout = to;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.imageMemoryBarrierCount = 1;
        d.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &d);
    };

    const auto pass = [&](VkImageView target, VkExtent2D extent,
                          VkPipeline pipeline, VkDescriptorSet set,
                          const float push[4], bool load) {
        VkRenderingAttachmentInfo c{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        c.imageView = target;
        c.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        c.loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD
                        : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        c.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo r{VK_STRUCTURE_TYPE_RENDERING_INFO};
        r.renderArea = {{0, 0}, extent};
        r.layerCount = 1;
        r.colorAttachmentCount = 1;
        r.pColorAttachments = &c;
        vkCmdBeginRendering(cmd, &r);
        VkViewport vp{0.0f, 0.0f, static_cast<float>(extent.width),
                      static_cast<float>(extent.height), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, extent};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                bloomLayout_, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, bloomLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           4 * sizeof(float), push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
    };

    // 1: scene -> bloom target, thresholded and downsampled.
    //
    // UNDEFINED as the old layout: legal from any state and it discards the
    // contents, which is what we want since the pass does not load them. It
    // also covers the first frame after (re)creation, when the image really
    // is UNDEFINED.
    barrier(bloomTex_.handle, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    barrier(sceneColor_.handle, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const float ex[4] = {1.0f / static_cast<float>(bloomExtent_.width),
                         1.0f / static_cast<float>(bloomExtent_.height),
                         0.45f,  // threshold: the glow clips well past this,
                                 // ordinary lit copper does not reach it
                         0.0f};
    pass(bloomTex_.view, bloomExtent_, bloomExtractPipeline_, bloomSrcSet_, ex,
         false);

    // 2: add it back over the scene.
    barrier(bloomTex_.handle, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(sceneColor_.handle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    const float co[4] = {1.0f / static_cast<float>(sceneExtent_.width),
                         1.0f / static_cast<float>(sceneExtent_.height),
                         2.6f,  // intensity
                         0.0f};
    pass(sceneColor_.view, sceneExtent_, bloomCompositePipeline_, bloomTexSet_,
         co, true);
}

void Renderer::createSyncAndCommands() {
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = device_.gpu.graphicsQueueFamily;
    check(vkCreateCommandPool(device_.handle, &poolInfo, nullptr, &commandPool_),
          "vkCreateCommandPool");

    commandBuffers_.resize(kFramesInFlight);
    VkCommandBufferAllocateInfo alloc{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = commandPool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = kFramesInFlight;
    check(vkAllocateCommandBuffers(device_.handle, &alloc, commandBuffers_.data()),
          "vkAllocateCommandBuffers");

    imageAvailable_.resize(kFramesInFlight);
    inFlight_.resize(kFramesInFlight);
    // One render-finished semaphore per swapchain image, not per frame in
    // flight: present waits on it, and reusing it across images trips the
    // validation layer.
    renderFinished_.resize(images_.size());

    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        check(vkCreateSemaphore(device_.handle, &sem, nullptr, &imageAvailable_[i]),
              "vkCreateSemaphore");
        check(vkCreateFence(device_.handle, &fence, nullptr, &inFlight_[i]),
              "vkCreateFence");
    }
    for (size_t i = 0; i < images_.size(); ++i) {
        check(vkCreateSemaphore(device_.handle, &sem, nullptr, &renderFinished_[i]),
              "vkCreateSemaphore");
    }
}

void Renderer::uploadBoard(const geom::BoardMesh& mesh) {
    // Feed the Embree tracer the same mesh (CPU device only). Its own BVH is
    // separate from the Vulkan buffers built below for raster.
    if (cpuMode_ && cpuTracer_) {
        cpuTracer_->setScene(mesh);
        cpuTracerGen_ = 0xFFFFFFFFu;  // restart accumulation on the new board
    }

    std::vector<geom::Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<MaterialGpu> materials;
    std::vector<uint32_t> triMaterial;  // per global triangle: material index (path tracer)
    std::vector<int32_t> triNet;        // per global triangle: net index, -1 none
    std::vector<uint32_t> vertexMat;    // per global vertex: material index (explode bake)
    std::vector<float> centreZ;  // per material, for explode ranking
    std::vector<int> mount;      // per material: 0 board layer, +/-1 component side
    draws_.clear();
    parts_.clear();

    stats_.trianglesTotal = 0;

    // Opaque parts FIRST, translucent films (mask, substrate) LAST. The BLAS is
    // built as two geometry ranges over this very index buffer -- [0, N) opaque
    // (hardware fast-path traversal), [N, end) candidate-traversed for the path
    // tracer's stochastic alpha -- and a range must be contiguous. Raster draws
    // by explicit per-part offsets, so the order is free to serve the tracer.
    // (Cosmetic side effect: the parts list shows films after the copper.)
    const auto isFilm = [](const geom::Part& p) {
        return p.material == geom::Material::Soldermask ||
               p.material == geom::Material::Substrate;
    };
    std::vector<const geom::Part*> ordered;
    ordered.reserve(mesh.parts.size());
    for (const geom::Part& p : mesh.parts)
        if (!p.mesh.indices.empty() && !isFilm(p)) ordered.push_back(&p);
    const size_t firstFilm = ordered.size();
    for (const geom::Part& p : mesh.parts)
        if (!p.mesh.indices.empty() && isFilm(p)) ordered.push_back(&p);

    opaqueTriCount_ = 0;
    size_t orderedIdx = 0;
    for (const geom::Part* orderedPart : ordered) {
        const geom::Part& part = *orderedPart;
        const bool opaquePart = orderedIdx++ < firstFilm;
        if (opaquePart)
            opaqueTriCount_ +=
                static_cast<uint32_t>(part.mesh.indices.size() / 3);

        DrawItem item;
        item.indexOffset = static_cast<uint32_t>(indices.size());
        item.indexCount = static_cast<uint32_t>(part.mesh.indices.size());
        // Indices are rebased to GLOBAL (see the insert below) so a single BLAS
        // and the path tracer address one flat vertex buffer correctly; the raster
        // draw therefore uses vertexOffset 0. A per-part vertexOffset with local
        // indices would leave the acceleration structure referencing the wrong
        // vertices for every part but the first.
        item.vertexOffset = 0;
        item.material = static_cast<uint32_t>(materials.size());
        item.blended = (part.material == geom::Material::Soldermask);
        // The laminate is what hides the copper, so it fades once the stack
        // starts peeling -- but stays fully solid at rest.
        item.fadesWhenPeeled = (part.material == geom::Material::Substrate);
        // Inner copper (In*.Cu, not the outer F/B foils) is buried inside the
        // laminate; the only place it shows on a solid collapsed board is a thin
        // z-fighting line at the cut edge, where its outline coincides with the
        // dielectric's. Hide it until the board peels open -- see drawFrame.
        item.hideWhenCollapsed =
            part.material == geom::Material::Copper && part.name != "F.Cu" &&
            part.name != "B.Cu" && part.name != "vias";
        item.part = static_cast<uint32_t>(parts_.size());

        PartInfo info;
        info.name = part.name;
        info.triangles = static_cast<uint32_t>(part.mesh.triangleCount());
        info.blended = item.blended;
        info.visible = true;
        info.material = part.material;
        info.partialBarrel = part.partialBarrel;
        parts_.push_back(std::move(info));

        // params: roughness, metallic, explode rank (filled below), fade flag.
        MaterialGpu m{};
        switch (part.material) {
            case geom::Material::Substrate:
                m = {{0.72f, 0.61f, 0.38f, 1.0f}, {0.85f, 0.0f, 0.0f, 1.0f}};
                break;
            case geom::Material::Soldermask:
                // Industry-standard green (#19882C, linearised), semi-gloss finish
                // (roughness 0.15-0.25 per fab data; matte boards run 0.4-0.6).
                m = {{0.010f, 0.246f, 0.025f, 0.72f}, {0.20f, 0.0f, 0.0f, 0.0f}};
                break;
            case geom::Material::Silkscreen:
                // Opaque ink. Not pure white: real silkscreen is slightly warm
                // and matte, and pure white blows out against the mask.
                m = {{0.90f, 0.90f, 0.87f, 1.0f}, {0.80f, 0.0f, 0.0f, 0.0f}};
                break;
            case geom::Material::Component:
                // Colour comes from the source 3D model, not the enum. Semi-matte
                // plastic/ceramic default; the model's own baseColorFactor wins.
                m = {{part.color[0], part.color[1], part.color[2], part.color[3]},
                     {0.55f, 0.1f, 0.0f, 0.0f}};
                break;
            default:
                // Copper / exposed pads: gold ENIG finish, near-mirror roughness
                // so pads read as polished plate -- a tight sharp highlight in
                // raster, a near-specular reflection + sun glint in the path
                // tracer (user request: pads more mirror than the chip sides).
                m = {{0.94f, 0.70f, 0.28f, 1.0f}, {0.05f, 1.0f, 0.0f, 0.0f}};
                break;
        }
        materials.push_back(m);

        double sx = 0.0, sy = 0.0, sz = 0.0;
        for (const geom::Vertex& v : part.mesh.vertices) {
            sx += v.position[0];
            sy += v.position[1];
            sz += v.position[2];
        }
        const double n =
            static_cast<double>(std::max<size_t>(part.mesh.vertices.size(), 1));
        item.centre[0] = static_cast<float>(sx / n);
        item.centre[1] = static_cast<float>(sy / n);
        item.centre[2] = static_cast<float>(sz / n);
        centreZ.push_back(item.centre[2]);
        mount.push_back(part.material == geom::Material::Component ? part.mountSide
                                                                   : 0);

        const uint32_t vbase = static_cast<uint32_t>(vertices.size());
        vertices.insert(vertices.end(), part.mesh.vertices.begin(),
                        part.mesh.vertices.end());
        // Remember which draw item (hence material, hence rank) each vertex belongs
        // to, as a flat parallel array. Filled with the real rank after ranks are
        // computed below -- for now record the material index.
        vertexMat.insert(vertexMat.end(), part.mesh.vertices.size(), item.material);
        indices.reserve(indices.size() + part.mesh.indices.size());
        for (uint32_t li : part.mesh.indices) indices.push_back(vbase + li);

        // Per-triangle material index, in the same global triangle order as the
        // index buffer -- the path tracer looks material up by primitive index.
        // The net index rides along in the same order for highlighting; the
        // material records where this draw's triangles start so the raster
        // shader can turn gl_PrimitiveID into a global index.
        materials[item.material].extra[0] =
            static_cast<uint32_t>(triMaterial.size());
        const uint32_t triCount = item.indexCount / 3;
        for (uint32_t t = 0; t < triCount; ++t) {
            triMaterial.push_back(item.material);
            triNet.push_back(t < part.triNet.size() ? part.triNet[t] : -1);
        }

        stats_.trianglesTotal += static_cast<uint32_t>(part.mesh.triangleCount());
        draws_.push_back(item);
    }

    if (vertices.empty()) throw std::runtime_error("board mesh is empty");

    // Explode ranks, derived from where each part actually sits in Z rather than
    // from layer names -- so it stays right for any stackup, and for the
    // substrate and masks which have no stack index at all.
    //
    // Ranks are signed and centred, so the stack opens symmetrically about the
    // board's mid-plane instead of drifting one way.
    //
    // Indexed by MATERIAL, not by mesh.parts: empty parts are skipped above, so
    // the two indices diverge.
    {
        // Only board layers get a stage. Components are excluded here and pinned
        // to the outer ring below, so a populated board explodes into the same
        // number of stages as a bare one -- the ICs float off with the top (or
        // bottom) copper instead of each claiming a stage of their own.
        // Via barrels are excluded: they are one intact plated tube through the
        // whole stack, not a layer, so they claim no stage and keep rank 0 (their
        // copper material's default) -- staying put while the board peels.
        std::vector<std::pair<float, size_t>> byHeight;
        byHeight.reserve(centreZ.size());
        for (size_t m = 0; m < centreZ.size(); ++m) {
            const bool isVia = m < parts_.size() && parts_[m].name == "vias";
            if (mount[m] == 0 && !isVia) byHeight.emplace_back(centreZ[m], m);
        }
        std::sort(byHeight.begin(), byHeight.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        const float mid = (byHeight.size() - 1) * 0.5f;
        for (size_t i = 0; i < byHeight.size(); ++i) {
            materials[byHeight[i].second].params[2] = static_cast<float>(i) - mid;
        }
        maxRank_ = mid;  // board rings span [-mid, +mid]

        // Components peel onto their OWN plane, one stage BEYOND the outermost
        // board ring on their mounting side -- so an IC lifts clear of the
        // silkscreen it sits on instead of sharing its plane. Being one ring past
        // maxRank, a component is the first thing to move and ends a full stage
        // above the top layer. This adds one stage to a complete peel.
        bool haveComponents = false;
        for (size_t m = 0; m < mount.size(); ++m) {
            if (mount[m] == 0) continue;
            materials[m].params[2] = (mount[m] > 0) ? (mid + 1.0f) : -(mid + 1.0f);
            haveComponents = true;
        }
        if (haveComponents) maxRank_ = mid + 1.0f;

        // Via barrels get their OWN peel plane (user request): a barrel is one
        // intact plated tube, and sharing a level with a substrate slab leaves it
        // poking through the laminate mid-explode. Layer ranks are consecutive
        // integers centred on 0, so 0 itself is taken exactly when the layer
        // count is odd; in that case park the barrels at +0.5 -- squarely between
        // two rings, on no one else's plane. (This was reverted once over what
        // turned out to be a capture-aliasing phantom; the logic is sound.)
        {
            const auto rankTaken = [&](float r) {
                for (const auto& bh : byHeight)
                    if (std::abs(materials[bh.second].params[2] - r) < 1e-3f)
                        return true;
                return false;
            };
            const float barrelRank = rankTaken(0.0f) ? 0.5f : 0.0f;

            // A blind/buried barrel is NOT one intact tube through the stack:
            // it spans a few layers and should travel WITH them, not stay
            // pinned. Rank it by its centre Z, interpolated between the board
            // layers' consecutive ranks -- fractional, so it claims no stage
            // of its own and rides between its end layers.
            const auto rankAtZ = [&](float z) {
                if (byHeight.empty()) return 0.0f;
                if (z <= byHeight.front().first)
                    return materials[byHeight.front().second].params[2];
                if (z >= byHeight.back().first)
                    return materials[byHeight.back().second].params[2];
                for (size_t i = 0; i + 1 < byHeight.size(); ++i) {
                    const float z0 = byHeight[i].first;
                    const float z1 = byHeight[i + 1].first;
                    if (z <= z1) {
                        const float t = (z1 > z0) ? (z - z0) / (z1 - z0) : 0.0f;
                        return materials[byHeight[i].second].params[2] + t;
                    }
                }
                return 0.0f;
            };
            for (size_t m = 0; m < parts_.size() && m < materials.size(); ++m) {
                if (parts_[m].name != "vias") continue;
                materials[m].params[2] = parts_[m].partialBarrel
                                             ? rankAtZ(centreZ[m])
                                             : barrelRank;
            }
        }

        // Mid-plane of the stack in Z, for the translucent sort's view-side test.
        if (!byHeight.empty()) {
            boardMidZ_ = 0.5f * (byHeight.front().first + byHeight.back().first);
        }

        // Mirror the rank onto the draw items so the blend pass can work out
        // where a part actually sits once peeled, and sort by it.
        for (DrawItem& item : draws_) {
            item.rank = materials[item.material].params[2];
        }
    }

    // Rule 2: these usage flags cost nothing today and mean phase 4 builds its
    // acceleration structures over these very buffers. STORAGE too, so the path
    // tracer can read vertices/indices as SSBOs to shade a ray hit.
    const VkBufferUsageFlags rtReady =
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;

    destroyBuffer(vertexBuffer_);
    destroyBuffer(indexBuffer_);
    destroyBuffer(materialBuffer_);
    destroyBuffer(triMaterialBuffer_);
    destroyBuffer(tracedVertexBuffer_);

    const VkDeviceSize vsize = vertices.size() * sizeof(geom::Vertex);
    vertexBuffer_ = createBuffer(vsize,
                                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT | rtReady,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    uploadViaStaging(vertexBuffer_, vertices.data(), vsize);

    // Exploded/visibility-baked copy the BLAS builds from; bakeExplode fills it
    // below and rebuildTracedGeometry re-bakes on peel or visibility changes.
    tracedVertexBuffer_ =
        createBuffer(vsize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | rtReady,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // Keep the rest geometry, a per-vertex rank and a per-vertex part index on
    // the CPU so the traced copy can be re-baked without the mesh. vertexMat
    // holds each vertex's material index (== part index); resolve rank now that
    // ranks are known, and keep the part index for the visibility bake.
    restVertices_ = vertices;
    restIndices_ = indices;
    triNetCpu_ = triNet;
    netCount_ = static_cast<uint32_t>(mesh.nets.size());
    highlightNets_.clear();
    uploadNetColors({}, {});  // sized to this board, all nets off
    vertexRank_.resize(vertexMat.size());
    for (size_t i = 0; i < vertexMat.size(); ++i)
        vertexRank_[i] = materials[vertexMat[i]].params[2];
    vertexPart_ = std::move(vertexMat);

    // Bake through the SAME path every later re-bake uses, so the very first
    // traced frame already honours the current peel and the collapsed-board
    // hide rule (a raw copy left inner copper in the BLAS until something
    // triggered a re-bake).
    bakeExplode(explodeProgress_);
    tracedExplode_ = explodeProgress_;
    tracedVisDirty_ = false;

    const VkDeviceSize isize = indices.size() * sizeof(uint32_t);
    indexBuffer_ = createBuffer(isize,
                                VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT | rtReady,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    uploadViaStaging(indexBuffer_, indices.data(), isize);

    if (!triMaterial.empty()) {
        const VkDeviceSize tsize = triMaterial.size() * sizeof(uint32_t);
        triMaterialBuffer_ = createBuffer(
            tsize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        uploadViaStaging(triMaterialBuffer_, triMaterial.data(), tsize);
    }

    // Per-triangle net AND position along that net, for highlighting and the
    // chase animation. Always allocated (a one-entry dummy when the board has
    // no nets) so the descriptor is never left dangling -- the raster shaders
    // read it unconditionally.
    //
    // The phase is how far along its net a triangle sits, 0 at one end and 1
    // at the other, so a shader can sweep a head down the run without knowing
    // anything about the geometry. Measured as straight-line distance from
    // the net's most extreme triangle: a graph walk would be truer to the
    // copper, but for a travelling highlight the eye only needs a consistent
    // ordering from one end to the other.
    {
        struct TriInfo {
            int32_t net;
            float phase;
        };
        std::vector<TriInfo> info(triNet.size());
        for (size_t i = 0; i < triNet.size(); ++i) info[i] = {triNet[i], 0.0f};

        // Centroids once, then per net: farthest-from-mean is one end.
        std::map<int, std::vector<size_t>> byNet;
        for (size_t t = 0; t < triNet.size(); ++t)
            if (triNet[t] >= 0) byNet[triNet[t]].push_back(t);

        const auto centroid = [&](size_t t) {
            const geom::Vertex& a = vertices[indices[t * 3 + 0]];
            const geom::Vertex& b = vertices[indices[t * 3 + 1]];
            const geom::Vertex& c = vertices[indices[t * 3 + 2]];
            return std::array<float, 3>{
                (a.position[0] + b.position[0] + c.position[0]) / 3.0f,
                (a.position[1] + b.position[1] + c.position[1]) / 3.0f,
                (a.position[2] + b.position[2] + c.position[2]) / 3.0f};
        };
        const auto dist = [](const std::array<float, 3>& p,
                             const std::array<float, 3>& q) {
            const float dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };

        for (const auto& [net, tris] : byNet) {
            std::vector<std::array<float, 3>> cs;
            cs.reserve(tris.size());
            std::array<float, 3> mean{0, 0, 0};
            for (size_t t : tris) {
                cs.push_back(centroid(t));
                for (int k = 0; k < 3; ++k) mean[k] += cs.back()[k];
            }
            for (int k = 0; k < 3; ++k) mean[k] /= static_cast<float>(cs.size());

            size_t endIdx = 0;
            float best = -1.0f;
            for (size_t i = 0; i < cs.size(); ++i) {
                const float d = dist(cs[i], mean);
                if (d > best) { best = d; endIdx = i; }
            }
            const std::array<float, 3> endPt = cs[endIdx];
            float span = 1e-6f;
            for (const auto& c : cs) span = std::max(span, dist(c, endPt));
            for (size_t i = 0; i < tris.size(); ++i)
                info[tris[i]].phase = dist(cs[i], endPt) / span;
        }

        destroyBuffer(triNetBuffer_);
        if (info.empty()) info.push_back({-1, 0.0f});
        const VkDeviceSize nsize = info.size() * sizeof(TriInfo);
        triNetBuffer_ = createBuffer(
            nsize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        // From info, NOT triNet: nsize counts 8-byte TriInfo entries, so
        // staging from the 4-byte-per-triangle net array reads twice its
        // length and walks off the end of the heap block.
        uploadViaStaging(triNetBuffer_, info.data(), nsize);
    }

    const VkDeviceSize msize = materials.size() * sizeof(MaterialGpu);
    materialBuffer_ = createBuffer(msize,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    uploadViaStaging(materialBuffer_, materials.data(), msize);

    VkDescriptorBufferInfo bufferInfo{materialBuffer_.handle, 0, msize};
    VkDescriptorBufferInfo netInfo{triNetBuffer_.handle, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet writes[2]{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet = materialSet_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &bufferInfo;
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[1].dstSet = materialSet_;
    writes[1].dstBinding = kNetBinding;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &netInfo;
    vkUpdateDescriptorSets(device_.handle, 2, writes, 0, nullptr);

    // Build the RT acceleration structure over this geometry and point binding 1
    // at it. Rebuilt every upload -- a new board / thickness changes the buffers.
    if (rtSupported_) {
        buildAccelerationStructures(static_cast<uint32_t>(vertices.size()),
                                    static_cast<uint32_t>(indices.size()));
        if (tlas_ != VK_NULL_HANDLE) {
            VkWriteDescriptorSetAccelerationStructureKHR asInfo{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
            asInfo.accelerationStructureCount = 1;
            asInfo.pAccelerationStructures = &tlas_;
            VkWriteDescriptorSet asWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            asWrite.pNext = &asInfo;
            asWrite.dstSet = materialSet_;
            asWrite.dstBinding = 1;
            asWrite.descriptorCount = 1;
            asWrite.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            vkUpdateDescriptorSets(device_.handle, 1, &asWrite, 0, nullptr);
        }
        // Point the path tracer's descriptors at the new geometry + TLAS.
        updatePathTraceDescriptors();
    }

    // Keep a CPU copy so appearance edits re-write the table without rebuilding
    // geometry. Re-apply the current substrate override so it survives a board
    // reload rather than snapping back to default tan.
    materials_ = std::move(materials);
    setSubstrateAppearance(substrateColor_[0], substrateColor_[1],
                           substrateColor_[2], substrateOpacity_);
    setMaskColor(maskColor_[0], maskColor_[1], maskColor_[2], maskOpacity_);
    setComponentShine(componentShine_);
    setPadShine(padShine_);
}

void Renderer::setComponentShine(float s01) {
    componentShine_ = std::clamp(s01, 0.0f, 1.0f);
    if (materials_.empty() || !materialBuffer_.handle) return;
    for (size_t i = 0; i < parts_.size() && i < materials_.size(); ++i) {
        if (parts_[i].material != geom::Material::Component) continue;
        // 0 -> the stock semi-matte plastic (rough 0.55, metallic 0.10);
        // 1 -> chrome (rough 0.04, metallic 1.0): in the path tracer the glossy
        // lobe becomes a near-mirror, so component bodies reflect the board and
        // their neighbours. Deliberately stylised, not physically accurate.
        materials_[i].params[0] = 0.55f - 0.51f * componentShine_;
        materials_[i].params[1] = 0.10f + 0.90f * componentShine_;
    }
    if (cpuMode_ && cpuTracer_) cpuTracer_->setComponentShine(componentShine_);
    vkDeviceWaitIdle(device_.handle);
    uploadViaStaging(materialBuffer_, materials_.data(),
                     materials_.size() * sizeof(MaterialGpu));
    resetAccumulation();
}

void Renderer::setPadShine(float s01) {
    padShine_ = std::clamp(s01, 0.0f, 1.0f);
    if (materials_.empty() || !materialBuffer_.handle) return;
    for (size_t i = 0; i < parts_.size() && i < materials_.size(); ++i) {
        if (parts_[i].material != geom::Material::Copper) continue;
        // Copper stays fully metallic; the slider polishes it: rough 0.5 (dull
        // brushed) down to 0.02 (mirror plate).
        materials_[i].params[0] = 0.5f - 0.48f * padShine_;
    }
    if (cpuMode_ && cpuTracer_) cpuTracer_->setPadShine(padShine_);
    vkDeviceWaitIdle(device_.handle);
    uploadViaStaging(materialBuffer_, materials_.data(),
                     materials_.size() * sizeof(MaterialGpu));
    resetAccumulation();
}

void Renderer::setShadowSoftness(float s01) {
    shadowSoftness_ = std::clamp(s01, 0.0f, 1.0f);
    if (cpuMode_ && cpuTracer_) {
        const float rad = shadowSoftness_ * 8.0f * 3.14159265f / 180.0f;
        cpuTracer_->setSunCosMax(std::cos(rad));
    }
    resetAccumulation();
    ptDenoisedValid_ = false;
}

void Renderer::setHighlightNet(int net) {
    if (net < 0) {
        setHighlightNets({}, {});
    } else {
        setHighlightNets({net}, {{{1.0f, 0.09f, 0.06f}}});
    }
}

void Renderer::setHighlightNets(
    const std::vector<int>& nets,
    const std::vector<std::array<float, 3>>& colours) {
    if (nets == highlightNets_) return;
    highlightNets_ = nets;
    highlightStart_ = std::chrono::steady_clock::now();
    uploadNetColors(nets, colours);
    buildNetLights();
    // The path tracer treats the highlighted net as an emitter, so the
    // converged image is no longer valid.
    resetAccumulation();
    ptDenoisedValid_ = false;
}

// One RGBA per net: the glow colour, with alpha marking it highlighted. The
// shaders index this by the triangle's net id, which is what lets any number
// of nets glow at once in different colours.
void Renderer::uploadNetColors(
    const std::vector<int>& nets,
    const std::vector<std::array<float, 3>>& colours) {
    std::vector<float> table(std::max<uint32_t>(netCount_, 1u) * 4, 0.0f);
    for (size_t i = 0; i < nets.size(); ++i) {
        const int n = nets[i];
        if (n < 0 || static_cast<uint32_t>(n) >= netCount_) continue;
        const std::array<float, 3> c =
            i < colours.size() ? colours[i]
                               : std::array<float, 3>{1.0f, 0.09f, 0.06f};
        table[n * 4 + 0] = c[0];
        table[n * 4 + 1] = c[1];
        table[n * 4 + 2] = c[2];
        table[n * 4 + 3] = 1.0f;  // highlighted
    }

    // Descriptor sets still reference the old buffer; the GPU must be done
    // with them before it can be freed.
    vkDeviceWaitIdle(device_.handle);
    destroyBuffer(netColorBuffer_);
    const VkDeviceSize size = table.size() * sizeof(float);
    netColorBuffer_ = createBuffer(
        size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    uploadViaStaging(netColorBuffer_, table.data(), size);

    // Rebind: the raster set reads it every frame, and a stale handle here is
    // a device-lost rather than a wrong colour.
    VkDescriptorBufferInfo info{netColorBuffer_.handle, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = materialSet_;
    w.dstBinding = kNetColorBinding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &info;
    vkUpdateDescriptorSets(device_.handle, 1, &w, 0, nullptr);
}

// Collect the highlighted nets' triangles into a buffer the path tracer can
// sample as area lights. Each entry is three corners, the triangle's area
// (the sampling weight) and its net id (so the estimator uses that net's
// colour).
void Renderer::buildNetLights() {
    netLightCount_ = 0;
    if (highlightNets_.empty() || restIndices_.empty() || triNetCpu_.empty())
        return;

    std::vector<float> lights;  // 12 floats per triangle: v0,v1,v2 (+area in w)
    const size_t triangles =
        std::min(triNetCpu_.size(), restIndices_.size() / 3);
    for (size_t t = 0; t < triangles; ++t) {
        const int tn = triNetCpu_[t];
        if (std::find(highlightNets_.begin(), highlightNets_.end(), tn) ==
            highlightNets_.end())
            continue;
        const geom::Vertex& a = restVertices_[restIndices_[t * 3 + 0]];
        const geom::Vertex& b = restVertices_[restIndices_[t * 3 + 1]];
        const geom::Vertex& c = restVertices_[restIndices_[t * 3 + 2]];
        // Area via the cross product of two edges.
        const float e1[3] = {b.position[0] - a.position[0],
                             b.position[1] - a.position[1],
                             b.position[2] - a.position[2]};
        const float e2[3] = {c.position[0] - a.position[0],
                             c.position[1] - a.position[1],
                             c.position[2] - a.position[2]};
        const float cx = e1[1] * e2[2] - e1[2] * e2[1];
        const float cy = e1[2] * e2[0] - e1[0] * e2[2];
        const float cz = e1[0] * e2[1] - e1[1] * e2[0];
        const float area = 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);
        if (area <= 0.0f) continue;  // degenerate, and a zero-area light is a
                                     // division by zero in the estimator

        lights.insert(lights.end(), {a.position[0], a.position[1],
                                     a.position[2], area});
        // v1.w carries the net id so the estimator can look up its colour.
        lights.insert(lights.end(), {b.position[0], b.position[1],
                                     b.position[2], static_cast<float>(tn)});
        lights.insert(lights.end(),
                      {c.position[0], c.position[1], c.position[2], 0.0f});
        ++netLightCount_;
        (void)0;
    }

    vkDeviceWaitIdle(device_.handle);  // still bound in the PT descriptor set
    destroyBuffer(netLightBuffer_);
    if (lights.empty()) lights.assign(12, 0.0f);  // never leave the binding null
    const VkDeviceSize size = lights.size() * sizeof(float);
    netLightBuffer_ = createBuffer(
        size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    uploadViaStaging(netLightBuffer_, lights.data(), size);
    updatePathTraceDescriptors();
}

void Renderer::setNetGlow(float strength) {
    strength = std::clamp(strength, 0.0f, 40.0f);
    if (std::abs(strength - netGlow_) < 1e-3f) return;
    netGlow_ = strength;
    resetAccumulation();  // the emitter changed, so the converged image is stale
    ptDenoisedValid_ = false;
}

void Renderer::setCameraAxis(const float fwd[3], float orthoDistance) {
    camFwd_[0] = fwd[0];
    camFwd_[1] = fwd[1];
    camFwd_[2] = fwd[2];
    camOrthoDistance_ = orthoDistance;
}

void Renderer::setMaskColor(float r, float g, float b, float opacity) {
    maskColor_ = {r, g, b};
    const float a = std::clamp(opacity, 0.05f, 1.0f);
    maskOpacity_ = a;
    if (materials_.empty() || !materialBuffer_.handle) return;
    for (size_t i = 0; i < parts_.size() && i < materials_.size(); ++i) {
        // Both F.Mask and B.Mask.
        const std::string& name = parts_[i].name;
        if (name.size() < 5 || name.compare(name.size() - 5, 5, ".Mask") != 0) {
            continue;
        }
        materials_[i].albedo[0] = r;
        materials_[i].albedo[1] = g;
        materials_[i].albedo[2] = b;
        // albedo.a drives the raster blend AND the path tracer's show-through
        // (passProb = 1 - alpha): lower it to let more copper read through.
        materials_[i].albedo[3] = a;
    }
    if (cpuMode_ && cpuTracer_) cpuTracer_->setMaskAppearance(r, g, b, a);
    vkDeviceWaitIdle(device_.handle);
    uploadViaStaging(materialBuffer_, materials_.data(),
                     materials_.size() * sizeof(MaterialGpu));
    // Path tracing accumulates a static image; a colour edit must restart it or
    // the change only appears once the camera next moves.
    resetAccumulation();
    ptDenoisedValid_ = false;
}

void Renderer::setSubstrateAppearance(float r, float g, float b, float opacity) {
    substrateColor_ = {r, g, b};
    substrateOpacity_ = std::clamp(opacity, 0.05f, 1.0f);
    if (materials_.empty() || !materialBuffer_.handle) return;

    for (size_t i = 0; i < parts_.size() && i < materials_.size(); ++i) {
        if (parts_[i].name != "substrate") continue;
        MaterialGpu& m = materials_[i];
        // albedo.a is the AT-REST opacity; the peel fade rides on top via
        // params.w in the shader.
        m.albedo[0] = r;
        m.albedo[1] = g;
        m.albedo[2] = b;
        m.albedo[3] = substrateOpacity_;

        // A translucent substrate lives in the blended pass permanently, not
        // only while peeling. A solid one stays opaque and fades only on peel.
        const bool translucent = substrateOpacity_ < 0.999f;
        for (DrawItem& item : draws_) {
            if (item.material != i) continue;
            item.blended = translucent;
            item.fadesWhenPeeled = !translucent;
        }
        // No break: the substrate is now several dielectric slabs (one between
        // each pair of copper foils), all named "substrate" -- recolour them all.
    }
    if (cpuMode_ && cpuTracer_)
        cpuTracer_->setSubstrateAppearance(r, g, b, substrateOpacity_);
    vkDeviceWaitIdle(device_.handle);
    uploadViaStaging(materialBuffer_, materials_.data(),
                     materials_.size() * sizeof(MaterialGpu));
    // Restart path-trace accumulation so the new appearance shows immediately.
    resetAccumulation();
    ptDenoisedValid_ = false;
}

bool Renderer::drawFrame(const float viewProj[16], const float cameraPos[3],
                         const std::function<void(VkCommandBuffer)>& drawUi) {
    vkWaitForFences(device_.handle, 1, &inFlight_[frame_], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    VkResult acquire =
        vkAcquireNextImageKHR(device_.handle, swapchain_, UINT64_MAX,
                              imageAvailable_[frame_], VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) return false;
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        check(acquire, "vkAcquireNextImageKHR");
    }

    vkResetFences(device_.handle, 1, &inFlight_[frame_]);

    // Keep the traced geometry (BLAS) in sync with whoever is about to trace
    // it: the path tracer always, the raster RT shadows only at rest (their
    // gate). Rebuild before recording (a synchronous AS build must not run
    // inside the command buffer) when the peel moved since the last bake OR the
    // baked-in visibility changed. Costs nothing when nothing changed, and the
    // raster explode animation itself never rebuilds -- it displaces in the
    // vertex shader while the RT gate is closed anyway.
    const bool tracingThisFrame =
        mode_ == RenderMode::PathTraced ||
        (rtRequested_ && explodeProgress_ < 0.01f);
    if (rtSupported_ && tracingThisFrame &&
        (std::abs(explodeProgress_ - tracedExplode_) > 1.0e-4f ||
         tracedVisDirty_)) {
        rebuildTracedGeometry(explodeProgress_);
        tracedVisDirty_ = false;
    }

    VkCommandBuffer cmd = commandBuffers_[frame_];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &begin);

    // Shared by the scene pass and the later blit/UI barriers.
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};

  // CPU device: Embree traces on the host -- full path tracing in PT mode, and
  // the RT preview (sun shadows + AO, no GI) when the RT toggle is on in raster
  // mode at rest (same at-rest gate as the GPU's RT shadows). The result is
  // copied into sceneColor_ (left COLOR_ATTACHMENT_OPTIMAL) and presented by
  // the shared blit.
  const bool cpuPt = cpuMode_ && mode_ == RenderMode::PathTraced;
  const bool cpuRt = cpuMode_ && mode_ == RenderMode::Raster && rtRequested_ &&
                     explodeProgress_ < 0.01f;
  if ((cpuPt || cpuRt) && cpuTracer_ && cpuTracer_->ready()) {
    recordCpuPathTrace(cmd, /*preview=*/cpuRt);
  } else if (mode_ == RenderMode::PathTraced && rtSupported_ &&
      tlas_ != VK_NULL_HANDLE) {
    // Path-trace + tonemap into sceneColor_ (left COLOR_ATTACHMENT_OPTIMAL), so
    // the shared blit + UI below present it exactly like the raster scene.
    recordPathTrace(cmd);
  } else {
    // --- Pass 1: the board, into the offscreen target at sceneExtent_ ---
    //
    // Depth is transitioned from UNDEFINED every frame: it is cleared on load, so
    // discarding the previous contents is free and correct.
    VkImageMemoryBarrier2 barriers[2]{};

    barriers[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barriers[0].image = sceneColor_.handle;
    barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    barriers[1] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barriers[1].dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    barriers[1].image = sceneDepth_.handle;
    barriers[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};

    dep.imageMemoryBarrierCount = 2;
    dep.pImageMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = sceneColor_.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0.118f, 0.118f, 0.118f, 1.0f}};

    VkRenderingAttachmentInfo depthAttach{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAttach.imageView = sceneDepth_.view;
    depthAttach.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttach.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // Reversed-Z: clear to 0 (the far plane), not 1.
    depthAttach.clearValue.depthStencil = {0.0f, 0};

    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, sceneExtent_};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    rendering.pDepthAttachment = &depthAttach;
    vkCmdBeginRendering(cmd, &rendering);

    VkViewport vp{0.0f, 0.0f, static_cast<float>(sceneExtent_.width),
                  static_cast<float>(sceneExtent_.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, sceneExtent_};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    PushConstants push{};
    std::memcpy(push.viewProj, viewProj, sizeof(push.viewProj));
    push.cameraPos[0] = cameraPos[0];
    push.cameraPos[1] = cameraPos[1];
    push.cameraPos[2] = cameraPos[2];
    // cameraPos.w carries the RT toggle for board_rt.frag. Only trace at rest:
    // the BLAS is over the un-exploded geometry, so shadows would be wrong once
    // the stack starts peeling.
    const bool rtOn = rtRequested_ && rtSupported_ &&
                      tlas_ != VK_NULL_HANDLE && explodeProgress_ < 0.01f;
    push.cameraPos[3] = rtOn ? 1.0f : 0.0f;
    push.params[0] = explodeStep_;
    push.params[1] = explodeProgress_;
    push.params[2] = maxRank_;
    // Normalised peel, 0..1: how far the substrate has faded. Exactly 0 at rest,
    // so a collapsed board is bit-for-bit unchanged.
    push.params[3] =
        (maxRank_ > 0.0f) ? std::clamp(explodeProgress_ / maxRank_, 0.0f, 1.0f)
                          : 0.0f;
    push.camAxis[0] = camFwd_[0];
    push.camAxis[1] = camFwd_[1];
    push.camAxis[2] = camFwd_[2];
    push.camAxis[3] = camOrthoDistance_;  // > 0 only in orthographic
    // Now just a flag: the colour (and which nets) live in the net-colour
    // table, so any number can glow at once.
    push.highlight[0] = highlightNets_.empty() ? -1 : 1;
    // Glow as hundredths, to avoid growing the push block past its 128-byte
    // budget for one float.
    push.highlight[1] = static_cast<int32_t>(netGlow_ * 100.0f);
    // Chase clock, milliseconds since the current selection was made -- it
    // restarts on every selection change so the wipe always plays from the
    // end of the net, rather than dropping you into the middle of a cycle.
    push.highlight[2] =
        (netAnimate_ && !highlightNets_.empty())
            ? static_cast<int32_t>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - highlightStart_)
                      .count())
            : 0;
    push.highlight[3] = netAnimate_ ? 1 : 0;
    vkCmdPushConstants(cmd, layout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1,
                            &materialSet_, 0, nullptr);

    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer_.handle, &offset);
    vkCmdBindIndexBuffer(cmd, indexBuffer_.handle, 0, VK_INDEX_TYPE_UINT32);

    stats_.drawCalls = 0;
    stats_.triangles = 0;

    // The substrate is opaque at rest but turns translucent while peeling, so
    // which pass it belongs to depends on the peel.
    const bool peeling = explodeProgress_ > 0.0f;
    const auto isBlended = [&](const DrawItem& i) {
        return i.blended || (i.fadesWhenPeeled && peeling);
    };
    // Buried inner copper only shows at the cut edge of a solid, collapsed board,
    // as a z-fighting line -- noise, not information. Hide it at rest so the board
    // reads as one solid block, and reveal it the moment it peels. A translucent
    // substrate is the exception: there the whole point is to see through to the
    // inner layers, so keep them shown even when collapsed.
    const bool substrateOpaque = substrateOpacity_ >= 0.999f;
    const auto visible = [&](const DrawItem& i) {
        if (i.part < parts_.size() && !parts_[i.part].visible) return false;
        if (i.hideWhenCollapsed && !peeling && substrateOpaque) return false;
        return true;
    };

    // Pass 1: opaque. Writes depth, so it establishes occlusion for everything.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineOpaque_);
    for (const DrawItem& item : draws_) {
        if (isBlended(item) || !visible(item)) continue;
        // firstInstance carries the material index -- gl_InstanceIndex picks it
        // up in the vertex shader. This is rule 3 in practice.
        vkCmdDrawIndexed(cmd, item.indexCount, 1, item.indexOffset,
                         item.vertexOffset, item.material);
        ++stats_.drawCalls;
        stats_.triangles += item.indexCount / 3;
    }

    // Pass 2: translucent, painted BACK TO FRONT.
    //
    // The blend pipeline does not write depth, so it cannot self-sort: a nearer
    // translucent surface drawn first would be composited under a farther one.
    // With mask above, mask below and a fading substrate between them, that is
    // guaranteed to happen -- so sort by distance from the eye each frame.
    // A handful of parts; the sort is noise next to 592k triangles.
    {
        std::vector<const DrawItem*> translucent;
        for (const DrawItem& item : draws_) {
            if (isBlended(item) && visible(item)) translucent.push_back(&item);
        }

        // Peeled Z of a part's centre. explodeProgress_ arrives already eased,
        // and this mirrors board.vert's travel() -- they must agree.
        const auto peeledZ = [&](const DrawItem* i) {
            const float ring = std::abs(i->rank);
            const float travel =
                std::max(explodeProgress_ - (maxRank_ - ring), 0.0f) * explodeStep_;
            return i->centre[2] + (i->rank > 0.0f    ? travel
                                   : i->rank < 0.0f  ? -travel
                                                     : 0.0f);
        };

        // Sort by Z-stack order relative to the camera, NOT eye distance to
        // centroid. The board's translucent layers (mask, substrate) span the
        // same X/Y and differ mainly in Z, so a 3D distance sort is dominated by
        // the near-equal X/Y terms and flips order as the camera orbits --
        // exactly the mask/substrate colour flicker. Ordering purely by Z, with
        // direction set by which side of the board the eye is on, is stable and
        // flips only when the camera crosses the board plane (which is correct).
        const bool cameraAbove = cameraPos[2] > boardMidZ_;
        std::sort(translucent.begin(), translucent.end(),
                  [&](const DrawItem* a, const DrawItem* b) {
                      // Back-to-front: farthest along the view drawn first.
                      return cameraAbove ? peeledZ(a) < peeledZ(b)
                                         : peeledZ(a) > peeledZ(b);
                  });

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineBlend_);
        for (const DrawItem* item : translucent) {
            vkCmdDrawIndexed(cmd, item->indexCount, 1, item->indexOffset,
                             item->vertexOffset, item->material);
            ++stats_.drawCalls;
            stats_.triangles += item->indexCount / 3;
        }
    }

    vkCmdEndRendering(cmd);
  }  // end raster-vs-pathtrace branch

    // Bloom runs on the finished scene, before the blit -- so it works for
    // raster, RT and path tracing alike, and lands in high-res exports too.
    recordBloom(cmd);

    // On an export frame the overlay goes into the SCENE image as well, at the
    // export resolution -- otherwise a high-res screenshot would silently drop
    // the measurements and dimension callouts the user is looking at. The
    // on-screen copy is still drawn later, onto the swapchain.
    if (!capturePath_.empty() && captureScene_ && !overlayTris_.empty()) {
        VkRenderingAttachmentInfo oColor{
            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        oColor.imageView = sceneColor_.view;
        oColor.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        oColor.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // keep the rendered board
        oColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo oPass{VK_STRUCTURE_TYPE_RENDERING_INFO};
        oPass.renderArea = {{0, 0}, sceneExtent_};
        oPass.layerCount = 1;
        oPass.colorAttachmentCount = 1;
        oPass.pColorAttachments = &oColor;
        vkCmdBeginRendering(cmd, &oPass);
        recordOverlay(cmd, sceneExtent_);
        vkCmdEndRendering(cmd);
    }

    // --- Blit the scene up (or down) onto the swapchain ---
    VkImageMemoryBarrier2 toBlit[2]{};
    toBlit[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toBlit[0].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toBlit[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toBlit[0].dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    toBlit[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    toBlit[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toBlit[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toBlit[0].image = sceneColor_.handle;
    toBlit[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    toBlit[1] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toBlit[1].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    toBlit[1].dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    toBlit[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toBlit[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toBlit[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toBlit[1].image = images_[imageIndex];
    toBlit[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    dep.imageMemoryBarrierCount = 2;
    dep.pImageMemoryBarriers = toBlit;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<int32_t>(sceneExtent_.width),
                          static_cast<int32_t>(sceneExtent_.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {static_cast<int32_t>(extent_.width),
                          static_cast<int32_t>(extent_.height), 1};
    // Linear: downsampling a supersampled scene is the whole point, and
    // upsampling a cheap one should at least not look like Lego.
    vkCmdBlitImage(cmd, sceneColor_.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   images_[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, VK_FILTER_LINEAR);

    // --- Pass 2: the UI, straight onto the swapchain at native resolution ---
    VkImageMemoryBarrier2 toUi{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toUi.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    toUi.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toUi.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toUi.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toUi.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toUi.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toUi.image = images_[imageIndex];
    toUi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &toUi;
    vkCmdPipelineBarrier2(cmd, &dep);

    if (drawUi || !overlayTris_.empty()) {
        VkRenderingAttachmentInfo uiColor{
            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        uiColor.imageView = views_[imageIndex];
        uiColor.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        uiColor.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // keep the blitted scene
        uiColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo uiPass{VK_STRUCTURE_TYPE_RENDERING_INFO};
        uiPass.renderArea = {{0, 0}, extent_};
        uiPass.layerCount = 1;
        uiPass.colorAttachmentCount = 1;
        uiPass.pColorAttachments = &uiColor;
        vkCmdBeginRendering(cmd, &uiPass);
        if (drawUi) drawUi(cmd);
        recordOverlay(cmd, extent_);
        vkCmdEndRendering(cmd);
    }

    // COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC
    VkImageMemoryBarrier2 toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toPresent.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toPresent.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    toPresent.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.image = images_[imageIndex];
    toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &toPresent;
    vkCmdPipelineBarrier2(cmd, &dep);

    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &imageAvailable_[frame_];
    submit.pWaitDstStageMask = &wait;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &renderFinished_[imageIndex];
    check(vkQueueSubmit(device_.graphicsQueue, 1, &submit, inFlight_[frame_]),
          "vkQueueSubmit");

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &renderFinished_[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &imageIndex;
    VkResult presented = vkQueuePresentKHR(device_.graphicsQueue, &present);

    if (!capturePath_.empty()) {
        vkQueueWaitIdle(device_.graphicsQueue);
        captureImage(imageIndex);
        capturePath_.clear();
    }

    frame_ = (frame_ + 1) % kFramesInFlight;

    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        return false;
    }
    check(presented, "vkQueuePresentKHR");
    return true;
}

void Renderer::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    vkDeviceWaitIdle(device_.handle);

    for (VkSemaphore s : renderFinished_) vkDestroySemaphore(device_.handle, s, nullptr);
    renderFinished_.clear();

    destroySceneTargets();
    destroySwapchain();
    createSwapchain(width, height);
    createSceneTargets();
    createBloomTargets();

    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    renderFinished_.resize(images_.size());
    for (size_t i = 0; i < images_.size(); ++i) {
        check(vkCreateSemaphore(device_.handle, &sem, nullptr, &renderFinished_[i]),
              "vkCreateSemaphore");
    }
}

void Renderer::waitIdle() { vkDeviceWaitIdle(device_.handle); }

void Renderer::captureImage(uint32_t imageIndex) {
    // Either the presented swapchain image (window size) or the offscreen
    // scene image (internal render scale -- the high-resolution export).
    const VkImage srcImage =
        captureScene_ ? sceneColor_.handle : images_[imageIndex];
    const VkExtent2D srcExtent = captureScene_ ? sceneExtent_ : extent_;
    // The scene image was left in TRANSFER_SRC by the blit; the swapchain in
    // PRESENT_SRC by the present barrier.
    const VkImageLayout srcLayout = captureScene_
                                        ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                        : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    const VkDeviceSize size =
        static_cast<VkDeviceSize>(srcExtent.width) * srcExtent.height * 4;

    Buffer host = createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkCommandBufferAllocateInfo alloc{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = commandPool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(device_.handle, &alloc, &cmd),
          "vkAllocateCommandBuffers(capture)");

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkImageMemoryBarrier2 toSrc{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    toSrc.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    toSrc.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toSrc.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    toSrc.oldLayout = srcLayout;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image = srcImage;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &toSrc;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {srcExtent.width, srcExtent.height, 1};
    vkCmdCopyImageToBuffer(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           host.handle, 1, &copy);

    VkImageMemoryBarrier2 back = toSrc;
    back.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    back.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    back.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = srcLayout;
    dep.pImageMemoryBarriers = &back;
    vkCmdPipelineBarrier2(cmd, &dep);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    check(vkQueueSubmit(device_.graphicsQueue, 1, &submit, VK_NULL_HANDLE),
          "vkQueueSubmit(capture)");
    vkQueueWaitIdle(device_.graphicsQueue);
    vkFreeCommandBuffers(device_.handle, commandPool_, 1, &cmd);

    void* mapped = nullptr;
    check(vkMapMemory(device_.handle, host.memory, 0, size, 0, &mapped),
          "vkMapMemory(capture)");

    // The swapchain is B8G8R8A8_SRGB, which is already the byte order a 24-bit
    // BMP wants -- so no channel swap, just drop alpha and flip rows.
    const uint8_t* src = static_cast<const uint8_t*>(mapped);
    const int rowBytes = static_cast<int>(srcExtent.width) * 3;
    const int padding = (4 - (rowBytes % 4)) % 4;
    const int imageBytes =
        (rowBytes + padding) * static_cast<int>(srcExtent.height);
    const int fileBytes = 54 + imageBytes;

    std::vector<uint8_t> bmp;
    bmp.reserve(fileBytes);
    uint8_t header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    std::memcpy(&header[2], &fileBytes, 4);
    const int offset = 54;
    std::memcpy(&header[10], &offset, 4);
    const int dibSize = 40;
    std::memcpy(&header[14], &dibSize, 4);
    const int w = static_cast<int>(srcExtent.width);
    const int h = static_cast<int>(srcExtent.height);
    std::memcpy(&header[18], &w, 4);
    std::memcpy(&header[22], &h, 4);
    const uint16_t planes = 1, bpp = 24;
    std::memcpy(&header[26], &planes, 2);
    std::memcpy(&header[28], &bpp, 2);
    std::memcpy(&header[34], &imageBytes, 4);
    bmp.insert(bmp.end(), header, header + 54);

    for (int y = h - 1; y >= 0; --y) {
        const uint8_t* row = src + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            bmp.push_back(row[x * 4 + 0]);
            bmp.push_back(row[x * 4 + 1]);
            bmp.push_back(row[x * 4 + 2]);
        }
        for (int p = 0; p < padding; ++p) bmp.push_back(0);
    }
    vkUnmapMemory(device_.handle, host.memory);
    destroyBuffer(host);

    std::ofstream out(capturePath_, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write capture: " + capturePath_);
    out.write(reinterpret_cast<const char*>(bmp.data()),
              static_cast<std::streamsize>(bmp.size()));
}

}  // namespace pcbview::vk
