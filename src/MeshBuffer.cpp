#include "MeshBuffer.h"
#include "DeviceResourceUtils.h"

MeshBuffer::MeshBuffer(std::shared_ptr<vk::Device> device, const vk::PhysicalDeviceMemoryProperties &memProperties,
                       ResourceTypes resourceType, MemoryTypes memoryType, GBufferSpec buffSpec) :
    m_buffSpec(buffSpec), MemoryObject(device, resourceType, memoryType, buffSpec.getTotalByte())
{
    reserve(memProperties); // create buffer and alloc memory
    mapRawData();
}

MeshBuffer::~MeshBuffer() {}

vk::BufferUsageFlags MeshBuffer::getPrimaryBufferUsageFlags()
{
    vk::BufferUsageFlags bufferUsage{};
    switch (this->ResourceType()) {
        case ResourceTypes::eVertex: bufferUsage = vk::BufferUsageFlagBits::eVertexBuffer; break;
        case ResourceTypes::eIndex: bufferUsage = vk::BufferUsageFlagBits::eIndexBuffer; break;
        case ResourceTypes::eUniform: bufferUsage = vk::BufferUsageFlagBits::eUniformBuffer; break;
        case ResourceTypes::eStorage: bufferUsage = vk::BufferUsageFlagBits::eStorageBuffer; break;
        default: throw std::runtime_error("invalid Usage type");
    }

    if (this->MemoryType() == MemoryTypes::eDevice) {
        bufferUsage |= vk::BufferUsageFlagBits::eTransferDst; //  maybe vk::BufferUsageFlagBits::eTransferSrc is when
                                                              //  recordCopyFromDeviceToStaging
    }
    return bufferUsage;
}
void MeshBuffer::reserve(vk::PhysicalDeviceMemoryProperties memProperties)
{
    vk::DeviceSize bufferSize = this->MemorySize();

    if (this->MemoryType() == MemoryTypes::eDevice) {
        // stagingBuffer, host
        DeviceMemoryProperty stagingProp{memProperties, getStagingMemoryPropertyFlags()};
        std::tie(m_stagingBuffer, m_stagingMemory) =
            DeviceResourceUtils::createBuffer(m_device, bufferSize, vk::BufferUsageFlagBits::eTransferSrc, stagingProp);
    }

    // primaryBuffer, deviceLocal
    DeviceMemoryProperty primaryProp{memProperties, getPrimaryMemoryPropertyFlags()};
    std::tie(m_primaryBuffer, m_primaryMemory) =
        DeviceResourceUtils::createBuffer(m_device, bufferSize, getPrimaryBufferUsageFlags(), primaryProp);
}

void MeshBuffer::recordCopyFromStagingToDevice(const vk::CommandBuffer &commandBuffer)
{
    if (this->MemoryType() == MemoryTypes::eDevice) {
        vk::DeviceSize bufferSize(this->MemorySize());
        vk::BufferCopy copyRegion(0, 0, bufferSize);
        recordCopyBuffer(commandBuffer, *m_stagingBuffer, *m_primaryBuffer, copyRegion);
        // std:: cout << " before copy barrier for type" << statɪc  cast<ɪnt>(ResourceType()) <<"\n";
        auto start = std::chrono::steady_clock::now();
        // Barrier, TODO: how to optimal
        if (ResourceType() == ResourceTypes::eVertex) {
            recordBufferMemoryBarrier(commandBuffer, *m_primaryBuffer, vk::AccessFlagBits::eTransferWrite,
                                      vk::AccessFlagBits::eVertexAttributeRead, vk::PipelineStageFlagBits::eTransfer,
                                      vk::PipelineStageFlagBits::eVertexInput);
        } else if (ResourceType() == ResourceTypes::eIndex) {
            recordBufferMemoryBarrier(commandBuffer, *m_primaryBuffer, vk::AccessFlagBits::eTransferWrite,
                                      vk::AccessFlagBits::eIndexRead, vk::PipelineStageFlagBits::eTransfer,
                                      vk::PipelineStageFlagBits::eVertexInput);
            // non-op
        }
        auto end = std::chrono::steady_clock::now();
        const std::chrono::duration<double, std::milli> diff = end - start;
        // std:: cout << " after copy barrier, cost: " << diff. count() << " ms\n";
    }
}

void MeshBuffer::recordCopyFromDeviceToStaging(const vk::CommandBuffer &commandBuffer)
{ // if (this->MemoryType() == MemoryTypes::eHost) {
  //	vk::DeviceSize bufferSize(this->MemorySize());
  //	vk::BufferCopy copyRegion(0, 0, bufferSize);
  //	recordCopyBuffer(commandBuffer, *m_primaryBuffer, *m_stagingBuffer, copyRegion);
  // }
}

void MeshBuffer::recordCopyBuffer(const vk::CommandBuffer &commandBuffer, vk::Buffer srcBuffer, vk::Buffer dstBuffer,
                                  vk::BufferCopy copyRegion)
{
    commandBuffer.copyBuffer(srcBuffer, dstBuffer, copyRegion);
}

void MeshBuffer::recordBufferMemoryBarrier(const vk::CommandBuffer &commandBuffer, const vk::Buffer &buffer,
                                           vk::AccessFlagBits srcAccessMask, vk::AccessFlagBits dstAccessMask,
                                           vk::PipelineStageFlagBits srcStageMask,
                                           vk::PipelineStageFlagBits dstStageMask)
{
    vk::DeviceSize bufferSize = this->MemorySize();
    vk::BufferMemoryBarrier bufferMemoryBarrier;
    bufferMemoryBarrier.buffer = buffer;
    bufferMemoryBarrier.size = bufferSize;
    bufferMemoryBarrier.srcAccessMask = srcAccessMask;
    bufferMemoryBarrier.dstAccessMask = dstAccessMask;
    bufferMemoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferMemoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    commandBuffer.pipelineBarrier(srcStageMask, dstStageMask, vk::DependencyFlags(), nullptr, bufferMemoryBarrier,
                                  nullptr);
}

void MeshBuffer::bindToPipeline(const vk::CommandBuffer &commandBuffer)
{
    switch (this->ResourceType()) {
        case ResourceTypes::eVertex: {
            vk::Buffer vertexBuffers[] = {*m_primaryBuffer};
            vk::DeviceSize offsets[] = {0};
            commandBuffer.bindVertexBuffers(0, 1, vertexBuffers, offsets);
            break;
        }
        case ResourceTypes::eIndex: {
            auto indexType = getVkIndexType();
            // std:: cout << "indexType: " << static   cast< int>(indexType) << "\n";
            commandBuffer.bindIndexBuffer(*m_primaryBuffer, 0, indexType);
            break;
        }
        case ResourceTypes::eUniform: break;
        default: throw std::runtime_error(" invalid Usage type");
    }
}

vk::IndexType MeshBuffer::getVkIndexType()
{
    switch (m_buffSpec.elementType) {
        case DataTypes::eShort:
        case DataTypes::eUnsignedShort: return vk::IndexType::eUint16; break;
        case DataTypes::eInt:
        case DataTypes::eUnsignedInt: return vk::IndexType::eUint32; break;
        case DataTypes::eChar:
        case DataTypes::eUnsignedChar: return vk::IndexType::eUint8EXT; break;
        case DataTypes::eBool:
        case DataTypes::eFloat:
        case DataTypes::eDouble:
        case DataTypes::eCustom:
        default: throw std::runtime_error(" cannot convert to IndexType");
    }
}

vk::WriteDescriptorSet MeshBuffer::constructDescriptorSet(vk::DescriptorSet descriptorSet,
                                                          vk::DescriptorType descriptorType, uint32_t binding)
{
    // NOTE: Pay attention to 'DescriptorBufferInfo' lifetime, which refer to descriptorWrite.pBufferInfo
    vk::DescriptorBufferInfo &bufferInfo = m_descriptorBufferInfo;
    bufferInfo.buffer = *m_primaryBuffer;
    bufferInfo.offset = 0;
    bufferInfo.range = MemorySize();

    vk::WriteDescriptorSet descriptorWrite{};
    descriptorWrite.dstSet = descriptorSet;
    descriptorWrite.dstBinding = binding;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = descriptorType;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pBufferInfo = &bufferInfo;
    return descriptorWrite;
}