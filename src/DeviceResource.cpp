#include "MemoryObject.h"
#include "typedefs.h"

MemoryObject::MemoryObject(std::shared_ptr<vk::Device> device, ResourceTypes resourceType, MemoryTypes memoryType,
                           uint32_t totalByte) :
    m_device(device), m_resourceType(resourceType), m_memoryType(memoryType), m_rawData(nullptr), m_memoryTotalByte(totalByte)
{
}

void MemoryObject::updateRawData(const void *data)
{
    if (m_rawData != nullptr && data != nullptr) {
        std::cout << "\nbefore copy to mapped for type " << static_cast<int>(m_resourceType)
                  << ", MemorySize(): " << MemorySize() << "\n";
        auto start = std::chrono::steady_clock::now();
        memcpy(m_rawData, data, MemorySize());
        auto end = std::chrono::steady_clock::now();
        const std::chrono::duration<double, std::milli> diff = end - start;
        // std:: cout <<" after copy complete cost: "<< diff. count()<<" ms\n";
    }
}

uint32_t MemoryObject::MemorySize() const
{
    std::cout << " check resourceType: " << static_cast<int>(m_resourceType)
              << ", memoryTotalByte: " << m_memoryTotalByte << "\n";
    return m_memoryTotalByte;
}

MemoryTypes MemoryObject::MemoryType() const
{
    return m_memoryType;
}

ResourceTypes MemoryObject::ResourceType() const
{
    return m_resourceType;
}

void MemoryObject::mapRawData()
{
    std::shared_ptr<vk::DeviceMemory> hostVisibleMemory = nullptr;
    if (m_memoryType == MemoryTypes::eHost || m_memoryType == MemoryTypes::eDeviceAndHost) {
        hostVisibleMemory = m_primaryMemory;
    } else if (m_memoryType == MemoryTypes::eDevice) {
        hostVisibleMemory = m_stagingMemory;
    } else {
        // unsupport
        return;
    }
    vk::DeviceSize size = this->MemorySize();
    // Given we request coherent host memory we don't need to invalidate / flush
    m_rawData = m_device->mapMemory(*hostVisibleMemory, 0, size, vk::MemoryMapFlags());
    m_unmapMemory = true;
}

void MemoryObject::unmapRawData()
{
    if (!m_unmapMemory) {
        return;
    }

    std::shared_ptr<vk::DeviceMemory> hostVisibleMemory = nullptr;
    if (m_memoryType == MemoryTypes::eHost || m_memoryType == MemoryTypes::eDeviceAndHost) {
        hostVisibleMemory = m_primaryMemory;
    } else if (m_memoryType == MemoryTypes::eDevice) {
        hostVisibleMemory = m_stagingMemory;
        // unsupport
        return;
    }

    vk::DeviceSize size = this->MemorySize();
    vk::MappedMemoryRange mappedRange(*hostVisibleMemory, 0, size);
    auto result = m_device->flushMappedMemoryRanges(1, &mappedRange);
    m_device->unmapMemory(*hostVisibleMemory);
    m_unmapMemory = false;
}

vk::MemoryPropertyFlags MemoryObject::getPrimaryMemoryPropertyFlags()
{
    switch (m_memoryType) {
        case MemoryTypes::eDevice: return vk::MemoryPropertyFlagBits::eDeviceLocal;
        case MemoryTypes::eHost:
            return vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
        case MemoryTypes::eDeviceAndHost:
            return vk::MemoryPropertyFlagBits::eDeviceLocal | vk::MemoryPropertyFlagBits::eHostVisible
                   | vk::MemoryPropertyFlagBits::eHostCoherent;
        default: throw std::runtime_error(" invalid memory type");
    }
}

vk::MemoryPropertyFlags MemoryObject::getStagingMemoryPropertyFlags()
{
    switch (m_memoryType) {
        case MemoryTypes::eDevice:
            return vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
            break;
        default: throw std::runtime_error(" Kompute Memory invalid memory type");
    }
}
