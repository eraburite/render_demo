#include "ImageReadback.h"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void check(vk::Result result, const char *what)
{
    if (result != vk::Result::eSuccess) {
        throw std::runtime_error(std::string(what) + " failed: " + vk::to_string(result));
    }
}

bool isDepthFormat(vk::Format format)
{
    switch (format) {
        case vk::Format::eD16Unorm:
        case vk::Format::eD32Sfloat:
        case vk::Format::eD16UnormS8Uint:
        case vk::Format::eD24UnormS8Uint:
        case vk::Format::eD32SfloatS8Uint:
        case vk::Format::eX8D24UnormPack32: return true;
        default: return false;
    }
}

vk::ImageAspectFlags aspectOf(vk::Format format)
{
    if (!isDepthFormat(format)) {
        return vk::ImageAspectFlagBits::eColor;
    }
    return DeviceResourceUtils::hasStencilComponent(format)
               ? (vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil)
               : vk::ImageAspectFlagBits::eDepth;
}

// Transition into TRANSFER_SRC.  A capture runs after the frame that produced
// the image has been submitted, so the barrier uses eAllCommands as the source
// scope: correctness first, and the stall is irrelevant for a manual capture.
// When the image is currently UNDEFINED its contents are undefined by
// definition, so no write scope may be declared (VUID-vkImageMemoryBarrier-oldLayout).
vk::ImageMemoryBarrier makeTransferSrcBarrier(vk::Image image, vk::Format format, vk::ImageLayout oldLayout)
{
    const bool undefinedSource = oldLayout == vk::ImageLayout::eUndefined;

    vk::ImageMemoryBarrier barrier{};
    barrier.oldLayout = oldLayout;
    barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspectOf(format);
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = undefinedSource ? vk::AccessFlags{} : vk::AccessFlagBits::eMemoryWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
    return barrier;
}

vk::ImageMemoryBarrier makeRestoreBarrier(vk::Image image, vk::Format format, vk::ImageLayout newLayout)
{
    vk::ImageMemoryBarrier barrier{};
    barrier.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspectOf(format);
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferRead;
    barrier.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
    return barrier;
}

} // namespace

uint32_t ImageReadback::texelSize(vk::Format format)
{
    switch (format) {
        case vk::Format::eR8G8B8A8Unorm:
        case vk::Format::eR8G8B8A8Srgb:
        case vk::Format::eB8G8R8A8Unorm:
        case vk::Format::eB8G8R8A8Srgb:
        case vk::Format::eR32Sfloat:
        case vk::Format::eD32Sfloat: return 4;
        case vk::Format::eR32G32B32A32Sfloat: return 16;
        default: break;
    }
    throw std::runtime_error("ImageReadback: unsupported format " + vk::to_string(format));
}

ImageReadback::ImageReadback(std::shared_ptr<vk::Device> device, const vk::PhysicalDeviceMemoryProperties &memProps,
                             std::shared_ptr<DeviceMemoryAllocator> allocator, std::shared_ptr<vk::Queue> queue,
                             uint32_t queueFamily) :
    m_device(std::move(device)), m_memProps(memProps), m_allocator(std::move(allocator)), m_queue(std::move(queue)),
    m_queueFamily(queueFamily)
{
    if (!m_device || !m_queue) {
        throw std::runtime_error("ImageReadback: device and queue are required");
    }

    vk::CommandPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    poolInfo.queueFamilyIndex = m_queueFamily;
    vk::CommandPool pool{};
    check(m_device->createCommandPool(&poolInfo, nullptr, &pool), "vkCreateCommandPool (readback)");
    m_pool = std::shared_ptr<vk::CommandPool>(new vk::CommandPool(pool), [device = m_device](vk::CommandPool *p) {
        if (p != nullptr && *p) {
            device->destroyCommandPool(*p, nullptr);
        }
        delete p;
    });

    vk::CommandBufferAllocateInfo allocInfo{};
    allocInfo.commandPool = *m_pool;
    allocInfo.level = vk::CommandBufferLevel::ePrimary;
    allocInfo.commandBufferCount = 1;
    vk::CommandBuffer cmd{};
    check(m_device->allocateCommandBuffers(&allocInfo, &cmd), "vkAllocateCommandBuffers (readback)");
    m_commandBuffer = std::shared_ptr<vk::CommandBuffer>(
        new vk::CommandBuffer(cmd), [device = m_device, pool = m_pool](vk::CommandBuffer *p) {
            if (p != nullptr && *p) {
                device->freeCommandBuffers(*pool, 1, p);
            }
            delete p;
        });

    vk::FenceCreateInfo fenceInfo{};
    vk::Fence fence{};
    check(m_device->createFence(&fenceInfo, nullptr, &fence), "vkCreateFence (readback)");
    m_fence = std::shared_ptr<vk::Fence>(new vk::Fence(fence), [device = m_device](vk::Fence *p) {
        if (p != nullptr && *p) {
            device->destroyFence(*p, nullptr);
        }
        delete p;
    });
}

ImageReadback::~ImageReadback()
{
    // The staging buffer must die before the allocator does; releasing our
    // references here (rather than relying on member destruction order) keeps
    // that ordering explicit.
    m_staging = BufferBundle{};
    m_stagingSize = 0;
    m_fence.reset();
    m_commandBuffer.reset();
    m_pool.reset();
}

void ImageReadback::ensureStaging(vk::DeviceSize size)
{
    if (m_staging.buffer && m_stagingSize >= size) {
        return;
    }

    // Release the old buffer first: a capture is synchronous, so nothing is
    // reading it at this point.
    m_staging = BufferBundle{};
    m_stagingSize = 0;

    DeviceMemoryProperty memProp{};
    memProp.properties = m_memProps;
    memProp.flags = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    m_staging = DeviceResourceUtils::createBuffer(m_device, size, vk::BufferUsageFlagBits::eTransferDst, memProp,
                                                  m_allocator);
    m_stagingSize = size;
}

ImageReadback::Capture ImageReadback::capture(vk::Image image, vk::Format format, vk::Extent2D extent,
                                              vk::ImageLayout layout)
{
    if (!image) {
        throw std::runtime_error("ImageReadback::capture called with a null image");
    }
    if (extent.width == 0 || extent.height == 0) {
        throw std::runtime_error("ImageReadback::capture called with an empty extent");
    }

    Capture result;
    result.extent = extent;
    result.format = format;

    const vk::DeviceSize size = static_cast<vk::DeviceSize>(extent.width) * extent.height * texelSize(format);
    ensureStaging(size);
    result.pixels.resize(static_cast<size_t>(size));

    // ---- record: TRANSFER_SRC -> copy -> original layout ------------------
    // resetCommandPool() is the exception-throwing enhanced overload.
    m_device->resetCommandPool(*m_pool, vk::CommandPoolResetFlags{});
    const vk::CommandBuffer &cmd = *m_commandBuffer;
    cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

    const vk::ImageMemoryBarrier toTransfer = makeTransferSrcBarrier(image, format, layout);
    const vk::PipelineStageFlags sourceStage = (layout == vk::ImageLayout::eUndefined)
                                                   ? vk::PipelineStageFlagBits::eTopOfPipe
                                                   : vk::PipelineStageFlagBits::eAllCommands;
    cmd.pipelineBarrier(sourceStage, vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlags{}, 0, nullptr, 0,
                        nullptr, 1, &toTransfer);

    vk::BufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;   // tightly packed rows
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = aspectOf(format);
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = vk::Offset3D(0, 0, 0);
    region.imageExtent = vk::Extent3D(extent.width, extent.height, 1);
    cmd.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, *m_staging.buffer, 1, &region);

    // Restoring UNDEFINED makes no sense: the caller declared the contents
    // undefined, and the next render pass discards them anyway.
    if (layout != vk::ImageLayout::eUndefined) {
        const vk::ImageMemoryBarrier restore = makeRestoreBarrier(image, format, layout);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
                            vk::DependencyFlags{}, 0, nullptr, 0, nullptr, 1, &restore);
    }
    cmd.end();

    // ---- submit and block -------------------------------------------------
    vk::SubmitInfo submitInfo{};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    check(m_device->resetFences(1, &*m_fence), "vkResetFences (readback)");
    check(m_queue->submit(1, &submitInfo, *m_fence), "vkQueueSubmit (readback)");
    check(m_device->waitForFences(1, &*m_fence, VK_TRUE, std::numeric_limits<uint64_t>::max()),
          "vkWaitForFences (readback)");

    // ---- map and copy out -------------------------------------------------
    void *mapped = m_device->mapMemory(*m_staging.memory.memory, m_staging.memory.offset, size);
    std::memcpy(result.pixels.data(), mapped, static_cast<size_t>(size));
    m_device->unmapMemory(*m_staging.memory.memory);
    return result;
}
