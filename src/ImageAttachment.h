#pragma once
#include <memory>
#include <vulkan/vulkan.hpp>
#include "DeviceMemoryAllocator.h"
#include "DeviceResourceUtils.h"

// Depth/MSAA/offscreen-color attachment: image + memory + image view.
class ImageAttachment {
public:
    ImageAttachment(std::shared_ptr<vk::Device> device, vk::PhysicalDeviceMemoryProperties memProps,
                    std::shared_ptr<DeviceMemoryAllocator> allocator, vk::Extent2D extent, vk::Format format,
                    vk::SampleCountFlagBits samples, vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect);
    ~ImageAttachment() = default;

    vk::ImageView imageView() const { return m_view ? *m_view : vk::ImageView{}; }
    vk::Image image() const { return m_image ? *m_image : vk::Image{}; }
    vk::Format format() const { return m_format; }

    // Correct height assignment (historical bug used extent.width).
    static vk::ImageCreateInfo multiSampleCreateInfo(vk::Extent2D extent, vk::Format format,
                                                     vk::SampleCountFlagBits samples, vk::ImageUsageFlags usage);

private:
    std::shared_ptr<vk::Device> m_device;
    vk::Format m_format;
    std::shared_ptr<vk::Image> m_image;
    DeviceMemorySlice m_memory;
    std::shared_ptr<vk::ImageView> m_view;
};
