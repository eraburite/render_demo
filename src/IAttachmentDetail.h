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

// An offscreen color attachment that the engine must allocate an image for.
struct OffscreenTargetSpec {
    uint32_t attachmentIndex = 0;
    vk::Format format = vk::Format::eR8G8B8A8Unorm;
    vk::ImageUsageFlagBits extraUsage = vk::ImageUsageFlagBits::eSampled;
    bool fixedSize = true;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct RenderPassBlueprint {
    std::vector<vk::AttachmentDescription> attachments;
    std::vector<SubpassBlueprint> subpasses;
    std::vector<vk::SubpassDependency> dependencies;
    std::vector<vk::ClearValue> clearValues;
    std::vector<OffscreenTargetSpec> offscreenTargets;

    uint32_t colorAttachmentCount = 1; // color outputs of subpass 0 (drives blend state / clear values)
    vk::SampleCountFlagBits sampleCount = vk::SampleCountFlagBits::e1;
    bool swapChainSized = false; // framebuffers depend on swapchain images -> must rebuild on resize
    uint32_t framebufferCount = 0; // for offscreen passes: number of framebuffers (0 = one)
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

inline std::vector<vk::SubpassDependency> makeDependencies(DependencyPreset preset)
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
            deps.push_back(vk::SubpassDependency(0, 1, vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                                 vk::PipelineStageFlagBits::eFragmentShader,
                                                 vk::AccessFlagBits::eColorAttachmentWrite,
                                                 vk::AccessFlagBits::eInputAttachmentRead,
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
