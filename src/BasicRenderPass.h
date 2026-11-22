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
    bool isSwapChainSized() const override { return m_blueprint.swapChainSized; }

    void createFramebuffers(const std::vector<vk::ImageView> &swapChainViews, vk::Extent2D swapChainExtent) override;
    uint32_t framebufferCount() const override { return static_cast<uint32_t>(m_framebuffers.size()); }
    vk::Framebuffer framebuffer(uint32_t index) const override
    {
        return index < m_framebuffers.size() ? *m_framebuffers[index] : vk::Framebuffer{};
    }
    vk::Extent2D extent() const override { return m_extent; }

    void begin(const vk::CommandBuffer &cmd, uint32_t frameIndex, vk::Extent2D renderArea) override;
    void end(const vk::CommandBuffer &cmd) override;
    void nextSubpass(const vk::CommandBuffer &cmd) override;

    const std::vector<vk::ImageView> &offscreenViews() const override { return m_offscreenViews; }
    uint32_t offscreenColorCountPerSet() const override { return m_colorCountPerSet; }

private:
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
    uint32_t m_colorCountPerSet = 0;
    std::vector<VulkanHandle<vk::Framebuffer, std::function<void(vk::Framebuffer)>>> m_framebuffers;
};
