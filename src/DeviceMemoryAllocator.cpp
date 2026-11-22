#include "DeviceMemoryAllocator.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void checkResult(vk::Result result, const char *what)
{
    if (result != vk::Result::eSuccess) {
        throw std::runtime_error(std::string(what) + " (Vulkan error: " + vk::to_string(result) + ")");
    }
}

// The allocator API has no per-request alignment parameter, so every carved
// region is aligned to a conservative power-of-two multiple that satisfies
// the usual buffer/image memory-requirement alignments.
constexpr vk::DeviceSize kSubAllocationAlignment = 1024;

constexpr vk::DeviceSize alignUp(vk::DeviceSize value, vk::DeviceSize alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

// Merge adjacent (or overlapping) free regions in a sorted vector.
template <typename Region>
void coalesce(std::vector<Region> &regions)
{
    if (regions.size() < 2) {
        return;
    }
    std::sort(regions.begin(), regions.end(),
              [](const Region &a, const Region &b) { return a.offset < b.offset; });

    size_t write = 0;
    for (size_t read = 1; read < regions.size(); ++read) {
        auto &last = regions[write];
        auto &cur = regions[read];
        if (cur.offset <= last.offset + last.size) {
            const vk::DeviceSize end = std::max(last.offset + last.size, cur.offset + cur.size);
            last.size = end - last.offset;
        } else {
            regions[++write] = cur;
        }
    }
    regions.resize(write + 1);
}

} // namespace

DeviceMemoryAllocator::DeviceMemoryAllocator(std::shared_ptr<vk::Device> device,
                                             const vk::PhysicalDeviceMemoryProperties &memProperties) :
    m_device(std::move(device)), m_memProperties(memProperties)
{
}

uint32_t DeviceMemoryAllocator::findMemoryType(uint32_t typeFilter, vk::MemoryPropertyFlags properties) const
{
    for (uint32_t i = 0; i < m_memProperties.memoryTypeCount; ++i) {
        const vk::MemoryPropertyFlags flags = m_memProperties.memoryTypes[i].propertyFlags;
        if ((typeFilter & (1u << i)) != 0 && (flags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("failed to find suitable memory type");
}

DeviceMemorySlice DeviceMemoryAllocator::allocate(vk::DeviceSize size, uint32_t memoryTypeIndex,
                                                  vk::MemoryPropertyFlags propertyFlags)
{
    (void)propertyFlags; // the caller resolves the exact type via findMemoryType
    if (size == 0) {
        throw std::invalid_argument("cannot allocate a zero-sized memory slice");
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (size > kSmallAllocationThreshold) {
        return allocateDedicated(size, memoryTypeIndex);
    }

    const vk::DeviceSize alignedSize = alignUp(size, kSubAllocationAlignment);
    int blockIndex = findSuitableBlock(alignedSize, memoryTypeIndex);
    if (blockIndex < 0) {
        auto block = std::make_shared<Block>();

        vk::MemoryAllocateInfo allocInfo(kBlockSize, memoryTypeIndex);
        vk::DeviceMemory rawMemory{};
        checkResult(m_device->allocateMemory(&allocInfo, nullptr, &rawMemory), "failed to allocate memory block");

        auto memory = std::shared_ptr<vk::DeviceMemory>(new vk::DeviceMemory(rawMemory),
                                                        [device = m_device](vk::DeviceMemory *p) {
                                                            if (p && *p) {
                                                                device->freeMemory(*p, nullptr);
                                                            }
                                                            delete p;
                                                        });
        block->slice.memory = memory;
        block->slice.offset = 0;
        block->slice.size = kBlockSize;
        block->memoryTypeIndex = memoryTypeIndex;
        block->freeRegions.push_back(Block::FreeRegion{0, kBlockSize});

        m_blockByMemory[memory.get()] = block;
        m_blocks.push_back(block);
        blockIndex = static_cast<int>(m_blocks.size()) - 1;
    }

    auto &block = *m_blocks[static_cast<size_t>(blockIndex)];
    for (auto it = block.freeRegions.begin(); it != block.freeRegions.end(); ++it) {
        const vk::DeviceSize alignedOffset = alignUp(it->offset, kSubAllocationAlignment);
        if (alignedOffset - it->offset + alignedSize > it->size) {
            continue;
        }

        DeviceMemorySlice slice;
        // Aliasing shared_ptr: keeps the block memory alive but does NOT free
        // it (the Block owns the actual vk::DeviceMemory).
        slice.memory = std::shared_ptr<vk::DeviceMemory>(block.slice.memory, block.slice.memory.get());
        slice.offset = alignedOffset;
        slice.size = size;

        const Block::FreeRegion region = *it;
        block.freeRegions.erase(it); // vector::insert/erase invalidates iterators

        if (alignedOffset > region.offset) {
            // Return the leading padding gap to the free list.
            block.freeRegions.push_back(Block::FreeRegion{region.offset, alignedOffset - region.offset});
        }
        const vk::DeviceSize trailingOffset = alignedOffset + alignedSize;
        const vk::DeviceSize trailingSize = region.size - (alignedOffset - region.offset) - alignedSize;
        if (trailingSize > 0) {
            block.freeRegions.push_back(Block::FreeRegion{trailingOffset, trailingSize});
        }
        return slice;
    }

    throw std::runtime_error("failed to carve a region from a device memory block");
}

void DeviceMemoryAllocator::free(const DeviceMemorySlice &slice)
{
    if (!slice.valid()) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    auto found = m_blockByMemory.find(slice.memory.get());
    if (found == m_blockByMemory.end()) {
        // Dedicated allocation: its shared_ptr deleter frees the memory.
        return;
    }
    auto block = found->second.lock();
    if (!block) {
        return;
    }

    const vk::DeviceSize begin = slice.offset;
    const vk::DeviceSize end = slice.offset + alignUp(slice.size, kSubAllocationAlignment);
    auto &regions = block->freeRegions;
    auto it = regions.begin();
    while (it != regions.end() && it->offset < begin) {
        ++it;
    }
    regions.insert(it, Block::FreeRegion{begin, end - begin});
    coalesce(regions);
}

DeviceMemorySlice DeviceMemoryAllocator::allocateDedicated(vk::DeviceSize size, uint32_t memoryTypeIndex)
{
    vk::MemoryAllocateInfo allocInfo(size, memoryTypeIndex);
    vk::DeviceMemory rawMemory{};
    checkResult(m_device->allocateMemory(&allocInfo, nullptr, &rawMemory), "failed to allocate dedicated memory");

    auto memory = std::shared_ptr<vk::DeviceMemory>(new vk::DeviceMemory(rawMemory),
                                                    [device = m_device](vk::DeviceMemory *p) {
                                                        if (p && *p) {
                                                            device->freeMemory(*p, nullptr);
                                                        }
                                                        delete p;
                                                    });
    return DeviceMemorySlice{memory, 0, size};
}

int DeviceMemoryAllocator::findSuitableBlock(vk::DeviceSize size, uint32_t memoryTypeIndex)
{
    for (size_t i = 0; i < m_blocks.size(); ++i) {
        const auto &block = *m_blocks[i];
        if (block.memoryTypeIndex != memoryTypeIndex) {
            continue;
        }
        for (const auto &region : block.freeRegions) {
            const vk::DeviceSize alignedOffset = alignUp(region.offset, kSubAllocationAlignment);
            if (alignedOffset - region.offset + size <= region.size) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}
