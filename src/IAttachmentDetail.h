#pragma once
#include <cstdint>
#include <optional>
#include <tuple>
#include <vector>
#include <vulkan/vulkan.hpp>

// Neutral description of a render pass, produced by an IAttachmentDetail and
// consumed by BasicRenderPass.  Keeps example-side config independent of the
// engine-side framebuffer creation.
struct SubpassBlueprint {
    std::vector<vk::AttachmentReference> colorAttachments;
    std::vector<vk::AttachmentReference> inputAttachments;
    std::optional<vk::AttachmentReference> depthStencil;
    std::vector<vk::AttachmentReference> resolveAttachments;
    std::vector<uint32_t> preserveAttachments;
};

// Declares that one of the pass' depth attachments also carries a stencil
// aspect and how that aspect behaves at the render pass boundaries.  This is
// the *declaration* half of stencil support: BasicRenderPass turns it into the
// attachment description / image view / clear value, while GraphicsPipeline
// checks IRenderPass::hasStencil() before enabling a stencil test.
//
// The format must contain an S8 component.  A D32Sfloat depth-only attachment
// silently makes every stencil write a no-op (or triggers VUID-...), so the
// engine rejects a mismatched spec instead of producing a blank outline.
struct StencilAttachmentSpec {
    uint32_t attachmentIndex = 1; // index of the depth-stencil attachment in RenderPassBlueprint::attachments
    vk::AttachmentLoadOp loadOp = vk::AttachmentLoadOp::eClear;
    vk::AttachmentStoreOp storeOp = vk::AttachmentStoreOp::eStore;
    uint32_t clearValue = 0;
    vk::ImageLayout finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    // Keep the stencil contents readable by the host (ImageReadback copies the
    // S8 aspect).  Implies eStore even if storeOp was eDontCare.
    bool hostReadable = false;
};

// Formats carrying a stencil aspect.  Shared by BasicRenderPass (image view
// aspect), ImageReadback (copy aspect) and the example-side config validation.
inline bool hasStencilAspect(vk::Format format)
{
    switch (format) {
        case vk::Format::eS8Uint:
        case vk::Format::eD16UnormS8Uint:
        case vk::Format::eD24UnormS8Uint:
        case vk::Format::eD32SfloatS8Uint: return true;
        default: return false;
    }
}

// An offscreen color attachment that the engine must allocate an image for.
struct OffscreenTargetSpec {
    uint32_t attachmentIndex = 0;
    vk::Format format = vk::Format::eR8G8B8A8Unorm;
    // Additional image usage beyond colour attachment + transfer source.
    // Subpass inputs need eInputAttachment here, otherwise the image cannot be
    // read by a later subpass (VUID-VkImageViewCreateInfo-usage-02275).
    vk::ImageUsageFlags extraUsage = vk::ImageUsageFlagBits::eSampled;
    bool fixedSize = true;
    uint32_t width = 0;
    uint32_t height = 0;
    // Array layers of the allocated image.  > 1 turns it into a 2D array whose
    // single view covers every layer, which is what layered rendering writes to
    // through gl_Layer and what a sampler2DArray reads back.  Consumed for depth
    // attachments too (a cascaded shadow map is a depth array with no colour
    // target at all).
    uint32_t layers = 1;
};

struct RenderPassBlueprint {
    std::vector<vk::AttachmentDescription> attachments;
    std::vector<SubpassBlueprint> subpasses;
    std::vector<vk::SubpassDependency> dependencies;
    std::vector<vk::ClearValue> clearValues;
    std::vector<OffscreenTargetSpec> offscreenTargets;
    // Optional: one of the depth attachments also owns a stencil aspect.
    std::optional<StencilAttachmentSpec> stencil;

    uint32_t colorAttachmentCount = 1; // color outputs of subpass 0 (drives blend state / clear values)
    vk::SampleCountFlagBits sampleCount = vk::SampleCountFlagBits::e1;
    bool swapChainSized = false; // framebuffers depend on swapchain images -> must rebuild on resize
    uint32_t framebufferCount = 0; // for offscreen passes: number of framebuffers (0 = one)
    // Layer count of every framebuffer created from this blueprint.  Must match
    // the arrayLayers of the attachments that carry them: layered rendering
    // addresses a layer through gl_Layer, which is only legal when the
    // framebuffer declares more than one layer.
    uint32_t layers = 1;
};

class IAttachmentDetail {
public:
    virtual ~IAttachmentDetail() = default;
    virtual RenderPassBlueprint build(uint32_t width, uint32_t height, vk::SampleCountFlagBits msaa) const = 0;
    virtual bool isSwapChainSized() const = 0;
};

enum class AttachmentRole { eColor, eDepthStencil, eResolve, eInput };

template <typename ComponentEnum>
struct AttachmentComponentSpec {
    ComponentEnum component;
    vk::Format format;
    vk::AttachmentLoadOp loadOp;
    vk::AttachmentStoreOp storeOp;
    vk::ImageLayout initialLayout;
    vk::ImageLayout finalLayout;
    AttachmentRole role;
};

// Dependency presets shared by the example configs.
enum class DependencyPreset { eSwapChain, eMultiSubpass, eOffscreen };

// `withStencil` adds DEPTH_STENCIL_ATTACHMENT_READ to the scopes that can
// actually perform a stencil test (early/late fragment tests).  Adding it to a
// BOTTOM_OF_PIPE / FRAGMENT_SHADER-only scope would violate
// VUID-VkSubpassDependency-dstAccessMask-00869.  Defaults to false so every
// existing preset keeps its exact old bit pattern.
inline std::vector<vk::SubpassDependency> makeDependencies(DependencyPreset preset, bool withStencil = false)
{
    std::vector<vk::SubpassDependency> deps;
    switch (preset) {
        case DependencyPreset::eSwapChain:
            deps.push_back(vk::SubpassDependency(
                VK_SUBPASS_EXTERNAL, 0, vk::PipelineStageFlagBits::eBottomOfPipe,
                vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests,
                vk::AccessFlagBits::eMemoryRead,
                vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite
                    | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
                vk::DependencyFlagBits::eByRegion));
            // Release both color and depth writes at the end of the pass.  The
            // depth attachment is shared across all swapchain framebuffers, so
            // without a release covering LATE_FRAGMENT_TESTS / DEPTH_STENCIL_WRITE
            // the next frame's beginRenderPass layout transition races with this
            // frame's depth writes (synchronization WRITE_AFTER_WRITE hazard).
            deps.push_back(vk::SubpassDependency(
                0, VK_SUBPASS_EXTERNAL,
                vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eLateFragmentTests,
                vk::PipelineStageFlagBits::eBottomOfPipe | vk::PipelineStageFlagBits::eColorAttachmentOutput
                    | vk::PipelineStageFlagBits::eEarlyFragmentTests,
                vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite
                    | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
                vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eColorAttachmentRead
                    | vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
                vk::DependencyFlagBits::eByRegion));
            break;
        case DependencyPreset::eMultiSubpass:
            // Subpass 1 writes the same color/depth attachments subpass 0 wrote,
            // so the 0->1 dependency must order those attachment writes before
            // the second subpass reads and writes them again.
            deps.push_back(vk::SubpassDependency(
                VK_SUBPASS_EXTERNAL, 0, vk::PipelineStageFlagBits::eBottomOfPipe,
                vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests,
                vk::AccessFlagBits::eMemoryRead,
                vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite
                    | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
                vk::DependencyFlagBits::eByRegion));
            deps.push_back(vk::SubpassDependency(
                0, 1,
                vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eLateFragmentTests,
                vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests,
                vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
                vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite
                    | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
                vk::DependencyFlagBits::eByRegion));
            // The resolve (presented) attachment is first used in subpass 1, so
            // its UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL transition happens at
            // vkCmdNextSubpass rather than at render pass begin.  An
            // EXTERNAL -> 1 dependency keeps that transition ordered against
            // the acquire that handed us the swapchain image.
            deps.push_back(vk::SubpassDependency(
                VK_SUBPASS_EXTERNAL, 1, vk::PipelineStageFlagBits::eBottomOfPipe,
                vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests,
                vk::AccessFlagBits::eMemoryRead,
                vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
                vk::DependencyFlagBits::eByRegion));
            // Release both subpasses' attachment writes towards the presentation
            // engine (or the next frame's layout transition).
            deps.push_back(vk::SubpassDependency(
                1, VK_SUBPASS_EXTERNAL,
                vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eLateFragmentTests,
                vk::PipelineStageFlagBits::eBottomOfPipe | vk::PipelineStageFlagBits::eColorAttachmentOutput
                    | vk::PipelineStageFlagBits::eEarlyFragmentTests,
                vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite
                    | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
                vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eColorAttachmentRead
                    | vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
                vk::DependencyFlagBits::eByRegion));
            break;
        case DependencyPreset::eOffscreen:
            // Offscreen framebuffers share one set of attachment images across
            // in-flight frames.  The pre-pass dependency must therefore cover
            // the previous frame's color/depth writes, not just generic reads,
            // otherwise the next frame's layout transition races with them.
            deps.push_back(vk::SubpassDependency(
                VK_SUBPASS_EXTERNAL, 0,
                vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eLateFragmentTests,
                vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests,
                vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite
                    | vk::AccessFlagBits::eMemoryRead,
                vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite,
                vk::DependencyFlagBits::eByRegion));
            // Offscreen color is consumed by shader reads (deferred/quad passes,
            // or compute sampling). BottomOfPipe does not support SHADER_READ, so
            // name the actual consumer stages explicitly.
            deps.push_back(vk::SubpassDependency(
                0, VK_SUBPASS_EXTERNAL, vk::PipelineStageFlagBits::eColorAttachmentOutput,
                vk::PipelineStageFlagBits::eFragmentShader | vk::PipelineStageFlagBits::eComputeShader,
                vk::AccessFlagBits::eColorAttachmentWrite,
                vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eShaderRead,
                vk::DependencyFlagBits::eByRegion));
            break;
    }

    if (withStencil) {
        const vk::PipelineStageFlags fragmentTests = vk::PipelineStageFlagBits::eEarlyFragmentTests
                                                     | vk::PipelineStageFlagBits::eLateFragmentTests;
        const vk::AccessFlags stencilRead = vk::AccessFlagBits::eDepthStencilAttachmentRead;
        for (vk::SubpassDependency &dep : deps) {
            if ((dep.srcStageMask & fragmentTests) != vk::PipelineStageFlags{}) {
                dep.srcAccessMask = dep.srcAccessMask | stencilRead;
            }
            if ((dep.dstStageMask & fragmentTests) != vk::PipelineStageFlags{}) {
                dep.dstAccessMask = dep.dstAccessMask | stencilRead;
            }
        }
    }
    return deps;
}

// Template skeleton an example config can specialize: a component table maps
// enum -> attachment spec; the config lists the components it uses in order.
template <typename ComponentEnum, const auto &ConfigTable>
class AttachmentDetailTemplate : public IAttachmentDetail {
public:
    template <typename... Components>
    explicit AttachmentDetailTemplate(DependencyPreset preset, Components... components) : m_preset(preset)
    {
        (m_components.push_back(components), ...);
    }

    bool isSwapChainSized() const override { return m_swapChainSized; }
    void setSwapChainSized(bool value) { m_swapChainSized = value; }

protected:
    AttachmentComponentSpec<ComponentEnum> specOf(ComponentEnum c) const
    {
        AttachmentComponentSpec<ComponentEnum> result{};
        bool found = false;
        std::apply(
            [&](auto &&...row) {
                ((row.component == c ? (result = row, found = true) : false), ...);
            },
            ConfigTable);
        if (!found) {
            throw std::runtime_error("unknown attachment component");
        }
        return result;
    }

protected:
    std::vector<ComponentEnum> m_components;
    DependencyPreset m_preset;
    bool m_swapChainSized = true;
};
