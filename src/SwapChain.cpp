#include "SwapChain.h"
#include "IUnifiedWindow.h"
#include "DeviceResourceUtils.h"

SwapChain::SwapChain(std::shared_ptr<vk::Device> device, IUnifiedWindow &window,
                     const QueueFamilyIndices &queueFamilyIndices, const SwapChainSupportDetails &swapChainSupport) :
    m_device(device)
{
    m_surface = window.getSurface();
    int width, height, depth;
    std::tie(width, height, depth) = window.getFramebufferSize();
    // std:: cout << " create swapchain width: " << width << ", height" << height << std:: endl;
    create(width, height, queueFamilyIndices, swapChainSupport);
}

SwapChain::~SwapChain() {}

void SwapChain::create(int window_width, int window_height, const QueueFamilyIndices &queueFamilyIndices,
                       const SwapChainSupportDetails &swapChainSupport)
{
    createSwapChain(window_width, window_height, queueFamilyIndices, swapChainSupport);
    createImageViews();
}

void SwapChain::createSwapChain(int window_width, int window_height, const QueueFamilyIndices &indices,
                                const SwapChainSupportDetails &swapChainSupport)
{
    vk::SurfaceFormatKHR surfaceFormat = swapChainSupport.chooseSwapSurfaceFormat();
    vk::PresentModeKHR presentMode = swapChainSupport.chooseSwapPresentMode();
    vk::Extent2D extent = swapChainSupport.chooseSwapExtent(window_width, window_height);
    uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
    if (swapChainSupport.capabilities.maxImageCount > 0 && imageCount > swapChainSupport.capabilities.maxImageCount) {
        imageCount = swapChainSupport.capabilities.maxImageCount;
    }

    vk::SwapchainCreateInfoKHR createInfo{};
    createInfo.surface = *m_surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;

    uint32_t queueFamilyIndices[] = {indices.graphicsFamily.value(), indices.presentFamily.value()};
    if (indices.graphicsFamily != indices.presentFamily) {
        createInfo.imageSharingMode = vk::SharingMode::eConcurrent;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode = vk::SharingMode::eExclusive;
    }

    createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
    createInfo.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;

    vk::SwapchainKHR swapChain;
    if (m_device->createSwapchainKHR(&createInfo, nullptr, &swapChain) != vk::Result::eSuccess) {
        throw std::runtime_error(" failed to create swap chain!");
    }

    m_swapChainKHR =
        std::shared_ptr<vk::SwapchainKHR>(new vk::SwapchainKHR(swapChain), [device = m_device](vk::SwapchainKHR *p) {
            if (p && *p) {
                device->destroySwapchainKHR(*p, nullptr);
            }
            delete p;
        });

    auto ret = m_device->getSwapchainImagesKHR(*m_swapChainKHR, &imageCount, nullptr);
    m_swapChainImages.resize(imageCount);
    // std:: cout << "swapChainImages imageCount: " << imageCount << "\n";
    ret = m_device->getSwapchainImagesKHR(*m_swapChainKHR, &imageCount, m_swapChainImages.data());
    m_swapChainImageFormat = std::make_shared<vk::Format>(surfaceFormat.format);
    m_swapChainExtent = std::make_shared<vk::Extent2D>(extent);
}

void SwapChain::createImageViews()
{
    m_swapChainImageViews.resize(m_swapChainImages.size());
    for (size_t i = 0; i < m_swapChainImages.size(); i++) {
        m_swapChainImageViews[i] = DeviceResourceUtils::createImageView(
            m_device, m_swapChainImages[i], *m_swapChainImageFormat, 1, vk::ImageAspectFlagBits::eColor);
    }
}

void SwapChain::createFramebuffers(std::shared_ptr<vk::RenderPass> renderPass,
                                   std::shared_ptr<vk::ImageView> massImageView,
                                   std::shared_ptr<vk::ImageView> depthImageView)
{
    auto swapChainExtent = getExtent2D();
    auto swapChainImageViews = getImageViews();
    m_swapChainFramebuffers.resize(swapChainImageViews.size());
    for (size_t i = 0; i < swapChainImageViews.size(); i++) {
        std::array<vk::ImageView, 3> attachments = {
            *massImageView,         // 对应 Index 0 (renderPass colorAttachment)
            *depthImageView,        // 对应 Index 1 (renderPass depthAttachment)
            *swapChainImageViews[i] // 对应 Index 2 (renderPass colorAttachmentResolve)
        };

        vk::FramebufferCreateInfo framebufferInfo{};
        framebufferInfo.renderPass = *renderPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = swapChainExtent->width;
        framebufferInfo.height = swapChainExtent->height;
        framebufferInfo.layers = 1;
        vk::Framebuffer swapChainFramebuffer{};
        if (m_device->createFramebuffer(&framebufferInfo, nullptr, &swapChainFramebuffer) != vk::Result::eSuccess) {
            throw std::runtime_error(" failed to create framebuffer!");
        }

        m_swapChainFramebuffers[i] = std::shared_ptr<vk::Framebuffer>(
            new vk::Framebuffer(swapChainFramebuffer),
            [renderPass, device = m_device, swapChainImageViews, swapChain = m_swapChainKHR](vk::Framebuffer *p) {
                if (p && *p) {
                    device->destroyFramebuffer(*p, nullptr);
                }
                delete p;
            });
    }
}

vk::Result SwapChain::acquireNextImage(std::shared_ptr<vk::Semaphore> imageAvailableSemaphore, uint32_t &imageIndex)
{
    // std:: cout << " before acquireNextImage\n";
    auto start = std::chrono::steady_clock::now();
    auto result = m_device->acquireNextImageKHR(*m_swapChainKHR, UINT64_MAX, *imageAvailableSemaphore, VK_NULL_HANDLE,
                                                &imageIndex);
    auto end = std::chrono::steady_clock::now();
    const std::chrono::duration<double, std::milli> diff = end - start;
    // std:: cout <<" after acquireNextImage cost:" << diff. count() << " ms, imageIndex:  " << imageIndex <<
    // "\n";
    return result;
}

vk::Result SwapChain::queuePresent(std::shared_ptr<vk::Queue> queue, uint32_t imageIndex,
                                   std::vector<vk::Semaphore> &waitSemaphore)
{
    vk::PresentInfoKHR presentInfo{};
    presentInfo.waitSemaphoreCount = waitSemaphore.size();
    presentInfo.pWaitSemaphores = waitSemaphore.data();
    vk::SwapchainKHR swapChains[] = {*m_swapChainKHR};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;
    presentInfo.pImageIndices = &imageIndex;
    // std:: cout << " before presentKHR\n";
    auto start = std::chrono::steady_clock::now();
    auto result = queue->presentKHR(&presentInfo);
    auto end = std::chrono::steady_clock::now();
    const std::chrono::duration<double, std::milli> diff = end - start;
    // std:: cout <<" after presentKHR cost: " << diff. count() << " ms, result:  " << result <<"\n";
    return result;
}