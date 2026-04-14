#include "GraphicsDebugger.h"
#include <iostream>
#include <unordered_set>

namespace {
static const std::vector<const char *> validationLayers = {
    "VK_LAYER_KHRONOS_validation",
};

static const std::vector<const char *> validationExtensions = {
    VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
    VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME,
};

// configure GPU Validation Features.
static const VkValidationFeatureEnableEXT validationFeatureEnables[] = {
    VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT, VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT,
    VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};

// debugger
VkResult CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT *pCreateInfo,
                                      const VkAllocationCallbacks *pAllocator,
                                      VkDebugUtilsMessengerEXT *pDebugMessenger)
{
    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    if (func != nullptr) {
        return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
    } else {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
}

void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger,
                                   const VkAllocationCallbacks *pAllocator)
{
    auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (func != nullptr) {
        func(instance, debugMessenger, pAllocator);
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                                             VkDebugUtilsMessageTypeFlagsEXT messageType,
                                             const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData, void *pUserData)
{
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
    createInfo.pNext = nullptr; // as the last in pNext chain
}
}; // end namespace

GraphicsDebugger::GraphicsDebugger() {}
GraphicsDebugger::~GraphicsDebugger() {}

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
    // uint32 t layerCount;
    // vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    // std:: vector<VkLayerProperties> availableLayers(layerCount);
    // vkEnumerateInstanceLayerProperties(&layerCount, availableLayers. data());
    std::vector<vk::LayerProperties> availableLayerProperties = vk::enumerateInstanceLayerProperties();
    std::unordered_set<std::string> availableLayerSet;
    for (const vk::LayerProperties &layerProperties : availableLayerProperties) {
        std::string layerName(layerProperties.layerName.data());
        std::cout << "layerName: " << layerName << "\n";
        availableLayerSet.insert(layerName);
    }

    for (const char *layerName : validationLayers) {
        if (availableLayerSet.find(layerName) == availableLayerSet.end()) {
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

    // uint32 t extensionCount = 0;
    // vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
    // std:: vector<VkExtensionProperties> availableExtensions(extensionCount);
    // vkEnumerateInstanceExtensionProperties(layerName, &extensionCount,  availableExtensions. data( ));
    std::vector<vk::ExtensionProperties> extensionProperties =
        vk::enumerateInstanceExtensionProperties(std::string(layerName));
    std::unordered_set<std::string> availableExtSet;
    for (const vk::ExtensionProperties &ext : extensionProperties) {
        std::cout << " ext.extensionName: " << ext.extensionName << "\n";
        availableExtSet.insert(ext.extensionName);
    }

    return std::all_of(validationExtensions.begin(), validationExtensions.end(),
                       [&availableExtSet](const char *extName) { return availableExtSet.count(extName) > 0; });
}

VkValidationFeaturesEXT &GraphicsDebugger::getValidationFeatures()
{
    populateDebugMessengerCreateInfo(m_debugCreateInfo);
    m_validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    m_validationFeatures.enabledValidationFeatureCount = 2;
    m_validationFeatures.pEnabledValidationFeatures = validationFeatureEnables;
    // setting the pNext chain, pointing to m debugCreateInfo
    m_validationFeatures.pNext = &m_debugCreateInfo;
    return m_validationFeatures;
}

void GraphicsDebugger::setupDebugMessenger(std::shared_ptr<vk::Instance> instance)
{
    VkDebugUtilsMessengerCreateInfoEXT createInfo;
    populateDebugMessengerCreateInfo(createInfo);
    VkDebugUtilsMessengerEXT debugMessenger{};

    if (CreateDebugUtilsMessengerEXT(*instance, &createInfo, nullptr, &debugMessenger) != VK_SUCCESS) {
        throw std::runtime_error(" failed to set up debug messenger!");
    }

    m_debugMessenger = std::shared_ptr<VkDebugUtilsMessengerEXT>(
        new VkDebugUtilsMessengerEXT(debugMessenger), [instance](VkDebugUtilsMessengerEXT *p) {
            if (p && *p) {
                DestroyDebugUtilsMessengerEXT(*instance, *p, nullptr);
            }
            delete p;
        });
}