#include "ImageResources.h"
#include "DeviceResourceUtils.h"

ImageResources::ImageResources(std::shared_ptr<vk::Device> device,
                               const vk::PhysicalDeviceMemoryProperties &memProperties,
                               std::shared_ptr<vk::Extent2D> swapChainExtent, vk::Format depthFormat,
                               vk::SampleCountFlagBits msaaSamples, vk::ImageUsageFlags imageUsageFlags,
                               vk::ImageAspectFlags aspectFlags) :
    m_device(device), m_swapChainExtent(swapChainExtent), m_imageFormat(depthFormat), m_msaaSamples(msaaSamples),
    m_imageUsageFlags(imageUsageFlags), m_aspectFlags(aspectFlags)
{
    createImageResources(memProperties);
}

ImageResources::~ImageResources() {}

void ImageResources::createImageResources(vk::PhysicalDeviceMemoryProperties memProperties)
{
    DeviceMemoryProperty memoryProp{memProperties, vk::MemoryPropertyFlagBits::eDeviceLocal};
    std::tie(m_image, m_imageMemory) =
        DeviceResourceUtils::createImage(m_device, m_swapChainExtent->width, m_swapChainExtent->height, m_imageFormat,
                                         1, m_msaaSamples, vk::ImageTiling::eOptimal, m_imageUsageFlags, memoryProp);
    m_imageView = DeviceResourceUtils::createImageView(m_device, *m_image, m_imageFormat, 1, m_aspectFlags);
}
