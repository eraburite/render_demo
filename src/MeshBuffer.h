#pragma once
#include <cstdint>
#include <memory>
#include <vulkan/vulkan.hpp>
#include "typedefs.h"
#include "MemoryObject.h"

class MeshBuffer : public MemoryObject {
public:
    MeshBuffer(std::shared_ptr<vk::Device> device, const vk::PhysicalDeviceMemoryProperties &memProperties,
               ResourceTypes resourceType, MemoryTypes memoryType, GBufferSpec buffSpec);
    virtual ~MeshBuffer();

    void recordCopyFromStagingToDevice(const vk::CommandBuffer &commandBuffer) override;
    void recordCopyFromDeviceToStaging(const vk::CommandBuffer &commandBuffer) override;
    void bindToPipeline(const vk::CommandBuffer &commandBuffer) override;
    vk::WriteDescriptorSet constructDescriptorSet(vk::DescriptorSet descriptorSet, vk::DescriptorType descriptorType,
                                                  uint32_t binding) override;

private:
    vk::BufferUsageFlags getPrimaryBufferUsageFlags();
    void reserve(vk::PhysicalDeviceMemoryProperties memProperties);
    void recordCopyBuffer(const vk::CommandBuffer &commandBuffer, vk::Buffer srcBuffer, vk::Buffer dstBuffer,
                          vk::BufferCopy copyRegion);
    void recordBufferMemoryBarrier(const vk::CommandBuffer &commandBuffer, const vk::Buffer &buffer,
                                   vk::AccessFlagBits srcAccessMask, vk::AccessFlagBits dstAccessMask,
                                   vk::PipelineStageFlagBits srcStageMask, vk::PipelineStageFlagBits dstStageMask);

    vk::IndexType getVkIndexType(); // for eIndexBuffer

private:
    GBufferSpec m_buffSpec;
    vk::DescriptorBufferInfo m_descriptorBufferInfo{};
    std::shared_ptr<vk::Buffer> m_stagingBuffer;
    std::shared_ptr<vk::Buffer> m_primaryBuffer;
};