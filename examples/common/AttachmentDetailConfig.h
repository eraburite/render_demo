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
            // Placeholder: RenderManager::CreateRenderPass overwrites this with
            // the actual in-flight frame count so each frame gets its own target.
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
        // Placeholder; RenderManager::CreateRenderPass sets it to framesInFlight.
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

        // Alpha 0 marks "no geometry here".  mrt.frag always writes alpha 1, so the
        // deferred pass can use the alpha channel as a coverage mask and avoid
        // running normalize(vec3(0)) on cleared texels (which yields NaN).
        for (int i = 0; i < 3; ++i) {
            bp.clearValues.push_back(
                vk::ClearValue(vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f})));
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

// Deferred renderer expressed as three subpasses of a single render pass
// (reference: examples/subpasses).  Layout:
//
//   0  swapchain colour  cleared, written by every subpass, presented
//   1  position          RGBA32F, alpha carries linear view-space depth
//   2  normal            RGBA16F
//   3  albedo            RGBA8
//   4  depth
//
// Subpass 0 fills the G-Buffer, subpass 1 reads attachments 1..3 as input
// attachments and shades 64 point lights into attachment 0, subpass 2 blends
// transparent geometry over attachment 0 while testing depth against the depth
// produced by subpass 0.
//
// The three G-Buffer images are framebuffer attachments of a *swapchain-sized*
// pass, so they are recreated on resize; the views handed to input attachment
// descriptors therefore die with the framebuffer.  IRenderPass::
// attachmentGeneration() reports that, and the example refreshes the descriptors
// (GraphicsPipeline::RefreshDescriptors) once per generation.
class SubpassDeferredAttachmentDetail : public IAttachmentDetail {
public:
    SubpassDeferredAttachmentDetail(vk::Format swapFormat, vk::Format depthFormat) :
        m_swapFormat(swapFormat), m_depthFormat(depthFormat)
    {
    }

    bool isSwapChainSized() const override { return true; }

    RenderPassBlueprint build(uint32_t, uint32_t, vk::SampleCountFlagBits) const override
    {
        RenderPassBlueprint bp;
        bp.sampleCount = vk::SampleCountFlagBits::e1;
        bp.swapChainSized = true;
        // Subpass 0 writes four colour targets: swapchain + position + normal + albedo.
        bp.colorAttachmentCount = 4;

        vk::AttachmentDescription swap{};
        swap.format = m_swapFormat;
        swap.samples = vk::SampleCountFlagBits::e1;
        swap.loadOp = vk::AttachmentLoadOp::eClear;
        swap.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
        swap.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
        swap.storeOp = vk::AttachmentStoreOp::eStore;
        swap.initialLayout = vk::ImageLayout::eUndefined;
        swap.finalLayout = vk::ImageLayout::ePresentSrcKHR;
        bp.attachments.push_back(swap);

        // G-Buffer.  Every target is stored and left in SHADER_READ_ONLY: the
        // contents are consumed by the next subpass, so DONT_CARE would be a
        // spec-level lie even though desktop drivers happen to keep the data.
        const vk::Format gbufferFormats[3] = {vk::Format::eR32G32B32A32Sfloat,
                                             vk::Format::eR16G16B16A16Sfloat,
                                             vk::Format::eR8G8B8A8Unorm};
        for (uint32_t slot = 0; slot < 3; ++slot) {
            vk::AttachmentDescription target{};
            target.format = gbufferFormats[slot];
            target.samples = vk::SampleCountFlagBits::e1;
            target.loadOp = vk::AttachmentLoadOp::eClear;
            target.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
            target.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
            target.storeOp = vk::AttachmentStoreOp::eStore;
            target.initialLayout = vk::ImageLayout::eUndefined;
            target.finalLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
            bp.attachments.push_back(target);

            OffscreenTargetSpec spec;
            spec.attachmentIndex = slot + 1;
            spec.format = gbufferFormats[slot];
            // INPUT_ATTACHMENT is mandatory here; eSampled keeps the debug image
            // dump (plain texel copies) available for these targets.
            spec.extraUsage = vk::ImageUsageFlagBits::eInputAttachment | vk::ImageUsageFlagBits::eSampled;
            spec.fixedSize = false; // follows the swapchain extent
            bp.offscreenTargets.push_back(spec);
        }

        vk::AttachmentDescription depth{};
        depth.format = m_depthFormat;
        depth.samples = vk::SampleCountFlagBits::e1;
        depth.loadOp = vk::AttachmentLoadOp::eClear;
        depth.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
        depth.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
        // Only subpasses 0..2 of this very pass read depth, and initialLayout is
        // UNDEFINED, so the contents never need to survive the pass.
        depth.storeOp = vk::AttachmentStoreOp::eDontCare;
        depth.initialLayout = vk::ImageLayout::eUndefined;
        depth.finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
        bp.attachments.push_back(depth);

        // Subpass 0: fill the G-Buffer (reference: gbuffer.vert/frag).
        SubpassBlueprint gbuffer;
        for (uint32_t attachment = 0; attachment < 4; ++attachment) {
            gbuffer.colorAttachments.push_back(
                vk::AttachmentReference(attachment, vk::ImageLayout::eColorAttachmentOptimal));
        }
        gbuffer.depthStencil = vk::AttachmentReference(4, vk::ImageLayout::eDepthStencilAttachmentOptimal);
        bp.subpasses.push_back(gbuffer);

        // Subpass 1: fullscreen deferred composition, reading the G-Buffer.
        SubpassBlueprint composition;
        composition.colorAttachments.push_back(
            vk::AttachmentReference(0, vk::ImageLayout::eColorAttachmentOptimal));
        for (uint32_t attachment = 1; attachment <= 3; ++attachment) {
            composition.inputAttachments.push_back(
                vk::AttachmentReference(attachment, vk::ImageLayout::eShaderReadOnlyOptimal));
        }
        composition.depthStencil = vk::AttachmentReference(4, vk::ImageLayout::eDepthStencilAttachmentOptimal);
        bp.subpasses.push_back(composition);

        // Subpass 2: forward transparency, depth-tested against subpass 0.
        SubpassBlueprint transparent;
        transparent.colorAttachments.push_back(
            vk::AttachmentReference(0, vk::ImageLayout::eColorAttachmentOptimal));
        transparent.inputAttachments.push_back(
            vk::AttachmentReference(1, vk::ImageLayout::eShaderReadOnlyOptimal));
        transparent.depthStencil = vk::AttachmentReference(4, vk::ImageLayout::eDepthStencilAttachmentOptimal);
        bp.subpasses.push_back(transparent);

        // Dependencies.  Unlike a single-subpass pass, subpasses do not get any
        // implicit ordering between each other, so every consumer needs an
        // explicit dependency covering both the colour/input read and the depth
        // test that reads subpass 0's depth writes.
        const auto attachmentWrite = vk::AccessFlagBits::eColorAttachmentWrite
                                     | vk::AccessFlagBits::eDepthStencilAttachmentWrite;

        // Previous frame's writes (the G-Buffer images are shared by every
        // swapchain framebuffer) -> this frame's clears and stores.
        bp.dependencies.push_back(vk::SubpassDependency(
            VK_SUBPASS_EXTERNAL, 0,
            vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eLateFragmentTests,
            vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests,
            attachmentWrite | vk::AccessFlagBits::eColorAttachmentRead,
            attachmentWrite | vk::AccessFlagBits::eColorAttachmentRead
                | vk::AccessFlagBits::eDepthStencilAttachmentRead,
            vk::DependencyFlagBits::eByRegion));

        // 0 -> 1 and 1 -> 2: colour/depth writes and the layout transition to
        // SHADER_READ_ONLY must finish before the next subpass reads them.  The
        // transition itself is a *write* performed in the destination scope of
        // this dependency, hence eColorAttachmentWrite / eColorAttachmentOutput
        // on the destination side as well.
        for (uint32_t src = 0; src < 2; ++src) {
            bp.dependencies.push_back(vk::SubpassDependency(
                src, src + 1,
                vk::PipelineStageFlagBits::eColorAttachmentOutput
                    | vk::PipelineStageFlagBits::eLateFragmentTests,
                vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eFragmentShader
                    | vk::PipelineStageFlagBits::eEarlyFragmentTests,
                attachmentWrite,
                vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eColorAttachmentRead
                    | vk::AccessFlagBits::eInputAttachmentRead | vk::AccessFlagBits::eShaderRead
                    | vk::AccessFlagBits::eDepthStencilAttachmentRead
                    | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
                vk::DependencyFlagBits::eByRegion));
        }

        // Release the final subpass' attachment writes towards presentation and
        // towards the end-of-pass layout transitions (again: those transitions
        // are writes in the destination scope).
        bp.dependencies.push_back(vk::SubpassDependency(
            2, VK_SUBPASS_EXTERNAL,
            vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eLateFragmentTests,
            vk::PipelineStageFlagBits::eBottomOfPipe | vk::PipelineStageFlagBits::eColorAttachmentOutput
                | vk::PipelineStageFlagBits::eLateFragmentTests,
            vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eColorAttachmentRead
                | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
            vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eColorAttachmentWrite
                | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
            vk::DependencyFlagBits::eByRegion));

        for (int i = 0; i < 4; ++i) {
            bp.clearValues.push_back(
                vk::ClearValue(vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f})));
        }
        bp.clearValues.push_back(vk::ClearValue(vk::ClearDepthStencilValue(1.0f, 0)));
        return bp;
    }

private:
    vk::Format m_swapFormat;
    vk::Format m_depthFormat;
};
