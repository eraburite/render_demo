#include "RayTracingPipeline.h"

#include <cstring>
#include <stdexcept>

namespace rt {
namespace {

vk::RayTracingShaderGroupTypeKHR ToGroupType(ShaderGroupType type)
{
    switch (type) {
    case ShaderGroupType::eTrianglesHit:
        return vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup;
    case ShaderGroupType::eProceduralHit:
        return vk::RayTracingShaderGroupTypeKHR::eProceduralHitGroup;
    case ShaderGroupType::eGeneral:
    default:
        return vk::RayTracingShaderGroupTypeKHR::eGeneral;
    }
}

uint32_t ToIndexOrUnused(int index)
{
    return index < 0 ? VK_SHADER_UNUSED_KHR : static_cast<uint32_t>(index);
}

ShaderBindingLayout::RegionSpec ToRegionSpec(const std::vector<ShaderGroupDesc> &groups)
{
    ShaderBindingLayout::RegionSpec spec;
    spec.groupCount = static_cast<uint32_t>(groups.size());
    spec.recordDataSizes.reserve(groups.size());
    for (const ShaderGroupDesc &group : groups) {
        spec.recordDataSizes.push_back(static_cast<uint32_t>(group.recordData.size()));
    }
    return spec;
}

} // namespace

void RayTracingPipeline::Create(vk::Device device, const vk::PhysicalDeviceMemoryProperties &memProps,
                                const Dispatch &dispatch, const Capabilities &caps, const PipelineDesc &desc)
{
    if (desc.stages.empty() || desc.raygen.empty()) {
        throw std::runtime_error("rt: a ray tracing pipeline needs at least one stage and one raygen group");
    }
    if (desc.maxRecursionDepth == 0) {
        throw std::runtime_error("rt: maxPipelineRayRecursionDepth must be at least 1");
    }

    m_pipelineLayout = desc.pipelineLayout;
    m_cmdTraceRays = dispatch.cmdTraceRays;

    // ---- shader modules + stages ------------------------------------------
    std::vector<vk::ShaderModule> modules;
    modules.reserve(desc.stages.size());
    for (const ShaderStageDesc &stage : desc.stages) {
        vk::ShaderModuleCreateInfo moduleInfo{};
        moduleInfo.codeSize = stage.spirv.size() * sizeof(uint32_t);
        moduleInfo.pCode = stage.spirv.data();
        modules.push_back(device.createShaderModule(moduleInfo));
    }

    // SpecializationInfo objects must outlive the vkCreateRayTracingPipelinesKHR
    // call, so they live in a reserved vector here rather than in loop scope.
    std::vector<vk::SpecializationInfo> specializationInfos;
    specializationInfos.reserve(desc.stages.size());

    std::vector<vk::PipelineShaderStageCreateInfo> shaderStages;
    shaderStages.reserve(desc.stages.size());
    for (size_t i = 0; i < desc.stages.size(); ++i) {
        const ShaderStageDesc &stage = desc.stages[i];
        vk::PipelineShaderStageCreateInfo stageInfo{};
        stageInfo.stage = stage.stage;
        stageInfo.module = modules[i];
        stageInfo.pName = stage.entry.c_str();
        if (!stage.specEntries.empty()) {
            vk::SpecializationInfo specInfo{};
            specInfo.mapEntryCount = static_cast<uint32_t>(stage.specEntries.size());
            specInfo.pMapEntries = stage.specEntries.data();
            specInfo.dataSize = stage.specData.size();
            specInfo.pData = stage.specData.data();
            specializationInfos.push_back(specInfo);
            stageInfo.pSpecializationInfo = &specializationInfos.back();
        }
        shaderStages.push_back(stageInfo);
    }

    // ---- shader groups, in raygen -> miss -> hit -> callable order --------
    std::vector<vk::RayTracingShaderGroupCreateInfoKHR> shaderGroups;
    const auto appendGroups = [&shaderGroups](const std::vector<ShaderGroupDesc> &groups) {
        for (const ShaderGroupDesc &group : groups) {
            vk::RayTracingShaderGroupCreateInfoKHR groupInfo{};
            groupInfo.type = ToGroupType(group.type);
            groupInfo.generalShader = ToIndexOrUnused(group.general);
            groupInfo.closestHitShader = ToIndexOrUnused(group.closestHit);
            groupInfo.anyHitShader = ToIndexOrUnused(group.anyHit);
            groupInfo.intersectionShader = ToIndexOrUnused(group.intersection);
            shaderGroups.push_back(groupInfo);
        }
    };
    appendGroups(desc.raygen);
    appendGroups(desc.miss);
    appendGroups(desc.hit);
    appendGroups(desc.callable);

    vk::RayTracingPipelineCreateInfoKHR pipelineInfo{};
    pipelineInfo.sType = vk::StructureType::eRayTracingPipelineCreateInfoKHR;
    pipelineInfo.stageCount = static_cast<uint32_t>(shaderStages.size());
    pipelineInfo.pStages = shaderStages.data();
    pipelineInfo.groupCount = static_cast<uint32_t>(shaderGroups.size());
    pipelineInfo.pGroups = shaderGroups.data();
    pipelineInfo.maxPipelineRayRecursionDepth = desc.maxRecursionDepth;
    pipelineInfo.layout = m_pipelineLayout;

    VkPipeline rawPipeline = VK_NULL_HANDLE;
    const VkResult result = dispatch.createRayTracingPipelines(
        static_cast<VkDevice>(device), VK_NULL_HANDLE, VK_NULL_HANDLE, 1,
        reinterpret_cast<const VkRayTracingPipelineCreateInfoKHR *>(&pipelineInfo), nullptr, &rawPipeline);
    if (result != VK_SUCCESS) {
        for (vk::ShaderModule module : modules) {
            device.destroyShaderModule(module);
        }
        throw std::runtime_error("rt: vkCreateRayTracingPipelinesKHR failed");
    }
    m_pipeline = vk::Pipeline{rawPipeline};

    for (vk::ShaderModule module : modules) {
        device.destroyShaderModule(module);
    }

    // ---- SBT layout (pure CPU) --------------------------------------------
    m_layout = ShaderBindingLayout::Compute(caps, ToRegionSpec(desc.raygen), ToRegionSpec(desc.miss),
                                            ToRegionSpec(desc.hit), ToRegionSpec(desc.callable));
    if (!m_layout.IsValid()) {
        throw std::runtime_error("rt: computed shader binding table layout is invalid");
    }

    // ---- SBT backing buffer -----------------------------------------------
    // One extra baseAlignment of slack lets the first region start on a
    // base-aligned device address even when the allocator's address is not.
    const vk::DeviceSize allocationSize = m_layout.totalSize + m_layout.baseAlignment;
    vk::BufferCreateInfo bufferInfo{};
    bufferInfo.size = allocationSize;
    bufferInfo.usage = vk::BufferUsageFlagBits::eShaderBindingTableKHR | vk::BufferUsageFlagBits::eShaderDeviceAddress;
    bufferInfo.sharingMode = vk::SharingMode::eExclusive;
    m_sbtBuffer = device.createBuffer(bufferInfo);

    const vk::MemoryRequirements requirements = device.getBufferMemoryRequirements(m_sbtBuffer);
    vk::MemoryAllocateFlagsInfo allocateFlags{};
    allocateFlags.flags = vk::MemoryAllocateFlagBits::eDeviceAddress;
    vk::MemoryAllocateInfo allocateInfo{};
    allocateInfo.pNext = &allocateFlags;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(
        memProps, requirements.memoryTypeBits,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    m_sbtMemory = device.allocateMemory(allocateInfo);
    device.bindBufferMemory(m_sbtBuffer, m_sbtMemory, 0);
    m_sbtMapped = device.mapMemory(m_sbtMemory, 0, VK_WHOLE_SIZE);

    const uint64_t rawAddress = dispatch.BufferDeviceAddress(device, m_sbtBuffer);
    m_sbtBaseOffset = AlignUp64(rawAddress, m_layout.baseAlignment) - rawAddress;

    // ---- fill the records --------------------------------------------------
    const uint32_t handleSize = m_layout.handleSize;
    const uint32_t handleStride = m_layout.handleSizeAligned;
    std::vector<uint8_t> handles(static_cast<size_t>(m_layout.totalGroupCount) * handleSize);
    const VkResult handleResult = dispatch.getRayTracingShaderGroupHandles(
        static_cast<VkDevice>(device), static_cast<VkPipeline>(m_pipeline), 0, m_layout.totalGroupCount,
        handles.size(), handles.data());
    if (handleResult != VK_SUCCESS) {
        throw std::runtime_error("rt: vkGetRayTracingShaderGroupHandlesKHR failed");
    }

    uint8_t *base = static_cast<uint8_t *>(m_sbtMapped) + m_sbtBaseOffset;
    const auto writeRegion = [&](const ShaderBindingLayout::Region &region,
                                 const std::vector<ShaderGroupDesc> &groups) {
        for (uint32_t i = 0; i < region.groupCount; ++i) {
            const uint32_t groupIndex = region.firstGroup + i;
            uint8_t *destination = base + region.offset + static_cast<uint64_t>(i) * region.recordStride;
            std::memcpy(destination, handles.data() + static_cast<size_t>(groupIndex) * handleSize, handleSize);
            const std::vector<uint8_t> &recordData = groups[i].recordData;
            if (!recordData.empty()) {
                std::memcpy(destination + handleStride, recordData.data(), recordData.size());
            }
        }
    };
    writeRegion(m_layout.raygen, desc.raygen);
    writeRegion(m_layout.miss, desc.miss);
    writeRegion(m_layout.hit, desc.hit);
    writeRegion(m_layout.callable, desc.callable);

    // ---- strided regions ---------------------------------------------------
    const uint64_t alignedBase = rawAddress + m_sbtBaseOffset;
    const auto makeRegion = [&alignedBase](const ShaderBindingLayout::Region &region) {
        vk::StridedDeviceAddressRegionKHR strided{};
        if (region.groupCount == 0) {
            return strided;
        }
        strided.deviceAddress = alignedBase + region.offset;
        strided.stride = region.recordStride;
        strided.size = region.byteSize;
        return strided;
    };
    m_raygen = makeRegion(m_layout.raygen);
    m_miss = makeRegion(m_layout.miss);
    m_hit = makeRegion(m_layout.hit);
    m_callable = makeRegion(m_layout.callable);
}

void RayTracingPipeline::Bind(vk::CommandBuffer cmd, vk::DescriptorSet descriptorSet) const
{
    cmd.bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, m_pipeline);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eRayTracingKHR, m_pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
}

void RayTracingPipeline::RecordTrace(vk::CommandBuffer cmd, uint32_t width, uint32_t height, uint32_t depth) const
{
    if (m_cmdTraceRays == nullptr) {
        throw std::runtime_error("rt: vkCmdTraceRaysKHR is not available");
    }
    m_cmdTraceRays(static_cast<VkCommandBuffer>(cmd),
                   reinterpret_cast<const VkStridedDeviceAddressRegionKHR *>(&m_raygen),
                   reinterpret_cast<const VkStridedDeviceAddressRegionKHR *>(&m_miss),
                   reinterpret_cast<const VkStridedDeviceAddressRegionKHR *>(&m_hit),
                   reinterpret_cast<const VkStridedDeviceAddressRegionKHR *>(&m_callable), width, height, depth);
}

void RayTracingPipeline::Destroy(vk::Device device, const Dispatch &dispatch)
{
    if (m_sbtMapped != nullptr) {
        device.unmapMemory(m_sbtMemory);
        m_sbtMapped = nullptr;
    }
    if (m_sbtBuffer) {
        device.destroyBuffer(m_sbtBuffer);
        m_sbtBuffer = nullptr;
    }
    if (m_sbtMemory) {
        device.freeMemory(m_sbtMemory);
        m_sbtMemory = nullptr;
    }
    if (m_pipeline) {
        device.destroyPipeline(m_pipeline);
        m_pipeline = nullptr;
    }
    (void)dispatch;
}

} // namespace rt
