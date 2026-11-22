#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>

// A slice of a larger vk::DeviceMemory block.  offset may be non-zero.
struct DeviceMemorySlice {
    std::shared_ptr<vk::DeviceMemory> memory;
    vk::DeviceSize offset = 0;
    vk::DeviceSize size = 0;
    bool valid() const { return memory != nullptr && size > 0; }
};

// Sub-allocates small allocations (<=4MB) from 16MB blocks with a free list and
// coalescing of adjacent free ranges.  Large allocations get dedicated memory.
class DeviceMemoryAllocator {
public:
    DeviceMemoryAllocator(std::shared_ptr<vk::Device> device,
                          const vk::PhysicalDeviceMemoryProperties &memProperties);

    DeviceMemorySlice allocate(vk::DeviceSize size, uint32_t memoryTypeIndex,
                               vk::MemoryPropertyFlags propertyFlags);
    void free(const DeviceMemorySlice &slice);

    uint32_t findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) const;

private:
    struct Block {
        DeviceMemorySlice slice;
        struct FreeRegion {
            vk::DeviceSize offset;
            vk::DeviceSize size;
        };
        std::vector<FreeRegion> freeRegions;
        uint32_t memoryTypeIndex = 0;
    };

    DeviceMemorySlice allocateDedicated(vk::DeviceSize size, uint32_t memoryTypeIndex);
    int findSuitableBlock(vk::DeviceSize size, uint32_t memoryTypeIndex);

private:
    std::shared_ptr<vk::Device> m_device;
    vk::PhysicalDeviceMemoryProperties m_memProperties;
    std::vector<std::shared_ptr<Block>> m_blocks;
    std::unordered_map<const vk::DeviceMemory *, std::weak_ptr<Block>> m_blockByMemory;
    std::mutex m_mutex;

    static constexpr vk::DeviceSize kBlockSize = 16 * 1024 * 1024;
    static constexpr vk::DeviceSize kSmallAllocationThreshold = 4 * 1024 * 1024;
};
