#pragma once
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>

// Owns the debug utils messenger and exposes the validation layer/extension
// configuration shared by instance and device creation.
class GraphicsDebugger {
public:
    GraphicsDebugger();
    ~GraphicsDebugger();

    bool checkValidationLayerSupport() const;
    void setupDebugMessenger(const std::shared_ptr<vk::Instance> &instance);

    std::vector<const char *> getRequiredLayerNames() const;
    std::vector<const char *> getRequiredExtensions() const;
    VkValidationFeaturesEXT &getValidationFeatures();

private:
    bool checkValidationFeatureSupport(const char *layerName) const;

private:
    std::shared_ptr<VkDebugUtilsMessengerEXT> m_debugMessenger;
    VkValidationFeaturesEXT m_validationFeatures{};
    VkDebugUtilsMessengerCreateInfoEXT m_debugCreateInfo{};
};
