#include "PhysicalDeviceUtils.h"
#include <set>
#include <string>

std::tuple<std::shared_ptr<vk::PhysicalDevice>, QueueFamilyIndices>
PhysicalDeviceUtils::select(std::shared_ptr<vk::Instance> instance, std::shared_ptr<vk::SurfaceKHR> surface,
                            const DeviceRequirement &req)
{
    uint32_t deviceCount = 0;
    auto result = instance->enumeratePhysicalDevices(&deviceCount, nullptr);
    std::vector<vk::PhysicalDevice> devices(deviceCount);
    result = instance->enumeratePhysicalDevices(&deviceCount, devices.data());

    for (auto &device : devices) {
        QueueFamilyIndices familyIndices{};
        SwapChainSupportDetails swapChainSupport{};
        if (checkDeviceSuitable(device, surface, req, familyIndices, swapChainSupport)) {
            return std::tuple(std::make_shared<vk::PhysicalDevice>(device), familyIndices);
        }
    }
    throw std::runtime_error(" failed to find a suitable GPU!");
}

bool PhysicalDeviceUtils::checkDeviceSuitable(vk::PhysicalDevice &device, std::shared_ptr<vk::SurfaceKHR> surface,
                                              const DeviceRequirement &req, QueueFamilyIndices &familyIndices,
                                              SwapChainSupportDetails &swapChainSupport)
{
    // 1. Check the queue family
    familyIndices = findQueueFamilies(device, surface);
    // 2. Check for extension support (such as the SwapChain extension)
    bool extensionsSupported = checkDeviceExtensionSupport(device, req.extensions);
    // 3. Check that the SwapChain details meet the requirements.
    bool swapChainAdequate = false;
    if (extensionsSupported) {
        swapChainSupport = querySwapChainSupport(device, surface);
        swapChainAdequate = swapChainSupport.IsAdequate();
    }

    // TODO: Other features support/match the vk::DeviceCreateInfo settings
    // during LogicalDevice creation
    vk::PhysicalDeviceFeatures supportedFeatures;
    device.getFeatures(&supportedFeatures);
    return familyIndices.isComplete() && extensionsSupported && swapChainAdequate
           && supportedFeatures.samplerAnisotropy;
}

QueueFamilyIndices PhysicalDeviceUtils::findQueueFamilies(vk::PhysicalDevice &device,
                                                          std::shared_ptr<vk::SurfaceKHR> surface)
{
    QueueFamilyIndices indices;
    uint32_t queueFamilyCount = 0;
    device.getQueueFamilyProperties(&queueFamilyCount, nullptr);
    std::vector<vk::QueueFamilyProperties> queueFamilies(queueFamilyCount);
    device.getQueueFamilyProperties(&queueFamilyCount, queueFamilies.data());
    int i = 0;
    for (const auto &queueFamily : queueFamilies) {
        if (queueFamily.queueFlags & vk::QueueFlagBits::eGraphics) {
            indices.graphicsFamily = i;
        }
        VkBool32 presentSupport = false;
        auto ret = device.getSurfaceSupportKHR(i, *surface, &presentSupport);
        if (presentSupport) {
            indices.presentFamily = i;
        }
        if (indices.isComplete()) {
            break;
        }
        i++;
    }
    return indices;
}

bool PhysicalDeviceUtils::checkDeviceExtensionSupport(vk::PhysicalDevice &device,
                                                      const std::vector<const char *> &extensions)
{
    uint32_t extensionCount;
    auto ret = device.enumerateDeviceExtensionProperties(nullptr, &extensionCount, nullptr);
    std::vector<vk::ExtensionProperties> availableExtensions(extensionCount);
    ret = device.enumerateDeviceExtensionProperties(nullptr, &extensionCount, availableExtensions.data());
    std::set<std::string> requiredExtensions(extensions.begin(), extensions.end());
    for (const auto &extension : availableExtensions) {
        requiredExtensions.erase(extension.extensionName);
    }
    return requiredExtensions.empty();
}

SwapChainSupportDetails PhysicalDeviceUtils::querySwapChainSupport(vk::PhysicalDevice &device,
                                                                   std::shared_ptr<vk::SurfaceKHR> surface)
{
    SwapChainSupportDetails details;
    auto ret = device.getSurfaceCapabilitiesKHR(*surface, &details.capabilities);
    uint32_t formatCount;
    ret = device.getSurfaceFormatsKHR(*surface, &formatCount, nullptr);
    if (formatCount != 0) {
        details.formats.resize(formatCount);
        ret = device.getSurfaceFormatsKHR(*surface, &formatCount, details.formats.data());
    }
    uint32_t presentModeCount;
    ret = device.getSurfacePresentModesKHR(*surface, &presentModeCount, nullptr);
    if (presentModeCount != 0) {
        details.presentModes.resize(presentModeCount);
        ret = device.getSurfacePresentModesKHR(*surface, &presentModeCount, details.presentModes.data());
    }
    return details;
}

vk::Format PhysicalDeviceUtils::findSupportedFormat(std::shared_ptr<vk::PhysicalDevice> physicalDevice,
                                                    const std::vector<vk::Format> &candidates, vk::ImageTiling tiling,
                                                    vk::FormatFeatureFlags features)
{
    for (vk::Format format : candidates) {
        vk::FormatProperties props;
        physicalDevice->getFormatProperties(format, &props);
        if (tiling == vk::ImageTiling::eLinear && (props.linearTilingFeatures & features) == features) {
            return format;
        } else if (tiling == vk::ImageTiling::eOptimal && (props.optimalTilingFeatures & features) == features) {
            return format;
        }
    }

    throw std::runtime_error(" failed to find supported format!");
}

vk::Format PhysicalDeviceUtils::findDepthFormat(std::shared_ptr<vk::PhysicalDevice> physicalDevice)
{
    return findSupportedFormat(physicalDevice,
                               {vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint},
                               vk::ImageTiling::eOptimal, vk::FormatFeatureFlagBits::eDepthStencilAttachment);
}

bool PhysicalDeviceUtils::hasStencilComponent(vk::Format format)
{
    return format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint;
}

bool PhysicalDeviceUtils::isFormatSupportLinearBlitting(std::shared_ptr<vk::PhysicalDevice> physicalDevice,
                                                        vk::Format format)
{
    // Check if image format supports linear blitting
    vk::FormatProperties formatProperties;
    physicalDevice->getFormatProperties(format, &formatProperties);
    if (formatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImageFilterLinear) {
        return true;
    }
    return false;
}