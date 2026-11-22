#pragma once
#define GLFW_INCLUDE_VULKAN
#include "GLFW/glfw3.h"

#include "IUnifiedWindow.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <tuple>
#include <vector>

class Camera;

// GLFW-backed window.  The surface is owned exclusively by RenderManager;
// this class keeps only a weak reference so it is destroyed before the
// instance and before glfwTerminate().
class GLFWwindow : public IUnifiedWindow {
public:
    GLFWwindow(uint32_t width, uint32_t height, std::shared_ptr<Camera> camera);
    ~GLFWwindow() override;

    std::vector<const char *> QueryRequiredExtensions() override;
    std::shared_ptr<vk::SurfaceKHR> createSurface(const std::shared_ptr<vk::Instance> &instance) override;
    std::tuple<int, int, int> getFramebufferSize() const override;
    void waitEvents() override;
    bool checkAndResetResize() override;
    bool shouldClose() const override;
    void swapBuffers() override;

    void processInputEvents();
    void processInputEvents(float &currentTime);
    void poolEvents();

    // Key query used by the debug views.  key is a GLFW_KEY_* constant.  The
    // caller must have pumped events first (poolEvents()), otherwise this
    // reports the previous frame's cached state.
    bool isKeyDown(int key) const;

    Camera *camera() const { return m_camera.get(); }
    const std::shared_ptr<Camera> &cameraShared() const { return m_camera; }

private:
    void mouseMovement(double xposIn, double yposIn);
    void mouseScroll(double xoffset, double yoffset);
    static void framebufferResizeCallback(GLFWwindow *window, int width, int height);
    static void mouseCallback(GLFWwindow *window, double xposIn, double yposIn);
    static void scrollCallback(GLFWwindow *window, double xoffset, double yoffset);

private:
    GLFWwindow *m_window = nullptr;
    std::atomic<bool> m_framebufferResized{false};
    std::weak_ptr<vk::SurfaceKHR> m_surface;
    std::shared_ptr<Camera> m_camera;

    float m_deltaTime = 0.0f;
    float m_lastFrame = 0.0f;
    float m_lastX = 0.0f;
    float m_lastY = 0.0f;
    bool m_firstMouse = true;
};
