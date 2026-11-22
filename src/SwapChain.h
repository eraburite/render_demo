#pragma once
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "typedefs.h"

class IUnifiedWindow;

// Owns the swapchain, its images and image views.  Framebuffer construction is
// kept generic: the render pass decides the attachment set.
class SwapChain {
public:
    SwapChain(std::shared_ptr<vk::Device> device, IUnifiedWindow &window, const QueueFamilyIndices &queueFamilyIndices,
              const SwapChainSupportDetails &swapChainSupport, const std::shared_ptr<vk::SurfaceKHR> &surface);
    ~SwapChain();

    const std::shared_ptr<vk::Extent2D> &getExtent2D() const { return m_swapChainExtent; }
    const std::vector<std::shared_ptr<vk::ImageView>> &getImageViews() const { return m_swapChainImageViews; }
    const std::shared_ptr<vk::Format> &getImageFormat() const { return m_swapChainImageFormat; }
    std::shared_ptr<vk::Framebuffer> getFramebuffer(uint32_t imageIndex) const
    {
        return imageIndex < m_swapChainFramebuffers.size() ? m_swapChainFramebuffers[imageIndex] : nullptr;
    }
    vk::SwapchainKHR handle() const { return m_swapChainKHR ? *m_swapChainKHR : vk::SwapchainKHR{}; }

    // Build one framebuffer per swapchain image.  Each entry in
    // attachmentsPerFrame is the complete attachment list for that image
    // (including the swapchain image view).
    void createFramebuffers(vk::RenderPass renderPass,
                            const std::vector<std::vector<vk::ImageView>> &attachmentsPerFrame);
    // Convenience overload: commonAttachments are followed by the matching
    // swapchain image view for each frame.
    void createFramebuffers(vk::RenderPass renderPass, const std::vector<vk::ImageView> &commonAttachments);

    vk::Result acquireNextImage(const std::shared_ptr<vk::Semaphore> &imageAvailableSemaphore, uint32_t &imageIndex);
    vk::Result queuePresent(const std::shared_ptr<vk::Queue> &queue, uint32_t imageIndex,
                            std::vector<vk::Semaphore> &waitSemaphores);

private:
    void create(int windowWidth, int windowHeight, const QueueFamilyIndices &queueFamilyIndices,
                const SwapChainSupportDetails &swapChainSupport);
    void createSwapChain(int windowWidth, int windowHeight, const QueueFamilyIndices &queueFamilyIndices,
                         const SwapChainSupportDetails &swapChainSupport);
    void createImageViews();

private:
    std::shared_ptr<vk::Device> m_device;
    std::shared_ptr<vk::SurfaceKHR> m_surface;
    std::shared_ptr<vk::SwapchainKHR> m_swapChainKHR;
    std::vector<vk::Image> m_swapChainImages;
    std::shared_ptr<vk::Format> m_swapChainImageFormat;
    std::shared_ptr<vk::Extent2D> m_swapChainExtent;
    std::vector<std::shared_ptr<vk::ImageView>> m_swapChainImageViews;
    std::vector<std::shared_ptr<vk::Framebuffer>> m_swapChainFramebuffers;
};
