#pragma once
#include <memory>
#include <vulkan/vulkan.hpp>
#include "typedefs.h"

class PhysicalDeviceUtils {
public:
    static std::tuple<std::shared_ptr<vk::PhysicalDevice>, QueueFamilyIndices>
    select(std::shared_ptr<vk::Instance> instance, std::shared_ptr<vk::SurfaceKHR> surface,
           const DeviceRequirement &req);

    static SwapChainSupportDetails querySwapChainSupport(vk::PhysicalDevice &device,
                                                         std::shared_ptr<vk::SurfaceKHR> surface);

    static vk::Format findSupportedFormat(std::shared_ptr<vk::PhysicalDevice> physicalDevice,
                                          const std::vector<vk::Format> &candidates, vk::ImageTiling tiling,
                                          vk::FormatFeatureFlags features);

    static vk::Format findDepthFormat(std::shared_ptr<vk::PhysicalDevice> physicalDevice);

    static bool hasStencilComponent(vk::Format format);

    static bool isFormatSupportLinearBlitting(std::shared_ptr<vk::PhysicalDevice> physicalDevice, vk::Format format);

    static bool checkDeviceSuitable(vk::PhysicalDevice &device, std::shared_ptr<vk::SurfaceKHR> surface,
                                    const DeviceRequirement &req, QueueFamilyIndices &familyIndices,
                                    SwapChainSupportDetails &swapChainSupport);

    static QueueFamilyIndices findQueueFamilies(vk::PhysicalDevice &device, std::shared_ptr<vk::SurfaceKHR> surface);

    static bool checkDeviceExtensionSupport(vk::PhysicalDevice &device, const std::vector<const char *> &extensions);
};