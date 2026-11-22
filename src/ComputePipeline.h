#pragma once
#include <memory>
#include <vulkan/vulkan.hpp>
#include "DescriptorBindingState.h"
#include "IDescriptorSetDetail.h"
#include "ShaderAssetGroup.h"
#include "VulkanHandle.h"
#include "typedefs.h"

// Compute pipeline sharing the same descriptor-layout / asset model as the
// graphics pipeline.  spirvEntry chooses which SPIR-V module in the asset to
// use (allows multiple compute stages sharing one asset).
class ComputePipeline {
public:
    ComputePipeline(std::shared_ptr<vk::Device> device,
                    std::shared_ptr<const IDescriptorSetLayoutGroup> descriptorLayout,
                    std::shared_ptr<ShaderAssetGroup> asset, const std::string &spirvEntry = "comp");
    ~ComputePipeline() = default;

    void builtUp();
    void recordDispatchCommand(const vk::CommandBuffer &cmd, uint32_t frame, const DispatchSize &size,
                               DescriptorBindingState &bindingState);

    vk::PipelineLayout pipelineLayout() const { return m_pipelineLayout ? *m_pipelineLayout : vk::PipelineLayout{}; }
    const std::shared_ptr<ShaderAssetGroup> &asset() const { return m_asset; }

private:
    void createSetLayouts();
    void createDescriptorPool();
    void allocateAndWriteSets();
    void createPipeline();

private:
    std::shared_ptr<vk::Device> m_device;
    std::shared_ptr<const IDescriptorSetLayoutGroup> m_descriptorLayout;
    std::shared_ptr<ShaderAssetGroup> m_asset;
    std::string m_spirvEntry;

    std::vector<VulkanHandle<vk::DescriptorSetLayout, std::function<void(vk::DescriptorSetLayout)>>> m_setLayouts;
    VulkanHandle<vk::DescriptorPool, std::function<void(vk::DescriptorPool)>> m_descriptorPool;
    std::vector<std::vector<vk::DescriptorSet>> m_descriptorSets;
    VulkanHandle<vk::PipelineLayout, std::function<void(vk::PipelineLayout)>> m_pipelineLayout;
    VulkanHandle<vk::Pipeline, std::function<void(vk::Pipeline)>> m_pipeline;
};
