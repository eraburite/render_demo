#include "DeviceResourceUtils.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void checkResult(vk::Result result, const char *what)
{
    if (result != vk::Result::eSuccess) {
        throw std::runtime_error(std::string(what) + " (Vulkan error: " + vk::to_string(result) + ")");
    }
}

constexpr vk::DeviceSize kSmallAllocationThreshold = 4 * 1024 * 1024;

vk::ImageAspectFlags aspectMaskFor(vk::Format format)
{
    switch (format) {
        case vk::Format::eS8Uint: return vk::ImageAspectFlagBits::eStencil;
        case vk::Format::eD16UnormS8Uint:
        case vk::Format::eD24UnormS8Uint:
        case vk::Format::eD32SfloatS8Uint:
            return vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
        case vk::Format::eD16Unorm:
        case vk::Format::eD32Sfloat:
        case vk::Format::eX8D24UnormPack32: return vk::ImageAspectFlagBits::eDepth;
        default: return vk::ImageAspectFlagBits::eColor;
    }
}

} // namespace

uint32_t DeviceResourceUtils::findMemoryType(const vk::PhysicalDeviceMemoryProperties &props, uint32_t typeFilter,
                                             vk::MemoryPropertyFlags properties)
{
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) != 0 && (props.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("failed to find suitable memory type");
}

BufferBundle DeviceResourceUtils::createBuffer(const std::shared_ptr<vk::Device> &device, vk::DeviceSize size,
                                               vk::BufferUsageFlags usage, const DeviceMemoryProperty &memProp,
                                               const std::shared_ptr<DeviceMemoryAllocator> &allocator,
                                               const std::vector<uint32_t> *sharedQueueFamilies)
{
    if (size == 0) {
        throw std::runtime_error("attempted to create a zero-sized buffer");
    }

    std::vector<uint32_t> families;
    if (sharedQueueFamilies != nullptr) {
        for (uint32_t family : *sharedQueueFamilies) {
            if (std::find(families.begin(), families.end(), family) == families.end()) {
                families.push_back(family);
            }
        }
    }
    const bool concurrent = families.size() >= 2;
    if (concurrent && families.size() > 2) {
        families.resize(2); // only two distinct families are shared
    }

    vk::BufferCreateInfo bufferInfo{};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = concurrent ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive;
    if (concurrent) {
        bufferInfo.queueFamilyIndexCount = static_cast<uint32_t>(families.size());
        bufferInfo.pQueueFamilyIndices = families.data();
    }

    vk::Buffer buffer{};
    checkResult(device->createBuffer(&bufferInfo, nullptr, &buffer), "failed to create buffer");

    vk::MemoryRequirements memRequirements{};
    device->getBufferMemoryRequirements(buffer, &memRequirements);
    const uint32_t typeIndex = findMemoryType(memProp.properties, memRequirements.memoryTypeBits, memProp.flags);

    const bool pooled = allocator != nullptr && memRequirements.size <= kSmallAllocationThreshold;
    DeviceMemorySlice slice;
    if (pooled) {
        slice = allocator->allocate(memRequirements.size, typeIndex, memProp.flags);
    } else {
        vk::MemoryAllocateInfo allocInfo(memRequirements.size, typeIndex);
        vk::DeviceMemory memory{};
        checkResult(device->allocateMemory(&allocInfo, nullptr, &memory), "failed to allocate buffer memory");
        slice.memory = std::shared_ptr<vk::DeviceMemory>(new vk::DeviceMemory(memory),
                                                         [device](vk::DeviceMemory *p) {
                                                             if (p && *p) {
                                                                 device->freeMemory(*p, nullptr);
                                                             }
                                                             delete p;
                                                         });
        slice.offset = 0;
        slice.size = memRequirements.size;
    }

    // Binding must use the slice offset (a pooled slice may start at non-zero).
    vk::BindBufferMemoryInfo bindInfo(buffer, *slice.memory, slice.offset);
    checkResult(device->bindBufferMemory2(1, &bindInfo), "failed to bind buffer memory");

    std::shared_ptr<DeviceMemoryAllocator> capturedAllocator = pooled ? allocator : nullptr;
    auto ownedBuffer = std::shared_ptr<vk::Buffer>(new vk::Buffer(buffer),
                                                   [device, slice, capturedAllocator](vk::Buffer *p) {
                                                       if (p && *p) {
                                                           device->destroyBuffer(*p, nullptr);
                                                       }
                                                       // Destroy the buffer before releasing its memory.
                                                       if (capturedAllocator && slice.valid()) {
                                                           capturedAllocator->free(slice);
                                                       }
                                                       delete p;
                                                   });
    return BufferBundle{ownedBuffer, slice};
}

ImageBundle DeviceResourceUtils::createImage(const std::shared_ptr<vk::Device> &device, vk::ImageCreateInfo imageInfo,
                                             const DeviceMemoryProperty &memProp,
                                             const std::shared_ptr<DeviceMemoryAllocator> &allocator)
{
    vk::Image image{};
    checkResult(device->createImage(&imageInfo, nullptr, &image), "failed to create image");

    vk::MemoryRequirements memRequirements{};
    device->getImageMemoryRequirements(image, &memRequirements);
    const uint32_t typeIndex = findMemoryType(memProp.properties, memRequirements.memoryTypeBits, memProp.flags);

    const bool pooled = allocator != nullptr && memRequirements.size <= kSmallAllocationThreshold;
    DeviceMemorySlice slice;
    if (pooled) {
        slice = allocator->allocate(memRequirements.size, typeIndex, memProp.flags);
    } else {
        vk::MemoryAllocateInfo allocInfo(memRequirements.size, typeIndex);
        vk::DeviceMemory memory{};
        checkResult(device->allocateMemory(&allocInfo, nullptr, &memory), "failed to allocate image memory");
        slice.memory = std::shared_ptr<vk::DeviceMemory>(new vk::DeviceMemory(memory),
                                                         [device](vk::DeviceMemory *p) {
                                                             if (p && *p) {
                                                                 device->freeMemory(*p, nullptr);
                                                             }
                                                             delete p;
                                                         });
        slice.offset = 0;
        slice.size = memRequirements.size;
    }

    vk::BindImageMemoryInfo bindInfo(image, *slice.memory, slice.offset);
    checkResult(device->bindImageMemory2(1, &bindInfo), "failed to bind image memory");

    std::shared_ptr<DeviceMemoryAllocator> capturedAllocator = pooled ? allocator : nullptr;
    auto ownedImage = std::shared_ptr<vk::Image>(new vk::Image(image),
                                                 [device, slice, capturedAllocator](vk::Image *p) {
                                                     if (p && *p) {
                                                         device->destroyImage(*p, nullptr);
                                                     }
                                                     if (capturedAllocator && slice.valid()) {
                                                         capturedAllocator->free(slice);
                                                     }
                                                     delete p;
                                                 });
    return ImageBundle{ownedImage, slice};
}

std::shared_ptr<vk::ImageView> DeviceResourceUtils::createImageView(const std::shared_ptr<vk::Device> &device,
                                                                    vk::Image image, vk::Format format,
                                                                    vk::ImageAspectFlags aspect, uint32_t mipLevels,
                                                                    vk::ImageViewType viewType)
{
    vk::ImageViewCreateInfo createInfo{};
    createInfo.image = image;
    createInfo.viewType = viewType;
    createInfo.format = format;
    createInfo.components.r = vk::ComponentSwizzle::eIdentity;
    createInfo.components.g = vk::ComponentSwizzle::eIdentity;
    createInfo.components.b = vk::ComponentSwizzle::eIdentity;
    createInfo.components.a = vk::ComponentSwizzle::eIdentity;
    createInfo.subresourceRange.aspectMask = aspect;
    createInfo.subresourceRange.baseMipLevel = 0;
    createInfo.subresourceRange.levelCount = mipLevels;
    createInfo.subresourceRange.baseArrayLayer = 0;
    createInfo.subresourceRange.layerCount = 1;

    vk::ImageView view{};
    checkResult(device->createImageView(&createInfo, nullptr, &view), "failed to create image view");
    return std::shared_ptr<vk::ImageView>(new vk::ImageView(view), [device](vk::ImageView *p) {
        if (p && *p) {
            device->destroyImageView(*p, nullptr);
        }
        delete p;
    });
}

void DeviceResourceUtils::transitionImageLayout(const vk::CommandBuffer &cmd, vk::Image image, vk::Format format,
                                                vk::ImageLayout oldLayout, vk::ImageLayout newLayout,
                                                uint32_t mipLevels, uint32_t layerCount)
{
    if (oldLayout == newLayout) {
        return;
    }

    vk::ImageMemoryBarrier barrier{};
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspectMaskFor(format);
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = layerCount;

    vk::PipelineStageFlags sourceStage{};
    vk::PipelineStageFlags destinationStage{};

    if (oldLayout == vk::ImageLayout::eUndefined && newLayout == vk::ImageLayout::eTransferDstOptimal) {
        barrier.srcAccessMask = vk::AccessFlagBits::eNone;
        barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
        sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;
        destinationStage = vk::PipelineStageFlagBits::eTransfer;
    } else if (oldLayout == vk::ImageLayout::eTransferDstOptimal
               && newLayout == vk::ImageLayout::eShaderReadOnlyOptimal) {
        barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
        barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
        sourceStage = vk::PipelineStageFlagBits::eTransfer;
        destinationStage = vk::PipelineStageFlagBits::eFragmentShader;
    } else if (oldLayout == vk::ImageLayout::eShaderReadOnlyOptimal
               && newLayout == vk::ImageLayout::eTransferSrcOptimal) {
        barrier.srcAccessMask = vk::AccessFlagBits::eShaderRead;
        barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
        sourceStage = vk::PipelineStageFlagBits::eFragmentShader;
        destinationStage = vk::PipelineStageFlagBits::eTransfer;
    } else if (oldLayout == vk::ImageLayout::eShaderReadOnlyOptimal
               && newLayout == vk::ImageLayout::eTransferDstOptimal) {
        barrier.srcAccessMask = vk::AccessFlagBits::eShaderRead;
        barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
        sourceStage = vk::PipelineStageFlagBits::eFragmentShader;
        destinationStage = vk::PipelineStageFlagBits::eTransfer;
    } else if (oldLayout == vk::ImageLayout::eTransferDstOptimal
               && newLayout == vk::ImageLayout::eTransferSrcOptimal) {
        barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
        barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
        sourceStage = vk::PipelineStageFlagBits::eTransfer;
        destinationStage = vk::PipelineStageFlagBits::eTransfer;
    } else if (oldLayout == vk::ImageLayout::eTransferSrcOptimal
               && newLayout == vk::ImageLayout::eShaderReadOnlyOptimal) {
        barrier.srcAccessMask = vk::AccessFlagBits::eTransferRead;
        barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
        sourceStage = vk::PipelineStageFlagBits::eTransfer;
        destinationStage = vk::PipelineStageFlagBits::eFragmentShader;
    } else if (oldLayout == vk::ImageLayout::eUndefined
               && newLayout == vk::ImageLayout::eColorAttachmentOptimal) {
        barrier.srcAccessMask = vk::AccessFlagBits::eNone;
        barrier.dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
        sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;
        destinationStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    } else if (oldLayout == vk::ImageLayout::eColorAttachmentOptimal
               && newLayout == vk::ImageLayout::eShaderReadOnlyOptimal) {
        barrier.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
        barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
        sourceStage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
        destinationStage = vk::PipelineStageFlagBits::eFragmentShader;
    } else if (oldLayout == vk::ImageLayout::eUndefined
               && newLayout == vk::ImageLayout::eDepthStencilAttachmentOptimal) {
        barrier.srcAccessMask = vk::AccessFlagBits::eNone;
        barrier.dstAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentWrite;
        sourceStage = vk::PipelineStageFlagBits::eTopOfPipe;
        destinationStage = vk::PipelineStageFlagBits::eEarlyFragmentTests | vk::PipelineStageFlagBits::eLateFragmentTests;
    } else {
        throw std::invalid_argument("unsupported image layout transition");
    }

    cmd.pipelineBarrier(sourceStage, destinationStage, vk::DependencyFlags(), 0, nullptr, 0, nullptr, 1, &barrier);
}

void DeviceResourceUtils::copyBufferToImage(const vk::CommandBuffer &cmd, vk::Buffer buffer, vk::Image image,
                                            uint32_t width, uint32_t height, uint32_t layerCount)
{
    vk::BufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = layerCount;
    region.imageOffset = vk::Offset3D(0, 0, 0);
    region.imageExtent = vk::Extent3D(width, height, 1);

    cmd.copyBufferToImage(buffer, image, vk::ImageLayout::eTransferDstOptimal, 1, &region);
}

void DeviceResourceUtils::generateMipmaps(const vk::CommandBuffer &cmd, vk::Image image, vk::Format format,
                                          int32_t width, int32_t height, uint32_t mipLevels, uint32_t layerCount)
{
    if (mipLevels <= 1) {
        return;
    }

    vk::ImageMemoryBarrier barrier{};
    barrier.image = image;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = aspectMaskFor(format);
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = layerCount;
    barrier.subresourceRange.levelCount = 1;

    int32_t mipWidth = width;
    int32_t mipHeight = height;
    for (uint32_t i = 1; i < mipLevels; ++i) {
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
        barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
        barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
        barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
                            vk::DependencyFlags(), 0, nullptr, 0, nullptr, 1, &barrier);

        vk::ImageBlit blit{};
        blit.srcOffsets[0] = vk::Offset3D(0, 0, 0);
        blit.srcOffsets[1] = vk::Offset3D(mipWidth, mipHeight, 1);
        blit.srcSubresource.aspectMask = aspectMaskFor(format);
        blit.srcSubresource.mipLevel = i - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = layerCount;
        blit.dstOffsets[0] = vk::Offset3D(0, 0, 0);
        blit.dstOffsets[1] = vk::Offset3D(mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1);
        blit.dstSubresource.aspectMask = aspectMaskFor(format);
        blit.dstSubresource.mipLevel = i;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = layerCount;
        cmd.blitImage(image, vk::ImageLayout::eTransferSrcOptimal, image, vk::ImageLayout::eTransferDstOptimal, 1,
                      &blit, vk::Filter::eLinear);

        barrier.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
        barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
        barrier.srcAccessMask = vk::AccessFlagBits::eTransferRead;
        barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader,
                            vk::DependencyFlags(), 0, nullptr, 0, nullptr, 1, &barrier);

        mipWidth = mipWidth > 1 ? mipWidth / 2 : 1;
        mipHeight = mipHeight > 1 ? mipHeight / 2 : 1;
    }

    // The last mip level was only written, never promoted above.
    barrier.subresourceRange.baseMipLevel = mipLevels - 1;
    barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
    barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
    barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
    cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader,
                        vk::DependencyFlags(), 0, nullptr, 0, nullptr, 1, &barrier);
}

bool DeviceResourceUtils::hasStencilComponent(vk::Format format)
{
    return format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint
           || format == vk::Format::eD16UnormS8Uint;
}
