#pragma once
#include <memory>
#include <tuple>
#include <vulkan/vulkan.hpp>

class IUnifiedWindow {
public:
    virtual ~IUnifiedWindow() = default;
    virtual std::vector<const char *> QueryRequiredExtensions() = 0;
    virtual std::shared_ptr<vk::SurfaceKHR> createSurface(const std::shared_ptr<vk::Instance> &instance) = 0;
    virtual std::tuple<int, int, int> getFramebufferSize() const = 0;
    virtual void waitEvents() = 0;
    virtual bool checkAndResetResize() = 0;
    virtual bool shouldClose() const = 0;
    virtual void swapBuffers() = 0;
};
