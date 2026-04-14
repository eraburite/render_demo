#pragma once
#include <memory>
#include <vulkan/vulkan.hpp>

class GraphicsDebugger {
public:
    GraphicsDebugger();
    ~GraphicsDebugger();

    bool checkValidationLayerSupport() const;
    void setupDebugMessenger(std::shared_ptr<vk::Instance> instance);

    std::vector<const char *> getRequiredLayerNames() const;
    std::vector<const char *> getRequiredExtensions() const;
    VkValidationFeaturesEXT &getValidationFeatures();

private:
    bool checkValidationFeatureSupport(const char *layerName) const;

private:
    std::shared_ptr<VkDebugUtilsMessengerEXT> m_debugMessenger;
    VkValidationFeaturesEXT m_validationFeatures = {};
    VkDebugUtilsMessengerCreateInfoEXT m_debugCreateInfo{};
};