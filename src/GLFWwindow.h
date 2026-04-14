#pragma once
#define GLFW_INCLUDE_VULKAN
#include "GLFW/glfw3.h"

#include "IUnifiedWindow.h"
#include <cstdint>
#include <atomic>
#include "Camera.h"

class GLFWwindow : public IUnifiedWindow {
public:
    GLFWwindow(uint32_t width, uint32_t height, std::shared_ptr<Camera> camera);
    virtual ~GLFWwindow();
    virtual bool shouldClose() override;
    virtual void swapBuffers() override;
    virtual void poolEvents() override;
    virtual bool waitEvents() override;
    virtual void processInputEvents(float &currentTime) override;
    virtual std::shared_ptr<vk::SurfaceKHR> createSurface(std::shared_ptr<vk::Instance> instance) override;
    virtual std::shared_ptr<vk::SurfaceKHR> getSurface() const override;
    virtual bool checkSurfaceSupport(std::shared_ptr<vk::PhysicalDevice> device, uint32_t queueFamilyIndex) override;
    virtual std::tuple<int, int, int> getFramebufferSize() override;
    virtual void notifyResize() override;
    virtual bool checkAndResetResize() override;
    virtual std::vector<const char *> QueryRequiredExtensions() const override;

private:
    void mouseMovement(double xposIn, double yposIn);
    void mouseScroll(double xoffset, double yoffset);
    static void framebufferResizeCallback(GLFWwindow *window, int width, int height);
    static void mouseCallback(GLFWwindow *window, double xposIn, double yposIn);
    static void scrollCallback(GLFWwindow *window, double xoffset, double yoffset);

private:
    GLFWwindow *m_window;
    std::atomic<bool> m_framebufferResized{false};
    std::shared_ptr<vk::SurfaceKHR> m_surface;
    std::shared_ptr<Camera> m_camera;

    // timing
    float m_deltaTime = 0.0f;
    float m_lastFrame = 0.0f;
    // input
    float m_lastX;
    float m_lastY;
    bool m_firstMouse;
};