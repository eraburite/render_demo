#include "RayTracingCommon.h"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace rt {
namespace {

bool HasExtension(const std::vector<vk::ExtensionProperties> &extensions, const char *name)
{
    return std::any_of(extensions.begin(), extensions.end(), [name](const vk::ExtensionProperties &entry) {
        return std::strcmp(entry.extensionName, name) == 0;
    });
}

} // namespace

Capabilities Capabilities::Query(vk::PhysicalDevice device)
{
    Capabilities caps;

    const std::vector<vk::ExtensionProperties> extensions = device.enumerateDeviceExtensionProperties();
    caps.accelerationStructure = HasExtension(extensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    caps.rayTracingPipeline = HasExtension(extensions, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    caps.rayQuery = HasExtension(extensions, VK_KHR_RAY_QUERY_EXTENSION_NAME);
    caps.bufferDeviceAddress = HasExtension(extensions, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    caps.deferredHostOperations = HasExtension(extensions, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);

    // A device may advertise the extension and still report the feature as
    // false; the feature is what actually gates the API, so both must agree.
    vk::PhysicalDeviceBufferDeviceAddressFeatures bufferDeviceAddress{};
    vk::PhysicalDeviceRayQueryFeaturesKHR rayQuery{};
    vk::PhysicalDeviceRayTracingPipelineFeaturesKHR rayTracingPipeline{};
    vk::PhysicalDeviceAccelerationStructureFeaturesKHR accelerationStructure{};
    bufferDeviceAddress.pNext = nullptr;
    rayQuery.pNext = &bufferDeviceAddress;
    rayTracingPipeline.pNext = &rayQuery;
    accelerationStructure.pNext = &rayTracingPipeline;
    vk::PhysicalDeviceFeatures2 features2{};
    features2.pNext = &accelerationStructure;
    device.getFeatures2(&features2);

    caps.accelerationStructure = caps.accelerationStructure && accelerationStructure.accelerationStructure;
    caps.rayTracingPipeline = caps.rayTracingPipeline && rayTracingPipeline.rayTracingPipeline;
    caps.rayQuery = caps.rayQuery && rayQuery.rayQuery;
    caps.bufferDeviceAddress = caps.bufferDeviceAddress && bufferDeviceAddress.bufferDeviceAddress;

    caps.pipelineProperties.sType = vk::StructureType::ePhysicalDeviceRayTracingPipelinePropertiesKHR;
    caps.pipelineProperties.pNext = nullptr;
    vk::PhysicalDeviceProperties2 properties2{};
    properties2.pNext = &caps.pipelineProperties;
    device.getProperties2(&properties2);

    return caps;
}

std::string Capabilities::Describe() const
{
    std::ostringstream out;
    out << "accelerationStructure=" << (accelerationStructure ? "yes" : "no")
        << " rayTracingPipeline=" << (rayTracingPipeline ? "yes" : "no")
        << " rayQuery=" << (rayQuery ? "yes" : "no")
        << " bufferDeviceAddress=" << (bufferDeviceAddress ? "yes" : "no");
    if (rayTracingPipeline) {
        out << " handleSize=" << shaderGroupHandleSize() << " handleAlign=" << shaderGroupHandleAlignment()
            << " baseAlign=" << shaderGroupBaseAlignment() << " maxRecursion=" << maxRayRecursionDepth();
    }
    return out.str();
}

void Dispatch::Load(vk::Device device)
{
    const auto get = [device](const char *name) { return device.getProcAddr(name); };
    getBufferDeviceAddress =
        reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>(get("vkGetBufferDeviceAddressKHR"));
    createAccelerationStructure =
        reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(get("vkCreateAccelerationStructureKHR"));
    destroyAccelerationStructure =
        reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(get("vkDestroyAccelerationStructureKHR"));
    getAccelerationStructureBuildSizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        get("vkGetAccelerationStructureBuildSizesKHR"));
    getAccelerationStructureDeviceAddress = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
        get("vkGetAccelerationStructureDeviceAddressKHR"));
    buildAccelerationStructures =
        reinterpret_cast<PFN_vkBuildAccelerationStructuresKHR>(get("vkBuildAccelerationStructuresKHR"));
    cmdBuildAccelerationStructures =
        reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(get("vkCmdBuildAccelerationStructuresKHR"));
    cmdTraceRays = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(get("vkCmdTraceRaysKHR"));
    getRayTracingShaderGroupHandles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
        get("vkGetRayTracingShaderGroupHandlesKHR"));
    createRayTracingPipelines =
        reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(get("vkCreateRayTracingPipelinesKHR"));
}

bool Dispatch::valid() const
{
    return accelerationStructureReady() && rayTracingPipelineReady();
}

bool Dispatch::accelerationStructureReady() const
{
    return getBufferDeviceAddress && createAccelerationStructure && destroyAccelerationStructure
           && getAccelerationStructureBuildSizes && getAccelerationStructureDeviceAddress
           && cmdBuildAccelerationStructures;
}

bool Dispatch::rayTracingPipelineReady() const
{
    return cmdTraceRays && getRayTracingShaderGroupHandles && createRayTracingPipelines;
}

std::string Dispatch::missing() const
{
    std::string names;
    const auto add = [&names](const char *name, auto pointer) {
        if (!pointer) {
            if (!names.empty()) {
                names += ", ";
            }
            names += name;
        }
    };
    add("vkGetBufferDeviceAddressKHR", getBufferDeviceAddress);
    add("vkCreateAccelerationStructureKHR", createAccelerationStructure);
    add("vkDestroyAccelerationStructureKHR", destroyAccelerationStructure);
    add("vkGetAccelerationStructureBuildSizesKHR", getAccelerationStructureBuildSizes);
    add("vkGetAccelerationStructureDeviceAddressKHR", getAccelerationStructureDeviceAddress);
    add("vkCmdBuildAccelerationStructuresKHR", cmdBuildAccelerationStructures);
    add("vkCmdTraceRaysKHR", cmdTraceRays);
    add("vkGetRayTracingShaderGroupHandlesKHR", getRayTracingShaderGroupHandles);
    add("vkCreateRayTracingPipelinesKHR", createRayTracingPipelines);
    return names;
}

uint64_t Dispatch::BufferDeviceAddress(vk::Device device, vk::Buffer buffer) const
{
    if (!getBufferDeviceAddress) {
        throw std::runtime_error("rt: vkGetBufferDeviceAddressKHR is not available");
    }
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.pNext = nullptr;
    info.buffer = static_cast<VkBuffer>(buffer);
    return getBufferDeviceAddress(static_cast<VkDevice>(device), &info);
}

void RunOnce(vk::Device device, vk::Queue queue, uint32_t queueFamilyIndex,
             const std::function<void(vk::CommandBuffer)> &body)
{
    vk::CommandPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::CommandPoolCreateFlagBits::eTransient;
    poolInfo.queueFamilyIndex = queueFamilyIndex;
    const vk::CommandPool pool = device.createCommandPool(poolInfo);

    vk::CommandBufferAllocateInfo allocateInfo{};
    allocateInfo.commandPool = pool;
    allocateInfo.level = vk::CommandBufferLevel::ePrimary;
    allocateInfo.commandBufferCount = 1;
    const vk::CommandBuffer cmd = device.allocateCommandBuffers(allocateInfo).front();

    cmd.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    body(cmd);
    cmd.end();

    vk::SubmitInfo submitInfo{};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    const vk::Result submitResult = queue.submit(1, &submitInfo, vk::Fence{});
    if (submitResult != vk::Result::eSuccess) {
        device.destroyCommandPool(pool);
        throw std::runtime_error("rt: one-time command submission failed");
    }
    queue.waitIdle();

    device.destroyCommandPool(pool);
}

void RecordCopyToSwapchain(vk::CommandBuffer cmd, vk::Image storageImage, vk::Image swapchainImage,
                           vk::Extent2D extent)
{
    const vk::ImageSubresourceRange range{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};

    // The traced image was written in the ray-tracing stage; make that visible
    // to the transfer read.
    vk::ImageMemoryBarrier toTransferSource{};
    toTransferSource.oldLayout = vk::ImageLayout::eGeneral;
    toTransferSource.newLayout = vk::ImageLayout::eTransferSrcOptimal;
    toTransferSource.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferSource.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferSource.image = storageImage;
    toTransferSource.subresourceRange = range;
    toTransferSource.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
    toTransferSource.dstAccessMask = vk::AccessFlagBits::eTransferRead;
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eRayTracingShaderKHR, vk::PipelineStageFlagBits::eTransfer, {},
                        {}, {}, toTransferSource);

    // The acquired swapchain image has undefined contents (we overwrite all of
    // it), so UNDEFINED is the correct old layout and needs no prior wait.
    vk::ImageMemoryBarrier toTransferDestination{};
    toTransferDestination.oldLayout = vk::ImageLayout::eUndefined;
    toTransferDestination.newLayout = vk::ImageLayout::eTransferDstOptimal;
    toTransferDestination.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferDestination.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferDestination.image = swapchainImage;
    toTransferDestination.subresourceRange = range;
    toTransferDestination.srcAccessMask = {};
    toTransferDestination.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer, {}, {}, {},
                        toTransferDestination);

    vk::ImageCopy copy{};
    copy.srcSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    copy.srcOffset = vk::Offset3D{0, 0, 0};
    copy.dstSubresource = vk::ImageSubresourceLayers{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
    copy.dstOffset = vk::Offset3D{0, 0, 0};
    copy.extent = vk::Extent3D{extent.width, extent.height, 1};
    cmd.copyImage(storageImage, vk::ImageLayout::eTransferSrcOptimal, swapchainImage,
                  vk::ImageLayout::eTransferDstOptimal, copy);

    vk::ImageMemoryBarrier toPresent{};
    toPresent.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    toPresent.newLayout = vk::ImageLayout::ePresentSrcKHR;
    toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.image = swapchainImage;
    toPresent.subresourceRange = range;
    toPresent.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    toPresent.dstAccessMask = {};
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {},
                        toPresent);

    // Back to GENERAL so the next frame's imageStore finds the layout it left.
    vk::ImageMemoryBarrier backToGeneral{};
    backToGeneral.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
    backToGeneral.newLayout = vk::ImageLayout::eGeneral;
    backToGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    backToGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    backToGeneral.image = storageImage;
    backToGeneral.subresourceRange = range;
    backToGeneral.srcAccessMask = vk::AccessFlagBits::eTransferRead;
    backToGeneral.dstAccessMask = vk::AccessFlagBits::eShaderWrite;
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eRayTracingShaderKHR, {},
                        {}, {}, backToGeneral);
}

void StorageImage::Create(vk::Device device, const vk::PhysicalDeviceMemoryProperties &memProps, vk::Format format,
                          vk::Extent2D extent)
{
    m_format = format;
    m_extent = extent;

    vk::ImageCreateInfo imageInfo{};
    imageInfo.imageType = vk::ImageType::e2D;
    imageInfo.format = format;
    imageInfo.extent = vk::Extent3D{extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = vk::SampleCountFlagBits::e1;
    imageInfo.tiling = vk::ImageTiling::eOptimal;
    imageInfo.usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferSrc;
    imageInfo.sharingMode = vk::SharingMode::eExclusive;
    imageInfo.initialLayout = vk::ImageLayout::eUndefined;
    m_image = device.createImage(imageInfo);

    const vk::MemoryRequirements requirements = device.getImageMemoryRequirements(m_image);
    vk::MemoryAllocateInfo allocateInfo{};
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex =
        FindMemoryType(memProps, requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    m_memory = device.allocateMemory(allocateInfo);
    device.bindImageMemory(m_image, m_memory, 0);

    vk::ImageViewCreateInfo viewInfo{};
    viewInfo.image = m_image;
    viewInfo.viewType = vk::ImageViewType::e2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    m_view = device.createImageView(viewInfo);
}

void StorageImage::TransitionToGeneral(vk::Device device, vk::Queue queue, uint32_t queueFamilyIndex)
{
    RunOnce(device, queue, queueFamilyIndex, [this](vk::CommandBuffer cmd) {
        vk::ImageMemoryBarrier barrier{};
        barrier.oldLayout = vk::ImageLayout::eUndefined;
        barrier.newLayout = vk::ImageLayout::eGeneral;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = m_image;
        barrier.subresourceRange = vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        barrier.srcAccessMask = {};
        barrier.dstAccessMask = vk::AccessFlagBits::eShaderWrite;
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eRayTracingShaderKHR,
                            {}, {}, {}, barrier);
    });
}

void StorageImage::Destroy(vk::Device device)
{
    if (m_view) {
        device.destroyImageView(m_view);
        m_view = nullptr;
    }
    if (m_image) {
        device.destroyImage(m_image);
        m_image = nullptr;
    }
    if (m_memory) {
        device.freeMemory(m_memory);
        m_memory = nullptr;
    }
}

} // namespace rt
