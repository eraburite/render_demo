#pragma once
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "IRenderPass.h"
#include "VulkanHandle.h"

// The single IRenderPass implementation.  Framebuffers are created from a
// RenderPassBlueprint; swapchain-sized passes rebuild on resize, offscreen
// passes keep fixed size.
class BasicRenderPass : public IRenderPass {
public:
    BasicRenderPass(std::shared_ptr<vk::Device> device, const vk::PhysicalDeviceMemoryProperties &memProps,
                    std::shared_ptr<DeviceMemoryAllocator> allocator, const RenderPassBlueprint &blueprint);
    ~BasicRenderPass() = default;

    vk::RenderPass handle() const override { return m_renderPass ? *m_renderPass : vk::RenderPass{}; }
    vk::SampleCountFlagBits sampleCount() const override { return m_blueprint.sampleCount; }
    uint32_t colorAttachmentCount() const override { return m_blueprint.colorAttachmentCount; }
    uint32_t colorAttachmentCount(uint32_t subpass) const override
    {
        return subpass < m_subpassColorCounts.size() ? m_subpassColorCounts[subpass]
                                                     : m_blueprint.colorAttachmentCount;
    }
    bool isSwapChainSized() const override { return m_blueprint.swapChainSized; }
    uint64_t attachmentGeneration() const override { return m_attachmentGeneration; }
    bool hasStencil() const override { return m_blueprint.stencil.has_value(); }
    // Depth-stencil image of one attachment set (set 0 for swapchain-sized
    // passes).  Debug captures read its S8 aspect to prove the stencil writes.
    vk::Image depthStencilImage(uint32_t set = 0) const override
    {
        return set < m_depthStencilImages.size() ? m_depthStencilImages[set] : vk::Image{};
    }
    // Array view of the same image (one view over every layer).  Sampling a
    // depth array as a texture needs the view, not the image, and it has to be
    // the *same* view the framebuffer was built from -- a second view of the
    // same image would be a fresh subresource range that the render pass' final
    // layout transition does not cover.
    vk::ImageView depthStencilView(uint32_t set = 0) const override
    {
        return set < m_depthStencilViews.size() ? m_depthStencilViews[set] : vk::ImageView{};
    }
    uint32_t depthStencilLayers() const override { return m_blueprint.layers; }
    vk::Format depthStencilFormat() const override { return m_depthStencilFormat; }

    void createFramebuffers(const std::vector<vk::ImageView> &swapChainViews, vk::Extent2D swapChainExtent) override;
    uint32_t framebufferCount() const override { return static_cast<uint32_t>(m_framebuffers.size()); }
    vk::Framebuffer framebuffer(uint32_t index) const override
    {
        return index < m_framebuffers.size() ? *m_framebuffers[index] : vk::Framebuffer{};
    }
    vk::Extent2D extent() const override { return m_extent; }

    void begin(const vk::CommandBuffer &cmd, uint32_t frameIndex, vk::Extent2D renderArea) override;
    void begin(const vk::CommandBuffer &cmd, uint32_t frameIndex, vk::Extent2D renderArea,
               vk::SubpassContents contents) override;
    void end(const vk::CommandBuffer &cmd) override;
    void nextSubpass(const vk::CommandBuffer &cmd) override;

    const std::vector<vk::ImageView> &offscreenViews() const override { return m_offscreenViews; }
    uint32_t offscreenColorCountPerSet() const override { return m_colorCountPerSet; }
    const std::vector<vk::Image> &offscreenImages() const override { return m_offscreenImages; }
    vk::Format offscreenColorFormat(uint32_t colorSlot) const override
    {
        return colorSlot < m_offscreenFormats.size() ? m_offscreenFormats[colorSlot] : vk::Format::eUndefined;
    }

private:
    // Applies RenderPassBlueprint::stencil to the attachment description and
    // clear values *before* createRenderPass() consumes them.  Also rejects a
    // depth-only format, which would otherwise make stencil writes a no-op.
    void applyStencilSpec();
    void createRenderPass();
    void createAttachments(vk::Extent2D extent);

private:
    std::shared_ptr<vk::Device> m_device;
    vk::PhysicalDeviceMemoryProperties m_memProps;
    std::shared_ptr<DeviceMemoryAllocator> m_allocator;
    RenderPassBlueprint m_blueprint;
    vk::Extent2D m_extent{1, 1};

    VulkanHandle<vk::RenderPass, std::function<void(vk::RenderPass)>> m_renderPass;
    // Offscreen passes allocate one independent set of attachment images per
    // frame slot; swapchain passes share a single set across swapchain views.
    // Indexed as [setIndex][attachmentIndex].
    std::vector<std::vector<std::shared_ptr<ImageAttachment>>> m_attachmentSets;
    // Color image views exposed for sampling, laid out frame-major:
    // [setIndex * colorCountPerSet + colorSlot].
    std::vector<vk::ImageView> m_offscreenViews;
    // Same targets as images (index for index with m_offscreenViews) plus their
    // per-slot formats; used by the debug image dump.
    std::vector<vk::Image> m_offscreenImages;
    std::vector<vk::Format> m_offscreenFormats;
    // Depth-stencil attachment of each set, kept for the stencil debug capture
    // and for sampling a depth array (cascaded shadow maps).
    std::vector<vk::Image> m_depthStencilImages;
    std::vector<vk::ImageView> m_depthStencilViews;
    vk::Format m_depthStencilFormat = vk::Format::eUndefined;
    uint32_t m_colorCountPerSet = 0;
    // Colour attachment count of each subpass, so a pipeline can be created for
    // a subpass that does not write every attachment the pass owns.
    std::vector<uint32_t> m_subpassColorCounts;
    // Incremented every time createAttachments() replaces the images/views.
    uint64_t m_attachmentGeneration = 0;
    std::vector<VulkanHandle<vk::Framebuffer, std::function<void(vk::Framebuffer)>>> m_framebuffers;
};
