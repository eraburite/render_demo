#pragma once
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "ComputePipeline.h" // SpecializationConstant / makeSpecializationConstant
#include "DescriptorBindingState.h"
#include "IDescriptorSetDetail.h"
#include "IVertexInputStateGroup.h"
#include "ShaderAssetGroup.h"
#include "VulkanHandle.h"

struct DeviceFeatureProperty {
    vk::SampleCountFlagBits msaaSamples = vk::SampleCountFlagBits::e1;
    uint32_t maxPushConstantSize = 0;
};

// One shader stage of a graphics pipeline.  `key` is the name the SPIR-V is
// registered under in ShaderAssetGroup (e.g. "vert", "geom", "tesc", "tese",
// "frag"); `stage` is the corresponding Vulkan stage flag.
struct GraphicsPipelineStage {
    std::string key;
    vk::ShaderStageFlagBits stage = vk::ShaderStageFlagBits::eVertex;
};

// Per-color-attachment blend state.  Plain alpha blending is one flag
// (GraphicsPipelineConfig::blendEnable), but weighted-blended OIT writes two
// targets that need *different* factors in the same subpass: the accumulation
// target adds (ONE, ONE) while the revealage target multiplies
// (ZERO, ONE_MINUS_SRC_COLOR).  Vulkan has always allowed per-attachment state;
// this struct is what lets a caller express it.
struct ColorBlendAttachment {
    bool enable = true;
    vk::BlendFactor srcColor = vk::BlendFactor::eSrcAlpha;
    vk::BlendFactor dstColor = vk::BlendFactor::eOneMinusSrcAlpha;
    vk::BlendOp colorOp = vk::BlendOp::eAdd;
    vk::BlendFactor srcAlpha = vk::BlendFactor::eOne;
    vk::BlendFactor dstAlpha = vk::BlendFactor::eOneMinusSrcAlpha;
    vk::BlendOp alphaOp = vk::BlendOp::eAdd;
    vk::ColorComponentFlags writeMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG
                                       | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

    // The plain "src alpha over" case, spelled out so examples can start from a
    // known state instead of the struct defaults.
    static ColorBlendAttachment alphaBlend()
    {
        ColorBlendAttachment state;
        return state;
    }
    // dst = src + dst, for accumulation targets.
    static ColorBlendAttachment additive()
    {
        ColorBlendAttachment state;
        state.srcColor = vk::BlendFactor::eOne;
        state.dstColor = vk::BlendFactor::eOne;
        state.srcAlpha = vk::BlendFactor::eOne;
        state.dstAlpha = vk::BlendFactor::eOne;
        return state;
    }
    // dst = dst * (1 - src), for revealage targets.
    static ColorBlendAttachment revealage()
    {
        ColorBlendAttachment state;
        state.srcColor = vk::BlendFactor::eZero;
        state.dstColor = vk::BlendFactor::eOneMinusSrcColor;
        state.srcAlpha = vk::BlendFactor::eZero;
        state.dstAlpha = vk::BlendFactor::eOneMinusSrcAlpha;
        return state;
    }
};

// Optional construction settings for GraphicsPipeline.  The default value is
// deliberately identical to the legacy hard-coded behaviour (vert+frag,
// triangle list, fill, no culling, depth less/write on), so existing examples
// do not have to change.  Extended examples (geometry shader, tessellation,
// wireframe) pass a customised config.
struct GraphicsPipelineConfig {
    // Empty means the legacy {vert, frag} pair.  Order matters only for the
    // SPIR-V module lookup, not for Vulkan.
    std::vector<GraphicsPipelineStage> stages;
    vk::PrimitiveTopology topology = vk::PrimitiveTopology::eTriangleList;
    vk::PolygonMode polygonMode = vk::PolygonMode::eFill;
    vk::CullModeFlags cullMode = vk::CullModeFlagBits::eNone;
    vk::FrontFace frontFace = vk::FrontFace::eCounterClockwise;
    float lineWidth = 1.0f;
    // 0 disables the tessellation state entirely.  >0 adds a
    // VkPipelineTessellationStateCreateInfo with that many control points per
    // patch (requires VK_PRIMITIVE_TOPOLOGY_PATCH_LIST and the
    // tessellationShader device feature).
    uint32_t patchControlPoints = 0;
    bool depthTestEnable = true;
    bool depthWriteEnable = true;
    vk::CompareOp depthCompareOp = vk::CompareOp::eLess;
    // Stencil test.  Disabled by default, so every existing pipeline keeps its
    // old state.  Enabling it requires a render pass with an S8 attachment
    // (IRenderPass::hasStencil()), checked at pipeline creation.
    bool stencilTestEnable = false;
    vk::StencilOpState stencilFront{};
    vk::StencilOpState stencilBack{};
    // Which stencil parameters are supplied by CmdOpStencilState at record time
    // instead of being baked into the pipeline.  Reference is a Vulkan 1.0
    // dynamic state; compare/write masks joined core in Vulkan 1.3.
    bool dynamicStencilReference = false;
    bool dynamicStencilCompareMask = false;
    bool dynamicStencilWriteMask = false;
    bool blendEnable = false;
    // Optional per-attachment override.  When non-empty, entry i describes
    // color attachment i of this pipeline's subpass; a list shorter than the
    // attachment count repeats its last entry.  Empty keeps the legacy single
    // `blendEnable` state, so every existing example is byte-for-byte unchanged.
    std::vector<ColorBlendAttachment> blendAttachments;
    bool primitiveRestartEnable = false;
    // Specialization constants, grouped by the stage key they apply to.  Each
    // group becomes a VkSpecializationInfo attached to that stage.  Kept as a
    // pair vector instead of a map so construction order is deterministic.
    std::vector<std::pair<std::string, std::vector<SpecializationConstant>>> specializations;

    GraphicsPipelineConfig &addStage(std::string key, vk::ShaderStageFlagBits stage)
    {
        stages.push_back({std::move(key), stage});
        return *this;
    }
    // Geometry stage.  The invocation count is *not* a pipeline field -- it is
    // the SPIR-V execution mode OpExecutionMode Invocations, which glslang
    // derives from the shader's `layout(invocations = N) in;`.  One invocation
    // per array layer is what turns a single draw into N gl_Layer writes (see
    // examples/csm).  It has to match renderPass->depthStencilLayers(); a
    // mismatch is not a pipeline error, it just leaves part of the depth array
    // untouched.
    GraphicsPipelineConfig &addGeometryStage(std::string key)
    {
        stages.push_back({std::move(key), vk::ShaderStageFlagBits::eGeometry});
        return *this;
    }
    GraphicsPipelineConfig &addSpecialization(std::string stageKey, std::vector<SpecializationConstant> constants)
    {
        specializations.emplace_back(std::move(stageKey), std::move(constants));
        return *this;
    }
    GraphicsPipelineConfig &setStencil(vk::StencilOpState front, vk::StencilOpState back)
    {
        stencilTestEnable = true;
        stencilFront = front;
        stencilBack = back;
        return *this;
    }
    GraphicsPipelineConfig &setDynamicStencil(bool reference, bool compareMask, bool writeMask)
    {
        dynamicStencilReference = reference;
        dynamicStencilCompareMask = compareMask;
        dynamicStencilWriteMask = writeMask;
        return *this;
    }
    // Per-attachment blend, in subpass color-attachment order.
    GraphicsPipelineConfig &addBlendAttachment(ColorBlendAttachment state)
    {
        blendAttachments.push_back(state);
        return *this;
    }
};

class IRenderPass;

// One indexed draw inside a shared index buffer.  A loaded model is a single
// vertex/index buffer cut into per-material primitive windows, so "one pipeline,
// N windows" is the normal case; baking firstIndex = 0 into the draw would force
// every caller to flatten the model into a non-indexed stream instead.
struct DrawRange {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0; // 0 = runs to the end of the bound index buffer
    uint32_t baseVertex = 0;
    uint32_t firstInstance = 0;
};

class GraphicsPipeline {
public:
    GraphicsPipeline(std::shared_ptr<vk::Device> device, DeviceFeatureProperty feature,
                     std::shared_ptr<IVertexInputStateGroup> vertexInput,
                     std::shared_ptr<const IDescriptorSetLayoutGroup> descriptorLayout,
                     std::shared_ptr<ShaderAssetGroup> asset,
                     const std::shared_ptr<IRenderPass> &renderPass, uint32_t subpass,
                     GraphicsPipelineConfig config = {});
    ~GraphicsPipeline() = default;

    void builtUp();
    void recordDrawCommand(const vk::CommandBuffer &cmd, uint32_t frame, DescriptorBindingState &bindingState);
    // Binds the pipeline once, then issues one draw per range.  This is the
    // multi-primitive path: descriptor sets are bound once for the whole batch,
    // because the per-layout binding cache only resets when the layout changes.
    void recordDrawCommand(const vk::CommandBuffer &cmd, uint32_t frame, DescriptorBindingState &bindingState,
                           const std::vector<DrawRange> &ranges);

    // Hook executed immediately after vkCmdBindPipeline, before the draw.
    // CmdOpDrawFrame uses it for dynamic stencil state: recording those
    // commands as a separate op risks landing before a pipeline bind that
    // declares the same state dynamic.
    using AfterBindHook = std::function<void(const vk::CommandBuffer &)>;
    void setAfterBindHook(AfterBindHook hook) { m_afterBindHook = std::move(hook); }

    // Overrides the draw's instance count.  Frustum culling writes this every
    // frame: the surviving instances are compacted to the front of the instance
    // table and only the first N are drawn.  The pipeline owns its own clone of
    // the vertex input, so the caller's atlas is intentionally not affected.
    void setInstanceCount(uint32_t count)
    {
        if (m_vertexInput) {
            m_vertexInput->setDrawInstanceCount(count);
        }
    }
    uint32_t instanceCount() const { return m_vertexInput ? m_vertexInput->drawInstanceCount() : 1u; }

    vk::PipelineLayout pipelineLayout() const { return m_pipelineLayout ? *m_pipelineLayout : vk::PipelineLayout{}; }
    const std::shared_ptr<ShaderAssetGroup> &asset() const { return m_asset; }

    // Re-writes every descriptor of every frame from the asset's current
    // resources.  Needed when a descriptor points at a framebuffer attachment
    // that was destroyed and recreated by a resize (see
    // IRenderPass::attachmentGeneration).  The caller must ensure the device is
    // idle: descriptor sets still referenced by pending submissions must not be
    // updated.
    void RefreshDescriptors() { writeDescriptors(); }

private:
    // Binds pipeline, push constants and the descriptor sets the per-layout
    // cache still misses, then the vertex/index buffers.  Returns false when
    // the pipeline object does not exist yet.  Shared by both draw overloads so
    // the batch path binds exactly as often as the single-draw path.
    bool bindForDraw(const vk::CommandBuffer &cmd, uint32_t frame, DescriptorBindingState &bindingState);
    void createSetLayouts();
    void createDescriptorPool();
    void allocateDescriptorSets();
    void writeDescriptors();
    void createPipeline(const std::shared_ptr<IRenderPass> &renderPass, uint32_t subpass,
                        vk::SampleCountFlagBits msaa);

private:
    std::shared_ptr<vk::Device> m_device;
    DeviceFeatureProperty m_feature;
    std::shared_ptr<IVertexInputStateGroup> m_vertexInput;
    std::shared_ptr<const IDescriptorSetLayoutGroup> m_descriptorLayout;
    std::shared_ptr<ShaderAssetGroup> m_asset;
    std::shared_ptr<IRenderPass> m_renderPass; // kept alive for pipeline creation / sample count
    uint32_t m_subpass = 0;
    GraphicsPipelineConfig m_config;
    AfterBindHook m_afterBindHook;

    std::vector<VulkanHandle<vk::DescriptorSetLayout, std::function<void(vk::DescriptorSetLayout)>>> m_setLayouts;
    VulkanHandle<vk::DescriptorPool, std::function<void(vk::DescriptorPool)>> m_descriptorPool;
    std::vector<std::vector<vk::DescriptorSet>> m_descriptorSets; // [frame][set]
    VulkanHandle<vk::PipelineLayout, std::function<void(vk::PipelineLayout)>> m_pipelineLayout;
    VulkanHandle<vk::Pipeline, std::function<void(vk::Pipeline)>> m_pipeline;
};
