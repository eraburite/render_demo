#pragma once
#include <cstdint>
#include <memory>
#include <vulkan/vulkan.hpp>
#include "typedefs.h"

struct DeviceMemoryProperty {
    vk::PhysicalDeviceMemoryProperties &physicalMemProperties;
    vk::MemoryPropertyFlags memPropertyFlags;
};

class DeviceResourceUtils {
public:
    static std::shared_ptr<vk::ImageView> createImageView(std::shared_ptr<vk::Device> device, const vk::Image &image,
                                                          vk::Format format, uint32_t miplevels,
                                                          vk::ImageAspectFlags aspectMask);

    static std::tuple<std::shared_ptr<vk::Image>, std::shared_ptr<vk::DeviceMemory>>
    createImage(std::shared_ptr<vk::Device> device, uint32_t width, uint32_t height, vk::Format format,
                uint32_t miplevels, vk::SampleCountFlagBits msaaSamples, vk::ImageTiling tiling,
                vk::ImageUsageFlags usage, const DeviceMemoryProperty &memProperties);

    static std::tuple<std::shared_ptr<vk::Buffer>, std::shared_ptr<vk::DeviceMemory>>
    createBuffer(std::shared_ptr<vk::Device> device, vk::DeviceSize bufferSize, vk::BufferUsageFlags usage,
                 const DeviceMemoryProperty &memProperties);

    static uint32_t findMemoryType(uint32_t typeFilter, const DeviceMemoryProperty &memProperties);
};