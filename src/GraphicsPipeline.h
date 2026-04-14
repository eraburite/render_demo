#pragma once
#include <memory>
#include <unordered_map>
#include <vulkan/vulkan.hpp>
#include "MemoryObject.h"
#include "VertexInputStructs.h"
#include "DescriptorStructs.h"

struct DeviceFeatureProperty {
    vk::SampleCountFlagBits msaaSamples = vk::SampleCountFlagBits::e1;
    uint32_t maxPushConstantSize = 0;
};

class GraphicsPipeline {
public:
    GraphicsPipeline(std::shared_ptr<vk::Device> device, DeviceFeatureProperty physicalProp,
                     std::shared_ptr<vk::RenderPass> renderPass, const VertexInputParam &inputLayout,
                     const PipelineDescriptorParams &descLayout,
                     const std::unordered_map<ResourceTypes, std::shared_ptr<MemoryObject>> &meshBuffers,
                     const std::unordered_map<ResourceKey, std::shared_ptr<MemoryObject>, ResourceKeyHasher> &descRes,
                     const std::unordered_map<std::string, std::vector<uint32_t>> &shadersSpirv);

    ~GraphicsPipeline();

    void BeginRenderPass(const vk::CommandBuffer &commandBuffer, std::shared_ptr<vk::Framebuffer> framebuffer,
                         std::shared_ptr<vk::Extent2D> swapChainExtent);
    void EndRenderPass(const vk::CommandBuffer &commandBuffer);

    void UploadInputResourceData(ResourceTypes resourceType, const void *data, size_t size);
    void UploadDescriptorResourceData(ResourceId resourceId, uint32_t currentFrame, const void *data, size_t size);
    void UploadConstantsResource(ResourceTypes resourceType, uint32_t currentFrame, const void *data, size_t size);

    void recordUploadCommand(const vk::CommandBuffer &commandBuffer, uint32_t currentFrame);
    void recordDrawCommand(const vk::CommandBuffer &commandBuffer, uint32_t currentFrame);

private:
    vk::ShaderModule createShaderModule(const std::vector<uint32_t> &code);
    void createDescriptorSetLayout(); // createParameters: ubo, ssbo, pushconst, ...
    void createDescriptorPool();
    void createDescriptorSets();
    void createGraphicsPipeline(vk::SampleCountFlagBits msaaSamples);
    void recordBindPush(const vk::CommandBuffer &commandBuffer, uint32_t currentFrame);

private:
    std::shared_ptr<vk::Device> m_device;
    std::shared_ptr<vk::RenderPass> m_renderPass; // TODO: multi-renderpass
    bool m_isRendpassReady;                       // TODO: multi-renderpass
    // uint32_t m indexCount; // indices. size(), TODO: multi-Objects
    VertexInputParam m_inputLayout;
    PipelineDescriptorParams m_descLayout;
    // for input: vertex and index
    std::unordered_map<ResourceTypes, std::shared_ptr<MemoryObject>> m_meshBuffers;
    // for descriptor: texture or uniform
    std::unordered_map<ResourceKey, std::shared_ptr<MemoryObject>, ResourceKeyHasher> m_descResources;
    // for shader, valid name list: vert, frag, geom, tese, tesc
    std::unordered_map<std::string, std::vector<uint32_t>> m_shadersSpirv;
    std::vector<std::vector<uint8_t>> m_pushConstantsData;
    std::vector<std::shared_ptr<vk::DescriptorSetLayout>> m_descriptorSetLayouts;
    std::shared_ptr<vk::DescriptorPool> m_descriptorPool;
    // for Double-bufering-rendering, descriptorSets layouts
    // inflightFrame0 - > Set0, Set1, Set2
    // infilghtFrame1 - > Set0, Set1, Set2
    std::vector<std::vector<vk::DescriptorSet>> m_descriptorSets;
    std::shared_ptr<vk::PipelineLayout> m_pipelineLayout;
    std::shared_ptr<vk::Pipeline> m_graphicsPipeline;
};