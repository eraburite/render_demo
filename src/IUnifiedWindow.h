#pragma once
#include <memory>
#include <tuple>
#include <vulkan/vulkan.hpp>

class IUnifiedWindow {
public:
    virtual ~IUnifiedWindow() = default;
    virtual bool shouldClose() = 0;
    virtual void swapBuffers() = 0;
    virtual void poolEvents() = 0;
    virtual bool waitEvents() = 0;
    virtual void processInputEvents(float &currentTime) = 0;
    virtual std::shared_ptr<vk::SurfaceKHR> createSurface(std::shared_ptr<vk::Instance> instance) = 0;
    virtual std::shared_ptr<vk::SurfaceKHR> getSurface() const = 0;
    virtual bool checkSurfaceSupport(std::shared_ptr<vk::PhysicalDevice> device, uint32_t queueFamilyIndex) = 0;
    virtual std::tuple<int, int, int> getFramebufferSize() = 0; // width, height, depth
    virtual void notifyResize() = 0;
    virtual bool checkAndResetResize() = 0;
    virtual std::vector<const char *> QueryRequiredExtensions() const = 0;
};