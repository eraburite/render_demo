#include "RayTracingAccelerationStructure.h"

#include <cstring>
#include <stdexcept>

namespace rt {

void DeviceAddressBuffer::Create(vk::Device device, const vk::PhysicalDeviceMemoryProperties &memProps,
                                 const Dispatch &dispatch, vk::DeviceSize byteSize, vk::BufferUsageFlags usage,
                                 bool hostVisible)
{
    size = byteSize;

    vk::BufferCreateInfo bufferInfo{};
    bufferInfo.size = byteSize;
    bufferInfo.usage = usage | vk::BufferUsageFlagBits::eShaderDeviceAddress;
    bufferInfo.sharingMode = vk::SharingMode::eExclusive;
    buffer = device.createBuffer(bufferInfo);

    const vk::MemoryRequirements requirements = device.getBufferMemoryRequirements(buffer);
    const vk::MemoryPropertyFlags flags =
        hostVisible ? (vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent)
                    : vk::MemoryPropertyFlagBits::eDeviceLocal;

    vk::MemoryAllocateFlagsInfo allocateFlags{};
    allocateFlags.flags = vk::MemoryAllocateFlagBits::eDeviceAddress;

    vk::MemoryAllocateInfo allocateInfo{};
    allocateInfo.pNext = &allocateFlags;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(memProps, requirements.memoryTypeBits, flags);
    memory = device.allocateMemory(allocateInfo);
    device.bindBufferMemory(buffer, memory, 0);

    if (hostVisible) {
        mapped = device.mapMemory(memory, 0, VK_WHOLE_SIZE);
    }
    address = dispatch.BufferDeviceAddress(device, buffer);
}

void DeviceAddressBuffer::Upload(const void *data, size_t byteSize)
{
    if (mapped == nullptr || data == nullptr) {
        return;
    }
    std::memcpy(mapped, data, byteSize);
}

void DeviceAddressBuffer::Destroy(vk::Device device)
{
    if (mapped != nullptr) {
        device.unmapMemory(memory);
        mapped = nullptr;
    }
    if (buffer) {
        device.destroyBuffer(buffer);
        buffer = nullptr;
    }
    if (memory) {
        device.freeMemory(memory);
        memory = nullptr;
    }
    size = 0;
    address = 0;
}

void AccelerationStructure::Destroy(vk::Device device, const Dispatch &dispatch)
{
    if (handle) {
        if (dispatch.destroyAccelerationStructure) {
            dispatch.destroyAccelerationStructure(static_cast<VkDevice>(device),
                                                  static_cast<VkAccelerationStructureKHR>(handle), nullptr);
        }
        handle = nullptr;
    }
    storage.Destroy(device);
    address = 0;
}

AccelerationStructureBuilder::AccelerationStructureBuilder(vk::Device device,
                                                           const vk::PhysicalDeviceMemoryProperties &memProps,
                                                           vk::Queue queue, uint32_t queueFamilyIndex,
                                                           const Dispatch &dispatch) :
    m_device(device), m_memProps(memProps), m_queue(queue), m_queueFamilyIndex(queueFamilyIndex), m_dispatch(dispatch)
{
}

AccelerationStructure AccelerationStructureBuilder::Allocate(vk::AccelerationStructureTypeKHR type,
                                                             const vk::AccelerationStructureBuildSizesInfoKHR &sizes)
{
    AccelerationStructure accel;
    accel.storage.Create(m_device, m_memProps, m_dispatch, sizes.accelerationStructureSize,
                         vk::BufferUsageFlagBits::eAccelerationStructureStorageKHR, false);

    vk::AccelerationStructureCreateInfoKHR createInfo{};
    createInfo.sType = vk::StructureType::eAccelerationStructureCreateInfoKHR;
    createInfo.buffer = accel.storage.buffer;
    createInfo.offset = 0;
    createInfo.size = sizes.accelerationStructureSize;
    createInfo.type = type;

    VkAccelerationStructureKHR rawHandle = VK_NULL_HANDLE;
    const VkResult result = m_dispatch.createAccelerationStructure(
        static_cast<VkDevice>(m_device),
        reinterpret_cast<const VkAccelerationStructureCreateInfoKHR *>(&createInfo), nullptr, &rawHandle);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("rt: vkCreateAccelerationStructureKHR failed");
    }
    accel.handle = vk::AccelerationStructureKHR{rawHandle};
    return accel;
}

AccelerationStructure AccelerationStructureBuilder::BuildBottomLevel(const TriangleGeometry &geometry)
{
    if (geometry.vertexBuffer.address == 0) {
        throw std::runtime_error("rt: bottom-level build needs a device-address vertex buffer");
    }

    vk::AccelerationStructureGeometryTrianglesDataKHR triangles{};
    triangles.sType = vk::StructureType::eAccelerationStructureGeometryTrianglesDataKHR;
    triangles.vertexFormat = vk::Format::eR32G32B32Sfloat;
    triangles.vertexData.deviceAddress = geometry.vertexBuffer.address;
    triangles.vertexStride = geometry.vertexStride;
    triangles.maxVertex = geometry.vertexCount == 0 ? 0 : geometry.vertexCount - 1;
    triangles.indexType = geometry.hasIndices ? vk::IndexType::eUint32 : vk::IndexType::eNoneKHR;
    triangles.indexData.deviceAddress = geometry.hasIndices ? geometry.indexBuffer.address : 0;
    triangles.transformData.deviceAddress = 0;

    vk::AccelerationStructureGeometryKHR geometryInfo{};
    geometryInfo.sType = vk::StructureType::eAccelerationStructureGeometryKHR;
    geometryInfo.geometryType = vk::GeometryTypeKHR::eTriangles;
    geometryInfo.geometry.triangles = triangles;
    geometryInfo.flags = geometry.opaque ? vk::GeometryFlagBitsKHR::eOpaque : vk::GeometryFlagBitsKHR{};

    vk::AccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = vk::StructureType::eAccelerationStructureBuildGeometryInfoKHR;
    buildInfo.type = vk::AccelerationStructureTypeKHR::eBottomLevel;
    buildInfo.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
    buildInfo.mode = vk::BuildAccelerationStructureModeKHR::eBuild;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometryInfo;

    const uint32_t primitiveCount =
        geometry.hasIndices ? geometry.indexCount / 3u : geometry.vertexCount / 3u;

    vk::AccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = vk::StructureType::eAccelerationStructureBuildSizesInfoKHR;
    m_dispatch.getAccelerationStructureBuildSizes(
        static_cast<VkDevice>(m_device), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        reinterpret_cast<const VkAccelerationStructureBuildGeometryInfoKHR *>(&buildInfo), &primitiveCount,
        reinterpret_cast<VkAccelerationStructureBuildSizesInfoKHR *>(&sizes));

    AccelerationStructure accel = Allocate(vk::AccelerationStructureTypeKHR::eBottomLevel, sizes);

    DeviceAddressBuffer scratch;
    scratch.Create(m_device, m_memProps, m_dispatch, sizes.buildScratchSize, vk::BufferUsageFlagBits::eStorageBuffer,
                   false);

    buildInfo.dstAccelerationStructure = accel.handle;
    buildInfo.scratchData.deviceAddress = scratch.address;

    vk::AccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = primitiveCount;
    range.primitiveOffset = 0;
    range.firstVertex = 0;
    range.transformOffset = 0;
    const VkAccelerationStructureBuildRangeInfoKHR *rangePointer =
        reinterpret_cast<const VkAccelerationStructureBuildRangeInfoKHR *>(&range);

    RunOnce(m_device, m_queue, m_queueFamilyIndex, [&](vk::CommandBuffer cmd) {
        m_dispatch.cmdBuildAccelerationStructures(
            static_cast<VkCommandBuffer>(cmd), 1,
            reinterpret_cast<const VkAccelerationStructureBuildGeometryInfoKHR *>(&buildInfo), &rangePointer);
    });

    scratch.Destroy(m_device);

    vk::AccelerationStructureDeviceAddressInfoKHR addressInfo{};
    addressInfo.sType = vk::StructureType::eAccelerationStructureDeviceAddressInfoKHR;
    addressInfo.accelerationStructure = accel.handle;
    accel.address = m_dispatch.getAccelerationStructureDeviceAddress(
        static_cast<VkDevice>(m_device),
        reinterpret_cast<const VkAccelerationStructureDeviceAddressInfoKHR *>(&addressInfo));
    return accel;
}

AccelerationStructure
AccelerationStructureBuilder::BuildTopLevel(const std::vector<vk::AccelerationStructureInstanceKHR> &instances)
{
    const size_t instanceBytes = instances.size() * sizeof(VkAccelerationStructureInstanceKHR);
    DeviceAddressBuffer instanceBuffer;
    instanceBuffer.Create(m_device, m_memProps, m_dispatch, instanceBytes,
                          vk::BufferUsageFlagBits::eAccelerationStructureBuildInputReadOnlyKHR, true);
    instanceBuffer.Upload(instances.data(), instanceBytes);

    vk::AccelerationStructureGeometryInstancesDataKHR instancesData{};
    instancesData.sType = vk::StructureType::eAccelerationStructureGeometryInstancesDataKHR;
    instancesData.arrayOfPointers = VK_FALSE;
    instancesData.data.deviceAddress = instanceBuffer.address;

    vk::AccelerationStructureGeometryKHR geometryInfo{};
    geometryInfo.sType = vk::StructureType::eAccelerationStructureGeometryKHR;
    geometryInfo.geometryType = vk::GeometryTypeKHR::eInstances;
    geometryInfo.geometry.instances = instancesData;
    geometryInfo.flags = vk::GeometryFlagBitsKHR::eOpaque;

    vk::AccelerationStructureBuildGeometryInfoKHR buildInfo{};
    buildInfo.sType = vk::StructureType::eAccelerationStructureBuildGeometryInfoKHR;
    buildInfo.type = vk::AccelerationStructureTypeKHR::eTopLevel;
    buildInfo.flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace;
    buildInfo.mode = vk::BuildAccelerationStructureModeKHR::eBuild;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometryInfo;

    const uint32_t primitiveCount = static_cast<uint32_t>(instances.size());

    vk::AccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = vk::StructureType::eAccelerationStructureBuildSizesInfoKHR;
    m_dispatch.getAccelerationStructureBuildSizes(
        static_cast<VkDevice>(m_device), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        reinterpret_cast<const VkAccelerationStructureBuildGeometryInfoKHR *>(&buildInfo), &primitiveCount,
        reinterpret_cast<VkAccelerationStructureBuildSizesInfoKHR *>(&sizes));

    AccelerationStructure accel = Allocate(vk::AccelerationStructureTypeKHR::eTopLevel, sizes);

    DeviceAddressBuffer scratch;
    scratch.Create(m_device, m_memProps, m_dispatch, sizes.buildScratchSize, vk::BufferUsageFlagBits::eStorageBuffer,
                   false);

    buildInfo.dstAccelerationStructure = accel.handle;
    buildInfo.scratchData.deviceAddress = scratch.address;

    vk::AccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = primitiveCount;
    range.primitiveOffset = 0;
    range.firstVertex = 0;
    range.transformOffset = 0;
    const VkAccelerationStructureBuildRangeInfoKHR *rangePointer =
        reinterpret_cast<const VkAccelerationStructureBuildRangeInfoKHR *>(&range);

    RunOnce(m_device, m_queue, m_queueFamilyIndex, [&](vk::CommandBuffer cmd) {
        m_dispatch.cmdBuildAccelerationStructures(
            static_cast<VkCommandBuffer>(cmd), 1,
            reinterpret_cast<const VkAccelerationStructureBuildGeometryInfoKHR *>(&buildInfo), &rangePointer);
    });

    scratch.Destroy(m_device);
    instanceBuffer.Destroy(m_device);

    vk::AccelerationStructureDeviceAddressInfoKHR addressInfo{};
    addressInfo.sType = vk::StructureType::eAccelerationStructureDeviceAddressInfoKHR;
    addressInfo.accelerationStructure = accel.handle;
    accel.address = m_dispatch.getAccelerationStructureDeviceAddress(
        static_cast<VkDevice>(m_device),
        reinterpret_cast<const VkAccelerationStructureDeviceAddressInfoKHR *>(&addressInfo));
    return accel;
}

} // namespace rt
