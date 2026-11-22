#pragma once
#include <algorithm>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "IAttachmentDetail.h"

// Simple single-pass attachment config (color [+resolve] + depth).
class SimpleAttachmentDetail : public IAttachmentDetail {
public:
    enum class Mode { eDirect, eMSAA, eOffscreen };

    SimpleAttachmentDetail(Mode mode, vk::Format colorFormat, vk::Format depthFormat,
                           vk::ImageLayout finalColorLayout = vk::ImageLayout::ePresentSrcKHR,
                           DependencyPreset preset = DependencyPreset::eSwapChain) :
        m_mode(mode), m_colorFormat(colorFormat), m_depthFormat(depthFormat),
        m_finalColorLayout(finalColorLayout), m_preset(preset)
    {
    }

    bool isSwapChainSized() const override { return m_mode != Mode::eOffscreen; }

    RenderPassBlueprint build(uint32_t width, uint32_t height, vk::SampleCountFlagBits msaa) const override
    {
        RenderPassBlueprint bp;
        bp.sampleCount = (m_mode == Mode::eMSAA) ? msaa : vk::SampleCountFlagBits::e1;
        bp.swapChainSized = isSwapChainSized();
        bp.colorAttachmentCount = 1;

        // eMultiSubpass turns the same color+depth attachments into two
        // subpasses: the second one (e.g. light cubes) reuses the results of
        // the first (e.g. lit scene) without leaving the render pass.
        const bool multiSubpass = (m_preset == DependencyPreset::eMultiSubpass);
        // A 1x device cannot resolve; fall back to a plain single-sample pass.
        const bool useResolve = (m_mode == Mode::eMSAA) && (bp.sampleCount != vk::SampleCountFlagBits::e1);

        // color attachment (attachment 0)
        vk::AttachmentDescription color{};
        color.format = m_colorFormat;
        color.samples = bp.sampleCount;
        color.loadOp = vk::AttachmentLoadOp::eClear;
        color.storeOp = (useResolve && !multiSubpass) ? vk::AttachmentStoreOp::eDontCare
                                                      : vk::AttachmentStoreOp::eStore;
        color.initialLayout = vk::ImageLayout::eUndefined;
        color.finalLayout = useResolve ? vk::ImageLayout::eColorAttachmentOptimal : m_finalColorLayout;
        bp.attachments.push_back(color);

        // depth attachment (attachment 1)
        if (m_depthFormat != vk::Format::eUndefined) {
            vk::AttachmentDescription depth{};
            depth.format = m_depthFormat;
            depth.samples = bp.sampleCount;
            depth.loadOp = vk::AttachmentLoadOp::eClear;
            // The second subpass performs depth tests against subpass 0's writes.
            depth.storeOp = multiSubpass ? vk::AttachmentStoreOp::eStore : vk::AttachmentStoreOp::eDontCare;
            // Vulkan-Hpp defaults stencilLoadOp to eLoad, which is invalid when
            // the format carries stencil and initialLayout is UNDEFINED.
            depth.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
            depth.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
            depth.initialLayout = vk::ImageLayout::eUndefined;
            depth.finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
            bp.attachments.push_back(depth);
        }

        auto makeSubpass = [&]() {
            SubpassBlueprint subpass;
            subpass.colorAttachments.push_back(vk::AttachmentReference(0, vk::ImageLayout::eColorAttachmentOptimal));
            if (m_depthFormat != vk::Format::eUndefined) {
                subpass.depthStencil = vk::AttachmentReference(1, vk::ImageLayout::eDepthStencilAttachmentOptimal);
            }
            return subpass;
        };

        SubpassBlueprint subpass = makeSubpass();

        if (useResolve) {
            // resolve attachment (attachment 2)
            vk::AttachmentDescription resolve{};
            resolve.format = m_colorFormat;
            resolve.samples = vk::SampleCountFlagBits::e1;
            resolve.loadOp = vk::AttachmentLoadOp::eDontCare;
            resolve.storeOp = vk::AttachmentStoreOp::eStore;
            resolve.initialLayout = vk::ImageLayout::eUndefined;
            resolve.finalLayout = m_finalColorLayout;
            bp.attachments.push_back(resolve);
            // Resolve must happen once, in the subpass that finishes the image.
            if (!multiSubpass) {
                subpass.resolveAttachments.push_back(
                    vk::AttachmentReference(2, vk::ImageLayout::eColorAttachmentOptimal));
            }
        }

        bp.subpasses.push_back(subpass);
        if (multiSubpass) {
            SubpassBlueprint second = makeSubpass();
            if (useResolve) {
                second.resolveAttachments.push_back(
                    vk::AttachmentReference(2, vk::ImageLayout::eColorAttachmentOptimal));
            }
            bp.subpasses.push_back(second);
        }
        bp.dependencies = makeDependencies(m_preset);

        bp.clearValues.push_back(vk::ClearValue(vk::ClearColorValue(std::array<float, 4>{0.05f, 0.06f, 0.09f, 1.0f})));
        if (m_depthFormat != vk::Format::eUndefined) {
            bp.clearValues.push_back(vk::ClearValue(vk::ClearDepthStencilValue(1.0f, 0)));
        }
        if (useResolve) {
            bp.clearValues.push_back(vk::ClearValue(vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f})));
        }

        if (m_mode == Mode::eOffscreen) {
            bp.swapChainSized = false;
            bp.framebufferCount = 2;
            OffscreenTargetSpec target;
            target.attachmentIndex = 0;
            target.format = m_colorFormat;
            target.fixedSize = true;
            target.width = width;
            target.height = height;
            bp.offscreenTargets.push_back(target);
        }
        return bp;
    }

private:
    Mode m_mode;
    vk::Format m_colorFormat;
    vk::Format m_depthFormat;
    vk::ImageLayout m_finalColorLayout;
    DependencyPreset m_preset;
};

// G-Buffer: position + normal + albedo (MRT) + depth, all sampled afterwards.
class GBufferAttachmentDetail : public IAttachmentDetail {
public:
    GBufferAttachmentDetail(vk::Format swapFormat, vk::Format depthFormat, uint32_t width, uint32_t height) :
        m_swapFormat(swapFormat), m_depthFormat(depthFormat), m_width(width), m_height(height)
    {
    }

    bool isSwapChainSized() const override { return false; }

    RenderPassBlueprint build(uint32_t, uint32_t, vk::SampleCountFlagBits) const override
    {
        RenderPassBlueprint bp;
        bp.sampleCount = vk::SampleCountFlagBits::e1;
        bp.swapChainSized = false;
        bp.colorAttachmentCount = 3;
        bp.framebufferCount = 2;

        const vk::Format formats[] = {vk::Format::eR32G32B32A32Sfloat, vk::Format::eR32G32B32A32Sfloat,
                                      vk::Format::eR8G8B8A8Unorm};
        for (int i = 0; i < 3; ++i) {
            vk::AttachmentDescription att{};
            att.format = formats[i];
            att.samples = vk::SampleCountFlagBits::e1;
            att.loadOp = vk::AttachmentLoadOp::eClear;
            att.storeOp = vk::AttachmentStoreOp::eStore;
            att.initialLayout = vk::ImageLayout::eUndefined;
            att.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
            bp.attachments.push_back(att);
            OffscreenTargetSpec target;
            target.attachmentIndex = static_cast<uint32_t>(i);
            target.format = formats[i];
            target.fixedSize = true;
            target.width = m_width;
            target.height = m_height;
            bp.offscreenTargets.push_back(target);
        }

        vk::AttachmentDescription depth{};
        depth.format = m_depthFormat;
        depth.samples = vk::SampleCountFlagBits::e1;
        depth.loadOp = vk::AttachmentLoadOp::eClear;
        depth.storeOp = vk::AttachmentStoreOp::eDontCare;
        depth.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
        depth.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
        depth.initialLayout = vk::ImageLayout::eUndefined;
        depth.finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
        bp.attachments.push_back(depth);

        SubpassBlueprint subpass;
        for (uint32_t i = 0; i < 3; ++i) {
            subpass.colorAttachments.push_back(vk::AttachmentReference(i, vk::ImageLayout::eColorAttachmentOptimal));
        }
        subpass.depthStencil = vk::AttachmentReference(3, vk::ImageLayout::eDepthStencilAttachmentOptimal);
        bp.subpasses.push_back(subpass);
        bp.dependencies = makeDependencies(DependencyPreset::eOffscreen);

        for (int i = 0; i < 3; ++i) {
            bp.clearValues.push_back(
                vk::ClearValue(vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f})));
        }
        bp.clearValues.push_back(vk::ClearValue(vk::ClearDepthStencilValue(1.0f, 0)));
        return bp;
    }

private:
    vk::Format m_swapFormat;
    vk::Format m_depthFormat;
    uint32_t m_width;
    uint32_t m_height;
};
