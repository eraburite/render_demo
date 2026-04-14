#include "GLFWwindow.h"
#include <tuple>

// static callback here
void GLFWwindow::framebufferResizeCallback(GLFWwindow *window, int width, int height)
{
    auto app = reinterpret_cast<GLFWwindow *>(glfwGetWindowUserPointer(window));
    app->notifyResize();
}

void GLFWwindow::mouseCallback(GLFWwindow *window, double xposIn, double yposIn)
{
    auto app = reinterpret_cast<GLFWwindow *>(glfwGetWindowUserPointer(window));
    app->mouseMovement(xposIn, yposIn);
}
void GLFWwindow::scrollCallback(GLFWwindow *window, double xoffset, double yoffset)
{
    auto app = reinterpret_cast<GLFWwindow *>(glfwGetWindowUserPointer(window));
    app->mouseScroll(xoffset, yoffset);
}

GLFWwindow::GLFWwindow(uint32_t width, uint32_t height, std::shared_ptr<Camera> camera)
{
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    m_window = glfwCreateWindow(width, height, " Vulkan", nullptr, nullptr);

    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, framebufferResizeCallback);
    glfwSetCursorPosCallback(m_window, mouseCallback);
    glfwSetScrollCallback(m_window, scrollCallback);
    // tell GLFW to capture our mouse
    glfwSetInputMode(m_window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    m_camera = camera;
    m_lastX = static_cast<float>(width) / 2.0f;
    m_lastY = static_cast<float>(height) / 2.0f;
    m_firstMouse = true;
}
GLFWwindow::~GLFWwindow()
{
    glfwDestroyWindow(m_window);
    glfwTerminate();
}

bool GLFWwindow::shouldClose()
{
    return glfwWindowShouldClose(m_window);
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

bool GLFWwindow::waitEvents()
{
    glfwWaitEvents();
    return true;
}

std::shared_ptr<vk::SurfaceKHR> GLFWwindow::createSurface(std::shared_ptr<vk::Instance> instance)
{
    vk::SurfaceKHR surface{nullptr};
    if (glfwCreateWindowSurface(*instance, m_window, nullptr, reinterpret_cast<VkSurfaceKHR *>(&surface))) {
        throw std::runtime_error(" failed to create window surface!");
    }

    m_surface = std::shared_ptr<vk::SurfaceKHR>(new vk::SurfaceKHR(surface), [instance](vk::SurfaceKHR *p) {
        if (p && *p) {
            instance->destroySurfaceKHR(*p);
        }
        delete p;
    });
    return m_surface;
}

std::shared_ptr<vk::SurfaceKHR> GLFWwindow::getSurface() const
{
    return m_surface;
}

bool GLFWwindow::checkSurfaceSupport(std::shared_ptr<vk::PhysicalDevice> device, uint32_t queueFamilyIndex)
{
    VkBool32 presentSupport = VK_FALSE;
    auto ret = device->getSurfaceSupportKHR(queueFamilyIndex, *m_surface, &presentSupport);
    return (presentSupport == VK_TRUE);
}

std::tuple<int, int, int> GLFWwindow::getFramebufferSize()
{
    int width = 0, height = 0;
    int depth = 1;
    glfwGetFramebufferSize(m_window, &width, &height);
    return std::make_tuple(width, height, depth);
}

void GLFWwindow::notifyResize()
{
    m_framebufferResized.store(true, std::memory_order_relaxed);
}

bool GLFWwindow::checkAndResetResize()
{
    return m_framebufferResized.exchange(false);
}

std::vector<const char *> GLFWwindow::QueryRequiredExtensions() const
{
    uint32_t glfwExtensionCount = 0;
    const char **glfwExtensions;
    glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    std::vector<const char *> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);
    return extensions;
}

void GLFWwindow::mouseMovement(double xposIn, double yposIn)
{
    float xpos = static_cast<float>(xposIn);
    float ypos = static_cast<float>(yposIn);

    if (m_firstMouse) {
        m_lastX = xpos;
        m_lastY = ypos;
        m_firstMouse = false;
    }

    float xoffset = xpos - m_lastX;
    float yoffset = m_lastY - ypos; // reversed since y-coordinates go from_bottom_to top
    m_lastX = xpos;
    m_lastY = ypos;
    m_camera->ProcessMouseMovement(xoffset, yoffset);
}

void GLFWwindow::mouseScroll(double xoffset, double yoffset)
{
    m_camera->ProcessMouseScroll(static_cast<float>(yoffset));
}

void GLFWwindow::processInputEvents(float &currentTime)
{
    currentTime = static_cast<float>(glfwGetTime());
    m_deltaTime = currentTime - m_lastFrame;
    m_lastFrame = currentTime;

    if (glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_PRESS) glfwSetWindowShouldClose(m_window, true);
    if (glfwGetKey(m_window, GLFW_KEY_W) == GLFW_PRESS) m_camera->ProcessKeyboard(FORWARD, m_deltaTime);
    if (glfwGetKey(m_window, GLFW_KEY_S) == GLFW_PRESS) m_camera->ProcessKeyboard(BACKWARD, m_deltaTime);
    if (glfwGetKey(m_window, GLFW_KEY_A) == GLFW_PRESS) m_camera->ProcessKeyboard(LEFT, m_deltaTime);
    if (glfwGetKey(m_window, GLFW_KEY_D) == GLFW_PRESS) m_camera->ProcessKeyboard(RIGHT, m_deltaTime);
}