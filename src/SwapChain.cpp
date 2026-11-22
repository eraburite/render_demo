#include "SwapChain.h"

#include "DeviceResourceUtils.h"
#include "IUnifiedWindow.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>

namespace {
void check(vk::Result result, const char *what)
{
    if (result != vk::Result::eSuccess) {
        throw std::runtime_error(std::string(what) + " failed: " + vk::to_string(result));
    }
}

bool isPresentResult(vk::Result result)
{
    return result == vk::Result::eSuccess || result == vk::Result::eSuboptimalKHR;
}
} // namespace

SwapChain::SwapChain(std::shared_ptr<vk::Device> device, IUnifiedWindow &window,
                     const QueueFamilyIndices &queueFamilyIndices, const SwapChainSupportDetails &swapChainSupport,
                     const std::shared_ptr<vk::SurfaceKHR> &surface) :
    m_device(std::move(device)), m_surface(surface)
{
    if (!m_device || !m_surface) {
        throw std::runtime_error("swapchain requires a device and a surface!");
    }
    int width = 0;
    int height = 0;
    int depth = 0;
    std::tie(width, height, depth) = window.getFramebufferSize();
    create(width, height, queueFamilyIndices, swapChainSupport);
}

SwapChain::~SwapChain() = default;

void SwapChain::create(int windowWidth, int windowHeight, const QueueFamilyIndices &queueFamilyIndices,
                       const SwapChainSupportDetails &swapChainSupport)
{
    createSwapChain(windowWidth, windowHeight, queueFamilyIndices, swapChainSupport);
    createImageViews();
}

void SwapChain::createSwapChain(int windowWidth, int windowHeight, const QueueFamilyIndices &indices,
                                const SwapChainSupportDetails &swapChainSupport)
{
    const vk::SurfaceFormatKHR surfaceFormat = swapChainSupport.chooseSwapSurfaceFormat();
    const vk::PresentModeKHR presentMode = swapChainSupport.chooseSwapPresentMode();
    const vk::Extent2D extent = swapChainSupport.chooseSwapExtent(windowWidth, windowHeight);

    uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
    if (swapChainSupport.capabilities.maxImageCount > 0
        && imageCount > swapChainSupport.capabilities.maxImageCount) {
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

    const uint32_t queueFamilies[] = {indices.graphicsFamily.value(), indices.presentFamily.value()};
    if (indices.graphicsFamily != indices.presentFamily) {
        createInfo.imageSharingMode = vk::SharingMode::eConcurrent;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilies;
    } else {
        createInfo.imageSharingMode = vk::SharingMode::eExclusive;
    }

    createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
    createInfo.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    createInfo.oldSwapchain = nullptr;

    vk::SwapchainKHR swapchain{};
    check(m_device->createSwapchainKHR(&createInfo, nullptr, &swapchain), "vkCreateSwapchainKHR");

    m_swapChainKHR = std::shared_ptr<vk::SwapchainKHR>(new vk::SwapchainKHR(swapchain),
                                                       [device = m_device](vk::SwapchainKHR *swapchainPtr) {
                                                           if (swapchainPtr != nullptr && *swapchainPtr) {
                                                               device->destroySwapchainKHR(*swapchainPtr, nullptr);
                                                           }
                                                           delete swapchainPtr;
                                                       });

    check(m_device->getSwapchainImagesKHR(*m_swapChainKHR, &imageCount, nullptr), "vkGetSwapchainImagesKHR (count)");
    m_swapChainImages.resize(imageCount);
    check(m_device->getSwapchainImagesKHR(*m_swapChainKHR, &imageCount, m_swapChainImages.data()),
          "vkGetSwapchainImagesKHR");

    m_swapChainImageFormat = std::make_shared<vk::Format>(surfaceFormat.format);
    m_swapChainExtent = std::make_shared<vk::Extent2D>(extent);
}

void SwapChain::createImageViews()
{
    m_swapChainImageViews.resize(m_swapChainImages.size());
    for (size_t i = 0; i < m_swapChainImages.size(); ++i) {
        m_swapChainImageViews[i] = DeviceResourceUtils::createImageView(
            m_device, m_swapChainImages[i], *m_swapChainImageFormat, vk::ImageAspectFlagBits::eColor, 1);
    }
}

void SwapChain::createFramebuffers(vk::RenderPass renderPass,
                                   const std::vector<std::vector<vk::ImageView>> &attachmentsPerFrame)
{
    if (!renderPass || !m_swapChainExtent) {
        throw std::runtime_error("cannot create swapchain framebuffers without a render pass and extent!");
    }
    if (attachmentsPerFrame.size() != m_swapChainImageViews.size()) {
        throw std::runtime_error("framebuffer attachment list must have one entry per swapchain image!");
    }

    m_swapChainFramebuffers.clear();
    m_swapChainFramebuffers.reserve(attachmentsPerFrame.size());
    for (size_t i = 0; i < attachmentsPerFrame.size(); ++i) {
        vk::FramebufferCreateInfo framebufferInfo{};
        framebufferInfo.renderPass = renderPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachmentsPerFrame[i].size());
        framebufferInfo.pAttachments = attachmentsPerFrame[i].data();
        framebufferInfo.width = m_swapChainExtent->width;
        framebufferInfo.height = m_swapChainExtent->height;
        framebufferInfo.layers = 1;

        vk::Framebuffer framebuffer{};
        check(m_device->createFramebuffer(&framebufferInfo, nullptr, &framebuffer), "vkCreateFramebuffer");

        m_swapChainFramebuffers.emplace_back(new vk::Framebuffer(framebuffer),
                                             [device = m_device, swapchain = m_swapChainKHR,
                                              views = m_swapChainImageViews](vk::Framebuffer *framebufferPtr) {
                                                 if (framebufferPtr != nullptr && *framebufferPtr) {
                                                     device->destroyFramebuffer(*framebufferPtr, nullptr);
                                                 }
                                                 delete framebufferPtr;
                                             });
    }
}

void SwapChain::createFramebuffers(vk::RenderPass renderPass, const std::vector<vk::ImageView> &commonAttachments)
{
    std::vector<std::vector<vk::ImageView>> attachmentsPerFrame;
    attachmentsPerFrame.reserve(m_swapChainImageViews.size());
    for (const std::shared_ptr<vk::ImageView> &view : m_swapChainImageViews) {
        std::vector<vk::ImageView> attachments = commonAttachments;
        attachments.push_back(view ? *view : vk::ImageView{});
        attachmentsPerFrame.push_back(std::move(attachments));
    }
    createFramebuffers(renderPass, attachmentsPerFrame);
}

vk::Result SwapChain::acquireNextImage(const std::shared_ptr<vk::Semaphore> &imageAvailableSemaphore,
                                       uint32_t &imageIndex)
{
    if (!imageAvailableSemaphore) {
        throw std::runtime_error("acquireNextImage requires a semaphore!");
    }

    // The out-parameter overload returns vk::Result without throwing, so a
    // suboptimal swapchain can be reported to the caller and trigger a rebuild.
    const vk::Result result = m_device->acquireNextImageKHR(*m_swapChainKHR,
                                                            std::numeric_limits<uint64_t>::max(),
                                                            *imageAvailableSemaphore, nullptr, &imageIndex);
    if (!isPresentResult(result)) {
        throw std::runtime_error(std::string("vkAcquireNextImageKHR failed: ") + vk::to_string(result));
    }
    return result;
}

vk::Result SwapChain::queuePresent(const std::shared_ptr<vk::Queue> &queue, uint32_t imageIndex,
                                   std::vector<vk::Semaphore> &waitSemaphores)
{
    if (!queue) {
        throw std::runtime_error("queuePresent requires a queue!");
    }

    vk::PresentInfoKHR presentInfo{};
    presentInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
    presentInfo.pWaitSemaphores = waitSemaphores.data();
    const vk::SwapchainKHR swapchains[] = {*m_swapChainKHR};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapchains;
    presentInfo.pImageIndices = &imageIndex;

    const vk::Result result = queue->presentKHR(&presentInfo);
    if (!isPresentResult(result)) {
        throw std::runtime_error(std::string("vkQueuePresentKHR failed: ") + vk::to_string(result));
    }
    return result;
}
