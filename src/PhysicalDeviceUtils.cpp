#include "PhysicalDeviceUtils.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

namespace {
void check(vk::Result result, const char *what)
{
    if (result != vk::Result::eSuccess) {
        throw std::runtime_error(std::string(what) + " failed: " + vk::to_string(result));
    }
}
} // namespace

std::tuple<std::shared_ptr<vk::PhysicalDevice>, QueueFamilyIndices>
PhysicalDeviceUtils::select(const std::shared_ptr<vk::Instance> &instance,
                            const std::shared_ptr<vk::SurfaceKHR> &surface, const DeviceRequirement &req)
{
    if (!instance || !surface) {
        throw std::runtime_error("physical device selection requires an instance and a surface!");
    }

    uint32_t deviceCount = 0;
    check(instance->enumeratePhysicalDevices(&deviceCount, nullptr), "vkEnumeratePhysicalDevices (count)");
    std::vector<vk::PhysicalDevice> devices(deviceCount);
    check(instance->enumeratePhysicalDevices(&deviceCount, devices.data()), "vkEnumeratePhysicalDevices");

    for (const vk::PhysicalDevice &device : devices) {
        QueueFamilyIndices familyIndices{};
        SwapChainSupportDetails swapChainSupport{};
        if (checkDeviceSuitable(device, surface, req, familyIndices, swapChainSupport)) {
            return std::make_tuple(std::make_shared<vk::PhysicalDevice>(device), familyIndices);
        }
    }
    throw std::runtime_error("failed to find a suitable GPU!");
}

bool PhysicalDeviceUtils::checkDeviceSuitable(const vk::PhysicalDevice &device,
                                              const std::shared_ptr<vk::SurfaceKHR> &surface,
                                              const DeviceRequirement &req, QueueFamilyIndices &familyIndices,
                                              SwapChainSupportDetails &swapChainSupport)
{
    familyIndices = findQueueFamilies(device, surface);
    const bool extensionsSupported = checkDeviceExtensionSupport(device, req.extensions);

    bool swapChainAdequate = false;
    if (extensionsSupported) {
        swapChainSupport = querySwapChainSupport(device, surface);
        swapChainAdequate = swapChainSupport.IsAdequate();
    }

    vk::PhysicalDeviceFeatures supportedFeatures{};
    device.getFeatures(&supportedFeatures);
    // Optional features are only required when the caller asked for them, so
    // existing examples keep selecting the same device as before.
    const bool featuresAdequate =
        (!req.samplerAnisotropy || supportedFeatures.samplerAnisotropy)
        && (!req.geometryShader || supportedFeatures.geometryShader)
        && (!req.tessellationShader || supportedFeatures.tessellationShader)
        && (!req.fillModeNonSolid || supportedFeatures.fillModeNonSolid);
    return familyIndices.isComplete() && extensionsSupported && swapChainAdequate && featuresAdequate
           && (!req.compute || familyIndices.computeFamily.has_value());
}

QueueFamilyIndices PhysicalDeviceUtils::findQueueFamilies(const vk::PhysicalDevice &device,
                                                          const std::shared_ptr<vk::SurfaceKHR> &surface)
{
    QueueFamilyIndices indices;
    if (!surface) {
        return indices;
    }

    uint32_t queueFamilyCount = 0;
    device.getQueueFamilyProperties(&queueFamilyCount, nullptr);
    std::vector<vk::QueueFamilyProperties> queueFamilies(queueFamilyCount);
    device.getQueueFamilyProperties(&queueFamilyCount, queueFamilies.data());

    // Cache surface support for every family: compute-only families usually
    // appear after the graphics family, so an early break would miss them.
    std::vector<bool> presentSupport(queueFamilyCount, false);
    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        vk::Bool32 supported = VK_FALSE;
        check(device.getSurfaceSupportKHR(i, *surface, &supported), "vkGetPhysicalDeviceSurfaceSupportKHR");
        presentSupport[i] = supported != VK_FALSE;
    }

    constexpr uint32_t kInvalid = std::numeric_limits<uint32_t>::max();
    uint32_t graphicsWithPresent = kInvalid;
    uint32_t firstGraphics = kInvalid;
    uint32_t firstPresent = kInvalid;
    uint32_t computeOnly = kInvalid;

    for (uint32_t i = 0; i < queueFamilyCount; ++i) {
        const vk::QueueFlags flags = queueFamilies[i].queueFlags;
        const bool isGraphics = (flags & vk::QueueFlagBits::eGraphics) != vk::QueueFlags{};
        const bool isCompute = (flags & vk::QueueFlagBits::eCompute) != vk::QueueFlags{};

        if (isGraphics && presentSupport[i] && graphicsWithPresent == kInvalid) {
            graphicsWithPresent = i;
        }
        if (isGraphics && firstGraphics == kInvalid) {
            firstGraphics = i;
        }
        if (presentSupport[i] && firstPresent == kInvalid) {
            firstPresent = i;
        }
        if (isCompute && !isGraphics && computeOnly == kInvalid) {
            computeOnly = i;
        }
    }

    if (graphicsWithPresent != kInvalid) {
        indices.graphicsFamily = graphicsWithPresent;
    } else if (firstGraphics != kInvalid) {
        indices.graphicsFamily = firstGraphics;
    }

    if (indices.graphicsFamily.has_value() && presentSupport[*indices.graphicsFamily]) {
        indices.presentFamily = indices.graphicsFamily;
    } else if (firstPresent != kInvalid) {
        indices.presentFamily = firstPresent;
    }

    if (computeOnly != kInvalid) {
        indices.computeFamily = computeOnly;
    } else {
        indices.computeFamily = indices.graphicsFamily;
    }

    return indices;
}

bool PhysicalDeviceUtils::checkDeviceExtensionSupport(const vk::PhysicalDevice &device,
                                                       const std::vector<const char *> &extensions)
{
    uint32_t extensionCount = 0;
    check(device.enumerateDeviceExtensionProperties(nullptr, &extensionCount, nullptr),
          "vkEnumerateDeviceExtensionProperties (count)");
    std::vector<vk::ExtensionProperties> availableExtensions(extensionCount);
    check(device.enumerateDeviceExtensionProperties(nullptr, &extensionCount, availableExtensions.data()),
          "vkEnumerateDeviceExtensionProperties");

    std::set<std::string> requiredExtensions(extensions.begin(), extensions.end());
    for (const vk::ExtensionProperties &extension : availableExtensions) {
        requiredExtensions.erase(extension.extensionName.data());
    }
    return requiredExtensions.empty();
}

SwapChainSupportDetails PhysicalDeviceUtils::querySwapChainSupport(
    const vk::PhysicalDevice &device, const std::shared_ptr<vk::SurfaceKHR> &surface)
{
    SwapChainSupportDetails details;
    if (!surface) {
        return details;
    }

    check(device.getSurfaceCapabilitiesKHR(*surface, &details.capabilities), "vkGetSurfaceCapabilitiesKHR");

    uint32_t formatCount = 0;
    check(device.getSurfaceFormatsKHR(*surface, &formatCount, nullptr), "vkGetSurfaceFormatsKHR (count)");
    if (formatCount != 0) {
        details.formats.resize(formatCount);
        check(device.getSurfaceFormatsKHR(*surface, &formatCount, details.formats.data()), "vkGetSurfaceFormatsKHR");
    }

    uint32_t presentModeCount = 0;
    check(device.getSurfacePresentModesKHR(*surface, &presentModeCount, nullptr), "vkGetSurfacePresentModesKHR (count)");
    if (presentModeCount != 0) {
        details.presentModes.resize(presentModeCount);
        check(device.getSurfacePresentModesKHR(*surface, &presentModeCount, details.presentModes.data()),
              "vkGetSurfacePresentModesKHR");
    }
    return details;
}

vk::Format PhysicalDeviceUtils::findSupportedFormat(const std::shared_ptr<vk::PhysicalDevice> &physicalDevice,
                                                    const std::vector<vk::Format> &candidates, vk::ImageTiling tiling,
                                                    vk::FormatFeatureFlags features)
{
    if (!physicalDevice) {
        throw std::runtime_error("format lookup requires a physical device!");
    }

    for (vk::Format format : candidates) {
        vk::FormatProperties properties{};
        physicalDevice->getFormatProperties(format, &properties);
        if (tiling == vk::ImageTiling::eLinear
            && (properties.linearTilingFeatures & features) == features) {
            return format;
        }
        if (tiling == vk::ImageTiling::eOptimal
            && (properties.optimalTilingFeatures & features) == features) {
            return format;
        }
    }
    throw std::runtime_error("failed to find a supported format!");
}

vk::Format PhysicalDeviceUtils::findDepthFormat(const std::shared_ptr<vk::PhysicalDevice> &physicalDevice)
{
    return findSupportedFormat(physicalDevice,
                               {vk::Format::eD32Sfloat, vk::Format::eD32SfloatS8Uint, vk::Format::eD24UnormS8Uint},
                               vk::ImageTiling::eOptimal, vk::FormatFeatureFlagBits::eDepthStencilAttachment);
}

bool PhysicalDeviceUtils::hasStencilComponent(vk::Format format)
{
    return format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eD24UnormS8Uint;
}

bool PhysicalDeviceUtils::isFormatSupportLinearBlitting(const std::shared_ptr<vk::PhysicalDevice> &physicalDevice,
                                                        vk::Format format)
{
    if (!physicalDevice) {
        return false;
    }
    vk::FormatProperties formatProperties{};
    physicalDevice->getFormatProperties(format, &formatProperties);
    const vk::FormatFeatureFlags flags = formatProperties.optimalTilingFeatures;
    return (flags & vk::FormatFeatureFlagBits::eSampledImageFilterLinear)
           == vk::FormatFeatureFlagBits::eSampledImageFilterLinear;
}
