#include "render/vk/renderer.h"

#include <algorithm>
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

constexpr uint32_t kFramesInFlight = 2;

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
};

}  // namespace

namespace pcbview::vk {

Renderer::Renderer(Device& device, VkSurfaceKHR surface, uint32_t width,
                   uint32_t height)
    : device_(device), surface_(surface) {
    createSwapchain(width, height);
    createSceneTargets();
    createDescriptors();
    createPipeline();
    createSyncAndCommands();
}

Renderer::~Renderer() {
    if (device_.handle == VK_NULL_HANDLE) return;
    vkDeviceWaitIdle(device_.handle);

    destroyBuffer(vertexBuffer_);
    destroyBuffer(indexBuffer_);
    destroyBuffer(materialBuffer_);
    destroySceneTargets();

    for (VkSemaphore s : imageAvailable_) vkDestroySemaphore(device_.handle, s, nullptr);
    for (VkSemaphore s : renderFinished_) vkDestroySemaphore(device_.handle, s, nullptr);
    for (VkFence f : inFlight_) vkDestroyFence(device_.handle, f, nullptr);
    if (commandPool_) vkDestroyCommandPool(device_.handle, commandPool_, nullptr);

    if (pipelineOpaque_) vkDestroyPipeline(device_.handle, pipelineOpaque_, nullptr);
    if (pipelineBlend_) vkDestroyPipeline(device_.handle, pipelineBlend_, nullptr);
    if (layout_) vkDestroyPipelineLayout(device_.handle, layout_, nullptr);
    if (descriptorPool_) vkDestroyDescriptorPool(device_.handle, descriptorPool_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(device_.handle, setLayout_, nullptr);

    destroySwapchain();
}

uint32_t Renderer::findMemoryType(uint32_t filter,
                                  VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(device_.gpu.handle, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if ((filter & (1u << i)) &&
            (mem.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    throw std::runtime_error("no suitable memory type");
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

    sceneColor_ = createImage(sceneExtent_, colorFormat_,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT);
    sceneDepth_ = createImage(sceneExtent_, depthFormat_,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                              VK_IMAGE_ASPECT_DEPTH_BIT);
}

void Renderer::destroySceneTargets() {
    destroyImage(sceneColor_);
    destroyImage(sceneDepth_);
}

void Renderer::setRenderScale(float scale) {
    scale = std::clamp(scale, 0.25f, 4.0f);
    if (std::abs(scale - renderScale_) < 1e-4f) return;
    vkDeviceWaitIdle(device_.handle);
    renderScale_ = scale;
    destroySceneTargets();
    createSceneTargets();
}

void Renderer::createDescriptors() {
    // Rule 3: one bindless SSBO of materials, indexed by instance ID. Not a
    // descriptor set per draw -- a closest-hit shader cannot rebind.
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    // Vertex too: it reads the per-part explode rank from the same table.
    binding.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = 1;
    info.pBindings = &binding;
    check(vkCreateDescriptorSetLayout(device_.handle, &info, nullptr, &setLayout_),
          "vkCreateDescriptorSetLayout");

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = 1;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
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

    VkShaderModuleCreateInfo fsInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    fsInfo.codeSize = sizeof(kBoardFrag);
    fsInfo.pCode = kBoardFrag;
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
    std::vector<geom::Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<MaterialGpu> materials;
    std::vector<float> centreZ;  // per material, for explode ranking
    std::vector<int> mount;      // per material: 0 board layer, +/-1 component side
    draws_.clear();
    parts_.clear();

    stats_.trianglesTotal = 0;

    for (const geom::Part& part : mesh.parts) {
        if (part.mesh.indices.empty()) continue;

        DrawItem item;
        item.indexOffset = static_cast<uint32_t>(indices.size());
        item.indexCount = static_cast<uint32_t>(part.mesh.indices.size());
        item.vertexOffset = static_cast<int32_t>(vertices.size());
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
            part.name != "B.Cu";
        item.part = static_cast<uint32_t>(parts_.size());

        PartInfo info;
        info.name = part.name;
        info.triangles = static_cast<uint32_t>(part.mesh.triangleCount());
        info.blended = item.blended;
        info.visible = true;
        parts_.push_back(std::move(info));

        // params: roughness, metallic, explode rank (filled below), fade flag.
        MaterialGpu m{};
        switch (part.material) {
            case geom::Material::Substrate:
                m = {{0.72f, 0.61f, 0.38f, 1.0f}, {0.85f, 0.0f, 0.0f, 1.0f}};
                break;
            case geom::Material::Soldermask:
                m = {{0.05f, 0.29f, 0.12f, 0.72f}, {0.35f, 0.0f, 0.0f, 0.0f}};
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
                m = {{0.90f, 0.66f, 0.24f, 1.0f}, {0.25f, 1.0f, 0.0f, 0.0f}};
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

        vertices.insert(vertices.end(), part.mesh.vertices.begin(),
                        part.mesh.vertices.end());
        indices.insert(indices.end(), part.mesh.indices.begin(),
                       part.mesh.indices.end());

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
        std::vector<std::pair<float, size_t>> byHeight;
        byHeight.reserve(centreZ.size());
        for (size_t m = 0; m < centreZ.size(); ++m) {
            if (mount[m] == 0) byHeight.emplace_back(centreZ[m], m);
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
    // acceleration structures over these very buffers.
    const VkBufferUsageFlags rtReady =
        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;

    destroyBuffer(vertexBuffer_);
    destroyBuffer(indexBuffer_);
    destroyBuffer(materialBuffer_);

    const VkDeviceSize vsize = vertices.size() * sizeof(geom::Vertex);
    vertexBuffer_ = createBuffer(vsize,
                                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT | rtReady,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    uploadViaStaging(vertexBuffer_, vertices.data(), vsize);

    const VkDeviceSize isize = indices.size() * sizeof(uint32_t);
    indexBuffer_ = createBuffer(isize,
                                VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT | rtReady,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    uploadViaStaging(indexBuffer_, indices.data(), isize);

    const VkDeviceSize msize = materials.size() * sizeof(MaterialGpu);
    materialBuffer_ = createBuffer(msize,
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                       VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    uploadViaStaging(materialBuffer_, materials.data(), msize);

    VkDescriptorBufferInfo bufferInfo{materialBuffer_.handle, 0, msize};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = materialSet_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(device_.handle, 1, &write, 0, nullptr);

    // Keep a CPU copy so appearance edits re-write the table without rebuilding
    // geometry. Re-apply the current substrate override so it survives a board
    // reload rather than snapping back to default tan.
    materials_ = std::move(materials);
    setSubstrateAppearance(substrateColor_[0], substrateColor_[1],
                           substrateColor_[2], substrateOpacity_);
    setMaskColor(maskColor_[0], maskColor_[1], maskColor_[2]);
}

void Renderer::setMaskColor(float r, float g, float b) {
    maskColor_ = {r, g, b};
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
        // albedo[3] (opacity) left untouched -- the film stays translucent.
    }
    vkDeviceWaitIdle(device_.handle);
    uploadViaStaging(materialBuffer_, materials_.data(),
                     materials_.size() * sizeof(MaterialGpu));
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
    vkDeviceWaitIdle(device_.handle);
    uploadViaStaging(materialBuffer_, materials_.data(),
                     materials_.size() * sizeof(MaterialGpu));
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

    VkCommandBuffer cmd = commandBuffers_[frame_];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &begin);

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

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
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
    push.cameraPos[3] = 1.0f;
    push.params[0] = explodeStep_;
    push.params[1] = explodeProgress_;
    push.params[2] = maxRank_;
    // Normalised peel, 0..1: how far the substrate has faded. Exactly 0 at rest,
    // so a collapsed board is bit-for-bit unchanged.
    push.params[3] =
        (maxRank_ > 0.0f) ? std::clamp(explodeProgress_ / maxRank_, 0.0f, 1.0f)
                          : 0.0f;
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

    if (drawUi) {
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
        drawUi(cmd);
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

    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    renderFinished_.resize(images_.size());
    for (size_t i = 0; i < images_.size(); ++i) {
        check(vkCreateSemaphore(device_.handle, &sem, nullptr, &renderFinished_[i]),
              "vkCreateSemaphore");
    }
}

void Renderer::waitIdle() { vkDeviceWaitIdle(device_.handle); }

void Renderer::captureImage(uint32_t imageIndex) {
    const VkDeviceSize size =
        static_cast<VkDeviceSize>(extent_.width) * extent_.height * 4;

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
    toSrc.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image = images_[imageIndex];
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &toSrc;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {extent_.width, extent_.height, 1};
    vkCmdCopyImageToBuffer(cmd, images_[imageIndex],
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, host.handle, 1,
                           &copy);

    VkImageMemoryBarrier2 back = toSrc;
    back.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    back.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    back.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
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
    const int rowBytes = static_cast<int>(extent_.width) * 3;
    const int padding = (4 - (rowBytes % 4)) % 4;
    const int imageBytes = (rowBytes + padding) * static_cast<int>(extent_.height);
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
    const int w = static_cast<int>(extent_.width);
    const int h = static_cast<int>(extent_.height);
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
