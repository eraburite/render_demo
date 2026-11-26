#pragma once
#include <memory>
#include <vulkan/vulkan.hpp>
#include "DeviceMemoryAllocator.h"
#include "DeviceResourceUtils.h"

// Depth/MSAA/offscreen-color attachment: image + memory + image view.
//
// `layers` > 1 allocates a 2D array and creates ONE view covering every layer.
// That is the form layered rendering needs: the framebuffer has to reference an
// array view, and the geometry shader then picks the layer per primitive with
// gl_Layer.  The same view is what a sampler2DArray reads (see
// SampledAttachmentView), so one allocation serves both ends of a cascaded
// shadow map without a copy.
class ImageAttachment {
public:
    ImageAttachment(std::shared_ptr<vk::Device> device, vk::PhysicalDeviceMemoryProperties memProps,
                    std::shared_ptr<DeviceMemoryAllocator> allocator, vk::Extent2D extent, vk::Format format,
                    vk::SampleCountFlagBits samples, vk::ImageUsageFlags usage, vk::ImageAspectFlags aspect,
                    uint32_t layers = 1);
    ~ImageAttachment() = default;

    vk::ImageView imageView() const { return m_view ? *m_view : vk::ImageView{}; }
    vk::Image image() const { return m_image ? *m_image : vk::Image{}; }
    vk::Format format() const { return m_format; }
    uint32_t layers() const { return m_layers; }

    // Correct height assignment (historical bug used extent.width).
    static vk::ImageCreateInfo multiSampleCreateInfo(vk::Extent2D extent, vk::Format format,
                                                     vk::SampleCountFlagBits samples, vk::ImageUsageFlags usage,
                                                     uint32_t layers = 1);

private:
    std::shared_ptr<vk::Device> m_device;
    vk::Format m_format;
    uint32_t m_layers = 1;
    std::shared_ptr<vk::Image> m_image;
    DeviceMemorySlice m_memory;
    std::shared_ptr<vk::ImageView> m_view;
};
