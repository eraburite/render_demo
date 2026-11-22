#include "GLFWwindow.h"

#include <stdexcept>
#include <tuple>

#if __has_include("Camera.h")
#include "Camera.h"
#define RENDER_DEMO_GLFW_HAS_CAMERA 1
#endif

void GLFWwindow::framebufferResizeCallback(GLFWwindow *window, int width, int height)
{
    (void)width;
    (void)height;
    auto *app = reinterpret_cast<GLFWwindow *>(glfwGetWindowUserPointer(window));
    if (app != nullptr) {
        app->m_framebufferResized.store(true, std::memory_order_relaxed);
    }
}

void GLFWwindow::mouseCallback(GLFWwindow *window, double xposIn, double yposIn)
{
    auto *app = reinterpret_cast<GLFWwindow *>(glfwGetWindowUserPointer(window));
    if (app != nullptr) {
        app->mouseMovement(xposIn, yposIn);
    }
}

void GLFWwindow::scrollCallback(GLFWwindow *window, double xoffset, double yoffset)
{
    (void)xoffset;
    auto *app = reinterpret_cast<GLFWwindow *>(glfwGetWindowUserPointer(window));
    if (app != nullptr) {
        app->mouseScroll(xoffset, yoffset);
    }
}

GLFWwindow::GLFWwindow(uint32_t width, uint32_t height, std::shared_ptr<Camera> camera) :
    m_camera(std::move(camera))
{
    if (glfwInit() != GLFW_TRUE) {
        throw std::runtime_error("failed to initialize GLFW!");
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    m_window = glfwCreateWindow(static_cast<int>(width), static_cast<int>(height), "Vulkan", nullptr, nullptr);
    if (m_window == nullptr) {
        glfwTerminate();
        throw std::runtime_error("failed to create GLFW window!");
    }

    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, framebufferResizeCallback);
    glfwSetCursorPosCallback(m_window, mouseCallback);
    glfwSetScrollCallback(m_window, scrollCallback);
    glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    m_lastX = static_cast<float>(width) / 2.0f;
    m_lastY = static_cast<float>(height) / 2.0f;
}

GLFWwindow::~GLFWwindow()
{
    if (m_window != nullptr) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
    }
    glfwTerminate();
}

std::vector<const char *> GLFWwindow::QueryRequiredExtensions()
{
    uint32_t count = 0;
    const char **extensions = glfwGetRequiredInstanceExtensions(&count);
    if (extensions == nullptr) {
        return {};
    }
    return std::vector<const char *>(extensions, extensions + count);
}

std::shared_ptr<vk::SurfaceKHR> GLFWwindow::createSurface(const std::shared_ptr<vk::Instance> &instance)
{
    if (m_window == nullptr) {
        throw std::runtime_error("cannot create a surface without a window!");
    }
    VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
    const VkResult result = glfwCreateWindowSurface(static_cast<VkInstance>(*instance), m_window, nullptr, &rawSurface);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("failed to create window surface!");
    }
    auto surface = std::shared_ptr<vk::SurfaceKHR>(new vk::SurfaceKHR(rawSurface),
        [instance](vk::SurfaceKHR *p) {
            if (p && *p) { instance->destroySurfaceKHR(*p); }
            delete p;
        });
    m_surface = surface;
    return surface;
}

std::tuple<int, int, int> GLFWwindow::getFramebufferSize() const
{
    int width = 0;
    int height = 0;
    if (m_window != nullptr) {
        glfwGetFramebufferSize(m_window, &width, &height);
    }
    return std::make_tuple(width, height, 1);
}

void GLFWwindow::waitEvents()
{
    glfwWaitEvents();
}

bool GLFWwindow::checkAndResetResize()
{
    return m_framebufferResized.exchange(false, std::memory_order_relaxed);
}

bool GLFWwindow::shouldClose() const
{
    return m_window != nullptr && glfwWindowShouldClose(m_window) != GLFW_FALSE;
}

void GLFWwindow::swapBuffers()
{
    glfwSwapBuffers(m_window);
    glfwPollEvents();
}

void GLFWwindow::poolEvents()
{
    glfwPollEvents();
}

void GLFWwindow::processInputEvents()
{
    float currentTime = 0.0f;
    processInputEvents(currentTime);
}

void GLFWwindow::processInputEvents(float &currentTime)
{
    currentTime = static_cast<float>(glfwGetTime());
    m_deltaTime = currentTime - m_lastFrame;
    m_lastFrame = currentTime;

    if (m_window == nullptr) {
        return;
    }

    if (glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(m_window, GLFW_TRUE);
    }

#ifdef RENDER_DEMO_GLFW_HAS_CAMERA
    if (m_camera) {
        if (glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS) {
            m_camera->ProcessKeyboard(FORWARD, m_deltaTime);
        }
        if (glfwGetKey(m_window, GLFW_KEY_S) == GLFW_PRESS) {
            m_camera->ProcessKeyboard(BACKWARD, m_deltaTime);
        }
        if (glfwGetKey(m_window, GLFW_KEY_A) == GLFW_PRESS) {
            m_camera->ProcessKeyboard(LEFT, m_deltaTime);
        }
        if (glfwGetKey(m_window, GLFW_KEY_D) == GLFW_PRESS) {
            m_camera->ProcessKeyboard(RIGHT, m_deltaTime);
        }
    }
#endif
}

void GLFWwindow::mouseMovement(double xposIn, double yposIn)
{
#ifdef RENDER_DEMO_GLFW_HAS_CAMERA
    if (!m_camera) {
        return;
    }
    const float xpos = static_cast<float>(xposIn);
    const float ypos = static_cast<float>(yposIn);

    if (m_firstMouse) {
        m_lastX = xpos;
        m_lastY = ypos;
        m_firstMouse = false;
    }

    const float xoffset = xpos - m_lastX;
    const float yoffset = m_lastY - ypos; // Y axis points down in window coordinates
    m_lastX = xpos;
    m_lastY = ypos;
    m_camera->ProcessMouseMovement(xoffset, yoffset);
#else
    (void)xposIn;
    (void)yposIn;
#endif
}

void GLFWwindow::mouseScroll(double xoffset, double yoffset)
{
    (void)xoffset;
#ifdef RENDER_DEMO_GLFW_HAS_CAMERA
    if (m_camera) {
        m_camera->ProcessMouseScroll(static_cast<float>(yoffset));
    }
#else
    (void)yoffset;
#endif
}
