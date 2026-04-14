#include "GraphicsPipeline.h"
#include "typedefs.h"
#include <cstdint>
#include <memory>

GraphicsPipeline::GraphicsPipeline(
    std::shared_ptr<vk::Device> device, DeviceFeatureProperty physicalProp, std::shared_ptr<vk::RenderPass> renderPass,
    const VertexInputParam &inputLayout, const PipelineDescriptorParams &descLayout,
    const std::unordered_map<ResourceTypes, std::shared_ptr<MemoryObject>> &meshBuffers,
    const std::unordered_map<ResourceKey, std::shared_ptr<MemoryObject>, ResourceKeyHasher> &descResources,
    const std::unordered_map<std::string, std::vector<uint32_t>> &shadersSpirv) :
    m_device(device), m_renderPass(renderPass), m_inputLayout(inputLayout), m_descLayout(descLayout),
    m_meshBuffers(meshBuffers), m_descResources(descResources), m_shadersSpirv(shadersSpirv)
{
    createDescriptorPool();
    createDescriptorSetLayout();
    createDescriptorSets();
    uint32_t maxFrame = m_descLayout.MaxFrameCountInFlight();
    m_pushConstantsData.assign(maxFrame, std::vector<uint8_t>(physicalProp.maxPushConstantSize));
    createGraphicsPipeline(physicalProp.msaaSamples);
    m_isRendpassReady = false;
}

GraphicsPipeline::~GraphicsPipeline() {}

void GraphicsPipeline::createGraphicsPipeline(vk::SampleCountFlagBits msaaSamples)
{
    // Vertex
    const std::vector<uint32_t> &vertShaderCode = m_shadersSpirv[" vert"];
    auto vertShaderModule = createShaderModule(vertShaderCode);
    vk::PipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.stage = vk::ShaderStageFlagBits::eVertex;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";
    // Fragment
    const std::vector<uint32_t> &fragShaderCode = m_shadersSpirv[" frag"];
    auto fragShaderModule = createShaderModule(fragShaderCode);
    vk::PipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.stage = vk::ShaderStageFlagBits::eFragment;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";
    // add all shader here
    std::vector<vk::PipelineShaderStageCreateInfo> shaderStages = {vertShaderStageInfo, fragShaderStageInfo};

    vk::PipelineVertexInputStateCreateInfo vertexInputInfo = m_inputLayout.getPipelineVertexInputStateCreateInfo();

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
    multisampling.rasterizationSamples = msaaSamples; // vk::SampleCountFlagBits::e1;

    vk::PipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = vk::CompareOp::eLess;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;
    vk::PipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG
                                          | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;

    vk::PipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.logicOp = vk::LogicOp::eCopy;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;
    colorBlending.blendConstants[0] = 0.0f;
    colorBlending.blendConstants[1] = 0.0f;
    colorBlending.blendConstants[2] = 0.0f;
    colorBlending.blendConstants[3] = 0.0f;

    std::vector<vk::DynamicState> dynamicStates = {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    std::vector<vk::DescriptorSetLayout> descriptorSetLayouts{};
    for (auto sp : m_descriptorSetLayouts) {
        descriptorSetLayouts.push_back(*sp);
    }

    vk::PipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.setLayoutCount = descriptorSetLayouts.size();
    pipelineLayoutInfo.pSetLayouts = descriptorSetLayouts.data();

    auto pushConstantRanges = m_descLayout.getPushConstantRanges();
    pipelineLayoutInfo.setPushConstantRangeCount(pushConstantRanges.size());
    pipelineLayoutInfo.setPPushConstantRanges(pushConstantRanges.data());

    vk::PipelineLayout pipelineLayout{};
    if (m_device->createPipelineLayout(&pipelineLayoutInfo, nullptr, &pipelineLayout) != vk::Result::eSuccess) {
        throw std::runtime_error(" failed to create pipeline layout!");
    }
    m_pipelineLayout = std::shared_ptr<vk::PipelineLayout>(new vk::PipelineLayout(pipelineLayout),
                                                           [device = m_device](vk::PipelineLayout *p) {
                                                               if (p && *p) {
                                                                   device->destroyPipelineLayout(*p, nullptr);
                                                               }
                                                               delete p;
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
    pipelineInfo.layout = *m_pipelineLayout;
    pipelineInfo.renderPass = *m_renderPass;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

    vk::Pipeline graphicsPipeline{};
    if (m_device->createGraphicsPipelines(VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline)
        != vk::Result::eSuccess) {
        throw std::runtime_error("failed to create graphics pipeline!");
    }

    m_graphicsPipeline = std::shared_ptr<vk::Pipeline>(
        new vk::Pipeline(graphicsPipeline), [device = m_device, pipelineLayout = m_pipelineLayout](vk::Pipeline *p) {
            if (p && *p) {
                device->destroyPipeline(*p, nullptr);
            }
            delete p;
        });

    m_device->destroyShaderModule(fragShaderModule, nullptr);
    m_device->destroyShaderModule(vertShaderModule, nullptr);
}

vk::ShaderModule GraphicsPipeline::createShaderModule(const std::vector<uint32_t> &code)
{
    vk::ShaderModuleCreateInfo createInfo{};
    createInfo.codeSize = code.size() * sizeof(uint32_t);
    createInfo.pCode = code.data();
    vk::ShaderModule shaderModule{};
    if (m_device->createShaderModule(&createInfo, nullptr, &shaderModule) != vk::Result::eSuccess) {
        throw std::runtime_error(" failed to create shader module!");
    }
    return shaderModule;
}

void GraphicsPipeline::UploadInputResourceData(ResourceTypes resourceType, const void *data, size_t size)
{
    if (IsInputResource(resourceType)) {
        m_meshBuffers[resourceType]->updateRawData(data);
    } else {
        throw std::runtime_error(" error resourceType");
    }
}

void GraphicsPipeline::UploadDescriptorResourceData(ResourceId resourceId, uint32_t currentFrame, const void *data,
                                                    size_t size)
{
    ResourceKey key = ResourceKey::make_key(resourceId, currentFrame);
    auto it = m_descResources.find(key);
    if (it != m_descResources.end()) {
        m_descResources[key]->updateRawData(data);
    }
}

void GraphicsPipeline::UploadConstantsResource(ResourceTypes resourceType, uint32_t currentFrame, const void *data,
                                               size_t size)
{
    if (IsConstantsResource(resourceType)) {
        // std:: cout << "UploadConstar" Re" our"
        memcpy(this->m_pushConstantsData[currentFrame].data(), data, size);
    } else {
        throw std::runtime_error(" error resourceType");
    }
}

void GraphicsPipeline::recordUploadCommand(const vk::CommandBuffer &commandBuffer, uint32_t currentFrame)
{
    for (const auto &[key, object] : m_meshBuffers) {
        object->recordCopyFromStagingToDevice(commandBuffer);
    }

    for (const auto &[key, object] : m_descResources) {
        object->recordCopyFromStagingToDevice(commandBuffer);
    }
}

void GraphicsPipeline::recordDrawCommand(const vk::CommandBuffer &commandBuffer, uint32_t currentFrame)
{
    // first BeginRenderPass(), and then
    if (m_isRendpassReady == true) {
        // recordBindCore
        commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, *m_graphicsPipeline);

        // recordBindPush, or UniformBuffer
        recordBindPush(commandBuffer, currentFrame);
        std::vector<vk::DescriptorSet> setsToBind{};
        auto getSetsToBind = [&setsToBind, this](uint32_t currentFrame, uint32_t setId) {
            vk::DescriptorSet targetDescSet = m_descriptorSets[currentFrame][setId];
            setsToBind.push_back(targetDescSet);
        };

        m_descLayout.for_each_set(currentFrame, getSetsToBind);
        // std:: cout << "setsToBind. size(): " << setsToBind. size() << "\n";

        commandBuffer.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *m_pipelineLayout, 0, setsToBind.size(),
                                         setsToBind.data(), 0, nullptr);

        // recordBindBuffer
        for (const auto &[key, object] : m_meshBuffers) {
            if (IsInputResource(key)) {
                m_meshBuffers[key]->bindToPipeline(commandBuffer);
            }
        }

        // std:: cout << " check IndexCount:  " << m_inputLayout. getIndexCount() << "\n";
        // std:: co ut << " check VertexCount:  " << m_inputLayout. getVertexCount() << "\n";
        if (m_inputLayout.getIndexCount() > 0) {
            commandBuffer.drawIndexed(m_inputLayout.getIndexCount(), 1, 0, 0, 0);
        } else {
            commandBuffer.draw(m_inputLayout.getVertexCount(), 1, 0, 0);
        }
    }
    // final, EndRenderPass()
}

void GraphicsPipeline::recordBindPush(const vk::CommandBuffer &commandBuffer, uint32_t currentFrame)
{
    auto pushConstantRanges = m_descLayout.getPushConstantRanges();
    // std:: co ut << "pushConstantRanges[0]. size:  " << pushConstantRanges[0]. size << "\n";
    commandBuffer.pushConstants(*m_pipelineLayout, pushConstantRanges[0].stageFlags, 0, pushConstantRanges[0].size,
                                m_pushConstantsData[currentFrame].data());
}

void GraphicsPipeline::BeginRenderPass(const vk::CommandBuffer &commandBuffer,
                                       std::shared_ptr<vk::Framebuffer> framebuffer,
                                       std::shared_ptr<vk::Extent2D> swapChainExtent)
{
    vk::RenderPassBeginInfo renderPassInfo{};
    renderPassInfo.renderPass = *m_renderPass;
    renderPassInfo.framebuffer = *framebuffer;
    renderPassInfo.renderArea.offset = vk::Offset2D(0, 0);
    renderPassInfo.renderArea.extent = *swapChainExtent;

    std::array<vk::ClearValue, 2> clearValues{};
    clearValues[0].color = vk::ClearColorValue{std::array<float, 4>{0.8f, 0.7f, 0.56f, 1.0f}};
    clearValues[1].depthStencil = vk::ClearDepthStencilValue{1.0f, 0};
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    // begin rederpass
    commandBuffer.beginRenderPass(&renderPassInfo, vk::SubpassContents::eInline);
    vk::Viewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapChainExtent->width);
    viewport.height = static_cast<float>(swapChainExtent->height);
    // std:: cout << " viewport width: " << viewport. width << ", height" << viewport. height << std:: endl;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    commandBuffer.setViewport(0, 1, &viewport);
    vk::Rect2D scissor{};
    scissor.offset = vk::Offset2D(0, 0);
    scissor.extent = *swapChainExtent;
    commandBuffer.setScissor(0, 1, &scissor);
    m_isRendpassReady = true;
}

void GraphicsPipeline::EndRenderPass(const vk::CommandBuffer &commandBuffer)
{
    // end rederpass
    commandBuffer.endRenderPass();
    m_isRendpassReady = false;
}

void GraphicsPipeline::createDescriptorSetLayout()
{
    auto setsLayoutCreateInfo = m_descLayout.getSetsLayoutCreateInfo();
    auto setsCount = m_descLayout.totalSetsCountEachFrame();
    m_descriptorSetLayouts.resize(setsCount);
    for (uint32_t i = 0; i < setsCount; i++) {
        vk::DescriptorSetLayout descriptorSetLayout{};
        if (m_device->createDescriptorSetLayout(&setsLayoutCreateInfo[i], nullptr, &descriptorSetLayout)
            != vk::Result::eSuccess) {
            throw std::runtime_error("failed to create descriptor set layout !");
        }

        m_descriptorSetLayouts[i] = std::shared_ptr<vk::DescriptorSetLayout>(
            new vk::DescriptorSetLayout(descriptorSetLayout), [device = m_device](vk::DescriptorSetLayout *p) {
                if (p && *p) {
                    device->destroyDescriptorSetLayout(*p, nullptr);
                }
                delete p;
            });
    }
}

void GraphicsPipeline::createDescriptorPool()
{
    vk::DescriptorPoolCreateInfo poolInfo = m_descLayout.getDescriptorPoolCreateInfo();
    vk::DescriptorPool descriptorPool;
    if (m_device->createDescriptorPool(&poolInfo, nullptr, &descriptorPool) != vk::Result::eSuccess) {
        throw std::runtime_error(" failed to create descriptor pool!");
    }

    m_descriptorPool = std::shared_ptr<vk::DescriptorPool>(new vk::DescriptorPool(descriptorPool),
                                                           [device = m_device](vk::DescriptorPool *p) {
                                                               if (p && *p) {
                                                                   device->destroyDescriptorPool(*p, nullptr);
                                                               }
                                                               delete p;
                                                           });
}

void GraphicsPipeline::createDescriptorSets()
{
    uint32_t maxFrame = m_descLayout.MaxFrameCountInFlight();
    auto setsCount = m_descLayout.totalSetsCountEachFrame();
    std::vector<vk::DescriptorSetLayout> layouts{};
    for (uint32_t i = 0; i < maxFrame; i++) {
        for (uint32_t j = 0; j < setsCount; j++) {
            layouts.push_back(*m_descriptorSetLayouts[j]);
        }
    }

    vk::DescriptorSetAllocateInfo allocInfo{};
    allocInfo.descriptorPool = *m_descriptorPool;
    allocInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocInfo.pSetLayouts = layouts.data();
    auto descriptorSets = std::move(m_device->allocateDescriptorSets(allocInfo));

    m_descriptorSets.resize(maxFrame);
    for (uint32_t i = 0; i < maxFrame; i++) {
        m_descriptorSets[i].resize(setsCount);
        for (uint32_t j = 0; j < setsCount; j++) {
            m_descriptorSets[i][j] = descriptorSets[i * setsCount + j];
        }
    }

    std::vector<vk::WriteDescriptorSet> descriptorWrites{};
    auto constructDescriptorSet = [&descriptorWrites, this](uint32_t currentFrame, ResourceId resourceId,
                                                            vk::DescriptorType descriptorType, const auto &spec) {
        using T = std::decay_t<decltype(spec)>;
        ResourceKey key = ResourceKey::make_key(resourceId, currentFrame);
        vk::DescriptorSet targetDescSet = m_descriptorSets[currentFrame][resourceId.getSetId()];
        // if constexpr (std·· is   same  v<T, GBufferSpec>) {
        auto it = m_descResources.find(key);
        if (it != m_descResources.end()) {
            descriptorWrites.push_back(
                it->second->constructDescriptorSet(targetDescSet, descriptorType, resourceId.getBindingId()));
        }
    };

    for (size_t i = 0; i < maxFrame; i++) {
        m_descLayout.for_each_binding(i, constructDescriptorSet);
    }

    m_device->updateDescriptorSets(descriptorWrites.size(), descriptorWrites.data(), 0, nullptr);
}
