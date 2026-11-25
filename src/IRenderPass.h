#pragma once
#include <functional>
#include <memory>
#include <vulkan/vulkan.hpp>
#include "IAttachmentDetail.h"

class IRenderPass {
public:
    virtual ~IRenderPass() = default;
    virtual vk::RenderPass handle() const = 0;
    virtual vk::SampleCountFlagBits sampleCount() const = 0;
    virtual uint32_t colorAttachmentCount() const = 0;
    virtual bool isSwapChainSized() const = 0;

    virtual void createFramebuffers(const std::vector<vk::ImageView> &swapChainViews, vk::Extent2D swapChainExtent) = 0;
    virtual uint32_t framebufferCount() const = 0;
    virtual vk::Framebuffer framebuffer(uint32_t index) const = 0;
    virtual vk::Extent2D extent() const = 0;

    virtual void begin(const vk::CommandBuffer &cmd, uint32_t frameIndex, vk::Extent2D renderArea) = 0;
    // Like begin(), but lets the caller ask for secondary command buffer
    // execution in the first subpass.  Default keeps the legacy inline path.
    virtual void begin(const vk::CommandBuffer &cmd, uint32_t frameIndex, vk::Extent2D renderArea,
                       vk::SubpassContents contents)
    {
        (void)contents;
        begin(cmd, frameIndex, renderArea);
    }
    virtual void end(const vk::CommandBuffer &cmd) = 0;
    virtual void nextSubpass(const vk::CommandBuffer &cmd) = 0;

    // Offscreen color target image views (for collecting into descriptor sets).
    // Laid out frame-major: index = frameSlot * offscreenColorCountPerSet() + colorSlot.
    virtual const std::vector<vk::ImageView> &offscreenViews() const = 0;
    // Number of color views belonging to each frame slot (MRT > 1).
    virtual uint32_t offscreenColorCountPerSet() const = 0;

    // The same colour targets as images, index for index with offscreenViews().
    // Needed by the debug image dump (src/ImageReadback.h), which copies a
    // target to host memory and therefore needs the image, not the view.
    virtual const std::vector<vk::Image> &offscreenImages() const = 0;
    // Format of each colour slot, indexed by colorSlot (not frame-major).
    virtual vk::Format offscreenColorFormat(uint32_t colorSlot) const = 0;
};
