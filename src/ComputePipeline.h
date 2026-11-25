#pragma once
#include <array>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "DescriptorBindingState.h"
#include "IDescriptorSetDetail.h"
#include "ShaderAssetGroup.h"
#include "VulkanHandle.h"
#include "typedefs.h"

// One specialization constant override for the compute module.  `data` must be
// 4 or 8 bytes and match the constant's type in the SPIR-V module (bool / int /
// uint / float / double / 64-bit integer), which is why the value travels as
// raw bytes instead of a typed field.  Constants the caller does not override
// keep the default written in the shader's `layout(constant_id = N)`.
struct SpecializationConstant {
    uint32_t constantId = 0;
    std::vector<std::byte> data;
};

// Typed sugar for the common 4/8-byte cases: the value is copied, so a literal
// or a local variable can be passed directly.
template <typename T>
SpecializationConstant makeSpecializationConstant(uint32_t constantId, T value)
{
    static_assert(std::is_arithmetic<T>::value, "specialization constants must be arithmetic");
    static_assert(sizeof(T) == 4 || sizeof(T) == 8, "specialization constants must be 4 or 8 bytes");
    SpecializationConstant constant;
    constant.constantId = constantId;
    constant.data.resize(sizeof(T));
    std::memcpy(constant.data.data(), &value, sizeof(T));
    return constant;
}

// Indirect dispatch: the three group counts come from a VkDispatchIndirectCommand
// (3 x uint32) read from `buffer` at `offset`.  Useful when the amount of work is
// itself produced on the GPU (culling, prefix sums), so the CPU never has to read
// it back.
struct IndirectDispatch {
    vk::Buffer buffer{};
    vk::DeviceSize offset = 0;

    bool valid() const { return static_cast<bool>(buffer); }
};

// Optional construction settings.  Everything here is fixed at build time: a
// pipeline can neither be re-specialised nor moved to another cache afterwards.
struct ComputePipelineOptions {
    // Shared PSO cache (RenderManager owns one).  An empty handle compiles from
    // scratch, which is the pre-cache behaviour.
    vk::PipelineCache pipelineCache{};
    std::vector<SpecializationConstant> specializationConstants;
};

// Compute pipeline sharing the same descriptor-layout / asset model as the
// graphics pipeline.  spirvEntry chooses which SPIR-V module in the asset to
// use (allows multiple compute stages sharing one asset).
class ComputePipeline {
public:
    // `limits` is only read for maxComputeWorkGroupCount; pass the device limits
    // (RenderManager does) so an oversized dispatch fails with a readable message
    // instead of a validation error.  A default-constructed value disables the
    // range check, because every limit is then 0.
    ComputePipeline(std::shared_ptr<vk::Device> device,
                    std::shared_ptr<const IDescriptorSetLayoutGroup> descriptorLayout,
                    std::shared_ptr<ShaderAssetGroup> asset, const std::string &spirvEntry = "comp",
                    const vk::PhysicalDeviceLimits &limits = {}, ComputePipelineOptions options = {});
    ~ComputePipeline() = default;

    // Creates the set layouts, pool, sets and the pipeline.  Idempotent: a second
    // call is a no-op, so callers may specialise the pipeline before building it.
    void builtUp();
    bool built() const { return m_built; }

    // Overrides one specialization constant.  Must be called before builtUp().
    void addSpecializationConstant(uint32_t constantId, const void *data, size_t size);

    void recordDispatchCommand(const vk::CommandBuffer &cmd, uint32_t frame, const DispatchSize &size,
                               DescriptorBindingState &bindingState);
    void recordDispatchCommand(const vk::CommandBuffer &cmd, uint32_t frame, const IndirectDispatch &indirect,
                               DescriptorBindingState &bindingState);

    vk::PipelineLayout pipelineLayout() const { return m_pipelineLayout ? *m_pipelineLayout : vk::PipelineLayout{}; }
    vk::Pipeline pipeline() const { return m_pipeline ? *m_pipeline : vk::Pipeline{}; }
    const std::shared_ptr<ShaderAssetGroup> &asset() const { return m_asset; }
    const std::string &spirvEntry() const { return m_spirvEntry; }

private:
    void createSetLayouts();
    void createDescriptorPool();
    void allocateAndWriteSets();
    void createPipeline();

    // Shared prologue of both dispatch flavours: pipeline, push constants, sets.
    void bindResources(const vk::CommandBuffer &cmd, uint32_t frame, DescriptorBindingState &bindingState);
    // Set `setIndex` of `frame`, honouring shared (frame-0-only) sets.
    vk::DescriptorSet descriptorSetFor(uint32_t frame, uint32_t setIndex) const;
    void validateDispatchSize(const DispatchSize &size) const;

private:
    std::shared_ptr<vk::Device> m_device;
    std::shared_ptr<const IDescriptorSetLayoutGroup> m_descriptorLayout;
    std::shared_ptr<ShaderAssetGroup> m_asset;
    std::string m_spirvEntry;
    ComputePipelineOptions m_options;
    std::array<uint32_t, 3> m_maxWorkGroupCount{0, 0, 0};
    bool m_built = false;

    std::vector<VulkanHandle<vk::DescriptorSetLayout, std::function<void(vk::DescriptorSetLayout)>>> m_setLayouts;
    VulkanHandle<vk::DescriptorPool, std::function<void(vk::DescriptorPool)>> m_descriptorPool;
    std::vector<std::vector<vk::DescriptorSet>> m_descriptorSets;
    VulkanHandle<vk::PipelineLayout, std::function<void(vk::PipelineLayout)>> m_pipelineLayout;
    VulkanHandle<vk::Pipeline, std::function<void(vk::Pipeline)>> m_pipeline;
};
