#include "BufferObject.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

BufferObject::BufferObject(std::shared_ptr<vk::Device> device, const vk::PhysicalDeviceMemoryProperties &memProps,
                           std::shared_ptr<DeviceMemoryAllocator> allocator, ResourceTypes resourceType,
                           MemoryTypes memoryType, GBufferSpec spec, const std::vector<uint32_t> *sharedQueueFamilies) :
    m_device(std::move(device)), m_allocator(std::move(allocator)), m_memProps(memProps), m_resourceType(resourceType),
    m_memoryType(memoryType), m_spec(spec), m_byteSize(spec.getTotalByte())
{
    if (sharedQueueFamilies != nullptr) {
        for (uint32_t family : *sharedQueueFamilies) {
            if (std::find(m_sharedQueueFamilies.begin(), m_sharedQueueFamilies.end(), family)
                == m_sharedQueueFamilies.end()) {
                m_sharedQueueFamilies.push_back(family);
            }
        }
    }

    // Primary buffer: device local, usage by resource type.  Both transfer
    // bits are always present (upload path + readback path).
    DeviceMemoryProperty primaryProp{memProps, vk::MemoryPropertyFlagBits::eDeviceLocal};
    BufferBundle primary = DeviceResourceUtils::createBuffer(m_device, m_byteSize, primaryUsageFlags(), primaryProp,
                                                             m_allocator, &m_sharedQueueFamilies);
    m_primary = primary.buffer;
    m_primaryMemory = primary.memory;

    m_bufferInfo.buffer = *m_primary;
    m_bufferInfo.offset = 0;
    m_bufferInfo.range = static_cast<vk::DeviceSize>(m_byteSize);
}

BufferObject::~BufferObject()
{
    releaseStaging();
    m_primary.reset();
    m_primaryMemory = DeviceMemorySlice{};
}

vk::BufferUsageFlags BufferObject::primaryUsageFlags() const
{
    vk::BufferUsageFlags usage{};
    switch (m_resourceType) {
        case ResourceTypes::eVertex:
        case ResourceTypes::eInstanceVertex: usage = vk::BufferUsageFlagBits::eVertexBuffer; break;
        case ResourceTypes::eIndex: usage = vk::BufferUsageFlagBits::eIndexBuffer; break;
        case ResourceTypes::eUniform: usage = vk::BufferUsageFlagBits::eUniformBuffer; break;
        case ResourceTypes::eStorage: usage = vk::BufferUsageFlagBits::eStorageBuffer; break;
        default: throw std::runtime_error("invalid buffer resource type");
    }
    return usage | vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;
}

void BufferObject::ensureStaging()
{
    if (m_staging && m_mapped != nullptr) {
        return;
    }
    if (!m_staging) {
        // Staging is short-lived, so use a dedicated host-visible allocation.
        DeviceMemoryProperty stagingProp{m_memProps, vk::MemoryPropertyFlagBits::eHostVisible
                                                    | vk::MemoryPropertyFlagBits::eHostCoherent};
        BufferBundle staging = DeviceResourceUtils::createBuffer(
            m_device, m_byteSize, vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
            stagingProp, nullptr, nullptr);
        m_staging = staging.buffer;
        m_stagingMemory = staging.memory;
    }

    void *data = nullptr;
    vk::Result result = m_device->mapMemory(*m_stagingMemory.memory, m_stagingMemory.offset, m_byteSize,
                                            vk::MemoryMapFlags{}, &data);
    if (result != vk::Result::eSuccess || data == nullptr) {
        throw std::runtime_error("failed to map staging buffer memory");
    }
    m_mapped = data;
}

void BufferObject::releaseStaging()
{
    if (m_mapped != nullptr && m_stagingMemory.valid()) {
        // Host-coherent memory needs no explicit flush.
        m_device->unmapMemory(*m_stagingMemory.memory);
        m_mapped = nullptr;
    }
    // The buffer deleter destroys the buffer first, then releases its memory.
    m_staging.reset();
    m_stagingMemory = DeviceMemorySlice{};
}

void BufferObject::updateRawData(const void *data, size_t size)
{
    if (data == nullptr || size == 0) {
        return;
    }
    ensureStaging();
    std::memcpy(m_mapped, data, std::min(size, m_byteSize));
    m_dirty = true;
}

void BufferObject::OnUploadCompleted()
{
    releaseStaging();
}

void BufferObject::OnReadbackRequested()
{
    ensureStaging();
}

void *BufferObject::MappedData()
{
    return m_mapped;
}

void BufferObject::recordCopyFromStagingToDevice(const vk::CommandBuffer &cmd)
{
    if (!m_primary || !m_staging) {
        return;
    }

    // Host writes on the staging buffer must be visible to the transfer stage.
    {
        vk::BufferMemoryBarrier barrier{};
        barrier.srcAccessMask = vk::AccessFlagBits::eHostWrite;
        barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = *m_staging;
        barrier.offset = 0;
        barrier.size = static_cast<vk::DeviceSize>(m_byteSize);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eHost, vk::PipelineStageFlagBits::eTransfer,
                            vk::DependencyFlags(), 0, nullptr, 1, &barrier, 0, nullptr);
    }

    vk::BufferCopy copyRegion(0, 0, static_cast<vk::DeviceSize>(m_byteSize));
    cmd.copyBuffer(*m_staging, *m_primary, 1, &copyRegion);

    // Transfer write -> consumer read on the primary buffer.
    switch (m_resourceType) {
        case ResourceTypes::eVertex:
            recordBarrier(cmd, vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eVertexAttributeRead,
                          vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eVertexInput);
            break;
        case ResourceTypes::eIndex:
            recordBarrier(cmd, vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eIndexRead,
                          vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eVertexInput);
            break;
        case ResourceTypes::eUniform:
            recordBarrier(cmd, vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eUniformRead,
                          vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands);
            break;
        case ResourceTypes::eStorage:
            recordBarrier(cmd, vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                          vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands);
            break;
        default:
            recordBarrier(cmd, vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eShaderRead,
                          vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands);
            break;
    }
}

void BufferObject::recordCopyFromDeviceToStaging(const vk::CommandBuffer &cmd)
{
    if (!m_primary || !m_staging) {
        return;
    }

    // Shader/compute writes must finish before the transfer reads the primary.
    {
        vk::BufferMemoryBarrier barrier{};
        barrier.srcAccessMask = vk::AccessFlagBits::eShaderWrite;
        barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = *m_primary;
        barrier.offset = 0;
        barrier.size = static_cast<vk::DeviceSize>(m_byteSize);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands, vk::PipelineStageFlagBits::eTransfer,
                            vk::DependencyFlags(), 0, nullptr, 1, &barrier, 0, nullptr);
    }

    vk::BufferCopy copyRegion(0, 0, static_cast<vk::DeviceSize>(m_byteSize));
    cmd.copyBuffer(*m_primary, *m_staging, 1, &copyRegion);

    // Make the transfer write visible to host reads after the fence.
    {
        vk::BufferMemoryBarrier barrier{};
        barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
        barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = *m_staging;
        barrier.offset = 0;
        barrier.size = static_cast<vk::DeviceSize>(m_byteSize);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
                            vk::DependencyFlags(), 0, nullptr, 1, &barrier, 0, nullptr);
    }
}

void BufferObject::recordBarrier(const vk::CommandBuffer &cmd, vk::AccessFlagBits src, vk::AccessFlagBits dst,
                                 vk::PipelineStageFlagBits srcStage, vk::PipelineStageFlagBits dstStage)
{
    vk::BufferMemoryBarrier barrier{};
    barrier.srcAccessMask = src;
    barrier.dstAccessMask = dst;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = *m_primary;
    barrier.offset = 0;
    barrier.size = static_cast<vk::DeviceSize>(m_byteSize);
    cmd.pipelineBarrier(srcStage, dstStage, vk::DependencyFlags(), 0, nullptr, 1, &barrier, 0, nullptr);
}

void BufferObject::bindToPipeline(const vk::CommandBuffer &cmd)
{
    if (!m_primary) {
        return;
    }
    switch (m_resourceType) {
        case ResourceTypes::eVertex: {
            vk::Buffer buffers[] = {*m_primary};
            vk::DeviceSize offsets[] = {0};
            cmd.bindVertexBuffers(0, 1, buffers, offsets);
            break;
        }
        case ResourceTypes::eInstanceVertex: {
            // Binding 1 is the per-instance stream (VK_VERTEX_INPUT_RATE_INSTANCE).
            // The vertex input state declares it explicitly; see 28.3.
            vk::Buffer buffers[] = {*m_primary};
            vk::DeviceSize offsets[] = {0};
            cmd.bindVertexBuffers(1, 1, buffers, offsets);
            break;
        }
        case ResourceTypes::eIndex: {
            vk::IndexType indexType = vk::IndexType::eUint32;
            switch (m_spec.elementType) {
                case DataTypes::eShort:
                case DataTypes::eUnsignedShort: indexType = vk::IndexType::eUint16; break;
                case DataTypes::eInt:
                case DataTypes::eUnsignedInt: indexType = vk::IndexType::eUint32; break;
                case DataTypes::eChar:
                case DataTypes::eUnsignedChar: indexType = vk::IndexType::eUint8EXT; break;
                default: throw std::runtime_error("cannot infer index type from buffer spec");
            }
            cmd.bindIndexBuffer(*m_primary, 0, indexType);
            break;
        }
        default: break; // uniform/storage buffers are bound through descriptors
    }
}

vk::WriteDescriptorSet BufferObject::constructWriteDescriptorSet(vk::DescriptorSet set, vk::DescriptorType type,
                                                                uint32_t binding)
{
    m_bufferInfo.buffer = primaryBuffer();
    m_bufferInfo.offset = 0;
    m_bufferInfo.range = static_cast<vk::DeviceSize>(m_byteSize);

    vk::WriteDescriptorSet write{};
    write.dstSet = set;
    write.dstBinding = binding;
    write.dstArrayElement = 0;
    write.descriptorType = type;
    write.descriptorCount = 1;
    write.pBufferInfo = &m_bufferInfo;
    return write;
}

void BufferObject::accept(DeviceResourceVisitor &visitor)
{
    visitor.visit(*this);
}
