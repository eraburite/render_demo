#pragma once
#include "typedefs.h"
#include <memory>

class IUnifiedWindow;

class SwapChain {
public:
    SwapChain(std::shared_ptr<vk::Device> device, IUnifiedWindow &window, const QueueFamilyIndices &queueFamilyIndices,
              const SwapChainSupportDetails &wapChainSupport);
    ~SwapChain();

    std::shared_ptr<vk::Extent2D> getExtent2D() const
    {
        return m_swapChainExtent;
    }

    std::vector<std::shared_ptr<vk::ImageView>> getImageViews() const
    {
        return m_swapChainImageViews;
    }

    std::shared_ptr<vk::Format> getImageFormat() const
    {
        return m_swapChainImageFormat;
    }

    std::shared_ptr<vk::Framebuffer> getFramebuffer(uint32_t imageIndex) const
    {
        return m_swapChainFramebuffers[imageIndex];
    }

    void createFramebuffers(std::shared_ptr<vk::RenderPass> renderPass, std::shared_ptr<vk::ImageView> msaaImageView,
                            std::shared_ptr<vk::ImageView> depthImageView);
    vk::Result acquireNextImage(std::shared_ptr<vk::Semaphore> imageAvailableSemaphore, uint32_t &imageIndex);
    vk::Result queuePresent(std::shared_ptr<vk::Queue> queue, uint32_t imageIndex,
                            std::vector<vk::Semaphore> &waitSemaphore);

private:
    void create(int window_width, int window_height, const QueueFamilyIndices &queueFamilyIndices,
                const SwapChainSupportDetails &swapChainSupport);
    void createSwapChain(int window_width, int window_height, const QueueFamilyIndices &queueFamilyIndices,
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
    // std:: unordered   map< FramebufferKey, vk:: Framebuffer, KeyHash> m_ framebufferCache;//TODO hash cached
};