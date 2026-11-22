#include "ImageAttachment.h"

#include <utility>

vk::ImageCreateInfo ImageAttachment::multiSampleCreateInfo(vk::Extent2D extent, vk::Format format,
                                                           vk::SampleCountFlagBits samples,
                                                           vk::ImageUsageFlags usage)
{
    vk::ImageCreateInfo createInfo{};
    createInfo.imageType = vk::ImageType::e2D;
    // Historical bug: height was assigned from extent.width.
    createInfo.extent = vk::Extent3D(extent.width, extent.height, 1);
    createInfo.mipLevels = 1;
    createInfo.arrayLayers = 1;
    createInfo.format = format;
    createInfo.tiling = vk::ImageTiling::eOptimal;
    createInfo.initialLayout = vk::ImageLayout::eUndefined;
    createInfo.usage = usage;
    createInfo.samples = samples;
    createInfo.sharingMode = vk::SharingMode::eExclusive;
    return createInfo;
}

ImageAttachment::ImageAttachment(std::shared_ptr<vk::Device> device, vk::PhysicalDeviceMemoryProperties memProps,
                                 std::shared_ptr<DeviceMemoryAllocator> allocator, vk::Extent2D extent,
                                 vk::Format format, vk::SampleCountFlagBits samples, vk::ImageUsageFlags usage,
                                 vk::ImageAspectFlags aspect) :
    m_device(std::move(device)), m_format(format)
{
    DeviceMemoryProperty memoryProp{memProps, vk::MemoryPropertyFlagBits::eDeviceLocal};
    ImageBundle bundle = DeviceResourceUtils::createImage(
        m_device, multiSampleCreateInfo(extent, format, samples, usage), memoryProp, allocator);
    m_image = bundle.image;
    m_memory = bundle.memory;
    m_view = DeviceResourceUtils::createImageView(m_device, *m_image, format, aspect, 1, vk::ImageViewType::e2D);
}
