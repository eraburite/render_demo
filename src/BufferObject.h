#pragma once
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "DeviceMemoryAllocator.h"
#include "DeviceResource.h"
#include "DeviceResourceUtils.h"
#include "typedefs.h"

// VBO/IBO/UBO/SSBO.  Device-local primary buffer + host-visible staging buffer.
// primary buffer may be CONCURRENT across graphics/compute queue families.
class BufferObject : public IDeviceResource {
public:
    BufferObject(std::shared_ptr<vk::Device> device, const vk::PhysicalDeviceMemoryProperties &memProps,
                 std::shared_ptr<DeviceMemoryAllocator> allocator, ResourceTypes resourceType, MemoryTypes memoryType,
                 GBufferSpec spec, const std::vector<uint32_t> *sharedQueueFamilies = nullptr);
    ~BufferObject() override;

    void updateRawData(const void *data, size_t size) override;
    ResourceTypes resourceType() const override { return m_resourceType; }
    size_t byteSize() const { return m_byteSize; }

    bool NeedsUpload() const override { return m_dirty; }
    void OnUploaded() override { m_dirty = false; }

    void OnUploadCompleted() override;
    void OnReadbackRequested() override;

    void *MappedData() override;
    size_t MappedByteSize() const override { return m_byteSize; }

    void recordCopyFromStagingToDevice(const vk::CommandBuffer &cmd) override;
    void recordCopyFromDeviceToStaging(const vk::CommandBuffer &cmd) override;
    void bindToPipeline(const vk::CommandBuffer &cmd) override;
    vk::WriteDescriptorSet constructWriteDescriptorSet(vk::DescriptorSet set, vk::DescriptorType type,
                                                       uint32_t binding) override;
    void accept(DeviceResourceVisitor &visitor) override;

    vk::Buffer primaryBuffer() const { return m_primary ? *m_primary : vk::Buffer{}; }
    vk::Buffer stagingBuffer() const { return m_staging ? *m_staging : vk::Buffer{}; }

private:
    void ensureStaging();
    void releaseStaging();
    vk::BufferUsageFlags primaryUsageFlags() const;
    void recordBarrier(const vk::CommandBuffer &cmd, vk::AccessFlagBits src, vk::AccessFlagBits dst,
                       vk::PipelineStageFlagBits srcStage, vk::PipelineStageFlagBits dstStage);

private:
    std::shared_ptr<vk::Device> m_device;
    std::shared_ptr<DeviceMemoryAllocator> m_allocator;
    vk::PhysicalDeviceMemoryProperties m_memProps;
    ResourceTypes m_resourceType;
    MemoryTypes m_memoryType;
    GBufferSpec m_spec{}; // retained for index-type selection in bindToPipeline
    size_t m_byteSize = 0;
    std::vector<uint32_t> m_sharedQueueFamilies;

    std::shared_ptr<vk::Buffer> m_primary;
    DeviceMemorySlice m_primaryMemory;
    std::shared_ptr<vk::Buffer> m_staging;
    DeviceMemorySlice m_stagingMemory;
    void *m_mapped = nullptr;
    bool m_dirty = false;
    vk::DescriptorBufferInfo m_bufferInfo{};
};
