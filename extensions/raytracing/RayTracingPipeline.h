#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "RayTracingCommon.h"
#include "ShaderBindingTable.h"

namespace rt {

struct ShaderStageDesc {
    vk::ShaderStageFlagBits stage = vk::ShaderStageFlagBits::eRaygenKHR;
    std::vector<uint32_t> spirv;
    std::string entry = "main";
    // Specialization constants, used by raytracingreflections to pass the bounce
    // count (constant_id = 0) without recompiling (see 31.5).
    std::vector<vk::SpecializationMapEntry> specEntries;
    std::vector<uint8_t> specData;
};

struct PipelineDesc {
    std::string debugName;
    std::vector<ShaderStageDesc> stages;
    std::vector<ShaderGroupDesc> raygen;
    std::vector<ShaderGroupDesc> miss;
    std::vector<ShaderGroupDesc> hit;
    std::vector<ShaderGroupDesc> callable;
    uint32_t maxRecursionDepth = 1;
    // Owned by the caller; the pipeline only borrows the layout.
    vk::DescriptorSetLayout descriptorLayout{};
    vk::PipelineLayout pipelineLayout{};
};

// A KHR ray tracing pipeline together with its shader binding table.  Building
// them together is deliberate: the SBT handle index is only meaningful relative
// to the exact group order the pipeline was created with, so keeping the two in
// one object removes an entire class of ordering bugs (see 31.4).
class RayTracingPipeline {
public:
    void Create(vk::Device device, const vk::PhysicalDeviceMemoryProperties &memProps, const Dispatch &dispatch,
                const Capabilities &caps, const PipelineDesc &desc);
    void Destroy(vk::Device device, const Dispatch &dispatch);

    void Bind(vk::CommandBuffer cmd, vk::DescriptorSet descriptorSet) const;
    void RecordTrace(vk::CommandBuffer cmd, uint32_t width, uint32_t height, uint32_t depth = 1) const;

    vk::Pipeline pipeline() const { return m_pipeline; }
    const ShaderBindingLayout &layout() const { return m_layout; }

private:
    vk::Pipeline m_pipeline{};
    vk::PipelineLayout m_pipelineLayout{};
    vk::Buffer m_sbtBuffer{};
    vk::DeviceMemory m_sbtMemory{};
    void *m_sbtMapped = nullptr;
    // Padding needed when the buffer's device address is not already a multiple
    // of shaderGroupBaseAlignment.  Region offsets are relative to this base.
    uint64_t m_sbtBaseOffset = 0;

    ShaderBindingLayout m_layout{};
    vk::StridedDeviceAddressRegionKHR m_raygen{};
    vk::StridedDeviceAddressRegionKHR m_miss{};
    vk::StridedDeviceAddressRegionKHR m_hit{};
    vk::StridedDeviceAddressRegionKHR m_callable{};
    PFN_vkCmdTraceRaysKHR m_cmdTraceRays = nullptr;
};

} // namespace rt
