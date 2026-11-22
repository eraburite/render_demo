#pragma once
#include <memory>
#include <optional>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "DeviceMemoryAllocator.h"
#include "typedefs.h"

struct DeviceMemoryProperty {
    vk::PhysicalDeviceMemoryProperties properties;
    vk::MemoryPropertyFlags flags;
    uint32_t typeIndex = 0; // filled by findMemoryType
};

struct BufferBundle {
    std::shared_ptr<vk::Buffer> buffer;
    DeviceMemorySlice memory;
};

struct ImageBundle {
    std::shared_ptr<vk::Image> image;
    DeviceMemorySlice memory;
};

class DeviceResourceUtils {
public:
    static uint32_t findMemoryType(const vk::PhysicalDeviceMemoryProperties &props, uint32_t typeFilter,
                                   vk::MemoryPropertyFlags properties);

    static BufferBundle createBuffer(const std::shared_ptr<vk::Device> &device, vk::DeviceSize size,
                                     vk::BufferUsageFlags usage, const DeviceMemoryProperty &memProp,
                                     const std::shared_ptr<DeviceMemoryAllocator> &allocator = nullptr,
                                     const std::vector<uint32_t> *sharedQueueFamilies = nullptr);

    static ImageBundle createImage(const std::shared_ptr<vk::Device> &device, vk::ImageCreateInfo imageInfo,
                                   const DeviceMemoryProperty &memProp,
                                   const std::shared_ptr<DeviceMemoryAllocator> &allocator = nullptr);

    static std::shared_ptr<vk::ImageView> createImageView(const std::shared_ptr<vk::Device> &device, vk::Image image,
                                                          vk::Format format, vk::ImageAspectFlags aspect,
                                                          uint32_t mipLevels = 1,
                                                          vk::ImageViewType viewType = vk::ImageViewType::e2D);

    static void transitionImageLayout(const vk::CommandBuffer &cmd, vk::Image image, vk::Format format,
                                      vk::ImageLayout oldLayout, vk::ImageLayout newLayout, uint32_t mipLevels,
                                      uint32_t layerCount = 1);

    static void copyBufferToImage(const vk::CommandBuffer &cmd, vk::Buffer buffer, vk::Image image, uint32_t width,
                                  uint32_t height, uint32_t layerCount = 1);

    static void generateMipmaps(const vk::CommandBuffer &cmd, vk::Image image, vk::Format format, int32_t width,
                                int32_t height, uint32_t mipLevels, uint32_t layerCount = 1);

    static bool hasStencilComponent(vk::Format format);
};
