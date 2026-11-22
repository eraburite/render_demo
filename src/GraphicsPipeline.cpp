#include "GraphicsPipeline.h"

#include "IRenderPass.h"

#include <array>
#include <functional>
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

using ShaderModuleHandle = VulkanHandle<vk::ShaderModule, std::function<void(vk::ShaderModule)>>;

ShaderModuleHandle createShaderModule(const std::shared_ptr<vk::Device> &device,
                                      const std::vector<uint32_t> &code)
{
    vk::ShaderModuleCreateInfo createInfo{};
    createInfo.codeSize = code.size() * sizeof(uint32_t);
    createInfo.pCode = code.data();

    vk::ShaderModule shaderModule{};
    check(device->createShaderModule(&createInfo, nullptr, &shaderModule), "vkCreateShaderModule");
    return ShaderModuleHandle(shaderModule,
                              [device](vk::ShaderModule module) { device->destroyShaderModule(module); });
}

// Shared (static) layers only ever use the frame-0 set; every other set is
// double/triple buffered per frame-in-flight.
vk::DescriptorSet descriptorSetFor(const std::vector<std::vector<vk::DescriptorSet>> &sets,
                                   const std::shared_ptr<const IDescriptorSetLayoutGroup> &layout, uint32_t frame,
                                   uint32_t setId)
{
    if (sets.empty()) {
        return {};
    }
    const uint32_t activeFrame = (layout && layout->isSetShared(setId)) ? 0u : frame;
    if (activeFrame >= sets.size() || setId >= sets[activeFrame].size()) {
        return {};
    }
    return sets[activeFrame][setId];
}

const std::unordered_map<std::string, std::vector<uint32_t>> &emptyShaderCodes()
{
    static const std::unordered_map<std::string, std::vector<uint32_t>> codes;
    return codes;
}

} // namespace

GraphicsPipeline::GraphicsPipeline(std::shared_ptr<vk::Device> device, DeviceFeatureProperty feature,
                                   std::shared_ptr<const IVertexInputStateGroup> vertexInput,
                                   std::shared_ptr<const IDescriptorSetLayoutGroup> descriptorLayout,
                                   std::shared_ptr<ShaderAssetGroup> asset,
                                   const std::shared_ptr<IRenderPass> &renderPass, uint32_t subpass) :
    m_device(std::move(device)), m_feature(feature), m_vertexInput(std::move(vertexInput)),
    m_descriptorLayout(std::move(descriptorLayout)), m_asset(std::move(asset)), m_renderPass(renderPass),
    m_subpass(subpass)
{
}

void GraphicsPipeline::builtUp()
{
    createSetLayouts();
    createDescriptorPool();
    allocateDescriptorSets();
    writeDescriptors();
    createPipeline(m_renderPass, m_subpass,
                   m_renderPass ? m_renderPass->sampleCount() : m_feature.msaaSamples);
}

void GraphicsPipeline::createSetLayouts()
{
    if (!m_descriptorLayout) {
        return;
    }
    const std::vector<vk::DescriptorSetLayoutCreateInfo> infos = m_descriptorLayout->layoutCreateInfos();
    m_setLayouts.clear();
    m_setLayouts.reserve(infos.size());
    for (const vk::DescriptorSetLayoutCreateInfo &info : infos) {
        vk::DescriptorSetLayout layout{};
        check(m_device->createDescriptorSetLayout(&info, nullptr, &layout), "vkCreateDescriptorSetLayout");
        m_setLayouts.emplace_back(layout, [device = m_device](vk::DescriptorSetLayout handle) {
            device->destroyDescriptorSetLayout(handle);
        });
    }
}

void GraphicsPipeline::createDescriptorPool()
{
    const uint32_t frameCount = m_asset ? m_asset->maxFrames() : MAX_FRAMES_IN_FLIGHT;
    const uint32_t setCount = m_descriptorLayout ? m_descriptorLayout->setCount() : 0;

    std::vector<vk::DescriptorPoolSize> poolSizes;
    if (m_descriptorLayout) {
        for (vk::DescriptorPoolSize size : m_descriptorLayout->poolSizesPerFrame()) {
            size.descriptorCount *= frameCount;
            poolSizes.push_back(size);
        }
    }

    vk::DescriptorPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    poolInfo.maxSets = frameCount * setCount;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();

    vk::DescriptorPool pool{};
    check(m_device->createDescriptorPool(&poolInfo, nullptr, &pool), "vkCreateDescriptorPool");
    m_descriptorPool = decltype(m_descriptorPool)(pool, [device = m_device](vk::DescriptorPool handle) {
        device->destroyDescriptorPool(handle);
    });
}

void GraphicsPipeline::allocateDescriptorSets()
{
    const uint32_t frameCount = m_asset ? m_asset->maxFrames() : MAX_FRAMES_IN_FLIGHT;
    const uint32_t setCount = static_cast<uint32_t>(m_setLayouts.size());
    if (setCount == 0 || frameCount == 0) {
        return;
    }

    std::vector<vk::DescriptorSetLayout> layouts;
    layouts.reserve(static_cast<size_t>(frameCount) * setCount);
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        for (const auto &setLayout : m_setLayouts) {
            layouts.push_back(static_cast<vk::DescriptorSetLayout>(setLayout));
        }
    }

    vk::DescriptorSetAllocateInfo allocInfo{};
    allocInfo.descriptorPool = static_cast<vk::DescriptorPool>(m_descriptorPool);
    allocInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocInfo.pSetLayouts = layouts.data();

    std::vector<vk::DescriptorSet> allocated(layouts.size());
    check(m_device->allocateDescriptorSets(&allocInfo, allocated.data()), "vkAllocateDescriptorSets");

    m_descriptorSets.assign(frameCount, std::vector<vk::DescriptorSet>(setCount));
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        for (uint32_t set = 0; set < setCount; ++set) {
            m_descriptorSets[frame][set] = allocated[static_cast<size_t>(frame) * setCount + set];
        }
    }
}

void GraphicsPipeline::writeDescriptors()
{
    if (!m_asset || !m_descriptorLayout) {
        return;
    }
    const uint32_t frameCount = m_asset->maxFrames();
    const std::vector<SetLayoutDetail> &details = m_descriptorLayout->setDetails();

    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        std::vector<vk::WriteDescriptorSet> writes;
        for (uint32_t setIndex = 0; setIndex < details.size(); ++setIndex) {
            const SetLayoutDetail &detail = details[setIndex];
            const bool shared = detail.shared || m_descriptorLayout->isSetShared(detail.setId);
            if (shared && frame != 0) {
                continue; // shared layers write / bind frame-0 set only
            }
            if (setIndex >= m_descriptorSets[frame].size()) {
                continue;
            }
            const vk::DescriptorSet targetSet = m_descriptorSets[frame][setIndex];
            if (!targetSet) {
                continue;
            }

            for (uint32_t bindingIndex = 0; bindingIndex < detail.bindings.size(); ++bindingIndex) {
                const vk::DescriptorSetLayoutBinding &binding = detail.bindings[bindingIndex];
                const ResourceId id = bindingIndex < detail.resourceIds.size()
                                          ? detail.resourceIds[bindingIndex]
                                          : ResourceId(detail.setId, binding.binding);
                const uint32_t resourceFrame = shared ? 0u : frame;
                const std::shared_ptr<IDeviceResource> resource =
                    m_asset->DescriptorResource(ResourceKey::make_key(id, resourceFrame));
                if (!resource) {
                    continue;
                }
                writes.push_back(resource->constructWriteDescriptorSet(targetSet, binding.descriptorType,
                                                                       binding.binding));
            }
        }
        if (!writes.empty()) {
            m_device->updateDescriptorSets(static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }
    }
}

void GraphicsPipeline::createPipeline(const std::shared_ptr<IRenderPass> &renderPass, uint32_t subpass,
                                      vk::SampleCountFlagBits msaa)
{
    const auto &codes = m_asset ? m_asset->shaderCodes() : emptyShaderCodes();
    const auto findCode = [&codes](const char *primary, const char *fallback) -> const std::vector<uint32_t> * {
        for (const char *key : {primary, fallback}) {
            const auto it = codes.find(key);
            if (it != codes.end() && !it->second.empty()) {
                return &it->second;
            }
        }
        return nullptr;
    };

    const std::vector<uint32_t> *vertexCode = findCode(" vert", "vert");
    const std::vector<uint32_t> *fragmentCode = findCode(" frag", "frag");
    if (!vertexCode) {
        throw std::runtime_error("GraphicsPipeline: missing \"vert\" SPIR-V code");
    }
    if (!fragmentCode) {
        throw std::runtime_error("GraphicsPipeline: missing \"frag\" SPIR-V code");
    }

    ShaderModuleHandle vertexModule = createShaderModule(m_device, *vertexCode);
    ShaderModuleHandle fragmentModule = createShaderModule(m_device, *fragmentCode);

    vk::PipelineShaderStageCreateInfo vertexStage{};
    vertexStage.stage = vk::ShaderStageFlagBits::eVertex;
    vertexStage.module = static_cast<vk::ShaderModule>(vertexModule);
    vertexStage.pName = "main";

    vk::PipelineShaderStageCreateInfo fragmentStage{};
    fragmentStage.stage = vk::ShaderStageFlagBits::eFragment;
    fragmentStage.module = static_cast<vk::ShaderModule>(fragmentModule);
    fragmentStage.pName = "main";

    const std::array<vk::PipelineShaderStageCreateInfo, 2> shaderStages{vertexStage, fragmentStage};

    const vk::PipelineVertexInputStateCreateInfo emptyVertexInput{};
    const vk::PipelineVertexInputStateCreateInfo &vertexInputInfo =
        m_vertexInput ? m_vertexInput->pipelineVertexInputState() : emptyVertexInput;

    vk::PipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.topology = vk::PrimitiveTopology::eTriangleList;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    vk::PipelineViewportStateCreateInfo viewportState{};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    vk::PipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = vk::PolygonMode::eFill;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = vk::CullModeFlagBits::eNone;
    rasterizer.frontFace = vk::FrontFace::eCounterClockwise;
    rasterizer.depthBiasEnable = VK_FALSE;

    vk::PipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = msaa;

    vk::PipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = vk::CompareOp::eLess;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    vk::PipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.blendEnable = VK_FALSE;
    colorBlendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG
                                          | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

    const uint32_t colorAttachmentCount = renderPass ? renderPass->colorAttachmentCount() : 1;
    std::vector<vk::PipelineColorBlendAttachmentState> colorAttachments(
        colorAttachmentCount == 0 ? 1 : colorAttachmentCount, colorBlendAttachment);

    vk::PipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.logicOp = vk::LogicOp::eCopy;
    colorBlending.attachmentCount = static_cast<uint32_t>(colorAttachments.size());
    colorBlending.pAttachments = colorAttachments.data();
    colorBlending.blendConstants = {{0.0f, 0.0f, 0.0f, 0.0f}};

    const std::array<vk::DynamicState, 2> dynamicStates{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    std::vector<vk::DescriptorSetLayout> setLayouts;
    setLayouts.reserve(m_setLayouts.size());
    for (const auto &layout : m_setLayouts) {
        setLayouts.push_back(static_cast<vk::DescriptorSetLayout>(layout));
    }

    const std::vector<vk::PushConstantRange> emptyRanges;
    const std::vector<vk::PushConstantRange> &pushConstantRanges =
        m_descriptorLayout ? m_descriptorLayout->pushConstantRanges() : emptyRanges;

    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    pipelineLayoutInfo.pSetLayouts = setLayouts.data();
    pipelineLayoutInfo.pushConstantRangeCount = static_cast<uint32_t>(pushConstantRanges.size());
    pipelineLayoutInfo.pPushConstantRanges = pushConstantRanges.data();

    vk::PipelineLayout layout{};
    check(m_device->createPipelineLayout(&pipelineLayoutInfo, nullptr, &layout), "vkCreatePipelineLayout");
    m_pipelineLayout = decltype(m_pipelineLayout)(layout, [device = m_device](vk::PipelineLayout handle) {
        device->destroyPipelineLayout(handle);
    });

    vk::GraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = static_cast<vk::PipelineLayout>(m_pipelineLayout);
    pipelineInfo.renderPass = renderPass ? renderPass->handle() : vk::RenderPass{};
    pipelineInfo.subpass = subpass;
    pipelineInfo.basePipelineHandle = vk::Pipeline{};
    pipelineInfo.basePipelineIndex = -1;

    vk::Pipeline pipeline{};
    check(m_device->createGraphicsPipelines(nullptr, 1, &pipelineInfo, nullptr, &pipeline),
          "vkCreateGraphicsPipelines");
    m_pipeline = decltype(m_pipeline)(pipeline, [device = m_device](vk::Pipeline handle) {
        device->destroyPipeline(handle);
    });
}

void GraphicsPipeline::recordDrawCommand(const vk::CommandBuffer &cmd, uint32_t frame,
                                         DescriptorBindingState &bindingState)
{
    if (!m_pipeline) {
        return;
    }
    const vk::PipelineLayout layout = static_cast<vk::PipelineLayout>(m_pipelineLayout);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, static_cast<vk::Pipeline>(m_pipeline));

    const std::vector<vk::PushConstantRange> emptyRanges;
    const std::vector<vk::PushConstantRange> &ranges =
        m_descriptorLayout ? m_descriptorLayout->pushConstantRanges() : emptyRanges;
    if (m_asset) {
        m_asset->PushConstantData(cmd, layout, ranges, frame);
    }

    // Incremental binding: only bind descriptor sets missing under this layout.
    bindingState.OnPipelineLayoutBound(layout);
    const uint32_t setCount = m_descriptorLayout ? m_descriptorLayout->setCount() : 0;
    const uint32_t firstSet = bindingState.FirstUnboundSet(setCount);
    if (firstSet < setCount) {
        std::vector<vk::DescriptorSet> sets;
        sets.reserve(setCount - firstSet);
        const std::vector<SetLayoutDetail> &details = m_descriptorLayout->setDetails();
        for (uint32_t index = firstSet; index < setCount; ++index) {
            const uint32_t setId = index < details.size() ? details[index].setId : index;
            sets.push_back(descriptorSetFor(m_descriptorSets, m_descriptorLayout, frame, setId));
        }
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, firstSet,
                               static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
        bindingState.MarkBoundRange(firstSet, setCount - firstSet);
    }

    if (m_asset) {
        m_asset->bindToPipeline(cmd);
    }

    const uint32_t instanceCount = m_vertexInput ? m_vertexInput->drawInstanceCount() : 1u;
    if (m_vertexInput && m_vertexInput->hasIndexBuffer()) {
        cmd.drawIndexed(m_vertexInput->indexCount(), instanceCount, 0, 0, 0);
    } else if (!m_vertexInput || m_vertexInput->isEmptyInputState()) {
        // No vertex input: shaders generate vertices from gl_VertexIndex.
        const uint32_t vertexCount = m_vertexInput ? m_vertexInput->getVertexCount() : 0;
        cmd.draw(vertexCount == 0 ? 3 : vertexCount, instanceCount, 0, 0);
    } else {
        cmd.draw(m_vertexInput->vertexCount(), instanceCount, 0, 0);
    }
}
