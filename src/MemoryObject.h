#pragma once
#include <cstdint>
#include <memory>
#include <vulkan/vulkan.hpp>
#include "typedefs.h"
class MemoryObject {
public:
    MemoryObject(std::shared_ptr<vk::Device> device, ResourceTypes resourceType, MemoryTypes memoryType,
                 uint32_t totalByte);

    virtual ~MemoryObject() = default;

    void updateRawData(const void *data);
    uint32_t MemorySize() const;
    MemoryTypes MemoryType() const;
    ResourceTypes ResourceType() const;

    virtual void recordCopyFromStagingToDevice(const vk::CommandBuffer &commandBuffer) = 0;
    virtual void recordCopyFromDeviceToStaging(const vk::CommandBuffer &commandBuffer) = 0;
    virtual void bindToPipeline(const vk::CommandBuffer &commandBuffer) = 0;
    virtual vk::WriteDescriptorSet constructDescriptorSet(vk::DescriptorSet descriptorSet,
                                                          vk::DescriptorType descriptorType, uint32_t binding) = 0;

protected:
    void mapRawData();
    void unmapRawData();
    vk::MemoryPropertyFlags getPrimaryMemoryPropertyFlags();
    vk::MemoryPropertyFlags getStagingMemoryPropertyFlags();

protected:
    std::shared_ptr<vk::Device> m_device = nullptr;
    std::shared_ptr<vk::DeviceMemory> m_stagingMemory;
    std::shared_ptr<vk::DeviceMemory> m_primaryMemory;

private:
    ResourceTypes m_resourceType;
    MemoryTypes m_memoryType;
    void *m_rawData; // bufferMemory mapped, in CPU side
    uint32_t m_memoryTotalByte;
    bool m_unmapMemory = false;
};