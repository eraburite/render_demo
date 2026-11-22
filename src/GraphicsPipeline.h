#pragma once
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "DescriptorBindingState.h"
#include "IDescriptorSetDetail.h"
#include "IVertexInputStateGroup.h"
#include "ShaderAssetGroup.h"
#include "VulkanHandle.h"

struct DeviceFeatureProperty {
    vk::SampleCountFlagBits msaaSamples = vk::SampleCountFlagBits::e1;
    uint32_t maxPushConstantSize = 0;
};

class IRenderPass;

class GraphicsPipeline {
public:
    GraphicsPipeline(std::shared_ptr<vk::Device> device, DeviceFeatureProperty feature,
                     std::shared_ptr<const IVertexInputStateGroup> vertexInput,
                     std::shared_ptr<const IDescriptorSetLayoutGroup> descriptorLayout,
                     std::shared_ptr<ShaderAssetGroup> asset,
                     const std::shared_ptr<IRenderPass> &renderPass, uint32_t subpass);
    ~GraphicsPipeline() = default;

    void builtUp();
    void recordDrawCommand(const vk::CommandBuffer &cmd, uint32_t frame, DescriptorBindingState &bindingState);

    vk::PipelineLayout pipelineLayout() const { return m_pipelineLayout ? *m_pipelineLayout : vk::PipelineLayout{}; }
    const std::shared_ptr<ShaderAssetGroup> &asset() const { return m_asset; }

private:
    void createSetLayouts();
    void createDescriptorPool();
    void allocateDescriptorSets();
    void writeDescriptors();
    void createPipeline(const std::shared_ptr<IRenderPass> &renderPass, uint32_t subpass,
                        vk::SampleCountFlagBits msaa);

private:
    std::shared_ptr<vk::Device> m_device;
    DeviceFeatureProperty m_feature;
    std::shared_ptr<const IVertexInputStateGroup> m_vertexInput;
    std::shared_ptr<const IDescriptorSetLayoutGroup> m_descriptorLayout;
    std::shared_ptr<ShaderAssetGroup> m_asset;
    std::shared_ptr<IRenderPass> m_renderPass; // kept alive for pipeline creation / sample count
    uint32_t m_subpass = 0;

    std::vector<VulkanHandle<vk::DescriptorSetLayout, std::function<void(vk::DescriptorSetLayout)>>> m_setLayouts;
    VulkanHandle<vk::DescriptorPool, std::function<void(vk::DescriptorPool)>> m_descriptorPool;
    std::vector<std::vector<vk::DescriptorSet>> m_descriptorSets; // [frame][set]
    VulkanHandle<vk::PipelineLayout, std::function<void(vk::PipelineLayout)>> m_pipelineLayout;
    VulkanHandle<vk::Pipeline, std::function<void(vk::Pipeline)>> m_pipeline;
};
