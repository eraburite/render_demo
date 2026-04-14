#pragma once
#include <cstdint>
#include <memory>
#include <vulkan/vulkan.hpp>
#include "typedefs.h"

class ImageResources {
public:
    ImageResources(std::shared_ptr<vk::Device> device, const vk::PhysicalDeviceMemoryProperties &memProperties,
                   std::shared_ptr<vk::Extent2D> swapChainExtent, vk::Format imageFormat,
                   vk::SampleCountFlagBits msaaSamples, vk::ImageUsageFlags imageUsageFlags,
                   vk::ImageAspectFlags aspectFlags);
    ~ImageResources();

    std::shared_ptr<vk::ImageView> getImageView() const
    {
        return m_imageView;
    }

private:
    void createImageResources(vk::PhysicalDeviceMemoryProperties memProperties);

private:
    std::shared_ptr<vk::Device> m_device = nullptr;
    std::shared_ptr<vk::Image> m_image;
    std::shared_ptr<vk::DeviceMemory> m_imageMemory;
    std::shared_ptr<vk::ImageView> m_imageView;
    std::shared_ptr<vk::Extent2D> m_swapChainExtent;
    vk::Format m_imageFormat;
    vk::SampleCountFlagBits m_msaaSamples;
    vk::ImageUsageFlags m_imageUsageFlags;
    vk::ImageAspectFlags m_aspectFlags;
};