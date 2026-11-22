#include "GraphicsDebugger.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace {
const std::vector<const char *> validationLayers = {
    "VK_LAYER_KHRONOS_validation",
};

const std::vector<const char *> validationExtensions = {
    VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
    VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME,
};

const VkValidationFeatureEnableEXT validationFeatureEnables[] = {
    VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
};

VkResult CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT *pCreateInfo,
                                      const VkAllocationCallbacks *pAllocator,
                                      VkDebugUtilsMessengerEXT *pDebugMessenger)
{
    auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    if (func != nullptr) {
        return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
    }
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}

void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger,
                                   const VkAllocationCallbacks *pAllocator)
{
    auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (func != nullptr) {
        func(instance, debugMessenger, pAllocator);
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                             VkDebugUtilsMessageTypeFlagsEXT messageType,
                                             const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData, void *pUserData)
{
    (void)messageSeverity;
    (void)messageType;
    (void)pUserData;
    std::cerr << " validation layer: " << pCallbackData->pMessage << std::endl;
    return VK_FALSE;
}

void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT &createInfo)
{
    createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT
                                 | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                                 | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                             | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                             | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugCallback;
    createInfo.pNext = nullptr; // keep this the last node in the pNext chain
}
} // namespace

GraphicsDebugger::GraphicsDebugger() = default;

GraphicsDebugger::~GraphicsDebugger() = default;

std::vector<const char *> GraphicsDebugger::getRequiredLayerNames() const
{
    return validationLayers;
}

std::vector<const char *> GraphicsDebugger::getRequiredExtensions() const
{
    return validationExtensions;
}

bool GraphicsDebugger::checkValidationLayerSupport() const
{
    const std::vector<vk::LayerProperties> availableLayers = vk::enumerateInstanceLayerProperties();
    std::unordered_set<std::string> availableLayerSet;
    for (const vk::LayerProperties &layer : availableLayers) {
        availableLayerSet.insert(layer.layerName.data());
    }

    for (const char *layerName : validationLayers) {
        if (availableLayerSet.count(layerName) == 0) {
            return false;
        }
        if (!checkValidationFeatureSupport(layerName)) {
            return false;
        }
    }
    return true;
}

bool GraphicsDebugger::checkValidationFeatureSupport(const char *layerName) const
{
    if (validationExtensions.empty()) {
        return true;
    }

    const std::vector<vk::ExtensionProperties> availableExtensions =
        vk::enumerateInstanceExtensionProperties(std::string(layerName));
    std::set<std::string> availableExtensionSet;
    for (const vk::ExtensionProperties &extension : availableExtensions) {
        availableExtensionSet.insert(extension.extensionName.data());
    }

    return std::all_of(validationExtensions.begin(), validationExtensions.end(),
                       [&availableExtensionSet](const char *name) {
                           return availableExtensionSet.count(name) > 0;
                       });
}

VkValidationFeaturesEXT &GraphicsDebugger::getValidationFeatures()
{
    populateDebugMessengerCreateInfo(m_debugCreateInfo);
    m_validationFeatures = {};
    m_validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    m_validationFeatures.enabledValidationFeatureCount =
        static_cast<uint32_t>(std::size(validationFeatureEnables));
    m_validationFeatures.pEnabledValidationFeatures = validationFeatureEnables;
    // The messenger create info is kept alive by this object and is the tail
    // of the chain, as required by the validation feature specification.
    m_validationFeatures.pNext = &m_debugCreateInfo;
    return m_validationFeatures;
}

void GraphicsDebugger::setupDebugMessenger(const std::shared_ptr<vk::Instance> &instance)
{
    if (!instance) {
        throw std::runtime_error("cannot set up debug messenger without a valid instance!");
    }

    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    populateDebugMessengerCreateInfo(createInfo);

    VkDebugUtilsMessengerEXT debugMessenger{};
    const VkResult result =
        CreateDebugUtilsMessengerEXT(static_cast<VkInstance>(*instance), &createInfo, nullptr, &debugMessenger);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("failed to set up debug messenger!");
    }

    // The deleter captures a (possibly non-owning) view of the instance so the
    // messenger is always destroyed before the instance itself.
    m_debugMessenger = std::shared_ptr<VkDebugUtilsMessengerEXT>(
        new VkDebugUtilsMessengerEXT(debugMessenger),
        [instance](VkDebugUtilsMessengerEXT *messenger) {
            if (messenger != nullptr && *messenger != VK_NULL_HANDLE) {
                DestroyDebugUtilsMessengerEXT(static_cast<VkInstance>(*instance), *messenger, nullptr);
            }
            delete messenger;
        });
}
