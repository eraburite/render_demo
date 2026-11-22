#pragma once
#include <memory>
#include <tuple>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "typedefs.h"

class PhysicalDeviceUtils {
public:
    static std::tuple<std::shared_ptr<vk::PhysicalDevice>, QueueFamilyIndices>
    select(const std::shared_ptr<vk::Instance> &instance, const std::shared_ptr<vk::SurfaceKHR> &surface,
           const DeviceRequirement &req);

    static SwapChainSupportDetails querySwapChainSupport(const vk::PhysicalDevice &device,
                                                         const std::shared_ptr<vk::SurfaceKHR> &surface);

    static vk::Format findSupportedFormat(const std::shared_ptr<vk::PhysicalDevice> &physicalDevice,
                                          const std::vector<vk::Format> &candidates, vk::ImageTiling tiling,
                                          vk::FormatFeatureFlags features);

    static vk::Format findDepthFormat(const std::shared_ptr<vk::PhysicalDevice> &physicalDevice);

    static bool hasStencilComponent(vk::Format format);

    static bool isFormatSupportLinearBlitting(const std::shared_ptr<vk::PhysicalDevice> &physicalDevice,
                                              vk::Format format);

    static bool checkDeviceSuitable(const vk::PhysicalDevice &device,
                                    const std::shared_ptr<vk::SurfaceKHR> &surface, const DeviceRequirement &req,
                                    QueueFamilyIndices &familyIndices, SwapChainSupportDetails &swapChainSupport);

    static QueueFamilyIndices findQueueFamilies(const vk::PhysicalDevice &device,
                                                const std::shared_ptr<vk::SurfaceKHR> &surface);

    static bool checkDeviceExtensionSupport(const vk::PhysicalDevice &device,
                                            const std::vector<const char *> &extensions);
};
