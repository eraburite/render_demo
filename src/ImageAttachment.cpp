#include "ImageAttachment.h"

#include <algorithm>
#include <utility>

vk::ImageCreateInfo ImageAttachment::multiSampleCreateInfo(vk::Extent2D extent, vk::Format format,
                                                           vk::SampleCountFlagBits samples,
                                                           vk::ImageUsageFlags usage, uint32_t layers)
{
    vk::ImageCreateInfo createInfo{};
    createInfo.imageType = vk::ImageType::e2D;
    // Historical bug: height was assigned from extent.width.
    createInfo.extent = vk::Extent3D(extent.width, extent.height, 1);
    createInfo.mipLevels = 1;
    createInfo.arrayLayers = std::max(1u, layers);
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
                                 vk::ImageAspectFlags aspect, uint32_t layers) :
    m_device(std::move(device)), m_format(format), m_layers(std::max(1u, layers))
{
    DeviceMemoryProperty memoryProp{memProps, vk::MemoryPropertyFlagBits::eDeviceLocal};
    ImageBundle bundle = DeviceResourceUtils::createImage(
        m_device, multiSampleCreateInfo(extent, format, samples, usage, m_layers), memoryProp, allocator);
    m_image = bundle.image;
    m_memory = bundle.memory;
    // One view over every layer: correct both as a layered framebuffer
    // attachment and as the source of a sampler2DArray.  createImageView()
    // defaults layerCount to 1, which silently produces a view of layer 0 only:
    // gl_Layer writes are then undefined and every array index in a shader
    // resolves to their clamped layer 0 -- no validation error, just a shadow
    // map that always samples the wrong cascade.
    m_view = DeviceResourceUtils::createImageView(m_device, *m_image, format, aspect, 1,
                                                  m_layers > 1 ? vk::ImageViewType::e2DArray
                                                               : vk::ImageViewType::e2D,
                                                  0, m_layers > 1 ? m_layers : 1);
}
