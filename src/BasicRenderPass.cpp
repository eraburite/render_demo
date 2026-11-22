// BasicRenderPass.h holds shared_ptr<DeviceMemoryAllocator>/<ImageAttachment>
// members; provide complete declarations before it is parsed.
#include "DeviceMemoryAllocator.h"
#include "ImageAttachment.h"
#include "BasicRenderPass.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(vk::Result result, const char *what)
{
    if (result != vk::Result::eSuccess) {
        throw std::runtime_error(std::string(what) + " failed: " + vk::to_string(result));
    }
}

bool isDepthFormat(vk::Format format)
{
    switch (format) {
        case vk::Format::eD16Unorm:
        case vk::Format::eD32Sfloat:
        case vk::Format::eS8Uint:
        case vk::Format::eD16UnormS8Uint:
        case vk::Format::eD24UnormS8Uint:
        case vk::Format::eD32SfloatS8Uint:
        case vk::Format::eX8D24UnormPack32:
            return true;
        default:
            return false;
    }
}

bool hasStencil(vk::Format format)
{
    switch (format) {
        case vk::Format::eS8Uint:
        case vk::Format::eD16UnormS8Uint:
        case vk::Format::eD24UnormS8Uint:
        case vk::Format::eD32SfloatS8Uint:
            return true;
        default:
            return false;
    }
}

vk::ImageAspectFlags aspectOf(vk::Format format)
{
    if (!isDepthFormat(format)) {
        return vk::ImageAspectFlagBits::eColor;
    }
    return hasStencil(format) ? (vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil)
                              : vk::ImageAspectFlagBits::eDepth;
}

const OffscreenTargetSpec *findOffscreenSpec(const RenderPassBlueprint &blueprint, uint32_t attachmentIndex)
{
    for (const OffscreenTargetSpec &spec : blueprint.offscreenTargets) {
        if (spec.attachmentIndex == attachmentIndex) {
            return &spec;
        }
    }
    return nullptr;
}

bool isSwapChainTarget(const RenderPassBlueprint &blueprint, const vk::AttachmentDescription &description)
{
    return blueprint.swapChainSized && description.finalLayout == vk::ImageLayout::ePresentSrcKHR;
}

} // namespace

BasicRenderPass::BasicRenderPass(std::shared_ptr<vk::Device> device,
                                 const vk::PhysicalDeviceMemoryProperties &memProps,
                                 std::shared_ptr<DeviceMemoryAllocator> allocator,
                                 const RenderPassBlueprint &blueprint) :
    m_device(std::move(device)), m_memProps(memProps), m_allocator(std::move(allocator)), m_blueprint(blueprint)
{
    createRenderPass();

    vk::Extent2D initialExtent{1, 1};
    for (const OffscreenTargetSpec &spec : m_blueprint.offscreenTargets) {
        if (spec.width > 0 && spec.height > 0) {
            initialExtent = vk::Extent2D(spec.width, spec.height);
            break;
        }
    }
    createAttachments(initialExtent);
}

void BasicRenderPass::createRenderPass()
{
    const std::vector<SubpassBlueprint> &subpassBlueprints = m_blueprint.subpasses;
    std::vector<vk::SubpassDescription> subpasses;
    subpasses.reserve(subpassBlueprints.size());

    for (const SubpassBlueprint &blueprintSubpass : subpassBlueprints) {
        vk::SubpassDescription subpass{};
        subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;

        subpass.colorAttachmentCount = static_cast<uint32_t>(blueprintSubpass.colorAttachments.size());
        subpass.pColorAttachments =
            blueprintSubpass.colorAttachments.empty() ? nullptr : blueprintSubpass.colorAttachments.data();

        subpass.inputAttachmentCount = static_cast<uint32_t>(blueprintSubpass.inputAttachments.size());
        subpass.pInputAttachments =
            blueprintSubpass.inputAttachments.empty() ? nullptr : blueprintSubpass.inputAttachments.data();

        subpass.pDepthStencilAttachment =
            blueprintSubpass.depthStencil.has_value() ? &*blueprintSubpass.depthStencil : nullptr;

        subpass.pResolveAttachments =
            blueprintSubpass.resolveAttachments.empty() ? nullptr : blueprintSubpass.resolveAttachments.data();

        subpass.preserveAttachmentCount = static_cast<uint32_t>(blueprintSubpass.preserveAttachments.size());
        subpass.pPreserveAttachments =
            blueprintSubpass.preserveAttachments.empty() ? nullptr : blueprintSubpass.preserveAttachments.data();

        subpasses.push_back(subpass);
    }

    vk::RenderPassCreateInfo createInfo{};
    createInfo.attachmentCount = static_cast<uint32_t>(m_blueprint.attachments.size());
    createInfo.pAttachments = m_blueprint.attachments.empty() ? nullptr : m_blueprint.attachments.data();
    createInfo.subpassCount = static_cast<uint32_t>(subpasses.size());
    createInfo.pSubpasses = subpasses.empty() ? nullptr : subpasses.data();
    createInfo.dependencyCount = static_cast<uint32_t>(m_blueprint.dependencies.size());
    createInfo.pDependencies = m_blueprint.dependencies.empty() ? nullptr : m_blueprint.dependencies.data();

    vk::RenderPass renderPass{};
    check(m_device->createRenderPass(&createInfo, nullptr, &renderPass), "vkCreateRenderPass");
    m_renderPass = decltype(m_renderPass)(renderPass, [device = m_device](vk::RenderPass handle) {
        device->destroyRenderPass(handle);
    });
}

void BasicRenderPass::createAttachments(vk::Extent2D extent)
{
    m_attachmentSets.clear();
    m_offscreenViews.clear();
    m_offscreenImages.clear();
    m_offscreenFormats.clear();
    m_colorCountPerSet = 0;
    m_extent = extent;

    // Swapchain-sized passes own exactly one set of MSAA/depth images that all
    // swapchain framebuffers reference.  Offscreen passes instead allocate one
    // complete, independent set per in-flight frame: multiple frames must never
    // write (and then sample) the same image, otherwise the layout transition
    // at beginRenderPass races with the previous frame's endRenderPass writes.
    const uint32_t setCount = m_blueprint.swapChainSized
                                  ? 1
                                  : (m_blueprint.framebufferCount == 0 ? 1 : m_blueprint.framebufferCount);
    m_attachmentSets.resize(setCount);

    // Iterate set-major so m_offscreenViews comes out frame-major:
    // [setIndex * colorCountPerSet + colorSlot].
    for (uint32_t set = 0; set < setCount; ++set) {
        m_attachmentSets[set].resize(m_blueprint.attachments.size());
        for (uint32_t index = 0; index < m_blueprint.attachments.size(); ++index) {
            const vk::AttachmentDescription &description = m_blueprint.attachments[index];
            const OffscreenTargetSpec *spec = findOffscreenSpec(m_blueprint, index);

            // The swapchain-owned attachment (usually the single-sampled resolve
            // target) is supplied externally as an image view.
            if (isSwapChainTarget(m_blueprint, description)) {
                continue;
            }

            const vk::Format format = (spec && spec->format != vk::Format::eUndefined) ? spec->format : description.format;
            const vk::SampleCountFlagBits samples = description.samples;
            const bool depthTarget = isDepthFormat(format);

            vk::ImageUsageFlags usage{};
            if (depthTarget) {
                usage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
            } else {
                usage |= vk::ImageUsageFlagBits::eColorAttachment;
            }
            usage |= vk::ImageUsageFlagBits::eTransferSrc;
            if (spec) {
                usage |= spec->extraUsage;
            }

            if (samples != vk::SampleCountFlagBits::e1) {
                // MSAA color/depth targets are transient; only the resolved
                // single-sampled image is ever sampled by a shader.
                usage |= vk::ImageUsageFlagBits::eTransientAttachment;
                usage &= ~vk::ImageUsageFlagBits::eSampled;
                usage &= ~vk::ImageUsageFlagBits::eTransferSrc;
            }

            auto attachment = std::make_shared<ImageAttachment>(m_device, m_memProps, m_allocator, extent, format,
                                                                samples, usage, aspectOf(format));
            const vk::ImageView view = attachment->imageView();
            if (view && samples == vk::SampleCountFlagBits::e1 && !depthTarget) {
                if (set == 0) {
                    ++m_colorCountPerSet;
                    // Slot formats and images repeat per frame set; collect them
                    // once, in slot order.
                    m_offscreenFormats.push_back(format);
                }
                m_offscreenViews.push_back(view);
                m_offscreenImages.push_back(attachment->image());
            }
            m_attachmentSets[set][index] = std::move(attachment);
        }
    }
}

void BasicRenderPass::createFramebuffers(const std::vector<vk::ImageView> &swapChainViews,
                                         vk::Extent2D swapChainExtent)
{
    // Release old framebuffers before rebuilding so resize never keeps two
    // generations of attachments alive at once.
    m_framebuffers.clear();

    const auto makeFramebuffer = [this](const std::vector<vk::ImageView> &attachments, vk::Extent2D extent) {
        vk::FramebufferCreateInfo framebufferInfo{};
        framebufferInfo.renderPass = static_cast<vk::RenderPass>(m_renderPass);
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.empty() ? nullptr : attachments.data();
        framebufferInfo.width = extent.width;
        framebufferInfo.height = extent.height;
        framebufferInfo.layers = 1;

        vk::Framebuffer framebuffer{};
        check(m_device->createFramebuffer(&framebufferInfo, nullptr, &framebuffer), "vkCreateFramebuffer");
        m_framebuffers.emplace_back(framebuffer, [device = m_device](vk::Framebuffer handle) {
            device->destroyFramebuffer(handle);
        });
    };

    if (m_blueprint.swapChainSized) {
        // MSAA color and depth attachments follow the swapchain extent.  One
        // shared set is referenced by every swapchain framebuffer.
        createAttachments(swapChainExtent);

        for (const vk::ImageView &swapChainView : swapChainViews) {
            std::vector<vk::ImageView> attachments;
            attachments.reserve(m_blueprint.attachments.size());
            for (uint32_t index = 0; index < m_blueprint.attachments.size(); ++index) {
                if (isSwapChainTarget(m_blueprint, m_blueprint.attachments[index])) {
                    attachments.push_back(swapChainView);
                } else {
                    attachments.push_back(m_attachmentSets[0][index] ? m_attachmentSets[0][index]->imageView()
                                                                     : vk::ImageView{});
                }
            }
            makeFramebuffer(attachments, swapChainExtent);
        }
    } else {
        // Each in-flight frame gets a framebuffer pointing at its own image
        // set, so frame N's layout transition can never race with the writes
        // of frame N-1 on a shared attachment.
        const uint32_t setCount = static_cast<uint32_t>(m_attachmentSets.size());
        for (uint32_t set = 0; set < setCount; ++set) {
            std::vector<vk::ImageView> attachments;
            attachments.reserve(m_blueprint.attachments.size());
            for (uint32_t index = 0; index < m_blueprint.attachments.size(); ++index) {
                attachments.push_back(m_attachmentSets[set][index] ? m_attachmentSets[set][index]->imageView()
                                                                    : vk::ImageView{});
            }
            makeFramebuffer(attachments, m_extent);
        }
    }
}

void BasicRenderPass::begin(const vk::CommandBuffer &cmd, uint32_t frameIndex, vk::Extent2D renderArea)
{
    if (m_framebuffers.empty()) {
        throw std::runtime_error("BasicRenderPass::begin called before framebuffers were created");
    }

    vk::RenderPassBeginInfo beginInfo{};
    beginInfo.renderPass = static_cast<vk::RenderPass>(m_renderPass);
    beginInfo.framebuffer = static_cast<vk::Framebuffer>(m_framebuffers[frameIndex % m_framebuffers.size()]);
    beginInfo.renderArea.offset = vk::Offset2D(0, 0);
    beginInfo.renderArea.extent = renderArea;

    std::vector<vk::ClearValue> clearValues = m_blueprint.clearValues;
    const size_t attachmentCount = m_blueprint.attachments.size();
    if (clearValues.size() < attachmentCount) {
        clearValues.resize(attachmentCount);
    }
    beginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    beginInfo.pClearValues = clearValues.data();

    cmd.beginRenderPass(&beginInfo, vk::SubpassContents::eInline);

    vk::Viewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(renderArea.width);
    viewport.height = static_cast<float>(renderArea.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    cmd.setViewport(0, 1, &viewport);

    vk::Rect2D scissor{};
    scissor.offset = vk::Offset2D(0, 0);
    scissor.extent = renderArea;
    cmd.setScissor(0, 1, &scissor);
}

void BasicRenderPass::end(const vk::CommandBuffer &cmd)
{
    cmd.endRenderPass();
}

void BasicRenderPass::nextSubpass(const vk::CommandBuffer &cmd)
{
    cmd.nextSubpass(vk::SubpassContents::eInline);
}
