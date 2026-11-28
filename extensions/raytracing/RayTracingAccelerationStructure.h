#pragma once
#include <cstdint>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "RayTracingCommon.h"

namespace rt {

// A buffer that can be reached through a device address: acceleration-structure
// storage, build scratch, triangle/index input and the top-level instance array
// all need one.  Deliberately independent of the core allocator, which does not
// set VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT; threading an allocate flag through
// it for three call sites would be more risk than the twenty lines here
// (see DESIGN.md 31.3).
struct DeviceAddressBuffer {
    vk::Buffer buffer{};
    vk::DeviceMemory memory{};
    vk::DeviceSize size = 0;
    uint64_t address = 0;
    void *mapped = nullptr;

    void Create(vk::Device device, const vk::PhysicalDeviceMemoryProperties &memProps, const Dispatch &dispatch,
                vk::DeviceSize byteSize, vk::BufferUsageFlags usage, bool hostVisible);
    void Upload(const void *data, size_t byteSize);
    void Destroy(vk::Device device);
};

// An acceleration structure plus the buffer that backs it.  The backing buffer
// must outlive the handle: vkDestroyAccelerationStructureKHR does not free it.
struct AccelerationStructure {
    vk::AccelerationStructureKHR handle{};
    DeviceAddressBuffer storage{};
    uint64_t address = 0;
    void Destroy(vk::Device device, const Dispatch &dispatch);
};

// One triangle geometry for a bottom-level structure.  vertexStride lets the
// same builder consume the framework's interleaved Vertex (position first)
// without copying.
struct TriangleGeometry {
    DeviceAddressBuffer vertexBuffer{};
    DeviceAddressBuffer indexBuffer{};
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t vertexStride = 0;
    bool hasIndices = true;
    bool opaque = true;
};

class AccelerationStructureBuilder {
public:
    AccelerationStructureBuilder(vk::Device device, const vk::PhysicalDeviceMemoryProperties &memProps,
                                 vk::Queue queue, uint32_t queueFamilyIndex, const Dispatch &dispatch);

    AccelerationStructure BuildBottomLevel(const TriangleGeometry &geometry);
    AccelerationStructure BuildTopLevel(const std::vector<vk::AccelerationStructureInstanceKHR> &instances);

private:
    AccelerationStructure Allocate(vk::AccelerationStructureTypeKHR type,
                                   const vk::AccelerationStructureBuildSizesInfoKHR &sizes);

    vk::Device m_device;
    vk::PhysicalDeviceMemoryProperties m_memProps;
    vk::Queue m_queue;
    uint32_t m_queueFamilyIndex;
    const Dispatch &m_dispatch;
};

} // namespace rt
